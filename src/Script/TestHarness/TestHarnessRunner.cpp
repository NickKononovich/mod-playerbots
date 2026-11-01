/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TestHarnessMgr.h"
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
#include "RouteSearch.h"  // [lead99] the leader's route search
#include "RouteLegs.h"    // [lead107] the leader's route legs
#include "RoutePatrol.h"  // [lead107] the leader's patrol decision
#include "RouteEvents.h"  // [lead107] the leader's event route steps
#include "RouteTargets.h"  // [lead107] the leader's route target choice
#include "RouteSteer.h"    // [lead107] the leader's route walk
#include "RouteMeet.h"     // [lead107] the leader's meet
#include "RouteWater.h"    // [lead107] the leader's water rule
#include "PullFacts.h"    // [lead104] the leader's pull / fight facts
#include "LeaderFightChecks.h"  // [lead107] the leader's checks
#include "LeaderEvents.h"       // [lead107] the leader's event tick
#include "LeaderPlan.h"         // [lead107] the leader's planner tick
#include "LeaderReports.h"      // [lead107] the order reports
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

namespace TestHarness
{
    // A body pull is the last resort when the pull logic keeps holding back. A real tank only walks into a pack
    // with the whole group present, alive, healthy and the healers' mana up.
    // [lead6] The group's readiness with its reason ("" = ready): GroupFullyReady, and the leader's one readiness
    // verdict (TestHarnessMgr::LeaderReadyWhy) built on it.
    static std::string GroupNotReadyWhy(Party const& party, Player* leader)
    {
        // [lead17b] LEADER_ONE_GATHER_OWNER: a led party's readiness is the leader's one verdict (LeaderReadyWhy,
        // published every 500 ms) - the harness's body pull / boss pull / skip gates read it, no lines of their own
        if (Leader::LEADER_ONE_GATHER_OWNER && Leader::LEADER_PLANNER_IN_LEADER && party.content &&
            !party.content->raid && party.content->size <= 5 && !party.leader.IsEmpty() &&
            Leader::Orders::IsTracked(party.leader) && party.lead.readyCheckMs)
            return party.lead.ready ? std::string()
                                    : (party.lead.notReadyWhy.empty() ? std::string("the leader holds")
                                                                      : party.lead.notReadyWhy);
        for (Member const& member : party.members)
        {
            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            // [b30_topped_up] a far member the far wait gave up on (far_wait_dropped) holds no readiness while far
            // [b33_bodypull] ...never a healer: strat-h 20261026-000434-strat-h-5D-1 - the ping-pong hold timed out at
            // 179 s (apartIgnoreUntilMs for 180 s on Rtshaab), so the group read "fully ready" with the healer 54-90 yd
            // off; pull_after_hold ("readySec 10") cleared the planner's "healer not with the group" hold and the
            // harness body-pulled the Ravaged Cadavers at 300 s: 17 mobs, the healer 66.8 yd away, wipe.
            if (bot && bot != leader && bot->IsAlive() && bot->GetMap() == leader->GetMap() &&
                bot->GetDistance(leader) > 30.0f && member.apartIgnoreUntilMs &&
                int32(getMSTime() - member.apartIgnoreUntilMs) < 0 && !PlayerbotAI::IsHeal(bot))
                continue;
            if (!bot || !bot->IsAlive())
                return member.name + " is dead or gone";
            if (bot->GetMap() != leader->GetMap())
                return member.name + " is not in the instance";
            if (bot->GetDistance(leader) > 30.0f)
                return Acore::StringFormat("{} is {:.0f} yd from the leader", member.name, bot->GetDistance(leader));
            if (bot->GetHealthPct() < 80.0f)
                return Acore::StringFormat("{} at {:.0f} % health", member.name, bot->GetHealthPct());
            // [b33_bodypull] the healer also sees the leader (the planner's "healer not with the tank")
            if (bot != leader && PlayerbotAI::IsHeal(bot) && !bot->IsWithinLOSInMap(leader))
                return "the healer " + member.name + " does not see the leader";
            if (PlayerbotAI::IsHeal(bot) && bot->getPowerType() == POWER_MANA && bot->GetPowerPct(POWER_MANA) < 80.0f)
                return Acore::StringFormat("the healer {} at {:.0f} % mana", member.name,
                                           bot->GetPowerPct(POWER_MANA));
        }
        // [hleft-mgt-selin-entry] Magisters' Terrace: nobody pulls Selin while a member is still outside her room
        // (MagistersTerrace::SelinRoom::EntryHolds: the room door shuts on IN_PROGRESS).
        if (MagistersTerrace::SelinRoom::EntryHolds(leader))
            return "a member is outside Selin's room";
        return std::string();
    }

    static bool GroupFullyReady(Party const& party, Player* leader) { return GroupNotReadyWhy(party, leader).empty(); }

    using Leader::Dungeon::MemberFull;  // [lead107] Ai/Leader/Dungeon/LeaderEvents.h

    namespace
    {
        using Leader::Travel::MAX_PATH_CANDIDATES;  // [lead107] RouteTargets.h
        using Leader::Travel::WAYPOINT_STEP;  // [lead107] RouteEvents.h
        using Leader::Travel::STEEP_WAYPOINT_STEP;  // [lead107] RouteEvents.h
        using Leader::Travel::PULL_RANGE;  // [lead107] RouteSteer.h
        using Leader::Travel::BODY_PULL_RANGE;  // [lead107] RouteSteer.h
        using Leader::Travel::BODY_PULL_MELEE_SLACK;  // [lead107] RouteSteer.h
        using Leader::Travel::BODY_PULL_STUCK_REACH;  // [lead107] RouteSteer.h
        using Leader::Travel::STARTER_RANGE;  // [lead107] RouteSteer.h
        using Leader::Travel::FAILED_BOSS_RADIUS;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_ROUTE_WEIGHT;  // [lead107] RouteTargets.h
        using Leader::Travel::LATER_BOSS_TRASH_RADIUS;  // [lead107] RouteTargets.h
        using Leader::Travel::DEFERRED_TRASH_PENALTY;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_BUFF_DISTANCE;  // [lead107] RouteSteer.h
        constexpr float BOSS_PREPOT_DISTANCE = 35.0f;
        using Leader::Travel::UNREACHABLE_SKIP_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::UNREACHABLE_RECHECK_DISTANCE;  // [lead107] RouteTargets.h
        using Leader::Travel::UNREACHABLE_RECHECK_HEIGHT;  // [lead107] RouteTargets.h
        using Leader::Travel::ORDER_HOLD_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::NOT_PULLED_SKIP_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::PULL_WAIT_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::EVENT_WAIT_FOR_SPAWNS_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::BODY_PULL_WAIT_MS;  // [lead107] RouteSteer.h
        using Leader::Dungeon::CAREFUL_REFUSED_SKIP_MS;  // [lead107] LeaderPlan.h (3 min)
        using Leader::Travel::ROUTE_ADOPT_GROUP_ESCORT;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_CAREFUL_GUARANTEE_HOLD;  // [lead107] RouteSteer.h
        using Leader::Travel::GUARANTEE_EXECUTE_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::GUARANTEE_HOLD_TOTAL_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::GUARANTEE_UNKNOWN_HOLD_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::PATROL_WAIT_HOLD_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::HELD_FOR_NOW_MAX_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::ROUTE_LIFT_REFUSED_SKIP;  // [lead107] RouteTargets.h
        using Leader::Travel::LIFT_REFUSALS_MAX;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_PULL_HOLD_WAIT_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::SAME_ENTRANCE_RANGE;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_WING_DONE_CHANGE;  // [lead107] RouteTargets.h
        using Leader::Travel::WING_DONE_NO_TARGET_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::GUARD_APPROACH_ARRIVE;  // [lead107] RouteSteer.h
        using Leader::Travel::GUARD_APPROACH_SLACK;  // [lead107] RouteSteer.h
        using Leader::Travel::GUARD_REFUSALS_BEFORE_BODY_PULL;  // [lead107] RouteSteer.h
        using Leader::Travel::RITUAL_CHANNEL_HOLD_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::STARTER_DOOR_CHECK_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::STARTER_DOOR_REARM_MAX;  // [lead107] RouteTargets.h
        using Leader::Travel::EVENT_GATHER_ARRIVE;  // [lead107] RouteEvents.h
        constexpr float PULL_HEALER_RANGE = 40.0f;
        constexpr uint32 TRIGGER_REFIRE_MS = 60 * IN_MILLISECONDS;
        constexpr uint32 TRIGGER_MAX_FIRES = 5;
        // [b32_pos] the trigger of a progress step that holds (EventSpotDef::progressTrigger) re-fires this often,
        // uncapped: its fire is what checks the kill condition (Pit of Saron 5579 / 5580).
        constexpr uint32 TRIGGER_STEP_REFIRE_MS = 10 * IN_MILLISECONDS;
        using Leader::Travel::BRD_MAP_ID;  // [lead107] RouteLegs.h
        using Leader::Travel::BRD_TYPE_RING_OF_LAW;  // [lead107] RouteLegs.h
        constexpr uint32 BRD_DATA_TIME_RING_FAIL = 41;  // DATA_COREN 34, then ANUBSHIAH .. OKTHOR 35-40, then this
        constexpr uint32 BRD_RING_TRIGGER = 1526;
        using Leader::Travel::BRD_GO_ARENA_EXIT_GATE;  // [lead107] RouteLegs.h
        constexpr int64 BRD_RING_LOCKOUT_SEC = 2 * 60 + 5;  // at_ring_of_law: 2 * 60 s after the FAIL, + 5 s margin
        constexpr uint8 BRD_RING_MAX_FAILS = 3;
        using Leader::Travel::EVENT_HOLD_LOG_MS;  // [lead107] RouteTargets.h
        constexpr uint32 ENTRANCE_REFIRE_MS = 10 * IN_MILLISECONDS;
        constexpr uint32 SPELL_HYPERSPEED_ACCELERATION = 54758;
        constexpr float GHOST_PATH_MAX_DROP = 60.0f;
        constexpr uint32 GHOST_STUCK_MS = 60 * IN_MILLISECONDS;
        using Leader::Travel::ESCORT_OPENED_DOORS;  // [lead107] RouteTargets.h
        using Leader::Travel::TRAP_GATE_DOORS;  // [lead107] RouteTargets.h
        using Leader::Travel::TRAP_GATE_LOG_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::IsTrapGate;  // [lead107] RouteTargets.h
        using Leader::Travel::NEIGHBOUR_TARGET_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::UNSEEN_BLOCKER_PACK_MAX;  // [lead107] RouteSteer.h
        using Leader::Travel::ROUTE_BLOCKER_BODY_PULL;  // [lead107] RouteSteer.h
        using Leader::Travel::ROUTE_BLOCKER_MAX_TRIES;  // [lead107] RouteSteer.h
        using Leader::Travel::ROUTE_BLOCKER_PREP_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::ROUTE_BLOCKER_SIGHT_HOLD_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::ROUTE_BLOCKER_THROUGH_RANGE;  // [lead107] RouteSteer.h
        using Leader::Travel::NEIGHBOUR_HOLD_MAX;  // [lead107] RouteSteer.h
        using Leader::Travel::NEIGHBOUR_PASS_PACK_MAX;  // [lead107] RouteSteer.h
        using Leader::Travel::MEET_PASS_PACK_MAX;  // [lead107] RouteMeet.h
        using Leader::Travel::BEHIND_SHADOW_RANGE;  // [lead107] RouteSteer.h
        using Leader::Travel::BEHIND_CONE_COS;  // [lead107] RouteSteer.h
        constexpr uint32 OUTSIDE_GIVE_UP_STUCKS = 3;  // alive outside: 3 x GHOST_STUCK_MS without progress
        constexpr float ENTRY_REFUSAL_RANGE = 20.0f;                           // 2D, a member on its entrance
        constexpr uint32 ENTRY_REFUSED_MAX_MS = 5 * MINUTE * IN_MILLISECONDS;  // then the run ends with the cause
        using Leader::Travel::HOR_MAP;  // [lead107] RouteSteer.h
        constexpr uint32 HOR_DATA_WAVE_NUMBER = 8;  // halls_of_reflection.h DATA_WAVE_NUMBER
        using Leader::Travel::SCRIPT_EVENT_MAX_MS;  // [lead107] RouteSteer.h
        constexpr float GHOST_DIRECT_RANGE = 40.0f;  // no usable path this close to the portal: straight to it
        constexpr float GHOST_ARRIVED_RANGE = 8.0f;  // standing on the portal, not worth a log line
        constexpr float GHOST_CORNER_STEP = 150.0f;  // one move along a corner path (corpse runs over 590 yd)
        constexpr float FLY_CRUISE_HEIGHT = 30.0f;   // above the higher of start and portal
        constexpr float FLY_ARRIVE_DISTANCE = 6.0f;  // 2D distance at which the descent to the portal starts
        constexpr float FLY_SIDE_DISTANCE = 25.0f;   // the descent point beside the portal (ROUTE_FLY_SIDE_APPROACH)
        constexpr uint32 MOUNT_CAST_WAIT_MS = 4 * IN_MILLISECONDS;
        using Leader::Travel::BREATH_MAX_MS;  // [lead107] RouteWater.h
        using Leader::Travel::BREATH_REFILL_RATE;  // [lead107] RouteWater.h
        using Leader::Travel::BREATH_SURFACE_MS;  // [lead107] RouteWater.h
        using Leader::Travel::BREATH_RESUME_MS;  // [lead107] RouteWater.h
        using Leader::Travel::SURFACE_HOLD_MS;  // [lead107] RouteWater.h
        using Leader::Travel::DEEP_TARGET_DEPTH;  // [lead107] RouteTargets.h
        using Leader::Travel::DRY_SPOT_SPREAD;  // [lead107] RouteEvents.h
        using Leader::Travel::ROUTE_SKIP_BEHIND_SKIPPED;  // [lead107] RouteSteer.h
        using Leader::Travel::ROUTE_STOP_KEEPS_WAIT;  // [lead107] RouteSteer.h
        using Leader::Travel::STOP_SAME_RANGE;  // [lead107] RouteSteer.h
        using Leader::Travel::STOP_KEEP_GAP_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::SAME_TARGET_MAX;  // [lead107] RouteTargets.h
        using Leader::Travel::RESPAWNING_SKIP_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::ESCORT_FIGHT_RADIUS;  // [lead107] RouteTargets.h
        using Leader::Travel::ESCORT_FOLLOW_RANGE;  // [lead107] RouteEvents.h
        using Leader::Travel::ESCORT_IDLE_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::ESCORT_MAX_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::ESCORT_DEATH_REARM_MAX;  // [lead107] RouteEvents.h
        using Leader::Travel::EVENT_SPOT_HEIGHT;  // [lead107] RouteEvents.h
        using Leader::Travel::HOLD_TARGET_RADIUS;  // [lead107] RouteTargets.h
        using Leader::Travel::HOLD_GUARD_PRIORITY;  // [lead107] RouteTargets.h
        using Leader::Travel::HOLD_GUARD_SHIELD_PCT;  // [lead107] RouteTargets.h
        using Leader::Travel::HOLD_KEEPER_DISTANT;  // [lead107] RouteTargets.h
        using Leader::Travel::HOLD_EVENT_MOB_RANGE;  // [lead107] RouteTargets.h
        using Leader::Travel::BLACK_MORASS_TIME_RIFT;  // [lead107] RouteTargets.h
        using Leader::Travel::BLACK_MORASS_MAP;  // [lead107] RouteTargets.h
        using Leader::Travel::BM_KEEPER_FETCH_RANGE;  // [lead107] RouteTargets.h
        using Leader::Travel::HOLD_OBJECTIVE_PRIORITY;  // [lead107] RouteTargets.h
        using Leader::Travel::GUARD_TARGET_KEEP;  // [lead107] RouteTargets.h
        using Leader::Travel::VIOLET_HOLD_MAP_ID;  // [lead107] RouteEvents.h
        constexpr uint32 GHOST_POS_MS = 30 * IN_MILLISECONDS;  // "ghost_pos" while members are outside
        constexpr size_t GHOST_POS_TRAIL = 300;
        using Leader::Travel::STEER_PROGRESS_RANGE;  // [lead107] RouteEvents.h
        using Leader::Travel::STEER_STUCK_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::STEER_REST_NOT_STUCK;  // [lead107] RouteSteer.h
        using Leader::Travel::STEER_TWIN_RANGE;  // [lead107] RouteSteer.h
        using Leader::Travel::STEER_GAP_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::STARTER_STUCK_RETRY_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::STARTER_NO_NEARER_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::DOOR_REARM_MAX;  // [lead107] RouteTargets.h
        using Leader::Travel::KEY_DOOR_CAST_RANGE;  // [lead107] RouteEvents.h
        using Leader::Travel::KEY_DOOR_HOLD_FRESH_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::KEY_DOOR_HELD_RANGE;  // [lead107] RouteTargets.h
        constexpr uint32 REARM_CHECK_LOG_MS = 15 * IN_MILLISECONDS;
        using Leader::Travel::KEY_DOOR_REACH;  // [lead107] RouteTargets.h
        // In-combat re-keys (ReopenKeyDoorInCombat), within DOOR_REARM_MAX: strat-h wave 11 spent all 6 in 70 s of one
        // fight. At most 3, 15 s apart, and only while nothing hits the leader (a hit interrupts the Opening cast).
        // [b51_strat] no small budget and a short gap: the gate is shut again (GO_STATE_READY) before any re-key, and a
        // key's 5 s Opening + 3 s open is the natural spacing
        constexpr uint32 DOOR_REARM_COMBAT_MAX = 1000;
        constexpr uint32 DOOR_REARM_COMBAT_GAP_MS = 6 * IN_MILLISECONDS;
        using Leader::Travel::DOOR_REARM_GAP_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::STARTER_OBJECT_STUCK_SKIP_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::GAP_CROSS_MAX_DISTANCE;  // [lead107] RouteSteer.h
        using Leader::Travel::GAP_CROSS_TRIES;  // [lead107] RouteSteer.h
        using Leader::Travel::GAP_REACHABLE_LEFT;  // [lead107] RouteLegs.h
        using Leader::Travel::GAP_REACHABLE_DZ;  // [lead107] RouteLegs.h
        using Leader::Travel::GAP_SWIM_ABOVE;  // [lead107] RouteLegs.h
        // AssistMember: a member with attackers this far from the leader, or on another floor.
        constexpr float ASSIST_RANGE = 25.0f;
        constexpr float ASSIST_FLOOR_DZ = 5.0f;
        using Leader::Travel::MEET_RANGE;  // [lead107] RouteMeet.h
        using Leader::Travel::MEET_END_RANGE;  // [lead107] RouteMeet.h
        // MemberStranded: the navmesh check per far member at most this often; stranded after this long within the
        // still range (Pit of Saron wave 7: Rtshaba on a 7-polygon island 42 yd up the Garfrost wall; Gundrak: the
        // moat floor under Gal'darah's bridge).
        constexpr uint32 STRANDED_RECHECK_MS = 10 * IN_MILLISECONDS;
        constexpr uint32 STRANDED_AFTER_MS = 30 * IN_MILLISECONDS;
        constexpr float STRANDED_STILL_RANGE = 20.0f;
        constexpr float STRANDED_LEVEL_DZ = 6.0f;      // REGROUP_PING_PONG_LEVEL_DZ (TestHarnessMgr.cpp)
        constexpr float STRANDED_LEVEL_RANGE = 30.0f;  // REGROUP_PING_PONG_TOGETHER
        // [b30_topped_up] TickStrandedReenter (user rule 2026-10-25: «если нет пути обратно, то можно перезайти в
        // данж. Но это будет считаться как смерть»): no walk back to the group for STRANDED_REENTER_MS within the still
        // range, the follow watchdog stuck at least STRANDED_REENTER_WATCHDOG_S meanwhile; at most
        // STRANDED_REENTER_MAX per member and run (each one is a death).
        constexpr uint32 STRANDED_REENTER_MS = 75 * IN_MILLISECONDS;
        constexpr uint32 STRANDED_REENTER_WATCHDOG_S = 20;
        constexpr uint32 STRANDED_REENTER_MAX = 3;
        // [b30_reenter] User rule 2026-10-25: «еще можно перезайти в данж, когда это реально ближе идти будет» - a
        // re-entry (WotLK's LFG teleport out / in) as a route choice, a death too: the walk to the goal here longer
        // than SHORTCUT_REENTER_RATIO x the walk from an entrance and by more than SHORTCUT_REENTER_MIN_GAIN.
        constexpr float SHORTCUT_REENTER_RATIO = 2.0f;
        using Leader::Travel::SHORTCUT_REENTER_MIN_GAIN;  // [lead107] RouteTargets.h
        constexpr uint32 SHORTCUT_REENTER_CHECK_MS = 20 * IN_MILLISECONDS;
        constexpr uint32 SHORTCUT_REENTER_COOLDOWN_MS = 3 * MINUTE * IN_MILLISECONDS;
        constexpr float SHORTCUT_REENTER_MEMBER_FAR = 100.0f;  // a member this far from the leader re-enters alone
        using Leader::Travel::ENDLESS_SUMMON_KILLS;  // [lead107] RouteTargets.h
        using Leader::Travel::DRIVE_AFTER_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::DRIVE_STEP;  // [lead107] RouteSteer.h
        using Leader::Travel::DRIVE_SAME_GOAL;  // [lead107] RouteEvents.h
        using Leader::Travel::EVENT_CHAIN_TRASH_RADIUS;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_CHAIN_NARROW;  // [lead107] RouteTargets.h
        using Leader::Travel::EVENT_CHAIN_JOIN_RANGE;  // [lead107] RouteTargets.h
        using Leader::Travel::FAR_TARGET_PATH;  // [lead107] RouteTargets.h
        using Leader::Travel::LEAD_SPOT_AHEAD_MARGIN;  // [lead107] RouteTargets.h
        using Leader::Travel::LEAD_SPOT_NEAR_RANGE;  // [lead107] RouteTargets.h
        using Leader::Travel::INTRO_WAIT_MAX_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::INTRO_WAIT_RANGE;  // [lead107] RouteSteer.h
        using Leader::Travel::introWaitSinceMs;  // [lead107] RouteTargets.h
        using Leader::Travel::ritualHoldUntilMs;  // [lead107] RouteTargets.h
        using Leader::Travel::guardRefusals;  // [lead107] RouteSteer.h
        using Leader::Travel::guardApproachLogged;  // [lead107] RouteSteer.h
        using Leader::Travel::guardApproachSinceMs;  // [lead107] RouteSteer.h
        using Leader::Travel::steerRestLogged;  // [lead107] RouteSteer.h
        using Leader::Travel::guardKeepLogged;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_TARGET_COMMIT;  // [lead107] RouteTargets.h
        using Leader::Travel::TARGET_COMMIT_MARGIN;  // [lead107] RouteTargets.h
        using Leader::Travel::TARGET_COMMIT_SHARE;  // [lead107] RouteTargets.h
        using Leader::Travel::TARGET_COMMIT_MAX_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::targetCommitLogged;  // [lead107] RouteTargets.h
        using Leader::Travel::doorSpinHolds;  // [lead107] RouteTargets.h
        using Leader::Travel::DOOR_SPIN_SKIPS;  // [lead107] RouteTargets.h
        using Leader::Travel::DOOR_SPIN_WINDOW_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::DOOR_SPIN_HOLD_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::DOOR_SPIN_LOG_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::DoorSpinCount;  // [lead107] RouteTargets.h
        using Leader::Travel::doorSpinCounts;  // [lead107] RouteTargets.h

        using Leader::Travel::CountDoorSpin;  // [lead107] RouteTargets.h

        // The door_spin_hold line: the door, the skips, and its opener (a key starter of the group, or none).
        JsonObject DoorSpinFields(Party const& party, Player* leader, GameObject const* door, uint32 skips,
                                  uint32 windowMs, char const* how, std::string& opener)
        {
            // [lead107] the leader's (RouteTargets.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::DoorSpinFields(group, leader, door, skips, windowMs, how, opener);
        }
        using Leader::Travel::gateKeyHeldLogged;  // [lead107] RouteTargets.h
        using Leader::Travel::phasedBossLogged;  // [lead107] RouteTargets.h
        using Leader::Travel::starterKeyLegLogged;  // [lead107] RouteTargets.h
        using Leader::Travel::pullAloneLogged;  // [lead107] RouteTargets.h
        using Leader::Travel::fledSurvivorLogged;  // [lead107] RouteTargets.h
        using Leader::Travel::lockoutClearLogged;  // [lead107] RouteTargets.h
        using Leader::Travel::PULL_ALONE_FIRST;  // [lead107] RouteTargets.h
        using Leader::Travel::PULL_ALONE_BONUS;  // [lead107] RouteTargets.h
        using Leader::Travel::FLED_SURVIVOR_BONUS;  // [lead107] RouteTargets.h
        using Leader::Travel::FLED_SURVIVOR_BEHIND;  // [lead107] RouteTargets.h
        using Leader::Travel::FLED_SURVIVOR_TRAIL;  // [lead107] RouteTargets.h
        using Leader::Travel::PULL_ALONE_ASSIST;  // [lead107] RouteTargets.h

        using Leader::Travel::ROUTE_REST_HOLD;  // [lead107] RouteTargets.h
        using Leader::Travel::REST_HOLD_SIT_GRACE_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::REST_HOLD_MAX_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::REST_HOLD_COOLDOWN_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::RestHold;  // [lead107] RouteTargets.h
        using Leader::Travel::restHolds;  // [lead107] RouteTargets.h
        using Leader::Travel::REST_HOLD_BELOW_LINE;  // [lead107] RouteTargets.h

        using Leader::Travel::HasRestItem;  // [lead107] RouteTargets.h

        // ROUTE_REST_HOLD: the members the group waits for ("name(drinking)", ...); empty: no hold. A fight ends
        // it (the bots fight, the harness has nothing to hold), and so does an escort (the group keeps up with it).
        std::string RestHoldMembers(Party const& party, Player* leader)
        {
            // [lead107] the leader's (RouteTargets.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::RestHoldMembers(group, leader);
        }
        using Leader::Travel::STARTER_WALKING_RETRY_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::FORGE_STATION_RADIUS;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_NOT_PULLED_SKIP_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::STARTER_ITEM_MIN_RANGE;  // [lead107] RouteEvents.h
        using Leader::Travel::STARTER_ITEM_STEP_OUT;  // [lead107] RouteEvents.h
        using Leader::Travel::STARTER_ITEM_MAX_FAILS;  // [lead107] RouteEvents.h
        using Leader::Travel::BOSS_ROOM_TRASH_RADIUS;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_ROOM_TRASH_DZ;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_ROOM_TRASH_PENALTY;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_WALK_RATIO;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_WALK_EXTRA;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_ROOM_CACHE_CELL;  // [lead107] RouteTargets.h
        using Leader::Travel::PATROL_EYE_HEIGHT;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_ROOM_CACHE_MAX;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_SKIP_DETOUR;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_DETOUR_FALLBACK;  // [lead107] RouteTargets.h
        using Leader::Travel::DETOUR_FALLBACK_MS;  // [lead107] RouteTargets.h
        using Leader::Fight::DETOUR_RATIO;  // [lead107] LeaderFightChecks.h (3.0)
        using Leader::Fight::DETOUR_EXTRA;  // (60 yd)
        using Leader::Travel::DETOUR_EXTRA_PATHS;  // [lead107] RouteTargets.h
        using Leader::Travel::DETOUR_BOSS_WAY_MARGIN;  // [lead107] RouteTargets.h
        using Leader::Travel::DETOUR_LOG_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::EVENT_GATHER_MEMBER_RANGE;  // [lead107] RouteEvents.h
        using Leader::Travel::GATHER_CALL_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::STARTER_ITEM_RETRY_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::EXPECT_DOOR_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::EXPECT_DOOR_MAX_RETRIES;  // [lead107] RouteEvents.h
        using Leader::Travel::EXPECT_DOOR_CAST_RANGE;  // [lead107] RouteEvents.h
        using Leader::Travel::EXPECT_DOOR_STEP;  // [lead107] RouteEvents.h
        using Leader::Travel::EXPECT_DOOR_WAIT_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::EXPECT_DOOR_LOG_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::KEY_PASS_LEADER_RANGE;  // [lead107] RouteEvents.h
        using Leader::Travel::KEY_PASS_GATHER_RANGE;  // [lead107] RouteEvents.h
        using Leader::Travel::KEY_PASS_SPREAD;  // [lead107] RouteEvents.h
        using Leader::Travel::KEY_PASS_RETRY_MS;  // [lead107] RouteEvents.h
        constexpr uint32 KEY_PASS_MAX_MS = 15 * IN_MILLISECONDS;  // cast 5 s + open 3 s + margin
        using Leader::Travel::KEY_PASS_LOG_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::KEY_PASS_RUNNER_REACH;  // [lead107] RouteEvents.h
        using Leader::Travel::KEY_PASS_CAST_CENTRE;  // [lead107] RouteEvents.h
        using Leader::Travel::KEY_PASS_GATHER_MAX_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::STARTER_GATHER_RETRY_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::STARTER_GATHER_MAX_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::STARTER_GATHER_LOG_MS;  // [lead107] RouteEvents.h
        // Bots walk by themselves (HARNESS_MOVES_BOTS off): the same bot_cannot_move (bot, capability, reason) is
        // logged at most this often; a walked wing change is over once every member stands this close (2D) to the
        // next entrance (SelfTravel's goal range), and fails after WING_WALK_MAX_MS.
        constexpr uint32 CANNOT_MOVE_LOG_MS = 30 * IN_MILLISECONDS;
        using Leader::Travel::WING_ARRIVE_RANGE;  // [lead107] RouteMeet.h
        constexpr uint32 WING_WALK_MAX_MS = 15 * MINUTE * IN_MILLISECONDS;
        // A member inside, near one spot this long during a wing walk: no walk out, left out of the walk.
        constexpr uint32 WING_MEMBER_STILL_MS = 2 * MINUTE * IN_MILLISECONDS;
        constexpr float WING_MEMBER_STILL_RANGE = 25.0f;

        using Leader::Travel::PathResult;  // [lead107] RouteLegs.h

        using Leader::Travel::ROUTE_PROGRESS;  // [lead107] RouteLegs.h
        using Leader::Travel::LONG_ROUTE_NODES;  // [lead99] RouteSearch.h
        // Both directions out of nodes: once more with Detour's largest pool, one shared query bound anew per call
        // (dtNavMeshQuery::init keeps a pool that is big enough). Old Hillsbrad 2026-10-25: every Durnholde barrel from
        // outside is 1300-2100 yd over the wall top and its SW stairs; 8192 nodes ended both ways on the wall 13-50 yd
        // off and 11 yd above the barrel (0 plants in 25 minutes), 65535 complete (461-552 polygons).
        using Leader::Travel::LONG_ROUTE_BIG_NODES;  // [lead99] RouteSearch.h
        using Leader::Travel::ROUTE_LONG_BIG_POOL;  // [lead99] RouteSearch.h
        using Leader::Travel::LONG_ROUTE_MAX_POLYS;  // [lead99] RouteSearch.h
        using Leader::Travel::LONG_ROUTE_MAX_CORNERS;  // [lead99] RouteSearch.h

        using Leader::Travel::POINT_PATH_FAILED;  // [lead107] RouteLegs.h

        using Leader::Travel::ROUTE_CORNER_PATH;  // [lead107] RouteLegs.h
        using Leader::Travel::ROUTE_LONG_SEARCH;  // [lead107] RouteLegs.h
        // Route backbone pass (2026-10-24, *.bak_routebackbone), false restores the behaviour before it of that piece:
        // - LongRoute searches back from the goal when its forward search ran out of nodes;
        // - an incomplete route leg (it ends ROUTE_BACKBONE_END_LEFT or more from its goal) is no backbone leg: no
        //   trash "blocks route" from a partial path's dead end (route backbone incomplete);
        // - a stop whose blocker the tank does not see walks on along the route to a point that sees it (approach
        //   to sight), or skips it at once - not after PULL_WAIT_MS (RFC 18:22-18:24: Earthborers round a corner
        //   on a rising slope, 45 s stood each);
        // - a mob the tank rejects (TankRejects) is logged when it would have held the approach.
        using Leader::Travel::ROUTE_LONG_REVERSE;  // [lead99] RouteSearch.h
        using Leader::Travel::ROUTE_BACKBONE_COMPLETE_ONLY;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_APPROACH_TO_SIGHT;  // [lead107] RouteSteer.h
        using Leader::Travel::ROUTE_LOG_TANK_REJECTS;  // [lead107] RouteSteer.h
        using Leader::Travel::ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED;  // [lead107] RouteSteer.h
        using Leader::Travel::ROUTE_BOSS_PULL_IMMEDIATE;  // [lead107] RouteSteer.h
        using Leader::Travel::ROUTE_HARNESS_WALK_GUARD;  // [lead107] RouteSteer.h
        using Leader::Travel::HARNESS_HOLD_LOG_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::ROUTE_BOSS_ROOM_FIRST;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_SWIM_UP;  // [lead107] RouteLegs.h
        using Leader::Travel::HOR_FALRIC_ENTRY;  // [lead107] RouteSteer.h
        using Leader::Travel::HOR_MARWYN_ENTRY;  // [lead107] RouteSteer.h
        using Leader::Travel::HOR_MARWYN_SLOT;  // [lead107] RouteSteer.h
        using Leader::Travel::HOR_CENTRE_X;  // [lead107] RouteSteer.h
        using Leader::Travel::HOR_CENTRE_Y;  // [lead107] RouteSteer.h
        using Leader::Travel::HOR_ENTRANCE_X;  // [lead107] RouteSteer.h
        using Leader::Travel::HOR_ENTRANCE_Y;  // [lead107] RouteSteer.h
        using Leader::Travel::HOR_ENTRANCE_Z;  // [lead107] RouteSteer.h
        using Leader::Travel::HOR_HOLD_FROM_CENTRE;  // [lead107] RouteSteer.h
        using Leader::Travel::HOR_HOLD_LOG_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::MECHANAR_MAP_ID;  // [lead107] RouteTargets.h
        using Leader::Travel::HOR_LICH_KING_ENTRY;  // [lead107] RouteSteer.h
        using Leader::Travel::HOR_LICH_KING_ESCAPE_ENTRY;  // [lead107] RouteSteer.h
        using Leader::Travel::AN_HADRONOX_ENTRY;  // [lead107] RouteSteer.h
        using Leader::Travel::AK_TWILIGHT_INITIATE_ENTRY;  // [lead107] RouteSteer.h
        using Leader::Travel::AK_JEDOGA_ENTRY;  // [lead107] RouteSteer.h
        using Leader::Travel::ROUTE_SWIM_BACK;  // [lead107] RouteSteer.h
        using Leader::Travel::SWIM_TRAIL_STEP;  // [lead107] RouteSteer.h
        using Leader::Travel::SWIM_TRAIL_MAX;  // [lead107] RouteSteer.h
        using Leader::Travel::SWIM_CLIMB_DZ;  // [lead107] RouteSteer.h
        using Leader::Travel::WAYPOINT_REACHED_SWIM;  // [lead107] RouteSteer.h
        using Leader::Travel::SWIM_BACK_MAX_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::ROUTE_STARTER_REARM;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_GATHER_MEMBERS;  // [lead107] RouteEvents.h
        // - FlyToPortal descends beside the portal (FLY_SIDE_DISTANCE out on its axis), not onto it.
        constexpr bool ROUTE_FLY_SIDE_APPROACH = true;
        using Leader::Travel::ROUTE_CHAIN_GUARD;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_TWIN_SKIP;  // [lead107] RouteSteer.h
        using Leader::Travel::ROUTE_OCULUS_DRAKE_ONLY;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_HOLD_SPOT_FIGHT;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_STARTER_ITEM_CHECK;  // [lead107] RouteEvents.h
        using Leader::Travel::ROUTE_EVENT_SPOT_FIRST;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_INTRO_WAIT;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_STARTER_NOT_WALKING;  // [lead107] RouteEvents.h
        using Leader::Travel::ROUTE_FORGE_ORDER;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_DOOR_STARTER_FIRST;  // [lead107] RouteTargets.h
        using Leader::Travel::DOOR_STARTER_LOG_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::doorStarterLogMs;  // [lead107] RouteTargets.h
        using Leader::Travel::leadSpotFirstLogMs;  // [lead107] RouteTargets.h
        using Leader::Travel::eventBossWaitLogMs;  // [lead107] RouteTargets.h
        using Leader::Travel::eventStartHoldLogMs;  // [lead107] RouteTargets.h
        using Leader::Travel::KEY_DOOR_CHAIN_FRESH_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::keyDoorChain;  // [lead107] RouteTargets.h
        using Leader::Travel::fallbackFloorLogMs;  // [lead107] RouteTargets.h
        using Leader::Travel::eventWaveFlipMs;  // [lead107] RouteTargets.h
        using Leader::Travel::EventWaveFresh;  // [lead107] RouteTargets.h
        using Leader::Travel::starterNearestPath;  // [lead107] RouteSteer.h
        using Leader::Travel::starterGatherMs;  // [lead107] RouteEvents.h
        using Leader::Travel::passageDetourLogMs;  // [lead107] RouteTargets.h
        using Leader::Travel::PASSAGE_DETOUR_LOG_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_ONLY_TRASH;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_ONLY_LOS_TRIES;  // [lead107] RouteTargets.h

        using Leader::Travel::BuildFilteredLeg;  // [lead107] RouteLegs.h

        using Leader::Travel::BuildLeg;  // [lead107] RouteLegs.h

        // [lead99] the route search lives in the leader's Travel area (Ai/Leader/Travel/RouteSearch): the harness
        // calls it
        using Leader::Travel::LongRoute;
        using Leader::Travel::NavMeshQueryFree;

        using Leader::Travel::WaterSurfaceAt;  // [lead107] RouteLegs.h

        using Leader::Travel::PolylineLength;  // [lead107] RouteLegs.h

        using Leader::Travel::ROUTE_SURFACE_START;  // [lead107] RouteLegs.h
        using Leader::Travel::SURFACE_START_MIN_DEPTH;  // [lead107] RouteLegs.h
        using Leader::Travel::ROUTE_REFUSE_DEAD_END;  // [lead107] RouteSteer.h
        using Leader::Travel::DEAD_END_CLIFF_MIN_RISE;  // [lead107] RouteSteer.h
        using Leader::Travel::DEAD_END_CLIFF_RATIO;  // [lead107] RouteSteer.h
        using Leader::Travel::DEAD_END_MIN_GAIN;  // [lead107] RouteSteer.h
        using Leader::Travel::DEAD_END_MIN_GAIN_SHARE;  // [lead107] RouteSteer.h
        using Leader::Travel::DEAD_END_WALK_MIN;  // [lead107] RouteSteer.h
        using Leader::Travel::DEAD_END_SAME_END;  // [lead107] RouteSteer.h
        using Leader::Travel::DEAD_END_SAME_GOAL;  // [lead107] RouteSteer.h
        using Leader::Travel::DEAD_END_PROGRESS;  // [lead107] RouteSteer.h
        using Leader::Travel::DEAD_END_MEMORY_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::DEAD_END_LINK_RANGE;  // [lead107] RouteSteer.h
        using Leader::Travel::DEAD_END_FALLBACK_REFUSALS;  // [lead107] RouteSteer.h
        using Leader::Travel::DEAD_END_FALLBACK_WINDOW_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::DEAD_END_FALLBACK_MS;  // [lead107] RouteSteer.h

        using Leader::Travel::DeadEndVisit;  // [lead107] RouteSteer.h
        using Leader::Travel::deadEndVisits;  // [lead107] RouteSteer.h

        using Leader::Travel::DeadEndRefusals;  // [lead107] RouteSteer.h
        using Leader::Travel::deadEndRefusals;  // [lead107] RouteSteer.h

        using Leader::Travel::DeadEndVerdict;  // [lead107] RouteSteer.h

        using Leader::Travel::JudgeIncompleteEnd;  // [lead107] RouteSteer.h

        using Leader::Travel::RoutePath;  // [lead107] RouteLegs.h

        using Leader::Travel::EvaluatePath;  // [lead107] RouteLegs.h

        using Leader::Travel::PASSAGE_DOOR_MARGIN;  // [lead107] RouteLegs.h

        using Leader::Travel::DoorFreeRoute;  // [lead107] RouteLegs.h

        using Leader::Travel::CachedLongRoute;  // [lead107] RouteLegs.h
        using Leader::Travel::LONG_ROUTE_REUSE_MS;  // [lead107] RouteLegs.h
        using Leader::Travel::ROUTE_PASSAGE_DOOR_DETOUR_LEG;  // [lead107] RouteLegs.h
        using Leader::Travel::LONG_ROUTE_SAME_GOAL;  // [lead107] RouteLegs.h
        using Leader::Travel::longRouteCache;  // [lead107] RouteLegs.h

        using Leader::Travel::TrimRoute;  // [lead107] RouteLegs.h

        using Leader::Travel::ClosedDoors;  // [lead107] RouteLegs.h
        using Leader::Travel::RouteThroughClosedDoor;  // [lead107] RouteLegs.h
        using Leader::Travel::PassageDoorBoss;  // [lead107] RouteLegs.h

        using Leader::Travel::PASSAGE_LEG_REUSE_MS;  // [lead107] RouteLegs.h
        using Leader::Travel::PassageLegMemo;  // [lead107] RouteLegs.h
        using Leader::Travel::passageLegMemos;  // [lead107] RouteLegs.h

        using Leader::Travel::PassageDetourLeg;  // [lead107] RouteLegs.h

        using Leader::Travel::HopelessDetourLeg;  // [lead107] RouteLegs.h

        using Leader::Travel::SteerLeg;  // [lead107] RouteLegs.h

        using Leader::Travel::ForgetPassedDeadEnds;  // [lead107] RouteSteer.h


        // [incomplete_route_deadend] SkipTarget's "route end" (steer_stuck: the leader stood at the end, no way on):
        // that end is a dead end for this target.
        void RecordRouteEndDeadEnd(Player* walker, ObjectGuid target, G3D::Vector3 const& goal)
        {
            G3D::Vector3 const here(walker->GetPositionX(), walker->GetPositionY(), walker->GetPositionZ());
            uint32 const now = getMSTime();
            std::vector<DeadEndVisit>& visits = deadEndVisits[walker->GetGUID()];
            if (visits.size() >= 32)
                visits.erase(visits.begin());
            visits.push_back(DeadEndVisit{here, goal, (goal - here).length(), now ? now : 1, target});
        }

        using Leader::Travel::DeadEndFallback;  // [lead107] RouteSteer.h

        using Leader::Travel::RouteWaypoint;  // [lead107] RouteLegs.h

        using Leader::Travel::FLOOR_WAYPOINT_MAX_DZ;  // [lead107] RouteLegs.h
        using Leader::Travel::FLOOR_SNAP_HALF_HEIGHT;  // [lead107] RouteLegs.h
        using Leader::Travel::FLOOR_SNAP_HALF_WIDTH;  // [lead107] RouteLegs.h

        using Leader::Travel::NavmeshFloor;  // [lead107] RouteLegs.h

        using Leader::Travel::FloorWaypoint;  // [lead107] RouteLegs.h

        std::set<uint32> const& FlaskSpells()
        {
            static std::set<uint32> const spells = []
            {
                std::set<uint32> result;
                for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
                    if (proto.Class == ITEM_CLASS_CONSUMABLE && proto.SubClass == ITEM_SUBCLASS_FLASK &&
                        proto.Spells[0].SpellId > 0)
                        result.insert(uint32(proto.Spells[0].SpellId));
                return result;
            }();
            return spells;
        }

        bool HasAuraNamed(Player* bot, char const* text)
        {
            for (auto const& [spellId, aurApp] : bot->GetAppliedAuras())
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                if (info && info->SpellName[LOCALE_enUS] && strstr(info->SpellName[LOCALE_enUS], text))
                    return true;
            }
            return false;
        }

        using Leader::Travel::STARTER_PATH_CANDIDATES;  // [lead107] RouteTargets.h
        using Leader::Travel::STARTER_PATH_SORT_RANGE;  // [lead107] RouteTargets.h
        using Leader::Travel::STARTER_PATH_ARRIVE;  // [lead107] RouteTargets.h

        using Leader::Travel::StarterPathArrives;  // [lead107] RouteTargets.h

        using Leader::Travel::StarterObjectStuck;  // [lead107] RouteTargets.h

        using Leader::Travel::KeyGateBarsOffset;  // [lead107] RouteEvents.h

        // [b32_strat_gate] An auto-closing key gate (KeyDoorPassDef) the whole living group is through: every member on
        // the far side of the gate plane (away from its use point). A door_hold there is a walk BACK to what stands
        // behind it - no reason to key it again (strat-h 221216-5C-1: 175352 re-armed at 1040 s for trash behind it).
        bool GroupThroughKeyGate(Party const& party, GameObject const* door)
        {
            // [lead107] the leader's (RouteTargets.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::GroupThroughKeyGate(group, door);
        }

        using Leader::Travel::NearestDist;  // [lead107] RouteTargets.h

        // Wing runs (Scarlet Monastery, Dire Maul, Blackrock Spire): a hostile closer to a boss of another wing than
        // to every boss and entrance of this one belongs to the other wing.
        bool InThisWing(Party const& party, WorldObject const* object)
        {
            // [lead107] the leader's (RouteTargets.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::InThisWing(group, object);
        }

        using Leader::Travel::EventGatherSpot;  // [lead107] RouteEvents.h

        using Leader::Travel::EventGatherReady;  // [lead107] RouteEvents.h

        using Leader::Travel::DIRECT_STEP;  // [lead107] RouteEvents.h
        using Leader::Travel::DIRECT_MAX_DROP;  // [lead107] RouteEvents.h

        using Leader::Travel::LineOverGround;  // [lead107] RouteEvents.h

        using Leader::Travel::DirectLineOverGround;  // [lead107] RouteEvents.h

        using Leader::Travel::LeaderDoorStop;  // [lead107] RouteLegs.h

        bool LeaderOwnWalkStalled(Party& party, Player* leader, G3D::Vector3 const& goal)
        {
            // [lead107] the leader's (RouteEvents.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::LeaderOwnWalkStalled(group, leader, goal);
        }

        using Leader::Travel::SUMMON_SCAN_RANGE;  // [lead107] RouteTargets.h
        using Leader::Travel::CHAIN_SUMMON_SCAN_RANGE;  // [lead107] RouteTargets.h

        using Leader::Travel::SummonedCreatureCheck;  // [lead107] RouteTargets.h

        using Leader::Travel::STARTER_SUMMON_SCAN_RANGE;  // [lead107] RouteTargets.h

        using Leader::Travel::SummonedEntryCheck;  // [lead107] RouteTargets.h

        using Leader::Travel::RouteCreatures;  // [lead107] RouteTargets.h

        using Leader::Travel::PACK_RADIUS;  // [lead107] RouteLegs.h
        using Leader::Travel::AGGRO_EDGE_MARGIN;  // [lead107] RouteSteer.h
        using Leader::Travel::SAME_FLOOR_DZ;  // [lead107] RouteSteer.h

        using Leader::Travel::ShadowMobCheck;  // [lead107] RoutePatrol.h

        using Leader::Travel::PackMobCheck;  // [lead107] RouteLegs.h

        using Leader::Travel::HOPELESS_AVOID_MS;  // [lead107] RouteLegs.h
        using Leader::Travel::HOPELESS_AVOID_MARGIN;  // [lead107] RouteLegs.h
        using Leader::Travel::HOPELESS_AVOID_DZ;  // [lead107] RouteLegs.h
        using Leader::Travel::HOPELESS_DETOUR_MAX_RATIO;  // [lead107] RouteLegs.h
        using Leader::Travel::HopelessMob;  // [lead107] RouteLegs.h
        using Leader::Travel::hopelessPacks;  // [lead107] RouteLegs.h
        using Leader::Travel::HopelessLegMemo;  // [lead107] RouteLegs.h
        using Leader::Travel::hopelessLegMemos;  // [lead107] RouteLegs.h

        using Leader::Travel::NoteHopelessPack;  // [lead107] RouteLegs.h

        using Leader::Travel::InDisc;  // [lead107] RouteLegs.h

        using Leader::Travel::LinkedChapelTrash;  // [lead107] RouteTargets.h


        using Leader::Travel::HOPELESS_WING_HOLD_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::HOPELESS_WING_CHECK_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::HOPELESS_WING_DEFER_MAX_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::HopelessWingMap;  // [lead107] RouteTargets.h
        using Leader::Travel::HopelessWingState;  // [lead107] RouteTargets.h
        using Leader::Travel::hopelessWings;  // [lead107] RouteTargets.h

        using Leader::Travel::HopelessWingDeferred;  // [lead107] RouteTargets.h

        using Leader::Travel::RouteHeldByHopelessPack;  // [lead107] RouteTargets.h

        using Leader::Travel::AtPackAggroEdge;  // [lead107] RouteSteer.h

        using Leader::Travel::APPROACH_SCAN_RADIUS;  // [lead107] RouteSteer.h
        using Leader::Travel::APPROACH_AGGRO_MARGIN;  // [lead107] RoutePatrol.h
        using Leader::Travel::APPROACH_SAMPLE_STEP;  // [lead107] RouteSteer.h
        using Leader::Travel::APPROACH_ARRIVED;  // [lead107] RouteSteer.h
        using Leader::Travel::TANK_PULL_HEIGHT;  // [lead107] RouteSteer.h
        using Leader::Travel::TANK_REJECT_HEIGHT;  // [lead107] RouteSteer.h
        using Leader::Travel::TANK_REJECT_LOG_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::tankRejectLogMs;  // [lead107] RouteSteer.h
        using Leader::Travel::APPROACH_SIGHT_PATH;  // [lead107] RouteSteer.h
        using Leader::Travel::APPROACH_SIGHT_TRIES;  // [lead107] RouteSteer.h
        using Leader::Travel::APPROACH_SIGHT_KEEP_OUT_TRIES;  // [lead107] RouteSteer.h

        using Leader::Fight::IdleAggroCheck;  // [lead107] the leader's idle-mob test

        using Leader::Travel::APPROACH_ROUTE_SCAN_MARGIN;  // [lead107] RouteSteer.h
        using Leader::Travel::APPROACH_WANDER_MAX;  // [lead107] RoutePatrol.h
        using Leader::Travel::APPROACH_OTHER_FLOOR_DZ;  // [lead107] RouteSteer.h
        using Leader::Travel::APPROACH_FOLLOW_SPREAD;  // [lead107] RoutePatrol.h

        using Leader::Travel::RealAggroRange;  // [lead107] RouteTargets.h

        using Leader::Travel::AggroReach;  // [lead107] RoutePatrol.h

        using Leader::Travel::PATROL_WAIT_SCAN;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_WAIT_ASSIST;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_WAIT_STEP;  // [lead107] RouteSteer.h
        using Leader::Travel::PATROL_WAIT_STEP_MAX;  // [lead107] RouteSteer.h
        using Leader::Travel::PATROL_WAIT_TRAIL_MAX;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_WAIT_TRAIL_FLOOR;  // [lead107] RoutePatrol.h
        using Leader::Travel::PatrolWaitSpot;  // [lead107] RouteSteer.h

        using Leader::Travel::SeesFromPoint;  // [lead107] RoutePatrol.h

        using Leader::Travel::AggroesAtPoint;  // [lead107] RoutePatrol.h

        using Leader::Travel::ROUTE_FAST_RESPAWN_PASS;  // [lead107] RouteTargets.h
        using Leader::Travel::FAST_RESPAWN_MIN_S;  // [lead107] RouteTargets.h
        using Leader::Travel::FAST_RESPAWN_MAX_S;  // [lead107] RouteTargets.h

        using Leader::Travel::ROUTE_RESPAWN_IGNORE;  // [lead107] RouteTargets.h
        constexpr float RESPAWN_ZONE_RADIUS = 30.0f;     // a walk this close to a zone point crosses the zone
        using Leader::Travel::RESPAWN_ZONE_MERGE;  // [lead107] RouteTargets.h
        using Leader::Travel::RESPAWN_ZONE_MAX_POINTS;  // [lead107] RouteTargets.h
        constexpr float RESPAWN_ZONE_WALK_COST = 150.0f;  // yd added to an entrance walk through a zone
        using Leader::Travel::respawnZones;  // [lead107] RouteTargets.h

        using Leader::Travel::FastRespawner;  // [lead107] RouteTargets.h

        using Leader::Travel::FastSpawn;  // [lead107] RouteTargets.h

        using Leader::Travel::FastSpawns;  // [lead107] RouteTargets.h

        using Leader::Travel::AddRespawnZonePoint;  // [lead107] RouteTargets.h

        // The walk `points` passes within RESPAWN_ZONE_RADIUS of a respawn zone point of the instance.
        bool RespawnZoneOnWalk(uint32 instanceId, Movement::PointsArray const& points)
        {
            auto const found = respawnZones.find(instanceId);
            if (!ROUTE_RESPAWN_IGNORE || found == respawnZones.end() || points.empty())
                return false;
            for (G3D::Vector3 const& point : found->second)
                for (size_t index = 0; index < points.size(); ++index)
                {
                    G3D::Vector3 closest = points[index];
                    if (index + 1 < points.size())
                    {
                        G3D::Vector3 const segment = points[index + 1] - points[index];
                        float const lengthSq = segment.squaredLength();
                        float const t = lengthSq > 0.0f
                                            ? std::clamp((point - points[index]).dot(segment) / lengthSq, 0.0f, 1.0f)
                                            : 0.0f;
                        closest = points[index] + segment * t;
                    }
                    if ((closest - point).length() < RESPAWN_ZONE_RADIUS)
                        return true;
                }
            return false;
        }

        using Leader::Travel::ROUTE_UNSEEN_ROUTE_END_PASS;  // [lead107] RouteSteer.h
        using Leader::Travel::unseenRouteEndUntilMs;  // [lead107] RouteSteer.h
        using Leader::Travel::unseenRouteEndLogMs;  // [lead107] RouteSteer.h

        using Leader::Travel::MarkUnseenRouteEnd;  // [lead107] RouteSteer.h

        using Leader::Travel::UnseenRouteEnd;  // [lead107] RouteSteer.h

        using Leader::Travel::JoinsBossFight;  // [lead107] RouteTargets.h

        using Leader::Travel::ROUTE_BOSS_OWN_ENCOUNTER_PACK;  // [lead107] RouteSteer.h

        using Leader::Travel::BossEncounterMob;  // [lead107] RouteSteer.h

        using Leader::Travel::ROUTE_BOSS_BROUGHT_BY_PACK;  // [lead107] RouteSteer.h

        using Leader::Travel::BossBroughtBy;  // [lead107] RouteSteer.h

        using Leader::Travel::ROUTE_BOSS_ROOM_PACK;  // [lead107] RouteSteer.h

        using Leader::Travel::BossRoomPack;  // [lead107] RouteSteer.h

        using Leader::Travel::ROUTE_STOP_ONLY_REAL_AGGRO;  // [lead107] RouteSteer.h
        using Leader::Travel::STOP_REAL_SLACK;  // [lead107] RouteSteer.h

        using Leader::Travel::ROUTE_STOP_IGNORES_Z_GATE;  // [lead107] RouteSteer.h
        using Leader::Travel::STOP_TRAIL_BACK_MAX;  // [lead107] RouteSteer.h
        using Leader::Travel::STOP_TRAIL_FLOOR;  // [lead107] RouteSteer.h

        // [lead3] C:/WoW/LEADER_REFACTOR_PLAN.md section 9: the leader decides this party's pulls (its planner runs
        // on the world thread, LEADER_PLANNER_IN_LEADER): a tracked 5-man tank. Raids keep the old harness pulls.
        bool LeaderDecidesFor(Party const& party)
        {
            return Leader::LEADER_PLANNER_IN_LEADER && party.content && !party.content->raid &&
                   party.content->size <= 5 && !party.leader.IsEmpty() && Leader::Orders::IsTracked(party.leader);
        }

        using Leader::Travel::LEADER_VERDICT_TTL_MS;  // [lead107] RoutePatrol.h
        bool LeaderPicksFor(Party const& party)
        {
            // [lead107] the leader's (Ai/Leader/Travel/RoutePatrol.h), read through its group view
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::LeaderPicksFor(group);
        }

        bool LeaderVerdictCovers(Party const& party, ObjectGuid mob)
        {
            // [lead107] the leader's (Ai/Leader/Travel/RoutePatrol.h), read through its group view
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::LeaderVerdictCovers(group, mob);
        }

        bool PartyVerdict(Party const& party, ObjectGuid mob, bool& accepted, uint32& mobs)
        {
            // [lead107] the leader's (Ai/Leader/Travel/RoutePatrol.h), read through its group view
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::PartyVerdict(group, mob, accepted, mobs);
        }

        bool PartyRefusal(Party const& party, ObjectGuid mob, bool& hopeless, uint32& waitMs, std::string& why)
        {
            // [lead107] the leader's (RouteEvents.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::PartyRefusal(group, mob, hopeless, waitMs, why);
        }

        // [lead5] the leader planner's aoe_pull_plan (its own memory, Decision::aoePlan) and the leader's required
        // marks it hands the planner (LeaderInputs::required), else the AoePull registry
        bool PartyAoePlanned(Party const& party, ObjectGuid mob, uint32* sinceMs = nullptr)
        {
            // [lead107] the leader's (RouteSteer.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::PartyAoePlanned(group, mob, sinceMs);
        }

        void PartyMarkRequired(Party& party, ObjectGuid mob)
        {
            if (!LeaderPicksFor(party))
            {
                AoePull::MarkRequired(party.leader, mob);
                return;
            }
            uint32 const now = getMSTime();
            party.lead.requiredMarks[mob] = now ? now : 1;
        }

        // [lead5] the tank's view of `unit` (AggressiveTargetValue::Explain) with the leader's own route target
        std::string PartyTankView(Party const& party, Player* leader, Unit* unit)
        {
            // [lead107] the leader's (RouteSteer.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::PartyTankView(group, leader, unit);
        }

        // [lead5] LEADER_ONE_PICKER: EncounterOrder::IsLocked is the one encounter-order rule (the planner's and the
        // pick's: the Utgarde Keep forge order with its formations, Blackheart's room), not the harness's own copy of
        // the forge rule (UKIds::IsForgeMasterLocked) for the route's target choice.
        bool EncounterLockedFor(Party const& party, Creature* creature, InstanceScript const* script)
        {
            // [lead107] the leader's (RouteTargets.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::EncounterLockedFor(group, creature, script);
        }

        // [lead4] LEADER_ONE_PATROL_OWNER: the leader owns the patrol decision (LeaderPatrolTick); the harness's
        // patrol hold is its forecast only (no wait budget, no handover, no hold clock or cooldown).
        bool LeaderOwnsPatrols(Party const& party)
        {
            // [lead107] the leader's (Ai/Leader/Travel/RoutePatrol.h), read through its group view
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::LeaderOwnsPatrols(group);
        }

        using Leader::Travel::ApproachStop;  // [lead107] RouteSteer.h

        // [lead107] the harness's Party form of the leader's ApproachStop (through the group view)
        bool ApproachStop(Player* leader, WorldObject* target, Movement::PointsArray const& points, float length,
                          G3D::Vector3& stop, Creature*& blocker, bool& unseenStop, Creature* exemptPack,
                          Party const* party)
        {
            if (!party)
                return Leader::Travel::ApproachStop(leader, target, points, length, stop, blocker, unseenStop,
                                                    exemptPack);
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(*party));
            return Leader::Travel::ApproachStop(leader, target, points, length, stop, blocker, unseenStop, exemptPack,
                                                &group);
        }

        using Leader::Travel::HarnessWalkStop;  // [lead107] RouteSteer.h

        // [lead107] the harness's Party form of the leader's HarnessWalkStop (through the group view)
        bool HarnessWalkStop(Player* leader, Movement::PointsArray const& points, float length, Creature* exemptPack,
                             G3D::Vector3& point, Creature*& blocker, bool& held, Party const* party)
        {
            if (!party)
                return Leader::Travel::HarnessWalkStop(leader, points, length, exemptPack, point, blocker, held);
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(*party));
            return Leader::Travel::HarnessWalkStop(leader, points, length, exemptPack, point, blocker, held, &group);
        }

        using Leader::Travel::harnessHoldLogMs;  // [lead107] RouteSteer.h

        using Leader::Travel::MechBridgeLifts;  // [lead107] RouteTargets.h
        using Leader::Travel::mechBridgeLifts;  // [lead107] RouteTargets.h

        using Leader::Travel::SightStop;  // [lead107] RouteTargets.h
        using Leader::Travel::sightStops;  // [lead107] RouteTargets.h

        using Leader::Travel::PULL_AFTER_HOLD;  // [lead107] RouteSteer.h
        using Leader::Travel::PULL_AFTER_HOLD_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::PULL_AFTER_HOLD_GATE_MS;  // [lead107] RouteSteer.h
        using Leader::Dungeon::DOOR_WALK_MS;  // [lead107] LeaderPlan.h (15 s)

        using Leader::Travel::HoldReady;  // [lead107] RouteSteer.h
        using Leader::Travel::holdReady;  // [lead107] RouteSteer.h

        using Leader::Travel::PullAfterHoldGate;  // [lead107] RouteSteer.h
        using Leader::Travel::pullAfterHoldGates;  // [lead107] RouteSteer.h

        using Leader::Travel::PullAfterHoldPasses;  // [lead107] RouteSteer.h

        using Leader::Travel::RefusalIsReadiness;  // [lead107] RouteSteer.h

        using Leader::Travel::RefusalIsOverCap;  // [lead107] RouteEvents.h

        using Leader::Travel::DoorWalk;  // [lead107] RouteSteer.h
        using Leader::Travel::doorWalks;  // [lead107] RouteSteer.h

        using Leader::Travel::BODY_PULL_VIA_PLANNER;  // [lead107] RouteSteer.h
        using Leader::Travel::BODY_PULL_PLANNER_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::PlannerAsk;  // [lead107] RouteSteer.h
        using Leader::Travel::plannerAsks;  // [lead107] RouteSteer.h

        using Leader::Travel::AOE_PULL_ASK_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::AOE_PULL_ASK_RESET_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::AOE_PULL_WALK_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::aoePullAsks;  // [lead107] RouteSteer.h

        using Leader::Travel::AoePullAskRuns;  // [lead107] RouteSteer.h

        using Leader::Travel::AskPlannerFirst;  // [lead107] RouteSteer.h

        using Leader::Travel::BEHIND_ROOT_PULL;  // [lead107] RouteSteer.h
        using Leader::Travel::BEHIND_ROOT_TRIES;  // [lead107] RouteSteer.h
        using Leader::Travel::BEHIND_ROOT_WINDOW_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::BehindRootTry;  // [lead107] RouteSteer.h
        using Leader::Travel::behindRootTries;  // [lead107] RouteSteer.h

        using Leader::Travel::pullEstimateSeen;  // [lead107] RouteSteer.h

        using Leader::Travel::FindSightPoint;  // [lead107] RouteSteer.h

        using Leader::Travel::PlanVerdict;  // [lead107] RouteSteer.h

        PlanVerdict TankPlanVerdict(Party const& party, Player* leader, std::vector<Creature*> const& pack)
        {
            // [lead107] the leader's (RouteSteer.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::TankPlanVerdict(group, leader, pack);
        }

        // The idle mobs of the target's pack and of the blocker's (PackMobCheck, PACK_RADIUS around each), bosses
        // left out (their body pull is the boss pull): what approach to sight keeps out of while no plan is accepted.
        std::vector<Creature*> SightKeepOutPack(Party const& party, Player* leader, Creature* target, Creature* blocker)
        {
            // [lead107] the leader's (RouteSteer.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::SightKeepOutPack(group, leader, target, blocker);
        }

        using Leader::Travel::SIGHT_GIVE_WAY_SCAN;  // [lead107] RouteSteer.h

        using Leader::Travel::OtherPackMobCheck;  // [lead107] RouteSteer.h

        using Leader::Travel::SightKeepOutOthers;  // [lead107] RouteSteer.h

        using Leader::Travel::SIGHT_HOLD_RECHECK_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::SIGHT_HOLD_LOG_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::SightHold;  // [lead107] RouteTargets.h
        using Leader::Travel::sightHolds;  // [lead107] RouteTargets.h

        using Leader::Travel::BOSS_PULL_READY_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::BossReady;  // [lead107] RouteTargets.h
        using Leader::Travel::bossReady;  // [lead107] RouteTargets.h

        using Leader::Travel::ASSIST_LINK_MARGIN;  // [lead107] RouteTargets.h

        using Leader::Travel::CanAnswerAssist;  // [lead107] RouteTargets.h

        using Leader::Travel::CallsBossForHelp;  // [lead107] RouteTargets.h

        using Leader::Travel::RouteBackbone;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_BACKBONE_END_LEFT;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_BACKBONE_LOG_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::backboneIncompleteLogMs;  // [lead107] RouteTargets.h

        using Leader::Travel::RouteSnapshot;  // [lead107] RouteTargets.h
        using Leader::Travel::routeSnapshots;  // [lead107] RouteTargets.h

        using Leader::Travel::ClosestOnSegment;  // [lead107] RouteTargets.h

        using Leader::Travel::ROUTE_REACH_MARGIN;  // [lead107] RouteTargets.h

        using Leader::Travel::RouteReach;  // [lead107] RouteTargets.h

        using Leader::Travel::RouteCheck;  // [lead107] RouteTargets.h

        using Leader::Travel::BlocksRoute;  // [lead107] RouteTargets.h

        using Leader::Travel::PatrolWalkPath;  // [lead107] RoutePatrol.h

        using Leader::Travel::PATROL_WAIT_BUDGET_MS;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_WAIT_BUDGET_STARTS;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_WAIT_TICK_CAP_MS;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_HANDOVER_MS;  // [lead107] RoutePatrol.h
        using Leader::Travel::PatrolWaitBudget;  // [lead107] RoutePatrol.h
        using Leader::Travel::patrolWaitBudgets;  // [lead107] RoutePatrol.h
        using Leader::Travel::patrolHandOvers;  // [lead107] RoutePatrol.h

        using Leader::Travel::PatrolHandedOver;  // [lead107] RoutePatrol.h

        using Leader::Travel::ChargePatrolWait;  // [lead107] RoutePatrol.h

        using Leader::Travel::ROUTE_PATROL_HOLD;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_HOLD_SCAN;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_HOLD_MARGIN;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_HOLD_MAX_MS;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_HOLD_COOLDOWN_MS;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_HOLD_LINGER_MS;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_LANE_HOLD;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_LANE_DEEPER;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_LANE_SAMPLE;  // [lead107] RoutePatrol.h
        using Leader::Travel::HALT_WALK_SAME_DEST;  // [lead107] RoutePatrol.h
        using Leader::Travel::PatrolHold;  // [lead107] RoutePatrol.h
        using Leader::Travel::patrolHolds;  // [lead107] RoutePatrol.h

        using Leader::Travel::PointSegmentDistance2d;  // [lead107] RoutePatrol.h
        using Leader::Travel::WalkEntersPatrolLane;  // [lead107] RoutePatrol.h

        // The patrol that holds the walk, with why ("planner pulls it" / "<yd> from <member>"); empty: no hold.
        std::string PatrolHoldWhy(Party const& party, Player* leader)
        {
            // [lead107] the leader's (Ai/Leader/Travel/RoutePatrol.h), read through its group view
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::PatrolHoldWhy(group, leader);
        }


        using Leader::Travel::SegmentsDistance2d;  // [lead107] RouteTargets.h


        using Leader::Travel::HaltLeaderWalk;  // [lead107] RoutePatrol.h

        using Leader::Travel::PATROL_STEP_BACK;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_STEP_BACK_SPREAD;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_STEP_BACK_MIN;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_STEP_BACK_ARRIVED;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_STEP_BACK_GONE;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_STEP_BACK_MAX_MS;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_STEP_BACK_COOLDOWN_MS;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_STEP_BACK_RETRY_MS;  // [lead107] RoutePatrol.h
        using Leader::Travel::PatrolStepBackState;  // [lead107] RoutePatrol.h
        using Leader::Travel::patrolStepBacks;  // [lead107] RoutePatrol.h

        using Leader::Travel::StaticPackMob;  // [lead107] RoutePatrol.h

        using Leader::Travel::PATROL_STEP_BACK_HOP;  // [lead107] RoutePatrol.h
        using Leader::Travel::PatrolStepBackSpot;  // [lead107] RoutePatrol.h

        using Leader::Travel::BossRoomKey;  // [lead107] RouteTargets.h
        using Leader::Travel::bossWalkCache;  // [lead107] RouteTargets.h
        using Leader::Travel::detourLoggedMs;  // [lead107] RouteTargets.h

        using Leader::Travel::BossWalk;  // [lead107] RouteTargets.h

        using Leader::Travel::WalkableToBoss;  // [lead107] RouteTargets.h

        using Leader::Travel::ScriptHelpDef;  // [lead107] RouteTargets.h
        using Leader::Travel::SCRIPT_HELP_BOSSES;  // [lead107] RouteTargets.h

        using Leader::Travel::ScriptCallForHelpReaches;  // [lead107] RouteTargets.h

        using Leader::Travel::smartHelpRanges;  // [lead107] RouteTargets.h

        using Leader::Travel::SmartCallForHelpRange;  // [lead107] RouteTargets.h


        using Leader::Travel::PULL_SIGHT_RANGE;  // [lead107] RouteSteer.h
        using Leader::Travel::PULL_SIGHT_RANGE_SLACK;  // [lead107] RouteSteer.h
        using Leader::Travel::PULL_SIGHT_MAX_LOS;  // [lead107] RouteSteer.h
        using Leader::Travel::PULL_SIGHT_MAX_PATHS;  // [lead107] RouteSteer.h
        using Leader::Travel::PULL_SIGHT_WALK_RATIO;  // [lead107] RouteSteer.h
        using Leader::Travel::PULL_SIGHT_WALK_EXTRA;  // [lead107] RouteSteer.h
        using Leader::Travel::PULL_SIGHT_PATH_END;  // [lead107] RouteSteer.h
        using Leader::Travel::PULL_TOUCH_STEP;  // [lead107] RouteSteer.h
        using Leader::Travel::PULL_IDLE_SCAN;  // [lead107] RouteSteer.h
        using Leader::Travel::PULL_OPTION_MOB_CAP;  // [lead107] RouteSteer.h

        using Leader::Travel::PullTouch;  // [lead107] RouteSteer.h

        using Leader::Travel::PullIdleMobs;  // [lead107] RouteSteer.h

        using Leader::Travel::PullTouchClosure;  // [lead107] RouteSteer.h

        using Leader::Travel::PullTouches;  // [lead107] RouteSteer.h

        using Leader::Travel::PullSightCandidate;  // [lead107] RouteSteer.h

        using Leader::Travel::PullSightSearch;  // [lead107] RouteSteer.h

        using Leader::Travel::PullTouchBetter;  // [lead107] RouteSteer.h

        using Leader::Travel::PULL_SIGHT_TIGHT_MARGIN;  // [lead107] RouteSteer.h
        using Leader::Travel::PULL_SIGHT_MAX_CENTRES;  // [lead107] RouteSteer.h

        using Leader::Travel::FindPullSightSpotWith;  // [lead107] RouteSteer.h

        using Leader::Travel::FindPullSightSpot;  // [lead107] RouteSteer.h

        using Leader::Travel::PullCandidatesJson;  // [lead107] RouteSteer.h

        using Leader::Travel::ROUTE_BOSS_AREA_CLEAR;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_AREA_ENCOUNTER_ADDS;  // [lead107] RouteTargets.h
        using Leader::Travel::ReachabilitySkip;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_AREA_UNSKIP_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_AREA_MAX_UNSKIPS;  // [lead107] RouteTargets.h
        using Leader::Travel::BossAreaUnskip;  // [lead107] RouteTargets.h
        using Leader::Travel::bossAreaUnskips;  // [lead107] RouteTargets.h
        using Leader::Travel::bossAreaHoldLogMs;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_AREA_SCAN_RANGE;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_AREA_BODY_PULL_HOLD_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::bossAreaBodyPullHoldMs;  // [lead107] RouteSteer.h

        using Leader::Travel::ROUTE_BOSS_FOOTPRINT;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_FOOTPRINT_PACK_MAX;  // [lead107] RouteTargets.h
        using Leader::Travel::BossFootprintDef;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_FOOTPRINTS;  // [lead107] RouteTargets.h

        using Leader::Travel::InBossFootprint;  // [lead107] RouteTargets.h

        using Leader::Travel::BossAreaTrashOf;  // [lead107] RouteTargets.h

        using Leader::Travel::RouteNextBoss;  // [lead107] RouteSteer.h

        using Leader::Travel::PatrolCrossesRoute;  // [lead107] RouteTargets.h

        using Leader::Travel::ROUTE_SKIPPED_PACK_PENALTY;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_SKIPPED_SAMPLE_STEP;  // [lead107] RouteTargets.h

        // [lead107] the leader's (RouteTargets.h), by the content's key
        bool AreaGateForContent(AreaGateDef const& gate, ContentDef const& content)
        {
            return Leader::Travel::AreaGateForContent(gate, content.key);
        }

        using Leader::Travel::InAreaGate;  // [lead107] RouteTargets.h

        using Leader::Travel::KeepOutAreas;  // [lead107] RouteTargets.h

        using Leader::Travel::InAnyAreaGate;  // [lead107] RouteTargets.h

        using Leader::Travel::RouteCrossesClosedArea;  // [lead107] RouteTargets.h

        using Leader::Travel::ROUTE_CLOSED_DOORS;  // [lead107] RouteLegs.h
        using Leader::Travel::DOOR_RAY_HEIGHT;  // [lead107] RouteLegs.h
        using Leader::Travel::DOOR_SEGMENT_MARGIN;  // [lead107] RouteLegs.h
        using Leader::Travel::CLOSED_DOOR_SKIP_MS;  // [lead107] RouteTargets.h


        using Leader::Travel::ROUTE_PASSAGE_DOOR_DETOUR;  // [lead107] RouteLegs.h

        using Leader::Travel::ShutForGoodDoorDef;  // [lead107] RouteLegs.h
        using Leader::Travel::SHUT_FOR_GOOD_DOORS;  // [lead107] RouteLegs.h

        using Leader::Travel::lockedInSince;  // [lead107] RouteSteer.h
        using Leader::Travel::LOCKED_IN_FAIL_MS;  // [lead107] RouteSteer.h

        using Leader::Travel::ShutForGoodDoorSlot;  // [lead107] RouteLegs.h



        using Leader::Travel::PASSAGE_ROUND_MEMO_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::PassageRoundMemo;  // [lead107] RouteTargets.h
        using Leader::Travel::passageRoundMemos;  // [lead107] RouteTargets.h

        using Leader::Travel::GoesRoundPassageDoor;  // [lead107] RouteTargets.h

        using Leader::Travel::ROUTE_CLOSED_DOORS_ALL;  // [lead107] RouteLegs.h
        using Leader::Travel::DOOR_HOLD_BACK;  // [lead107] RouteLegs.h
        using Leader::Travel::DOOR_HOLD_MIN_WALK;  // [lead107] RouteLegs.h
        constexpr uint32 DOOR_HOLD_LOG_MS = 5 * IN_MILLISECONDS;
        using Leader::Travel::DOOR_HOLD_FRESH_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::DOOR_HOLD_MEMO_MS;  // [lead107] RouteLegs.h
        using Leader::Travel::DOOR_HOLD_MEMO_DISTANCE;  // [lead107] RouteLegs.h

        using Leader::Travel::DoorEventOf;  // [lead107] RouteLegs.h

        using Leader::Travel::IsDoorKey;  // [lead107] RouteEvents.h

        using Leader::Travel::DoorOpensFor;  // [lead107] RouteEvents.h

        using Leader::Travel::DoorKeyLocked;  // [lead107] RouteTargets.h

        using Leader::Travel::PendingDoorGuards;  // [lead107] RouteTargets.h

        using Leader::Travel::PendingGuardApproach;  // [lead107] RouteTargets.h

        using Leader::Travel::IsPendingDoorGuard;  // [lead107] RouteSteer.h

        using Leader::Travel::ChestOpeningSpell;  // [lead107] RouteEvents.h

        using Leader::Travel::GUNDRAK_MAP_ID;  // [lead107] RouteTargets.h
        using Leader::Travel::GUNDRAK_GALDARAH_ENTRY;  // [lead107] RouteTargets.h
        using Leader::Travel::GUNDRAK_SLADRAN_SLOT;  // [lead107] RouteTargets.h
        using Leader::Travel::GUNDRAK_MOORABI_SLOT;  // [lead107] RouteTargets.h
        using Leader::Travel::GUNDRAK_COLOSSUS_SLOT;  // [lead107] RouteTargets.h
        using Leader::Travel::COS_MAP_ID;  // [lead107] RouteTargets.h
        using Leader::Travel::COS_ARTHAS_ENTRY;  // [lead107] RouteTargets.h
        using Leader::Travel::COS_CHROMIE_MIDDLE_ENTRY;  // [lead107] RouteTargets.h

        using Leader::Travel::GossipStarterHeld;  // [lead107] RouteTargets.h

        using Leader::Travel::ScriptBossDone;  // [lead107] RouteTargets.h

        using Leader::Travel::DoorsClosedFor;  // [lead107] RouteEvents.h

        using Leader::Travel::PointOnPath;  // [lead107] RouteSearch.h

        using Leader::Travel::DoorStopMemo;  // [lead107] RouteLegs.h
        using Leader::Travel::doorStopMemos;  // [lead107] RouteLegs.h


        using Leader::Travel::CLOSED_AREA_SKIP_RANGE;  // [lead107] RouteSteer.h
        using Leader::Travel::CLOSED_AREA_SKIP_MAX;  // [lead107] RouteSteer.h

        using Leader::Travel::ClosedAreaMobCheck;  // [lead107] RouteSteer.h

        using Leader::Travel::KeepTankOutOfClosedAreas;  // [lead107] RouteSteer.h

        using Leader::Travel::RouteCrossesSkipped;  // [lead107] RouteTargets.h

        using Leader::Travel::ARTHAS_STARTER_MOVING_WAIT_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::ARTHAS_STARTER_SEEN_GAP_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::ArthasMovingWait;  // [lead107] RouteEvents.h
        using Leader::Travel::arthasMovingWaits;  // [lead107] RouteEvents.h

        using Leader::Travel::ArthasStarterMoving;  // [lead107] RouteEvents.h

        using Leader::Travel::WalksScriptedPath;  // [lead107] RouteEvents.h

        using Leader::Travel::SkadiGauntletRunning;  // [lead107] RouteTargets.h

        using Leader::Travel::InSkadiHall;  // [lead107] RouteTargets.h

        using Leader::Travel::RunningIntroBoss;  // [lead107] RouteTargets.h

        using Leader::Travel::AtHoldGuard;  // [lead107] RouteTargets.h

        // A script slot IN_PROGRESS for at most SCRIPT_EVENT_MAX_MS (an event between its waves).
        bool ScriptEventRunning(Party const& party)
        {
            for (BossTrack const& boss : party.bosses)
                if (!boss.dbc && !boss.killed && !boss.failed && boss.lastState == IN_PROGRESS &&
                    (!boss.attemptStartMs || getMSTimeDiff(boss.attemptStartMs, getMSTime()) < SCRIPT_EVENT_MAX_MS))
                    return true;
            return false;
        }

        bool SpotBossIsNext(Party const& party, EventSpotDef const& spot, Player* leader)
        {
            // [lead107] the leader's (RouteTargets.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::SpotBossIsNext(group, spot, leader);
        }

        using Leader::Travel::CreatureStarterKind;  // [lead107] RouteTargets.h

        using Leader::Travel::StarterNpcFlag;  // [lead107] RouteTargets.h

        using Leader::Travel::QuestItemsReady;  // [lead107] RouteTargets.h

        // [b32_pos] The instance script of the party's instance, through its leader (nullptr outside it).
        InstanceScript* PartyInstanceScript(Party const& party)
        {
            Player* leader = ObjectAccessor::FindPlayer(party.leader);
            Map* map = leader ? leader->GetMap() : nullptr;
            InstanceMap* instance = map && map->GetInstanceId() == party.instanceId ? map->ToInstanceMap() : nullptr;
            return instance ? instance->GetInstanceScript() : nullptr;
        }

        // [b32_pos] A progress step (EventSpotDef::progressData) that was reached (eventSpotState 1, not 2 "no path")
        // and whose instance progress is still below progressMin: the group stays and its trigger re-fires.
        bool ProgressStepPending(Party const& party, EventSpotDef const& spot)
        {
            // [lead107] the leader's (RouteEvents.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::ProgressStepPending(group, spot);
        }

        // [brd_ring_hold] A spot that holds the group now: a hold spot, or a step whose event runs
        // (EventSpotDef::eventData IN_PROGRESS) or waits for its retry after a FAIL (Party::eventSpotRetry,
        // WatchEventState).
        bool SpotHolds(Party const& party, EventSpotDef const& spot)
        {
            // [lead107] the leader's (RouteEvents.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::SpotHolds(group, spot);
        }

        // [door_hold_pass] EventStarterDef::passDoors: the door is one of the starter the leader walks to now, or of
        // the running escort.
        bool PassDoorOfActiveStarter(Party const& party, Map* map, ObjectGuid const& doorGuid)
        {
            GameObject const* door = map && !doorGuid.IsEmpty() ? map->GetGameObject(doorGuid) : nullptr;
            if (!door)
                return false;
            for (size_t index = 0; index < party.starters.size(); ++index)
            {
                EventStarterDef const* def = party.starters[index].def;
                if (std::find(def->passDoors.begin(), def->passDoors.end(), door->GetEntry()) == def->passDoors.end())
                    continue;
                if ((party.targetIsStarter && party.starterIdx == index) ||
                    (!party.escort.IsEmpty() && def->kind == StarterKind::Escort))
                    return true;
            }
            return false;
        }

        using Leader::Travel::OCULUS_MAP_ID;  // [lead107] RouteTargets.h
        using Leader::Travel::OCULUS_DATA_DRAKOS;  // [lead107] RouteTargets.h

        using Leader::Travel::OculusDrakePhase;  // [lead107] RouteTargets.h

        bool AnyMemberOnVehicle(Party const& party)
        {
            // [lead107] the leader's (RouteTargets.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::AnyMemberOnVehicle(group);
        }

        using Leader::Travel::LINK_ARRIVE_RANGE;  // [lead107] RouteEvents.h
        using Leader::Travel::LINK_ARRIVE_HEIGHT;  // [lead107] RouteEvents.h
        using Leader::Travel::LINK_DONE_RANGE;  // [lead107] RouteEvents.h
        using Leader::Travel::LINK_GATHER_RANGE;  // [lead107] RouteEvents.h
        using Leader::Travel::LINK_MAX_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::LINK_CROSS_LOG_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::LINK_PROGRESS_STEP;  // [lead107] RouteEvents.h
        using Leader::Travel::LINK_WALK_MAX_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::LINK_HOLD_MS;  // [lead107] RouteEvents.h
        using Leader::Travel::LINK_MAX_FAILS;  // [lead107] RouteEvents.h
        using Leader::Travel::LIFT_MAX_MS;  // [lead107] RouteEvents.h

        using Leader::Travel::AtLinkPoint;  // [lead107] RouteEvents.h

        using Leader::Travel::LINK_OFF_LINE_RANGE;  // [lead107] RouteEvents.h

        using Leader::Travel::LinkLineDistance;  // [lead107] RouteEvents.h

        using Leader::Travel::LinkOffLine;  // [lead107] RouteEvents.h

        using Leader::Travel::LinkIsLift;  // [lead107] RouteEvents.h

        using Leader::Travel::ROUTE_NO_WALK_IN_PULL;  // [lead107] RouteEvents.h
        using Leader::Travel::pullWalkLogMs;  // [lead107] RouteEvents.h

        using Leader::Dungeon::PULL_ABORT_HOLD_MS;  // [lead107] LeaderReports.h

        using Leader::Travel::TankPullRunning;  // [lead107] RouteEvents.h

        using Leader::Travel::PULL_HOLD_STALL_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::PULL_HOLD_RELEASE_MS;  // [lead107] RouteSteer.h
        using Leader::Travel::PullHoldTrack;  // [lead107] RouteEvents.h
        using Leader::Travel::pullHolds;  // [lead107] RouteEvents.h

        using Leader::Travel::PullHoldReleased;  // [lead107] RouteEvents.h

        // The leader's waypoint is withheld for its own running pull (GiveWaypoint's ROUTE_NO_WALK_IN_PULL rule).
        // [lead2] LEADER_ONE_TANK_MOVER (C:/WoW/LEADER_REFACTOR_PLAN.md step 2): the tank's open pull order (Leader::
        // Orders) is the one owner of its movement, a boss target's too; the leader gets the tank back on the order's
        // events only (the tank's Aborted, the order's fight over, the tank gone, order_orphan). TankPullRunning's
        // clocks (GetMaxPullTime, the RecentlyAborted window) and the PULL_HOLD_STALL_MS / PULL_HOLD_RELEASE_MS
        // release are the A/B's old path. Violet Hold's released prisoners stay the harness's walk in both.
        bool WaypointHeldForPull(Party const& party, Player* bot)
        {
            // [lead107] the leader's (RouteEvents.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::WaypointHeldForPull(group, bot);
        }

        using Leader::Dungeon::TankPullState;  // [lead107] LeaderReports.h

        void GiveWaypoint(Party& party, Player* bot, float x, float y, float z, bool jump = false,
                          bool straight = false)
        {
            // Not for a boss target (the body pull stays) and not in Violet Hold (the released prisoners).
            if (WaypointHeldForPull(party, bot))
            {
                TestRunner::ClearWaypoint(bot->GetGUID());
                uint32 const now = getMSTime();
                uint32& loggedMs = pullWalkLogMs[bot->GetGUID()];
                if (!loggedMs || getMSTimeDiff(loggedMs, now) > 10 * IN_MILLISECONDS)
                {
                    loggedMs = now ? now : 1;
                    if (Leader::LEADER_ONE_TANK_MOVER)  // [lead2]
                    {
                        Leader::PullOrder order;
                        Leader::Orders::GetOpen(party.leader, order);
                        LOG_INFO("playerbots",
                                 "[lead2] {}: no harness waypoint ({:.1f},{:.1f},{:.1f}): its pull order {} ({} {}, "
                                 "{}) owns its movement",
                                 bot->GetName(), x, y, z, order.id, Leader::MethodName(order.method), order.targetName,
                                 Leader::StateName(order.state));
                    }
                    else
                        LOG_INFO("playerbots",
                                 "[approach] {}: no harness waypoint ({:.1f},{:.1f},{:.1f}): its pull is running or "
                                 "was just aborted",
                                 bot->GetName(), x, y, z);
                }
                return;
            }
            // [lead1] A leader waypoint for the tank while its pull order is open: two owners of one body, the
            // conflict step 2 (LEADER_ONE_TANK_MOVER) removes. Counted once per order (C:/WoW/LEADER_REFACTOR_PLAN.md
            // step 1, "waypoint while planner holds"); Violet Hold's prisoners apart.
            if (Leader::LEADER_DECISION_LOG && bot->GetGUID() == party.leader)
                if (Leader::PullOrder order; Leader::Orders::GetOpen(party.leader, order))
                {
                    static std::unordered_map<uint32, uint32> notedOrder;  // party seq -> order (world thread)
                    uint32& noted = notedOrder[party.seq];
                    if (noted != order.id)
                    {
                        noted = order.id;
                        bool const violetHold = bot->GetMapId() == VIOLET_HOLD_MAP_ID;
                        DecisionLog::Instance().NoteConflict(
                            DecisionRun{party.id, party.seq, party.runStartMs},
                            violetHold ? "waypoint_in_order_vh" : "waypoint_in_order", order.target,
                            "leader waypoint while order " + std::to_string(order.id) + " is " +
                                Leader::StateName(order.state),
                            JsonObject().Num("order", order.id).Str("mob", order.targetName)
                                .Str("method", Leader::MethodName(order.method)).Bool("bossTarget", party.targetIsBoss)
                                .Real("x", bot->GetPositionX()).Real("y", bot->GetPositionY())
                                .Real("z", bot->GetPositionZ()).Real("toX", x).Real("toY", y).Real("toZ", z));
                    }
                }
            TestRunner::Waypoint target;
            target.mapId = bot->GetMapId();
            target.x = x;
            target.y = y;
            target.z = z;
            target.jump = jump;
            target.straight = straight && !jump;
            target.serial = ++party.waypointSerial;
            TestRunner::SetWaypoint(bot->GetGUID(), target);
        }

        // Over the link as a player walks it: a straight walk (no navmesh on a game-object bridge). Bots walk by
        // themselves: the far end is the bot's waypoint, its own "test runner move" walks straight where the navmesh
        // ends (MovementAction::MoveStraight). HARNESS_MOVES_BOTS: the harness walks it, the bot AI held.
        void StartLinkCross(Party& party, Player* bot, RouteLinkDef const& link)
        {
            // [lead107] the leader's (RouteEvents.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            Leader::Travel::StartLinkCross(group, bot, link);
        }

        using Leader::Travel::LandLinkJump;  // [lead107] RouteEvents.h

        using Leader::Travel::LeaderPullHeld;  // [lead107] RouteSteer.h

        // [boss_dead_encounter_open] the script slot of a boss creature, as the boss_state event knows it (BossTrack:
        // its bound creature, else its entry); -1 when the run tracks no slot for it.
        int32 BossSlotOf(Party const& party, Creature const* boss)
        {
            // [lead107] the leader's (RouteTargets.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::BossSlotOf(group, boss);
        }

        using Leader::Travel::BOSS_SUMMON_SEARCH;  // [lead107] RouteTargets.h
        using Leader::Travel::BossSummonCheck;  // [lead107] RouteTargets.h

        // The entrance nearest (2D) to an object: the wing it stands in (Scarlet Monastery, Dire Maul).
        int32 NearestEntrance(Party const& party, WorldObject const* object)
        {
            // [lead107] the leader's (RouteTargets.h)
            PartyLeaderGroup group(TestHarnessMgr::Instance(), const_cast<Party&>(party));
            return Leader::Travel::NearestEntrance(group, object);
        }

        // Spirit healers near a ghost (the graveyard's), for a corpse run that cannot be walked.
        constexpr float SPIRIT_HEALER_RANGE = 60.0f;
        constexpr float SPIRIT_HEALER_USE_RANGE = 4.0f;  // within INTERACTION_DISTANCE

        struct SpiritHealerCheck
        {
            bool operator()(Creature* creature) const
            {
                return creature->IsInWorld() && creature->IsAlive() && creature->HasNpcFlag(UNIT_NPC_FLAG_SPIRITHEALER);
            }
        };

        Member const* FindMember(Party const& party, ObjectGuid guid)
        {
            for (Member const& member : party.members)
                if (member.guid == guid)
                    return &member;
            return nullptr;
        }
    }

    bool HasFlaskAura(Player* bot)
    {
        std::set<uint32> const& flasks = FlaskSpells();
        for (auto const& [spellId, aurApp] : bot->GetAppliedAuras())
            if (flasks.count(spellId))
                return true;
        return false;
    }

    bool HasWellFed(Player* bot) { return HasAuraNamed(bot, "Well Fed"); }

    bool LeaderLayerHandlesConsumables(Party const& party)
    {
        return party.content && party.content->raid && sPlayerbotAIConfig.RaidLeader &&
               sPlayerbotAIConfig.RaidConsumables;
    }

    namespace
    {
        using Leader::Travel::ROUTE_REGROUP;  // [lead107] RouteMeet.h
        using Leader::Travel::REGROUP_GROUP_RADIUS;  // [lead107] RouteMeet.h
        using Leader::Travel::REGROUP_STUCK_MS;  // [lead107] RouteMeet.h
        using Leader::Travel::REGROUP_PROGRESS;  // [lead107] RouteMeet.h
        using Leader::Travel::REGROUP_RESPAWN_MARGIN_SEC;  // [lead107] RouteMeet.h
        using Leader::Travel::REGROUP_MAX_REPLANS;  // [lead107] RouteMeet.h
        using Leader::Travel::REGROUP_COOLDOWN_MS;  // [lead107] RouteMeet.h
        using Leader::Travel::REGROUP_EXIT_NEEDS_WALK;  // [lead107] RouteMeet.h
        using Leader::Travel::REGROUP_BACK_IN_ONLY;  // [lead107] RouteMeet.h
        using Leader::Travel::REGROUP_BACK_IN_MS;  // [lead107] RouteMeet.h
        using Leader::Travel::REGROUP_RESPAWN_NEVER_SEC;  // [lead107] RouteMeet.h

        using Leader::Travel::RegroupMode;  // [lead107] RouteMeet.h

        using Leader::Travel::RegroupState;  // [lead107] RouteMeet.h
        using Leader::Travel::regroups;  // [lead107] RouteMeet.h
        using Leader::Travel::regroupCooldownUntilMs;  // [lead107] RouteMeet.h

        using Leader::Travel::MeetProgress;  // [lead107] RouteMeet.h
        using Leader::Travel::meetProgress;  // [lead107] RouteMeet.h

        using Leader::Travel::MEET_GRACE_MS;  // [lead107] RouteMeet.h
        using Leader::Travel::MEET_MAX_STUCK;  // [lead107] RouteMeet.h
        using Leader::Travel::MEET_EPISODE_GAP_MS;  // [lead107] RouteMeet.h

        using Leader::Travel::StragglerEpisode;  // [lead107] RouteMeet.h
        using Leader::Travel::stragglers;  // [lead107] RouteMeet.h

        using Leader::Travel::RegroupPathCheck;  // [lead107] RouteMeet.h

        using Leader::Travel::CheckRegroupPath;  // [lead107] RouteMeet.h
    }

    void TestHarnessMgr::SkipTarget(Party& party, ObjectGuid guid, uint32 ms, std::string const& reason)
    {
        if (guid.IsEmpty())
            return;

        party.skipUntil[guid] = getMSTime() + ms;
        party.skipReason[guid] = reason;
        if (guid == party.target)
            party.guaranteeHolding = false;  // ub-n build 17: "careful guarantee hold" with no target for 119 s
        // "route end (+N same entry)" too (steer_stuck with the twins of a ledge).
        if (reason == "unreachable" || reason == "next boss unreachable" || reason == "path lost" ||
            reason.rfind("route end", 0) == 0)
        {
            if (Player* leader = ObjectAccessor::FindPlayer(party.leader))
            {
                party.unreachableFrom[guid] = leader->GetPosition();
                // [incomplete_route_deadend] a steer_stuck "route end": the end the leader stands at is a dead end
                // for this target ("same dead end again" in JudgeIncompleteEnd) - not the refusal's own skips.
                if (reason.rfind("route end", 0) == 0 && reason.rfind("route end: incomplete", 0) != 0)
                    if (WorldObject* object = ObjectAccessor::GetWorldObject(*leader, guid))
                        RecordRouteEndDeadEnd(leader, guid,
                                              G3D::Vector3(object->GetPositionX(), object->GetPositionY(),
                                                           object->GetPositionZ()));
            }
        }
        else
            party.unreachableFrom.erase(guid);
        if (party.target == guid)
        {
            party.target.Clear();
            party.nextRetargetMs = getMSTime();
            TestRunner::ClearWaypoint(party.leader);
        }

        if (reason != "unreachable")
            LogEvent(party, "skip_target", JsonObject().Str("guid", guid.ToString()).Str("reason", reason)
                                               .Num("skipSec", ms / IN_MILLISECONDS));
    }

    bool TestHarnessMgr::IsRouteBlocker(Party const& party, ObjectGuid guid) const
    {
        // [lead107] the leader's (RouteSteer.h)
        PartyLeaderGroup group(const_cast<TestHarnessMgr&>(*this), const_cast<Party&>(party));
        return Leader::Travel::IsRouteBlocker(group, guid);
    }

    bool TestHarnessMgr::RouteBlockerBehindRequired(Party const& party, Map* map, WorldObject const* behind,
                                                    std::string& why) const
    {
        // [lead107] the leader's (RouteSteer.h)
        PartyLeaderGroup group(const_cast<TestHarnessMgr&>(*this), const_cast<Party&>(party));
        return Leader::Travel::RouteBlockerBehindRequired(group, map, behind, why);
    }

    std::string TestHarnessMgr::BlockerPullGate(Party& party, Player* leader, Creature* blocker,
                                                WorldObject const* behind, bool bodyPull, bool& hold) const
    {
        // [lead107] the leader's (RouteSteer.h)
        PartyLeaderGroup group(const_cast<TestHarnessMgr&>(*this), const_cast<Party&>(party));
        return Leader::Travel::BlockerPullGate(group, leader, blocker, behind, bodyPull, hold);
    }

    bool TestHarnessMgr::TargetRouteBlocker(Party& party, Player* leader, Creature* blocker, WorldObject const* behind,
                                            std::string const& why)
    {
        // [lead107] the leader's (RouteSteer.h)
        PartyLeaderGroup group(*this, party);
        return Leader::Travel::TargetRouteBlocker(group, leader, blocker, behind, why);
    }

    void TestHarnessMgr::RouteBlockerBodyPull(Party& party, Player* leader, Creature* target, std::string const& why)
    {
        // [lead107] the leader's (RouteSteer.h)
        PartyLeaderGroup group(*this, party);
        Leader::Travel::RouteBlockerBodyPull(group, leader, target, why);
    }

    void TestHarnessMgr::Retarget(Party& party, Player* leader)
    {
        // [lead107] the leader's (RouteTargets.h)
        PartyLeaderGroup group(*this, party);
        Leader::Travel::Retarget(group, leader);
    }

    // [lead107] the leader's patrol decision lives in the leader (Ai/Leader/Travel/RoutePatrol.h)
    bool TestHarnessMgr::PatrolStepBack(Party& party, Player* leader)
    {
        PartyLeaderGroup group(*this, party);
        return Leader::Travel::PatrolStepBack(group, leader);
    }

    void TestHarnessMgr::Steer(Party& party, Player* leader)
    {
        // [lead107] the leader's (RouteSteer.h)
        PartyLeaderGroup group(*this, party);
        Leader::Travel::Steer(group, leader);
    }

    bool TestHarnessMgr::WingBossesLeft(Party const& party, Map* map, Player* leader, int32& nextWing) const
    {
        // [lead107] the leader's (RouteTargets.h)
        PartyLeaderGroup group(const_cast<TestHarnessMgr&>(*this), const_cast<Party&>(party));
        return Leader::Travel::WingBossesLeft(group, map, leader, nextWing);
    }

    void TestHarnessMgr::StartWingChange(Party& party, size_t next, char const* why)
    {
        // [lead107] the leader's (RouteTargets.h)
        PartyLeaderGroup group(*this, party);
        Leader::Travel::StartWingChange(group, next, why);
    }

    bool TestHarnessMgr::EventGatherApplies(Party const& party) const
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(const_cast<TestHarnessMgr&>(*this), const_cast<Party&>(party));
        return Leader::Travel::EventGatherApplies(group);
    }

    bool TestHarnessMgr::WalkToEventGather(Party& party, Player* leader)
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(*this, party);
        return Leader::Travel::WalkToEventGather(group, leader);
    }

    void TestHarnessMgr::CheckPullHealer(Party& party, std::vector<Player*> const& players)
    {
        // The tank of the pull: the main tank slot, else the first tank; nobody to compare with without one.
        Player* tank = nullptr;
        for (Member const& member : party.members)
        {
            if (member.spec->role != Role::Tank)
                continue;
            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            if (!bot || !bot->IsAlive())
                continue;
            if (!tank || (member.slot.flags & SLOT_MAIN_TANK))
                tank = bot;
        }
        if (!tank || !tank->IsInCombat())
            return;

        JsonArray healers;
        bool anyHealer = false;
        bool healerInRange = false;
        for (Member const& member : party.members)
        {
            if (member.spec->role != Role::Healer)
                continue;
            anyHealer = true;
            Player* bot = nullptr;
            for (Player* candidate : players)
                if (candidate->GetGUID() == member.guid)
                    bot = candidate;
            bool const alive = bot && bot->IsAlive();
            float const distance = bot && bot->GetMap() == tank->GetMap() ? bot->GetExactDist(tank) : -1.0f;
            healerInRange |= alive && distance >= 0.0f && distance <= PULL_HEALER_RANGE;
            healers.Raw(JsonObject().Str("name", member.name).Bool("alive", alive).Real("distance", distance)
                            .Build());
        }
        if (!anyHealer || healerInRange)
            return;

        Unit* victim = tank->GetVictim();
        Creature* mob = victim ? victim->ToCreature() : nullptr;
        bool const boss = mob && (mob->IsDungeonBoss() || mob->isWorldBoss());
        LogEvent(party, "pull_without_healer",
                 JsonObject().Str("tank", tank->GetName()).Str("target", mob ? mob->GetName() : "")
                     .Num("entry", mob ? mob->GetEntry() : 0).Bool("boss", boss)
                     .Num("attackers", int64(tank->getAttackers().size())).Raw("healers", healers.Build()));
    }

    // A key door that shut itself between the leader and a member mid-fight (strat-n build 10: the Service Entrance
    // Gate closed 3 s after the key, the tank fought on the far side for 510 s, both healers "no line of sight", the
    // tank died): the leader uses the key again from where it stands. Retarget (the out-of-combat re-arm) does not
    // run in combat. Same budget as that re-arm (DOOR_REARM_MAX, DOOR_REARM_GAP_MS between uses).
    void TestHarnessMgr::ReopenKeyDoorInCombat(Party& party, Player* leader)
    {
        Map* map = leader->GetMap();
        if (!map || map->GetInstanceId() != party.instanceId || !leader->IsAlive() ||
            leader->IsNonMeleeSpellCast(false))
            return;
        uint32 const now = getMSTime();
        for (StarterTrack& starter : party.starters)
        {
            if (starter.def->kind != StarterKind::UseItem || starter.uses < starter.def->maxUses ||
                starter.doorRearms >= DOOR_REARM_MAX || starter.combatRearms >= DOOR_REARM_COMBAT_MAX ||
                !party.lastStarterUseMs || getMSTimeDiff(party.lastStarterUseMs, now) <= DOOR_REARM_GAP_MS ||
                (starter.combatRearmMs && getMSTimeDiff(starter.combatRearmMs, now) < DOOR_REARM_COMBAT_GAP_MS) ||
                !leader->getAttackers().empty() || !leader->HasItemCount(starter.def->itemId, 1))
                continue;
            for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
            {
                if (!go || !go->isSpawned() || go->GetGoType() != GAMEOBJECT_TYPE_DOOR ||
                    go->GetGoState() != GO_STATE_READY || !go->IsWithinDistInMap(leader, INTERACTION_DISTANCE) ||
                    std::find(starter.def->entries.begin(), starter.def->entries.end(), go->GetEntry()) ==
                        starter.def->entries.end())
                    continue;
                std::vector<GameObject*> const door = {go};
                Player* behind = nullptr;
                for (Member const& member : party.members)
                    if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                        bot && bot != leader && bot->IsAlive() && bot->GetMap() == map && !behind)
                    {
                        PathResult const way = RoutePath(bot, leader->GetPositionX(), leader->GetPositionY(),
                                                         leader->GetPositionZ());
                        if (way.reachable && RouteThroughClosedDoor(way.points, door, leader->GetPhaseMask()))
                            behind = bot;
                    }
                if (!behind)
                    continue;
                ++starter.doorRearms;
                ++starter.combatRearms;
                starter.combatRearmMs = now ? now : 1;
                starter.uses = 0;
                starter.usedObjects.clear();
                LogEvent(party, "starter_rearmed", JsonObject().Str("note", starter.def->note)
                                                       .Str("why", "the door closed again in a fight, " +
                                                                       std::string(behind->GetName()) + " behind it")
                                                       .Num("rearms", starter.doorRearms).Bool("inCombat", true));
                UseStarter(party, leader, starter, go);
                party.lastStarterUseMs = now;  // DOOR_REARM_GAP_MS throttles a refused or interrupted cast too
                return;
            }
        }
    }

    // Out of combat, every tick: the leader stands at a closed key door its walk holds at (door_hold) - the whole
    // group on the near side, or members behind it - and uses the key from there (strat-h builds 14 / 16: the
    // Retarget re-arm never fired, 0 starter_rearmed, the leader held at the gate at 3.3 yd for minutes; four builds
    // stuck at this gate). Every failed check near such a door logs "rearm_check" with its reason (REARM_CHECK_LOG_MS).
    // [expect_door] EventStarterDef::expectDoor: EXPECT_DOOR_MS after a counted use the door must have left
    // GO_STATE_READY (Deadmines: smart_scripts 1639800 id 1 sets the Iron Clad Door 16397 to state 2 500 ms after Fire
    // Cannon 6250 hits the cannon). Still shut: the cast broke or was refused - the use is undone and the starter armed
    // again, at most EXPECT_DOOR_MAX_RETRIES times (the Defias Gunpowder is consumed only by a completed cast).
    void TestHarnessMgr::CheckExpectedDoors(Party& party, Player* leader)
    {
        Map* map = leader ? leader->GetMap() : nullptr;
        if (!map || map->GetInstanceId() != party.instanceId)
            return;
        uint32 const now = getMSTime();
        for (StarterTrack& starter : party.starters)
        {
            if (!starter.def->expectDoor || !starter.expectDoorCheckMs || int32(now - starter.expectDoorCheckMs) < 0)
                continue;
            starter.expectDoorCheckMs = 0;
            GameObject* door = nullptr;
            for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
                if (go && go->isSpawned() && go->GetEntry() == starter.def->expectDoor)
                {
                    door = go;
                    break;
                }
            if (door && door->GetGoState() != GO_STATE_READY)
            {
                party.lastProgressMs = now;
                LogEvent(party, "expect_door_ok", JsonObject().Str("note", starter.def->note)
                                                      .Str("door", door->GetName()).Num("doorEntry", door->GetEntry())
                                                      .Num("goState", uint32(door->GetGoState()))
                                                      .Num("retries", starter.expectDoorFails));
                continue;
            }
            ++starter.expectDoorFails;
            bool const item = !starter.def->itemId || leader->HasItemCount(starter.def->itemId, 1);
            bool const rearm = starter.expectDoorFails <= EXPECT_DOOR_MAX_RETRIES && item;
            if (rearm)
            {
                starter.uses = 0;
                starter.usedObjects.clear();
                starter.failedUses.clear();
                starter.steppedOut.clear();
                starter.nextAllowedMs = now + STARTER_ITEM_RETRY_MS;
            }
            LogEvent(party, rearm ? "expect_door_retry" : "expect_door_gave_up",
                     JsonObject().Str("note", starter.def->note).Num("doorEntry", starter.def->expectDoor)
                         .Bool("doorFound", door != nullptr).Num("retries", starter.expectDoorFails)
                         .Bool("itemInBags", item).Real("leaderDistance", door ? leader->GetExactDist(door) : -1.0f)
                         .Bool("leaderInCombat", leader->IsInCombat()));
        }
    }

    // [b33_strat_gate] The harness waypoints of an auto-closing gate (the pass through it, the step to it for a
    // member's own key, the cut-off member's gather) are marked gatePass and cleared once the pass / cut is over. strat-h
    // 20261026-000434-strat-h-5D-1: the healer's member_key_step waypoint 3 yd from the Service Entrance Gate (98 s)
    // outlived the cut ("together" at 102 s); "test runner move" (11) walked it back to the gate against its follow
    // (1) and the stuck watchdog for 200 s ("[move_conflict] Rtshaab: 'test runner move' and 'follow' pull it opposite
    // ways"), 47-90 yd from the tank, and the pull at 304 s went without it - the wipe at 425 s.
    static void MarkGateWaypoint(Player* bot)
    {
        TestRunner::Waypoint waypoint;
        if (bot && TestRunner::GetWaypoint(bot->GetGUID(), waypoint))
        {
            waypoint.gatePass = true;
            TestRunner::SetWaypoint(bot->GetGUID(), waypoint);
        }
    }

    static void ClearGateWaypoints(Party& party)
    {
        for (Member& member : party.members)
        {
            TestRunner::Waypoint waypoint;
            if (member.guid != party.leader && TestRunner::GetWaypoint(member.guid, waypoint) && waypoint.gatePass)
            {
                TestRunner::ClearWaypoint(member.guid);
                member.hasWaypoint = false;
            }
        }
    }

    // [strat_key_pass] KeyDoorPassDef: the key gate used (UseStarter set keyPassDoor) opened - every member still on
    // the use side gets the far point as its waypoint at once (the gate shuts 3 s later); shut again with members on
    // that side, they are logged (the existing re-key: ReopenKeyDoorHeld / ReopenKeyDoorInCombat, DOOR_REARM_MAX).
    // Never opened within KEY_PASS_MAX_MS: the cast broke (moving, a hit) - logged, the starter's own retry runs.
    // [b39_strat] The fight an opening woke (strat_key_pass_cancelled) is fought on the use side: the tank's fight spot
    // (FightHold, its chase walks end RADIUS round it) is held KEY_OPENING_FIGHT_BACK yd back from the gate on that
    // side, so no chase walk takes it through the open gate - the mobs come to it (a creature's path ignores the gate).
    // strat-h 20261026-054314-strat-h-5C-1: the pass was cancelled at 149 s with the tank at the use point, then its
    // own "reach melee" on a Skeletal Berserker walked it through the gate (still open, 148-151 s) to 3634.7; the gate
    // shut, the tank stood 0.9 yd past it, the healer had no sight of it, dead at 211 s.
    struct OpeningFight
    {
        ObjectGuid door;
        Position anchor;
        uint32 sinceMs = 0;
        uint32 calmMs = 0;  // since when nobody of the group fights (0: someone does)
    };
    static std::unordered_map<uint32, OpeningFight> openingFights;  // per instance; the harness's world thread only
    constexpr float KEY_OPENING_FIGHT_BACK = FightHold::RADIUS + 2.0f;
    constexpr uint32 KEY_OPENING_FIGHT_MAX_MS = 3 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 KEY_OPENING_FIGHT_CALM_MS = 3 * IN_MILLISECONDS;

    static void TickOpeningFight(Party& party, Player* leader)
    {
        auto const found = openingFights.find(party.instanceId);
        if (found == openingFights.end())
            return;
        OpeningFight& fight = found->second;
        uint32 const now = getMSTime();
        bool fighting = false;
        for (Member const& member : party.members)
            if (Player* bot = ObjectAccessor::FindPlayer(member.guid); bot && bot->IsAlive() && bot->IsInCombat())
                fighting = true;
        if (fighting)
            fight.calmMs = 0;
        else if (!fight.calmMs)
            fight.calmMs = now ? now : 1;
        if (!leader || !leader->IsAlive() || getMSTimeDiff(fight.sinceMs, now) > KEY_OPENING_FIGHT_MAX_MS ||
            (fight.calmMs && getMSTimeDiff(fight.calmMs, now) >= KEY_OPENING_FIGHT_CALM_MS))
        {
            openingFights.erase(found);
            return;
        }
        FightHold::Move(leader, fight.anchor);  // kept every tick: a new fight's first FightHold::Tick resets it
    }

    void TestHarnessMgr::TickKeyDoorPass(Party& party, Player* leader)
    {
        TickOpeningFight(party, leader);  // [b39_strat]
        if (party.keyPassDoor.IsEmpty() || !leader)
            return;
        Map* map = leader->GetMap();
        GameObject* door =
            map && map->GetInstanceId() == party.instanceId ? map->GetGameObject(party.keyPassDoor) : nullptr;
        KeyDoorPassDef const* pass = door ? KeyDoorPass(door->GetMapId(), door->GetEntry()) : nullptr;
        uint32 const now = getMSTime();
        if (!door || !pass)
        {
            party.keyPassDoor.Clear();
            return;
        }
        // Side of the gate plane (its facing): the use point's side is "near".
        float const fx = std::cos(door->GetOrientation());
        float const fy = std::sin(door->GetOrientation());
        float const bars = KeyGateBarsOffset(door, *pass);  // [b35_ss2] the bars' plane (GroupThroughKeyGate)
        auto side = [&](float x, float y)
        { return (x - door->GetPositionX()) * fx + (y - door->GetPositionY()) * fy - bars; };
        bool const useSign = side(pass->useX, pass->useY) >= 0.0f;
        auto useSide = [&](Player const* bot)
        { return (side(bot->GetPositionX(), bot->GetPositionY()) >= 0.0f) == useSign; };
        // [b36_ss] A pass back: nobody alive on the use side when the pass starts (the leader re-keyed it from the far
        // side for a walk back, ReopenKeyDoorHeld) - the far side's members are the ones sent, to the use point.
        // strat-n 20261026-034959-strat-n-5A-2: the group through both King's Square Gates, every target behind
        // 175353, the leader held at it from 2129 s; a key from that side had sent nobody (all "far side").
        static std::unordered_map<uint64, bool> passBack;  // the harness's world thread only
        uint64 const passKey = (uint64(party.instanceId) << 32) | party.keyPassSinceMs;
        auto latched = passBack.find(passKey);
        if (latched == passBack.end())
        {
            bool anyUseSide = false;
            for (Member const& member : party.members)
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    bot && bot->IsAlive() && bot->GetMap() == map && useSide(bot))
                    anyUseSide = true;
            if (passBack.size() > 32)
                passBack.clear();
            latched = passBack.emplace(passKey, !anyUseSide).first;
        }
        bool back = latched->second;
        // [b51_strat] A re-open for a member stranded behind this gate (TickDoorCut: Party::doorCutDoor): only the
        // stranded side walks through, to the group; the group's side (the side with more living members, the leader's
        // on a tie) stays where it is - nobody crosses back to fetch it (user 2026-10-26). Latched per pass.
        static std::unordered_map<uint64, int8> reopenSide;  // passKey -> 0 no re-open, 1 the use side stays, 2 far
        auto reopenAt = reopenSide.find(passKey);
        if (reopenAt == reopenSide.end())
        {
            int8 stay = 0;
            if (!party.doorCutMember.IsEmpty() && party.doorCutDoor == door->GetGUID())
            {
                uint32 onUse = 0;
                uint32 onFar = 0;
                for (Member const& member : party.members)
                    if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                        bot && bot->IsAlive() && bot->GetMap() == map)
                        ++(useSide(bot) ? onUse : onFar);
                // [lead14] the leader's side stays, whatever the count: the tank passes first ([b35_ss]), so a gate
                // that shut behind it strands the others on the use side, and the old majority rule sent the leader
                // back through (b58 strat-h-5C-1, King's Square Gate 175352, 1117-1172 s: "stays use side, useSide 4 farSide
                // 1", strat_key_pass "sent Rtdekaa" again and again, keys used 5 -> 9). User rule: once through, the
                // group stays; only the stranded come through, nobody who is through walks back.
                bool const useStays = LeaderDecidesFor(party) ? useSide(leader)
                                                              : (onUse != onFar ? onUse > onFar : useSide(leader));
                stay = useStays ? 1 : 2;
                Player* cutBot = ObjectAccessor::FindPlayer(party.doorCutMember);
                LogEvent(party, "door_reopen_for",
                         JsonObject().Str("member", cutBot ? cutBot->GetName() : "?").Str("door", door->GetName())
                             .Num("doorEntry", door->GetEntry()).Str("stays", useStays ? "use side" : "far side")
                             .Num("useSide", onUse).Num("farSide", onFar));
                LOG_INFO("playerbots", "[b51_strat] door_reopen_for {}: {} {} opened, the {} side ({} members) stays, "
                         "only the other side walks through", cutBot ? cutBot->GetName() : "?", door->GetName(),
                         door->GetEntry(), useStays ? "use" : "far", useStays ? onUse : onFar);
            }
            if (reopenSide.size() > 32)
                reopenSide.clear();
            reopenAt = reopenSide.emplace(passKey, stay).first;
        }
        if (reopenAt->second)
            back = reopenAt->second == 1;  // the use side stays: the far side walks back to the use point
        auto nearSide = [&](Player const* bot) { return useSide(bot) != back; };
        float const destX = back ? pass->useX : pass->farX;
        float const destY = back ? pass->useY : pass->farY;
        float const destZ = back ? pass->useZ : pass->farZ;

        // [queue-key-gate-autoclose] the near-side members start for the far point while the key's 5 s Opening is
        // still being cast: the closed-door guard holds them at the gate, they walk through the moment it opens (it
        // stays open 3 s; sent 5 s after the use, strat-h 163739 at 3108 s, they came too late).
        // [b32_strat_gate] the key's caster: the leader, or a member who used its own key (MemberKeyAtDoor).
        Player* caster = party.keyPassCaster.IsEmpty() ? leader : ObjectAccessor::FindPlayer(party.keyPassCaster);
        if (!caster || caster->GetMap() != map)
            caster = leader;
        bool const opening =
            caster->IsNonMeleeSpellCast(false) && caster->GetExactDist(door) <= KEY_PASS_GATHER_RANGE + 3.0f;
        if (door->GetGoState() != GO_STATE_READY && !party.keyPassOpened)
        {
            party.keyPassOpened = true;  // [b32_strat_gate]
            // [b34_ss] The gate stands open 3 s: the near-side members think now, not at their next AI check (1-3 s
            // later, the walk started after it shut - strat-n 20261026-014521-5C-2, the healer at King's Square Gate
            // 175353 through five openings).
            for (Member const& member : party.members)
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    bot && bot->IsAlive() && bot->GetMap() == map && nearSide(bot))
                    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                        botAI->SetNextCheckDelay(0);
        }
        // [b32_strat_gate] the pass walk is a door pass (TestRunner::Waypoint::gatePass): the idle-mob guard does not
        // shorten it. strat-n 221216: "[follow path] Rlpriaf: test runner move to (3630.3,-3639.5) would wake Ravaged
        // Cadaver (3.4 yd of reach 10.4) -> alt shorter (3624.4,-3641.8)" - the healer's pass was cut to 1.9 yd
        // before the gate, it never stepped through in the 3 s the gate stood open, the gate shut on it. A group
        // walks through together and fights what stands behind the gate together.
        auto const sendThrough = [&](Member& member, Player* bot)
        {
            SetMemberWaypoint(party, member, bot, destX, destY, destZ);
            TestRunner::Waypoint waypoint;
            if (TestRunner::GetWaypoint(bot->GetGUID(), waypoint))
            {
                waypoint.gatePass = true;
                TestRunner::SetWaypoint(bot->GetGUID(), waypoint);
            }
        };
        // [b35_ss] The tank goes through first, the rest behind it: while the leader is alive on the near side only it
        // is sent; the others get the far point once it is past the gate plane (the open branch below), within the
        // same opening. strat-n 20261026-024524-strat-n-5E-2: all five sent at once, the far-wait walk held the
        // leader at the gate, the four others went through at 85-91 s and woke the Skeletal Berserker pack 10 yd past
        // the far point; the tank, on the near side and hit through the bars, died alone at 155 s.
        bool const leaderNear = leader->IsAlive() && leader->GetMap() == map && nearSide(leader);
        auto const waitsForLeader = [&](Player const* bot) { return bot != leader && leaderNear; };
        // [b38_strat] whether the group was out of combat when this pass started (the gathered first use), and so
        // whether a fight that starts during the opening is one the opening woke on the far side (below)
        static std::unordered_map<uint64, bool> passCalm;  // the harness's world thread only
        if ((door->GetGoState() != GO_STATE_READY || opening) && !party.keyPassSent)
        {
            bool calm = true;
            for (Member const& member : party.members)
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    bot && bot->IsAlive() && bot->GetMap() == map && (bot->IsInCombat() || !nearSide(bot)))
                    calm = false;
            if (passCalm.size() > 32)
                passCalm.clear();
            passCalm[passKey] = calm;
            party.keyPassSent = true;
            JsonArray sent;
            for (Member& member : party.members)
            {
                Player* bot = ObjectAccessor::FindPlayer(member.guid);
                if (!bot || !bot->IsAlive() || bot->GetMap() != map || !nearSide(bot) || waitsForLeader(bot))
                    continue;
                sendThrough(member, bot);
                sent.Str(member.name);
            }
            party.lastProgressMs = now;
            LogEvent(party, "strat_key_pass", JsonObject().Str("note", pass->note).Raw("sent", sent.Build())
                                                  .Num("afterMs", getMSTimeDiff(party.keyPassSinceMs, now)));
            // [b30_overpull] the skips this gate made - "leader held at a closed door (<gate>)" and every "behind
            // skipped ..." chain rooted in it - are lifted now: the gate shuts again 3 s later, so the per-retarget
            // lift ([queue-door-skip-lift], open state only) missed them, and they outlived the pass by 180 s
            // (strat-h 20261025-184229-5: keyed at 87 s, the 1 s target thrash on them from 141 s to the 185 s pull).
            std::string const doorTag = Acore::StringFormat("leader held at a closed door ({} {})", door->GetName(),
                                                            door->GetEntry());
            std::vector<ObjectGuid> lifted;
            for (auto const& [guid, why] : party.skipReason)
                if (why.find(doorTag) != std::string::npos)
                    lifted.push_back(guid);
            for (ObjectGuid const& guid : lifted)
            {
                party.skipUntil.erase(guid);
                party.skipReason.erase(guid);
                party.unreachableFrom.erase(guid);
            }
            if (!lifted.empty())
                LogEvent(party, "skip_lifted",
                         JsonObject().Str("why", Acore::StringFormat("the key opened {} {}", door->GetName(),
                                                                     door->GetEntry()))
                             .Num("count", uint32(lifted.size())));
            return;
        }
        // [b32_strat_gate] open: a near-side member whose pass waypoint was taken (a fight's clear, a gather) gets it
        // again, every tick of the 3 s.
        if (door->GetGoState() != GO_STATE_READY && party.keyPassSent)
        {
            // [b38_strat] The opening woke the far side (a pass started with the whole group out of combat on the use
            // side, and now a member fights): nobody walks through. The mobs come to the group through the gate (a
            // creature's path ignores it), the group fights them where it stands, all on one side - the healer's sight
            // is not cut by the gate shutting 3 s later. strat-n 20261026-050341-strat-n-5H-2 at 1443-1447 s: the
            // tank's pass walked it into the woken Mangled Cadaver chain past 175353, the gate shut on the healer.
            if (auto const calm = passCalm.find(passKey); calm != passCalm.end() && calm->second)
            {
                bool fighting = false;
                bool anyFar = false;
                for (Member const& member : party.members)
                    if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                        bot && bot->IsAlive() && bot->GetMap() == map)
                    {
                        fighting = fighting || bot->IsInCombat();
                        anyFar = anyFar || !nearSide(bot);
                    }
                if (fighting && !anyFar)
                {
                    calm->second = false;
                    ClearGateWaypoints(party);
                    TestRunner::Waypoint waypoint;
                    if (TestRunner::GetWaypoint(party.leader, waypoint) && waypoint.gatePass)
                        TestRunner::ClearWaypoint(party.leader);
                    // [b39_strat] the fight's spot on the use side, KEY_OPENING_FIGHT_BACK yd from the gate
                    float const backX = back ? pass->farX : pass->useX;
                    float const backY = back ? pass->farY : pass->useY;
                    float dx = backX - door->GetPositionX();
                    float dy = backY - door->GetPositionY();
                    float const len = std::max(0.1f, std::hypot(dx, dy));
                    dx /= len;
                    dy /= len;
                    OpeningFight fight;
                    fight.door = door->GetGUID();
                    fight.sinceMs = now ? now : 1;
                    float ax = door->GetPositionX() + dx * KEY_OPENING_FIGHT_BACK;
                    float ay = door->GetPositionY() + dy * KEY_OPENING_FIGHT_BACK;
                    float az = map->GetHeight(leader->GetPhaseMask(), ax, ay, door->GetPositionZ() + 3.0f, true, 10.0f);
                    if (az <= INVALID_HEIGHT)
                    {
                        ax = backX;  // no floor there: the use point (the hold still ends short of the gate)
                        ay = backY;
                        az = back ? pass->farZ : pass->useZ;
                    }
                    fight.anchor.Relocate(ax, ay, az);
                    openingFights[party.instanceId] = fight;
                    FightHold::Move(leader, fight.anchor);
                    LogEvent(party, "strat_key_pass_cancelled",
                             JsonObject().Str("note", pass->note).Str("why", "the opening woke the far side")
                                 .Num("afterMs", getMSTimeDiff(party.keyPassSinceMs, now))
                                 .Real("fightX", ax).Real("fightY", ay).Real("fightZ", az));  // [b39_strat]
                    party.keyPassDoor.Clear();
                    return;
                }
            }
            // [b35_ss] the caster too once its cast is over (the leader who keyed it lost its pass walk to the far
            // wait and was never sent again: 5E-2 at 86-93 s); the others once the leader is through (waitsForLeader).
            for (Member& member : party.members)
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    bot && (bot != caster || !caster->IsNonMeleeSpellCast(false)) && bot->IsAlive() &&
                    bot->GetMap() == map && nearSide(bot) && !waitsForLeader(bot))
                {
                    TestRunner::Waypoint waypoint;
                    if (!TestRunner::GetWaypoint(bot->GetGUID(), waypoint) || !waypoint.gatePass)
                    {
                        sendThrough(member, bot);
                        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                            botAI->SetNextCheckDelay(0);  // [b35_ss] the walk starts now, the gate shuts in 3 s
                    }
                }
            return;
        }
        // [b32_strat_gate] shut before it was ever open: the Opening is still being cast (strat-n 221216: the pass
        // sent at the cast's start, "strat_key_member_left" with all five 1 s later and the pass forgotten before
        // the gate opened).
        if (door->GetGoState() == GO_STATE_READY && party.keyPassSent && !party.keyPassOpened &&
            getMSTimeDiff(party.keyPassSinceMs, now) <= KEY_PASS_MAX_MS)
            return;
        if (door->GetGoState() == GO_STATE_READY && party.keyPassSent)
        {
            JsonArray left;
            for (Member const& member : party.members)
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    bot && bot->IsAlive() && bot->GetMap() == map && nearSide(bot))
                    left.Str(member.name);
            if (!left.Empty())
                LogEvent(party, "strat_key_member_left",
                         JsonObject().Str("note", pass->note).Raw("members", left.Build()));
            ClearGateWaypoints(party);  // [b33_strat_gate] the gate shut: its pass walks are over
            party.keyPassDoor.Clear();
            return;
        }
        if (getMSTimeDiff(party.keyPassSinceMs, now) > KEY_PASS_MAX_MS)
        {
            if (!party.keyPassSent)
                LogEvent(party, "strat_key_no_open", JsonObject().Str("note", pass->note)
                                                         .Real("leaderDistance", leader->GetExactDist(door))
                                                         .Bool("leaderCasting", leader->IsNonMeleeSpellCast(false)));
            ClearGateWaypoints(party);  // [b33_strat_gate]
            party.keyPassDoor.Clear();
        }
    }

    bool TestHarnessMgr::DoorGatherHolds(Party& party, Player* opener, GameObject* door)
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(*this, party);
        return Leader::Travel::DoorGatherHolds(group, opener, door);
    }

    // [b51_strat] Stratholme's two rat-trap alleys (instance_stratholme.cpp aGateTrap / Update / StartGateTrap): a
    // non-GM player within 5.5 yd 2D of the trap point drops both portcullises at once (Scarlet 175350 / 175351, 9.3 yd
    // either side of the point; Undead 175354 / 175355, 11.5 yd), the plagued critters come 2 s later, the gates open
    // after 20 s, then the trap rests 30 min. strat-h 20261026-122322-strat-h-5B-2 at 1598 s: the tank's waypoint
    // ran over the Scarlet point with the group 17-30 yd behind (follow): tank inside, everyone else outside for 21 s.
    // User: «Танк за воротами, а все остальные с другой стороны. Ну ошибка же.» The player way: before anyone
    // steps within TRAP_HOLD_RANGE of an armed point, the group walks in to the band between the trigger circle and the
    // approach-side gate and waits there for each other; then the leader walks on over it, everyone inside. Each
    // member's spot is TRAP_SPOT_AXIS yd from the point on the alley's axis: a walk that stops at it is outside the
    // trigger, one that stops the runner's 2.5 yd short of it is still inside the gate. The cooldown is not instance
    // data (InstanceScript::GetData exposes no trap state): a trap counts armed until one of its portcullises is seen
    // shut in this run, then resting for GATE_TRAP_REST_MS.
    void TestHarnessMgr::TickGateTrapGather(Party& party, Player* leader)
    {
        constexpr uint32 STRATHOLME_MAP = 329;
        struct GateTrapDef
        {
            float x;
            float y;
            uint32 gateA;
            uint32 gateB;
            char const* note;
        };
        static GateTrapDef const traps[] = {
            {3612.29f, -3335.39f, 175350, 175351, "Scarlet side rat trap"},
            {3919.88f, -3547.34f, 175354, 175355, "Undead side rat trap"},
        };
        constexpr float TRAP_TRIGGER = 5.5f;      // the script's 2D radius
        constexpr float TRAP_HOLD_RANGE = 8.0f;   // nobody steps this close to an armed point before the gather
        constexpr float TRAP_SPOT_AXIS = 6.3f;    // a member's spot on the axis (see above)
        constexpr float TRAP_INSIDE_MIN = 5.9f;   // inside the band: past this from the point ...
        constexpr float TRAP_GATE_MARGIN = 0.4f;  // ... and this far inside the approach-side gate
        constexpr float TRAP_WATCH = 25.0f;       // the leader this close: the alley is looked at
        constexpr uint32 GATE_TRAP_REST_MS = 30 * MINUTE * IN_MILLISECONDS + 25 * IN_MILLISECONDS;
        constexpr uint32 TRAP_GO_MS = 30 * IN_MILLISECONDS;  // after the gather, the walk over it is free this long
        constexpr uint32 TRAP_SEND_MS = 3 * IN_MILLISECONDS;
        constexpr uint32 TRAP_LOG_MS = 10 * IN_MILLISECONDS;
        Map* map = leader ? leader->GetMap() : nullptr;
        if (!map || map->GetId() != STRATHOLME_MAP || map->GetInstanceId() != party.instanceId || !leader->IsAlive())
            return;
        uint32 const now = getMSTime();
        struct TrapState
        {
            uint32 restUntilMs = 0;
            uint32 goUntilMs = 0;
            uint32 holdSinceMs = 0;
            uint32 logMs = 0;
            uint32 sentMs = 0;
        };
        static std::unordered_map<uint64, TrapState> states;  // the harness's world thread only
        for (uint32 i = 0; i < 2; ++i)
        {
            GateTrapDef const& trap = traps[i];
            TrapState& state = states[(uint64(party.instanceId) << 32) | i];
            if (leader->GetExactDist2d(trap.x, trap.y) > TRAP_WATCH)
            {
                state.holdSinceMs = 0;
                continue;
            }
            GameObject* gateA = nullptr;
            GameObject* gateB = nullptr;
            for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
                if (go && go->isSpawned() && go->GetExactDist2d(trap.x, trap.y) <= 15.0f)
                {
                    if (go->GetEntry() == trap.gateA)
                        gateA = go;
                    else if (go->GetEntry() == trap.gateB)
                        gateB = go;
                }
            if (!gateA || !gateB)
                continue;
            // seen shut: it fired (in this run) - resting
            if (gateA->GetGoState() == GO_STATE_READY || gateB->GetGoState() == GO_STATE_READY)
            {
                if (!state.restUntilMs || int32(now - state.restUntilMs) >= 0)
                    LogEvent(party, "trap_fired", JsonObject().Str("note", trap.note)
                                                      .Num("restSec", GATE_TRAP_REST_MS / IN_MILLISECONDS));
                state.restUntilMs = now + GATE_TRAP_REST_MS;
                state.holdSinceMs = 0;
                continue;
            }
            if (state.restUntilMs && int32(now - state.restUntilMs) < 0)
                continue;
            if (state.goUntilMs && int32(now - state.goUntilMs) < 0)
                continue;  // gathered: the leader walks over it with everyone inside
            // the alley's axis: from the point toward the gate on the leader's side (the approach)
            GameObject* nearGate = leader->GetExactDist2d(gateA) <= leader->GetExactDist2d(gateB) ? gateA : gateB;
            float ax = nearGate->GetPositionX() - trap.x;
            float ay = nearGate->GetPositionY() - trap.y;
            float const gateDist = std::max(0.1f, std::hypot(ax, ay));
            ax /= gateDist;
            ay /= gateDist;
            auto const inside = [&](WorldObject const* unit)
            {
                float const along = (unit->GetPositionX() - trap.x) * ax + (unit->GetPositionY() - trap.y) * ay;
                return unit->GetExactDist2d(trap.x, trap.y) >= TRAP_INSIDE_MIN && along > 0.0f &&
                       along <= gateDist - TRAP_GATE_MARGIN;
            };
            // Does the leader's walk come within TRAP_HOLD_RANGE (its waypoint's line, or where it stands)?
            bool heading = leader->GetExactDist2d(trap.x, trap.y) <= TRAP_HOLD_RANGE;
            TestRunner::Waypoint waypoint;
            if (!heading && TestRunner::GetWaypoint(party.leader, waypoint))
            {
                float const sx = leader->GetPositionX();
                float const sy = leader->GetPositionY();
                float const dx = waypoint.x - sx;
                float const dy = waypoint.y - sy;
                float const len2 = dx * dx + dy * dy;
                float const share =
                    len2 > 0.0f ? std::clamp(((trap.x - sx) * dx + (trap.y - sy) * dy) / len2, 0.0f, 1.0f) : 0.0f;
                heading = std::hypot(sx + dx * share - trap.x, sy + dy * share - trap.y) <= TRAP_HOLD_RANGE;
            }
            if (!heading && !state.holdSinceMs)
                continue;
            bool anyCombat = false;
            for (Member const& member : party.members)
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    bot && bot->IsAlive() && bot->GetMap() == map && bot->IsInCombat())
                    anyCombat = true;
            if (anyCombat)
            {
                state.holdSinceMs = 0;  // a fight is on: nobody is walked; the gather starts again after it
                continue;
            }
            std::string waitFor;
            bool const send = !state.sentMs || getMSTimeDiff(state.sentMs, now) >= TRAP_SEND_MS;
            uint32 slot = 0;
            for (Member& member : party.members)
            {
                Player* bot = ObjectAccessor::FindPlayer(member.guid);
                if (!bot || bot == leader || !bot->IsAlive() || bot->GetMap() != map)
                    continue;
                if (inside(bot))
                    continue;
                if (send)
                {
                    // on the axis, side by side: 0, +0.9, -0.9, +1.6, -1.6 yd off it
                    static float const offsets[] = {0.0f, 0.9f, -0.9f, 1.6f, -1.6f};
                    float const off = offsets[slot++ % 5];
                    float const x = trap.x + ax * TRAP_SPOT_AXIS - ay * off;
                    float const y = trap.y + ay * TRAP_SPOT_AXIS + ax * off;
                    float const ground =
                        map->GetHeight(bot->GetPhaseMask(), x, y, nearGate->GetPositionZ() + 3.0f, true, 10.0f);
                    SetMemberWaypoint(party, member, bot, x, y, ground > INVALID_HEIGHT ? ground : bot->GetPositionZ());
                    TestRunner::Waypoint spot;
                    if (TestRunner::GetWaypoint(bot->GetGUID(), spot))
                    {
                        spot.gatePass = true;  // walked as asked, not shortened by the idle-mob guard
                        TestRunner::SetWaypoint(bot->GetGUID(), spot);
                    }
                }
                waitFor += (waitFor.empty() ? "" : ",") + member.name +
                           Acore::StringFormat(" ({:.1f} yd)", bot->GetExactDist2d(trap.x, trap.y));
            }
            if (send)
                state.sentMs = now ? now : 1;
            if (!state.holdSinceMs)
                state.holdSinceMs = now ? now : 1;
            uint32 const waitSec = getMSTimeDiff(state.holdSinceMs, now) / IN_MILLISECONDS;
            if (waitFor.empty())
            {
                // everyone inside the band: the leader walks over the point now, the gates drop behind the group
                for (Member& member : party.members)
                    if (member.guid != party.leader && member.hasWaypoint)
                        ClearMemberWaypoint(member);
                state.goUntilMs = now + TRAP_GO_MS;
                state.holdSinceMs = 0;
                party.trapHoldUntilMs = 0;
                party.nextRetargetMs = now;
                LogEvent(party, "trap_gather", JsonObject().Str("note", trap.note).Str("state", "together, in")
                                                   .Num("waitSec", waitSec));
                LOG_INFO("playerbots", "[b51_strat] trap_gather: the group is inside the {} band, {} walks in",
                         trap.note, leader->GetName());
                continue;
            }
            // the leader holds outside TRAP_HOLD_RANGE: no retarget, no walk (Party::trapHoldUntilMs), it stops here
            party.trapHoldUntilMs = now + 2 * IN_MILLISECONDS;
            if (leader->GetExactDist2d(trap.x, trap.y) > TRAP_TRIGGER + 0.5f)
            {
                TestRunner::ClearWaypoint(party.leader);
                if (leader->isMoving() && leader->GetExactDist2d(trap.x, trap.y) <= TRAP_HOLD_RANGE + 4.0f)
                    leader->StopMoving();
            }
            if (!state.logMs || getMSTimeDiff(state.logMs, now) >= TRAP_LOG_MS)
            {
                state.logMs = now ? now : 1;
                LogEvent(party, "trap_gather", JsonObject().Str("note", trap.note).Str("state", "waiting")
                                                   .Str("waitFor", waitFor).Num("waitSec", waitSec)
                                                   .Real("leaderToTrap", leader->GetExactDist2d(trap.x, trap.y)));
                LOG_INFO("playerbots", "[b51_strat] trap_gather waiting for {} ({} s): {} holds before the armed {}",
                         waitFor, waitSec, leader->GetName(), trap.note);
            }
        }
    }

    // [b32_strat_gate] Who is cut off behind a closed door: a living member whose own walk to the leader runs through a
    // closed door within DOOR_CUT_DOOR_RANGE of it (the healer first). While one is, the group holds (the far wait in
    // TestHarnessMgr.cpp: no wait_timeout, no route, no new pull) and the door is keyed again - by the member itself
    // (MemberKeyAtDoor) or by the leader from its side (ReopenKeyDoorHeld: the waiting leader's waypoint is the member,
    // its walk holds at the door). strat-n 20261025-221216-strat-n-5H-2: the gate shut on the healer at ~56 s, the far
    // wait timed out at 188 s ("Rlpriaf(far)", 8-30 yd away behind the gate) and the group walked on without its
    // healer. User: "why did they go on without the healer? This must not happen."
    void TestHarnessMgr::TickDoorCut(Party& party, Player* leader)
    {
        constexpr uint32 DOOR_CUT_CHECK_MS = 2 * IN_MILLISECONDS;
        constexpr float DOOR_CUT_DOOR_RANGE = 40.0f;
        constexpr uint32 DOOR_CUT_OTHER_MAX_MS = 2 * MINUTE * IN_MILLISECONDS;  // a door no key of the group opens
        constexpr uint32 DOOR_CUT_LOG_MS = 10 * IN_MILLISECONDS;
        uint32 const now = getMSTime();
        if (party.doorCutCheckMs && getMSTimeDiff(party.doorCutCheckMs, now) < DOOR_CUT_CHECK_MS)
            return;
        party.doorCutCheckMs = now ? now : 1;
        Map* map = leader ? leader->GetMap() : nullptr;
        auto const clear = [&](char const* why)
        {
            if (!party.doorCutMember.IsEmpty())
                LogEvent(party, "door_cut",
                         JsonObject().Str("state", "end").Str("why", why)
                             .Num("sec", getMSTimeDiff(party.doorCutSinceMs, now) / IN_MILLISECONDS));
            if (!party.doorCutMember.IsEmpty())
                TestRunner::SetDoorCut(party.doorCutMember, false);
            if (!party.doorCutMember.IsEmpty() && party.keyPassDoor.IsEmpty())
                ClearGateWaypoints(party);  // [b33_strat_gate] the cut is over: no walk back to the gate
            party.doorCutMember.Clear();
            party.doorCutDoor.Clear();
            party.doorCutSinceMs = 0;
            party.doorCutHold = false;
        };
        if (!map || map->GetInstanceId() != party.instanceId || !leader->IsAlive())
        {
            clear("leader dead or out");
            return;
        }
        // [b34_gnomer] EventStarterDef::passDoors cut nobody off: the starter's own script opens and seals them, no key
        // of the group does (gnomer-n 20261026-014521-5B: the Cave In 146086 next to Emi "cut" Rtpribg and Rlpalbu,
        // 4.5 yd apart and 9-11 yd from its model, in turn; the leader walked between them, 67 door_cut starts, no
        // escort, stalled). The same rule as door_hold_pass for the leader's own walk.
        auto const passDoor = [&](GameObject const* door)
        {
            for (StarterTrack const& starter : party.starters)
                if (std::find(starter.def->passDoors.begin(), starter.def->passDoors.end(), door->GetEntry()) !=
                    starter.def->passDoors.end())
                    return true;
            return false;
        };
        std::vector<GameObject*> doors;
        for (GameObject* door : ClosedDoors(map, leader))
            if (door->GetExactDist2d(leader) <= DOOR_CUT_DOOR_RANGE && !passDoor(door))
                doors.push_back(door);
        Player* cut = nullptr;
        GameObject* cutDoor = nullptr;
        if (!doors.empty())
            for (Member const& member : party.members)
            {
                Player* bot = ObjectAccessor::FindPlayer(member.guid);
                if (!bot || bot == leader || !bot->IsAlive() || bot->GetMap() != map ||
                    (cut && !PlayerbotAI::IsHeal(bot)))
                    continue;
                // Only a door near the line between them (an ellipse round the two: a cheap test before the path).
                std::vector<GameObject*> nearDoors;
                float const apart2d = bot->GetExactDist2d(leader);
                for (GameObject* door : doors)
                    if (door->GetExactDist2d(bot) <= DOOR_CUT_DOOR_RANGE &&
                        door->GetExactDist2d(bot) + door->GetExactDist2d(leader) <= apart2d + 20.0f)
                        nearDoors.push_back(door);
                if (nearDoors.empty())
                    continue;
                PathResult const way =
                    RoutePath(bot, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
                if (!way.reachable)
                    continue;
                if (GameObject* door = RouteThroughClosedDoor(way.points, nearDoors, leader->GetPhaseMask()))
                {
                    cut = bot;
                    cutDoor = door;
                    if (PlayerbotAI::IsHeal(bot))
                        break;
                }
            }
        if (!cut)
        {
            clear("together");
            return;
        }
        // A door some member's key opens (an item starter of this content on it, the item in someone's bags).
        bool keyDoor = false;
        for (StarterTrack const& starter : party.starters)
            if (starter.def->kind == StarterKind::UseItem && starter.def->itemId &&
                std::find(starter.def->entries.begin(), starter.def->entries.end(), cutDoor->GetEntry()) !=
                    starter.def->entries.end())
                for (Member const& member : party.members)
                    if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                        bot && bot->IsAlive() && bot->HasItemCount(starter.def->itemId, 1))
                        keyDoor = true;
        // [b34_ss] A door a click opens (TerrainHazard.h DoorOpensByUse: Scholomance's Iron Gates) is opened as well -
        // by the member from its side (MemberKeyAtDoor): held for, not walked away from after DOOR_CUT_OTHER_MAX_MS.
        // scholo-n 20261026-014521-5E-4: the healer behind the Iron Gate 175615 at 2.1 yd from 1942 s; after the tank's
        // corpse run the hold ended and its walk round to the tank went through the bone pits (deaths at 2319-2337 s).
        bool const clickDoor = !keyDoor && !DoorEventOf(cutDoor) && DoorOpensByUse(cutDoor);
        keyDoor = keyDoor || clickDoor;
        bool const fresh = party.doorCutMember != cut->GetGUID() || party.doorCutDoor != cutDoor->GetGUID();
        if (fresh)
        {
            if (!party.doorCutMember.IsEmpty())
                TestRunner::SetDoorCut(party.doorCutMember, false);
            // [b34_gnomer] the DOOR_CUT_OTHER_MAX_MS cap runs per door: two members cut by the same door in turn
            // restarted it on every switch, and the hold never ended (gnomer-n 014521: "start", sec 0, x67).
            if (party.doorCutDoor != cutDoor->GetGUID() || !party.doorCutSinceMs)
                party.doorCutSinceMs = now ? now : 1;
            party.doorCutMember = cut->GetGUID();
            party.doorCutDoor = cutDoor->GetGUID();
            party.doorCutLogMs = 0;
        }
        party.doorCutHold = keyDoor || getMSTimeDiff(party.doorCutSinceMs, now) < DOOR_CUT_OTHER_MAX_MS;
        TestRunner::SetDoorCut(cut->GetGUID(), party.doorCutHold);  // the tank's pull gate (ChooseTargetActions)
        if (fresh || !party.doorCutLogMs || getMSTimeDiff(party.doorCutLogMs, now) >= DOOR_CUT_LOG_MS)
        {
            party.doorCutLogMs = now ? now : 1;
            LogEvent(party, "door_cut", JsonObject().Str("state", fresh ? "start" : "holding")
                                            .Str("member", cut->GetName()).Bool("healer", PlayerbotAI::IsHeal(cut))
                                            .Str("door", cutDoor->GetName()).Num("doorEntry", cutDoor->GetEntry())
                                            .Bool("keyDoor", keyDoor).Bool("hold", party.doorCutHold)
                                            .Real("memberToDoor", cut->GetExactDist(cutDoor))
                                            .Real("leaderToDoor", leader->GetExactDist(cutDoor))
                                            .Bool("leaderInCombat", leader->IsInCombat())
                                            .Num("sec", getMSTimeDiff(party.doorCutSinceMs, now) /
                                                             IN_MILLISECONDS));
        }
    }

    // [b32_strat_gate] Recovery, as a player does it: the member shut out uses its own key on the door from its side
    // (Key to the City: every member carries one, EventItemDef::everyMember). The tank inside is in the fight - the
    // key's 5 s Opening breaks on the hits it takes (ReopenKeyDoorInCombat waits for "nothing hits the leader") - while
    // the member outside stands at the gate untouched: its cast is the one that goes through, in a fight too. Once the
    // gate opens TickKeyDoorPass sends it through (the far point, a gatePass waypoint).
    void TestHarnessMgr::MemberKeyAtDoor(Party& party, Player* leader)
    {
        constexpr uint32 MEMBER_KEY_GAP_MS = 7 * IN_MILLISECONDS;  // the 5 s cast and the walk through, then again
        constexpr uint32 MEMBER_KEY_MAX = 12;
        // [b34_ss] past MEMBER_KEY_MAX (one budget for the whole run, every gate) the member keys again this seldom,
        // never not at all: with DOOR_REARM_MAX spent as well, a key door's hold (TickDoorCut keyDoor) had nobody left
        // to open it - strat-n 20261026-014521-5C-2, uses 12/12 and re-arms 6/6 at 2121 s, the healer 0.7 yd from
        // King's Square Gate 175353 and the group held 230 s until the stop.
        // [b51_strat] no slow-down past the budget (user 2026-10-26: open it at once, as often as needed)
        constexpr uint32 MEMBER_KEY_SLOW_GAP_MS = MEMBER_KEY_GAP_MS;
        constexpr float MEMBER_KEY_CAST_RANGE = 4.5f;  // Opening 3366: 5 yd to the object's centre
        constexpr float MEMBER_KEY_STEP = 3.0f;        // the step in: this far from the centre, on the member's side
        uint32 const now = getMSTime();
        if (party.doorCutMember.IsEmpty() || !party.keyPassDoor.IsEmpty() || !leader ||
            (party.memberKeyMs &&
             getMSTimeDiff(party.memberKeyMs, now) <
                 (party.memberKeyUses >= MEMBER_KEY_MAX ? MEMBER_KEY_SLOW_GAP_MS : MEMBER_KEY_GAP_MS)))
            return;
        Map* map = leader->GetMap();
        Player* bot = ObjectAccessor::FindPlayer(party.doorCutMember);
        GameObject* door = map ? map->GetGameObject(party.doorCutDoor) : nullptr;
        if (!door || door->GetGoState() != GO_STATE_READY)
            return;
        auto const canKey = [&](Player const* each)
        {
            return each && each->GetMap() == map && each->IsAlive() && !each->IsNonMeleeSpellCast(false) &&
                   each->getAttackers().empty();
        };
        // [b35_ss] The cut member under attack or casting cannot key it: any other member untouched at the gate,
        // either side, with a key does - in a fight too (its cast is not broken). strat-n 20261026-024524-5E-2: the
        // tank shut out on the near side and hit through the bars, the healer on the far side under attack, nobody
        // keyed it for 63 s (member_key_step only) while Rlhunap stood 3.3 yd from the gate; the tank died at 155 s.
        if (!canKey(bot))
        {
            bot = nullptr;
            for (Member const& each : party.members)
            {
                Player* other = ObjectAccessor::FindPlayer(each.guid);
                if (!canKey(other) || other->GetExactDist(door) > MEMBER_KEY_CAST_RANGE ||
                    !other->IsWithinLOS(door->GetPositionX(), door->GetPositionY(), door->GetPositionZ() + 1.5f,
                                        VMAP::ModelIgnoreFlags::M2, LINEOFSIGHT_CHECK_VMAP))
                    continue;
                for (StarterTrack const& track : party.starters)
                    if (track.def->kind == StarterKind::UseItem && track.def->itemId &&
                        other->HasItemCount(track.def->itemId, 1) &&
                        std::find(track.def->entries.begin(), track.def->entries.end(), door->GetEntry()) !=
                            track.def->entries.end())
                        bot = other;
                if (bot)
                    break;
            }
            if (!bot)
                return;
        }
        StarterTrack* starter = nullptr;
        for (StarterTrack& track : party.starters)
            if (track.def->kind == StarterKind::UseItem && track.def->itemId &&
                bot->HasItemCount(track.def->itemId, 1) &&
                std::find(track.def->entries.begin(), track.def->entries.end(), door->GetEntry()) !=
                    track.def->entries.end())
                starter = &track;
        Member* member = nullptr;
        for (Member& each : party.members)
            if (each.guid == bot->GetGUID())
                member = &each;
        // [b34_ss] or a door a click opens (TickDoorCut clickDoor): the member clicks it from its side, as a player
        // does - out of combat, nobody of the group fighting (OpenDoorByUse).
        bool const click = !starter && !DoorEventOf(door) && DoorOpensByUse(door);
        if ((!starter && !click) || !member)
            return;
        float const distance = bot->GetExactDist(door);
        bool const los = bot->IsWithinLOS(door->GetPositionX(), door->GetPositionY(), door->GetPositionZ() + 1.5f,
                                          VMAP::ModelIgnoreFlags::M2, LINEOFSIGHT_CHECK_VMAP);
        float const reach =
            click ? std::max(MEMBER_KEY_STEP + 0.5f, door->GetInteractionDistance() - 0.5f) : MEMBER_KEY_CAST_RANGE;
        // [b48_key] a key's cast in the core's own range too (GameObject::IsAtInteractDistance with the item's spell:
        // the door's model box grown by its range), as the leader's use (UseStarterItem). strat-h
        // 20261026-024524-strat-h-5C-1: Rtpriaa 4.6 yd from King's Square Gate, in the core's range, refused by the
        // 4.5 yd: member_key_step x8 (1315-1525 s), its step point (3 yd from the centre) within the runner's reach.
        bool inReach = distance <= reach;
        if (!click && !inReach)
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(starter->def->itemId))
                for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS && !inReach; ++i)
                    if (proto->Spells[i].SpellId > 0 && proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                        if (SpellInfo const* keySpell = sSpellMgr->GetSpellInfo(uint32(proto->Spells[i].SpellId)))
                            inReach = door->IsAtInteractDistance(bot, keySpell);
        if (click && inReach && los)
        {
            if (bot->IsInCombat())
                return;
            bool const opened = OpenDoorByUse(bot, door);
            ++party.memberKeyUses;
            party.memberKeyMs = now ? now : 1;
            LogEvent(party, "member_door_use", JsonObject().Str("name", bot->GetName()).Str("door", door->GetName())
                                                   .Num("doorEntry", door->GetEntry()).Real("distance", distance)
                                                   .Bool("opened", opened)
                                                   .Bool("leaderInCombat", leader->IsInCombat())
                                                   .Num("uses", party.memberKeyUses));
            return;
        }
        if (!inReach || !los)
        {
            // A step to the door on its own side (never through it): MEMBER_KEY_STEP yd from its centre toward the bot.
            float const dx = bot->GetPositionX() - door->GetPositionX();
            float const dy = bot->GetPositionY() - door->GetPositionY();
            float const len = std::max(0.1f, std::hypot(dx, dy));
            float const x = door->GetPositionX() + dx / len * MEMBER_KEY_STEP;
            float const y = door->GetPositionY() + dy / len * MEMBER_KEY_STEP;
            float const ground = map->GetHeight(bot->GetPhaseMask(), x, y, door->GetPositionZ() + 3.0f, true, 10.0f);
            // [b39_strat] in a fight too, while nothing hits the member (canKey): strat-h 054314-5C-1, the healer shut
            // out at 12.6 yd in combat with no attacker logged member_key_step x9 (152-208 s) and never stepped in,
            // nobody keyed the gate, the tank died behind it.
            if (!bot->IsInCombat() || bot->getAttackers().empty())
            {
                SetMemberWaypoint(party, *member, bot, x, y, ground > INVALID_HEIGHT ? ground : bot->GetPositionZ());
                MarkGateWaypoint(bot);  // [b33_strat_gate] cleared with the cut (ClearGateWaypoints)
            }
            party.memberKeyMs = now ? now : 1;  // the step's time, then the cast
            LogEvent(party, "member_key_step", JsonObject().Str("name", bot->GetName()).Str("door", door->GetName())
                                                   .Real("distance", distance).Bool("los", los)
                                                   .Bool("inCombat", bot->IsInCombat()));
            return;
        }
        Item* item = bot->GetItemByEntry(starter->def->itemId);
        uint32 spellId = 0;
        if (item)
            for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS && !spellId; ++i)
                if (item->GetTemplate()->Spells[i].SpellId > 0 &&
                    item->GetTemplate()->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                    spellId = uint32(item->GetTemplate()->Spells[i].SpellId);
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!item || !info || bot->CanUseItem(item) != EQUIP_ERR_OK)
            return;
        if (ShapeshiftForm const form = bot->GetShapeshiftForm();
            form != FORM_NONE && info->CheckShapeshift(form) != SPELL_CAST_OK)
            bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
        TestRunner::ClearWaypoint(bot->GetGUID());
        bot->StopMoving();
        bot->SetFacingToObject(door);
        // As the client sends it (UseStarterItem): the door as the target when the spell takes one.
        WorldPacket packet(CMSG_USE_ITEM);
        packet << item->GetBagSlot() << item->GetSlot() << uint8(1) << spellId << item->GetGUID() << uint32(0)
               << uint8(0);
        if (info->GetExplicitTargetMask() & TARGET_FLAG_GAMEOBJECT_MASK)
            packet << uint32(TARGET_FLAG_GAMEOBJECT) << door->GetPackGUID();
        else
            packet << uint32(TARGET_FLAG_NONE);
        bot->GetSession()->HandleUseItemOpcode(packet);
        bool const casting = bot->IsNonMeleeSpellCast(false);
        ++party.memberKeyUses;
        party.memberKeyMs = now ? now : 1;
        LogEvent(party, "member_key", JsonObject().Str("name", bot->GetName()).Str("door", door->GetName())
                                          .Num("doorEntry", door->GetEntry()).Num("spell", spellId)
                                          .Real("distance", distance).Bool("casting", casting)
                                          .Bool("inCombat", bot->IsInCombat())
                                          .Bool("leaderInCombat", leader->IsInCombat())
                                          .Num("uses", party.memberKeyUses));
        if (casting && KeyDoorPass(door->GetMapId(), door->GetEntry()))
        {
            // This opening's pass (TickKeyDoorPass): the members on the near side go through to the far point.
            party.keyPassDoor = door->GetGUID();
            party.keyPassSinceMs = now ? now : 1;
            party.keyPassSent = false;
            party.keyPassOpened = false;
            party.keyPassCaster = bot->GetGUID();
        }
    }

    void TestHarnessMgr::ReopenKeyDoorHeld(Party& party, Player* leader)
    {
        Map* map = leader ? leader->GetMap() : nullptr;
        if (!map || map->GetInstanceId() != party.instanceId || !leader->IsAlive())
            return;
        uint32 const now = getMSTime();
        for (StarterTrack& starter : party.starters)
        {
            if (starter.def->kind != StarterKind::UseItem)
                continue;
            // Not used yet: only the door the route targets as this starter (its first use, no re-arm).
            bool const firstUse = starter.uses < starter.def->maxUses;
            // [b32_strat_gate] a re-armed key (doorRearms: ReopenKeyDoorInCombat armed it for a member behind the door)
            // is no first use: the leader holding at the door for the cut-off member (the far wait's door_hold) keys
            // it whatever the route targets - strat-n 221216: armed at 92 s, never cast, the group went on.
            if (firstUse && !starter.doorRearms &&
                (!party.targetIsStarter || party.starterIdx >= party.starters.size() ||
                 &party.starters[party.starterIdx] != &starter || int32(now - starter.nextAllowedMs) < 0))
                continue;
            GameObject* door = nullptr;
            for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
                if (go && go->isSpawned() && go->GetGoType() == GAMEOBJECT_TYPE_DOOR &&
                    go->GetGoState() == GO_STATE_READY && go->IsWithinDistInMap(leader, KEY_DOOR_HELD_RANGE) &&
                    std::find(starter.def->entries.begin(), starter.def->entries.end(), go->GetEntry()) !=
                        starter.def->entries.end())
                    door = go;
            if (!door)
                continue;

            std::string reason;
            // [queue-wing-walk-rekey] a wing walk is the bots' own portal walk (no harness waypoint, so no door_hold):
            // standing at the key door it is held there as well. strat-n 20261025-115805-strat-n-5A-6: the walk to
            // entrance 1 from 999 s, the leader 4.2 yd from the closed Service Entrance Gate 175368, "no walk to the
            // portal" for every member and rearm_check "no door_hold" every 15 s until the stop at 1083 s.
            bool const wingWalkHeld = party.wingWalk && !leader->isMoving() &&
                                      leader->GetExactDist(door) <= KEY_DOOR_HELD_RANGE;
            // [b36_ss] Any hold of the leader at this door counts, not only the live door_hold: the retarget clears
            // Party::doorHold (ROUTE_CLOSED_DOORS_ALL) and skips its target "leader held at a closed door (<gate>)",
            // so between two door_holds the re-key saw "no door_hold". strat-n 20261026-034959-strat-n-5A-2: from
            // ~2129 s the leader stood 3.4 yd from King's Square Gate 175353, door_hold every 5 s and skip_target
            // "leader held at a closed door (King's Square Gate 175353)" for every target, while rearm_check said
            // "no door_hold" (and "the whole group is through it") every 15 s to the stop.
            constexpr uint32 KEY_DOOR_HOLD_RECENT_MS = 10 * IN_MILLISECONDS;
            constexpr uint32 KEY_DOOR_WALK_BACK_MS = 30 * IN_MILLISECONDS;
            bool skipHeld = false;
            std::string const heldTag = Acore::StringFormat("leader held at a closed door ({} {})", door->GetName(),
                                                            door->GetEntry());
            for (auto const& [guid, why] : party.skipReason)
                if (why.find(heldTag) != std::string::npos)
                {
                    skipHeld = true;
                    break;
                }
            bool const atDoor = !leader->isMoving() && leader->GetExactDist(door) <= KEY_DOOR_HELD_RANGE;
            bool const recentHold = party.doorHoldMs && getMSTimeDiff(party.doorHoldMs, now) <= KEY_DOOR_HOLD_RECENT_MS;
            // [b51_strat] the leader waiting on its own side of the door a member is stranded behind (the far wait's
            // door_reopen_for walk, which no longer runs through the door, so it makes no door_hold)
            bool const reopenHeld = atDoor && party.doorCutHold && party.doorCutDoor == door->GetGUID();
            bool const held = party.doorHold == door->GetGUID() || wingWalkHeld || reopenHeld ||
                              (atDoor && (skipHeld || recentHold));
            // [b36_ss] Held this long with the whole group through: the walk back IS what the route wants (every
            // target behind the gate, 5A-2: "fallback (nothing on the route)" targets all behind 175353, wing walk
            // done at 2129 s) - the re-key goes ahead, and TickKeyDoorPass sends the far side back (its pass back).
            static std::unordered_map<uint64, uint32> heldSinceMs;  // the harness's world thread only
            uint64 const heldKey = (uint64(party.instanceId) << 32) | uint32(door->GetSpawnId());
            if (!held)
                heldSinceMs.erase(heldKey);
            else if (heldSinceMs.size() > 256)
                heldSinceMs.clear();
            if (held && !heldSinceMs.count(heldKey))
                heldSinceMs[heldKey] = now ? now : 1;
            bool const longHeld = held && getMSTimeDiff(heldSinceMs[heldKey], now) >= KEY_DOOR_WALK_BACK_MS;
            if (!held)
                reason = "the leader's walk does not hold at it (no door_hold)";
            else if ((!firstUse || starter.doorRearms) && !longHeld && GroupThroughKeyGate(party, door))
                reason = "the whole group is through it (a walk back to what stands behind it)";
            else if (!firstUse && starter.doorRearms >= DOOR_REARM_MAX)
                reason = Acore::StringFormat("re-arms used up ({}/{})", starter.doorRearms, DOOR_REARM_MAX);
            else if (!firstUse && party.lastStarterUseMs &&
                     getMSTimeDiff(party.lastStarterUseMs, now) <= DOOR_REARM_GAP_MS)
                reason = "last use too recent";
            else if (leader->IsInCombat())
                reason = "leader in combat";
            else if (leader->IsNonMeleeSpellCast(false))
                reason = "leader casting";
            // [b34_ss] the member it is opened for cannot walk the 3 s it stands open: casting (a resurrection, the
            // after-death buffs), in a fight or sitting to drink - a re-arm spent for nothing. strat-n
            // 20261026-014521-5C-2: six re-arms at King's Square Gate 175353 (2068-2117 s) while the healer rezzed
            // and buffed, none walked, "re-arms used up (6/6)".
            else if (Player* cut = party.doorCutMember.IsEmpty() ? nullptr
                                                                 : ObjectAccessor::FindPlayer(party.doorCutMember);
                     cut && cut != leader && cut->IsAlive() &&
                     (cut->IsNonMeleeSpellCast(false) || cut->IsInCombat() || cut->IsSitState()))
                reason = Acore::StringFormat("{} behind it is busy ({})", cut->GetName(),
                                             cut->IsNonMeleeSpellCast(false) ? "casting"
                                             : cut->IsInCombat()             ? "in combat"
                                                                             : "sitting");
            else if (!leader->HasItemCount(starter.def->itemId, 1))
            {
                std::string holders;
                for (Member const& member : party.members)
                    if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                        bot && bot->HasItemCount(starter.def->itemId, 1))
                        holders += (holders.empty() ? "" : ",") + member.name;
                reason = Acore::StringFormat("leader has no key {} (carried by: {})", starter.def->itemId,
                                             holders.empty() ? "nobody" : holders);
            }
            if (!reason.empty())
            {
                if (!party.rearmCheckLogMs || getMSTimeDiff(party.rearmCheckLogMs, now) >= REARM_CHECK_LOG_MS)
                {
                    party.rearmCheckLogMs = now ? now : 1;
                    LogEvent(party, "rearm_check", JsonObject().Str("note", starter.def->note)
                                                       .Str("door", door->GetName()).Num("doorEntry", door->GetEntry())
                                                       .Real("distance", leader->GetExactDist(door))
                                                       .Num("uses", starter.uses).Num("rearms", starter.doorRearms)
                                                       .Str("reason", reason));
                    LOG_INFO("playerbots", "[closed door] {}: no re-key of {} ({:.1f} yd): {}", leader->GetName(),
                             door->GetName(), leader->GetExactDist(door), reason);
                }
                continue;
            }

            if (!firstUse)
            {
                ++starter.doorRearms;
                starter.uses = 0;
                starter.usedObjects.clear();
                LogEvent(party, "starter_rearmed", JsonObject().Str("note", starter.def->note)
                                                       .Str("why", "the door closed again, the leader holds at it")
                                                       .Real("distance", leader->GetExactDist(door))
                                                       .Num("rearms", starter.doorRearms).Bool("inCombat", false));
            }
            UseStarter(party, leader, starter, door);
            party.lastStarterUseMs = now;
            return;
        }
    }

    bool TestHarnessMgr::UseStarter(Party& party, Player* leader, StarterTrack& starter, WorldObject* object)
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(*this, party);
        return Leader::Travel::UseStarter(group, leader, starter, object);
    }

    bool TestHarnessMgr::UseStarterAtNpc(Party& party, Player* leader, StarterTrack& starter, Creature* creature)
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(*this, party);
        return Leader::Travel::UseStarterAtNpc(group, leader, starter, creature);
    }

    bool TestHarnessMgr::UseStarterItem(Party& party, Player* leader, StarterTrack& starter, GameObject* go)
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(*this, party);
        return Leader::Travel::UseStarterItem(group, leader, starter, go);
    }

    bool TestHarnessMgr::CanOpenLock(Party const& party, GameObject const* go) const
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(const_cast<TestHarnessMgr&>(*this), const_cast<Party&>(party));
        return Leader::Travel::CanOpenLock(group, go);
    }

    bool TestHarnessMgr::EntriesDead(Party const& party, std::vector<uint32> const& entries) const
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(const_cast<TestHarnessMgr&>(*this), const_cast<Party&>(party));
        return Leader::Travel::EntriesDead(group, entries);
    }

    Creature* TestHarnessMgr::ActiveEscort(Party& party, Map* map)
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(*this, party);
        return Leader::Travel::ActiveEscort(group, map);
    }

    bool TestHarnessMgr::StayWithEscort(Party& party, Player* leader)
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(*this, party);
        return Leader::Travel::StayWithEscort(group, leader);
    }

    int32 TestHarnessMgr::EventSpotIndex(Party const& party) const
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(const_cast<TestHarnessMgr&>(*this), const_cast<Party&>(party));
        return Leader::Travel::EventSpotIndex(group);
    }

    bool TestHarnessMgr::WalkToEventSpot(Party& party, Player* leader)
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(*this, party);
        return Leader::Travel::WalkToEventSpot(group, leader);
    }

    bool TestHarnessMgr::LinkUsable(Party const& party, Map* map, RouteLinkDef const& link) const
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(const_cast<TestHarnessMgr&>(*this), const_cast<Party&>(party));
        return Leader::Travel::LinkUsable(group, map, link);
    }

    int32 TestHarnessMgr::LinkTo(Party& party, Player* leader, WorldObject* target)
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(*this, party);
        return Leader::Travel::LinkTo(group, leader, target);
    }

    void TestHarnessMgr::EndLink(Party& party, char const* reason)
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(*this, party);
        Leader::Travel::EndLink(group, reason);
    }

    bool TestHarnessMgr::WalkToLink(Party& party, Player* leader)
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(*this, party);
        return Leader::Travel::WalkToLink(group, leader);
    }

    void TestHarnessMgr::CrossLinks(Party& party, std::vector<Player*> const& players, Player* leader)
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(*this, party);
        Leader::Travel::CrossLinks(group, players, leader);
    }

    bool TestHarnessMgr::AssistMember(Party& party, std::vector<Player*> const& players, Player* leader)
    {
        // Sunken Temple heroic wave 8: the tank stood idle at the pool floor (z -172.9) for 20 minutes while the
        // healer and the mage, back from their corpse runs, fought on the gallery above (z -148.8) and died again and
        // again; Maraudon heroic: members killed 150+ yd from the tank. The bot's "aggressive target" leaves mobs
        // with |dz| > 5 alone, so the tank never came. A tank runs to the member who has aggro.
        if (!leader || !leader->IsAlive() || leader->IsInCombat() || party.wiped || party.linkIdx >= 0)
        {
            party.assisting.Clear();
            return false;
        }

        Player* member = nullptr;
        float best = 0.0f;
        for (Player* bot : players)
        {
            if (bot == leader || !bot->IsAlive() || bot->GetMap() != leader->GetMap() || !bot->IsInCombat() ||
                bot->getAttackers().empty())
                continue;
            float const distance = leader->GetExactDist(bot);
            bool const away = distance > ASSIST_RANGE ||
                              std::fabs(bot->GetPositionZ() - leader->GetPositionZ()) > ASSIST_FLOOR_DZ;
            if (away && (!member || distance < best))
            {
                member = bot;
                best = distance;
            }
        }
        if (!member)
        {
            party.assisting.Clear();
            return false;
        }

        PathResult leg;
        G3D::Vector3 const goal(member->GetPositionX(), member->GetPositionY(), member->GetPositionZ());
        SteerLeg(leader, goal, leg);
        if (leg.type & PATHFIND_NOPATH)
            return false;
        G3D::Vector3 const waypoint =
            FloorWaypoint(leader, leg, RouteWaypoint(leg, goal, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP));

        if (party.assisting != member->GetGUID())
        {
            party.assisting = member->GetGUID();
            Unit* attacker = *member->getAttackers().begin();
            LogEvent(party, "assist_member", JsonObject().Str("name", member->GetName())
                                                 .Str("attacker", attacker ? attacker->GetName() : "")
                                                 .Real("distance", best)
                                                 .Real("dz", member->GetPositionZ() - leader->GetPositionZ())
                                                 .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                                                 .Real("z", leader->GetPositionZ()));
        }

        SetLeaderWaypoint(party, leader, waypoint.x, waypoint.y, waypoint.z);
        party.lastProgressMs = getMSTime();
        return true;
    }

    bool TestHarnessMgr::MemberStranded(Party& party, Player* bot, Player* leader)
    {
        Member* member = nullptr;
        for (Member& candidate : party.members)
            if (candidate.guid == bot->GetGUID())
                member = &candidate;
        if (!member)
            return false;
        // Within MEET_RANGE only a member on another level (the ping-pong hold counts it as apart from 30 yd: ok-n
        // wave 8, two members 41 yd away, dz -8, 279 s of holds) is checked.
        bool const otherLevel = std::fabs(bot->GetPositionZ() - leader->GetPositionZ()) > STRANDED_LEVEL_DZ;
        if (!leader || bot == leader || !bot->IsAlive() || !leader->IsAlive() || bot->GetMap() != leader->GetMap() ||
            bot->IsInCombat() || bot->IsBeingTeleported() ||
            bot->GetExactDist(leader) <= (otherLevel ? STRANDED_LEVEL_RANGE : MEET_RANGE))
        {
            member->stranded = false;
            member->strandedSinceMs = 0;
            return false;
        }
        uint32 const now = getMSTime();
        if (member->strandedCheckMs && getMSTimeDiff(member->strandedCheckMs, now) < STRANDED_RECHECK_MS)
            return member->stranded;
        member->strandedCheckMs = now ? now : 1;

        G3D::Vector3 const leaderPos(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
        G3D::Vector3 const botPos(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        PathResult const out = RoutePath(bot, leaderPos.x, leaderPos.y, leaderPos.z);
        PathResult const back = RoutePath(leader, botPos.x, botPos.y, botPos.z);
        bool const noWalk = !out.complete && !back.complete &&
                            ((out.type & PATHFIND_NOPATH) ||
                             (out.end - leaderPos).length() > (otherLevel ? STRANDED_LEVEL_RANGE : MEET_RANGE));
        if (!noWalk)
        {
            if (member->stranded)
                LogEvent(party, "member_unstranded", JsonObject().Str("name", bot->GetName()));
            member->stranded = false;
            member->strandedSinceMs = 0;
            return false;
        }
        if (!member->strandedSinceMs || bot->GetExactDist(&member->strandedPos) > STRANDED_STILL_RANGE)
        {
            member->strandedSinceMs = now ? now : 1;
            member->strandedPos = bot->GetPosition();
            member->stranded = false;
            return false;
        }
        if (!member->stranded && getMSTimeDiff(member->strandedSinceMs, now) >= STRANDED_AFTER_MS)
        {
            member->stranded = true;
            LogCannotMove(party, bot, "meet", "stranded: no navmesh walk to the leader either way, not waited for",
                          leaderPos.x, leaderPos.y, leaderPos.z);
            LogEvent(party, "member_stranded", JsonObject().Str("name", bot->GetName())
                                                   .Real("distance", bot->GetExactDist(leader))
                                                   .Real("x", botPos.x).Real("y", botPos.y).Real("z", botPos.z));
        }
        return member->stranded;
    }

    // [b30_reenter] Every re-entry is a death (user 2026-10-25: «перезаход это считается как смерть, понимаешь? А
    // смерти мы все должны анализировать»): the death recorder's deaths.jsonl record (killer `killer`, "reentry":
    // `context` + the stuck watchdog's state + the member's last intention with its movement source) and the run's
    // death counters (run_end deaths, the clean-pass verdict, the "death" event). Called before the teleport: the
    // record's position is where the member stood.
    static void RecordReentryDeath(Player* bot, std::string const& killer, JsonObject context)
    {
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            if (auto* watchdog = dynamic_cast<StuckWatchdogValue*>(
                    botAI->GetAiObjectContext()->GetUntypedValue("stuck watchdog")))
                context.Raw("watchdog", JsonObject().Num("stuckSec", watchdog->GetStuckSeconds())
                                            .Num("stillSec", watchdog->GetStillSeconds())
                                            .Num("trailTries", watchdog->GetTrailTries()).Build());
        context.Raw("intent", MemberIntentJson(bot->GetGUID()));
        DeathRecorder::WriteReentry(bot, killer, context.Build());
        StatsRegistry::Instance().RecordReentry(bot, killer);
    }

    // [b30_topped_up] A member with no walk back to the group leaves the instance and comes back in at its entrance,
    // counted as a death (user rule 2026-10-25). dmw-n 20261025-184229-dmw-n-5C-16: Immol'thar's portal put the healer
    // on a perch over his spawn, a follow later glided it into the pit (a navmesh region of 186 polygons joined to the
    // rest only by the one-way route link down), StuckWatchdog "needs path fix" for 6 minutes, the group held until
    // the run was stopped. The leader's reference is its nearest living member. The landing is the entrance the
    // group came in by (party.entranceIdx), only when a walk from there reaches the group: no content is skipped.
    bool TestHarnessMgr::TickStrandedReenter(Party& party, Member& member, Player* bot, Player* leader)
    {
        auto const reset = [&member]()
        {
            member.reenterSinceMs = 0;
            member.reenterWatchdogSec = 0;
        };

        Player* reference = bot != leader ? leader : nullptr;
        if (bot == leader)
            for (Member const& other : party.members)
                if (Player* candidate = ObjectAccessor::FindPlayer(other.guid);
                    candidate && candidate != bot && candidate->IsAlive() && candidate->GetMap() == bot->GetMap() &&
                    (!reference || bot->GetExactDist(candidate) < bot->GetExactDist(reference)))
                    reference = candidate;

        Map* map = bot->GetMap();
        if (!reference || !leader || !bot->IsAlive() || !reference->IsAlive() || !map || !map->IsDungeon() ||
            reference->GetMap() != map || bot->IsInCombat() || reference->IsInCombat() || bot->IsBeingTeleported() ||
            bot->GetTransport() || bot->GetVehicle() || bot->IsFlying() || reference->GetTransport() ||
            reference->GetVehicle() || reference->IsFlying() ||  // a lift, a drake (Oculus): no walk is the plan
            party.linkIdx >= 0 || !party.escort.IsEmpty() ||
            party.wingWalk || member.hasPortalGoal || party.entrances.empty() ||
            party.entranceIdx >= party.entrances.size() || bot->GetExactDist(reference) <= MEET_RANGE ||
            member.strandedReentries >= STRANDED_REENTER_MAX)
        {
            reset();
            return false;
        }

        uint32 now = getMSTime();
        now = now ? now : 1;
        if (member.reenterCheckMs && getMSTimeDiff(member.reenterCheckMs, now) < STRANDED_RECHECK_MS)
            return false;
        member.reenterCheckMs = now;

        PathResult const out = RoutePath(bot, reference->GetPositionX(), reference->GetPositionY(),
                                         reference->GetPositionZ());
        if (out.complete)
        {
            reset();
            return false;
        }

        if (!member.reenterSinceMs || bot->GetExactDist(&member.reenterPos) > STRANDED_STILL_RANGE)
        {
            reset();
            member.reenterSinceMs = now;
            member.reenterPos = bot->GetPosition();
            return false;
        }

        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            if (auto* watchdog = dynamic_cast<StuckWatchdogValue*>(
                    botAI->GetAiObjectContext()->GetUntypedValue("stuck watchdog")))
                member.reenterWatchdogSec = std::max(member.reenterWatchdogSec, watchdog->GetStuckSeconds());

        uint32 const noWalkMs = getMSTimeDiff(member.reenterSinceMs, now);
        if (noWalkMs < STRANDED_REENTER_MS || member.reenterWatchdogSec < STRANDED_REENTER_WATCHDOG_S)
            return false;

        Position const& entrance = party.entrances[party.entranceIdx];
        G3D::Vector3 const entranceAt(entrance.GetPositionX(), entrance.GetPositionY(), entrance.GetPositionZ());
        PathResult const back = RoutePath(bot, reference->GetPositionX(), reference->GetPositionY(),
                                          reference->GetPositionZ(), &entranceAt);
        if (!back.complete)
        {
            // re-entering would not help: no walk from the entrance either (kept as stranded, logged once)
            if (member.reenterWatchdogSec != UINT32_MAX)
                LogEvent(party, "stranded_reenter",
                         JsonObject().Str("name", bot->GetName()).Str("state", "refused")
                             .Str("why", "no walk from the entrance to the group either")
                             .Real("x", bot->GetPositionX()).Real("y", bot->GetPositionY())
                             .Real("z", bot->GetPositionZ()));
            member.reenterWatchdogSec = UINT32_MAX;  // logged; the watch restarts when the member moves
            return false;
        }

        G3D::Vector3 const from(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        float const distance = bot->GetExactDist(reference);
        RecordReentryDeath(bot, "stranded_reenter: no path back, re-entered the instance",
                           JsonObject().Str("kind", "stranded").Str("goal", reference->GetName())
                               .Real("distance", distance).Num("noWalkSec", noWalkMs / IN_MILLISECONDS)
                               .Real("walk", -1.0f).Real("entranceWalk", back.length)
                               .Num("entrance", int64(party.entranceIdx)).Real("toX", entrance.GetPositionX())
                               .Real("toY", entrance.GetPositionY()).Real("toZ", entrance.GetPositionZ()));
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        if (!bot->TeleportTo(map->GetId(), entrance.GetPositionX(), entrance.GetPositionY(), entrance.GetPositionZ(),
                             entrance.GetOrientation()))
        {
            LogEvent(party, "stranded_reenter",
                     JsonObject().Str("name", bot->GetName()).Str("state", "teleport refused (death recorded)"));
            reset();
            return false;
        }

        ++member.strandedReentries;
        member.stranded = false;
        member.strandedSinceMs = 0;
        member.apartIgnoreUntilMs = 0;
        member.farTimeouts = 0;
        // [b32_sh_ooze] The walk from the entrance crosses a respawn zone: the member counts as back in through the
        // portal (Member::backInMs), so MeetMember's regroup decides - it runs only on a clear walk, else the group
        // comes to the entrance and walks back with it - instead of a lone walk through the refilled mobs.
        bool const respawnZone = RespawnZoneOnWalk(party.instanceId, back.points);
        if (respawnZone)
            member.backInMs = now;
        reset();
        LOG_INFO("playerbots",
                 "[stranded_reenter] {}: {} had no walk back to {} for {} s (watchdog stuck {} s, {:.0f} yd, at "
                 "{:.1f} {:.1f} {:.1f}): re-enters the instance at entrance {} - counted as a death ({} of {})",
                 party.id, bot->GetName(), reference->GetName(), noWalkMs / IN_MILLISECONDS,
                 member.reenterWatchdogSec, distance, from.x, from.y, from.z, party.entranceIdx,
                 member.strandedReentries, STRANDED_REENTER_MAX);
        LogEvent(party, "stranded_reenter",
                 JsonObject().Str("name", bot->GetName()).Str("state", "re-entered").Str("to", reference->GetName())
                     .Num("noWalkSec", noWalkMs / IN_MILLISECONDS).Real("distance", distance)
                     .Real("x", from.x).Real("y", from.y).Real("z", from.z)
                     .Num("entrance", int64(party.entranceIdx)).Num("count", member.strandedReentries)
                     .Bool("countedAsDeath", true).Bool("respawnZone", respawnZone));
        return true;
    }

    // [b30_reenter] The movers out of the instance and in again at `entrance` (WotLK's LFG teleport out / in): not
    // a death ([b31_shortcut_not_death]), no harness move. The goal stays the route's; only the way
    // there changes.
    void TestHarnessMgr::ShortcutReenter(Party& party, std::vector<Player*> const& movers, size_t entrance,
                                         char const* why, float walk, float entranceWalk, std::string const& goal)
    {
        Position const& at = party.entrances[entrance];
        std::string names;
        for (Player* bot : movers)
        {
            // [b31_shortcut_not_death] user 2026-10-25: a shortcut re-entry is a route choice, NOT a death (only the
            // stranded no-path re-entry is); it stays visible in the shortcut_reenter event below.
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
            if (!bot->TeleportTo(bot->GetMapId(), at.GetPositionX(), at.GetPositionY(), at.GetPositionZ(),
                                 at.GetOrientation()))
                continue;
            names += (names.empty() ? "" : ",") + bot->GetName();
            if (bot->GetGUID() == party.leader)
                TestRunner::ClearWaypoint(party.leader);
        }

        uint32 const now = getMSTime();
        ++party.shortcutReentries;
        party.shortcutCooldownUntilMs = now + SHORTCUT_REENTER_COOLDOWN_MS;
        party.lastProgressMs = now;
        LOG_INFO("playerbots",
                 "[shortcut_reenter] {}: {} re-enter at entrance {} ({}): walk to {} {:.0f} yd from here, {:.0f} yd "
                 "from the entrance - a route choice, not a death",
                 party.id, names, entrance, why, goal, walk, entranceWalk);
        LogEvent(party, "shortcut_reenter",
                 JsonObject().Str("members", names).Str("why", why).Str("goal", goal).Real("walk", walk)
                     .Real("entranceWalk", entranceWalk).Num("entrance", int64(entrance))
                     .Real("x", at.GetPositionX()).Real("y", at.GetPositionY()).Real("z", at.GetPositionZ())
                     .Bool("countedAsDeath", false).Bool("harnessMove", false));
    }

    bool TestHarnessMgr::TickShortcutReenter(Party& party, std::vector<Player*> const& players, Player* leader)
    {
        uint32 now = getMSTime();
        now = now ? now : 1;
        Map* map = leader ? leader->GetMap() : nullptr;
        if (!map || !map->IsDungeon() || party.entrances.empty() || party.content->raid || party.linkIdx >= 0 ||
            !party.escort.IsEmpty() || party.wingWalk || party.approach || party.guaranteeHolding ||
            int32(now - party.shortcutCooldownUntilMs) < 0 ||
            (party.shortcutCheckMs && getMSTimeDiff(party.shortcutCheckMs, now) < SHORTCUT_REENTER_CHECK_MS))
            return false;
        // [b35_up] never out of the instance while Skadi's gauntlet runs: leaving the hall resets it
        if (SkadiGauntletRunning(leader))
            return false;
        // [b49_hos] never out of the instance while an encounter runs (a boss slot IN_PROGRESS): an event fought by
        // an NPC goes on without the group. hos-h 20261026-111724-hos-h-5F-20 (build 47): Brann's Tribunal of Ages
        // went IN_PROGRESS at 1891 s with the group 56-60 yd from the hold spot; at 1904 s Sjonnir was skipped (closed
        // door), the fallback target was a Dark Rune Warrior 491 yd off and "shortcut_reenter" put all five at the
        // entrance (1153,806), 535 yd from Brann. The healer came back first (Protectors on it from 1942 s), the dps
        // ~2020 s, 4 deaths 2024-2048 s. hos-h 093620-5E-22 (build 45) the same: IN_PROGRESS 1985 s, re-entry 2019 s,
        // back at 2088-2110 s, 3 deaths 2112-2119 s. Both "Tribunal wipes" were this, not the add DPS.
        if (InstanceScript* script = leader->GetInstanceScript(); script && script->IsEncounterInProgress())
        {
            if (!party.shortcutEncounterLogMs ||
                getMSTimeDiff(party.shortcutEncounterLogMs, now) >= 30 * IN_MILLISECONDS)
            {
                party.shortcutEncounterLogMs = now;
                LOG_INFO("playerbots", "[b49_hos] {}: no shortcut re-entry, an encounter is in progress",
                         leader->GetName());
            }
            return false;
        }
        party.shortcutCheckMs = now;

        auto const canReenter = [](Player* bot)
        {
            return bot->IsAlive() && !bot->IsInCombat() && !bot->IsBeingTeleported() && !bot->GetTransport() &&
                   !bot->GetVehicle() && !bot->IsFlying();
        };
        for (Player* bot : players)
            if (bot->GetMap() == map && (bot->IsInCombat() || bot->GetTransport() || bot->GetVehicle()))
                return false;  // nobody fights, rides a lift or a vehicle
        if (!canReenter(leader))
            return false;

        // The entrance with the shortest complete walk from it to (x, y, z), as `walker` walks; -1 for none.
        auto const bestEntrance = [&](Player* walker, float x, float y, float z, float& length) -> int32
        {
            int32 best = -1;
            for (size_t i = 0; i < party.entrances.size(); ++i)
            {
                Position const& entrance = party.entrances[i];
                if (entrance.GetExactDist(x, y, z) >= length)
                    continue;  // even the straight line is no shortcut
                G3D::Vector3 const from(entrance.GetPositionX(), entrance.GetPositionY(), entrance.GetPositionZ());
                PathResult const walk = RoutePath(walker, x, y, z, &from);
                // [b32_sh_ooze] a walk from the entrance through a respawn zone (refilled behind the group) costs
                // RESPAWN_ZONE_WALK_COST more: the re-entered side meets its mobs again
                float const walkCost =
                    walk.length + (RespawnZoneOnWalk(party.instanceId, walk.points) ? RESPAWN_ZONE_WALK_COST : 0.0f);
                if (walk.complete && (best < 0 || walkCost < length))
                {
                    best = int32(i);
                    length = walkCost;
                }
            }
            return best;
        };
        auto const worth = [](float walk, float entranceWalk)
        {
            return walk > SHORTCUT_REENTER_RATIO * entranceWalk && walk - entranceWalk > SHORTCUT_REENTER_MIN_GAIN;
        };

        // The group's route: everyone in here is with the leader, the goal its target.
        WorldObject* goal = party.target.IsEmpty() ? nullptr : ObjectAccessor::GetWorldObject(*leader, party.target);
        std::vector<Player*> group;
        bool together = true;
        for (Player* bot : players)
        {
            if (bot->GetMap() != map || !bot->IsAlive())
                continue;
            if (bot != leader && bot->GetExactDist(leader) > MEET_RANGE)
                together = false;
            group.push_back(bot);
        }
        if (goal && goal->GetMap() == map && together)
        {
            PathResult const here = RoutePath(leader, goal->GetPositionX(), goal->GetPositionY(), goal->GetPositionZ());
            if (here.complete && here.length > SHORTCUT_REENTER_MIN_GAIN)
            {
                float entranceWalk = here.length / SHORTCUT_REENTER_RATIO;
                int32 const entrance = bestEntrance(leader, goal->GetPositionX(), goal->GetPositionY(),
                                                    goal->GetPositionZ(), entranceWalk);
                if (entrance >= 0 && worth(here.length, entranceWalk) &&
                    std::all_of(group.begin(), group.end(), canReenter))
                {
                    ShortcutReenter(party, group, size_t(entrance), "the group's route", here.length, entranceWalk,
                                    goal->GetName());
                    return true;
                }
            }
            return false;
        }

        // A single member far behind the group: its walk to the leader against the walk from an entrance.
        for (Player* bot : players)
        {
            if (bot == leader || bot->GetMap() != map || !canReenter(bot) ||
                bot->GetExactDist(leader) <= SHORTCUT_REENTER_MEMBER_FAR)
                continue;
            PathResult const here = RoutePath(bot, leader->GetPositionX(), leader->GetPositionY(),
                                              leader->GetPositionZ());
            if (!here.complete || here.length <= SHORTCUT_REENTER_MIN_GAIN)
                continue;  // no walk at all is TickStrandedReenter's (a death)
            float entranceWalk = here.length / SHORTCUT_REENTER_RATIO;
            int32 const entrance =
                bestEntrance(bot, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(), entranceWalk);
            if (entrance >= 0 && worth(here.length, entranceWalk))
            {
                ShortcutReenter(party, {bot}, size_t(entrance), "a member far behind", here.length, entranceWalk,
                                leader->GetName());
                return true;
            }
        }
        return false;
    }

    bool TestHarnessMgr::MeetMember(Party& party, std::vector<Player*> const& players, Player* leader)
    {
        // [lead107] the leader's (RouteMeet.h)
        PartyLeaderGroup group(*this, party);
        return Leader::Travel::MeetMember(group, players, leader);
    }

    void TestHarnessMgr::LogHarnessHold(Party& party, Player* leader, Creature* blocker, char const* walk, float x,
                                        float y, float z)
    {
        // [lead107] the leader's (RouteSteer.h)
        PartyLeaderGroup group(*this, party);
        Leader::Travel::LogHarnessHold(group, leader, blocker, walk, x, y, z);
    }

    void TestHarnessMgr::PrepareEventItems(Party& party, Player* leader, bool atStart)
    {
        if (!leader || !party.content)
            return;

        // [hleft-bm-quest] the event's quest in every member's log (taken or rewarded already: left as it is)
        if (atStart)
            for (PrepQuestDef const* def : PrepQuests(party.content->mapId))
                if (Quest const* quest = sObjectMgr->GetQuestTemplate(def->questId))
                    for (Member const& member : party.members)
                        if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                            bot && bot->GetQuestStatus(def->questId) == QUEST_STATUS_NONE &&
                            !bot->GetQuestRewardStatus(def->questId) && bot->CanAddQuest(quest, false))
                        {
                            bot->AddQuestAndCheckCompletion(quest, nullptr);
                            LogEvent(party, "prep_quest",
                                     JsonObject().Str("name", member.name).Num("quest", def->questId)
                                         .Str("note", def->note)
                                         .Num("status", uint32(bot->GetQuestStatus(def->questId))));
                        }

        for (EventItemDef const* def : EventItems(party.content->mapId))
        {
            if (!sObjectMgr->GetItemTemplate(def->itemId) || def->dropFrom.empty() != atStart ||
                (def->difficulty >= 0 && def->difficulty != int8(party.content->difficulty)))
                continue;

            // [b32_strat_gate] EventItemDef::everyMember: the other members' own copy (the leader's below). strat-n
            // 221216: only the tank had the Key to the City, the healer shut out behind the Service Entrance Gate
            // stood 2 yd from it with no way to open it.
            if (atStart && def->everyMember)
                for (Member const& member : party.members)
                    if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                        bot && bot != leader && bot->GetItemCount(def->itemId) < def->count)
                    {
                        uint32 const add = def->count - bot->GetItemCount(def->itemId);
                        bool const stored = bot->StoreNewItemInBestSlots(def->itemId, add);
                        LogEvent(party, "event_item", JsonObject().Str("name", bot->GetName()).Num("item", def->itemId)
                                                          .Num("count", add).Bool("stored", stored)
                                                          .Str("kind", "carried (every member)")
                                                          .Str("source", def->source));
                    }

            uint32 want = 0;
            uint32 const have = leader->GetItemCount(def->itemId);
            if (atStart)
                want = have < def->count ? def->count - have : 0;
            else if (def->perKill)
            {
                // One per kill: what the kills dropped minus what was handed out already.
                auto const kills = party.deadEntries.find(def->dropFrom.front());
                uint32 const dropped = kills != party.deadEntries.end() ? kills->second : 0;
                uint32 const given = party.eventItemsGiven[def->itemId];
                want = dropped > given ? dropped - given : 0;
            }
            else if (!party.eventItemsGiven[def->itemId] && EntriesDead(party, def->dropFrom))
                want = have < def->count ? def->count - have : 0;

            if (!want)
                continue;

            bool const stored = leader->StoreNewItemInBestSlots(def->itemId, want);
            party.eventItemsGiven[def->itemId] += want;
            LogEvent(party, "event_item", JsonObject().Str("name", leader->GetName()).Num("item", def->itemId)
                                              .Num("count", want).Bool("stored", stored)
                                              .Str("kind", atStart ? "carried" : "dungeon drop")
                                              .Str("source", def->source));
        }
    }

    bool TestHarnessMgr::StartersUsed(Party const& party, std::vector<uint32> const& entries) const
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(const_cast<TestHarnessMgr&>(*this), const_cast<Party&>(party));
        return Leader::Travel::StartersUsed(group, entries);
    }

    void TestHarnessMgr::MarkStartersDoneByGroup(Party& party, Player* leader)
    {
        // [lead107] the leader's (RouteEvents.h)
        PartyLeaderGroup group(*this, party);
        Leader::Travel::MarkStartersDoneByGroup(group, leader);
    }

    void TestHarnessMgr::WatchEventState(Party& party, Map* map)
    {
        // [b31_dg] A boss track whose creature left the world is re-bound to the same entry's new creature. A boss with
        // CREATURE_FLAG_EXTRA_HARD_RESET (Hadronox 28921, Krik'thir 28684: flags_extra 0x80000000) despawns on evade
        // and respawns 2 s later as a NEW object (CreatureAI::EnterEvadeMode -> DespawnOnEvade, ForcedDespawn ->
        // RemoveCorpse, a new guid): the track kept the dead guid, map->GetCreature() found nothing for the rest of
        // the run, its dbc state stayed NOT_STARTED and run_end said "creature not loaded (grid inactive)" with the
        // group standing on her platform (an-n 20261025-184229-an-n-5G-32 from 482 s on; also an-n 20261024-004658,
        // an-h 20261025-022018, an-h 20261025-184229-5D-36 after a Hadronox reset; Krik'thir an-n 20261024-010147,
        // an-h 20261024-040751). ResolveBoss binds only an empty track, so the stale guid is cleared and replaced here.
        if (map && map->GetInstanceId() == party.instanceId)
        {
            constexpr uint32 STALE_BOSS_SCAN_MS = 2 * IN_MILLISECONDS;
            static std::unordered_map<uint32, uint32> staleScanMs;  // per party seq (the harness tick, world thread)
            uint32 const nowMs = getMSTime();
            uint32& last = staleScanMs[party.seq];
            if (!last || getMSTimeDiff(last, nowMs) >= STALE_BOSS_SCAN_MS)
            {
                last = nowMs ? nowMs : 1;
                for (BossTrack& boss : party.bosses)
                {
                    if (boss.killed || boss.creature.IsEmpty() || map->GetCreature(boss.creature))
                        continue;

                    std::vector<uint32> entries = boss.credits;
                    if (boss.entry)
                        entries.push_back(boss.entry);
                    Creature* fresh = nullptr;
                    for (auto const& [spawnId, candidate] : map->GetCreatureBySpawnIdStore())
                        if (candidate && candidate->IsInWorld() &&
                            std::find(entries.begin(), entries.end(), candidate->GetEntry()) != entries.end())
                        {
                            fresh = candidate;
                            break;
                        }
                    if (!fresh)
                        continue;  // not respawned yet: the next scan

                    ObjectGuid const old = boss.creature;
                    boss.creature = fresh->GetGUID();
                    if (party.bossCreatures.erase(old))
                        party.bossCreatures.insert(fresh->GetGUID());
                    LogEvent(party, "boss_rebound", JsonObject().Num("bossId", boss.bossId).Str("boss", boss.name)
                                                        .Bool("dbc", boss.dbc).Num("entry", fresh->GetEntry())
                                                        .Num("oldLow", old.GetCounter())
                                                        .Num("newLow", fresh->GetGUID().GetCounter())
                                                        .Bool("alive", fresh->IsAlive())
                                                        .Str("why", "the boss object left the world (despawn on evade "
                                                                    "/ respawn): bound to its new creature"));
                }
            }
        }

        // [brd_ring_*] Blackrock Depths, the Ring of Law: TYPE_RING_OF_LAW goes NOT_STARTED -> IN_PROGRESS on
        // at_ring_of_law and to DONE at Grimstone's waypoint 5. A FAIL (30 s with no summon in combat, npc_grimstone
        // updateReset) is never seen: instance_blackrock_depths SetData(FAIL) stores DATA_TIME_RING_FAIL and sets
        // NOT_STARTED at once, and the trigger refuses for 2 minutes. Seen as IN_PROGRESS -> NOT_STARTED: the step spot
        // holds the group (eventSpotRetry, SpotHolds) and areatrigger 1526 is armed again after the lockout,
        // BRD_RING_MAX_FAILS times. While the event runs it is progress: the waves come by themselves (no 4-minute
        // stall), and its start opens one more boss-progress window (as Medivh's shield below).
        if (map && party.content && party.content->mapId == BRD_MAP_ID && map->GetInstanceId() == party.instanceId)
        {
            InstanceScript* script = map->ToInstanceMap() ? map->ToInstanceMap()->GetInstanceScript() : nullptr;
            if (!script)
                return;
            uint32 const now = getMSTime();
            int32 const ring = int32(script->GetData(BRD_TYPE_RING_OF_LAW));
            if (ring == IN_PROGRESS)
                party.lastProgressMs = now;
            int32 const before = party.watchedRing;
            if (ring == before)
                return;
            party.watchedRing = ring;
            if (before < 0)
                return;  // first read
            if (ring == IN_PROGRESS)
            {
                party.eventSpotRetry = false;
                party.lastBossProgressMs = now;
                party.lastEvent = "event: Ring of Law";
                LogEvent(party, "brd_ring_start", JsonObject().Num("fails", party.ringFails));
            }
            else if (ring == DONE)
            {
                party.eventSpotRetry = false;
                LogEvent(party, "brd_ring_done", JsonObject().Num("fails", party.ringFails));
            }
            else if (before == IN_PROGRESS)
            {
                ++party.ringFails;
                int64 const failAt = int64(script->GetData(BRD_DATA_TIME_RING_FAIL));
                int64 const nowSec = int64(GameTime::GetGameTime().count());
                party.ringRetryAtSec = (failAt > 0 ? failAt : nowSec) + BRD_RING_LOCKOUT_SEC;
                bool const retry = party.ringFails <= BRD_RING_MAX_FAILS;
                party.eventSpotRetry = retry;
                if (retry)
                    for (TriggerTrack& track : party.triggers)
                        if (track.triggerId == BRD_RING_TRIGGER)
                            track.fires = 0;  // its 60 s refires while the event ran used up TRIGGER_MAX_FIRES
                party.lastBossProgressMs = now;
                party.lastEvent = "event failed: Ring of Law";
                LogEvent(party, retry ? "brd_ring_retry" : "brd_ring_gave_up",
                         JsonObject().Num("fails", party.ringFails)
                             .Num("retryInSec", std::max<int64>(0, party.ringRetryAtSec - nowSec)));
            }
            return;
        }

        // the_black_morass.h: DATA_SHIELD_PERCENT 13, DATA_RIFT_NUMBER 14 (GetData of instance_the_black_morass).
        constexpr uint32 BLACK_MORASS_MAP_ID = 269;
        constexpr uint32 DATA_BM_SHIELD_PERCENT = 13;
        constexpr uint32 DATA_BM_RIFT_NUMBER = 14;
        constexpr uint32 WATCH_EVENT_INTERVAL_MS = 2 * IN_MILLISECONDS;
        constexpr int32 SHIELD_LOG_STEP = 10;

        if (!map || !party.content || party.content->mapId != BLACK_MORASS_MAP_ID ||
            map->GetInstanceId() != party.instanceId)
            return;
        uint32 const now = getMSTime();
        if (party.watchedEventMs && getMSTimeDiff(party.watchedEventMs, now) < WATCH_EVENT_INTERVAL_MS)
            return;
        party.watchedEventMs = now;

        InstanceScript* script = map->ToInstanceMap() ? map->ToInstanceMap()->GetInstanceScript() : nullptr;
        if (!script)
            return;
        int32 const shield = int32(script->GetData(DATA_BM_SHIELD_PERCENT));
        int32 const rift = int32(script->GetData(DATA_BM_RIFT_NUMBER));
        bool const riftChanged = rift != party.watchedRift;
        bool const shieldStep = party.watchedShield < 0 || std::abs(shield - party.watchedShield) >= SHIELD_LOG_STEP ||
                                (shield == 0 && party.watchedShield != 0);
        if (!riftChanged && !shieldStep)
            return;

        char const* state = "running";
        if (shield == 0 && party.watchedShield > 0)
            state = "failed: Medivh's shield fell";
        else if (rift == 0 && party.watchedRift > 0)
            state = "reset";
        else if (rift == 0)
            state = "not started";
        // What stands at Medivh when the shield moves (bm-h build 21: 47 -> 0 in 22 s at rift 6, unknown hitters):
        // every living creature within 30 yd of him but the group, its entry, its victim and its current spell.
        JsonArray atMedivh;
        Creature* medivh = nullptr;
        for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
            if (creature && creature->IsAlive() && creature->GetEntry() == 15608)
                medivh = creature;
        if (medivh && shieldStep)
        {
            std::list<Creature*> around;
            Acore::AnyUnitInObjectRangeCheck check(medivh, 30.0f);
            Acore::CreatureListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(medivh, around, check);
            Cell::VisitObjects(medivh, searcher, 30.0f);
            for (Creature* creature : around)
            {
                if (creature == medivh || !creature->IsAlive() || creature->IsFriendlyTo(medivh))
                    continue;
                Spell const* spell = creature->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
                if (!spell)
                    spell = creature->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                atMedivh.Raw(JsonObject().Str("name", creature->GetName()).Num("entry", creature->GetEntry())
                                 .Real("dist", creature->GetExactDist(medivh))
                                 .Str("victim", creature->GetVictim() ? creature->GetVictim()->GetName() : "")
                                 .Num("spell", spell && spell->GetSpellInfo() ? spell->GetSpellInfo()->Id : 0)
                                 .Bool("inCombat", creature->IsInCombat()).Build());
            }
        }
        LogEvent(party, "event_state", JsonObject().Str("event", "Black Morass: Medivh").Str("state", state)
                                           .Num("shield", shield).Num("rift", rift)
                                           .Num("shieldBefore", party.watchedShield)
                                           .Num("riftBefore", party.watchedRift).Raw("atMedivh", atMedivh.Build()));
        // A new rift is event progress: it opens 1 s after a keeper dies, else every 90 s while a rift spot is free,
        // and Chrono Lord Deja only comes with rift 6 (~10 min in), so the 25 min no-boss-progress window counts from
        // the last rift (bm-n build 25: shield 100% at rift 7, the run ended "no boss progress" with Deja just up).
        if (riftChanged && party.watchedRift >= 0 && rift > party.watchedRift && shield > 0)
        {
            party.lastBossProgressMs = now;
            party.lastEvent = Acore::StringFormat("event: Black Morass rift {}", rift);
        }
        if (shield == 0 && party.watchedShield > 0)
        {
            party.lastEvent = Acore::StringFormat("event failed: Medivh's shield fell at rift {}", rift);
            // The core resets the event (300 s) and it starts again: one more full boss-progress window from here.
            party.lastBossProgressMs = getMSTime();
        }
        party.watchedShield = shield;
        party.watchedRift = rift;
    }

    void TestHarnessMgr::EmulateAreaTriggers(Party& party, std::vector<Player*> const& players)
    {
        uint32 const now = getMSTime();
        for (TriggerTrack& track : party.triggers)
        {
            AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(track.triggerId);
            if (!trigger)
                continue;

            // [brd_ring_lockout] at_ring_of_law refuses for 2 minutes after a FAIL (DATA_TIME_RING_FAIL): no fire
            // until then, the group waits at the spot (SpotHolds).
            if (track.triggerId == BRD_RING_TRIGGER && party.eventSpotRetry &&
                int64(GameTime::GetGameTime().count()) < party.ringRetryAtSec)
            {
                if (!track.lastFireMs || getMSTimeDiff(track.lastFireMs, now) >= TRIGGER_REFIRE_MS)
                {
                    track.lastFireMs = now ? now : 1;  // throttles this line to one per minute
                    LogEvent(party, "brd_ring_lockout",
                             JsonObject().Num("waitSec",
                                              party.ringRetryAtSec - int64(GameTime::GetGameTime().count())));
                }
                continue;
            }

            // [b32_pos] the trigger of a reached progress step whose progress is not there yet (ProgressStepPending):
            // re-fired every TRIGGER_STEP_REFIRE_MS, no cap - a fire with its condition unmet does nothing.
            EventSpotDef const* pendingStep = nullptr;
            if (party.content && !track.entrance)
                for (EventSpotDef const* spot : EventSpots(party.content->mapId))
                    if (spot->progressTrigger == track.triggerId && ProgressStepPending(party, *spot))
                        pendingStep = spot;

            if (pendingStep)
            {
                if (track.fires && getMSTimeDiff(track.lastFireMs, now) < TRIGGER_STEP_REFIRE_MS)
                    continue;
            }
            else if (!track.entrance && (track.fires >= TRIGGER_MAX_FIRES ||
                                         (track.fires && getMSTimeDiff(track.lastFireMs, now) < TRIGGER_REFIRE_MS)))
                continue;

            for (Player* bot : players)
            {
                if (bot->GetMapId() != trigger->map || bot->IsBeingTeleported() || !bot->IsInAreaTriggerRadius(trigger))
                    continue;

                // The instance portal: only someone outside the instance walks into it (a ghost going back).
                if (track.entrance && (bot->GetMapId() == party.content->mapId ||
                                       (track.lastFireMs && getMSTimeDiff(track.lastFireMs, now) < ENTRANCE_REFIRE_MS)))
                    continue;

                WorldPacket packet(CMSG_AREATRIGGER);
                packet << uint32(track.triggerId);
                bot->GetSession()->HandleAreaTriggerOpcode(packet);

                ++track.fires;
                track.lastFireMs = now;
                JsonObject fired;
                fired.Num("id", track.triggerId).Str("name", bot->GetName()).Bool("entrance", track.entrance)
                    .Bool("alive", bot->IsAlive());
                if (pendingStep)
                {
                    // [b32_pos] a step re-fire is no progress by itself (the stall clock keeps running); the
                    // progress value before / after the fire says whether its condition was met.
                    InstanceScript* script = PartyInstanceScript(party);
                    uint32 const after = script ? script->GetData(uint32(pendingStep->progressData)) : 0;
                    fired.Num("progressAfter", after).Num("progressNeeded", pendingStep->progressMin)
                        .Bool("stepHold", true);
                    if (after >= pendingStep->progressMin)
                        party.lastProgressMs = now;
                    if (track.fires > 1 && after < pendingStep->progressMin &&
                        getMSTimeDiff(party.stepRefireLogMs, now) < TRIGGER_REFIRE_MS)
                        break;  // the 10 s re-fires log once a minute while nothing changes
                    party.stepRefireLogMs = now;
                }
                else
                    party.lastProgressMs = now;
                LogEvent(party, "areatrigger", fired);
                break;
            }
        }
    }

    // Why the core refuses the entrance, read-only (InstanceMap::CannotEnter without its transfer-abort packet): the
    // bound instance's player cap and its encounter lock (raids and Halls of Reflection, Map.cpp checkProgress).
    static std::string EntryRefusal(Player* bot, uint32 mapId)
    {
        MapEntry const* entry = sMapStore.LookupEntry(mapId);
        if (!entry || !entry->IsDungeon())
            return "";
        uint32 const instanceId =
            sInstanceSaveMgr->PlayerGetDestinationInstanceId(bot, mapId, bot->GetDifficulty(entry->IsRaid()));
        Map* bound = instanceId ? sMapMgr->FindMap(mapId, instanceId) : nullptr;
        InstanceMap* instance = bound ? bound->ToInstanceMap() : nullptr;
        if (!instance)
            return "";
        if (instance->GetPlayersCountExceptGMs() >= instance->GetMaxPlayers())
            return "instance is full (TRANSFER_ABORT_MAX_PLAYERS)";
        InstanceScript* script = instance->GetInstanceScript();
        if (!(entry->IsRaid() || mapId == HOR_MAP) || !script || !script->IsEncounterInProgress())
            return "";
        std::string reason = "encounter in progress (TRANSFER_ABORT_ZONE_IN_COMBAT), " +
                             std::to_string(instance->GetPlayersCountExceptGMs(true)) + " alive inside";
        if (mapId == HOR_MAP)
            reason += ", wave " + std::to_string(script->GetData(HOR_DATA_WAVE_NUMBER));
        return reason;
    }

    void TestHarnessMgr::GuideGhosts(Party& party, std::vector<Player*> const& players)
    {
        if (!HARNESS_MOVES_BOTS)
        {
            // Bots walk by themselves: a ghost runs to the portal on its own ("ghost portal": a walk, else the spirit
            // healer), a living member outside walks or flies in ("instance portal") once the group's intent says so
            // (the portal goal: back in through the entrance the run uses). The harness only watches and logs what
            // the bot cannot do.
            uint32 const now = getMSTime();
            // Where the members outside are (wave 13: RFK's ghosts paced under the Great Lift for 20 minutes and the
            // run log had no position outside the instance): every GHOST_POS_MS while anyone is out.
            if (!party.ghostPosMs || getMSTimeDiff(party.ghostPosMs, now) >= GHOST_POS_MS)
            {
                JsonArray outside;
                for (Player* bot : players)
                {
                    if (bot->GetMapId() == party.content->mapId)
                        continue;
                    std::string trail;
                    bool ghostPortal = false;
                    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                    {
                        if (Engine* engine = botAI->GetCurrentEngine())
                            trail = engine->GetLastAction();
                        ghostPortal = botAI->HasStrategy("ghost portal", BOT_STATE_DEAD);
                    }
                    if (trail.size() > GHOST_POS_TRAIL)
                        trail = trail.substr(trail.size() - GHOST_POS_TRAIL);
                    outside.Raw(JsonObject().Str("name", bot->GetName()).Bool("alive", bot->IsAlive())
                                    .Bool("ghost", bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
                                    .Bool("ghostPortal", ghostPortal).Num("map", bot->GetMapId())
                                    .Real("x", bot->GetPositionX()).Real("y", bot->GetPositionY())
                                    .Real("z", bot->GetPositionZ()).Bool("moving", bot->isMoving())
                                    .Str("lastAction", trail).Build());
                }
                if (!outside.Empty())
                {
                    party.ghostPosMs = now;
                    LogEvent(party, "ghost_pos", JsonObject().Raw("members", outside.Build()));
                }
            }
            for (Player* bot : players)
            {
                Member* member = nullptr;
                for (Member& candidate : party.members)
                    if (candidate.guid == bot->GetGUID())
                        member = &candidate;
                if (!member)
                    continue;

                bool const inside = bot->GetMapId() == party.content->mapId;
                bool const ghost = !bot->IsAlive() && bot->HasPlayerFlag(PLAYER_FLAGS_GHOST);
                if (inside || bot->IsBeingTeleported() || bot->IsInCombat() || (!ghost && !bot->IsAlive()))
                {
                    if (inside && member->hasPortalGoal && !party.wingWalk && !bot->IsBeingTeleported())
                    {
                        SelfTravel::ClearPortalGoal(member->guid);
                        member->hasPortalGoal = false;
                    }
                    if (inside)
                    {
                        member->leftOutside = false;
                        member->outsideStuckCount = 0;
                    }
                    member->noWalkSinceMs = 0;
                    continue;
                }
                if (ghost && bot->GetCorpseLocation().GetMapId() != party.content->mapId)
                    continue;

                if (!ghost && !member->hasPortalGoal && !party.wingWalk && party.entranceIdx < party.entrances.size())
                {
                    // Back in next to the group (Gundrak heroic 2026-10-25: a revived shaman went in through the
                    // run's south entrance, 1002 yd of walk from the group at the north one, and fell off its
                    // ledge): the entrance whose arrival is nearest the leader inside (FindPortal: the arrival
                    // nearest the goal).
                    Player* const inLeader = ObjectAccessor::FindPlayer(party.leader);
                    bool const leaderIn = inLeader && inLeader != bot && inLeader->IsAlive() &&
                                          inLeader->GetMapId() == party.content->mapId;
                    Position const at = leaderIn ? inLeader->GetPosition() : party.entrances[party.entranceIdx];
                    SelfTravel::PortalGoal goal;
                    goal.mapId = party.content->mapId;
                    goal.x = at.GetPositionX();
                    goal.y = at.GetPositionY();
                    goal.z = at.GetPositionZ();
                    SelfTravel::SetPortalGoal(member->guid, goal);
                    member->hasPortalGoal = true;
                }

                // The portal it is heading for, for the log only.
                AreaTrigger const* best = nullptr;
                for (TriggerTrack const& track : party.triggers)
                {
                    AreaTrigger const* trigger = track.entrance ? sObjectMgr->GetAreaTrigger(track.triggerId) : nullptr;
                    if (trigger && trigger->map == bot->GetMapId() &&
                        (!best || bot->GetExactDist(trigger->x, trigger->y, trigger->z) <
                                      bot->GetExactDist(best->x, best->y, best->z)))
                        best = trigger;
                }
                float const toX = best ? best->x : 0.0f;
                float const toY = best ? best->y : 0.0f;
                float const toZ = best ? best->z : 0.0f;

                // On its entrance and still outside: the core's refusal by name (hor-n / hor-h 2026-10-25: the members
                // inside kept the Falric / Marwyn waves running, the others stood on the portal 400 s logging only "the
                // portal did not take the bot"). The bot keeps trying by itself; the run waits, then ends with it.
                bool const onEntrance = best && bot->GetExactDist2d(toX, toY) < ENTRY_REFUSAL_RANGE;
                std::string const refusal = onEntrance ? EntryRefusal(bot, party.content->mapId) : std::string();
                if (refusal.empty())
                    member->entryRefusedSinceMs = 0;
                else
                {
                    if (!member->entryRefusedSinceMs)
                        member->entryRefusedSinceMs = now ? now : 1;
                    if (refusal != member->entryRefusal)
                    {
                        member->entryRefusal = refusal;
                        party.lastEvent = "entry refused: " + refusal;
                        LogEvent(party, "entry_refused",
                                 JsonObject().Str("name", bot->GetName()).Str("reason", refusal)
                                     .Num("trigger", best->entry)
                                     .Num("refusedSec",
                                          getMSTimeDiff(member->entryRefusedSinceMs, now) / IN_MILLISECONDS));
                    }
                    if (getMSTimeDiff(member->entryRefusedSinceMs, now) > ENTRY_REFUSED_MAX_MS)
                    {
                        uint32 const minutes = ENTRY_REFUSED_MAX_MS / MINUTE / IN_MILLISECONDS;
                        Finish(party, "failed",
                               member->name + " cannot enter for " + std::to_string(minutes) + " min: " + refusal);
                        return;
                    }
                }

                if (!member->noWalkSinceMs)
                {
                    float const left = best ? bot->GetExactDist(toX, toY, toZ) : 0.0f;
                    LogEvent(party, "ghost_guide", JsonObject().Str("name", bot->GetName()).Bool("own", true)
                                                       .Bool("ghost", ghost).Num("trigger", best ? best->entry : 0)
                                                       .Real("distance", left));
                }

                if (!member->noWalkSinceMs || bot->GetExactDist(&member->noWalkPos) > 3.0f)
                {
                    if (member->noWalkSinceMs)
                    {
                        party.lastProgressMs = now;  // a member walking back is progress for the stall detector
                        member->outsideStuckCount = 0;
                    }
                    member->noWalkPos = bot->GetPosition();
                    member->noWalkSinceMs = now;
                }
                else if (getMSTimeDiff(member->noWalkSinceMs, now) > GHOST_STUCK_MS)
                {
                    member->noWalkSinceMs = now;
                    // Outside and stuck for good, alive or a ghost (dtk-n build 10: a spirit-healed shaman stood 30
                    // yd above the portal for 4 minutes; bfd-h: a ghost with no walk to the underwater portal, 265
                    // s): not waited for any more; it keeps trying by itself and is waited for again once inside.
                    if (++member->outsideStuckCount >= OUTSIDE_GIVE_UP_STUCKS && !member->leftOutside)
                    {
                        member->leftOutside = true;
                        LogEvent(party, "member_left_outside", JsonObject().Str("name", bot->GetName())
                                                                   .Bool("ghost", ghost)
                                                                   .Real("x", bot->GetPositionX())
                                                                   .Real("y", bot->GetPositionY())
                                                                   .Real("z", bot->GetPositionZ()));
                        // [b30_st_spirit] Going on without it needs a healer alive inside (st-n
                        // 20261025-184229-st-n-5F-11: the druid healer's and the warlock's ghosts stood above the
                        // Sunken Temple portal - no navmesh from the Pool of Tears down into the temple - while the
                        // tank, hunter and rogue sat idle inside for 5 minutes). Nobody resurrects a released ghost
                        // (PartyMemberToResurrect takes corpses only), so without a healer the run cannot go on.
                        bool healerInside = false;
                        for (Member const& other : party.members)
                        {
                            Player* mate = ObjectAccessor::FindPlayer(other.guid);
                            healerInside |= mate && mate != bot && mate->IsInWorld() && mate->IsAlive() &&
                                            mate->GetMapId() == party.content->mapId && PlayerbotAI::IsHeal(mate);
                        }
                        if (!healerInside)
                        {
                            Finish(party, "failed",
                                   member->name + (ghost ? " (ghost)" : " (alive, outside)") +
                                       " cannot get back in: no walk to the portal (trigger " +
                                       std::to_string(best ? best->entry : 0) + ", " +
                                       std::to_string(int32(best ? bot->GetExactDist(toX, toY, toZ) : 0.0f)) +
                                       " yd) and no healer alive inside to go on without it");
                            return;
                        }
                    }
                    LogCannotMove(party, bot, "corpse run",
                                  ghost ? "ghost made no progress to the portal for 60 s"
                                        : "made no progress back into the instance for 60 s",
                                  toX, toY, toZ);
                }
            }
            return;
        }

        for (Player* bot : players)
        {
            if (bot->GetMapId() == party.content->mapId || bot->isMoving() || bot->IsInCombat())
                continue;
            // A ghost walks back to its corpse inside; a member who was revived outside (spirit healer, a res at
            // the entrance) walks in through the same portal.
            bool const ghost = !bot->IsAlive() && bot->HasPlayerFlag(PLAYER_FLAGS_GHOST);
            if (!ghost && !bot->IsAlive())
                continue;
            if (ghost && bot->GetCorpseLocation().GetMapId() != party.content->mapId)
                continue;

            AreaTrigger const* best = nullptr;
            for (TriggerTrack const& track : party.triggers)
            {
                if (!track.entrance)
                    continue;
                AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(track.triggerId);
                if (!trigger || trigger->map != bot->GetMapId())
                    continue;
                if (!best || bot->GetExactDist(trigger->x, trigger->y, trigger->z) <
                                 bot->GetExactDist(best->x, best->y, best->z))
                    best = trigger;
            }

            if (!best)
                continue;

            Member* member = nullptr;
            for (Member& candidate : party.members)
                if (candidate.guid == bot->GetGUID())
                    member = &candidate;
            if (!member)
                continue;

            // The ghost walks to the instance portal like a player would, on the navmesh, one path segment at a time:
            // a path is capped at MAX_POINT_PATH_LENGTH points (~300 yd), and a MovePoint whose path fails falls back
            // to a straight spline through the terrain (Icecrown: graveyard 6447,2061,564 -> Frozen Halls portal at
            // z 808 went down to z -490 and up again). The portal trigger then takes it in.
            // A long corpse run (Blackrock Mountain: graveyard -> Blackrock Spire portal) is over the smooth path's
            // point limit: the corner path then, and the ghost walks GHOST_CORNER_STEP yd of it per move (the
            // MovePoint builds its own smooth path, which must stay under that limit).
            PathResult leg;
            G3D::Vector3 const portal(best->x, best->y, best->z);
            G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            uint32 const type = BuildLeg(bot, here, portal, true, leg.points, leg.end, leg.straightPath);
            Movement::PointsArray const& points = leg.points;
            float const left = bot->GetExactDist(best->x, best->y, best->z);
            float const floorZ = std::min(bot->GetPositionZ(), best->z) - GHOST_PATH_MAX_DROP;
            bool usable = !(type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH)) &&
                          points.size() >= 2;
            for (G3D::Vector3 const& point : points)
                usable = usable && point.z > floorZ;

            G3D::Vector3 const end = !usable ? portal
                                     : leg.straightPath ? RouteWaypoint(leg, points.back(), GHOST_CORNER_STEP)
                                                        : points.back();
            usable = usable && (end - G3D::Vector3(best->x, best->y, best->z)).length() + 2.0f < left;
            // The last yards to the portal where the navmesh ends short of it (Deadmines: the mine ramp 17 yd above
            // the portal, The Stockade, Utgarde Pinnacle) or the path is too short to count: straight to the trigger.
            // Straight to the portal: a ghost always (no fall damage), a living member only over solid ground (Oculus:
            // the entrance ring floats above Coldarra).
            bool const directSafe = ghost || DirectLineOverGround(bot, best->x, best->y, best->z);
            bool const nearPortal = !usable && left <= GHOST_DIRECT_RANGE && directSafe;

            // Progress check: a ghost that stood still for GHOST_STUCK_MS on a usable path is stuck on geometry.
            uint32 const now = getMSTime();
            if (bot->GetExactDist(&member->ghostLastPos) > 3.0f)
            {
                member->ghostLastPos = bot->GetPosition();
                member->ghostStillMs = now;
                party.lastProgressMs = now;  // a member walking back is progress for the stall detector
            }
            else if (!member->ghostStillMs)
                member->ghostStillMs = now;

            bool const stuck = member->ghostStillMs && getMSTimeDiff(member->ghostStillMs, now) > GHOST_STUCK_MS;

            // No walk to the portal for a ghost (Tempest Keep: the portals float above Netherstorm; wave 8 Mechanar
            // "corpse run did not finish in 20 minutes", Arcatraz "did not come back"), or it got stuck on the way:
            // as a player does, it takes the spirit healer at the graveyard (resurrection sickness and durability loss
            // included) and flies from there (FlyToPortal below, on the next ticks).
            if (ghost && ((!usable && !nearPortal) || stuck))
            {
                std::list<Creature*> healers;
                SpiritHealerCheck check;
                Acore::CreatureListSearcher<SpiritHealerCheck> searcher(bot, healers, check);
                Cell::VisitObjects(bot, searcher, SPIRIT_HEALER_RANGE);
                Creature* healer = nullptr;
                for (Creature* candidate : healers)
                    if (!healer || bot->GetExactDist(candidate) < bot->GetExactDist(healer))
                        healer = candidate;
                if (healer)
                {
                    if (bot->GetExactDist(healer) <= SPIRIT_HEALER_USE_RANGE)
                    {
                        WorldPacket activate(CMSG_SPIRIT_HEALER_ACTIVATE, 8);
                        activate << healer->GetGUID();
                        bot->GetSession()->HandleSpiritHealerActivateOpcode(activate);
                        LogEvent(party, "spirit_healer", JsonObject().Str("name", bot->GetName())
                                                            .Bool("alive", bot->IsAlive()).Bool("stuck", stuck)
                                                            .Num("trigger", best->entry).Real("distance", left));
                        member->ghostStillMs = now;
                    }
                    else
                        bot->GetMotionMaster()->MovePoint(0, healer->GetPositionX(), healer->GetPositionY(),
                                                          healer->GetPositionZ(), FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                                          true, true);
                    continue;
                }
            }

            ++party.harnessMoves;  // HARNESS_MOVES_BOTS only: the harness walks / flies this member
            // No walkable route for a living member (Tempest Keep: the portals float above Netherstorm), or stuck on
            // the way: its own flying mount.
            if (!ghost && ((!usable && !nearPortal) || stuck || member->flyCruiseZ > 0.0f) &&
                FlyToPortal(party, *member, bot, best))
            {
                if (!member->ghostGuided)
                {
                    member->ghostGuided = true;
                    member->ghostLastPos = bot->GetPosition();
                    member->ghostStillMs = now;
                }
                continue;
            }

            if (usable)
                bot->GetMotionMaster()->MovePoint(0, end.x, end.y, end.z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, true,
                                                  true);
            else if (nearPortal)
            {
                // generatePath false: a generated path ends where the navmesh does, and the ghost never moved.
                bot->GetMotionMaster()->MovePoint(0, best->x, best->y, best->z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                                  false, true);
                if (!member->ghostNoPathLogged && left > GHOST_ARRIVED_RANGE)
                {
                    member->ghostNoPathLogged = true;
                    LogEvent(party, "ghost_direct",
                             JsonObject().Str("name", bot->GetName()).Num("trigger", best->entry)
                                 .Num("pathType", type).Num("points", int64(points.size()))
                                 .Real("x", bot->GetPositionX()).Real("y", bot->GetPositionY())
                                 .Real("z", bot->GetPositionZ()).Real("distance", left));
                }
            }
            else
            {
                // No navmesh route (or one through the ground): last resort is the old direct move, logged so the
                // missing route can be added; it is the only way the run can still continue. Not for a living member
                // whose straight line leaves the ground: it waits (ghost_no_path says why) instead of falling.
                if (directSafe)
                    bot->GetMotionMaster()->MovePoint(0, best->x, best->y, best->z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                                      true, false);
                if (!member->ghostNoPathLogged)
                {
                    member->ghostNoPathLogged = true;
                    LogEvent(party, "ghost_no_path",
                             JsonObject().Str("name", bot->GetName()).Num("trigger", best->entry)
                                 .Bool("directSafe", directSafe).Bool("ghost", ghost)
                                 .Num("pathType", type).Num("points", int64(points.size()))
                                 .Real("x", bot->GetPositionX()).Real("y", bot->GetPositionY())
                                 .Real("z", bot->GetPositionZ()).Real("distance", left));
                }
            }

            if (getMSTimeDiff(member->ghostStillMs, now) > GHOST_STUCK_MS)
            {
                member->ghostStillMs = now;
                LogEvent(party, "ghost_stuck", JsonObject().Str("name", bot->GetName()).Num("trigger", best->entry)
                                                   .Bool("navmesh", usable).Real("x", bot->GetPositionX())
                                                   .Real("y", bot->GetPositionY()).Real("z", bot->GetPositionZ())
                                                   .Real("distance", left));
            }

            if (!member->ghostGuided)
            {
                member->ghostGuided = true;
                member->ghostNoPathLogged = false;
                member->ghostLastPos = bot->GetPosition();
                member->ghostStillMs = now;
                LogEvent(party, "ghost_guide", JsonObject().Str("name", bot->GetName()).Num("trigger", best->entry)
                                                   .Bool("navmesh", usable).Real("distance", left));
            }
        }
    }

    bool TestHarnessMgr::FlyToPortal(Party& party, Member& member, Player* bot, AreaTrigger const* portal)
    {
        if (!bot->IsAlive() || bot->IsInCombat() || !portal)
            return false;

        uint32 const now = getMSTime();
        if (!bot->HasIncreaseMountedFlightSpeedAura() && !bot->HasFlyAura())
        {
            if (bot->IsNonMeleeSpellCast(false) ||
                (member.flyCastMs && getMSTimeDiff(member.flyCastMs, now) < MOUNT_CAST_WAIT_MS))
                return true;  // mounting

            // The fastest flying mount the character knows (PlayerbotFactory::InitMounts in the preparation).
            uint32 mount = 0;
            int32 bestSpeed = -1;
            for (auto const& [spellId, spell] : bot->GetSpellMap())
            {
                if (!spell || spell->State == PLAYERSPELL_REMOVED || !spell->Active)
                    continue;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                if (!info || !info->HasAura(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED))
                    continue;
                int32 speed = 0;
                for (uint8 effect = EFFECT_0; effect <= EFFECT_2; ++effect)
                    if (info->Effects[effect].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED)
                        speed = std::max(speed, info->Effects[effect].CalcValue());
                if (speed > bestSpeed)
                {
                    bestSpeed = speed;
                    mount = spellId;
                }
            }

            auto refuse = [&](char const* reason, uint32 result)
            {
                if (!member.flyRefusedLogged)
                {
                    member.flyRefusedLogged = true;
                    LogEvent(party, "fly_refused", JsonObject().Str("name", bot->GetName()).Str("reason", reason)
                                                       .Num("castResult", result).Num("map", bot->GetMapId())
                                                       .Real("x", bot->GetPositionX()).Real("y", bot->GetPositionY())
                                                       .Real("z", bot->GetPositionZ()));
                }
                member.flyCruiseZ = 0.0f;
                return false;
            };

            if (!mount)
                return refuse("no flying mount", 0);

            if (bot->IsMounted())
                bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
            if (bot->IsSitState())
                bot->SetStandState(UNIT_STAND_STATE_STAND);
            bot->StopMoving();

            // A normal cast: cast time, outdoor and flyable-zone checks (Cold Weather Flying in Northrend).
            SpellCastResult const result = bot->CastSpell(bot, mount, false);
            member.flyCastMs = now;
            if (result != SPELL_CAST_OK)
                return refuse("mount cast failed", uint32(result));
            return true;
        }

        // Mounted: the same movement flags the bot AI sets for a flying mount (MovementAction::UpdateMovementState).
        if (!bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING))
        {
            bot->AddUnitMovementFlag(MOVEMENTFLAG_CAN_FLY);
            bot->AddUnitMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY);
            bot->AddUnitMovementFlag(MOVEMENTFLAG_FLYING);
            bot->SendMovementFlagUpdate();
        }

        if (member.flyCruiseZ <= 0.0f)
        {
            member.flyCruiseZ = std::max(bot->GetPositionZ(), portal->z) + FLY_CRUISE_HEIGHT;
            member.flyStage = 0;
            member.flyApproach.Relocate(portal->x, portal->y, portal->z);
            // The descent beside the portal, not onto it (ROUTE_FLY_SIDE_APPROACH). Mechanar wave 10: a walkable roof
            // at z ~283 over the portal (navmesh_probe 530 --at 2857.9 1541.9 284): the four flyers descended onto
            // it, stood there 18 minutes ("corpse run did not finish in 20 minutes"). A player flies in from the
            // side the portal faces: a point FLY_SIDE_DISTANCE out along the trigger's thin axis, either side,
            // with a clear drop from cruise height and a clear line into the trigger.
            if (ROUTE_FLY_SIDE_APPROACH)
            {
                Map* map = bot->GetMap();
                float const approachZ = portal->z + 2.0f;
                for (float const side : {1.0f, -1.0f})
                {
                    float const ax = portal->x + std::cos(portal->orientation) * FLY_SIDE_DISTANCE * side;
                    float const ay = portal->y + std::sin(portal->orientation) * FLY_SIDE_DISTANCE * side;
                    LineOfSightChecks const checks = LINEOFSIGHT_ALL_CHECKS;
                    if (map->isInLineOfSight(ax, ay, member.flyCruiseZ, ax, ay, approachZ, bot->GetPhaseMask(), checks,
                                             VMAP::ModelIgnoreFlags::Nothing) &&
                        map->isInLineOfSight(ax, ay, approachZ, portal->x, portal->y, approachZ, bot->GetPhaseMask(),
                                             checks, VMAP::ModelIgnoreFlags::Nothing))
                    {
                        member.flyApproach.Relocate(ax, ay, approachZ);
                        break;
                    }
                }
            }
            LogEvent(party, "fly_to_portal", JsonObject().Str("name", bot->GetName()).Num("trigger", portal->entry)
                                                 .Real("distance", bot->GetExactDist(portal->x, portal->y, portal->z))
                                                 .Real("cruiseZ", member.flyCruiseZ)
                                                 .Real("approachX", member.flyApproach.GetPositionX())
                                                 .Real("approachY", member.flyApproach.GetPositionY()));
        }

        // The bot AI is held while the harness flies it (it would dismount at cruise height).
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            botAI->SetNextCheckDelay(LINK_HOLD_MS);

        if (bot->isMoving())
            return true;

        // Beside the portal, at its height: straight into the trigger.
        float const approachFlat = bot->GetExactDist2d(&member.flyApproach);
        if (member.flyStage == 0 && approachFlat <= FLY_ARRIVE_DISTANCE &&
            bot->GetPositionZ() <= member.flyApproach.GetPositionZ() + 3.0f)
            member.flyStage = 1;
        if (member.flyStage == 1)
        {
            bot->GetMotionMaster()->MovePoint(0, portal->x, portal->y, portal->z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                              false, true);
            return true;
        }

        // Climb, cross at altitude, descend at the approach point: a straight line from the ground would cut through
        // the floating islands.
        float const flat = approachFlat;
        if (flat > FLY_ARRIVE_DISTANCE)
        {
            if (bot->GetPositionZ() < member.flyCruiseZ - 3.0f)
                bot->GetMotionMaster()->MovePoint(0, bot->GetPositionX(), bot->GetPositionY(), member.flyCruiseZ,
                                                  FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, true);
            else
                bot->GetMotionMaster()->MovePoint(0, member.flyApproach.GetPositionX(),
                                                  member.flyApproach.GetPositionY(), member.flyCruiseZ,
                                                  FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, true);
        }
        else
            bot->GetMotionMaster()->MovePoint(0, member.flyApproach.GetPositionX(), member.flyApproach.GetPositionY(),
                                              member.flyApproach.GetPositionZ(), FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                              false, true);
        return true;
    }

    void TestHarnessMgr::LeaveWater(Party& party, std::vector<Player*> const& players, bool resting)
    {
        // [lead107] the leader's (RouteWater.h)
        PartyLeaderGroup group(*this, party);
        Leader::Travel::LeaveWater(group, players, resting);
    }

    bool TestHarnessMgr::UseItem(Player* bot, uint32 itemId, uint32 spellOverride)
    {
        if (!bot || !itemId || !bot->IsAlive() || bot->IsNonMeleeSpellCast(false))
            return false;

        Item* item = bot->GetItemByEntry(itemId);
        if (!item || bot->CanUseItem(item) != EQUIP_ERR_OK)
            return false;

        ItemTemplate const* proto = item->GetTemplate();
        uint32 spellId = spellOverride;
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS && !spellId; ++i)
            if (proto->Spells[i].SpellId > 0 && proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                spellId = uint32(proto->Spells[i].SpellId);
        if (!spellId)
            return false;

        if (bot->HasSpellCooldown(spellId))
            return false;

        WorldPacket packet(CMSG_USE_ITEM);
        packet << item->GetBagSlot() << item->GetSlot() << uint8(1) << spellId << item->GetGUID() << uint32(0)
               << uint8(0);
        packet << uint32(TARGET_FLAG_NONE) << bot->GetPackGUID();
        bot->GetSession()->HandleUseItemOpcode(packet);
        return true;
    }

    bool TestHarnessMgr::UseEquipped(Player* bot, uint8 slot, uint32 spellId)
    {
        if (!bot || !bot->IsAlive())
            return false;

        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || bot->HasSpellCooldown(spellId))
            return false;

        WorldPacket packet(CMSG_USE_ITEM);
        packet << uint8(INVENTORY_SLOT_BAG_0) << slot << uint8(1) << spellId << item->GetGUID() << uint32(0)
               << uint8(0);
        packet << uint32(TARGET_FLAG_NONE) << bot->GetPackGUID();
        bot->GetSession()->HandleUseItemOpcode(packet);
        return true;
    }

    bool TestHarnessMgr::UseConsumables(Party& party, Player* bot, bool food)
    {
        Member const* member = FindMember(party, bot->GetGUID());
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!member || !botAI || !bot->IsAlive() || bot->IsInCombat())
            return false;

        RoleConsumables const role = ConsumablesFor(*member->spec, party.level);
        if (!food)
        {
            if (!role.flask || HasFlaskAura(bot) || !UseItem(bot, role.flask))
                return false;
            LogEvent(party, "consumable", JsonObject().Str("name", bot->GetName()).Str("kind", "flask")
                                              .Num("item", role.flask));
            return true;
        }

        if (HasWellFed(bot))
            return false;

        if (HARNESS_MOVES_BOTS)
            bot->StopMoving();
        if (!UseItem(bot, role.food))
            return false;
        botAI->SetNextCheckDelay(11 * IN_MILLISECONDS);  // sit through the ten seconds of eating
        LogEvent(party, "consumable", JsonObject().Str("name", bot->GetName()).Str("kind", "food")
                                          .Num("item", role.food));
        return true;
    }

    namespace
    {
        // Buffing before a pull (TickBuffing): every step ends on its condition; the old fixed timers (15 s buffs,
        // 2 s, 12 s food, 10 s ready check: 39 s before every boss, RFC 5B-1 Taragaman / Jergosh / Bazzalan) are
        // upper bounds only.
        constexpr uint32 BUFF_STEP_MAX_MS = 15 * IN_MILLISECONDS;
        constexpr uint32 BUFF_STEP_MIN_MS = 1500;                   // the bots' first ticks with the window open
        constexpr uint32 BUFF_IDLE_GRACE_MS = 3 * IN_MILLISECONDS;  // nobody buffs, a givable buff still missing
        constexpr uint32 FLASK_GCD_MS = 1600;                       // a flask used: its global cooldown before food
        constexpr uint32 FOOD_STEP_MIN_MS = IN_MILLISECONDS;        // the food just used shows its aura
        constexpr uint32 FOOD_STEP_MAX_MS = 12 * IN_MILLISECONDS;
        constexpr uint32 READY_STEP_MIN_MS = 1500;  // the ready check reaches the bots on their next ticks
        constexpr uint32 READY_STEP_MAX_MS = 10 * IN_MILLISECONDS;
        // [ready_reply_now] mirrors ReadyCheckAction: the bots answer a ready check when it reaches them, so the
        // harness waits READY_STEP_MIN_MS for the packet only (the 10 s "no answer from ..." of DOWNTIME_AUDIT).
        constexpr bool READY_REPLY_ON_RECEIPT = true;
        // [single_ready_check] User rule 2026-10-25: one ready check per boss. With the bots' raid leader layer
        // running the boss's own ready check (RaidLeader "pull hold"), the harness skips its step-4 check before a
        // boss (brd-n Gerstahn: buffing 30 s, then the leader's pull hold 22 s more with everyone at 100 %).
        constexpr bool HARNESS_SINGLE_READY_CHECK = true;
        // A whole buffing pass at most (the step caps add up to ~39 s; Stockade heroic 2026-10-25 counted 125 s idle
        // ending in buffing): past it the ready check ends the pass now.
        constexpr uint32 BUFF_TOTAL_MAX_MS = 30 * IN_MILLISECONDS;
        constexpr float BUFF_GROUP_RANGE = 40.0f;   // members farther from the leader neither give nor wait for buffs

        enum class BuffTargets : uint8
        {
            All,
            ManaUsers,
            Physical
        };

        // A buff the group gives when a member of the class knows any rank of one of `spells` at its level (a level
        // 15 paladin has no Kings, a level 15 priest no Shadow Protection); any aura named like `auras` counts.
        struct GroupBuffDef
        {
            uint8 casterClass;
            std::vector<uint32> spells;  // first ranks
            std::vector<char const*> auras;
            BuffTargets targets;
            char const* label;
        };

        std::vector<GroupBuffDef> const groupBuffs = {
            {CLASS_PRIEST, {1243}, {"Fortitude"}, BuffTargets::All, "Fortitude"},
            {CLASS_PRIEST, {14752}, {"Divine Spirit", "Prayer of Spirit"}, BuffTargets::All, "Divine Spirit"},
            {CLASS_PRIEST, {976}, {"Shadow Protection"}, BuffTargets::All, "Shadow Protection"},
            {CLASS_DRUID, {1126}, {"of the Wild"}, BuffTargets::All, "Mark of the Wild"},
            {CLASS_MAGE, {1459}, {"Intellect", "Brilliance"}, BuffTargets::ManaUsers, "Arcane Intellect"},
            // One blessing per paladin and target, whichever its blessing strategy gives.
            {CLASS_PALADIN, {19740, 19742, 20217, 20911}, {"Blessing of"}, BuffTargets::All, "a Blessing"},
            {CLASS_WARRIOR, {6673, 469}, {"Battle Shout", "Commanding Shout", "Blessing of Might"}, BuffTargets::All,
             "Battle/Commanding Shout"},
            {CLASS_DEATH_KNIGHT, {57330}, {"Horn of Winter", "Strength of Earth"}, BuffTargets::Physical,
             "Horn of Winter"},
        };

        bool KnowsAnyRank(Player* caster, std::vector<uint32> const& firstRanks)
        {
            for (uint32 const first : firstRanks)
                for (uint32 spell = first; spell; spell = sSpellMgr->GetNextSpellInChain(spell))
                    if (caster->HasSpell(spell))
                        return true;
            return false;
        }

        bool InBuffRange(Player* bot, Player* leader)
        {
            return bot->IsAlive() && bot->IsInWorld() && bot->GetMap() == leader->GetMap() && !bot->IsInCombat() &&
                   bot->GetDistance(leader) <= BUFF_GROUP_RANGE;
        }

        // "<member> <buff>, ..." for every buff the group can give that a member lacks; empty: all present.
        std::string MissingGivableBuffs(Party const& party, std::vector<Player*> const& players, Player* leader)
        {
            std::string missing;
            for (GroupBuffDef const& def : groupBuffs)
            {
                bool givable = false;
                for (Player* caster : players)
                    if (!givable && caster->getClass() == def.casterClass && InBuffRange(caster, leader) &&
                        KnowsAnyRank(caster, def.spells))
                        givable = true;
                if (!givable)
                    continue;

                for (Player* bot : players)
                {
                    Member const* member = FindMember(party, bot->GetGUID());
                    if (!member || !member->spec || !InBuffRange(bot, leader))
                        continue;
                    Role const role = member->spec->role;
                    if ((def.targets == BuffTargets::ManaUsers && !UsesMana(role, member->cls)) ||
                        (def.targets == BuffTargets::Physical && role != Role::Melee && role != Role::Ranged &&
                         role != Role::Tank))
                        continue;
                    bool has = false;
                    for (char const* aura : def.auras)
                        has = has || HasAuraNamed(bot, aura);
                    if (!has)
                        missing += (missing.empty() ? "" : ", ") + bot->GetName() + " " + def.label;
                }
            }
            return missing;
        }

        // Who still buffs: casting, its rebuff window open (ForceRebuffState closes it once no buff trigger fires),
        // or walking (to a buff target out of range).
        std::string BuffWorkers(std::vector<Player*> const& players, Player* leader)
        {
            std::string workers;
            for (Player* bot : players)
            {
                if (!InBuffRange(bot, leader))
                    continue;
                PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                char const* what = nullptr;
                if (bot->IsNonMeleeSpellCast(false))
                    what = "casting";
                else if (botAI && botAI->forceRebuff.IsPending())
                    what = "rebuff window open";
                else if (bot != leader && bot->isMoving())
                    what = "moving";
                if (what)
                    workers += (workers.empty() ? "" : ", ") + bot->GetName() + " " + what;
            }
            return workers;
        }

        // Eating for Well Fed: sitting with food and no Well Fed yet.
        std::string Eaters(std::vector<Player*> const& players, Player* leader)
        {
            std::string eaters;
            for (Player* bot : players)
                if (InBuffRange(bot, leader) && RestState::IsEating(bot) && !HasWellFed(bot))
                    eaters += (eaters.empty() ? "" : ", ") + bot->GetName();
            return eaters;
        }

        // A member answers the ready check by itself: at once, or with the "force rebuff" strategy when the rebuff
        // window the check opened (Begin(true): the reply flag, kept after End) closes (ReadyReplyAction). The
        // buffing step's Begin(false) cleared the flag of a closed window before.
        bool AnsweredReadyCheck(Player* bot)
        {
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            // [ready_reply_now] the bot answers on receipt (ReadyCheckAction), its rebuff pass runs after the answer
            if (READY_REPLY_ON_RECEIPT)
                return true;
            if (!botAI || !sPlayerbotAIConfig.ForceRebuffOnReadyCheck ||
                !botAI->HasStrategy("force rebuff", BOT_STATE_NON_COMBAT))
                return true;
            return botAI->forceRebuff.ShouldReplyToReadyCheck() && !botAI->forceRebuff.IsPending();
        }

        // The current buffing step of a party: what it waited for, the idle clock.
        struct BuffWait
        {
            std::string waitedFor;
            uint32 idleSinceMs = 0;
            bool consumableUsed = false;  // step 1 -> 2: a flask was used; step 2 -> 3: food was used
        };
        std::unordered_map<uint32, BuffWait> buffWaits;  // per instance, world thread only
    }

    // [lead107 stage 5] the test setup of a buffing pass (stays in the harness): the run start puts everyone full
    void TestHarnessMgr::BuffPassSetup(Party& party, std::vector<Player*> const& players)
    {
        // [lead59] b106: the run started with members at 50 % health / mana (the last run's state) - the
        // run start is test setup: everyone full
        if (party.buffReason == "run start")
            for (Player* bot : players)
                if (bot->IsAlive())
                {
                    // [lead67] PerfMon b112: "new rpg do quest" ran in the led dungeon bots - off for the run
                    if (PlayerbotAI* runAI = GET_PLAYERBOT_AI(bot))
                        runAI->ChangeStrategy("-new rpg,-rpg,-quest,-grind,-travel", BOT_STATE_NON_COMBAT);
                    bot->SetFullHealth();
                    if (bot->GetMaxPower(POWER_MANA))
                        bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));
                }
    }

    void TestHarnessMgr::TickBuffing(Party& party, std::vector<Player*> const& players, Player* leader)
    {
        uint32 const now = getMSTime();
        TestRunner::ClearWaypoint(party.leader);
        // A step done goes on to the next one in the same tick while that one has nothing to wait for (no flask, no
        // food): nothing missing takes about 4 s.
        for (uint32 pass = 0; pass < 4 && party.needBuff; ++pass)
        {
            if (party.buffStep > 0 && party.buffStep < 4 && getMSTimeDiff(party.buffSinceMs, now) > BUFF_TOTAL_MAX_MS)
            {
                LOG_INFO("playerbots", "[buffing] {}: pass over {} s at step {}: to the ready check end", party.id,
                         BUFF_TOTAL_MAX_MS / IN_MILLISECONDS, uint32(party.buffStep));
                party.buffStep = 4;
                party.buffStepMs = now - READY_STEP_MAX_MS - 1;
            }
            uint8 const stepBefore = party.buffStep;
            uint32 const stepMs = getMSTimeDiff(party.buffStepMs, now);
            BuffWait& wait = buffWaits[party.instanceId];

            // The step is over: its line ("[buffing] step N done after X s: why"), the next step's clock.
            auto const stepDone = [&](uint8 next, std::string const& why)
            {
                float const sec = stepMs / 1000.0f;
                LOG_INFO("playerbots", "[buffing] step {} done after {:.1f} s: {}{} ({}, {})", uint32(party.buffStep),
                         sec, why, wait.waitedFor.empty() ? std::string() : "; waited for " + wait.waitedFor,
                         party.id, party.buffReason);
                LogEvent(party, "buff_step",
                         JsonObject().Num("step", party.buffStep).Real("sec", sec).Str("why", why)
                             .Str("waitedFor", wait.waitedFor).Str("reason", party.buffReason));
                party.buffStep = next;
                party.buffStepMs = now;
                bool const consumableUsed = wait.consumableUsed;
                wait = BuffWait();
                wait.consumableUsed = consumableUsed;
            };
            auto const waitingFor = [&wait](std::string const& what)
            {
                if (!what.empty())
                    wait.waitedFor = what;
            };

            switch (party.buffStep)
            {
                case 0:
                    BuffPassSetup(party, players);  // [lead107 stage 5] the test setup of a buffing pass
                    for (Player* bot : players)
                        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                            if (bot->IsAlive() && !bot->IsInCombat() &&
                                botAI->HasStrategy("force rebuff", BOT_STATE_NON_COMBAT))
                                botAI->forceRebuff.Begin(false);
                    LogEvent(party, "buffing", JsonObject().Str("reason", party.buffReason));
                    party.lastEvent = "buffing: " + party.buffReason;
                    party.buffStep = 1;
                    party.buffStepMs = now;
                    party.buffSinceMs = now;
                    wait = BuffWait();
                    break;
                case 1:
                {
                    // Buffs: nobody casts, walks to buff or keeps its rebuff window open, and every buff the group can
                    // give is on every member it is for. Nobody buffing while one is missing ends it after the grace.
                    std::string const workers = BuffWorkers(players, leader);
                    std::string const missing = MissingGivableBuffs(party, players, leader);
                    waitingFor(workers.empty() ? missing : workers);
                    if (!workers.empty())
                        wait.idleSinceMs = 0;
                    else if (!wait.idleSinceMs)
                        wait.idleSinceMs = now;
                    std::string why;
                    if (stepMs >= BUFF_STEP_MAX_MS)
                        why = "upper bound " + std::to_string(BUFF_STEP_MAX_MS / IN_MILLISECONDS) + " s" +
                              (workers.empty() ? std::string() : ", still buffing: " + workers) +
                              (missing.empty() ? std::string() : ", missing: " + missing);
                    // [ready_instant] User 2026-10-25 («ботам зачем проверять готовность? просто стоять и тупить»):
                    // nothing missing and nobody buffing ends the step at once, no BUFF_STEP_MIN_MS pause
                    else if (workers.empty() && missing.empty())
                        why = "every buff the group can give is present";
                    else if (stepMs >= BUFF_STEP_MIN_MS && workers.empty() &&
                             getMSTimeDiff(wait.idleSinceMs, now) >= BUFF_IDLE_GRACE_MS)
                        why = "nobody buffs any more, missing: " + missing;
                    if (why.empty())
                        break;
                    stepDone(2, why);
                    for (Player* bot : players)
                        wait.consumableUsed = UseConsumables(party, bot, false) || wait.consumableUsed;
                    break;
                }
                case 2:
                {
                    // A flask's global cooldown, then the food.
                    if (wait.consumableUsed && stepMs < FLASK_GCD_MS)
                    {
                        waitingFor("flask global cooldown");
                        break;
                    }
                    uint32 eating = 0;
                    for (Player* bot : players)
                        eating += UseConsumables(party, bot, true) ? 1 : 0;
                    stepDone(3, eating ? std::to_string(eating) + " member(s) sat down to eat"
                                       : std::string("Well Fed on everyone, no food needed"));
                    wait.consumableUsed = eating != 0;  // step 3: somebody eats
                    break;
                }
                case 3:
                {
                    // Food: only while somebody actually eats and has no Well Fed yet.
                    std::string const eaters = Eaters(players, leader);
                    waitingFor(eaters.empty() ? std::string() : "eating: " + eaters);
                    std::string why;
                    if (stepMs >= FOOD_STEP_MAX_MS)
                        why = "upper bound " + std::to_string(FOOD_STEP_MAX_MS / IN_MILLISECONDS) + " s" +
                              (eaters.empty() ? std::string() : ", still eating: " + eaters);
                    else if (eaters.empty() && (!wait.consumableUsed || stepMs >= FOOD_STEP_MIN_MS))
                        why = wait.consumableUsed ? "Well Fed on everyone who ate" : "nobody eats";
                    if (why.empty())
                        break;
                    stepDone(4, why);
                    if (HARNESS_SINGLE_READY_CHECK && party.buffReason.rfind("before ", 0) == 0 &&
                        RaidLeader::FindCoordinator(leader))
                        LogEvent(party, "single_ready_check",
                                 JsonObject().Str("reason", party.buffReason).Str("by", "raid leader"));
                    else if (leader->GetGroup() && leader->GetGroup()->IsLeader(leader->GetGUID()))
                    {
                        WorldPacket packet(MSG_RAID_READY_CHECK);
                        leader->GetSession()->HandleRaidReadyCheckOpcode(packet);
                        LogEvent(party, "ready_check", JsonObject().Str("reason", party.buffReason));
                    }
                    break;
                }
                default:
                {
                    // Ready check: every member answered (AnsweredReadyCheck).
                    std::string unanswered;
                    for (Player* bot : players)
                        if (bot != leader && InBuffRange(bot, leader) && !AnsweredReadyCheck(bot))
                            unanswered += (unanswered.empty() ? "" : ", ") + bot->GetName();
                    waitingFor(unanswered.empty() ? std::string() : "ready answers: " + unanswered);
                    std::string why;
                    if (HARNESS_SINGLE_READY_CHECK && party.buffReason.rfind("before ", 0) == 0 &&
                        RaidLeader::FindCoordinator(leader))
                        why = "the raid leader runs the boss's ready check (single_ready_check)";
                    else if (stepMs >= READY_STEP_MAX_MS)
                        why = "upper bound " + std::to_string(READY_STEP_MAX_MS / IN_MILLISECONDS) + " s" +
                              (unanswered.empty() ? std::string() : ", no answer from " + unanswered);
                    // [ready_instant] the check is a check, not a pause: the bots answer on receipt
                    // (READY_REPLY_ON_RECEIPT), so with every member in range and nobody below its own rest line the
                    // group goes on in the same tick (else the READY_STEP_MIN_MS end below, as before)
                    else if (READY_REPLY_ON_RECEIPT && unanswered.empty() &&
                             std::none_of(players.begin(), players.end(),
                                          [leader](Player* bot)
                                          {
                                              return bot != leader &&
                                                     (!bot->IsAlive() || !InBuffRange(bot, leader) ||
                                                      RestState::WantsDrink(bot) || RestState::WantsFood(bot));
                                          }))
                        why = "nothing missing: buffs present, everyone in range and above its rest line";
                    else if (stepMs >= READY_STEP_MIN_MS && unanswered.empty())
                        why = "every member answered the ready check";
                    if (why.empty())
                        break;
                    stepDone(0, why);
                    buffWaits.erase(party.instanceId);
                    {
                        JsonArray state;
                        for (Player* bot : players)
                            state.Raw(MemberBuffJson(party, bot));
                        LogEvent(party, "buffed",
                                 JsonObject().Str("reason", party.buffReason).Raw("members", state.Build()));
                    }
                    party.needBuff = false;
                    party.buffStep = 0;
                    party.lastProgressMs = now;
                    SetPhase(party, Phase::Traveling);
                    break;
                }
            }
            if (party.buffStep == stepBefore)
                break;
        }
    }

    void TestHarnessMgr::PrePot(Party& party, std::vector<Player*> const& players)
    {
        if (!party.targetIsBoss || party.target.IsEmpty())
            return;

        Player* leader = ObjectAccessor::FindPlayer(party.leader);
        Creature* boss = leader ? leader->GetMap()->GetCreature(party.target) : nullptr;
        if (!boss || boss->IsInCombat())
            return;

        float const distance = leader->GetExactDist(boss);
        if (distance < BOSS_BUFF_DISTANCE && !party.bossBuffed.count(party.target))
        {
            party.bossBuffed.insert(party.target);
            // Black Morass: the shield drains during buffs + ready check (50 -> 30 % before Temporus, 33 -> 11 % before
            // Deja in bm-n 2026-10-25); a rift boss is pulled at once.
            if (party.watchedRift > 0)
                return;
            // [brd_seven_fast] a scripted wave that just became attackable: no buff pass / ready check, pulled at
            // once (the Tomb of the Seven resets after 15 s with no dwarf in combat).
            if (EventWaveFresh(party.instanceId, party.target))
            {
                LogEvent(party, "event_wave_no_ready_check",
                         JsonObject().Str("name", boss->GetName()).Num("entry", boss->GetEntry()));
                return;
            }
            party.needBuff = true;
            party.buffStep = 0;
            party.buffReason = std::string("before ") + boss->GetName();
            return;
        }

        if (distance > BOSS_PREPOT_DISTANCE || party.prePotted.count(party.target) ||
            LeaderLayerHandlesConsumables(party))
            return;

        party.prePotted.insert(party.target);
        JsonArray users;
        for (Player* bot : players)
        {
            Member const* member = FindMember(party, bot->GetGUID());
            if (member && UseItem(bot, ConsumablesFor(*member->spec, party.level).prePot))
                users.Str(bot->GetName());
        }
        LogEvent(party, "prepot", JsonObject().Str("boss", boss->GetName()).Raw("users", users.Build()));
    }

    void TestHarnessMgr::LogCannotMove(Party& party, Player* bot, std::string const& capability,
                                       std::string const& reason, float toX, float toY, float toZ)
    {
        if (!bot)
            return;
        uint32 const now = getMSTime();
        uint32& last = party.cannotMoveLoggedMs[bot->GetName() + "|" + capability + "|" + reason];
        if (last && getMSTimeDiff(last, now) < CANNOT_MOVE_LOG_MS)
            return;
        last = now;
        ++party.cannotMoveEvents;
        bool const hasTarget = toX != 0.0f || toY != 0.0f || toZ != 0.0f;
        LogEvent(party, "bot_cannot_move",
                 JsonObject().Str("name", bot->GetName()).Str("capability", capability).Str("reason", reason)
                     .Bool("alive", bot->IsAlive()).Bool("ghost", bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
                     .Num("map", bot->GetMapId()).Real("x", bot->GetPositionX()).Real("y", bot->GetPositionY())
                     .Real("z", bot->GetPositionZ()).Real("toX", toX).Real("toY", toY).Real("toZ", toZ)
                     .Real("distance", hasTarget ? bot->GetExactDist(toX, toY, toZ) : 0.0f)
                     .Num("count", party.cannotMoveEvents));
    }

    void TestHarnessMgr::DrainMoveReports(Party& party, std::vector<Player*> const& players)
    {
        for (Player* bot : players)
        {
            SelfTravel::MoveReport report;
            if (SelfTravel::TakeReport(bot->GetGUID(), report))
                LogCannotMove(party, bot, report.action, report.reason, report.toX, report.toY, report.toZ);

            // [shside] obstacle_sidestep, obstacle_sidestep_failed, move_conflict, waypoint_lane_veto
            // (PATCH_shaman_sidestep.md, applied by plan29)
            BotMoveEvents::Event moveEvent;
            while (BotMoveEvents::Take(bot->GetGUID(), moveEvent))
            {
                JsonObject fields;
                for (auto const& [key, value] : moveEvent.strs)
                    fields.Str(key.c_str(), value);
                for (auto const& [key, value] : moveEvent.nums)
                    fields.Real(key.c_str(), value);
                LogEvent(party, moveEvent.type, fields);
            }
        }
    }

    // [lead107] the order reports live in the leader (Ai/Leader/Dungeon/LeaderReports.h)
    void TestHarnessMgr::DrainOrderReports(Party& party, Player* leader)
    {
        PartyLeaderGroup group(*this, party);
        Leader::Dungeon::DrainOrderReports(group, leader);
    }

    // [lead3] .. [lead10] C:/WoW/LEADER_REFACTOR_PLAN.md section 9: the dungeon leader's own decisions around its
    // planner library. The leader decides what and how; the tank only executes its PullOrder. Every function here is
    // the world thread's (the harness tick, no map thread runs).

    bool TestHarnessMgr::LeaderDecides(Party const& party) const { return LeaderDecidesFor(party); }

    // [lead107] the leader's planner tick lives in the leader (Ai/Leader/Dungeon/LeaderPlan.h)
    std::string TestHarnessMgr::LeaderReadyWhy(Party& party, Player* leader)
    {
        PartyLeaderGroup group(*this, party);
        return Leader::Dungeon::LeaderReadyWhy(group, leader);
    }

    bool TestHarnessMgr::LeaderNeedsPack(Party const& party, Map* map, WorldObject const* object,
                                         std::string& why) const
    {
        if (!object)
            return false;
        if (IsRouteBlocker(party, object->GetGUID()))
        {
            why = "route blocker";
            return true;
        }
        return RouteBlockerBehindRequired(party, map, object, why);
    }

    void TestHarnessMgr::LeaderWholePack(Party& party, Player* leader, Creature* pack, std::string const& why,
                                         bool walkIn, bool required)
    {
        PartyLeaderGroup group(*this, party);
        Leader::Dungeon::LeaderWholePack(group, leader, pack, why, walkIn, required);
    }

    void TestHarnessMgr::LeaderBossPull(Party& party, Player* leader, Creature* boss, std::string const& why)
    {
        PartyLeaderGroup group(*this, party);
        Leader::Dungeon::LeaderBossPull(group, leader, boss, why);
    }

    bool TestHarnessMgr::LeaderNoOption(Party& party, Player* leader, Unit* mob, std::vector<ObjectGuid> const& pack,
                                        std::string const& why, bool stops, JsonObject fields)
    {
        PartyLeaderGroup group(*this, party);
        return Leader::Dungeon::LeaderNoOption(group, leader, mob, pack, why, stops, std::move(fields));
    }

    // [lead107] the leader's patrol decision lives in the leader (Ai/Leader/Travel/RoutePatrol.h)
    bool TestHarnessMgr::LeaderPatrolTick(Party& party, Player* leader)
    {
        PartyLeaderGroup group(*this, party);
        return Leader::Travel::LeaderPatrolTick(group, leader);
    }

    // [lead15] C:/WoW/LEADER_REFACTOR_PLAN.md section 9 / user 2026-10-26: the leader issues events to EVERY member
    // (LeaderOrders.h Events), on every update while urgent (else every 250 ms), and the members decide only how.
    // A changed event is a decision line (leader_event, the member named) and wakes the member's AI at once
    // (member_wake); the members' acks are drained by DrainOrderReports (member_ack). Events this step issues:
    // - Rest (eat / drink to full) to whoever needs it, Ready? while the pull waits for the assigned spots, Prepare
    //   (buffs, forms, a resurrection) while the group stands;
    // - StandAt (the pull's spots), MoveTo along with the tank (the member's own follow is off), MoveTo out of a
    //   cleave cone (DungeonTactics::InFrontalCleave), MoveTo of the healer for the heal reach;
    // - in the fight: Heal(group) to the healer, Attack(target, killOrder, aoe rule) and Intensity to every damage
    //   dealer, Interrupt (one kicker per casting mob), External (Misdirection / Tricks, Pain Suppression, Hand of
    //   Sacrifice / Protection, Innervate, Hymn of Hope, Divine Hymn, Power Infusion: who and when).
    // The executor side: LeaderTriggers / LeaderActions, RaidLeaderHoldMultiplier, PlayerbotAI::DoNextAction (no
    // command, out of combat: no engine), Ai/Base/Util/LeaderCommand.h (the rotation's getters).
    namespace
    {
        // [lead107] the leader's checks live in the leader (Ai/Leader/Fight/LeaderFightChecks.h)
        using Leader::Fight::AttackerDps;
        using Leader::Fight::HealReaches;
        using Leader::Fight::IsBossLook;
        using Leader::Fight::LEADER_AOE_GUARD_YD;
        using Leader::Fight::LEADER_AOE_RADIUS;
        using Leader::Fight::LEADER_AOE_RECHECK_MS;
        using Leader::Fight::LEADER_CALM_TICK_MS;
        using Leader::Fight::LEADER_FIGHT_TICK_MS;
        using Leader::Fight::LEADER_FOLLOW_START_YD;
        using Leader::Fight::LEADER_FOLLOW_TOLERANCE;
        using Leader::Fight::LEADER_HAZARD_DZ;
        using Leader::Fight::LEADER_HAZARD_EDGE;
        using Leader::Fight::LEADER_HAZARD_SCAN_MS;
        using Leader::Fight::LEADER_HAZARD_SCAN_YD;
        using Leader::Fight::LEADER_HEALER_YD;
        using Leader::Fight::LEADER_HEAL_REACH_CHECK_MS;
        using Leader::Fight::LEADER_HEAL_REACH_LOG_MS;
        using Leader::Fight::LEADER_LOOSE_TANK_YD;
        using Leader::Fight::LEADER_OPENING_MS;
        using Leader::Fight::LEADER_PATROL_GROUP_YD;
        using Leader::Fight::LEADER_RANGED_MAX_YD;
        using Leader::Fight::LEADER_RANGED_MIN_YD;
        using Leader::Fight::LEADER_RANGED_YD;
        using Leader::Fight::LEADER_SAVE_HP_PCT;
        using Leader::Fight::LEADER_SAVE_MANA_PCT;
        using Leader::Fight::LEADER_SPOT_LOS_MS;
        using Leader::Fight::LEADER_TANK_STILL_MS;
        using Leader::Fight::LEADER_TERRAIN_EDGE;
        using Leader::Fight::LEADER_WAKE_LOG_MS;
        using Leader::Fight::LeaderAggroAt;
        using Leader::Fight::LeaderAggroOnWalk;
        using Leader::Fight::LeaderAoeRule;
        using Leader::Fight::LeaderCandidate;
        using Leader::Fight::LeaderCleaveExit;
        using Leader::Fight::LeaderCollectHazards;
        using Leader::Fight::LeaderCombatSpot;
        using Leader::Fight::LeaderCombatSpotBreaks;
        using Leader::Fight::LeaderCombatSpotValid;
        using Leader::Fight::LeaderEdgePick;
        using Leader::Fight::LeaderFightMob;
        using Leader::Fight::LeaderFollowSpot;
        using Leader::Fight::LeaderHealerRespot;
        using Leader::Fight::LeaderMeleeWait;
        using Leader::Fight::LeaderMemberWalks;
        using Leader::Fight::LeaderNearestCombatSpot;
        using Leader::Fight::LeaderOnCleared;
        using Leader::Fight::LeaderPatrolNear;
        using Leader::Fight::LeaderPullMobs;
        using Leader::Fight::LeaderReadyInterrupt;
        using Leader::Fight::LeaderReadySpell;
        using Leader::Fight::LeaderSafeSpot;
        using Leader::Fight::LeaderSpellReach;
        using Leader::Fight::LeaderSpotOk;
        using Leader::Fight::LeaderSpotSafe;
        using Leader::Fight::LeaderTauntReach;
        using Leader::Fight::LeaderWalkStaysOnFloor;
        using Leader::Fight::LeaderWalkVerdict;
        using Leader::Fight::LeaderZoneAt;
        using Leader::Fight::LogFightDrag;
        using Leader::Fight::MoveEventName;
        using Leader::Fight::RefusedCount;
        using Leader::Fight::SPELL_ARMY_OF_THE_DEAD;
        using Leader::Fight::SPELL_BLOODLUST;
        using Leader::Fight::SPELL_DIVINE_HYMN;
        using Leader::Fight::SPELL_HAND_OF_PROTECTION_1;
        using Leader::Fight::SPELL_HAND_OF_PROTECTION_2;
        using Leader::Fight::SPELL_HAND_OF_PROTECTION_3;
        using Leader::Fight::SPELL_HAND_OF_SACRIFICE;
        using Leader::Fight::SPELL_HEROISM;
        using Leader::Fight::SPELL_HYMN_OF_HOPE;
        using Leader::Fight::SPELL_INNERVATE;
        using Leader::Fight::SPELL_MISDIRECTION;
        using Leader::Fight::SPELL_PAIN_SUPPRESSION;
        using Leader::Fight::SPELL_POWER_INFUSION;
        using Leader::Fight::SPELL_TRICKS_OF_THE_TRADE;
        using Leader::Fight::SpotAcquire;
        using Leader::Fight::TANK_AOE_THREAT;
        using Leader::Fight::TANK_TAUNTS;
        using Leader::Fight::g_leaderCleared;
        using Leader::Fight::g_leaderExempt;
        using Leader::Fight::g_leaderIdle;
        using Leader::Fight::g_spotAcquireMargin;
    }

    bool TestHarnessMgr::LeaderAssignSpots(Party& party, Player* tank, PullPlanner::Decision const& decision)
    {
        PartyLeaderGroup group(*this, party);
        return Leader::Dungeon::LeaderAssignSpots(group, tank, decision);
    }

    std::string TestHarnessMgr::LeaderSpotsWait(Party& party, Player* tank)
    {
        PartyLeaderGroup group(*this, party);
        return Leader::Dungeon::LeaderSpotsWait(group, tank);
    }

    bool Party::FightOn(Player* leader) const
    {
        return inCombat || (leader && leader->IsInCombat() && !leader->getAttackers().empty());
    }

    // [lead107] the leader's event tick lives in the leader (Ai/Leader/Dungeon/LeaderEvents.h)
    void TestHarnessMgr::TickLeaderEvents(Party& party, Player* tank)
    {
        PartyLeaderGroup group(*this, party);
        Leader::Dungeon::TickLeaderEvents(group, tank);
    }

    void TestHarnessMgr::WaitedLong(Party& party, std::string const& kind, uint32 ms, std::string const& why)
    {
        PartyLeaderGroup group(*this, party);
        Leader::Dungeon::WaitedLong(group, kind, ms, why);
    }

    void TestHarnessMgr::LogHealReach(Party& party, Player* member, Player* healer, std::string const& why,
                                      std::string const& fix)
    {
        PartyLeaderGroup group(*this, party);
        Leader::Dungeon::LogHealReach(group, member, healer, why, fix);
    }

    void TestHarnessMgr::TickLeaderPlanner(Party& party)
    {
        PartyLeaderGroup group(*this, party);
        Leader::Dungeon::TickLeaderPlanner(group);
    }

    // [lead107] the leader's group view: the harness's route state behind it (the same reads and writes the
    // leader's code made on the Party before it moved)
    std::vector<Leader::LeaderGroup::MemberInfo> PartyLeaderGroup::Members() const
    {
        std::vector<MemberInfo> out;
        out.reserve(_party.members.size());
        for (Member const& member : _party.members)
            out.push_back({member.guid, member.name, member.apartIgnoreUntilMs,
                           member.spec && UsesMana(member.spec->role, member.cls), member.backInMs});
        return out;
    }

    int32 PartyLeaderGroup::DoorStarterFor(GameObject const* door) const
    {
        int32 doorStarter = -1;
        for (size_t index = 0; index < _party.starters.size() && doorStarter < 0; ++index)
        {
            StarterTrack const& starter = _party.starters[index];
            if (!starter.def || starter.uses >= starter.def->maxUses ||
                std::find(starter.def->entries.begin(), starter.def->entries.end(), door->GetEntry()) ==
                    starter.def->entries.end())
                continue;
            bool hasItem = !starter.def->itemId;
            for (Member const& member : _party.members)
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    !hasItem && bot && bot->IsAlive() && bot->HasItemCount(starter.def->itemId, 1))
                    hasItem = true;
            if (hasItem)
                doorStarter = int32(index);
        }
        return doorStarter;
    }

    std::string PartyLeaderGroup::StarterNote(int32 index) const { return _party.starters[size_t(index)].def->note; }

    bool PartyLeaderGroup::HasDoorWalk() const { return doorWalks.count(_party.leader) != 0; }

    void PartyLeaderGroup::StartDoorWalk(ObjectGuid door, ObjectGuid blocker, G3D::Vector3 const& point,
                                         uint32 untilMs)
    {
        doorWalks[_party.leader] = {door, blocker, point, untilMs};
    }

    bool PartyLeaderGroup::GetWaypoint(ObjectGuid bot, WalkPoint& out) const
    {
        TestRunner::Waypoint waypoint;
        if (!TestRunner::GetWaypoint(bot, waypoint))
            return false;
        out = {waypoint.mapId, waypoint.x,    waypoint.y,        waypoint.z,
               waypoint.serial, waypoint.jump, waypoint.straight, waypoint.gatePass};
        return true;
    }

    void PartyLeaderGroup::ClearWaypoint(ObjectGuid bot) { TestRunner::ClearWaypoint(bot); }

    void PartyLeaderGroup::SetBossOnTheWay(ObjectGuid boss, uint32 now)
    {
        _party.target = boss;
        _party.targetIsBoss = true;
        _party.targetIsStarter = false;
        _party.targetRouteRule = "boss on the way";
        _party.targetSinceMs = now;
        _party.arrivedMs = 0;
        _party.stopSinceMs = 0;
        _party.approachBlocker.Clear();
        _party.orderHoldSinceMs.erase(boss);
    }

    void PartyLeaderGroup::MarkRequired(ObjectGuid mob) { PartyMarkRequired(_party, mob); }

    void PartyLeaderGroup::NoteHopelessPack(Player* leader, Creature* mob)
    {
        TestHarness::NoteHopelessPack(leader, mob);
    }

    void PartyLeaderGroup::FailRun(std::string const& reason) { _mgr.Finish(_party, "failed", reason); }

    void PartyLeaderGroup::SetLeaderWaypoint(Player* leader, float x, float y, float z, bool straight)
    {
        _mgr.SetLeaderWaypoint(_party, leader, x, y, z, straight);
    }

    void PartyLeaderGroup::GiveWaypoint(Player* bot, float x, float y, float z, bool jump, bool straight)
    {
        TestHarness::GiveWaypoint(_party, bot, x, y, z, jump, straight);
    }

    ::InstanceScript* PartyLeaderGroup::InstanceScript() const { return PartyInstanceScript(_party); }

    void PartyLeaderGroup::MarkGateWaypoint(Player* bot) { TestHarness::MarkGateWaypoint(bot); }

    bool PartyLeaderGroup::GroupFullyReady(Player* leader) const
    {
        return TestHarness::GroupFullyReady(_party, leader);
    }

    bool PartyLeaderGroup::ScriptEventRunning() const { return TestHarness::ScriptEventRunning(_party); }

    bool PartyLeaderGroup::Levelling() const
    {
        return IsNormalDungeon(*_party.content) && _party.level < MAX_TEST_LEVEL;
    }

    bool PartyLeaderGroup::Regrouping() const { return regroups.count(_party.instanceId) > 0; }

    std::vector<std::reference_wrapper<Leader::BossFacts const>> PartyLeaderGroup::Bosses() const
    {
        std::vector<std::reference_wrapper<Leader::BossFacts const>> out;
        for (BossTrack const& boss : _party.bosses)
            out.push_back(std::cref(static_cast<Leader::BossFacts const&>(boss)));
        return out;
    }

    std::vector<uint32> const& PartyLeaderGroup::ContentBossEntries() const
    {
        static std::vector<uint32> const none;
        return _party.content ? _party.content->bossEntries : none;
    }

    bool PartyLeaderGroup::BossKilledFor(uint32 entry) const
    {
        bool done = false;
        for (BossTrack const& boss : _party.bosses)
            done |= boss.killed && (boss.entry == entry || std::find(boss.credits.begin(), boss.credits.end(),
                                                                     entry) != boss.credits.end());
        return done;
    }

    bool PartyLeaderGroup::BossesOpen() const
    {
        bool bossesOpen = false;
        for (BossTrack const& boss : _party.bosses)
            bossesOpen |= !boss.killed && !boss.failed;
        return bossesOpen;
    }

    void PartyLeaderGroup::SetMemberWaypoint(ObjectGuid member, Player* bot, float x, float y, float z)
    {
        for (Member& candidate : _party.members)
            if (candidate.guid == member)
                _mgr.SetMemberWaypoint(_party, candidate, bot, x, y, z);
    }

    void PartyLeaderGroup::ClearMemberWaypoint(ObjectGuid member)
    {
        for (Member& candidate : _party.members)
            if (candidate.guid == member)
                _mgr.ClearMemberWaypoint(candidate);
    }

    bool PartyLeaderGroup::MemberHasWaypoint(ObjectGuid member) const
    {
        for (Member const& candidate : _party.members)
            if (candidate.guid == member)
                return candidate.hasWaypoint;
        return false;
    }

    void PartyLeaderGroup::SetMemberHasPortalGoal(ObjectGuid member, bool has)
    {
        for (Member& candidate : _party.members)
            if (candidate.guid == member)
                candidate.hasPortalGoal = has;
    }

    void PartyLeaderGroup::SetMemberHasWaypoint(ObjectGuid member, bool has)
    {
        for (Member& candidate : _party.members)
            if (candidate.guid == member)
                candidate.hasWaypoint = has;
    }

    bool PartyLeaderGroup::BossEncounterInProgress() const
    {
        for (BossTrack const& boss : _party.bosses)
            if (boss.lastState == IN_PROGRESS)
                return true;
        return false;
    }

    void TestHarnessMgr::SetMemberWaypoint(Party& party, Member& member, Player* bot, float x, float y, float z)
    {
        if (bot->GetGUID() == party.leader)
            SetLeaderWaypoint(party, bot, x, y, z);
        else
            GiveWaypoint(party, bot, x, y, z);
        member.hasWaypoint = true;
    }

    void TestHarnessMgr::SetLeaderWaypoint(Party& party, Player* leader, float x, float y, float z, bool straight)
    {
        G3D::Vector3 point(x, y, z);
        ObjectGuid doorGuid;
        float walked = 0.0f;
        bool doorStop = LeaderDoorStop(leader, point, doorGuid, walked);
        // [door_hold_pass] EventStarterDef::passDoors: the escort's own doors (Gnomeregan: Emi opens / seals the Cave
        // Ins herself) - the waypoint as asked.
        if (doorStop && PassDoorOfActiveStarter(party, leader->GetMap(), doorGuid))
        {
            uint32 const passNow = getMSTime();
            if (!party.doorHoldLogMs || getMSTimeDiff(party.doorHoldLogMs, passNow) >= DOOR_HOLD_LOG_MS)
            {
                party.doorHoldLogMs = passNow ? passNow : 1;
                GameObject const* door = leader->GetMap()->GetGameObject(doorGuid);
                LogEvent(party, "door_hold_pass", JsonObject().Str("door", door ? door->GetName() : "?")
                                                      .Num("doorEntry", door ? door->GetEntry() : 0)
                                                      .Real("walked", walked).Real("toX", x).Real("toY", y)
                                                      .Real("toZ", z));
            }
            point = G3D::Vector3(x, y, z);
            doorStop = false;
        }
        if (doorStop)
        {
            uint32 const now = getMSTime();
            party.doorHold = doorGuid;
            party.doorHoldMs = now ? now : 1;
            if (!party.doorHoldLogMs || getMSTimeDiff(party.doorHoldLogMs, now) >= DOOR_HOLD_LOG_MS)
            {
                party.doorHoldLogMs = now ? now : 1;
                Map* map = leader->GetMap();
                GameObject* door = map ? map->GetGameObject(doorGuid) : nullptr;
                WorldObject* target = nullptr;
                if (!party.target.IsEmpty() && map)
                {
                    target = map->GetCreature(party.target);
                    if (!target)
                        target = map->GetGameObject(party.target);
                }
                bool const opens = door && target && DoorOpensFor(door, target);
                LOG_INFO("playerbots",
                         "[closed door] leader {}: waypoint ({:.1f},{:.1f},{:.1f}) runs through {} {} at {:.1f} yd -> "
                         "hold ({:.1f},{:.1f},{:.1f}), target {}{}",
                         leader->GetName(), x, y, z, door ? door->GetName() : "?", door ? door->GetEntry() : 0, walked,
                         point.x, point.y, point.z, target ? target->GetName() : "none", opens ? " (opens it)" : "");
                LogEvent(party, "door_hold",
                         JsonObject().Str("door", door ? door->GetName() : "?")
                             .Num("doorEntry", door ? door->GetEntry() : 0).Real("walked", walked)
                             .Str("target", target ? target->GetName() : "").Bool("targetOpensIt", opens)
                             .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                             .Real("z", leader->GetPositionZ()).Real("toX", x).Real("toY", y).Real("toZ", z)
                             .Real("holdX", point.x).Real("holdY", point.y).Real("holdZ", point.z));
            }
            straight = false;  // cut at the door: no longer the link's far end
        }
        GiveWaypoint(party, leader, point.x, point.y, point.z, false, straight);
    }

    void TestHarnessMgr::ClearMemberWaypoint(Member& member)
    {
        TestRunner::ClearWaypoint(member.guid);
        member.hasWaypoint = false;
    }

    void TestHarnessMgr::ClearMemberIntent(Party& party)
    {
        for (Member& member : party.members)
            if (member.guid != party.leader && member.hasWaypoint)
                ClearMemberWaypoint(member);
    }

    bool TestHarnessMgr::TickWingWalk(Party& party, std::vector<Player*> const& players, Player* leader)
    {
        if (!party.wingWalk || party.entranceIdx >= party.entrances.size())
        {
            party.wingWalk = false;
            return false;
        }

        uint32 const now = getMSTime();
        Position const& entrance = party.entrances[party.entranceIdx];
        bool everybody = true;
        for (Player* bot : players)
        {
            Member* member = nullptr;
            for (Member& candidate : party.members)
                if (candidate.guid == bot->GetGUID())
                    member = &candidate;
            if (!member || !member->hasPortalGoal)
                continue;
            if (bot->IsAlive() && bot->GetMapId() == party.content->mapId && !bot->IsBeingTeleported() &&
                bot->GetExactDist2d(&entrance) <= WING_ARRIVE_RANGE)
            {
                SelfTravel::ClearPortalGoal(member->guid);
                member->hasPortalGoal = false;
                continue;
            }
            // Inside and near one spot for WING_MEMBER_STILL_MS: no walk out (Gundrak heroic 2026-10-25, Rtshabe on a
            // 19-polygon island of the moat floor, 387 s "no walk to the portal"): left out of the walk, logged.
            if (bot->IsAlive() && bot->GetMapId() == party.content->mapId)
            {
                if (!member->wingStillMs || int32(member->wingStillMs - party.wingWalkSinceMs) < 0 ||
                    bot->GetExactDist(&member->wingStillPos) > WING_MEMBER_STILL_RANGE)
                {
                    member->wingStillMs = now ? now : 1;
                    member->wingStillPos = bot->GetPosition();
                }
                else if (getMSTimeDiff(member->wingStillMs, now) > WING_MEMBER_STILL_MS)
                {
                    SelfTravel::ClearPortalGoal(member->guid);
                    member->hasPortalGoal = false;
                    member->wingStillMs = 0;
                    LogCannotMove(party, bot, "wing change", "no walk out for 2 minutes: left out of the wing walk",
                                  entrance.GetPositionX(), entrance.GetPositionY(), entrance.GetPositionZ());
                    continue;
                }
            }
            everybody = false;
            if (bot->isMoving())
                party.lastProgressMs = now;
        }

        if (everybody)
        {
            party.wingWalk = false;
            party.nextRetargetMs = now;
            LogEvent(party, "wing_walk_done", JsonObject().Num("entrance", int64(party.entranceIdx))
                                                  .Num("durationSec",
                                                       getMSTimeDiff(party.wingWalkSinceMs, now) / IN_MILLISECONDS));
            return false;
        }

        if (getMSTimeDiff(party.wingWalkSinceMs, now) > WING_WALK_MAX_MS)
        {
            for (Player* bot : players)
                for (Member const& member : party.members)
                    if (member.guid == bot->GetGUID() && member.hasPortalGoal)
                        LogCannotMove(party, bot, "wing change", "did not reach the next entrance in 15 minutes",
                                      entrance.GetPositionX(), entrance.GetPositionY(), entrance.GetPositionZ());
            Finish(party, "failed", "wing change walk did not finish in 15 minutes");
            return true;
        }

        // The members walk on their own (out through the exit, in through the entrance); no route meanwhile.
        if (leader)
            TestRunner::ClearWaypoint(party.leader);
        party.lastEvent = "wing change walk";
        return true;
    }

    // Route visualisation ("playerbots route show <run> [seconds]" / "route hide <run>", user request 2026-10-24: "is
    // the path right?"). GM-only waypoint markers (VISUAL_WAYPOINT, entry 1 "Waypoint (Only GM can see it)", the
    // creature ".wp show" spawns; Creature::Create makes it SetVisible(false), so only a GM sees it) are summoned as
    // timed TempSummons in the run's instance, in the leader's phase, and redrawn every ROUTE_VIZ_REFRESH_MS. The kind
    // of a marker is its level (target it in the client) and its size; a column (markers stacked) marks a spot:
    //   level 1, small, every ~4 yd: the route backbone Retarget built last (ROUTE_ONLY_TRASH: leader -> next boss,
    //            event spot, starter) - the polylines "blocks route" is measured against;
    //   level 2, tiny: a backbone leg left out as incomplete (ROUTE_BACKBONE_COMPLETE_ONLY);
    //   level 3, medium, every ~6 yd: the leader's current walk (its movement spline);
    //   level 4, big, every ~8 yd: the route from the leader to the current target (RoutePath now);
    //   column of 2 (level 5): the leader's harness waypoint; column of 3 (level 6): the current target;
    //   column of 4 (level 7): the next boss.
    // Markers are friendly, non-attackable, immune to players and NPCs and passive (entry 1 is a civilian critter of
    // faction 35 anyway): Retarget's candidates (RouteCreatures lists summons without a spawn id) drop critters,
    // civilians and non-attackable creatures, and the bots' hostile-unit values never see a friendly one. Threads:
    // everything here runs on the world thread (the commands from the CLI / a THREADUNSAFE chat packet, TickRouteViz
    // from a WorldScript OnUpdate), i.e. inside World::Update after MapMgr::Update has waited for every map thread
    // (MapUpdater::wait), the same place the harness already builds paths and moves bots from - no map thread runs
    // while a marker is summoned or unsummoned.
    namespace
    {
        constexpr uint32 ROUTE_VIZ_REFRESH_MS = 10 * IN_MILLISECONDS;
        // A marker despawns by itself this long after its draw (a refresh that never comes leaves nothing behind).
        constexpr uint32 ROUTE_VIZ_LINGER_MS = 3 * ROUTE_VIZ_REFRESH_MS;
        constexpr uint32 ROUTE_VIZ_MAX_SECONDS = 2 * HOUR;
        constexpr size_t ROUTE_VIZ_MAX_MARKERS = 150;
        constexpr size_t ROUTE_VIZ_PATH_MAX = 25;        // walk and target route each
        constexpr size_t ROUTE_VIZ_INCOMPLETE_MAX = 30;
        constexpr float ROUTE_VIZ_BACKBONE_STEP = 4.0f;  // the backbone spreads wider when it would pass the cap
        constexpr float ROUTE_VIZ_WALK_STEP = 6.0f;
        constexpr float ROUTE_VIZ_TARGET_STEP = 8.0f;
        constexpr float ROUTE_VIZ_COLUMN_STEP = 2.5f;    // a column's markers stand this far apart in height

        enum class VizKind : uint8
        {
            Backbone = 1,
            Incomplete = 2,
            Walk = 3,
            TargetRoute = 4,
            Waypoint = 5,
            Target = 6,
            NextBoss = 7,
            // [lead14] the default view (ROUTE_VIZ_AUTO): blue = the backbone to the next main goal, red = the path of
            // the leader's current pull order to its stand spot, and a red column at that spot
            PullPath = 8,
            PullSpot = 9
        };
        // [lead14] the marker displays, from CreatureDisplayInfo.dbc / CreatureModelData.dbc (model 207
        // Creature\Wisp\Wisp.mdx): 10045 = VISUAL_WAYPOINT's own (creature_template_model of entry 1; textures
        // "Wisp" / "WispGlow", the blue-white wisp), 16587 = the same model with "WispRed" / "WispGlowRed"
        constexpr uint32 ROUTE_VIZ_DISPLAY_BLUE = 10045;
        constexpr uint32 ROUTE_VIZ_DISPLAY_RED = 16587;

        float VizScale(VizKind kind)
        {
            switch (kind)
            {
                case VizKind::Backbone:
                    return 0.7f;
                case VizKind::Incomplete:
                    return 0.4f;
                case VizKind::Walk:
                    return 1.2f;
                case VizKind::TargetRoute:
                    return 1.6f;
                case VizKind::Waypoint:
                    return 1.2f;
                case VizKind::Target:
                    return 1.6f;
                case VizKind::NextBoss:
                    return 2.0f;
                case VizKind::PullPath:
                    return 3.0f;  // [lead18] the red display (16587) is half the blue one's size: x2.5
                case VizKind::PullSpot:
                    return 4.5f;
            }
            return 1.0f;
        }

        struct RouteViz
        {
            uint32 mapId = 0;
            uint32 instanceId = 0;
            uint32 untilMs = 0;
            uint32 nextDrawMs = 0;
            std::vector<ObjectGuid> markers;
            std::string lastKey;  // routeType / next boss / target of the last chat line to the GMs
            // ROUTE_VIZ_AUTO: shown because a GM is inside the run's instance (no command); ends when none is.
            bool automatic = false;
            std::string drawnBackbone;  // BackboneSignature of the last draw
            std::string drawnPull;      // [lead14] the red line's signature of the last draw (issued pull, order)
        };
        std::unordered_map<uint32, RouteViz> routeViz;  // party seq -> markers; world thread only

        void ClearRouteMarkers(RouteViz& viz)
        {
            if (Map* map = viz.markers.empty() ? nullptr : sMapMgr->FindMap(viz.mapId, viz.instanceId))
                for (ObjectGuid const& guid : viz.markers)
                    if (Creature* creature = map->GetCreature(guid))
                        if (TempSummon* marker = creature->ToTempSummon())
                            marker->UnSummon();
            viz.markers.clear();
        }

        // Points every step yd along the polyline, its first and last point included; at most maxPoints (then wider).
        std::vector<G3D::Vector3> SamplePolyline(Movement::PointsArray const& points, float step, size_t maxPoints)
        {
            std::vector<G3D::Vector3> out;
            if (points.empty() || !maxPoints)
                return out;
            float const length = PolylineLength(points);
            if (maxPoints > 1 && length / step + 1.0f > float(maxPoints))
                step = length / float(maxPoints - 1);
            out.push_back(points.front());
            float sinceLast = 0.0f;
            for (size_t index = 1; index < points.size() && out.size() < maxPoints; ++index)
            {
                G3D::Vector3 const segment = points[index] - points[index - 1];
                float const segmentLength = segment.length();
                if (segmentLength <= 0.0f)
                    continue;
                float at = step - sinceLast;
                while (at <= segmentLength && out.size() < maxPoints)
                {
                    out.push_back(points[index - 1] + segment * (at / segmentLength));
                    at += step;
                }
                sinceLast = segmentLength - (at - step);
            }
            if (out.size() < maxPoints && (points.back() - out.back()).length() > 1.0f)
                out.push_back(points.back());
            return out;
        }

        bool SpawnRouteMarker(RouteViz& viz, Map* map, uint32 phaseMask, G3D::Vector3 const& at, VizKind kind,
                              uint32 durationMs)
        {
            if (viz.markers.size() >= ROUTE_VIZ_MAX_MARKERS)
                return false;
            TempSummon* marker =
                map->SummonCreature(VISUAL_WAYPOINT, Position(at.x, at.y, at.z, 0.0f), nullptr, durationMs);
            if (!marker)
                return false;
            if (phaseMask != PHASEMASK_NORMAL)
                marker->SetPhaseMask(phaseMask, true);
            marker->SetTempSummonType(TEMPSUMMON_TIMED_DESPAWN);
            marker->SetReactState(REACT_PASSIVE);
            marker->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
            marker->SetImmuneToAll(true);
            marker->SetObjectScale(VizScale(kind));
            marker->SetLevel(uint8(kind), false);
            // [lead14] red for the leader's pull order, blue (the waypoint's own display) for everything else
            marker->SetDisplayId(kind == VizKind::PullPath || kind == VizKind::PullSpot ? ROUTE_VIZ_DISPLAY_RED
                                                                                         : ROUTE_VIZ_DISPLAY_BLUE);
            viz.markers.push_back(marker->GetGUID());
            return true;
        }

        // A column of count markers from at upwards; the number spawned.
        uint32 SpawnRouteColumn(RouteViz& viz, Map* map, uint32 phaseMask, G3D::Vector3 const& at, VizKind kind,
                                uint32 count, uint32 durationMs)
        {
            uint32 spawned = 0;
            for (uint32 index = 0; index < count; ++index)
                spawned += SpawnRouteMarker(viz, map, phaseMask,
                                            G3D::Vector3(at.x, at.y, at.z + ROUTE_VIZ_COLUMN_STEP * float(index)),
                                            kind, durationMs)
                               ? 1
                               : 0;
            return spawned;
        }

        uint32 SpawnRoutePoints(RouteViz& viz, Map* map, uint32 phaseMask, std::vector<G3D::Vector3> const& points,
                                VizKind kind, uint32 durationMs)
        {
            // A path is a chain of straight corner-to-corner segments; a marker sampled on a segment floats above a
            // dip or sinks into a rise (user 2026-10-24: «путь не учитывает склоны»). Put it on the ground under it.
            constexpr float GROUND_SEARCH = 6.0f;
            uint32 spawned = 0;
            for (G3D::Vector3 point : points)
            {
                float const ground = map->GetHeight(phaseMask, point.x, point.y, point.z + GROUND_SEARCH, true,
                                                    GROUND_SEARCH * 2.0f);
                if (ground > INVALID_HEIGHT && std::fabs(ground - point.z) <= GROUND_SEARCH)
                    point.z = ground;
                spawned += SpawnRouteMarker(viz, map, phaseMask, point, kind, durationMs) ? 1 : 0;
            }
            return spawned;
        }

        float BackboneLength(RouteBackbone const& legs)
        {
            float length = 0.0f;
            for (Movement::PointsArray const& leg : legs)
                length += PolylineLength(leg);
            return length;
        }

        // Despawns the old markers of the run and draws the current route; the summary line, empty when nothing
        // could be drawn (error says why). key: what changed for the GM chat line (route type, next boss, target).
        // [lead14] the red line's source: the leader's last issued pull (its stand spot), while the tank's order for
        // it runs or for ROUTE_VIZ_PULL_FRESH_MS after the issue (the planned next pull); none otherwise
        constexpr uint32 ROUTE_VIZ_PULL_FRESH_MS = 30 * IN_MILLISECONDS;
        bool IssuedPull(Party const& party, Player* leader, Unit*& mob, Position& spot, std::string& kind)
        {
            auto const& lead = party.lead;
            if (!lead.issuedMs || lead.issuedMob.IsEmpty())
                return false;
            Leader::PullOrder order;
            bool const open = Leader::Orders::GetOpen(party.leader, order) && order.target == lead.issuedMob;
            // [lead18] drawn until the pull ends: its order runs, or its fight is on
            if (!open && !party.inCombat && getMSTimeDiff(lead.issuedMs, getMSTime()) > ROUTE_VIZ_PULL_FRESH_MS)
                return false;
            mob = ObjectAccessor::GetUnit(*leader, lead.issuedMob);
            if (!mob || (!mob->IsAlive() && !party.inCombat))
                return false;
            spot = lead.issuedSpot;
            kind = lead.issuedKind + (open ? std::string(" (order ") + Leader::StateName(order.state) + ")" : "");
            return true;
        }

        std::string PullSignature(Party const& party)
        {
            Leader::PullOrder order;
            bool const open = Leader::Orders::GetOpen(party.leader, order);
            return Acore::StringFormat("{}|{}|{}", party.lead.issuedMs, open ? order.id : 0,
                                       open ? uint32(order.state) : 0);
        }

        // [lead14] The default view of a GM inside the run (ROUTE_VIZ_AUTO), two lines (user 2026-10-26: «по
        // умолчанию одну линию… можно 2 цвета: следующая основная цель синим и красным следующий пул»): blue, the
        // backbone to the next main goal (boss / required spot / starter) with a column at its end; red, the path from
        // the tank to the stand spot of the leader's issued pull, with a red column at the spot. The full legend
        // stays with "playerbots route show".
        std::string DrawCompactRouteViz(Party const& party, RouteViz& viz, uint32 now, std::string& key,
                                        std::string& error)
        {
            ClearRouteMarkers(viz);
            Player* leader = ObjectAccessor::FindPlayer(party.leader);
            if (!party.instanceId || !leader || !leader->IsInWorld() || !leader->GetMap() ||
                leader->GetMap()->GetInstanceId() != party.instanceId)
            {
                error = "the leader is not inside the run's instance";
                return std::string();
            }
            Map* map = leader->GetMap();
            viz.mapId = map->GetId();
            viz.instanceId = map->GetInstanceId();
            uint32 const phaseMask = leader->GetPhaseMask();
            uint32 const remainingMs = int32(viz.untilMs - now) > 0 ? viz.untilMs - now : 0;
            uint32 const durationMs = std::max<uint32>(IN_MILLISECONDS, std::min(remainingMs, ROUTE_VIZ_LINGER_MS));
            auto const found = routeSnapshots.find(party.seq);
            RouteSnapshot const* snapshot = found != routeSnapshots.end() ? &found->second : nullptr;

            // red first (the cap never drops it): the issued pull's path and its spot
            std::string redText = "none";
            Unit* pullMob = nullptr;
            Position pullSpot;
            std::string pullKind;
            if (IssuedPull(party, leader, pullMob, pullSpot, pullKind))
            {
                // [lead18] the whole latched plan: the tank's stand at the issue -> the tag spot -> the fight spot,
                // a column at the fight spot and one at the target
                auto const vec = [](Position const& p)
                { return G3D::Vector3(p.GetPositionX(), p.GetPositionY(), p.GetPositionZ()); };
                auto const leg = [&](Position const& from, Position const& to)
                {
                    if (from.GetExactDist(to) < 1.0f)
                        return;
                    Movement::PointsArray points;
                    Leader::RouteResult const route = Leader::Route(leader, vec(from), vec(to));  // [lead99]
                    if (route.found)
                        points.assign(route.points.begin(), route.points.end());
                    if (points.size() < 2)
                        points = {vec(from), vec(to)};
                    size_t const budget = std::min(ROUTE_VIZ_PATH_MAX, ROUTE_VIZ_MAX_MARKERS - viz.markers.size());
                    SpawnRoutePoints(viz, map, phaseMask, SamplePolyline(points, ROUTE_VIZ_WALK_STEP, budget),
                                     VizKind::PullPath, durationMs);
                };
                Position const& from = party.lead.issuedFrom;
                Position const& fightAt = party.lead.issuedFight;
                Position const& targetAt = party.lead.issuedTargetAt;
                SpawnRouteColumn(viz, map, phaseMask, vec(fightAt), VizKind::PullSpot, 3, durationMs);
                SpawnRouteColumn(viz, map, phaseMask, vec(targetAt), VizKind::PullSpot, 3, durationMs);
                leg(from, pullSpot);
                leg(pullSpot, fightAt);
                redText = Acore::StringFormat(
                    "pull order {} {}: tag spot ({:.1f},{:.1f}) {:.0f} yd from the tank's stand -> fight spot "
                    "({:.1f},{:.1f}) {:.0f} yd -> target ({:.1f},{:.1f}) {:.0f} yd from the fight spot",
                    pullKind, pullMob->GetName(), pullSpot.GetPositionX(), pullSpot.GetPositionY(),
                    from.GetExactDist(pullSpot), fightAt.GetPositionX(), fightAt.GetPositionY(),
                    pullSpot.GetExactDist(fightAt), targetAt.GetPositionX(), targetAt.GetPositionY(),
                    fightAt.GetExactDist(targetAt));
            }

            // blue: the backbone and the next main goal
            std::string blueText = "none";
            if (snapshot && !snapshot->legs.empty())
            {
                float const length = BackboneLength(snapshot->legs);
                size_t const room = ROUTE_VIZ_MAX_MARKERS - viz.markers.size();
                size_t const ends = 2 * snapshot->legs.size();
                float step = ROUTE_VIZ_BACKBONE_STEP;
                float const spread = float(room > ends + 4 ? room - ends - 4 : 1);
                if (length / step > spread)
                    step = length / spread;
                for (Movement::PointsArray const& leg : snapshot->legs)
                    SpawnRoutePoints(viz, map, phaseMask,
                                     SamplePolyline(leg, step, ROUTE_VIZ_MAX_MARKERS - viz.markers.size()),
                                     VizKind::Backbone, durationMs);
                G3D::Vector3 const& end = snapshot->legs.back().back();
                SpawnRouteColumn(viz, map, phaseMask, end, VizKind::NextBoss, 3, durationMs);
                Creature* nextBoss = snapshot->nextBoss.IsEmpty() ? nullptr : map->GetCreature(snapshot->nextBoss);
                blueText = Acore::StringFormat(
                    "next {} {} ({:.0f} yd)", nextBoss ? "boss" : "goal",
                    nextBoss ? nextBoss->GetName()
                             : (snapshot->nextBossName.empty() ? snapshot->routeType : snapshot->nextBossName),
                    length);
            }
            key = (snapshot ? snapshot->routeType + "|" + snapshot->nextBoss.ToString() : std::string("-")) + "|" +
                  PullSignature(party);
            return Acore::StringFormat("route {} [{}] {}: blue: {}; red: {} ({} markers, {}s left)", party.seq,
                                       party.id, party.content ? party.content->key : "?", blueText, redText,
                                       viz.markers.size(), remainingMs / IN_MILLISECONDS);
        }

        std::string DrawRouteViz(Party const& party, RouteViz& viz, uint32 now, std::string& key, std::string& error)
        {
            if (viz.automatic)  // [lead14] the default view: two lines
                return DrawCompactRouteViz(party, viz, now, key, error);
            ClearRouteMarkers(viz);
            Player* leader = ObjectAccessor::FindPlayer(party.leader);
            if (!party.instanceId || !leader || !leader->IsInWorld() || !leader->GetMap() ||
                leader->GetMap()->GetInstanceId() != party.instanceId)
            {
                error = "the leader is not inside the run's instance";
                return std::string();
            }

            Map* map = leader->GetMap();
            viz.mapId = map->GetId();
            viz.instanceId = map->GetInstanceId();
            uint32 const phaseMask = leader->GetPhaseMask();
            uint32 const remainingMs = int32(viz.untilMs - now) > 0 ? viz.untilMs - now : 0;
            uint32 const durationMs = std::max<uint32>(IN_MILLISECONDS, std::min(remainingMs, ROUTE_VIZ_LINGER_MS));

            auto const found = routeSnapshots.find(party.seq);
            RouteSnapshot const* snapshot = found != routeSnapshots.end() ? &found->second : nullptr;

            // Columns first, so the cap never drops them: the next boss, the target, the leader's waypoint.
            std::string bossText = "none";
            Creature* nextBoss = snapshot && !snapshot->nextBoss.IsEmpty() ? map->GetCreature(snapshot->nextBoss)
                                                                           : nullptr;
            if (nextBoss && nextBoss->IsAlive())
            {
                SpawnRouteColumn(viz, map, phaseMask,
                                 G3D::Vector3(nextBoss->GetPositionX(), nextBoss->GetPositionY(),
                                              nextBoss->GetPositionZ()),
                                 VizKind::NextBoss, 4, durationMs);
                bossText = Acore::StringFormat("{} ({:.0f} yd)", nextBoss->GetName(), leader->GetExactDist(nextBoss));
            }
            else if (snapshot && !snapshot->nextBossName.empty())
                bossText = snapshot->nextBossName + " (not found / dead)";

            std::string targetText = "none";
            uint32 targetRouteMarkers = 0;
            WorldObject* target =
                party.target.IsEmpty() ? nullptr : ObjectAccessor::GetWorldObject(*leader, party.target);
            if (target && target->IsInWorld() && target->GetMap() == map)
            {
                G3D::Vector3 const at(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
                if (target != nextBoss)
                    SpawnRouteColumn(viz, map, phaseMask, at, VizKind::Target, 3, durationMs);
                PathResult const route = EvaluatePath(leader, target);
                size_t const budget = std::min(ROUTE_VIZ_PATH_MAX, ROUTE_VIZ_MAX_MARKERS - viz.markers.size());
                targetRouteMarkers = SpawnRoutePoints(viz, map, phaseMask,
                                                      SamplePolyline(route.points, ROUTE_VIZ_TARGET_STEP, budget),
                                                      VizKind::TargetRoute, durationMs);
                targetText = Acore::StringFormat("{} ({:.0f} yd, route {:.0f} yd type {}{}{}, {} markers)",
                                                 target->GetName(), leader->GetExactDist(target), route.length,
                                                 route.type, route.longRoute ? " long" : "",
                                                 route.complete ? "" : (route.reachable ? " partial" : " UNREACHABLE"),
                                                 targetRouteMarkers);
            }

            bool hasWaypoint = false;
            TestRunner::Waypoint waypoint;
            if (TestRunner::GetWaypoint(party.leader, waypoint) && waypoint.mapId == map->GetId())
            {
                hasWaypoint = true;
                SpawnRouteColumn(viz, map, phaseMask, G3D::Vector3(waypoint.x, waypoint.y, waypoint.z),
                                 VizKind::Waypoint, 2, durationMs);
            }

            // The leader's walk now: its movement spline from the current point on.
            uint32 walkMarkers = 0;
            if (leader->movespline && leader->movespline->Initialized() && !leader->movespline->Finalized())
            {
                Movement::PointsArray walk;
                walk.emplace_back(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
                // MoveSpline::getPath() is protected: the next point and the end of the walk are public.
                walk.push_back(leader->movespline->CurrentDestination());
                walk.push_back(leader->movespline->FinalDestination());
                size_t const budget = std::min(ROUTE_VIZ_PATH_MAX, ROUTE_VIZ_MAX_MARKERS - viz.markers.size());
                walkMarkers = SpawnRoutePoints(viz, map, phaseMask, SamplePolyline(walk, ROUTE_VIZ_WALK_STEP, budget),
                                               VizKind::Walk, durationMs);
            }

            // The backbone (leader -> next boss / spot / starter as Retarget saw it), then its incomplete legs.
            uint32 backboneMarkers = 0;
            uint32 incompleteMarkers = 0;
            float step = ROUTE_VIZ_BACKBONE_STEP;
            float backboneLength = 0.0f;
            if (snapshot)
            {
                backboneLength = BackboneLength(snapshot->legs);
                size_t const incompleteReserve = snapshot->dropped.empty() ? 0 : ROUTE_VIZ_INCOMPLETE_MAX / 2;
                size_t const room = ROUTE_VIZ_MAX_MARKERS - viz.markers.size();
                size_t const budget = room > incompleteReserve ? room - incompleteReserve : 0;
                // Every leg adds its first and last point to length / step: the step widens to fit the budget.
                size_t const ends = 2 * snapshot->legs.size();
                float const spread = float(budget > ends ? budget - ends : 1);
                if (backboneLength / step > spread)
                    step = backboneLength / spread;
                for (Movement::PointsArray const& leg : snapshot->legs)
                    backboneMarkers += SpawnRoutePoints(
                        viz, map, phaseMask,
                        SamplePolyline(leg, step, ROUTE_VIZ_MAX_MARKERS - viz.markers.size()),
                        VizKind::Backbone, durationMs);
                for (Movement::PointsArray const& leg : snapshot->dropped)
                    incompleteMarkers += SpawnRoutePoints(
                        viz, map, phaseMask,
                        SamplePolyline(leg, ROUTE_VIZ_BACKBONE_STEP,
                                       std::min(ROUTE_VIZ_INCOMPLETE_MAX, ROUTE_VIZ_MAX_MARKERS - viz.markers.size())),
                        VizKind::Incomplete, durationMs);
            }

            std::string backboneText;
            if (!snapshot)
                backboneText = "no retarget yet";
            else if (!snapshot->routeOnly)
                backboneText = "off (no route-only trash here: raid, hold spot or escort)";
            else
                backboneText = Acore::StringFormat(
                    "{} - {} leg(s) {:.0f} yd, {} markers every {:.1f} yd; incomplete leg(s) {} ({} markers); "
                    "built {}s ago",
                    snapshot->routeType, snapshot->legs.size(), backboneLength, backboneMarkers, step,
                    snapshot->dropped.size(), incompleteMarkers,
                    getMSTimeDiff(snapshot->builtMs, now) / IN_MILLISECONDS);

            key = (snapshot ? snapshot->routeType : std::string("-")) + "|" +
                  (snapshot ? snapshot->nextBoss.ToString() : std::string()) + "|" + party.target.ToString();
            return Acore::StringFormat("route {} [{}] {}: backbone {}; next boss {}; target {}; leader walk {} "
                                       "markers; waypoint {}; {} markers in total ({}s left)",
                                       party.seq, party.id, party.content ? party.content->key : "?", backboneText,
                                       bossText, targetText, walkMarkers, hasWaypoint ? "yes" : "none",
                                       viz.markers.size(), remainingMs / IN_MILLISECONDS);
        }

        // The summary to every real GM inside the instance (the watcher), except the one who typed the command.
        void TellRouteGMs(Map* map, std::string const& text, WorldSession const* skip)
        {
            if (!map)
                return;
            map->DoForAllPlayers(
                [&text, skip](Player* player)
                {
                    if (player && player->IsGameMaster() && player->GetSession() && player->GetSession() != skip &&
                        !GET_PLAYERBOT_AI(player))
                        ChatHandler(player->GetSession()).SendSysMessage(text);
                });
        }

        // User request 2026-10-24: the route markers are always shown to a GM inside a run's instance, no command:
        // while a real GM (account security SEC_GAMEMASTER or more, not a bot) is on the run's map instance the
        // markers stay up and are redrawn whenever Retarget rebuilt the backbone (or every ROUTE_VIZ_REFRESH_MS),
        // and they go when the last GM leaves or the run ends. The markers stay GM-only (VISUAL_WAYPOINT; a GM sees
        // them with .gm on). false: only "playerbots route show".
        constexpr bool ROUTE_VIZ_AUTO = true;
        constexpr uint32 ROUTE_VIZ_AUTO_SCAN_MS = IN_MILLISECONDS;

        // What a redraw shows of the backbone (Retarget rebuilds it every few seconds, mostly the same): its kind,
        // next boss and the goal of each leg to the yard (a leg starts at the leader, who walks: that part is redrawn
        // on the ROUTE_VIZ_REFRESH_MS clock).
        std::string BackboneSignature(RouteSnapshot const& snapshot)
        {
            std::string signature = snapshot.routeType + "|" + snapshot.nextBoss.ToString();
            for (Movement::PointsArray const& leg : snapshot.legs)
                if (!leg.empty())
                    signature +=
                        Acore::StringFormat("|{:.0f},{:.0f},{:.0f}", leg.back().x, leg.back().y, leg.back().z);
            return signature;
        }

        bool GameMasterInside(Map* map)
        {
            if (!map)
                return false;
            bool found = false;
            map->DoForAllPlayers(
                [&found](Player* player)
                {
                    if (!found && player && player->GetSession() && !GET_PLAYERBOT_AI(player) &&
                        player->GetSession()->GetSecurity() >= SEC_GAMEMASTER)
                        found = true;
                });
            return found;
        }

        constexpr char const* ROUTE_VIZ_LEGEND =
            "route markers (target one: its level is the kind): lvl1 small = backbone, lvl2 tiny = incomplete leg, "
            "lvl3 medium = leader's walk, lvl4 big = route to the target, column of 2 = waypoint, 3 = target, "
            "4 = next boss";
    }

    bool TestHarnessMgr::HandleRouteShow(ChatHandler* handler, uint32 seq, uint32 seconds)
    {
        Party const* party = nullptr;
        for (auto const& candidate : _parties)
            if (candidate->seq == seq)
                party = candidate.get();
        if (!party || !party->content || IsTerminalPhase(party->phase) || !party->instanceId)
        {
            handler->PSendSysMessage("Route: run {} is not inside a dungeon", seq);
            return false;
        }

        uint32 const now = getMSTime();
        RouteViz& viz = routeViz[seq];
        viz.untilMs = now + std::clamp<uint32>(seconds, 10, ROUTE_VIZ_MAX_SECONDS) * IN_MILLISECONDS;
        std::string key;
        std::string error;
        std::string const summary = DrawRouteViz(*party, viz, now, key, error);
        viz.nextDrawMs = now + ROUTE_VIZ_REFRESH_MS;
        if (summary.empty())
        {
            handler->PSendSysMessage("Route: run {} [{}]: {}; tries again every {}s", seq, party->id, error,
                                     ROUTE_VIZ_REFRESH_MS / IN_MILLISECONDS);
            return true;
        }

        viz.lastKey = key;
        handler->SendSysMessage(summary);
        handler->SendSysMessage(ROUTE_VIZ_LEGEND);
        LOG_INFO("playerbots", "[route viz] {}", summary);
        Map* map = sMapMgr->FindMap(viz.mapId, viz.instanceId);
        WorldSession const* skip = handler->GetSession();
        TellRouteGMs(map, ROUTE_VIZ_LEGEND, skip);
        TellRouteGMs(map, summary, skip);
        return true;
    }

    bool TestHarnessMgr::HandleRouteHide(ChatHandler* handler, uint32 seq)
    {
        auto const found = routeViz.find(seq);
        if (found == routeViz.end())
        {
            handler->PSendSysMessage("Route: no markers shown for run {}", seq);
            return false;
        }

        size_t const count = found->second.markers.size();
        ClearRouteMarkers(found->second);
        routeViz.erase(found);
        handler->PSendSysMessage("Route: run {} - {} markers removed, refresh stopped", seq, count);
        return true;
    }

    void TestHarnessMgr::TickRouteViz()
    {
        uint32 const now = getMSTime();
        // [speech] what the leader says goes to the GM inside the run's instance (system messages), twice a second
        if (Leader::LEADER_SPEECH)
        {
            static uint32 speechScanMs = 0;
            if (!speechScanMs || getMSTimeDiff(speechScanMs, now) >= 500)
            {
                speechScanMs = now ? now : 1;
                for (auto const& candidate : _parties)
                {
                    Party const& party = *candidate;
                    if (!party.content || IsTerminalPhase(party.phase) || !party.instanceId)
                        continue;
                    std::vector<std::string> const lines = DecisionLog::Instance().DrainSpeech(party.seq);
                    if (lines.empty())
                        continue;
                    Player* speechLeader = ObjectAccessor::FindPlayer(party.leader);
                    Map* speechMap = speechLeader && speechLeader->IsInWorld() ? speechLeader->GetMap() : nullptr;
                    if (!speechMap || speechMap->GetInstanceId() != party.instanceId || !GameMasterInside(speechMap))
                        continue;
                    for (std::string const& line : lines)
                        TellRouteGMs(speechMap, line, nullptr);
                }
            }
        }
        // ROUTE_VIZ_AUTO: a GM inside a run's instance gets its markers without a command, until it leaves.
        static uint32 autoScanMs = 0;
        if (ROUTE_VIZ_AUTO && (!autoScanMs || getMSTimeDiff(autoScanMs, now) >= ROUTE_VIZ_AUTO_SCAN_MS))
        {
            autoScanMs = now ? now : 1;
            for (auto const& candidate : _parties)
            {
                Party const& party = *candidate;
                if (!party.content || IsTerminalPhase(party.phase) || !party.instanceId)
                    continue;
                Player* leader = ObjectAccessor::FindPlayer(party.leader);
                Map* map = leader && leader->IsInWorld() ? leader->GetMap() : nullptr;
                bool const gmInside = map && map->GetInstanceId() == party.instanceId && GameMasterInside(map);
                auto const found = routeViz.find(party.seq);
                if (gmInside && found == routeViz.end())
                {
                    RouteViz& viz = routeViz[party.seq];
                    viz.automatic = true;
                    viz.untilMs = now + ROUTE_VIZ_MAX_SECONDS * IN_MILLISECONDS;
                    viz.nextDrawMs = now;
                    LOG_INFO("playerbots", "[route viz] run {} [{}]: a GM is inside - markers on (automatic)",
                             party.seq, party.id);
                }
                else if (found != routeViz.end() && found->second.automatic)
                {
                    if (gmInside)
                        found->second.untilMs = now + ROUTE_VIZ_MAX_SECONDS * IN_MILLISECONDS;
                    else
                    {
                        LOG_INFO("playerbots", "[route viz] run {} [{}]: no GM inside - markers removed", party.seq,
                                 party.id);
                        ClearRouteMarkers(found->second);
                        routeViz.erase(found);
                    }
                }
            }
        }
        if (routeViz.empty())
            return;

        for (auto itr = routeViz.begin(); itr != routeViz.end();)
        {
            RouteViz& viz = itr->second;
            // A backbone Retarget rebuilt since the last draw is drawn at once (ROUTE_VIZ_AUTO), not on the clock.
            auto const snapshot = routeSnapshots.find(itr->first);
            std::string const signature =
                snapshot != routeSnapshots.end() ? BackboneSignature(snapshot->second) : std::string();
            bool const rebuilt = ROUTE_VIZ_AUTO && signature != viz.drawnBackbone;
            // [lead14] the red line is redrawn whenever the leader issues or an order changes / ends
            std::string pullSignature;
            for (auto const& candidate : _parties)
                if (candidate->seq == itr->first)
                    pullSignature = PullSignature(*candidate);
            bool const pullChanged = viz.automatic && pullSignature != viz.drawnPull;
            if (int32(now - viz.nextDrawMs) < 0 && !rebuilt && !pullChanged)
            {
                ++itr;
                continue;
            }
            viz.drawnBackbone = signature;
            viz.drawnPull = pullSignature;

            Party const* party = nullptr;
            for (auto const& candidate : _parties)
                if (candidate->seq == itr->first)
                    party = candidate.get();
            if (!party || IsTerminalPhase(party->phase) || int32(now - viz.untilMs) >= 0)
            {
                LOG_INFO("playerbots", "[route viz] run {}: {} - markers removed", itr->first,
                         !party ? "run gone" : (IsTerminalPhase(party->phase) ? "run over" : "time is up"));
                ClearRouteMarkers(viz);
                itr = routeViz.erase(itr);
                continue;
            }

            viz.nextDrawMs = now + ROUTE_VIZ_REFRESH_MS;
            std::string key;
            std::string error;
            std::string const summary = DrawRouteViz(*party, viz, now, key, error);
            if (summary.empty())
                LOG_INFO("playerbots", "[route viz] run {} [{}]: {}", party->seq, party->id, error);
            else
            {
                LOG_INFO("playerbots", "[route viz] {}", summary);
                if (key != viz.lastKey)
                {
                    viz.lastKey = key;
                    TellRouteGMs(sMapMgr->FindMap(viz.mapId, viz.instanceId), summary, nullptr);
                }
            }
            ++itr;
        }
    }

    namespace
    {
        // The idle guard (user rule 2026-10-25): a run with no fight for two minutes is a blocker; a genuine event wait
        // (Black Morass between rifts, the Halls of Reflection gather, Svala's intro) gets four.
        constexpr uint32 IDLE_GUARD_MS = 2 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 IDLE_EVENT_WAIT_MS = 4 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 IDLE_TICK_CAP_MS = 5 * IN_MILLISECONDS;  // one tick counts at most this much (a hitch)
        constexpr size_t IDLE_SKIP_REASONS = 4;                   // the most frequent skip reasons in the text
        // An event worked through by starters without a fight (Culling of Stratholme: five crates with the Arcane
        // Disruptor, 2026-10-25 wave 7 cos-n / cos-h flagged as idle): a starter target or a use this recent.
        constexpr uint32 IDLE_STARTER_GAP_MS = 90 * IN_MILLISECONDS;
    }

    std::string TestHarnessMgr::IdleEventWait(Party& party, Player* leader)
    {
        if (!party.content || !leader->IsAlive())
            return std::string();
        if (!party.escort.IsEmpty())
            return "escort: " + party.escortNote;
        if ((party.targetIsStarter && !party.target.IsEmpty()) ||
            (party.lastStarterUseMs && getMSTimeDiff(party.lastStarterUseMs, getMSTime()) < IDLE_STARTER_GAP_MS))
            return "event starter: " + party.lastEvent;
        if (Creature* intro = RunningIntroBoss(leader))
            return std::string("intro: ") + intro->GetName();
        // [queue-cos-intro-wait] Culling of Stratholme: Arthas' scripted intro (progress START_INTRO: the Uther /
        // Jaina talk, ~110 s of culling_of_stratholme.cpp EVENT_ACTION_PHASE1 steps) and the city intro (progress
        // FINISHED_INTRO with Arthas busy - no gossip flag - in EVENT_ACTION_PHASE2) are the event talking, not idle
        // (cos-n 20261025-134139-cos-n-5A-73: the leader beside the talking Arthas at (1905, 1298) from 226 s, "no
        // fight for 298 s" at 327 s with eventWait false, stopped at 330 s - the waves come after the city intro).
        if (leader->GetMapId() == COS_MAP_ID)
        {
            uint32 const progress = CoSHelpers::GetProgress(leader);
            Creature* arthas = CoSHelpers::GetArthas(leader);
            if (arthas && arthas->IsAlive() &&
                (progress == CoSIds::COS_PROGRESS_START_INTRO ||
                 (progress == CoSIds::COS_PROGRESS_FINISHED_INTRO && !arthas->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP)) ||
                 progress == CoSIds::COS_PROGRESS_FINISHED_CITY_INTRO))
                return Acore::StringFormat("cos intro: Arthas (progress {})", progress);
        }
        if (party.eventGatherArrived && EventGatherApplies(party))
            return "event gather";
        int32 const index = EventSpotIndex(party);
        if (index < 0)
            return std::string();
        EventSpotDef const* spot = EventSpots(party.content->mapId)[size_t(index)];
        auto const state = party.eventSpotState.find(size_t(index));
        float const radius = spot->holdRadius > 0.0f ? spot->holdRadius : HOLD_TARGET_RADIUS;
        if (SpotHolds(party, *spot) && state != party.eventSpotState.end() && state->second == 1 &&
            leader->GetExactDist(spot->x, spot->y, spot->z) <= radius)
            return std::string("event hold: ") + spot->note;
        return std::string();
    }

    std::string TestHarnessMgr::IdleHolders(Party& party, Player* leader)
    {
        uint32 const now = getMSTime();
        std::vector<std::string> parts;
        parts.push_back("phase " + PhaseLabel(party));
        if (!party.waitingFor.empty())
        {
            std::string text = "waiting for " + party.waitingFor;
            if (party.waitSinceMs)
                text += Acore::StringFormat(" ({} s)", getMSTimeDiff(party.waitSinceMs, now) / IN_MILLISECONDS);
            parts.push_back(text);
        }
        if (party.pingPongHoldUntilMs)
            parts.push_back("regroup ping-pong hold");
        if (party.guaranteeHolding)
            parts.push_back("careful guarantee hold");
        if (party.castHoldUntilMs && int32(now - party.castHoldUntilMs) < 0)
            parts.push_back("event item cast hold");
        if (party.wingWalk)
            parts.push_back("wing walk");
        if (party.needBuff)
            parts.push_back(Acore::StringFormat("buffing step {} ({})", party.buffStep, party.buffReason));
        if (party.linkIdx >= 0)
        {
            std::vector<RouteLinkDef const*> const links = RouteLinks(party.content->mapId);
            if (size_t(party.linkIdx) < links.size())
                parts.push_back(Acore::StringFormat("route_link {} ({}, {} s, {} crossed)",
                                                    links[size_t(party.linkIdx)]->note,
                                                    party.linkLeaderCrossed ? "leader crossing" : "walking to it",
                                                    getMSTimeDiff(party.linkSinceMs, now) / IN_MILLISECONDS,
                                                    party.linkCrossed.size()));
        }
        std::string const eventWait = IdleEventWait(party, leader);
        if (!eventWait.empty())
            parts.push_back(eventWait);
        else if (int32 const index = EventSpotIndex(party); index >= 0)
            parts.push_back(std::string("event spot walk: ") + EventSpots(party.content->mapId)[size_t(index)]->note);

        Map* map = leader->GetMap();
        WorldObject* target = nullptr;
        if (!party.target.IsEmpty() && map)
        {
            target = map->GetCreature(party.target);
            if (!target)
                target = map->GetGameObject(party.target);
        }
        if (target)
        {
            char const* kind = party.targetIsBoss ? " (boss)" : party.targetIsStarter ? " (event starter)" : "";
            std::string text = Acore::StringFormat("target {}{} {:.1f} yd for {} s", target->GetName(), kind,
                                                   leader->GetExactDist(target),
                                                   getMSTimeDiff(party.targetSinceMs, now) / IN_MILLISECONDS);
            if (party.arrivedMs)  // [b43_harn] a stamp later than `now` is 0 s, not a wrapped 4294967 s
                text += Acore::StringFormat(", at the stop {} s",
                                            int32(now - party.arrivedMs) > 0
                                                ? getMSTimeDiff(party.arrivedMs, now) / IN_MILLISECONDS
                                                : 0);
            if (party.approach)
                text += ", body pull";
            if (!party.approachBlocker.IsEmpty())
                if (Creature* blocker = map->GetCreature(party.approachBlocker))
                    text += std::string(", stopped by ") + blocker->GetName();
            // The verdict: what the tank (the leader) makes of the target ("accepted", or why not).
            if (Unit* unit = target->ToUnit())
                text += ", tank view: " + AggressiveTargetValue::Explain(leader, unit);
            parts.push_back(text);
        }
        else if (party.noTargets)
            parts.push_back(Acore::StringFormat("no target ({} s)",
                                                getMSTimeDiff(party.noTargetsSinceMs, now) / IN_MILLISECONDS));
        else
            parts.push_back("no target");
        if (!party.behindSkipped.empty())
            parts.push_back("behind skipped " + party.behindSkipped);

        // The skips in force, by reason (the most frequent first): a route that skipped everything says so.
        std::map<std::string, uint32> reasons;
        for (auto const& [guid, untilMs] : party.skipUntil)
        {
            if (int32(now - untilMs) >= 0)
                continue;
            auto const why = party.skipReason.find(guid);
            std::string reason = why != party.skipReason.end() ? why->second : "?";
            if (size_t const cut = reason.find(" ("); cut != std::string::npos)
                reason.resize(cut);
            ++reasons[reason];
        }
        if (!reasons.empty())
        {
            std::vector<std::pair<std::string, uint32>> sorted(reasons.begin(), reasons.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](auto const& left, auto const& right) { return left.second > right.second; });
            std::string text = "skipped:";
            for (size_t index = 0; index < sorted.size() && index < IDLE_SKIP_REASONS; ++index)
                text += Acore::StringFormat(" {} x{};", sorted[index].first, sorted[index].second);
            parts.push_back(text);
        }

        // What the leader's engine did last (the executed action, who pushed it, how long ago) and the action that
        // started its last movement: "leader busy (action duration)" alone was only nextAICheckDelay > 0, set by
        // every tick (an-h / scholo-h / sm-h wave 7).
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(leader))
            if (Engine* engine = botAI->GetCurrentEngine())
            {
                IntentExecuted const& executed = engine->GetIntentExecuted();
                std::string text = "leader doing ";
                text += executed.name.empty() ? std::string("nothing recorded") : executed.name;
                if (!executed.by.empty())
                    text += " [" + executed.by + "]";
                if (executed.ms)
                    text += Acore::StringFormat(" {} s ago", getMSTimeDiff(executed.ms, now) / IN_MILLISECONDS);
                if (!engine->GetIntentMover().empty())
                    text += Acore::StringFormat("; last move by {} {} s ago", engine->GetIntentMover(),
                                                getMSTimeDiff(engine->GetIntentMoverMs(), now) / IN_MILLISECONDS);
                if (botAI->IsActionDurationActive())
                    text += "; action delay";
                parts.push_back(text);
            }
        if (!party.lastEvent.empty())
            parts.push_back("last " + party.lastEvent);

        std::string text;
        for (std::string const& part : parts)
            text += (text.empty() ? "" : "; ") + part;
        return text;
    }

    void TestHarnessMgr::CheckIdle(Party& party, std::vector<Player*> const& players, Player* leader, bool anyInCombat)
    {
        uint32 const now = getMSTime();
        uint32 const tickMs = party.idleTickMs ? std::min(getMSTimeDiff(party.idleTickMs, now), IDLE_TICK_CAP_MS) : 0;
        party.idleTickMs = now;
        if (anyInCombat || party.inCombat)
        {
            party.idleMs = 0;
            party.eventWaitMs = 0;
            party.idleLogged = false;
            party.eventWaitLogged = false;
            return;
        }

        // Buffing is no idle time (BUFF_TOTAL_MAX_MS caps it). A rest counts at the event-wait limit (wave 8 nex-h:
        // 138 s of regen_stall without an [idle] line).
        if (party.phase == Phase::Buffing || party.phase == Phase::ReadyCheck)
            return;

        std::string const eventWait =
            party.phase == Phase::Drinking ? "rest: " + party.waitingFor : IdleEventWait(party, leader);
        bool const inEventWait = !eventWait.empty();
        if (inEventWait)
            party.eventWaitMs += tickMs;
        else
            party.idleMs += tickMs;

        // The two lanes add up (user rule: two minutes without a fight): strat-h wave 11 stood 204 s without a fight,
        // split into a key re-use's 90 s event-starter window and ~110 s of meet holds, and no lane reached its limit.
        // At least IDLE_GUARD_MS / 2 of it is plain idle, so a designed event wait (CoS crates) still needs its own.
        bool const idleDue = !inEventWait && !party.idleLogged &&
                             (party.idleMs >= IDLE_GUARD_MS ||
                              (party.idleMs + party.eventWaitMs >= IDLE_GUARD_MS && party.idleMs >= IDLE_GUARD_MS / 2));
        bool const eventDue = inEventWait && !party.eventWaitLogged && party.eventWaitMs >= IDLE_EVENT_WAIT_MS;
        if (!idleDue && !eventDue)
            return;
        if (idleDue)
            party.idleLogged = true;
        else
            party.eventWaitLogged = true;

        uint32 const seconds = (idleDue ? party.idleMs + party.eventWaitMs : party.eventWaitMs) / IN_MILLISECONDS;
        std::string const holders = IdleHolders(party, leader);
        if (inEventWait)
            LOG_INFO("playerbots", "[idle] {}: no fight for {} s: event wait: {}", party.id, seconds, holders);
        else
            LOG_INFO("playerbots", "[idle] {}: no fight for {} s: {}", party.id, seconds, holders);
        JsonArray diagnosis;
        for (Player* bot : players)
            diagnosis.Raw(MemberDiagnosisJson(party, bot, leader));
        LogEvent(party, "idle", JsonObject().Num("sec", seconds).Bool("eventWait", inEventWait)
                                    .Str("eventWaitWhat", eventWait).Str("holding", holders)
                                    .Str("phase", PhaseLabel(party)).Str("target", party.target.ToString())
                                    .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                                    .Real("z", leader->GetPositionZ()).Raw("members", diagnosis.Build()));
    }
}
