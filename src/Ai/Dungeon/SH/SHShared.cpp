/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SHShared.h"
#include "Creature.h"
#include "Player.h"
#include "Unit.h"

namespace ShatteredHalls
{
bool IsBladeDancing(Unit* kargath)
{
    if (!kargath || !kargath->IsAlive() || !kargath->IsInCombat())
        return false;

    // Passive is what the eye sees as the dance: he stops fighting his target and charges round the room.
    Creature* creature = kargath->ToCreature();
    return (creature && creature->HasReactState(REACT_PASSIVE)) || kargath->HasUnitState(UNIT_STATE_CHARGING);
}

float BladeDanceDangerRadius(Player* bot, Unit* kargath)
{
    return BLADE_DANCE_RADIUS + bot->GetCombatReach() + kargath->GetCombatReach();
}
}  // namespace ShatteredHalls
