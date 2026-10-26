/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BFShared.h"
#include "InstanceScript.h"
#include "Playerbots.h"

namespace BloodFurnace
{
Unit* FindLooseCellOrc(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (bot->GetMapId() != MAP_BLOOD_FURNACE)
        return nullptr;

    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetBossState(BOSS_DATA_BROGGOK) != IN_PROGRESS)
        return nullptr;

    Unit* nearest = nullptr;
    float nearestDist = BROGGOK_LOOSE_ADD_RADIUS;
    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const& guid : attackers)
    {
        if (guid.GetEntry() != NPC_NASCENT_FEL_ORC)
            continue;

        Unit* orc = botAI->GetUnit(guid);
        if (!orc || !orc->IsAlive())
            continue;

        Player* victim = orc->GetVictim() ? orc->GetVictim()->ToPlayer() : nullptr;
        if (victim && PlayerbotAI::IsTank(victim))
            continue;

        float const dist = bot->GetExactDist2d(orc);
        if (dist < nearestDist)
        {
            nearest = orc;
            nearestDist = dist;
        }
    }

    return nearest;
}
}
