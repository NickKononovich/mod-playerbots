/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ANActions.h"
#include "DungeonInteract.h"
#include "GridTerrainData.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "PullStrategy.h"
#include "RaidBotDuty.h"
#include "RaidLeaderState.h"
#include "TerrainHazard.h"  // [b35_z0] SafeFloorZ

#include <algorithm>
#include <cmath>

namespace
{
// The walk down to the rim (DropToLowerKingdomAction): legs of a complete route, a leg this close is reached.
constexpr float RIM_LEG_STEP = 15.0f;
constexpr float RIM_LEG_REACHED = 1.0f;
// Up from the pool's bottom to just above its surface (SurfaceForAirAction's height).
constexpr float POOL_SURFACE_ABOVE_LEVEL = 0.5f;
}  // namespace

bool AttackWebWrapAction::isUseful() { return !PlayerbotAI::IsHeal(bot); }
bool AttackWebWrapAction::Execute(Event /*event*/)
{
    Unit* webWrap = nullptr;

    // Target is not findable from threat table using AI_VALUE2(),
    // therefore need to search manually for the unit name
    GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");

    for (auto i = targets.begin(); i != targets.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (unit && unit->GetEntry() == NPC_WEB_WRAP)
        {
            webWrap = unit;
            break;
        }
    }
    if (!webWrap || AI_VALUE(Unit*, "current target") == webWrap)
    {
        return false;
    }

    return Attack(webWrap);
}

bool WatchersTargetAction::isUseful() { return !PlayerbotAI::IsHeal(bot); }
bool WatchersTargetAction::Execute(Event /*event*/)
{
    // Always prioritise web wraps
    Unit* currTarget = AI_VALUE(Unit*, "current target");
    if (currTarget && currTarget->GetEntry() == NPC_WEB_WRAP) { return false; }

    // Do not search all units in range!
    // There are many adds we don't want to aggro in close proximity,
    // only check in-combat adds now.
    GuidVector attackers = AI_VALUE(GuidVector, "attackers");
    Unit* priorityTargets[4] = {nullptr, nullptr, nullptr, nullptr};

    for (auto& attacker : attackers)
    {
        Unit* npc = botAI->GetUnit(attacker);
        // A mob standing in an untouched pack's reach is not walked to (the ranged kill it from the rally).
        if (!npc || ANWatcherRoom::ApproachUnsafe(botAI, npc))
        {
            continue;
        }
        switch (npc->GetEntry())
        {
            // Focus skirmishers first
            case NPC_WATCHER_SKIRMISHER:
                priorityTargets[0] = npc;
                break;
            // Then shadowcaster. This doesn't work so well for the shadowcaster
            // + skirmisher pack - ideally we would kill the watcher second.
            // But don't want to make this unnecessarily complex and rigid...
            // Will revisit if this causes problems in heroic.
            case NPC_WATCHER_SHADOWCASTER:
                priorityTargets[1] = npc;
                break;
            // Named watcher next
            case NPC_WATCHER_SILTHIK:
            case NPC_WATCHER_GASHRA:
            case NPC_WATCHER_NARJIL:
                priorityTargets[2] = npc;
                break;
            // Warrior last
            case NPC_WATCHER_WARRIOR:
                priorityTargets[3] = npc;
                break;
        }
    }

    for (Unit* target : priorityTargets)
    {
        // Attack the first valid split target in the priority list
        if (target)
        {
            if (currTarget != target)
            {
                // bot->Yell("ATTACKING "+target->GetName(), LANG_UNIVERSAL);
                return Attack(target);
            }
            // Don't continue loop here, the target exists so we don't
            // want to move down the prio list. We just don't need to send attack
            // command again, just return false and exit the loop that way
            return false;
        }
    }

    return false;
}

bool KrikthirWatcherPullAction::IsGroupReady(bool eventRunning)
{
    uint32 const now = getMSTime();
    // A wait broken off (a fight, the tactic off) starts again.
    if (!_waitSinceMs || getMSTimeDiff(_waitCheckMs, now) >= 5 * IN_MILLISECONDS)
        _waitSinceMs = now;
    _waitCheckMs = now;

    // Between packs the next Watcher is already on its way: only gathering counts, as it does after a long wait.
    uint32 const waited = getMSTimeDiff(_waitSinceMs, now);
    bool const relaxed = eventRunning || waited >= ANWatcherRoom::READY_WAIT_MS;
    // A member that cannot reach the rally does not hold the group for good (user rule: nobody stands still).
    bool const anyPlace = waited >= 2 * ANWatcherRoom::READY_WAIT_MS;
    char const* missing = nullptr;
    std::string who;
    auto const check = [&](Player* member)
    {
        if (!member || member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            return;

        if (!member->IsAlive())
        {
            if (!relaxed)
                missing = "dead";
        }
        else if (member->IsInCombat())
            missing = "in combat";
        else if (!anyPlace && member->GetExactDist2d(ANWatcherRoom::RALLY_X, ANWatcherRoom::RALLY_Y) >
                                  ANWatcherRoom::GATHER_RADIUS)
            missing = "not at the rally";
        else if (!relaxed)
        {
            bool drinking = false;
            if (RaidLeader::IsEatingOrDrinking(member, drinking))
                missing = "eating or drinking";
            else if (member->GetHealthPct() < ANWatcherRoom::READY_HEALTH_PCT)
                missing = "health";
            else if (member->getPowerType() == POWER_MANA && PlayerbotAI::IsHeal(member) &&
                     member->GetPowerPct(POWER_MANA) < ANWatcherRoom::READY_HEALER_MANA_PCT)
                missing = "healer mana";
            else if (member == bot && member->getPowerType() == POWER_MANA &&
                     member->GetPowerPct(POWER_MANA) < ANWatcherRoom::READY_TANK_MANA_PCT)
                missing = "tank mana";
        }

        if (missing && who.empty())
            who = member->GetName();
    };

    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref && !missing; ref = ref->next())
            check(ref->GetSource());
    }
    else
        check(bot);

    if (!missing)
        return true;

    if (!_waitLogMs || getMSTimeDiff(_waitLogMs, now) >= 10 * IN_MILLISECONDS)
    {
        _waitLogMs = now;
        LOG_INFO("playerbots", "Azjol-Nerub: {} holds the Watcher pull at the rally ({}: {}, waited {} s)",
                 bot->GetName(), who, missing, waited / IN_MILLISECONDS);
    }
    return false;
}

bool KrikthirWatcherPullAction::Execute(Event /*event*/)
{
    ANWatcherRoom::Snapshot const& snapshot = ANWatcherRoom::Get(botAI);
    if (!snapshot.active || snapshot.pullTarget.IsEmpty())
        return false;

    // The group fights at the foot of the ramp, out of every other pack's reach.
    if (bot->GetExactDist2d(ANWatcherRoom::RALLY_X, ANWatcherRoom::RALLY_Y) > ANWatcherRoom::RALLY_ARRIVED)
        return MoveTo(bot->GetMapId(), ANWatcherRoom::RALLY_X, ANWatcherRoom::RALLY_Y, ANWatcherRoom::RALLY_Z);

    if (!IsGroupReady(snapshot.watchersAlive < 3))
        return false;

    Unit* target = botAI->GetUnit(snapshot.pullTarget);
    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!target || !target->IsAlive() || target->IsInCombat() || !strategy)
        return false;

    // Nothing of the group's crowd control on the pack: every member must die for the next Watcher to come.
    PrePullCc::Cancel(bot);

    // The class's ranged pull, then back to the rally ("return to pull position"); the whole formation follows it.
    PositionMap& posMap = AI_VALUE(PositionMap&, "position");
    PositionInfo pullPosition = posMap["pull"];
    pullPosition.Set(ANWatcherRoom::RALLY_X, ANWatcherRoom::RALLY_Y, ANWatcherRoom::RALLY_Z, bot->GetMapId());
    posMap["pull"] = pullPosition;

    strategy->RequestPull(target, true, false);
    bot->SetSelection(target->GetGUID());
    context->GetValue<Unit*>("current target")->Set(target);
    botAI->ChangeEngine(BOT_STATE_COMBAT);
    _waitSinceMs = 0;
    LOG_INFO("playerbots", "Azjol-Nerub: {} pulls the Watcher pack of {} to the rally ({} Watchers alive)",
             bot->GetName(), target->GetName(), snapshot.watchersAlive);
    return true;
}

bool KrikthirBackToRallyAction::Execute(Event /*event*/)
{
    if (bot->GetExactDist2d(ANWatcherRoom::RALLY_X, ANWatcherRoom::RALLY_Y) <= ANWatcherRoom::RALLY_ARRIVED)
        return false;

    return MoveTo(bot->GetMapId(), ANWatcherRoom::RALLY_X, ANWatcherRoom::RALLY_Y, ANWatcherRoom::RALLY_Z, false,
                  false, false, false, MovementPriority::MOVEMENT_COMBAT, true);
}

// Only a bot next to him runs through him while he casts; from farther the cast ends first, those keep to his side
// beforehand (AnubarakFlankAction).
bool AnubarakDodgePoundAction::isUseful()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "anub'arak");
    return boss && !PlayerbotAI::IsTank(bot) && bot->GetExactDist2d(boss) <= ANIds::POUND_DODGE_RANGE &&
           !AI_VALUE2(bool, "behind", "current target");
}

bool AnubarakFlankAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "anub'arak");
    if (!boss || !boss->IsAlive())
        return false;

    // His side at the bot's own distance (a ranged keeps its range, a melee its reach), the nearer one.
    float const radius = std::clamp(bot->GetExactDist2d(boss), ANIds::POUND_FLANK_MIN, ANIds::POUND_FLANK_MAX);
    float bestX = 0.0f, bestY = 0.0f, bestZ = 0.0f;
    float best = -1.0f;
    // [b35_z0] A side's spot stands on the floor near the boss's height (SafeFloorZ: the navmesh within 4 yd of his
    // z), or it is skipped; the reach is tried again at POUND_FLANK_MIN. UpdateAllowedPositionZ gave a spot past the
    // room's edge the grid's 0.0 of the WMO-only tile (an-n 20261026: "anub'arak flank" to (575.3,243.5,0.0) from z
    // 223.9, refused as a 224 yd ledge drop, 37 times on the night; the nearest polygon is 6.9 yd off that spot).
    for (float const reach : {radius, ANIds::POUND_FLANK_MIN})
    {
        for (float const side : {ANIds::POUND_FLANK_ANGLE, -ANIds::POUND_FLANK_ANGLE})
        {
            float const angle = boss->GetOrientation() + side;
            float x = boss->GetPositionX() + std::cos(angle) * reach;
            float y = boss->GetPositionY() + std::sin(angle) * reach;
            float z = boss->GetPositionZ();
            if (!SafeFloorZ(bot, x, y, z))
                continue;
            float const walk = bot->GetExactDist2d(x, y);
            if (best < 0.0f || walk < best)
            {
                best = walk;
                bestX = x;
                bestY = y;
                bestZ = z;
            }
        }
        if (best >= 0.0f || reach <= ANIds::POUND_FLANK_MIN)
            break;
    }
    if (best < 0.0f)
        return false;
    return MoveTo(bot->GetMapId(), bestX, bestY, bestZ, false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
}
bool AnubarakDodgePoundAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "anub'arak");
    if (!boss) { return false; }

    float distance = bot->GetExactDist2d(boss->GetPosition());
    // Extra units to move into the boss, instead of being just 1 pixel past his midpoint.
    // Can be adjusted - this value tends to mirror how a human would play,
    // and visibly ensures you won't get hit while not creating excessive movements.
    float distanceExtra = 2.0f;
    return Move(bot->GetAngle(boss), distance + distanceExtra);
}

bool AvoidAnubarakImpaleAction::Execute(Event /*event*/)
{
    Creature* impale = bot->FindNearestCreature(ANIds::NPC_IMPALE_TARGET, ANIds::IMPALE_DANGER_RADIUS);
    if (!impale)
        return false;

    return MoveAway(impale, ANIds::IMPALE_SAFE_DISTANCE - bot->GetExactDist2d(impale));
}

bool HadronoxReturnToPlatformAction::Execute(Event /*event*/)
{
    return MoveTo(bot->GetMapId(), ANIds::HADRONOX_PLATFORM_X, ANIds::HADRONOX_PLATFORM_Y, ANIds::HADRONOX_PLATFORM_Z,
                  false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
}

bool DropToLowerKingdomAction::Execute(Event /*event*/)
{
    if (bot->HasUnitMovementFlag(MOVEMENTFLAG_FALLING))
    {
        // Keep the fall spline running; once it ends, clear the flag the way a landing client would.
        if (!bot->movespline->Finalized())
            return true;

        // Landing: a client in the pool sends MSG_MOVE_START_SWIM (no fall damage), one that hits the rim or the
        // ground sends MSG_MOVE_FALL_LAND and Player::HandleFall deals the damage from m_lastFallZ (set by MoveFall).
        // Player::IsInWater is the client's swim flag, never set for a bot: the liquid under it decides (wave 11:
        // the whole group "landed out of the water" in the pool and died of the fall).
        LiquidData const& liquid = bot->GetLiquidData();
        bool const swimming = (liquid.Status & MAP_LIQUID_STATUS_SWIMMING) &&
                              liquid.Level - bot->GetPositionZ() >= bot->GetCollisionHeight() * 0.5f;
        if (!swimming)
        {
            MovementInfo landing = bot->m_movementInfo;
            landing.pos.Relocate(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            bot->HandleFall(landing);
            LOG_INFO("playerbots", "Azjol-Nerub: {} landed out of the water at ({:.1f}, {:.1f}, {:.1f})",
                     bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        }
        bot->SetFallInformation(0, bot->GetPositionZ());
        bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
        if (!bot->IsRooted())
            bot->SendMovementFlagUpdate();
        return true;
    }

    // The hop over the hole is still in the air.
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == EFFECT_MOTION_TYPE)
        return true;

    if (bot->GetPositionZ() < ANIds::LOWER_LEVEL_MAX_Z)
        return SurfaceFromPool();

    if (bot->GetPositionZ() < ANIds::UPPER_LEVEL_MIN_Z)
        return false;

    // Fall as soon as there is no floor under the bot, as a player walking off the edge would.
    float const groundZ = bot->GetMapHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), true,
                                            MAX_FALL_DISTANCE);
    if (groundZ > INVALID_HEIGHT && bot->GetPositionZ() - groundZ > ANIds::HOLE_MIN_DROP)
    {
        bot->StopMoving();
        bot->GetMotionMaster()->MoveFall();
        LOG_INFO("playerbots", "Azjol-Nerub: {} drops to the lower kingdom from ({:.1f}, {:.1f}), pool below: {}",
                 bot->GetName(), bot->GetPositionX(), bot->GetPositionY(),
                 IsPoolBelow(bot->GetPositionX(), bot->GetPositionY()) ? "yes" : "no");
        return true;
    }

    // Walk the navmesh to the rim over the pool first: from any other point of the rim the fall ends on the ledge.
    float const rimDist = bot->GetExactDist2d(ANIds::DROP_RIM_X, ANIds::DROP_RIM_Y);
    if (rimDist > ANIds::DROP_RIM_TOLERANCE ||
        std::fabs(bot->GetPositionZ() - ANIds::RAMP_BOTTOM_Z) > ANIds::HOLE_FLOOR_Z_TOLERANCE)
    {
        // From the ledge (z 668) the only way down to the rim winds over steep slopes, longer than one PathGenerator
        // path: its shortcut went straight over the hole and PlayerbotAI::GuardAirborneSpline refused it, the leader
        // walked to and fro on the ledge (an-h 2026-10-25). The complete route instead, leg by leg.
        G3D::Vector3 leg;
        if (SelfTravel::NextRouteLeg(bot, _rimRoute, G3D::Vector3(ANIds::DROP_RIM_X, ANIds::DROP_RIM_Y,
                                                                   ANIds::RAMP_BOTTOM_Z),
                                     0.0f, RIM_LEG_STEP, leg))
        {
            if (bot->GetExactDist(leg.x, leg.y, leg.z) > RIM_LEG_REACHED &&
                MoveTo(bot->GetMapId(), leg.x, leg.y, leg.z))
                return true;
            if (bot->isMoving())
                return true;
        }
        return MoveTo(bot->GetMapId(), ANIds::DROP_RIM_X, ANIds::DROP_RIM_Y, ANIds::RAMP_BOTTOM_Z);
    }

    // Check the landing as a player looks down before stepping off: no water there, no drop.
    if (!IsPoolBelow(ANIds::DROP_POOL_X, ANIds::DROP_POOL_Y))
    {
        RaidBotDuty::LogWorkaroundOnce(bot, "azjol-nerub-lower-kingdom",
                                       "needs path fix: no water under the Hadronox shaft drop, refusing the fall");
        return false;
    }

    float const poolDist = bot->GetExactDist2d(ANIds::DROP_POOL_X, ANIds::DROP_POOL_Y);
    if (poolDist <= ANIds::HOLE_CENTRE_TOLERANCE)
        return false;

    // The navmesh ends at the rim: a straight step while there is floor ahead, then a hop that ends over the pool
    // (a straight spline over the void is cut back to the rim by PlayerbotAI::GuardAirborneSpline).
    float const angle = bot->GetAngle(ANIds::DROP_POOL_X, ANIds::DROP_POOL_Y);
    float const step = std::min(ANIds::HOLE_STEP_LENGTH, poolDist);
    float const stepX = bot->GetPositionX() + std::cos(angle) * step;
    float const stepY = bot->GetPositionY() + std::sin(angle) * step;
    if (PlayerbotAI::HasGroundSupport(bot, stepX, stepY, bot->GetPositionZ()))
    {
        bot->GetMotionMaster()->MovePoint(0, stepX, stepY, bot->GetPositionZ(), FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                          false);
        return true;
    }

    bot->StopMoving();
    bot->GetMotionMaster()->MoveJump(ANIds::DROP_POOL_X, ANIds::DROP_POOL_Y, bot->GetPositionZ(),
                                     ANIds::HOLE_JUMP_SPEED_XY, ANIds::HOLE_JUMP_SPEED_Z);
    LOG_INFO("playerbots", "Azjol-Nerub: {} steps off the rim into the hole", bot->GetName());
    return true;
}

bool DropToLowerKingdomAction::SurfaceFromPool()
{
    // The fall ends on the pool's bottom (z 280), a navmesh island of its own; the lower kingdom's navmesh starts on
    // the surface (z 288.6). A player swims up; a bot left down there found no path to Anub'arak (an-n 2026-10-25:
    // boss_unreachable from 529.5,551.0,280.3, a shortcut of 2 points).
    LiquidData const& liquid = bot->GetLiquidData();
    if (liquid.Level <= INVALID_HEIGHT || !bot->IsUnderWater())
        return false;

    return MoveTo(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                  liquid.Level + POOL_SURFACE_ABOVE_LEVEL, false, false, false, true,
                  MovementPriority::MOVEMENT_FORCED) ||
           bot->isMoving();
}

bool DropToLowerKingdomAction::IsPoolBelow(float x, float y) const
{
    Map* map = bot->GetMap();
    float const floorZ =
        map->GetHeight(bot->GetPhaseMask(), x, y, ANIds::RAMP_BOTTOM_Z + 1.0f, true, MAX_FALL_DISTANCE);
    if (floorZ <= INVALID_HEIGHT || floorZ > ANIds::RAMP_BOTTOM_Z - ANIds::HOLE_MIN_DROP)
        return false;

    LiquidData const liquid =
        map->GetLiquidData(bot->GetPhaseMask(), x, y, floorZ + 0.5f, bot->GetCollisionHeight(), {});
    return (liquid.Status & MAP_LIQUID_STATUS_SWIMMING) && liquid.Level - floorZ >= ANIds::POOL_MIN_DEPTH;
}
