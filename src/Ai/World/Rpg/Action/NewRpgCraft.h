/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOT_NEWRPGCRAFT_H
#define PLAYERBOT_NEWRPGCRAFT_H

#include <string>

#include "Define.h"

class Creature;
class Item;
class Player;
class PlayerbotAI;

// [guild70] Honest crafting for the guild project bots: they level their crafting professions by crafting.
//   - recipes are learned at the profession trainer with the bot's own money (with the rank visits, or a visit of
//     its own once no known recipe still gives skill);
//   - in a town (rest area) the bot crafts the known recipes that still give skill (orange, then yellow, by the
//     recipe's SkillLineAbility thresholds), useful products first, then the cheapest per expected skill point;
//     reagents come from its bags, the guild bank (a BANK_WITHDRAW order) or a vendor in the zone (its own money);
//     Mining smelts ore at a forge the same way;
//   - the product goes the coordinator's way (OnLoot: an upgrade for a member, the bank, sale or disenchant);
//     consumables and bags stay with the crafter.
// The decision runs from New RPG's IDLE (an event), never per tick.
namespace NewRpgCraft
{
    enum : uint8
    {
        TASK_RECIPES = 3,  // a trainer visit for recipes only
        TASK_CRAFT = 4     // skill-up crafting (buy vendor reagents first, walk to a forge / anvil / fire)
    };

    void Load();  // world thread, startup (after the profession index)

    // the bot's money for reagents and recipes: free money (repair, spells, travel kept) minus the next ranks' cost
    uint32 Budget(PlayerbotAI* botAI, Player* bot);
    // at a trainer of `skill`: learns every recipe it teaches now that still gives skill, cheapest first, within
    // `budget`; returns the recipes learned
    uint32 LearnRecipes(Player* bot, Creature* trainer, uint32 skill, uint32 budget);
    // the trainer teaches the bot now a recipe of `skill` that gives skill at its value (cheapest cost)
    bool TeachesSkillUpRecipe(Player* bot, uint32 trainerEntry, uint32 skill, uint32& cost);
    // a known recipe of `skill` still gives skill at the bot's value
    bool HasSkillUpRecipe(Player* bot, uint32 skill);
    // a town / inn: crafting is decided only there
    bool InSafePlace(Player* bot);
    // starts TASK_CRAFT when a skill-up craft is possible here (bags, bank requests, zone vendors, a forge in sight)
    bool StartCraft(PlayerbotAI* botAI, Player* bot);
    // the skill-up recipe the bot can cast where it stands now (reagents in its bags, tools, focus), 0 = none
    uint32 PickHere(Player* bot);
    // map thread, Spell::DoCreateItem: a skill-up product goes the coordinator's way
    void OnCreateItem(Player* player, Item* item, uint32 count);
    std::string RecipeName(uint32 spellId);
    uint32 RecipeSkill(uint32 spellId);
    uint32 RecipeFocus(uint32 spellId);
    bool FocusNear(Player* bot, uint32 focusId);
}

#endif
