/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LeaderFightChecks.h"

#include "AiFactory.h"  // [lead660] GetPlayerSpecTab
#include "CellImpl.h"
#include "ChooseTargetActions.h"
#include "DungeonHazard.h"
#include "DungeonTactics.h"
#include "DynamicObject.h"
#include "EstimatedLifetimeValue.h"  // [lead454] ExpectedLifetime
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "MovementActions.h"
#include "MovementTypedefs.h"  // [lead691] Movement::gravity
#include "SpellAuraEffects.h"  // [lead691] AuraEffect
#include "HealthTriggers.h"   // [lead691] RestState mana lines
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TerrainHazard.h"
#include <algorithm>
#include <cmath>
#include <map>  // [lead991]
#include <unordered_map>  // [lead420]

namespace Leader::Fight
{
    // the first of `spells` the bot knows and has off cooldown (the highest rank listed last); 0: none
    uint32 LeaderReadySpell(Player* bot, std::initializer_list<uint32> spells)
    {
        uint32 ready = 0;
        for (uint32 spell : spells)
            if (bot->HasSpell(spell) && !bot->HasSpellCooldown(spell))
                ready = spell;
        return ready;
    }

    bool LeaderChannelsManaTool(Player* bot)
    {
        Spell const* channel = bot ? bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) : nullptr;
        if (!channel || channel->getState() == SPELL_STATE_FINISHED || !channel->GetSpellInfo())
            return false;
        uint32 const first = channel->GetSpellInfo()->GetFirstRankSpell()->Id;
        return first == RestState::SPELL_EVOCATION || first == RestState::SPELL_HYMN_OF_HOPE;
    }

    // the bot's interrupt with no cooldown running (Kick, Pummel, Shield Bash, Counterspell, Wind Shear, Mind
    // Freeze, Silence); 0: none
    uint32 LeaderReadyInterrupt(Player* bot)
    {
        // [lead776] b220 rfc-h 5H Taragaman 681.7 / 721.7 s: no kicker for his Fire Nova (the mage's Counterspell on
        // its cooldown, 708.1 s), the cat walked out of it twice - the marksman's Silencing Shot (34490, off the
        // global cooldown, the bot's own interrupt option: InterruptValues) was no interrupt to the leader
        static constexpr uint32 INTERRUPTS[] = {1766,  1767, 1768,  1769,  38768, 6552,  6554, 72,
                                                1671,  1672, 29704, 2139,  57994, 47528, 15487, 34490};
        uint32 ready = 0;
        for (uint32 spell : INTERRUPTS)
            if (bot->HasSpell(spell) && !bot->HasSpellCooldown(spell))
                ready = spell;
        return ready;
    }

    uint8 LeaderInterruptCostRank(Player* bot)
    {
        uint32 const spell = LeaderReadyInterrupt(bot);
        SpellInfo const* info = spell ? sSpellMgr->GetSpellInfo(spell) : nullptr;
        if (!info)
            return 2;
        // the game's global cooldown: its start recovery category (133) with a time (Spell::TriggerGlobalCooldown)
        if (info->StartRecoveryCategory && info->StartRecoveryTime)
            return 2;
        bool const damageResource = info->PowerType == POWER_ENERGY || info->PowerType == POWER_RAGE ||
                                    info->PowerType == POWER_RUNIC_POWER;
        return damageResource && info->CalcPowerCost(bot, info->GetSchoolMask()) > 0 ? 1 : 0;
    }

    bool LeaderInterruptReaches(Player* bot, Unit* caster)
    {
        uint32 const spell = LeaderReadyInterrupt(bot);
        SpellInfo const* info = spell ? sSpellMgr->GetSpellInfo(spell) : nullptr;
        if (!info || !caster)
            return false;
        if (info->RangeEntry && (info->RangeEntry->Flags & SPELL_RANGE_MELEE))
            return bot->IsWithinMeleeRange(caster);
        return bot->IsWithinCombatRange(caster, info->GetMaxRange(false, bot)) && bot->IsWithinLOSInMap(caster);
    }

    float g_spotAcquireMargin = 0.0f;

    float LeaderShootReach(Player* bot)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        return botAI ? botAI->GetRange("spell") : 0.0f;
    }

    float LeaderRangedMinGap(Player* bot, Unit* target)
    {
        constexpr float MELEE_REACH_EXTRA = 4.0f / 3.0f;  // the game's melee range bonus (NOMINAL_MELEE_RANGE)
        constexpr float AUTO_SHOT_MIN_YD = 8.0f;          // the hunter's dead zone (Auto Shot's minimum range)
        float const reaches = target->GetCombatReach() + bot->GetCombatReach();
        float gap = reaches + MELEE_REACH_EXTRA + 1.0f;
        if (bot->getClass() == CLASS_HUNTER)
            gap = std::max(gap, reaches + AUTO_SHOT_MIN_YD);
        return gap;
    }

    bool LeaderSeesFrom(Player* bot, Unit* target, Position const& at)
    {
        // WorldObject::IsWithinLOSInMap as the member would stand at `at`: its eye (its collision height) to the
        // target's hit sphere point towards it
        float const eye = at.GetPositionZ() + bot->GetCollisionHeight();
        float ox = 0.0f;
        float oy = 0.0f;
        float oz = 0.0f;
        target->GetHitSpherePointFor(Position(at.GetPositionX(), at.GetPositionY(), eye), ox, oy, oz);
        return target->GetMap()->isInLineOfSight(at.GetPositionX(), at.GetPositionY(), eye, ox, oy, oz,
                                                 bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                                 VMAP::ModelIgnoreFlags::Nothing);
    }

    bool LeaderMeleeReachesFrom(Player* bot, Unit* target, Position const& at)
    {
        float const reach =
            std::max(target->GetCombatReach() + bot->GetCombatReach() + 4.0f / 3.0f, float(NOMINAL_MELEE_RANGE));
        return target->GetExactDist(at) <= reach && LeaderSeesFrom(bot, target, at);
    }

    bool LeaderMeleePlace(Player* bot, Unit* target, float side, Position& out, bool* behind)
    {
        // [lead580] b215 rfc-h 5E 365-411 s: the assassination rogue's chase of Oggleflint ended on Oggleflint's own
        // centre (-147.2, 17.6, -38.6): the one place behind him met the wall at his back (no valid ground), the
        // place fell back to the target's position, and from there every ability failed "no line of sight" for 45 s.
        // The places round the target at the follow's melee distance, behind first, the first the member stands on
        // and hits it from (the game's melee range and sight)
        static constexpr float TURNS[] = {0.35f, 0.0f,  -0.35f, 0.7f,  -0.7f, 1.05f, -1.05f, 1.4f, -1.4f,
                                          1.75f, -1.75f, 2.1f,  -2.1f, 2.45f, -2.45f, 2.8f, -2.8f, 3.14f};
        if (!target || !target->IsInWorld() || !bot || bot->GetMap() != target->GetMap())
            return false;
        float const ring = std::max(1.5f, target->GetCombatReach() + bot->GetCombatReach() * 0.5f);
        float const base = target->GetOrientation() + float(M_PI);
        float const sign = side < 0.0f ? -1.0f : 1.0f;
        Map* map = target->GetMap();
        for (float const turn : TURNS)
        {
            float const angle = base + sign * turn;
            float x = target->GetPositionX() + std::cos(angle) * ring;
            float y = target->GetPositionY() + std::sin(angle) * ring;
            float z = target->GetPositionZ();
            if (!std::isfinite(x) || !std::isfinite(y) ||
                !map->CheckCollisionAndGetValidCoords(target, target->GetPositionX(), target->GetPositionY(),
                                                      target->GetPositionZ(), x, y, z) ||
                !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                continue;
            Position const at(x, y, z);
            if (!LeaderMeleeReachesFrom(bot, target, at))
                continue;
            if (behind)
                *behind = !target->HasInArc(float(M_PI), &at);
            out = at;
            return true;
        }
        return false;
    }

    bool LeaderOutOfMobsMelee(Player* bot, Position const& at, std::vector<ObjectGuid> const& fightMobs,
                              bool whereTheyStand)
    {
        for (ObjectGuid const& guid : fightMobs)
        {
            Unit* mob = ObjectAccessor::GetUnit(*bot, guid);
            if (!mob || !mob->IsAlive())
                continue;
            // [lead214] b184 rfc-h: 48-87 "inside a fight mob's melee reach" re-spots a run, half of them within 3 s of
            // the last. A mob on the member itself follows it to every spot (the aggro-loss walk owns that member); a
            // mob on the move is judged where it runs to - its victim - not at each step it takes
            Unit* const victim = mob->GetVictim();
            if (victim == bot)
                continue;
            // [lead380] b200 rfc-h (8 runs): 375 re-spots "inside a fight mob's melee reach" and 296 "out of its
            // reach / sight of the tank its Attack target runs to" moved ranged members and healers that cast from
            // where they stood - a mob running to the tank was judged already there. For the place the member stands
            // on, the fact is where the mob stands now
            Position const stands =
                !whereTheyStand && mob->isMoving() && victim ? victim->GetPosition() : mob->GetPosition();
            if (stands.GetExactDist(at) < LeaderRangedMinGap(bot, mob))
                return false;
        }
        return true;
    }

    bool LeaderCastsFrom(Player* bot, Unit* target, Position const& at, std::vector<ObjectGuid> const* fightMobs,
                         bool whereTheyStand)
    {
        float const gap = at.GetExactDist(target->GetPosition());
        float const reach = LeaderCastReach(bot, target);  // [lead660] the game's reach, both combat reaches in it
        float const minGap = bot->getClass() == CLASS_HUNTER ? LeaderRangedMinGap(bot, target) : 0.0f;
        return reach > 0.0f && gap <= reach && gap >= minGap && LeaderSeesFrom(bot, target, at) &&
               (!fightMobs || LeaderOutOfMobsMelee(bot, at, *fightMobs, whereTheyStand));
    }

    bool HealReaches(Player* tank, Position const& from, Position const& to, Unit const* healer)
    {
        // [lead723] the game's heal reach (Spell::CheckRange -> Unit::IsWithinCombatRange): the 3D gap between the
        // centres under the heal's range plus both combat reaches - healer and healed are players, the tank's reach
        // stands for both (b219 rfc-h: 107 of the 225 heal_reach re-spots at 30-40 yd, under the 43 yd the heals
        // land from; the healers walked 339 s of "heal reach" with the tank in 40 yd and sight)
        // [lead950] b227 5C-11 95-115 s (the wipe): the priest stood 30.5 yd off the blood DK under 7 attackers,
        // out of its sight (the bot's own cast test, WorldObject::IsWithinLOSInMap: each player's eyes at its body's
        // collision height), while this model judged the sight 2 yd over each foot - the eyes are the game's
        // collision heights now (the healer's at `from` when it is known, the tank's at `to`)
        float const reach = Leader::LEADER_HEAL_REACH_YD + 2.0f * tank->GetCombatReach();
        float const fromEyes = (healer ? healer : tank)->GetCollisionHeight();
        float const toEyes = tank->GetCollisionHeight();
        return from.GetExactDist(to) <= reach - g_spotAcquireMargin &&
               tank->GetMap()->isInLineOfSight(from.GetPositionX(), from.GetPositionY(),
                                               from.GetPositionZ() + fromEyes, to.GetPositionX(), to.GetPositionY(),
                                               to.GetPositionZ() + toEyes, tank->GetPhaseMask(),
                                               LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
    }

    // the AoE rule around the target: the mobs to spare (crowd-controlled, not pulled, without the tank's threat)
    Leader::AoeRule LeaderAoeRule(Player* tank, Unit* target, float range, std::string& why,
                                  std::vector<ObjectGuid>& spared)
    {
        // [lead415] b201 rfc-h 5G: the frost DK held Howling Blast with Rime 73 times "the leader forbids AoE" - one
        // unpulled mob 13 yd from the target forbade every area spell, while Howling Blast hits 10 yd around its
        // target. The rule names the mobs to spare; each AoE's own centre and radius (its spell data) is checked
        // against them where it is cast (ThreatStrategy's AreaFootprint)
        spared.clear();
        why.clear();
        std::list<Unit*> nearUnits;
        Acore::AnyUnitInObjectRangeCheck check(target, range);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(target, nearUnits, check);
        Cell::VisitObjects(target, searcher, range);
        for (Unit* unit : nearUnits)
        {
            if (unit == target || !unit->ToCreature() || !tank->IsValidAttackTarget(unit))
                continue;
            std::string spare;
            if (unit->HasBreakableByDamageCrowdControlAura())
                spare = Acore::StringFormat("{} is crowd-controlled {:.0f} yd from the target", unit->GetName(),
                                            target->GetExactDist(unit));
            else if (!unit->IsInCombat())
                spare = Acore::StringFormat("{} is not pulled, {:.0f} yd from the target", unit->GetName(),
                                            target->GetExactDist(unit));
            // [lead90] b131 rfc-h 941.9-943.7 s: "allowed-on-pack" while a Searing Blade Warlock of the pulled
            // pack ran round the corner with no tank threat (not attacking anyone yet, so outside the kill
            // order's headroom check) - the mage's AoE took it (84 threat against the tank's 0)
            else if (unit->GetThreatMgr().GetThreat(tank) <= 0.0f)
                spare = Acore::StringFormat("{} has no tank threat yet, {:.0f} yd from the target", unit->GetName(),
                                            target->GetExactDist(unit));
            if (spare.empty())
                continue;
            spared.push_back(unit->GetGUID());
            if (why.empty())
                why = "spares " + spare;
        }
        if (why.empty())
            why = "the pack only";
        return Leader::AoeRule::AllowedOnPack;
    }

    namespace
    {
        // [lead420] a spell's area radius (its effects', then the spells they trigger, two deep: Starfall's stars);
        // a chain without a radius: the config's AoE radius, as the member's gate reads it (AreaRadius)
        float SpellAreaRadius(Player* bot, SpellInfo const* info, uint8 depth)
        {
            float radius = 0.0f;
            for (SpellEffectInfo const& effect : info->GetEffects())
            {
                if (!effect.IsEffect())
                    continue;
                radius = std::max(radius, effect.CalcRadius(bot));
                if (effect.ChainTarget > 1)
                    radius = std::max(radius, float(sPlayerbotAIConfig.AoeRadius));
                if (depth && effect.TriggerSpell)
                    if (SpellInfo const* triggered = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                        radius = std::max(radius, SpellAreaRadius(bot, triggered, depth - 1));
            }
            return radius;
        }
    }

    float LeaderMaxAreaRadius(Player* bot)
    {
        // the spell scan is cached per bot while its spells stay the same (a lookup, no clock)
        struct Cached
        {
            size_t spells = 0;
            float radius = 0.0f;
        };
        thread_local std::unordered_map<uint64, Cached> cache;
        Cached& cached = cache[bot->GetGUID().GetRawValue()];
        if (cached.spells == bot->GetSpellMap().size())
            return cached.radius;
        float radius = 0.0f;
        for (auto const& [spellId, known] : bot->GetSpellMap())
        {
            if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || info->IsPassive() || info->IsPositive())
                continue;
            radius = std::max(radius, SpellAreaRadius(bot, info, 2));
        }
        cached = {bot->GetSpellMap().size(), radius};
        return radius;
    }

    namespace
    {
        // the tank's own state that stops its walks (MovementAction::UpdateMovementState's restriction)
        bool TankHeld(Player* tank)
        {
            return tank->HasUnitState(UNIT_STATE_LOST_CONTROL) || tank->IsRooted() || tank->isFrozen() ||
                   tank->IsPolymorphed();
        }
    }

    Leader::DungeonLeaderState::Unreachable LeaderUnreachable(Player* tank, Unit* mob, std::string const& why,
                                                              uint32 peaceSinceMs)
    {
        Leader::DungeonLeaderState::Unreachable verdict;
        verdict.tankAt = tank->GetPosition();
        verdict.mobAt = mob ? mob->GetPosition() : tank->GetPosition();
        verdict.why = why;
        verdict.tankHeld = TankHeld(tank);
        verdict.tankFighting = tank->IsInCombat();
        verdict.peaceSinceMs = peaceSinceMs;
        verdict.mobFighting = mob && mob->IsInCombat();
        return verdict;
    }

    std::string LeaderUnreachableChanged(Leader::DungeonLeaderState::Unreachable const& verdict, Player* tank,
                                         Unit const* mob, uint32 peaceSinceMs)
    {
        // [lead1011] b229 rfc-h 5H-16 724.8 s: Taragaman stored unreachable while the bear stood stunned at the end of
        // the Enforcer fight; neither the tank nor the boss moved again, the entry was never cleared and the run
        // stalled 240 s ("the route target out of the pick"). The verdict lasts while every fact it was judged
        // under holds
        if (!mob || !mob->IsAlive())
            return "the mob is gone";
        if (tank->GetExactDist(verdict.tankAt) > Leader::LEADER_COMMIT_MOVED_YD)
            return "the tank moved";
        if (mob->GetExactDist(verdict.mobAt) > Leader::LEADER_COMMIT_MOVED_YD)
            return "the mob moved";
        if (TankHeld(tank) != verdict.tankHeld)
            return verdict.tankHeld ? "the tank's stun / root ended" : "the tank is stunned / rooted";
        if (tank->IsInCombat() != verdict.tankFighting)
            return verdict.tankFighting ? "the tank's fight ended" : "the tank fights";
        if (peaceSinceMs != verdict.peaceSinceMs)
            return "a fight ended since";
        if (mob->IsInCombat() != verdict.mobFighting)
            return verdict.mobFighting ? "the mob left its fight" : "the mob fights";
        return std::string();
    }

    // [lead53] the tank's taunt reach: its known taunt spell's range (0: none known)
    float LeaderTauntReach(Player* tank)
    {
        for (uint32 spell : TANK_TAUNTS)
            if (tank->HasSpell(spell))
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell))
                    return info->GetMaxRange(false);
        return 0.0f;
    }

    // the member's walk-along spot behind the tank (two rows of two), on reachable ground
    Position LeaderFollowSpot(Player* tank, uint32 index)
    {
        float const back = 3.0f + 2.5f * float((index / 2) % 2);
        float const side = (index % 2 ? 1.5f : -1.5f);
        float const o = tank->GetOrientation();
        float x = tank->GetPositionX() + std::cos(o + float(M_PI)) * back + std::cos(o + float(M_PI) / 2.0f) * side;
        float y = tank->GetPositionY() + std::sin(o + float(M_PI)) * back + std::sin(o + float(M_PI) / 2.0f) * side;
        float z = tank->GetPositionZ();
        if (!tank->GetMap()->CheckCollisionAndGetValidCoords(tank, tank->GetPositionX(), tank->GetPositionY(),
                                                              tank->GetPositionZ(), x, y, z))
            return tank->GetPosition();
        return Position(x, y, z);
    }

    std::vector<Creature*> const* g_leaderIdle = nullptr;
    std::vector<Position> const* g_leaderCleared = nullptr;
    std::vector<ObjectGuid> const* g_leaderExempt = nullptr;  // [lead24] the committed pull's mobs
    std::set<ObjectGuid> const* g_leaderJoined = nullptr;     // [lead600] the packs decided into the fight

    // [lead24] the committed pull's own mobs (target, pack, expected pack)
    std::vector<ObjectGuid> LeaderPullMobs(std::shared_ptr<PullPlanner::Decision> const& commit, ObjectGuid mob)
    {
        std::vector<ObjectGuid> out;
        if (!mob.IsEmpty())
            out.push_back(mob);
        if (commit)
        {
            for (ObjectGuid const guid : {commit->planTarget, commit->attack, commit->lookTarget,
                                          commit->verdictTarget})
                if (!guid.IsEmpty())
                    out.push_back(guid);
            out.insert(out.end(), commit->pack.begin(), commit->pack.end());
            out.insert(out.end(), commit->verdictPack.begin(), commit->verdictPack.end());
        }
        return out;
    }

    bool LeaderOnCleared(Position const& spot)
    {
        if (!g_leaderCleared)
            return false;
        for (Position const& at : *g_leaderCleared)
            if (at.GetExactDist2d(spot.GetPositionX(), spot.GetPositionY()) <= 4.0f &&
                std::fabs(at.GetPositionZ() - spot.GetPositionZ()) < 4.0f)
                return true;
        return false;
    }

    namespace
    {
        // the unpulled mob's reach (IdleMobReachesSpot, plus LEADER_SPOT_AGGRO_MARGIN towards it) covers the spot
        bool IdleMobCovers(Player* bot, Creature* mob, Position const& spot)
        {
            if (!mob->IsInWorld() || !mob->IsAlive() || mob->IsInCombat())
                return false;
            // [lead24] the mob being pulled now and its pack: no reason to keep away from them - for the tank, who
            // pulls them. [lead500] b210 rfc-h: 22 of the 87 aggro losses were "the tank never had it" (b209: 8), the
            // victim at 0 threat, most of them melee (5D DK 7, 5H cat 5): 5D 187.8 s the DK got "combat spot: behind
            // the target" on the Ragefire Shaman the bear had just charged and woke the pack's Trogg on its walk;
            // 5H 635.8 s the cat got Attack on a pulled-back Cultist still at its pack and woke two Enforcers. A pull's
            // mob out of combat is an idle mob for every other member (the tank has not got it yet)
            if (g_leaderExempt && PlayerbotAI::IsTank(bot) &&
                std::find(g_leaderExempt->begin(), g_leaderExempt->end(), mob->GetGUID()) != g_leaderExempt->end())
                return false;
            // [lead600] b217 rfc-h 5G pull 7 (95-131 s, the wipe): the leader judged three idle packs into the fight
            // ("pack_joins", 6-7 mobs, overload 0.86-1.00) because they blocked the healer's and the melee's spots,
            // and then went on rejecting every spot and chase for those same mobs (114 "heal reach" and 40 "aggro
            // loss: to the tank" spot_rejected_aggro, 21 chase_refused, heal_reach "fix: none" at 45 yd): lead500 made
            // the exempt list the tank's only, and lead76's joined packs were in it. The healer and the three melee
            // stood 27 s where the pull found them, the blood DK died alone out of the healer's sight and the group
            // wiped. A pack the leader decided into the fight is the fight's (its capacity verdict counts it): it
            // blocks nobody's spot or walk
            if (g_leaderJoined && g_leaderJoined->count(mob->GetGUID()))
                return false;
            if (IdleMobReachesSpot(bot, mob, spot))
                return true;
            float const gap = mob->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY());
            if (gap > 0.1f && gap < 60.0f)
            {
                float const step = std::min(Leader::LEADER_SPOT_AGGRO_MARGIN, gap) / gap;
                Position const closer(spot.GetPositionX() + (mob->GetPositionX() - spot.GetPositionX()) * step,
                                      spot.GetPositionY() + (mob->GetPositionY() - spot.GetPositionY()) * step,
                                      spot.GetPositionZ());
                if (IdleMobReachesSpot(bot, mob, closer))
                    return true;
            }
            return false;
        }
    }

    // the unpulled mob whose reach (the planner's IdleMobReachesSpot, plus LEADER_SPOT_AGGRO_MARGIN towards it)
    // covers the spot; nullptr: none
    Creature* LeaderAggroAt(Player* bot, Position const& spot)
    {
        if (!Leader::LEADER_SPOT_AGGRO_GUARD || !g_leaderIdle)
            return nullptr;
        for (Creature* mob : *g_leaderIdle)
            if (IdleMobCovers(bot, mob, spot))
                return mob;
        return nullptr;
    }

    Creature* LeaderFightAggroAt(Player* bot, Position const& spot, Unit* target)
    {
        if (!Leader::LEADER_SPOT_AGGRO_GUARD || !g_leaderIdle || !target)
            return nullptr;
        // the one the target fights (the tank): it stands where it is, in the fight, without waking the mob
        Unit* const holder = target->GetVictim();
        bool const held = holder && holder != bot && holder->IsAlive() && holder->IsInCombat();
        for (Creature* mob : *g_leaderIdle)
        {
            if (!IdleMobCovers(bot, mob, spot))
                continue;
            // the fight already stands in this mob's modelled reach without waking it: the target's victim (a player)
            // where it stands. A spot no nearer the mob than it is no deeper into its reach than the fight itself. A
            // place out of the modelled reach proves nothing (its sight / height facts differ from the spot's): not
            // counted. [lead500] the target itself is no such proof: a mob standing beside its idle pack mates wakes
            // none of them (b210 5D 187.8 s: the spot behind the charged Shaman, no nearer the Trogg than the Shaman,
            // was the DK's walk into that Trogg while the bear was still on its way)
            float fightGap = -1.0f;
            if (held && IdleMobCovers(bot, mob, holder->GetPosition()))
                fightGap = mob->GetExactDist2d(holder);
            if (fightGap < 0.0f || mob->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY()) < fightGap)
                return mob;
        }
        return nullptr;
    }

    // the walk to the spot passes no unpulled mob's reach (and there is a walk)
    Creature* LeaderAggroOnWalk(Player* bot, Position const& spot, bool& noPath, Unit* fightTarget)
    {
        noPath = false;
        if (!Leader::LEADER_SPOT_AGGRO_GUARD || !g_leaderIdle)
            return nullptr;
        Leader::RouteResult const route = Leader::Route(bot, spot);  // [lead99]
        if (!route.found)
        {
            noPath = true;
            return nullptr;
        }
        for (G3D::Vector3 const& point : route.points)
        {
            Position const at(point.x, point.y, point.z);
            if (Creature* mob = fightTarget ? LeaderFightAggroAt(bot, at, fightTarget) : LeaderAggroAt(bot, at))
                return mob;
        }
        return nullptr;
    }

    // a candidate of a spot search: 0 rejected (an unpulled mob reaches it), 1 valid on new ground (kept as the
    // fallback), 2 valid on cleared ground (taken at once)
    int LeaderCandidate(Player* bot, Position const& spot, std::optional<Position>& fallback, Unit* fightTarget)
    {
        if (fightTarget ? LeaderFightAggroAt(bot, spot, fightTarget) != nullptr : LeaderAggroAt(bot, spot) != nullptr)
            return 0;
        if (!Leader::LEADER_PREFER_CLEARED || LeaderOnCleared(spot))
            return 2;
        if (!fallback)
            fallback = spot;
        return 1;
    }

    // a free spot near the healer, out of every cleave cone, from which it reaches everyone (`keeps`)
    bool LeaderHealerRespot(Player* healer, PlayerbotAI* healerAI,
                            std::function<bool(Position const&)> const& keeps, Position& out)
    {
        std::optional<Position> fallback;  // [lead18] new ground, when no cleared spot fits
        constexpr float STEPS[] = {4.0f, 8.0f, 12.0f, 16.0f};
        constexpr uint32 DIRECTIONS = 16;
        Map* map = healer->GetMap();
        for (float step : STEPS)
            for (uint32 i = 0; i < DIRECTIONS; ++i)
            {
                float const angle = 2.0f * float(M_PI) * float(i) / float(DIRECTIONS);
                float x = healer->GetPositionX() + std::cos(angle) * step;
                float y = healer->GetPositionY() + std::sin(angle) * step;
                float z = healer->GetPositionZ();
                if (!map->CheckCollisionAndGetValidCoords(healer, healer->GetPositionX(), healer->GetPositionY(),
                                                          healer->GetPositionZ(), x, y, z))
                    continue;
                if (healerAI && DungeonTactics::SpotInFrontalCleave(healer, healerAI, x, y, 1.0f,
                                                                    20.0f * float(M_PI) / 180.0f))
                    continue;
                Position const spot(x, y, z);
                if (!keeps(spot))
                    continue;
                if (LeaderCandidate(healer, spot, fallback) != 2)
                    continue;
                out = spot;
                return true;
            }
        if (fallback)
        {
            out = *fallback;
            return true;
        }
        return false;
    }

    char const* MoveEventName(Leader::MoveEvent move)
    {
        switch (move)
        {
            case Leader::MoveEvent::MoveTo:
                return "MoveTo";
            case Leader::MoveEvent::StandAt:
                return "StandAt";
            case Leader::MoveEvent::Hold:
                return "Hold";
            default:
                return "None";
        }
    }

    // A free spot out of every cleave cone near the member, nearest first (DungeonTactics's exit steps), that
    // `keeps` (the heal reach).
    bool LeaderCleaveExit(Player* bot, PlayerbotAI* botAI, Position& out,
                          std::function<bool(Position const&)> const& keeps)
    {
        std::optional<Position> fallback;  // [lead18] new ground, when no cleared spot fits
        constexpr float STEPS[] = {4.0f, 7.0f, 10.0f, 13.0f};
        constexpr uint32 DIRECTIONS = 16;
        constexpr float DIST_MARGIN = 1.0f;
        constexpr float ANGLE_MARGIN = 20.0f * float(M_PI) / 180.0f;
        Map* map = bot->GetMap();
        for (float step : STEPS)
            for (uint32 i = 0; i < DIRECTIONS; ++i)
            {
                float const angle = 2.0f * float(M_PI) * float(i) / float(DIRECTIONS);
                float x = bot->GetPositionX() + std::cos(angle) * step;
                float y = bot->GetPositionY() + std::sin(angle) * step;
                float z = bot->GetPositionZ();
                if (!map->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                          bot->GetPositionZ(), x, y, z))
                    continue;
                if (DungeonTactics::SpotInFrontalCleave(bot, botAI, x, y, DIST_MARGIN, ANGLE_MARGIN))
                    continue;
                if (!keeps(Position(x, y, z)))
                    continue;
                if (LeaderCandidate(bot, Position(x, y, z), fallback) != 2)
                    continue;
                out.Relocate(x, y, z);
                return true;
            }
        if (fallback)
        {
            out = *fallback;
            return true;
        }
        return false;
    }

    void LeaderCollectHazards(Player* bot, std::vector<HazardZone>& out)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return;
        AvoidAoeAction::CollectZones(botAI, out);
        std::list<WorldObject*> objects;
        Acore::AllWorldObjectsInRange check(bot, LEADER_HAZARD_SCAN_YD);
        Acore::WorldObjectListSearcher<Acore::AllWorldObjectsInRange> searcher(bot, objects, check,
                                                                                GRID_MAP_TYPE_MASK_DYNAMICOBJECT);
        Cell::VisitObjects(bot, searcher, LEADER_HAZARD_SCAN_YD);
        for (WorldObject* object : objects)
        {
            DynamicObject* dyn = object ? object->ToDynObject() : nullptr;
            if (!dyn || !dyn->IsInWorld() || dyn->GetByteValue(DYNAMICOBJECT_BYTES, 0) != DYNAMIC_OBJECT_AREA_SPELL)
                continue;
            Unit* caster = dyn->GetCaster();
            SpellInfo const* info = sSpellMgr->GetSpellInfo(dyn->GetSpellId());
            if (!caster || !info || info->IsPositive() || !caster->IsHostileTo(bot) ||
                sPlayerbotAIConfig.AoeAvoidSpellWhitelist.count(info->Id))
                continue;
            float const radius = dyn->GetRadius();
            if (radius > 0.0f && radius <= sPlayerbotAIConfig.MaxAoeAvoidRadius)
            {
                // [lead780] its spell, caster, time left and tick for the leader's stay-or-leave verdict
                uint32 periodMs = 0;
                for (SpellEffectInfo const& effect : info->GetEffects())
                    if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE && effect.Amplitude > 0)
                        periodMs = uint32(effect.Amplitude);
                out.push_back({dyn->GetPosition(), radius, info->SpellName[LOCALE_enUS], ObjectGuid::Empty, info->Id,
                               caster->GetGUID(), dyn->GetDuration(), periodMs});
            }
        }
        for (DungeonHazard::Circle const& circle : DungeonHazard::Collect(bot))
            if (!circle.chasing)
                out.push_back({Position(circle.x, circle.y, bot->GetPositionZ()), circle.radius,
                               "dungeon hazard"});
    }

    // the circle of `zones` that holds (x, y, z) (with `extra` around it), nullptr: none
    HazardZone const* LeaderZoneAt(std::vector<HazardZone> const& zones, float x, float y, float z, float extra)
    {
        for (HazardZone const& zone : zones)
            if (std::fabs(zone.centre.GetPositionZ() - z) <= LEADER_HAZARD_DZ &&
                zone.centre.GetExactDist2d(x, y) < zone.radius + extra)
                return &zone;
        return nullptr;
    }

    // the spot is fit to stand on in a fight: out of every hazard and cleave cone, off lava / drop-offs
    bool LeaderSpotSafe(Player* bot, PlayerbotAI* botAI, std::vector<HazardZone> const& zones, float x, float y,
                        float z)
    {
        if (LeaderZoneAt(zones, x, y, z, LEADER_HAZARD_EDGE))
            return false;
        if (NearHazard(bot->GetMap(), bot->GetPhaseMask(), x, y, z, LEADER_TERRAIN_EDGE))
            return false;
        return !botAI || !DungeonTactics::SpotInFrontalCleave(bot, botAI, x, y, 1.0f, 20.0f * float(M_PI) / 180.0f);
    }

    // the nearest safe spot around the member that `keeps` (the heal reach), on its floor
    bool LeaderSafeSpot(Player* bot, PlayerbotAI* botAI, std::vector<HazardZone> const& zones,
                        std::function<bool(Position const&)> const& keeps, Position& out)
    {
        std::optional<Position> fallback;  // [lead18] new ground, when no cleared spot fits
        constexpr float STEPS[] = {3.0f, 5.0f, 7.0f, 10.0f, 13.0f, 16.0f};
        constexpr uint32 DIRECTIONS = 16;
        Map* map = bot->GetMap();
        // [lead155] b174 20261101-155639 669.5 s: "no safe spot out of area cast of Taragaman the Hungerer keeps the
        // heal reach" - the rings round the member end at 16 yd, the cast's circle (its spell radius) reaches farther.
        // First the spots just outside each circle the member stands in (its radius + the edge, along the member's
        // way out and round it), then the rings
        for (HazardZone const& zone : zones)
        {
            float const fromCentre = zone.centre.GetExactDist2d(bot->GetPositionX(), bot->GetPositionY());
            if (fromCentre > zone.radius)
                continue;
            float const away = std::atan2(bot->GetPositionY() - zone.centre.GetPositionY(),
                                          bot->GetPositionX() - zone.centre.GetPositionX());
            for (uint32 i = 0; i < DIRECTIONS; ++i)
            {
                float const turn = float((i + 1) / 2) * 2.0f * float(M_PI) / float(DIRECTIONS) * (i % 2 ? 1.0f : -1.0f);
                float const angle = away + turn;
                float const ring = zone.radius + LEADER_HAZARD_EDGE + 0.5f;
                float x = zone.centre.GetPositionX() + std::cos(angle) * ring;
                float y = zone.centre.GetPositionY() + std::sin(angle) * ring;
                float z = bot->GetPositionZ();
                if (!map->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                          bot->GetPositionZ(), x, y, z) ||
                    std::fabs(z - bot->GetPositionZ()) > 4.0f || !LeaderSpotSafe(bot, botAI, zones, x, y, z))
                    continue;
                Position const spot(x, y, z);
                if (!keeps(spot))
                    continue;
                if (LeaderCandidate(bot, spot, fallback) != 2)
                    continue;
                out = spot;
                return true;
            }
        }
        for (float step : STEPS)
            for (uint32 i = 0; i < DIRECTIONS; ++i)
            {
                float const angle = 2.0f * float(M_PI) * float(i) / float(DIRECTIONS);
                float x = bot->GetPositionX() + std::cos(angle) * step;
                float y = bot->GetPositionY() + std::sin(angle) * step;
                float z = bot->GetPositionZ();
                if (!map->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                          bot->GetPositionZ(), x, y, z) ||
                    std::fabs(z - bot->GetPositionZ()) > 4.0f || !LeaderSpotSafe(bot, botAI, zones, x, y, z))
                    continue;
                Position const spot(x, y, z);
                if (!keeps(spot))
                    continue;
                if (LeaderCandidate(bot, spot, fallback) != 2)
                    continue;
                out = spot;
                return true;
            }
        if (fallback)
        {
            out = *fallback;
            return true;
        }
        return false;
    }

    // a combat spot still fits: in its window from the target (melee: behind it), safe, in heal reach; `sight`:
    // also the line of sight to the target (ranged / healer)
    namespace
    {
        thread_local SpotRejects* g_spotRejects = nullptr;

        // [lead455] LeaderCombatSpotValid's checks, "" when the spot fits, else the first that fails
        std::string CombatSpotValidWhy(Player* bot, PlayerbotAI* botAI, Unit* target, Position const& spot, bool melee,
                                       std::vector<HazardZone> const& zones,
                                       std::function<bool(Position const&)> const& keeps, bool sight)
        {
            float const gap = target->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY());
            if (melee)
            {
                float const reach = target->GetCombatReach() + bot->GetCombatReach() + 1.0f;
                if (gap > reach)
                    return "out of melee reach";
                if (target->HasInArc(float(M_PI), &spot))
                    return "in front of the target";
            }
            else if (PlayerbotAI::IsHeal(bot))
            {
                if (gap < LEADER_HEALER_YD - 6.0f || gap > LEADER_HEALER_YD + 10.0f)
                    return "out of the healer's band";
            }
            else
            {
                // [lead127] the member's own reach (LeaderShootReach), not a fixed band: within it
                // [lead463] b207 rfc-h: 39 combat_spot_none, the ring's candidates rejected "out of its band" 4-80
                // times each - the floor of the band was half the reach (15 yd), in RFC's caves the target's sight
                // ends nearer than that on every ray, so no spot existed. The floor is the facts it stood for: out of
                // the target's melee (LeaderRangedMinGap, a hunter's dead zone) - every fight mob's melee reach, the
                // cleaves and the hazards are their own checks (LeaderOutOfMobsMelee, LeaderSpotSafe)
                // [lead660] the reach it casts by (LeaderCastReach), not the configured spell distance alone
                float const reach = LeaderShootReach(bot) > 0.0f ? LeaderCastReach(bot, target) : 0.0f;
                if (reach <= 0.0f || gap < LeaderRangedMinGap(bot, target) || gap > reach)
                    return "out of its band";
            }
            if (!LeaderSpotSafe(bot, botAI, zones, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ()))
                return "unsafe (hazard / drop-off / cleave)";
            if (!keeps(spot))
                return "heal reach";
            // [lead127] the game's own sight from the spot (b159: the hunter's spot passed a +2 yd eye-to-eye ray, its
            // shots "no line of sight" all fight)
            // [lead580] a melee spot too (b215 5E: the rogue 45 s at Oggleflint, every ability "no line of sight")
            if (sight && !LeaderSeesFrom(bot, target, spot))
                return "no line of sight to the target";
            return std::string();
        }
    }

    SpotRejects::SpotRejects() : previous(g_spotRejects) { g_spotRejects = this; }

    SpotRejects::~SpotRejects() { g_spotRejects = previous; }

    std::string SpotRejects::Summary() const
    {
        std::string out;
        for (auto const& [why, count] : counts)
            out += Acore::StringFormat("{}{} x{}", out.empty() ? "" : "; ", why, count);
        return out.empty() ? std::string("no candidate searched") : out;
    }

    void NoteSpotReject(char const* search, std::string const& why)
    {
        if (g_spotRejects)
            ++g_spotRejects->counts[std::string(search) + ": " + why];
    }

    bool LeaderCombatSpotValid(Player* bot, PlayerbotAI* botAI, Unit* target, Position const& spot, bool melee,
                               std::vector<HazardZone> const& zones,
                               std::function<bool(Position const&)> const& keeps, bool sight)
    {
        return CombatSpotValidWhy(bot, botAI, target, spot, melee, zones, keeps, sight).empty();
    }

    // a new combat spot: melee behind the target, ranged at its own reach (LeaderShootReach) / the healer at LEADER_HEALER_YD
    // nearest the member's own side, each safe, in heal reach and (ranged) in sight of the target
    bool LeaderCombatSpot(Player* bot, PlayerbotAI* botAI, Unit* target, bool melee,
                          std::vector<HazardZone> const& zones, std::function<bool(Position const&)> const& keeps,
                          Position& out, std::vector<ObjectGuid> const* fightMobs)
    {
        std::optional<Position> fallback;  // [lead18] new ground, when no cleared spot fits
        static constexpr float TURNS[] = {0.0f,  0.35f, -0.35f, 0.7f,  -0.7f, 1.05f, -1.05f, 1.4f,
                                          -1.4f, 1.75f, -1.75f, 2.1f,  -2.1f, 2.45f, -2.45f, 3.14f};
        float const base = melee ? target->GetOrientation() + float(M_PI) : target->GetAngle(bot);
        float const distance = melee ? std::max(1.5f, target->GetCombatReach() + bot->GetCombatReach() * 0.5f)
                               : PlayerbotAI::IsHeal(bot) ? LEADER_HEALER_YD
                                                          : LeaderCastReach(bot, target) - 2.0f;  // [lead660]
        // [lead452] b205 / b206 rfc-h Oggleflint: the ranged damage dealers stood 85-90 s at 0 damage (5F the warlock
        // and the SV hunter, 5E the warlock; "combat_spot_none ... first spot") - every point of the one ring at its
        // reach minus 2 yd round the boss on his ledge fell off it (the ground more than 4 yd below him) and the
        // rings round the member, on the floor below, saw nothing. A ranged spot fits anywhere in its band
        // (LeaderCombatSpotValid: half its reach .. its reach, out of the target's melee): when the outer ring has
        // none, the rings inside the band are searched too, the search step of LeaderNearestCombatSpot's rings
        constexpr float RING_STEP_YD = 3.0f;
        // [lead492] b209 rfc-h: the healers' one ring at LEADER_HEALER_YD lost 3-16 of its 16 points "out of the
        // healer's band" (the rays hit RFC's walls short of 20 yd, 5C 201.2 s Rtpriab x16); the healer's band reaches
        // down to LEADER_HEALER_YD - 6 (CombatSpotValidWhy), its rings inside are searched too
        float const inner = melee                     ? distance
                            : PlayerbotAI::IsHeal(bot) ? LEADER_HEALER_YD - 6.0f
                                                       : LeaderRangedMinGap(bot, target) + 1.0f;  // [lead463]
        Map* map = target->GetMap();
        for (float ring = distance; ring >= inner || ring == distance; ring -= RING_STEP_YD)
        {
            // [lead902] user 2026-11-03: of the ring's fitting points the one nearest by the member's real walk
            std::optional<Position> best;
            float bestWalk = 0.0f;
            for (float turn : TURNS)
            {
                if (melee && std::fabs(turn) > 1.2f)
                    break;  // behind it only
                float const angle = base + turn;
                float x = target->GetPositionX() + std::cos(angle) * ring;
                float y = target->GetPositionY() + std::sin(angle) * ring;
                float z = target->GetPositionZ();
                if (!map->CheckCollisionAndGetValidCoords(target, target->GetPositionX(), target->GetPositionY(),
                                                          target->GetPositionZ(), x, y, z))
                {
                    NoteSpotReject("ring", "no ground");
                    continue;
                }
                // [lead463] a melee spot is on the target's floor; a ranged / healer one anywhere its ground sees the
                // target (the sight test below; the walk there is checkedMove's): b207 "ring: off the target's floor"
                // took 3-19 candidates of a search, on RFC's slopes round the boss ledges
                if (melee && std::fabs(z - target->GetPositionZ()) > 4.0f)
                {
                    NoteSpotReject("ring", "off the target's floor");
                    continue;
                }
                Position const spot(x, y, z);
                if (best && bot->GetExactDist(spot) >= bestWalk)  // [lead902] no walk to it can be shorter
                    continue;
                if (std::string const why = CombatSpotValidWhy(bot, botAI, target, spot, melee, zones, keeps, true);
                    !why.empty())
                {
                    NoteSpotReject("ring", why);
                    continue;
                }
                // [lead214] a fresh ranged / healer spot is out of every fight mob's melee reach too (lead210), else
                // it breaks on the next look
                if (!melee && fightMobs && !LeaderOutOfMobsMelee(bot, spot, *fightMobs))
                {
                    NoteSpotReject("ring", "inside a fight mob's melee reach");
                    continue;
                }
                // [lead490] [lead492] a fight spot is judged by how deep it lies in an idle mob's reach against the
                // fight there (LeaderFightAggroAt)
                if (int const candidate = LeaderCandidate(bot, spot, fallback, target);
                    candidate != 2)
                {
                    if (!candidate)
                        NoteSpotReject("ring", "an idle mob's aggro reach");
                    continue;
                }
                if (float const walk = Leader::WalkYd(bot, spot); !best || walk < bestWalk)
                {
                    best = spot;
                    bestWalk = walk;
                }
            }
            if (best)
            {
                out = *best;
                return true;
            }
            if (fallback)
            {
                out = *fallback;
                return true;
            }
            if (ring < inner)
                break;
        }
        // [lead580] no melee spot behind the target fits (the ring's checks, its sight among them): the melee place in
        // front of it in its reach and sight (LeaderMeleePlace) when it is safe and keeps the heal reach - the spot
        // breaks "in front of the target" only while a place behind fits
        if (melee)
        {
            Position at;
            bool behind = true;
            if (LeaderMeleePlace(bot, target, 1.0f, at, &behind) && !behind &&
                LeaderSpotSafe(bot, botAI, zones, at.GetPositionX(), at.GetPositionY(), at.GetPositionZ()) &&
                keeps(at))
            {
                out = at;
                return true;
            }
            NoteSpotReject("front", "no melee place in reach and sight in front of the target");
        }
        // [lead471] b208 rfc-h: combat_spot_none "ring: out of its band x105" / "ring: no ground x91" for hunters on a
        // Ragefire Trogg in RFC's tunnels - every straight ray from the target hit the tunnel wall inside the band's
        // floor (the hunter's dead zone), so no ring point was left in it. The ground the member walks to the target
        // on is a band too: the member's own route to the target, point by point at the ring step from its end, the
        // first point that fits (the shortest walk); the same checks as a ring point
        if (!melee)
        {
            Leader::RouteResult const route = Leader::Route(bot, target->GetPosition());
            if (!route.found || route.points.empty())
                NoteSpotReject("walk", "no route to the target");
            else
            {
                std::vector<G3D::Vector3> along;
                along.push_back(route.points.front());
                for (size_t i = 1; i < route.points.size(); ++i)
                {
                    G3D::Vector3 const from = route.points[i - 1];
                    G3D::Vector3 const leg = route.points[i] - from;
                    float const length = leg.length();
                    for (float step = RING_STEP_YD; step < length; step += RING_STEP_YD)
                        along.push_back(from + leg * (step / length));
                    along.push_back(route.points[i]);
                }
                for (G3D::Vector3 const& point : along)
                {
                    Position const spot(point.x, point.y, point.z);
                    if (std::string const why = CombatSpotValidWhy(bot, botAI, target, spot, melee, zones, keeps, true);
                        !why.empty())
                    {
                        NoteSpotReject("walk", why);
                        continue;
                    }
                    if (fightMobs && !LeaderOutOfMobsMelee(bot, spot, *fightMobs))
                    {
                        NoteSpotReject("walk", "inside a fight mob's melee reach");
                        continue;
                    }
                    if (int const candidate = LeaderCandidate(bot, spot, fallback, target);  // [lead492]
                        candidate != 2)
                    {
                        if (!candidate)
                            NoteSpotReject("walk", "an idle mob's aggro reach");
                        continue;
                    }
                    out = spot;
                    return true;
                }
                if (fallback)
                {
                    out = *fallback;
                    return true;
                }
            }
        }
        return false;
    }

    // [lead21] a mob of the fight: a hostile creature the tank may attack, never a player's summon or guardian
    // (slabs-h 20261026-195203: the tank was told to take back the mage's Mirror Image 1187 times)
    bool LeaderFightMob(Player* tank, Unit* unit)
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        return creature && tank->IsValidAttackTarget(creature) && !creature->IsCharmedOwnedByPlayerOrPlayer() &&
               !creature->GetOwnerGUID().IsPlayer() && !creature->GetCharmerOrOwnerPlayerOrPlayerItself();
    }

    // [lead18] the nearest walking patrol (idle, a waypoint walker or on the move, not of the committed pull)
    // around the pull spot or the tank; `gap`: its distance to the nearer of the two, `reach`: its aggro reach
    Creature* LeaderPatrolNear(Player* tank, Position const& spot, PullPlanner::Decision const& d, float& gap,
                               float& reach)
    {
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(tank, 90.0f);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(tank, units, check);
        Cell::VisitObjects(tank, searcher, 90.0f);
        Creature* best = nullptr;
        for (Unit* unit : units)
        {
            Creature* creature = unit->ToCreature();
            if (!creature || !creature->IsAlive() || creature->IsInCombat() ||
                !tank->IsValidAttackTarget(creature) ||
                creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) ||
                creature->GetDefaultMovementType() != WAYPOINT_MOTION_TYPE ||  // [lead24] a wanderer is a target
                creature->GetGUID() == d.planTarget || creature->GetGUID() == d.attack ||
                creature->GetGUID() == d.lookTarget || creature->GetGUID() == d.verdictTarget ||
                std::find(d.verdictPack.begin(), d.verdictPack.end(), creature->GetGUID()) != d.verdictPack.end() ||
                std::find(d.pack.begin(), d.pack.end(), creature->GetGUID()) != d.pack.end())
                continue;
            float const closest = std::min(creature->GetExactDist(spot), creature->GetExactDist(tank));
            if (!best || closest < gap)
            {
                best = creature;
                gap = closest;
                reach = creature->GetAggroRange(tank);
            }
        }
        return best;
    }

    // [lead18] LEADER_EDGE_FIRST: the room around `picked` (idle mobs within 40 yd of it, patrols out) as packs
    // (linked within 8 yd); the edge packs have the fewest other packs within 20 yd. The pick moves to the edge
    // pack nearest the tank when `picked` is not on the edge. `order`: the room, edge first (room_order).
    Unit* LeaderEdgePick(Player* tank, Unit* picked, std::string& why, JsonArray& order, std::string& sig)
    {
        std::list<Creature*> idle;
        IdleAggroCheck check{tank};
        Acore::CreatureListSearcher<IdleAggroCheck> searcher(picked, idle, check);
        Cell::VisitObjects(picked, searcher, 40.0f);
        std::vector<Creature*> mobs;
        for (Creature* mob : idle)
            if (mob->GetDefaultMovementType() != WAYPOINT_MOTION_TYPE &&  // [lead24] wanderers are room mobs
                std::fabs(mob->GetPositionZ() - picked->GetPositionZ()) < 6.0f && tank->GetExactDist(mob) < 80.0f)
                mobs.push_back(mob);
        if (mobs.size() < 4)
            return nullptr;
        std::vector<size_t> root(mobs.size());
        for (size_t i = 0; i < root.size(); ++i)
            root[i] = i;
        std::function<size_t(size_t)> const find = [&](size_t i)
        { return root[i] == i ? i : root[i] = find(root[i]); };
        for (size_t i = 0; i < mobs.size(); ++i)
            for (size_t j = i + 1; j < mobs.size(); ++j)
                if (mobs[i]->GetExactDist2d(mobs[j]) <= 8.0f)
                    root[find(i)] = find(j);
        std::map<size_t, std::vector<Creature*>> packs;
        for (size_t i = 0; i < mobs.size(); ++i)
            packs[find(i)].push_back(mobs[i]);
        if (packs.size() < 3)
            return nullptr;
        struct Pack
        {
            std::vector<Creature*> mobs;
            uint32 neighbors = 0;
            float tankDist = 0.0f;
            Creature* nearest = nullptr;
            bool hasPicked = false;
        };
        std::vector<Pack> room;
        for (auto& [key, members] : packs)
        {
            Pack pack;
            pack.mobs = members;
            for (Creature* mob : members)
            {
                float const dist = tank->GetExactDist(mob);
                if (!pack.nearest || dist < pack.tankDist)
                {
                    pack.nearest = mob;
                    pack.tankDist = dist;
                }
                pack.hasPicked |= mob == picked;
            }
            room.push_back(pack);
        }
        for (Pack& a : room)
            for (Pack const& b : room)
            {
                if (&a == &b)
                    continue;
                bool close = false;
                for (Creature* x : a.mobs)
                    for (Creature* y : b.mobs)
                        close |= x->GetExactDist2d(y) <= 20.0f;
                a.neighbors += close ? 1 : 0;
            }
        std::sort(room.begin(), room.end(), [](Pack const& a, Pack const& b)
                  { return a.neighbors != b.neighbors ? a.neighbors < b.neighbors : a.tankDist < b.tankDist; });
        for (Pack const& pack : room)
        {
            order.Raw(JsonObject().Str("first", pack.nearest->GetName()).Num("size", int64(pack.mobs.size()))
                          .Num("neighbors", pack.neighbors).Real("dist", pack.tankDist)
                          .Real("x", pack.nearest->GetPositionX()).Real("y", pack.nearest->GetPositionY())
                          .Bool("picked", pack.hasPicked).Build());
            sig += std::to_string(pack.nearest->GetGUID().GetCounter()) + ",";
        }
        Pack const& edge = room.front();
        Pack const* mine = nullptr;
        for (Pack const& pack : room)
            if (pack.hasPicked)
                mine = &pack;
        if (!mine || mine->neighbors <= edge.neighbors)
            return nullptr;
        why = Acore::StringFormat("{} ({} mobs, {} packs beside it) is the room's edge; {} has {}",
                                  edge.nearest->GetName(), edge.mobs.size(), edge.neighbors, picked->GetName(),
                                  mine->neighbors);
        return edge.nearest;
    }

    // [lead18] a spot the member walks to: a path from where it stands, no idle mob (outside the pull) reaching it
    bool LeaderMemberWalks(Player* bot, Position const& spot, std::list<Creature*> const& idle,
                           std::vector<ObjectGuid> const& pack)
    {
        Leader::RouteResult const route = Leader::Route(bot, spot);  // [lead99]
        if (!route.found || !route.complete)
            return false;
        std::vector<G3D::Vector3> const& points = route.points;
        for (size_t i = 0; i < points.size(); ++i)
        {
            Position const at(points[i].x, points[i].y, points[i].z);
            for (Creature* mob : idle)
                if (std::find(pack.begin(), pack.end(), mob->GetGUID()) == pack.end() &&
                    IdleMobReachesSpot(bot, mob, at))
                    return false;
        }
        return true;
    }

    // [lead45] a boss is always walked to: never a refused pick (its hold is a readiness wait, not a refusal)
    bool IsBossLook(Player* tank, ObjectGuid guid)
    {
        Creature const* creature = tank->GetMap() ? tank->GetMap()->GetCreature(guid) : nullptr;
        return creature && (creature->IsDungeonBoss() || creature->isWorldBoss());
    }

    // [lead63] user: «берите физ. атаку / маг. атаку того, кто бьёт» - an attacker's expected damage per second on
    // `victim`, from the server's numbers: its melee damage range (the unit's final min / max damage) over its
    // attack time, after the victim's armor (Unit::CalcArmorReducedDamage at the attacker's level), times the
    // swings that land (1 - the victim's dodge, parry and miss chance), minus the blocked share (block chance x
    // shield block value); plus the spell it is casting now (its school damage effects over its cast time, at
    // least the global cooldown). `spell`: that spell part
    float AttackerDps(Unit* attacker, Unit* victim, float& spell)
    {
        spell = 0.0f;
        float const minDamage = attacker->GetFloatValue(UNIT_FIELD_MINDAMAGE);
        float const maxDamage = attacker->GetFloatValue(UNIT_FIELD_MAXDAMAGE);
        float const swingSec = std::max(attacker->GetAttackTime(BASE_ATTACK) / 1000.0f, 0.1f);
        uint32 const raw = uint32(std::max(0.0f, (minDamage + maxDamage) / 2.0f));
        float const afterArmor =
            float(Unit::CalcArmorReducedDamage(attacker, victim, raw, nullptr, attacker->GetLevel(), BASE_ATTACK));
        float const avoid = std::clamp((victim->GetUnitDodgeChance() + victim->GetUnitParryChance() +
                                        victim->GetUnitMissChance(BASE_ATTACK)) / 100.0f, 0.0f, 1.0f);
        float const blocked = victim->GetUnitBlockChance() / 100.0f * float(victim->GetShieldBlockValue());
        float const melee = std::max(0.0f, afterArmor * (1.0f - avoid) - blocked) / swingSec;
        if (Spell const* casting = attacker->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (SpellInfo const* info = casting->GetSpellInfo())
            {
                float damage = 0.0f;
                for (SpellEffectInfo const& effect : info->GetEffects())
                    if (effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
                        damage += float(effect.CalcValue(attacker));
                spell = damage / std::max(info->CalcCastTime(attacker) / 1000.0f, 1.5f);
            }
        return melee + spell;
    }

    // [lead64] where a melee member waits at a pull: short of the meeting point on its own side by
    // LEADER_LOOSE_TANK_YD (next to the tank), never into the mob before the tank has hit it
    Position LeaderMeleeWait(Player* bot, Position const& meet)
    {
        float const dx = bot->GetPositionX() - meet.GetPositionX();
        float const dy = bot->GetPositionY() - meet.GetPositionY();
        float const length = std::hypot(dx, dy);
        if (length <= LEADER_LOOSE_TANK_YD)
            return bot->GetPosition();
        return Position(meet.GetPositionX() + dx / length * LEADER_LOOSE_TANK_YD,
                        meet.GetPositionY() + dy / length * LEADER_LOOSE_TANK_YD, meet.GetPositionZ());
    }

    // [lead42] how many members refused their Attack on `unit` this fight
    int64 RefusedCount(std::map<ObjectGuid, std::set<ObjectGuid>> const& refused, Unit const* unit)
    {
        auto const found = refused.find(unit->GetGUID());
        return found == refused.end() ? 0 : int64(found->second.size());
    }

    // [lead40] the leader's reachability verdict of a mob for `walker`: the route walk to it (EvaluatePath) and the
    // skip_target detour metric (DETOUR_RATIO / DETOUR_EXTRA). Empty: reachable on a fair walk; else why not
    // [lead79] `onRoute`: the mob lies on the route's path (Retarget's route rules): the route IS that walk, so
    // only "no walk at all" refuses it - a winding route is no detour (b122: "detour (534 yd for 102 yd)" on the
    // way to Taragaman, which b100 walked fine)
    std::string LeaderWalkVerdict(Player* walker, Creature* mob, bool onRoute)
    {
        // [lead99] the one route search (Leader::Route); the verdict's text names the path's source and whether
        // it is complete (b138 could not tell a partial path)
        Leader::RouteResult const path = Leader::Route(walker, mob->GetPosition());
        float const straight = walker->GetExactDist(mob);
        // a partial walk still counts when it ends nearer the mob than the walker stands (RoutePath's rule)
        float const left = path.found && !path.points.empty()
                               ? (path.points.back() - G3D::Vector3(mob->GetPositionX(), mob->GetPositionY(),
                                                                    mob->GetPositionZ()))
                                     .length()
                               : straight;
        bool const reachable = path.found && (path.complete || left + 5.0f < straight);
        if (!reachable)
            return Acore::StringFormat("unreachable (no walk for {:.0f} yd; {})", straight, path.Describe());
        float const length = path.length + (path.complete ? 0.0f : left);
        if (!onRoute && length > std::max(DETOUR_RATIO * straight, straight + DETOUR_EXTRA))
            return Acore::StringFormat("detour ({:.0f} yd for {:.0f} yd; {})", length, straight, path.Describe());
        return std::string();
    }

    // [lead39] a walk on the walker's floor: a full path, no step down or up over 2.5 yd (no ledge drop)
    bool LeaderWalkStaysOnFloor(Player* bot, Position const& to)
    {
        if (std::fabs(to.GetPositionZ() - bot->GetPositionZ()) > 4.0f)
            return false;
        Leader::RouteResult const route = Leader::Route(bot, to);  // [lead99]
        if (!route.found || !route.complete)
            return false;
        std::vector<G3D::Vector3> const& points = route.points;
        for (size_t i = 1; i < points.size(); ++i)
            if (std::fabs(points[i].z - points[i - 1].z) > 2.5f)
                return false;
        return true;
    }

    // [lead39] fight_drag: the tank's fight moved by the leader (from, to, the distance, off the meeting point)
    void LogFightDrag(DecisionRun const& run, Player* tank, Position const& to, Position const& meet,
                      char const* why)
    {
        Decisions().Write(run, "leader", "fight_drag", tank->GetGUID(), tank->GetName(), true,
                                      JsonObject().Str("why", why).Real("fromX", tank->GetPositionX())
                                          .Real("fromY", tank->GetPositionY()).Real("toX", to.GetPositionX())
                                          .Real("toY", to.GetPositionY())
                                          .Real("distance", tank->GetExactDist(to))
                                          .Real("offMeetingPoint", meet.GetExactDist(to)));
    }

    // [lead991] the spec's rotation spells (LeaderSpellReach's list): their reach, their school
    std::vector<char const*> LeaderRotationSpellNames(Player* bot)
    {
        std::vector<char const*> spells;
        uint8 const tab = AiFactory::GetPlayerSpecTab(bot);
        switch (bot->getClass())
        {
            case CLASS_MAGE:
                spells = tab == MAGE_TAB_ARCANE ? std::vector<char const*>{"arcane blast", "arcane missiles"}
                         : tab == MAGE_TAB_FIRE ? std::vector<char const*>{"fireball", "living bomb"}
                                                : std::vector<char const*>{"frostbolt", "ice lance"};
                break;
            case CLASS_WARLOCK:
                spells = tab == WARLOCK_TAB_AFFLICTION ? std::vector<char const*>{"shadow bolt", "haunt", "corruption"}
                         : tab == WARLOCK_TAB_DESTRUCTION ? std::vector<char const*>{"incinerate", "immolate"}
                                                          : std::vector<char const*>{"shadow bolt", "immolate"};
                break;
            case CLASS_PRIEST:
                if (tab == PRIEST_TAB_SHADOW)
                    spells = {"mind flay", "mind blast", "vampiric touch"};
                break;
            case CLASS_DRUID:
                if (tab == DRUID_TAB_BALANCE)
                    spells = {"wrath", "starfire", "moonfire"};
                break;
            case CLASS_SHAMAN:
                if (tab == SHAMAN_TAB_ELEMENTAL)
                    spells = {"lightning bolt", "lava burst", "flame shock"};
                break;
            case CLASS_HUNTER:
                spells = {"steady shot"};
                break;
            default:
                break;
        }
        return spells;
    }

    // [lead18] the member's spell reach to its target (a hunter 35 yd, a caster 30 yd)
    // [lead660] b218 rfc-h (8 runs, fight_breakdown.py): ranged damage dealers walked 305 s "respot: out of spell
    // range of the target" and 260 s to first spots for the same reason, 46 % of those re-spots at an edge gap
    // (Unit::GetDistance) under 30 yd - where the game casts. The reach is the spec's rotation spells' own range with
    // the member's talents (SpellInfo::GetMaxRange applies SPELLMOD_RANGE as Spell::CheckRange does: Arctic Reach,
    // Shadow Reach, Destructive Reach, Nature's Reach, Hawk Eye, Flame Throwing), the shortest of the ones it knows;
    // the fixed 30 / 35 yd only without them
    float LeaderSpellReach(Player* bot)
    {
        float const fixed = bot->getClass() == CLASS_HUNTER ? 35.0f : 30.0f;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return fixed;
        // the talents the member has (a respec changes them): the reach is computed again only then
        uint64 talents = uint64(bot->GetLevel()) << 56 | uint64(bot->GetActiveSpec()) << 48;
        for (auto const& [spellId, talent] : bot->GetTalentMap())
            if (talent && talent->State != PLAYERSPELL_REMOVED && talent->IsInSpec(bot->GetActiveSpec()))
                talents += uint64(spellId) * 2654435761ull;
        struct Known
        {
            uint64 talents;
            float reach;
        };
        thread_local std::unordered_map<ObjectGuid, Known> known;
        if (auto const it = known.find(bot->GetGUID()); it != known.end() && it->second.talents == talents)
            return it->second.reach;
        std::vector<char const*> const spells = LeaderRotationSpellNames(bot);
        float reach = 0.0f;
        auto const take = [&](SpellInfo const* info)
        {
            if (!info)
                return;
            float const range = info->GetMaxRange(false, bot);
            if (range > 0.0f)
                reach = reach > 0.0f ? std::min(reach, range) : range;
        };
        for (char const* name : spells)
            if (uint32 const id = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", name)->Get())
                take(sSpellMgr->GetSpellInfo(id));
        // Auto Shot (no "spell id" name): the hunter's ranged weapon shot, its range mods too
        if (bot->getClass() == CLASS_HUNTER && bot->GetWeaponForAttack(RANGED_ATTACK, true))
            take(sSpellMgr->GetSpellInfo(75));
        if (reach <= 0.0f)
            reach = fixed;
        if (known.size() > 4096)
            known.clear();
        known[bot->GetGUID()] = {talents, reach};
        return reach;
    }

    bool LeaderRotationImmune(Player* bot, Unit* mob)
    {
        PlayerbotAI* botAI = mob ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!botAI || bot->getClass() == CLASS_HUNTER)
            return false;
        // the schools of its rotation spells, kept while its spells and spec stay the same (a lookup per call)
        struct Schools
        {
            size_t spells = 0;
            uint8 spec = 0;
            std::vector<SpellSchoolMask> masks;
        };
        thread_local std::unordered_map<ObjectGuid, Schools> known;
        Schools& schools = known[bot->GetGUID()];
        if (schools.spells != bot->GetSpellMap().size() || schools.spec != bot->GetActiveSpec() ||
            schools.masks.empty())
        {
            schools = {bot->GetSpellMap().size(), bot->GetActiveSpec(), {}};
            for (char const* name : LeaderRotationSpellNames(bot))
                if (uint32 const id = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", name)->Get())
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(id))
                        schools.masks.push_back(info->GetSchoolMask());
        }
        for (SpellSchoolMask const mask : schools.masks)
            if (!mob->IsImmunedToDamageOrSchool(mask))
                return false;
        return !schools.masks.empty();
    }

    // [lead991] the reach on a mob its rotation's school cannot hurt: the shortest range of the spells it knows that
    // do hurt it and that it can cast on and on (a mana damage spell on one target with no cooldown of its own: the
    // fire mage's Frostbolt, Arcane Missiles), its talents' range mods in; -1 without one
    float LeaderImmuneReach(Player* bot, Unit* mob)
    {
        uint32 immune = 0;
        for (uint8 school = SPELL_SCHOOL_NORMAL; school < MAX_SPELL_SCHOOL; ++school)
            if (mob->IsImmunedToDamageOrSchool(SpellSchoolMask(1 << school)))
                immune |= 1u << school;
        struct Known
        {
            size_t spells;
            float reach;
        };
        thread_local std::map<std::pair<ObjectGuid, uint32>, Known> known;
        if (auto const it = known.find({bot->GetGUID(), immune});
            it != known.end() && it->second.spells == bot->GetSpellMap().size())
            return it->second.reach;
        float reach = -1.0f;
        for (auto const& [spellId, spell] : bot->GetSpellMap())
        {
            if (!spell || spell->State == PLAYERSPELL_REMOVED || !spell->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || info->IsPassive() || info->IsPositive() || info->PowerType != POWER_MANA ||
                info->IsAffectingArea() || (uint32(info->GetSchoolMask()) & immune) ||
                std::max(info->RecoveryTime, info->CategoryRecoveryTime) > 0 ||
                info->CalcPowerCost(bot, info->GetSchoolMask()) <= 0)
                continue;
            bool damages = info->IsChanneled();
            for (SpellEffectInfo const& effect : info->GetEffects())
                damages |= effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE;
            float const range = damages ? info->GetMaxRange(false, bot) : 0.0f;
            if (range > 0.0f)
                reach = reach > 0.0f ? std::min(reach, range) : range;
        }
        if (known.size() > 4096)
            known.clear();
        known[{bot->GetGUID(), immune}] = {bot->GetSpellMap().size(), reach};
        return reach;
    }

    float LeaderCastReach(Player* bot, Unit* target)
    {
        // [lead991] b228 rfc-h 5H-8 32-42 / 484-604 s: the fire mage stood 30-38 yd from a fire-immune Molten
        // Elemental ("fireball ... target immune", "frostbolt ... out of range (37.2 > 30.0)") and wanded 40 s a run:
        // its spot read Fireball's 35 yd. On a mob its rotation's school cannot hurt the reach is the spells that do
        if (target && LeaderRotationImmune(bot, target))
            if (float const reach = LeaderImmuneReach(bot, target); reach > 0.0f)
                return reach + bot->GetCombatReach() + target->GetCombatReach();
        // [lead660] Unit::IsWithinCombatRange, the check Spell::CheckRange casts by: the 3D gap between the centres
        // under the spell's range plus both combat reaches
        return std::max(LeaderShootReach(bot), LeaderSpellReach(bot)) + bot->GetCombatReach() +
               (target ? target->GetCombatReach() : 0.0f);
    }

    // [lead18] why a held combat spot no longer fits ("" = it does): out of the member's reach of the target
    // (melee: of its melee reach), out of sight (ranged), a hazard, a cleave, lava / a drop-off, the heal reach
    std::string LeaderCombatSpotBreaks(Player* bot, PlayerbotAI* botAI, Unit* target, Position const& spot,
                                       bool melee, std::vector<HazardZone> const& zones,
                                       std::function<bool(Position const&)> const& keeps, bool sight,
                                       std::vector<ObjectGuid> const* fightMobs, bool whereTheyStand)
    {
        float const gap = target->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY());
        if (melee && gap > target->GetCombatReach() + bot->GetCombatReach() + 1.5f)
            return "out of melee reach of the target";
        // [lead242] b189 rfc-h 5H: the cat never stood behind Bazzalan (no Shred all fight, "bh" false in every
        // sample) and 30 s not behind Oggleflint: the chase ended in melee reach at the boss's side and a melee spot in
        // reach was kept. A melee damage dealer's spot is behind the target (out of its front half: no parry, no
        // frontal attack, Shred / Backstab land) unless the target fights that member itself
        // [lead580] ... when a melee place behind it fits (LeaderMeleePlace: in its melee range and sight); with none
        // behind, a place in front in its reach and sight is the melee place
        if (melee && !PlayerbotAI::IsTank(bot) && target->GetVictim() != bot && target->HasInArc(float(M_PI), &spot))
        {
            Position behindAt;
            bool behind = false;
            if (LeaderMeleePlace(bot, target, 1.0f, behindAt, &behind) && behind)
                return "in front of the target";
        }
        // [lead580] b215 rfc-h 5E 365-411 s: the rogue's place at Oggleflint had no line of sight of him, every
        // ability failed 45 s: a melee spot the game does not see the target from breaks, on the same sight cadence
        if (sight && melee && !LeaderSeesFrom(bot, target, spot))
            return "no line of sight to the target";
        // [lead203] b181 (8 runs): 240 re-spots "inside its minimum range", and this rule (30 yd, the 2D gap) and the
        // Attack block's (28.5 yd SpellDistance, 3D) left each other's spots. A held ranged spot breaks only where the
        // member cannot cast from it (LeaderCastsFrom's range; the minimum range is Auto Shot's dead zone)
        float const castGap = target->GetExactDist(spot);
        float const castReach = LeaderCastReach(bot, target);  // [lead660]
        if (!melee && !PlayerbotAI::IsHeal(bot) && castGap > castReach - g_spotAcquireMargin)
            return "out of spell range of the target";
        if (!melee && !PlayerbotAI::IsHeal(bot) && bot->getClass() == CLASS_HUNTER &&
            castGap < LeaderRangedMinGap(bot, target))  // [lead157]
            return "inside its minimum range of the target";
        // [lead210] b182 rfc-h: ranged members within 10 yd of the tank in 33 % of the fight samples (b181 22 %), mobs
        // lost off the tank 5C 28 (b181 8). In a mob's melee reach the game moves it to a member at 110 % of its
        // victim's threat, not 130 % (ThreatManager): a ranged or healer spot there is no spot
        if (!melee && fightMobs && !LeaderOutOfMobsMelee(bot, spot, *fightMobs, whereTheyStand))
            return "inside a fight mob's melee reach";
        if (LeaderZoneAt(zones, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), LEADER_HAZARD_EDGE))
            return "a hazard covers it";
        if (NearHazard(bot->GetMap(), bot->GetPhaseMask(), spot.GetPositionX(), spot.GetPositionY(),
                       spot.GetPositionZ(), LEADER_TERRAIN_EDGE))
            return "lava / a drop-off";
        if (botAI && DungeonTactics::SpotInFrontalCleave(bot, botAI, spot.GetPositionX(), spot.GetPositionY(), 1.0f,
                                                         20.0f * float(M_PI) / 180.0f))
            return "a cleave covers it";
        if (!keeps(spot))
            return "heal reach";
        // [lead136] the same sight as LeaderCombatSpotValid (lead127: the game's own), else a spot it chose broke
        if (sight && !melee && !PlayerbotAI::IsHeal(bot) && !LeaderSeesFrom(bot, target, spot))
            return "no line of sight to the target";
        return std::string();
    }

    bool LeaderFrontCostsLess(Player* bot, Unit* target, std::string* why, Position* standAt)
    {
        Creature* mob = target ? target->ToCreature() : nullptr;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!mob || !botAI || !mob->IsAlive() || mob->IsDungeonBoss() || mob->isWorldBoss() ||
            bot->GetMap() != mob->GetMap())
            return false;
        // the rotations that need the back: Shred (cat form), Backstab as the Subtlety builder (Hemorrhage talent)
        if ((bot->getClass() == CLASS_DRUID && botAI->HasSpell("shred")) ||
            (bot->getClass() == CLASS_ROGUE && botAI->HasSpell("hemorrhage")))
            return false;
        // [lead680] where the member hits it from without the walk round: where it stands when it is in the game's
        // melee range and sight; out of reach the nearest place in reach on the line to the mob (the ring the chase
        // places stand on, LeaderMeleePlace), judged only once the fight stands (the mob on its victim, in its reach)
        bool const inReach = bot->IsWithinMeleeRange(mob) && bot->IsWithinLOSInMap(mob);
        Position nearAt = bot->GetPosition();
        if (!inReach)
        {
            Unit* const victim = mob->GetVictim();
            if (mob->isMoving() || !victim || !mob->IsWithinMeleeRange(victim))
                return false;
            float const ring = std::max(1.5f, mob->GetCombatReach() + bot->GetCombatReach() * 0.5f);
            float const angle = mob->GetAbsoluteAngle(bot);
            float x = mob->GetPositionX() + std::cos(angle) * ring;
            float y = mob->GetPositionY() + std::sin(angle) * ring;
            float z = mob->GetPositionZ();
            if (!mob->GetMap()->CheckCollisionAndGetValidCoords(mob, mob->GetPositionX(), mob->GetPositionY(),
                                                                 mob->GetPositionZ(), x, y, z) ||
                !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                return false;
            nearAt.Relocate(x, y, z);
            if (!LeaderMeleeReachesFrom(bot, mob, nearAt))
                return false;
        }
        // the near place in the mob's back half already: behind is the nearest place, no front to weigh
        if (!mob->HasInArc(float(M_PI), &nearAt))
            return false;
        Position behindAt;
        bool behind = false;
        if (!LeaderMeleePlace(bot, mob, 1.0f, behindAt, &behind) || !behind)
            return false;
        // The walk the chase takes (LeaderMoveOrderAction's Follow): the straight walk to its place behind the mob,
        // stopped as soon as the member stands in the mob's reach out of its front half (ChaseDoneHere); against it
        // the walk to the near place (none in reach)
        float const nearWalk = bot->GetExactDist(nearAt);
        float behindWalk = bot->GetExactDist(behindAt);
        {
            float const reach =
                std::max(mob->GetCombatReach() + bot->GetCombatReach() + 4.0f / 3.0f, float(NOMINAL_MELEE_RANGE));
            constexpr float STEP = 0.25f;
            float const whole = behindWalk;
            for (float done = STEP; done < whole; done += STEP)
            {
                float const share = done / whole;
                Position const at(bot->GetPositionX() + (behindAt.GetPositionX() - bot->GetPositionX()) * share,
                                  bot->GetPositionY() + (behindAt.GetPositionY() - bot->GetPositionY()) * share,
                                  bot->GetPositionZ() + (behindAt.GetPositionZ() - bot->GetPositionZ()) * share);
                if (!mob->HasInArc(float(M_PI), &at) && mob->GetExactDist(at) <= reach)
                {
                    behindWalk = done;
                    break;
                }
            }
        }
        // The front's loss, the core's melee roll (Unit::RollMeleeOutcomeAgainst): parry and block only from the
        // front (none with SPELL_AURA_IGNORE_HIT_DIRECTION: front and back are the same there), the parry chance less
        // the member's expertise (Player::GetExpertiseDodgeOrParryReduction), both less the weapon skill bonus
        // (0.04 % per point of weapon skill over the mob's max skill for its level: the level difference), none for
        // a mob flagged CREATURE_FLAG_EXTRA_NO_PARRY / _NO_BLOCK; a block takes the mob's block value off the hit
        float parry = 0.0f;
        float block = 0.0f;
        if (!mob->HasIgnoreHitDirectionAura())
        {
            float const skillBonus = 0.04f * (float(bot->GetWeaponSkillValue(BASE_ATTACK, mob)) -
                                              float(mob->GetMaxSkillValueForLevel(bot)));
            if (!mob->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_PARRY))
                parry = std::max(0.0f, mob->GetUnitParryChance() -
                                           bot->GetExpertiseDodgeOrParryReduction(BASE_ATTACK) - skillBonus);
            if (!mob->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_BLOCK))
                block = std::max(0.0f, mob->GetUnitBlockChance() - skillBonus);
        }
        float const avgHit =
            (bot->GetFloatValue(UNIT_FIELD_MINDAMAGE) + bot->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2.0f;
        float const blockShare = avgHit > 0.0f ? std::min(1.0f, float(mob->GetShieldBlockValue()) / avgHit) : 0.0f;
        float const lossShare = (parry + block * blockShare) / 100.0f;
        float const lifetime = ExpectedLifetime(botAI, mob);
        float const lossSec = lifetime * lossShare;
        float const speed = bot->GetSpeed(MOVE_RUN);
        float const extraSec = speed > 0.0f ? std::max(0.0f, behindWalk - nearWalk) / speed : 0.0f;
        if (why)
            *why = Acore::StringFormat("front loss {:.2f} s ({:.1f} % parry + {:.1f} % block x {:.2f} over {:.1f} s) "
                                       "vs the walk round {:.2f} s ({:.1f} yd behind vs {:.1f} yd {})",
                                       lossSec, parry, block, blockShare, lifetime, extraSec, behindWalk, nearWalk,
                                       inReach ? "where it stands" : "to the near place in reach");
        if (lossSec >= extraSec)
            return false;
        if (standAt)
            *standAt = nearAt;
        return true;
    }

    uint32 LeaderLeapSpell(Player* bot, float* leap)
    {
        // the mage's Blink (1953, one rank): its forward leap (SPELL_EFFECT_LEAP, the effect's radius: 20 yd)
        static constexpr uint32 BLINK = 1953;
        if (!bot || bot->getClass() != CLASS_MAGE || !bot->HasSpell(BLINK))
            return 0;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(BLINK);
        if (!info)
            return 0;
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == SPELL_EFFECT_LEAP)
            {
                if (leap)
                    *leap = effect.CalcRadius(bot);
                return BLINK;
            }
        return 0;
    }

    namespace
    {
        // [lead691] the movement skills by their first rank (the highest rank the member knows is cast)
        constexpr uint32 SPELL_BLINK = 1953;
        constexpr uint32 SPELL_SPRINT = 2983;
        constexpr uint32 SPELL_SHADOWSTEP = 36554;
        constexpr uint32 SPELL_DASH = 1850;
        constexpr uint32 SPELL_CAT_FORM = 768;
        constexpr uint32 SPELL_MOONKIN_FORM = 24858;
        constexpr uint32 SPELL_FERAL_CHARGE_CAT = 49376;
        constexpr uint32 SPELL_GHOST_WOLF = 2645;
        constexpr uint32 SPELL_ASPECT_OF_THE_CHEETAH = 5118;
        constexpr uint32 SPELL_ASPECT_OF_THE_PACK = 13159;
        constexpr uint32 SPELL_POWER_WORD_SHIELD = 17;
        constexpr uint32 SPELL_WEAKENED_SOUL = 6788;
        constexpr uint32 SPELL_BODY_AND_SOUL = 64127;
        constexpr uint32 SPELL_DISENGAGE = 781;

        // the highest rank of `first`'s chain the member knows (0: none)
        uint32 KnownRank(Player* bot, uint32 first)
        {
            for (uint32 rank = sSpellMgr->GetLastSpellInChain(first); rank; rank = sSpellMgr->GetPrevSpellInChain(rank))
                if (bot->HasSpell(rank))
                    return rank;
            return 0;
        }

        // the global cooldown a cast starts (Spell::TriggerGlobalCooldown: StartRecoveryTime x cast speed, 1-1.5 s)
        float GcdSec(Player* bot, SpellInfo const* info)
        {
            float gcd = float(info->StartRecoveryTime) / 1000.0f;
            if (gcd > 0.0f)
                gcd = std::clamp(gcd * bot->GetFloatValue(UNIT_MOD_CAST_SPEED), 1.0f, 1.5f);
            return gcd;
        }

        uint32 ManaCost(Player* bot, SpellInfo const* info)
        {
            return info->PowerType == POWER_MANA ? uint32(std::max(0, info->CalcPowerCost(bot, info->GetSchoolMask())))
                                                 : 0;
        }

        // the member's mana after `cost` still covers `line` (% of max; < 0: no line); no mana pool: none needed
        bool ManaCovers(Player* bot, uint32 cost, float line, std::string& why)
        {
            uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
            if (!maxMana)
                return true;
            uint32 const mana = bot->GetPower(POWER_MANA);
            if (mana < cost)
                return false;
            float const after = 100.0f * float(mana - cost) / float(maxMana);
            if (line >= 0.0f && after < line)
                return false;
            why += Acore::StringFormat("; {} mana, {:.0f} % after it", cost, after) +
                   (line >= 0.0f ? Acore::StringFormat(" >= its line {:.0f} %", line) : std::string());
            return true;
        }

        // the amount of the spell's SPELL_AURA_MOD_INCREASE_SPEED (0: none)
        int32 SpeedAmount(Player* bot, SpellInfo const* info)
        {
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.IsAura(SPELL_AURA_MOD_INCREASE_SPEED))
                    return effect.CalcValue(bot);
            return 0;
        }

        // the run speed `unit` would have with a SPELL_AURA_MOD_INCREASE_SPEED of `amount` (Unit::UpdateSpeed: the
        // largest one of that aura counts, not their sum)
        float SpeedWith(Unit* unit, int32 amount)
        {
            float const speed = unit->GetSpeed(MOVE_RUN);
            if (unit->IsMounted())
                return speed;
            int32 const main = unit->GetMaxPositiveAuraModifier(SPELL_AURA_MOD_INCREASE_SPEED);
            return speed * float(100 + std::max(main, amount)) / float(100 + main);
        }

        // the seconds a walk of `length` yd takes at `fast` for `durSec` (< 0: for good), then at `speed`
        float WalkSec(float length, float speed, float fast, float durSec)
        {
            if (durSec < 0.0f || length <= fast * durSec)
                return length / fast;
            return durSec + (length - fast * durSec) / speed;
        }

        // one candidate: what it saves and costs on the walk (seconds)
        struct Pick
        {
            uint32 spell = 0;
            ObjectGuid on;  // Empty: on itself
            float saved = 0.0f;
            float cost = 0.0f;
            std::string why;
            float Net() const { return saved - cost; }
        };

        // a speed aura of `amount` for `durSec` (< 0: for good), cast in `castSec` with a global cooldown of `gcd`,
        // on the member's walk of `length` yd: the cast stands, the walk goes on in the global cooldown (a cost
        // only where the walk ends sooner), `extraCost` on top
        bool SpeedSaves(Player* bot, SpellInfo const* info, int32 amount, float durSec, float castSec, float gcd,
                        float extraCost, float length, Pick& pick)
        {
            float const speed = bot->GetSpeed(MOVE_RUN);
            float const fast = SpeedWith(bot, amount);
            if (speed <= 0.0f || fast <= speed || length <= 0.0f)
                return false;
            float const with = WalkSec(length, speed, fast, durSec);
            pick.spell = info->Id;
            pick.saved = length / speed - with;
            pick.cost = castSec + std::max(0.0f, gcd - with) + extraCost;
            pick.why = Acore::StringFormat("{}: {:.1f} s of a {:.0f} yd walk saved ({:.1f} -> {:.1f} yd/s{}) vs "
                                           "{:.1f} s cast / GCD after it",
                                           info->SpellName[0], pick.saved, length, speed, fast,
                                           durSec < 0.0f ? std::string() : Acore::StringFormat(" for {:.0f} s", durSec),
                                           pick.cost);
            return pick.saved > pick.cost;
        }

        // [lead690] the leap of `leap` yd ahead (or, `back`, behind the member's facing) along its walk to `to`:
        // the walk (the leader's route) at least the leap long and straight for the leap (its point the leap's
        // length along lies the leap off, within the member's combat reach), in sight, the leap's line and landing
        // clear of hazard zones, lava / drop-offs and idle mobs' aggro reach
        bool LeapSaves(Player* bot, SpellInfo const* info, float leap, bool back, Position const& to,
                       std::vector<HazardZone> const& zones, Pick& pick)
        {
            if (leap <= 0.0f || bot->IsRooted() || !bot->isMoving() || bot->IsNonMeleeSpellCast(false) ||
                bot->HasUnitState(UNIT_STATE_LOST_CONTROL) || bot->IsInFlight() || bot->GetTransport() ||
                bot->isSwimming())
                return false;
            // [lead691] a leap backwards only where the walk goes back: its spot behind the member's facing
            if (back)
            {
                float const turn = Position::NormalizeOrientation(bot->GetAbsoluteAngle(&to) - bot->GetOrientation());
                if (turn < float(M_PI) / 2.0f || turn > 1.5f * float(M_PI))
                    return false;
            }
            // a leap past the place is no saving: the walk left is at least the leap
            if (bot->GetExactDist2d(to.GetPositionX(), to.GetPositionY()) < leap)
                return false;
            Leader::RouteResult const route = Leader::Route(bot, to);  // [lead99] the leader's walk
            if (!route.found || !route.complete || route.points.size() < 2 || route.length < leap)
                return false;
            // The point `leap` yd along the walk; the leap goes straight, so it saves that much walk only when the
            // walk runs straight there: its point that far along lies the leap's length off (within the member's
            // own combat reach, the size the game gives it), on the same ground
            G3D::Vector3 const start(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            G3D::Vector3 land = route.points.back();
            float along = 0.0f;
            for (size_t i = 1; i < route.points.size(); ++i)
            {
                float const step = (route.points[i] - route.points[i - 1]).length();
                if (along + step >= leap)
                {
                    float const share = step > 0.0f ? (leap - along) / step : 0.0f;
                    land = route.points[i - 1] + (route.points[i] - route.points[i - 1]) * share;
                    break;
                }
                along += step;
            }
            float const off = (land - start).length();
            if (off < leap - bot->GetCombatReach())
                return false;
            // [lead980] b228 rfc-h 5D-4 (20261103-221518) 310.4 / 313.1 s: two Blinks on a 193 yd walk down the
            // Ragefire ramp were judged on the route's first 20 yd, while the member faces the walk's END before it
            // leaps (LeaderExternalAction: SetFacingTo the ordered spot, 121 yd off): the leaps went straight off
            // the ledge, 14 yd down into an idle pack of eight; the mage died in it at 337 s. The leap goes where
            // the member will face - toward `to` (a leap back turns its back to `to` and flies the same way): that
            // line must be the route's, its landing within the member's combat reach of the route's point
            {
                float const dx = to.GetPositionX() - start.x;
                float const dy = to.GetPositionY() - start.y;
                float const across = std::hypot(dx, dy);
                if (across <= 0.1f)
                    return false;
                float const faceX = start.x + dx / across * leap;
                float const faceY = start.y + dy / across * leap;
                if (std::hypot(faceX - land.x, faceY - land.y) > bot->GetCombatReach())
                    return false;
            }
            Map* map = bot->GetMap();
            if (!map->isInLineOfSight(start.x, start.y, start.z + 2.0f, land.x, land.y, land.z + 2.0f,
                                      bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                return false;
            // safe along the leap and where it lands: no hazard zone, no lava / drop-off, no idle mob's aggro reach
            constexpr uint32 SAMPLES = 4;
            for (uint32 i = 1; i <= SAMPLES; ++i)
            {
                G3D::Vector3 const at = start + (land - start) * (float(i) / float(SAMPLES));
                if (LeaderZoneAt(zones, at.x, at.y, at.z, LEADER_HAZARD_EDGE) ||
                    NearHazard(map, bot->GetPhaseMask(), at.x, at.y, at.z, LEADER_TERRAIN_EDGE))
                    return false;
            }
            if (LeaderAggroAt(bot, Position(land.x, land.y, land.z)))
                return false;
            // Seconds: the leap saves its length at the member's run speed; it costs its cast time and the global
            // cooldown it starts where the walk left after it ends sooner than that cooldown (no cast while walking)
            float const speed = bot->GetSpeed(MOVE_RUN);
            if (speed <= 0.0f)
                return false;
            float const gcd = GcdSec(bot, info);
            float const castSec = info->CalcCastTime(bot) / 1000.0f;
            float const left = (route.length - leap) / speed;
            pick.spell = info->Id;
            pick.saved = leap / speed;
            pick.cost = castSec + std::max(0.0f, gcd - left);
            pick.why = Acore::StringFormat("{}: {:.1f} s of a {:.0f} yd walk saved ({:.0f} yd leap{}) vs {:.1f} s "
                                           "cast / GCD after it",
                                           info->SpellName[0], pick.saved, route.length, leap,
                                           back ? " backwards" : "", pick.cost);
            return pick.saved > pick.cost;
        }

        // the leap length of a SPELL_EFFECT_LEAP_BACK (Spell::EffectLeapBack: MiscValue / 10 yd/s across,
        // BasePoints / 10 yd/s up; the flight lasts 2 x up / gravity)
        float LeapBackYd(Player* bot, SpellInfo const* info)
        {
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.Effect == SPELL_EFFECT_LEAP_BACK)
                {
                    float const across = float(effect.MiscValue) / 10.0f;
                    float const up = float(effect.CalcValue(bot)) / 10.0f;
                    return across * 2.0f * up / float(Movement::gravity);
                }
            return 0.0f;
        }

        bool Ready(Player* bot, uint32 spell) { return spell && bot->HasSpell(spell) && !bot->HasSpellCooldown(spell); }

        bool IsMoveSkill(uint32 spell)
        {
            if (!spell)
                return false;
            uint32 const first = sSpellMgr->GetFirstSpellInChain(spell);
            for (uint32 one : {SPELL_BLINK, SPELL_SPRINT, SPELL_SHADOWSTEP, SPELL_DASH, SPELL_FERAL_CHARGE_CAT,
                               SPELL_GHOST_WOLF, SPELL_ASPECT_OF_THE_CHEETAH, SPELL_ASPECT_OF_THE_PACK,
                               SPELL_POWER_WORD_SHIELD, SPELL_DISENGAGE})
                if (first == one || spell == one)
                    return true;
            return false;
        }

        // the External is the movement pass's own: a movement skill on the member itself (Empty or its guid), or
        // a gap closer onto a mob (a Power Word: Shield on another member is the fight's save, 3c)
        bool OwnMoveExternal(Player* bot, Leader::MemberEvents const& live)
        {
            if (!IsMoveSkill(live.externalSpell))
                return false;
            if (live.externalSpell == SPELL_SHADOWSTEP || live.externalSpell == SPELL_FERAL_CHARGE_CAT)
                return true;
            return live.externalTarget.IsEmpty() || live.externalTarget == bot->GetGUID();
        }

        // a movement skill of its own the member knows off cooldown (the route search only for those)
        bool AnyOwnSkillReady(Player* bot)
        {
            return Ready(bot, SPELL_BLINK) || Ready(bot, KnownRank(bot, SPELL_SPRINT)) ||
                   Ready(bot, KnownRank(bot, SPELL_DASH)) || Ready(bot, SPELL_GHOST_WOLF) ||
                   Ready(bot, SPELL_ASPECT_OF_THE_CHEETAH) || Ready(bot, SPELL_DISENGAGE) ||
                   (bot->GetAuraEffectOfRankedSpell(SPELL_BODY_AND_SOUL, EFFECT_0) &&
                    Ready(bot, KnownRank(bot, SPELL_POWER_WORD_SHIELD)));
        }

        // the member's walk the leader ordered now: its end and its length (the leader's route); false: none
        bool OrderedWalk(Player* bot, Player* tank, Leader::MemberEvents const& live, bool trails, Position& to,
                         float& length, std::string& what)
        {
            if (live.move == Leader::MoveEvent::MoveTo || live.move == Leader::MoveEvent::StandAt)
            {
                if (bot->GetExactDist(live.spot) <= live.tolerance)
                    return false;
                Leader::RouteResult const route = Leader::Route(bot, live.spot);
                if (!route.found)
                    return false;
                to = live.spot;
                length = route.length;
                what = live.moveWhy;
                return true;
            }
            // [lead17] a member the group waits for walks to the tank until it is back within LEADER_TRAIL_BACK_YD
            if (trails && tank && live.move == Leader::MoveEvent::Follow && live.followTarget == tank->GetGUID())
            {
                Leader::RouteResult const route = Leader::Route(bot, tank->GetPosition());
                if (!route.found || route.length <= Leader::LEADER_TRAIL_BACK_YD)
                    return false;
                to = tank->GetPosition();
                length = route.length - Leader::LEADER_TRAIL_BACK_YD;
                what = "trails the group";
                return true;
            }
            return false;
        }

        // the member's best movement skill for its own walk (`to`, `length`), in a fight or out of it
        // [lead905] user 2026-11-03 (37): a speed skill with a long recharge (Sprint, Dash) goes on the longest walk
        // leg the route plan has within its recharge: a leg ahead that begins before it is back and is longer than
        // the walk now holds it for that leg. `why`: the leg
        bool HeldForLongerLeg(Player* bot, SpellInfo const* info, float length,
                              std::vector<std::pair<float, float>> const& legs, std::string& why)
        {
            int32 rechargeMs = int32(std::max(info->RecoveryTime, info->CategoryRecoveryTime));
            bot->ApplySpellMod(info->Id, SPELLMOD_COOLDOWN, rechargeMs);
            float const rechargeSec = float(std::max(rechargeMs, 0)) / 1000.0f;
            for (auto const& [startSec, legYd] : legs)
                if (startSec > 0.0f && startSec < rechargeSec && legYd > length)
                {
                    why = Acore::StringFormat("{} held for the route's {:.0f} yd leg in {:.0f} s (this walk {:.0f} yd, "
                                              "recharge {:.0f} s)",
                                              info->SpellName[0], legYd, startSec, length, rechargeSec);
                    return true;
                }
            return false;
        }

        Pick BestOwnSkill(Player* bot, Position const& to, float length, std::vector<HazardZone> const& zones,
                          float manaLine, std::vector<std::pair<float, float>> const& legs)
        {
            Pick best;
            auto const legHolds = [&](SpellInfo const* info)
            {
                std::string why;
                if (!HeldForLongerLeg(bot, info, length, legs, why))
                    return false;
                thread_local std::map<uint64, std::string> legLogged;
                std::string& logged = legLogged[bot->GetGUID().GetRawValue()];
                if (std::string const key = why.substr(0, why.find(" (")); logged != key)
                {
                    logged = key;
                    LOG_INFO("playerbots", "[lead905] {}: {}", bot->GetName(), why);
                }
                return true;
            };
            auto const consider = [&](Pick& pick, uint32 cost)
            {
                if (pick.saved > pick.cost && (!best.spell || pick.Net() > best.Net()) &&
                    ManaCovers(bot, cost, manaLine, pick.why))
                    best = pick;
            };
            bool const combat = bot->IsInCombat();
            // the mage's Blink (lead690)
            if (Ready(bot, SPELL_BLINK))
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_BLINK))
                {
                    float leap = 0.0f;
                    LeaderLeapSpell(bot, &leap);
                    Pick pick;
                    if (LeapSaves(bot, info, leap, false, to, zones, pick))
                        consider(pick, ManaCost(bot, info));
                }
            // the rogue's Sprint
            if (uint32 const sprint = KnownRank(bot, SPELL_SPRINT); Ready(bot, sprint) && !bot->HasAura(sprint))
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(sprint); info && !legHolds(info))
                {
                    Pick pick;
                    if (SpeedSaves(bot, info, SpeedAmount(bot, info), info->GetMaxDuration() / 1000.0f,
                                   info->CalcCastTime(bot) / 1000.0f, GcdSec(bot, info), 0.0f, length, pick))
                        consider(pick, 0);
                }
            // the druid's Dash: in Cat Form (spell_dru_dash); a druid out of every form shifts first out of a fight
            // (Cat Form's global cooldown and mana; leaving it again is free: the form's cancel). A bear tank or a
            // Moonkin keeps its form: its own form upkeep takes it back within seconds and Dash's speed with it
            // (recalculated to 0 out of Cat Form), and in a fight the form is its role
            if (uint32 const dash = KnownRank(bot, SPELL_DASH); Ready(bot, dash) && !bot->HasAura(dash))
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(dash); info && !legHolds(info))
                {
                    bool const inCat = bot->GetShapeshiftForm() == FORM_CAT;
                    SpellInfo const* cat = sSpellMgr->GetSpellInfo(SPELL_CAT_FORM);
                    bool const shifts = !inCat && !combat && bot->GetShapeshiftForm() == FORM_NONE && cat &&
                                        bot->HasSpell(SPELL_CAT_FORM) && !PlayerbotAI::IsTank(bot) &&
                                        !bot->HasSpell(SPELL_MOONKIN_FORM);
                    if (inCat || shifts)
                    {
                        float const shiftGcd = shifts ? GcdSec(bot, cat) : 0.0f;
                        Pick pick;
                        // its amount in Cat Form (the aura's own value; spell_dru_dash_aura zeroes it outside)
                        if (SpeedSaves(bot, info, SpeedAmount(bot, info), info->GetMaxDuration() / 1000.0f, 0.0f,
                                       shiftGcd, 0.0f, length, pick))
                        {
                            if (shifts)
                                pick.why += Acore::StringFormat(" (Cat Form first: {:.1f} s GCD)", shiftGcd);
                            consider(pick, shifts ? ManaCost(bot, cat) : 0);
                        }
                    }
                }
            // the shaman's Ghost Wolf, out of a fight: its cast stands (2 s less Improved Ghost Wolf)
            if (!combat && Ready(bot, SPELL_GHOST_WOLF) && bot->GetShapeshiftForm() == FORM_NONE)
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_GHOST_WOLF))
                {
                    Pick pick;
                    if (SpeedSaves(bot, info, SpeedAmount(bot, info), -1.0f, info->CalcCastTime(bot) / 1000.0f,
                                   GcdSec(bot, info), 0.0f, length, pick))
                        consider(pick, ManaCost(bot, info));
                }
            // the hunter's Aspect of the Cheetah, out of a fight (Dazed on a hit): no global cooldown; its swap back
            // to the fight's aspect waits for the aspects' category cooldown where the walk ends sooner
            if (!combat && Ready(bot, SPELL_ASPECT_OF_THE_CHEETAH) && !LeaderInMoveAspect(bot))
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_ASPECT_OF_THE_CHEETAH))
                {
                    int32 const amount = SpeedAmount(bot, info);
                    float const fast = SpeedWith(bot, amount);
                    float const back = float(info->CategoryRecoveryTime) / 1000.0f;
                    float const swapBack = fast > 0.0f ? std::max(0.0f, back - length / fast) : 0.0f;
                    Pick pick;
                    if (SpeedSaves(bot, info, amount, -1.0f, info->CalcCastTime(bot) / 1000.0f, GcdSec(bot, info),
                                   swapBack, length, pick))
                        consider(pick, ManaCost(bot, info));
                }
            // the priest's Power Word: Shield on itself with Body and Soul (its proc's speed for its duration)
            if (AuraEffect const* talent = bot->GetAuraEffectOfRankedSpell(SPELL_BODY_AND_SOUL, EFFECT_0))
                if (uint32 const shield = KnownRank(bot, SPELL_POWER_WORD_SHIELD);
                    Ready(bot, shield) && !bot->HasAura(SPELL_WEAKENED_SOUL))
                {
                    SpellInfo const* info = sSpellMgr->GetSpellInfo(shield);
                    SpellInfo const* proc =
                        sSpellMgr->GetSpellInfo(talent->GetSpellInfo()->Effects[EFFECT_0].TriggerSpell);
                    Pick pick;
                    if (info && proc &&
                        SpeedSaves(bot, info, SpeedAmount(bot, proc), proc->GetMaxDuration() / 1000.0f,
                                   info->CalcCastTime(bot) / 1000.0f, GcdSec(bot, info), 0.0f, length, pick))
                    {
                        pick.why += Acore::StringFormat(" (its {} {})", proc->SpellName[0], proc->Id);
                        consider(pick, ManaCost(bot, info));
                    }
                }
            // the hunter's Disengage: in a fight (spell_hun_disengage), only on a walk backwards
            if (combat && Ready(bot, SPELL_DISENGAGE))
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_DISENGAGE))
                {
                    Pick pick;
                    if (LeapSaves(bot, info, LeapBackYd(bot, info), true, to, zones, pick))
                        consider(pick, ManaCost(bot, info));
                }
            return best;
        }

        // the melee member's gap closer onto the mob the leader told it to attack, which the tank holds
        // (Shadowstep, Feral Charge - Cat): the walk into its melee reach it saves at the run speed
        Pick BestGapCloser(Player* bot, Player* tank, Leader::MemberEvents const& live)
        {
            Pick best;
            if (!bot->IsInCombat() || live.attack != Leader::AttackEvent::Attack || live.target.IsEmpty())
                return best;
            Unit* target = ObjectAccessor::GetUnit(*bot, live.target);
            if (!target || !target->IsAlive() || !target->IsInCombat() || target->GetVictim() != tank ||
                bot->IsWithinMeleeRange(target) || !bot->IsWithinLOSInMap(target))
                return best;
            float const speed = bot->GetSpeed(MOVE_RUN);
            if (speed <= 0.0f)
                return best;
            float const dist = bot->GetExactDist(target);
            float const walk = std::max(0.0f, bot->GetDistance(target) - bot->GetMeleeRange(target));
            for (uint32 spell : {SPELL_SHADOWSTEP, SPELL_FERAL_CHARGE_CAT})
            {
                SpellInfo const* info = Ready(bot, spell) ? sSpellMgr->GetSpellInfo(spell) : nullptr;
                if (!info || (spell == SPELL_FERAL_CHARGE_CAT && bot->GetShapeshiftForm() != FORM_CAT))
                    continue;
                if (dist < info->GetMinRange() || dist > info->GetMaxRange(false, bot))
                    continue;
                int32 const cost = std::max(0, info->CalcPowerCost(bot, info->GetSchoolMask()));
                if (info->PowerType != POWER_MANA && int32(bot->GetPower(Powers(info->PowerType))) < cost)
                    continue;
                Pick pick;
                pick.spell = spell;
                pick.on = target->GetGUID();
                pick.saved = walk / speed;
                pick.cost = info->CalcCastTime(bot) / 1000.0f + GcdSec(bot, info);
                pick.why = Acore::StringFormat("{}: {:.1f} s of the {:.0f} yd walk to {} (on the tank) saved vs "
                                               "{:.1f} s cast / GCD",
                                               info->SpellName[0], pick.saved, walk, target->GetName(), pick.cost);
                if (pick.saved > pick.cost && (!best.spell || pick.Net() > best.Net()))
                    best = pick;
            }
            return best;
        }

    }

    float LeaderMoveManaLine(Player* member, bool fighting, float fightSecLeft, bool bossFight, Unit* waitBoss)
    {
        if (!member->GetMaxPower(POWER_MANA))
            return -1.0f;
        if (fighting)
            return RestState::FightManaLine(member, fightSecLeft, 0.0f, nullptr, bossFight);
        if (waitBoss && waitBoss->IsAlive())
            return PullReadiness::MemberBossManaLine(member, waitBoss);
        return RestState::OwnManaLine(member);
    }

    bool LeaderInMoveAspect(Player* hunter)
    {
        return hunter && (hunter->HasAura(SPELL_ASPECT_OF_THE_CHEETAH, hunter->GetGUID()) ||
                          hunter->HasAura(SPELL_ASPECT_OF_THE_PACK, hunter->GetGUID()));
    }

    bool LeaderMoveFormLeave(Player* bot, uint32 spell, ObjectGuid target)
    {
        return spell == SPELL_GHOST_WOLF && target == bot->GetGUID() && bot->HasAura(SPELL_GHOST_WOLF);
    }

    bool LeaderMoveSkillUp(Player* bot, uint32 spell, ObjectGuid target)
    {
        if (!target.IsEmpty())
            return false;
        uint32 const first = sSpellMgr->GetFirstSpellInChain(spell);
        if (first != SPELL_SPRINT && first != SPELL_DASH && first != SPELL_GHOST_WOLF &&
            first != SPELL_ASPECT_OF_THE_CHEETAH && first != SPELL_ASPECT_OF_THE_PACK)
            return false;
        return bot->HasAura(spell, bot->GetGUID());
    }

    uint32 LeaderMoveSkillForm(Player* bot, uint32 spell)
    {
        return sSpellMgr->GetFirstSpellInChain(spell) == SPELL_DASH && bot->GetShapeshiftForm() != FORM_CAT
                   ? SPELL_CAT_FORM
                   : 0;
    }

    std::set<ObjectGuid> LeaderTickMoveSkills(LeaderMoveTick const& facts, std::vector<LeaderMoveTold>& told)
    {
        std::set<ObjectGuid> held;
        Player* tank = facts.tank;
        if (!tank)
            return held;
        std::vector<Player*> walkers = facts.members;
        walkers.push_back(tank);
        static std::vector<HazardZone> const noZones;
        auto const zonesOf = [&](Player* bot) -> std::vector<HazardZone> const&
        {
            if (!facts.hazardZones)
                return noZones;
            auto const found = facts.hazardZones->find(bot->GetGUID());
            return found != facts.hazardZones->end() ? found->second : noZones;
        };
        auto const tell = [&](Player* bot, Pick const& pick, std::string const& why)
        {
            if (Leader::Events::SetExternal(bot->GetGUID(), pick.spell, pick.on))
            {
                Unit* on = pick.on.IsEmpty() ? bot : ObjectAccessor::GetUnit(*bot, pick.on);
                told.push_back({bot, pick.spell, on ? on->GetName() : bot->GetName(), why});
            }
        };

        // The group's route walk out of a fight: one hunter's Aspect of the Pack speeds every walker in its radius
        // (40 yd) - the group walks at its slowest walker's speed (the members follow the tank, the far gate waits
        // for the ones behind), so the time it saves is the route left at the slowest walker's speed with the
        // aspect against without. The hunter keeps it while the route walk goes on; a pull ordered, a fight or the
        // walk's end hands it back to the Viper switch (the fight's or the walk's aspect)
        float routeLeft = 0.0f;
        Leader::MemberEvents tankLive;
        bool const routeWalk = !facts.fighting && !facts.pullOrdered && facts.routePath &&
                               facts.routePathIndex < facts.routePath->size() &&
                               Leader::Events::Get(tank->GetGUID(), tankLive) &&
                               tankLive.move == Leader::MoveEvent::MoveTo && tankLive.moveWhy == "route walk";
        if (routeWalk)
        {
            std::vector<G3D::Vector3> const& path = *facts.routePath;
            G3D::Vector3 const& next = path[facts.routePathIndex];
            routeLeft = tank->GetExactDist(next.x, next.y, next.z);
            for (size_t i = facts.routePathIndex + 1; i < path.size(); ++i)
                routeLeft += (path[i] - path[i - 1]).length();
        }
        Player* packHunter = nullptr;
        SpellInfo const* pack = sSpellMgr->GetSpellInfo(SPELL_ASPECT_OF_THE_PACK);
        if (routeWalk && routeLeft > 0.0f && pack)
        {
            Pick bestPack;
            float bestMargin = 0.0f;
            for (Player* hunter : facts.members)
            {
                if (!hunter->IsAlive() || hunter->IsInCombat() || !hunter->HasSpell(SPELL_ASPECT_OF_THE_PACK))
                    continue;
                bool const inPack = hunter->HasAura(SPELL_ASPECT_OF_THE_PACK, hunter->GetGUID());
                if (!inPack && hunter->HasSpellCooldown(SPELL_ASPECT_OF_THE_PACK))
                    continue;
                float const radius = pack->Effects[EFFECT_0].CalcRadius(hunter);
                int32 const amount = SpeedAmount(hunter, pack);
                float before = 0.0f;
                float after = 0.0f;
                for (Player* walker : walkers)
                {
                    if (!walker->IsAlive() || walker->GetMapId() != tank->GetMapId())
                        continue;
                    float const speed = walker->GetSpeed(MOVE_RUN);
                    // in the aspect already, its speed now is the aspect's
                    float const fast = walker->GetExactDist(hunter) <= radius ? SpeedWith(walker, amount) : speed;
                    before = before > 0.0f ? std::min(before, speed) : speed;
                    after = after > 0.0f ? std::min(after, fast) : fast;
                }
                Pick pick;
                pick.spell = SPELL_ASPECT_OF_THE_PACK;
                if (!inPack)
                {
                    if (before <= 0.0f || after <= before)
                        continue;
                    pick.saved = routeLeft / before - routeLeft / after;
                    float const back = float(pack->CategoryRecoveryTime) / 1000.0f;
                    pick.cost = std::max(0.0f, back - routeLeft / after);
                    pick.why = Acore::StringFormat(
                        "Aspect of the Pack: {:.1f} s of the group's {:.0f} yd route walk saved (the slowest walker "
                        "{:.1f} -> {:.1f} yd/s) vs {:.1f} s to its swap back (aspect category cooldown)",
                        pick.saved, routeLeft, before, after, pick.cost);
                    float const line = LeaderMoveManaLine(hunter, false, 0.0f, false, facts.waitBoss);
                    if (pick.saved <= pick.cost || !ManaCovers(hunter, ManaCost(hunter, pack), line, pick.why))
                        continue;
                }
                // the hunter already in it keeps it; else the one with the most mana over its line
                float const margin =
                    inPack ? 1000.0f
                           : 1.0f + hunter->GetPowerPct(POWER_MANA) -
                                 std::max(0.0f, LeaderMoveManaLine(hunter, false, 0.0f, false, facts.waitBoss));
                if (!packHunter || margin > bestMargin)
                {
                    bestMargin = margin;
                    bestPack = pick;
                    packHunter = hunter;
                }
            }
            if (packHunter)
            {
                held.insert(packHunter->GetGUID());
                Leader::MemberEvents live;
                bool const known = Leader::Events::Get(packHunter->GetGUID(), live);
                if (!packHunter->HasAura(SPELL_ASPECT_OF_THE_PACK, packHunter->GetGUID()))
                {
                    if (known && (!live.externalSpell || OwnMoveExternal(packHunter, live)))
                        tell(packHunter, bestPack, "the group's route walk: " + bestPack.why);
                }
                else if (known && live.externalSpell == SPELL_ASPECT_OF_THE_PACK)
                    Leader::Events::SetExternal(packHunter->GetGUID(), 0, ObjectGuid::Empty);
            }
        }

        for (Player* bot : walkers)
        {
            if (!bot->IsAlive() || bot == packHunter)
                continue;
            Leader::MemberEvents live;
            if (!Leader::Events::Get(bot->GetGUID(), live))
                continue;
            // another External goes first
            if (live.externalSpell && !OwnMoveExternal(bot, live))
                continue;
            bool const trails = facts.trailing && facts.trailing->count(bot->GetGUID());
            // out of a fight the tank's walk is the group's walk (the members follow it, the far gate waits for the
            // ones behind): a speed of its own saves the group nothing
            bool const groupWalk = bot == tank && !facts.fighting;
            Position to;
            float length = 0.0f;
            std::string what;
            bool const walks =
                !groupWalk && (AnyOwnSkillReady(bot) || LeaderInMoveAspect(bot)) &&
                OrderedWalk(bot, tank, live, trails, to, length, what);
            // a hunter in its own movement aspect stays in it while its walk goes on out of a fight
            if (LeaderInMoveAspect(bot) && walks && !facts.fighting && !facts.pullOrdered && !bot->IsInCombat())
                held.insert(bot->GetGUID());
            // Ghost Wolf: no spell is cast in it - left once the fight starts, the pull is ordered, the walk stops,
            // the leader readies or rests the group, or the shaman has heals to cast
            if (bot->HasAura(SPELL_GHOST_WOLF))
            {
                bool const keep = !facts.fighting && !facts.pullOrdered && !bot->IsInCombat() &&
                                  (bot->isMoving() || tank->isMoving()) && live.ready == Leader::ReadyEvent::None &&
                                  !RestState::HealerHasWork(bot);
                if (!keep)
                {
                    Pick leave;
                    leave.spell = SPELL_GHOST_WOLF;
                    leave.on = bot->GetGUID();
                    tell(bot, leave, "leaves Ghost Wolf: the walk is over");
                }
                else if (live.externalSpell == SPELL_GHOST_WOLF)
                    Leader::Events::SetExternal(bot->GetGUID(), 0, ObjectGuid::Empty);
                continue;
            }
            Pick pick;
            if (walks)
                pick = BestOwnSkill(bot, to, length, zonesOf(bot),
                                    LeaderMoveManaLine(bot, facts.fighting, facts.fightSecLeft, facts.bossFight,
                                                       facts.waitBoss),
                                    facts.routeLegs);
            if (!pick.spell && facts.fighting && PlayerbotAI::IsMelee(bot))
                pick = BestGapCloser(bot, tank, live);
            if (pick.spell)
            {
                // a leap goes facing the walk (LeaderExternalAction): told on the member by its guid (lead690)
                if (pick.spell == SPELL_BLINK)
                    pick.on = bot->GetGUID();
                tell(bot, pick, (walks ? "the walk (" + what + "): " : std::string("the attack: ")) + pick.why);
            }
            else if (OwnMoveExternal(bot, live))
                Leader::Events::SetExternal(bot->GetGUID(), 0, ObjectGuid::Empty);
        }
        return held;
    }

    // [lead690] [lead691] the mage's Blink on the member's walk (LeapSaves), its mana after it over `manaLine`
    bool LeaderLeapSaves(Player* bot, Position const& to, std::vector<HazardZone> const& zones, std::string* why,
                         float manaLine)
    {
        float leap = 0.0f;
        uint32 const spell = LeaderLeapSpell(bot, &leap);
        SpellInfo const* info = spell ? sSpellMgr->GetSpellInfo(spell) : nullptr;
        if (!info || bot->HasSpellCooldown(spell))
            return false;
        Pick pick;
        if (!LeapSaves(bot, info, leap, false, to, zones, pick) ||
            !ManaCovers(bot, ManaCost(bot, info), manaLine, pick.why))
            return false;
        if (why)
            *why = pick.why;
        return true;
    }

    // [lead18] the nearest spot around `from` that fits (ranged: at least 8 yd off the target)
    // [lead902] user 2026-11-03: nearest by the member's real walk (navmesh, Leader::WalkYd), not the straight line:
    // every fitting spot whose straight distance from the member is below the best walk found is priced
    bool LeaderNearestCombatSpot(Player* bot, PlayerbotAI* botAI, Unit* target, Position const& from, bool melee,
                                 std::vector<HazardZone> const& zones,
                                 std::function<bool(Position const&)> const& keeps, Position& out,
                                 std::vector<ObjectGuid> const* fightMobs)
    {
        std::optional<Position> fallback;  // [lead18] new ground, when no cleared spot fits
        constexpr float RINGS[] = {0.0f, 3.0f, 6.0f, 9.0f, 12.0f};
        constexpr uint32 DIRECTIONS = 12;
        Map* map = bot->GetMap();
        std::optional<Position> best;  // [lead902]
        float bestWalk = 0.0f;
        float firstWalk = -1.0f;
        for (float ring : RINGS)
            for (uint32 i = 0; i < (ring > 0.0f ? DIRECTIONS : 1u); ++i)
            {
                float const angle = 2.0f * float(M_PI) * float(i) / float(DIRECTIONS);
                float x = from.GetPositionX() + std::cos(angle) * ring;
                float y = from.GetPositionY() + std::sin(angle) * ring;
                float z = from.GetPositionZ();
                if (ring > 0.0f && (!map->CheckCollisionAndGetValidCoords(bot, from.GetPositionX(),
                                                                          from.GetPositionY(), from.GetPositionZ(),
                                                                          x, y, z) ||
                                    std::fabs(z - from.GetPositionZ()) > 4.0f))
                {
                    NoteSpotReject("near", "no ground on its floor");
                    continue;
                }
                Position const spot(x, y, z);
                if (best && bot->GetExactDist(spot) >= bestWalk)  // [lead902] no walk to it can be shorter
                    continue;
                if (!melee && target->GetExactDist2d(x, y) < std::max(8.0f, LeaderRangedMinGap(bot, target)))
                {
                    NoteSpotReject("near", "inside its minimum gap to the target");
                    continue;
                }
                if (std::string const why =
                        LeaderCombatSpotBreaks(bot, botAI, target, spot, melee, zones, keeps, true, fightMobs);
                    !why.empty())
                {
                    NoteSpotReject("near", why);
                    continue;
                }
                if (int const candidate = LeaderCandidate(bot, spot, fallback, target);
                    candidate != 2)  // [lead490] [lead492]
                {
                    if (!candidate)
                        NoteSpotReject("near", "an idle mob's aggro reach");
                    continue;
                }
                float const walk = Leader::WalkYd(bot, spot);
                if (firstWalk < 0.0f)
                    firstWalk = walk;
                if (!best || walk < bestWalk)
                {
                    best = spot;
                    bestWalk = walk;
                }
            }
        if (best)
        {
            if (bestWalk + 1.0f < firstWalk)
            {
                thread_local uint32 shorterLogged = 0;
                if (shorterLogged++ % 50 == 0)
                    LOG_INFO("playerbots", "[lead902] {}: the combat spot by its real walk {:.1f} yd against {:.1f} yd "
                             "to the first fitting spot by straight distance ({} such picks)",
                             bot->GetName(), bestWalk, firstWalk, shorterLogged);
            }
            out = *best;
            return true;
        }
        if (fallback)
        {
            out = *fallback;
            return true;
        }
        return false;
    }

    // [lead15] the assigned spot's checks: ground, sight to the fight spot, no idle mob outside the pulled pack
    // reaching it, a walk to it.
    bool LeaderSpotOk(Player* tank, Position const& fight, Position& spot, std::vector<ObjectGuid> const& pack)
    {
        Map* map = tank->GetMap();
        float const ground = map->GetHeight(tank->GetPhaseMask(), spot.GetPositionX(), spot.GetPositionY(),
                                            fight.GetPositionZ() + 5.0f, true, 15.0f);
        if (ground <= INVALID_HEIGHT || std::fabs(ground - fight.GetPositionZ()) > 6.0f)
            return false;
        spot.Relocate(spot.GetPositionX(), spot.GetPositionY(), ground);
        if (!map->isInLineOfSight(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ() + 2.0f,
                                  fight.GetPositionX(), fight.GetPositionY(), fight.GetPositionZ() + 2.0f,
                                  tank->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
            return false;
        std::list<Creature*> idle;
        IdleAggroCheck check{tank};
        Acore::CreatureListSearcher<IdleAggroCheck> searcher(tank, idle, check);
        Cell::VisitObjects(tank, searcher, tank->GetExactDist(spot) + 30.0f);
        for (Creature* mob : idle)
            if (std::find(pack.begin(), pack.end(), mob->GetGUID()) == pack.end() &&
                IdleMobReachesSpot(tank, mob, spot))
                return false;
        return Leader::Route(tank, spot).found;  // [lead99]
    }
}
