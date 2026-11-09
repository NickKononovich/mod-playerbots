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
    // [roles59] the global leader planned the bot a tank / healer role: with two specs it switches (role spec in a
    // five, solo spec questing); a role spec its talents do not hold yet, or dual spec from its level (1000 g), is a
    // walk to its class trainer (true: the DO_PROFESSION walk started)
    bool StartRoleSpec(PlayerbotAI* botAI, Player* bot);

    // "skill:value/max,..." of the bot's planned professions (guild board), empty when it has no plan.
    std::string PlannedSkills(Player* bot);

    // [guild50] The global leader's BUY order: walk to this vendor spawn and buy the item with the bot's own money
    // (the tool purchase path: Player::BuyItemFromVendorSlot).
    void StartBuy(PlayerbotAI* botAI, Player* bot, uint32 itemId, uint32 npcEntry, WorldPosition const& pos,
                  uint32 cost);
    // [guild50] the buy failed at that vendor (gone, sold out, refused) while money and level stayed the same
    bool BuyFailed(Player* bot, uint32 npcEntry, uint32 itemId);
    // [guild50] the money the next rank of each planned primary profession costs at a trainer that can teach it now
    // (Trainer::CanTeachSpell): kept back from gear purchases
    uint32 NextRankCost(Player* bot);
    // [guild70] the tools of the bot's learned planned professions it still lacks: their vendor price, their names
    uint32 MissingTools(Player* bot, std::string& names);
}

class NewRpgDoProfessionAction : public NewRpgBaseAction
{
public:
    NewRpgDoProfessionAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg do profession") {}
    bool Execute(Event event) override;

private:
    bool ExecuteCraft(NewRpgInfo::DoProfession& data);  // [guild70] NewRpgCraft.cpp
};

#endif
