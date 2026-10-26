/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GiveItemAction.h"
#include "Event.h"
#include "ItemCountValue.h"
#include "Map.h"
#include "Playerbots.h"

std::vector<std::string> split(std::string const s, char delim);

bool GiveItemAction::Execute(Event /*event*/)
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    Player* receiver = dynamic_cast<Player*>(target);
    if (!receiver)
        return false;

    PlayerbotAI* receiverAi = GET_PLAYERBOT_AI(receiver);
    if (!receiverAi)
        return false;

    if (receiverAi->GetAiObjectContext()->GetValue<uint32>("item count", item)->Get())
        return true;

    std::vector<Item*> items = InventoryAction::parseItems(item, ITERATE_ITEMS_IN_BAGS);
    for (Item* item : items)
    {
        if (receiver->CanUseItem(item->GetTemplate()) != EQUIP_ERR_OK)
            continue;

        ItemPosCountVec dest;
        InventoryResult msg = receiver->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false);
        if (msg == EQUIP_ERR_OK)
        {
            std::ostringstream out;
            out << "Got " << chat->FormatItem(item->GetTemplate(), item->GetCount()) << " from " << bot->GetName();

            bot->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
            item->SetOwnerGUID(target->GetGUID());
            receiver->MoveItemToInventory(dest, item, true);

            receiverAi->TellMasterNoFacing(out.str());
            // [give_one_stack] one stack per receiver: the loop handed every conjured stack to the first member,
            // the giver had none left and conjured again at once (hor-h-5A-1, 50 s of conjure / give at the start)
            break;
        }
        else
        {
            std::ostringstream out;
            out << "Cannot get " << chat->FormatItem(item->GetTemplate(), item->GetCount()) << " from "
                << bot->GetName() << "- my bags are full";
            receiverAi->TellError(out.str());
        }
    }

    return true;
}

Unit* GiveItemAction::GetTarget() { return AI_VALUE2(Unit*, "party member without item", item); }

bool GiveItemAction::isUseful()
{
    return GetTarget() && AI_VALUE2(uint8, "mana", "self target") > sPlayerbotAIConfig.LowMana;
}

namespace
{
// [conjure_outside_instances] food / water is handed out before the run, not in a dungeon or raid out of combat
bool GiveBlockedInInstance(Player* bot)
{
    Map* map = bot->GetMap();
    return map && map->Instanceable() && !map->IsBattlegroundOrArena() && !bot->IsInCombat();
}
}  // namespace

Unit* GiveFoodAction::GetTarget() { return AI_VALUE(Unit*, "party member without food"); }

bool GiveFoodAction::isUseful()
{
    return !GiveBlockedInInstance(bot) && GetTarget() != nullptr;
}

Unit* GiveWaterAction::GetTarget() { return AI_VALUE(Unit*, "party member without water"); }

bool GiveWaterAction::isUseful()
{
    return !GiveBlockedInInstance(bot) && GetTarget() != nullptr;
}
