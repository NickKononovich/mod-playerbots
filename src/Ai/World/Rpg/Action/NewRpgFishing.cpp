/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgFishing.h"

#include <cmath>
#include <mutex>
#include <unordered_map>

#include "Bag.h"
#include "Event.h"
#include "FishingAction.h"
#include "Item.h"
#include "ItemPackets.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "WorldSession.h"

namespace
{
    struct Held
    {
        ObjectGuid mainHand;  // what was in hand before the pole (put back by Stop)
        ObjectGuid offHand;
    };
    std::mutex fishLock;
    std::unordered_map<ObjectGuid::LowType, Held> fishing;  // bots on a fishing task

    bool IsPole(Item const* item)
    {
        if (!item)
            return false;
        ItemTemplate const* proto = item->GetTemplate();
        return proto && proto->Class == ITEM_CLASS_WEAPON && proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE;
    }

    void AutoEquip(Player* bot, Item* item, uint8 slot)
    {
        if (!item || item->IsEquipped())
            return;
        // the same client packet EquipFishingPoleAction sends
        WorldPacket packet(CMSG_AUTOEQUIP_ITEM_SLOT, 9);
        packet << item->GetGUID() << slot;
        WorldPackets::Item::AutoEquipItemSlot equip(std::move(packet));
        equip.Read();
        bot->GetSession()->HandleAutoEquipItemSlotOpcode(equip);
    }
}

namespace
{
    // the bot's own pole: in hand, in the backpack or in a bag
    Item* FindPole(Player* bot)
    {
        if (Item* hand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND); IsPole(hand))
            return hand;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); IsPole(item))
                return item;
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = bot->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    if (Item* item = bag->GetItemByPos(uint8(j)); IsPole(item))
                        return item;
        return nullptr;
    }
}

bool NewRpgFishing::HasPole(Player* bot) { return FindPole(bot) != nullptr; }

bool NewRpgFishing::PutPoleAway(Player* bot)
{
    // [fish14] b354 Sycina L11 (rogue) fought the whole run with its Fishing Pole in its main hand (DB: slot 15 Fishing
    // Pole, a Stiletto and a Pruning Knife in its bags), ~4.5 dps: a fishing task of an earlier session ended without
    // Stop (the logout, a task taken away), so nothing put its weapons back. Out of a fishing task the pole is put
    // away for the best main-hand weapon in its bags (by its damage per second), as Stop would have
    if (!bot || !bot->IsAlive() || bot->IsInCombat() || IsFishing(bot) ||
        !IsPole(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND)))
        return false;
    Item* best = nullptr;
    float bestDps = 0.0f;
    auto const consider = [&](Item* item)
    {
        if (!item || IsPole(item))
            return;
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->Class != ITEM_CLASS_WEAPON)
            return;
        uint16 dest = 0;
        if (bot->CanEquipItem(EQUIPMENT_SLOT_MAINHAND, dest, item, true) != EQUIP_ERR_OK ||
            dest != ((INVENTORY_SLOT_BAG_0 << 8) | EQUIPMENT_SLOT_MAINHAND))
            return;
        if (float const dps = proto->getDPS(); !best || dps > bestDps)
        {
            best = item;
            bestDps = dps;
        }
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
        if (Bag* bag = bot->GetBagByPos(b))
            for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                consider(bag->GetItemByPos(uint8(j)));
    if (!best)
        return false;
    LOG_INFO("playerbots", "[fish14] {} L{} puts its Fishing Pole away: {} ({:.1f} dps) back in its main hand",
             bot->GetName(), bot->GetLevel(), best->GetTemplate()->Name1, bestDps);
    AutoEquip(bot, best, EQUIPMENT_SLOT_MAINHAND);
    return true;
}

bool NewRpgFishing::IsFishing(Player* bot)
{
    std::lock_guard<std::mutex> guard(fishLock);
    return fishing.count(bot->GetGUID().GetCounter()) != 0;
}

void NewRpgFishing::Stop(PlayerbotAI* botAI, Player* bot)
{
    Held held;
    {
        std::lock_guard<std::mutex> guard(fishLock);
        auto const itr = fishing.find(bot->GetGUID().GetCounter());
        if (itr == fishing.end())
            return;
        held = itr->second;
        fishing.erase(itr);
    }
    if (Spell* channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        if (channel->m_spellInfo->Id == FISHING_SPELL)
            bot->InterruptSpell(CURRENT_CHANNELED_SPELL);
    botAI->ChangeStrategy("-use bobber", BOT_STATE_NON_COMBAT);
    if (IsPole(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND)))
    {
        AutoEquip(bot, bot->GetItemByGuid(held.mainHand), EQUIPMENT_SLOT_MAINHAND);
        AutoEquip(bot, bot->GetItemByGuid(held.offHand), EQUIPMENT_SLOT_OFFHAND);
    }
    LOG_INFO("playerbots", "[gather] fish {} stop", bot->GetName());
}

NewRpgFishing::Step NewRpgFishing::Tick(PlayerbotAI* botAI, Player* bot, WorldPosition const& water,
                                        WorldPosition& walkTo, std::string& why)
{
    if (!bot->IsAlive() || bot->IsInCombat())
        return Step::NONE;
    if (!bot->HasSkill(SKILL_FISHING))
    {
        why = "no Fishing skill";
        return Step::DONE;
    }
    if (!HasPole(bot))
    {
        why = "no Fishing Pole (bought at a vendor: NewRpgProfession)";
        return Step::DONE;
    }

    // a cast is out: click the bobber when it bites (GO_READY), else wait at the water
    if (Spell* channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        if (channel->m_spellInfo->Id == FISHING_SPELL)
        {
            UseBobberAction bobber(botAI);
            if (bobber.isUseful())
                bobber.Execute(Event());
            return Step::BUSY;
        }

    // far from the water: walk to a point short of the marker by half the search window (the shore, not into it);
    // MoveNearWaterAction then finds the land spot within the window
    float const window = sPlayerbotAIConfig.FishingDistance;
    if (bot->GetMapId() != water.GetMapId() || bot->GetExactDist2d(water.GetPositionX(), water.GetPositionY()) > window)
    {
        walkTo = water;
        if (bot->GetMapId() == water.GetMapId())
        {
            float const dx = bot->GetPositionX() - water.GetPositionX();
            float const dy = bot->GetPositionY() - water.GetPositionY();
            float const d = std::sqrt(dx * dx + dy * dy);
            if (d > 0.0f)
                walkTo = WorldPosition(water.GetMapId(), water.GetPositionX() + dx / d * window / 2.0f,
                                       water.GetPositionY() + dy / d * window / 2.0f, water.GetPositionZ());
        }
        return Step::WALK;
    }

    {
        std::lock_guard<std::mutex> guard(fishLock);
        auto const [itr, added] = fishing.try_emplace(bot->GetGUID().GetCounter());
        if (added)
        {
            Item* main = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
            Item* off = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            if (!IsPole(main))
            {
                itr->second.mainHand = main ? main->GetGUID() : ObjectGuid::Empty;
                itr->second.offHand = off ? off->GetGUID() : ObjectGuid::Empty;
            }
            LOG_INFO("playerbots", "[gather] fish {} start at map {} ({:.0f}, {:.0f}) skill {}", bot->GetName(),
                     water.GetMapId(), water.GetPositionX(), water.GetPositionY(),
                     bot->GetSkillValue(SKILL_FISHING));
        }
    }

    // [gather] the pole in hand first: FishingAction's own equip looks in the backpack only, a pole in a bag was
    // never equipped and the cast failed without one
    if (Item* pole = FindPole(bot); pole && !pole->IsEquipped())
    {
        std::string const name = pole->GetTemplate()->Name1;
        AutoEquip(bot, pole, EQUIPMENT_SLOT_MAINHAND);
        if (!IsPole(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND)))
        {
            why = "cannot equip its " + name;
            return Step::DONE;
        }
        LOG_INFO("playerbots", "[gather] fish {} equips {}", bot->GetName(), name);
        return Step::BUSY;
    }

    // the shore spot: MoveNearWaterAction sets the "fishing spot" value (water in cast range from here, or a land spot
    // next to the nearest water within the window) and walks there
    MoveNearWaterAction shore(botAI);
    if (shore.isPossible() && shore.isUseful() && shore.Execute(Event()))
        return Step::BUSY;

    FishingAction cast(botAI);
    if (cast.isUseful() && cast.Execute(Event()))
        return Step::BUSY;

    why = "no fishable water within " + std::to_string(uint32(window)) + " yd of the place";
    return Step::DONE;
}
