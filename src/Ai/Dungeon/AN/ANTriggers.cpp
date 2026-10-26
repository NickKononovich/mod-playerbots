/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ANTriggers.h"
#include "ANAddTactics.h"  // [b33_hadronox2] ANAdds::HadronoxFailedAttempts
#include "AiObjectContext.h"
#include "CreatureGroups.h"
#include "DungeonInteract.h"
#include "InstanceScript.h"
#include "MoveConflict.h"  // [b34_an2] the rally as the group's event hold
#include "Playerbots.h"
#include "PullStrategy.h"
#include "RaidLeaderState.h"

#include <algorithm>
#include <array>
#include <unordered_map>

namespace ANWatcherRoom
{
namespace
{
constexpr std::array<uint32, 3> WATCHERS = {NPC_WATCHER_NARJIL, NPC_WATCHER_GASHRA, NPC_WATCHER_SILTHIK};

bool IsWatcher(uint32 entry) { return std::find(WATCHERS.begin(), WATCHERS.end(), entry) != WATCHERS.end(); }

// A Watcher, or a member of a Watcher's formation (instance_azjol_nerub.cpp OnUnitDeath checks it the same way).
bool IsRoomMob(Creature* creature)
{
    if (IsWatcher(creature->GetEntry()))
        return true;

    CreatureGroup* formation = creature->GetFormation();
    Creature* leader = formation ? formation->GetLeader() : nullptr;
    return leader && IsWatcher(leader->GetEntry());
}

bool InRoom(Player* bot)
{
    return bot->GetMapId() == MAP_AZJOL_NERUB && bot->GetPositionZ() > ROOM_MIN_Z &&
           bot->GetPositionZ() < ROOM_MAX_Z && bot->GetExactDist2d(KRIK_X, KRIK_Y) < ROOM_RADIUS;
}

bool GroupInCombat(Player* bot)
{
    if (bot->IsInCombat())
        return true;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (DungeonInteract::IsActiveMember(bot, member) && member->IsInCombat())
            return true;
    }
    return false;
}

void Compute(PlayerbotAI* botAI, Snapshot& snapshot)
{
    snapshot.active = false;
    snapshot.fighting = false;
    snapshot.otherTrash = false;
    snapshot.packFighting = false;  // [b34_an2]
    snapshot.watchersAlive = 0;
    snapshot.pullTarget.Clear();
    snapshot.idle.clear();

    Player* bot = botAI->GetBot();
    if (!bot->IsAlive() || !InRoom(bot))
        return;

    Creature* krik = bot->FindNearestCreature(NPC_KRIKTHIR, SCAN_RANGE, true);
    if (!krik || krik->IsInCombat())
        return;

    snapshot.fighting = GroupInCombat(bot);
    float bestDist = 0.0f;
    for (uint32 entry : WATCHERS)
    {
        Creature* watcher = bot->FindNearestCreature(entry, SCAN_RANGE, true);
        if (!watcher)
            continue;

        ++snapshot.watchersAlive;
        std::vector<Creature*> pack;
        if (CreatureGroup* formation = watcher->GetFormation())
        {
            for (auto const& [member, info] : formation->GetMembers())
                if (member && member->IsAlive() && member->IsInWorld())
                    pack.push_back(member);
        }
        if (std::find(pack.begin(), pack.end(), watcher) == pack.end())
            pack.push_back(watcher);

        if (std::any_of(pack.begin(), pack.end(), [](Creature* member) { return member->IsInCombat(); }))
        {
            snapshot.fighting = true;
            snapshot.packFighting = true;  // [b34_an2]
            continue;
        }

        for (Creature* member : pack)
        {
            snapshot.idle.emplace_back(member->GetPositionX(), member->GetPositionY(), member->GetPositionZ());
            float const dist = member->GetExactDist2d(RALLY_X, RALLY_Y);
            if (snapshot.pullTarget.IsEmpty() || dist < bestDist)
            {
                snapshot.pullTarget = member->GetGUID();
                bestDist = dist;
            }
        }
    }

    snapshot.active = snapshot.watchersAlive > 0;
    if (!snapshot.active)
        return;

    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets")->Get())
    {
        Creature* creature = botAI->GetCreature(guid);
        if (!creature || !creature->IsAlive() || creature->IsInCombat() || creature == krik || IsRoomMob(creature))
            continue;

        if (creature->GetExactDist2d(RALLY_X, RALLY_Y) < OTHER_TRASH_RADIUS)
        {
            snapshot.otherTrash = true;
            break;
        }
    }
}
}  // namespace

Snapshot const& Get(PlayerbotAI* botAI)
{
    // Map threads update their bots in parallel: one cache per thread, keyed by the bot.
    thread_local std::unordered_map<uint64, Snapshot> cache;
    Player* bot = botAI->GetBot();
    Snapshot& snapshot = cache[bot->GetGUID().GetRawValue()];
    uint32 const now = getMSTime();
    if (snapshot.ms && getMSTimeDiff(snapshot.ms, now) < CACHE_MS)
        return snapshot;

    snapshot.ms = now;
    Compute(botAI, snapshot);
    if (snapshot.fighting)
        snapshot.lastFightMs = now;

    return snapshot;
}

bool OwnsPull(PlayerbotAI* botAI)
{
    Snapshot const& snapshot = Get(botAI);
    return snapshot.active && !snapshot.fighting && !snapshot.otherTrash;
}

bool WaitsForSend(PlayerbotAI* botAI)
{
    Snapshot const& snapshot = Get(botAI);
    // All three Watchers alive: no pack has died, nothing is on its way.
    return snapshot.active && snapshot.watchersAlive < WATCHERS.size() && snapshot.lastFightMs &&
           getMSTimeDiff(snapshot.lastFightMs, getMSTime()) < SEND_GAP_MS;
}

bool IdleNear(Snapshot const& snapshot, float x, float y, float radius)
{
    return std::any_of(snapshot.idle.begin(), snapshot.idle.end(),
                       [x, y, radius](Position const& mob) { return mob.GetExactDist2d(x, y) < radius; });
}

bool ApproachUnsafe(PlayerbotAI* botAI, Unit* target)
{
    if (!target)
        return false;

    Snapshot const& snapshot = Get(botAI);
    if (!snapshot.active || snapshot.idle.empty())
        return false;

    Player* bot = botAI->GetBot();
    float const reach = PlayerbotAI::IsRanged(bot) ? RANGED_STAND : MELEE_STAND;
    float const dist = bot->GetExactDist2d(target);
    if (dist <= reach)
        return false;

    // Where the walk to the target ends: on the line to it, `reach` short of it.
    float const share = reach / dist;
    float const x = target->GetPositionX() + (bot->GetPositionX() - target->GetPositionX()) * share;
    float const y = target->GetPositionY() + (bot->GetPositionY() - target->GetPositionY()) * share;
    return IdleNear(snapshot, x, y, AGGRO_DANGER);
}

bool IsPullingTank(Player* bot)
{
    if (!PlayerbotAI::IsTank(bot))
        return false;

    if (PlayerbotAI::IsMainTank(bot))
        return true;

    Group* group = bot->GetGroup();
    if (!group)
        return true;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member != bot && DungeonInteract::IsActiveMember(bot, member) && PlayerbotAI::IsMainTank(member))
            return false;
    }
    return true;
}

// [b34_an2]
bool HoldsRally(PlayerbotAI* botAI)
{
    Snapshot const& snapshot = Get(botAI);
    if (!snapshot.active)
        return false;

    // Trash of its own near the rally is the planner's pull first (OwnsPull): its walk is not held before the phase.
    return snapshot.packFighting || snapshot.watchersAlive < WATCHERS.size() || !snapshot.otherTrash;
}
}  // namespace ANWatcherRoom

bool KrikthirWatcherPullTrigger::IsActive()
{
    if (bot->IsInCombat() || !ANWatcherRoom::IsPullingTank(bot) || !ANWatcherRoom::OwnsPull(botAI))
        return false;

    ANWatcherRoom::Snapshot const& snapshot = ANWatcherRoom::Get(botAI);
    if (snapshot.pullTarget.IsEmpty() || ANWatcherRoom::WaitsForSend(botAI))
        return false;

    // The group's coordinator holds every new pull (ready check, buffs, a break): so does this one.
    if (RaidLeader::IsEnabled() && AI_VALUE(bool, "pull hold"))
        return false;

    // The pull goes through the tank's pull strategy; not while a pull of its own is already under way.
    PullStrategy* strategy = PullStrategy::Get(botAI);
    return strategy && !strategy->HasTarget() && !strategy->IsPullPendingToStart() && !strategy->HasPullStarted();
}

bool KrikthirWatcherDriftTrigger::IsActive()
{
    if (!bot->IsAlive())
        return false;

    ANWatcherRoom::Snapshot const& snapshot = ANWatcherRoom::Get(botAI);
    if (!snapshot.active || snapshot.idle.empty() ||
        !ANWatcherRoom::IdleNear(snapshot, bot->GetPositionX(), bot->GetPositionY(), ANWatcherRoom::DRIFT_RADIUS))
        return false;

    // The tank walking to its own pull's cast spot.
    PullStrategy* strategy = PullStrategy::Get(botAI);
    return !strategy || (!strategy->HasTarget() && !strategy->IsPullPendingToStart() && !strategy->HasPullStarted());
}

// [b34_an2] Each sent Watcher reaches the group alone only where the group stands out of the idle packs' reach: the
// rally, between sends, while resting and after each pack (an-n 20261026-014521-an-n-5G-25: after Silthik's pack
// died at (549,680) nobody walked back; Narjil came there past Gashra, 11 mobs, 3 dead).
bool KrikthirWatcherRallyTrigger::IsActive()
{
    if (!bot->IsAlive() || !ANWatcherRoom::HoldsRally(botAI))
        return false;

    // Every member evaluates this: the harness waypoint walk and a LoS pull do not take the group off the rally
    // (the harness's pull_after_hold body pull walked the tank 14 yd off it into Silthik's pack).
    MoveConflict::NoteEventHold(bot, ANWatcherRoom::RALLY_X, ANWatcherRoom::RALLY_Y, ANWatcherRoom::RALLY_Z,
                                ANWatcherRoom::RALLY_HOLD_RADIUS, "krik'thir watcher rally");

    if (bot->IsInCombat() ||
        bot->GetExactDist2d(ANWatcherRoom::RALLY_X, ANWatcherRoom::RALLY_Y) <= ANWatcherRoom::RALLY_HOLD_RADIUS)
        return false;

    // The tank walking to its own pull's cast spot and back (KrikthirWatcherPullAction).
    PullStrategy* strategy = PullStrategy::Get(botAI);
    return !strategy || (!strategy->HasTarget() && !strategy->IsPullPendingToStart() && !strategy->HasPullStarted());
}

bool KrikthirWebWrapTrigger::IsActive()
{
    if (!PlayerbotAI::IsDps(bot)) { return false; }

    // Target is not findable from threat table using AI_VALUE2(),
    // therefore need to search manually for the unit name
    GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");

    for (auto i = targets.begin(); i != targets.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (unit && unit->GetEntry() == NPC_WEB_WRAP)
        {
            return true;
        }
    }

    return false;
}

bool KrikthirWatchersTrigger::IsActive()
{
    if (!PlayerbotAI::IsDps(bot)) { return false; }

    // Target is not findable from threat table using AI_VALUE2(),
    // therefore need to search manually for the unit name
    GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");

    for (auto i = targets.begin(); i != targets.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (unit && unit->GetEntry() == NPC_KRIKTHIR)
        {
            return true;
        }
    }
    return false;
}

bool AnubarakImpaleTrigger::IsActive()
{
    if (!bot->IsInCombat())
        return false;

    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetBossState(ANIds::DATA_ANUBARAK) != IN_PROGRESS)
        return false;

    return bot->FindNearestCreature(ANIds::NPC_IMPALE_TARGET, ANIds::IMPALE_DANGER_RADIUS) != nullptr;
}

bool ANIds::IsBelowHadronoxPlatform(Player* bot, Unit const* unit)
{
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetBossState(DATA_HADRONOX) != IN_PROGRESS)
        return false;

    return unit->GetPositionZ() < HADRONOX_PLATFORM_MIN_Z && unit->GetPositionZ() > UPPER_LEVEL_MIN_Z &&
           unit->GetExactDist2d(HADRONOX_PLATFORM_X, HADRONOX_PLATFORM_Y) < HADRONOX_SHAFT_RADIUS;
}

bool HadronoxPlatformTrigger::IsActive()
{
    return bot->IsAlive() && ANIds::IsBelowHadronoxPlatform(bot, bot);
}

bool AzjolNerubLowerKingdomTrigger::IsActive()
{
    if (!bot->IsAlive() ||
        bot->GetExactDist2d(ANIds::LOWER_KINGDOM_HOLE_X, ANIds::LOWER_KINGDOM_HOLE_Y) > ANIds::HADRONOX_SHAFT_RADIUS)
        return false;

    // Still falling, or landed with the fall flag left over (no client clears it).
    if (bot->HasUnitMovementFlag(MOVEMENTFLAG_FALLING))
        return true;

    // Landed on the pool's bottom under the hole: up to the surface first (DropToLowerKingdomAction).
    if (bot->GetPositionZ() < ANIds::LOWER_LEVEL_MAX_Z)
        return bot->GetExactDist2d(ANIds::DROP_POOL_X, ANIds::DROP_POOL_Y) <= ANIds::POOL_SURFACE_RADIUS &&
               bot->IsUnderWater();

    if (bot->IsInCombat() || bot->GetPositionZ() < ANIds::UPPER_LEVEL_MIN_Z)
        return false;

    InstanceScript* instance = bot->GetInstanceScript();
    // [b33_hadronox2] Or she is given up: HADRONOX_FAILED_ATTEMPTS_DROP encounter resets in this instance and not in
    // a fight now (Anub'arak's doors do not depend on her, instance_azjol_nerub.cpp doorData). an-h 20261026-000434-
    // an-h-5E-35: boss_failed after 3 resets, then the group stood 120 s at the hole waiting for DONE.
    if (!instance || instance->GetBossState(ANIds::DATA_ANUBARAK) == DONE)
        return false;
    uint32 const hadronoxState = instance->GetBossState(ANIds::DATA_HADRONOX);
    if (hadronoxState != DONE &&
        (hadronoxState == IN_PROGRESS || ANAdds::HadronoxFailedAttempts(bot) < ANAdds::HADRONOX_FAILED_ATTEMPTS_DROP))
        return false;

    // Follow members that already dropped; otherwise the interactor leads once the group stands together.
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (DungeonInteract::IsActiveMember(bot, member) && member->GetPositionZ() < ANIds::LOWER_LEVEL_MAX_Z)
                return true;
        }
    }

    return DungeonInteract::IsInteractor(botAI) &&
           DungeonInteract::IsGroupGathered(bot, bot, ANIds::DROP_GATHER_RADIUS);
}

bool AnubarakFrontalTrigger::IsActive()
{
    if (PlayerbotAI::IsTank(bot) || !bot->IsInCombat())
        return false;

    Unit* boss = AI_VALUE2(Unit*, "find target", "anub'arak");
    if (!boss || !boss->IsAlive() || !boss->IsVisible() || boss->GetVictim() == bot)
        return false;

    return bot->GetExactDist2d(boss) <= ANIds::POUND_REACH && boss->HasInArc(ANIds::POUND_SAFE_ARC, bot);
}

bool AnubarakPoundTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "anub'arak");
    if (!boss) { return false; }

    return boss->HasUnitState(UNIT_STATE_CASTING) && boss->FindCurrentSpellBySpellId(SPELL_POUND);
}
