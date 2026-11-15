/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "PullFacts.h"

#include "AttackerCountValues.h"
#include "Creature.h"
#include "Group.h"
#include "CreatureGroups.h"
#include "GenericSpellActions.h"
#include "LeaderActions.h"
#include "LeaderKnowledge.h"
#include "Player.h"
#include "Playerbots.h"
#include "LeaderOrders.h"  // [lead108] Leader::Route
#include "MobRoles.h"      // [lead1041] HoldsRange
#include "RouteSteer.h"  // [lead167] BossBroughtBy
#include "ChooseTargetActions.h"  // [lead760] FightSpot
#include "LeaderFightChecks.h"   // [lead760] TANK_TAUNTS
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "HealthTriggers.h"  // [lead925] RestState::HealerThroughput
#include "Log.h"             // [lead925]
#include "StringFormat.h"    // [lead925]
#include "GroupRouteState.h"      // [lead1170] the route's plan
#include "ChainPullJudge.h"       // [test1] the chain pull judge's numeric gates, as a seam
#include "LeaderGroup.h"          // [lead1170]
#include "LeaderPlannerShared.h"  // [lead1170] TankEngageOpeners
#include "LeaderSupport.h"        // [lead1170] HealerOutOfMana
#include "Map.h"                  // [lead1171]
#include "MotionMaster.h"         // [lead1171] SPEED_CHARGE
#include "ObjectAccessor.h"       // [lead1170]
#include "ReachTargetActions.h"   // [lead1171] TankWalkIntoIdlePack
#include "TankAoeThreat.h"        // [lead1171] IsGapCloser
#include "CellImpl.h"       // [lead1230] FleeHelp
#include "GridNotifiers.h"  // [lead1230]
#include "GridNotifiersImpl.h"
#include "World.h"  // [lead1230] the flee assistance radius
#include <algorithm>
#include <cmath>  // [lead1190] std::sqrt
#include <mutex>          // [lead925]
#include <set>            // [lead925]
#include <unordered_map>  // [lead925]

namespace Leader::Fight
{
    Unit* WalkInWakes(Player* tank, Unit* target, std::vector<ObjectGuid> const& planMobs)
    {
        PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        Creature* creature = target ? target->ToCreature() : nullptr;
        if (!tankAI || !creature)
            return nullptr;
        auto* mover =
            dynamic_cast<LeaderMoveOrderAction*>(tankAI->GetAiObjectContext()->GetAction("leader move order"));
        if (!mover)
            return nullptr;
        // [lead167] b179 rfc-h 5G 264-330 s: the walk to Oggleflint "woke" Ragefire Trogg 54, which joins his fight by
        // script - the planner refuses that pack as "brings a boss, pulled as his fight", the walk-in check released
        // the boss for it as "the pull first": 74 releases. A mob the boss brings into his fight anyway is his pull
        bool const boss = creature->IsDungeonBoss() || creature->isWorldBoss();
        auto const counts = [&](Unit* woken)
        {
            return woken && woken->ToCreature() &&
                   std::find(planMobs.begin(), planMobs.end(), woken->GetGUID()) == planMobs.end() &&
                   !(creature->GetFormation() && woken->ToCreature()->GetFormation() == creature->GetFormation()) &&
                   !(boss && Leader::Travel::BossBroughtBy(tank, creature, woken->ToCreature()));
        };
        if (Unit* woken = mover->WalkWakes(target->GetPosition(), target->GetGUID()); counts(woken))
            return woken;
        // [lead113] b147 rfc-h 59.9 s: the healer following the tank 12 yd behind woke Earthborer 4 (45 yd from the
        // fight spot) - the followers walk in too: each member's own walk to the target (its route search against
        // every idle pack's reach, its own aggro check) is judged the same way
        if (Group* group = tank->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                PlayerbotAI* memberAI = member && member != tank ? GET_PLAYERBOT_AI(member) : nullptr;
                if (!memberAI || !member->IsAlive() || member->GetMap() != tank->GetMap())
                    continue;
                if (auto* follower = dynamic_cast<LeaderMoveOrderAction*>(
                        memberAI->GetAiObjectContext()->GetAction("leader move order")))
                    if (Unit* woken = follower->WalkWakes(target->GetPosition(), target->GetGUID()); counts(woken))
                        return woken;
            }
        return nullptr;
    }

    bool OrderMobStaysPick(Player* tank, Creature* own)
    {
        return own && own->IsAlive() && !own->IsInCombat() && TankHasWalk(tank, own);
    }

    bool TankHasWalk(Player* tank, Unit* mob)
    {
        if (!tank || !mob)
            return false;
        Leader::RouteResult const path = Leader::Route(tank, mob->GetPosition());
        if (!path.found)
            return false;
        if (path.complete || path.points.empty())
            return path.complete;
        float const straight = tank->GetExactDist(mob);
        float const left =
            (path.points.back() - G3D::Vector3(mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ())).length();
        return left + 5.0f < straight;
    }

    bool NeedsFleeStop(Unit* mob)
    {
        // [lead1230] held only by what holds it now (a stun or a root aura): no stop spread of 2 s after a snare -
        // b245 rfc-h: a Frostbolt-snared runner kept running with no stronger stop for those seconds
        if (!mob || !mob->IsAlive() || !mob->ToCreature() || mob->HasAuraType(SPELL_AURA_MOD_STUN) ||
            mob->HasAuraType(SPELL_AURA_MOD_ROOT) || mob->HasBreakableByDamageCrowdControlAura())
            return false;
        float const fleeAt = Leader::Knowledge::FleeForAssistHealthPct(mob->GetEntry());
        return (fleeAt > 0.0f && mob->GetHealthPct() <= fleeAt) || FledMobs::IsRunning(mob);
    }

    bool IsFleer(Unit* mob)
    {
        if (!mob || !mob->IsAlive() || !mob->ToCreature())
            return false;
        float const fleeAt = Leader::Knowledge::FleeForAssistHealthPct(mob->GetEntry());
        return (fleeAt > 0.0f && mob->GetHealthPct() <= fleeAt) || FledMobs::IsRunning(mob);
    }

    Player* FleeStopper(std::vector<Player*> const& members, Unit* mob, bool afterHold)
    {
        // [lead1210] the member whose ready stop holds the runner most (a stun before a root before a snare, by spell
        // data); a runner already snared takes only a stronger hold; the members' order breaks a tie
        // [lead1230] `afterHold`: the next stop of the chain, cast the moment the hold on it now ends
        uint8 const held = afterHold ? FleeStopAction::HOLD_NONE : FleeStopAction::HeldBy(mob);
        Player* best = nullptr;
        uint8 bestHold = 0;
        for (Player* member : members)
        {
            PlayerbotAI* memberAI = member ? GET_PLAYERBOT_AI(member) : nullptr;
            if (!memberAI || !member->IsAlive() || PlayerbotAI::IsHeal(member))
                continue;
            auto* stop = dynamic_cast<FleeStopAction*>(memberAI->GetAiObjectContext()->GetAction("flee stop"));
            if (!stop || !stop->CanStop(mob))
                continue;
            uint8 const hold = stop->BestHold(mob);
            if (held != FleeStopAction::HOLD_NONE && hold <= held)
                continue;
            if (!best || hold > bestHold)
            {
                best = member;
                bestHold = hold;
            }
        }
        return best;
    }

    Creature* FleeHelp(Unit* mob, float& reachSec)
    {
        // [lead1230] the core's own search at the flee (Creature::DoFleeToGetAssistance): the nearest creature that
        // may assist it against its victim (CanAssistTo: out of combat, in sight) within the flee assistance radius;
        // the runner runs to it at its run speed and calls it there (AssistanceMovementGenerator::Finalize)
        reachSec = -1.0f;
        Creature* runner = mob ? mob->ToCreature() : nullptr;
        Unit* victim = runner ? runner->GetVictim() : nullptr;
        float const radius = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS);
        if (!runner || !runner->IsAlive() || !victim || radius <= 0.0f)
            return nullptr;
        Creature* helper = nullptr;
        Acore::NearestAssistCreatureInCreatureRangeCheck check(runner, victim, radius);
        Acore::CreatureLastSearcher<Acore::NearestAssistCreatureInCreatureRangeCheck> searcher(runner, helper, check);
        Cell::VisitObjects(runner, searcher, radius);
        if (helper)
            reachSec = runner->GetExactDist(helper) / std::max(runner->GetSpeed(MOVE_RUN), 0.1f);
        return helper;
    }

    bool MobComesToTank(Player* tank, Unit* mob)
    {
        if (!tank || !mob || !mob->IsAlive() || !mob->IsInCombat() || mob->GetVictim() != tank ||
            mob->HasUnitState(UNIT_STATE_ROOT) || tank->IsWithinMeleeRange(mob))
            return false;
        Creature* creature = mob->ToCreature();
        if (creature && creature->CanNotReachTarget())
            return false;
        if (mob->isMoving())
            return true;
        // [lead1041] b230 rfc-h (16 runs): the tank's "reach melee" walked 26 s after a mob it had just taunted and
        // 73 s toward a mob already on it ("same mob, coming at the tank") - on the first ticks after the victim switch
        // the mob has not started its chase yet (isMoving false). A mob whose victim is the tank chases it by the
        // game's combat movement: one that may move in combat (Creature::IsCombatMovementAllowed), is not held (stun,
        // fear, confuse, its own cast) and does not stand to cast from range (MobRoles::HoldsRange) comes to the tank
        return creature && creature->IsCombatMovementAllowed() && !MobRoles::HoldsRange(mob) &&
               !mob->HasUnitState(UNIT_STATE_CONTROLLED | UNIT_STATE_CASTING);
    }

    uint32 TauntReachesFromHere(Player* tank, Unit* mob)
    {
        if (!tank || !mob || !mob->IsAlive())
            return 0;
        constexpr uint32 SPELL_RIGHTEOUS_DEFENSE = 31789;
        for (uint32 spell : TANK_TAUNTS)
        {
            if (!tank->HasSpell(spell) || tank->HasSpellCooldown(spell))
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
            // Righteous Defense goes on the attacked friend, the others on the mob
            Unit* on = spell == SPELL_RIGHTEOUS_DEFENSE ? mob->GetVictim() : mob;
            if (info && on && on != tank && tank->IsWithinCombatRange(on, info->GetMaxRange(on->IsFriendlyTo(tank))) &&
                tank->IsWithinLOSInMap(on))
                return spell;
        }
        return 0;
    }

    std::string TankWalkMovesFight(Player* tank, Unit* target, bool gapCloser)
    {
        if (!tank || !target || !target->IsAlive() || !Leader::LedBot(tank) || tank->IsWithinMeleeRange(target))
            return std::string();
        Creature* creature = target->ToCreature();
        // a mob that cannot reach its victim evades every hit: only a walk to it ends that (b39_hos)
        if (!creature || creature->CanNotReachTarget())
            return std::string();
        // [lead760] b220 rfc-h: 48 pulls walked back by "move fight spot back" (519 yd) after the tank met its mob
        // forward first (reach melee 190 yd, tank open incoming 108, charges 33, aggressive target 26 before the
        // walk back): an engage with a fight spot back is fought there - the tank goes there and the mob comes
        // [lead1322] b255 rfc-h 5D-12 140-156 s (the healer's death): a Trogg and two Shamans on the resto shaman
        // 25-35 yd off, the bear's "reach melee" refused 15 s ("the plan's fight spot back is not where the mob is",
        // the in-fight caster corner's spot). A mob on another member does not come to the fight spot: the spot back
        // is for the mobs that come to the tank, never a hold off a member's attacker
        Unit* const onWhom = target->IsInCombat() ? target->GetVictim() : nullptr;
        bool const onMember = onWhom && onWhom != tank && onWhom->IsPlayer() &&
                              tank->IsInSameRaidWith(onWhom->ToPlayer());
        Position back;
        if (!onMember && FightSpot::Ahead(tank, back) &&
            target->GetExactDist(back) > tank->GetCombatReach() + target->GetCombatReach() + NOMINAL_MELEE_RANGE)
            return "the plan's fight spot back is not where the mob is";
        if (!target->IsInCombat())
            return std::string();
        // [lead1171] user 2026-11-04 (Q66): the leader's meeting verdict walks the tank in to meet it
        if (!gapCloser && MobComesToTank(tank, target) && TankMeetFor(tank, target) != TankMeet::Walk)  // [lead925]
            return "it comes to the tank";
        Unit* victim = target->GetVictim();
        if (victim && victim != tank && victim->GetCharmerOrOwnerPlayerOrPlayerItself() &&
            TauntReachesFromHere(tank, target))
            return "a ready taunt reaches it from here";
        // [lead760] b220: "reach melee" to a mob on the tank standing at range (a caster) 75 s - the walk drags the
        // mobs the tank holds; while the tank holds another fight mob in its melee it stays, the ranged hit the caster
        if (victim == tank && !target->isMoving())
            for (Unit* attacker : tank->getAttackers())
                if (attacker && attacker != target && attacker->IsAlive() && attacker->GetVictim() == tank &&
                    tank->IsWithinMeleeRange(attacker))
                    return "a caster at range while the tank holds " + attacker->GetName() + " in its melee";
        return std::string();
    }

    namespace
    {
        // [lead925] the tank's defensives a gap closer's landing may wait for: up, or ready for the leader to tell
        constexpr uint32 SPELL_SURVIVAL_INSTINCTS = 61336;
        constexpr uint32 SPELL_BARKSKIN = 22812;
        constexpr uint32 SPELL_SHIELD_WALL = 871;
        constexpr uint32 SPELL_LAST_STAND = 12975;
        constexpr uint32 SPELL_LAST_STAND_AURA = 12976;

        std::mutex gapSaveLock;
        std::unordered_map<uint64, ObjectGuid> gapSaveAsked;  // [lead925] under gapSaveLock: tank -> target
    }

    std::string TankGapCloserRisk(Player* tank, Unit* target, uint32* save)
    {
        if (save)
            *save = 0;
        if (!tank || !target || !target->IsAlive() || !Leader::LedBot(tank) || tank->IsWithinMeleeRange(target))
            return std::string();
        std::vector<uint32> defensives;
        if (tank->getClass() == CLASS_DRUID)
            defensives = {SPELL_SURVIVAL_INSTINCTS, SPELL_BARKSKIN};
        else if (tank->getClass() == CLASS_WARRIOR)
            defensives = {SPELL_SHIELD_WALL, SPELL_LAST_STAND};
        for (uint32 spell : defensives)
            if (GapCloserSaveUp(tank, spell))
                return std::string();
        Group* group = tank->GetGroup();
        if (!group)
            return std::string();
        Position const landing = target->GetPosition();
        Player* nearest = nullptr;
        float nearestSec = -1.0f;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* healer = ref->GetSource();
            if (!healer || healer == tank || !healer->IsAlive() || healer->GetMap() != tank->GetMap() ||
                !PlayerbotAI::IsHeal(healer))
                continue;
            if (HealReaches(tank, healer->GetPosition(), landing, healer))
                return std::string();
            // its walk into heal reach of the landing: the walk less the heal's reach, the whole walk where the
            // landing is in range but out of its sight (it walks round the corner)
            float const walk = Leader::WalkYd(healer, landing);
            float const reach = Leader::LEADER_HEAL_REACH_YD + 2.0f * tank->GetCombatReach();
            float const into = healer->GetExactDist(landing) > reach ? std::max(walk - reach, 0.0f) : walk;
            float const speed = std::max(healer->GetSpeed(MOVE_RUN), 0.1f);
            float const sec = into / speed + RestState::HealerThroughput(healer, tank).gapSec;
            if (nearestSec < 0.0f || sec < nearestSec)
            {
                nearest = healer;
                nearestSec = sec;
            }
        }
        if (!nearest)
            return std::string();  // no healer to wait for: no wait changes the landing
        float dps = 0.0f;
        float spellPart = 0.0f;
        bool counted = false;
        for (Unit* attacker : tank->getAttackers())
            if (attacker && attacker->IsAlive())
            {
                dps += LandedOnDps(attacker, tank, spellPart);  // [judge18]
                counted |= attacker == target;
            }
        if (!counted)
            dps += LandedOnDps(target, tank, spellPart);  // [judge18]
        float const health = float(tank->GetHealth());
        if (dps <= 0.0f || health > dps * nearestSec)
            return std::string();
        for (uint32 spell : defensives)
            if (save && !*save && tank->HasSpell(spell) && !tank->HasSpellCooldown(spell))
                *save = spell;
        return Acore::StringFormat("no heal reaches its landing at {}: {} needs {:.1f} s to heal it there, it takes "
                                   "{:.0f}/s against {:.0f} health",
                                   target->GetName(), nearest->GetName(), nearestSec, dps, health);
    }

    bool IsGapCloserSave(uint32 spell)
    {
        return spell && (spell == SPELL_SURVIVAL_INSTINCTS || spell == SPELL_BARKSKIN || spell == SPELL_SHIELD_WALL ||
                         spell == SPELL_LAST_STAND);
    }

    bool GapCloserSaveUp(Player* tank, uint32 spell)
    {
        return tank->HasAura(spell) || (spell == SPELL_LAST_STAND && tank->HasAura(SPELL_LAST_STAND_AURA));
    }

    void AskGapCloserSave(Player* tank, Unit* target)
    {
        std::lock_guard<std::mutex> lock(gapSaveLock);
        gapSaveAsked[tank->GetGUID().GetRawValue()] = target->GetGUID();
    }

    bool GapCloserSaveAsked(Player* tank, ObjectGuid& target)
    {
        std::lock_guard<std::mutex> lock(gapSaveLock);
        auto const found = gapSaveAsked.find(tank->GetGUID().GetRawValue());
        if (found == gapSaveAsked.end())
            return false;
        target = found->second;
        return true;
    }

    void ClearGapCloserSave(Player* tank)
    {
        std::lock_guard<std::mutex> lock(gapSaveLock);
        gapSaveAsked.erase(tank->GetGUID().GetRawValue());
    }

    bool TankGapCloserAllowed(Player* tank, Unit* target, char const* what)
    {
        uint32 save = 0;
        std::string const risk = TankGapCloserRisk(tank, target, &save);
        if (risk.empty())
            return true;
        if (save)
            AskGapCloserSave(tank, target);
        thread_local std::set<std::pair<uint64, uint64>> logged;
        if (logged.insert({tank->GetGUID().GetRawValue(), target->GetGUID().GetRawValue()}).second)
            LOG_INFO("playerbots", "[lead925] {}: no {} on {} ({:.1f} yd) - {}{}", tank->GetName(), what,
                     target->GetName(), tank->GetExactDist(target), risk,
                     save ? Acore::StringFormat("; its save {} asked for first", save)
                          : std::string("; no save ready"));
        return false;
    }

    // [lead1170] [lead1171] the chain pull and the tank's meeting of its incoming mob (PullFacts.h)
    namespace
    {
        // [lead1171] the core's Death Grip jump: Spell::EffectJumpDest gives spell 57604 (Death Grip's jump of the
        // gripped mob to the caster) speedXY 50 yd/s
        constexpr float DEATH_GRIP_JUMP_SPEED = 50.0f;

        std::mutex tankMeetLock;
        struct TankMeetEntry
        {
            ObjectGuid mob;
            TankMeet meet = TankMeet::None;
            std::string spell;
            Position landing;
        };
        std::unordered_map<uint64, std::vector<TankMeetEntry>> tankMeets;  // under tankMeetLock: tank -> its verdicts

        // the seconds from now to `member`'s first hit on `mob` fought at `landing`, the tank's contact after
        // `contactSec`: a ranged member's walk into its spell range of the landing (it hits while the mob runs), a
        // melee member's walk into its melee range there, no sooner than the contact
        float MemberFirstHitSec(Player* member, Unit* mob, Position const& landing, float contactSec)
        {
            float const speed = std::max(member->GetSpeed(MOVE_RUN), 0.1f);
            float const gap = member->GetExactDist2d(landing.GetPositionX(), landing.GetPositionY());
            if (PlayerbotAI::IsRanged(member))
            {
                PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
                float const range = memberAI ? memberAI->GetRange("spell") : 0.0f;
                return std::max(gap - range, 0.0f) / speed;
            }
            return std::max(contactSec, std::max(gap - member->GetMeleeRange(mob), 0.0f) / speed);
        }
    }

    char const* TankMeetName(TankMeet meet)
    {
        switch (meet)
        {
            case TankMeet::Stand:
                return "stand";
            case TankMeet::Walk:
                return "walk";
            case TankMeet::Closer:
                return "gap closer";
            case TankMeet::Grip:
                return "grip";
            default:
                return "none";
        }
    }

    std::string HealerKeepsLanding(Player* tank, Position const& landing, float inSec,
                                   std::vector<Player*> const& members, HealerAlong& along)
    {
        along = HealerAlong();
        Map* map = tank ? tank->GetMap() : nullptr;
        if (!map)
            return "no tank";
        std::vector<Player*> healers;
        for (Player* member : members)
            if (member && member != tank && member->IsAlive() && PlayerbotAI::IsHeal(member))
                healers.push_back(member);
        if (healers.empty())
            return std::string();
        for (Player* healer : healers)
            if (HealReaches(tank, healer->GetPosition(), landing, healer))
            {
                along.healer = healer;
                along.spot = healer->GetPosition();
                return std::string();
            }
        // a new spot of the healer: the acquire margin (the heal reach rule's own spot search)
        SpotAcquire const acquire;
        float const band = Leader::LEADER_HEALER_TANK_YD - g_spotAcquireMargin;
        std::string why;
        for (Player* healer : healers)
        {
            float const speed = std::max(healer->GetSpeed(MOVE_RUN), 0.1f);
            std::vector<Position> candidates;
            // the point on its line to the landing at the healer band
            if (float const gap = healer->GetExactDist2d(landing.GetPositionX(), landing.GetPositionY()); gap > band)
                candidates.emplace_back(
                    healer->GetPositionX() + (landing.GetPositionX() - healer->GetPositionX()) / gap * (gap - band),
                    healer->GetPositionY() + (landing.GetPositionY() - healer->GetPositionY()) / gap * (gap - band),
                    healer->GetPositionZ());
            // its offset to the tank kept (it walks along with the tank)
            candidates.emplace_back(healer->GetPositionX() + landing.GetPositionX() - tank->GetPositionX(),
                                    healer->GetPositionY() + landing.GetPositionY() - tank->GetPositionY(),
                                    healer->GetPositionZ());
            // [lead1350] b257 rfc-h 5F-14 883.9 s / 5F-6 907.3 s, the same pack (Searing Blade Enforcer x2), the same
            // tank spot (36 yd), the same fight left: the chain taken in one run, refused "no reach or sight of the
            // landing from its spot" in the other - the two points above are one line from where the healer happened
            // to stand, a wall's corner a few yards off decides. The healer's places round the landing (the ring
            // search lead1332 uses: 12 bearings at the band and half of it), nearest first, judged by the same
            // checks: the verdict is whether the healer has a place that heals the landing in time, not whether its
            // own line happens to see it
            {
                std::vector<std::pair<float, Position>> ring;
                for (float radius : {band, band * 0.5f})
                    for (uint32 i = 0; i < 12; ++i)
                    {
                        float const angle = 2.0f * float(M_PI) * float(i) / 12.0f;
                        Position const point(landing.GetPositionX() + std::cos(angle) * radius,
                                             landing.GetPositionY() + std::sin(angle) * radius, landing.GetPositionZ());
                        float const gap = healer->GetExactDist2d(point.GetPositionX(), point.GetPositionY());
                        if (gap / speed <= inSec)  // a straight walk already too long is no candidate
                            ring.emplace_back(gap, point);
                    }
                std::sort(ring.begin(), ring.end(),
                          [](auto const& a, auto const& b) { return a.first < b.first; });
                for (auto const& point : ring)
                    candidates.push_back(point.second);
            }
            std::string last;
            for (Position const& candidate : candidates)
            {
                float x = candidate.GetPositionX();
                float y = candidate.GetPositionY();
                float z = candidate.GetPositionZ();
                if (!map->CheckCollisionAndGetValidCoords(healer, healer->GetPositionX(), healer->GetPositionY(),
                                                          healer->GetPositionZ(), x, y, z))
                {
                    last = "no ground";
                    continue;
                }
                Position const spot(x, y, z);
                if (!HealReaches(tank, spot, landing, healer))
                {
                    last = "no reach or sight of the landing from its spot";
                    continue;
                }
                float const sec = Leader::WalkYd(healer, spot) / speed;
                if (sec > inSec)
                {
                    last = Acore::StringFormat("its walk {:.1f} s > the tank's {:.1f} s", sec, inSec);
                    continue;
                }
                if (Creature* idleMob = LeaderAggroAt(healer, spot))
                {
                    last = "its spot is in " + idleMob->GetName() + "'s reach";
                    continue;
                }
                bool noPath = false;
                if (Creature* idleMob = LeaderAggroOnWalk(healer, spot, noPath))
                {
                    last = "its walk passes " + idleMob->GetName() + "'s reach";
                    continue;
                }
                if (noPath)
                {
                    last = "no walk to its spot";
                    continue;
                }
                along.healer = healer;
                along.spot = spot;
                along.walks = true;
                along.walkSec = sec;
                return std::string();
            }
            why += std::string(why.empty() ? "" : ", ") + healer->GetName() + ": " + last;
        }
        return "no healer reaches the landing where it stands or by its walk there in time (" + why + ")";
    }

    ChainPull JudgeChainPull(LeaderGroup& group, Player* tank, std::vector<Unit*> const& fightMobs, float fightSecLeft,
                             std::function<std::vector<ObjectGuid>(Creature*)> const& packOf, float walkArriveYd)
    {
        ChainPull chain;
        chain.fightSecLeft = fightSecLeft;
        PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        Map* map = tank ? tank->GetMap() : nullptr;
        if (!tankAI || !map || !tank->IsAlive())
            return chain;
        auto const inFight = [&](ObjectGuid guid)
        {
            return std::any_of(fightMobs.begin(), fightMobs.end(),
                               [&](Unit* mob) { return mob && mob->GetGUID() == guid; });
        };
        // the route plan's next stop with an idle mob left (its order: the stops fought already hold none)
        GroupRouteState const& route = group.Route();
        GroupRouteState::PlannedStop const* stop = nullptr;
        std::vector<Creature*> idle;
        for (size_t i = 0; i < route.routePlan.size() && !stop; ++i)
        {
            for (ObjectGuid const& guid : route.routePlan[i].mobs)
                if (Creature* mob = map->GetCreature(guid);
                    mob && mob->IsInWorld() && mob->IsAlive() && !mob->IsInCombat() && !inFight(guid))
                    idle.push_back(mob);
            if (!idle.empty())
            {
                stop = &route.routePlan[i];
                chain.stop = i;
            }
        }
        if (!stop)
            return chain;
        for (Creature* mob : idle)
            if (!chain.anchor || tank->GetExactDist(mob) < tank->GetExactDist(chain.anchor))
                chain.anchor = mob;
        Creature* const anchor = chain.anchor;
        chain.distance = tank->GetExactDist(anchor);
        for (Creature* mob : idle)
            chain.mobs.push_back(mob->GetGUID());
        auto const reject = [&](char const* kind, std::string const& why)
        {
            chain.kind = kind;
            chain.why = why;
            return chain;
        };
        // the run in and the opening (spell data): the opener that reaches the anchor from where the tank stands now
        float const runSpeed = std::max(anchor->GetSpeed(MOVE_RUN), 0.1f);
        float const walk = Leader::WalkYd(anchor, anchor->GetPosition(), tank->GetPosition());
        chain.runInSec = std::max(walk - tank->GetMeleeRange(anchor), 0.0f) / runSpeed;
        SpellInfo const* opener = nullptr;
        SpellInfo const* walkOpener = nullptr;  // [lead1190] the ready ranged opener of the longest range
        std::string walkOpenerName;
        bool onCooldown = false;
        for (char const* name : PullPlanning::TankEngageOpeners(tank, false))
        {
            uint32 const spell = tankAI->GetAiObjectContext()->GetValue<uint32>("spell id", name)->Get();
            SpellInfo const* info = spell && tank->HasSpell(spell) ? sSpellMgr->GetSpellInfo(spell) : nullptr;
            if (!info)
                continue;
            if (tank->HasSpellCooldown(spell))
            {
                onCooldown = true;
                continue;
            }
            if (!walkOpener || info->GetMaxRange(false) > walkOpener->GetMaxRange(false))
            {
                walkOpener = info;
                walkOpenerName = name;
            }
            if (opener || !tank->IsWithinCombatRange(anchor, info->GetMaxRange(false)) ||
                !tank->IsWithinLOSInMap(anchor))
                continue;
            opener = info;
            chain.opener = name;
        }
        // [lead1190] the walk's end: the nearest point on the tank's line to the anchor from which `walkOpener`
        // reaches every mob of `pack` (the centres' gap within its range less the arrival tolerance) in sight
        float const tankSpeed = std::max(tank->GetSpeed(MOVE_RUN), 0.1f);
        auto const walkEnd = [&](std::vector<ObjectGuid> const& pack, std::string& whyNot) -> bool
        {
            chain.walk = false;
            float const length = std::max(tank->GetExactDist2d(anchor), 0.1f);
            float const ux = (anchor->GetPositionX() - tank->GetPositionX()) / length;
            float const uy = (anchor->GetPositionY() - tank->GetPositionY()) / length;
            float const range = walkOpener->GetMaxRange(false) - walkArriveYd;
            float from = 0.0f;
            float to = length;
            for (ObjectGuid const& guid : pack)
            {
                Creature* mob = map->GetCreature(guid);
                if (!mob)
                    continue;
                float const dz = mob->GetPositionZ() - tank->GetPositionZ();
                float const flat = range * range - dz * dz;
                float const mx = mob->GetPositionX() - tank->GetPositionX();
                float const my = mob->GetPositionY() - tank->GetPositionY();
                // |tank + s u - mob|^2 <= flat: s^2 - 2 b s + c <= 0
                float const along = ux * mx + uy * my;
                float const square = along * along - (mx * mx + my * my - flat);
                if (flat <= 0.0f || square < 0.0f)
                {
                    whyNot = mob->GetName() + " of the pack is out of " + walkOpenerName + "'s range on the way";
                    return false;
                }
                from = std::max(from, along - std::sqrt(square));
                to = std::min(to, along + std::sqrt(square));
            }
            if (from > to)
            {
                whyNot = "no point on the way reaches every mob of the pack with " + walkOpenerName;
                return false;
            }
            float x = tank->GetPositionX() + ux * from;
            float y = tank->GetPositionY() + uy * from;
            float z = tank->GetPositionZ();
            if (!map->CheckCollisionAndGetValidCoords(tank, tank->GetPositionX(), tank->GetPositionY(),
                                                      tank->GetPositionZ(), x, y, z))
            {
                whyNot = "no ground on the way";
                return false;
            }
            Position const end(x, y, z);
            for (ObjectGuid const& guid : pack)
                if (Creature* mob = map->GetCreature(guid);
                    mob && (end.GetExactDist(mob) > range ||
                            !map->isInLineOfSight(x, y, z + tank->GetCollisionHeight(), mob->GetPositionX(),
                                                  mob->GetPositionY(), mob->GetPositionZ() + mob->GetCollisionHeight(),
                                                  tank->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                                  VMAP::ModelIgnoreFlags::Nothing)))
                {
                    whyNot = mob->GetName() + " of the pack is out of reach or sight from the walk's end";
                    return false;
                }
            chain.walk = true;
            chain.walkTo = end;
            chain.walkSec = tank->GetExactDist(end) / tankSpeed;
            chain.opener = walkOpenerName;
            chain.runInSec = std::max(end.GetExactDist(anchor) - tank->GetMeleeRange(anchor), 0.0f) / runSpeed;
            chain.openSec = walkOpener->CalcCastTime(tank) / 1000.0f +
                            (walkOpener->Speed > 0.0f ? end.GetExactDist(anchor) / walkOpener->Speed : 0.0f);
            return true;
        };
        bool fromHere = opener != nullptr;
        for (ObjectGuid const& guid : chain.mobs)
            if (Creature* mob = map->GetCreature(guid); fromHere && mob && !TankOpensFromHere(tankAI, tank, mob))
                fromHere = false;
        std::string walkWhy;
        if (fromHere)
            chain.openSec = opener->CalcCastTime(tank) / 1000.0f +
                            (opener->Speed > 0.0f ? chain.distance / opener->Speed : 0.0f);
        else if (walkOpener)
            walkEnd(chain.mobs, walkWhy);
        // [test1] the trigger through the chain pull judge's seam (ChainPullJudge.h): the same expression,
        // callable from a recorded fixture with no map and no clock behind it
        Leader::Judge::ChainTiming const timing{fightSecLeft, chain.walkSec, chain.runInSec, chain.openSec};
        chain.due = Leader::Judge::DueNow(timing);
        // the facts that do not change while the fight goes on first
        for (ObjectGuid const& guid : chain.mobs)
            if (Creature* mob = map->GetCreature(guid); mob && (mob->IsDungeonBoss() || mob->isWorldBoss()))
                return reject("boss", mob->GetName() + " is a boss: his own pull (the buff pass first)");
        if (stop->patrol)
            return reject("walker", anchor->GetName() + "'s stop is a walker's: it meets the plan where it walks");
        for (GroupRouteState::PlannedStop const& meets : route.routePlanMeets)
            if (meets.meetsAt == stop->anchor)
                if (Creature* walker = map->GetCreature(meets.anchor); walker && walker->IsAlive())
                    return reject("walker", walker->GetName() + " walks to that stop's fight");
        if (!fromHere && !chain.walk)
            return reject("opener",
                          !walkOpener ? Acore::StringFormat("no ranged opener of the tank is ready{} ({:.1f} yd)",
                                                            onCooldown ? " (one is on its cooldown)" : "",
                                                            chain.distance)
                                      : Acore::StringFormat("no ranged opener reaches {} from where the tank stands "
                                                            "({:.1f} yd) and no walk to its reach: {}",
                                                            anchor->GetName(), chain.distance, walkWhy));
        if (!chain.due)
        {
            chain.waiting = true;
            return reject("waits", Acore::StringFormat("fight left {:.1f} s >= walk {:.1f} s + run in {:.1f} s + "
                                                       "opening {:.1f} s",
                                                       fightSecLeft, chain.walkSec, chain.runInSec, chain.openSec));
        }
        // the pack: the stop's idle mobs and every idle pack mate of theirs (the core's assistance takes them along);
        // searched once due (a grid visit per stop mob)
        for (Creature* mob : idle)
            for (ObjectGuid const& guid : packOf(mob))
                if (std::find(chain.mobs.begin(), chain.mobs.end(), guid) == chain.mobs.end())
                    chain.mobs.push_back(guid);
        for (ObjectGuid const& guid : chain.mobs)
            if (Creature* mob = map->GetCreature(guid); mob && (mob->IsDungeonBoss() || mob->isWorldBoss()))
                return reject("boss", mob->GetName() + " is a boss: his own pull (the buff pass first)");
        // [lead1190] a pack mate the opener does not reach from here: the walk's end covers the whole pack
        if (!chain.walk)
        {
            Creature* unreached = nullptr;
            for (ObjectGuid const& guid : chain.mobs)
                if (Creature* mob = map->GetCreature(guid); mob && !TankOpensFromHere(tankAI, tank, mob))
                {
                    unreached = mob;
                    break;
                }
            if (unreached && (!walkOpener || !walkEnd(chain.mobs, walkWhy)))
                return reject("pack_reach",
                              unreached->GetName() +
                                  " of the pack: no opener of the tank reaches it from where it stands (it would come "
                                  "unheld)" +
                                  (walkWhy.empty() ? std::string() : "; no walk to its reach: " + walkWhy));
        }
        else if (!walkEnd(chain.mobs, walkWhy))
            return reject("pack_reach", "no walk's end reaches the whole pack: " + walkWhy);
        if (chain.walk && !Leader::Judge::DueNow(
                             Leader::Judge::ChainTiming{fightSecLeft, chain.walkSec, chain.runInSec, chain.openSec}))
        {
            chain.waiting = true;
            return reject("waits", "the whole pack's walk is not due yet");
        }
        // the facts of the fight now
        if (group.NeedBuff())
            return reject("buff", "the group needs its buffs (a buff pass first, out of the fight)");
        std::vector<Player*> everyone;
        for (LeaderGroup::MemberInfo const& member : group.Members())
            if (Player* bot = ObjectAccessor::FindPlayer(member.guid))
            {
                if (!bot->IsAlive())
                    return reject("dead", bot->GetName() + " is dead");
                everyone.push_back(bot);
            }
        for (Unit* mob : fightMobs)
            if (mob && mob->IsAlive() && mob->GetVictim() != tank && !mob->HasBreakableByDamageCrowdControlAura())
                return reject("holds", "the tank does not hold " + mob->GetName());
        std::vector<ObjectGuid> combined;
        for (Unit* mob : fightMobs)
            if (mob && mob->IsAlive())
                combined.push_back(mob->GetGUID());
        combined.insert(combined.end(), chain.mobs.begin(), chain.mobs.end());
        chain.unionMobs = uint32(combined.size());
        std::string judged;
        bool const fits =
            PullPlanner::JudgePack(tank, combined, chain.overload, judged, &chain.drop, &chain.unionSec);
        chain.tankHealth = tank->GetHealthPct() / 100.0f;
        if (!fits)
            return reject("overload", "the overload gate: " + judged);
        if (!Leader::Judge::TankOutlastsDrop(Leader::Judge::ChainSurvival{chain.tankHealth, chain.drop}))
            return reject("health", Acore::StringFormat("the tank at {:.0f} % health, the union's drop {:.0f} %",
                                                        chain.tankHealth * 100.0f, chain.drop * 100.0f));
        // [lead1190] a healer keeps the tank: where it fights by the pack's arrival (its opening and run in) or, for
        // a walk, at the walk's end no later than the tank gets there (the fight's damage goes on along the way)
        if (std::string const healerWhy =
                chain.walk ? HealerKeepsLanding(tank, chain.walkTo, chain.walkSec, everyone, chain.healer)
                           : HealerKeepsLanding(tank, tank->GetPosition(), chain.openSec + chain.runInSec, everyone,
                                                chain.healer);
            !healerWhy.empty())
            return reject("healer", healerWhy);
        if (std::string const dry = Leader::Support::HealerOutOfMana(everyone, chain.unionSec); !dry.empty())
            return reject("mana", dry);
        if (Unit* woken = WalkInWakes(tank, anchor, combined))
            return reject("wakes", "its way in wakes " + woken->GetName());
        if (chain.walk)
        {
            // [lead1190] the tank's walk with the fight: its end and its way out of every unpulled mob's reach (the
            // pack's own too: it is opened from range, not walked into)
            if (Creature* idleMob = LeaderAggroAt(tank, chain.walkTo))
                return reject("wakes", "the walk's end is in " + idleMob->GetName() + "'s reach");
            bool noPath = false;
            if (Creature* idleMob = LeaderAggroOnWalk(tank, chain.walkTo, noPath))
                return reject("wakes", "the walk passes " + idleMob->GetName() + "'s reach");
            if (noPath)
                return reject("opener", "no walk to the opener's reach");
            chain.taken = true;
            chain.kind = "walk";
            chain.why = Acore::StringFormat("{} mobs with {} left, {:.1f} s fight left < walk {:.1f} s + run in "
                                            "{:.1f} s + {} {:.1f} s; {}{}",
                                            chain.mobs.size(), fightMobs.size(), fightSecLeft, chain.walkSec,
                                            chain.runInSec, chain.opener, chain.openSec, judged,
                                            chain.healer.walks && chain.healer.healer
                                                ? "; " + chain.healer.healer->GetName() + " walks along"
                                                : std::string());
            return chain;
        }
        chain.taken = true;
        chain.kind = "taken";
        chain.why = Acore::StringFormat("{} mobs with {} left, {:.1f} s fight left < run in {:.1f} s + {} {:.1f} s; {}",
                                        chain.mobs.size(), fightMobs.size(), fightSecLeft, chain.runInSec,
                                        chain.opener, chain.openSec, judged);
        return chain;
    }

    TankMeetPlan PlanTankMeet(Player* tank, Unit* mob, std::vector<Player*> const& members)
    {
        TankMeetPlan plan;
        PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        Map* map = tank ? tank->GetMap() : nullptr;
        if (!tankAI || !map || !mob || !mob->IsAlive())
            return plan;
        float const dist = std::max(tank->GetExactDist2d(mob), 0.1f);
        plan.gap = std::max(dist - tank->GetMeleeRange(mob), 0.0f);
        float const mobSpeed = std::max(mob->GetSpeed(MOVE_RUN), 0.1f);
        float const tankSpeed = std::max(tank->GetSpeed(MOVE_RUN), 0.1f);
        float const dx = (mob->GetPositionX() - tank->GetPositionX()) / dist;
        float const dy = (mob->GetPositionY() - tank->GetPositionY()) / dist;
        // the point `along` yd from the tank toward the mob, on its ground (false: none)
        auto const pointAt = [&](float along, Position& out) -> bool
        {
            float x = tank->GetPositionX() + dx * along;
            float y = tank->GetPositionY() + dy * along;
            float z = tank->GetPositionZ();
            if (!map->CheckCollisionAndGetValidCoords(tank, tank->GetPositionX(), tank->GetPositionY(),
                                                      tank->GetPositionZ(), x, y, z))
                return false;
            out.Relocate(x, y, z);
            return true;
        };
        auto const groupSec = [&](Position const& landing, float contactSec)
        {
            float sum = contactSec;
            for (Player* member : members)
                if (member && member != tank && member->IsAlive() && !PlayerbotAI::IsHeal(member))
                    sum += MemberFirstHitSec(member, mob, landing, contactSec);
            return sum;
        };
        // stand: the mob runs the gap
        plan.meet = TankMeet::Stand;
        plan.landing = tank->GetPosition();
        plan.standSec = plan.gap / mobSpeed;
        plan.standGroup = groupSec(plan.landing, plan.standSec);
        float best = plan.standGroup;
        // grip: the mob jumps to the tank (spell data range and cast, the core's jump speed)
        if (uint32 const grip = tankAI->GetAiObjectContext()->GetValue<uint32>("spell id", "death grip")->Get();
            grip && tank->HasSpell(grip) && !tank->HasSpellCooldown(grip))
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(grip);
                info && tank->IsWithinCombatRange(mob, info->GetMaxRange(false)) && tank->IsWithinLOSInMap(mob) &&
                tankAI->CanCastSpell("death grip", mob))
            {
                plan.gripSec = info->CalcCastTime(tank) / 1000.0f + dist / DEATH_GRIP_JUMP_SPEED;
                plan.gripGroup = groupSec(tank->GetPosition(), plan.gripSec);
                if (plan.gripGroup < best)
                {
                    best = plan.gripGroup;
                    plan.meet = TankMeet::Grip;
                    plan.spell = "death grip";
                }
            }
        // a landing off the tank's spot: no drag, no unpulled mob's reach there or on the way, a healer reaches it
        for (Unit* attacker : tank->getAttackers())
            if (attacker && attacker != mob && attacker->IsAlive() && tank->IsWithinMeleeRange(attacker))
            {
                plan.why = "the tank holds " + attacker->GetName() + " in its melee";
                return plan;
            }
        // [lead1190] the healer: one that keeps the landing where it stands or by its walk there no later than the
        // tank's contact (`contactSec`: its run speed against the tank's walk; HealerKeepsLanding)
        auto const landingSafe = [&](Position const& landing, float contactSec, HealerAlong& along) -> std::string
        {
            if (Creature* idleMob = LeaderAggroAt(tank, landing))
                return "the landing is in " + idleMob->GetName() + "'s reach";
            bool noPath = false;
            if (Creature* idleMob = LeaderAggroOnWalk(tank, landing, noPath))
                return "the way there passes " + idleMob->GetName() + "'s reach";
            if (noPath)
                return "no walk to the landing";
            return HealerKeepsLanding(tank, landing, contactSec, members, along);
        };
        // walk: both run, the gap closes at both speeds; they meet where the tank's share of it ends
        Position walkTo;
        plan.walkSec = plan.gap / (tankSpeed + mobSpeed);
        if (!pointAt(plan.gap * tankSpeed / (tankSpeed + mobSpeed), walkTo))
            plan.why = "walk: no ground toward the mob";
        else
        {
            HealerAlong along;
            if (std::string const unsafe = landingSafe(walkTo, plan.walkSec, along); !unsafe.empty())
                plan.why = "walk: " + unsafe;
            else
            {
                plan.walkGroup = groupSec(walkTo, plan.walkSec);
                if (plan.walkGroup < best)
                {
                    best = plan.walkGroup;
                    plan.meet = TankMeet::Walk;
                    plan.landing = walkTo;
                    plan.spell.clear();
                    plan.healer = along;
                }
            }
        }
        // a gap closer: the core's charge speed (SPEED_CHARGE); it lands the tank at the mob
        for (char const* name : PullPlanning::TankEngageOpeners(tank, true))
        {
            uint32 const spell = tankAI->GetAiObjectContext()->GetValue<uint32>("spell id", name)->Get();
            SpellInfo const* info = spell && tank->HasSpell(spell) ? sSpellMgr->GetSpellInfo(spell) : nullptr;
            if (!info || !TankPeel::IsGapCloser(info) || tank->HasSpellCooldown(spell) ||
                !tank->IsWithinCombatRange(mob, info->GetMaxRange(false)) ||
                tank->GetExactDist(mob) < info->GetMinRange(false) || !tankAI->CanCastSpell(name, mob))
                continue;
            Position landing;
            float const closerSec = plan.gap / (SPEED_CHARGE + mobSpeed);
            if (!pointAt(plan.gap * SPEED_CHARGE / (SPEED_CHARGE + mobSpeed), landing))
                continue;
            plan.closerSec = closerSec;
            plan.closerGroup = groupSec(landing, closerSec);
            if (plan.closerGroup >= best)
                break;
            HealerAlong along;
            if (std::string const unsafe = landingSafe(landing, closerSec, along); !unsafe.empty())
            {
                plan.why += std::string(plan.why.empty() ? "" : "; ") + name + ": " + unsafe;
                break;
            }
            if (TankWalkIntoIdlePack(tankAI, tank, mob) || !TankGapCloserAllowed(tank, mob, name))
            {
                plan.why += std::string(plan.why.empty() ? "" : "; ") + name + ": the landing check refuses it";
                break;
            }
            best = plan.closerGroup;
            plan.meet = TankMeet::Closer;
            plan.landing = landing;
            plan.spell = name;
            plan.healer = along;
            break;
        }
        return plan;
    }

    void SetTankMeet(Player* tank, Unit* mob, TankMeetPlan const& plan)
    {
        if (!tank || !mob)
            return;
        std::lock_guard<std::mutex> guard(tankMeetLock);
        std::vector<TankMeetEntry>& entries = tankMeets[tank->GetGUID().GetRawValue()];
        // the verdicts of mobs that are gone or dead go (resolved on the leader's tick, the tank's map)
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](TankMeetEntry const& entry)
                                     {
                                         Unit* other = ObjectAccessor::GetUnit(*tank, entry.mob);
                                         return !other || !other->IsAlive() || entry.mob == mob->GetGUID();
                                     }),
                      entries.end());
        entries.push_back({mob->GetGUID(), plan.meet, plan.spell, plan.landing});
    }

    TankMeet TankMeetFor(Player* tank, Unit* mob, std::string* spell, Position* landing)
    {
        if (!tank || !mob)
            return TankMeet::None;
        std::lock_guard<std::mutex> guard(tankMeetLock);
        auto const found = tankMeets.find(tank->GetGUID().GetRawValue());
        if (found == tankMeets.end())
            return TankMeet::None;
        for (TankMeetEntry const& entry : found->second)
            if (entry.mob == mob->GetGUID())
            {
                if (spell)
                    *spell = entry.spell;
                if (landing)
                    *landing = entry.landing;
                return entry.meet;
            }
        return TankMeet::None;
    }
}
