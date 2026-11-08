/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUILDHONEST_H
#define PLAYERBOTS_GUILDHONEST_H

#include "Creature.h"
#include "GossipDef.h"
#include "GuildCoordinator.h"
#include "Log.h"
#include "Player.h"
#include "World.h"

// [honest] USER RULE 2026-11-08 «ничего даром нету»: the guild-plan bots (GuildCoordinator::IsPlanned, groups A, B
// and C; B and C are random bots in code, accounts GUILDBOT*) get nothing for free. The only approved exceptions:
// class spells and skills auto-learned on level-up (AutoLearnTrainerSpells / AutoLearnQuestSpells) and talents
// auto-picked with earned points. Every random-bot handout path asks IsHonestBot() and skips the grant; a grant that
// still happens logs "[alert] HANDOUT <bot> <what>" so the run shows the leftovers.
// The plan index is read-only after GuildCoordinator::Load (startup), so map threads may ask.
namespace GuildHonest
{
inline bool IsHonestBot(Player const* bot) { return bot && sGuildCoordinator.IsPlanned(bot->GetGUID().GetCounter()); }

// true (= skip the grant) for a guild-plan bot
inline bool Skip(Player const* bot, char const* what)
{
    if (!IsHonestBot(bot))
        return false;
    LOG_DEBUG("playerbots", "[honest] {} skips the free {}", bot->GetName(), what);
    return true;
}

// a free grant that still reached a guild-plan bot
inline void Handout(Player const* bot, char const* what)
{
    if (IsHonestBot(bot))
        LOG_INFO("playerbots", "[alert] HANDOUT {} {}", bot->GetName(), what);
}

inline void LogStartup(size_t planned)
{
    LOG_INFO("playerbots",
             "[honest] {} guild-plan bots get nothing for free. Disabled for them: factory Randomize / Refresh / "
             "level change, gear (InitEquipment, AutoGear, autogear / bis commands), bags, ammo, food, potions, "
             "reagents, consumables, soul shards, mounts, riding, weapon skills, professions and tools "
             "(InitTradeSkills / InitInventorySkill), profession specializations, reputation, attunement / instance "
             "quests, keyring, special spells (RandomBotSpellIds), pet creation (InitPet), enchants and gems "
             "(ApplyEnchantAndGemsNew / EnchantItem / enchant templates), premade glyphs (InitGlyphs, glyph equip), "
             "free dual spec (talents switch 2), the maintenance command, free repair / revive / heal / gold "
             "(Refresh, summon), summon teleports, random-bot teleports, random guild join. Kept: class trainer "
             "spells and quest spells on level-up, talents with earned points. Glyphs only from glyph items, dual "
             "spec only bought at the class trainer.",
             planned);
}

// Dual Talent Specialization bought like a player: the class trainer's gossip (GOSSIP_OPTION_DUALSPEC_INFO ->
// GOSSIP_OPTION_LEARNDUALSPEC), the core checks the level, the trainer (CanResetTalents) and takes BoxMoney
// (1000 g in the DB) from the bot's own gold. World or the bot's map thread, the trainer within interaction range.
// Returns true when the bot has two specs afterwards; `why` gets the reason when not.
inline bool BuyDualSpecAtTrainer(Player* bot, Creature* trainer, std::string* why = nullptr)
{
    auto fail = [why](char const* text)
    {
        if (why)
            *why = text;
        return false;
    };
    if (!bot || !trainer)
        return fail("no trainer");
    if (bot->GetSpecsCount() > 1)
        return true;
    if (bot->GetLevel() < sWorld->getIntConfig(CONFIG_MIN_DUALSPEC_LEVEL))
        return fail("level too low");
    if (!trainer->CanResetTalents(bot))
        return fail("not my class trainer");
    if (!bot->IsWithinDistInMap(trainer, INTERACTION_DISTANCE))
        return fail("trainer too far");

    uint32 menuId = trainer->GetGossipMenuId();
    // two levels at most: the trainer's menu, then the dual spec info page that holds the buy option
    for (uint8 depth = 0; depth < 2; ++depth)
    {
        bot->PrepareGossipMenu(trainer, menuId);
        GossipMenu& menu = bot->PlayerTalkClass->GetGossipMenu();
        uint32 infoIndex = UINT32_MAX;
        for (auto const& [index, item] : menu.GetMenuItems())
        {
            if (item.OptionType == GOSSIP_OPTION_LEARNDUALSPEC)
            {
                if (!bot->HasEnoughMoney(int32(item.BoxMoney)))
                {
                    bot->PlayerTalkClass->SendCloseGossip();
                    return fail("not enough gold");
                }
                uint32 const before = bot->GetMoney();
                bot->OnGossipSelect(trainer, index, menu.GetMenuId());
                bot->PlayerTalkClass->SendCloseGossip();
                if (bot->GetSpecsCount() < 2)
                    return fail("trainer refused");
                LOG_INFO("playerbots", "[honest] {} bought Dual Talent Specialization at {} for {} copper",
                         bot->GetName(), trainer->GetName(), before - bot->GetMoney());
                return true;
            }
            if (item.OptionType == GOSSIP_OPTION_DUALSPEC_INFO && infoIndex == UINT32_MAX)
                infoIndex = index;
        }
        if (infoIndex == UINT32_MAX)
            break;
        GossipMenuItemData const* data = menu.GetItemData(infoIndex);
        if (!data || !data->GossipActionMenuId)
            break;
        menuId = data->GossipActionMenuId;
    }
    bot->PlayerTalkClass->SendCloseGossip();
    return fail("trainer has no dual spec option");
}
}  // namespace GuildHonest

#endif
