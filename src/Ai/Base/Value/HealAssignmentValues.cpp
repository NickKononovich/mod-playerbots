/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HealAssignmentValues.h"

#include <algorithm>
#include <map>
#include <vector>

#include "Group.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Timer.h"

namespace
{
    constexpr size_t MAX_ASSIGNED_TANKS = 2;
    constexpr size_t SECOND_MAIN_TANK_HEALER_MIN_HEALERS = 5;

    bool IsHealEffect(uint32 effect)
    {
        return effect == SPELL_EFFECT_HEAL || effect == SPELL_EFFECT_HEAL_MAX_HEALTH ||
               effect == SPELL_EFFECT_HEAL_MECHANICAL;
    }

    bool CompareByGuid(Player const* left, Player const* right) { return left->GetGUID() < right->GetGUID(); }
}

float PredictedHealthPct(Unit const* unit, IncomingHealMap const& incoming)
{
    uint32 const maxHealth = unit->GetMaxHealth();
    if (!maxHealth)
        return 100.0f;

    uint64 health = unit->GetHealth();
    auto const itr = incoming.find(unit->GetGUID());
    if (itr != incoming.end())
        health += itr->second;

    return std::min(100.0f, static_cast<float>(health) * 100.0f / maxHealth);
}

IncomingHealMap IncomingHealsValue::Calculate()
{
    IncomingHealMap incoming;

    Group* group = bot->GetGroup();
    if (!group)
        return incoming;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || member->GetMapId() != bot->GetMapId())
            continue;

        if (!member->IsNonMeleeSpellCast(true))
            continue;

        for (uint8 type = CURRENT_GENERIC_SPELL; type < CURRENT_MAX_SPELL; ++type)
        {
            Spell* spell = member->GetCurrentSpell(static_cast<CurrentSpellTypes>(type));
            if (!spell || !spell->m_spellInfo)
                continue;

            ObjectGuid const targetGuid = spell->m_targets.GetUnitTargetGUID();
            if (!targetGuid)
                continue;

            SpellInfo const* spellInfo = spell->m_spellInfo;
            bool isHeal = false;
            uint32 amount = 0;
            Unit* target = nullptr;
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                if (!IsHealEffect(spellInfo->Effects[i].Effect))
                    continue;

                isHeal = true;
                // Only a direct heal of a cast-time spell has an amount worth predicting.
                if (type != CURRENT_GENERIC_SPELL || spellInfo->Effects[i].Effect != SPELL_EFFECT_HEAL)
                    continue;

                if (!target)
                    target = botAI->GetUnit(targetGuid);

                if (!target)
                    break;

                int32 const basePoints = spellInfo->Effects[i].CalcValue(member);
                if (basePoints <= 0)
                    continue;

                uint32 const heal =
                    member->SpellHealingBonusDone(target, spellInfo, static_cast<uint32>(basePoints), HEAL, i);
                amount += target->SpellHealingBonusTaken(member, spellInfo, heal, HEAL);
            }

            if (isHeal)
                incoming[targetGuid] += std::max<uint32>(amount, 1);
        }
    }

    return incoming;
}

std::list<ObjectGuid> AutoFocusHealTargetsValue::Calculate()
{
    std::list<ObjectGuid> assigned;

    if (!sPlayerbotAIConfig.AutoHealerAssignment)
        return assigned;

    Group* group = bot->GetGroup();
    if (!group || !group->isRaidGroup() || !PlayerbotAI::IsHeal(bot))
        return assigned;

    Map* map = bot->GetMap();
    if (!map)
        return assigned;

    // With the healer-dps restriction on, its map list says where healers must only heal; otherwise any raid.
    bool const assignmentMap = sPlayerbotAIConfig.RestrictHealerDPS
                                   ? sPlayerbotAIConfig.IsRestrictedHealerDPSMap(bot->GetMapId())
                                   : map->IsRaid();
    if (!assignmentMap)
        return assigned;

    ObjectGuid const mainTankGuid = PlayerbotAI::GetMainTankGuid(group);
    std::vector<Player*> healers;
    std::vector<Player*> tanks;
    Player* mainTank = nullptr;
    std::map<uint8, std::vector<ObjectGuid>> subGroups;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || member->GetMapId() != bot->GetMapId())
            continue;

        subGroups[ref->getSubGroup()].push_back(member->GetGUID());

        if (!member->IsAlive())
            continue;

        if (PlayerbotAI::IsHeal(member))
            healers.push_back(member);
        else if (member->GetGUID() == mainTankGuid)
            mainTank = member;
        else if (PlayerbotAI::IsTank(member))
            tanks.push_back(member);
    }

    if (healers.size() < 2)
        return assigned;

    std::sort(healers.begin(), healers.end(), CompareByGuid);
    std::sort(tanks.begin(), tanks.end(), CompareByGuid);
    if (mainTank)
        tanks.insert(tanks.begin(), mainTank);

    if (tanks.size() > MAX_ASSIGNED_TANKS)
        tanks.resize(MAX_ASSIGNED_TANKS);

    auto const self = std::find(healers.begin(), healers.end(), bot);
    if (self == healers.end())
        return assigned;

    size_t const healerIndex = std::distance(healers.begin(), self);

    // At least one healer always stays on the raid.
    size_t tankHealers = std::min(tanks.size(), healers.size() - 1);
    if (tankHealers && healers.size() >= SECOND_MAIN_TANK_HEALER_MIN_HEALERS)
        ++tankHealers;

    if (healerIndex < tankHealers)
    {
        assigned.push_back(tanks[healerIndex < tanks.size() ? healerIndex : 0]->GetGUID());

        // Too few healers for one per tank: the last tank healer also covers the uncovered tanks.
        if (healerIndex + 1 == tankHealers)
        {
            for (size_t tankIndex = tankHealers; tankIndex < tanks.size(); ++tankIndex)
                assigned.push_back(tanks[tankIndex]->GetGUID());
        }

        return assigned;
    }

    size_t const raidHealers = healers.size() - tankHealers;
    size_t const raidIndex = healerIndex - tankHealers;
    size_t groupIndex = 0;
    for (auto const& subGroup : subGroups)
    {
        bool const mine = raidHealers >= subGroups.size() ? groupIndex == raidIndex % subGroups.size()
                                                          : groupIndex % raidHealers == raidIndex;
        if (mine)
            assigned.insert(assigned.end(), subGroup.second.begin(), subGroup.second.end());

        ++groupIndex;
    }

    return assigned;
}

float RecentHealthLossPct(Unit const* unit, HealthLossMap const& losses)
{
    auto const itr = losses.find(unit->GetGUID());
    return itr == losses.end() ? 0.0f : itr->second;
}

HealthLossMap GroupHealthLossValue::Calculate()
{
    HealthLossMap losses;

    if (sPlayerbotAIConfig.HealerHealthLossWeight <= 0.0f && sPlayerbotAIConfig.HealerFastHealthLossPct <= 0.0f)
        return losses;

    Group* group = bot->GetGroup();
    if (!group)
        return losses;

    uint32 const now = getMSTime();
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || member->GetMapId() != bot->GetMapId())
            continue;

        float const loss = _tracker.Sample(member, now);
        if (loss > 0.0f)
            losses[member->GetGUID()] = loss;
    }

    return losses;
}
