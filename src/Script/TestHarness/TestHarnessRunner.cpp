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
        constexpr float PULL_RANGE = 20.0f;
        constexpr float BODY_PULL_RANGE = 5.0f;
        constexpr float BODY_PULL_MELEE_SLACK = 1.0f;  // [b32_bm] a walk-in arrives this far past the melee range
        // [b32_bm] a walk-in this far past the melee range (in sight) is no "route end": the tank stands beside it
        constexpr float BODY_PULL_STUCK_REACH = 3.0f;
        constexpr float STARTER_RANGE = 4.0f;
        using Leader::Travel::FAILED_BOSS_RADIUS;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_ROUTE_WEIGHT;  // [lead107] RouteTargets.h
        using Leader::Travel::LATER_BOSS_TRASH_RADIUS;  // [lead107] RouteTargets.h
        using Leader::Travel::DEFERRED_TRASH_PENALTY;  // [lead107] RouteTargets.h
        constexpr float BOSS_BUFF_DISTANCE = 70.0f;
        constexpr float BOSS_PREPOT_DISTANCE = 35.0f;
        using Leader::Travel::UNREACHABLE_SKIP_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::UNREACHABLE_RECHECK_DISTANCE;  // [lead107] RouteTargets.h
        using Leader::Travel::UNREACHABLE_RECHECK_HEIGHT;  // [lead107] RouteTargets.h
        using Leader::Travel::ORDER_HOLD_MS;  // [lead107] RouteTargets.h
        constexpr uint32 NOT_PULLED_SKIP_MS = 10 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 PULL_WAIT_MS = 45 * IN_MILLISECONDS;
        constexpr uint32 EVENT_WAIT_FOR_SPAWNS_MS = 5 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 BODY_PULL_WAIT_MS = 10 * IN_MILLISECONDS;
        using Leader::Dungeon::CAREFUL_REFUSED_SKIP_MS;  // [lead107] LeaderPlan.h (3 min)
        using Leader::Travel::ROUTE_ADOPT_GROUP_ESCORT;  // [lead107] RouteTargets.h
        // ROUTE_CAREFUL_GUARANTEE_HOLD (build 11): a pack the planner refused but will take once its stand guarantee is
        // due (CarefulPlanVerdict::GetRefusal, not hopeless) is not skipped: the group holds at it while the guarantee
        // runs, GUARANTEE_EXECUTE_MS more for the pull itself, GUARANTEE_HOLD_TOTAL_MS from the arrival at most
        // (build 14: the planner's 1.5x maxMobs tier comes after 180 s, waitMs up to ~200 s). A hopeless refusal (a
        // real dead end) is skipped at once, the planner's `why` in the skip reason, and the route goes round it.
        constexpr bool ROUTE_CAREFUL_GUARANTEE_HOLD = true;
        constexpr uint32 GUARANTEE_EXECUTE_MS = 30 * IN_MILLISECONDS;
        // [guarantee_hold_trim] 150 s (was 240): the planner's guarantee now comes after 30 s within the cap and 90 s
        // over it (ChooseTargetActions.cpp careful_guarantee_trim), plus GUARANTEE_EXECUTE_MS for the pull.
        constexpr uint32 GUARANTEE_HOLD_TOTAL_MS = 150 * IN_MILLISECONDS;
        constexpr uint32 GUARANTEE_UNKNOWN_HOLD_MS = 120 * IN_MILLISECONDS;
        constexpr uint32 PATROL_WAIT_HOLD_MS = 30 * IN_MILLISECONDS;  // [patrol_wait_spot] the stand there at most
        // [held_for_now_cap] a "held for now" with the group fully ready this long is a refusal (under the 120 s idle
        // stall watchdog)
        constexpr uint32 HELD_FOR_NOW_MAX_MS = 75 * IN_MILLISECONDS;
        using Leader::Travel::ROUTE_LIFT_REFUSED_SKIP;  // [lead107] RouteTargets.h
        using Leader::Travel::LIFT_REFUSALS_MAX;  // [lead107] RouteTargets.h
        // The bots' own boss hold (RaidLeader "pull hold": its ready check, PULL_HOLD_MAX_MS 150 s) is waited out
        // before a boss counts as "not pulled" (Oculus heroic 2026-10-25: Drakos, who never aggroes by sight, was
        // skipped after 10 s while the tank was held).
        constexpr uint32 BOSS_PULL_HOLD_WAIT_MS = 150 * IN_MILLISECONDS;
        using Leader::Travel::SAME_ENTRANCE_RANGE;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_WING_DONE_CHANGE;  // [lead107] RouteTargets.h
        constexpr uint32 WING_DONE_NO_TARGET_MS = 3 * IN_MILLISECONDS;
        // [st-guard-approach] the approach point counts as reached within this; on the guard's floor (SAME_FLOOR_DZ)
        // and no farther from it than the point + slack, the point is skipped.
        constexpr float GUARD_APPROACH_ARRIVE = 5.0f;
        constexpr float GUARD_APPROACH_SLACK = 8.0f;
        // [dmw-guard-body-pull] a pending door guard refused this often by the careful planner is body-pulled (user
        // decision 2026-10-25: approved, tag guard_body_pull).
        constexpr uint32 GUARD_REFUSALS_BEFORE_BODY_PULL = 2;
        // [ulda-ritual-hold] a summoning ritual (GAMEOBJECT_TYPE_SUMMONING_RITUAL) casts its spell 5 s after the
        // click (GameObject::Use m_cooldownTime) and only while the user still channels the anim spell.
        constexpr uint32 RITUAL_CHANNEL_HOLD_MS = 6500;
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
        constexpr uint32 NEIGHBOUR_TARGET_MS = 4 * MINUTE * IN_MILLISECONDS;  // the guarantee hold's 240 s
        // [unseen-blocker-pull] an unseen neighbour pack this small that holds an approach becomes the target
        constexpr size_t UNSEEN_BLOCKER_PACK_MAX = 4;
        // [route_blocker_body_pull] User decision 2026-10-25 (overrides "the harness never walks the group into a pack
        // the tank refused" for this case): a pack the route's walk has to pass (ApproachStop: the leg to the target
        // enters its reach) is never skipped - neither it nor the targets behind it. It becomes the target, the tank
        // pulls it by sight when it can (its planner, the guarantee), else the group gets ready (GroupFullyReady:
        // health, healer mana, everyone here) and the leader walks into it (body pull, navmesh walk, no door
        // clipping). Only a pack that is not on the way (a side pack the route picked as a target) may be skipped.
        // st-n 2026-10-25 081744 08:56-08:57: a 5-mob Atal'ai Slave pack at the balcony stairs held the edge, then
        // Hukku, Loro, Mijan, Zul'Lor were skipped "no sight point" and Gasher, Zolo and the Witch Doctors "behind
        // skipped Atal'ai Slave"; the group stood.
        constexpr bool ROUTE_BLOCKER_BODY_PULL = true;
        constexpr uint32 ROUTE_BLOCKER_MAX_TRIES = 3;      // made the target this often per blocker, then as before
        constexpr uint32 ROUTE_BLOCKER_PREP_MS = 30 * IN_MILLISECONDS;   // the planner's chance before the walk in
        constexpr uint32 ROUTE_BLOCKER_SIGHT_HOLD_MS = 8 * IN_MILLISECONDS;  // the edge hold for a route blocker
        // [route_blocker_walk_through] an idle pack this close to the route blocker is part of its body pull
        constexpr float ROUTE_BLOCKER_THROUGH_RANGE = 30.0f;
        constexpr uint32 NEIGHBOUR_HOLD_MAX = 2;          // boss approach holds at one neighbour before it passes
        constexpr size_t NEIGHBOUR_PASS_PACK_MAX = 3;     // a neighbour pack this small no longer stops it
        constexpr size_t MEET_PASS_PACK_MAX = 2;  // the walk back goes on into at most this many idle mobs
        constexpr float BEHIND_SHADOW_RANGE = 60.0f;  // ROUTE_SKIP_BEHIND_SKIPPED: the shadow skipped with a target
        constexpr float BEHIND_CONE_COS = 0.82f;      // ~35 degrees either side of the leader -> blocker line
        constexpr uint32 OUTSIDE_GIVE_UP_STUCKS = 3;  // alive outside: 3 x GHOST_STUCK_MS without progress
        constexpr float ENTRY_REFUSAL_RANGE = 20.0f;                           // 2D, a member on its entrance
        constexpr uint32 ENTRY_REFUSED_MAX_MS = 5 * MINUTE * IN_MILLISECONDS;  // then the run ends with the cause
        constexpr uint32 HOR_MAP = 668;
        constexpr uint32 HOR_DATA_WAVE_NUMBER = 8;  // halls_of_reflection.h DATA_WAVE_NUMBER
        // A script slot IN_PROGRESS with nothing to fight is an event between its waves (Halls of Stone: the Tribunal
        // of Ages, three 100 s phases with dialogue): no end of the run meanwhile, at most this long.
        constexpr uint32 SCRIPT_EVENT_MAX_MS = 10 * MINUTE * IN_MILLISECONDS;
        constexpr float GHOST_DIRECT_RANGE = 40.0f;  // no usable path this close to the portal: straight to it
        constexpr float GHOST_ARRIVED_RANGE = 8.0f;  // standing on the portal, not worth a log line
        constexpr float GHOST_CORNER_STEP = 150.0f;  // one move along a corner path (corpse runs over 590 yd)
        constexpr float FLY_CRUISE_HEIGHT = 30.0f;   // above the higher of start and portal
        constexpr float FLY_ARRIVE_DISTANCE = 6.0f;  // 2D distance at which the descent to the portal starts
        constexpr float FLY_SIDE_DISTANCE = 25.0f;   // the descent point beside the portal (ROUTE_FLY_SIDE_APPROACH)
        constexpr uint32 MOUNT_CAST_WAIT_MS = 4 * IN_MILLISECONDS;
        constexpr int32 BREATH_MAX_MS = 3 * MINUTE * IN_MILLISECONDS;  // Player::getMaxTimer(BREATH_TIMER)
        constexpr int32 BREATH_REFILL_RATE = 10;                         // Player::HandleDrowning above water
        constexpr int32 BREATH_SURFACE_MS = BREATH_MAX_MS * 40 / 100;    // surface below 40 %
        constexpr int32 BREATH_RESUME_MS = BREATH_MAX_MS * 90 / 100;     // and dive again from 90 %
        constexpr uint32 SURFACE_HOLD_MS = 2 * IN_MILLISECONDS;          // bot AI paused while it swims up
        using Leader::Travel::DEEP_TARGET_DEPTH;  // [lead107] RouteTargets.h
        using Leader::Travel::DRY_SPOT_SPREAD;  // [lead107] RouteEvents.h
        // A target whose approach a skipped neighbour pack stops is skipped at once (Steer, approach stop).
        constexpr bool ROUTE_SKIP_BEHIND_SKIPPED = true;
        // The pull wait (PULL_WAIT_MS) runs per stop, not per target: a new target at the same stop keeps it.
        constexpr bool ROUTE_STOP_KEEPS_WAIT = true;
        constexpr float STOP_SAME_RANGE = 5.0f;
        constexpr uint32 STOP_KEEP_GAP_MS = 15 * IN_MILLISECONDS;
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
        constexpr uint32 STEER_STUCK_MS = 20 * IN_MILLISECONDS;
        // [steer_rest_not_stuck] The leader seated with food / drink (RestState: the aura by its spell specific) out of
        // combat is no lack of progress: st-n build 23 (103125 / 103436 -2 / -3) - all 7 "route end" skips of the
        // Sunken Temple defenders were the paladin tank drinking at 29-34 % mana for 20 s (phase "traveling", the
        // group not waited for above its line), and each skip sent the walk to the next defender and back.
        constexpr bool STEER_REST_NOT_STUCK = true;
        constexpr float STEER_TWIN_RANGE = 30.0f;  // same-entry mobs skipped with an unreachable one (a ledge of them)
        using Leader::Travel::STEER_GAP_MS;  // [lead107] RouteEvents.h
        constexpr uint32 STARTER_STUCK_RETRY_MS = 2 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 STARTER_NO_NEARER_MS = 90 * IN_MILLISECONDS;
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
        constexpr uint32 STARTER_OBJECT_STUCK_SKIP_MS = 10 * MINUTE * IN_MILLISECONDS;  // StarterTrack::stuckUntilMs
        // A target on another navmesh part next to the route's end (a ramp without polygons): walked over straight.
        constexpr float GAP_CROSS_MAX_DISTANCE = 45.0f;
        constexpr uint32 GAP_CROSS_TRIES = 3;
        using Leader::Travel::GAP_REACHABLE_LEFT;  // [lead107] RouteLegs.h
        using Leader::Travel::GAP_REACHABLE_DZ;  // [lead107] RouteLegs.h
        using Leader::Travel::GAP_SWIM_ABOVE;  // [lead107] RouteLegs.h
        // AssistMember: a member with attackers this far from the leader, or on another floor.
        constexpr float ASSIST_RANGE = 25.0f;
        constexpr float ASSIST_FLOOR_DZ = 5.0f;
        // MeetMember: a living member this far behind the leader out of combat (the far-wait threshold is 50 yd).
        constexpr float MEET_RANGE = 45.0f;
        // [meet_hysteresis] STN92_EFFICIENCY 2026-10-25 (lagging hunter, "[regroup] ping-pong meet <-> route",
        // meet_stuck x2, "gives way ... far 276 s"): a meet started past MEET_RANGE ended the moment the member was
        // back inside it - at the edge, so the route's next step put it past the far line again and the leader walked
        // back again. The member being met stays met until it is within MEET_END_RANGE (one gather: the leader walks
        // toward it, its own follow toward the leader); TickParty's far wait uses the same line
        // (REGROUP_MEET_END_RANGE, TestHarnessMgr.cpp).
        // [meet_end_follow_trail] 30 yd (was 20): a following bot trails a walking leader 13-31 yd on ramps, so 20 yd
        // flipped the far wait on every step (wc-h wave 23: "hold meet" 15 flips in 109 s, 19 in 159 s, 22 in 292 s).
        // [b31_far_threshold] 15 yd (was 30): the meet ends with the member close, the group goes on together; the far
        // wait counts it far down to the same line (FAR_RESUME_RANGE, TestHarnessMgr.cpp)
        constexpr float MEET_END_RANGE = 15.0f;
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
        constexpr float DRIVE_STEP = 8.0f;
        using Leader::Travel::DRIVE_SAME_GOAL;  // [lead107] RouteEvents.h
        using Leader::Travel::EVENT_CHAIN_TRASH_RADIUS;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_CHAIN_NARROW;  // [lead107] RouteTargets.h
        using Leader::Travel::EVENT_CHAIN_JOIN_RANGE;  // [lead107] RouteTargets.h
        using Leader::Travel::FAR_TARGET_PATH;  // [lead107] RouteTargets.h
        using Leader::Travel::LEAD_SPOT_AHEAD_MARGIN;  // [lead107] RouteTargets.h
        using Leader::Travel::LEAD_SPOT_NEAR_RANGE;  // [lead107] RouteTargets.h
        using Leader::Travel::INTRO_WAIT_MAX_MS;  // [lead107] RouteTargets.h
        // Farther than this from the intro's boss the leader first walks on to the event spot (Svala picks her first
        // target within 100 yd).
        constexpr float INTRO_WAIT_RANGE = 80.0f;
        using Leader::Travel::introWaitSinceMs;  // [lead107] RouteTargets.h
        using Leader::Travel::ritualHoldUntilMs;  // [lead107] RouteTargets.h
        // [dmw-guard-body-pull] (party seq, guard guid) -> careful refusals of a pending door guard.
        std::map<std::pair<uint32, ObjectGuid>, uint32> guardRefusals;
        // once-per-target / once-per-boss log keys (party seq, guid)
        std::set<std::pair<uint32, ObjectGuid>> guardApproachLogged;
        // [guard_approach_arrived] (party seq, guard guid) -> when its "walking" line was logged; erased on arrival
        std::map<std::pair<uint32, ObjectGuid>, uint32> guardApproachSinceMs;
        // [steer_rest_not_stuck] (party seq, target guid): the leader's own rest during that walk, logged once
        std::set<std::pair<uint32, ObjectGuid>> steerRestLogged;
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
        // A boss whose body pull brought no fight: tried again after this (Oculus wave 9/10: Drakos "not pulled" at
        // 372 / 415 s; the 10 minute skip left nothing to do before him on foot).
        constexpr uint32 BOSS_NOT_PULLED_SKIP_MS = 2 * MINUTE * IN_MILLISECONDS;
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
        constexpr float WING_ARRIVE_RANGE = 40.0f;
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
        constexpr bool ROUTE_APPROACH_TO_SIGHT = true;
        constexpr bool ROUTE_LOG_TANK_REJECTS = true;
        // Refused-walk pass (2026-10-24, *.bak_refusedwalk), false restores the behaviour before it of that piece:
        // - approach to sight never walks into the aggro reach (AggroReach, the margin included) of an idle mob of the
        //   target's or the blocker's pack that sees the point, unless the tank's careful planner accepted a plan for
        //   that pack (TankPlanVerdict); the leader holds at the edge instead. RFC 20:08 (user: "the tank aggroed one
        //   mob, then two more - why?"): the planner refused the 3-trogg pack, the sight point 10 yd on lay inside
        //   the trogg's real aggro range, the walk there body-pulled the pack;
        // - a boss the group stands ready for is body-pulled after BOSS_PULL_READY_MS, not after PULL_WAIT_MS (RFC
        //   20:10: Oggleflint, the tank's planner never picks a boss, 45 s stood for nothing).
        constexpr bool ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED = true;
        constexpr bool ROUTE_BOSS_PULL_IMMEDIATE = true;
        // Harness walks that are no approach to their target (a body pull past its own pack, MeetMember's walk back)
        // stop at the aggro edge of every other idle pack (wave 7 scholo-h / mt-h / mt-n / lbrs-n mega-pulls: the
        // tank's last intent "test runner move", no [pull plan], refused 20-mob packs pulled by the walk).
        constexpr bool ROUTE_HARNESS_WALK_GUARD = true;
        constexpr uint32 HARNESS_HOLD_LOG_MS = 15 * IN_MILLISECONDS;
        using Leader::Travel::ROUTE_BOSS_ROOM_FIRST;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_SWIM_UP;  // [lead107] RouteLegs.h
        // [hleft-hor-reentry-hold] halls_of_reflection.h: DATA_MARWYN 1, CenterPos; areatrigger_teleport 5636.
        constexpr uint32 HOR_FALRIC_ENTRY = 38112;
        constexpr uint32 HOR_MARWYN_ENTRY = 38113;
        constexpr uint32 HOR_MARWYN_SLOT = 1;
        constexpr float HOR_CENTRE_X = 5309.46f;
        constexpr float HOR_CENTRE_Y = 2006.48f;
        constexpr float HOR_ENTRANCE_X = 5239.01f;
        constexpr float HOR_ENTRANCE_Y = 1932.64f;
        constexpr float HOR_ENTRANCE_Z = 707.70f;
        constexpr float HOR_HOLD_FROM_CENTRE = 60.0f;  // the script's 40 yd start range plus a margin
        constexpr uint32 HOR_HOLD_LOG_MS = 30 * IN_MILLISECONDS;
        using Leader::Travel::MECHANAR_MAP_ID;  // [lead107] RouteTargets.h
        constexpr uint32 HOR_LICH_KING_ENTRY = 36954;  // [hleft-never-pulled]
        constexpr uint32 HOR_LICH_KING_ESCAPE_ENTRY = 37226;
        constexpr uint32 AN_HADRONOX_ENTRY = 28921;
        constexpr uint32 AK_TWILIGHT_INITIATE_ENTRY = 30114;  // [hleft-ak-jedoga-owns]
        constexpr uint32 AK_JEDOGA_ENTRY = 29310;
        // [hleft-swim-back] the swim trail (TestHarnessMgr::Steer)
        constexpr bool ROUTE_SWIM_BACK = true;
        constexpr float SWIM_TRAIL_STEP = 5.0f;
        constexpr size_t SWIM_TRAIL_MAX = 200;
        constexpr float SWIM_CLIMB_DZ = 2.0f;  // a target this far above the swimmer is no climb out of the water
        constexpr float WAYPOINT_REACHED_SWIM = 3.0f;
        constexpr uint32 SWIM_BACK_MAX_MS = 2 * MINUTE * IN_MILLISECONDS;
        using Leader::Travel::ROUTE_STARTER_REARM;  // [lead107] RouteTargets.h
        using Leader::Travel::ROUTE_GATHER_MEMBERS;  // [lead107] RouteEvents.h
        // - FlyToPortal descends beside the portal (FLY_SIDE_DISTANCE out on its axis), not onto it.
        constexpr bool ROUTE_FLY_SIDE_APPROACH = true;
        using Leader::Travel::ROUTE_CHAIN_GUARD;  // [lead107] RouteTargets.h
        constexpr bool ROUTE_TWIN_SKIP = true;
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
        std::unordered_map<uint32, float> starterNearestPath;   // instance id -> shortest remaining walk to the starter
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
        constexpr bool ROUTE_REFUSE_DEAD_END = true;
        // A partial leg is judged only while the target is farther than GAP_CROSS_MAX_DISTANCE: nearer, the gap cross,
        // the own move and the "pull from here" of steer_stuck keep their say.
        // Below a cliff: the target stands this much above the end (above the water surface there, a swimmer rises)
        // and the rise is this share of the horizontal gap or more. Measured (route_backbone, 2026-10-24/25 logs):
        // Maraudon Earth Song Falls rise 71 over 95 / 113 yd (0.74 / 0.63) - dead ends; the Mechanar stairs to
        // Sepethrea 28 over 83 (0.34), Gundrak's Gal'darah altar 23 over 36 (0.64, killed in every run) and
        // Ahn'kahet's ramps (7.8 over 20.8) - walked on by the core's own move, kept by the 40 yd minimum.
        constexpr float DEAD_END_CLIFF_MIN_RISE = 40.0f;
        constexpr float DEAD_END_CLIFF_RATIO = 0.6f;
        // Ends no nearer: the end must gain this much on the walker's own distance (yd, or that share of it).
        constexpr float DEAD_END_MIN_GAIN = 5.0f;
        constexpr float DEAD_END_MIN_GAIN_SHARE = 0.1f;
        // Only when the walk to the end is this long: a leader next to the end walks it (Underbog build 30: 11.6 yd
        // from the root's foot 222.4,-154.1 Musel'ek was refused "ends no nearer", the own move there is the climb).
        constexpr float DEAD_END_WALK_MIN = 25.0f;
        // The same end of the same target where a "route end" skip (steer_stuck) already left it: no walk back to it.
        // Recorded by SkipTarget only - not every end the leader stood at (Underbog build 30: the steep knob
        // 232.3,-194.4,12.4 under the root, where the own move climbs, was refused "same dead end again" 11 times).
        constexpr float DEAD_END_SAME_END = 5.0f;
        constexpr float DEAD_END_SAME_GOAL = 10.0f;
        constexpr float DEAD_END_PROGRESS = 10.0f;
        constexpr uint32 DEAD_END_MEMORY_MS = 10 * MINUTE * IN_MILLISECONDS;
        // A known way on: an end (or goal) this near a route link's start or far end (RouteLinkDef: the Underbog root
        // to Musel'ek's terrace) is never refused.
        constexpr float DEAD_END_LINK_RANGE = 30.0f;
        // Never a stall: DEAD_END_FALLBACK_REFUSALS refusals within DEAD_END_FALLBACK_WINDOW_MS put the leader back on
        // the old walk (the partial route, the own move at its end, steer_stuck) for DEAD_END_FALLBACK_MS.
        constexpr uint32 DEAD_END_FALLBACK_REFUSALS = 3;
        constexpr uint32 DEAD_END_FALLBACK_WINDOW_MS = 3 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 DEAD_END_FALLBACK_MS = 10 * MINUTE * IN_MILLISECONDS;

        struct DeadEndVisit
        {
            G3D::Vector3 end;
            G3D::Vector3 goal;
            float distance = 0.0f;  // walker to goal while it stood at the end
            uint32 ms = 0;
            ObjectGuid target;
        };
        std::unordered_map<ObjectGuid, std::vector<DeadEndVisit>> deadEndVisits;  // per walker, world thread only

        struct DeadEndRefusals
        {
            std::vector<uint32> ms;      // recent refusals
            uint32 fallbackUntilMs = 0;  // the old walk until then
        };
        std::unordered_map<ObjectGuid, DeadEndRefusals> deadEndRefusals;  // per leader, world thread only

        struct DeadEndVerdict
        {
            std::string why;  // empty: walking to the end is progress (or not judged here)
            float distance = 0.0f;
            float left = 0.0f;
            float hgap = 0.0f;
            float dz = 0.0f;
            float rise = 0.0f;
            float surface = INVALID_HEIGHT;
            bool underwater = false;
            uint32 visitSec = 0;
        };

        DeadEndVerdict JudgeIncompleteEnd(Player* walker, G3D::Vector3 const& end, G3D::Vector3 const& goal,
                                          uint32 pathType, ObjectGuid target);

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

        // [incomplete_route_deadend] Is walking to the end of a partial leg progress towards goal? The why of a no
        // (Steer then skips the target as "route end", rechecked once the leader stands elsewhere), empty for a yes or
        // when the target is near enough for the gap cross / own move / "pull from here". Records the ends the walker
        // stood at (DeadEndVisit) and forgets one once the walker got nearer to the goal than it was there.
        // Every Steer tick, whatever the leg: an end the walker got past (nearer to the goal by DEAD_END_PROGRESS than
        // it stood there - the Mechanar stairs climbed by the core's own move) was no dead end.
        void ForgetPassedDeadEnds(Player* walker, G3D::Vector3 const& goal)
        {
            auto const found = deadEndVisits.find(walker->GetGUID());
            if (found == deadEndVisits.end() || found->second.empty())
                return;
            G3D::Vector3 const here(walker->GetPositionX(), walker->GetPositionY(), walker->GetPositionZ());
            float const distance = (goal - here).length();
            uint32 const now = getMSTime();
            std::vector<DeadEndVisit>& visits = found->second;
            visits.erase(std::remove_if(visits.begin(), visits.end(),
                                        [&](DeadEndVisit const& visit)
                                        {
                                            return getMSTimeDiff(visit.ms, now) > DEAD_END_MEMORY_MS ||
                                                   ((visit.goal - goal).length() <= DEAD_END_SAME_GOAL &&
                                                    distance + DEAD_END_PROGRESS < visit.distance);
                                        }),
                         visits.end());
        }

        DeadEndVerdict JudgeIncompleteEnd(Player* walker, G3D::Vector3 const& end, G3D::Vector3 const& goal,
                                          uint32 pathType, ObjectGuid target)
        {
            DeadEndVerdict verdict;
            G3D::Vector3 const here(walker->GetPositionX(), walker->GetPositionY(), walker->GetPositionZ());
            verdict.distance = (goal - here).length();
            verdict.left = (goal - end).length();
            verdict.hgap = std::sqrt((goal.x - end.x) * (goal.x - end.x) + (goal.y - end.y) * (goal.y - end.y));
            verdict.dz = goal.z - end.z;
            verdict.surface = WaterSurfaceAt(walker, end);
            verdict.underwater = verdict.surface > INVALID_HEIGHT && verdict.surface > end.z + 1.0f;
            // A swimmer rises to the surface by itself: the climb counts from there.
            verdict.rise = goal.z - (verdict.surface > INVALID_HEIGHT ? std::max(end.z, verdict.surface) : end.z);
            float const walk = (end - here).length();
            uint32 const now = getMSTime();

            if (verdict.distance <= GAP_CROSS_MAX_DISTANCE)
                return verdict;
            // A known way on: the end or the goal at a route link (the link walk / the own move there carry on).
            for (RouteLinkDef const* link : RouteLinks(walker->GetMapId()))
            {
                G3D::Vector3 const linkFrom(link->fromX, link->fromY, link->fromZ);
                G3D::Vector3 const linkTo(link->toX, link->toY, link->toZ);
                if ((linkFrom - end).length() <= DEAD_END_LINK_RANGE ||
                    (linkTo - end).length() <= DEAD_END_LINK_RANGE ||
                    (linkFrom - goal).length() <= DEAD_END_LINK_RANGE ||
                    (linkTo - goal).length() <= DEAD_END_LINK_RANGE)
                    return verdict;
            }

            ForgetPassedDeadEnds(walker, goal);
            DeadEndVisit const* visited = nullptr;
            if (auto const found = deadEndVisits.find(walker->GetGUID()); found != deadEndVisits.end())
                for (DeadEndVisit const& visit : found->second)
                    if ((visit.end - end).length() <= DEAD_END_SAME_END &&
                        (visit.target == target || (visit.goal - goal).length() <= DEAD_END_SAME_GOAL))
                        visited = &visit;

            // A target off the navmesh (FARFROMPOLY_END: hovering, on a model without polygons) says nothing about
            // the climb.
            bool const cliff = !(pathType & PATHFIND_FARFROMPOLY_END) && verdict.left > GAP_REACHABLE_LEFT &&
                               verdict.rise > DEAD_END_CLIFF_MIN_RISE &&
                               verdict.rise > DEAD_END_CLIFF_RATIO * verdict.hgap;
            // An end in water: a swim (the Ghaz'an lake shortcut, the swim to the Black Stalker) goes on in 3D - only
            // the cliff test.
            bool const water = verdict.surface > INVALID_HEIGHT;
            if (cliff)
                verdict.why = verdict.underwater ? "ends in water below a cliff" : "ends below a cliff";
            else if (!water && visited && walk > DEAD_END_SAME_END * 2.0f)
            {
                verdict.why = "same dead end again";
                verdict.visitSec = getMSTimeDiff(visited->ms, now) / IN_MILLISECONDS;
            }
            else if (!water && walk > DEAD_END_WALK_MIN &&
                     verdict.left + std::max(DEAD_END_MIN_GAIN, DEAD_END_MIN_GAIN_SHARE * verdict.distance) >=
                         verdict.distance)
                verdict.why = "ends no nearer";
            return verdict;
        }

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

        // [incomplete_route_deadend] The never-a-stall fallback: true while the leader walks partial routes the old
        // way. A refusal counts; the DEAD_END_FALLBACK_REFUSALS-th within the window starts it (started: this call).
        bool DeadEndFallback(Player* leader, bool refusal, bool& started)
        {
            started = false;
            uint32 const now = getMSTime();
            DeadEndRefusals& refusals = deadEndRefusals[leader->GetGUID()];
            if (refusals.fallbackUntilMs && int32(refusals.fallbackUntilMs - now) > 0)
                return true;
            refusals.fallbackUntilMs = 0;
            refusals.ms.erase(std::remove_if(refusals.ms.begin(), refusals.ms.end(),
                                             [now](uint32 ms)
                                             { return getMSTimeDiff(ms, now) > DEAD_END_FALLBACK_WINDOW_MS; }),
                              refusals.ms.end());
            if (!refusal)
                return false;
            refusals.ms.push_back(now);
            if (refusals.ms.size() < DEAD_END_FALLBACK_REFUSALS)
                return false;
            refusals.ms.clear();
            uint32 const until = now + DEAD_END_FALLBACK_MS;
            refusals.fallbackUntilMs = until ? until : 1;
            started = true;
            return true;
        }

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
        constexpr float AGGRO_EDGE_MARGIN = 8.0f;  // one harness tick of walking (7 yd/s) stays outside aggro
        constexpr float SAME_FLOOR_DZ = 3.0f;

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

        // The leader stands at the aggro edge of an unengaged mob of the target's pack: on its floor, in line of
        // sight, no farther than its aggro range plus the margin. The pull logic takes over from here.
        bool AtPackAggroEdge(Player* leader, Creature* target)
        {
            std::list<Creature*> pack;
            PackMobCheck check{leader, target};
            Acore::CreatureListSearcher<PackMobCheck> searcher(target, pack, check);
            Cell::VisitObjects(target, searcher, PACK_RADIUS);
            for (Creature* mob : pack)
                if (leader->GetExactDist(mob) <= mob->GetAggroRange(leader) + AGGRO_EDGE_MARGIN &&
                    std::fabs(leader->GetPositionZ() - mob->GetPositionZ()) <= SAME_FLOOR_DZ &&
                    leader->IsWithinLOSInMap(mob))
                    return true;
            return false;
        }

        // Approach stop (route pass 4): the leader's waypoint never lies inside the aggro reach of an idle hostile
        // around the target (SV-n wave 11: the waypoint 30 yd ahead lay in a refused pack; arriving at its edge only
        // dropped the waypoint, the walk under way went on and pulled 10 mobs). A player stops outside the aggro
        // range of every idle mob, plus a margin, and pulls or skips from there.
        constexpr float APPROACH_SCAN_RADIUS = 45.0f;  // around the target: its pack and the neighbours on the way
        using Leader::Travel::APPROACH_AGGRO_MARGIN;  // [lead107] RoutePatrol.h
        constexpr float APPROACH_SAMPLE_STEP = 1.0f;
        constexpr float APPROACH_ARRIVED = 3.0f;
        // AggressiveTargetValue MAX_PULL_HEIGHT_DIFF (12 yd), less a margin.
        constexpr float TANK_PULL_HEIGHT = 11.5f;
        // AggressiveTargetValue's MAX_PULL_HEIGHT_DIFF today (18 yd): beyond it the tank lists a mob as rejected
        // (TankRejects) for its height. Only for the "approach ignores" line; the registry itself decides.
        constexpr float TANK_REJECT_HEIGHT = 18.0f;
        constexpr uint32 TANK_REJECT_LOG_MS = 10 * IN_MILLISECONDS;
        std::unordered_map<uint64, uint32> tankRejectLogMs;  // mob -> last "approach ignores" line, world thread only
        // Approach to sight (ROUTE_APPROACH_TO_SIGHT): from a stop that does not see its blocker, the route ahead is
        // sampled every APPROACH_SAMPLE_STEP up to APPROACH_SIGHT_PATH yd for the first point that sees it, at most
        // APPROACH_SIGHT_TRIES line-of-sight tests.
        constexpr float APPROACH_SIGHT_PATH = 15.0f;
        constexpr uint32 APPROACH_SIGHT_TRIES = 25;
        // The keep-out tests of approach to sight (ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED): line-of-sight tests from the
        // route points inside a pack mob's aggro reach, a budget of their own.
        constexpr uint32 APPROACH_SIGHT_KEEP_OUT_TRIES = 40;

        using Leader::Fight::IdleAggroCheck;  // [lead107] the leader's idle-mob test

        // The route's own stretch is searched too, not only the target's surroundings: around the leader, the walked
        // length plus this (the largest aggro reach of a dungeon mob). Stratholme wave of 2026-10-24 09:27 / 09:48:
        // the route to a Ravaged Cadaver 108 yd away was checked only against the mobs within 45 yd of it.
        constexpr float APPROACH_ROUTE_SCAN_MARGIN = 40.0f;
        using Leader::Travel::APPROACH_WANDER_MAX;  // [lead107] RoutePatrol.h
        // A mob more than this above or below the tank stands on another floor (a ledge, a gallery over the hall):
        // it stops the walk only where the route reaches its floor. RFC wave 37: a trogg 23-25 yd from the group,
        // the targets on the ledges 13-23 yd above it, held every route at (-173.4, 1.8, -52.4) and the run stalled.
        constexpr float APPROACH_OTHER_FLOOR_DZ = 5.0f;
        using Leader::Travel::APPROACH_FOLLOW_SPREAD;  // [lead107] RoutePatrol.h

        using Leader::Travel::RealAggroRange;  // [lead107] RouteTargets.h

        using Leader::Travel::AggroReach;  // [lead107] RoutePatrol.h

        using Leader::Travel::PATROL_WAIT_SCAN;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_WAIT_ASSIST;  // [lead107] RoutePatrol.h
        constexpr float PATROL_WAIT_STEP = 5.0f;
        constexpr float PATROL_WAIT_STEP_MAX = 40.0f;
        using Leader::Travel::PATROL_WAIT_TRAIL_MAX;  // [lead107] RoutePatrol.h
        using Leader::Travel::PATROL_WAIT_TRAIL_FLOOR;  // [lead107] RoutePatrol.h
        bool PatrolWaitSpot(Player* leader, G3D::Vector3& spot, Creature*& nearMob)
        {
            std::list<Creature*> idle;
            ShadowMobCheck check{leader};
            Acore::CreatureListSearcher<ShadowMobCheck> searcher(leader, idle, check);
            Cell::VisitObjects(leader, searcher, PATROL_WAIT_SCAN);
            auto const reaches = [&](float x, float y, float z) -> Creature*
            {
                for (Creature* mob : idle)
                    if (mob->GetDefaultMovementType() != WAYPOINT_MOTION_TYPE &&
                        mob->GetExactDist(x, y, z) <= AggroReach(mob, leader) + PATROL_WAIT_ASSIST)
                        return mob;
                return nullptr;
            };
            nearMob = reaches(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
            if (!nearMob)
                return false;
            float const away = nearMob->GetAngle(leader);
            for (float step = PATROL_WAIT_STEP; step <= PATROL_WAIT_STEP_MAX; step += PATROL_WAIT_STEP)
                for (float turn : {0.0f, 0.5f, -0.5f, 1.0f, -1.0f})
                {
                    float const x = leader->GetPositionX() + std::cos(away + turn) * step;
                    float const y = leader->GetPositionY() + std::sin(away + turn) * step;
                    float const ground = leader->GetMap()->GetHeight(leader->GetPhaseMask(), x, y,
                                                                     leader->GetPositionZ() + 3.0f, true, 10.0f);
                    if (ground <= INVALID_HEIGHT || !DirectLineOverGround(leader, x, y, ground) ||
                        reaches(x, y, ground))
                        continue;
                    spot = G3D::Vector3(x, y, ground);
                    return true;
                }

            // [patrol_wait_trail_back] No clear spot straight away from the nearest idle mob (a room full of packs,
            // a corridor whose sides are walls): the group steps back along the way it came, the tank's own trail
            // (MovementAction::TankTrailPoints, every 2 yd), to the first point out of every idle mob's reach and
            // assistance. Walked already, the way back pulls nothing new. scholo-h w24 (115805-scholo-h-5A-34,
            // pull 11), scholo-n w23 and brd-n w24 logged "no clear spot" and waited in the patrol's path next to the
            // neighbour pack; the patrol fight called it in (wipe analysis w24-25, cause C).
            std::vector<G3D::Vector3> const trail = MovementAction::TankTrailPoints(leader);
            G3D::Vector3 const here(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
            float walkedBack = 0.0f;
            G3D::Vector3 previous = here;
            for (auto point = trail.rbegin(); point != trail.rend(); ++point)
            {
                walkedBack += (*point - previous).length();
                previous = *point;
                if (walkedBack > PATROL_WAIT_TRAIL_MAX)
                    break;
                if (walkedBack < PATROL_WAIT_STEP || std::fabs(point->z - here.z) > PATROL_WAIT_TRAIL_FLOOR ||
                    reaches(point->x, point->y, point->z))
                    continue;
                spot = *point;
                return true;
            }
            return false;
        }

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

        // ROUTE_UNSEEN_ROUTE_END_PASS (wave 21, *.bak_w21idle): a mob skipped as "route end" while the leader could
        // not see it (no walk to its sight point) stops no walk by the follower-spread rule any more (unseenStop): the
        // spread stop held the boss approach at the edge of what nobody can see nor reach. sm-h wave 21: Scarlet
        // Monk / Diviner behind the Library shelves, "route end", then Arcanist Doan's approach stopped by them
        // (unseenStop) 64 s, the run idle 138 s. A mob that sees the route point still stops it (line of sight).
        constexpr bool ROUTE_UNSEEN_ROUTE_END_PASS = true;
        std::unordered_map<uint64, uint32> unseenRouteEndUntilMs;  // creature guid -> until (world thread)
        std::unordered_map<uint64, uint32> unseenRouteEndLogMs;

        void MarkUnseenRouteEnd(Creature* mob, uint32 untilMs)
        {
            if (ROUTE_UNSEEN_ROUTE_END_PASS && mob)
                unseenRouteEndUntilMs[mob->GetGUID().GetRawValue()] = untilMs ? untilMs : 1;
        }

        bool UnseenRouteEnd(Player* leader, Creature* mob)
        {
            if (!ROUTE_UNSEEN_ROUTE_END_PASS)
                return false;
            auto const found = unseenRouteEndUntilMs.find(mob->GetGUID().GetRawValue());
            if (found == unseenRouteEndUntilMs.end())
                return false;
            uint32 const now = getMSTime();
            if (int32(now - found->second) >= 0)
            {
                unseenRouteEndUntilMs.erase(found);
                return false;
            }
            uint32& loggedMs = unseenRouteEndLogMs[mob->GetGUID().GetRawValue()];
            if (!loggedMs || getMSTimeDiff(loggedMs, now) > 30 * IN_MILLISECONDS)
            {
                loggedMs = now ? now : 1;
                LOG_INFO("playerbots",
                         "[approach] {}: unseen_route_end_pass: {} ({:.1f} yd) stops no walk unseen - skipped as "
                         "route end out of the leader's sight",
                         leader->GetName(), mob->GetName(), leader->GetExactDist(mob));
            }
            return true;
        }

        using Leader::Travel::JoinsBossFight;  // [lead107] RouteTargets.h

        // [hleft-boss-own-pack] A mob that comes into `boss`'s fight by itself when he is pulled - his formation or a
        // script call for help (JoinsBossFight "formation" / "script") - is his encounter, not a neighbour pack: the
        // careful planner refuses such a pack as "brings a boss, pulled as his fight" (ChooseTargetActions), so a
        // harness that stops the boss's walk at it waits for a pull nobody makes. Razorfen Downs heroic 2026-10-25
        // (20261025-070358-rfd-h-5A-78): the Splinterbone Skeleton pack "joins boss: script" refused for Mordresh,
        // Mordresh's approach stopped at it as a neighbour ("harness_hold_refused_pack" boss approach) until the stall
        // skipped him.
        constexpr bool ROUTE_BOSS_OWN_ENCOUNTER_PACK = true;

        bool BossEncounterMob(Player* leader, Creature* boss, Creature* mob)
        {
            if (!ROUTE_BOSS_OWN_ENCOUNTER_PACK || !leader || !boss || !mob || boss == mob)
                return false;
            char const* joins = JoinsBossFight(leader, boss, mob);
            return joins && (std::strcmp(joins, "joins boss: formation") == 0 ||
                             std::strcmp(joins, "joins boss: script") == 0);
        }

        // [b33_dme] A boss whose wander or aggro reach covers `mob`'s stand (the careful planner's "wanders to the
        // spot" / "aggro reaches spot" for an idle boss): pulling `mob` is his fight, the planner refuses its pack as
        // "brings a boss, pulled as his fight once the group is ready for him". Dire Maul East 5H (bear tank)
        // 20261026-000434-dme-n-5H-40: the Wildspawn Hellcaller (a caster, no lone-mob pull) 25 yd from Hydrospawn's
        // pool stopped every walk to Zevrim; boss_on_the_way_first fired only for a blocker that IS a boss, the body
        // pull was held at the refused Hellcaller ("harness_hold_refused_pack" body pull) every 10 s, 0/4 at 24 min.
        constexpr bool ROUTE_BOSS_BROUGHT_BY_PACK = true;  // [b33_dme] false: the build-32 behaviour

        bool BossBroughtBy(Player* leader, Creature* boss, Creature* mob)
        {
            if (!ROUTE_BOSS_BROUGHT_BY_PACK || !leader || !boss || !mob || boss == mob || !boss->IsAlive() ||
                !mob->IsAlive() || boss->IsInCombat() || boss->IsImmuneToPC() || !boss->IsHostileTo(leader))
                return false;
            if (BossEncounterMob(leader, boss, mob))
                return true;
            if (std::fabs(boss->GetPositionZ() - mob->GetPositionZ()) > APPROACH_OTHER_FLOOR_DZ)
                return false;
            float const reach = AggroReach(boss, leader);
            float const apart = std::min(boss->GetExactDist2d(mob), boss->GetHomePosition().GetExactDist2d(
                                                                        mob->GetPositionX(), mob->GetPositionY()));
            return apart <= reach && boss->IsWithinLOSInMap(mob);
        }

        // [b33_harn] The boss's own room pack: an idle mob on the boss's floor within BOSS_ROOM_TRASH_RADIUS of him
        // that he sees but the leader does not (no pull of its own from here: the planner never sees it, verdict
        // "none", 0 mobs) is part of the boss pull - the walk to the boss does not stop at it. scholo-h
        // 20261026-000434-scholo-h-5A-3 (2453-2572 s): every body pull of Vectus held at a Scholomance Student 31.8 yd
        // off ("harness_hold_refused_pack", verdict none, realPack 3-4, no line of sight from the tank), then
        // pull_after_hold targeted the unseen Student, "route end", Vectus skipped 300 s. A footprint mob the leader
        // DOES see stays a stop: it is pulled first when cheap (TickBossAreaFirst / boss-area trash).
        constexpr bool ROUTE_BOSS_ROOM_PACK = true;  // false: the build-33 behaviour

        bool BossRoomPack(Player* leader, Creature* boss, Creature* mob)
        {
            if (!ROUTE_BOSS_ROOM_PACK || !leader || !boss || !mob || boss == mob || !boss->IsAlive() ||
                !mob->IsAlive() || boss->IsInCombat() || mob->IsInCombat() ||
                !(boss->IsDungeonBoss() || boss->isWorldBoss()))
                return false;
            if (std::fabs(boss->GetPositionZ() - mob->GetPositionZ()) > APPROACH_OTHER_FLOOR_DZ ||
                boss->GetExactDist(mob) > BOSS_ROOM_TRASH_RADIUS)
                return false;
            return boss->IsWithinLOSInMap(mob) && !leader->IsWithinLOSInMap(mob);
        }

        constexpr bool ROUTE_STOP_ONLY_REAL_AGGRO = true;  // [stop_only_real_aggro] false: the margin stops too
        constexpr float STOP_REAL_SLACK = 1.5f;

        // [b34_petramp] The Z gate (Creature::CanStartAttack: no proximity aggro on a unit more than
        // CREATURE_Z_ATTACK_RANGE + m_CombatDistance above / below the mob, object sizes off) keeps the TANK idle on a
        // ramp, never the group: the followers and pets stand lower or further ahead, and a flier has no gate at all.
        // scholo-h 20261026-014521-scholo-h-5H-3 (116-133 s): the stop on the Reliquary ramp at (221.1,51.6,122.1)
        // stood 6.3 yd above the hall floor, 20.8 yd from a Scholomance Neophyte (real aggro 21.0, approach_stop
        // insideRealAggro true) - the gated ramp points counted as out of reach, so the stop lay where the gate first
        // let the mob see the leader; the hunter's Cat by the tank opened the 33-mob pull 5 (wipe). A point inside the
        // real 3D range of a mob that sees it is no stop whatever the Z gate says: the stop goes back up the stairs,
        // out of the floor's sight (a line-of-sight pull from there), or back along the tank's trail when the leader
        // stands inside already (STOP_TRAIL_BACK_MAX).
        constexpr bool ROUTE_STOP_IGNORES_Z_GATE = true;  // [b34_petramp] false: the build-33 gated stop
        constexpr float STOP_TRAIL_BACK_MAX = 30.0f;
        constexpr float STOP_TRAIL_FLOOR = 8.0f;  // a trail point this far above / below the leader is another floor

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
            if (!LeaderPicksFor(party))
                return AoePull::Planned(party.leader, mob, sinceMs);
            auto const& lead = party.lead;
            if (!lead.aoePlanMs || getMSTimeDiff(lead.aoePlanMs, getMSTime()) > LEADER_VERDICT_TTL_MS ||
                std::find(lead.aoePlan.begin(), lead.aoePlan.end(), mob) == lead.aoePlan.end())
                return false;
            if (sinceMs)
                *sinceMs = lead.aoePlanSinceMs;
            return true;
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
            if (!LeaderPicksFor(party))
                return AggressiveTargetValue::Explain(leader, unit);
            return AggressiveTargetValue::Explain(leader, unit,
                                                  party.targetIsStarter ? ObjectGuid::Empty : party.target);
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

        bool ApproachStop(Player* leader, WorldObject* target, Movement::PointsArray const& points, float length,
                          G3D::Vector3& stop, Creature*& blocker, bool& unseenStop, Creature* exemptPack = nullptr,
                          Party const* party = nullptr)
        {
            unseenStop = false;
            std::list<Creature*> mobs;
            IdleAggroCheck check{leader};
            Acore::CreatureListSearcher<IdleAggroCheck> searcher(target, mobs, check);
            Cell::VisitObjects(target, searcher, APPROACH_SCAN_RADIUS);
            std::list<Creature*> routeMobs;
            Acore::CreatureListSearcher<IdleAggroCheck> routeSearcher(leader, routeMobs, check);
            Cell::VisitObjects(leader, routeSearcher, length + APPROACH_ROUTE_SCAN_MARGIN);
            for (Creature* mob : routeMobs)
                if (std::find(mobs.begin(), mobs.end(), mob) == mobs.end())
                    mobs.push_back(mob);
            // A mob the tank rejected (another floor, no path) holds nothing: nobody would pull it (Gundrak wave 32:
            // the walk stopped at a Drakkari Lancer the tank listed as unreachable). Thread-safe registry.
            // The tank's reject list holds its height rejects too (AggressiveTargetValue: more than
            // TANK_REJECT_HEIGHT above or below it); "approach ignores" names each mob it kept from holding the walk,
            // once per TANK_REJECT_LOG_MS. (RFC 18:19, the trogg on the ledge was not one: the tank's "1 height" was
            // another mob, the trogg was out of its line of sight - approach to sight in Steer.)
            // [b30_mech_pos] ...except the walk's own target and its pack: the harness walks to it anyway, so a
            // reject (no full path: pos-n 20261025-184229, the ambush Ymirjar Wrathbringer summoned on a rock at
            // 911.9,63.4,547.7, the path ending 7.7 yd from it at 904.2,62.2,542.9) let the walk run into its pack
            // instead of stopping at the edge, and the tank pulled 7 mobs mounted. The edge is the pull's place.
            Creature* const walkTarget = target ? target->ToCreature() : nullptr;
            mobs.remove_if(
                [leader, walkTarget, party](Creature* mob)
                {
                    if (!TankRejects::IsRejected(leader->GetGUID(), mob->GetGUID()))
                        return false;
                    if (walkTarget && (mob == walkTarget || mob->GetExactDist(walkTarget) <= PACK_RADIUS))
                        return false;
                    // A pack the tank's careful planner refused keeps stopping the walk even when the tank rejects it
                    // as unseen: walked through, it is the mega-pull (wave 7 scholo-h / mt-h / mt-n / lbrs-n: the
                    // harness walk into a refused 20-mob pack, no [pull plan] before the pull).
                    bool accepted = true;
                    uint32 planMobs = 0;
                    // [lead5] the leader planner's own memory with LEADER_ONE_PICKER (PartyVerdict)
                    if ((party ? PartyVerdict(*party, mob->GetGUID(), accepted, planMobs)
                               : CarefulPlanVerdict::Get(leader->GetGUID(), mob->GetGUID(), accepted, planMobs)) &&
                        !accepted)
                        return false;
                    if (ROUTE_LOG_TANK_REJECTS && leader->GetExactDist(mob) < AggroReach(mob, leader))
                    {
                        uint32 const now = getMSTime();
                        uint32& loggedMs = tankRejectLogMs[mob->GetGUID().GetRawValue()];
                        if (!loggedMs || getMSTimeDiff(loggedMs, now) > TANK_REJECT_LOG_MS)
                        {
                            loggedMs = now;
                            bool const height =
                                std::fabs(mob->GetPositionZ() - leader->GetPositionZ()) > TANK_REJECT_HEIGHT;
                            LOG_INFO("playerbots",
                                     "[approach] {}: approach ignores {}: tank rejects ({}) {:.1f} yd, dz {:.1f}",
                                     leader->GetName(), mob->GetName(),
                                     height ? "height" : "unseen / unreachable", leader->GetExactDist(mob),
                                     mob->GetPositionZ() - leader->GetPositionZ());
                        }
                    }
                    return true;
                });
            // A locked Forge Master (UKIds::IsForgeMasterLocked) stops no walk: engaged he only evades home, and
            // standing at his edge the tank's nearest pull is him (Utgarde Keep 10:44: the leader stopped 1 yd from
            // master 3 on the way to forge 2 and never left). Retarget keeps the route out of his reach instead.
            if (ROUTE_FORGE_ORDER)
                if (InstanceScript const* instance = leader->GetInstanceScript())
                    mobs.remove_if([instance](Creature* mob) { return UKIds::IsForgeMasterLocked(mob, instance); });
            // Fast respawners (ROUTE_FAST_RESPAWN_PASS) are walked past.
            mobs.remove_if([](Creature* mob) { return FastRespawner(mob); });
            // exemptPack (a body pull's target): its own pack - the mobs within PACK_RADIUS of it and its formation -
            // is the pull and stops nothing; every other idle mob stops the walk.
            // [hleft-boss-own-pack] ... and a boss's encounter mobs (BossEncounterMob) with him.
            if (exemptPack)
                mobs.remove_if(
                    [exemptPack, leader](Creature* mob)
                    {
                        return mob == exemptPack || mob->GetExactDist(exemptPack) <= PACK_RADIUS ||
                               (exemptPack->GetFormation() && mob->GetFormation() == exemptPack->GetFormation()) ||
                               ((exemptPack->IsDungeonBoss() || exemptPack->isWorldBoss() ||
                                 mob->GetExactDist(exemptPack) <= BOSS_ROOM_TRASH_RADIUS) &&
                                BossEncounterMob(leader, exemptPack, mob)) ||
                               // [b33_dme] a boss's walk: the mob his wander / aggro reach brings is his fight
                               ((exemptPack->IsDungeonBoss() || exemptPack->isWorldBoss()) &&
                                BossBroughtBy(leader, exemptPack, mob)) ||
                               BossRoomPack(leader, exemptPack, mob);  // [b33_harn]
                    });
            if (mobs.empty())
                return false;

            // reach: AggroReach (real aggro plus the margin). holds: the distance inside which the mob stops the
            // walk - its reach, or the leader's own distance when it stands in the margin already: from there only a
            // step closer to the mob stops it. DM wave 37: the leader stood 22.4 yd from a refused Miner (reach 23),
            // and every route, also the ones leading away from it, stopped at its first sample - the leader's own
            // position - until the 4-minute stall. otherFloor: more than APPROACH_OTHER_FLOOR_DZ above or below the
            // leader (the tank).
            // unseenHolds: the same for the follower spread (APPROACH_FOLLOW_SPREAD, real aggro plus the spread, no
            // line of sight), only at route points on the mob's floor (SAME_FLOOR_DZ).
            struct MobReach
            {
                Creature* mob;
                float reach;
                float holds;
                float unseenHolds;
                bool otherFloor;
            };
            std::vector<MobReach> reaches;
            reaches.reserve(mobs.size());
            for (Creature* mob : mobs)
            {
                float const reach = AggroReach(mob, leader);
                float const distance = leader->GetExactDist(mob);
                reaches.push_back({mob, reach, std::min(reach, distance),
                                   std::min(RealAggroRange(mob, leader) + APPROACH_FOLLOW_SPREAD, distance),
                                   std::fabs(mob->GetPositionZ() - leader->GetPositionZ()) > APPROACH_OTHER_FLOOR_DZ});
            }

            // Only a mob that sees the point stops the walk: a creature aggroes in line of sight only
            // (Creature::MoveInLineOfSight), and the tank chooses its pull among the mobs it sees ("possible targets").
            // Wave 12: the leader stopped behind corners and doors at the reach of mobs it could not see, the tank had
            // no target and every levelling normal stalled at its entrance. A mob on another floor than the tank
            // counts only at the points of the route on its own floor.
            // The followers are not on the leader's eye line, though: a mob on the point's floor (SAME_FLOOR_DZ) stops
            // the walk at its real aggro range plus APPROACH_FOLLOW_SPREAD without the line of sight (Stockade wave
            // 40: a cell wall hid the Captive from the leader's point, its open door did not hide the shaman). A
            // mob the tank stood at and did not see is marked as not seen (TankRejects, tank_view) and stops no
            // later walk, as before. `unseen` tells which rule stopped it.
            auto inside = [&reaches, leader](G3D::Vector3 const& point, bool& unseen) -> Creature*
            {
                for (MobReach const& entry : reaches)
                {
                    if ((!entry.otherFloor ||
                         std::fabs(point.z - entry.mob->GetPositionZ()) <= APPROACH_OTHER_FLOOR_DZ) &&
                        AggroesAtPoint(entry.mob, leader, point, entry.holds, true))
                    {
                        unseen = false;
                        return entry.mob;
                    }
                    if (std::fabs(point.z - entry.mob->GetPositionZ()) <= SAME_FLOOR_DZ &&
                        AggroesAtPoint(entry.mob, leader, point, entry.unseenHolds, false) &&
                        !UnseenRouteEnd(leader, entry.mob))
                    {
                        unseen = true;
                        return entry.mob;
                    }
                }
                return nullptr;
            };

            // The route sampled every APPROACH_SAMPLE_STEP yd up to `length`, from the leader's own position.
            std::vector<G3D::Vector3> samples;
            samples.emplace_back(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
            float walked = 0.0f;
            for (size_t index = 1; index < points.size() && walked < length; ++index)
            {
                G3D::Vector3 const from = index == 1 ? samples.front() : points[index - 1];
                G3D::Vector3 const segment = points[index] - from;
                float const segmentLength = segment.length();
                if (segmentLength <= 0.0f)
                    continue;

                float along = 0.0f;
                while (along < segmentLength && walked + along < length)
                {
                    along = std::min(along + APPROACH_SAMPLE_STEP, segmentLength);
                    samples.push_back(from + segment * (along / segmentLength));
                }
                walked += segmentLength;
            }

            // [stop_only_real_aggro] User 2026-10-25 (scholo-h 153805, 15:56:04: approach_stop on a Necromancer pack
            // at 28.8 yd, insideRealAggro false, the tank stood with nothing to do): a mob whose real aggro range
            // (RealAggroRange plus STOP_REAL_SLACK; on its floor without line of sight, the follower rule, else seen)
            // this stretch of the route never enters stops no walk - its margin is only passed. The followers keep
            // to the tank's distance from an idle mob (MovementActions CollectIdleReaches [melee_behind_tank]) and
            // the pets out of every reach ([pet_idle_reach]). A route that enters the real range later stops there
            // on a later tick, at the margin point it stands in then.
            if (ROUTE_STOP_ONLY_REAL_AGGRO)
            {
                reaches.erase(std::remove_if(reaches.begin(), reaches.end(),
                                             [&samples, leader](MobReach const& entry)
                                             {
                                                 float const real = RealAggroRange(entry.mob, leader) + STOP_REAL_SLACK;
                                                 for (G3D::Vector3 const& point : samples)
                                                 {
                                                     float const dz = std::fabs(point.z - entry.mob->GetPositionZ());
                                                     if (dz <= SAME_FLOOR_DZ &&
                                                         AggroesAtPoint(entry.mob, leader, point, real, false))
                                                         return false;
                                                     if ((!entry.otherFloor || dz <= APPROACH_OTHER_FLOOR_DZ) &&
                                                         AggroesAtPoint(entry.mob, leader, point, real, true))
                                                         return false;
                                                 }
                                                 return true;
                                             }),
                              reaches.end());
                if (reaches.empty())
                    return false;
            }

            for (size_t index = 0; index < samples.size(); ++index)
            {
                bool unseen = false;
                blocker = inside(samples[index], unseen);
                if (!blocker)
                    continue;

                // The spread stop is the last point before the spread reach (nothing to see from there anyway).
                unseenStop = unseen;
                size_t stopIndex =
                    index > 0 && (unseen || SeesFromPoint(leader, blocker, samples[index - 1])) ? index - 1 : index;
                // The tank chooses no pull farther above or below it than AggressiveTargetValue's height limit: on a
                // ramp the stop moves on down within the margin, still outside every mob's real aggro range (ST
                // wave 15: stopped 12.3 yd above the Witch Doctor, the tank never saw it, the run stalled there).
                if (std::fabs(samples[stopIndex].z - blocker->GetPositionZ()) > TANK_PULL_HEIGHT)
                    for (size_t next = stopIndex + 1; next < samples.size(); ++next)
                    {
                        G3D::Vector3 const& point = samples[next];
                        bool aggroes = false;
                        for (MobReach const& entry : reaches)
                        {
                            bool zGated = false;  // [b34_petramp] the gate keeps only the tank idle there
                            aggroes |= AggroesAtPoint(entry.mob, leader, point, entry.reach - APPROACH_AGGRO_MARGIN,
                                                      false, &zGated) ||
                                       (ROUTE_STOP_IGNORES_Z_GATE && zGated);
                        }
                        if (aggroes)
                            break;
                        if (std::fabs(point.z - blocker->GetPositionZ()) <= TANK_PULL_HEIGHT &&
                            SeesFromPoint(leader, blocker, point))
                        {
                            stopIndex = next;
                            break;
                        }
                    }
                // A corner that shows the mob only from inside its real aggro range (no margin) is no place to stop:
                // the mob sees the leader there and comes (Stratholme 09:36: the street corner at 3644.9,-3538.5 was
                // the stop with the cadaver it showed 15.6-18.9 yd away, inside its 18-19 yd real reach). The stop
                // goes back to the last point no idle mob both reaches and sees; the tank pulls from there or
                // the target is skipped as not seen (tank_view).
                auto const aggroesSeen = [&reaches, leader](G3D::Vector3 const& point)
                {
                    for (MobReach const& entry : reaches)
                    {
                        bool zGated = false;
                        if (AggroesAtPoint(entry.mob, leader, point, entry.reach - APPROACH_AGGRO_MARGIN, true,
                                           &zGated))
                            return true;
                        // [b34_petramp] inside the real 3D range, kept idle by the Z gate only: the mob sees the point
                        if (ROUTE_STOP_IGNORES_Z_GATE && zGated && SeesFromPoint(leader, entry.mob, point))
                            return true;
                    }
                    return false;
                };
                while (stopIndex > 0 && aggroesSeen(samples[stopIndex]))
                    --stopIndex;
                stop = samples[stopIndex];
                // [b34_petramp] the leader stands inside such a reach already: back along its own trail (walked, it
                // pulls nothing new) to the first point no idle mob reaches and sees, on its floor
                if (ROUTE_STOP_IGNORES_Z_GATE && stopIndex == 0 && aggroesSeen(samples[0]))
                {
                    std::vector<G3D::Vector3> const trail = MovementAction::TankTrailPoints(leader);
                    G3D::Vector3 previous = samples[0];
                    float walkedBack = 0.0f;
                    for (auto point = trail.rbegin(); point != trail.rend(); ++point)
                    {
                        walkedBack += (*point - previous).length();
                        previous = *point;
                        if (walkedBack > STOP_TRAIL_BACK_MAX)
                            break;
                        if (std::fabs(point->z - samples[0].z) > STOP_TRAIL_FLOOR || aggroesSeen(*point))
                            continue;
                        LOG_INFO("playerbots",
                                 "[approach] {}: stop_out_of_real_reach: the leader stands where an idle mob reaches "
                                 "and sees it (blocker {}, {:.1f} yd) - back {:.1f} yd along its trail to ({:.1f},"
                                 "{:.1f},{:.1f}) [b34_petramp]",
                                 leader->GetName(), blocker->GetName(), leader->GetExactDist(blocker), walkedBack,
                                 point->x, point->y, point->z);
                        stop = *point;
                        break;
                    }
                }
                return true;
            }
            return false;
        }

        // ROUTE_HARNESS_WALK_GUARD: the stop of a harness walk before the aggro reach of the idle mobs on its route
        // (ApproachStop around the leader, or around exemptPack, whose own pack is exempt). True: `point` is the stop,
        // `held`: the leader stands there already.
        bool HarnessWalkStop(Player* leader, Movement::PointsArray const& points, float length, Creature* exemptPack,
                             G3D::Vector3& point, Creature*& blocker, bool& held, Party const* party = nullptr)
        {
            G3D::Vector3 stop;
            bool unseenStop = false;
            blocker = nullptr;
            held = false;
            WorldObject* const centre = exemptPack ? static_cast<WorldObject*>(exemptPack) : leader;
            if (!ApproachStop(leader, centre, points, length, stop, blocker, unseenStop, exemptPack, party) || !blocker)
                return false;
            G3D::Vector3 const here(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
            held = (stop - here).length() <= APPROACH_ARRIVED;
            point = stop;
            return true;
        }

        std::unordered_map<uint64, uint32> harnessHoldLogMs;  // blocker -> last harness_hold_refused_pack, world thread

        using Leader::Travel::MechBridgeLifts;  // [lead107] RouteTargets.h
        using Leader::Travel::mechBridgeLifts;  // [lead107] RouteTargets.h

        using Leader::Travel::SightStop;  // [lead107] RouteTargets.h
        using Leader::Travel::sightStops;  // [lead107] RouteTargets.h

        // [pull_after_hold] User 2026-10-25 (scholo-h 153805, «тупят, излишне ждут»): every approach stop, guarantee
        // hold and careful-pull refusal of a pack on the way or on the required route ends PULL_AFTER_HOLD_MS after
        // the group stands fully ready (GroupFullyReady) at it: the deliberate pull - the blocker behind an openable
        // closed door has its door opened first (a lattice Iron Gate, 15:42-15:44: no line of sight, nobody opened it,
        // the 120 s idle event ended the stand), then the route blocker target (its sight pull, else its walk in) or
        // the body pull - instead of the 45 s wait, the planner's stand guarantee or a skip. A patrol / overlap wait
        // of the planner keeps its own short clock. Per leader, world thread.
        constexpr bool PULL_AFTER_HOLD = true;
        constexpr uint32 PULL_AFTER_HOLD_MS = 10 * IN_MILLISECONDS;
        constexpr uint32 PULL_AFTER_HOLD_GATE_MS = 20 * IN_MILLISECONDS;  // the body pull's gate lets it through
        using Leader::Dungeon::DOOR_WALK_MS;  // [lead107] LeaderPlan.h (15 s)

        struct HoldReady
        {
            ObjectGuid target;
            uint32 sinceMs = 0;  // since when the group stands ready at this target (0: not ready)
            Position at;         // where the leader stood when the clock began (a new target there keeps it)
        };
        std::unordered_map<ObjectGuid, HoldReady> holdReady;

        struct PullAfterHoldGate
        {
            ObjectGuid blocker;
            uint32 untilMs = 0;
        };
        std::unordered_map<ObjectGuid, PullAfterHoldGate> pullAfterHoldGates;  // by leader

        bool PullAfterHoldPasses(ObjectGuid leader, ObjectGuid blocker)
        {
            auto const found = pullAfterHoldGates.find(leader);
            return found != pullAfterHoldGates.end() && found->second.blocker == blocker &&
                   int32(found->second.untilMs - getMSTime()) > 0;
        }

        // [b33_bodypull] The classes of a planner refusal (CarefulPlanVerdict's why, ChooseTargetActions.cpp) the
        // harness reads before a walk-in. A READINESS refusal (the healer not with the tank / the group, a member
        // missing, dead, sitting below its line or buffing, not topped up) is a wait the walk-in can never end: no
        // pull_after_hold clock nor the planner's time-out overrides it, the group holds. An OVER-CAP refusal (the
        // pack does not fit, over the cap, a split search still running) is the planner's own [b32_smpull] split /
        // guarantee to take: the harness never walks the whole group into it (strat-h 20261026-000434-5D-1: a 4-6 mob
        // Ravaged Cadaver blocker the planner held as "does not fit" chained to 17 on the walk-in). Only the rest -
        // the stuck route / no plan from range class ("no pull of X from range ... the route's body pull decides",
        // "given up for range", a within-cap pack the stand's guarantee takes) - is the harness's walk-in to make.
        bool RefusalIsReadiness(std::string const& why)
        {
            static char const* const READINESS[] = {
                "not topped up", "not all here", "not with the group", "not with the tank", "from the tank",
                "rests below its line", " buffing", "the group rests or gathers", "group not ready",
                "a group member still fights", "the tank rests", "waits for ", "resurrection sickness", " dead",
                "no dps with the group", "% mana", "% health"};
            for (char const* const part : READINESS)
                if (why.find(part) != std::string::npos)
                    return true;
            return false;
        }

        using Leader::Travel::RefusalIsOverCap;  // [lead107] RouteEvents.h

        // [pull_after_hold] (4) the walk to a closed door that opens by use, between the leader and a blocker it
        // cannot see: kept over the ticks until it is open (the stop would walk the leader back).
        struct DoorWalk
        {
            ObjectGuid door;
            ObjectGuid blocker;
            G3D::Vector3 point;
            uint32 untilMs = 0;
        };
        std::unordered_map<ObjectGuid, DoorWalk> doorWalks;  // by leader

        // [body_pull_via_planner] plan29 follow-up 4 (build 29: harness body pulls / walks were 13 % of all pulls,
        // and the unplanned rest died five times as often as the planned pulls): before the harness walks the
        // leader into a trash pack, the tank's planner is asked to take it (GuaranteeRequest: its guarantee pull of
        // the pack as it stands, at range when it has a pull, with its readiness check) for BODY_PULL_PLANNER_MS;
        // only then the walk in. Bosses keep their body pull (the planner never picks a boss). Per leader.
        constexpr bool BODY_PULL_VIA_PLANNER = true;
        constexpr uint32 BODY_PULL_PLANNER_MS = 8 * IN_MILLISECONDS;
        struct PlannerAsk
        {
            ObjectGuid target;
            uint32 sinceMs = 0;
        };
        std::unordered_map<ObjectGuid, PlannerAsk> plannerAsks;

        // [b33_aoepull] A required route blocker the planner called hopeless (its split search exhausted) is held
        // AOE_PULL_ASK_MS for the planner's aoe_pull_plan (AoePull::MarkRequired / Planned, ChooseTargetActions.cpp)
        // before the dead end: mgt-h 20261026-000434-mgt-h-5G-16 (Sunblade Mage Guard before Delrissa, "with it 21
        // mobs, over 18 (no split in 19 searches)") and strat-n 20261026-000434-strat-n-5A-2 (King's Square) stood
        // refused on both sides - the planner's hopeless verdict and the harness's refused walk - until stopped. While
        // the planner's issued aoe_pull_plan is fresh the route's walk-in holds AOE_PULL_WALK_MS (the tank pulls to its
        // stack spot itself); only an issued plan the tank has not started by then is walked in (via the planner ask).
        constexpr uint32 AOE_PULL_ASK_MS = 10 * IN_MILLISECONDS;
        constexpr uint32 AOE_PULL_ASK_RESET_MS = 5 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 AOE_PULL_WALK_MS = 30 * IN_MILLISECONDS;
        std::unordered_map<ObjectGuid, PlannerAsk> aoePullAsks;  // by leader

        bool AoePullAskRuns(ObjectGuid leaderGuid, ObjectGuid blocker, uint32 now)
        {
            PlannerAsk& ask = aoePullAsks[leaderGuid];
            if (ask.target != blocker || !ask.sinceMs || getMSTimeDiff(ask.sinceMs, now) > AOE_PULL_ASK_RESET_MS)
            {
                ask.target = blocker;
                ask.sinceMs = now ? now : 1;
            }
            return getMSTimeDiff(ask.sinceMs, now) < AOE_PULL_ASK_MS;
        }

        // True while the planner still has its time (the caller holds the leader); `first`: the ask began now.
        bool AskPlannerFirst(ObjectGuid leaderGuid, Creature* target, uint32 now, bool& first)
        {
            first = false;
            PlannerAsk& ask = plannerAsks[leaderGuid];
            if (ask.target != target->GetGUID() || !ask.sinceMs ||
                getMSTimeDiff(ask.sinceMs, now) > 4 * BODY_PULL_PLANNER_MS)
            {
                ask.target = target->GetGUID();
                ask.sinceMs = now ? now : 1;
                first = true;
            }
            if (getMSTimeDiff(ask.sinceMs, now) >= BODY_PULL_PLANNER_MS)
                return false;
            GuaranteeRequest::Set(leaderGuid, target->GetGUID(), false);
            return true;
        }

        // [behind_root_pull] the skipped pack a walk stopped at is retargeted at most this often (world thread)
        constexpr bool BEHIND_ROOT_PULL = true;
        constexpr uint32 BEHIND_ROOT_TRIES = 2;
        constexpr uint32 BEHIND_ROOT_WINDOW_MS = 5 * MINUTE * IN_MILLISECONDS;
        struct BehindRootTry
        {
            uint32 firstMs = 0;
            uint32 count = 0;
        };
        std::unordered_map<ObjectGuid, BehindRootTry> behindRootTries;

        // [pull_estimate_vs_actual] the last PullEstimate result the leader's JSONL has
        std::unordered_map<ObjectGuid, uint32> pullEstimateSeen;

        // The first point of the route ahead (from the leader, up to APPROACH_SIGHT_PATH yd) from which the tank's eye
        // line reaches the blocker on a pullable height (TANK_PULL_HEIGHT) - the corner the stop lay before. A point
        // inside the real aggro range of the blocker is fine only for a pack the tank's planner accepted (keepOut
        // empty): walking into its sight is the pull. keepOut (ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED): the idle mobs of the
        // target's and the blocker's packs while no plan for them is accepted - a point inside the aggro reach
        // (AggroReach, the margin included) of one that sees it is a body pull of a pack the planner did not take:
        // the search ends there, heldAtEdge (the route only goes deeper from there). For a blocker outside the
        // target's pack, no point from which the target sees the leader inside its real aggro range (one pack at a
        // time). At most APPROACH_SIGHT_TRIES line-of-sight tests.
        bool FindSightPoint(Player* leader, Creature* blocker, Creature* target, bool ownPack,
                            Movement::PointsArray const& points, std::vector<Creature*> const& keepOut,
                            G3D::Vector3& sight, bool& heldAtEdge)
        {
            heldAtEdge = false;
            G3D::Vector3 const here(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
            float const targetAggro = RealAggroRange(target, leader);
            uint32 tries = 0;
            uint32 keepOutTries = 0;
            float walked = 0.0f;
            for (size_t index = 1; index < points.size() && walked < APPROACH_SIGHT_PATH; ++index)
            {
                G3D::Vector3 const from = index == 1 ? here : points[index - 1];
                G3D::Vector3 const segment = points[index] - from;
                float const segmentLength = segment.length();
                if (segmentLength <= 0.0f)
                    continue;

                float along = 0.0f;
                while (along < segmentLength && walked + along < APPROACH_SIGHT_PATH)
                {
                    along = std::min(along + APPROACH_SAMPLE_STEP, segmentLength);
                    G3D::Vector3 const point = from + segment * (along / segmentLength);
                    // Every point of the walk, not only the one that shows the blocker: the way there passes them. As
                    // in ApproachStop, a leader standing in a mob's margin already is held only by a step closer to
                    // it (or one into its real aggro range).
                    for (Creature* mob : keepOut)
                    {
                        // [sight-arc] the real aggro distance + the shared small margin (was AggroReach: +5 yd)
                        float const real = RealAggroRange(mob, leader);
                        float const holds = std::max(
                            std::min(real + MovementAction::SIGHT_IDLE_AGGRO_MARGIN, leader->GetExactDist(mob)), real);
                        bool zGated = false;  // [b34_petramp] the Z gate keeps only the tank idle there
                        if (!AggroesAtPoint(mob, leader, point, holds, false, &zGated) &&
                            !(ROUTE_STOP_IGNORES_Z_GATE && zGated))
                            continue;
                        // Out of tests: not shown to be safe, the leader holds.
                        if (keepOutTries++ >= APPROACH_SIGHT_KEEP_OUT_TRIES || SeesFromPoint(leader, mob, point))
                        {
                            heldAtEdge = true;
                            return false;
                        }
                    }
                    if (std::fabs(point.z - blocker->GetPositionZ()) > TANK_PULL_HEIGHT)
                        continue;
                    if (tries++ >= APPROACH_SIGHT_TRIES)
                        return false;
                    if (!SeesFromPoint(leader, blocker, point))
                        continue;
                    if (!ownPack && AggroesAtPoint(target, leader, point, targetAggro, false))
                    {
                        if (tries++ >= APPROACH_SIGHT_TRIES)
                            return false;
                        if (SeesFromPoint(leader, target, point))
                            return false;  // the target's sight comes first: no way to the blocker alone
                    }
                    sight = point;
                    return true;
                }
                walked += segmentLength;
            }
            return false;
        }

        // What the tank's careful planner said about a pack, as far as the harness reads it without the bot's AI
        // context (no cross-thread GetValue): the group's pre-pull crowd control plan (PrePullCc, behind its lock)
        // names the tank's planned pack - accepted, or refused as it stands (`heavy`, also a fallback plan: no
        // accepted one) - and the harness's own count of the pack's careful refusals (the PULL_WAIT_MS skips).
        // The planner's published verdict (CarefulPlanVerdict) is read first. Unknown otherwise: approach to sight
        // treats it as "not accepted" and keeps out of the pack's reach.
        enum class PlanVerdict
        {
            Unknown,
            Accepted,
            Refused
        };

        PlanVerdict TankPlanVerdict(Party const& party, Player* leader, std::vector<Creature*> const& pack)
        {
            // The planner's own published verdict (CarefulPlanVerdict, fresh for a few seconds) on any mob of the
            // pack comes first; a refusal of one of them is a refusal of the pack.
            bool anyAccepted = false;
            for (Creature* mob : pack)
            {
                bool accepted = false;
                uint32 mobs = 0;
                if (!PartyVerdict(party, mob->GetGUID(), accepted, mobs))  // [lead5]
                    continue;
                if (!accepted)
                    return PlanVerdict::Refused;
                anyAccepted = true;
            }
            if (anyAccepted)
                return PlanVerdict::Accepted;

            for (Creature* mob : pack)
            {
                auto const refused = party.carefulRefused.find(mob->GetGUID());
                if (refused != party.carefulRefused.end() && refused->second > 0)
                    return PlanVerdict::Refused;
            }
            PrePullCc::Plan plan;
            if (!PrePullCc::Get(leader, plan) || plan.tank != leader->GetGUID() || plan.pulledMs)
                return PlanVerdict::Unknown;
            for (Creature* mob : pack)
                if (plan.pullTarget == mob->GetGUID() ||
                    std::find(plan.pack.begin(), plan.pack.end(), mob->GetGUID()) != plan.pack.end())
                    return plan.heavy ? PlanVerdict::Refused : PlanVerdict::Accepted;
            return PlanVerdict::Unknown;
        }

        // The idle mobs of the target's pack and of the blocker's (PackMobCheck, PACK_RADIUS around each), bosses
        // left out (their body pull is the boss pull): what approach to sight keeps out of while no plan is accepted.
        std::vector<Creature*> SightKeepOutPack(Party const& party, Player* leader, Creature* target, Creature* blocker)
        {
            std::vector<Creature*> result;
            for (Creature* center : {target, blocker})
            {
                std::list<Creature*> pack;
                PackMobCheck check{leader, center};
                Acore::CreatureListSearcher<PackMobCheck> searcher(center, pack, check);
                Cell::VisitObjects(center, searcher, PACK_RADIUS);
                if (center->IsAlive() && !center->IsInCombat() &&
                    std::find(pack.begin(), pack.end(), center) == pack.end())
                    pack.push_back(center);
                for (Creature* mob : pack)
                    if (!party.bossCreatures.count(mob->GetGUID()) && !mob->IsDungeonBoss() &&
                        std::find(result.begin(), result.end(), mob) == result.end())
                        result.push_back(mob);
            }
            return result;
        }

        // The edge hold's give-way (SIGHT_UNKNOWN_HOLD_MS): the target's own pack may be walked into sight of now, no
        // other one. BRS heroic 22:10:30: the give-way cleared the whole keep-out, the sight point 15 yd on lay in the
        // Smolderthorn and Firebrand packs' aggro - 26 attackers, wipe. The idle hostiles within
        // SIGHT_GIVE_WAY_SCAN of the leader outside the target's pack (PACK_RADIUS around it), bosses included (a
        // neighbour boss is a boss pull nobody chose); FindSightPoint keeps every point out of their aggro
        // (IdleAggroBlocker) as it does for a refused pack.
        constexpr float SIGHT_GIVE_WAY_SCAN = 50.0f;  // APPROACH_SIGHT_PATH plus the widest idle aggro reach

        struct OtherPackMobCheck
        {
            Player* leader;
            Creature* target;
            bool operator()(Creature* creature) const
            {
                return creature->IsAlive() && !creature->IsInCombat() && !creature->IsCritter() &&
                       !creature->IsCivilian() && creature->IsHostileTo(leader) &&
                       creature->GetExactDist(target) > PACK_RADIUS;
            }
        };

        std::vector<Creature*> SightKeepOutOthers(Player* leader, Creature* target)
        {
            std::list<Creature*> mobs;
            OtherPackMobCheck check{leader, target};
            Acore::CreatureListSearcher<OtherPackMobCheck> searcher(leader, mobs, check);
            Cell::VisitObjects(leader, searcher, SIGHT_GIVE_WAY_SCAN);
            return std::vector<Creature*>(mobs.begin(), mobs.end());
        }

        // A leader held at the edge by approach to sight: the search is not run again for SIGHT_HOLD_RECHECK_MS (it
        // costs up to APPROACH_SIGHT_TRIES + APPROACH_SIGHT_KEEP_OUT_TRIES line-of-sight tests), the line is logged
        // once per SIGHT_HOLD_LOG_MS. Per leader, for one target and blocker. The search runs only with the leader at
        // its stop; 2.5 s, not 5 s: Utgarde Keep heroic 22:26:01 held at the edge, 22:26:11 skip at once, the pull at
        // 22:26:16 - two rechecks of idling the user noticed (*.bak_doorgate: 5 s).
        constexpr uint32 SIGHT_HOLD_RECHECK_MS = 2500;
        constexpr uint32 SIGHT_HOLD_LOG_MS = 10 * IN_MILLISECONDS;
        using Leader::Travel::SightHold;  // [lead107] RouteTargets.h
        using Leader::Travel::sightHolds;  // [lead107] RouteTargets.h

        // ROUTE_BOSS_PULL_IMMEDIATE: a boss the group stands ready for (GroupFullyReady, the boss buffs done, no
        // crowd control hold of the tank's) is body-pulled after this, not after PULL_WAIT_MS: the tank's careful
        // planner never picks a boss ("[pull hold] no aggressive target"), the wait was always spent for nothing.
        constexpr uint32 BOSS_PULL_READY_MS = 3 * IN_MILLISECONDS;
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


        // [pull_option] The tank's two ways to pull a pack, cheapest first (user 2026-10-25, MgT heroic Selin room):
        // (1) the sight pull - a spot on the arc round the pack (MovementAction::SightArcCandidates, the sight-arc
        // search the ranged / healers use) that sees it within the pull spell's range and stands outside every idle
        // mob's real aggro distance (GetAggroRange + combat distance + SIGHT_IDLE_AGGRO_MARGIN), with a walk there
        // that stays out of them too; (2) the body pull - the walk into the pack. For both the mobs they touch are
        // counted: every idle mob whose real aggro distance the walk or the spot enters (and that sees it), the pack
        // itself, and their links (formation, the core's call for assistance: same faction within the assistance
        // radius, in sight) - a boss among them is a boss pull nobody chose (Selin and her Wretched on AzerothCore).
        constexpr float PULL_SIGHT_RANGE = 30.0f;          // Hand of Reckoning, Avenger's Shield, Shoot / Throw
        constexpr float PULL_SIGHT_RANGE_SLACK = 1.5f;     // the spot stands this far inside it
        constexpr uint32 PULL_SIGHT_MAX_LOS = 120;         // target sight tests of the arc candidates
        constexpr uint32 PULL_SIGHT_MAX_PATHS = 8;         // candidates that get a path
        constexpr float PULL_SIGHT_WALK_RATIO = 2.5f;      // a walk longer than this x straight + extra is a detour
        constexpr float PULL_SIGHT_WALK_EXTRA = 25.0f;
        constexpr float PULL_SIGHT_PATH_END = 2.0f;        // the walk ends this close to its spot
        constexpr float PULL_TOUCH_STEP = 1.5f;            // the walk is tested every this many yards
        constexpr float PULL_IDLE_SCAN = 60.0f;            // idle mobs looked at, round the leader and the pack
        constexpr uint32 PULL_OPTION_MOB_CAP = 6;          // a pull that touches more is "over the cap"

        struct PullTouch
        {
            std::vector<Creature*> mobs;  // every mob the option brings
            uint32 extra = 0;             // of them, outside the intended pack (and its links)
            bool boss = false;            // a boss among them
            std::string bossName;
            std::string firstExtra;       // the first one outside the intended pack (for the log)
        };

        // The idle hostiles (IdleAggroCheck) within PULL_IDLE_SCAN of the leader or of the pack's mob.
        std::vector<Creature*> PullIdleMobs(Player* leader, Creature* target)
        {
            std::vector<Creature*> result;
            IdleAggroCheck check{leader};
            for (WorldObject* centre : {static_cast<WorldObject*>(leader), static_cast<WorldObject*>(target)})
            {
                std::list<Creature*> found;
                Acore::CreatureListSearcher<IdleAggroCheck> searcher(centre, found, check);
                Cell::VisitObjects(centre, searcher, PULL_IDLE_SCAN);
                for (Creature* mob : found)
                    if (std::find(result.begin(), result.end(), mob) == result.end())
                        result.push_back(mob);
            }
            if (check(target) && std::find(result.begin(), result.end(), target) == result.end())
                result.push_back(target);
            return result;
        }

        // The pack itself and its links: the target's pack (PACK_RADIUS), its formation, and what answers their
        // call for assistance (CanAnswerAssist with the faction test, within the assistance radius of a touched mob
        // plus ASSIST_LINK_MARGIN, in its sight), grown until nothing new comes.
        // build 27 (20261025-150737-mgt-h-5A-14): the assistance test had ASSIST_LINK_MARGIN on top (13 yd + both
        // sizes) and linked Selin's side packs to the centre pair (15.1 yd apart; the core's 10 yd + sizes = 13.3 does
        // not), so the "own pack" grew to the pair and every arc spot was "inside the pack's aggro". The core's own
        // radius now (AnyAssistCreatureInRangeCheck: IsWithinDistInMap, sizes included), and the own pack (`assist`
        // false: pack radius and formation) is told apart from the links it may call (`assist` true).
        void PullTouchClosure(Player* leader, std::vector<Creature*> const& idle, std::vector<Creature*>& touched,
                              bool withAssist = true)
        {
            float const assist =
                withAssist ? sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS) : 0.0f;
            for (size_t index = 0; index < touched.size() && touched.size() < 60; ++index)
            {
                Creature* source = touched[index];
                for (Creature* other : idle)
                {
                    if (other == source || std::find(touched.begin(), touched.end(), other) != touched.end())
                        continue;
                    bool const formation = source->GetFormation() && other->GetFormation() == source->GetFormation();
                    bool const pack = other->GetExactDist(source) <= PACK_RADIUS && other->IsHostileTo(leader);
                    bool const assists = assist > 0.0f && CanAnswerAssist(other, source, leader, true) &&
                                         source->IsWithinDistInMap(other, assist) &&
                                         source->IsWithinLOSInMap(other);
                    if (formation || pack || assists)
                        touched.push_back(other);
                }
            }
        }

        // The mobs a pull of `target` brings when the leader walks `walk` (the spot at its end): the target and its
        // links, every idle mob whose real aggro distance (+ SIGHT_IDLE_AGGRO_MARGIN) a walk point enters and that
        // sees the point (AggroesAtPoint: the core's Z gate, 3D range, the eye line), and their links. `intended`
        // gets the target with its links alone; `isBoss` tells a boss.
        PullTouch PullTouches(Player* leader, Creature* target, Movement::PointsArray const& walk,
                              std::vector<Creature*> const& idle, std::function<bool(Creature*)> const& isBoss,
                              std::vector<Creature*>& intended)
        {
            intended.clear();
            intended.push_back(target);
            PullTouchClosure(leader, idle, intended, false);

            PullTouch result;
            result.mobs = intended;
            PullTouchClosure(leader, idle, result.mobs);  // what the pack itself calls
            std::vector<Creature*> woken;
            for (size_t i = 1; i < walk.size(); ++i)
            {
                G3D::Vector3 const from = walk[i - 1];
                G3D::Vector3 const segment = walk[i] - from;
                float const length = segment.length();
                uint32 const steps = std::max<uint32>(1, uint32(std::ceil(length / PULL_TOUCH_STEP)));
                for (uint32 step = 0; step <= steps; ++step)
                {
                    G3D::Vector3 const point = from + segment * (float(step) / float(steps));
                    for (Creature* mob : idle)
                    {
                        if (std::find(result.mobs.begin(), result.mobs.end(), mob) != result.mobs.end() ||
                            std::find(woken.begin(), woken.end(), mob) != woken.end())
                            continue;
                        if (AggroesAtPoint(mob, leader, point,
                                           RealAggroRange(mob, leader) + MovementAction::SIGHT_IDLE_AGGRO_MARGIN,
                                           true))
                            woken.push_back(mob);
                    }
                }
            }
            if (!woken.empty())
            {
                PullTouchClosure(leader, idle, woken);
                for (Creature* mob : woken)
                    if (std::find(result.mobs.begin(), result.mobs.end(), mob) == result.mobs.end())
                        result.mobs.push_back(mob);
            }
            for (Creature* mob : result.mobs)
            {
                if (std::find(intended.begin(), intended.end(), mob) == intended.end())
                {
                    if (!result.extra)
                        result.firstExtra = mob->GetName();
                    ++result.extra;
                }
                if (!result.boss && isBoss(mob))
                {
                    result.boss = true;
                    result.bossName = mob->GetName();
                }
            }
            return result;
        }

        struct PullSightCandidate
        {
            G3D::Vector3 at;
            float straight = 0.0f;
            float radius = 0.0f;
            float angleOffDeg = 0.0f;
            float walk = -1.0f;  // the path's length (-1: no path searched / none)
            std::string verdict;  // "ok" or why not
            PullTouch touch;
        };

        struct PullSightSearch
        {
            bool found = false;
            PullSightCandidate best;           // the chosen sight spot (found)
            bool hasFallback = false;          // a pathable spot that sees the pack but touches more
            PullSightCandidate fallback;       // the least dangerous of them (no boss first, then the fewest mobs)
            uint32 candidates = 0;             // arc spots on the floor that see the pack
            uint32 inOtherAggro = 0;
            uint32 inOwnAggro = 0;
            uint32 noPath = 0;
            uint32 touchesMore = 0;
            uint32 touchesBoss = 0;
            float minRadius = 0.0f;
            float maxRadius = 0.0f;
            float margin = 0.0f;
            std::vector<PullSightCandidate> top;  // the first three looked at, with their verdicts
        };

        bool PullTouchBetter(PullTouch const& a, PullTouch const& b)
        {
            if (a.boss != b.boss)
                return !a.boss;
            return a.mobs.size() < b.mobs.size();
        }

        // [pull_option] (1) the sight pull spot for `target` (see above). The candidates nearest to the leader first;
        // of the clean ones (the spot and the walk touch the intended pack alone, no boss) the shortest walk.
        // A tight second pass: the core's aggro test is an exact 3D distance (Creature::CanStartAttack), so half a yard
        // is still safe; Selin's room on heroic (real aggro 21) has its side-pack spots only there (PATCH_selin.md).
        constexpr float PULL_SIGHT_TIGHT_MARGIN = 0.5f;
        constexpr size_t PULL_SIGHT_MAX_CENTRES = 6;  // pack members the arc is searched round

        void FindPullSightSpotWith(Player* leader, Creature* target, std::vector<Creature*> const& idle,
                                   std::function<bool(Creature*)> const& isBoss, float margin, PullSightSearch& out)
        {
            out = PullSightSearch();
            out.margin = margin;
            out.minRadius = RealAggroRange(target, leader) + margin;
            out.maxRadius = PULL_SIGHT_RANGE - PULL_SIGHT_RANGE_SLACK;
            if (out.minRadius > out.maxRadius)
                out.minRadius = out.maxRadius;

            // The own pack: the target, its pack radius and formation - not the packs its call may bring (those are
            // touched mobs, counted by PullTouches).
            std::vector<Creature*> intended;
            intended.push_back(target);
            PullTouchClosure(leader, idle, intended, false);

            // The arc round every member of the pack (the nearest to the leader first): a spot must stay outside
            // the aggro of all of them, and any member it sees within the pull range is the one to pull. Round the
            // target alone, every ring lay inside a neighbour member's aggro (build 27: 16-27 of 16-27 "inOwnAggro").
            std::vector<Creature*> centres = intended;
            std::sort(centres.begin(), centres.end(), [leader](Creature* a, Creature* b)
                      { return leader->GetExactDist(a) < leader->GetExactDist(b); });
            if (centres.size() > PULL_SIGHT_MAX_CENTRES)
                centres.resize(PULL_SIGHT_MAX_CENTRES);
            uint32 const losPerCentre = std::max<uint32>(12, PULL_SIGHT_MAX_LOS / uint32(centres.size()));
            std::vector<MovementAction::SightArcCandidate> spots;
            for (Creature* centre : centres)
            {
                float const minRadius = std::min(RealAggroRange(centre, leader) + margin, out.maxRadius);
                for (MovementAction::SightArcCandidate const& spot : MovementAction::SightArcCandidates(
                         leader, centre, minRadius, out.maxRadius, TANK_PULL_HEIGHT, losPerCentre))
                {
                    bool const duplicate = std::any_of(spots.begin(), spots.end(), [&spot](auto const& other)
                                                       { return std::fabs(other.x - spot.x) < 0.5f &&
                                                                std::fabs(other.y - spot.y) < 0.5f; });
                    if (!duplicate)
                        spots.push_back(spot);
                }
            }
            std::sort(spots.begin(), spots.end(),
                      [](auto const& a, auto const& b) { return a.straight < b.straight; });
            out.candidates = uint32(spots.size());
            uint32 paths = 0;
            auto const note = [&out](PullSightCandidate const& candidate)
            {
                if (out.top.size() < 3)
                    out.top.push_back(candidate);
            };
            for (MovementAction::SightArcCandidate const& spot : spots)
            {
                PullSightCandidate candidate;
                candidate.at = G3D::Vector3(spot.x, spot.y, spot.z);
                candidate.straight = spot.straight;
                candidate.radius = spot.radius;
                candidate.angleOffDeg = spot.angleOffDeg;
                // A walk is never shorter than the straight line: nothing farther beats the best one.
                if (out.found && candidate.straight >= out.best.walk)
                    break;

                // The spot itself: inside the pack's own aggro distance it is a body pull, not a sight pull;
                // inside another idle mob's (that sees it) it pulls that one.
                Creature* own = nullptr;
                for (Creature* mob : intended)
                    if (AggroesAtPoint(mob, leader, candidate.at, RealAggroRange(mob, leader) + margin, false))
                    {
                        own = mob;
                        break;
                    }
                if (own)
                {
                    ++out.inOwnAggro;
                    candidate.verdict = "inside the pack's aggro (" + own->GetName() + ")";
                    note(candidate);
                    continue;
                }
                Creature* other = nullptr;
                for (Creature* mob : idle)
                    if (std::find(intended.begin(), intended.end(), mob) == intended.end() &&
                        AggroesAtPoint(mob, leader, candidate.at, RealAggroRange(mob, leader) + margin, true))
                    {
                        other = mob;
                        break;
                    }
                if (other)
                {
                    ++out.inOtherAggro;
                    candidate.verdict = Acore::StringFormat("in the aggro of idle {} ({:.1f} yd of {:.1f})",
                                                            other->GetName(),
                                                            other->GetExactDist(candidate.at.x, candidate.at.y,
                                                                                candidate.at.z),
                                                            RealAggroRange(other, leader) + margin);
                    note(candidate);
                    continue;
                }

                if (paths >= PULL_SIGHT_MAX_PATHS)
                    break;
                ++paths;
                PathResult const path = RoutePath(leader, candidate.at.x, candidate.at.y, candidate.at.z);
                if (!path.reachable || path.points.size() < 2 ||
                    (path.end - candidate.at).length() > PULL_SIGHT_PATH_END ||
                    path.length > std::max(PULL_SIGHT_WALK_RATIO * candidate.straight,
                                           candidate.straight + PULL_SIGHT_WALK_EXTRA))
                {
                    ++out.noPath;
                    candidate.verdict = path.reachable ? Acore::StringFormat("detour ({:.0f} yd walk)", path.length)
                                                       : std::string("no path");
                    note(candidate);
                    continue;
                }
                candidate.walk = path.length;
                Movement::PointsArray walk = path.points;
                walk.push_back(candidate.at);
                std::vector<Creature*> intendedHere;
                candidate.touch = PullTouches(leader, target, walk, idle, isBoss, intendedHere);
                if (candidate.touch.boss || candidate.touch.extra)
                {
                    if (candidate.touch.boss)
                        ++out.touchesBoss;
                    else
                        ++out.touchesMore;
                    candidate.verdict =
                        candidate.touch.boss
                            ? "touches boss " + candidate.touch.bossName
                            : Acore::StringFormat("the walk touches {} more ({})", candidate.touch.extra,
                                                  candidate.touch.firstExtra);
                    if (!out.hasFallback || PullTouchBetter(candidate.touch, out.fallback.touch))
                    {
                        out.hasFallback = true;
                        out.fallback = candidate;
                    }
                    note(candidate);
                    continue;
                }

                candidate.verdict = "ok";
                note(candidate);
                if (!out.found || candidate.walk < out.best.walk)
                {
                    out.found = true;
                    out.best = candidate;
                }
            }
        }

        // The usual margin first, then the tight one; the fallback (a spot that touches more) of either pass.
        void FindPullSightSpot(Player* leader, Creature* target, std::vector<Creature*> const& idle,
                               std::function<bool(Creature*)> const& isBoss, PullSightSearch& out)
        {
            FindPullSightSpotWith(leader, target, idle, isBoss, MovementAction::SIGHT_IDLE_AGGRO_MARGIN, out);
            if (out.found)
                return;
            PullSightSearch tight;
            FindPullSightSpotWith(leader, target, idle, isBoss, PULL_SIGHT_TIGHT_MARGIN, tight);
            if (tight.found || (tight.hasFallback && !out.hasFallback))
                out = tight;
        }

        // The JSON of a search's candidates (sight_point_search / pull_option events).
        std::string PullCandidatesJson(std::vector<PullSightCandidate> const& top)
        {
            JsonArray array;
            for (PullSightCandidate const& candidate : top)
                array.Raw(JsonObject()
                              .Real("x", candidate.at.x).Real("y", candidate.at.y).Real("z", candidate.at.z)
                              .Real("straight", candidate.straight).Real("radius", candidate.radius)
                              .Real("angleOff", candidate.angleOffDeg).Real("walk", candidate.walk)
                              .Num("mobs", int64(candidate.touch.mobs.size())).Bool("boss", candidate.touch.boss)
                              .Str("verdict", candidate.verdict)
                              .Build());
            return array.Build();
        }

        using Leader::Travel::ROUTE_BOSS_AREA_CLEAR;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_AREA_ENCOUNTER_ADDS;  // [lead107] RouteTargets.h
        using Leader::Travel::ReachabilitySkip;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_AREA_UNSKIP_MS;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_AREA_MAX_UNSKIPS;  // [lead107] RouteTargets.h
        using Leader::Travel::BossAreaUnskip;  // [lead107] RouteTargets.h
        using Leader::Travel::bossAreaUnskips;  // [lead107] RouteTargets.h
        using Leader::Travel::bossAreaHoldLogMs;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_AREA_SCAN_RANGE;  // [lead107] RouteTargets.h
        // [b33_mech2] A boss's body pull waits for his killable area trash (BossAreaBodyPullHold) at most this long in
        // all, from its first hold; then it goes, logged "body pull: given up" (no deadlock on trash nothing pulls).
        constexpr uint32 BOSS_AREA_BODY_PULL_HOLD_MS = 4 * MINUTE * IN_MILLISECONDS;
        std::map<std::pair<uint32, ObjectGuid>, uint32> bossAreaBodyPullHoldMs;  // {party seq, boss} -> first hold

        using Leader::Travel::ROUTE_BOSS_FOOTPRINT;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_FOOTPRINT_PACK_MAX;  // [lead107] RouteTargets.h
        using Leader::Travel::BossFootprintDef;  // [lead107] RouteTargets.h
        using Leader::Travel::BOSS_FOOTPRINTS;  // [lead107] RouteTargets.h

        using Leader::Travel::InBossFootprint;  // [lead107] RouteTargets.h

        using Leader::Travel::BossAreaTrashOf;  // [lead107] RouteTargets.h

        // The next boss of the party's last route (Retarget's RouteSnapshot), nullptr when none is alive.
        Creature* RouteNextBoss(uint32 seq, Map* map)
        {
            auto const found = routeSnapshots.find(seq);
            if (!map || found == routeSnapshots.end() || found->second.nextBoss.IsEmpty())
                return nullptr;
            Creature* boss = map->GetCreature(found->second.nextBoss);
            return boss && boss->IsAlive() ? boss : nullptr;
        }

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

        // [b31_ubrs_door] Party::seq -> since when the leader's walk out of a closed area (AreaGateDef exit) holds at a
        // door that stays shut for good (world thread only).
        std::unordered_map<uint32, uint32> lockedInSince;
        constexpr uint32 LOCKED_IN_FAIL_MS = 2 * MINUTE * IN_MILLISECONDS;

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

        // [dmw-guard-body-pull] A living guard of a guarded door that is still closed (PendingDoorGuards).
        bool IsPendingDoorGuard(Map* map, Creature const* creature)
        {
            if (!map || !creature)
                return false;
            for (GuardedDoorDef const* def : GuardedDoors(map->GetId()))
            {
                if (std::find(def->guardEntries.begin(), def->guardEntries.end(), creature->GetEntry()) ==
                    def->guardEntries.end())
                    continue;
                for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
                    if (go && go->isSpawned() && go->GetEntry() == def->doorEntry)
                    {
                        std::vector<Creature*> const guards = PendingDoorGuards(map, go);
                        if (std::find(guards.begin(), guards.end(), creature) != guards.end())
                            return true;
                    }
            }
            return false;
        }

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


        // The tank picks its next pull itself (AggressiveTargetValue: the nearest hostile in sight within 40 yd). A
        // mob of a closed part is no pull for it either: such mobs near the tank are marked as careful-pull skips
        // every tick (CarefulPullSkip keeps an entry 60 s, at most 32 per tank). Blackrock Spire, LBRS waves
        // 2026-10-24: every target was LBRS trash, yet the tank walked from the Legionnaires' room into the UBRS
        // Dragonspire Hall after a Blackhand niche it saw through the open Dragonspine Door.
        constexpr float CLOSED_AREA_SKIP_RANGE = 60.0f;
        constexpr size_t CLOSED_AREA_SKIP_MAX = 16;

        struct ClosedAreaMobCheck
        {
            Player* tank;
            std::vector<AreaGateDef const*> const& gates;

            bool operator()(Creature* creature) const
            {
                return creature->IsAlive() && !creature->IsInCombat() && creature->IsHostileTo(tank) &&
                       InAnyAreaGate(gates, creature->GetPositionX(), creature->GetPositionY(),
                                     creature->GetPositionZ());
            }
        };

        void KeepTankOutOfClosedAreas(Player* tank, std::vector<AreaGateDef const*> const& gates)
        {
            if (gates.empty() || !tank->IsInWorld())
                return;
            std::list<Creature*> mobs;
            ClosedAreaMobCheck check{tank, gates};
            Acore::CreatureListSearcher<ClosedAreaMobCheck> searcher(tank, mobs, check);
            Cell::VisitObjects(tank, searcher, CLOSED_AREA_SKIP_RANGE);
            if (mobs.empty())
                return;
            std::vector<Creature*> nearest(mobs.begin(), mobs.end());
            std::sort(nearest.begin(), nearest.end(), [tank](Creature* left, Creature* right)
                      { return tank->GetExactDistSq(left) < tank->GetExactDistSq(right); });
            if (nearest.size() > CLOSED_AREA_SKIP_MAX)
                nearest.resize(CLOSED_AREA_SKIP_MAX);
            for (Creature* mob : nearest)
                CarefulPullSkip::Mark(tank, mob->GetGUID());
        }

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

        // [queue-pull-hold-not-stuck] ROUTE_NO_WALK_IN_PULL withholds the leader's waypoint while its own pull runs;
        // the steering / drive watchdogs then saw "own walk made no progress" although the harness itself gave no
        // walk: bot_cannot_move "route" every ~40 s and a steer_stuck "route end" skip of the target every 20 s
        // (brs-h 20261025-115805-brs-h-5A-32: the tank at (64.2, -441.6) from 303 s to 527 s, "pull start" 106
        // executed every tick, 83 line-of-sight pull plans on the Spellbinder / Warlock packs never cast, 10 route
        // targets skipped as "route end" from a spot it never left; fos-n 123018-fos-n-5A-84: "Soul Horror out of pull
        // range" 310 times, 3x bot_cannot_move at (5138.4, 2129.0); bfd-h 115805-bfd-h-5A-23 Rtpalad 2880 s). A
        // pull hold is no route stall for PULL_HOLD_STALL_MS; past it the hold is the pull planner's deadlock: logged
        // once (pull_hold_stall, with the pull's state), the waypoint is no longer withheld for PULL_HOLD_RELEASE_MS
        // and the steer clock runs again.
        constexpr uint32 PULL_HOLD_STALL_MS = 60 * IN_MILLISECONDS;
        constexpr uint32 PULL_HOLD_RELEASE_MS = 30 * IN_MILLISECONDS;
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

        // The bots' own raid-leader pull hold (LeaderValues PullHoldValue) holds the leader now.
        bool LeaderPullHeld(Player* leader)
        {
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(leader);
            Value<bool>* hold =
                botAI ? dynamic_cast<Value<bool>*>(botAI->GetAiObjectContext()->GetUntypedValue("pull hold")) : nullptr;
            return hold && hold->Get();
        }

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
        // Regroup after a revive (ROUTE_REGROUP, 2026-10-24, *.bak_routebackbone). RFC 18:34-18:35: the tank died,
        // came back in through the portal and stood at the entrance; MeetMember walked it back towards the group
        // 83-85 yd deeper, the four others followed it (follow) into a cave wall, wait_timeout after 60 s, a new
        // target. User rule: the group stands. The returning side (who stands at the entrance, REGROUP_ENTRANCE_RANGE)
        // runs to the group when the way is free - no idle hostile whose route reach (RouteReach) covers the walk
        // where it sees it (BlocksRoute), and no killed mob with its spawn point that close respawning before the walk
        // is over (its length at run speed plus REGROUP_RESPAWN_MARGIN_SEC: "close to the respawn the group comes
        // anyway") - and the group holds there ("stay"); else the group walks out through the exit and in through
        // the entrance (the portal goal, its own "instance portal", as a wing change: TickWingWalk watches) and is
        // at the entrance with them. The decision is the returning side's signal, taken once when the situation
        // starts and again when its walk makes no progress for REGROUP_STUCK_MS (meet_stuck).
        constexpr bool ROUTE_REGROUP = true;
        constexpr float REGROUP_GROUP_RADIUS = 30.0f;
        constexpr uint32 REGROUP_STUCK_MS = 10 * IN_MILLISECONDS;
        constexpr float REGROUP_PROGRESS = 3.0f;
        constexpr float REGROUP_RESPAWN_MARGIN_SEC = 30.0f;
        constexpr uint32 REGROUP_MAX_REPLANS = 3;  // then the group comes to the entrance
        constexpr uint32 REGROUP_COOLDOWN_MS = 3 * MINUTE * IN_MILLISECONDS;  // after giving up: the old meeting walk
        // The exit decision needs a complete walk to the entrance for every member of the group (regroup_exit_refused).
        constexpr bool REGROUP_EXIT_NEEDS_WALK = true;
        // Gruul 2026-10-24, false restores the old side test (anybody within WING_ARRIVE_RANGE of the entrance): the
        // returning side is a member that came back in through the portal (Member::backInMs) at most this long ago.
        constexpr bool REGROUP_BACK_IN_ONLY = true;
        constexpr uint32 REGROUP_BACK_IN_MS = 3 * MINUTE * IN_MILLISECONDS;
        // A killed mob respawning later than this (Map::SaveCreatureRespawnTime: a respawn past the instance reset
        // is stored as now + YEAR, raid trash "in 31535988 s") does not respawn during the run.
        constexpr float REGROUP_RESPAWN_NEVER_SEC = float(DAY);

        enum class RegroupMode : uint8
        {
            None,
            Run,   // the way is free: the group holds, the returning side runs to it
            Exit,  // the group walks out and in through the entrance
        };

        struct RegroupState
        {
            RegroupMode mode = RegroupMode::None;
            std::vector<ObjectGuid> held;  // members put on "stay"
            ObjectGuid runner;             // whose walk is watched (Run)
            ObjectGuid anchor;             // the group member it walks to
            float bestDistance = 0.0f;
            uint32 progressMs = 0;
            uint32 replans = 0;
        };
        std::unordered_map<uint32, RegroupState> regroups;  // per instance, world thread only
        std::unordered_map<uint32, uint32> regroupCooldownUntilMs;  // per instance, world thread only

        // MeetMember's own walk (the leader back to a straggler): its progress, for meet_stuck.
        struct MeetProgress
        {
            ObjectGuid member;
            float bestDistance = 0.0f;
            uint32 progressMs = 0;
        };
        std::unordered_map<uint32, MeetProgress> meetProgress;  // per instance, world thread only

        // A straggler episode of MeetMember (not a regroup). The member's own follow comes first: the follow guard's
        // deadlock breaker lets a vetoed follower go after 5 s with the tank out of combat and over 30 yd away
        // (MovementActions), so the leader stands MEET_GRACE_MS before it walks back, and again after each
        // meet_stuck; after MEET_MAX_STUCK of them it walks back no more ("give way": the breaker and the far wait's
        // timeout decide). SFK 5B-5 20:29-20:38: walk back / route on for 8 minutes, 30 meet_stuck, run stalled.
        constexpr uint32 MEET_GRACE_MS = 12 * IN_MILLISECONDS;
        constexpr uint32 MEET_MAX_STUCK = 2;
        constexpr uint32 MEET_EPISODE_GAP_MS = 90 * IN_MILLISECONDS;  // not seen this long: a new episode

        struct StragglerEpisode
        {
            ObjectGuid member;
            uint32 sinceMs = 0;
            uint32 lastSeenMs = 0;
            uint32 holdSinceMs = 0;     // the leader stands (grace) since; 0: it walks back
            float bestDistance = 0.0f;  // while it stands: the member closing in by itself restarts the grace
            uint32 stuck = 0;
            bool gaveWay = false;
        };
        std::unordered_map<uint32, StragglerEpisode> stragglers;  // per instance, world thread only

        // What lies on the walk: idle hostiles that would aggro on it, and the soonest respawn (s, -1: none) of a
        // killed hostile whose spawn point is within its route reach of the walk.
        struct RegroupPathCheck
        {
            uint32 aliveMobs = 0;
            std::string firstMob;
            float nextRespawnSec = -1.0f;
            std::string nextRespawnMob;
        };

        RegroupPathCheck CheckRegroupPath(Player* runner, Movement::PointsArray const& points)
        {
            RegroupPathCheck result;
            if (points.size() < 2)
                return result;
            RouteBackbone const path{points};
            time_t const nowSec = GameTime::GetGameTime().count();
            for (auto const& [spawnId, creature] : runner->GetMap()->GetCreatureBySpawnIdStore())
            {
                if (!creature || !creature->IsInWorld() || creature->IsCritter() || creature->IsCivilian() ||
                    creature->IsTotem() || creature->GetCreatureTemplate()->type == CREATURE_TYPE_TOTEM || creature->IsTrigger() || !creature->IsHostileTo(runner))
                    continue;
                float const reach = RouteReach(creature, runner);
                if (creature->IsAlive())
                {
                    RouteCheck check;
                    if (!creature->IsInCombat() && IdleAggroCheck{runner}(creature) &&
                        BlocksRoute(runner, creature, reach, path, check))
                    {
                        if (!result.aliveMobs++)
                            result.firstMob = creature->GetName();
                    }
                    continue;
                }
                float x = 0.0f;
                float y = 0.0f;
                float z = 0.0f;
                creature->GetRespawnPosition(x, y, z);
                G3D::Vector3 const spawn(x, y, z);
                bool spawnClose = false;
                for (size_t index = 1; index < points.size() && !spawnClose; ++index)
                    spawnClose = (ClosestOnSegment(spawn, points[index - 1], points[index]) - spawn).length() < reach;
                if (!spawnClose)
                    continue;
                float const in = float(creature->GetRespawnTimeEx() - nowSec);
                if (in > REGROUP_RESPAWN_NEVER_SEC)
                    continue;
                if (result.nextRespawnSec < 0.0f || in < result.nextRespawnSec)
                {
                    result.nextRespawnSec = in;
                    result.nextRespawnMob = creature->GetName();
                }
            }
            return result;
        }
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
        return ROUTE_BLOCKER_BODY_PULL && !guid.IsEmpty() && party.routeBlockerUntilMs &&
               int32(getMSTime() - party.routeBlockerUntilMs) < 0 &&
               std::find(party.routeBlockerPack.begin(), party.routeBlockerPack.end(), guid) !=
                   party.routeBlockerPack.end();
    }

    bool TestHarnessMgr::RouteBlockerBehindRequired(Party const& party, Map* map, WorldObject const* behind,
                                                    std::string& why) const
    {
        if (!behind)
        {
            why = "nothing behind";
            return false;
        }
        Creature const* creature = behind->ToCreature();
        // a gate, a key object, an event's object: the run cannot go on without it
        if (!creature)
        {
            why = "object";
            return true;
        }
        if (behind->GetGUID() == party.target && (party.targetIsBoss || party.targetIsStarter))
        {
            why = party.targetIsBoss ? "boss" : "event starter";
            return true;
        }
        if (creature->IsDungeonBoss() || creature->isWorldBoss() || party.bossCreatures.count(creature->GetGUID()) ||
            BossOrder(party, creature) >= 0)
        {
            why = "boss";
            return true;
        }
        if (IsPendingDoorGuard(map, creature))
        {
            why = "door guard";
            return true;
        }
        // [banished_boss_gate] the mobs by a boss immune to players (his banish keepers: Hellmaw's Cabal Ritualists)
        // open his fight: never "optional"
        {
            std::list<Creature*> nearMobs;
            PackMobCheck check{ObjectAccessor::FindPlayer(party.leader), const_cast<Creature*>(creature)};
            if (check.leader)
            {
                Acore::CreatureListSearcher<PackMobCheck> searcher(creature, nearMobs, check);
                Cell::VisitObjects(creature, searcher, PACK_RADIUS);
                for (Creature* mob : nearMobs)
                    if (mob != creature && (mob->IsDungeonBoss() || party.bossCreatures.count(mob->GetGUID())) &&
                        mob->IsImmuneToPC())
                    {
                        why = "banish keeper";
                        return true;
                    }
            }
        }
        // [boss_area_clear] trash round the next boss is cleared before him: never skipped as "optional"
        if (ROUTE_BOSS_AREA_CLEAR)
            if (Player* leader = ObjectAccessor::FindPlayer(party.leader))
                if (Creature* areaBoss = RouteNextBoss(party.seq, map))
                    if (char const* area = BossAreaTrashOf(leader, areaBoss, const_cast<Creature*>(creature),
                                                           party.bossCreatures.count(creature->GetGUID()) != 0))
                    {
                        why = area;
                        return true;
                    }
        if (int32 const spotIdx = EventSpotIndex(party); spotIdx >= 0)
            if (EventSpotDef const* spot = EventSpots(party.content->mapId)[size_t(spotIdx)]; spot && spot->hold &&
                std::find(spot->holdAlways.begin(), spot->holdAlways.end(), creature->GetEntry()) !=
                    spot->holdAlways.end())
            {
                why = "event keeper";
                return true;
            }
        // the route rule that chose it (Retarget): an objective's key, the boss's own fight, the event's way
        if (behind->GetGUID() == party.target)
        {
            std::string const& rule = party.targetRouteRule;
            if (rule == "required" || rule == "boss" || rule == "event starter" || rule == "event chain" ||
                rule == "encounter" || rule == "fled survivor" || rule == "phantom mob hunt" ||
                rule.rfind("joins boss", 0) == 0 || rule.rfind("hold guard", 0) == 0)
            {
                why = rule;
                return true;
            }
            why = rule.empty() ? std::string("trash (no route rule)") : "trash (" + rule + ")";
            return false;
        }
        why = "trash (not the target)";
        return false;
    }

    // [queue-blocker-pull-gate] WIPE_ANALYSIS_w24_25: scholo-n w24 walked the tank to 17 yd of a 20 yd aggro radius
    // into a pack the planner had REFUSED ("refused, 97 mobs") once the edge hold "gave way" after 8 s; the body pull
    // and the blocker retarget had their own, different checks. One gate now:
    // - the planner's chain count did not refuse the pack as hopeless (CarefulPlanVerdict::GetRefusal: its smallest
    //   pull is over the over-cap scale - "no safe pull: X brings N mobs, over M");
    // - the group is ready (GroupFullyReady: everyone here, health and healer mana up) - else hold, no skip;
    // - a body pull only for a blocker on the only way to a boss / event / required target (IsRouteBlocker, or
    //   RouteBlockerBehindRequired - [boss_area_clear] trash included).
    // The sight pull ([pull_option] (1), FindPullSightSpot) runs before any of these and is not gated here.
    std::string TestHarnessMgr::BlockerPullGate(Party& party, Player* leader, Creature* blocker,
                                                WorldObject const* behind, bool bodyPull, bool& hold) const
    {
        hold = false;
        if (!blocker)
            return "no blocker";
        // [cos-event-trash] the idle citizens outside Arthas' event are walked past while the Guardian of Time runs
        if (leader->GetMapId() == CoSIds::MAP_CULLING_OF_STRATHOLME &&
            CoSHelpers::IsSkippableEventTrash(leader, blocker))
            return "idle event trash (Culling heroic, Guardian of Time running)";
        bool hopeless = false;
        uint32 waitMs = 0;
        std::string refusalWhy;
        bool refused =
            PartyRefusal(party, blocker->GetGUID(), hopeless, waitMs, refusalWhy);  // [lead5]
        // [b34_strat] the tank keeps ONE verdict, about the mob it planned: strat-h 20261026-014521-strat-h-5E-1
        // (299 s) refused the target BEHIND ("Skeletal Guardian: no safe pull: Ravaged Cadaver brings 20 mobs, over
        // 18"), the lookup by the blocker's own guid found nothing and the Ravaged Cadaver became the route target
        // (302 s, "packMobs": 1 by PACK_RADIUS). The behind's over-cap refusal that names the blocker is its refusal.
        if (!refused && behind && behind != blocker && behind->ToCreature())
        {
            bool behindHopeless = false;
            uint32 behindWaitMs = 0;
            std::string behindWhy;
            if (PartyRefusal(party, behind->GetGUID(), behindHopeless, behindWaitMs,  // [lead5]
                                               behindWhy) &&
                RefusalIsOverCap(behindWhy) && behindWhy.find(blocker->GetName()) != std::string::npos)
            {
                refused = true;
                hopeless = behindHopeless;
                waitMs = behindWaitMs;
                refusalWhy = behindWhy;
            }
        }
        // [banished_boss_gate] the pack's only "boss" is banished / immune to players (its keepers' channel): the
        // planner's "brings a boss" refusal waits for a fight that only this pull can start (slabs-n 153805). Not a
        // refusal, for [drift_body_pull_planner_hold] below either: that hold stays for every real fresh refusal.
        if (refused && refusalWhy.find("brings a boss") != std::string::npos)
        {
            std::list<Creature*> pack;
            PackMobCheck check{leader, blocker};
            Acore::CreatureListSearcher<PackMobCheck> searcher(blocker, pack, check);
            Cell::VisitObjects(blocker, searcher, PACK_RADIUS);
            bool anyBoss = false;
            bool banishedOnly = true;
            for (Creature* mob : pack)
                if (mob->IsDungeonBoss() || party.bossCreatures.count(mob->GetGUID()))
                {
                    anyBoss = true;
                    banishedOnly = banishedOnly && mob->IsImmuneToPC();
                }
            if (anyBoss && banishedOnly)
                refused = false;
        }
        // [b35_bfd] the planner's boss hold ("the pack does not fit (a boss pull: no fallback plan)": its target was
        // the boss, which it never pulls, ChooseTargetActions.cpp [b30_topped_up]) is no over-cap verdict on this
        // trash: the boss's pull is the harness's (the body-pull walk stops at him, boss_on_the_way_first
        // [b30_stalls] makes him the target with his buffs and ready check). bfd-n 20261026-024524-bfd-n-5B-29: the
        // last Blindlight Muckdweller of Gelihast's room (40 yd, Gelihast 25 yd on the way) was held "over the
        // planner's cap (its split / guarantee)" by [b33_bodypull] every 10 s from 2041 s, stalled 2/7 at 2283 s
        // (build 31/33 bfd runs took this same walk into boss_on_the_way_first). Readiness stays checked below.
        if (refused && refusalWhy.find("(a boss pull: no fallback plan)") != std::string::npos && leader->GetMap())
            for (ObjectGuid const& bossGuid : party.bossCreatures)
                if (Creature* boss = leader->GetMap()->GetCreature(bossGuid);
                    boss && boss != blocker && boss->IsAlive() && !boss->IsInCombat() &&
                    (BossBroughtBy(leader, boss, blocker) || JoinsBossFight(leader, boss, blocker)))
                {
                    refused = false;
                    break;
                }
        // [b33_aoepull] a refused (or aoe-planned) pack on the only way to a required target is marked for the
        // planner's aoe_pull_plan; TargetRouteBlocker (bodyPull false) checked the required way before the gate
        bool const aoePlanned = PartyAoePlanned(party, blocker->GetGUID());  // [lead5]
        bool requiredBlocker = false;
        if ((refused || aoePlanned) && !blocker->IsDungeonBoss() && !party.bossCreatures.count(blocker->GetGUID()))
        {
            std::string requiredWhy;
            requiredBlocker = !bodyPull || IsRouteBlocker(party, blocker->GetGUID()) ||
                              RouteBlockerBehindRequired(party, leader->GetMap(), behind ? behind : blocker,
                                                         requiredWhy);
            if (requiredBlocker)
                PartyMarkRequired(party, blocker->GetGUID());  // [lead5]
        }
        if (refused && hopeless && requiredBlocker && AoePullAskRuns(party.leader, blocker->GetGUID(), getMSTime()))
        {
            hold = true;  // the planner weighs its aoe_pull_plan first; the dead end after AOE_PULL_ASK_MS
            return "held: the planner weighs an aoe_pull_plan of the required blocker (" + refusalWhy + ")";
        }
        if (refused && hopeless)
        {
            NoteHopelessPack(leader, blocker);  // [b32_brs] the walks go round it (HopelessDetourLeg)
            return "refused by the chain count (" + refusalWhy + ")";
        }
        // [drift_body_pull_planner_hold] scholo-n 20261025-150737-7 pull 12 (437 s): the planner held the Necromancer
        // pack "for now" (an overlap wait: the Risen Protector patrol crossed it, the planner waits to take the pack
        // without it), the gate let the body pull through because the refusal was not hopeless, the tank walked in
        // and ~15 mobs came (patrol, Adepts, Tutors), wipe at 517 s. A body pull never goes against a fresh planner
        // refusal: while the planner's own wait runs (its split, its stand's guarantee) the group holds; a refusal
        // still standing once that wait is over is refused here too (skipped, the route comes back to it).
        // [pull_after_hold] the group stood ready PULL_AFTER_HOLD_MS at this pack: the planner's wait is over
        // [b33_bodypull] ...but never over a readiness refusal (the healer not with the tank, sitters, not topped up)
        // nor an over-cap one (the planner's split / guarantee takes it, or calls it hopeless after its split search
        // and the detour above takes over): the walk-in of the whole group into a pack the planner says does not fit
        // is the strat-h 20261026-000434-5D-1 wipe (4-6 mobs at the walk-in, 17 in the fight)
        bool const readinessRefusal = refused && RefusalIsReadiness(refusalWhy);
        bool const overCapRefusal = refused && !readinessRefusal && RefusalIsOverCap(refusalWhy);
        if (refused && bodyPull && !readinessRefusal && !overCapRefusal &&
            PullAfterHoldPasses(party.leader, blocker->GetGUID()))
            refused = false;
        // [b34_strat] ...and the over-cap hold for the route TARGET too (TargetRouteBlocker, bodyPull false): it
        // cleared the pack's skips and walked the leader to the edge of a refused 17-26 mob chain (strat-h
        // 20261026-014521-5E-1, 302 s). The group holds where it stands while the planner splits or calls it
        // hopeless (then the chain-count branch above: the aoe ask, then the dead end and the skip).
        if (refused && !bodyPull && overCapRefusal)
        {
            hold = true;
            return "held: over the planner's cap, no route target (" + refusalWhy + ")";
        }
        if (refused && bodyPull && (readinessRefusal || overCapRefusal))
        {
            hold = true;  // nothing skipped: the group gets ready / the planner plans where it stands
            return Acore::StringFormat("{} ({})",
                                       readinessRefusal ? "held: the planner waits for the group"
                                                        : "held: over the planner's cap (its split / guarantee)",
                                       refusalWhy);
        }
        if (refused && bodyPull)
        {
            if (waitMs > 0)
            {
                hold = true;
                return Acore::StringFormat("held by the planner for {} s ({})", waitMs / IN_MILLISECONDS, refusalWhy);
            }
            return "refused by the planner (" + refusalWhy + ")";
        }
        if (!GroupFullyReady(party, leader))
        {
            hold = true;
            return "the group is not ready";
        }
        // [b33_bodypull] the planner's sitter gate for the walk-in too: nobody eats / drinks when the pack comes
        if (bodyPull)
            for (Member const& member : party.members)
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid); bot && bot->IsAlive() && bot->IsSitState())
                {
                    hold = true;
                    return "the group is not ready: " + bot->GetName() + " sits";
                }
        if (bodyPull && !IsRouteBlocker(party, blocker->GetGUID()))
        {
            std::string requiredWhy;
            if (!RouteBlockerBehindRequired(party, leader->GetMap(), behind ? behind : blocker, requiredWhy))
                return "not on the only way to a boss or event (" + requiredWhy + ")";
        }
        return std::string();
    }

    bool TestHarnessMgr::TargetRouteBlocker(Party& party, Player* leader, Creature* blocker, WorldObject const* behind,
                                            std::string const& why)
    {
        if (!ROUTE_BLOCKER_BODY_PULL || !blocker || !blocker->IsAlive() || blocker->IsInCombat() ||
            blocker->IsDungeonBoss() || party.bossCreatures.count(blocker->GetGUID()) || BossOrder(party, blocker) >= 0)
            return false;

        // [route_blocker_required_only] User rule 2026-10-25: a pack on the only way to the next REQUIRED target
        // (boss, event, gate, key) is never skipped; a side / optional trash target never triggers it - the target
        // behind is skipped as before and the route takes another one. scholo-n wave 23 (112011, 473-529 s): a
        // 3-mob Scholomance Necromancer pack (the planner: 8 mobs at cost 2.38) was body-pulled for the trash target
        // Risen Aberration ("blocks route") and the group wiped.
        if (std::string requiredWhy; !RouteBlockerBehindRequired(party, leader->GetMap(), behind, requiredWhy))
        {
            LOG_INFO("playerbots", "[route_blocker_required_only] {}: {} stands on the way to {} - {}: no body pull, "
                     "the target is skipped as before", leader->GetName(), blocker->GetName(),
                     behind ? behind->GetName() : std::string("?"), requiredWhy);
            LogEvent(party, "route_blocker_required_only",
                     JsonObject().Str("action", "not required, skipped as before").Str("blocker", blocker->GetName())
                         .Num("blockerEntry", blocker->GetEntry())
                         .Str("behind", behind ? behind->GetName() : std::string()).Str("behindRule", requiredWhy)
                         .Str("why", why));
            return false;
        }

        // [queue-blocker-pull-gate]
        bool gateHold = false;
        if (std::string const gate = BlockerPullGate(party, leader, blocker, behind, false, gateHold); !gate.empty())
        {
            LogEvent(party, "blocker_pull_gate", JsonObject().Str("pull", "route blocker target")
                                                     .Str("blocker", blocker->GetName())
                                                     .Num("blockerEntry", blocker->GetEntry())
                                                     .Str("behind", behind ? behind->GetName() : std::string())
                                                     .Str("why", gate).Bool("hold", gateHold));
            if (gateHold)
            {
                TestRunner::ClearWaypoint(party.leader);
                return true;  // the group gets ready where it stands, nothing skipped
            }
            return false;
        }

        std::list<Creature*> pack;
        PackMobCheck check{leader, blocker};
        Acore::CreatureListSearcher<PackMobCheck> searcher(blocker, pack, check);
        Cell::VisitObjects(blocker, searcher, PACK_RADIUS);
        if (std::find(pack.begin(), pack.end(), blocker) == pack.end())
            pack.push_back(blocker);
        // a boss in the pack is the boss's pull (the boss logic), not a trash blocker's
        for (Creature* mob : pack)
            if (mob->IsDungeonBoss() || party.bossCreatures.count(mob->GetGUID()) || BossOrder(party, mob) >= 0)
                return false;

        // [b32_pull] ... and so is a pack that comes into a boss's fight by itself (his formation, a script call):
        // the walk to it is the boss's pull without the boss's preparation. dmn-n 20261025-221216-dmn-n-5H-48: a
        // 2-mob Gordok Mastiff pack ("required: joins boss: script") became the target at 190 s, the leader walked to
        // it while the hunter still drank, Guard Mol'dar came at 200 s and his room chained 1 -> 9 -> 15.
        if (Map* map = leader->GetMap())
            for (ObjectGuid const& guid : party.bossCreatures)
            {
                Creature* boss = map->GetCreature(guid);
                if (!boss || !boss->IsAlive() || boss->IsInCombat())
                    continue;
                for (Creature* mob : pack)
                {
                    if (!BossEncounterMob(leader, boss, mob))
                        continue;
                    LOG_INFO("playerbots", "[route_blocker_body_pull] {}: {} joins {}'s fight: his pull, not a route "
                             "blocker's", leader->GetName(), mob->GetName(), boss->GetName());
                    LogEvent(party, "route_blocker_boss_encounter",
                             JsonObject().Str("blocker", blocker->GetName()).Num("blockerEntry", blocker->GetEntry())
                                 .Str("mob", mob->GetName()).Str("boss", boss->GetName())
                                 .Str("behind", behind ? behind->GetName() : std::string()).Str("why", why));
                    return false;
                }
            }

        uint32& tries = party.routeBlockerTries[blocker->GetGUID()];
        if (tries >= ROUTE_BLOCKER_MAX_TRIES)
            return false;
        ++tries;

        uint32 const now = getMSTime();
        party.routeBlockerPack.clear();
        for (Creature* mob : pack)
        {
            party.routeBlockerPack.push_back(mob->GetGUID());
            party.skipUntil.erase(mob->GetGUID());
            party.skipReason.erase(mob->GetGUID());
        }
        party.routeBlockerUntilMs = now + NEIGHBOUR_TARGET_MS;
        LOG_INFO("playerbots", "[route_blocker_body_pull] {}: {} ({} mobs, {:.1f} yd) stands on the way to {}: the "
                 "target, pulled by sight or walked in ({}, try {})", leader->GetName(), blocker->GetName(),
                 pack.size(), leader->GetExactDist(blocker), behind ? behind->GetName() : std::string("?"), why,
                 tries);
        LogEvent(party, "route_blocker_body_pull",
                 JsonObject().Str("action", "target").Str("blocker", blocker->GetName())
                     .Num("blockerEntry", blocker->GetEntry()).Num("packMobs", uint32(pack.size()))
                     .Str("behind", behind ? behind->GetName() : std::string()).Str("why", why).Num("try", tries)
                     .Real("blockerDistance", leader->GetExactDist(blocker)));
        sightStops.erase(party.leader);
        sightHolds.erase(party.leader);
        party.neighbourTarget = blocker->GetGUID();
        party.neighbourTargetUntilMs = now + NEIGHBOUR_TARGET_MS;
        party.target = blocker->GetGUID();
        party.targetIsBoss = false;
        party.targetIsStarter = false;
        party.targetRouteRule.clear();
        party.targetSinceMs = now;
        party.guaranteeHolding = false;
        party.arrivedMs = 0;
        party.approach = 0;
        party.approachBlocker.Clear();
        party.stopSinceMs = 0;
        return true;
    }

    void TestHarnessMgr::RouteBlockerBodyPull(Party& party, Player* leader, Creature* target, std::string const& why)
    {
        // [lead3] LEADER_NO_BODY_FALLBACK: no harness walk-in. The route blocker is the leader's to clear: asked of
        // its planner whole (the walk-in priced by the planner's own count, the aoe_pull_plan allowed: required), a
        // boss's pack as the boss pull. Its answer is the leader's (TickLeaderPlanner).
        if (Leader::Leads(Leader::LEADER_NO_BODY_FALLBACK) && LeaderDecidesFor(party) && target)
        {
            party.approach = 0;
            if (target->IsDungeonBoss() || party.bossCreatures.count(target->GetGUID()))
                LeaderBossPull(party, leader, target, "route blocker: " + why);
            else
                LeaderWholePack(party, leader, target, "route blocker: " + why, true, true);
            return;
        }
        // the group gets ready first (the user rule: full health and mana, everyone here), then the walk in; the
        // leader holds where it stands meanwhile (TestHarnessMgr's regen wait tops the group up)
        // [queue-blocker-pull-gate] and never into a pack the chain count refused, nor off the required way
        bool gateHold = false;
        if (std::string const gate = BlockerPullGate(party, leader, target, nullptr, true, gateHold); !gate.empty())
        {
            TestRunner::ClearWaypoint(party.leader);
            uint32 const nowMs = getMSTime();
            // [b33_bodypull] a hold is logged too (it was silent: the trace showed no reason for the stand)
            if (!party.blockerGateLogMs || getMSTimeDiff(party.blockerGateLogMs, nowMs) > 10 * IN_MILLISECONDS)
            {
                party.blockerGateLogMs = nowMs ? nowMs : 1;
                LogEvent(party, "blocker_pull_gate", JsonObject().Str("pull", "body pull")
                                                         .Str("blocker", target->GetName())
                                                         .Num("blockerEntry", target->GetEntry()).Str("why", gate)
                                                         .Str("was", why).Bool("hold", gateHold));
            }
            if (gateHold)
                return;
            party.approach = 0;
            SkipTarget(party, target->GetGUID(), CAREFUL_REFUSED_SKIP_MS, "body pull refused: " + gate);
            return;
        }

        // [b33_aoepull] the planner's issued aoe_pull_plan is the pull: the tank takes the chain to its stack spot,
        // the leader is not walked in while the plan is fresh and younger than AOE_PULL_WALK_MS
        if (uint32 aoeSinceMs = 0; PartyAoePlanned(party, target->GetGUID(), &aoeSinceMs) &&  // [lead5]
                                   getMSTimeDiff(aoeSinceMs, getMSTime()) < AOE_PULL_WALK_MS)
        {
            TestRunner::ClearWaypoint(party.leader);
            uint32 const nowMs = getMSTime();
            if (!party.blockerGateLogMs || getMSTimeDiff(party.blockerGateLogMs, nowMs) > 10 * IN_MILLISECONDS)
            {
                party.blockerGateLogMs = nowMs ? nowMs : 1;
                LogEvent(party, "aoe_pull_plan_accepted",
                         JsonObject().Str("blocker", target->GetName()).Num("blockerEntry", target->GetEntry())
                             .Num("planSec", getMSTimeDiff(aoeSinceMs, nowMs) / IN_MILLISECONDS).Str("was", why));
            }
            return;
        }

        std::list<Creature*> pack;
        PackMobCheck check{leader, target};
        Acore::CreatureListSearcher<PackMobCheck> searcher(target, pack, check);
        Cell::VisitObjects(target, searcher, PACK_RADIUS);
        if (std::find(pack.begin(), pack.end(), target) == pack.end())
            pack.push_back(target);
        // [body_pull_via_planner] the planner first
        if (BODY_PULL_VIA_PLANNER && !target->IsDungeonBoss() && !party.bossCreatures.count(target->GetGUID()))
        {
            bool askBegan = false;
            if (AskPlannerFirst(party.leader, target, getMSTime(), askBegan))
            {
                if (askBegan)
                    LogEvent(party, "body_pull_planner_request",
                             JsonObject().Str("name", target->GetName()).Num("entry", target->GetEntry())
                                 .Num("packMobs", uint32(pack.size())).Str("why", why)
                                 .Num("waitSec", BODY_PULL_PLANNER_MS / IN_MILLISECONDS));
                TestRunner::ClearWaypoint(party.leader);
                return;
            }
            LogEvent(party, "body_pull_planner_timeout",
                     JsonObject().Str("name", target->GetName()).Num("entry", target->GetEntry())
                         .Num("packMobs", uint32(pack.size())));
        }
        // [b49_voone] No walk-in to a pack an idle boss's own aggro reach covers (BossBroughtBy): the walk is his pull,
        // and the planner did not price it (refused, or no answer in time). brs-h 20261026-111724-brs-h-5A-10
        // (build 47): the planner refused the Smolderthorn Mystic pack at (-4,-462) ("refused for now: no pull of
        // Smolderthorn Mystic from range"), the body pull walked in at 1342-1346 s ("unplanned (harness body pull)"),
        // War Master Voone 13 yd from it came at 1353 s with his room (Berserkers, Shadow Priests, Headhunters), 3
        // deaths 1437-1444 s. The pack is skipped; the boss logic pulls him as his fight. Not when the planner's own
        // verdict was the boss's pull ("(a boss pull: no fallback plan)", [b35_bfd]): that walk is meant to stop at
        // him.
        bool const bossPullVerdict = why.find("(a boss pull: no fallback plan)") != std::string::npos;
        Map* const bossMap = bossPullVerdict ? nullptr : leader->GetMap();
        if (Map* map = bossMap)
            for (ObjectGuid const& guid : party.bossCreatures)
            {
                Creature* boss = map->GetCreature(guid);
                if (!boss || !boss->IsAlive() || boss->IsInCombat())
                    continue;
                for (Creature* mob : pack)
                {
                    if (!BossBroughtBy(leader, boss, mob))
                        continue;
                    LOG_INFO("playerbots", "[b49_voone] {}: no body pull of {} ({} mobs): {} ({:.1f} yd from {}) "
                             "comes with it, the planner did not price it ({})", leader->GetName(),
                             target->GetName(), pack.size(), boss->GetName(), boss->GetExactDist(mob),
                             mob->GetName(), why);
                    LogEvent(party, "blocker_pull_gate",
                             JsonObject().Str("pull", "body pull").Str("blocker", target->GetName())
                                 .Num("blockerEntry", target->GetEntry())
                                 .Str("why", "[b49_voone] brings boss " + boss->GetName() + " (unpriced)")
                                 .Str("was", why).Bool("hold", false));
                    // [b51_dme] User rule 2026-10-26 (open the way, no detours): a pack that is the way on and stands
                    // in an idle boss's reach is no skip - the boss is pulled deliberately first, as his own fight (the
                    // boss logic: his buffs, ready check and the planner's boss pull, which prices him with what he
                    // brings). dme-h 20261026-130307-5E-11 stalled 0/16: "body pull refused: brings boss Hydrospawn"
                    // x3, "behind skipped Wildspawn Hellcaller" x8. The Voone intent stays: no blind walk-in to the
                    // pack; a class-gated boss keeps the old skip.
                    if (!ClassGatedBoss(party.content->mapId, party.content->difficulty, boss->GetEntry()) &&
                        boss->GetGUID() != party.target)
                    {
                        LogEvent(party, "boss_on_the_way_first",
                                 JsonObject().Str("first", boss->GetName()).Num("firstEntry", boss->GetEntry())
                                     .Real("firstDistance", leader->GetExactDist(boss)).Str("via", target->GetName())
                                     .Num("viaEntry", target->GetEntry())
                                     .Str("why", "[b51_dme] the pack on the way brings this boss: he is pulled first"));
                        LOG_INFO("playerbots", "[b51_dme] {}: {} brings boss {}: the boss is the next pull (his "
                                 "fight), no skip",
                                 leader->GetName(), target->GetName(), boss->GetName());
                        party.target = boss->GetGUID();
                        party.targetIsBoss = true;
                        party.targetIsStarter = false;
                        party.targetRouteRule = "boss on the way";
                        party.targetSinceMs = getMSTime();
                        party.approach = 0;
                        party.arrivedMs = 0;
                        party.stopSinceMs = 0;
                        party.approachBlocker.Clear();
                        party.orderHoldSinceMs.erase(boss->GetGUID());
                        TestRunner::ClearWaypoint(party.leader);
                        return;
                    }
                    TestRunner::ClearWaypoint(party.leader);
                    party.approach = 0;
                    SkipTarget(party, target->GetGUID(), CAREFUL_REFUSED_SKIP_MS,
                               "body pull refused: brings boss " + boss->GetName());
                    return;
                }
            }
        sightStops.erase(party.leader);
        sightHolds.erase(party.leader);
        party.approach = 1;
        party.arrivedMs = 0;
        party.guaranteeHolding = false;
        LOG_INFO("playerbots", "[route_blocker_body_pull] {}: walks into {} ({} mobs, {:.1f} yd): {}",
                 leader->GetName(), target->GetName(), pack.size(), leader->GetExactDist(target), why);
        LogEvent(party, "route_blocker_body_pull",
                 JsonObject().Str("action", "body pull").Str("blocker", target->GetName())
                     .Num("blockerEntry", target->GetEntry()).Num("packMobs", uint32(pack.size())).Str("why", why)
                     .Real("blockerDistance", leader->GetExactDist(target)));
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
        // The tank's trail for the guarded follows of its members: the harness walks (MovePoint, its waypoint) are no
        // MoveTo of the tank's own (MovementAction::RecordTankTrail samples it every 2 yd).
        MovementAction::RecordTankTrail(leader);

        // [lead27] rfc-h 20261031-122130: the engage order was issued at 69.8 s and the route's approach stop kept
        // stepping the tank back ("stop_out_of_real_reach", its waypoint) - the tank never took the order, the run
        // stood. An issued leader order (or its committed pull) wins: no route walk of the tank meanwhile
        // [lead33] ... and for any committed pull (its gate, window and order are the leader's), the route target
        // being the commitment's mob (one target)
        if (LeaderDecidesFor(party) && (Leader::Orders::IsOpen(party.leader) || party.lead.commit))
        {
            if (party.lead.commit && !party.lead.commitMob.IsEmpty())
                party.target = party.lead.commitMob;
            TestRunner::ClearWaypoint(party.leader);
            return;
        }

        // [ledge_drop_link] plan29 follow-up 5: while the group crosses a route link (the leader over it, the members
        // following), its step down is the route - the ledge guard (JudgeLedgeDrop) lets the drops pass.
        // [link_stale_end] a link chosen and never walked (WalkToLink runs only with no target: dmw-n 20261025-
        // 163739-4 kept "walking to it, 1527 s, 0 crossed" for Immol'thar long after his kill from the ledge) ends
        // after LINK_STALE_MS.
        constexpr uint32 LINK_DROP_ALLOW_MS = 5 * IN_MILLISECONDS;
        constexpr uint32 LINK_STALE_MS = 10 * MINUTE * IN_MILLISECONDS;
        if (party.linkIdx >= 0)
        {
            if (party.linkLeaderCrossed)
                for (Member const& member : party.members)
                    AllowLedgeDrop(member.guid, LINK_DROP_ALLOW_MS);
            else if (party.linkSinceMs && getMSTimeDiff(party.linkSinceMs, getMSTime()) > LINK_STALE_MS)
                EndLink(party, "stale (never walked)");
        }

        // [pull_estimate_vs_actual] each planned pull, once over: the planner's estimate against what engaged
        {
            PullEstimate::Result result;
            uint32& seen = pullEstimateSeen[party.leader];
            if (PullEstimate::Take(party.leader, seen, result))
            {
                seen = result.id;
                Creature* pulled = leader->GetMap() ? leader->GetMap()->GetCreature(result.target) : nullptr;
                LogEvent(party, "pull_estimate_vs_actual",
                         JsonObject().Num("pull", party.pullSeq)
                             .Str("target", pulled ? pulled->GetName() : std::string())
                             .Num("targetEntry", result.target.GetEntry()).Str("kind", result.kind)
                             .Str("via", result.via).Num("estimated", result.estimated).Real("cost", result.cost)
                             .Num("engaged", result.engaged)
                             .Num("diff", int64(result.engaged) - int64(result.estimated))
                             .Num("deaths", result.deaths).Num("durationSec", result.durationSec)
                             .Bool("fought", result.fought).Str("plan", result.reason));
            }
        }

        // [pull_after_hold] (4) the walk to the door that hides the blocker: on until it is open
        if (auto walk = doorWalks.find(party.leader); walk != doorWalks.end())
        {
            Map* doorMap = leader->GetMap();
            GameObject* door = doorMap ? doorMap->GetGameObject(walk->second.door) : nullptr;
            Creature* behind = doorMap ? doorMap->GetCreature(walk->second.blocker) : nullptr;
            uint32 const nowMs = getMSTime();
            if (!door || door->GetGoState() != GO_STATE_READY || !behind || !behind->IsAlive() ||
                behind->IsInCombat() || leader->IsInCombat() || int32(walk->second.untilMs - nowMs) <= 0)
                doorWalks.erase(walk);
            else
            {
                // [lead11] a door that closes again by itself: the whole group at the opener first, no cap
                if (leader->GetExactDist(door) <= door->GetInteractionDistance() &&
                    DoorGatherHolds(party, leader, door))
                {
                    TestRunner::ClearWaypoint(party.leader);
                    return;
                }
                if (OpenDoorByUse(leader, door))
                {
                    LogEvent(party, "pull_after_hold",
                             JsonObject().Str("action", "opened the door").Str("door", door->GetName())
                                 .Num("doorEntry", door->GetEntry()).Str("blocker", behind->GetName())
                                 .Num("blockerEntry", behind->GetEntry())
                                 .Real("blockerDistance", leader->GetExactDist(behind)));
                    doorWalks.erase(walk);
                }
                else
                    SetLeaderWaypoint(party, leader, walk->second.point.x, walk->second.point.y,
                                      walk->second.point.z);
                return;
            }
        }

        // [hleft-hor-reentry-hold] Halls of Reflection: after a Falric / Marwyn wave wipe the waves resume once every
        // player INSIDE stands alive within 40 yd of the hall centre (instance_halls_of_reflection.cpp
        // MAX_DIST_FROM_CENTER_TO_START), and nobody else gets in while they run (TRANSFER_ABORT_ZONE_IN_COMBAT).
        // hor-h 2026-10-25 (20261025-065720-hor-h-5A-73): the first member back walked to the centre, the waves
        // restarted for one or two, they died, the others bounced off areatrigger 5636 for ten minutes. The leader
        // holds at the entrance until every member is inside and alive.
        if (leader->GetMapId() == HOR_MAP && leader->IsAlive() && !leader->IsInCombat())
        {
            InstanceScript const* horScript = leader->GetInstanceScript();
            bool waveWiped = false;
            for (BossTrack const& boss : party.bosses)
                waveWiped |= (boss.entry == HOR_FALRIC_ENTRY || boss.entry == HOR_MARWYN_ENTRY) && boss.wipes > 0;
            if (horScript && waveWiped && horScript->GetBossState(HOR_MARWYN_SLOT) != DONE)
            {
                std::string away;
                for (Member const& member : party.members)
                {
                    Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    if (!bot || !bot->IsAlive() || bot->GetMap() != leader->GetMap())
                        away += (away.empty() ? "" : ", ") + member.name;
                }
                if (!away.empty())
                {
                    float const fromCentre = leader->GetExactDist2d(HOR_CENTRE_X, HOR_CENTRE_Y);
                    uint32 const nowMs = getMSTime();
                    if (!party.horHoldLogMs || getMSTimeDiff(party.horHoldLogMs, nowMs) > HOR_HOLD_LOG_MS)
                    {
                        party.horHoldLogMs = nowMs ? nowMs : 1;
                        LogEvent(party, "hor_entrance_hold", JsonObject().Str("waitingFor", away)
                                                                 .Real("fromCentre", fromCentre));
                    }
                    party.target.Clear();
                    party.lastProgressMs = nowMs;
                    if (fromCentre < HOR_HOLD_FROM_CENTRE)
                        SetLeaderWaypoint(party, leader, HOR_ENTRANCE_X, HOR_ENTRANCE_Y, HOR_ENTRANCE_Z);
                    else
                        TestRunner::ClearWaypoint(party.leader);
                    return;
                }
            }
        }

        // [hleft-swim-back] Blackfathom Deeps heroic 2026-10-25 (20261025-062726-bfd-h-5A-19): after Old Serra'kis the
        // group surfaced in the moat round Twilight Lord Kelris's platform (-787,-196,-31.4); the moat is a navmesh
        // part of its own (tools/navmesh_probe.py 48 -787.3 -195.6 -31.4 -797.2 -223.8 -25.6: NOT CONNECTED, the
        // ledge 6.7 yd above the water), the route "ended beside a gap" under the platform and the leader stood
        // "own path makes no progress, no straight walk over ground" for 8 minutes. A swimmer leaves the water the
        // way it came in: the trail of its swim back to the last dry ground, straight swims point to point.
        if (ROUTE_SWIM_BACK && leader->IsAlive())
        {
            uint32 const nowMs = getMSTime();
            bool const inWater = leader->IsInWater();
            if (!inWater && !leader->IsFalling() && !leader->IsFlying())
            {
                party.swimDry = leader->GetPosition();
                party.swimDryKnown = true;
                party.swimTrail.clear();
                if (party.swimBack)
                    LogEvent(party, "swim_back", JsonObject().Str("action", "on dry ground")
                                                     .Real("x", leader->GetPositionX())
                                                     .Real("y", leader->GetPositionY())
                                                     .Real("z", leader->GetPositionZ()));
                party.swimBack = false;
            }
            else if (inWater)
            {
                if (party.swimTrail.empty() && party.swimDryKnown)
                    party.swimTrail.push_back(party.swimDry);
                if (party.swimTrail.empty() || leader->GetExactDist(&party.swimTrail.back()) >= SWIM_TRAIL_STEP)
                {
                    if (party.swimTrail.size() >= SWIM_TRAIL_MAX)
                        party.swimTrail.erase(party.swimTrail.begin() + 1);  // the dry spot stays first
                    party.swimTrail.push_back(leader->GetPosition());
                }
            }
            if (party.swimBack && getMSTimeDiff(party.swimBackSinceMs, nowMs) > SWIM_BACK_MAX_MS)
            {
                LogEvent(party, "swim_back",
                         JsonObject().Str("action", "given up").Num("trail", uint32(party.swimTrail.size())));
                party.swimBack = false;
            }
            if (party.swimBack && inWater && !leader->IsInCombat())
            {
                // The point nearest the dry spot the swimmer sees from here (a shortcut over the trail).
                for (Position const& point : party.swimTrail)
                {
                    if (leader->GetExactDist(&point) <= WAYPOINT_REACHED_SWIM)
                        continue;
                    if (!leader->IsWithinLOS(point.GetPositionX(), point.GetPositionY(), point.GetPositionZ() + 1.0f))
                        continue;
                    SetLeaderWaypoint(party, leader, point.GetPositionX(), point.GetPositionY(),
                                      point.GetPositionZ(), true);
                    party.steerSinceMs = nowMs;
                    party.lastProgressMs = nowMs;
                    return;
                }
            }
        }

        // A regroup after a revive goes on past the far-member wait (its 60 s wait_timeout): the group stays on
        // "stay" until the returning side is with it, the route waits; MeetMember ends it when everybody is together.
        if (regroups.count(party.instanceId))
        {
            std::vector<Player*> players;
            for (Member const& member : party.members)
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid))
                    if (bot->IsInWorld())
                        players.push_back(bot);
            if (MeetMember(party, players, leader))
                return;
        }

        // A wing closed to this run (AreaGateDef::onlyContent): the tank's own pull choice stays out of it
        // (KeepTankOutOfClosedAreas) and the walk never leads through it (below).
        std::vector<AreaGateDef const*> steerClosedAreas;
        for (AreaGateDef const* gate : AreaGates(party.content->mapId))
            if (AreaGateForContent(*gate, *party.content) &&
                (gate->afterDead.empty() || !EntriesDead(party, gate->afterDead)))
                steerClosedAreas.push_back(gate);
        steerClosedAreas = KeepOutAreas(steerClosedAreas);
        KeepTankOutOfClosedAreas(leader, steerClosedAreas);

        // Inside such a wing out of combat (a fight ran in, a chase): out through its exit before the route goes on.
        // The routes from in there pass its packs (LBRS run 13:04: 6 minutes in the UBRS hall, as deep as x 225).
        for (AreaGateDef const* gate : steerClosedAreas)
        {
            if (gate->exitZ <= -100000.0f || !leader->IsAlive() || leader->IsInCombat() ||
                !InAreaGate(*gate, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()))
                continue;
            G3D::Vector3 const goal(gate->exitX, gate->exitY, gate->exitZ);
            PathResult leg;
            SteerLeg(leader, goal, leg);
            if (leg.type & PATHFIND_NOPATH)
                break;
            G3D::Vector3 const waypoint =
                FloorWaypoint(leader, leg, RouteWaypoint(leg, goal, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP));
            if (party.lastEvent.rfind("closed area exit", 0) != 0)
            {
                party.lastEvent = std::string("closed area exit: ") + gate->note;
                LogEvent(party, "closed_area_exit", JsonObject().Str("note", gate->note)
                                                        .Real("x", leader->GetPositionX())
                                                        .Real("y", leader->GetPositionY())
                                                        .Real("z", leader->GetPositionZ()));
            }
            SetLeaderWaypoint(party, leader, waypoint.x, waypoint.y, waypoint.z);
            if (leader->isMoving())
                party.lastProgressMs = getMSTime();
            // [b31_ubrs_door] The way out holds at a door that stays shut for good now (ShutForGoodDoorSlot: UBRS'
            // wave portcullis 175185 after Rend): the group is locked in. ubrs-n 20261025-184229-10B-43 stood 45 min
            // in the drake pen until "no boss progress"; the verdict says what happened, LOCKED_IN_FAIL_MS after.
            uint32 const lockNow = getMSTime();
            GameObject* lockDoor = party.doorHoldMs && getMSTimeDiff(party.doorHoldMs, lockNow) < IN_MILLISECONDS
                                       ? leader->GetMap()->GetGameObject(party.doorHold)
                                       : nullptr;
            InstanceScript const* lockScript = leader->GetInstanceScript();
            if (lockDoor && lockScript && ShutForGoodDoorSlot(lockScript, lockDoor) >= 0)
            {
                uint32& since = lockedInSince[party.seq];
                if (!since)
                {
                    since = lockNow ? lockNow : 1;
                    LOG_INFO("playerbots", "[b31_ubrs_door] {}: the leader is locked in ({}): the way out runs through "
                                           "{} {}, shut for good",
                             party.id, gate->note, lockDoor->GetName(), lockDoor->GetEntry());
                    LogEvent(party, "locked_in", JsonObject().Str("note", gate->note).Str("door", lockDoor->GetName())
                                                     .Num("doorEntry", lockDoor->GetEntry())
                                                     .Real("x", leader->GetPositionX())
                                                     .Real("y", leader->GetPositionY())
                                                     .Real("z", leader->GetPositionZ()));
                }
                else if (getMSTimeDiff(since, lockNow) >= LOCKED_IN_FAIL_MS)
                {
                    lockedInSince.erase(party.seq);
                    Finish(party, "failed",
                           std::string("group locked in: ") + gate->note + " - the way out runs through " +
                               lockDoor->GetName() + " " + std::to_string(lockDoor->GetEntry()) +
                               ", which nothing opens any more");
                }
            }
            else
                lockedInSince.erase(party.seq);
            return;
        }

        // [b46_patrol] PATROL_STEP_BACK: out of a patrol's way first; the rest hold then holds at the step-back spot.
        // [lead4] LEADER_ONE_PATROL_OWNER: one patrol decision (LeaderPatrolTick: wait or pull the patrol), the step
        // back only as its wait's spot; the old independent step back and patrol hold below are off
        if (LeaderOwnsPatrols(party) ? LeaderPatrolTick(party, leader) : PatrolStepBack(party, leader))
            return;

        // ROUTE_REST_HOLD: no walk while a member sits eating / drinking or is about to; the leader stands.
        {
            std::string const resting = RestHoldMembers(party, leader);
            RestHold& hold = restHolds[party.instanceId];
            if (!resting.empty())
            {
                if (!hold.logged)
                {
                    hold.logged = true;
                    party.lastEvent = "rest hold: " + resting;
                    std::string const target = party.target.IsEmpty() ? std::string() : party.target.ToString();
                    LogEvent(party, "rest_hold", JsonObject().Str("state", "waiting").Str("members", resting)
                                                     .Str("target", target)
                                                     .Real("x", leader->GetPositionX())
                                                     .Real("y", leader->GetPositionY())
                                                     .Real("z", leader->GetPositionZ()));
                }
                // [b48_scholo] the walk under way stops too (HaltLeaderWalk)
                if (G3D::Vector3 haltedTo; HaltLeaderWalk(leader, haltedTo))
                    LogEvent(party, "harness_walk_halt", JsonObject().Str("hold", "rest").Str("why", resting)
                                                             .Real("x", leader->GetPositionX())
                                                             .Real("y", leader->GetPositionY())
                                                             .Real("z", leader->GetPositionZ())
                                                             .Real("toX", haltedTo.x).Real("toY", haltedTo.y)
                                                             .Real("toZ", haltedTo.z));
                TestRunner::ClearWaypoint(party.leader);
                return;
            }
            if (hold.logged)
            {
                hold.logged = false;
                // Retarget skipped its 4 s turns during the hold (RestHoldMembers): the next target at once.
                party.nextRetargetMs = getMSTime();
                LogEvent(party, "rest_hold", JsonObject().Str("state", hold.timedOut ? "timeout" : "done")
                                                 .Str("members", hold.members));
            }
            hold.timedOut = false;
        }

        // [b43_harn] ROUTE_PATROL_HOLD (PatrolHoldWhy): no walk while a patrol comes to the group or the tank's planner
        // pulls one; the leader stands, the planner meets it where the group is. A running patrol wait spot walk
        // ([patrol_wait_spot]) is the planner's own patrol answer and keeps its waypoint.
        if (!LeaderOwnsPatrols(party) &&  // [lead4] the leader's patrol decision ran above (LeaderPatrolTick)
            !(party.patrolWaitUntilMs && party.patrolWaitTarget == party.target &&
              int32(party.patrolWaitUntilMs - getMSTime()) > 0))
        {
            std::string const patrolWhy = PatrolHoldWhy(party, leader);
            PatrolHold& hold = patrolHolds[party.instanceId];
            if (!patrolWhy.empty())
            {
                uint32 const holdNow = getMSTime();
                if (!hold.logged)
                {
                    hold.logged = true;
                    party.lastEvent = "patrol hold: " + patrolWhy;
                    LOG_INFO("playerbots", "[b43_harn] patrol_hold {}: {}", leader->GetName(), patrolWhy);
                    LogEvent(party, "patrol_hold", JsonObject().Str("state", "waiting").Str("why", patrolWhy)
                                                       .Real("x", leader->GetPositionX())
                                                       .Real("y", leader->GetPositionY())
                                                       .Real("z", leader->GetPositionZ()));
                }
                // The tank keeps planning the route target meanwhile; the hold is no route stall.
                if (!party.target.IsEmpty() && !party.targetIsStarter && !LeaderPicksFor(party))  // [lead5]
                    RouteTarget::Set(party.leader, party.target);
                party.steerSinceMs = holdNow;
                party.steerAnchor = leader->GetPosition();
                party.steerNoNearerMs = 0;
                party.driveSinceMs = holdNow;
                party.driveAnchor = leader->GetPosition();
                // [b48_scholo] the walk under way stops too (scholo-h 111724-5C-4: 12 yd on into the patrol's walk)
                if (G3D::Vector3 haltedTo; HaltLeaderWalk(leader, haltedTo))
                {
                    LOG_INFO("playerbots", "[b48_scholo] halt_walk {}: patrol hold stops the walk to ({:.1f},{:.1f},"
                             "{:.1f}) at ({:.1f},{:.1f},{:.1f}): {}", leader->GetName(), haltedTo.x, haltedTo.y,
                             haltedTo.z, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(),
                             patrolWhy);
                    LogEvent(party, "harness_walk_halt", JsonObject().Str("hold", "patrol").Str("why", patrolWhy)
                                                             .Real("x", leader->GetPositionX())
                                                             .Real("y", leader->GetPositionY())
                                                             .Real("z", leader->GetPositionZ())
                                                             .Real("toX", haltedTo.x).Real("toY", haltedTo.y)
                                                             .Real("toZ", haltedTo.z));
                }
                TestRunner::ClearWaypoint(party.leader);
                return;
            }
            if (hold.logged)
            {
                hold.logged = false;
                LogEvent(party, "patrol_hold", JsonObject().Str("state", hold.timedOut ? "timeout" : "done")
                                                   .Str("why", hold.why));
            }
            hold.timedOut = false;
        }

        // [b33_sh2] sh-n 20261026-003732-sh-n-5B-89, 1370-1512 s: the target Shattered Hand Champion set, the phantom
        // combat already declared and ended (the harness out of combat, phase traveling), yet the tank Rlwaraj stayed
        // flagged in combat with 0 attackers; this gate cleared its waypoint and the route target every tick without a
        // word ("pull action: no target", "test runner move -> 0.0 yd" for 140 s, then the stall skipped the target).
        // Steer runs only while the harness counts the party out of combat, so a leader flagged here with nobody on
        // it is that phantom flag: the walk goes on (the route target too, the tank's pull gets its target). A leader
        // somebody attacks still holds. Either way a held walk is logged (harness_hold).
        bool const leaderFlagOnly = !party.target.IsEmpty() && leader->IsInCombat() && !party.FightOn(leader);
        if (!party.target.IsEmpty() && leader->IsInCombat())
        {
            uint32 const holdNow = getMSTime();
            std::string const why = leaderFlagOnly
                                        ? "leader flagged in combat, nobody on it (phantom): the walk goes on"
                                        : "leader in combat with attackers: no walk, no pull";
            if (why != party.harnessHoldWhy || !party.harnessHoldLogMs ||
                getMSTimeDiff(party.harnessHoldLogMs, holdNow) > 15 * IN_MILLISECONDS)
            {
                party.harnessHoldWhy = why;
                party.harnessHoldLogMs = holdNow ? holdNow : 1;
                std::string refs;
                for (auto const& [guid, ref] : leader->GetCombatManager().GetPvECombatRefs())
                    if (Unit* other = ref ? ref->GetOther(leader) : nullptr)
                        refs += (refs.empty() ? "" : ", ") + std::string(other->GetName()) +
                                Acore::StringFormat(" {:.1f} yd", leader->GetExactDist(other));
                Unit const* holdTarget = leader->GetMap() ? leader->GetMap()->GetCreature(party.target) : nullptr;
                LOG_INFO("playerbots", "[b33_sh2] harness_hold {}: {} (attackers {}, refs: {})", leader->GetName(), why,
                         leader->getAttackers().size(), refs.empty() ? "none" : refs);
                LogEvent(party, "harness_hold",
                         JsonObject().Str("why", why).Str("target", holdTarget ? holdTarget->GetName() : "")
                             .Real("distance", holdTarget ? leader->GetExactDist(holdTarget) : -1.0f)
                             .Num("attackers", int64(leader->getAttackers().size())).Str("combatRefs", refs)
                             .Bool("walks", leaderFlagOnly));
            }
        }

        if (party.target.IsEmpty() || (leader->IsInCombat() && !leaderFlagOnly))
        {
            RouteTarget::Clear(party.leader);  // no route target to plan (or a fight on)
            // [ulda-ritual-hold] no event-spot / link / escort walk while the ritual channel runs
            if (auto const hold = ritualHoldUntilMs.find(party.instanceId);
                hold != ritualHoldUntilMs.end() && int32(hold->second - getMSTime()) > 0 && !leader->IsInCombat())
            {
                TestRunner::ClearWaypoint(party.leader);
                return;
            }
            bool const idle = party.noTargets && !leader->IsInCombat() && !party.inCombat;
            // A scripted intro keeps its boss immune (ROUTE_INTRO_WAIT, RunningIntroBoss: Svala's Arthas talk, 72 s
            // from areatrigger 5140): the group waits where it stands - within her 100 yd target pick - instead of
            // pulling elsewhere, buffing and drinking meanwhile; neither the "no targets" end nor a wing change
            // comes from that wait. At most INTRO_WAIT_MAX_MS.
            if (idle && ROUTE_INTRO_WAIT)
            {
                Creature* intro = RunningIntroBoss(leader);
                uint32 const now = getMSTime();
                auto const since = introWaitSinceMs.find(party.instanceId);
                if (!intro)
                    introWaitSinceMs.erase(party.instanceId);
                else if (since == introWaitSinceMs.end() || getMSTimeDiff(since->second, now) < INTRO_WAIT_MAX_MS)
                {
                    // A member started it (the UP strategy's "svala start event" walks in from 70 yd) while the
                    // leader was still on the way: the walk to the spot goes on first.
                    if (leader->GetExactDist(intro) > INTRO_WAIT_RANGE && WalkToEventSpot(party, leader))
                        return;
                    if (since == introWaitSinceMs.end())
                    {
                        introWaitSinceMs[party.instanceId] = now;
                        party.lastEvent = std::string("intro wait: ") + intro->GetName();
                        LogEvent(party, "intro_wait", JsonObject().Str("state", "waiting").Str("boss", intro->GetName())
                                                          .Num("entry", intro->GetEntry())
                                                          .Real("distance", leader->GetExactDist(intro))
                                                          .Real("x", leader->GetPositionX())
                                                          .Real("y", leader->GetPositionY())
                                                          .Real("z", leader->GetPositionZ()));
                    }
                    TestRunner::ClearWaypoint(party.leader);
                    party.noTargetsSinceMs = now;
                    return;
                }
            }
            if (idle && (StayWithEscort(party, leader) || WalkToEventGather(party, leader) ||
                         WalkToEventSpot(party, leader) || WalkToLink(party, leader)))
                return;
            // A starter step-out the leader walks on its own (UseStarterItem) keeps its waypoint.
            if (!party.waypointHoldUntilMs || int32(getMSTime() - party.waypointHoldUntilMs) >= 0)
                TestRunner::ClearWaypoint(party.leader);
            // An escort still walking (or waiting for its next wave) is no reason to end the run or change wings,
            // nor the Oculus drake flow between the rings (the OC strategy mounts and flies the group).
            if (idle && (!party.escort.IsEmpty() || OculusDrakePhase(leader) || ScriptEventRunning(party)))
            {
                party.noTargetsSinceMs = getMSTime();
                return;
            }
            // [sm-wing-done] an empty wing waits WING_DONE_NO_TARGET_MS, not 30 s (one-run contents only)
            int32 doneWingNext = -1;
            bool const wingDone = ROUTE_WING_DONE_CHANGE && party.noTargets && party.content->bossEntries.empty() &&
                                  party.entrances.size() > 1 && party.entranceIdx < party.entrances.size() &&
                                  !WingBossesLeft(party, leader->GetMap(), leader, doneWingNext);
            if (party.noTargets && !leader->IsInCombat() && !party.inCombat &&
                getMSTimeDiff(party.noTargetsSinceMs, getMSTime()) >
                    (wingDone ? WING_DONE_NO_TARGET_MS : 30 * IN_MILLISECONDS))
            {
                // Entrances on the same spot (Halls of Reflection: from Icecrown and from Pit of Saron) are one.
                size_t next = party.entranceIdx + 1;
                while (next < party.entrances.size() && party.entranceIdx < party.entrances.size() &&
                       party.entrances[next].GetExactDist(&party.entrances[party.entranceIdx]) < SAME_ENTRANCE_RANGE)
                    ++next;

                // Another wing of the same instance (Scarlet Monastery, Dire Maul, Maraudon): the group walks
                // out and in through the next portal. Bots walk by themselves: every member gets the portal goal
                // (the leader's "we go in over there") and its own "instance portal" walks out through the exit and
                // in through that entrance (TickWingWalk watches). HARNESS_MOVES_BOTS: the old teleport to it. Not
                // while an event the group started waits for its next wave.
                // A next boss behind a closed door that does not open for him: another wing does not open it
                // (Gnomeregan heroic 2026-10-25: Thermaplugg behind The Final Chamber 142207; the walk to the Train
                // Depot entrance, the same caves, paced between two exits 4 minutes without a fight).
                bool doorBlocked = false;
                for (auto const& [guid, reason] : party.skipReason)
                    doorBlocked |=
                        party.skipUntil.count(guid) && reason.rfind("next boss behind a closed door", 0) == 0;
                // Only when the next entrance's arrival is walkable from here anyway (the same caves): a wing reached
                // only from outside (Stratholme's service entrance) is the way round the door.
                bool const sameCaves =
                    doorBlocked && next < party.entrances.size() &&
                    RoutePath(leader, party.entrances[next].GetPositionX(), party.entrances[next].GetPositionY(),
                              party.entrances[next].GetPositionZ())
                        .complete;
                // [queue-door-skip-lift] not right after a starter was used (a key on a gate: strat-n
                // 20261025-115805-strat-n-5A-6, Gauntlet Gate keyed at 962 s, wing change at 999 s): what it opened
                // is the way on, the route is built again first.
                constexpr uint32 WING_CHANGE_AFTER_STARTER_MS = 60 * IN_MILLISECONDS;
                bool const starterJustUsed = party.lastStarterUseMs &&
                                             getMSTimeDiff(party.lastStarterUseMs, getMSTime()) <
                                                 WING_CHANGE_AFTER_STARTER_MS;
                if (starterJustUsed && !sameCaves && next < party.entrances.size())
                {
                    // waits: the "no targets" clock goes on, the change comes once the window is over
                }
                else if (!sameCaves && next < party.entrances.size() && !EventGatherApplies(party) &&
                         EventSpotIndex(party) < 0)
                {
                    StartWingChange(party, next, "no targets");
                }
                else
                {
                    // Event dungeons (ToC5, Violet Hold, Halls of Reflection, Culling) spawn their next wave or
                    // boss after a pause: with bosses still open the group waits before the run is called partial.
                    bool bossesOpen = false;
                    for (BossTrack const& boss : party.bosses)
                        bossesOpen |= !boss.killed && !boss.failed;
                    // A boss behind a closed door changes only the reason, never the timing: the order hold
                    // (ORDER_HOLD_MS) keeps the later bosses out of the route while the next one is unreachable, so
                    // "no targets" for 30 s says nothing about them (wave 7 build 7: strat-n / strat-h / brs-h ended
                    // "partial: next boss behind a closed door" with most bosses alive and reachable).
                    // [hleft-mech-bridge] The Mechanar: Pathaleon comes only after the bridge pack (MechanarBridge,
                    // Mech strategy): never "no reachable targets" while he is dormant.
                    bool const pathaleonDormant =
                        leader->GetMapId() == MECHANAR_MAP_ID && MechanarBridge::PathaleonDormant(leader);
                    // [hor-hollow-waves] Halls of Reflection after a wave wipe: spirits removed from the map are not
                    // respawned by HandleWaveWipe, the waves come with 0-2 spirits and end only on their 150 s timer
                    // (hor-h 20261025-153805: waves 6-9 from 876 s, Marwyn due ~1476 s, run ended 1456 s).
                    // (PATCH_hor_gd.md 1.4 (a), applied by plan29)
                    constexpr uint32 HOR_WAVE_WAIT_MS = 11 * MINUTE * IN_MILLISECONDS;  // 4 x 150 s + call + margin
                    InstanceScript* horInstance =
                        leader->GetMapId() == HoRIds::HOR_MAP_ID ? HoRHelpers::GetInstance(leader) : nullptr;
                    bool const horWaves = horInstance && HoRHelpers::IsWaveEventActive(leader) &&
                                          horInstance->GetData(HoRIds::DATA_WAVE_NUMBER) != 0;
                    uint32 const spawnWaitMs = horWaves ? HOR_WAVE_WAIT_MS : EVENT_WAIT_FOR_SPAWNS_MS;
                    if (!pathaleonDormant && (!bossesOpen ||
                        getMSTimeDiff(party.noTargetsSinceMs, getMSTime()) > spawnWaitMs))
                        Finish(party, "partial",
                               doorBlocked ? "no reachable targets left: next boss behind a closed door"
                                           : "no reachable targets left");
                }
            }
            return;
        }

        Map* map = leader->GetMap();
        WorldObject* object = nullptr;
        if (party.targetIsStarter)
        {
            object = map->GetCreature(party.target);
            if (!object)
                object = map->GetGameObject(party.target);
        }
        else
        {
            Creature* creature = map->GetCreature(party.target);
            if (creature && creature->IsAlive())
                object = creature;
        }

        if (!object)
        {
            RouteTarget::Clear(party.leader);
            party.target.Clear();
            party.nextRetargetMs = getMSTime();
            TestRunner::ClearWaypoint(party.leader);
            return;
        }

        // The tank plans the harness's route target out to 70 yd (AggressiveTargetValue RouteTarget, TTL 10 s,
        // refreshed every steer tick), not only its own 40 yd pick; a starter is no pull.
        // [lead5] LEADER_ONE_PICKER: no registry - the leader picks with its own route target (TickLeaderPlanner)
        if (!party.targetIsStarter && object->ToCreature() && !LeaderPicksFor(party))
            RouteTarget::Set(party.leader, party.target);
        else
            RouteTarget::Clear(party.leader);

        uint32 const now = getMSTime();
        float const distance = leader->GetExactDist(object);
        float range = party.targetIsStarter ? STARTER_RANGE : party.approach ? BODY_PULL_RANGE : PULL_RANGE;
        // [b32_bm] a walk-in arrives beside a big model too: BODY_PULL_RANGE (exact distance) is inside the two
        // combat reaches of Aeonus (3) and a bear (1.5) - the tank stood at 6.0 yd, never "arrived", steer_stuck.
        if (!party.targetIsStarter && party.approach)
            if (Unit const* walkedTo = object->ToUnit())
                range = std::max(range, leader->GetMeleeRange(walkedTo) + BODY_PULL_MELEE_SLACK);

        // A game object starter is used from within reach, as a client clicks it: no line of sight to its centre (UBRS
        // wave 8: the leader stood on the Blackrock Altar 175706 for 8 minutes, the ray to the object's own origin
        // runs into its model, the altar was never used and the run stalled "traveling").
        bool const starterObject = party.targetIsStarter && object->ToGameObject();
        // A creature starter counts as the core's gossip check does (object sizes included): CoS-n wave 11 stood 4.9
        // yd from Chromie, in her talk range, never "arrived" at 4.0 and never used her.
        bool const inRange = distance <= range || (party.targetIsStarter && object->ToCreature() &&
                                                   leader->IsWithinDistInMap(object, STARTER_RANGE));
        bool arrived = inRange && (starterObject || leader->IsWithinLOSInMap(object));
        if (!party.targetIsStarter && !party.approach)
        {
            // Same floor as the target (a stairwell under its room is "in range" but sees nothing): keep walking the
            // path until the height matches; a body pull stands next to it anyway.
            if (arrived && std::fabs(leader->GetPositionZ() - object->GetPositionZ()) > SAME_FLOOR_DZ)
                arrived = false;
            // Never into the aggro radius of the nearest mob of the pack: the pull logic decides from its edge.
            if (!arrived)
                if (Creature* mob = object->ToCreature())
                    arrived = AtPackAggroEdge(leader, mob);
        }

        // [patrol_wait_spot] standing at the patrol wait spot: the tank's own planner pulls once the patrol is alone or
        // past; the walk back to the target waits until then (PATROL_WAIT_HOLD_MS at most).
        if (party.patrolWaitUntilMs && party.patrolWaitTarget == party.target &&
            int32(party.patrolWaitUntilMs - now) > 0 && !leader->IsInCombat())
            return;
        // The route of this tick, and where it enters the aggro reach of an idle mob around the target (ApproachStop):
        // the waypoint stops there, and standing there is the arrival.
        G3D::Vector3 goal(object->GetPositionX(), object->GetPositionY(), object->GetPositionZ());
        // [st-guard-approach] A guard of a closed guarded door with an approach point (Sunken Temple's defenders on the
        // z -67 balconies): the walk goes to the point on the guard's floor first - from the pit / the z -91 ring every
        // stop was "no sight point" or "tank does not see it" (st-h 062726: 16 skips, Jammal'an never reached).
        if (!party.targetIsStarter && !party.approach)
            if (Creature* guard = object->ToCreature())
                if (GuardApproachDef const* point = PendingGuardApproach(map, guard))
                {
                    float const toPoint = leader->GetExactDist(point->x, point->y, point->z);
                    bool const onGuardFloor =
                        std::fabs(leader->GetPositionZ() - guard->GetPositionZ()) <= SAME_FLOOR_DZ &&
                        distance <= guard->GetExactDist(point->x, point->y, point->z) + GUARD_APPROACH_SLACK;
                    std::pair<uint32, ObjectGuid> const approachKey{party.seq, guard->GetGUID()};
                    if (toPoint > GUARD_APPROACH_ARRIVE && !onGuardFloor)
                    {
                        goal = G3D::Vector3(point->x, point->y, point->z);
                        arrived = false;
                        if (guardApproachLogged.insert(approachKey).second)
                        {
                            guardApproachSinceMs[approachKey] = now ? now : 1;
                            LogEvent(party, "guard_approach",
                                     JsonObject().Str("name", guard->GetName()).Num("entry", guard->GetEntry())
                                         .Str("state", "walking").Real("toPoint", toPoint).Real("distance", distance)
                                         .Real("x", point->x).Real("y", point->y).Real("z", point->z));
                        }
                    }
                    // [guard_approach_arrived] The walk's end, once: st-n build 23 logged only "walking" (toPoint
                    // 80-140 yd) and the log could not tell a reached point from an aborted walk.
                    else if (auto const since = guardApproachSinceMs.find(approachKey);
                             since != guardApproachSinceMs.end())
                    {
                        LogEvent(party, "guard_approach",
                                 JsonObject().Str("name", guard->GetName()).Num("entry", guard->GetEntry())
                                     .Str("state", "arrived").Str("by", onGuardFloor ? "guard floor" : "point")
                                     .Real("toPoint", toPoint).Real("distance", distance)
                                     .Num("sec", getMSTimeDiff(since->second, now) / IN_MILLISECONDS)
                                     .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                                     .Real("z", leader->GetPositionZ()));
                        guardApproachSinceMs.erase(since);
                    }
                }
        // [selin_room_owns_steer] Magisters' Terrace: while Selin's room tactic holds the group (her Wretched idle, no
        // room fight) the harness walks the leader to none of her room's mobs: the room tactic picks the pack, the
        // cast spot out of the other packs' reach and the moment. Build 27 (20261025-150737-mgt-h-5A-14, 264-267 s):
        // the harness's approach to sight walked the tank to (212.0,-6.6), 20.3 yd from a south-pack Bruiser (real
        // aggro 21); the pack came by the pair (the core's call for assistance) and Selin 2 s later, the door shut on
        // the hunter and the shaman.
        if (leader->GetMapId() == MagistersTerrace::MAP_MAGISTERS_TERRACE)
            if (Creature const* roomMob = object->ToCreature())
            {
                uint32 const entry = roomMob->GetEntry();
                bool const selinRoomMob =
                    entry == 24688 || entry == 24689 || entry == 24690 || entry == 24723;  // Wretched x3, Selin
                PlayerbotAI* leaderAI = selinRoomMob ? GET_PLAYERBOT_AI(leader) : nullptr;
                // At most SELIN_ROOM_OWNS_MAX_MS in a row (a room tactic that never pulls is the harness's again).
                // The room tactic's spot search runs in 800-ray slices and waits for the group's regen (build 29:
                // 137 s from the first look to the pull): 4 minutes.
                constexpr uint32 SELIN_ROOM_OWNS_MAX_MS = 240 * IN_MILLISECONDS;
                struct RoomOwns
                {
                    uint32 sinceMs = 0;
                    uint32 seenMs = 0;
                };
                static std::unordered_map<uint32, RoomOwns> roomOwns;  // party seq (world thread)
                // ... and the entry walk of a clear room (EntryHolds): the members' "mgt out of room" walks them to
                // (221, +-9); the harness's approach stop (216.1,-4.0) held the leader outside (build 29, stall).
                bool owns = leaderAI && ((MagistersTerrace::SelinRoom::Get(leaderAI).active &&
                                          !MagistersTerrace::SelinRoom::InRoomFight(leaderAI)) ||
                                         (entry == 24723 && MagistersTerrace::SelinRoom::EntryHolds(leader)));
                if (owns)
                {
                    RoomOwns& held = roomOwns[party.seq];
                    if (!held.sinceMs || getMSTimeDiff(held.seenMs, now) > 15 * IN_MILLISECONDS)
                        held.sinceMs = now ? now : 1;
                    held.seenMs = now ? now : 1;
                    owns = getMSTimeDiff(held.sinceMs, now) < SELIN_ROOM_OWNS_MAX_MS;
                    // [b35_mgt] ... and only while the group fought less than SELIN_ROOM_OWNS_IDLE_MS ago: a room
                    // tactic that holds its pull that long (20261026-024524-mgt-h-5E-15, 337-498 s: "holds the pull
                    // at the rally (Rtrogab: not at the rally)", the rogue parked by its walk guard 10 yd short) gives
                    // the pull back to the harness. The fight's start resets the idle clock (CheckIdle).
                    constexpr uint32 SELIN_ROOM_OWNS_IDLE_MS = 90 * IN_MILLISECONDS;
                    if (owns && party.idleMs + party.eventWaitMs >= SELIN_ROOM_OWNS_IDLE_MS)
                    {
                        owns = false;
                        static std::unordered_map<uint32, uint32> releaseLogMs;  // party seq (world thread)
                        uint32& logged = releaseLogMs[party.seq];
                        if (!logged || getMSTimeDiff(logged, now) >= 30 * IN_MILLISECONDS)
                        {
                            logged = now ? now : 1;
                            LogEvent(party, "boss_area_clear",
                                     JsonObject().Str("boss", "Selin Fireheart").Str("mob", roomMob->GetName())
                                         .Num("entry", entry).Str("action", "room tactic idle: the harness pulls")
                                         .Num("idleSec", (party.idleMs + party.eventWaitMs) / IN_MILLISECONDS));
                        }
                    }
                }
                if (owns)
                {
                    static std::unordered_map<uint32, uint32> roomOwnsLogMs;  // party seq (world thread)
                    uint32& logged = roomOwnsLogMs[party.seq];
                    if (!logged || getMSTimeDiff(logged, now) >= 10 * IN_MILLISECONDS)
                    {
                        logged = now ? now : 1;
                        LogEvent(party, "boss_area_clear",
                                 JsonObject().Str("boss", "Selin Fireheart").Str("mob", roomMob->GetName())
                                     .Num("entry", entry).Str("action", "room tactic owns the pull")
                                     .Num("idle", int64(MagistersTerrace::SelinRoom::Get(leaderAI).idle.size())));
                    }
                    party.steerSinceMs = now;
                    TestRunner::ClearWaypoint(party.leader);
                    return;
                }
            }
        PathResult leg;
        bool legBuilt = false;
        bool approachStopped = false;
        G3D::Vector3 approachStop;
        // [combat_no_damage_release] the group fights and nobody has hit anything for a while: no approach hold
        if (!arrived && !party.targetIsStarter && !party.approach && !GroupDamage::NoDamageRelease(leader))
            if (Creature* mob = object->ToCreature())
            {
                SteerLeg(leader, goal, leg);
                legBuilt = true;
                Creature* blocker = nullptr;
                bool unseenStop = false;
                float const walk = leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP;
                if (!(leg.type & PATHFIND_NOPATH) &&
                    ApproachStop(leader, mob, leg.points, walk, approachStop, blocker, unseenStop, nullptr, &party) &&
                    blocker)
                {
                    G3D::Vector3 const here(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
                    approachStopped = true;
                    G3D::Vector3 const edgeStop = approachStop;
                    // Approach to sight: a stop that hid the blocker from the tank was replaced by a point of the
                    // route that shows it (below); that point stays the stop while target and blocker are the same.
                    auto sightStop = sightStops.find(party.leader);
                    if (sightStop != sightStops.end() && sightStop->second.target != party.target)
                    {
                        sightStops.erase(sightStop);
                        sightStop = sightStops.end();
                    }
                    auto sightHold = sightHolds.find(party.leader);
                    if (sightHold != sightHolds.end() && (sightHold->second.target != party.target ||
                                                          sightHold->second.blocker != blocker->GetGUID()))
                    {
                        sightHolds.erase(sightHold);
                        sightHold = sightHolds.end();
                    }
                    bool sightTried = sightStop != sightStops.end() && sightStop->second.blocker == blocker->GetGUID();
                    // The held line (ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED): "[approach] <leader>: holds at the edge:
                    // planner refused <target> (<n> mobs)", or "no accepted plan for" while the planner said nothing
                    // the harness can read. Once per SIGHT_HOLD_LOG_MS per target and blocker.
                    auto const logHold = [&](PlanVerdict verdict, std::vector<Creature*> const& pack, char const* how)
                    {
                        SightHold& hold = sightHolds[party.leader];
                        // [queue-blocker-pull-gate] the give-way clock counts an unknown verdict only: a refusal
                        // restarts it (scholo-n w24: "refused, 97 mobs" gave way after 8 s)
                        if (hold.blocker != blocker->GetGUID() || verdict == PlanVerdict::Refused)
                            hold.unknownSinceMs = 0;
                        if (!hold.unknownSinceMs)
                            hold.unknownSinceMs = now;
                        hold.target = party.target;
                        hold.blocker = blocker->GetGUID();
                        hold.checkedMs = now;
                        hold.pack.clear();
                        for (Creature* packMob : pack)
                            hold.pack.push_back(packMob->GetGUID());
                        if (hold.loggedMs && getMSTimeDiff(hold.loggedMs, now) < SIGHT_HOLD_LOG_MS)
                            return;
                        hold.loggedMs = now;
                        bool const refused = verdict == PlanVerdict::Refused;
                        LOG_INFO("playerbots", "[approach] {}: holds at the edge: {} {} ({} mobs){}", leader->GetName(),
                                 refused ? "planner refused" : "no accepted plan for", mob->GetName(), pack.size(),
                                 how);
                        LogEvent(party, "approach_sight",
                                 JsonObject().Str("action", "holds at the edge").Str("name", object->GetName())
                                     .Num("entry", object->GetEntry()).Str("blocker", blocker->GetName())
                                     .Num("blockerEntry", blocker->GetEntry())
                                     .Str("verdict", refused ? "refused" : "not accepted")
                                     .Num("packMobs", uint32(pack.size()))
                                     .Real("blockerDistance", leader->GetExactDist(blocker))
                                     .Real("blockerRealAggro", RealAggroRange(blocker, leader))
                                     .Real("x", here.x).Real("y", here.y).Real("z", here.z)
                                     .Real("edgeX", edgeStop.x).Real("edgeY", edgeStop.y).Real("edgeZ", edgeStop.z));
                    };
                    // A sight point that lay in a pack's reach was taken while the planner had accepted its plan:
                    // no longer accepted (refused since, or the plan expired), the leader goes back to the edge (RFC
                    // 20:08: refused while walking to the sight point, the pack came on the walk).
                    if (sightTried && sightStop->second.intoReach && ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED)
                    {
                        std::vector<Creature*> const pack = SightKeepOutPack(party, leader, mob, blocker);
                        PlanVerdict const verdict = TankPlanVerdict(party, leader, pack);
                        if (verdict != PlanVerdict::Accepted)
                        {
                            sightStops.erase(sightStop);
                            sightStop = sightStops.end();
                            sightTried = false;
                            logHold(verdict, pack, ", back from the sight point");
                        }
                    }
                    if (sightTried)
                        approachStop = sightStop->second.point;
                    bool const atStop = (approachStop - here).length() <= APPROACH_ARRIVED;
                    // RFC 18:22-18:24 (user: "why does it stand a minute?"): the stop lay before a corner on a rising
                    // slope, the Earthborer round it out of the tank's sight; the group stood PULL_WAIT_MS, tank_view
                    // found no line of sight and the target was skipped - three times. Checked when the leader
                    // stands at the stop, not after the wait: no line of sight from the tank (its possible-targets
                    // test) - it walks on along the route to the first point that sees the blocker (the corner) and
                    // the pull comes from there (walking into its sight is the pull); no such point within
                    // APPROACH_SIGHT_PATH - the target (its own pack) is skipped at once, a neighbour that stopped
                    // the walk is marked as not seen (TankRejects) and holds no walk. Bosses keep the wait and the
                    // body pull.
                    // [hleft-boss-own-pack] a boss's encounter mob (his formation, his script's call for help) is his
                    // own pack: the boss pull brings it, as the planner says.
                    // [b33_dme] ... and the mob his wander / aggro reach brings (BossBroughtBy)
                    bool const ownPackHere = blocker == mob || blocker->GetExactDist(mob) <= PACK_RADIUS ||
                                             (party.targetIsBoss && BossEncounterMob(leader, mob, blocker)) ||
                                             (party.targetIsBoss && BossBroughtBy(leader, mob, blocker));
                    bool const bossInvolved = party.targetIsBoss || party.bossCreatures.count(blocker->GetGUID()) ||
                                              blocker->IsDungeonBoss() || BossOrder(party, blocker) >= 0;
                    // ROUTE_SKIP_BEHIND_SKIPPED: a neighbour pack the route skipped already (the tank refused it or
                    // did not see it) stops the walk to every target behind it. Drak'Tharon heroic / Mana-Tombs
                    // 2026-10-25: the refused Scourge Brute / Ethereal Spellbinder stopped each next target, the
                    // targets flipped every ~20 s (the 45 s tank view never came) until the 4-minute stall. The target
                    // is skipped at once with the blocker named: the idle guard shows the refusal, not a walk.
                    // An event objective (a hold spot's holdAlways entry: Black Morass' rift keepers) is never
                    // skipped behind a skipped pack: its own walk and body pull decide.
                    std::vector<uint32> keeperEntries;
                    if (int32 const spotIdx = EventSpotIndex(party); spotIdx >= 0)
                        if (EventSpotDef const* spot = EventSpots(party.content->mapId)[size_t(spotIdx)]; spot->hold)
                            keeperEntries = spot->holdAlways;
                    bool const keeperTarget = std::find(keeperEntries.begin(), keeperEntries.end(), mob->GetEntry()) !=
                                              keeperEntries.end();
                    // [dmw-guard-behind-exempt] a door's pending guard keeps its own walk and pull, as a keeper
                    bool const guardTarget = IsPendingDoorGuard(map, mob);
                    // [lead108] b141 rfc-h 119.2 s on: the route target the leader refused (its routeSkips) skipped
                    // Trogg 56, Earthborer 4 ... "behind" it - the leader's refusal of one mob is no refusal of the
                    // walkable mobs behind it (the route walks on to the next, lead96)
                    bool const leaderSkipOnly = party.lead.routeSkips.count(blocker->GetGUID()) &&
                                                Leader::Fight::TankHasWalk(leader, mob);
                    if (ROUTE_SKIP_BEHIND_SKIPPED && !ownPackHere && !bossInvolved && !keeperTarget && !guardTarget &&
                        !leaderSkipOnly && party.skipUntil.count(blocker->GetGUID()))
                    {
                        auto const why = party.skipReason.find(blocker->GetGUID());
                        std::string const blockerWhy = why != party.skipReason.end() ? why->second : "skipped";
                        // [route_blocker_body_pull] the skipped pack is on the way to this target: it is pulled, not
                        // the target skipped behind it - unless its skip is final (unreachable, a door, a body pull
                        // that brought no fight)
                        bool const finalSkip = blockerWhy == "not pulled" ||
                                               blockerWhy.find("unreachable") != std::string::npos ||
                                               blockerWhy.find("door") != std::string::npos ||
                                               blockerWhy.rfind("route end", 0) == 0 || blockerWhy == "path lost";
                        if (!finalSkip && TargetRouteBlocker(party, leader, blocker, object, "skipped " + blockerWhy))
                            return;
                        // [behind_root_pull] plan29 follow-up 2 (scholo-n 20261025-163739-8: "behind skipped
                        // Necrofiend (behind skipped Adept (behind skipped Adept (behind skipped Necrolyte (careful
                        // pull refused (pack)))))", PULL_ESTIMATE_AUDIT: 473 packs skipped only for standing behind a
                        // refused one): the skipped pack on the way is the target again once the group is ready - its
                        // skip and its pack's are cleared, the planner plans it afresh (the adaptive cap, the 12 s
                        // guarantee, a split or los pull) and pull_after_hold ends its hold. A blocker skipped behind
                        // another stops the walk to it the same way, so the chain is walked back to its root. At most
                        // BEHIND_ROOT_TRIES per blocker in BEHIND_ROOT_WINDOW_MS; then the skip stands as before.
                        if (!finalSkip && BEHIND_ROOT_PULL && GroupFullyReady(party, leader) && !party.needBuff)
                        {
                            BehindRootTry& tried = behindRootTries[blocker->GetGUID()];
                            if (!tried.firstMs || getMSTimeDiff(tried.firstMs, now) > BEHIND_ROOT_WINDOW_MS)
                            {
                                tried.firstMs = now ? now : 1;
                                tried.count = 0;
                            }
                            if (tried.count < BEHIND_ROOT_TRIES)
                            {
                                ++tried.count;
                                std::list<Creature*> rootPack;
                                PackMobCheck rootCheck{leader, blocker};
                                Acore::CreatureListSearcher<PackMobCheck> rootSearcher(blocker, rootPack, rootCheck);
                                Cell::VisitObjects(blocker, rootSearcher, PACK_RADIUS);
                                party.skipUntil.erase(blocker->GetGUID());
                                party.skipReason.erase(blocker->GetGUID());
                                for (Creature* packMob : rootPack)
                                {
                                    party.skipUntil.erase(packMob->GetGUID());
                                    party.skipReason.erase(packMob->GetGUID());
                                }
                                LogEvent(party, "behind_root_pull",
                                         JsonObject().Str("name", mob->GetName()).Num("entry", mob->GetEntry())
                                             .Str("blocker", blocker->GetName())
                                             .Num("blockerEntry", blocker->GetEntry())
                                             .Str("blockerSkip", blockerWhy).Num("try", tried.count)
                                             .Num("packMobs", uint32(rootPack.size()))
                                             .Real("blockerDistance", leader->GetExactDist(blocker)));
                                sightStops.erase(party.leader);
                                sightHolds.erase(party.leader);
                                party.neighbourTarget = blocker->GetGUID();
                                party.neighbourTargetUntilMs = now + NEIGHBOUR_TARGET_MS;
                                party.target = blocker->GetGUID();
                                party.targetIsBoss = false;
                                party.targetIsStarter = false;
                                party.targetRouteRule.clear();
                                party.targetSinceMs = now;
                                party.guaranteeHolding = false;
                                party.arrivedMs = 0;
                                party.approach = 0;
                                party.approachBlocker.Clear();
                                party.stopSinceMs = 0;
                                return;
                            }
                        }
                        LOG_INFO("playerbots", "[approach] {}: {} lies behind skipped {} ({:.1f} yd, {}): skip",
                                 leader->GetName(), mob->GetName(), blocker->GetName(), leader->GetExactDist(blocker),
                                 blockerWhy);
                        party.behindSkipped = blocker->GetName() + " (" + blockerWhy + ")";
                        std::string const behindWhy = "behind skipped " + blocker->GetName() + " (" + blockerWhy + ")";
                        // The whole shadow at once (scholo-n wave 11: 78 targets behind one hopeless Necrolyte pack,
                        // one per tick, 78 s without a fight): every idle non-boss mob farther than the blocker in a
                        // BEHIND_CONE_COS cone from the leader through it, within BEHIND_SHADOW_RANGE of it.
                        G3D::Vector3 const toBlocker = G3D::Vector3(blocker->GetPositionX(), blocker->GetPositionY(),
                                                                    blocker->GetPositionZ()) - here;
                        float const blockerDist = toBlocker.length();
                        if (blockerDist > 0.0f)
                        {
                            std::list<Creature*> shadow;
                            ShadowMobCheck shadowCheck{leader};
                            Acore::CreatureListSearcher<ShadowMobCheck> shadowSearcher(blocker, shadow, shadowCheck);
                            Cell::VisitObjects(blocker, shadowSearcher, BEHIND_SHADOW_RANGE);
                            for (Creature* other : shadow)
                            {
                                if (other->GetGUID() == party.target || party.skipUntil.count(other->GetGUID()) ||
                                    party.bossCreatures.count(other->GetGUID()) || other->IsDungeonBoss() ||
                                    BossOrder(party, other) >= 0 || IsPendingDoorGuard(map, other) ||
                                    std::find(keeperEntries.begin(), keeperEntries.end(), other->GetEntry()) !=
                                        keeperEntries.end())
                                    continue;
                                G3D::Vector3 const toOther = G3D::Vector3(other->GetPositionX(), other->GetPositionY(),
                                                                          other->GetPositionZ()) - here;
                                float const otherDist = toOther.length();
                                if (otherDist <= blockerDist ||
                                    toOther.dot(toBlocker) < BEHIND_CONE_COS * otherDist * blockerDist)
                                    continue;
                                party.skipUntil[other->GetGUID()] = now + CAREFUL_REFUSED_SKIP_MS;
                                party.skipReason[other->GetGUID()] = behindWhy;
                            }
                        }
                        SkipTarget(party, party.target, CAREFUL_REFUSED_SKIP_MS, behindWhy);
                        // [b30_overpull] door spin guard (CountDoorSpin): a chain rooted in a closed door that still
                        // stands closed counts as a skip behind that door
                        constexpr char const* doorPrefix = "leader held at a closed door (";
                        if (size_t const at = behindWhy.find(doorPrefix); at != std::string::npos)
                        {
                            size_t const close = behindWhy.find(')', at);
                            size_t const space = close == std::string::npos ? close : behindWhy.rfind(' ', close);
                            uint32 const doorEntry =
                                space != std::string::npos && space > at
                                    ? uint32(std::strtoul(behindWhy.substr(space + 1, close - space - 1).c_str(),
                                                          nullptr, 10))
                                    : 0;
                            for (GameObject* door : ClosedDoors(map, leader))
                            {
                                if (!doorEntry || door->GetEntry() != doorEntry)
                                    continue;
                                bool spinLog = false;
                                uint32 spinSkips = 0;
                                uint32 spinWindowMs = 0;
                                if (CountDoorSpin(party.seq, door->GetGUID(), now, spinLog, spinSkips, spinWindowMs))
                                {
                                    TestRunner::ClearWaypoint(party.leader);
                                    if (spinLog)
                                    {
                                        std::string opener;
                                        JsonObject fields = DoorSpinFields(party, leader, door, spinSkips,
                                                                           spinWindowMs, "behind a skip it rooted",
                                                                           opener);
                                        LOG_INFO("playerbots", "[door spin] {}: {} skips behind closed {} {} in {} s: "
                                                 "retarget holds {} s (opener: {})", party.id, spinSkips,
                                                 door->GetName(), door->GetEntry(), spinWindowMs / IN_MILLISECONDS,
                                                 DOOR_SPIN_HOLD_MS / IN_MILLISECONDS, opener);
                                        LogEvent(party, "door_spin_hold", std::move(fields));
                                    }
                                }
                                break;
                            }
                        }
                        return;
                    }
                    // [b30_overpull] no walk to sight while the tank's planner holds this pull (a patrol / overlap
                    // wait, "held for now") or the group waits out a patrol here (patrol_wait_spot): scholo-n
                    // 20261025-184229-scholo-n-5D-8 147 s "patrol wait: Risen Guard comes to the group (eta 13 s)",
                    // 150 s "approach to sight of Risen Guard ... 16.8 yd on" past a Spectral Researcher (the
                    // pull_option spot's walk is not checked), 152 s the unplanned pull, the patrol joined it, 11
                    // attackers, the wipe. The hold stands at the edge stop; the planner's wait decides.
                    bool plannerHoldsHere = false;
                    {
                        bool holdHopeless = false;
                        uint32 holdWaitMs = 0;
                        std::string holdWhy;
                        bool const targetHeld =
                            PartyRefusal(party, party.target, holdHopeless, holdWaitMs,
                                                           holdWhy) &&
                            holdWhy.rfind("held for now", 0) == 0;
                        holdWhy.clear();
                        bool const blockerHeld =
                            PartyRefusal(party, blocker->GetGUID(), holdHopeless,
                                                           holdWaitMs, holdWhy) &&
                            holdWhy.rfind("held for now", 0) == 0;
                        bool const patrolWait = party.patrolWaitMs && party.patrolWaitTarget == party.target &&
                                                getMSTimeDiff(party.patrolWaitMs, now) <= PATROL_WAIT_HOLD_MS;
                        plannerHoldsHere = !leader->IsInCombat() && (targetHeld || blockerHeld || patrolWait) &&
                                           !bossInvolved;
                        // a sight point taken before the hold is left: back to the edge stop
                        if (plannerHoldsHere && sightTried)
                        {
                            sightStops.erase(party.leader);
                            sightTried = false;
                            approachStop = edgeStop;
                        }
                        if (plannerHoldsHere && ROUTE_APPROACH_TO_SIGHT && atStop &&
                            !leader->IsWithinLOSInMap(blocker))
                        {
                            static std::unordered_map<uint32, uint32> sightHeldLogMs;  // party seq (world thread)
                            uint32& logged = sightHeldLogMs[party.seq];
                            if (!logged || getMSTimeDiff(logged, now) >= SIGHT_HOLD_LOG_MS)
                            {
                                logged = now ? now : 1;
                                LogEvent(party, "approach_sight",
                                         JsonObject().Str("action", "held: the planner waits")
                                             .Str("name", object->GetName()).Num("entry", object->GetEntry())
                                             .Str("blocker", blocker->GetName())
                                             .Num("blockerEntry", blocker->GetEntry())
                                             .Bool("patrolWait", patrolWait).Bool("targetHeld", targetHeld)
                                             .Bool("blockerHeld", blockerHeld)
                                             .Real("x", here.x).Real("y", here.y).Real("z", here.z));
                            }
                        }
                    }
                    if (ROUTE_APPROACH_TO_SIGHT && atStop && !bossInvolved && !plannerHoldsHere &&
                        !leader->IsWithinLOSInMap(blocker))
                    {
                        // ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED: the walk to the sight point stays out of the reach of the
                        // target's and the blocker's packs unless the tank's planner accepted their plan; a point
                        // only inside it holds the leader at the edge (the stop ApproachStop chose), where the
                        // planner's hard minimum / pull back or the wait's skip decide. A hold is searched again
                        // after SIGHT_HOLD_RECHECK_MS.
                        // A fresh look-up: logHold above may have inserted into sightHolds (iterators invalidated).
                        auto const held = sightHolds.find(party.leader);
                        bool const recentHold = held != sightHolds.end() &&
                                                getMSTimeDiff(held->second.checkedMs, now) < SIGHT_HOLD_RECHECK_MS;
                        std::vector<Creature*> keepOut;
                        PlanVerdict verdict = PlanVerdict::Unknown;
                        bool gaveWay = false;    // the edge hold timed out (SIGHT_UNKNOWN_HOLD_MS) without a plan
                        bool intoReach = false;  // the sight point may lie in the target's (accepted) pack's reach
                        if (ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED && !sightTried && !recentHold)
                        {
                            keepOut = SightKeepOutPack(party, leader, mob, blocker);
                            verdict = TankPlanVerdict(party, leader, keepOut);
                            // Sweep batch3 (SM Library 21:47): "holds at the edge: no accepted plan" for minutes
                            // while the tank had "no aggressive target" - it cannot plan a pack it does not see, so
                            // the hold waited for a verdict that never came. An unknown verdict holds at most
                            // SIGHT_UNKNOWN_HOLD_MS, then walking into sight is the pull again (a refusal still holds).
                            constexpr uint32 SIGHT_UNKNOWN_HOLD_MS = 20 * IN_MILLISECONDS;
                            // Uldaman heroic (same wave): a stale refusal (the harness's carefulRefused count)
                            // held a tank with "no aggressive target" just as long - after the same time out of
                            // combat the hold gives way to the planner, which then sees the pack (hard minimum,
                            // pull back or "[pull blocked]"; user rule: always pull at least one mob when ready).
                            uint32 const unknownHoldMs =
                                IsRouteBlocker(party, party.target) || IsRouteBlocker(party, blocker->GetGUID())
                                    ? ROUTE_BLOCKER_SIGHT_HOLD_MS
                                    : SIGHT_UNKNOWN_HOLD_MS;
                            if (!leader->IsInCombat() && held != sightHolds.end() &&
                                held->second.blocker == blocker->GetGUID() && held->second.unknownSinceMs &&
                                getMSTimeDiff(held->second.unknownSinceMs, now) >= unknownHoldMs)
                                gaveWay = verdict == PlanVerdict::Unknown &&  // [queue-blocker-pull-gate]
                                          !(Leader::Leads(Leader::LEADER_NO_BODY_FALLBACK) &&
                                            LeaderDecidesFor(party));  // [lead3] the leader never gives way
                            // Accepted (or given way): walking into sight of the pack is the pull - but of that pack
                            // only. Every other idle pack stays kept out (SightKeepOutOthers; BRS heroic 22:10:30). A
                            // give-way approaches the target's own pack alone, a neighbour blocker's pack included in
                            // the keep-out; an accepted plan covers the target's and the blocker's packs it was asked
                            // about.
                            if (verdict == PlanVerdict::Accepted || gaveWay)
                            {
                                std::vector<Creature*> const accepted = keepOut;
                                keepOut = SightKeepOutOthers(leader, mob);
                                if (!gaveWay)
                                    keepOut.erase(std::remove_if(keepOut.begin(), keepOut.end(),
                                                                 [&accepted](Creature* other)
                                                                 {
                                                                     return std::find(accepted.begin(), accepted.end(),
                                                                                      other) != accepted.end();
                                                                 }),
                                                  keepOut.end());
                                intoReach = true;
                            }
                        }
                        G3D::Vector3 sight;
                        bool heldAtEdge = recentHold;
                        bool found = !sightTried && !recentHold &&
                                     FindSightPoint(leader, blocker, mob, ownPackHere, leg.points, keepOut, sight,
                                                    heldAtEdge);
                        // [pull_option] (1) the sight pull: the route ahead showed no spot, so the arc round the
                        // pack (FindPullSightSpot: side steps that open a corner, real aggro distances). The spot
                        // stands outside every idle mob's aggro, so it needs no accepted plan: from there the tank's
                        // own pull (planner, 30 yd skill) takes the pack.
                        PullSightSearch pullSearch;
                        std::vector<Creature*> pullIdle;
                        auto const isBossMob = [this, &party](Creature* unit)
                        {
                            return unit->IsDungeonBoss() || unit->isWorldBoss() ||
                                   party.bossCreatures.count(unit->GetGUID()) != 0 || BossOrder(party, unit) >= 0;
                        };
                        if (!found && !sightTried && !recentHold)
                        {
                            pullIdle = PullIdleMobs(leader, blocker);
                            FindPullSightSpot(leader, blocker, pullIdle, isBossMob, pullSearch);
                            LogEvent(party, "sight_point_search",
                                     JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                                         .Str("blocker", blocker->GetName()).Num("blockerEntry", blocker->GetEntry())
                                         .Bool("found", pullSearch.found).Num("candidates", pullSearch.candidates)
                                         .Num("inOtherAggro", pullSearch.inOtherAggro)
                                         .Num("inOwnAggro", pullSearch.inOwnAggro).Num("noPath", pullSearch.noPath)
                                         .Num("touchesMore", pullSearch.touchesMore)
                                         .Num("touchesBoss", pullSearch.touchesBoss)
                                         .Real("margin", pullSearch.margin)
                                         .Real("minRadius", pullSearch.minRadius)
                                         .Real("maxRadius", pullSearch.maxRadius)
                                         .Real("x", here.x).Real("y", here.y).Real("z", here.z)
                                         .Real("toX", pullSearch.found ? pullSearch.best.at.x : 0.0f)
                                         .Real("toY", pullSearch.found ? pullSearch.best.at.y : 0.0f)
                                         .Real("toZ", pullSearch.found ? pullSearch.best.at.z : 0.0f)
                                         .Raw("top", PullCandidatesJson(pullSearch.top)));
                            if (pullSearch.found)
                            {
                                found = true;
                                heldAtEdge = false;
                                sight = pullSearch.best.at;
                                LOG_INFO("playerbots",
                                         "[pull_option] {}: sight pull of {} ({} mobs, boss no): arc spot "
                                         "({:.1f},{:.1f},{:.1f}) {:.1f} yd from it, {:.0f} deg off, {:.1f} yd walk",
                                         leader->GetName(), blocker->GetName(), pullSearch.best.touch.mobs.size(),
                                         sight.x, sight.y, sight.z, pullSearch.best.radius,
                                         pullSearch.best.angleOffDeg, pullSearch.best.walk);
                            }
                        }
                        std::string const view = PartyTankView(party, leader, blocker);
                        // The tank takes it as it stands (its view "accepted ..."): no skip, the normal wait at the
                        // stop decides, as for "skip at once: no LoS" below (lbrs-n build 10: the Scarshield Worg
                        // skipped "accepted", 21 targets behind it skipped with it, 0/9 in 12 minutes).
                        if (!found && gaveWay && view.rfind("accepted", 0) != 0)
                        {
                            // [unseen-blocker-pull] A small neighbour pack that holds the approach and that the tank
                            // does not see (no line of sight, so the planner never plans it) becomes the target
                            // itself: its own walk then steps into its sight - inside its own reach when nothing else
                            // stands there (the give-way keeps only the other packs out) - and that is its pull.
                            // Without this every target behind it was skipped in turn (st-n 2026-10-25 081744: Mijan,
                            // Zul'Lor and Hukku each "holds at the edge" behind the same unseen 3-mob Atal'ai Witch
                            // Doctor pack, then "no sight point outside other packs' aggro", the tank ping-ponged).
                            // [route_blocker_body_pull] a neighbour pack on the way, whatever its size, is the target
                            if (!ownPackHere && TargetRouteBlocker(party, leader, blocker, object,
                                                                   "holds the approach, no sight point (" + view +
                                                                       ")"))
                                return;
                            // ... and the route blocker that is the target is walked into
                            if (ownPackHere && IsRouteBlocker(party, party.target))
                            {
                                RouteBlockerBodyPull(party, leader, mob,
                                                     "no sight point outside other packs' aggro (" + view + ")");
                                return;
                            }
                            if (!ownPackHere && !party.skipUntil.count(blocker->GetGUID()) &&
                                party.neighbourTarget != blocker->GetGUID())
                            {
                                std::list<Creature*> blockerPack;
                                PackMobCheck blockerCheck{leader, blocker};
                                Acore::CreatureListSearcher<PackMobCheck> blockerSearcher(blocker, blockerPack,
                                                                                          blockerCheck);
                                Cell::VisitObjects(blocker, blockerSearcher, PACK_RADIUS);
                                if (std::find(blockerPack.begin(), blockerPack.end(), blocker) == blockerPack.end())
                                    blockerPack.push_back(blocker);
                                // [unseen_blocker_any_size] this blocker holds the approach (ApproachStop: the leg to
                                // the target enters its reach), so it stands on the way: no size cap (user rule, a
                                // pack on the only way is never skipped; st-n-92: a 5-mob Atal'ai Slave pack)
                                // [route_blocker_required_only] ... when the target behind it is a required one
                                std::string requiredWhy;
                                bool unseenHold = false;
                                std::string const unseenGate =
                                    BlockerPullGate(party, leader, blocker, object, false, unseenHold);
                                if (!unseenGate.empty() && unseenHold)
                                {
                                    TestRunner::ClearWaypoint(party.leader);  // ready first, nothing skipped
                                    return;
                                }
                                if (unseenGate.empty() &&
                                    (blockerPack.size() <= UNSEEN_BLOCKER_PACK_MAX ||
                                     (ROUTE_BLOCKER_BODY_PULL &&
                                      RouteBlockerBehindRequired(party, map, object, requiredWhy))))
                                {
                                    if (blockerPack.size() > UNSEEN_BLOCKER_PACK_MAX)
                                        LOG_INFO("playerbots", "[unseen_blocker_any_size] {}: {} ({} mobs) holds the "
                                                 "approach to {}: the target despite its size", leader->GetName(),
                                                 blocker->GetName(), blockerPack.size(), object->GetName());
                                    LogEvent(party, "approach_sight",
                                             JsonObject().Str("action", "pull the unseen blocker")
                                                 .Str("name", object->GetName()).Num("entry", object->GetEntry())
                                                 .Str("blocker", blocker->GetName())
                                                 .Num("blockerEntry", blocker->GetEntry())
                                                 .Num("packMobs", uint32(blockerPack.size())).Str("tankView", view)
                                                 .Real("blockerDistance", leader->GetExactDist(blocker)));
                                    sightStops.erase(party.leader);
                                    sightHolds.erase(party.leader);
                                    party.neighbourTarget = blocker->GetGUID();
                                    party.neighbourTargetUntilMs = now + NEIGHBOUR_TARGET_MS;
                                    party.target = blocker->GetGUID();
                                    party.targetIsBoss = false;
                                    party.targetIsStarter = false;
                                    party.targetRouteRule.clear();
                                    party.targetSinceMs = now;
                                    party.guaranteeHolding = false;
                                    party.arrivedMs = 0;
                                    party.approach = 0;
                                    party.approachBlocker.Clear();
                                    party.stopSinceMs = 0;
                                    return;
                                }
                            }
                            // [pull_option] (2) / [boss_area_clear] No sight spot touches the pack alone. Trash round
                            // the next boss (BossAreaTrashOf) or a route blocker is never skipped: the group gets
                            // ready (RouteBlockerBodyPull: full health and mana, everyone here) and the tank takes
                            // the least dangerous option - the body pull, or a sight spot that touches more but
                            // less than the walk in (no boss first, then the fewest mobs). Never stand.
                            if (ROUTE_BOSS_AREA_CLEAR)
                            {
                                Creature* const areaBoss = RouteNextBoss(party.seq, map);
                                char const* const area =
                                    areaBoss ? BossAreaTrashOf(leader, areaBoss, mob,
                                                               party.bossCreatures.count(mob->GetGUID()) != 0)
                                             : nullptr;
                                if ((area || IsRouteBlocker(party, party.target)) &&
                                    Leader::Leads(Leader::LEADER_NO_BODY_FALLBACK) && LeaderDecidesFor(party))
                                {
                                    // [lead3] no sight fallback walk into more mobs, no body pull: the planner whole
                                    LeaderWholePack(party, leader, mob,
                                                    Acore::StringFormat("{}: no clean sight spot ({})",
                                                                        area ? area : "route blocker", view),
                                                    true, true);
                                    return;
                                }
                                if (area || IsRouteBlocker(party, party.target))
                                {
                                    if (pullIdle.empty())
                                        pullIdle = PullIdleMobs(leader, mob);
                                    std::vector<Creature*> intended;
                                    PullTouch const body = PullTouches(leader, mob, leg.points, pullIdle, isBossMob,
                                                                       intended);
                                    bool const sightFallback =
                                        pullSearch.hasFallback && PullTouchBetter(pullSearch.fallback.touch, body);
                                    PullTouch const& chosen = sightFallback ? pullSearch.fallback.touch : body;
                                    LOG_INFO("playerbots",
                                             "[pull_option] {}: {} ({}): no clean sight spot; body pull touches {} "
                                             "mobs (boss {}), best sight spot {} -> {}{}",
                                             leader->GetName(), mob->GetName(), area ? area : "route blocker",
                                             body.mobs.size(), body.boss ? body.bossName : std::string("no"),
                                             pullSearch.hasFallback
                                                 ? Acore::StringFormat("touches {} (boss {})",
                                                                       pullSearch.fallback.touch.mobs.size(),
                                                                       pullSearch.fallback.touch.boss
                                                                           ? pullSearch.fallback.touch.bossName
                                                                           : std::string("no"))
                                                 : std::string("none"),
                                             sightFallback ? "sight spot" : "body pull",
                                             chosen.mobs.size() > PULL_OPTION_MOB_CAP || chosen.boss
                                                 ? " (over the cap / boss: the least dangerous, group ready first)"
                                                 : "");
                                    LogEvent(party, "pull_option",
                                             JsonObject().Str("name", mob->GetName()).Num("entry", mob->GetEntry())
                                                 .Str("choice", sightFallback ? "sight fallback" : "body pull")
                                                 .Num("bodyMobs", int64(body.mobs.size())).Bool("bodyBoss", body.boss)
                                                 .Num("bodyExtra", body.extra)
                                                 .Num("sightMobs", pullSearch.hasFallback
                                                                       ? int64(pullSearch.fallback.touch.mobs.size())
                                                                       : -1)
                                                 .Bool("sightBoss", pullSearch.hasFallback &&
                                                                        pullSearch.fallback.touch.boss)
                                                 .Num("intended", int64(intended.size()))
                                                 .Num("cap", PULL_OPTION_MOB_CAP)
                                                 .Bool("overCap", chosen.mobs.size() > PULL_OPTION_MOB_CAP)
                                                 .Bool("bossTouched", chosen.boss));
                                    if (area)
                                        LogEvent(party, "boss_area_clear",
                                                 JsonObject().Str("boss", areaBoss->GetName())
                                                     .Num("bossEntry", areaBoss->GetEntry())
                                                     .Str("mob", mob->GetName()).Num("entry", mob->GetEntry())
                                                     .Str("why", area)
                                                     .Str("action", sightFallback ? "sight fallback" : "body pull"));
                                    if (sightFallback)
                                    {
                                        if (!GroupFullyReady(party, leader))
                                        {
                                            TestRunner::ClearWaypoint(party.leader);
                                            return;
                                        }
                                        sightStops[party.leader] = {party.target, blocker->GetGUID(),
                                                                    pullSearch.fallback.at, true};
                                        sightHolds.erase(party.leader);
                                        return;
                                    }
                                    RouteBlockerBodyPull(party, leader, mob,
                                                         Acore::StringFormat("{}: no sight spot outside other packs' "
                                                                             "aggro ({})",
                                                                             area ? area : "route blocker", view));
                                    return;
                                }
                            }
                            // The hold gave way and still no point shows the target without another pack's aggro: no
                            // walk in, the target is skipped as not seen (its pack marked unseen, as below).
                            LOG_INFO("playerbots",
                                     "[approach] {}: skip: no sight of {} ({:.1f} yd, {}) outside other packs' aggro "
                                     "({} kept out) after the edge hold",
                                     leader->GetName(), blocker->GetName(), leader->GetExactDist(blocker), view,
                                     keepOut.size());
                            LogEvent(party, "approach_sight",
                                     JsonObject().Str("action", "skip: no sight outside other packs")
                                         .Str("name", object->GetName()).Num("entry", object->GetEntry())
                                         .Str("blocker", blocker->GetName()).Num("blockerEntry", blocker->GetEntry())
                                         .Bool("ownPack", ownPackHere).Bool("heldAtEdge", heldAtEdge)
                                         .Num("keptOut", uint32(keepOut.size())).Str("tankView", view)
                                         .Real("x", here.x).Real("y", here.y).Real("z", here.z));
                            sightStops.erase(party.leader);
                            sightHolds.erase(party.leader);
                            std::list<Creature*> pack;
                            PackMobCheck packCheck{leader, mob};
                            Acore::CreatureListSearcher<PackMobCheck> packSearcher(mob, pack, packCheck);
                            Cell::VisitObjects(mob, packSearcher, PACK_RADIUS);
                            if (std::find(pack.begin(), pack.end(), mob) == pack.end())
                                pack.push_back(mob);
                            for (Creature* packMob : pack)
                                if (!party.bossCreatures.count(packMob->GetGUID()) && !packMob->IsDungeonBoss() &&
                                    !leader->IsWithinLOSInMap(packMob))
                                    TankRejects::MarkUnseen(leader->GetGUID(), packMob->GetGUID());
                            SkipTarget(party, party.target, CAREFUL_REFUSED_SKIP_MS,
                                       "skip: no sight point outside other packs' aggro after the edge hold (" + view +
                                           ")");
                            return;
                        }
                        if (!found && heldAtEdge)
                        {
                            if (!recentHold)
                                logHold(verdict, keepOut, "");
                        }
                        else if (found)
                        {
                            sightStops[party.leader] = {party.target, blocker->GetGUID(), sight,
                                                     intoReach && !gaveWay};
                            sightHolds.erase(party.leader);
                            approachStop = sight;
                            LOG_INFO("playerbots",
                                     "[approach] {}: approach to sight of {} ({:.1f} yd, {}): stop ({:.1f},{:.1f},"
                                     "{:.1f}) -> ({:.1f},{:.1f},{:.1f}) {:.1f} yd on",
                                     leader->GetName(), blocker->GetName(), leader->GetExactDist(blocker), view,
                                     here.x, here.y, here.z, sight.x, sight.y, sight.z, (sight - here).length());
                            LogEvent(party, "approach_sight",
                                     JsonObject().Str("action", "approach to sight").Str("name", object->GetName())
                                         .Num("entry", object->GetEntry()).Str("blocker", blocker->GetName())
                                         .Num("blockerEntry", blocker->GetEntry()).Bool("ownPack", ownPackHere)
                                         .Real("blockerDistance", leader->GetExactDist(blocker)).Str("tankView", view)
                                         .Real("sightDistance", blocker->GetExactDist(sight.x, sight.y, sight.z))
                                         .Real("blockerRealAggro", RealAggroRange(blocker, leader))
                                         .Real("x", here.x).Real("y", here.y).Real("z", here.z)
                                         .Real("toX", sight.x).Real("toY", sight.y).Real("toZ", sight.z));
                        }
                        // The tank takes it without line of sight (its possible targets accept it: the planner's
                        // build-8 rule for route targets): no skip, the normal wait at the stop decides (wave 8
                        // ubrs-n: "skip at once: no LoS" with the tank view "accepted").
                        else if (view.rfind("accepted", 0) != 0 && ownPackHere && IsRouteBlocker(party, party.target))
                        {
                            // [route_blocker_body_pull] no sight of the route blocker: walked into
                            RouteBlockerBodyPull(party, leader, mob, "no line of sight (" + view + ")");
                            return;
                        }
                        else if (view.rfind("accepted", 0) != 0 && !ownPackHere &&
                                 TargetRouteBlocker(party, leader, blocker, object, "no line of sight (" + view + ")"))
                            return;
                        else if (view.rfind("accepted", 0) != 0)
                        {
                            LOG_INFO("playerbots", "[approach] {}: skip at once: no LoS to {} ({:.1f} yd, {}){}",
                                     leader->GetName(), blocker->GetName(), leader->GetExactDist(blocker), view,
                                     sightTried ? " from the sight point" : ", no sight point on the route ahead");
                            LogEvent(party, "approach_sight",
                                     JsonObject().Str("action", "skip at once: no LoS").Str("name", object->GetName())
                                         .Num("entry", object->GetEntry()).Str("blocker", blocker->GetName())
                                         .Num("blockerEntry", blocker->GetEntry()).Bool("ownPack", ownPackHere)
                                         .Real("blockerDistance", leader->GetExactDist(blocker)).Str("tankView", view)
                                         .Bool("fromSightPoint", sightTried)
                                         .Real("x", here.x).Real("y", here.y).Real("z", here.z));
                            sightStops.erase(party.leader);
                            // The blocker's pack, what the tank does not see of it from here, holds no later walk.
                            std::list<Creature*> pack;
                            PackMobCheck packCheck{leader, blocker};
                            Acore::CreatureListSearcher<PackMobCheck> packSearcher(blocker, pack, packCheck);
                            Cell::VisitObjects(blocker, packSearcher, PACK_RADIUS);
                            if (std::find(pack.begin(), pack.end(), blocker) == pack.end())
                                pack.push_back(blocker);
                            for (Creature* packMob : pack)
                                if (!party.bossCreatures.count(packMob->GetGUID()) && !packMob->IsDungeonBoss() &&
                                    (packMob == blocker || !leader->IsWithinLOSInMap(packMob)))
                                    TankRejects::MarkUnseen(leader->GetGUID(), packMob->GetGUID());
                            if (ownPackHere)
                            {
                                SkipTarget(party, party.target, CAREFUL_REFUSED_SKIP_MS,
                                           "skip at once: no LoS (not pulled, tank does not see it: " + view + ")");
                                return;
                            }
                            // A neighbour: the next tick's stop no longer counts it, the walk goes on.
                            approachStopped = false;
                        }
                    }
                    if (approachStopped && (approachStop - here).length() <= APPROACH_ARRIVED)
                    {
                        arrived = true;
                        // A mob outside the target's pack (a neighbour on the way) ended the walk: a refusal from
                        // here skips its pack too.
                        bool const ownPack = ownPackHere;
                        ObjectGuid const blockerGuid = ownPack ? ObjectGuid::Empty : blocker->GetGUID();
                        // A stop that came late (DM wave 38: 16 yd from a static Goblin Woodcarver, inside its real
                        // aggro range, the lumber yard aggroed the next second, Sneed's Shredder walked in, wipe):
                        // the real range of the blocker and of the target (no margin) and the leader's own line of
                        // sight to them (the arrival test) against the eye-line test the stop used.
                        float const blockerAggro = blocker->GetAggroRange(leader) + blocker->m_CombatDistance;
                        float const targetAggro = mob->GetAggroRange(leader) + mob->m_CombatDistance;
                        if (party.approachBlocker != blockerGuid || !party.arrivedMs)
                            LogEvent(party, "approach_stop",
                                     JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                                         .Str("blocker", blocker->GetName()).Num("blockerEntry", blocker->GetEntry())
                                         .Bool("ownPack", ownPack).Real("distance", distance)
                                         .Real("blockerDistance", leader->GetExactDist(blocker))
                                         .Real("blockerRealAggro", blockerAggro)
                                         .Real("targetRealAggro", targetAggro)
                                         .Bool("insideRealAggro", leader->GetExactDist(blocker) < blockerAggro ||
                                                                      distance < targetAggro)
                                         .Bool("blockerLos", leader->IsWithinLOSInMap(blocker))
                                         .Bool("targetLos", leader->IsWithinLOSInMap(mob))
                                         .Real("aggroReach", AggroReach(blocker, leader))
                                         .Bool("unseenStop", unseenStop)
                                         .Real("spreadReach", blockerAggro + APPROACH_FOLLOW_SPREAD)
                                         .Real("blockerDz", blocker->GetPositionZ() - here.z)
                                         .Real("x", here.x).Real("y", here.y).Real("z", here.z));
                        party.approachBlocker = blockerGuid;
                    }
                }
            }

        // [b47_plan] The walk to an event starter (a key door, a lever) had no idle-mob guard at all: the approach stop
        // above skips starters, and so does the walk guard below. scholo-h 20261026-093620-scholo-h-5G-4 (2359 s):
        // the walk to the Viewing Room Door 175167 (256 yd, through the Aberration / Ghoul rooms) sent the tank to
        // (192.0,99.3,104.7), 5 yd from the wandering Diseased Ghoul 48768 - pull 42 "unplanned (no planner pull)",
        // the Risen Aberrations with it, three deaths in 9 s; scholo-n 093620-5E-5 the same at ~2849 s. An idle mob
        // whose reach the next stretch of that walk enters becomes the target (TargetRouteBlocker): the tank's
        // planner pulls it like any route blocker, then the walk goes on.
        if (!arrived && party.targetIsStarter && !leader->IsInCombat() && !GroupDamage::NoDamageRelease(leader))
        {
            if (!legBuilt)
            {
                SteerLeg(leader, goal, leg);
                legBuilt = true;
            }
            Creature* blocker = nullptr;
            bool unseen = false;
            G3D::Vector3 stop;
            if (!(leg.type & PATHFIND_NOPATH) &&
                ApproachStop(leader, object, leg.points, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP, stop, blocker,
                             unseen, nullptr, &party) &&
                blocker && !blocker->IsInCombat())
            {
                static std::set<std::pair<uint32, ObjectGuid>> starterBlockerLogged;  // world thread
                if (starterBlockerLogged.insert({party.seq, blocker->GetGUID()}).second)
                    LogEvent(party, "starter_walk_blocker",
                             JsonObject().Str("starter", object->GetName()).Num("starterEntry", object->GetEntry())
                                 .Str("blocker", blocker->GetName()).Num("blockerEntry", blocker->GetEntry())
                                 .Real("blockerDistance", leader->GetExactDist(blocker)).Real("x", stop.x)
                                 .Real("y", stop.y).Real("z", stop.z));
                if (TargetRouteBlocker(party, leader, blocker, object,
                                       "[b47_plan] stands on the walk to the event starter"))
                    return;
            }
        }
        // ROUTE_HARNESS_WALK_GUARD: a body pull walks into its own pack only. The approach stop above skips a body
        // pull, and its 30-yd waypoint ran through every pack on the boss's route (wave 7: scholo-h pulls 9-12 on the
        // 325-yd route to Jandice, the refused Dark Summoner pack, the Adepts behind the opened Iron Gate; mt-h / mt-n
        // on the 215-yd walk to Shaffar). Another idle pack ends the body pull at its edge.
        if (ROUTE_HARNESS_WALK_GUARD && !arrived && party.approach && !party.targetIsStarter)
            if (Creature* mob = object->ToCreature())
            {
                SteerLeg(leader, goal, leg);
                legBuilt = true;
                Creature* blocker = nullptr;
                bool held = false;
                G3D::Vector3 stop;
                bool const walkStopped =
                    !(leg.type & PATHFIND_NOPATH) &&
                    HarnessWalkStop(leader, leg.points, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP, mob, stop,
                                    blocker, held, &party);
                // [route_blocker_walk_through] The walk into a route blocker (IsRouteBlocker: the user's body pull of
                // a pack on the only way) is not ended by the next idle pack of the same crowd: it is part of that
                // pull, it joins the route blocker's pack and the walk goes on. st-h 2026-10-25 2705-2823 s: the
                // body pull of the Mummified Atal'ai (Jammal'an's room, the planner: 19 mobs come either way)
                // stopped every 16 s at the Atal'ai Deathwalker 14 yd beside it ("harness_hold_refused_pack",
                // walk "body pull"), approach reset, the body pull logged again - 2 minutes, then the 4-minute stall.
                if (walkStopped && blocker && IsRouteBlocker(party, party.target) && !blocker->IsDungeonBoss() &&
                    !party.bossCreatures.count(blocker->GetGUID()) && BossOrder(party, blocker) < 0 &&
                    blocker->GetExactDist(mob) <= ROUTE_BLOCKER_THROUGH_RANGE)
                {
                    if (std::find(party.routeBlockerPack.begin(), party.routeBlockerPack.end(), blocker->GetGUID()) ==
                        party.routeBlockerPack.end())
                    {
                        party.routeBlockerPack.push_back(blocker->GetGUID());
                        party.skipUntil.erase(blocker->GetGUID());
                        party.skipReason.erase(blocker->GetGUID());
                        LogEvent(party, "route_blocker_walk_through",
                                 JsonObject().Str("target", mob->GetName()).Num("targetEntry", mob->GetEntry())
                                     .Str("joins", blocker->GetName()).Num("joinsEntry", blocker->GetEntry())
                                     .Real("apart", blocker->GetExactDist(mob))
                                     .Real("blockerDistance", leader->GetExactDist(blocker)));
                    }
                }
                else if (walkStopped)
                {
                    // [boss_on_the_way_first] another boss of the run stops the body-pull walk to this boss (dme-h
                    // 20261025-153805-dme-h-5A-22: Hydrospawn in the pool between the tank and Zevrim 20 yd above,
                    // body_pull <-> approach_stop every 2 s for 2 minutes, waves 12 / 17 the same): the boss on the
                    // way is fought first - a player clears what stands on the only way up.
                    bool const blockerIsRunBoss =
                        blocker && blocker->IsAlive() && !blocker->IsInCombat() && !blocker->IsImmuneToPC() &&
                        (blocker->IsDungeonBoss() || party.bossCreatures.count(blocker->GetGUID())) &&
                        blocker->GetGUID() != party.target &&
                        !ClassGatedBoss(party.content->mapId, party.content->difficulty, blocker->GetEntry());
                    if (blockerIsRunBoss && party.targetIsBoss)
                    {
                        LogEvent(party, "boss_on_the_way_first",
                                 JsonObject().Str("boss", mob->GetName()).Num("bossEntry", mob->GetEntry())
                                     .Str("first", blocker->GetName()).Num("firstEntry", blocker->GetEntry())
                                     .Real("firstDistance", leader->GetExactDist(blocker))
                                     .Real("bossDz", mob->GetPositionZ() - leader->GetPositionZ()));
                        party.target = blocker->GetGUID();
                        party.targetIsBoss = true;
                        party.targetIsStarter = false;
                        party.targetRouteRule = "boss on the way";
                        party.targetSinceMs = now;
                        party.approach = 0;
                        party.arrivedMs = 0;
                        party.stopSinceMs = 0;
                        party.approachBlocker.Clear();
                        party.orderHoldSinceMs.erase(blocker->GetGUID());
                        TestRunner::ClearWaypoint(party.leader);
                        return;
                    }
                    // [b33_dme] the blocker is no boss, but its pack brings one of the run (BossBroughtBy: Hydrospawn's
                    // wander reaches the Wildspawn Hellcaller's stand, dme-n 20261026-000434-5H-40): the planner
                    // refuses it as that boss's fight, so that boss is fought first, as when he stood on the way.
                    Creature* broughtBoss = nullptr;
                    if (ROUTE_BOSS_BROUGHT_BY_PACK && blocker && !blockerIsRunBoss && party.targetIsBoss &&
                        !blocker->IsInCombat())
                        if (Map* walkMap = leader->GetMap())
                            for (ObjectGuid const& guid : party.bossCreatures)
                            {
                                Creature* boss = walkMap->GetCreature(guid);
                                if (!boss || guid == party.target || boss == blocker ||
                                    ClassGatedBoss(party.content->mapId, party.content->difficulty, boss->GetEntry()) ||
                                    !BossBroughtBy(leader, boss, blocker))
                                    continue;
                                if (!broughtBoss || leader->GetExactDist(boss) < leader->GetExactDist(broughtBoss))
                                    broughtBoss = boss;
                            }
                    if (broughtBoss)
                    {
                        LogEvent(party, "boss_on_the_way_first",
                                 JsonObject().Str("boss", mob->GetName()).Num("bossEntry", mob->GetEntry())
                                     .Str("first", broughtBoss->GetName()).Num("firstEntry", broughtBoss->GetEntry())
                                     .Real("firstDistance", leader->GetExactDist(broughtBoss))
                                     .Str("via", blocker->GetName()).Num("viaEntry", blocker->GetEntry())
                                     .Real("viaDistance", leader->GetExactDist(blocker))
                                     .Str("why", "the blocker's pack brings this boss [b33_dme]")
                                     .Real("bossDz", mob->GetPositionZ() - leader->GetPositionZ()));
                        party.target = broughtBoss->GetGUID();
                        party.targetIsBoss = true;
                        party.targetIsStarter = false;
                        party.targetRouteRule = "boss on the way";
                        party.targetSinceMs = now;
                        party.approach = 0;
                        party.arrivedMs = 0;
                        party.stopSinceMs = 0;
                        party.approachBlocker.Clear();
                        party.orderHoldSinceMs.erase(broughtBoss->GetGUID());
                        TestRunner::ClearWaypoint(party.leader);
                        return;
                    }
                    // [b30_stalls] trash of a boss's area whose only walk runs through that boss (it joins his fight,
                    // JoinsBossFight): it is no pull before him - the planner refuses its pack ("brings a boss"), the
                    // boss_area_clear hold keeps the boss back, deadlock. brd-h 20261025-184229-brd-h-5H-9: Anvilrage
                    // Warden / Bloodhound below Gerstahn, body pull stopped at Gerstahn every 10 s, stalled 0/19
                    // (build 29 broke out only by an accidental edge-walk aggro). The trash stays skipped (a
                    // ReachabilitySkip reason, never unskipped by boss_area_clear) and the boss is the pull.
                    if (blockerIsRunBoss && !party.targetIsBoss && JoinsBossFight(leader, blocker, mob))
                    {
                        LogEvent(party, "boss_on_the_way_first",
                                 JsonObject().Str("boss", blocker->GetName()).Num("bossEntry", blocker->GetEntry())
                                     .Str("trash", mob->GetName()).Num("trashEntry", mob->GetEntry())
                                     .Str("why", "trash reached only through its boss [b30_stalls]")
                                     .Real("bossDistance", leader->GetExactDist(blocker)));
                        std::list<Creature*> walled;
                        PackMobCheck wallCheck{leader, mob};
                        Acore::CreatureListSearcher<PackMobCheck> wallSearcher(mob, walled, wallCheck);
                        Cell::VisitObjects(mob, wallSearcher, PACK_RADIUS);
                        walled.push_back(mob);
                        for (Creature* trash : walled)
                            if (trash != blocker && !trash->IsDungeonBoss() &&
                                !party.bossCreatures.count(trash->GetGUID()) && JoinsBossFight(leader, blocker, trash))
                            {
                                party.skipUntil[trash->GetGUID()] = now + UNREACHABLE_SKIP_MS;
                                party.skipReason[trash->GetGUID()] =
                                    "boss area: given up (reached only through the boss)";
                            }
                        party.target = blocker->GetGUID();
                        party.targetIsBoss = true;
                        party.targetIsStarter = false;
                        party.targetRouteRule = "boss on the way";
                        party.targetSinceMs = now;
                        party.approach = 0;
                        party.arrivedMs = 0;
                        party.stopSinceMs = 0;
                        party.approachBlocker.Clear();
                        party.orderHoldSinceMs.erase(blocker->GetGUID());
                        TestRunner::ClearWaypoint(party.leader);
                        return;
                    }
                    LogHarnessHold(party, leader, blocker, "body pull", stop.x, stop.y, stop.z);
                    party.approach = 0;  // back to the wait at the stop: the neighbour hold decides
                    party.arrivedMs = 0;
                    party.stopSinceMs = 0;
                    party.approachBlocker = blocker->GetGUID();
                    approachStopped = true;
                    approachStop = stop;
                }
            }

        // Progress of the walk to this target. Utgarde Pinnacle wave 8 (and wave 9 at the entrance): a Mindless
        // Servant on a ledge 8 yd above the hall floor - the route ends under it, the same-floor rule never lets the
        // leader "arrive", the waypoint is where it stands (or the leader's own move refuses it), and the group stood
        // there 21 minutes; servants walking in kept the stall detector quiet. A player pulls it from below when it
        // is in range and sight, and else goes elsewhere.
        // A walk that swings back and forth (ohb-h wave 8: the harness route against the bots' own event step,
        // 2144,256 <-> 2167,275 every 30 s, 4 minutes) resets the 3-yd anchor on every swing: an item starter's object
        // the leader comes no nearer to in STARTER_NO_NEARER_MS of steering is stuck as well.
        if (party.steerTarget != party.target)
        {
            party.steerNearest = -1.0f;
            party.steerNoNearerMs = 0;
            starterNearestPath.erase(party.instanceId);
        }
        else if (getMSTimeDiff(party.steerLastMs, now) <= STEER_GAP_MS)
            party.steerNoNearerMs += getMSTimeDiff(party.steerLastMs, now);
        if (party.steerNearest < 0.0f || distance + STEER_PROGRESS_RANGE < party.steerNearest)
        {
            party.steerNearest = distance;
            party.steerNoNearerMs = 0;
        }
        bool swingStuck = party.targetIsStarter && party.steerNoNearerMs > STARTER_NO_NEARER_MS &&
                          party.starterIdx < party.starters.size() &&
                          party.starters[party.starterIdx].def->kind == StarterKind::UseItem;
        // [starter_route_progress] A walk that has to go round: progress is the remaining walk, not the straight
        // distance (LBRS: Urok's Tribute Pile is ~1100 yd of path for 95 yd straight, over the z -18.9 floor; brs-h
        // 02:56 / 06:58 and lbrs-n 05:33 were steer_stuck 75-81 yd under it, dz 67.4, on the right route). Measured
        // only when the straight metric fires; a swing (Old Hillsbrad) shortens no path and stays stuck.
        if (swingStuck)
        {
            PathResult const togo = EvaluatePath(leader, object);
            float& nearestPath = starterNearestPath[party.instanceId];
            if (togo.reachable && togo.length > 0.0f &&
                (nearestPath <= 0.0f || togo.length + STEER_PROGRESS_RANGE < nearestPath))
            {
                LogEvent(party, "starter_route_progress",
                         JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                             .Real("straight", distance).Real("route", togo.length).Real("routeBefore", nearestPath));
                nearestPath = togo.length;
                party.steerNoNearerMs = 0;
                swingStuck = false;
            }
        }
        if (party.steerTarget != party.target || getMSTimeDiff(party.steerLastMs, now) > STEER_GAP_MS ||
            leader->GetExactDist(&party.steerAnchor) > STEER_PROGRESS_RANGE)
        {
            // [w24b-steer-twin-keep] The 4 s retarget flipping between the wanderers of one pack (Botanica bot-n
            // 2026-10-25: Frayer Wildlings, 8 flips from 1538 s to the stop, the leader stood 95 s, no steer_stuck)
            // keeps the clock: a new target of the old one's entry within STEER_TWIN_RANGE of it, the leader not
            // moved, is the same stop.
            Creature const* before = party.steerTarget != party.target ? map->GetCreature(party.steerTarget) : nullptr;
            Creature const* after = object ? object->ToCreature() : nullptr;
            bool const twinFlip = before && after && before->IsAlive() && before->GetEntry() == after->GetEntry() &&
                                  before->GetExactDist(after) <= STEER_TWIN_RANGE &&
                                  getMSTimeDiff(party.steerLastMs, now) <= STEER_GAP_MS &&
                                  leader->GetExactDist(&party.steerAnchor) <= STEER_PROGRESS_RANGE;
            if (twinFlip)
            {
                LOG_INFO("playerbots", "[w24b-steer-twin-keep] {}: {} -> a twin, the steer clock keeps {} s",
                         party.id, after->GetName(), getMSTimeDiff(party.steerSinceMs, now) / IN_MILLISECONDS);
                party.steerTarget = party.target;
            }
            else
            {
                if (party.steerTarget != party.target)
                    party.steerReachWaived = false;
                party.steerTarget = party.target;
                party.steerAnchor = leader->GetPosition();
                party.steerSinceMs = now;
            }
        }
        party.steerLastMs = now;
        if (STEER_REST_NOT_STUCK && !arrived && !leader->IsInCombat() &&
            (RestState::IsDrinking(leader) || RestState::IsEating(leader)))
        {
            party.steerSinceMs = now;
            party.steerAnchor = leader->GetPosition();
            party.steerNoNearerMs = 0;
            // [queue-pull-hold-not-stuck] nor the drive watchdog's "own walk made no progress" (brd-n
            // 20261025-115805-brd-n-5A-2: Rlpalbx drinking at (610.4, -184.4) at 1146-1157 s, bot_cannot_move "route").
            party.driveSinceMs = now;
            party.driveAnchor = leader->GetPosition();
            if (steerRestLogged.insert({party.seq, party.target}).second)
                LogEvent(party, "steer_rest_not_stuck",
                         JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                             .Str("rest", RestState::IsDrinking(leader) ? "drinking" : "eating")
                             .Real("mana", leader->GetMaxPower(POWER_MANA) ? leader->GetPowerPct(POWER_MANA) : 100.0f)
                             .Real("hp", leader->GetHealthPct()).Real("distance", distance)
                             .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                             .Real("z", leader->GetPositionZ()));
        }
        // [b47_door] A member still preparing (its rebuff round open: a warlock's Create Spellstone / Soulstone casts
        // after the run-start buffing step closed at its 15 s bound; or sitting below its line) holds the tank's pull
        // and its walk (PullReadiness::GroupPreparing) - no lack of progress either. brd-h 20261026-104908-brd-h-5D-6:
        // the bear stood 27-48 s at the entrance while Rtlocaa made its stones, "own walk made no progress", the
        // Anvilrage Overseer was skipped as a route end for 300 s, every mob behind it "behind skipped", and the route
        // went to the Shadowforge Gate 170559, whose key starter waits for Roccor (afterDead): 2+ min idle. Bounded by
        // PREPARING_NOT_STUCK_MAX_MS per stand, so a member whose window never closes still ends in the old skip.
        {
            constexpr uint32 PREPARING_NOT_STUCK_MAX_MS = 45 * IN_MILLISECONDS;
            static std::unordered_map<uint32, std::pair<uint32, uint32>> preparingSince;  // seq -> since, seen
            std::pair<uint32, uint32>& prep = preparingSince[party.seq];
            if (!arrived && !leader->IsInCombat() && PullReadiness::GroupPreparing(leader))
            {
                if (!prep.first || getMSTimeDiff(prep.second, now) > STEER_GAP_MS + 3 * IN_MILLISECONDS)
                    prep.first = now ? now : 1;
                prep.second = now ? now : 1;
                if (getMSTimeDiff(prep.first, now) < PREPARING_NOT_STUCK_MAX_MS)
                {
                    party.steerSinceMs = now;
                    party.steerAnchor = leader->GetPosition();
                    party.steerNoNearerMs = 0;
                    party.driveSinceMs = now;
                    party.driveAnchor = leader->GetPosition();
                }
            }
        }
        // [lead2] LEADER_ONE_TANK_MOVER: the tank's open pull order owns its movement - no route stall while it is
        // open, and no clock hands the tank back (PULL_HOLD_STALL_MS / PULL_HOLD_RELEASE_MS are the A/B's old path):
        // the order's own events do (DrainOrderReports; a lost report is order_orphan, a bug event).
        if (Leader::LEADER_ONE_TANK_MOVER)
        {
            if (!arrived && !leader->IsInCombat() && WaypointHeldForPull(party, leader))
            {
                party.steerSinceMs = now;
                party.steerAnchor = leader->GetPosition();
                party.steerNoNearerMs = 0;
                party.driveSinceMs = now;
                party.driveAnchor = leader->GetPosition();
                Leader::PullOrder order;
                PullHoldTrack& hold = pullHolds[party.leader];
                if (Leader::Orders::GetOpen(party.leader, order) && hold.orderId != order.id)
                {
                    hold.orderId = order.id;
                    LogEvent(party, "pull_hold", JsonObject().Str("name", object->GetName())
                                                     .Num("entry", object->GetEntry()).Real("distance", distance)
                                                     .Str("pull", TankPullState(leader)).Num("order", order.id)
                                                     .Str("orderState", Leader::StateName(order.state))
                                                     .Str("orderMethod", Leader::MethodName(order.method))
                                                     .Str("orderTarget", order.targetName)
                                                     .Real("x", leader->GetPositionX())
                                                     .Real("y", leader->GetPositionY())
                                                     .Real("z", leader->GetPositionZ()));
                }
            }
        }
        // [queue-pull-hold-not-stuck] the harness withholds the waypoint for the tank's own pull (GiveWaypoint): no
        // route stall, up to PULL_HOLD_STALL_MS in place; then pull_hold_stall, the waypoint released.
        else if (!arrived && !leader->IsInCombat() && WaypointHeldForPull(party, leader))
        {
            PullHoldTrack& hold = pullHolds[party.leader];
            if (!hold.sinceMs || getMSTimeDiff(hold.seenMs, now) > STEER_GAP_MS ||
                leader->GetExactDist(&hold.anchor) > STEER_PROGRESS_RANGE)
            {
                hold.sinceMs = now ? now : 1;
                hold.anchor = leader->GetPosition();
                hold.logged = false;
                hold.stallLogged = false;
            }
            hold.seenMs = now ? now : 1;
            uint32 const heldMs = getMSTimeDiff(hold.sinceMs, now);
            if (heldMs < PULL_HOLD_STALL_MS)
            {
                party.steerSinceMs = now;
                party.steerAnchor = leader->GetPosition();
                party.steerNoNearerMs = 0;
                party.driveSinceMs = now;
                party.driveAnchor = leader->GetPosition();
                if (!hold.logged)
                {
                    hold.logged = true;
                    LogEvent(party, "pull_hold", JsonObject().Str("name", object->GetName())
                                                     .Num("entry", object->GetEntry()).Real("distance", distance)
                                                     .Str("pull", TankPullState(leader))
                                                     .Real("x", leader->GetPositionX())
                                                     .Real("y", leader->GetPositionY())
                                                     .Real("z", leader->GetPositionZ()));
                }
            }
            else if (!hold.stallLogged)
            {
                hold.stallLogged = true;
                hold.releaseUntilMs = now + PULL_HOLD_RELEASE_MS;
                party.steerSinceMs = now;  // the released walk gets its own STEER_STUCK_MS
                party.steerAnchor = leader->GetPosition();
                LogEvent(party, "pull_hold_stall",
                         JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                             .Real("distance", distance).Num("heldSec", heldMs / IN_MILLISECONDS)
                             .Str("pull", TankPullState(leader))
                             .Num("releaseSec", PULL_HOLD_RELEASE_MS / IN_MILLISECONDS)
                             .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                             .Real("z", leader->GetPositionZ()));
                LOG_INFO("playerbots",
                         "[queue-pull-hold-not-stuck] {}: {} held {} s in place by its own pull ({}) - the waypoint "
                         "to {} is no longer withheld for {} s",
                         party.id, leader->GetName(), heldMs / IN_MILLISECONDS, TankPullState(leader),
                         object->GetName(), PULL_HOLD_RELEASE_MS / IN_MILLISECONDS);
            }
        }
        if (!arrived && party.steerReachWaived && distance <= PULL_RANGE)
            arrived = true;
        if (!arrived && (getMSTimeDiff(party.steerSinceMs, now) > STEER_STUCK_MS || swingStuck))
        {
            // A released Violet Hold prisoner is the objective, never "route end" (vh-h wave 9: Zuramat skipped at
            // 7 yd): the bots' own "vh engage" walks to him from here.
            Creature const* stuckOn = object->ToCreature();
            bool const releasedPrisoner = stuckOn && leader->GetMapId() == VIOLET_HOLD_MAP_ID &&
                                          party.bossCreatures.count(stuckOn->GetGUID()) &&
                                          !stuckOn->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
            // [b32_bm] a walk-in (party.approach) that stopped beside the target is in reach too: bm-h
            // 20261025-223301-bm-h-5H-72 skipped Aeonus at 6.0 yd as "route end" for 300 s (melee range 5.8).
            bool const walkInBeside = !party.targetIsStarter && party.approach && stuckOn &&
                                      distance <= leader->GetMeleeRange(stuckOn) + BODY_PULL_STUCK_REACH &&
                                      leader->IsWithinLOSInMap(object);
            bool const inReach = releasedPrisoner || walkInBeside ||
                                 (!party.targetIsStarter && !party.approach && distance <= PULL_RANGE &&
                                  leader->IsWithinLOSInMap(object));
            // [hleft-ak-jedoga-owns] Ahn'kahet: a Twilight Initiate 30114 is never "route end" while Jedoga lives or
            // her room owns the pull (AKJedogaRoom::OwnsPull): the AK strategy brings them, she comes only after.
            if (object->GetEntry() == AK_TWILIGHT_INITIATE_ENTRY)
            {
                PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
                bool jedogaAlive = false;
                for (ObjectGuid const& guid : party.bossCreatures)
                    if (Creature* boss = map->GetCreature(guid))
                        jedogaAlive |= boss->IsAlive() && boss->GetEntry() == AK_JEDOGA_ENTRY;
                if ((leaderAI && AKJedogaRoom::OwnsPull(leaderAI)) || jedogaAlive)
                {
                    party.steerSinceMs = now;
                    TestRunner::ClearWaypoint(party.leader);
                    return;
                }
            }
            // [selin_entry_no_route_end] A live, idle Selin is never "route end" while her room tactic holds the
            // entry (MagistersTerrace::SelinRoom::EntryHolds: nobody pulls her until every member stands inside):
            // the steer stands still by design. mgt-n 2026-10-25 (20261025-115805-mgt-n-5A-15, 627 s): she was
            // skipped for 300 s while the members swayed at x 206-218, everything else lay behind passage gate
            // 187979, 215 s idle.
            // At most SELIN_ENTRY_HOLD_MAX_MS in a row (a member that never walks in is the stall watchdog's).
            constexpr uint32 SELIN_ENTRY_HOLD_MAX_MS = 180 * IN_MILLISECONDS;
            struct SelinEntryHold
            {
                uint32 sinceMs = 0;
                uint32 seenMs = 0;
                uint32 logMs = 0;
            };
            static std::unordered_map<uint32, SelinEntryHold> selinEntryHolds;  // instance id (world thread)
            if (Creature const* boss = object->ToCreature();
                boss && boss->GetEntry() == 24723 /* Selin Fireheart */ && boss->IsAlive() && !boss->IsInCombat() &&
                leader->GetMapId() == MagistersTerrace::MAP_MAGISTERS_TERRACE &&
                MagistersTerrace::SelinRoom::EntryHolds(leader))
            {
                SelinEntryHold& hold = selinEntryHolds[party.instanceId];
                if (!hold.sinceMs || getMSTimeDiff(hold.seenMs, now) > STEER_STUCK_MS + 10 * IN_MILLISECONDS)
                    hold.sinceMs = now ? now : 1;
                hold.seenMs = now ? now : 1;
                if (getMSTimeDiff(hold.sinceMs, now) < SELIN_ENTRY_HOLD_MAX_MS)
                {
                    party.steerSinceMs = now;
                    TestRunner::ClearWaypoint(party.leader);
                    if (!hold.logMs || getMSTimeDiff(hold.logMs, now) >= 10 * IN_MILLISECONDS)
                    {
                        hold.logMs = now ? now : 1;
                        LOG_INFO("playerbots", "[selin_entry_no_route_end] {}: Selin is no route end while her room "
                                 "entry holds ({:.1f} yd, {} s)", party.id, distance,
                                 getMSTimeDiff(hold.sinceMs, now) / IN_MILLISECONDS);
                    }
                    return;
                }
            }
            // [hleft-swim-back] held in the water below the target (a ledge the swimmer cannot climb): back the way
            // it swam in, before any skip.
            if (ROUTE_SWIM_BACK && !party.swimBack && !releasedPrisoner && leader->IsInWater() &&
                party.swimTrail.size() >= 2 && object->GetPositionZ() > leader->GetPositionZ() + SWIM_CLIMB_DZ)
            {
                party.swimBack = true;
                party.swimBackSinceMs = now;
                party.steerSinceMs = now;
                LogEvent(party, "swim_back", JsonObject().Str("action", "back along the swim")
                                                 .Str("target", object->GetName())
                                                 .Real("dz", object->GetPositionZ() - leader->GetPositionZ())
                                                 .Num("trail", uint32(party.swimTrail.size()))
                                                 .Real("dryX", party.swimTrail.front().GetPositionX())
                                                 .Real("dryY", party.swimTrail.front().GetPositionY())
                                                 .Real("dryZ", party.swimTrail.front().GetPositionZ()));
                return;
            }
            LogEvent(party, "steer_stuck", JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                                               .Bool("eventStarter", party.targetIsStarter)
                                               .Bool("bodyPull", party.approach != 0).Real("distance", distance)
                                               .Real("dz", object->GetPositionZ() - leader->GetPositionZ())
                                               .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                                               .Real("z", leader->GetPositionZ())
                                               .Str("action", inReach ? "pull from here" : "skip"));
            if (inReach)
            {
                party.steerReachWaived = true;
                arrived = true;
            }
            else if (party.targetIsStarter)
            {
                StarterTrack& starter = party.starters[party.starterIdx];
                // One of several objects that take the item (Old Hillsbrad wave 34: the walk to the barrel at
                // 2119,42 ended 11 yd above it on the Durnholde wall, steer_stuck; the starter waited 2 minutes and
                // chose the same nearest barrel again, four times, 0 of 5 planted in 25 minutes): that object is
                // passed over, the next nearest one comes at once - a player goes to another barrel.
                if (starter.def->kind == StarterKind::UseItem)
                {
                    starter.stuckUntilMs[party.target] = now + STARTER_OBJECT_STUCK_SKIP_MS;
                    starter.nextAllowedMs = now;
                }
                else
                    starter.nextAllowedMs = now + STARTER_STUCK_RETRY_MS;
                party.target.Clear();
                party.nextRetargetMs = now;
                TestRunner::ClearWaypoint(party.leader);
                return;
            }
            else
            {
                // A boss holds the later ones as "next boss unreachable" does (order hold).
                if (party.targetIsBoss)
                    party.orderHoldSinceMs.emplace(party.target, now);
                // A boss in another wing (Scarlet Monastery heroic 2026-10-25: Graveyard cleared, Loksey / Doan
                // walked to the Graveyard exit 5 times, 1959 yd short, 4 minutes without a fight - the wing change
                // waited for "no targets", which the boss itself never let come): the group changes to the
                // entrance nearest to him.
                if (party.targetIsBoss && party.entrances.size() > 1 && party.entranceIdx < party.entrances.size() &&
                    !party.wingWalk && !EventGatherApplies(party) && EventSpotIndex(party) < 0)
                    if (int32 const wing = NearestEntrance(party, object);
                        wing >= 0 && size_t(wing) != party.entranceIdx &&
                        party.entrances[size_t(wing)].GetExactDist(&party.entrances[party.entranceIdx]) >=
                            SAME_ENTRANCE_RANGE)
                    {
                        StartWingChange(party, size_t(wing), "boss route end in another wing");
                        return;
                    }
                // Ahn'kahet wave 9/10: a ledge of Swarmers 7 yd above the leader - skipping them one at a time
                // retargets the next one 20 s later, for 4 minutes. Skip the same-entry mobs around it with it, as
                // "route end" from here (unreachableFrom: checked again once the leader stands elsewhere); never a
                // boss or an encounter creature.
                uint32 twinsSkipped = 0;
                // Never for the Twilight Initiates 30114 (Ahn'kahet): Jedoga's gate, she comes only once they are
                // dead (wotlk tactic audit, wave 21) - skipped by the dozen they kept her away ("twin_skip_exempt").
                bool const twinExempt = object->GetEntry() == 30114;
                if (twinExempt)
                    LOG_INFO("playerbots", "[route] {}: twin_skip_exempt: {} {} - only this one skipped", party.id,
                             object->GetName(), object->GetEntry());
                if (!party.targetIsBoss && ROUTE_TWIN_SKIP && !twinExempt)
                    if (Creature* mob = object->ToCreature())
                    {
                        std::list<Creature*> twins;
                        Acore::AllCreaturesOfEntryInRange check(mob, mob->GetEntry(), STEER_TWIN_RANGE);
                        Acore::CreatureListSearcher<Acore::AllCreaturesOfEntryInRange> searcher(mob, twins, check);
                        Cell::VisitObjects(mob, searcher, STEER_TWIN_RANGE);
                        for (Creature* twin : twins)
                            if (twin != mob && twin->IsAlive() && !twin->IsInCombat() && !twin->IsDungeonBoss() &&
                                !twin->isWorldBoss() && !party.bossCreatures.count(twin->GetGUID()) &&
                                BossOrder(party, twin) < 0)
                            {
                                party.skipUntil[twin->GetGUID()] = now + UNREACHABLE_SKIP_MS;
                                party.skipReason[twin->GetGUID()] = "route end (same entry)";
                                party.unreachableFrom[twin->GetGUID()] = leader->GetPosition();
                                if (!leader->IsWithinLOSInMap(twin))
                                    MarkUnseenRouteEnd(twin, now + UNREACHABLE_SKIP_MS);
                                ++twinsSkipped;
                            }
                    }
                if (!party.targetIsBoss)
                    if (Creature* mob = object->ToCreature(); mob && !leader->IsWithinLOSInMap(mob))
                    {
                        MarkUnseenRouteEnd(mob, now + UNREACHABLE_SKIP_MS);
                        // Its pack too, what of it the leader does not see (the route skips them "behind" it).
                        std::list<Creature*> pack;
                        PackMobCheck check{leader, mob};
                        Acore::CreatureListSearcher<PackMobCheck> searcher(mob, pack, check);
                        Cell::VisitObjects(mob, searcher, PACK_RADIUS);
                        for (Creature* packMob : pack)
                            if (packMob != mob && !packMob->IsDungeonBoss() &&
                                !party.bossCreatures.count(packMob->GetGUID()) &&
                                !leader->IsWithinLOSInMap(packMob))
                                MarkUnseenRouteEnd(packMob, now + UNREACHABLE_SKIP_MS);
                        LogEvent(party, "unseen_route_end",
                                 JsonObject().Str("name", mob->GetName()).Num("entry", mob->GetEntry())
                                     .Real("distance", leader->GetExactDist(mob))
                                     .Num("twins", twinsSkipped));
                    }
                SkipTarget(party, party.target, UNREACHABLE_SKIP_MS,
                           twinsSkipped ? "route end (+" + std::to_string(twinsSkipped) + " same entry)"
                                        : std::string("route end"));
                return;
            }
        }

        if (arrived)
        {
            party.steerSinceMs = now;  // waiting for the pull is no lack of progress
            TestRunner::ClearWaypoint(party.leader);
            if (party.targetIsStarter)
            {
                StarterTrack& starter = party.starters[party.starterIdx];
                // Used up meanwhile (MarkStartersDoneByGroup): nothing to click any more.
                bool const used = starter.uses < starter.def->maxUses && UseStarter(party, leader, starter, object);
                party.target.Clear();
                party.nextRetargetMs = now + 3 * IN_MILLISECONDS;
                // [ulda-ritual-hold] a summoning ritual casts its spell 5 s after the click, only while the user still
                // channels (GameObject::Update CheckRitualList): no new target, no walk until then.
                if (GameObject const* ritual = object->ToGameObject();
                    used && ritual && ritual->GetGoType() == GAMEOBJECT_TYPE_SUMMONING_RITUAL)
                {
                    ritualHoldUntilMs[party.instanceId] = now + RITUAL_CHANNEL_HOLD_MS;
                    party.nextRetargetMs = now + RITUAL_CHANNEL_HOLD_MS;
                    LogEvent(party, "ritual_hold", JsonObject().Str("note", starter.def->note)
                                                       .Num("entry", ritual->GetEntry())
                                                       .Num("holdMs", RITUAL_CHANNEL_HOLD_MS));
                }
                return;
            }

            if (!party.arrivedMs)
            {
                // ROUTE_STOP_KEEPS_WAIT: Stratholme / Uldaman / Drak'Tharon / Mana-Tombs 2026-10-25 - the 4 s retarget
                // flipped between the mobs of one refused pack (or the targets behind one blocker) every 4-20 s, each
                // flip reset this clock, the 45 s tank view never came and the group stood 3-4 minutes. The same
                // stop keeps the clock; a boss and a body pull keep their own.
                bool const sameStop = ROUTE_STOP_KEEPS_WAIT && party.stopSinceMs && !party.targetIsBoss &&
                                      !party.approach && getMSTimeDiff(party.stopLastMs, now) <= STOP_KEEP_GAP_MS &&
                                      leader->GetExactDist(&party.stopPos) <= STOP_SAME_RANGE;
                party.arrivedMs = sameStop ? party.stopSinceMs : now;
                party.stopSinceMs = party.arrivedMs;
                party.stopPos = leader->GetPosition();
            }
            party.stopLastMs = now;

            uint32 waited = getMSTimeDiff(party.arrivedMs, now);  // [held_for_now_cap] may be set waited out
            // An event's waiting place (EventSpotDef::hold) while its event runs: what stands there attacks the event
            // (Black Morass wave 10: a rift pack of 8 was "careful pull refused" for 3 minutes next to Medivh, broke
            // his shield, the event failed and everything despawned). No deferral there, and a short wait only.
            bool atEventHold = false;
            if (ROUTE_HOLD_SPOT_FIGHT)
                if (int32 const spotIdx = EventSpotIndex(party); spotIdx >= 0)
                    if (EventSpotDef const* spot = EventSpots(party.content->mapId)[size_t(spotIdx)];
                        SpotHolds(party, *spot))
                        // The event's own objective (a rift keeper at its rift) is fought wherever it stands.
                        atEventHold = object->GetExactDist(spot->x, spot->y, spot->z) <=
                                          (spot->holdRadius > 0.0f ? spot->holdRadius : HOLD_TARGET_RADIUS) ||
                                      std::find(spot->holdAlways.begin(), spot->holdAlways.end(),
                                                object->GetEntry()) != spot->holdAlways.end();
            uint32 const pullWaitMs = atEventHold ? BODY_PULL_WAIT_MS : PULL_WAIT_MS;
            // [pull_after_hold] the group stands ready at this target PULL_AFTER_HOLD_MS: the hold ends (below)
            bool holdForce = false;
            // [lead24] LEADER_NO_FORCING_TIMERS: no ready-hold release for a led party (pull_after_hold off)
            bool const ledNoForce = Leader::LEADER_NO_FORCING_TIMERS && LeaderDecidesFor(party);
            if (PULL_AFTER_HOLD && !ledNoForce && !party.approach && !atEventHold && !party.content->raid &&
                !party.targetIsStarter)
            {
                bool const groupReady =
                    GroupFullyReady(party, leader) && !party.needBuff && !PrePullCc::IsHolding(leader);
                HoldReady& ready = holdReady[party.leader];
                // plan29 follow-up 2: the targets flip at one stop (the 4 s retarget between the mobs of a pack or
                // the targets behind one blocker): the same stop keeps the clock, as ROUTE_STOP_KEEPS_WAIT does
                bool const sameStop = ready.sinceMs && leader->GetExactDist(&ready.at) <= STOP_SAME_RANGE;
                if ((ready.target != party.target && !sameStop) || !groupReady)
                {
                    ready.target = party.target;
                    ready.sinceMs = groupReady ? (now ? now : 1) : 0;
                    ready.at = leader->GetPosition();
                }
                else if (ready.target != party.target)
                    ready.target = party.target;
                else if (!ready.sinceMs)
                {
                    ready.sinceMs = now ? now : 1;
                    ready.at = leader->GetPosition();
                }
                Creature* const stopper =
                    party.approachBlocker.IsEmpty() ? nullptr : map->GetCreature(party.approachBlocker);
                Creature* const holdBlocker =
                    stopper && stopper->IsAlive() && !stopper->IsInCombat() ? stopper : object->ToCreature();
                std::string requiredWhy;
                bool const onWay = stopper && stopper->IsAlive() && !stopper->IsInCombat();
                bool const required = party.targetIsBoss || IsRouteBlocker(party, party.target) ||
                                      RouteBlockerBehindRequired(party, map, object, requiredWhy);
                if (groupReady && ready.sinceMs && getMSTimeDiff(ready.sinceMs, now) >= PULL_AFTER_HOLD_MS &&
                    holdBlocker && (onWay || required) && (!party.targetIsBoss || onWay))
                {
                    holdForce = true;
                    // (4) a closed door that opens by use hides the blocker: the tank opens it, the pull follows
                    if (!leader->IsWithinLOSInMap(holdBlocker) && !holdBlocker->IsDungeonBoss())
                    {
                        Movement::PointsArray doorPath;
                        {
                            PathGenerator path(leader);
                            path.CalculatePath(holdBlocker->GetPositionX(), holdBlocker->GetPositionY(),
                                               holdBlocker->GetPositionZ(), false);
                            if (!(path.GetPathType() & PATHFIND_NOPATH))
                                doorPath = path.GetPath();
                        }
                        if (doorPath.size() < 2)
                            doorPath = {G3D::Vector3(leader->GetPositionX(), leader->GetPositionY(),
                                                     leader->GetPositionZ()),
                                        G3D::Vector3(holdBlocker->GetPositionX(), holdBlocker->GetPositionY(),
                                                     holdBlocker->GetPositionZ())};
                        ClosedDoorHit const hit = PathThroughClosedDoor(leader, doorPath);
                        if (hit.Found() && !DoorEventOf(hit.door) && DoorOpensByUse(hit.door))
                        {
                            bool const opened = OpenDoorByUse(leader, hit.door);
                            G3D::Vector3 const at = PointOnPath(doorPath, std::max(0.0f, hit.walked - DOOR_OPEN_BACK));
                            if (!opened)
                                doorWalks[party.leader] = {hit.door->GetGUID(), holdBlocker->GetGUID(), at,
                                                           now + DOOR_WALK_MS};
                            LogEvent(party, "pull_after_hold",
                                     JsonObject().Str("action", opened ? "opened the door" : "walks to the door")
                                         .Str("name", object->GetName()).Num("entry", object->GetEntry())
                                         .Str("blocker", holdBlocker->GetName())
                                         .Num("blockerEntry", holdBlocker->GetEntry())
                                         .Str("door", hit.door->GetName()).Num("doorEntry", hit.door->GetEntry())
                                         .Real("doorWalk", hit.walked)
                                         .Num("readySec", getMSTimeDiff(ready.sinceMs, now) / IN_MILLISECONDS)
                                         .Num("waitedSec", waited / IN_MILLISECONDS));
                            ready.sinceMs = now ? now : 1;  // the pull through the open door gets its own clock
                            if (!opened)
                                SetLeaderWaypoint(party, leader, at.x, at.y, at.z);
                            return;
                        }
                    }
                    pullAfterHoldGates[party.leader] = {holdBlocker->GetGUID(), now + PULL_AFTER_HOLD_GATE_MS};
                    LogEvent(party, "pull_after_hold",
                             JsonObject().Str("action", onWay && holdBlocker == stopper ? "pull the blocker on the way"
                                                                                        : "pull the target")
                                 .Str("name", object->GetName()).Num("entry", object->GetEntry())
                                 .Str("blocker", holdBlocker->GetName()).Num("blockerEntry", holdBlocker->GetEntry())
                                 .Bool("boss", party.targetIsBoss).Str("required", requiredWhy)
                                 .Real("blockerDistance", leader->GetExactDist(holdBlocker))
                                 .Bool("blockerLos", leader->IsWithinLOSInMap(holdBlocker))
                                 .Num("readySec", getMSTimeDiff(ready.sinceMs, now) / IN_MILLISECONDS)
                                 .Num("waitedSec", waited / IN_MILLISECONDS));
                    ready.sinceMs = now ? now : 1;  // one forced decision per PULL_AFTER_HOLD_MS
                }
            }
            // ROUTE_BOSS_PULL_IMMEDIATE: a boss is body-pulled BOSS_PULL_READY_MS after the group stands ready for it
            // (GroupFullyReady, its boss buffs done, the rest hold above passed), not after PULL_WAIT_MS. The tank's
            // careful planner never picks a boss (RFC 20:10, Oggleflint: "[pull hold] no aggressive target" until
            // the 45 s ran out). The long wait stays while the tank holds its pull for a known reason the harness
            // can read: the group's pre-pull crowd control (PrePullCc::IsHolding). Not in raids (encounter set-ups).
            // A stop a neighbour pack made (approachBlocker: not the boss's own pack) is no body-pull spot: the body
            // pull would walk the whole route through it (wave 7 scholo-h 01:30:28, mt-h 01:44:09, mt-n 01:44:21).
            Creature* const neighbour =
                party.approachBlocker.IsEmpty() ? nullptr : map->GetCreature(party.approachBlocker);
            bool neighbourStop = ROUTE_HARNESS_WALK_GUARD && party.targetIsBoss && neighbour && neighbour->IsAlive() &&
                                 !neighbour->IsInCombat() && !neighbour->IsDungeonBoss() &&
                                 !party.bossCreatures.count(neighbour->GetGUID()) && BossOrder(party, neighbour) < 0;
            // [hleft-boss-own-pack] the boss's encounter mob is no neighbour (the planner pulls it as his fight).
            if (neighbourStop)
                if (Creature* bossTarget = object->ToCreature(); bossTarget && BossEncounterMob(leader, bossTarget,
                                                                                               neighbour))
                    neighbourStop = false;
            if (neighbourStop && neighbour->GetFormation())
                for (ObjectGuid const& bossGuid : party.bossCreatures)
                    if (Creature* boss = map->GetCreature(bossGuid);
                        boss && boss->IsAlive() && boss->GetFormation() == neighbour->GetFormation())
                        neighbourStop = false;
            bool bossPullNow = false;
            if (ROUTE_BOSS_PULL_IMMEDIATE && party.targetIsBoss && !party.approach && !party.content->raid &&
                !neighbourStop)
            {
                // [lead13] arc-n / arc-h b56 (Zereketh: "held for now: Rtpalbe at 70 % mana" for good): a member
                // above its own rest line does not drink, so the 80 % lines alone never let the boss pull come. In
                // leader mode the boss is pulled ready by the old gate, or - the RaidLeader's old [ready_hold_30s]
                // rule - after PULL_HOLD_READY_MAX_MS with the leader ready and every member above its own lines.
                bool levelsReady = GroupFullyReady(party, leader);
                if (!levelsReady && LeaderDecidesFor(party) && party.lead.ready)
                {
                    bool atLines = true;
                    for (Member const& member : party.members)
                        if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                            bot && bot->IsAlive() && GET_PLAYERBOT_AI(bot) &&
                            (RestState::WantsDrink(bot) || RestState::WantsFood(bot) || RestState::IsDrinking(bot) ||
                             RestState::IsEating(bot)))
                            atLines = false;
                    if (!atLines)
                        party.lead.atLinesSinceMs = 0;
                    else if (!party.lead.atLinesSinceMs)
                        party.lead.atLinesSinceMs = now ? now : 1;
                    levelsReady = party.lead.atLinesSinceMs &&
                                  getMSTimeDiff(party.lead.atLinesSinceMs, now) >= RaidLeader::PULL_HOLD_READY_MAX_MS;
                    if (levelsReady)
                        LOG_INFO("playerbots", "[lead13] boss_ready_at_lines {}: {} - every member above its own "
                                 "rest lines {} s, the leader ready: the boss pull", party.id, object->GetName(),
                                 getMSTimeDiff(party.lead.atLinesSinceMs, now) / IN_MILLISECONDS);
                }
                bool const ready = levelsReady && !party.needBuff &&
                                   (party.bossBuffed.count(party.target) || distance >= BOSS_BUFF_DISTANCE) &&
                                   !PrePullCc::IsHolding(leader);
                BossReady& boss = bossReady[party.leader];
                if (boss.target != party.target || !ready)
                {
                    boss.target = party.target;
                    boss.sinceMs = ready ? now : 0;
                }
                else if (!boss.sinceMs)
                    boss.sinceMs = now;
                bossPullNow = ready && boss.sinceMs && getMSTimeDiff(boss.sinceMs, now) >= BOSS_PULL_READY_MS &&
                              waited <= pullWaitMs;
                if (bossPullNow)
                    LOG_INFO("playerbots", "[approach] {}: boss pull: immediate (group ready) - {} {:.1f} yd, "
                             "waited {} s", leader->GetName(), object->GetName(), distance, waited / IN_MILLISECONDS);
            }
            // [route_blocker_body_pull] the target is a pack on the route's way: the planner and the guarantee get
            // ROUTE_BLOCKER_PREP_MS, then the ready group walks in (no skip).
            bool const routeBlocker = IsRouteBlocker(party, party.target);
            // The planner's own refusal of this pack (or of the neighbour that stopped the walk), when fresh.
            bool refusalHopeless = false;
            uint32 refusalWaitMs = 0;
            std::string refusalWhy;
            // [lead43] no guarantee hold for a led party: the leader's refusal moved the route off the mob (Retarget)
            bool const plannerRefused =
                ROUTE_CAREFUL_GUARANTEE_HOLD && !LeaderDecidesFor(party) && !party.approach && !party.targetIsBoss &&
                !atEventHold &&
                !party.content->raid && party.content->size <= 5 &&
                (PartyRefusal(party, party.target, refusalHopeless, refusalWaitMs,
                                                refusalWhy) ||
                 (!party.approachBlocker.IsEmpty() &&
                  PartyRefusal(party, party.approachBlocker, refusalHopeless,
                                                 refusalWaitMs, refusalWhy)));
            // No verdict at all (GetRefusal false: unknown) while the tank sees the target: held up to
            // GUARANTEE_UNKNOWN_HOLD_MS before the careful-refused skip below, the planner may still take it.
            if (ROUTE_CAREFUL_GUARANTEE_HOLD && !ledNoForce && !plannerRefused && !party.approach &&
                !party.targetIsBoss && !atEventHold && !party.content->raid && party.content->size <= 5 &&
                waited > pullWaitMs && waited < GUARANTEE_UNKNOWN_HOLD_MS && !holdForce)
                if (Unit* unit = object->ToUnit();
                    unit && PartyTankView(party, leader, unit).rfind("accepted", 0) == 0)
                {
                    if (party.guaranteeTarget != party.target || !party.guaranteeHolding)
                    {
                        party.guaranteeTarget = party.target;
                        party.guaranteeDueMs = 0;
                        party.guaranteeHolding = true;
                        LogEvent(party, "guarantee_hold", JsonObject().Str("name", object->GetName())
                                                              .Num("entry", object->GetEntry())
                                                              .Num("waitSec", 0)
                                                              .Num("waitedSec", waited / IN_MILLISECONDS)
                                                              .Str("why", "no verdict (unknown)"));
                    }
                    return;
                }
            // "held for now" (the planner's honest pause: the group rests / tops up, a boss-ready hold): the group
            // waits and the wait does not count - the arrival clock restarts, no guarantee skip timer runs.
            // [held_for_now_cap] ... unless the group stands fully ready for HELD_FOR_NOW_MAX_MS and the planner still
            // holds the same target: then no rest can change it, and it is a refusal - the guarantee branch below
            // decides at once (its skip / route blocker / body pull rules). Wave 23 (build 24): gd-h, nex-h and
            // scholo-h held "fallback plan costs 0.9x tank hp" / "overlap wait ... (0 s)" with the group at 100 %
            // until the 4-minute stall.
            bool heldForNowCapped = false;
            // [lead3] LEADER_NO_BODY_FALLBACK: the leader never turns its planner's hold into a walk-in; a hold no rest
            // can change is its planner_no_option report (TickLeaderPlanner, LEADER_HELD_READY_MAX_MS)
            bool const leaderPulls = Leader::Leads(Leader::LEADER_NO_BODY_FALLBACK) && LeaderDecidesFor(party);
            // [held_for_now_cap] a route blocker keeps its clock: its walk-in comes after ROUTE_BLOCKER_PREP_MS
            // whatever the planner holds (nex-h wave 23: the Horde Ranger route blocker held "fallback plan costs
            // 0.92" from 593 s to the stall, the walk-in never came)
            if (leaderPulls)
                heldForNowCapped = false;
            else if (routeBlocker && waited >= ROUTE_BLOCKER_PREP_MS)
                heldForNowCapped = true;
            // [pull_after_hold] a ready group's "held for now" (not a patrol / overlap wait) is a refusal now
            else if (!leaderPulls && holdForce && plannerRefused && !refusalHopeless &&
                     refusalWhy.rfind("held for now", 0) == 0 &&
                     refusalWhy.find("patrol wait") == std::string::npos &&
                     refusalWhy.find("overlap wait") == std::string::npos)
            {
                heldForNowCapped = true;
                party.heldForNowSinceMs = 0;
                party.arrivedMs = now - GUARANTEE_HOLD_TOTAL_MS;
                waited = GUARANTEE_HOLD_TOTAL_MS;
                party.guaranteeTarget = party.target;
                party.guaranteeDueMs = 0;
                party.guaranteeHolding = false;
            }
            else if (!leaderPulls && plannerRefused && !refusalHopeless && refusalWhy.rfind("held for now", 0) == 0)
            {
                if (party.heldForNowTarget != party.target || !party.heldForNowSinceMs)
                {
                    party.heldForNowTarget = party.target;
                    party.heldForNowSinceMs = now ? now : 1;
                }
                if (!GroupFullyReady(party, leader))
                    party.heldForNowSinceMs = now ? now : 1;
                else if (getMSTimeDiff(party.heldForNowSinceMs, now) >= HELD_FOR_NOW_MAX_MS)
                {
                    heldForNowCapped = true;
                    LOG_INFO("playerbots", "[held_for_now_cap] {}: {} held {} s with the group fully ready ({}): "
                             "taken as a refusal", leader->GetName(), object->GetName(),
                             getMSTimeDiff(party.heldForNowSinceMs, now) / IN_MILLISECONDS, refusalWhy);
                    LogEvent(party, "held_for_now_cap",
                             JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                                 .Num("heldSec", getMSTimeDiff(party.heldForNowSinceMs, now) / IN_MILLISECONDS)
                                 .Str("why", refusalWhy));
                    party.heldForNowSinceMs = 0;
                    // the guarantee branch reads the hold as waited out
                    party.arrivedMs = now - GUARANTEE_HOLD_TOTAL_MS;
                    waited = GUARANTEE_HOLD_TOTAL_MS;
                    party.guaranteeTarget = party.target;
                    party.guaranteeDueMs = 0;
                    party.guaranteeHolding = false;
                }
            }
            if (!heldForNowCapped && plannerRefused && !refusalHopeless && refusalWhy.rfind("held for now", 0) == 0)
            {
                // [patrol_wait_spot] a patrol wait: the group waits out of every idle pack's reach (the patrol fight
                // happens where the group stands).
                if (!LeaderOwnsPatrols(party) &&  // [lead4] the leader's patrol decision owns the wait's spot
                    refusalWhy.find("patrol wait") != std::string::npos &&
                    (party.patrolWaitTarget != party.target || !party.patrolWaitMs ||
                     getMSTimeDiff(party.patrolWaitMs, now) > PATROL_WAIT_HOLD_MS))
                {
                    party.patrolWaitMs = now ? now : 1;
                    party.patrolWaitWhy = refusalWhy;
                    party.patrolWaitTarget = party.target;
                    G3D::Vector3 spot;
                    Creature* nearMob = nullptr;
                    if (PatrolWaitSpot(leader, spot, nearMob))
                    {
                        party.patrolWaitUntilMs = now + PATROL_WAIT_HOLD_MS;
                        SetLeaderWaypoint(party, leader, spot.x, spot.y, spot.z);
                        LogEvent(party, "patrol_wait_spot",
                                 JsonObject().Str("why", refusalWhy).Str("idleMob", nearMob->GetName())
                                     .Num("idleEntry", nearMob->GetEntry())
                                     .Real("idleDistance", leader->GetExactDist(nearMob)).Real("x", spot.x)
                                     .Real("y", spot.y).Real("z", spot.z));
                    }
                    else if (nearMob)
                        LogEvent(party, "patrol_wait_spot",
                                 JsonObject().Str("why", refusalWhy).Str("idleMob", nearMob->GetName())
                                     .Num("idleEntry", nearMob->GetEntry()).Str("state", "no clear spot"));
                }
                if (!party.guaranteeHolding || party.guaranteeTarget != party.target)
                {
                    party.guaranteeTarget = party.target;
                    party.guaranteeDueMs = 0;
                    party.guaranteeHolding = true;
                    LogEvent(party, "guarantee_hold", JsonObject().Str("name", object->GetName())
                                                          .Num("entry", object->GetEntry()).Num("waitSec", 0)
                                                          .Num("waitedSec", waited / IN_MILLISECONDS)
                                                          .Str("why", refusalWhy));
                }
                if (!routeBlocker)
                {
                    party.arrivedMs = now;
                    party.stopSinceMs = now;
                }
                return;
            }
            if (plannerRefused && !refusalHopeless)
            {
                if (party.guaranteeTarget != party.target)
                {
                    party.guaranteeTarget = party.target;
                    party.guaranteeDueMs = 0;
                    party.guaranteeHolding = false;
                }
                if (!refusalWaitMs && !party.guaranteeDueMs)
                    party.guaranteeDueMs = now;
                // No remaining time and no reason from the planner (wave 11: waitSec 0, why "" while its guarantee
                // needs ~90 s of block time; [pull guarantee] fired 0 times): held up to GUARANTEE_UNKNOWN_HOLD_MS.
                bool const unknown = !refusalWaitMs && refusalWhy.empty();
                bool const hold = !holdForce && waited < GUARANTEE_HOLD_TOTAL_MS &&
                                  !(routeBlocker && waited >= ROUTE_BLOCKER_PREP_MS) &&
                                  (refusalWaitMs || (unknown && waited < GUARANTEE_UNKNOWN_HOLD_MS) ||
                                   getMSTimeDiff(party.guaranteeDueMs, now) < GUARANTEE_EXECUTE_MS);
                if (hold)
                {
                    if (!party.guaranteeHolding)
                    {
                        party.guaranteeHolding = true;
                        LogEvent(party, "guarantee_hold", JsonObject().Str("name", object->GetName())
                                                              .Num("entry", object->GetEntry())
                                                              .Num("waitSec", refusalWaitMs / IN_MILLISECONDS)
                                                              .Num("waitedSec", waited / IN_MILLISECONDS)
                                                              .Str("why", refusalWhy));
                        LOG_INFO("playerbots", "[approach] {}: holding at {} for the planner's guarantee ({} s, {})",
                                 leader->GetName(), object->GetName(), refusalWaitMs / IN_MILLISECONDS, refusalWhy);
                    }
                    return;
                }
            }
            // [hleft-never-pulled] no body pull of an encounter creature no tank pulls (AggressiveTargetValue's
            // list): the Halls of Reflection Lich King, Hadronox before she webbed the doors.
            if (Creature const* held = object->ToCreature();
                held && (held->GetEntry() == HOR_LICH_KING_ENTRY || held->GetEntry() == HOR_LICH_KING_ESCAPE_ENTRY ||
                         (held->GetEntry() == AN_HADRONOX_ENTRY && GET_PLAYERBOT_AI(leader) &&
                          ANAdds::HadronoxHandsOff(GET_PLAYERBOT_AI(leader)))))
            {
                party.arrivedMs = now;
                party.stopSinceMs = now;
                return;
            }
            // [lead3] a boss pull the leader issued that brought no fight: "not pulled", as the old walk-in's was
            if (leaderPulls && party.targetIsBoss && party.lead.bossPullIssued == party.target &&
                party.lead.bossPullIssuedMs && getMSTimeDiff(party.lead.bossPullIssuedMs, now) > BOSS_PULL_HOLD_WAIT_MS)
            {
                party.lead.bossPullIssued.Clear();
                party.lead.bossPullIssuedMs = 0;
                SkipTarget(party, party.target, ROUTE_OCULUS_DRAKE_ONLY ? BOSS_NOT_PULLED_SKIP_MS : NOT_PULLED_SKIP_MS,
                           "not pulled (the leader's boss pull brought no fight)");
                return;
            }
            // [lead6] the leader's one readiness verdict, not the body pull's own 80 % lines
            bool const pullReady = leaderPulls && Leader::Leads(Leader::LEADER_ONE_READINESS)
                                       ? party.lead.ready
                                       : GroupFullyReady(party, leader);
            // [lead24] LEADER_NO_FORCING_TIMERS: a led party's stop resolves on evaluations only (a boss pull now, a
            // hopeless refusal); the stop's wait / the route blocker's prep time only log waited_long
            bool const timedOut = waited > pullWaitMs || (routeBlocker && waited > ROUTE_BLOCKER_PREP_MS) || holdForce;
            if (ledNoForce && timedOut && !bossPullNow && !(plannerRefused && refusalHopeless))
                WaitedLong(party, "stop_wait", waited, object->GetName() + (plannerRefused ? ": " + refusalWhy : ""));
            if (!party.approach &&
                ((!ledNoForce && timedOut) || bossPullNow || (plannerRefused && refusalHopeless)) &&
                pullReady)
            {
                // [b33_mech2] BossAreaBodyPullHold: no body pull of a boss while killable trash of his area stands
                // (BossAreaTrashOf: what joins his fight, his room, BOSS_AREA_EXTRA) - it becomes the target first
                // (unskipped, kept as boss_neighbour_target is). Honest exceptions are left: fast respawners
                // (FastRespawner), a reachability skip (ReachabilitySkip: unreachable, closed, respawn, "boss area:
                // given up"), what the leader cannot attack; and after BOSS_AREA_BODY_PULL_HOLD_MS from the first
                // hold the pull goes anyway. mech-n 20261026-000434-mech-n-5D-17: Sepethrea body-pulled (immediate,
                // 1180 s) with the listed Tempest-Forge Destroyer 138862 alive south of her room; it killed the tank.
                if (ROUTE_BOSS_AREA_CLEAR && party.targetIsBoss)
                    if (Creature* areaBoss = object->ToCreature(); areaBoss && areaBoss->IsAlive())
                    {
                        Creature* areaMob = nullptr;
                        char const* areaWhy = nullptr;
                        uint32 areaLeft = 0;
                        for (auto const& [spawnId, mob] : map->GetCreatureBySpawnIdStore())
                        {
                            if (!mob || !mob->IsInWorld() || !mob->IsAlive() || mob->IsInCombat() || mob == areaBoss ||
                                mob->GetExactDist(areaBoss) > BOSS_AREA_SCAN_RANGE || FastRespawner(mob) ||
                                mob->IsImmuneToPC() || mob->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) ||
                                mob->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) || !leader->IsValidAttackTarget(mob) ||
                                !InThisWing(party, mob))
                                continue;
                            char const* const why = BossAreaTrashOf(leader, areaBoss, mob,
                                                                    party.bossCreatures.count(mob->GetGUID()) != 0);
                            if (!why)
                                continue;
                            if (party.skipUntil.count(mob->GetGUID()))
                                if (auto const reason = party.skipReason.find(mob->GetGUID());
                                    reason != party.skipReason.end() && ReachabilitySkip(reason->second))
                                    continue;
                            ++areaLeft;
                            if (!areaMob || leader->GetExactDist(mob) < leader->GetExactDist(areaMob))
                            {
                                areaMob = mob;
                                areaWhy = why;
                            }
                        }
                        std::pair<uint32, ObjectGuid> const holdKey{party.seq, areaBoss->GetGUID()};
                        if (!areaMob)
                            bossAreaBodyPullHoldMs.erase(holdKey);
                        else
                        {
                            uint32& firstHoldMs = bossAreaBodyPullHoldMs[holdKey];
                            if (!firstHoldMs)
                                firstHoldMs = now ? now : 1;
                            uint32 const heldMs = getMSTimeDiff(firstHoldMs, now);
                            if (heldMs < BOSS_AREA_BODY_PULL_HOLD_MS)
                            {
                                LogEvent(party, "boss_area_clear",
                                         JsonObject().Str("boss", areaBoss->GetName())
                                             .Num("bossEntry", areaBoss->GetEntry()).Str("action", "body pull held")
                                             .Str("mob", areaMob->GetName()).Num("entry", areaMob->GetEntry())
                                             .Num("spawnId", uint32(areaMob->GetSpawnId())).Str("why", areaWhy)
                                             .Num("remaining", areaLeft).Num("heldSec", heldMs / IN_MILLISECONDS)
                                             .Bool("immediate", bossPullNow).Real("x", areaMob->GetPositionX())
                                             .Real("y", areaMob->GetPositionY()).Real("z", areaMob->GetPositionZ()));
                                party.skipUntil.erase(areaMob->GetGUID());
                                party.skipReason.erase(areaMob->GetGUID());
                                party.neighbourTarget = areaMob->GetGUID();
                                party.neighbourTargetUntilMs = now + NEIGHBOUR_TARGET_MS;
                                party.target = areaMob->GetGUID();
                                party.targetIsBoss = false;
                                party.targetIsStarter = false;
                                party.targetRouteRule = areaWhy;
                                party.targetSinceMs = now;
                                party.guaranteeHolding = false;
                                party.arrivedMs = 0;
                                party.approach = 0;
                                party.approachBlocker.Clear();
                                party.stopSinceMs = 0;
                                TestRunner::ClearWaypoint(party.leader);
                                return;
                            }
                            LogEvent(party, "boss_area_clear",
                                     JsonObject().Str("boss", areaBoss->GetName())
                                         .Num("bossEntry", areaBoss->GetEntry()).Str("action", "body pull: given up")
                                         .Str("mob", areaMob->GetName()).Num("entry", areaMob->GetEntry())
                                         .Num("remaining", areaLeft).Num("heldSec", heldMs / IN_MILLISECONDS));
                        }
                    }
                // What the tank (the leader) sees of the target and of the mob that stopped the walk: the refusal
                // below was assumed from the wait alone (wave 13: SM Armory's Guardsman was "careful pull refused"
                // while it was not even in the tank's possible targets).
                std::string tankView;
                bool targetUnseen = false;
                if (Unit* unit = object->ToUnit())
                {
                    std::string const targetView = PartyTankView(party, leader, unit);
                    // [b36_scholo] "accepted (not in the cached possible targets)" is seen: in sight and accepted,
                    // only the tank's cached list is stale. scholo-n 20261026-034959-scholo-n-5H-4: the entrance
                    // Adepts at 22 yd were skipped as "tank does not see it" and their pack marked unseen.
                    bool const accepted = targetView.rfind("accepted", 0) == 0;
                    targetUnseen = !accepted;
                    Creature* stopper = map->GetCreature(party.approachBlocker);
                    std::string const blockerView =
                        stopper ? PartyTankView(party, leader, stopper) : std::string();
                    LogEvent(party, "tank_view", JsonObject().Str("name", unit->GetName())
                                                     .Num("entry", unit->GetEntry())
                                                     .Real("distance", leader->GetExactDist(unit))
                                                     .Str("target", targetView)
                                                     .Bool("nonAttackable", unit->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE))
                                                     .Bool("notSelectable", unit->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
                                                     .Bool("immuneToPc", unit->IsImmuneToPC())
                                                     .Bool("validTarget", leader->IsValidAttackTarget(unit))
                                                     .Str("blocker", stopper ? stopper->GetName() : "")
                                                     .Real("blockerDistance",
                                                           stopper ? leader->GetExactDist(stopper) : 0.0f)
                                                     .Str("blockerView", blockerView));
                    LOG_INFO("playerbots", "[tank view] {}: {} {:.1f} yd - {}{}{} (flags{}{}{}, {})",
                             leader->GetName(),
                             unit->GetName(), leader->GetExactDist(unit), targetView, stopper ? "; blocker " : "",
                             stopper ? stopper->GetName() + " - " + blockerView : std::string(),
                             unit->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) ? " non-attackable" : "",
                             unit->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) ? " not-selectable" : "",
                             unit->IsImmuneToPC() ? " immune-to-pc" : "",
                             leader->IsValidAttackTarget(unit) ? "valid target" : "no valid target");
                    if (!accepted && (!stopper || blockerView.rfind("accepted", 0) != 0))  // [b36_scholo]
                        tankView = targetView;
                }
                // The boss's walk stopped at a neighbour: no body pull through it. What of its pack the tank does not
                // see is marked unseen (the next walk passes it, as the trash refusal below does); a pack it sees
                // holds the group here - the planner pulls it, or the hold shows its refusal (user rule).
                // zf-h build 17: Gahz'rilla's walk stopped at a Sandfury Blood Drinker the planner never pulled, the
                // hold came every 45 s (3 times) until the stall skip. After NEIGHBOUR_HOLD_MAX holds a small
                // neighbour pack (NEIGHBOUR_PASS_PACK_MAX) no longer stops the boss's body pull; a bigger one stays.
                std::list<Creature*> neighbourPack;
                if (neighbourStop)
                {
                    PackMobCheck check{leader, neighbour};
                    Acore::CreatureListSearcher<PackMobCheck> searcher(neighbour, neighbourPack, check);
                    Cell::VisitObjects(neighbour, searcher, PACK_RADIUS);
                    if (std::find(neighbourPack.begin(), neighbourPack.end(), neighbour) == neighbourPack.end())
                        neighbourPack.push_back(neighbour);
                    // [route_blocker_boss_neighbour] a neighbour pack on the boss's way is the target now (pulled by
                    // sight, else walked into once the group is ready), not after NEIGHBOUR_HOLD_MAX holds. sv-n
                    // wave 23 (112011, 1097-1172 s): the walk to Warlord Kalithresh stopped at a Coilfang Siren the
                    // tank accepted 21.7 yd off, nothing made it the target until the stop.
                    if (TargetRouteBlocker(party, leader, neighbour, object, "stopped the walk to the boss"))
                        return;
                    if (party.neighbourHoldBoss != party.target)
                    {
                        party.neighbourHoldBoss = party.target;
                        party.neighbourHolds = 0;
                    }
                    if (party.neighbourHolds >= NEIGHBOUR_HOLD_MAX && neighbourPack.size() <= NEIGHBOUR_PASS_PACK_MAX)
                    {
                        LogEvent(party, "boss_neighbour_pass", JsonObject().Str("boss", object->GetName())
                                                                   .Str("neighbour", neighbour->GetName())
                                                                   .Num("packMobs", uint32(neighbourPack.size()))
                                                                   .Num("holds", party.neighbourHolds));
                        neighbourStop = false;
                    }
                    else if (party.neighbourHolds >= NEIGHBOUR_HOLD_MAX && !party.skipUntil.count(neighbour->GetGUID()))
                    {
                        // A bigger neighbour pack (dme-h build 17: Zevrim held 119 s by the Wildspawn pack): it
                        // becomes the route target, the careful pull / guarantee hold decides on it, hopeless or
                        // refused it is skipped as any trash; the boss comes back after it.
                        LogEvent(party, "boss_neighbour_target", JsonObject().Str("boss", object->GetName())
                                                                     .Str("neighbour", neighbour->GetName())
                                                                     .Num("entry", neighbour->GetEntry())
                                                                     .Num("packMobs", uint32(neighbourPack.size()))
                                                                     .Num("holds", party.neighbourHolds));
                        party.neighbourHolds = 0;
                        party.neighbourTarget = neighbour->GetGUID();
                        party.neighbourTargetUntilMs = now + NEIGHBOUR_TARGET_MS;
                        party.target = neighbour->GetGUID();
                        party.targetIsBoss = false;
                        party.targetIsStarter = false;
                        party.targetRouteRule.clear();
                        party.targetSinceMs = now;
                        party.guaranteeHolding = false;
                        party.arrivedMs = 0;
                        party.approach = 0;
                        party.approachBlocker.Clear();
                        party.stopSinceMs = 0;
                        return;
                    }
                    else
                        ++party.neighbourHolds;
                }
                if (neighbourStop)
                {
                    std::list<Creature*>& pack = neighbourPack;
                    G3D::Vector3 const tankAt(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
                    for (Creature* packMob : pack)
                        if (!packMob->IsDungeonBoss() && !party.bossCreatures.count(packMob->GetGUID()) &&
                            (std::fabs(packMob->GetPositionZ() - tankAt.z) > APPROACH_OTHER_FLOOR_DZ ||
                             !SeesFromPoint(leader, packMob, tankAt)))
                            TankRejects::MarkUnseen(leader->GetGUID(), packMob->GetGUID());
                    LogHarnessHold(party, leader, neighbour, "boss approach", tankAt.x, tankAt.y, tankAt.z);
                    party.approachBlocker.Clear();
                    party.stopSinceMs = 0;
                    party.arrivedMs = 0;
                    return;
                }
                // Careful pulls (5-man, ChooseTargetActions.cpp; heroics and level 80 too since the careful-pull pass):
                // the tank refused this pack as too big for the group, a body pull would bring all of it. The route
                // goes on elsewhere and comes back later, when its neighbours are dead or the bot finds an edge mob to
                // pull. Levelling normals always defer; heroic / level 80 at most twice per pack, the third time the
                // group body-pulls as before, so the route never loops on a pack that cannot be split. Bosses and
                // raids keep the body pull.
                bool const levelling = IsNormalDungeon(*party.content) && party.level < MAX_TEST_LEVEL;
                // A boss's own formation (Blood Furnace: the Shadowmoon Channelers round Keli'dan) is his encounter:
                // it never splits, and the tank's boss-readiness hold kept all five "careful pull refused" for 3
                // minutes (2026-10-25, a regression of the planner's boss gate). It is body-pulled as the boss is.
                bool bossFormation = false;
                if (Creature* refused = object->ToCreature(); refused && refused->GetFormation())
                    for (ObjectGuid const& bossGuid : party.bossCreatures)
                        if (Creature* boss = map->GetCreature(bossGuid);
                            boss && boss->IsAlive() && boss->GetFormation() == refused->GetFormation())
                            bossFormation = true;
                // [dmw-guard-body-pull] A pending guard of a closed guarded door (Dire Maul West's pylon guards, Sunken
                // Temple's defenders) is the door's objective: refused GUARD_REFUSALS_BEFORE_BODY_PULL times, it is
                // body-pulled as a boss formation is (dmw-n 062726: guarantee holds and "careful pull refused" on the
                // ledge guards until the stop, Immol'thar 0/1). User decision 2026-10-25: approved.
                bool guardBodyPull = false;
                if (Creature* refused = object->ToCreature(); refused && IsPendingDoorGuard(map, refused))
                {
                    uint32& refusalsOfGuard = guardRefusals[{party.seq, party.target}];
                    guardBodyPull = ++refusalsOfGuard > GUARD_REFUSALS_BEFORE_BODY_PULL;
                    if (guardBodyPull)
                        LogEvent(party, "guard_body_pull", JsonObject().Str("name", refused->GetName())
                                                               .Num("entry", refused->GetEntry())
                                                               .Num("refusals", refusalsOfGuard));
                }
                // [route_blocker_body_pull] the neighbour pack that stopped the walk to this target is on the way: it
                // becomes the target instead of being skipped with it; the route blocker itself is walked into.
                if (Creature* onWay = map->GetCreature(party.approachBlocker);
                    !routeBlocker && onWay && object->ToCreature() &&
                    onWay->GetExactDist(object) > PACK_RADIUS &&
                    TargetRouteBlocker(party, leader, onWay, object, "stopped the walk to a refused target"))
                    return;
                // [pull_after_hold] the ready group's hold at a pack of the required route ends in its pull
                if (holdForce && !routeBlocker && !party.targetIsBoss && object->ToCreature())
                    if (std::string requiredWhy; RouteBlockerBehindRequired(party, map, object, requiredWhy))
                    {
                        RouteBlockerBodyPull(party, leader, object->ToCreature(),
                                             "pull_after_hold (" + requiredWhy + ")");
                        return;
                    }
                // [lead3] LEADER_NO_BODY_FALLBACK: no careful-refused skip of a pack the tank sees. A hopeless pack the
                // way on does not need keeps the old skip (a route decision); any other is asked of the planner whole
                // (walked in when the leader must clear it), and its answer is the leader's (TickLeaderPlanner: the
                // plan, or planner_no_option: a stop for a pack it must clear, a skip for one it need not).
                if (leaderPulls && !party.targetIsBoss && !atEventHold && !bossFormation && !guardBodyPull &&
                    !routeBlocker && tankView.empty() && object->ToCreature())
                {
                    std::string needWhy;
                    bool const needs = LeaderNeedsPack(party, map, object, needWhy);
                    if (needs || !(plannerRefused && refusalHopeless))
                    {
                        LeaderWholePack(party, leader, object->ToCreature(),
                                        Acore::StringFormat("the stop's wait ({} s{}{})", waited / IN_MILLISECONDS,
                                                            plannerRefused ? ": " + refusalWhy : std::string(),
                                                            needs ? ", " + needWhy : std::string()),
                                        needs, needs);
                        return;
                    }
                }
                if (!party.content->raid && party.content->size <= 5 && !party.targetIsBoss && !atEventHold &&
                    !bossFormation && !guardBodyPull && !routeBlocker &&
                    (levelling || ++party.carefulRefused[party.target] > 0))
                {
                    // User rule (bots act on their own): the harness never walks the group into a pack the tank
                    // refused; a pack that is never split is a bot gap and shows as a stalled run.
                    if (party.carefulRefused[party.target] == 3)
                        LogEvent(party, "pack_too_big", JsonObject().Num("entry", object->GetEntry())
                                                            .Num("refusals", party.carefulRefused[party.target]));
                    uint32 const refusals = party.carefulRefused[party.target];
                    // [b43_harn] A target the planner never evaluated (no fresh verdict on its pack nor on the stop's
                    // neighbour) is no "careful pull refused": scholo-h 20261026-054314-scholo-h-5A-3 skipped the lone
                    // Spectral Researcher so while every refusal was of the nearer Neophyte chain, the planner's pick.
                    std::string neverPlanned;
                    {
                        bool verdictAccepted = false;
                        uint32 verdictMobs = 0;
                        bool const anyVerdict =
                            PartyVerdict(party, party.target, verdictAccepted, verdictMobs) ||
                            (!party.approachBlocker.IsEmpty() &&
                             PartyVerdict(party, party.approachBlocker, verdictAccepted,
                                                     verdictMobs));
                        if (!plannerRefused && !anyVerdict)
                        {
                            // The nearer pack the planner refused instead (its pick is the nearest candidate).
                            float const targetDist = leader->GetExactDist(object);
                            std::list<Creature*> nearer;
                            IdleAggroCheck nearerCheck{leader};
                            Acore::CreatureListSearcher<IdleAggroCheck> nearerSearcher(leader, nearer, nearerCheck);
                            Cell::VisitObjects(leader, nearerSearcher, targetDist);
                            Creature* refusedNearer = nullptr;
                            for (Creature* mob : nearer)
                            {
                                bool nearHopeless = false;
                                uint32 nearWaitMs = 0;
                                std::string nearWhy;
                                if (mob->GetGUID() != party.target && leader->GetExactDist(mob) < targetDist &&
                                    PartyRefusal(party, mob->GetGUID(), nearHopeless,
                                                                   nearWaitMs, nearWhy) &&
                                    (!refusedNearer ||
                                     leader->GetExactDist(mob) < leader->GetExactDist(refusedNearer)))
                                    refusedNearer = mob;
                            }
                            if (refusedNearer)
                                neverPlanned = Acore::StringFormat(
                                    "planner never planned it (nearer refused pack: {} {:.1f} yd)",
                                    refusedNearer->GetName(), leader->GetExactDist(refusedNearer));
                            else
                                neverPlanned = "planner never planned it (no verdict)";
                        }
                    }
                    std::string const packSkipReason =
                        neverPlanned.empty() ? std::string("careful pull refused (pack)")
                                             : std::string("planner never planned it (pack)");
                    // The tank does not take the target: a mob of its pack or of the blocker's that the tank cannot
                    // see from where it stands (no line of sight from its eyes) or that stands on another floor is
                    // marked as not seen (TankRejects TTL) and stops no later walk (RFC wave 37: the same unseen
                    // trogg stopped the next approach after every skip). A mob the tank sees on its floor is not
                    // marked: it still stops the walk, the tank pulls it first. Bosses are never marked.
                    // A pack approach to sight held the leader at the edge of (ROUTE_SIGHT_KEEPS_OUT_OF_REFUSED) is
                    // unseen only because the leader kept out of its reach: it is not marked, it keeps stopping the
                    // walks (marked, the next route would walk through it).
                    std::vector<ObjectGuid> heldPack;
                    if (auto const hold = sightHolds.find(party.leader);
                        hold != sightHolds.end() && hold->second.target == party.target)
                        heldPack = hold->second.pack;
                    auto const isHeld = [&heldPack](Creature* mob)
                    { return std::find(heldPack.begin(), heldPack.end(), mob->GetGUID()) != heldPack.end(); };
                    G3D::Vector3 const tankAt(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
                    auto const markIfUnseen = [&party, leader, &tankAt, targetUnseen, &isHeld](Creature* mob)
                    {
                        if (!targetUnseen || party.bossCreatures.count(mob->GetGUID()) || mob->IsDungeonBoss() ||
                            isHeld(mob))
                            return;
                        if (std::fabs(mob->GetPositionZ() - tankAt.z) > APPROACH_OTHER_FLOOR_DZ ||
                            !SeesFromPoint(leader, mob, tankAt))
                            TankRejects::MarkUnseen(leader->GetGUID(), mob->GetGUID());
                    };
                    // The whole pack, not only the mob the route chose: Shadow Labyrinth / Mechanar wave 9 refused a
                    // pack, then chose its next member, waited 45 s again, and the stall detector fired.
                    uint32 packSkipped = 0;
                    if (Creature* refused = object->ToCreature())
                    {
                        std::list<Creature*> pack;
                        PackMobCheck check{leader, refused};
                        Acore::CreatureListSearcher<PackMobCheck> searcher(refused, pack, check);
                        Cell::VisitObjects(refused, searcher, PACK_RADIUS);
                        if (std::find(pack.begin(), pack.end(), refused) == pack.end())
                            pack.push_back(refused);
                        for (Creature* mob : pack)
                        {
                            markIfUnseen(mob);
                            if (mob != refused && !party.bossCreatures.count(mob->GetGUID()) &&
                                !mob->IsDungeonBoss() && BossOrder(party, mob) < 0)
                            {
                                party.skipUntil[mob->GetGUID()] = now + CAREFUL_REFUSED_SKIP_MS;
                                party.skipReason[mob->GetGUID()] = packSkipReason;
                                party.unreachableFrom.erase(mob->GetGUID());
                                // The pack's count, whichever member the route picks next time.
                                uint32& count = party.carefulRefused[mob->GetGUID()];
                                count = std::max(count, refusals);
                                ++packSkipped;
                            }
                        }
                    }
                    // The neighbour pack that stopped the approach on the way (ApproachStop) is the one in front of
                    // the tank: it is skipped with the target, or the next route stops at it again.
                    if (Creature* blocker = map->GetCreature(party.approachBlocker);
                        blocker && blocker->IsAlive() && !blocker->IsInCombat() &&
                        !party.bossCreatures.count(blocker->GetGUID()) && !blocker->IsDungeonBoss() &&
                        BossOrder(party, blocker) < 0)
                    {
                        std::list<Creature*> pack;
                        PackMobCheck check{leader, blocker};
                        Acore::CreatureListSearcher<PackMobCheck> searcher(blocker, pack, check);
                        Cell::VisitObjects(blocker, searcher, PACK_RADIUS);
                        if (std::find(pack.begin(), pack.end(), blocker) == pack.end())
                            pack.push_back(blocker);
                        for (Creature* mob : pack)
                        {
                            // A blocker the tank does not see stops no walk any more (SM Library wave 32).
                            if (!tankView.empty() && !isHeld(mob))
                                TankRejects::MarkUnseen(leader->GetGUID(), mob->GetGUID());
                            else
                                markIfUnseen(mob);
                            if (!party.bossCreatures.count(mob->GetGUID()) && !mob->IsDungeonBoss() &&
                                BossOrder(party, mob) < 0 && !party.skipUntil.count(mob->GetGUID()))
                            {
                                party.skipUntil[mob->GetGUID()] = now + CAREFUL_REFUSED_SKIP_MS;
                                party.skipReason[mob->GetGUID()] = packSkipReason;
                                party.unreachableFrom.erase(mob->GetGUID());
                                ++packSkipped;
                            }
                        }
                    }
                    party.approachBlocker.Clear();
                    party.stopSinceMs = 0;  // the next pack at this stop gets its own wait
                    std::string reason = tankView.empty() ? std::string("careful pull refused")
                                                          : "not pulled, tank does not see it (" + tankView + ")";
                    if (tankView.empty() && !neverPlanned.empty())
                        reason = neverPlanned;  // [b43_harn]
                    if (plannerRefused && refusalHopeless)
                    {
                        reason = "careful pull hopeless (" + refusalWhy + ")";
                        // [b32_brs] the walks go round the hopeless pack while there is a way (HopelessDetourLeg)
                        NoteHopelessPack(leader, object->ToCreature());
                    }
                    else if (plannerRefused && party.guaranteeHolding)
                        reason = Acore::StringFormat("careful pull refused, guarantee not taken in {} s ({})",
                                                     waited / IN_MILLISECONDS, refusalWhy);
                    party.guaranteeHolding = false;
                    if (packSkipped)
                        reason += " (+" + std::to_string(packSkipped) + " pack)";
                    SkipTarget(party, party.target, CAREFUL_REFUSED_SKIP_MS, reason);
                    return;
                }

                if (routeBlocker)
                {
                    if (Creature* blockerPack = object->ToCreature())
                        RouteBlockerBodyPull(party, leader, blockerPack,
                                             plannerRefused ? "planner refused (" + refusalWhy + ")"
                                                            : std::string("not pulled from the stop"));
                    return;
                }
                Creature* pack = object->ToCreature();
                // [lead3] LEADER_NO_BODY_FALLBACK: the leader's pull of what the harness body-pulled - a boss, his
                // formation: the leader's BossPull order; a door guard, an event pack, a trash pack the stop's wait
                // ended at: asked of its planner whole, walked in (priced by its count). The boss-on-the-way rule
                // ([b51_dme]) still makes a boss the pack brings the next pull first.
                if (leaderPulls && pack)
                {
                    for (ObjectGuid const& guid : party.bossCreatures)
                    {
                        Creature* boss = map->GetCreature(guid);
                        if (!party.targetIsBoss && !pack->IsDungeonBoss() && boss && boss->IsAlive() &&
                            !boss->IsInCombat() && boss->GetGUID() != party.target &&
                            BossBroughtBy(leader, boss, pack) &&
                            !ClassGatedBoss(party.content->mapId, party.content->difficulty, boss->GetEntry()))
                        {
                            LogEvent(party, "boss_on_the_way_first",
                                     JsonObject().Str("first", boss->GetName()).Num("firstEntry", boss->GetEntry())
                                         .Real("firstDistance", leader->GetExactDist(boss))
                                         .Str("via", pack->GetName()).Num("viaEntry", pack->GetEntry())
                                         .Str("why", "[lead3] the pack on the way brings this boss: he is pulled "
                                                     "first"));
                            party.target = boss->GetGUID();
                            party.targetIsBoss = true;
                            party.targetIsStarter = false;
                            party.targetRouteRule = "boss on the way";
                            party.targetSinceMs = getMSTime();
                            party.arrivedMs = 0;
                            party.stopSinceMs = 0;
                            party.approachBlocker.Clear();
                            party.orderHoldSinceMs.erase(boss->GetGUID());
                            TestRunner::ClearWaypoint(party.leader);
                            return;
                        }
                    }
                    if (party.targetIsBoss || pack->IsDungeonBoss() || bossFormation)
                        LeaderBossPull(party, leader, pack,
                                       Acore::StringFormat("{}{} (waited {} s)", bossPullNow ? "ready" : "the wait",
                                                           bossFormation ? ", his formation" : "",
                                                           waited / IN_MILLISECONDS));
                    else
                        LeaderWholePack(party, leader, pack,
                                        Acore::StringFormat("{} (waited {} s)",
                                                            guardBodyPull ? "a pending door guard"
                                                            : atEventHold ? "the event's pack"
                                                            : holdForce   ? "the ready group's hold"
                                                                          : "the stop's wait",
                                                            waited / IN_MILLISECONDS),
                                        true, true);
                    return;
                }
                // [body_pull_via_planner] a trash pack: the planner first
                if (BODY_PULL_VIA_PLANNER && pack && !party.targetIsBoss && !pack->IsDungeonBoss())
                {
                    bool askBegan = false;
                    if (AskPlannerFirst(party.leader, pack, now, askBegan))
                    {
                        if (askBegan)
                            LogEvent(party, "body_pull_planner_request",
                                     JsonObject().Str("name", pack->GetName()).Num("entry", pack->GetEntry())
                                         .Str("why", "the stop's wait")
                                         .Num("waitSec", BODY_PULL_PLANNER_MS / IN_MILLISECONDS));
                        TestRunner::ClearWaypoint(party.leader);
                        return;
                    }
                    LogEvent(party, "body_pull_planner_timeout",
                             JsonObject().Str("name", pack->GetName()).Num("entry", pack->GetEntry()));
                }
                // [b33_bodypull] no harness walk-in (a boss, his formation, a door guard, an event's pack) against
                // the planner's readiness refusal (the healer not with the tank, sitters, not topped up), nor with a
                // member sitting: the group holds where it stands, nothing skipped
                {
                    std::string sitter;
                    for (Member const& member : party.members)
                        if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                            bot && bot->IsAlive() && bot->IsSitState())
                            sitter = bot->GetName();
                    if ((plannerRefused && RefusalIsReadiness(refusalWhy)) || !sitter.empty())
                    {
                        if (!party.blockerGateLogMs ||
                            getMSTimeDiff(party.blockerGateLogMs, now) > 10 * IN_MILLISECONDS)
                        {
                            party.blockerGateLogMs = now ? now : 1;
                            LogEvent(party, "blocker_pull_gate",
                                     JsonObject().Str("pull", "harness body pull")
                                         .Str("blocker", pack ? pack->GetName() : std::string("?"))
                                         .Num("blockerEntry", pack ? pack->GetEntry() : 0)
                                         .Str("why", sitter.empty() ? "held: the planner waits for the group (" +
                                                                          refusalWhy + ")"
                                                                    : "the group is not ready: " + sitter + " sits")
                                         .Bool("hold", true));
                        }
                        TestRunner::ClearWaypoint(party.leader);
                        return;
                    }
                }
                // [b49_voone] the same rule as RouteBlockerBodyPull: no walk-in to a trash pack an idle boss's own
                // aggro reach covers (BossBroughtBy) while the planner did not price that pull
                if (pack && !party.targetIsBoss && !pack->IsDungeonBoss() &&
                    !(plannerRefused && refusalWhy.find("(a boss pull: no fallback plan)") != std::string::npos))
                {
                    std::list<Creature*> walkPack;
                    PackMobCheck walkCheck{leader, pack};
                    Acore::CreatureListSearcher<PackMobCheck> walkSearcher(pack, walkPack, walkCheck);
                    Cell::VisitObjects(pack, walkSearcher, PACK_RADIUS);
                    if (std::find(walkPack.begin(), walkPack.end(), pack) == walkPack.end())
                        walkPack.push_back(pack);
                    for (ObjectGuid const& guid : party.bossCreatures)
                    {
                        Creature* boss = leader->GetMap()->GetCreature(guid);
                        if (!boss || !boss->IsAlive() || boss->IsInCombat())
                            continue;
                        auto const brings = [&](Creature* mob) { return BossBroughtBy(leader, boss, mob); };
                        if (std::none_of(walkPack.begin(), walkPack.end(), brings))
                            continue;
                        LOG_INFO("playerbots", "[b49_voone] {}: no harness body pull of {}: it brings {}, the "
                                 "planner did not price it", leader->GetName(), pack->GetName(), boss->GetName());
                        LogEvent(party, "blocker_pull_gate",
                                 JsonObject().Str("pull", "harness body pull").Str("blocker", pack->GetName())
                                     .Num("blockerEntry", pack->GetEntry())
                                     .Str("why", "[b49_voone] brings boss " + boss->GetName() + " (unpriced)")
                                     .Bool("hold", false));
                        // [b51_dme] User rule 2026-10-26 (open the way, no detours): a pack that is the way on and
                        // stands in an idle boss's reach is no skip - the boss is pulled deliberately first, as his own
                        // fight (the boss logic: his buffs, ready check and the planner's boss pull, which prices him
                        // with what he brings). dme-h 20261026-130307-5E-11 stalled 0/16: "body pull refused: brings
                        // boss Hydrospawn" x3, "behind skipped Wildspawn Hellcaller" x8. The Voone intent stays: no
                        // blind walk-in to the pack; a class-gated boss keeps the old skip.
                        if (!ClassGatedBoss(party.content->mapId, party.content->difficulty, boss->GetEntry()) &&
                            boss->GetGUID() != party.target)
                        {
                            LogEvent(party, "boss_on_the_way_first",
                                     JsonObject().Str("first", boss->GetName()).Num("firstEntry", boss->GetEntry())
                                         .Real("firstDistance", leader->GetExactDist(boss)).Str("via", pack->GetName())
                                         .Num("viaEntry", pack->GetEntry())
                                         .Str("why", "[b51_dme] the pack on the way brings this boss: he is "
                                                     "pulled first"));
                            LOG_INFO("playerbots", "[b51_dme] {}: {} brings boss {}: the boss is the next pull (his "
                                     "fight), no skip",
                                     leader->GetName(), pack->GetName(), boss->GetName());
                            party.target = boss->GetGUID();
                            party.targetIsBoss = true;
                            party.targetIsStarter = false;
                            party.targetRouteRule = "boss on the way";
                            party.targetSinceMs = getMSTime();
                            party.approach = 0;
                            party.arrivedMs = 0;
                            party.stopSinceMs = 0;
                            party.approachBlocker.Clear();
                            party.orderHoldSinceMs.erase(boss->GetGUID());
                            TestRunner::ClearWaypoint(party.leader);
                            return;
                        }
                        TestRunner::ClearWaypoint(party.leader);
                        SkipTarget(party, party.target, CAREFUL_REFUSED_SKIP_MS,
                                   "body pull refused: brings boss " + boss->GetName());
                        return;
                    }
                }
                // [b32_bm] A creature that is not REACT_AGGRESSIVE (Aeonus and the Black Morass adds: REACT_DEFENSIVE)
                // never aggroes on a walk-in: the walk only brings the tank beside it, the tank's own attack opens
                // (tank open idle boss / pull / reach melee). Logged as attack_pull, not body_pull.
                bool const attackPull = pack && !pack->HasReactState(REACT_AGGRESSIVE);
                party.approach = 1;  // the pull logic held back: walk into the pack
                party.arrivedMs = 0;
                LogEvent(party, attackPull ? "attack_pull" : "body_pull",
                         JsonObject().Str("name", pack ? pack->GetName() : "?")
                             .Num("entry", pack ? pack->GetEntry() : 0)
                             .Bool("boss", party.targetIsBoss)
                             .Num("carefulRefusals", party.carefulRefused.count(party.target)
                                                         ? party.carefulRefused[party.target]
                                                         : 0)
                             .Num("waitedSec", waited / IN_MILLISECONDS)
                             .Bool("immediate", bossPullNow)
                             .Num("react", pack ? uint32(pack->GetReactState()) : 0));
            }
            // [b32_bm] beside a non-aggressive boss the tank's own attack needs its idle-boss delay (10 s) and its
            // tries: the walk-in's 10 s were gone before it (the boss hold's wait instead).
            else if (party.approach &&
                     waited > (party.targetIsBoss && object->ToCreature() &&
                                       !object->ToCreature()->HasReactState(REACT_AGGRESSIVE)
                                   ? BOSS_PULL_HOLD_WAIT_MS
                                   : BODY_PULL_WAIT_MS) &&
                     !(party.targetIsBoss && waited <= BOSS_PULL_HOLD_WAIT_MS && LeaderPullHeld(leader)))
                SkipTarget(party, party.target,
                           party.targetIsBoss && ROUTE_OCULUS_DRAKE_ONLY ? BOSS_NOT_PULLED_SKIP_MS : NOT_PULLED_SKIP_MS,
                           "not pulled");
            return;
        }

        // The first leg of the route (the corner path when the smooth one is over its point limit, RoutePath).
        if (!legBuilt)
            SteerLeg(leader, goal, leg);
        if (leg.type & PATHFIND_NOPATH)
        {
            SkipTarget(party, party.target, UNREACHABLE_SKIP_MS, "path lost");
            return;
        }
        // The walk of this tick never leads through a part closed to this run (AreaGateDef: an LBRS group and the
        // UBRS hall); from inside one the way out is no crossing.
        if (!InAnyAreaGate(steerClosedAreas, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()) &&
            RouteCrossesClosedArea(leg.points, steerClosedAreas))
        {
            SkipTarget(party, party.target, UNREACHABLE_SKIP_MS, "route through a closed area");
            return;
        }

        // [incomplete_route_deadend] (user 2026-10-25: "if the path is incomplete, why walk it?") A partial leg is
        // walked only when its end is progress (JudgeIncompleteEnd): not below a cliff / in water under one, not an end
        // no nearer to the goal, not an end the leader stood at already without getting nearer since. Maraudon heroic
        // 20261025-163739-mara-h-5A-17 swam to the floor end under the Earth Song waterfall (124 / 140 yd short, 79 /
        // 83 yd below Princess Theradras), low_breath 39 %, "own path refused". A complete way (the long search, from
        // the water surface too) is no partial leg; without one the target is skipped as a "route end" from here
        // (unreachableFrom: checked again once the leader stands elsewhere), as steer_stuck does after the walk.
        ForgetPassedDeadEnds(leader, goal);
        // Never a stall (Underbog heroic build 30, 20261025-184229-ub-h-5F-63: 13 refusals, 235 s idle): the third
        // refusal within DEAD_END_FALLBACK_WINDOW_MS puts the leader back on the old walk for DEAD_END_FALLBACK_MS.
        bool fallbackStarted = false;
        if (ROUTE_REFUSE_DEAD_END && leg.partial && !approachStopped && !party.approach && !party.targetIsStarter &&
            leg.points.size() >= 2 && !leader->IsFlying() && !leader->GetVehicle() &&
            !DeadEndFallback(leader, false, fallbackStarted))
        {
            DeadEndVerdict const verdict = JudgeIncompleteEnd(leader, leg.end, goal, leg.type, party.target);
            if (!verdict.why.empty() && DeadEndFallback(leader, true, fallbackStarted))
            {
                LogEvent(party, "incomplete_route_fallback",
                         JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                             .Str("why", verdict.why).Num("refusals", DEAD_END_FALLBACK_REFUSALS)
                             .Num("windowSec", DEAD_END_FALLBACK_WINDOW_MS / IN_MILLISECONDS)
                             .Num("fallbackSec", DEAD_END_FALLBACK_MS / IN_MILLISECONDS)
                             .Real("endLeft", verdict.left).Real("x", leader->GetPositionX())
                             .Real("y", leader->GetPositionY()).Real("z", leader->GetPositionZ()));
            }
            else if (!verdict.why.empty())
            {
                LOG_INFO("playerbots",
                         "[route] {}: incomplete_route_refused: {} {} ({}): end ({:.1f},{:.1f},{:.1f}) {:.1f} yd "
                         "short (horizontal {:.1f}, dz {:.1f}, rise {:.1f}, under water {}), {:.1f} yd away now",
                         party.id, object->GetName(), object->GetEntry(), verdict.why, leg.end.x, leg.end.y,
                         leg.end.z, verdict.left, verdict.hgap, verdict.dz, verdict.rise, verdict.underwater,
                         verdict.distance);
                LogEvent(party, "incomplete_route_refused",
                         JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                             .Bool("boss", party.targetIsBoss).Str("why", verdict.why).Num("pathType", leg.type)
                             .Bool("straightPath", leg.straightPath)
                             .Real("distance", verdict.distance).Real("endLeft", verdict.left)
                             .Real("horizontal", verdict.hgap).Real("dz", verdict.dz).Real("rise", verdict.rise)
                             .Bool("underwater", verdict.underwater).Real("surface", verdict.surface)
                             .Num("visitSec", verdict.visitSec).Real("endX", leg.end.x).Real("endY", leg.end.y)
                             .Real("endZ", leg.end.z).Real("x", leader->GetPositionX())
                             .Real("y", leader->GetPositionY()).Real("z", leader->GetPositionZ())
                             .Real("toX", goal.x).Real("toY", goal.y).Real("toZ", goal.z));
                if (party.targetIsBoss)
                    party.orderHoldSinceMs.emplace(party.target, now);  // as the steer_stuck "route end" does
                SkipTarget(party, party.target, UNREACHABLE_SKIP_MS, "route end: incomplete route refused (" +
                                                                         verdict.why + ")");
                return;
            }
        }
        // [incomplete_route_deadend] the long search ran from the water surface: logged once per target
        if (leg.fromSurface)
        {
            static std::set<std::pair<ObjectGuid, ObjectGuid>> surfaceLogged;  // world thread only
            if (surfaceLogged.insert({party.leader, party.target}).second)
                LogEvent(party, "route_surface_start",
                         JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                             .Real("distance", distance).Real("x", leader->GetPositionX())
                             .Real("y", leader->GetPositionY()).Real("z", leader->GetPositionZ())
                             .Real("endX", leg.end.x).Real("endY", leg.end.y).Real("endZ", leg.end.z));
        }

        // A waypoint between two corners of a corner path / long route has an interpolated height: on stairs and
        // slopes it floats above or sinks below the floor, and the leader's own MoveTo refuses it (SearchForBestPath
        // finds no height: "test runner move - FAILED", the wave 9 freezes at Pit of Saron's entrance and others).
        G3D::Vector3 const waypoint = FloorWaypoint(
            leader, leg,
            approachStopped ? approachStop
                            : RouteWaypoint(leg, goal, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP));

        // The navmesh ends here and the target stands on another part of it. Ahn'kahet wave 9: the entrance hall and
        // the Swarmers' level above it are two parts (tools/navmesh_probe.py 619 505.4 -963.4 30.7 525.8 -963.4 37.9:
        // gap 20.8 yd, dz 7.8), the ramp between has collision but no polygons; the leader stood at the end of the
        // partial route, its own move refused the target and the run stalled in 5 minutes. A player walks up the
        // ramp: straight, over ground the whole way (DirectLineOverGround), to the aggro edge of the target.
        G3D::Vector3 const here(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
        bool const atRouteEnd = (leg.type & PATHFIND_INCOMPLETE) && (waypoint - here).length() <= STEER_PROGRESS_RANGE;
        G3D::Vector3 next = waypoint;
        // In water the walk over the gap is a swim (ROUTE_SWIM_UP): no floor needed under the line, the swimmer
        // rises to the target's edge (Slave Pens: the route to Rokmar ends on the lake floor below him).
        bool const swimming = leader->Unit::IsInWater();
        if (atRouteEnd && (!swimming || ROUTE_SWIM_UP) && distance <= GAP_CROSS_MAX_DISTANCE)
        {
            if (party.gapTarget != party.target)
            {
                party.gapTarget = party.target;
                party.gapTries = 0;
            }
            if (leader->isMoving() &&
                leader->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
                return;  // on the way over already

            Creature* mob = object->ToCreature();
            float const stop = party.targetIsStarter || !mob
                                   ? STARTER_RANGE - 1.0f
                                   : std::min(PULL_RANGE - 2.0f, mob->GetAggroRange(leader) + 2.0f);
            if (party.gapTries < GAP_CROSS_TRIES && distance > stop + 1.0f)
            {
                G3D::Vector3 dest = here + (goal - here) * ((distance - stop) / distance);
                float const ground = map->GetHeight(leader->GetPhaseMask(), dest.x, dest.y, dest.z + 2.0f, true, 10.0f);
                if (ground > INVALID_HEIGHT && (!swimming || ground > dest.z))
                    dest.z = ground;
                bool const lineOk = swimming ? ground > INVALID_HEIGHT || WaterSurfaceAt(leader, dest) > INVALID_HEIGHT
                                             : ground > INVALID_HEIGHT &&
                                                   DirectLineOverGround(leader, dest.x, dest.y, dest.z);
                if (lineOk && leader->IsWithinLOS(dest.x, dest.y, dest.z + 2.0f))
                {
                    ++party.gapTries;
                    if (!HARNESS_MOVES_BOTS)
                    {
                        // The aggro edge across the gap is the leader's waypoint: its own "test runner move" walks
                        // straight where its path ends (MovementAction::MoveStraight, over ground, in sight).
                        SetLeaderWaypoint(party, leader, dest.x, dest.y, dest.z);
                    }
                    else
                    {
                        ++party.harnessMoves;
                        TestRunner::ClearWaypoint(party.leader);
                        leader->GetMotionMaster()->Clear();
                        MovementAction::RecordTankTrail(leader);
                        // A harness step never runs through a closed door either (ROUTE_CLOSED_DOORS_ALL).
                        {
                            ObjectGuid doorGuid;
                            float doorWalked = 0.0f;
                            LeaderDoorStop(leader, dest, doorGuid, doorWalked);
                        }
                        leader->GetMotionMaster()->MovePoint(0, dest.x, dest.y, dest.z, FORCED_MOVEMENT_NONE, 0.0f,
                                                             0.0f, false, true);
                        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(leader))
                        {
                            float const speed = std::max(1.0f, leader->GetSpeed(MOVE_RUN));
                            botAI->SetNextCheckDelay(uint32((dest - here).length() / speed * IN_MILLISECONDS) +
                                                     LINK_HOLD_MS);
                        }
                    }
                    party.lastProgressMs = now;
                    LogEvent(party, "gap_cross", JsonObject().Str("name", object->GetName())
                                                     .Num("entry", object->GetEntry()).Num("try", party.gapTries)
                                                     .Bool("swim", swimming).Bool("own", !HARNESS_MOVES_BOTS)
                                                     .Real("distance", distance).Real("x", here.x).Real("y", here.y)
                                                     .Real("z", here.z).Real("toX", dest.x).Real("toY", dest.y)
                                                     .Real("toZ", dest.z));
                    return;
                }
            }
        }
        // No straight walk over the gap: the leader's own move gets the target itself, as before the corner-path pass
        // (the core's point movement walks a partial path and goes straight on where polygons are missing - the
        // Mechanar stairs to Sepethrea in waves 6-7; wave 9 stood at their foot, 235.3,-30.2, for 4 minutes). When
        // that move is refused too, the steering watchdog above ends it.
        if (atRouteEnd)
            next = goal;
        else
        {
            // The leader's own move makes no progress on a route that goes on: logged as the bot's gap (bots walk by
            // themselves). [lead2] the dead harness drive walk is gone.
            if (LeaderOwnWalkStalled(party, leader, goal))
                LogCannotMove(party, leader, "route", "own walk made no progress for 6 s on a route that goes on",
                              waypoint.x, waypoint.y, waypoint.z);
            // [b32_brs] Bots walk by themselves: once the leader's own walk stalled (party.driving), its waypoint is
            // the leg's next corner, DRIVE_STEP yd at most and over ground the whole way, instead of the point
            // WAYPOINT_STEP along the leg - the near corner the runner's own MoveStraight walks when its path is
            // refused. brs-h 20261025-221216-brs-h-5F-9 (2421-2539 s): Rtwarac at (26.1,-551.5,-18.6) was sent to
            // (27.9,-530.7) / (30.8,-525.7), 21-26 yd along the leg that goes east round a raised object (navmesh
            // islands z -15..-12 at x 22-28, y -546..-549; navmesh_probe: connected via 30.9,-552.8 / 33.4,-546.9);
            // its own path was refused and the straight line ran over the object: bot_cannot_move x13, 173 s idle.
            if (party.driving && !HARNESS_MOVES_BOTS)
                for (size_t index = 1; index < leg.points.size(); ++index)
                {
                    G3D::Vector3 const ahead = leg.points[index] - here;
                    float const length = ahead.length();
                    if (length < 1.0f)
                        continue;
                    G3D::Vector3 step = here + ahead * (std::min(length, DRIVE_STEP) / length);
                    float const ground =
                        map->GetHeight(leader->GetPhaseMask(), step.x, step.y, step.z + 3.0f, true, 10.0f);
                    if (ground > INVALID_HEIGHT && std::fabs(ground - step.z) <= 3.0f &&
                        DirectLineOverGround(leader, step.x, step.y, ground) && (step - next).length() > 2.0f)
                    {
                        step.z = ground;
                        next = step;
                        static std::unordered_map<uint32, uint32> shortStepLogMs;  // party seq -> last log (world)
                        uint32 const logNow = getMSTime();
                        uint32& loggedMs = shortStepLogMs[party.seq];
                        if (!loggedMs || getMSTimeDiff(loggedMs, logNow) >= 10 * IN_MILLISECONDS)
                        {
                            loggedMs = logNow ? logNow : 1;
                            LogEvent(party, "stall_short_step",
                                     JsonObject().Str("name", object->GetName()).Num("entry", object->GetEntry())
                                         .Real("x", here.x).Real("y", here.y).Real("z", here.z)
                                         .Real("toX", step.x).Real("toY", step.y).Real("toZ", step.z)
                                         .Real("was", (waypoint - here).length()));
                        }
                    }
                    break;
                }
        }

        SetLeaderWaypoint(party, leader, next.x, next.y, next.z);
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
        uint32 const now = getMSTime();
        // [hleft-bm-tank-fetch] the group holds at Medivh while the tank fetches a keeper: nobody to meet.
        {
            bool fetch = leader && leader->GetMapId() == BLACK_MORASS_MAP;
            for (Player* bot : players)
                if (fetch && bot != leader && bot->IsAlive())
                    fetch = BlackMorassHoldsAtMedivh(bot);
            if (fetch)
                return false;
        }
        // The end of a regroup: the members on "stay" follow again.
        auto const releaseRegroup = [&](char const* why)
        {
            auto const found = regroups.find(party.instanceId);
            if (found == regroups.end())
                return;
            for (ObjectGuid const guid : found->second.held)
                if (Player* bot = ObjectAccessor::FindPlayer(guid))
                    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                        botAI->ChangeStrategy("-stay,+follow", BOT_STATE_NON_COMBAT);
            LogEvent(party, "regroup_end", JsonObject().Str("why", why).Num("held", int64(found->second.held.size()))
                                               .Num("replans", found->second.replans));
            regroups.erase(found);
        };

        // Steamvault wave 9 (peel agent): the healer, resurrected and walking back alone, followed the leader
        // straight through the idle packs' aggro radius and died 7 times in the Bog Overlord loop. A group waits for
        // it - here the leader (the others follow it) walks back to meet it; the member keeps following the leader,
        // so the two close in and nobody waits for the other.
        if (!leader || !leader->IsAlive() || leader->IsInCombat() || party.wiped || party.linkIdx >= 0 ||
            !party.escort.IsEmpty())
        {
            party.meeting.Clear();
            meetProgress.erase(party.instanceId);
            // A dead leader: the group keeps standing where it waits.
            if (leader && leader->IsAlive())
                releaseRegroup(leader->IsInCombat() ? "combat" : "no meeting");
            return false;
        }

        Player* member = nullptr;
        float best = 0.0f;
        for (Player* bot : players)
        {
            if (bot == leader || !bot->IsAlive() || bot->IsInCombat() || bot->GetMap() != leader->GetMap() ||
                bot->IsBeingTeleported() || MemberStranded(party, bot, leader))
                continue;
            float const distance = leader->GetExactDist(bot);
            // [meet_hysteresis] the member being met stays met down to MEET_END_RANGE
            float const meetFrom = party.meeting == bot->GetGUID() ? MEET_END_RANGE : MEET_RANGE;
            if (distance > meetFrom && (!member || distance > best))
            {
                member = bot;
                best = distance;
            }
        }
        if (!member)
        {
            party.meeting.Clear();
            meetProgress.erase(party.instanceId);
            stragglers.erase(party.instanceId);
            releaseRegroup("together");
            return false;
        }

        // Regroup: one side at the entrance (a member back in through the portal), the other farther than
        // MEET_RANGE from all of it.
        std::vector<Player*> atEntrance;
        std::vector<Player*> group;
        Position const* entrance =
            party.entranceIdx < party.entrances.size() ? &party.entrances[party.entranceIdx] : nullptr;
        // A walk out and in (a wing change, a regroup's exit) is under way: nobody walks to meet anybody.
        if (party.wingWalk)
        {
            TestRunner::ClearWaypoint(party.leader);
            return true;
        }
        // A regroup whose returning side runs already keeps its sides until the two are together (the runner leaves
        // the entrance on its walk).
        auto const running = regroups.find(party.instanceId);
        Player* const runningMember = running != regroups.end() && running->second.mode == RegroupMode::Run
                                          ? ObjectAccessor::FindPlayer(running->second.runner)
                                          : nullptr;
        Player* const runningAnchor =
            runningMember ? ObjectAccessor::FindPlayer(running->second.anchor) : nullptr;
        if (runningMember && runningAnchor && runningMember->IsAlive() && runningAnchor->IsAlive() &&
            runningMember->GetMap() == leader->GetMap() && runningAnchor->GetMap() == leader->GetMap() &&
            runningMember->GetExactDist(runningAnchor) > MEET_RANGE)
        {
            atEntrance.push_back(runningMember);
            for (Player* bot : players)
                if (bot != runningMember && bot->IsAlive() && bot->GetMap() == leader->GetMap() &&
                    (bot == runningAnchor || bot->GetExactDist(runningMember) > MEET_RANGE))
                    group.push_back(bot);
        }
        else if (ROUTE_REGROUP && !HARNESS_MOVES_BOTS && entrance &&
                 (!regroupCooldownUntilMs.count(party.instanceId) ||
                  int32(now - regroupCooldownUntilMs[party.instanceId]) >= 0))
        {
            std::vector<Player*> standing;
            for (Player* bot : players)
                if (bot->IsAlive() && !bot->IsInCombat() && bot->GetMap() == leader->GetMap() &&
                    !bot->IsBeingTeleported() && !MemberStranded(party, bot, leader))
                    standing.push_back(bot);
            // The returning side is a member back in through the portal (REGROUP_BACK_IN_MS), not whoever stands near
            // it: Gruul 25A 2026-10-24, the first pack fights 45-70 yd from the entrance, two mages casting from
            // 30 yd off it were "at the entrance", the off-tank and a warlock 45 yd ahead "the group" - an exit for two
            // with nobody dead or outside, and 22 members put on "stay" for the second regroup.
            auto const backIn = [&party, now](Player* bot)
            {
                for (Member const& candidate : party.members)
                    if (candidate.guid == bot->GetGUID())
                        return candidate.backInMs && getMSTimeDiff(candidate.backInMs, now) <= REGROUP_BACK_IN_MS;
                return false;
            };
            for (Player* bot : standing)
                if (bot->GetExactDist2d(entrance) <= WING_ARRIVE_RANGE && (!REGROUP_BACK_IN_ONLY || backIn(bot)))
                    atEntrance.push_back(bot);
            for (Player* bot : standing)
            {
                if (std::find(atEntrance.begin(), atEntrance.end(), bot) != atEntrance.end())
                    continue;
                bool away = true;
                for (Player* back : atEntrance)
                    away &= back->GetExactDist(bot) > MEET_RANGE;
                if (away)
                    group.push_back(bot);
            }
        }
        if (atEntrance.empty() || group.empty())
            releaseRegroup("no regroup");
        else
        {
            RegroupState& state = regroups[party.instanceId];
            bool const leaderAtEntrance =
                std::find(atEntrance.begin(), atEntrance.end(), leader) != atEntrance.end();
            Player* const runner = leaderAtEntrance ? leader : atEntrance.front();
            // The group's anchor: the member with the most of the group around it.
            Player* anchor = nullptr;
            size_t anchorNear = 0;
            for (Player* bot : group)
            {
                size_t around = 0;
                for (Player* other : group)
                    around += bot->GetExactDist(other) <= REGROUP_GROUP_RADIUS ? 1 : 0;
                if (!anchor || around > anchorNear)
                {
                    anchor = bot;
                    anchorNear = around;
                }
            }

            // The walk of the returning side makes no progress: decided again (meet_stuck).
            if (state.mode == RegroupMode::Run)
            {
                Player* watched = ObjectAccessor::FindPlayer(state.runner);
                Player* target = ObjectAccessor::FindPlayer(state.anchor);
                if (!watched || !target || watched->GetMap() != leader->GetMap() ||
                    target->GetMap() != leader->GetMap())
                    state.mode = RegroupMode::None;
                else
                {
                    float const distance = watched->GetExactDist(target);
                    if (distance + REGROUP_PROGRESS < state.bestDistance)
                    {
                        state.bestDistance = distance;
                        state.progressMs = now;
                    }
                    else if (getMSTimeDiff(state.progressMs, now) > REGROUP_STUCK_MS)
                    {
                        ++state.replans;
                        LOG_INFO("playerbots", "[regroup] {}: meet_stuck: {} {:.1f} yd from {} for {} s, replan {}",
                                 party.id, watched->GetName(), distance, target->GetName(),
                                 REGROUP_STUCK_MS / IN_MILLISECONDS, state.replans);
                        LogEvent(party, "meet_stuck",
                                 JsonObject().Str("name", watched->GetName()).Str("to", target->GetName())
                                     .Real("distance", distance).Num("replan", state.replans)
                                     .Real("x", watched->GetPositionX()).Real("y", watched->GetPositionY())
                                     .Real("z", watched->GetPositionZ()));
                        longRouteCache.erase(watched->GetGUID());
                        state.mode = RegroupMode::None;
                    }
                }
            }

            // The walk out and in is over (TickWingWalk) and the sides are still apart: decided anew, at most
            // REGROUP_MAX_REPLANS times with the stuck walks, then the meeting walk as before.
            if (state.mode == RegroupMode::Exit)
            {
                state.mode = RegroupMode::None;
                ++state.replans;
            }
            if (state.replans > REGROUP_MAX_REPLANS)
            {
                regroupCooldownUntilMs[party.instanceId] = now + REGROUP_COOLDOWN_MS;
                releaseRegroup("gave up");
            }
            else
            {
                if (state.mode == RegroupMode::None)
                {
                    // The signal: may the returning side run, or does the group come to the entrance?
                    G3D::Vector3 const goal(anchor->GetPositionX(), anchor->GetPositionY(), anchor->GetPositionZ());
                    PathResult const route = RoutePath(runner, goal.x, goal.y, goal.z);
                    RegroupPathCheck const check = CheckRegroupPath(runner, route.points);
                    float const speed = std::max(1.0f, runner->GetSpeed(MOVE_RUN));
                    float const runSec = route.length / speed + REGROUP_RESPAWN_MARGIN_SEC;
                    bool const respawnSoon = check.nextRespawnSec >= 0.0f && check.nextRespawnSec <= runSec;
                    bool const clear = route.reachable && !check.aliveMobs && !respawnSoon &&
                                       state.replans < REGROUP_MAX_REPLANS;
                    std::string const respawn =
                        check.nextRespawnSec < 0.0f
                            ? std::string("none")
                            : std::to_string(int32(check.nextRespawnSec)) + " s (" + check.nextRespawnMob + ")";
                    char const* const why = !route.reachable                        ? ", no walk"
                                            : state.replans >= REGROUP_MAX_REPLANS ? ", walk stuck"
                                                                                   : "";
                    std::string const decision =
                        clear ? Acore::StringFormat("regroup: {} can run (path clear, next respawn in {})",
                                                    runner->GetName(), respawn)
                              : Acore::StringFormat("regroup: group comes to the entrance ({} mobs alive{}{} / "
                                                    "respawn in {}{})",
                                                    check.aliveMobs, check.aliveMobs ? ", first " : "",
                                                    check.firstMob, respawn, why);
                    LOG_INFO("playerbots",
                             "[regroup] {}: {} (walk {:.0f} yd, {:.0f} s with margin; {} at the entrance, {} in the "
                             "group)",
                             party.id, decision, route.length, runSec, atEntrance.size(), group.size());
                    JsonArray backNames;
                    for (Player* bot : atEntrance)
                        backNames.Str(bot->GetName());
                    JsonArray groupNames;
                    for (Player* bot : group)
                        groupNames.Str(bot->GetName());
                    LogEvent(party, "regroup",
                             JsonObject().Str("decision", clear ? "run" : "exit").Str("signal", decision)
                                 .Str("runner", runner->GetName()).Str("anchor", anchor->GetName())
                                 .Raw("atEntrance", backNames.Build()).Raw("group", groupNames.Build())
                                 .Bool("reachable", route.reachable).Real("pathLength", route.length)
                                 .Real("runSec", runSec).Num("aliveMobs", check.aliveMobs)
                                 .Str("firstMob", check.firstMob).Real("nextRespawnSec", check.nextRespawnSec)
                                 .Str("nextRespawnMob", check.nextRespawnMob)
                                 .Num("replans", state.replans));
                    party.lastEvent = decision;
                    state.runner = runner->GetGUID();
                    state.anchor = anchor->GetGUID();
                    state.bestDistance = runner->GetExactDist(anchor);
                    state.progressMs = now;
                    // REGROUP_EXIT_NEEDS_WALK: the exit only for a group that can walk to the entrance. Wave 21 mech-h:
                    // the group stood on the Mechanar's upper level (joined to the entrance only by the lift link), the
                    // exit gave all four the portal goal, none had a walk ("no walk out for 2 minutes: left out of the
                    // wing walk"), the wing walk held the route 412 s. Refused, the regroup ends and the route (its
                    // route links, the stranded-member rule) brings the sides together.
                    std::string noWalkOut;
                    if (!clear && REGROUP_EXIT_NEEDS_WALK)
                        for (Player* bot : group)
                            if (!RoutePath(bot, entrance->GetPositionX(), entrance->GetPositionY(),
                                           entrance->GetPositionZ())
                                     .complete)
                                noWalkOut += (noWalkOut.empty() ? "" : ",") + bot->GetName();
                    if (clear)
                        state.mode = RegroupMode::Run;
                    else if (!noWalkOut.empty())
                    {
                        LOG_INFO("playerbots",
                                 "[regroup] {}: regroup_exit_refused: no walk to the entrance for {} - no exit, the "
                                 "route goes on",
                                 party.id, noWalkOut);
                        LogEvent(party, "regroup_exit_refused",
                                 JsonObject().Str("noWalkOut", noWalkOut).Str("runner", runner->GetName())
                                     .Str("anchor", anchor->GetName()).Num("replans", state.replans));
                        regroupCooldownUntilMs[party.instanceId] = now + REGROUP_COOLDOWN_MS;
                        releaseRegroup("exit refused: no walk to the entrance");
                        return false;
                    }
                    else
                    {
                        // Out through the exit and in through the entrance the returning side stands at, each on its
                        // own ("instance portal" with the exact goal, as a wing change; TickWingWalk ends it there).
                        state.mode = RegroupMode::Exit;
                        for (Player* bot : group)
                            for (Member& candidate : party.members)
                                if (candidate.guid == bot->GetGUID())
                                {
                                    SelfTravel::PortalGoal goal;
                                    goal.mapId = party.content->mapId;
                                    goal.x = entrance->GetPositionX();
                                    goal.y = entrance->GetPositionY();
                                    goal.z = entrance->GetPositionZ();
                                    goal.exact = true;
                                    SelfTravel::SetPortalGoal(candidate.guid, goal);
                                    candidate.hasPortalGoal = true;
                                }
                        party.wingWalk = true;
                        party.wingWalkSinceMs = now;
                    }
                }

                if (state.mode == RegroupMode::Exit)
                {
                    TestRunner::ClearWaypoint(party.leader);
                    return true;
                }

                // Run: the group holds where it stands (drinking is above "stay"), the returning side walks to it -
                // the leader by its waypoint, the others at the entrance by following the leader.
                for (Player* bot : group)
                {
                    if (bot == leader)
                        continue;
                    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                    if (!botAI)
                        continue;
                    if (botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT) ||
                        !botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
                        botAI->ChangeStrategy("-follow,+stay", BOT_STATE_NON_COMBAT);
                    if (std::find(state.held.begin(), state.held.end(), bot->GetGUID()) == state.held.end())
                        state.held.push_back(bot->GetGUID());
                }
                if (!leaderAtEntrance)
                {
                    TestRunner::ClearWaypoint(party.leader);
                    return true;
                }
                member = anchor;
                best = leader->GetExactDist(anchor);
            }
        }

        bool const regrouping = regroups.find(party.instanceId) != regroups.end();
        StragglerEpisode* episode = nullptr;
        if (!regrouping)
        {
            episode = &stragglers[party.instanceId];
            if (episode->member != member->GetGUID() || getMSTimeDiff(episode->lastSeenMs, now) > MEET_EPISODE_GAP_MS)
            {
                *episode = StragglerEpisode();
                episode->member = member->GetGUID();
                episode->sinceMs = now;
                episode->holdSinceMs = now;
                episode->bestDistance = best;
            }
            episode->lastSeenMs = now;
            // The far-member wait counts it far until it is within MEET_RANGE (TickParty), not 50 yd: no flip-flop.
            party.meeting = member->GetGUID();
            if (episode->gaveWay)
            {
                // [meet_end_follow_trail] gave way: the far wait uses its 50 yd line again, not the meet's
                party.meeting.Clear();
                party.lastEvent = "meet: gave way to the follow of " + member->GetName();
                return false;
            }
            if (episode->holdSinceMs)
            {
                if (best + REGROUP_PROGRESS < episode->bestDistance)
                {
                    episode->bestDistance = best;
                    episode->holdSinceMs = now;
                }
                if (getMSTimeDiff(episode->holdSinceMs, now) < MEET_GRACE_MS)
                {
                    TestRunner::ClearWaypoint(party.leader);
                    party.lastEvent = "meet hold: " + member->GetName() + " follows first";
                    return true;
                }
            }
        }

        PathResult leg;
        G3D::Vector3 const goal(member->GetPositionX(), member->GetPositionY(), member->GetPositionZ());
        SteerLeg(leader, goal, leg);
        if (leg.type & PATHFIND_NOPATH)
            return false;
        G3D::Vector3 const waypoint =
            FloorWaypoint(leader, leg, RouteWaypoint(leg, goal, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP));

        if (episode && episode->holdSinceMs)
        {
            // The grace is over and the member did not come: the leader walks back to it.
            episode->holdSinceMs = 0;
            meetProgress[party.instanceId] = {member->GetGUID(), best, now};
            LogEvent(party, "meet_member", JsonObject().Str("name", member->GetName()).Real("distance", best)
                                               .Num("farSec", getMSTimeDiff(episode->sinceMs, now) / IN_MILLISECONDS)
                                               .Num("stuck", episode->stuck)
                                               .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                                               .Real("z", leader->GetPositionZ()));
        }
        else if (regrouping && party.meeting != member->GetGUID())
        {
            party.meeting = member->GetGUID();
            meetProgress[party.instanceId] = {member->GetGUID(), best, now};
            LogEvent(party, "meet_member", JsonObject().Str("name", member->GetName()).Real("distance", best)
                                               .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                                               .Real("z", leader->GetPositionZ()));
        }
        else if (episode)
        {
            // The leader's walk back to the member makes no progress: its route is searched anew (meet_stuck) after
            // another grace for the member's own follow; MEET_MAX_STUCK times, then the leader gives way.
            MeetProgress& progress = meetProgress[party.instanceId];
            if (progress.member != member->GetGUID())
                progress = {member->GetGUID(), best, now};
            else if (best + REGROUP_PROGRESS < progress.bestDistance)
            {
                progress.bestDistance = best;
                progress.progressMs = now;
            }
            else if (getMSTimeDiff(progress.progressMs, now) > REGROUP_STUCK_MS)
            {
                ++episode->stuck;
                episode->gaveWay = episode->stuck >= MEET_MAX_STUCK;
                LogEvent(party, "meet_stuck", JsonObject().Str("name", leader->GetName()).Str("to", member->GetName())
                                                  .Real("distance", best).Num("stuck", episode->stuck)
                                                  .Str("next", episode->gaveWay ? "give way" : "hold, then walk back")
                                                  .Real("x", leader->GetPositionX())
                                                  .Real("y", leader->GetPositionY())
                                                  .Real("z", leader->GetPositionZ()));
                longRouteCache.erase(leader->GetGUID());
                progress.progressMs = now;
                progress.bestDistance = best;
                TestRunner::ClearWaypoint(party.leader);
                if (episode->gaveWay)
                {
                    LOG_INFO("playerbots",
                             "[regroup] {}: {} gives way to {} ({:.1f} yd, far {} s, {} meet_stuck): no more walks "
                             "back, its own follow comes (the guard's deadlock breaker), the far wait times out",
                             party.id, leader->GetName(), member->GetName(), best,
                             getMSTimeDiff(episode->sinceMs, now) / IN_MILLISECONDS, episode->stuck);
                    LogEvent(party, "meet_give_way",
                             JsonObject().Str("name", member->GetName()).Real("distance", best)
                                 .Num("farSec", getMSTimeDiff(episode->sinceMs, now) / IN_MILLISECONDS)
                                 .Num("stuck", episode->stuck));
                    return false;
                }
                episode->holdSinceMs = now;
                episode->bestDistance = best;
                return true;
            }
        }

        // ROUTE_HARNESS_WALK_GUARD: the walk back never wakes an idle pack (lbrs-n wave 7 01:40:38: the route up to
        // Rlhunas ran through the refused Firebrand pack, 24 mobs, wipe). At the edge the leader holds; that is no
        // progress, so meet_stuck and then the give-way end it.
        G3D::Vector3 walkTo = waypoint;
        Creature* blocker = nullptr;
        bool held = false;
        // Only a pack the tank's planner refused holds the walk back; with no verdict (or an accepted one) the walk
        // goes on and the tank fights what it meets (strat-h wave 11: the meet stopped at a lone Mangled Cadaver,
        // "none, 0 mobs", for 100 s until the give-way, the members 60 yd behind it never came).
        bool refusedBlocker = false;
        if (ROUTE_HARNESS_WALK_GUARD &&
            HarnessWalkStop(leader, leg.points, leg.steep ? STEEP_WAYPOINT_STEP : WAYPOINT_STEP, nullptr, walkTo,
                            blocker, held, &party))
        {
            bool accepted = true;
            uint32 mobs = 0;
            std::list<Creature*> pack;
            PackMobCheck packCheck{leader, blocker};
            Acore::CreatureListSearcher<PackMobCheck> packSearcher(blocker, pack, packCheck);
            Cell::VisitObjects(blocker, packSearcher, PACK_RADIUS);
            // Refused now, refused or skipped before (the verdict ages out in seconds), or a real pack: held.
            refusedBlocker = (PartyVerdict(party, blocker->GetGUID(), accepted, mobs) &&
                              !accepted) ||
                             party.carefulRefused.count(blocker->GetGUID()) ||
                             party.skipUntil.count(blocker->GetGUID()) ||
                             pack.size() > MEET_PASS_PACK_MAX;
            if (!refusedBlocker)
            {
                walkTo = waypoint;
                held = false;
            }
            else
                LogHarnessHold(party, leader, blocker, "meet", walkTo.x, walkTo.y, walkTo.z);
            if (held)
            {
                TestRunner::ClearWaypoint(party.leader);
                return true;
            }
        }
        SetLeaderWaypoint(party, leader, walkTo.x, walkTo.y, walkTo.z);
        return true;
    }

    void TestHarnessMgr::LogHarnessHold(Party& party, Player* leader, Creature* blocker, char const* walk, float x,
                                        float y, float z)
    {
        if (!leader || !blocker)
            return;
        uint32 const now = getMSTime();
        uint32& loggedMs = harnessHoldLogMs[blocker->GetGUID().GetRawValue()];
        if (loggedMs && getMSTimeDiff(loggedMs, now) < HARNESS_HOLD_LOG_MS)
            return;
        loggedMs = now ? now : 1;
        bool accepted = true;
        uint32 mobs = 0;
        bool const known = PartyVerdict(party, blocker->GetGUID(), accepted, mobs);
        char const* verdict = !known ? "none" : accepted ? "accepted" : "refused";
        // [b30_mech_pos] packMobs was the planner's count only: 0 with no verdict (zf-h 20261025-184229: "Sandfury
        // Blood Drinker, none, 0 mobs" with ~20 Sandfury round the pool). The idle pack round the blocker is counted
        // too (realPack, PackMobCheck within PACK_RADIUS, the blocker included).
        std::list<Creature*> pack;
        PackMobCheck packCheck{leader, blocker};
        Acore::CreatureListSearcher<PackMobCheck> packSearcher(blocker, pack, packCheck);
        Cell::VisitObjects(blocker, packSearcher, PACK_RADIUS);
        uint32 const realPack =
            uint32(pack.size()) + (std::find(pack.begin(), pack.end(), blocker) == pack.end() ? 1u : 0u);
        LOG_INFO("playerbots",
                 "[approach] {}: harness_hold_refused_pack: {} stops at {} ({}, {} mobs, {} idle round it) {:.1f} yd",
                 leader->GetName(), walk, blocker->GetName(), verdict, mobs, realPack, leader->GetExactDist(blocker));
        LogEvent(party, "harness_hold_refused_pack",
                 JsonObject().Str("walk", walk).Str("blocker", blocker->GetName()).Num("entry", blocker->GetEntry())
                     .Str("verdict", verdict).Num("packMobs", mobs).Num("realPack", realPack)
                     .Real("distance", leader->GetExactDist(blocker)).Real("x", x).Real("y", y).Real("z", z));
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
        uint32 const now = getMSTime();
        uint32 index = 0;
        for (Player* bot : players)
        {
            ++index;
            Member* member = nullptr;
            for (Member& candidate : party.members)
                if (candidate.guid == bot->GetGUID())
                    member = &candidate;
            if (!member)
                continue;

            if (!bot->IsAlive() || bot->GetMapId() != party.content->mapId)
            {
                member->breathMs = -1;
                member->surfacing = false;
                member->leavingWater = false;
                continue;
            }

            // Breath, as Player::HandleDrowning counts it (no breath used with a water breathing aura).
            bool const under = bot->IsUnderWater() && !bot->HasWaterBreathingAura();
            int32 const elapsed =
                member->breathSampleMs ? int32(std::min<uint32>(getMSTimeDiff(member->breathSampleMs, now), 5000)) : 0;
            member->breathSampleMs = now;
            if (member->breathMs < 0)
                member->breathMs = BREATH_MAX_MS;
            member->breathMs = under ? std::max<int32>(0, member->breathMs - elapsed)
                                     : std::min<int32>(BREATH_MAX_MS, member->breathMs + elapsed * BREATH_REFILL_RATE);

            if (!HARNESS_MOVES_BOTS)
            {
                // Bots walk by themselves: the bot's own "breathe" swims up. Logged once per dive, for comparison
                // with its death events (environment: drowning).
                if (under && member->breathMs < BREATH_SURFACE_MS && !member->lowBreathLogged)
                {
                    member->lowBreathLogged = true;
                    LogEvent(party, "low_breath", JsonObject().Str("name", bot->GetName())
                                                      .Num("breathPct", member->breathMs * 100 / BREATH_MAX_MS)
                                                      .Bool("combat", bot->IsInCombat()));
                }
                if (!under)
                    member->lowBreathLogged = false;
            }
            else if (under && member->breathMs < BREATH_SURFACE_MS)
            {
                // Straight up to the surface, like a player would, then back to what it was doing.
                LiquidData const& liquid = bot->GetLiquidData();
                if (liquid.Level != INVALID_HEIGHT)
                {
                    if (!member->surfacing)
                        LogEvent(party, "surface", JsonObject().Str("name", bot->GetName())
                                                       .Num("breathPct", member->breathMs * 100 / BREATH_MAX_MS)
                                                       .Real("depth", liquid.Level - bot->GetPositionZ())
                                                       .Bool("combat", bot->IsInCombat()));
                    member->surfacing = true;
                    ++party.harnessMoves;
                    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                        botAI->SetNextCheckDelay(SURFACE_HOLD_MS);
                    bot->GetMotionMaster()->MovePoint(0, bot->GetPositionX(), bot->GetPositionY(), liquid.Level + 0.5f,
                                                      FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, true);
                }
                continue;
            }
            if (HARNESS_MOVES_BOTS && member->surfacing && (under || member->breathMs < BREATH_RESUME_MS))
            {
                // At the surface: hold there until breathed (the bot AI would dive straight back).
                if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                    botAI->SetNextCheckDelay(SURFACE_HOLD_MS);
                continue;
            }
            member->surfacing = false;

            bool const wet = bot->Unit::IsInWater();
            // Resting (eating / drinking / waiting for mana) happens on dry ground, out of combat.
            bool const leave = resting && wet && !bot->IsInCombat();
            if (!wet || !leave)
            {
                // Out of the water, or the rest is over: the dry spot is no longer its waypoint.
                if (member->leavingWater && !HARNESS_MOVES_BOTS && member->guid != party.leader)
                    ClearMemberWaypoint(*member);
                if (!wet)
                    member->leavingWater = false;
            }
            if (!leave || !party.hasDryPos || (HARNESS_MOVES_BOTS && bot->isMoving()))
                continue;

            float const angle = float(index) * 2.0f * float(M_PI) / 5.0f;
            float const dryX = party.dryPos.GetPositionX() + DRY_SPOT_SPREAD * std::cos(angle);
            float const dryY = party.dryPos.GetPositionY() + DRY_SPOT_SPREAD * std::sin(angle);
            if (!HARNESS_MOVES_BOTS)
            {
                // As a leader says "rest over here": the dry spot is the member's waypoint, its own walk out.
                SetMemberWaypoint(party, *member, bot, dryX, dryY, party.dryPos.GetPositionZ());
            }
            else
            {
                if (bot->IsSitState())
                    bot->SetStandState(UNIT_STAND_STATE_STAND);  // also ends eating / drinking under water
                ++party.harnessMoves;
                bot->GetMotionMaster()->MovePoint(0, dryX, dryY, party.dryPos.GetPositionZ(), FORCED_MOVEMENT_NONE,
                                                  0.0f, 0.0f, true, false);
            }
            if (!member->leavingWater)
            {
                member->leavingWater = true;
                LogEvent(party, "leave_water", JsonObject().Str("name", bot->GetName()).Bool("underWater", under)
                                                   .Bool("resting", resting).Real("z", bot->GetPositionZ())
                                                   .Real("dryX", party.dryPos.GetPositionX())
                                                   .Real("dryY", party.dryPos.GetPositionY())
                                                   .Real("dryZ", party.dryPos.GetPositionZ()));
            }
        }
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
                           member.spec && UsesMana(member.spec->role, member.cls)});
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
