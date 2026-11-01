/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "RouteWater.h"

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
#include "RouteMeet.h"

namespace Leader::Travel
{
    using namespace Leader::Fight;
    using namespace Leader::Knowledge;

    void LeaveWater(LeaderGroup& group, std::vector<Player*> const& players, bool resting)
    {
        GroupRouteState& routeState = group.Route();
        uint32 const now = getMSTime();
        uint32 index = 0;
        for (Player* bot : players)
        {
            ++index;
            ObjectGuid memberGuid;
            for (LeaderGroup::MemberInfo const& candidate : group.Members())
                if (candidate.guid == bot->GetGUID())
                    memberGuid = candidate.guid;
            bool const member = !memberGuid.IsEmpty();
            if (!member)
                continue;
            MemberWater& water = routeState.water[memberGuid];  // [lead107] its water facts

            if (!bot->IsAlive() || bot->GetMapId() != group.MapId())
            {
                water.breathMs = -1;
                water.surfacing = false;
                water.leavingWater = false;
                continue;
            }

            // Breath, as Player::HandleDrowning counts it (no breath used with a water breathing aura).
            bool const under = bot->IsUnderWater() && !bot->HasWaterBreathingAura();
            int32 const elapsed =
                water.breathSampleMs ? int32(std::min<uint32>(getMSTimeDiff(water.breathSampleMs, now), 5000)) : 0;
            water.breathSampleMs = now;
            if (water.breathMs < 0)
                water.breathMs = BREATH_MAX_MS;
            water.breathMs = under ? std::max<int32>(0, water.breathMs - elapsed)
                                     : std::min<int32>(BREATH_MAX_MS, water.breathMs + elapsed * BREATH_REFILL_RATE);

            if (!HARNESS_MOVES_BOTS)
            {
                // Bots walk by themselves: the bot's own "breathe" swims up. Logged once per dive, for comparison
                // with its death events (environment: drowning).
                if (under && water.breathMs < BREATH_SURFACE_MS && !water.lowBreathLogged)
                {
                    water.lowBreathLogged = true;
                    group.LogEvent("low_breath", JsonObject().Str("name", bot->GetName())
                                                      .Num("breathPct", water.breathMs * 100 / BREATH_MAX_MS)
                                                      .Bool("combat", bot->IsInCombat()));
                }
                if (!under)
                    water.lowBreathLogged = false;
            }
            else if (under && water.breathMs < BREATH_SURFACE_MS)
            {
                // Straight up to the surface, like a player would, then back to what it was doing.
                LiquidData const& liquid = bot->GetLiquidData();
                if (liquid.Level != INVALID_HEIGHT)
                {
                    if (!water.surfacing)
                        group.LogEvent("surface", JsonObject().Str("name", bot->GetName())
                                                       .Num("breathPct", water.breathMs * 100 / BREATH_MAX_MS)
                                                       .Real("depth", liquid.Level - bot->GetPositionZ())
                                                       .Bool("combat", bot->IsInCombat()));
                    water.surfacing = true;
                    group.CountHarnessMove();
                    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                        botAI->SetNextCheckDelay(SURFACE_HOLD_MS);
                    bot->GetMotionMaster()->MovePoint(0, bot->GetPositionX(), bot->GetPositionY(), liquid.Level + 0.5f,
                                                      FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, true);
                }
                continue;
            }
            if (HARNESS_MOVES_BOTS && water.surfacing && (under || water.breathMs < BREATH_RESUME_MS))
            {
                // At the surface: hold there until breathed (the bot AI would dive straight back).
                if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                    botAI->SetNextCheckDelay(SURFACE_HOLD_MS);
                continue;
            }
            water.surfacing = false;

            // [lead226] lava / slime under a member out of a fight is left at once, resting or not (b185 5B: the tank
            // ate in Ragefire's magma for 2 min and burned to death)
            bool const burns = InHarmfulLiquid(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            bool const wet = bot->Unit::IsInWater() || burns;
            // Resting (eating / drinking / waiting for mana) happens on dry ground, out of combat.
            bool const leave = (resting || burns) && wet && !bot->IsInCombat();
            if (!wet || !leave)
            {
                // Out of the water, or the rest is over: the dry spot is no longer its waypoint.
                if (water.leavingWater && !HARNESS_MOVES_BOTS && memberGuid != group.LeaderGuid())
                    group.ClearMemberWaypoint(memberGuid);
                if (!wet)
                    water.leavingWater = false;
            }
            if (!leave || !routeState.hasDryPos || (HARNESS_MOVES_BOTS && bot->isMoving()))
                continue;

            float const angle = float(index) * 2.0f * float(M_PI) / 5.0f;
            float const dryX = routeState.dryPos.GetPositionX() + DRY_SPOT_SPREAD * std::cos(angle);
            float const dryY = routeState.dryPos.GetPositionY() + DRY_SPOT_SPREAD * std::sin(angle);
            if (!HARNESS_MOVES_BOTS)
            {
                // As a leader says "rest over here": the dry spot is the member's waypoint, its own walk out.
                group.SetMemberWaypoint(memberGuid, bot, dryX, dryY, routeState.dryPos.GetPositionZ());
            }
            else
            {
                if (bot->IsSitState())
                    bot->SetStandState(UNIT_STAND_STATE_STAND);  // also ends eating / drinking under water
                group.CountHarnessMove();
                bot->GetMotionMaster()->MovePoint(0, dryX, dryY, routeState.dryPos.GetPositionZ(), FORCED_MOVEMENT_NONE,
                                                  0.0f, 0.0f, true, false);
            }
            if (!water.leavingWater)
            {
                water.leavingWater = true;
                group.LogEvent("leave_water", JsonObject().Str("name", bot->GetName()).Bool("underWater", under)
                                                   .Bool("resting", resting).Real("z", bot->GetPositionZ())
                                                   .Real("dryX", routeState.dryPos.GetPositionX())
                                                   .Real("dryY", routeState.dryPos.GetPositionY())
                                                   .Real("dryZ", routeState.dryPos.GetPositionZ()));
            }
        }
    }
}
