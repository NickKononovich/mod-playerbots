/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ImbueAction.h"
#include "Event.h"
#include "Group.h"
#include "Log.h"
#include "Playerbots.h"
#include "RoguePoisons.h"
#include "Timer.h"

#include <algorithm>

namespace
{
    // [rjoin-imbue] Sunken Temple 2026-10-25 (20261025-081744-st-n-5A-92): "apply oil" / "apply stone" "executed" on
    // every "often" tick of a hunter and a mage that had nothing to apply (always true, no isUseful), stood eating
    // bots up, and ran while the rest of the group fought (the hunter's whole pull 48). A weapon enhancement is put on
    // between fights, once: never while the bot or a member of its group nearby fights, only on a weapon without
    // one, and a look that found nothing to apply is not repeated for IMBUE_RECHECK_MS.
    constexpr uint32 IMBUE_RECHECK_MS = 30 * IN_MILLISECONDS;
    constexpr float IMBUE_GROUP_FIGHT_RANGE = 100.0f;

    bool GroupFights(Player* bot)
    {
        if (bot->IsInCombat())
            return true;

        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsAlive() && member->IsInCombat() &&
                member->GetMap() == bot->GetMap() && bot->IsWithinDist(member, IMBUE_GROUP_FIGHT_RANGE))
                return true;
        }

        return false;
    }

    // [b34_gnomer] gnomer-n 5B 20261026-014521: a warrior tank's off hand is a shield, "apply stone" cast a
    // sharpening stone on it every tick (the stone matched the shield's subclass 6 as a polearm, the cast failed,
    // Done(true) cleared the backoff). Only a weapon counts; a slot that already carries any temporary enchant is
    // never overwritten (so a better enhancement is kept); an apply that did not take backs off, doubling.
    constexpr uint32 IMBUE_VERIFY_MS = 10 * IN_MILLISECONDS;
    constexpr uint32 IMBUE_FAILED_BACKOFF_MS = 30 * IN_MILLISECONDS;
    constexpr uint32 IMBUE_FAILED_BACKOFF_MAX_MS = 10 * MINUTE * IN_MILLISECONDS;

    bool LacksTempEnchant(Player* bot, uint8 slot)
    {
        Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        return weapon && weapon->GetTemplate() && weapon->GetTemplate()->Class == ITEM_CLASS_WEAPON &&
               weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) == 0;
    }

    uint8 SlotBit(uint8 slot) { return slot == EQUIPMENT_SLOT_OFFHAND ? 2 : 1; }

    bool ImbueUseful(Player* bot, uint32 retryAfterMs, bool offhand)
    {
        if (retryAfterMs && static_cast<int32>(retryAfterMs - getMSTime()) > 0)
            return false;

        if (!LacksTempEnchant(bot, EQUIPMENT_SLOT_MAINHAND) &&
            (!offhand || !LacksTempEnchant(bot, EQUIPMENT_SLOT_OFFHAND)))
            return false;

        return !GroupFights(bot);
    }

    void LogImbue(Player* bot, char const* what, Item* item, uint8 slot)
    {
        LOG_INFO("playerbots", "[rjoin-imbue] {} applies {} {} to the {} hand", bot->GetName(), what,
                 item->GetTemplate()->Name1, slot == EQUIPMENT_SLOT_OFFHAND ? "off" : "main");
    }
}

void ImbueAction::PrepareImbue()
{
    // remove stealth
    if (bot->HasAura(SPELL_AURA_MOD_STEALTH))
        bot->RemoveAurasByType(SPELL_AURA_MOD_STEALTH);

    if (bot->getStandState() != UNIT_STAND_STATE_STAND)
        bot->SetStandState(UNIT_STAND_STATE_STAND);
}

void ImbueAction::Tried(uint8 slot) { _triedSlotMask |= SlotBit(slot); }

bool ImbueAction::Done(bool applied)
{
    // [b34_gnomer] after an apply, wait for the cast before isUseful() checks whether it took (was: retry at once).
    _retryAfterMs = getMSTime() + (applied ? IMBUE_VERIFY_MS : IMBUE_RECHECK_MS);
    return applied;
}

bool ImbueAction::isUseful()
{
    // [b34_gnomer] verify the last apply: a tried slot still without an enhancement is a failed apply (wrong item,
    // interrupted cast, rejected spell), and the action backs off 30 s, 60 s, ... up to 10 min instead of repeating.
    if (_triedSlotMask && !(_retryAfterMs && static_cast<int32>(_retryAfterMs - getMSTime()) > 0))
    {
        bool failed = false;
        for (uint8 slot : { EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND })
            if ((_triedSlotMask & SlotBit(slot)) && LacksTempEnchant(bot, slot))
                failed = true;

        _triedSlotMask = 0;
        if (failed)
        {
            uint32 backoff = IMBUE_FAILED_BACKOFF_MS << std::min<uint8>(_failures, 5);
            if (_failures < 255)
                ++_failures;

            _retryAfterMs = getMSTime() + std::min(backoff, IMBUE_FAILED_BACKOFF_MAX_MS);
            LOG_INFO("playerbots", "[b34_gnomer] {} {} did not take, retry in {}s", bot->GetName(), getName(),
                     std::min(backoff, IMBUE_FAILED_BACKOFF_MAX_MS) / IN_MILLISECONDS);
            return false;
        }

        _failures = 0;
    }

    return ImbueUseful(bot, _retryAfterMs, _offhand);
}

ImbueWithPoisonAction::ImbueWithPoisonAction(PlayerbotAI* botAI) : ImbueAction(botAI, "apply poison", true) {}

bool ImbueWithPoisonAction::Execute(Event /*event*/)
{
    if (GroupFights(bot))
        return false;

    bool applied = false;

    // [b31_wr] Which poison goes on which hand (spec, Wound next to another rogue's Deadly) is RoguePoisons' call, the
    // same as the rogue's own poison actions; this was a second, hard-wired Instant MH / Deadly OH copy.
    for (uint8 slot : { EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND })
    {
        if (!LacksTempEnchant(bot, slot))
            continue;

        Item* poison = RoguePoisons::ItemFor(botAI, slot);
        if (!poison)
            continue;

        PrepareImbue();
        LogImbue(bot, "poison", poison, slot);
        botAI->ImbueItem(poison, slot);
        Tried(slot);  // [b34_gnomer]
        botAI->SetNextCheckDelay(5);
        applied = true;
    }

    return Done(applied);
}

// Search and apply stone to weapons
ImbueWithStoneAction::ImbueWithStoneAction(PlayerbotAI* botAI) : ImbueAction(botAI, "apply stone", true) {}

bool ImbueWithStoneAction::Execute(Event /*event*/)
{
    if (GroupFights(bot))
        return false;

    bool applied = false;

    // Search and apply stone to weapons
    // Mainhand ...
    Item* stone = nullptr;
    Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (weapon && weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) == 0)
    {
        stone = botAI->FindStoneFor(weapon);
        if (stone)
        {
            PrepareImbue();
            LogImbue(bot, "stone", stone, EQUIPMENT_SLOT_MAINHAND);
            botAI->ImbueItem(stone, EQUIPMENT_SLOT_MAINHAND);
            Tried(EQUIPMENT_SLOT_MAINHAND);  // [b34_gnomer]
            botAI->SetNextCheckDelay(5);
            applied = true;
        }
    }

    //... and offhand
    weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (weapon && weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) == 0)
    {
        stone = botAI->FindStoneFor(weapon);
        if (stone)
        {
            PrepareImbue();
            LogImbue(bot, "stone", stone, EQUIPMENT_SLOT_OFFHAND);
            botAI->ImbueItem(stone, EQUIPMENT_SLOT_OFFHAND);
            Tried(EQUIPMENT_SLOT_OFFHAND);  // [b34_gnomer]
            botAI->SetNextCheckDelay(5);
            applied = true;
        }
    }

    return Done(applied);
}

// Search and apply oil to weapons
ImbueWithOilAction::ImbueWithOilAction(PlayerbotAI* botAI) : ImbueAction(botAI, "apply oil", false) {}

bool ImbueWithOilAction::Execute(Event /*event*/)
{
    if (GroupFights(bot))
        return false;

    bool applied = false;

    // Search and apply oil to weapons
    Item* oil = nullptr;
    Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (weapon && weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) == 0)
    {
        oil = botAI->FindOilFor(weapon);
        if (oil)
        {
            PrepareImbue();
            LogImbue(bot, "oil", oil, EQUIPMENT_SLOT_MAINHAND);
            botAI->ImbueItem(oil, EQUIPMENT_SLOT_MAINHAND);
            Tried(EQUIPMENT_SLOT_MAINHAND);  // [b34_gnomer]
            botAI->SetNextCheckDelay(5);
            applied = true;
        }
    }

    return Done(applied);
}

static const uint32 uPrioritizedHealingItemIds[19] = {
    HEALTHSTONE,
    FEL_REGENERATION_POTION,
    SUPER_HEALING_POTION,
    CRYSTAL_HEALING_POTION,
    MAJOR_DREAMLESS_SLEEP_POTION,
    VOLATILE_HEALING_POTION,
    MAJOR_HEALING_POTION,
    WHIPPER_ROOT_TUBER,
    NIGHT_DRAGON_BREATH,
    LIMITED_INVULNERABILITY_POTION,
    GREATER_DREAMLESS_SLEEP_POTION,
    SUPERIOR_HEALING_POTION,
    CRYSTAL_RESTORE,
    DREAMLESS_SLEEP_POTION,
    GREATER_HEALING_POTION,
    HEALING_POTION,
    LESSER_HEALING_POTION,
    DISCOLORED_HEALING_POTION,
    MINOR_HEALING_POTION,
};

TryEmergencyAction::TryEmergencyAction(PlayerbotAI* botAI) : Action(botAI, "try emergency") {}

bool TryEmergencyAction::Execute(Event /*event*/)
{
    // Do not use consumable if bot can heal self
    if ((PlayerbotAI::IsHeal(bot)) && (bot->GetPowerPct(POWER_MANA) > 20))
        return false;

    // If bot does not have aggro: use bandage instead of potion/stone/crystal
    if ((!AI_VALUE(uint8, "my attacker count")) && !bot->HasAura(11196))  // Recently bandaged
    {
        if (Item* bandage = botAI->FindBandage())
        {
            botAI->ImbueItem(bandage, bot);
            botAI->SetNextCheckDelay(8);
        }
    }

    // Else loop over the list of health consumable to pick one
    for (uint8 i = 0; i < std::size(uPrioritizedHealingItemIds); ++i)
    {
        if (Item* healthItem = botAI->FindConsumable(uPrioritizedHealingItemIds[i]))
        {
            botAI->ImbueItem(healthItem);
        }
    }

    return true;
}
