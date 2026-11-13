/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DestroyItemAction.h"
#include "Event.h"
#include "GuildCoordinator.h"  // [econ61] the guild's own routes for an item it would otherwise throw away
#include "ItemCountValue.h"
#include "Playerbots.h"
#include <algorithm>

namespace
{
// Percent of bag slots used at or above which the bags are reported full. No destroy hangs off it any more
// ([econ70]); it is the same number the old smart destroy used, so nothing new is introduced here.
constexpr uint8 BAG_SPACE_DESTROY_THRESHOLD = 90;
}  // namespace

bool DestroyItemAction::Execute(Event event)
{
    std::string const text = event.getParam();
    ItemIds ids = chat->parseItems(text);

    for (ItemIds::iterator i = ids.begin(); i != ids.end(); i++)
    {
        FindItemByIdVisitor visitor(*i);
        DestroyItem(&visitor);
    }

    return true;
}

void DestroyItemAction::DestroyItem(FindItemVisitor* visitor)
{
    IterateItems(visitor);
    std::vector<Item*> items = visitor->GetResult();
    for (Item* item : items)
    {
        std::ostringstream out;
        out << chat->FormatItem(item->GetTemplate()) << " destroyed";
        botAI->TellMaster(out);

        // [econ61] every destroy, named: world_triage.py reads this line and its target is 0. [econ70] after the
        // user's rule of 2026-11-13 no bot decision reaches here at all - the only caller left is the master's own
        // `destroy <item>` chat command, a human deleting a human's item. A line here with no human at the keyboard
        // is a bug in that rule, which is exactly what the counter is for.
        ItemTemplate const* proto = item->GetTemplate();
        LOG_INFO("playerbots", "[econ61] {} L{} destroys {} ×{} (class {}, a vendor pays {}, guild member {})",
                 bot->GetName(), bot->GetLevel(), proto->Name1, item->GetCount(), uint32(proto->Class),
                 proto->SellPrice, sGuildCoordinator.IsPlanned(bot->GetGUID().GetCounter()) ? "yes" : "no");

        bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
    }
}

bool SmartDestroyItemAction::isUseful() { return !IsRealPlayer(botAI->GetMaster()); }

// [econ70] the action that used to free bag slots by destroying. It destroys nothing now: the three disposals a bot
// has are sell, hand over and use, and a bot with no reachable disposal keeps the item and says so. The old walk
// (ITEM_USAGE_NONE, then VENDOR and AH, then SKILL, then USE) ate exactly what the guild had just made - a crafted
// potion a member is owed is a USE item, the surplus held for another member a VENDOR one - and [econ61] closed only
// the half of that a vendor price or the wanted list could prove.
bool SmartDestroyItemAction::Execute(Event /*event*/)
{
    if (AI_VALUE(uint8, "bag space") < BAG_SPACE_DESTROY_THRESHOLD)
    {
        lastSaid_.clear();  // the bags came free: the next pressure says its own figure again
        return false;
    }

    SayDisposals();
    // Nothing was destroyed and nothing else happened here, so the bot's turn is not spent on this: the vendor run
    // ([guild3] on full bags), the trade and the disenchant queue are what free the slot.
    return false;
}

void SmartDestroyItemAction::SayDisposals()
{
    // the same order the destroy used to walk, so the line reports the same items it used to eat
    static std::vector<uint32> const order = {ITEM_USAGE_NONE, ITEM_USAGE_VENDOR, ITEM_USAGE_AH, ITEM_USAGE_SKILL,
                                              ITEM_USAGE_USE};
    uint32 toSell = 0, toHandOver = 0, toUse = 0, kept = 0;
    std::string keptFirst;
    bool const planned = sGuildCoordinator.IsPlanned(bot->GetGUID().GetCounter());
    for (uint32 const usage : order)
        for (Item* item : AI_VALUE2(std::vector<Item*>, "inventory items", "usage " + std::to_string(usage)))
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            if (!proto)
                continue;
            if (proto->SellPrice)  // a vendor buys it: the SELL task
                ++toSell;
            else if (planned &&
                     (sGuildCoordinator.IsWanted(proto->ItemId) ||
                      sGuildCoordinator.NeedsForOwnCalls(bot, proto->ItemId)))  // a member or its own call wants it
                ++toHandOver;
            else if (usage == ITEM_USAGE_USE || usage == ITEM_USAGE_SKILL)  // it uses it itself
                ++toUse;
            else
            {
                ++kept;  // no vendor, no member, no use of its own: it keeps it, and this line names it
                if (keptFirst.empty())
                    keptFirst = proto->Name1;
            }
        }

    std::string const said =
        Acore::StringFormat("[econ70] {} L{} bags {} % full and destroys nothing: {} to sell, {} to hand over, {} to "
                            "use, {} kept with no route{}",
                            bot->GetName(), bot->GetLevel(), uint32(AI_VALUE(uint8, "bag space")), toSell, toHandOver,
                            toUse, kept, keptFirst.empty() ? std::string() : " (" + keptFirst + " ...)");
    if (said == lastSaid_)
        return;
    lastSaid_ = said;
    LOG_INFO("playerbots", "{}", said);
}
