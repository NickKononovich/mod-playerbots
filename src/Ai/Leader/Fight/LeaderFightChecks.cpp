/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LeaderFightChecks.h"

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

    // the bot's interrupt with no cooldown running (Kick, Pummel, Shield Bash, Counterspell, Wind Shear, Mind
    // Freeze, Silence); 0: none
    uint32 LeaderReadyInterrupt(Player* bot)
    {
        static constexpr uint32 INTERRUPTS[] = {1766,  1767, 1768,  1769,  38768, 6552,  6554, 72,
                                                1671,  1672, 29704, 2139,  57994, 47528, 15487};
        uint32 ready = 0;
        for (uint32 spell : INTERRUPTS)
            if (bot->HasSpell(spell) && !bot->HasSpellCooldown(spell))
                ready = spell;
        return ready;
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
        float const reach = std::max(LeaderShootReach(bot), LeaderSpellReach(bot));
        float const minGap = bot->getClass() == CLASS_HUNTER ? LeaderRangedMinGap(bot, target) : 0.0f;
        return reach > 0.0f && gap <= reach && gap >= minGap && LeaderSeesFrom(bot, target, at) &&
               (!fightMobs || LeaderOutOfMobsMelee(bot, at, *fightMobs, whereTheyStand));
    }

    bool HealReaches(Player* tank, Position const& from, Position const& to)
    {
        return from.GetExactDist(to) <= Leader::LEADER_HEAL_REACH_YD - g_spotAcquireMargin &&
               tank->GetMap()->isInLineOfSight(from.GetPositionX(), from.GetPositionY(),
                                               from.GetPositionZ() + 2.0f, to.GetPositionX(), to.GetPositionY(),
                                               to.GetPositionZ() + 2.0f, tank->GetPhaseMask(),
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
                out.push_back({dyn->GetPosition(), radius, info->SpellName[LOCALE_enUS]});
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
                float const reach = LeaderShootReach(bot);
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
                                                          : LeaderShootReach(bot) - 2.0f;  // [lead127] its reach
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
                out = spot;
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

    // [lead18] the member's spell reach to its target (a hunter 35 yd, a caster 30 yd)
    float LeaderSpellReach(Player* bot) { return bot->getClass() == CLASS_HUNTER ? 35.0f : 30.0f; }

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
        float const castReach = std::max(LeaderShootReach(bot), LeaderSpellReach(bot));
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

    bool LeaderFrontCostsLess(Player* bot, Unit* target, std::string* why)
    {
        Creature* mob = target ? target->ToCreature() : nullptr;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        // [lead580] staying in front is staying where the member hits it: in the game's sight too
        if (!mob || !botAI || mob->IsDungeonBoss() || mob->isWorldBoss() || !bot->IsWithinMeleeRange(mob) ||
            !mob->HasInArc(float(M_PI), bot) || !bot->IsWithinLOSInMap(mob))
            return false;
        // the rotations that need the back: Shred (cat form), Backstab as the Subtlety builder (Hemorrhage talent)
        if ((bot->getClass() == CLASS_DRUID && botAI->HasSpell("shred")) ||
            (bot->getClass() == CLASS_ROGUE && botAI->HasSpell("hemorrhage")))
            return false;
        float const avgHit =
            (bot->GetFloatValue(UNIT_FIELD_MINDAMAGE) + bot->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2.0f;
        float const blockShare = avgHit > 0.0f ? std::min(1.0f, float(mob->GetShieldBlockValue()) / avgHit) : 0.0f;
        float const lossShare = (mob->GetUnitParryChance() + mob->GetUnitBlockChance() * blockShare) / 100.0f;
        float const lifetime = ExpectedLifetime(botAI, mob);
        float const lossSec = lifetime * lossShare;
        // the arc into its back half: from the member's angle off the mob's facing to a right angle
        float const off = std::fabs(Position::NormalizeOrientation(mob->GetRelativeAngle(bot) + float(M_PI)) -
                                    float(M_PI));
        float const arc = mob->GetExactDist2d(bot) * std::max(0.0f, float(M_PI) / 2.0f - off);
        float const speed = bot->GetSpeed(MOVE_RUN);
        float const walkSec = speed > 0.0f ? arc / speed : 0.0f;
        if (why)
            *why = Acore::StringFormat("front loss {:.2f} s ({:.1f} % parry + {:.1f} % block x {:.2f} over {:.1f} s) "
                                       "vs the walk {:.2f} s ({:.1f} yd)",
                                       lossSec, mob->GetUnitParryChance(), mob->GetUnitBlockChance(), blockShare,
                                       lifetime, walkSec, arc);
        return lossSec < walkSec;
    }

    // [lead18] the nearest spot around `from` that fits (ranged: at least 8 yd off the target)
    bool LeaderNearestCombatSpot(Player* bot, PlayerbotAI* botAI, Unit* target, Position const& from, bool melee,
                                 std::vector<HazardZone> const& zones,
                                 std::function<bool(Position const&)> const& keeps, Position& out,
                                 std::vector<ObjectGuid> const* fightMobs)
    {
        std::optional<Position> fallback;  // [lead18] new ground, when no cleared spot fits
        constexpr float RINGS[] = {0.0f, 3.0f, 6.0f, 9.0f, 12.0f};
        constexpr uint32 DIRECTIONS = 12;
        Map* map = bot->GetMap();
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
