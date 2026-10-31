/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NonCombatActions.h"
#include "BuyAction.h"  // [b33_dme] CountBagFood
#include "Creature.h"   // [b46_patrol]
#include "Event.h"
#include "HealthTriggers.h"
#include "LeaderOrders.h"  // [lead47] the Rest ack
#include "Playerbots.h"
#include "StringFormat.h"
#include "Timer.h"

#include <unordered_map>

namespace
{
// [b46_patrol] No sitting down while an idle patrol walks in (RestState::PatrolComes); logged every 5 s per bot.
bool PatrolKeepsStanding(Player* bot, char const* what)
{
    float gap = 0.0f;
    Creature* patrol = RestState::PatrolComes(bot, &gap);
    if (!patrol)
        return false;

    thread_local std::unordered_map<ObjectGuid, uint32> logged;
    uint32 const now = getMSTime();
    uint32& last = logged[bot->GetGUID()];
    if (!last || getMSTimeDiff(last, now) >= 5 * IN_MILLISECONDS)
    {
        last = now ? now : 1;
        LOG_INFO("playerbots", "[b46_patrol] rest_patrol_gate: {} does not sit down to {}: patrol {} {} walks in "
                 "({:.1f} yd, mana {:.0f} %, health {:.0f} %)", bot->GetName(), what, patrol->GetName(),
                 patrol->GetEntry(), gap, bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 100.0f,
                 bot->GetHealthPct());
    }
    return true;
}

constexpr uint32 BG_WS_SPELL_WARSONG_FLAG = 23333;
constexpr uint32 BG_WS_SPELL_SILVERWING_FLAG = 23335;
constexpr uint32 BG_EY_NETHERSTORM_FLAG_SPELL = 34976;

bool IsDisallowedFoodState(Player* bot)
{
    if (bot->HasUnitState(UNIT_STATE_LOST_CONTROL))
        return true;

    if (bot->getClass() == CLASS_DRUID)
    {
        ShapeshiftForm form = bot->GetShapeshiftForm();
        return form == FORM_TRAVEL || form == FORM_AQUA || form == FORM_FLIGHT || form == FORM_FLIGHT_EPIC ||
               form == FORM_BEAR || form == FORM_DIREBEAR || form == FORM_CAT;
    }
    else if (bot->getClass() == CLASS_PRIEST)
    {
        return bot->GetShapeshiftForm() == FORM_SPIRITOFREDEMPTION;
    }

    return false;
}
}  // namespace

// Drinking needs a real drink from the bags: the free "food" cheat path is gone. Every sit-down logs its reason.
namespace
{
    // [lead47] the member's ack of the leader's Rest: what it started
    void AckLeaderRest(Player* bot, std::string const& what)
    {
        Leader::MemberEvents events;
        if (Leader::LedBot(bot) && Leader::Events::Get(bot->GetGUID(), events) &&
            events.ready == Leader::ReadyEvent::Rest)
            Leader::Events::Ack(bot->GetGUID(), events.seq, "rest", what);
    }
}

bool DrinkAction::Execute(Event event)
{
    if (IsDisallowedFoodState(bot))
        return false;

    std::string why;
    if (!RestState::WantsDrink(bot, &why))
        why = Acore::StringFormat("mana {:.0f} % < low line {} %", bot->GetPowerPct(POWER_MANA),
                                  sPlayerbotAIConfig.LowMana);
    if (!UseItemAction::Execute(event))
        return false;

    LOG_INFO("playerbots", "[rest] {} drinks: {}", bot->GetName(), why);
    AckLeaderRest(bot, "drinks: " + why);  // [lead47]
    return true;
}

bool DrinkAction::isUseful()
{
    // Between waves and phases the bot is out of combat but the encounter is still running. Non-mana classes never.
    if (!UseItemAction::isUseful() || AI_VALUE(bool, "encounter in progress") || !bot->GetMaxPower(POWER_MANA) ||
        RestState::IsDrinking(bot))
        return false;

    // [b46_patrol] not in a patrol's way, the last-resort drink either (the patrol's first hit stands it up)
    if (PatrolKeepsStanding(bot, "drink"))
        return false;

    // Below the low line the old last-resort drink, even with the group fighting close by; above it only by the
    // rest lines ("rest mana" decides when) and not while the group fights (it would stand up at the first hit).
    if (bot->GetPowerPct(POWER_MANA) < sPlayerbotAIConfig.LowMana)
        return true;
    return !RestState::GroupFighting(bot) && RestState::WantsDrink(bot);
}

bool DrinkAction::isPossible()
{
    if (bot->IsInCombat() || bot->IsMounted() || IsDisallowedFoodState(bot))
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
    if (IsDisallowedFoodState(bot))
        return false;

    std::string why;
    RestState::WantsFood(bot, &why);
    if (!UseItemAction::Execute(event))
        return false;

    LOG_INFO("playerbots", "[rest] {} eats: {}", bot->GetName(), why);
    AckLeaderRest(bot, "eats: " + why);  // [lead47]
    return true;
}

bool EatAction::isUseful()
{
    if (!UseItemAction::isUseful() || AI_VALUE(bool, "encounter in progress") || RestState::IsEating(bot))
        return false;

    if (PatrolKeepsStanding(bot, "eat"))  // [b46_patrol]
        return false;

    // By the rest lines only ("rest health" decides when): a hurt bot with a healer close by gets healed, for free.
    return !RestState::GroupFighting(bot) && RestState::WantsFood(bot);
}

bool EatAction::isPossible()
{
    if (bot->IsInCombat() || bot->IsMounted() || IsDisallowedFoodState(bot))
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
    Creature* patrol = bot->IsInCombat() ? nullptr : RestState::PatrolComes(bot);  // [b46_patrol]
    LOG_INFO("playerbots", "[rest] {} stands up: {}{} (hp {:.0f} %, mana {:.0f} %)", bot->GetName(),
             bot->IsInCombat() ? "a fight started" : patrol ? "[b46_patrol] a patrol walks in: " : "full",
             patrol ? patrol->GetName() : std::string(), bot->GetHealthPct(),
             bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 100.0f);
    bot->SetStandState(UNIT_STAND_STATE_STAND);
    return true;
}

bool StandUpAction::isUseful() { return RestState::RestDone(bot); }
