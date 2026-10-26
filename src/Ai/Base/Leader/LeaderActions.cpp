/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LeaderActions.h"

#include "Event.h"
#include "Group.h"
#include "HealthTriggers.h"
#include "InterruptValues.h"
#include "LeaderValues.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "RaidLeaderState.h"
#include "ReadyCheckAction.h"
#include "SelfBotControl.h"
#include "Timer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

using namespace RaidLeader;

namespace
{
    constexpr size_t NOT_READY_LIST_MAX = 6;

    std::string JoinNotReady(std::vector<std::string> const& entries)
    {
        std::string text;
        for (size_t i = 0; i < entries.size() && i < NOT_READY_LIST_MAX; ++i)
            text += (i ? ", " : "") + entries[i];

        if (entries.size() > NOT_READY_LIST_MAX)
            text += " +" + std::to_string(entries.size() - NOT_READY_LIST_MAX);

        return text;
    }

    std::string WipeReasonText(std::string const& reason)
    {
        if (reason == "tanks dead, no battle res")
            return PlayerbotTextMgr::instance().GetBotTextOrDefault("leader_wipe_reason_tanks", reason, {});
        if (reason == "healers dead")
            return PlayerbotTextMgr::instance().GetBotTextOrDefault("leader_wipe_reason_healers", reason, {});
        if (reason == "most of the group dead")
            return PlayerbotTextMgr::instance().GetBotTextOrDefault("leader_wipe_reason_dead", reason, {});
        if (reason == "healers out of mana")
            return PlayerbotTextMgr::instance().GetBotTextOrDefault("leader_wipe_reason_oom", reason, {});
        if (reason == "boss reset")
            return PlayerbotTextMgr::instance().GetBotTextOrDefault("leader_wipe_reason_reset", reason, {});
        return reason;
    }
}

bool LeaderReadyCheckAction::Execute(Event /*event*/)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    ObjectGuid const boss = AI_VALUE(ObjectGuid, "leader nearby boss");
    uint32 const now = getMSTime();
    uint8 attempt = 0;
    bool recovering = false;
    Registry::Instance().With(group->GetGUID(),
                              [&](GroupState& state)
                              {
                                  if (boss && state.holdBoss != boss)
                                  {
                                      state.holdBoss = boss;
                                      state.holdSinceMs = now;
                                      state.attempts = 0;
                                  }
                                  else if (!state.holdSinceMs || state.phase == ReadyCheckPhase::Idle)
                                      state.holdSinceMs = now;

                                  state.phase = ReadyCheckPhase::Pending;
                                  state.checkStartMs = now;
                                  state.answers.clear();
                                  attempt = ++state.attempts;
                                  recovering = state.recovering;
                              });

    // Empty packet = a new ready check; only the leader or an assistant may start one, others are ignored and
    // their members are judged directly by the result action.
    WorldPacket request(MSG_RAID_READY_CHECK);
    bot->GetSession()->HandleRaidReadyCheckOpcode(request);

    // The leader does not receive its own check, so it starts its own rebuff pass.
    if (sPlayerbotAIConfig.ForceRebuffOnReadyCheck && botAI->HasStrategy("force rebuff", BOT_STATE_NON_COMBAT))
        botAI->forceRebuff.Begin(false);

    if (attempt == 1)
    {
        Unit* bossUnit = boss ? botAI->GetUnit(boss) : nullptr;
        if (recovering)
            Announce(botAI, "leader_ready_check_wipe", "Everyone is up. Rebuff and drink, ready check.");
        else if (bossUnit)
            Announce(botAI, "leader_ready_check_boss", "Ready check before %boss.", {{"%boss", bossUnit->GetName()}});
    }

    return true;
}

bool LeaderReadyCheckResultAction::Execute(Event /*event*/)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    if (bot->IsInCombat())
    {
        Registry::Instance().With(group->GetGUID(),
                                  [](GroupState& state)
                                  {
                                      state.phase = ReadyCheckPhase::Idle;
                                      state.answers.clear();
                                  });
        return false;
    }

    std::unordered_map<ObjectGuid, ReadyAnswer> const answers =
        Registry::Instance().With(group->GetGUID(), [](GroupState& state) { return state.answers; });

    std::vector<std::string> notReady;
    Map* map = bot->GetMap();
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || member->GetMap() != map)
            continue;

        PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
        if (!memberAI)
        {
            // A person answers in the game client; only a dead one is surely not ready.
            if (!member->IsAlive())
                notReady.push_back(member->GetName() + " (" +
                                   PlayerbotTextMgr::instance().GetBotTextOrDefault("ready_reason_dead", "dead", {}) +
                                   ")");
            continue;
        }

        BotReadiness readiness;
        auto const answer = answers.find(member->GetGUID());
        if (member != bot && answer != answers.end())
            readiness = {answer->second.ready, answer->second.reason};
        else if (member != bot && memberAI->forceRebuff.IsPending())
            readiness = {false,
                         PlayerbotTextMgr::instance().GetBotTextOrDefault("ready_reason_buffing", "buffing", {})};
        else
            readiness = ReadyCheckAction::EvaluateReadiness(memberAI);

        if (!readiness.ready)
            notReady.push_back(member->GetName() + " (" + readiness.reason + ")");
    }

    uint32 const now = getMSTime();
    uint8 attempts = 0;
    uint32 holdSinceMs = 0;
    Registry::Instance().With(group->GetGUID(),
                              [&](GroupState& state)
                              {
                                  attempts = state.attempts;
                                  holdSinceMs = state.holdSinceMs;
                              });

    // [ready_hold_30s] the group is ready by the bots' own lines: held PULL_HOLD_READY_MAX_MS, the pull goes on
    bool groupAtLines = !notReady.empty() && holdSinceMs && getMSTimeDiff(holdSinceMs, now) >= PULL_HOLD_READY_MAX_MS;
    for (GroupReference* ref = group->GetFirstMember(); groupAtLines && ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || member->GetMap() != map)
            continue;
        if (!member->IsAlive() || bot->GetDistance(member) > READY_MAX_LEADER_DISTANCE ||
            (GET_PLAYERBOT_AI(member) && (RestState::WantsDrink(member) || RestState::WantsFood(member))))
            groupAtLines = false;
    }
    if (groupAtLines)
        LOG_INFO("playerbots", "[ready_hold_30s] {}: held {} s, every member alive, here and above its rest lines: "
                 "pull (not ready: {})", bot->GetName(), getMSTimeDiff(holdSinceMs, now) / IN_MILLISECONDS,
                 JoinNotReady(notReady));

    std::string const seconds = std::to_string(PULL_DELAY_MS / IN_MILLISECONDS);
    if (notReady.empty() || attempts >= READY_CHECK_MAX_ATTEMPTS || groupAtLines)
    {
        Registry::Instance().With(group->GetGUID(),
                                  [now](GroupState& state)
                                  {
                                      state.phase = ReadyCheckPhase::Idle;
                                      state.attempts = 0;
                                      state.answers.clear();
                                      state.readyBoss = state.holdBoss;
                                      state.readyMs = now;
                                      state.recovering = false;
                                  });

        // A person leads: the bots wait for his pull (or his "pull" order) instead of counting down.
        if (IsLedByPerson(bot))
        {
            if (notReady.empty())
                Announce(botAI, "leader_ready_person", "Everyone is ready. Pull when you are.");
            else
                Announce(botAI, "leader_ready_person_partly", "Still not ready: %list. Pull when you want.",
                         {{"%list", JoinNotReady(notReady)}});
        }
        else if (notReady.empty())
            Announce(botAI, "leader_ready_pull", "Everyone is ready. Pull in %seconds.", {{"%seconds", seconds}});
        else
        {
            LOG_INFO("playerbots", "[leader] {}: ready_check_pull_anyway after {} attempts, not ready: {}",
                     bot->GetName(), uint32(attempts), JoinNotReady(notReady));
            Announce(botAI, "leader_ready_pull_anyway", "Still not ready: %list. Pulling anyway in %seconds.",
                     {{"%list", JoinNotReady(notReady)}, {"%seconds", seconds}});
        }

        return true;
    }

    Registry::Instance().With(group->GetGUID(),
                              [now](GroupState& state)
                              {
                                  state.phase = ReadyCheckPhase::RetryWait;
                                  state.retryAtMs = now + READY_CHECK_RETRY_MS;
                              });

    Announce(botAI, "leader_not_ready", "Not ready: %list. Next check in %seconds seconds.",
             {{"%list", JoinNotReady(notReady)}, {"%seconds", std::to_string(READY_CHECK_RETRY_MS / IN_MILLISECONDS)}});
    return true;
}

bool LeaderEncounterEventAction::Execute(Event /*event*/)
{
    Group* group = bot->GetGroup();
    LeaderEvent const event = AI_VALUE(LeaderEvent, "leader encounter event");
    context->GetValue<LeaderEvent>("leader encounter event")->Reset();
    if (!group || event.type == LeaderEventType::None)
        return false;

    Unit* boss = event.boss ? botAI->GetUnit(event.boss) : nullptr;
    std::string const bossName = boss ? boss->GetName() : "";
    ObjectGuid const groupGuid = group->GetGUID();

    switch (event.type)
    {
        case LeaderEventType::Engage:
        {
            NoteEngage(bot, boss);
            Registry::Instance().With(groupGuid, [](GroupState& state) { state.pullAnnounced = true; });
            uint32 const holdSecs = sPlayerbotAIConfig.RaidLeaderPullHoldSecs;
            if (holdSecs)
                Announce(botAI, "leader_pull_hold", "%boss pulled! DPS and healers hold attacks for %seconds seconds.",
                         {{"%boss", bossName}, {"%seconds", std::to_string(holdSecs)}});
            else
                Announce(botAI, "leader_pull", "%boss pulled!", {{"%boss", bossName}});
            break;
        }
        case LeaderEventType::BossDown:
            Announce(botAI, "leader_boss_down", "%boss is down. Well done!", {{"%boss", bossName}});
            ClearEngage(bot);
            Registry::Instance().With(groupGuid,
                                      [](GroupState& state)
                                      {
                                          state.recovering = false;
                                          state.holdBoss.Clear();
                                      });
            break;
        case LeaderEventType::Reset:
            ClearEngage(bot);
            break;
        case LeaderEventType::Wipe:
            // The fight is already over here (the boss reset with the group dead): bookkeeping and recovery only.
            if (CallWipe(bot, boss, event.reason))
                Announce(botAI, "leader_wipe_over", "Wipe: %reason. Release or wait for a resurrection.",
                         {{"%reason", WipeReasonText(event.reason)}});
            if (!boss || !boss->IsInCombat())
                ClearEngage(bot);
            break;
        case LeaderEventType::LostFight:
        {
            // Nobody stops: everyone fights to the end, only the members who can leave the fight do, to resurrect
            // the group on the spot. A wipe call in the fight (CallWipe) stays off; the fight's end records it.
            if (!OrderWipeEscape(bot))
                break;

            FightOutlook const outlook = AI_VALUE(FightOutlook, "leader fight outlook");
            LOG_INFO("playerbots", "[wipe] {}: fight lost, {} {}", bot->GetName(), event.reason,
                     FormatOutlook(outlook));
            Announce(botAI, "leader_wipe_escape",
                     "This fight is lost (%reason). Fight to the end; whoever can get out, get out and resurrect us.",
                     {{"%reason", WipeReasonText(event.reason)}});
            break;
        }
        case LeaderEventType::HpMark:
        {
            uint8 const mark = event.mark;
            Registry::Instance().With(groupGuid, [mark](GroupState& state) { state.hpMarks |= mark; });
            std::string const percent = std::to_string(boss ? uint32(boss->GetHealthPct()) : 0);
            if (boss && boss->GetHealthPct() <= LAST_PHASE_HP_PCT)
                Announce(botAI, "leader_last_phase", "Last phase: %boss at %percent%, use everything!",
                         {{"%boss", bossName}, {"%percent", percent}});
            else
                Announce(botAI, "leader_boss_half", "%boss at %percent%.",
                         {{"%boss", bossName}, {"%percent", percent}});
            break;
        }
        case LeaderEventType::Bloodlust:
            Registry::Instance().With(groupGuid, [](GroupState& state) { state.lustAnnounced = true; });
            Announce(botAI, "leader_bloodlust", "Bloodlust is up, burn now!");
            break;
        case LeaderEventType::BattleRes:
        {
            ObjectGuid const target = event.target;
            Registry::Instance().With(groupGuid,
                                      [target](GroupState& state) { state.battleResAnnounced.insert(target); });
            if (Player* player = ObjectAccessor::FindPlayer(target))
                Announce(botAI, "leader_battle_res", "Battle res on %name.", {{"%name", player->GetName()}});
            break;
        }
        default:
            return false;
    }

    return true;
}

bool WipeEscapeAction::isUseful() { return !AI_VALUE(std::string, "wipe escape spell").empty(); }

bool WipeEscapeAction::Execute(Event /*event*/)
{
    std::string const spell = AI_VALUE(std::string, "wipe escape spell");
    if (spell.empty())
        return false;

    Player* saved = spell == "divine intervention" ? FindDivineInterventionTarget(botAI, bot) : nullptr;
    if (spell == "divine intervention" && !saved)
        return false;

    // Feign Death and Shadowmeld break on movement, every escape on an attack.
    if (bot->IsNonMeleeSpellCast(false))
        bot->InterruptNonMeleeSpells(false);
    bot->AttackStop();
    bot->GetMotionMaster()->Clear();
    bot->StopMoving();

    if (!botAI->CastSpell(spell, saved ? static_cast<Unit*>(saved) : bot))
        return false;

    context->GetValue<uint32>("wipe escape at")->Set(std::max(getMSTime(), 1u));
    context->GetValue<std::string>("wipe escape spell")->Reset();
    if (saved)
        LOG_INFO("playerbots", "[wipe] {}: survives by divine intervention from {}", saved->GetName(),
                 bot->GetName());
    else
        LOG_INFO("playerbots", "[wipe] {}: survives by {}", bot->GetName(), spell);

    return true;
}

bool WipeEscapeEndAction::Execute(Event /*event*/)
{
    bool const fightOver = !bot->IsInCombat();
    context->GetValue<uint32>("wipe escape at")->Set(0);
    bot->RemoveAurasDueToSpell(SPELL_DIVINE_INTERVENTION_AURA);  // cancelled as a player right-clicks it
    bot->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);
    LOG_INFO("playerbots", "[wipe] {}: escape over ({})", bot->GetName(),
             fightOver ? "the fight is gone" : "found again, fighting on");
    return true;
}

bool LeaderAssignTanksAction::Execute(Event /*event*/)
{
    Group* group = bot->GetGroup();
    TankAssignment const assignment = AI_VALUE(TankAssignment, "leader tank assignment");
    if (!group || !assignment.changed || !assignment.mainTank)
        return false;

    uint32 const now = getMSTime();
    Registry::Instance().With(group->GetGUID(), [now](GroupState& state) { state.tankFlagsQueuedMs = now; });
    QueueTankFlags(bot, assignment.mainTank, assignment.mainAssist);
    context->GetValue<TankAssignment>("leader tank assignment")->Reset();

    if (assignment.mainTank == _announcedTank && assignment.mainAssist == _announcedAssist)
        return true;

    _announcedTank = assignment.mainTank;
    _announcedAssist = assignment.mainAssist;
    Player* mainTank = ObjectAccessor::FindPlayer(assignment.mainTank);
    Player* mainAssist = assignment.mainAssist ? ObjectAccessor::FindPlayer(assignment.mainAssist) : nullptr;
    if (!mainTank)
        return true;

    if (mainAssist)
        Announce(botAI, "leader_tanks", "%tank is main tank, %assist is off-tank.",
                 {{"%tank", mainTank->GetName()}, {"%assist", mainAssist->GetName()}});
    else
        Announce(botAI, "leader_main_tank", "%tank is main tank.", {{"%tank", mainTank->GetName()}});

    return true;
}

namespace
{
    constexpr uint32 BURST_DEFAULT_SECS = 20;
    constexpr uint32 BURST_MIN_SECS = 5;
    constexpr uint32 BURST_MAX_SECS = 60;

    bool AnyMemberInCombat(Group* group)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member->IsInWorld() && member->IsInCombat())
                    return true;

        return false;
    }

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
        return text;
    }
}

bool GroupPauseAction::Execute(Event event)
{
    Group* group = bot->GetGroup();
    Player* by = event.getOwner() ? event.getOwner() : GetMaster();
    if (!group)
        return false;

    // Every bot of the group hears the order; the first one sets the pause and answers.
    if (!SetPaused(bot, true, by))
        return true;

    if (AnyMemberInCombat(group))
        Announce(botAI, "group_pause_fight", "Pause: we finish this fight, then hold here. No pulls.");
    else
        Announce(botAI, "group_pause", "Pause: holding here, no pulls until you are back.");

    SelfBotControl::SendAddonStatus(by);
    return true;
}

bool GroupResumeAction::Execute(Event event)
{
    Player* by = event.getOwner() ? event.getOwner() : GetMaster();
    if (!bot->GetGroup() || !SetPaused(bot, false, by))
        return true;

    Announce(botAI, "group_resume", "Break is over, carrying on.");
    SelfBotControl::SendAddonStatus(by);
    return true;
}

bool RestChatAction::Execute(Event /*event*/)
{
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsMounted() || bot->IsInFlight())
        return false;

    // Real food and water from the bags, as a person sits down to eat and drink.
    bool const needFood = bot->GetHealthPct() < 100.0f && !bot->HasAuraType(SPELL_AURA_MOD_REGEN);
    bool const needDrink = bot->GetMaxPower(POWER_MANA) > 0 && bot->GetPowerPct(POWER_MANA) < 100.0f &&
                           !bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN);
    if (!needFood && !needDrink)
        return false;

    bool resting = false;
    if (needFood)
        resting = UseItemAction::Execute(Event("rest", "food")) || resting;

    if (needDrink)
        resting = UseItemAction::Execute(Event("rest", "drink")) || resting;

    if (resting)
        botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault("resting", "Resting", {}));

    return resting;
}

bool InterruptOrderAction::Execute(Event event)
{
    Player* by = event.getOwner() ? event.getOwner() : GetMaster();
    Unit* caster = by ? botAI->GetUnit(by->GetGuidValue(UNIT_FIELD_TARGET)) : nullptr;
    if (!caster || !caster->IsAlive() || !bot->IsHostileTo(caster))
        return false;

    ObjectGuid interrupter;
    std::string spell;
    if (!InterruptCoordination::PickOrderedInterrupter(bot, caster, interrupter, spell) ||
        interrupter != bot->GetGUID())
        return false;

    if (bot->IsNonMeleeSpellCast(false))
        bot->InterruptNonMeleeSpells(false);

    if (!botAI->CastSpell(spell, caster))
        return false;

    InterruptCoordination::MarkFired(botAI, caster);
    botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "interrupt_order", "Interrupting %target", {{"%target", caster->GetName()}}));
    return true;
}

bool ResurrectFirstAction::Execute(Event event)
{
    Group* group = bot->GetGroup();
    Player* by = event.getOwner() ? event.getOwner() : GetMaster();
    if (!group)
        return false;

    std::string const name = Lower(event.getParam());
    Player* target = nullptr;
    if (name.empty() || name == "me")
        target = by;
    else
    {
        for (GroupReference* ref = group->GetFirstMember(); ref && !target; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (Lower(member->GetName()) == name)
                    target = member;
    }

    if (!target || !group->IsMember(target->GetGUID()) || target->IsAlive())
        return false;

    if (GetResurrectFirst(bot) == target->GetGUID())
        return true;  // another member already took the order

    SetResurrectFirst(bot, target->GetGUID());
    Announce(botAI, "resurrect_first", "%name is resurrected first.", {{"%name", target->GetName()}});
    return true;
}

bool BurstOrderAction::Execute(Event event)
{
    auto* burst = dynamic_cast<BurstUntilValue*>(context->GetValue<uint32>("burst until"));
    if (!burst || !bot->IsAlive())
        return false;

    std::string const param = event.getParam();
    uint32 seconds = param.empty() ? BURST_DEFAULT_SECS : uint32(std::max(0, atoi(param.c_str())));
    seconds = std::clamp(seconds, BURST_MIN_SECS, BURST_MAX_SECS);

    // The boost triggers (cooldowns, Bloodlust/Heroism of a shaman) treat the window as a burn phase.
    burst->Set(std::max(getMSTime() + seconds * IN_MILLISECONDS, 1u));
    if (!botAI->HasStrategy("boost", BOT_STATE_COMBAT))
    {
        botAI->ChangeStrategy("+boost", BOT_STATE_COMBAT);
        burst->addedBoost = true;
    }

    botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "burst_order", "Burst for %seconds s", {{"%seconds", std::to_string(seconds)}}));
    return true;
}

bool BurstEndAction::Execute(Event /*event*/)
{
    auto* burst = dynamic_cast<BurstUntilValue*>(context->GetValue<uint32>("burst until"));
    if (!burst)
        return false;

    burst->Set(0);
    if (burst->addedBoost && botAI->HasStrategy("boost", BOT_STATE_COMBAT))
        botAI->ChangeStrategy("-boost", BOT_STATE_COMBAT);

    burst->addedBoost = false;
    return true;
}
