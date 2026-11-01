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
#include "LastMovementValue.h"  // [lead36]
#include "LeaderValues.h"
#include "LeaderOrders.h"  // [lead15]
#include "PathGenerator.h"  // [lead19]
#include "TerrainHazard.h"  // [lead19] closed doors
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

// [lead15] the leader's member events
bool LeaderMoveOrderAction::Execute(Event /*event*/)
{
    Leader::MemberEvents events;
    if (!Leader::Events::Get(bot->GetGUID(), events))
        return false;
    // [lead27] Follow: the place `followIndex` behind the target (two rows of two), walked with the member's own path
    // guards (the leader issued no point)
    if (events.move == Leader::MoveEvent::Follow)
    {
        Unit* target = botAI->GetUnit(events.followTarget);
        if (!target || !target->IsAlive())
            return false;
        float const back = 3.0f + 2.5f * float((events.followIndex / 2) % 2);
        float const side = events.followIndex % 2 ? 1.5f : -1.5f;
        float const o = target->GetOrientation();
        float x = target->GetPositionX() + std::cos(o + float(M_PI)) * back + std::cos(o + float(M_PI) / 2.0f) * side;
        float y = target->GetPositionY() + std::sin(o + float(M_PI)) * back + std::sin(o + float(M_PI) / 2.0f) * side;
        float z = target->GetPositionZ();
        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(target, target->GetPositionX(), target->GetPositionY(),
                                                             target->GetPositionZ(), x, y, z))
        {
            x = target->GetPositionX();
            y = target->GetPositionY();
            z = target->GetPositionZ();
        }
        // [lead44] b93 rfc-h 313-362 s: Rtshaab's Follow fired "leader move order -> 0.0 yd" every tick at 94 and its
        // "ready reply" never ran - the leader waited 50 s for its rebuff. A walk already under way (or done) is no
        // live work: not executed, the member's other actions run
        if (IsDuplicateMove(x, y, z))
            return false;
        if (MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_NORMAL, true))
            return true;
        // [lead40] b89 rfc-h 57.8 s: Rtshaab "member_refusal: the member's guard refused the follow walk (ledge /
        // ground effect)". Members never refuse (the leader checks before it issues): the follow place is walked on
        // the plain navmesh path, no refusal. Honest play: never through a closed door (that one stays, reported).
        std::string const refusal = WalkRefusal(Position(x, y, z));
        if (refusal.rfind("closed door", 0) == 0)
        {
            Leader::Events::Ack(bot->GetGUID(), events.seq, "follow", "member_refusal: " + refusal + ", stays");
            return true;
        }
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
        return false;
    if (!bot->IsStandState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);
    if (bot->GetExactDist2d(events.spot.GetPositionX(), events.spot.GetPositionY()) <= events.tolerance)
    {
        Leader::Events::Ack(bot->GetGUID(), events.seq, events.move == Leader::MoveEvent::MoveTo ? "move" : "stand",
                            "on the spot");
        return true;
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
        return false;
    }
    bool const moved = MoveTo(bot->GetMapId(), events.spot.GetPositionX(), events.spot.GetPositionY(),
                              events.spot.GetPositionZ(), false, false, false, false,
                              MovementPriority::MOVEMENT_COMBAT, true);
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
        refusal = WalkRefusal(events.spot);
        if (refusal.empty())
            refusal = "another guard (ledge, ground effect)";
        // [lead29] a member never refuses a leader command: the leader validated the spot, the member walks the plain
        // path to it; the refusal is a blocker (member_refusal) - the leader's model and the member's disagree
        // honest play: never through a closed door (that one stays refused - the leader's check missed it)
        if (refusal.rfind("closed door", 0) != 0)
            bot->GetMotionMaster()->MovePoint(0, events.spot.GetPositionX(), events.spot.GetPositionY(),
                                              events.spot.GetPositionZ(), FORCED_MOVEMENT_NONE, 0.0f, 0.0f, true);
    }
    Leader::Events::Ack(bot->GetGUID(), events.seq, events.move == Leader::MoveEvent::MoveTo ? "move" : "stand",
                        moved ? "walks to the spot: " + events.moveWhy
                              : "member_refusal: " + events.moveWhy + " - " + refusal + ", walked anyway");
    return true;
}

bool LeaderMoveOrderAction::SkipsIdleGuard()
{
    // [lead27] a Follow walk is the member's own path (no point the leader checked): its guards stay
    // [lead34] ... and so does the tank's route walk (the route's point, not a leader-checked spot)
    Leader::MemberEvents events;
    bool const got = Leader::Events::Get(bot->GetGUID(), events);
    return Leader::LedBot(bot) &&
           !(got && (events.move == Leader::MoveEvent::Follow || events.moveWhy == "route walk"));
}

bool LeaderMoveOrderAction::DryMoveTo(Position const& to)
{
    if (bot->GetExactDist2d(to.GetPositionX(), to.GetPositionY()) <= 1.0f)
        return true;
    MovementAction::s_dryRun = true;
    bool const goes = MoveTo(bot->GetMapId(), to.GetPositionX(), to.GetPositionY(), to.GetPositionZ(), false, false,
                             false, false, MovementPriority::MOVEMENT_COMBAT, true);
    MovementAction::s_dryRun = false;
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
    // the healer's own heals, the most efficient it can cast on the member now (a top-up, not a burst)
    for (char const* heal : {"rejuvenation", "nourish", "healing touch", "regrowth", "renew", "heal", "lesser heal",
                             "greater heal", "flash heal", "healing wave", "lesser healing wave", "holy light",
                             "flash of light"})
        if (botAI->CanCastSpell(heal, target) && botAI->CastSpell(heal, target))
        {
            Leader::Events::Ack(bot->GetGUID(), events.seq, "heal", std::string("tops up ") + target->GetName() +
                                                                        " with " + heal);
            return true;
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
