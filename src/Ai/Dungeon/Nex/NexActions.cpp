/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NexActions.h"
#include "DungeonInteract.h"
#include "GameObject.h"
#include "Playerbots.h"
#include "RaidBotDuty.h"

bool MoveFromWhirlwindAction::Execute(Event /*event*/)
{
    Unit* boss = nullptr;
    uint8 faction = bot->GetTeamId();
    float targetDist = 10.0f; // Whirlwind has a range of 8, adding a safety buffer

    switch (bot->GetMap()->GetDifficulty())
    {
        case DUNGEON_DIFFICULTY_NORMAL:
            if (faction == TEAM_ALLIANCE)
                boss = AI_VALUE2(Unit*, "find target", "horde commander");

            else // TEAM_HORDE
                boss = AI_VALUE2(Unit*, "find target", "alliance commander");

            break;
        case DUNGEON_DIFFICULTY_HEROIC:
            if (faction == TEAM_ALLIANCE)
                boss = AI_VALUE2(Unit*, "find target", "commander kolurg");

            else // TEAM_HORDE
                boss = AI_VALUE2(Unit*, "find target", "commander stoutbeard");

            break;
        default:
            break;
    }

    // Ensure boss is valid before accessing its methods
    if (!boss)
        return false;

    float bossDistance = bot->GetExactDist2d(boss->GetPosition());

    // Check if the bot is already at a safe distance
    if (bossDistance > targetDist)
        return false;

    // Move away from the boss to avoid Whirlwind
    return MoveAway(boss, targetDist - bossDistance);
}

bool FirebombSpreadAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "grand magus telestra");
    float radius = 5.0f;
    float targetDist = radius + 1.0f;
    if (!boss) { return false; }

    GuidVector members = AI_VALUE(GuidVector, "group members");
    for (auto& member : members)
    {
        Unit* unit = botAI->GetUnit(member);
        if (!unit || bot->GetGUID() == member) { continue; }

        if (bot->GetExactDist2d(unit) < targetDist)
            return MoveAway(unit, targetDist);

    }
    return false;
}

bool TelestraSplitTargetAction::isUseful() { return !PlayerbotAI::IsHeal(bot); }
bool TelestraSplitTargetAction::Execute(Event /*event*/)
{
    GuidVector attackers = AI_VALUE(GuidVector, "attackers");
    Unit* splitTargets[3] = {nullptr, nullptr, nullptr};

    for (auto& attacker : attackers)
    {
        Unit* unit = botAI->GetUnit(attacker);
        if (!unit) { continue; }

        switch (unit->GetEntry())
        {
            // Focus arcane clone first
            case NPC_ARCANE_MAGUS:
                splitTargets[0] = unit;
                break;
            // Then the frost clone
            case NPC_FROST_MAGUS:
                splitTargets[1] = unit;
                break;
            // Fire clone last
            case NPC_FIRE_MAGUS:
                splitTargets[2] = unit;
                break;
        }
    }

    for (Unit* target : splitTargets)
    {
        // Attack the first valid split target in the priority list
        if (target)
        {
            if (AI_VALUE(Unit*, "current target") != target)
                return Attack(target);

            // Don't continue loop here, the target exists so we don't
            // want to move down the prio list. We just don't need to send attack
            // command again, just return false and exit the loop that way
            return false;
        }
    }

    return false;
}

// The damage dealers kill the rifts; the tank holds Anomalus and the Crazed Mana-Wraiths the rifts spawn (nex-h
// 2026-10-25: the tank stood on a rift with 6 attackers and the wraiths went for the casters).
bool ChaoticRiftTargetAction::isUseful() { return !PlayerbotAI::IsHeal(bot) && !PlayerbotAI::IsTank(bot); }
bool ChaoticRiftTargetAction::Execute(Event /*event*/)
{
    Unit* chaoticRift = NexIds::FindChaoticRift(botAI);
    if (!chaoticRift || AI_VALUE(Unit*, "current target") == chaoticRift)
        return false;

    return Attack(chaoticRift);
}

bool DodgeSpikesAction::isUseful()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "ormorok the tree-shaper");
    if (!boss) { return false; }

    return bot->GetExactDist2d(boss) > 0.5f;
}
bool DodgeSpikesAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "ormorok the tree-shaper");
    if (!boss) { return false; }

    return Move(bot->GetAngle(boss), bot->GetExactDist2d(boss) - 0.3f);
}

bool IntenseColdJumpAction::Execute(Event /*event*/)
{
    // This needs improving but maybe it should be done in the playerbot core.
    // Jump doesn't seem to support zero offset (eg. jump on the spot) so need to add a tiny delta.
    // This does a tiny bunnyhop that takes a couple of ms, it doesn't do a natural jump.
    // Adding extra Z offset causes floating, and appears to scale the jump speed based on Z difference.
    // Probably best to revisit once bot movement is improved
    return JumpTo(bot->GetMap()->GetId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() + 0.01f);
    // bot->GetMotionMaster()->MoveFall();
}

namespace
{
// The walk to a containment sphere: legs of a complete route, a leg this close is reached.
constexpr float SPHERE_LEG_STEP = 40.0f;
constexpr float SPHERE_LEG_REACHED = 1.0f;
}  // namespace

bool NexusClickContainmentSphereAction::Execute(Event /*event*/)
{
    NexIds::ContainmentSphere const* sphere = NexIds::FindReadySphere(bot);
    if (!sphere)
        return false;

    if (bot->GetExactDist(sphere->x, sphere->y, sphere->z) > NexIds::SPHERE_SEARCH_RANGE)
    {
        // A complete walk, leg by leg: PathGenerator's partial path to the sphere ended on a steep dead end above
        // Ormorok's room (nex-n/h 2026-10-25: the leader at 285.1,-176.8,7.4 for 4 minutes). A route ending within
        // the search range counts (the sphere's floor may be a navmesh island of its own).
        G3D::Vector3 leg;
        if (SelfTravel::NextRouteLeg(bot, _route, G3D::Vector3(sphere->x, sphere->y, sphere->z),
                                     NexIds::SPHERE_SEARCH_RANGE, SPHERE_LEG_STEP, leg))
        {
            if (bot->GetExactDist(leg.x, leg.y, leg.z) > SPHERE_LEG_REACHED &&
                MoveTo(bot->GetMapId(), leg.x, leg.y, leg.z))
                return true;
            if (bot->isMoving())
                return true;
            return false;
        }
        // No complete route: PathGenerator's partial path is the steep dead end above Ormorok's room, not a walk.
        RaidBotDuty::LogWorkaroundOnce(bot, "nexus-sphere-route",
                                       "needs path fix: no complete navmesh route to containment sphere " +
                                           std::to_string(sphere->goEntry));
        return false;
    }

    GameObject* go = bot->FindNearestGameObject(sphere->goEntry, NexIds::SPHERE_SEARCH_RANGE);
    if (!go || !go->isSpawned() || go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE))
    {
        RaidBotDuty::LogWorkaroundOnce(bot, "nexus-containment-sphere",
                                       "needs honest tactic: Nexus containment sphere " +
                                           std::to_string(sphere->goEntry) +
                                           " is missing or not selectable although its boss is dead");
        return false;
    }

    if (!go->IsWithinDistInMap(bot, go->GetInteractionDistance()))
    {
        if (MoveNear(go, NexIds::SPHERE_USE_DISTANCE))
            return true;

        return MoveTo(go->GetMapId(), go->GetPositionX(), go->GetPositionY(), go->GetPositionZ());
    }

    bot->StopMoving();
    if (!DungeonInteract::UseGameObject(bot, go))
        return false;

    LOG_INFO("playerbots", "Nexus: {} used containment sphere {}", bot->GetName(), sphere->goEntry);
    return true;
}
