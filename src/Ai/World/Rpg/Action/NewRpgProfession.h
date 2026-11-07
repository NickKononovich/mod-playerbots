/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOT_NEWRPGPROFESSION_H
#define PLAYERBOT_NEWRPGPROFESSION_H

#include <string>

#include "NewRpgBaseAction.h"

class Player;
class PlayerbotAI;

// [guild20] Honest professions for the guild project bots (acore_playerbots.guild_project_plan): the bot walks to the
// nearest friendly trainer of a planned profession on its map and learns the next rank there with its own money
// (the core's Trainer::TeachSpell, as the trainer window does), and buys the profession's tool at the nearest vendor
// that sells it (Player::BuyItemFromVendorSlot). Secondary skills (Cooking, First Aid, Fishing) only at a trainer in
// the zone the bot stands in, and only when no primary rank or tool is waiting for money.
namespace NewRpgProfession
{
    // Reads guild_project_plan and builds the trainer/tool vendor index (startup, world thread).
    void LoadPlan();

    // Starts a DO_PROFESSION walk when the bot has a profession errand it can pay for now.
    bool StartTask(PlayerbotAI* botAI, Player* bot);

    // "skill:value/max,..." of the bot's planned professions (guild board), empty when it has no plan.
    std::string PlannedSkills(Player* bot);
}

class NewRpgDoProfessionAction : public NewRpgBaseAction
{
public:
    NewRpgDoProfessionAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg do profession") {}
    bool Execute(Event event) override;
};

#endif
