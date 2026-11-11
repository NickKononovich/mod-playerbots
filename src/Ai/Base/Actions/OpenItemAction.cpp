/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "OpenItemAction.h"

#include "AiObjectContext.h"
#include "Bag.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "LootMgr.h"
#include "LootObjectStack.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "SharedDefines.h"
#include "WorldPacket.h"

bool OpenItemAction::Execute(Event /*event*/)
{
    bool foundOpenable = false;

    Item* item = botAI->FindOpenableItem();
    if (item)
    {
        uint8 bag = item->GetBagSlot();  // Retrieves the bag slot (0 for main inventory)
        uint8 slot = item->GetSlot();    // Retrieves the actual slot inside the bag

        OpenItem(item, bag, slot);
        foundOpenable = true;
    }
    else if (Item* container = FindContainer())
        foundOpenable = UseContainer(container);

    return foundOpenable;
}

// [ench168] a container opened by its use spell, as a player opens it: the clams (Small Barnacled / Thick-shelled /
// Big-mouth Clam, their spell's SPELL_EFFECT_CREATE_RANDOM_ITEM rolls spell_loot_template: Clam Meat, Tangy Clam Meat,
// Zesty Clam Meat). CMSG_OPEN_ITEM opens only ITEM_FLAG_HAS_LOOT items, so the bots kept every clam (b442 sold as "no
// upgrade") and the cooks' walks had no clam meat but the 0.02-0.1 % direct drops
Item* OpenItemAction::FindContainer() const
{
    auto const isContainer = [](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        return proto && proto->Class != ITEM_CLASS_QUEST && proto->Spells[0].SpellId > 0 &&
               proto->Spells[0].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE &&
               LootTemplates_Spell.HaveLootFor(uint32(proto->Spells[0].SpellId));
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); isContainer(item))
            return item;
    for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
        if (Bag* bag = bot->GetBagByPos(b))
            for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                if (Item* item = bag->GetItemByPos(uint8(j)); isContainer(item))
                    return item;
    return nullptr;
}

bool OpenItemAction::UseContainer(Item* item)
{
    if (bot->IsNonMeleeSpellCast(false) || bot->CanUseItem(item) != EQUIP_ERR_OK)
        return false;
    uint32 const spellId = uint32(item->GetTemplate()->Spells[0].SpellId);
    WorldPacket packet(CMSG_USE_ITEM);
    packet << item->GetBagSlot() << item->GetSlot() << uint8(1) << spellId << item->GetGUID() << uint32(0) << uint8(0)
           << uint32(TARGET_FLAG_NONE);
    LOG_INFO("playerbots", "[ench168] {} opens {}", bot->GetName(), item->GetTemplate()->Name1);
    bot->GetSession()->HandleUseItemOpcode(packet);
    return true;
}

void OpenItemAction::OpenItem(Item* item, uint8 bag, uint8 slot)
{
    ObjectGuid const itemGuid = item->GetGUID();
    std::ostringstream out;
    out << "Opened item: " << item->GetTemplate()->Name1;

    WorldPacket packet(CMSG_OPEN_ITEM);
    packet << bag << slot;
    bot->GetSession()->HandleOpenItemOpcode(packet);

    // Store the item GUID as the loot target
    LootObject lootObject;
    lootObject.guid = itemGuid;
    botAI->GetAiObjectContext()->GetValue<LootObject>("loot target")->Set(lootObject);

    botAI->TellMaster(out.str());
}
