/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_LEADERTANKSAVES_H
#define _PLAYERBOT_LEADERTANKSAVES_H

// [split251] The tank's defensive cooldowns as the pull planner (LeaderPlanner.cpp) and the five gate
// (GuildFives.cpp) price a pull with them, and the planner's planned save the leader tells (LeaderEvents.cpp).

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

class Player;

namespace PullPlanning
{
    // [split251] A tank's defensive cooldown from its spell data at the rank it knows (the cooldown and duration mods
    // applied): Shield Wall, Last Stand, Survival Instincts, Barkskin, Divine Protection, Icebound Fortitude,
    // Vampiric Blood. Frenzied Regeneration (rage into health) is not modelled and gives nothing.
    struct TankSave
    {
        uint32 spell = 0;
        std::string name;
        float reduction = 0.0f;   // the share of the damage taken it removes
        float healthGain = 0.0f;  // the max health it adds, a share of the tank's own
        float durationSec = 0.0f;
        float rechargeSec = 0.0f;
    };
    // the tank's known defensives, ready or not
    std::vector<TankSave> TankSaves(Player* tank);
    // what is left of a pull's drop and cost with the save cast at the pull: its reduction over the share of the
    // fight it lasts
    float TankSaveFactor(TankSave const& save, float fightSec);
    // the max health it adds for a fight of `fightSec`: only when it lasts the fight (the extra health goes with it)
    float TankSaveHealth(TankSave const& save, float fightSec);
    // [split251] the planner priced the tank's pull of `pack` with its defensive `spell`: the leader tells it once
    // the tank fights a mob of that pack (PlannedPullSave), the record ends with ClearPullSave or the pack's death
    void PlanPullSave(Player* tank, uint32 spell, std::vector<ObjectGuid> const& pack, std::string const& why);
    bool PlannedPullSave(Player* tank, uint32& spell, std::string& why);
    void ClearPullSave(Player* tank);
}

#endif
