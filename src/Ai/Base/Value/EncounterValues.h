/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ENCOUNTERVALUES_H
#define PLAYERBOTS_ENCOUNTERVALUES_H

#include "Value.h"

#include <string>

class Map;
class PlayerbotAI;

namespace InstanceEncounter
{
    constexpr int32 NO_ENCOUNTER = -1;

    // Boss id (InstanceScript index) of the first encounter whose state is IN_PROGRESS on this map, or
    // NO_ENCOUNTER. Read from the InstanceScript, not from the combat flag: units leave combat mid-encounter
    // between waves and phases (AC #28026), the encounter state stays IN_PROGRESS.
    int32 GetInProgressBossId(Map* map);

    // Best available name for that encounter: an engaged boss of the map, else a registered minion. Scans the
    // map's creatures, so it is meant for rare callers (death logs), not for triggers.
    std::string GetEncounterName(Map* map, int32 bossId);
}

// True while any boss of the bot's instance is IN_PROGRESS, also between waves when nobody is in combat.
class EncounterInProgressValue : public BoolCalculatedValue
{
public:
    EncounterInProgressValue(PlayerbotAI* botAI) : BoolCalculatedValue(botAI, "encounter in progress") {}

    bool Calculate() override;
};

#endif
