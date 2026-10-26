/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AKTriggers.h"
#include "AiObjectContext.h"
#include "DungeonInteract.h"
#include "InstanceScript.h"
#include "ObjectAccessor.h"
#include "Playerbots.h"
#include "RaidLeaderState.h"

#include <cmath>
#include <list>
#include <unordered_map>

bool NadoxGuardianTrigger::IsActive()
{
    if (PlayerbotAI::IsHeal(bot)) { return false; }

    Unit* boss = AI_VALUE2(Unit*, "find target", "elder nadox");
    Unit* guardian = AI_VALUE2(Unit*, "find target", "ahn'kahar guardian");

    return boss && guardian;
}

bool JedogaVolunteerTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "jedoga shadowseeker");
    if (!boss) { return false; }

    // Volunteer is not findable from threat table using AI_VALUE2(),
    // therefore need to search manually for the unit name
    GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");

    for (auto i = targets.begin(); i != targets.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (unit && unit->GetEntry() == NPC_TWILIGHT_VOLUNTEER)
        {
            return true;
        }
    }
    return false;
}

bool ShadowCrashTrigger::IsActive()
{
    Unit* unit = AI_VALUE2(Unit*, "find target", "forgotten one");
    if (!unit) { return false; }

    return !PlayerbotAI::IsMelee(bot);
}

AKIds::NerubianDevice const* AKIds::FindPendingDevice(Player* bot)
{
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetBossState(DATA_PRINCE_TALDARAM) == DONE)
        return nullptr;

    // The devices sit on Taldaram's platform, past Elder Nadox: a player kills Nadox first (wave 10: the device
    // action took every tick from the entrance and the leader never walked).
    if (instance->GetBossState(DATA_ELDER_NADOX) != DONE)
        return nullptr;

    NerubianDevice const* nearest = nullptr;
    float nearestDistSq = 0.0f;
    for (auto const& device : NERUBIAN_DEVICES)
    {
        if (instance->GetPersistentData(device.persistentIndex) == DONE)
            continue;

        float const distSq = bot->GetExactDistSq(device.x, device.y, device.z);
        if (!nearest || distSq < nearestDistSq)
        {
            nearest = &device;
            nearestDistSq = distSq;
        }
    }
    return nearest;
}

bool TaldaramNerubianDeviceTrigger::IsActive()
{
    if (bot->IsInCombat() || !AKIds::FindPendingDevice(bot))
        return false;

    return DungeonInteract::IsInteractor(botAI);
}

namespace AKJedogaRoom
{
namespace
{
void Compute(PlayerbotAI* botAI, Snapshot& snapshot)
{
    snapshot.active = false;
    snapshot.initiatesAlive = 0;
    snapshot.visibleTarget.Clear();
    snapshot.nearestTarget.Clear();

    Player* bot = botAI->GetBot();
    if (!bot->IsAlive() || bot->GetMapId() != MAP_AHNKAHET || bot->GetPositionZ() < ROOM_MIN_Z ||
        bot->GetPositionZ() > ROOM_MAX_Z || bot->GetExactDist2d(ALTAR_X, ALTAR_Y) > ROOM_RADIUS)
        return;

    // Once she is engaged the initiates are gone (DespawnOOCSummons) and the fight is hers.
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance)
        return;

    EncounterState const state = instance->GetBossState(AKIds::DATA_JEDOGA_SHADOWSEEKER);
    if (state == DONE || state == IN_PROGRESS)
        return;

    std::list<Creature*> initiates;
    bot->GetCreatureListWithEntryInGrid(initiates, AKIds::NPC_TWILIGHT_INITIATE, ROOM_RADIUS * 2.0f);
    float nearestDist = 0.0f;
    float visibleDist = 0.0f;
    for (Creature* initiate : initiates)
    {
        if (!initiate || !initiate->IsAlive() || initiate->GetExactDist2d(ALTAR_X, ALTAR_Y) > ROOM_RADIUS)
            continue;

        ++snapshot.initiatesAlive;
        float const dist = bot->GetExactDist(initiate);
        if (snapshot.nearestTarget.IsEmpty() || dist < nearestDist)
        {
            snapshot.nearestTarget = initiate->GetGUID();
            nearestDist = dist;
        }

        // AttackAction refuses a target out of sight (AttackAction.cpp): the nearest one by distance can sit
        // behind the altar's pillars while another is in plain view.
        if ((snapshot.visibleTarget.IsEmpty() || dist < visibleDist) && bot->IsWithinLOSInMap(initiate))
        {
            snapshot.visibleTarget = initiate->GetGUID();
            visibleDist = dist;
        }
    }

    snapshot.active = snapshot.initiatesAlive > 0;
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
    return snapshot;
}

bool OwnsPull(PlayerbotAI* botAI) { return Get(botAI).active; }
}  // namespace AKJedogaRoom

bool JedogaInitiatesTrigger::IsActive()
{
    if (bot->IsInCombat() || !AKJedogaRoom::OwnsPull(botAI))
        return false;

    // The group's coordinator holds every new pull (ready check, buffs, a break): so does this one.
    if (RaidLeader::IsEnabled() && AI_VALUE(bool, "pull hold"))
        return false;

    return PlayerbotAI::IsTank(bot) || DungeonInteract::IsInteractor(botAI);
}

namespace AKShared
{
bool IsInInsanity(Player* bot)
{
    for (uint32 spell : AKIds::SPELL_INSANITY_PHASING)
        if (bot->HasAura(spell))
            return true;

    return false;
}

Unit* FindVisibleVisage(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Unit* weakest = nullptr;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        if (guid.GetEntry() != AKIds::NPC_TWISTED_VISAGE)
            continue;

        // "attackers" also lists the visages that fight the other members in their own phases
        Unit* visage = botAI->GetUnit(guid);
        if (!visage || !visage->IsAlive() || !visage->InSamePhase(bot) || !bot->CanSeeOrDetect(visage))
            continue;

        if (!weakest || visage->GetHealthPct() < weakest->GetHealthPct())
            weakest = visage;
    }
    return weakest;
}
}  // namespace AKShared

bool VolazjInsanityTrigger::IsActive()
{
    if (!AKShared::IsInInsanity(bot))
        return false;

    // Nobody can heal across the phases: a hurt healer keeps healing itself.
    if (PlayerbotAI::IsHeal(bot) && bot->GetHealthPct() < AKIds::INSANITY_HEALER_SELF_PCT)
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (target && target->IsAlive() && target->GetEntry() == AKIds::NPC_TWISTED_VISAGE && target->InSamePhase(bot))
        return false;

    return AKShared::FindVisibleVisage(botAI) != nullptr;
}

// [b33_ok] Amanitar: the mushrooms around the bot and the Healthy one it walks to while Mini is on it.
namespace AKAmanitar
{
namespace
{
bool PoisonNear(Player* bot, Snapshot const& snapshot, Creature const* healthy)
{
    for (ObjectGuid const& guid : snapshot.poisonous)
    {
        Creature* poisonous = ObjectAccessor::GetCreature(*bot, guid);
        if (poisonous && poisonous->IsAlive() && poisonous->GetExactDist2d(healthy) < POISON_CLEARANCE)
            return true;
    }
    return false;
}

float RoleReach(Player* bot)
{
    if (PlayerbotAI::IsTank(bot))
        return TANK_REACH;

    return PlayerbotAI::IsMelee(bot) ? MELEE_REACH : RANGED_REACH;
}

void Compute(PlayerbotAI* botAI, Snapshot& snapshot)
{
    snapshot.active = false;
    snapshot.healthy.clear();
    snapshot.poisonous.clear();
    snapshot.fungusTarget.Clear();

    Player* bot = botAI->GetBot();
    if (!bot->IsAlive() || !bot->IsInCombat())
        return;

    Creature* boss = bot->FindNearestCreature(NPC_AMANITAR, SCAN_RANGE, true);
    if (!boss || !boss->IsInCombat())
        return;

    snapshot.active = true;
    for (uint32 entry : {NPC_HEALTHY_MUSHROOM, NPC_POISONOUS_MUSHROOM})
    {
        std::list<Creature*> mushrooms;
        bot->GetCreatureListWithEntryInGrid(mushrooms, entry, SCAN_RANGE);
        GuidVector& guids = entry == NPC_HEALTHY_MUSHROOM ? snapshot.healthy : snapshot.poisonous;
        for (Creature* mushroom : mushrooms)
            if (mushroom && mushroom->IsAlive())
                guids.push_back(mushroom->GetGUID());
    }

    if (!NeedsFungus(bot))
        return;

    float const reach = RoleReach(bot);
    float bestDist = 0.0f;
    for (ObjectGuid const& guid : snapshot.healthy)
    {
        Creature* healthy = ObjectAccessor::GetCreature(*bot, guid);
        if (!healthy || !healthy->IsAlive())
            continue;

        float const dist = bot->GetExactDist2d(healthy);
        if (dist > reach || std::fabs(healthy->GetPositionZ() - bot->GetPositionZ()) > MAX_DZ ||
            (!snapshot.fungusTarget.IsEmpty() && dist >= bestDist))
            continue;

        // A Poisonous Mushroom next to it would cloud the bot on the way in (3 yd sight trigger, 5 yd cloud).
        if (PoisonNear(bot, snapshot, healthy) || !bot->IsWithinLOSInMap(healthy))
            continue;

        snapshot.fungusTarget = guid;
        bestDist = dist;
    }
}
}  // namespace

Snapshot const& Get(PlayerbotAI* botAI)
{
    thread_local std::unordered_map<uint64, Snapshot> cache;
    static Snapshot const inactive;
    Player* bot = botAI->GetBot();
    // Amanitar only spawns on heroic.
    if (bot->GetMapId() != MAP_AHNKAHET)
        return inactive;

    Snapshot& snapshot = cache[bot->GetGUID().GetRawValue()];
    uint32 const now = getMSTime();
    if (snapshot.ms && getMSTimeDiff(snapshot.ms, now) < CACHE_MS)
        return snapshot;

    snapshot.ms = now;
    Compute(botAI, snapshot);
    return snapshot;
}

bool IsMushroom(Unit const* unit)
{
    return unit && (unit->GetEntry() == NPC_HEALTHY_MUSHROOM || unit->GetEntry() == NPC_POISONOUS_MUSHROOM);
}

bool NeedsFungus(Player* bot)
{
    return !PlayerbotAI::IsHeal(bot) && bot->HasAura(SPELL_MINI) && !bot->HasAura(SPELL_POTENT_FUNGUS);
}

Creature* FindFungusMushroom(PlayerbotAI* botAI)
{
    Snapshot const& snapshot = Get(botAI);
    if (!snapshot.active || snapshot.fungusTarget.IsEmpty())
        return nullptr;

    Creature* healthy = ObjectAccessor::GetCreature(*botAI->GetBot(), snapshot.fungusTarget);
    return healthy && healthy->IsAlive() ? healthy : nullptr;
}
}  // namespace AKAmanitar

bool AmanitarMiniTrigger::IsActive()
{
    return AKAmanitar::NeedsFungus(bot) && AKAmanitar::FindFungusMushroom(botAI) != nullptr;
}
