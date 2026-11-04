/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ReadyCheckAction.h"

#include "Event.h"
#include "Pet.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "LeaderOrders.h"  // [lead1084] Leader::LedBot
#include "RaidLeaderState.h"

namespace
{
    // The client answers with the state byte only; the server adds the guid when it relays the answer.
    void SendReadyConfirm(Player* bot, bool ready)
    {
        WorldPacket packet(MSG_RAID_READY_CHECK);
        packet << uint8(ready ? 1 : 0);
        bot->GetSession()->HandleRaidReadyCheckOpcode(packet);
    }

    std::string FormatPercent(std::string const& name, uint32 value, float percent)
    {
        std::ostringstream out;

        std::string color;
        if (percent > 75)
            color = "|cff00ff00";
        else if (percent > 50)
            color = "|cffffff00";
        else
            color = "|cffff0000";

        out << "|cffffffff[" << name << "]" << color << "x" << value;
        return out.str();
    }

    BotReadiness NotReady(std::string const& key, std::string const& defaultText)
    {
        return {false, PlayerbotTextMgr::instance().GetBotTextOrDefault(key, defaultText, {})};
    }
}

BotReadiness ReadyCheckAction::EvaluateReadiness(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    AiObjectContext* context = botAI->GetAiObjectContext();

    if (!bot->IsAlive())
        return NotReady("ready_reason_dead", "dead");

    bool drinking = false;
    if (RaidLeader::IsEatingOrDrinking(bot, drinking))
        return drinking ? NotReady("ready_reason_drinking", "drinking") : NotReady("ready_reason_eating", "eating");

    if (AI_VALUE2(uint8, "health", "self target") < sPlayerbotAIConfig.AlmostFullHealth)
        return NotReady("ready_reason_low_health", "low health");

    if (AI_VALUE2(bool, "has mana", "self target") &&
        AI_VALUE2(uint8, "mana", "self target") < sPlayerbotAIConfig.HighMana)
        return NotReady("ready_reason_low_mana", "low mana");

    Player* leader = botAI->GetGroupLeader();
    if (leader && leader != bot &&
        (leader->GetMap() != bot->GetMap() || bot->GetDistance(leader) > RaidLeader::READY_MAX_LEADER_DISTANCE))
        return NotReady("ready_reason_far", "far away");

    if (bot->getClass() == CLASS_HUNTER)
    {
        if (!bot->GetUInt32Value(PLAYER_AMMO_ID))
            return NotReady("ready_reason_no_ammo", "no ammo");

        Pet* pet = bot->GetPet();
        if (!pet || pet->GetHappinessState() == UNHAPPY)
            return NotReady("ready_reason_no_pet", "no pet");
    }

    // Only consumables the bot carries count: a bot without a flask is not asked to have one.
    if (!AI_VALUE(ObjectGuid, "raid flask").IsEmpty())
        return NotReady("ready_reason_no_flask", "no flask");

    if (!AI_VALUE(ObjectGuid, "raid food buff").IsEmpty())
        return NotReady("ready_reason_no_food_buff", "no food buff");

    return {};
}

void ReadyCheckAction::AnswerReadyCheck(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    BotReadiness const readiness = EvaluateReadiness(botAI);
    SendReadyConfirm(bot, readiness.ready);
    RaidLeader::RecordReadyAnswer(bot, readiness.ready, readiness.reason);
}

bool ReadyCheckAction::Execute(Event event)
{
    WorldPacket& p = event.getPacket();
    ObjectGuid player;
    p.rpos(0);
    if (!p.empty())
    {
        p >> player;
        if (player == bot->GetGUID())
            return false;

        // [ready_reply_now] The answer goes out at once and the rebuff pass runs after it (Begin(false): no reply
        // at its end). DOWNTIME_AUDIT 2026-10-25: the deferred reply ("ready reply [force rebuff pending]") held
        // boss pulls up to the leader's 150 s (mgt-n 153 s, pos-h 148 s with everyone at full health); a missing buff
        // never makes a member not ready (EvaluateReadiness), and the tank's planner waits for buffs by itself.
        constexpr bool READY_REPLY_ON_RECEIPT = true;
        // [lead1084] user 2026-11-04 (Q55): a member the dungeon leader leads was buffed in the leader's common pass
        // before this check; it answers at once and opens no second rebuff pass (it would hold the pull)
        if (Leader::LedBot(bot))
            return ReadyCheck();
        if (READY_REPLY_ON_RECEIPT && sPlayerbotAIConfig.ForceRebuffOnReadyCheck && !bot->IsInCombat() &&
            botAI->HasStrategy("force rebuff", BOT_STATE_NON_COMBAT))
        {
            bool const answered = ReadyCheck();
            botAI->forceRebuff.Begin(false);
            return answered;
        }

        // Defer the ready reply until buffs are topped off.
        if (sPlayerbotAIConfig.ForceRebuffOnReadyCheck && !bot->IsInCombat() &&
            botAI->HasStrategy("force rebuff", BOT_STATE_NON_COMBAT))
        {
            ReportReadinessToMaster();
            botAI->forceRebuff.Begin(true);
            return true;
        }
    }

    return ReadyCheck();
}

void ReadyCheckAction::ReportReadinessToMaster()
{
    // The automatic checks before each boss would flood the master with inventory reports.
    if (RaidLeader::IsAutomaticReadyCheckPending(bot))
        return;

    std::ostringstream out;

    uint32 hp = AI_VALUE2(uint32, "item count", "healing potion");
    out << FormatPercent("Hp", hp, 100.0f * hp / 5);

    out << ", ";
    uint32 food = AI_VALUE2(uint32, "item count", "food");
    out << FormatPercent("Food", food, 100.0f * food / 20);

    if (AI_VALUE2(bool, "has mana", "self target"))
    {
        out << ", ";
        uint32 mp = AI_VALUE2(uint32, "item count", "mana potion");
        out << FormatPercent("Mp", mp, 100.0f * mp / 5);

        out << ", ";
        uint32 water = AI_VALUE2(uint32, "item count", "water");
        out << FormatPercent("Water", water, 100.0f * water / 20);
    }

    botAI->TellMaster(out);
}

bool ReadyCheckAction::ReadyCheck()
{
    ReportReadinessToMaster();

    AnswerReadyCheck(botAI);

    botAI->forceRebuff.End();

    botAI->ChangeStrategy("-ready check", BOT_STATE_NON_COMBAT);

    return true;
}

bool FinishReadyCheckAction::Execute(Event /*event*/) { return ReadyCheck(); }

// Manual "rebuff" command: opens a rebuff window with no ready check to answer.
bool ForceRebuffAction::Execute(Event /*event*/)
{
    if (bot->IsInCombat() || !botAI->HasStrategy("force rebuff", BOT_STATE_NON_COMBAT))
        return false;

    botAI->forceRebuff.Begin(false);
    return true;
}

bool ReadyReplyAction::isUseful()
{
    ForceRebuffState const& rebuff = botAI->forceRebuff;
    return rebuff.IsPending() && !rebuff.IsOnGlobalCooldown() &&
           !bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) && !bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) &&
           !rebuff.IsBuffPendingThisCycle();
}

bool ReadyReplyAction::Execute(Event /*event*/)
{
    if (botAI->forceRebuff.ShouldReplyToReadyCheck())
        ReadyCheckAction::AnswerReadyCheck(botAI);

    botAI->forceRebuff.End();
    return true;
}
