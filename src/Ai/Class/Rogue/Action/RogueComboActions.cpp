/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RogueComboActions.h"
#include "Playerbots.h"
#include "SpellMgr.h"

#include <algorithm>

constexpr int32 SLICE_AND_DICE_ENERGY_COST = 25;
constexpr int32 SLICE_AND_DICE_POOL_WINDOW_MS = 3000;

bool CastComboAction::isUseful()
{
    return CastMeleeSpellAction::isUseful() && AI_VALUE2(uint8, "combo", "current target") < 5;
}

bool CastRogueBuilderAction::isUseful()
{
    if (!CastSpellAction::isUseful())
        return false;

    if (!AI_VALUE2(uint8, "combo", "current target"))
        return true;

    Aura* sliceAndDice = botAI->GetAura("slice and dice", bot);
    if (!sliceAndDice || sliceAndDice->GetDuration() > SLICE_AND_DICE_POOL_WINDOW_MS)
        return true;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(AI_VALUE2(uint32, "spell id", spell));
    if (!spellInfo)
        return true;

    int32 const builderCost = spellInfo->CalcPowerCost(bot, spellInfo->GetSchoolMask());
    return bot->GetPower(POWER_ENERGY) >= static_cast<uint32>(builderCost + SLICE_AND_DICE_ENERGY_COST);
}

// [melee_audit_b30] Mutilate (60 energy) failed its cast short of energy and the node fell through Backstab and
// Hemorrhage to Sinister Strike (45): build 30 Assassination 80 cast Sinister Strike 211 times against 146
// Mutilates, one combo point per 45 energy instead of two to three per 60. With a dagger in the main hand (Mutilate
// is castable at all) the bot now waits for the energy; without one the old fallback chain stays.
bool CastMutilateAction::isUseful()
{
    if (!CastRogueBuilderAction::isUseful())
        return false;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!spellInfo)
        return true;

    // [b44_cos] Mutilate needs a dagger in BOTH hands (Spell::CheckItems, ..._EQUIPPED_ITEM_CLASS_OFFHAND): a
    // dagger / sword rogue claimed it every tick on the main-hand test alone and failed it. Not dagger / dagger: not
    // useful, the node's fallback (Backstab, Hemorrhage, Sinister Strike) takes the builder.
    auto const isDagger = [this](uint8 slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        return item && !item->IsBroken() && item->GetTemplate()->Class == ITEM_CLASS_WEAPON &&
               item->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER;
    };
    if (!isDagger(EQUIPMENT_SLOT_MAINHAND) || !isDagger(EQUIPMENT_SLOT_OFFHAND))
        return false;

    int32 const cost = spellInfo->CalcPowerCost(bot, spellInfo->GetSchoolMask());
    return bot->GetPower(POWER_ENERGY) >= static_cast<uint32>(std::max(cost, 0));
}
