/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LeaderActions.h"
#include <cmath>
#include "BotPath.h"  // [lead109]

#include "Event.h"
#include "Group.h"
#include "HealthTriggers.h"
#include "InterruptValues.h"
#include "LastMovementValue.h"  // [lead36]
#include "LeaderFightChecks.h"  // [lead491] the leader's melee idle-reach test
#include "LeaderValues.h"
#include "LeaderOrders.h"  // [lead15]
#include "PartyMemberToHeal.h"  // [lead940] [lead946] HealTriage
#include "PathGenerator.h"  // [lead19]
#include "TerrainHazard.h"  // [lead19] closed doors
#include "Log.h"
#include "MotionMaster.h"
#include "MoveSpline.h"  // [lead762]
#include "ObjectAccessor.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "RaidLeaderState.h"
#include "ReadyCheckAction.h"
#include "SelfBotControl.h"
#include "SpellInfo.h"  // [lead396]
#include "SpellMgr.h"   // [lead396]
#include "Timer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <tuple>  // [nav190]

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

    // [lead372] a MoveTo "refusal" that refuses no X: the same walk is on, or the game holds the bot (a root, a
    // stun, a cast); the walk goes once that ends
    bool TransientRefusal(std::string const& guard)
    {
        return guard.empty() || guard.rfind("IsDuplicateMove", 0) == 0 || guard.rfind("IsWaitingForLastMove", 0) == 0 ||
               guard.rfind("IsMovingAllowed", 0) == 0 || guard.rfind("the walk ends where", 0) == 0;
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
    // [lead1084] user 2026-11-04 (Q12 / Q55): in a dungeon group the leader leads, the buff pass before the boss is
    // the one common pass (the tank's own buffs included: its step 1 opened every member's window); the ready check
    // and the pull come after it, so no second rebuff pass of the tank's here (b233: the pull waited ~14 s on it)
    if (sPlayerbotAIConfig.ForceRebuffOnReadyCheck && botAI->HasStrategy("force rebuff", BOT_STATE_NON_COMBAT) &&
        !Leader::LedBot(bot) && !Leader::Orders::IsTracked(bot->GetGUID()))
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

// [lead15] the leader's member events
bool LeaderMoveOrderAction::Execute(Event /*event*/)
{
    Leader::MemberEvents events;
    if (!Leader::Events::Get(bot->GetGUID(), events))
        return false;
    // [lead281] b193 / b195 rfc-h (16 runs): damage dealers stood out of combat 1500 s of fights with an Attack held -
    // "leader attack order" (23) not evaluated, the walk to the combat spot / along with the tank (94) ran first every
    // tick. An Attack the member has not started (its victim is not the target yet) on a target it sees goes first,
    // once per command: the walk of combat-spot rank or lower goes on the next tick (the attack sets no walk)
    if (events.seq != _attackFirstSeq && Leader::ExecutorMember(bot) && events.attack == Leader::AttackEvent::Attack &&
        events.intensity != Leader::Intensity::Stop && Leader::Events::MovePrecedence(events.moveWhy) <= 2)
        if (Unit* attackTarget = botAI->GetUnit(events.target);
            attackTarget && attackTarget->IsAlive() && attackTarget->IsInCombat() && bot->GetVictim() != attackTarget &&
            bot->IsValidAttackTarget(attackTarget) && bot->IsWithinLOSInMap(attackTarget))
        {
            _attackFirstSeq = events.seq;
            return false;
        }
    // [lead27] Follow: the place `followIndex` behind the target (two rows of two), walked with the member's own path
    // guards (the leader issued no point)
    if (events.move == Leader::MoveEvent::Follow)
    {
        Unit* target = botAI->GetUnit(events.followTarget);
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        // [nav191] a Follow with no place to walk to is the dead end DryFollow already names ("no melee place in
        // reach and sight of the target", [lead580]): the trigger stays active (the target out of reach or out of
        // sight) and this returned false every tick, so the member stood for the rest of the run and the leader
        // never heard. It is the member's refusal of a walk it cannot take: it stays and reports, the leader decides
        // again ([lead372])
        if (!FollowPlace(target, events.followIndex, x, y, z))
        {
            ReportRefusal(events, bot->GetPosition(),
                          bot->IsValidAttackTarget(target) ? "no melee place in reach and sight of the target"
                                                           : "no follow place behind the target",
                          ObjectGuid::Empty);
            return false;
        }
        float const tolerance = std::max(events.tolerance, Leader::LEADER_SPOT_TOLERANCE);
        LastMovement const& last = *context->GetValue<LastMovement&>("last movement");
        // [lead249] b191 rfc-h (8 runs): each melee damage dealer walked 37-65 s of its fights within 5 yd of the
        // tank's target and already behind it (out of its front arc, where the game parries / blocks) - the walk to
        // the chase's place, started when the mob turned, carried on to that place. In reach behind the mob that walk
        // has nothing left to reach: it stops (a walk to elsewhere, a hazard's, is not this one)
        // [lead453] b205 / b206 rfc-h: still 47-49 s a run of the rogue's / cat's fights walking under the chase while
        // already behind the mob and in its reach, the tank standing: a standing mob's follow place is the same
        // point every tick, so the duplicate-move check below returned first and this stop never ran, and the walk
        // ended at the follow place (two rows behind the mob) beyond its reach plus the tolerance. Checked first now;
        // a mob that stands leaves nothing for that walk to reach: in reach behind it the member stops
        // [lead580] ... and in its sight (ChaseDoneHere): a stop where the game sees no target is no place
        if (bot->isMoving() && bot->IsValidAttackTarget(target) && ChaseDoneHere(target) &&
            (!target->isMoving() || target->GetExactDist2d(last.lastMoveToX, last.lastMoveToY) <=
                                        target->GetCombatReach() + bot->GetCombatReach() + tolerance))
        {
            bot->StopMoving();
            return false;
        }
        // [lead44] b93 rfc-h 313-362 s: Rtshaab's Follow fired "leader move order -> 0.0 yd" every tick at 94 and its
        // "ready reply" never ran - the leader waited 50 s for its rebuff. A walk already under way (or done) is no
        // live work: not executed, the member's other actions run
        // [lead523] b211 rfc-h 5A 655-705 s: the enhancement shaman stood 6.4 yd from Taragaman under "chase: behind
        // the target", out of the game's melee range (both combat reaches + 4/3), Execute false every tick: its
        // place (Taragaman's reach behind him) was 2.6 yd off, inside the follow's 3 yd tolerance, so "standing at
        // its place" ended the walk. A chase is done only in the mob's melee range (the game's IsWithinMeleeRange,
        // the reach-behind stop below); out of it, stopped, the walk to the place is walked again
        // [lead580] ... and in its sight: a member stopped in reach without the game's line of sight of the mob
        // (b215 5E: the rogue on Oggleflint's centre, 45 s "no line of sight") walks to its place, which is in sight
        bool const chase = bot->IsValidAttackTarget(target);
        bool const chaseShort =
            chase && !bot->isMoving() && (!bot->IsWithinMeleeRange(target) || !bot->IsWithinLOSInMap(target));
        if (!chaseShort && IsDuplicateMove(x, y, z))
            return false;
        // [lead154] b173 281-366 s: Rtshaab stood at its follow place, the tank still; the walk there was refused and
        // re-issued as a MovePoint every tick (executed, nothing moved) - the rebuff window never closed. Standing at
        // its place (the follow's own tolerance) is no work
        if (!chase && !target->isMoving() &&
            bot->GetExactDist2d(x, y) <= std::max(events.tolerance, Leader::LEADER_SPOT_TOLERANCE))
        {
            // [nav190] the trigger walks a member out of its target's sight; this reads it as standing at its place.
            // Both at once is a member that stands for good: the line names which of the two is wrong
            if (!bot->IsWithinLOSInMap(target))
                NoteDeadEnd(events, "at its follow place but out of sight of the target",
                            Acore::StringFormat("{} is {:.1f} yd off, its place ({:.1f},{:.1f},{:.1f}) {:.1f} yd",
                                                target->GetName(), bot->GetExactDist(target), x, y, z,
                                                bot->GetExactDist2d(x, y)));
            return false;
        }
        // [lead159] b176 20261101-163239: "leader move order" executed in 20 % of the shaman's fight samples and
        // pre-empted Stormstrike 60x, Earth Shock 54x, Lava Lash 40x - the chase (lead156) re-walked every tick. A walk
        // already heading for the place (within the follow's tolerance), or a chased mob already in melee reach, is no
        // work: the core movement carries it, the rotation runs
        // [lead281] the walk under way is known by its end as asked too (a guard may have moved the end it walks to)
        if (bot->isMoving() &&
            (std::hypot(last.lastMoveToX - x, last.lastMoveToY - y) <= tolerance ||
             std::hypot(last.lastMoveAsked.GetPositionX() - x, last.lastMoveAsked.GetPositionY() - y) <= tolerance))
            return false;
        // [lead418] b201 rfc-h 5H: the cat druid stood in Oggleflint's front arc in 91 of 91 trace states (Bazzalan
        // 102 / 103) - Shred impossible: a chased mob in melee reach was no work wherever the member stood. The
        // leader's place is behind the mob: in reach out of its front arc (the core's HasInArc, where it parries /
        // blocks) is done, in reach in its front arc the walk behind goes on (the mob's victim stands where it is)
        if (bot->IsValidAttackTarget(target) && ChaseDoneHere(target))
            return false;
        Position const place(x, y, z);
        // [lead370] b200 5G 632-862 s: DryFollow passed Rtdekad's walk, the executor's own GuardIdleMobPath refused
        // it a moment later (its inputs had moved: the member, the tank's orientation, the cached idle mobs) and the
        // leader issued it again - 56 times. user: the leader's check is the check: the walk the leader judged clear
        // from where the member stood to that place (both within the follow's spot tolerance) is walked as judged,
        // its idle guard not run a second time; any other walk is the member's own and keeps its guards
        _judgedWalk = _judgedFollow.valid && target->GetGUID() == _judgedFollow.target &&
                      bot->GetExactDist(_judgedFollow.from) <= Leader::LEADER_SPOT_TOLERANCE &&
                      place.GetExactDist(_judgedFollow.place) <= Leader::LEADER_SPOT_TOLERANCE;
        bool const walks =
            MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_NORMAL, true);
        _judgedWalk = false;
        if (walks)
        {
            // [lead372] user: the member walks to the leader's place safely and never swaps it for another: a guard
            // that ended the walk elsewhere (a safe-zone clamp, a stop short of an idle mob's reach) is no way there -
            // the member stays and reports, the leader decides again (a walk round a problem that still ends at the
            // place, a trail leg, is the walk's own how)
            if (std::string const substitute = LastMoveSubstitute(); !substitute.empty())
            {
                ObjectGuid const mob = LastMoveMob();
                bot->StopMoving();
                bot->GetMotionMaster()->Clear();
                ReportRefusal(events, place, substitute, mob);
                return false;
            }
            return true;
        }
        // [lead40] b89 rfc-h 57.8 s: Rtshaab "member_refusal: the member's guard refused the follow walk (ledge /
        // ground effect)" and walked it anyway. [lead372] user: no safe way to the place is the leader's to decide -
        // the member stays and reports; only a refusal of no place (the walk under way, the game holding the bot)
        // walks the plain path. Honest play: never through a closed door
        if (std::string const guard = LastMoveRefusal(); !TransientRefusal(guard))
        {
            ReportRefusal(events, place, guard, LastMoveMob());
            return false;
        }
        std::string const refusal = WalkRefusal(place);
        if (refusal.rfind("closed door", 0) == 0)
        {
            ReportRefusal(events, place, refusal, ObjectGuid::Empty);
            return false;
        }
        if (!WalkBotRoute(bot, x, y, z))  // [lead109] the leader's route (BotPath.h)
            bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, true);
        return true;
    }
    // [lead81] the leader's stand / hold: an old walk that leads off the spot stops at once
    if (Leader::WalksOffStand(bot, events))
    {
        bot->StopMoving();
        bot->GetMotionMaster()->Clear();
        Leader::Events::Ack(bot->GetGUID(), events.seq, events.move == Leader::MoveEvent::Hold ? "hold" : "stand",
                            "stops its walk: " + events.moveWhy);
        return true;
    }
    if (events.move != Leader::MoveEvent::MoveTo && events.move != Leader::MoveEvent::StandAt)
    {
        // [nav190] the trigger fired (WalksOffStand) and the kind is one this action does not walk: a mismatch that
        // ends every tick standing
        NoteDeadEnd(events, "the command's move kind is not one this action walks", "nothing to execute");
        return false;
    }
    if (!std::isfinite(events.spot.GetPositionX()) || !std::isfinite(events.spot.GetPositionY()) ||
        !std::isfinite(events.spot.GetPositionZ()))  // [lead161]
    {
        NoteDeadEnd(events, "the commanded spot is no number", "[lead161]");
        return false;
    }
    if (!bot->IsStandState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);
    // [lead1070] a MoveTo whose purpose is a unit in reach (the tank's pick-up: its taunt's range, in sight) is reached
    // where that holds: the walk under way stops, the tick is the rotation's (the taunt from where it stands)
    if (events.move == Leader::MoveEvent::MoveTo && !events.moveReachOf.IsEmpty() && events.moveReachRange > 0.0f)
        if (Unit* reachOf = botAI->GetUnit(events.moveReachOf);
            reachOf && reachOf->IsAlive() && bot->GetExactDist(reachOf) <= events.moveReachRange &&
            bot->IsWithinLOSInMap(reachOf))
        {
            // only the walk to this order's spot (lead762's test): the tank's own fight moves (reach melee, a charge)
            // are its own
            if (bot->isMoving() && !bot->movespline->Finalized())
            {
                G3D::Vector3 const end = bot->movespline->FinalDestination();
                if (events.spot.GetExactDist2d(end.x, end.y) <= Leader::LEADER_SPOT_TOLERANCE)
                {
                    bot->StopMoving();
                    bot->GetMotionMaster()->Clear();
                }
            }
            Leader::Events::Ack(bot->GetGUID(), events.seq, "move", "[lead1070] in reach of its purpose's unit");
            thread_local std::unordered_map<ObjectGuid, uint32> reachLogged;
            if (uint32& seen = reachLogged[bot->GetGUID()]; seen != events.seq)
            {
                seen = events.seq;
                LOG_INFO("playerbots",
                         "[lead1070] {}: {} reached: {} {:.1f} yd off (reach {:.1f}), {:.1f} yd short of the spot",
                         bot->GetName(), events.moveWhy, reachOf->GetName(), bot->GetExactDist(reachOf),
                         events.moveReachRange,
                         bot->GetExactDist2d(events.spot.GetPositionX(), events.spot.GetPositionY()));
            }
            return false;
        }
    if (bot->GetExactDist2d(events.spot.GetPositionX(), events.spot.GetPositionY()) <= events.tolerance)
    {
        // [lead762] b220 rfc-h (8 runs): "aggro loss: the tank goes to the member" / "... to the mob" walked the tank
        // ~650 yd after its first hits - its tolerance is the taunt's reach (the leader's arrival: in reach and in
        // sight), but the walk went on to the member's own spot (5A 85.8 s: 44.3 yd off, 36 yd walked in 8 s where 14
        // reached the taunt). A MoveTo arrives at its tolerance: the walk to the spot stops there once the spot is in
        // sight (the leader's tolerance for a spot in reach out of sight is the melee one, lead572)
        if (events.move == Leader::MoveEvent::MoveTo && bot->isMoving() && !bot->movespline->Finalized())
        {
            G3D::Vector3 const end = bot->movespline->FinalDestination();
            if (events.spot.GetExactDist2d(end.x, end.y) <= Leader::LEADER_SPOT_TOLERANCE &&
                bot->IsWithinLOS(events.spot.GetPositionX(), events.spot.GetPositionY(),
                                 events.spot.GetPositionZ() + bot->GetCollisionHeight()))
            {
                bot->StopMoving();
                bot->GetMotionMaster()->Clear();
            }
        }
        // [lead1087] user 2026-11-04 (Q11: "how can anyone lose its target?"): a ranged member ran ~2 yd past its
        // stop (its run speed over a tick), lost its target's sight there and got a new spot. On the spot with a walk
        // under way that ends off it (an older walk's spline, a walk on past it), the walk stops here: the member
        // stands on the spot it was told
        if (bot->isMoving() && !bot->movespline->Finalized())
        {
            G3D::Vector3 const end = bot->movespline->FinalDestination();
            if (events.spot.GetExactDist2d(end.x, end.y) > events.tolerance)
            {
                bot->StopMoving();
                bot->GetMotionMaster()->Clear();
                LOG_INFO("playerbots", "[lead1087] {}: stops on its spot ({}): its walk ran on {:.1f} yd past it",
                         bot->GetName(), events.moveWhy, events.spot.GetExactDist2d(end.x, end.y));
            }
        }
        Leader::Events::Ack(bot->GetGUID(), events.seq, events.move == Leader::MoveEvent::MoveTo ? "move" : "stand",
                            "on the spot");
        // [lead159] standing on the spot is no work: the tick is the rotation's (it was executed every tick)
        return false;
    }
    // [lead162] a cast / channel under way that reaches its target (in range and sight) is not broken for a spot move
    // of combat-spot rank or lower (Mind Flay, Arcane Missiles, Drain Soul were cut by re-spots); a hazard, an aggro
    // loss or the heal reach moves it at once
    // [lead523] b211 rfc-h 5E: the BM hunter's approved trap weaves never took a step (13 weaves, 0 yd walked, 0
    // Explosive Traps): Auto Shot is an autorepeat spell, IsNonMeleeSpellCast(false) read it as a cast under way every
    // tick. The autorepeat shot is no cast this check keeps (the walk pauses it, the core resumes it)
    if (bot->IsNonMeleeSpellCast(false, false, true) && Leader::Events::MovePrecedence(events.moveWhy) <= 2)
        if (Unit* victim = AI_VALUE(Unit*, "current target");
            victim && victim->IsAlive() && bot->GetExactDist(victim) <= botAI->GetRange("spell") &&
            bot->IsWithinLOSInMap(victim))
        {
            // [nav190] a cast that never ends holds the member on every tick of the command
            NoteDeadEnd(events, "a cast of its own runs", victim->GetName());
            return false;
        }
    // [lead1044] b230 rfc-h 5B-10 1028-1036 s: Rtpriaa's combat spot (-321.2, 240.3, -5.6) stood 4.6 yd above the
    // ground its walk ended on, 2.7 yd off (tolerance 2); standing at that end, the walk was asked again every tick
    // (a 0 yd spline), executed, and its spells were "not evaluated: leader move order ran first" (about 25 s of
    // idle a batch). A walk to this spot that already ended where the member stands has nothing left to walk: the
    // spot as the ground lets it be reached is reached, the tick is the rotation's
    // [lead1062] b231: lead1044 never fired - LastMovement keeps the spot's own x / y as the walk's end (MoveTo's Set
    // with the floor search's z only), so "standing at the walk's end" was "standing at the spot" (the check above).
    // The end the ground gives is the leader route's own end (FindBotPath: a complete route, PATHFIND_NORMAL, ends on
    // the spot's navmesh point - SearchForBestPath walks exactly that). Only here, standing after a walk to this very
    // spot, where the re-asked walk would search the same route anyway. Its ack shares the walk's (seq, kind) and is
    // dropped as a repeat: the log line names it once per order
    if (!bot->isMoving())
    {
        LastMovement const& last = *context->GetValue<LastMovement&>("last movement");
        if (last.lastMoveAsked.GetExactDist(events.spot.GetPositionX(), events.spot.GetPositionY(),
                                            events.spot.GetPositionZ()) <= 0.01f)
        {
            BotPath const route =
                FindBotPath(bot, events.spot.GetPositionX(), events.spot.GetPositionY(), events.spot.GetPositionZ());
            if (route.leaderRoute && (route.type & PATHFIND_NORMAL) && !route.points.empty() &&
                bot->GetExactDist2d(route.points.back().x, route.points.back().y) <= events.tolerance)
            {
                Leader::Events::Ack(bot->GetGUID(), events.seq,
                                    events.move == Leader::MoveEvent::MoveTo ? "move" : "stand",
                                    "[lead1044] on the spot: its walk's end on the ground");
                thread_local std::unordered_map<ObjectGuid, uint32> loggedSeq;
                if (uint32& seen = loggedSeq[bot->GetGUID()]; seen != events.seq)
                {
                    seen = events.seq;
                    LOG_INFO("playerbots",
                             "[lead1044] {}: on its spot ({}): the route's end on the ground, {:.1f} yd off the spot "
                             "(tolerance {:.1f})",
                             bot->GetName(), events.moveWhy,
                             bot->GetExactDist2d(events.spot.GetPositionX(), events.spot.GetPositionY()),
                             events.tolerance);
                }
                return false;
            }
        }
    }
    // [lead27] already walking there: the same move again is no refusal
    // [lead44] ... and no execution either: the member's other actions run meanwhile (no starvation)
    if (IsDuplicateMove(events.spot.GetPositionX(), events.spot.GetPositionY(), events.spot.GetPositionZ()))
        return false;
    // [lead36] b85 rfc-h: 45+ "member_refusal: combat spot - another guard": the leader's newer spot met the wait for
    // the last move (IsWaitingForLastMove, the walk to the old spot) - the new command replaces that walk at once
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
    {
        LastMovement& last = *context->GetValue<LastMovement&>("last movement");
        last.lastdelayTime = 0.0f;
        last.msTime = 0;
    }
    if (!IsMovingAllowed())
    {
        Leader::Events::Ack(bot->GetGUID(), events.seq, events.move == Leader::MoveEvent::MoveTo ? "move" : "stand",
                            "can not move now (lost control / rooted): " + events.moveWhy);
        // [nav190] the ack is sent once per command, so a member the game never gives back stood silently: name the
        // state that holds it (a controlled motion slot a jump or a fear never gave up is the one that lasts)
        NoteDeadEnd(events, "the game does not let it move",
                    Acore::StringFormat(
                        "lost control {}, rooted {}, charmed {}, frozen {}, polymorphed {}, controlled motion slot "
                        "{}, in flight {}, being teleported {}",
                        bot->HasUnitState(UNIT_STATE_LOST_CONTROL), bot->IsRooted(), bot->IsCharmed(),
                        bot->isFrozen(), bot->IsPolymorphed(),
                        uint32(bot->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED)), bot->IsInFlight(),
                        bot->IsBeingTeleported()));
        return false;
    }
    bool const moved = MoveTo(bot->GetMapId(), events.spot.GetPositionX(), events.spot.GetPositionY(),
                              events.spot.GetPositionZ(), false, false, false, false,
                              MovementPriority::MOVEMENT_COMBAT, true);
    // [lead372] a guard that ended the walk short of the spot (a closed door on its way) is no way there: the
    // member stays and reports, the leader decides again
    if (moved && !LastMoveSubstitute().empty())
    {
        std::string const substitute = LastMoveSubstitute();
        ObjectGuid const mob = LastMoveMob();
        bot->StopMoving();
        bot->GetMotionMaster()->Clear();
        ReportRefusal(events, events.spot, substitute, mob);
        return false;
    }
    // [lead19] a refusal names its reason (invalid_spot): what the member's guards saw
    // [lead27] the old command's walk stops at once (user: «старая отменяется, слушается только новая»): no spline to
    // a superseded point goes on
    std::string refusal;
    if (!moved)
    {
        if (bot->isMoving())
        {
            bot->StopMoving();
            bot->GetMotionMaster()->Clear();
        }
        std::string const guard = LastMoveRefusal();  // [lead235] the MoveTo guard that refused, by name
        ObjectGuid const mob = LastMoveMob();
        refusal = WalkRefusal(events.spot);
        // [lead372] user: the member reaches the spot safely: the leader checked it with these guards (DryMoveTo); a
        // member that still finds no safe way there stays and reports, the leader decides again (lead29 walked it
        // anyway). Only a refusal of no place (the walk under way, the game holding the bot) walks the plain path
        // to it. Honest play: never through a closed door
        bool const lasting = !TransientRefusal(guard);
        if (lasting || refusal.rfind("closed door", 0) == 0)
        {
            ReportRefusal(events, events.spot, lasting ? guard : refusal, mob);
            return false;
        }
        if (refusal.empty())
            refusal = guard;
        if (!WalkBotRoute(bot, events.spot.GetPositionX(), events.spot.GetPositionY(),
                          events.spot.GetPositionZ()))  // [lead109] the leader's route (BotPath.h)
            bot->GetMotionMaster()->MovePoint(0, events.spot.GetPositionX(), events.spot.GetPositionY(),
                                              events.spot.GetPositionZ(), FORCED_MOVEMENT_NONE, 0.0f, 0.0f, true);
    }
    Leader::Events::Ack(bot->GetGUID(), events.seq, events.move == Leader::MoveEvent::MoveTo ? "move" : "stand",
                        moved ? "walks to the spot: " + events.moveWhy
                              : "member_refusal: " + events.moveWhy + " - " + refusal + ", walked anyway");
    return true;
}

bool LeaderMoveOrderAction::FollowPlace(Unit* target, uint8 index, float& x, float& y, float& z)
{
    if (!target || !target->IsAlive())
        return false;
    float const back = 3.0f + 2.5f * float((index / 2) % 2);
    float const side = index % 2 ? 1.5f : -1.5f;
    // [lead219] the chase of a mob: behind it within its melee reach (LeaderCombatSpot's melee distance, the two
    // places a little apart), not the follow rows behind a walking member (3-5.5 yd: out of melee of a small mob)
    // [lead580] b215 rfc-h 5E 365-411 s: the place behind Oggleflint met the wall at his back, the place fell back to
    // his own centre and the rogue stood there 45 s, every ability "no line of sight". A chase place is a melee place
    // the game hits the target from (LeaderMeleePlace: in its melee range and sight, behind it first); none is no
    // place, never the target's own position
    if (bot->IsValidAttackTarget(target))
    {
        Position at;
        bool behind = true;
        if (!Leader::Fight::LeaderMeleePlace(bot, target, index % 2 ? -1.0f : 1.0f, at, &behind))
            return false;
        _placeBehind = behind;
        x = at.GetPositionX();
        y = at.GetPositionY();
        z = at.GetPositionZ();
        return true;
    }
    float const o = target->GetOrientation();
    // the follow row turned round the target by `turn` (0 behind it, 1 in front of it)
    auto const placeAt = [&](float turn, float& px, float& py, float& pz)
    {
        float const angle = o + float(M_PI) * (1.0f + turn);
        px = target->GetPositionX() + std::cos(angle) * back + std::cos(angle - float(M_PI) / 2.0f) * side;
        py = target->GetPositionY() + std::sin(angle) * back + std::sin(angle - float(M_PI) / 2.0f) * side;
        pz = target->GetPositionZ();
        // [lead161] a non-finite place (an orientation or a collision answer that is no number) is no place: the
        // target's own position, and none when that is not finite either
        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz) ||
            !bot->GetMap()->CheckCollisionAndGetValidCoords(target, target->GetPositionX(), target->GetPositionY(),
                                                            target->GetPositionZ(), px, py, pz) ||
            !std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz))
        {
            px = target->GetPositionX();
            py = target->GetPositionY();
            pz = target->GetPositionZ();
        }
        return std::isfinite(px) && std::isfinite(py) && std::isfinite(pz);
    };
    if (!placeAt(0.0f, x, y, z))
        return false;
    // [sfk240] b445 sfk-n five3-3 / five5-5: the tank back from its corpse run stood at the entry spot (-229.1,
    // 2109.2), 5.4 yd from the exit areatrigger 194 (radius 5), facing in; the row behind it lay in the exit, every
    // member's walk along with it was refused (InInstanceExit), lead77 held the tank for them: 0/8 to the stall.
    // A row in the exit turns round the target to the first place out of it
    uint32 const exit = bot->GetMap()->IsDungeon()
                            ? InInstanceExit(bot->GetMapId(), x, y, z, bot->GetCombatReach())
                            : 0;
    if (!exit)
        return true;
    static constexpr float TURNS[] = {0.5f, -0.5f, 0.75f, -0.75f, 0.25f, -0.25f, 1.0f};
    for (float const turn : TURNS)
    {
        float tx = 0.0f;
        float ty = 0.0f;
        float tz = 0.0f;
        if (!placeAt(turn, tx, ty, tz) || InInstanceExit(bot->GetMapId(), tx, ty, tz, bot->GetCombatReach()))
            continue;
        thread_local std::set<std::pair<ObjectGuid, uint32>> logged;  // (member, exit) once
        if (logged.size() > 4096)
            logged.clear();
        if (logged.insert({bot->GetGUID(), exit}).second)
            LOG_INFO("playerbots",
                     "[sfk240] {}: its follow place behind {} is in the instance's exit (areatrigger {}): turned "
                     "{:.0f} deg round it to ({:.1f}, {:.1f}, {:.1f})",
                     bot->GetName(), target->GetName(), exit, turn * 180.0f, tx, ty, tz);
        x = tx;
        y = ty;
        z = tz;
        return true;
    }
    return true;
}

std::string LeaderMoveOrderAction::DryFollow(Unit* target, uint8 index, ObjectGuid& mob, bool chase)
{
    mob.Clear();
    _judgedFollow.valid = false;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    // [lead580] a chase with no melee place in the target's reach and sight is no chase the member can walk
    if (!FollowPlace(target, index, x, y, z))
        return target && target->IsAlive() && bot->IsValidAttackTarget(target)
                   ? std::string("no melee place in reach and sight of the target")
                   : std::string();
    // the executor's own no-walk facts: a chased mob in melee reach, the member at its place
    // [lead532] b212 rfc-h 5H 1044-1050 s / 5C 637-834 s / 5G 91.8 s: 9 member_refusals "chase: behind the target -
    // GuardIdleMobPath" - the member stood in melee reach in the mob's front arc, this returned "no refusal" without
    // judging the walk (no judged follow left), and the executor walked behind the mob (lead418: in reach in the front
    // arc the walk goes on) under its own idle guard, which refused it. The no-walk facts are the executor's: a chased
    // mob in reach is no walk only when its victim is the member or the member stands out of its front arc; a chase
    // out of reach is walked even from the place (lead523)
    // [lead580] in reach also means in the game's sight of the mob (ChaseDoneHere)
    bool const chased = bot->IsValidAttackTarget(target);
    if (chased ? ChaseDoneHere(target) : bot->GetExactDist2d(x, y) <= Leader::LEADER_SPOT_TOLERANCE)
        return std::string();
    MovementAction::s_dryRun = true;
    _dryFollow = true;
    _dryChase = chase;
    bool const goes =
        MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_NORMAL, true);
    _dryFollow = false;
    _dryChase = false;
    MovementAction::s_dryRun = false;
    mob = MovementAction::s_moveMob;
    // [lead491] b209 rfc-h 5A 97.0 s: "chase_refused ... GuardIdleMobPath (the walk would wake Ragefire Trogg)" -
    // the member's guard refused the leader's chase of a target that itself stood inside that Trogg's reach. The
    // chase's idle reach is the leader's melee test, the one its combat spots at the target are judged by
    if (chase && (goes || TransientRefusal(MovementAction::s_moveRefusal)))
    {
        bool noPath = false;
        if (Creature* idle = Leader::Fight::LeaderAggroOnWalk(bot, Position(x, y, z), noPath, target))
        {
            mob = idle->GetGUID();
            return "the chase walks deeper into " + idle->GetName() + "'s reach than the fight stands";
        }
    }
    // the executor's verdicts (Execute's Follow branch): a guard that ends the walk elsewhere, a lasting refusal
    if (goes && !MovementAction::s_moveSubstitute.empty())
        return MovementAction::s_moveSubstitute;
    if (!goes && !TransientRefusal(MovementAction::s_moveRefusal))
        return MovementAction::s_moveRefusal;
    mob.Clear();
    if (goes)  // [lead370] the walk judged clear: the executor's walk with the same inputs takes it as judged
        _judgedFollow = {true, target->GetGUID(), bot->GetPosition(), Position(x, y, z), chase};
    return std::string();
}

bool LeaderMoveOrderAction::ChaseDoneHere(Unit* target) const
{
    // [lead418] behind the mob (out of its front half, where it parries / blocks) or its own victim; [lead580] in
    // front only when no place behind it fits (FollowPlace's last answer), and always in the game's melee range and
    // line of sight of it - the member's spells answer "no line of sight" otherwise
    return bot->IsWithinMeleeRange(target) && bot->IsWithinLOSInMap(target) &&
           (target->GetVictim() == bot || !_placeBehind || !target->HasInArc(float(M_PI), bot));
}

void LeaderMoveOrderAction::NoteDeadEnd(Leader::MemberEvents const& events, char const* path,
                                        std::string const& detail)
{
    // the key holds `path`'s address (always a string literal here), so a standing member costs no allocation
    thread_local std::set<std::tuple<uint64, uint32, void const*>> logged;
    if (logged.size() > 4096)
        logged.clear();
    if (!logged.insert({bot->GetGUID().GetRawValue(), events.seq, static_cast<void const*>(path)}).second)
        return;
    char const* const what = events.move == Leader::MoveEvent::Follow   ? "follow"
                             : events.move == Leader::MoveEvent::MoveTo ? "move"
                             : events.move == Leader::MoveEvent::StandAt ? "stand"
                             : events.move == Leader::MoveEvent::Hold    ? "hold"
                                                                         : "none";
    LOG_INFO("playerbots",
             "[nav190] {} stands on its {} order ({}, seq {}): {} - {}; at ({:.1f},{:.1f},{:.1f}) map {}, spot "
             "({:.1f},{:.1f},{:.1f}) {:.1f} yd off (tolerance {:.1f}), moving {}, can move {}",
             bot->GetName(), what, events.moveWhy, events.seq, path, detail, bot->GetPositionX(), bot->GetPositionY(),
             bot->GetPositionZ(), bot->GetMapId(), events.spot.GetPositionX(), events.spot.GetPositionY(),
             events.spot.GetPositionZ(),
             bot->GetExactDist2d(events.spot.GetPositionX(), events.spot.GetPositionY()), events.tolerance,
             bot->isMoving() ? "yes" : "no", botAI->CanMove() ? "yes" : "no");
}

void LeaderMoveOrderAction::ReportRefusal(Leader::MemberEvents const& events, Position const& spot,
                                          std::string const& reason, ObjectGuid mob)
{
    Leader::MoveRefusal refusal;
    refusal.seq = events.seq;
    refusal.move = events.move;
    refusal.why = events.moveWhy;
    refusal.spot = spot;
    refusal.from = bot->GetPosition();
    refusal.reason = reason;
    refusal.mob = mob;
    if (Unit* woken = mob.IsEmpty() ? nullptr : botAI->GetUnit(mob))
        refusal.mobAt = woken->GetPosition();
    if (Unit* target = events.move == Leader::MoveEvent::Follow ? botAI->GetUnit(events.followTarget) : nullptr)
        refusal.targetAt = target->GetPosition();
    Leader::Events::NoteMoveRefused(bot->GetGUID(), refusal);
    char const* const what = events.move == Leader::MoveEvent::Follow   ? "follow"
                             : events.move == Leader::MoveEvent::MoveTo ? "move"
                                                                        : "stand";
    Leader::Events::Ack(bot->GetGUID(), events.seq, what,
                        "member_refusal: " + events.moveWhy + " - " + reason + ", stays (reported)");
    // [nav190] the ack reaches the leader's decisions file only while a harness run is open: a refused walk outside
    // one left no trace at all (b475 dm-n five 10, 0 "member_refusal" lines in a 41 min stall)
    NoteDeadEnd(events, "member_refusal", reason);
}

bool LeaderMoveOrderAction::SkipsIdleGuard()
{
    // [lead27] a Follow walk is the member's own path (no point the leader checked): its guards stay
    // [lead34] ... and so does the tank's route walk (the route's point, not a leader-checked spot)
    // [lead373] the leader's dry check judges the move it is about to issue, not the member's current one
    if (_judgedWalk)  // [lead370] the leader judged this very walk (DryFollow)
        return true;
    if (_dryWhy)
        return Leader::LedBot(bot) && *_dryWhy != "route walk";
    if (_dryFollow)  // [lead374] a follow place is the member's own path: its guards run
        return _dryChase;  // [lead491] the leader's chase: its idle reach is the leader's melee test
    Leader::MemberEvents events;
    bool const got = Leader::Events::Get(bot->GetGUID(), events);
    // [lead491] the leader's chase of a fight target, judged clear by the leader's last DryFollow of it (chase: the
    // leader's melee test): the member's idle guard does not judge it again (project rule: leader commands are
    // final, no member refusal). A chase the leader refused (no judged follow left) keeps the member's guards
    if (got && events.move == Leader::MoveEvent::Follow && Leader::LedBot(bot) && _judgedFollow.valid &&
        _judgedFollow.chase && _judgedFollow.target == events.followTarget)
        if (Unit* chased = botAI->GetUnit(events.followTarget); chased && bot->IsValidAttackTarget(chased))
            return true;
    return Leader::LedBot(bot) &&
           !(got && (events.move == Leader::MoveEvent::Follow || events.moveWhy == "route walk"));
}

bool LeaderMoveOrderAction::DryMoveTo(Position const& to, std::string const& why)
{
    if (bot->GetExactDist2d(to.GetPositionX(), to.GetPositionY()) <= 1.0f)
        return true;
    MovementAction::s_dryRun = true;
    _dryWhy = &why;
    bool goes = MoveTo(bot->GetMapId(), to.GetPositionX(), to.GetPositionY(), to.GetPositionZ(), false, false, false,
                       false, MovementPriority::MOVEMENT_COMBAT, true);
    _dryWhy = nullptr;
    MovementAction::s_dryRun = false;
    // [lead372] a walk a guard would end short of the spot does not reach it: the member would stay and report
    if (goes && !MovementAction::s_moveSubstitute.empty())
    {
        MovementAction::s_moveRefusal = MovementAction::s_moveSubstitute;
        goes = false;
    }
    return goes;
}

std::string LeaderMoveOrderAction::WalkRefusal(Position const& to)
{
    // [lead99] the one route search (Leader::Route), no PathGenerator
    Leader::RouteResult const route = Leader::Route(bot, to);
    if (!route.found)
        return "no path (" + route.Describe() + ")";
    Movement::PointsArray const points(route.points.begin(), route.points.end());
    if (points.size() >= 2)
    {
        std::vector<G3D::Vector3> walk(points.begin(), points.end());
        if (ClosedDoorNear(bot, bot->GetPositionX(), bot->GetPositionY(), to.GetPositionX(), to.GetPositionY(),
                           5.0f))
            if (ClosedDoorHit const hit = PathThroughClosedDoor(bot, walk); hit.Found())
                return Acore::StringFormat("closed door {} {} at {:.1f} yd", hit.door->GetName(),
                                           hit.door->GetEntry(), hit.walked);
        std::string const wakes = IdleWakeOnWalk(points, ObjectGuid::Empty);
        if (!wakes.empty())
            return "would wake " + wakes;
    }
    return std::string();
}

Unit* LeaderMoveOrderAction::WalkWakes(Position const& to, ObjectGuid exempt)
{
    Leader::RouteResult const route = Leader::Route(bot, to);
    if (!route.found)
        return nullptr;
    return IdleWokenOnWalk(Movement::PointsArray(route.points.begin(), route.points.end()), exempt);
}

bool LeaderHealTargetAction::Execute(Event /*event*/)
{
    Leader::MemberEvents events;
    if (!Leader::Events::Get(bot->GetGUID(), events) || events.healTarget.IsEmpty())
        return false;
    Unit* target = botAI->GetUnit(events.healTarget);
    if (!target || !target->IsAlive() || target->GetHealthPct() >= 100.0f)
        return false;
    // [lead946] a heal that cannot land (Cyclone, Banish: the core's own test with this healer as the caster) or on a
    // member that cannot lose health now (Ice Block, Divine Shield: immune to every school's damage past this global
    // cooldown) waits - it heals nothing the member could lose before the immunity ends
    if (bool const immune = HealTriage::IsHealImmune(bot, target);
        immune || HealTriage::CannotLoseHealth(bot, target))
    {
        thread_local std::map<ObjectGuid, ObjectGuid> waitLogged;  // healer -> the member its top-up waits for
        if (waitLogged[bot->GetGUID()] != target->GetGUID())
        {
            waitLogged[bot->GetGUID()] = target->GetGUID();
            LOG_INFO("playerbots", "[lead946] {} holds its top-up of {} ({:.0f} %): {}", bot->GetName(),
                     target->GetName(), target->GetHealthPct(),
                     immune ? "no heal lands on it" : "it cannot lose health until its immunity ends");
        }
        return false;
    }
    // the healer's own heals, the most efficient it can cast on the member now (a top-up, not a burst)
    static constexpr char const* TOP_UP_HEALS[] = {"rejuvenation", "nourish",     "healing touch", "regrowth",
                                                   "renew",        "heal",        "lesser heal",   "greater heal",
                                                   "flash heal",   "healing wave", "lesser healing wave",
                                                   "holy light",   "flash of light"};
    // [lead940] user 2026-11-03 (b226 5F: the tree's TopUp Rejuvenation on the tank and the warlock 1-15 s before the
    // pull ticked into it - 32 of 127 healer heal-threat losses had such a top-up HoT running, the game splits each
    // tick's threat over every mob with the healed member on its threat list, ThreatManager::
    // ForwardThreatForAssistingMe, and a mob the tank has 0 on goes to the druid). Out of a fight a direct heal tops
    // the member up within its cast time, a heal over time only over its whole duration - always the slower way to
    // the same health and its ticks run into the pull. Out of a fight the direct heals go first; a heal over time
    // only when no direct heal can be cast, and the leader's readiness holds the pull until it ends (LeaderReadyWhy)
    bool const outOfFight = !bot->IsInCombat();
    for (bool const hots : {false, true})
    {
        if (hots && !outOfFight)
            break;  // in a fight one pass, the list's order (the leader's Heal gate judges the threat)
        // [lead1263] user rule (careful with heals over time on the tank before the pull; heal only when needed):
        // out of a fight a heal over time only when the pull waits on this member's health (the leader's healHolds:
        // a readiness hold or the boss gap names it, or it is below its health line) - b249 rfc-h 14 top-ups at
        // 95-99.7 % ended in Renew / Rejuvenation ticks the pull waited out (93 s)
        if (hots && !events.healHolds)
        {
            thread_local std::map<ObjectGuid, ObjectGuid> skipLogged;  // healer -> the member its HoT was skipped on
            if (skipLogged[bot->GetGUID()] != target->GetGUID())
            {
                skipLogged[bot->GetGUID()] = target->GetGUID();
                LOG_INFO("playerbots", "[lead1263] {} casts no heal over time on {} ({:.0f} %) out of a fight: no "
                         "direct heal can be cast and the pull does not wait on its health", bot->GetName(),
                         target->GetName(), target->GetHealthPct());
            }
            break;
        }
        for (char const* heal : TOP_UP_HEALS)
        {
            uint32 const healId = AI_VALUE2(uint32, "spell id", heal);
            SpellInfo const* info = healId ? sSpellMgr->GetSpellInfo(healId) : nullptr;
            bool const periodic = HealTriage::IsPeriodicHeal(info);
            if (outOfFight && periodic != hots)
                continue;
            // [lead396] b201 rfc-h 5A 1226-1246 s: the pull waited 20 s on "Rtdruaa buffing" - the resto druid's TopUp
            // on itself (83 %) recast Rejuvenation each global cooldown over its own running Rejuvenation, and its
            // rebuff window closes only on a tick off the cooldown. A heal over time this healer has running on the
            // member is healing already: the next heal of the list
            if (periodic && target->HasAura(healId, bot->GetGUID()))
                continue;
            // [lead1434] b263 5B-10 7.6 s: "tops up Rtwaraa with holy light" on a tank missing 102 health - the
            // list's order put Holy Light before Flash of Light. A heal bigger than the healer's smallest is skipped
            // while the member misses less than that one heals (spell data): the smaller heal of the list tops it up
            if (RestState::OverhealsSmallerHeal(bot, target, info))
                continue;
            // [lead1440] in a fight a heal over time first in the list (Rejuvenation) heals no more than the member
            // misses plus the leader's estimate of the main tank's damage before it expires (RestState::HotOverheals);
            // out of a fight lead940 / lead1263 hold it already (only when no direct heal can be cast)
            if (periodic && !outOfFight && RestState::HotOverheals(bot, target, info))
                continue;
            if (botAI->CanCastSpell(heal, target) && botAI->CastSpell(heal, target))
            {
                if (outOfFight && periodic)
                    LOG_INFO("playerbots", "[lead940] {} tops up {} with {}: no direct heal can be cast, the pull "
                             "waits for its end", bot->GetName(), target->GetName(), heal);
                Leader::Events::Ack(bot->GetGUID(), events.seq, "heal", std::string("tops up ") + target->GetName() +
                                                                            " with " + heal);
                return true;
            }
        }
    }
    return false;
}

bool LeaderAttackOrderAction::Execute(Event /*event*/)
{
    Leader::MemberEvents events;
    if (!Leader::Events::Get(bot->GetGUID(), events) || events.attack != Leader::AttackEvent::Attack)
        return false;
    // [lead68] the leader's Stop (no threat headroom): no white swing / auto shot either - the attack stops until
    // the tank leads again
    if (events.intensity == Leader::Intensity::Stop)
    {
        bool const was = bot->GetVictim() || bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
        bot->AttackStop();
        bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
        if (was)
            Leader::Events::Ack(bot->GetGUID(), events.seq, "attack", "stops its auto attack: the leader's Stop");
        return was;
    }
    Unit* target = botAI->GetUnit(events.target);
    if (!target || !target->IsAlive() || !target->IsInCombat())
        return false;
    bool const attacked = Attack(target);
    // [lead42] b91 rfc-h 203-245 s: "the attack refused: Ragefire Trogg" from every member, no reason - named now
    std::string why;
    if (!attacked)
        why = !bot->IsWithinLOSInMap(target)                                        ? " (no line of sight)"
              : target->ToCreature() && target->ToCreature()->IsInEvadeMode()       ? " (it evades)"
              : !bot->IsValidAttackTarget(target)                                   ? " (no valid attack target)"
              : bot->GetVictim() == target                                          ? " (already attacking it)"
                                                                                    : " (the attack itself failed)";
    Leader::Events::Ack(bot->GetGUID(), events.seq, "attack",
                        (attacked ? "attacks " : "the attack refused: ") + target->GetName() + why);
    return attacked;
}
