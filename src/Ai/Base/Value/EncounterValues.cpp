/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EncounterValues.h"

#include "Creature.h"
#include "InstanceScript.h"
#include "Map.h"
#include "Playerbots.h"

namespace InstanceEncounter
{
    int32 GetInProgressBossId(Map* map)
    {
        if (!map || !map->IsDungeon())
            return NO_ENCOUNTER;

        InstanceScript const* instance = map->ToInstanceMap()->GetInstanceScript();
        if (!instance)
            return NO_ENCOUNTER;

        uint32 const count = instance->GetEncounterCount();
        for (uint32 bossId = 0; bossId < count; ++bossId)
            if (instance->GetBossState(bossId) == IN_PROGRESS)
                return int32(bossId);

        return NO_ENCOUNTER;
    }

    std::string GetEncounterName(Map* map, int32 bossId)
    {
        if (!map || bossId == NO_ENCOUNTER)
            return "";

        for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
            if (creature && creature->IsAlive() && creature->IsInCombat() &&
                (creature->IsDungeonBoss() || creature->isWorldBoss()))
                return creature->GetName();

        InstanceScript const* instance = map->IsDungeon() ? map->ToInstanceMap()->GetInstanceScript() : nullptr;
        if (instance && uint32(bossId) < instance->GetEncounterCount())
            for (Creature* minion : instance->GetBossInfo(bossId)->minion)
                if (minion && minion->IsAlive())
                    return minion->GetName();

        return "";
    }
}

bool EncounterInProgressValue::Calculate()
{
    return InstanceEncounter::GetInProgressBossId(bot->GetMap()) != InstanceEncounter::NO_ENCOUNTER;
}
