/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "LeaderCombatEnd.h"

#include "LeaderReports.h"
#include "LeaderEvents.h"
#include "AggressiveTargetValue.h"
#include "AiObjectContext.h"
#include "AKTriggers.h"
#include "ANAddTactics.h"
#include "Bag.h"
#include "BotMoveEvents.h"
#include "CellImpl.h"
#include "Chat.h"
#include "CoSTriggers.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "DBCStores.h"
#include "Engine.h"
#include "EscortProtect.h"  // [b36_ohb] EscortGossipHeld
#include "EstimatedLifetimeValue.h"
#include "GameObject.h"
#include "GameObjectModel.h"
#include "GameTime.h"
#include "GDTriggers.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "HealthTriggers.h"
#include "RaidLeaderState.h"
#include "HoRTriggers.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MechShared.h"
#include "MgTShared.h"
#include "ClassicShared.h"  // [b32_smpull] Mograine's linked chapel trash
#include "MotionMaster.h"
#include "OCTriggers.h"
#include "MoveSpline.h"
#include "AttackerCountValues.h"
#include "MovementActions.h"
#include "LeaderOrders.h"  // [lead1] the tank's pull orders
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "LeaderKnowledge.h"
#include "RouteSearch.h"  // [lead99] the leader's route search
#include "PullFacts.h"    // [lead104] the leader's pull / fight facts
#include "LeaderFightChecks.h"  // [lead107] the leader's checks
#include "LeaderPlan.h"         // [lead107] the leader's planner tick
#include "Pet.h"  // [lead38] the pet redirect
#include <source_location>  // [lead76]
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSpellRepository.h"
#include "Playerbots.h"
#include "PullStrategy.h"
#include "PullActions.h"  // [b39_strat] FightHold: the opening fight held on the use side of a key gate
#include "ChooseTargetActions.h"  // [b47_door] PullReadiness::GroupPreparing
#include "SmartAI.h"
#include "SmartScriptMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StuckWatchdog.h"
#include "DamageTaken.h"
#include "DungeonHazard.h"   // [lead16] the leader's hazard view
#include "LeaderActions.h"   // [lead19] the member guard model (LeaderMoveOrderAction::WalkRefusal)
#include "RtiValue.h"        // [lead16] the leader's CC assignments
#include "DungeonTactics.h"  // [lead15] the leader's cleave dodge (InFrontalCleave)
#include "InstanceScript.h"
#include "InstanceSaveMgr.h"
#include "SelfTravelActions.h"
#include "TemporarySummon.h"
#include "TerrainHazard.h"
#include "TestRunnerStrategy.h"
#include "Transport.h"
#include "UKTriggers.h"
#include "UPTriggers.h"
#include "Timer.h"
#include <chrono>  // [lead15] the leader cadence cost
#include <functional>
#include <optional>  // [lead18]
#include "WaypointMgr.h"
#include "WorldPacket.h"
#include "World.h"
#include "WorldSession.h"
#include "G3D/Vector4.h"  // [b32_brs] HopelessDetourLeg discs
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "RouteTargets.h"
#include "RouteSteer.h"

namespace Leader::Fight
{
    void TickPhantomCombat(LeaderGroup& group, std::vector<Player*> const& players, Player* leader,
                           uint32 now, bool activeBoss, bool& anyInCombat)
    {
        GroupRouteState& route = group.Route();
        // Phantom combat: somebody stays flagged in combat while nobody of the party deals, takes or heals anything
        // (a mob stuck on a ledge, a fish below the surface). It held Ragefire Chasm in "clearing trash" for 18
        // minutes: no stall detection, no resurrection (the paladin's Redemption is a non-combat action). After
        // PHANTOM_COMBAT_MS the run treats the party as out of combat; after PHANTOM_COMBAT_END_MS the stale combat
        // references are ended, as an evading mob would. Not during a boss attempt (phases without damage).
        if (anyInCombat && !activeBoss)
        {
            uint64 activity = group.CombatActivity();

            if (!route.phantomCombatSinceMs || activity != route.combatActivity)
            {
                route.combatActivity = activity;
                route.phantomCombatSinceMs = now;
                route.phantomCombatLogged = false;
                route.phantomCombatEnded = false;
                route.phantomCombatEndedMs = 0;  // [b33_sh2]
                route.phantomCombatEnds = 0;
            }
            else if (getMSTimeDiff(route.phantomCombatSinceMs, now) > PHANTOM_COMBAT_MS)
            {
                if (!route.phantomCombatLogged)
                {
                    route.phantomCombatLogged = true;
                    JsonArray refs;
                    for (Player* bot : players)
                    {
                        if (!bot->IsAlive() || !bot->IsInCombat())
                            continue;
                        for (auto const& [guid, ref] : bot->GetCombatManager().GetPvECombatRefs())
                        {
                            Unit* other = ref ? ref->GetOther(bot) : nullptr;
                            if (!other)
                                continue;
                            Unit* victim = other->GetVictim();
                            refs.Raw(JsonObject().Str("member", bot->GetName()).Str("with", other->GetName())
                                         .Num("entry", other->IsCreature() ? other->GetEntry() : 0)
                                         .Bool("alive", other->IsAlive())
                                         .Bool("evading", other->IsCreature() && other->ToCreature()->IsInEvadeMode())
                                         .Bool("underWater", other->IsUnderWater())
                                         .Real("dist", bot->GetExactDist(other))
                                         .Real("dz", other->GetPositionZ() - bot->GetPositionZ())
                                         .Str("victim", victim ? victim->GetName() : "").Build());
                        }
                    }
                    group.LogEvent("phantom_combat",
                             JsonObject().Num("sec", getMSTimeDiff(route.phantomCombatSinceMs, now) / IN_MILLISECONDS)
                                 .Raw("refs", refs.Build()));

                    // [phantom_mob_hunt] User 2026-10-25: «стоит моб, а группа просто стоит... почему группа не бежит
                    // и не бьет». strat-h 20261025-163739-strat-h-5A-11 pulls 52-55: a Crimson Initiate 10420 alive,
                    // not evading, 97-133 yd off on the hunter's Raptor kept every pull 45 s in phantom combat. A live
                    // hostile the leader can walk to (a complete path) is made the group's target; only an
                    // unreachable one has its combat ended below.
                    if (route.phantomHuntTries < PHANTOM_HUNT_MAX_TRIES)
                    {
                        Creature* hunt = nullptr;
                        float huntDistance = 0.0f;
                        std::string huntPath;
                        auto const consider = [&](Unit* owner)
                        {
                            for (auto const& [guid, ref] : owner->GetCombatManager().GetPvECombatRefs())
                            {
                                Unit* other = ref ? ref->GetOther(owner) : nullptr;
                                Creature* creature = other ? other->ToCreature() : nullptr;
                                if (!creature || !creature->IsAlive() || creature->IsInEvadeMode() ||
                                    !leader->IsValidAttackTarget(creature) || creature->GetMap() != leader->GetMap())
                                    continue;

                                float const distance = leader->GetExactDist(creature);
                                if (hunt && distance >= huntDistance)
                                    continue;

                                PathGenerator path(leader);
                                path.CalculatePath(creature->GetPositionX(), creature->GetPositionY(),
                                                   creature->GetPositionZ());
                                if (path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
                                    continue;

                                hunt = creature;
                                huntDistance = distance;
                                huntPath = Acore::StringFormat("path type {}, {} points", uint32(path.GetPathType()),
                                                               path.GetPath().size());
                            }
                        };
                        for (Player* bot : players)
                        {
                            if (!bot->IsAlive())
                                continue;
                            consider(bot);
                            for (Unit* controlled : bot->m_Controlled)
                                if (controlled && controlled->IsInWorld())
                                    consider(controlled);
                        }

                        if (hunt)
                        {
                            ++route.phantomHuntTries;
                            route.phantomHunt = hunt->GetGUID();
                            route.neighbourTarget = hunt->GetGUID();
                            route.neighbourTargetUntilMs = now + PHANTOM_HUNT_MS;
                            route.target = hunt->GetGUID();
                            route.targetIsBoss = false;
                            route.targetIsStarter = false;
                            route.targetRouteRule = "phantom mob hunt";
                            route.targetSinceMs = now;
                            route.guaranteeHolding = false;
                            route.arrivedMs = 0;
                            route.approach = 0;
                            route.approachBlocker.Clear();
                            route.stopSinceMs = 0;
                            Unit* victim = hunt->GetVictim();
                            LOG_INFO("playerbots", "[phantom_mob_hunt] {}: {} {} alive, {:.0f} % health, {:.1f} yd "
                                     "(dz {:.1f}), on {}: reachable ({}), the group's target (try {} of {})",
                                     leader->GetName(), hunt->GetName(), hunt->GetEntry(), hunt->GetHealthPct(),
                                     huntDistance, hunt->GetPositionZ() - leader->GetPositionZ(),
                                     victim ? victim->GetName() : "nobody", huntPath, route.phantomHuntTries,
                                     PHANTOM_HUNT_MAX_TRIES);
                            group.LogEvent("phantom_mob_hunt",
                                     JsonObject().Str("mob", hunt->GetName()).Num("entry", hunt->GetEntry())
                                         .Real("hpPct", hunt->GetHealthPct()).Real("dist", huntDistance)
                                         .Real("dz", hunt->GetPositionZ() - leader->GetPositionZ())
                                         .Str("victim", victim ? victim->GetName() : "").Str("path", huntPath)
                                         .Num("try", route.phantomHuntTries));
                        }
                    }
                }

                // [phantom_mob_hunt] while the group walks to a hunted mob its combat stays: no end, the clock again
                Creature* hunted = route.phantomHunt ? leader->GetMap()->GetCreature(route.phantomHunt) : nullptr;
                bool const hunting = hunted && hunted->IsAlive() && route.target == route.phantomHunt &&
                                     int32(now - route.neighbourTargetUntilMs) < 0;
                if (!hunting)
                    route.phantomHunt.Clear();

                // [b33_sh2] sh-n 20261026-003732-sh-n-5B-89: the one end at 1421 s left the tank Rlwaraj flagged in
                // combat (0 attackers, a stealthed Shattered Hand Assassin 17695 19 yd off that the hunt cannot see);
                // nobody left combat, so the episode never reset and never ended again, and Steer held every walk
                // on leader->IsInCombat() for 90 s. The end repeats every PHANTOM_COMBAT_END_MS while it lasts.
                bool const endAgain = route.phantomCombatEnded && route.phantomCombatEndedMs &&
                                      getMSTimeDiff(route.phantomCombatEndedMs, now) > PHANTOM_COMBAT_END_MS;
                if ((!route.phantomCombatEnded || endAgain) && !hunting &&
                    getMSTimeDiff(route.phantomCombatSinceMs, now) > PHANTOM_COMBAT_END_MS)
                {
                    route.phantomCombatEnded = true;
                    route.phantomCombatEndedMs = now ? now : 1;
                    ++route.phantomCombatEnds;
                    // the pets and guardians too: a pet's ref put its owner back in combat after every pull
                    // (strat-h 5A-11: the Crimson Initiate on the hunter's Raptor, 45 s after pulls 52-55)
                    uint32 controlledEnded = 0;
                    for (Player* bot : players)
                    {
                        if (!bot->IsAlive())
                            continue;
                        bot->GetCombatManager().EndAllPvECombat();
                        for (Unit* controlled : bot->m_Controlled)
                        {
                            if (controlled && controlled->IsInWorld() && controlled->IsInCombat())
                            {
                                controlled->GetCombatManager().EndAllPvECombat();
                                ++controlledEnded;
                            }
                        }
                    }
                    // [b33_sh2] who is still flagged right after the end, and with what (the ref that comes back)
                    JsonArray stillFlagged;
                    for (Player* bot : players)
                    {
                        if (!bot->IsAlive() || !bot->IsInCombat())
                            continue;
                        std::string with;
                        for (auto const& [guid, ref] : bot->GetCombatManager().GetPvECombatRefs())
                            if (Unit* other = ref ? ref->GetOther(bot) : nullptr)
                                with += (with.empty() ? "" : ", ") + std::string(other->GetName());
                        stillFlagged.Raw(JsonObject().Str("member", bot->GetName()).Str("with", with)
                                             .Num("attackers", bot->getAttackers().size())
                                             .Bool("pvp", bot->GetCombatManager().HasPvPCombat()).Build());
                    }
                    group.LogEvent("phantom_combat_end",
                             JsonObject().Num("sec", getMSTimeDiff(route.phantomCombatSinceMs, now) / IN_MILLISECONDS)
                                 .Num("petsEnded", controlledEnded).Num("huntTries", route.phantomHuntTries)
                                 .Num("end", route.phantomCombatEnds).Raw("stillFlagged", stillFlagged.Build()));
                }
                anyInCombat = false;
            }
        }
        else if (!anyInCombat)
        {
            route.phantomCombatSinceMs = 0;
            route.phantomHuntTries = 0;  // [phantom_mob_hunt] a real fight end: the next phantom gets its tries
            route.phantomHunt.Clear();
        }
    }


    void TickStaleVictim(LeaderGroup& group, Player* leader, uint32 now, bool anyInCombat, bool activeBoss)
    {
        GroupRouteState& route = group.Route();
        // [queue-stale-idle-victim] build 27 (ac-n 20261025-150737-ac-n-5A-55, ub-n 150737-ub-n-5A-29, strat-h
        // 150737-strat-h-5A-10): the tank's own target pick took an idle boss (Shirrak / Hungarfen / Hearthsinger)
        // into its swing and the combat engine, out of any fight (no attacker, not in combat), while
        // [reach_idle_waits_ready] (ReachMeleeAction::isUseful) refuses the walk until the group is ready - the tank
        // at 0-67 % mana. In the combat engine it neither drinks nor walks the harness waypoint ("melee (5.00) ->
        // Shirrak 24.8 yd" every tick, 219 s / 240 s / 181 s without a fight). A swing / target on an idle mob out of
        // melee range for STALE_VICTIM_MS outside any fight is dropped: AttackStop, no current target, the non-combat
        // engine - the rest triggers run, and the tank's own pick takes the mob again once the group is ready.
        constexpr uint32 STALE_VICTIM_MS = 15 * IN_MILLISECONDS;
        if (!anyInCombat && !activeBoss && leader->IsAlive() && !leader->IsInCombat() &&
            leader->getAttackers().empty())
        {
            PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
            Unit* victim = leader->GetVictim();
            Unit* current = leaderAI ? leaderAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get()
                                     : nullptr;
            Unit* stale = victim ? victim : current;
            bool const idleOut = stale && stale->IsCreature() && stale->IsAlive() && !stale->IsInCombat() &&
                                 !leader->IsWithinMeleeRange(stale);
            if (!idleOut)
            {
                route.staleVictim.Clear();
                route.staleVictimSinceMs = 0;
            }
            else if (route.staleVictim != stale->GetGUID())
            {
                route.staleVictim = stale->GetGUID();
                route.staleVictimSinceMs = now ? now : 1;
            }
            else if (getMSTimeDiff(route.staleVictimSinceMs, now) > STALE_VICTIM_MS)
            {
                ++route.staleVictimReleases;
                group.LogEvent("stale_idle_victim_release",
                         JsonObject().Str("name", stale->GetName()).Num("entry", stale->GetEntry())
                             .Bool("boss", stale->ToCreature()->IsDungeonBoss())
                             .Real("distance", leader->GetExactDist(stale))
                             .Num("sec", getMSTimeDiff(route.staleVictimSinceMs, now) / IN_MILLISECONDS)
                             .Real("leaderMana",
                                   leader->GetMaxPower(POWER_MANA) ? leader->GetPowerPct(POWER_MANA) : 100.0f)
                             .Num("releases", route.staleVictimReleases));
                LOG_INFO("playerbots",
                         "[queue-stale-idle-victim] {}: {} held idle {} ({:.1f} yd) as its target outside a fight for "
                         "{} s - dropped (release {})",
                         group.Run().runId, leader->GetName(), stale->GetName(), leader->GetExactDist(stale),
                         getMSTimeDiff(route.staleVictimSinceMs, now) / IN_MILLISECONDS, route.staleVictimReleases);
                leader->AttackStop();
                leader->SetTarget(ObjectGuid::Empty);
                if (leaderAI)
                {
                    leaderAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
                    leaderAI->ChangeEngine(BOT_STATE_NON_COMBAT);
                }
                route.staleVictim.Clear();
                route.staleVictimSinceMs = 0;
            }
        }
    }
}
