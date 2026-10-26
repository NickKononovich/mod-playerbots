/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NonCombatActions.h"
#include "BuyAction.h"  // [b33_dme] CountBagFood
#include "Event.h"
#include "HealthTriggers.h"
#include "Playerbots.h"
#include "StringFormat.h"

namespace
{
constexpr uint32 BG_WS_SPELL_WARSONG_FLAG = 23333;
constexpr uint32 BG_WS_SPELL_SILVERWING_FLAG = 23335;
constexpr uint32 BG_EY_NETHERSTORM_FLAG_SPELL = 34976;

bool IsDisallowedShapeshiftForm(Player* bot)
{
    if (bot->getClass() == CLASS_DRUID)
    {
        ShapeshiftForm form = bot->GetShapeshiftForm();
        return form == FORM_TRAVEL || form == FORM_AQUA ||
               form == FORM_FLIGHT || form == FORM_FLIGHT_EPIC ||
               form == FORM_BEAR || form == FORM_DIREBEAR ||
               form == FORM_CAT;
    }
    else if (bot->getClass() == CLASS_PRIEST)
    {
        return bot->GetShapeshiftForm() == FORM_SPIRITOFREDEMPTION;
    }

    return false;
}
}

// Drinking needs a real drink from the bags: the free "food" cheat path is gone. Every sit-down logs its reason.
bool DrinkAction::Execute(Event event)
{
    std::string why;
    if (!RestState::WantsDrink(bot, &why))
        why = Acore::StringFormat("mana {:.0f} % < low line {} %", bot->GetPowerPct(POWER_MANA),
                                  sPlayerbotAIConfig.LowMana);
    if (!UseItemAction::Execute(event))
        return false;

    LOG_INFO("playerbots", "[rest] {} drinks: {}", bot->GetName(), why);
    return true;
}

bool DrinkAction::isUseful()
{
    // Between waves and phases the bot is out of combat but the encounter is still running. Non-mana classes never.
    if (!UseItemAction::isUseful() || AI_VALUE(bool, "encounter in progress") || !bot->GetMaxPower(POWER_MANA) ||
        RestState::IsDrinking(bot))
        return false;

    // Below the low line the old last-resort drink, even with the group fighting close by; above it only by the
    // rest lines ("rest mana" decides when) and not while the group fights (it would stand up at the first hit).
    if (bot->GetPowerPct(POWER_MANA) < sPlayerbotAIConfig.LowMana)
        return true;
    return !RestState::GroupFighting(bot) && RestState::WantsDrink(bot);
}

bool DrinkAction::isPossible()
{
    if (bot->IsInCombat() || bot->IsMounted() || IsDisallowedShapeshiftForm(bot))
        return false;

    // [b33_dme] No real drink (spell category 59) in the bags: the "drink" item list falls back to food
    // (InventoryAction), whose aura is SPELL_SPECIFIC_FOOD - IsDrinking stays false, so the drink re-runs every tick,
    // and RestDone stands the bot up as "full" once its health is 100 % (manaDone = !drinking). scholo-n
    // 20261026-000434-5H-4: the bear tank (out of water, restock failed in the dungeon) sat 15-20 s on food,
    // stood up "full" at mana 39-54 %, sat again - 137 s at Jandice Barov, steer_rest_not_stuck "eating". The harness
    // takes such a bot as "no drink in the bags, spirit regen only" (regen_stall, then released).
    if (!CountBagFood(bot, RESTOCK_SPELL_CATEGORY_DRINK))
        return false;

    if (bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) || bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) ||
        bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL))
    {
        return false;
    }

    return UseItemAction::isPossible();
}

bool EatAction::Execute(Event event)
{
    std::string why;
    RestState::WantsFood(bot, &why);
    if (!UseItemAction::Execute(event))
        return false;

    LOG_INFO("playerbots", "[rest] {} eats: {}", bot->GetName(), why);
    return true;
}

bool EatAction::isUseful()
{
    if (!UseItemAction::isUseful() || AI_VALUE(bool, "encounter in progress") || RestState::IsEating(bot))
        return false;

    // By the rest lines only ("rest health" decides when): a hurt bot with a healer close by gets healed, for free.
    return !RestState::GroupFighting(bot) && RestState::WantsFood(bot);
}

bool EatAction::isPossible()
{
    if (bot->IsInCombat() || bot->IsMounted() || IsDisallowedShapeshiftForm(bot))
        return false;

    if (bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) || bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) ||
        bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL))
    {
        return false;
    }

    return UseItemAction::isPossible();
}

bool StandUpAction::Execute(Event /*event*/)
{
    // Standing up removes the food / drink auras (AURA_INTERRUPT_FLAG_NOT_SEATED), nothing else.
    LOG_INFO("playerbots", "[rest] {} stands up: {} (hp {:.0f} %, mana {:.0f} %)", bot->GetName(),
             bot->IsInCombat() ? "a fight started" : "full", bot->GetHealthPct(),
             bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 100.0f);
    bot->SetStandState(UNIT_STAND_STATE_STAND);
    return true;
}

bool StandUpAction::isUseful() { return RestState::RestDone(bot); }
