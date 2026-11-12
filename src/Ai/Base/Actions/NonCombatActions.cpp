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
#include "MotionMaster.h"  // [dun238]
#include "Playerbots.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Transport.h"  // [ride133]
#include "TravelMgr.h"  // [ride133] the trip's exit stop

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

// [ride133] aboard a transport docked at the stop its trip gets off at: stepping off wins over a sit-down (the bot
// drinks on the pier). b426 Bhogagnuth L13 rode Deeprun Tram car 176083 for 1497 s (one way is 63 s): at every stop it
// sat down to drink ("mana 38 % < 50 %"), the stop ended under it, the car took it back
bool AtTripExitStop(Player* bot, PlayerbotAI* botAI)
{
    Transport* transport = bot->GetTransport();
    if (!transport || !botAI)
        return false;
    NewRpgInfo::Trip const& trip = botAI->rpgInfo.trip;
    TravelRouteMgr::Line const* line = sTravelRouteMgr.GetLine(transport->GetEntry());
    if (!line || trip.phase != NewRpgInfo::TripPhase::Ride || trip.leg.lineEntry != line->entry ||
        trip.leg.toDock >= line->docks.size())
        return false;
    uint32 remaining = 0;
    return TravelRouteMgr::IsDocked(transport, line->docks[trip.leg.toDock], remaining);
}

bool IsDisallowedFoodState(Player* bot)
{
    if (bot->HasUnitState(UNIT_STATE_LOST_CONTROL))
        return true;
    // [dun238] no meal in the air: sitting down clears the jump's spline and the bot hangs where it was (b446 wc-n
    // five2-2: Thamesh's Rest drink mid-jump down Verdan's ledge, 17.5 yd over the landing)
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == EFFECT_MOTION_TYPE ||
        bot->HasUnitMovementFlag(MOVEMENTFLAG_FALLING))
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

// [lead1261] b249 rfc-h 5H-8 574-646 s: the led bear tank at 48 % health stood under the leader's Rest for 72 s
// ("under the leader's Rest, does not sit down", food 60 in its bags; the same in b248 5H-8 702 s) - no led bear or
// cat druid ate in either batch: food is not eaten in an animal form and nothing left it. The leader's Rest is the
// command: the druid leaves its bear / cat form for it (the "food" node's "leave animal form", as Thorns does)
bool LeavesAnimalFormToEat(Player* bot)
{
    if (bot->getClass() != CLASS_DRUID || bot->HasUnitState(UNIT_STATE_LOST_CONTROL) ||
        !RestState::LeaderRests(bot))
        return false;
    ShapeshiftForm const form = bot->GetShapeshiftForm();
    return form == FORM_BEAR || form == FORM_DIREBEAR || form == FORM_CAT;
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
        RestState::IsDrinking(bot) || AtTripExitStop(bot, botAI))
        return false;

    // [b46_patrol] not in a patrol's way, the last-resort drink either (the patrol's first hit stands it up)
    if (PatrolKeepsStanding(bot, "drink"))
        return false;

    // [lead150] user: rest is the leader's decision - a led executor member drinks only under the leader's Rest
    // (b170 356-428 s: the hunter's own last-resort drink at 1-7 % mana against the leader's Follow)
    // [lead473] the led tank too: the leader's Rest covers it (its lines, "who": "tank"); b207 rfc-h 5B 1047-1059 s and
    // 1111-1120 s the warrior tank ate on its own line ("hp 97 % < 100 %, healer mana 29 %") with no Rest from the
    // leader, its meal held the pull ("held for now: the tank eats ...") - two deciders for one rest
    if (Leader::ExecutorMember(bot) || Leader::LedBot(bot))
        return RestState::LeaderRests(bot);

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
    // [craft400] the food of the least worth that still covers the rest's health need (its restore, at most the max
    // health, against the missing health), the old food first while it fills; none covers it: the one restoring
    // most. Before, the highest-level food went first (the "inventory items" order) and the outgrown stacks stayed
    Item* pick = nullptr;
    if (event.getParam().empty())
    {
        float const missing = float(bot->GetMaxHealth() - std::min(bot->GetHealth(), bot->GetMaxHealth()));
        float const maxHealth = float(std::max<uint32>(1, bot->GetMaxHealth()));
        float pickRestore = 0.0f;
        bool pickCovers = false;
        for (Item* item : AI_VALUE2(std::vector<Item*>, "inventory items", "food"))
        {
            float const restore = item ? FoodItemRests(bot, item->GetTemplate()) * maxHealth : 0.0f;
            if (restore <= 0.0f)
                continue;
            bool const covers = restore >= missing;
            if (!pick || (covers && (!pickCovers || restore < pickRestore)) || (!covers && !pickCovers &&
                                                                                  restore > pickRestore))
            {
                pick = item;
                pickRestore = restore;
                pickCovers = covers;
            }
        }
    }
    if (pick ? !UseItemAuto(pick) : !UseItemAction::Execute(event))
        return false;

    LOG_INFO("playerbots", "[rest] {} eats: {}", bot->GetName(), why);
    AckLeaderRest(bot, "eats: " + why);  // [lead47]
    return true;
}

bool EatAction::isUseful()
{
    if (!UseItemAction::isUseful() || AI_VALUE(bool, "encounter in progress") || RestState::IsEating(bot) ||
        AtTripExitStop(bot, botAI))  // [ride133]
        return false;

    if (PatrolKeepsStanding(bot, "eat"))  // [b46_patrol]
        return false;
    if (Leader::ExecutorMember(bot) || Leader::LedBot(bot))  // [lead150] [lead473] the leader's Rest only
        return RestState::LeaderRests(bot);

    // By the rest lines only ("rest health" decides when): a hurt bot with a healer close by gets healed, for free.
    return !RestState::GroupFighting(bot) && RestState::WantsFood(bot);
}

bool EatAction::isPossible()
{
    // [lead1261] a led druid under the leader's Rest in bear / cat form: possible, its prerequisite leaves the form
    // (Execute still eats in no animal form)
    if (bot->IsInCombat() || bot->IsMounted() || (IsDisallowedFoodState(bot) && !LeavesAnimalFormToEat(bot)))
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
    if (!bot->IsInCombat())
        RestState::NoteStoodFull(bot);  // [dun242] (only at 99 % or more)
    // Standing up removes the food / drink auras (AURA_INTERRUPT_FLAG_NOT_SEATED), nothing else.
    Creature* patrol = bot->IsInCombat() ? nullptr : RestState::PatrolComes(bot);  // [b46_patrol]
    LOG_INFO("playerbots", "[rest] {} stands up: {}{} (hp {:.0f} %, mana {:.0f} %)", bot->GetName(),
             bot->IsInCombat() ? "a fight started"
             : patrol          ? "[b46_patrol] a patrol walks in: "
             : RestState::IsDrinking(bot) && bot->GetMaxPower(POWER_MANA) && bot->GetPowerPct(POWER_MANA) < 100.0f
                 ? "mana at its line"  // [lead221]
             // [death404] a drink alone ends at full mana, its health as it is (b453 Lanbuzz "full" at hp 54 %)
             : RestState::IsDrinking(bot) && !RestState::IsEating(bot) && bot->GetHealthPct() < 100.0f
                 ? "[death404] its drink done, health not eaten"
                 : "full",
             patrol ? patrol->GetName() : std::string(), bot->GetHealthPct(),
             bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 100.0f);
    bot->SetStandState(UNIT_STAND_STATE_STAND);
    return true;
}

bool StandUpAction::isUseful() { return RestState::RestDone(bot); }
