/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BFTECHNICIAN_H
#define PLAYERBOTS_BFTECHNICIAN_H

#include "AttackAction.h"
#include "Playerbots.h"
#include "Trigger.h"

// Header-only on purpose (no new translation unit, so no CMake reconfigure).
// smart_scripts 17414: the Shadowmoon Technician throws Dynamite (40062, 1 s, 5 yd at a random player) every
// 1-7 s, Proximity Bombs (30846, 10 yd blast) and Silence. Wave 8: the healer died to both while the group fought
// the Enforcers around it. Damage dealers take the Technicians first; their casts are then on the current target
// of the interrupters, and the thrown Dynamite is dodged by the generic "avoid aoe" (see DungeonScripts.cpp).
namespace BloodFurnaceTechnician
{
inline constexpr uint32 NPC_SHADOWMOON_TECHNICIAN = 17414;
inline constexpr float TECHNICIAN_SEARCH_RANGE = 40.0f;

// The Technician with the least health in combat with the group, within reach.
inline Unit* Find(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Unit* best = nullptr;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        if (guid.GetEntry() != NPC_SHADOWMOON_TECHNICIAN)
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || bot->GetExactDist(unit) > TECHNICIAN_SEARCH_RANGE)
            continue;

        if (!best || unit->GetHealthPct() < best->GetHealthPct())
            best = unit;
    }
    return best;
}
}  // namespace BloodFurnaceTechnician

class ShadowmoonTechnicianTrigger : public Trigger
{
public:
    ShadowmoonTechnicianTrigger(PlayerbotAI* botAI) : Trigger(botAI, "shadowmoon technician") {}

    bool IsActive() override
    {
        if (!PlayerbotAI::IsDps(bot))
            return false;

        Unit* target = AI_VALUE(Unit*, "current target");
        if (target && target->IsAlive() && target->GetEntry() == BloodFurnaceTechnician::NPC_SHADOWMOON_TECHNICIAN)
            return false;

        return BloodFurnaceTechnician::Find(botAI) != nullptr;
    }
};

class AttackShadowmoonTechnicianAction : public AttackAction
{
public:
    AttackShadowmoonTechnicianAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack shadowmoon technician") {}

    bool isUseful() override { return !PlayerbotAI::IsHeal(bot); }

    bool Execute(Event /*event*/) override
    {
        Unit* technician = BloodFurnaceTechnician::Find(botAI);
        if (!technician || AI_VALUE(Unit*, "current target") == technician)
            return false;

        return Attack(technician);
    }
};

#endif
