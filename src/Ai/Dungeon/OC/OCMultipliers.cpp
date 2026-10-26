/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "OCMultipliers.h"
#include "CheckMountStateAction.h"
#include "ChooseTargetActions.h"
#include "FollowActions.h"
#include "GenericSpellActions.h"
#include "MovementActions.h"
#include "OCActions.h"
#include "OCTriggers.h"
#include "Playerbots.h"
#include "ReachTargetActions.h"
#include "StringFormat.h"
#include "TbcHazard.h"

float MountingDrakeMultiplier::GetValue(Action* action)
{
    // P.I.T.A bug where the bots will somehow interrupt their item spell use,
    // even though the 0.5 sec cast goes off, it puts the drake essence on 15 sec cd
    // and no drake comes down.
    // It seems like this is due to moving/other actions being processed during the 0.5 secs.
    // If we suppress everything, they seem to mount properly. A bit of a ham-fisted solution but it works
    if (!OculusShouldMountDrake(botAI))
        return 1.0f;

    if (!dynamic_cast<MountDrakeAction*>(action))
        return 0.0f;

    return 1.0f;
}

float OccPortalMountMultiplier::GetValue(Action* action)
{
    // [b33_occ] ... nor within a pullable boss' range on his island (OCRoute::GetPullableBoss: 40 yd, same level):
    // "oc pull boss" (61) dismounts and returns, then "check mount state" (100) - no current target, so shouldMount -
    // mounts again, every tick. occ-h 20261026-000434-occ-h-5C-21: the tank stood 27.4 yd from Drakos from 302 s
    // to the stop (~25 "[oc-pull-boss-dismount]" lines), never walked in; the harness' waypoint (relevance 11) was
    // starved too, Drakos was skipped "route end" and the harness wandered to the ring mobs. A player walks the
    // last 40 yd to a boss on foot. A mounted bot is left alone: "oc pull boss" dismounts it.
    if (dynamic_cast<CheckMountStateAction*>(action) && bot->GetMapId() == OCULUS_MAP_ID && bot->IsAlive() &&
        !bot->IsInCombat() && !bot->GetVehicleBase() && !bot->IsMounted() && OCRoute::GetPullableBoss(bot))
        return 0.0f;

    if (!dynamic_cast<CheckMountStateAction*>(action) || bot->GetMapId() != OCULUS_MAP_ID || !bot->IsAlive() ||
        bot->IsInCombat() || bot->GetVehicleBase() || !OCRoute::IsOnEntranceIsland(bot))
        return 1.0f;

    bool const atPortal =
        bot->GetExactDist2d(OCIds::NEXUS_PORTAL_X, OCIds::NEXUS_PORTAL_Y) <= OCIds::NEXUS_PORTAL_FOOT_RANGE;
    return atPortal ? 0.0f : 1.0f;
}

float OccFlyingMultiplier::GetValue(Action* action)
{
    if (bot->GetMapId() != OCULUS_MAP_ID || !bot->GetVehicleBase()) { return 1.0f; }

    // Suppresses FollowAction as well as some attack-based movements
    // [b36_occ] ... but the Planar Anomaly dodge steers the drake itself.
    if (dynamic_cast<MovementAction*>(action) && !dynamic_cast<OccFlyDrakeAction*>(action) &&
        !dynamic_cast<AvoidPlanarAnomalyAction*>(action))
        return 0.0f;

    return 1.0f;
}

float UromMultiplier::GetValue(Action* action)
{
    if (GetPhaseByCurrentPosition(bot) < 3)
    {
        Unit* target = action->GetTarget();
        // The first hit on a platform is what makes Urom summon his menagerie and move on.
        if (target && target->GetEntry() == NPC_MAGE_LORD_UROM && !dynamic_cast<OcPullBossAction*>(action))
            return 0.0f;
    }

    Unit* boss = AI_VALUE2(Unit*, "find target", "mage-lord urom");
    if (!boss) { return 1.0f; }

    // REAL BOSS FIGHT
    if (boss->HasUnitState(UNIT_STATE_CASTING) &&
        boss->FindCurrentSpellBySpellId(SPELL_EMPOWERED_ARCANE_EXPLOSION))
    {
        if (dynamic_cast<MovementAction*>(action) && !dynamic_cast<AvoidArcaneExplosionAction*>(action))
            return 0.0f;
    }

    // Don't bother avoiding Frostbomb for melee
    if (PlayerbotAI::IsMelee(bot))
    {
        if (dynamic_cast<AvoidAoeAction*>(action))
            return 0.0f;
    }

    if (bot->HasAura(SPELL_TIME_BOMB))
    {
        if (dynamic_cast<MovementAction*>(action) && !dynamic_cast<TimeBombSpreadAction*>(action))
            return 0.0f;
    }

    return 1.0f;
}

uint8 UromMultiplier::GetPhaseByCurrentPosition(Unit* unit)
{
    // Distance to return a positive match for spawn platforms, tweak slightly if needed/
    // Make sure this doesn't get too large and reach the central ring as well
    float distance = 60.0f;

    for (uint8 i = 0; i < 3; ++i)
    {
        if (unit->GetDistance(uromCoords[i][0], uromCoords[i][1], uromCoords[i][2]) < distance)
            return i;
    }
    return 3;
}

float EregosMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "ley-guardian eregos");
    if (!boss) { return 1.0f; }

    if (boss->HasAura(SPELL_PLANAR_SHIFT) && dynamic_cast<OccDrakeAttackAction*>(action))
        return 0.0f;

    return 1.0f;
}

namespace
{
constexpr float RING_GATHER_RANGE = 30.0f;
constexpr uint32 RING_GATHER_TIMEOUT_MS = 90 * IN_MILLISECONDS;

bool IsPullStarterName(std::string const& name)
{
    static constexpr std::string_view starters[] = {"aggressive target", "attack anything", "pull start",
                                                    "pull action",       "reach pull",      "attack rti target",
                                                    "tank assist",       "dps assist"};
    return std::find(std::begin(starters), std::end(starters), name) != std::end(starters);
}
}  // namespace

float OccRingGatherMultiplier::GetValue(Action* action)
{
    if (bot->GetMapId() != OCULUS_MAP_ID || bot->IsInCombat() || !PlayerbotAI::IsTank(bot) || bot->GetVehicleBase())
    {
        _waitSinceMs = 0;  // every wait starts afresh after a fight or a flight
        return 1.0f;
    }
    if (!IsPullStarterName(action->getName()))
        return 1.0f;

    Group* group = bot->GetGroup();
    if (!group)
        return 1.0f;

    Player* away = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref && !away; ref = ref->next())
    {
        Player* member = ref->GetSource();
        // A member stranded below the islands never comes (occ-h 2026-10-25 150737: the mage at z 207 held every
        // pull of the tank on the west ring landing from 576 s until the harness' stall skipped all 27 ring mobs).
        if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
            OCRoute::IsStrandedBelowIslands(member))
            continue;

        // Still on its drake, or not here yet: the pull waits for it.
        if (member->GetVehicleBase() || bot->GetExactDist(member) > RING_GATHER_RANGE)
            away = member;
    }

    uint32 const now = getMSTime();
    if (!away)
    {
        _waitSinceMs = 0;
        return 1.0f;
    }

    // Waited for long enough (a member that cannot come in any other way): the pull goes on without him.
    if (!_waitSinceMs)
        _waitSinceMs = now ? now : 1;
    if (getMSTimeDiff(_waitSinceMs, now) < RING_GATHER_TIMEOUT_MS)
        return 0.0f;

    if (OCRoute::ShouldLog(bot, "ring-gather-timeout", 10 * IN_MILLISECONDS))
        LOG_INFO("playerbots", "[oc ring gather timeout] {}: pulls ({}) without {} after {} s ({:.1f} yd{})",
                 bot->GetName(), action->getName(), away->GetName(), RING_GATHER_TIMEOUT_MS / IN_MILLISECONDS,
                 bot->GetExactDist(away), away->GetVehicleBase() ? ", on its drake" : "");
    return 1.0f;
}

namespace
{
constexpr float HEALER_GATHER_RANGE = 40.0f;
constexpr uint32 OC_MULTIPLIER_CACHE_MS = 500;
}  // namespace

bool OccHealerGatherMultiplier::IsHealerAway()
{
    uint32 const now = getMSTime();
    if (_checkedMs && getMSTimeDiff(_checkedMs, now) < OC_MULTIPLIER_CACHE_MS)
        return _healerAway;

    _checkedMs = now ? now : 1;
    _healerAway = false;
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    auto const here = [this](Player* member)
    {
        return member && member->IsAlive() && member->IsInWorld() && member->GetMapId() == bot->GetMapId() &&
               member->GetInstanceId() == bot->GetInstanceId();
    };

    Player* tank = PlayerbotAI::IsTank(bot) ? bot : nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref && !tank; ref = ref->next())
        if (Player* member = ref->GetSource(); here(member) && PlayerbotAI::IsTank(member))
            tank = member;
    if (!tank)
        tank = bot;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* healer = ref->GetSource();
        if (!here(healer) || healer == tank || !PlayerbotAI::IsHeal(healer))
            continue;

        bool const flying = healer->GetVehicleBase() != nullptr;
        float const distance = healer->GetExactDist(tank);
        if (!flying && distance <= HEALER_GATHER_RANGE)
            continue;

        _healerAway = true;
        _awayWhy = Acore::StringFormat("healer {} is {:.1f} yd from tank {}{}", healer->GetName(), distance,
                                       tank->GetName(), flying ? " and still on its drake" : "");
        break;
    }
    return _healerAway;
}

float OccHealerGatherMultiplier::GetValue(Action* action)
{
    if (bot->GetMapId() != OCULUS_MAP_ID || !bot->IsAlive() || bot->IsInCombat() || bot->GetVehicleBase())
        return 1.0f;

    bool const aggressive = dynamic_cast<AggressiveTargetAction*>(action) != nullptr;
    bool const named = !aggressive && (dynamic_cast<OcPullBossAction*>(action) || IsPullStarterName(action->getName()));
    bool const attack = !aggressive && !named && dynamic_cast<AttackAction*>(action);
    if (!aggressive && !((named || attack) && PlayerbotAI::IsTank(bot)))
        return 1.0f;

    if (!IsHealerAway())
        return 1.0f;

    // The tank still answers a mob that is already fighting (an add on a member, a patrol that walked in).
    if (attack)
    {
        Unit* target = action->GetTarget();
        if (!target || !target->IsAlive() || target->IsInCombat())
            return 1.0f;
    }

    if (OCRoute::ShouldLog(bot, "healer-gate", 10 * IN_MILLISECONDS))
        LOG_INFO("playerbots", "[oc healer gate] {}: no pull ({}), {}", bot->GetName(), action->getName(), _awayWhy);
    return 0.0f;
}

float OccLandingHoldMultiplier::GetValue(Action* action)
{
    if (bot->GetMapId() != OCULUS_MAP_ID || bot->GetVehicleBase() || bot->IsInCombat() ||
        !dynamic_cast<MovementAction*>(action) || dynamic_cast<OcHoldLandingSpotAction*>(action) ||
        dynamic_cast<TbcHazardMoveAction*>(action))
        return 1.0f;

    uint32 const now = getMSTime();
    if (!_checkedMs || getMSTimeDiff(_checkedMs, now) >= OC_MULTIPLIER_CACHE_MS)
    {
        _checkedMs = now ? now : 1;
        _holding = OculusShouldHoldLanding(botAI);
    }
    return _holding ? 0.0f : 1.0f;
}
