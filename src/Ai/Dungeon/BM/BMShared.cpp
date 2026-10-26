/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BMShared.h"
#include "AiObjectContext.h"
#include "Creature.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellAuras.h"
#include "Timer.h"

#include <algorithm>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace BMShared
{
namespace
{
std::vector<uint32> const ADD_ENTRIES = {
    NPC_INFINITE_ASSASSIN,     NPC_INFINITE_ASSASSIN_2,   NPC_INFINITE_WHELP,
    NPC_INFINITE_CHRONOMANCER, NPC_INFINITE_CHRONOMANCER_2, NPC_INFINITE_EXECUTIONER,
    NPC_INFINITE_EXECUTIONER_2, NPC_INFINITE_VANQUISHER, NPC_INFINITE_VANQUISHER_2};

std::vector<uint32> const GUARDIAN_ENTRIES = {
    NPC_RIFT_KEEPER_WARLOCK, NPC_RIFT_KEEPER_MAGE,     NPC_RIFT_LORD,
    NPC_RIFT_LORD_2,         NPC_CHRONO_LORD_DEJA,     NPC_INFINITE_CHRONO_LORD,
    NPC_TEMPORUS,            NPC_INFINITE_TIMEREAVER,  NPC_AEONUS};

bool Usable(Creature* creature)
{
    return creature && creature->IsInWorld() && creature->IsAlive() && !creature->IsDuringRemoveFromWorld();
}

GuidVector Ordered(std::vector<std::pair<float, ObjectGuid>>& scored)
{
    std::sort(scored.begin(), scored.end(),
              [](auto const& left, auto const& right) { return left.first < right.first; });
    GuidVector result;
    result.reserve(scored.size());
    for (auto const& entry : scored)
        result.push_back(entry.second);
    return result;
}
}  // namespace

InstanceScript* Instance(Player* bot)
{
    if (!bot || bot->GetMapId() != MAP_ID)
        return nullptr;
    return bot->GetInstanceScript();
}

uint32 RiftNumber(Player* bot)
{
    InstanceScript* instance = Instance(bot);
    return instance ? instance->GetData(DATA_RIFT_NUMBER) : 0;
}

int32 ShieldPct(Player* bot)
{
    InstanceScript* instance = Instance(bot);
    return instance ? static_cast<int32>(instance->GetData(DATA_SHIELD_PERCENT)) : 0;
}

bool IsEventInProgress(Player* bot)
{
    InstanceScript* instance = Instance(bot);
    if (!instance || instance->GetBossState(DATA_AEONUS) == DONE)
        return false;
    return instance->GetData(DATA_RIFT_NUMBER) > 0 && instance->GetData(DATA_SHIELD_PERCENT) > 0;
}

bool IsEventNotStarted(Player* bot)
{
    InstanceScript* instance = Instance(bot);
    return instance && instance->GetBossState(DATA_AEONUS) != DONE && instance->GetData(DATA_RIFT_NUMBER) == 0;
}

Creature* Medivh(Player* bot)
{
    InstanceScript* instance = Instance(bot);
    Creature* medivh = instance ? instance->GetCreature(DATA_MEDIVH) : nullptr;
    return Usable(medivh) ? medivh : nullptr;
}

bool IsAddEntry(uint32 entry) { return std::find(ADD_ENTRIES.begin(), ADD_ENTRIES.end(), entry) != ADD_ENTRIES.end(); }

bool IsGuardianEntry(uint32 entry)
{
    return std::find(GUARDIAN_ENTRIES.begin(), GUARDIAN_ENTRIES.end(), entry) != GUARDIAN_ENTRIES.end();
}

bool IsBossKeeperEntry(uint32 entry)
{
    return entry == NPC_CHRONO_LORD_DEJA || entry == NPC_INFINITE_CHRONO_LORD || entry == NPC_TEMPORUS ||
           entry == NPC_INFINITE_TIMEREAVER || entry == NPC_AEONUS;
}

bool IsCorruptingMedivh(Creature* medivh, Unit* unit)
{
    if (!medivh || !unit)
        return false;

    for (uint32 spell : {SPELL_CORRUPT_MEDIVH, SPELL_CORRUPT_MEDIVH_AEONUS})
    {
        auto const bounds = medivh->GetAppliedAuras().equal_range(spell);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            Aura const* aura = itr->second ? itr->second->GetBase() : nullptr;
            if (aura && aura->GetCasterGUID() == unit->GetGUID())
                return true;
        }
    }
    return false;
}

GuidVector ScanThreats(Player* bot)
{
    Creature* medivh = Medivh(bot);
    if (!medivh || !IsEventInProgress(bot))
        return {};

    std::list<Creature*> adds;
    medivh->GetCreatureListWithEntryInGrid(adds, ADD_ENTRIES, THREAT_RADIUS);

    std::vector<std::pair<float, ObjectGuid>> scored;
    for (Creature* add : adds)
    {
        if (!Usable(add) || add->IsImmuneToPC())
            continue;

        // Corrupting adds first (a negative score), the rest by how close they came to Medivh.
        float const distance = add->GetExactDist2d(medivh);
        scored.emplace_back(IsCorruptingMedivh(medivh, add) ? distance - 1000.0f : distance, add->GetGUID());
    }
    return Ordered(scored);
}

GuidVector ScanCorrupting(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Creature* medivh = Medivh(bot);
    if (!medivh || !IsEventInProgress(bot))
        return {};

    GuidVector result;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("bm medivh threats")->Get())
        if (Unit* unit = botAI->GetUnit(guid); unit && unit->IsAlive() && IsCorruptingMedivh(medivh, unit))
            result.push_back(guid);
    return result;
}

GuidVector ScanGuardians(Player* bot)
{
    Creature* medivh = Medivh(bot);
    if (!medivh || !IsEventInProgress(bot))
        return {};

    std::list<Creature*> keepers;
    medivh->GetCreatureListWithEntryInGrid(keepers, GUARDIAN_ENTRIES, GUARDIAN_SCAN_RADIUS);

    std::vector<std::pair<float, ObjectGuid>> scored;
    for (Creature* keeper : keepers)
        if (Usable(keeper) && !keeper->IsImmuneToPC() && !keeper->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE))
            scored.emplace_back(keeper->GetExactDist2d(medivh), keeper->GetGUID());
    return Ordered(scored);
}

Unit* FirstThreat(PlayerbotAI* botAI)
{
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("bm medivh threats")->Get())
        if (Unit* unit = botAI->GetUnit(guid); unit && unit->IsAlive())
            return unit;
    return nullptr;
}

bool AnyCorrupting(PlayerbotAI* botAI)
{
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("bm corrupting adds")->Get())
        if (Unit* unit = botAI->GetUnit(guid); unit && unit->IsAlive())
            return true;
    return false;
}

Unit* ThreatFor(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Creature* medivh = Medivh(bot);
    std::vector<Unit*> channeling;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("bm corrupting adds")->Get())
        if (Unit* unit = botAI->GetUnit(guid); unit && unit->IsAlive() && IsCorruptingMedivh(medivh, unit))
            channeling.push_back(unit);

    if (channeling.empty())
        return FirstThreat(botAI);

    // Ranged damage dealers spread over the channels by their place in the group; a channel breaks on the first hit
    // (the add turns on its attacker), so each cast frees a point of the shield and the tank picks the add up.
    if (PlayerbotAI::IsRanged(bot) && !PlayerbotAI::IsTank(bot) && !PlayerbotAI::IsHeal(bot))
    {
        uint32 slot = 0;
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member == bot)
                    break;
                if (member && member->IsAlive() && member->GetMapId() == bot->GetMapId() &&
                    member->GetInstanceId() == bot->GetInstanceId() && PlayerbotAI::IsRanged(member) &&
                    !PlayerbotAI::IsTank(member) && !PlayerbotAI::IsHeal(member))
                    ++slot;
            }
        }
        return channeling[slot % channeling.size()];
    }

    Unit* nearest = nullptr;
    for (Unit* unit : channeling)
        if (!nearest || bot->GetExactDist(unit) < bot->GetExactDist(nearest))
            nearest = unit;
    return nearest;
}

Unit* NearestGuardian(PlayerbotAI* botAI)
{
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("bm rift guardians")->Get())
        if (Unit* unit = botAI->GetUnit(guid); unit && unit->IsAlive())
            return unit;
    return nullptr;
}

Unit* GuardianAtMedivh(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Unit* guardian = NearestGuardian(botAI);
    if (!guardian || !guardian->IsInCombat() || !Medivh(bot) || IsFarGuardian(bot, guardian))
        return nullptr;
    return guardian;
}

bool IsThreat(PlayerbotAI* botAI, Unit* unit)
{
    if (!unit || !unit->IsAlive())
        return false;
    GuidVector const threats = botAI->GetAiObjectContext()->GetValue<GuidVector>("bm medivh threats")->Get();
    return std::find(threats.begin(), threats.end(), unit->GetGUID()) != threats.end();
}

bool IsFarGuardian(Player* bot, Unit* unit)
{
    if (!unit || !IsGuardianEntry(unit->GetEntry()))
        return false;
    Creature* medivh = Medivh(bot);
    float const radius = IsStandingOff(bot, unit) ? GUARDIAN_STANDOFF_RADIUS : GUARDIAN_ENGAGE_RADIUS;
    return medivh && unit->GetExactDist2d(medivh) > radius;
}

bool IsStandingOff(Player* bot, Unit* unit)
{
    if (!unit || !unit->IsAlive() || !unit->IsInCombat() || unit->isMoving())
        return false;

    Unit* victim = unit->GetVictim();
    Player* player = victim ? victim->ToPlayer() : nullptr;
    if (!player || (player != bot && !player->IsInSameGroupWith(bot)))
        return false;

    return unit->GetExactDist(victim) > STANDOFF_GAP;
}

bool HasLivingTank(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMapId() != bot->GetMapId() ||
            member->GetInstanceId() != bot->GetInstanceId())
            continue;

        if (PlayerbotAI::IsTank(member))
            return true;
    }
    return false;
}

void Log(Player* bot, char const* tag, std::string const& key, std::string const& message, uint32 intervalMs)
{
    static std::mutex lock;
    static std::unordered_map<std::string, uint32> lastLogged;

    std::string const fullKey = std::to_string(bot->GetInstanceId()) + ":" + bot->GetGUID().ToString() + ":" + tag +
                                ":" + key;
    uint32 const now = getMSTime();
    {
        std::lock_guard<std::mutex> guard(lock);
        auto const it = lastLogged.find(fullKey);
        if (it != lastLogged.end() && getMSTimeDiff(it->second, now) < intervalMs)
            return;
        lastLogged[fullKey] = now;
    }

    LOG_INFO("playerbots", "[{}] {}: {} (rift {}, shield {}%)", tag, bot->GetName(), message, RiftNumber(bot),
             ShieldPct(bot));
}
}  // namespace BMShared
