/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ChooseTargetActions.h"

#include <mutex>
#include <unordered_map>
#include "AggressiveTargetValue.h"
#include "Battleground.h"
#include "ChooseRpgTargetAction.h"
#include "ClassicShared.h"
#include "CreatureGroups.h"
#include "DamageTaken.h"
#include "DBCStores.h"
#include "EscortProtect.h"  // [b36_ohb] EscortWalksIntoPack
#include "EstimatedLifetimeValue.h"
#include "Event.h"
#include "Group.h"
#include "HealthTriggers.h"
#include "InstanceScript.h"
#include "KillOrder.h"
#include "LeaderOrders.h"  // [lead1] the tank's commit as a pull order (C:/WoW/LEADER_REFACTOR_PLAN.md step 1)
#include "LootObjectStack.h"
#include "Map.h"
#include "MobRoles.h"
#include "MotionMaster.h"
#include "NewRpgStrategy.h"
#include "ObjectMgr.h"
#include "PartyMemberToHeal.h"
#include "PartyStrengthValue.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "AKTriggers.h"
#include "ArcShared.h"  // [b34_arc] Arcatraz::IsCorpseTrap
#include "ANTriggers.h"
#include "CoSTriggers.h"  // [b39_cos] CoSHelpers::IsGuardianTimerRunning
#include "MgTShared.h"
#include "PossibleRpgTargetsValue.h"
#include "TestRunnerStrategy.h"  // [b32_strat_gate] TestRunner::IsDoorCut
#include "PullStrategy.h"
#include "PullActions.h"  // FightHold ([drift_fight_hold])
#include "PvpTriggers.h"
#include "PvpValues.h"
#include "RtiTargetValue.h"
#include "RtiValue.h"
#include "ServerFacade.h"
#include "SmartAI.h"
#include "SmartScriptMgr.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"  // [b33_aoepull] KnowsAnyRank
#include "TargetValue.h"
#include "TerrainHazard.h"
#include "WaypointMgr.h"
#include "World.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>   // [lead7]
#include <iterator>
#include <map>
#include <memory>  // [lead7]
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include "LeaderPlannerShared.h"  // [lead31a] the pull planner lives in the leader (Ai/Leader/LeaderPlanner.cpp)

// [lead31a] C:/WoW/LEADER_REFACTOR_PLAN.md: the pull planner moved into the leader module (Ai/Leader); this file keeps
// the tank's EXECUTION of a pull decision (isUseful / TakeLeaderDecision / LoadDecision / Execute / PullBack /
// StartLosPull) and the other target actions.
using namespace PullPlanning;

bool AttackEnemyPlayerAction::isUseful()
{
    if (PlayerHasFlag::IsCapturingFlag(bot))
        return false;

    return !sPlayerbotAIConfig.IsPvpProhibited(bot->GetZoneId(), bot->GetAreaId());
}

bool AttackEnemyFlagCarrierAction::isUseful()
{
    Unit* target = context->GetValue<Unit*>("enemy flag carrier")->Get();
    Battleground* bg = bot->GetBattleground();
    if (bg && bg->GetBgTypeID(true) == BATTLEGROUND_WS)
    {
        if (botAI->GetState() == BOT_STATE_COMBAT && AI_VALUE(Unit*, "current target") == target)
            return false;
        // A queued action can outlive the trigger that selected the carrier.
        return CanRespondToWsgCarrier(botAI, target);
    }

    if (!target || !ServerFacade::instance().IsDistanceLessOrEqualThan(
                       ServerFacade::instance().GetDistance2d(bot, target), 100.0f))
        return false;

    return PlayerHasFlag::IsCapturingFlag(bot);
}

// [lead7] C:/WoW/LEADER_REFACTOR_PLAN.md step 7. A tank the harness leads: LEADER_PLANNER_IN_LEADER asks the leader
// and executes its decision (no look of its own); LEADER_PLANNER_DIFF looks as before, and the tank's planner
// instance looks in shadow right after (the same instant, the same world state), the two decisions compared. Every
// other bot looks as before.
bool AggressiveTargetAction::isUseful()
{
    if ((Leader::LEADER_PLANNER_IN_LEADER || Leader::LEADER_PLANNER_DIFF) && PlayerbotAI::IsTank(bot) &&
        Leader::Orders::IsTracked(bot->GetGUID()))
    {
        if (Leader::LEADER_PLANNER_IN_LEADER)
            return TakeLeaderDecision();

        if (Leader::LEADER_PLANNER_DIFF)
        {
            _fromLeader = false;
            bool const go = DecideLook();
            PullPlanner::Decision own;
            FillDecision(go, own);
            PullPlanner::Snapshot snap(botAI);
            PullPlanner::Decision library;
            if (PullPlanner::Decide(bot, snap, true, library))
                PullPlanner::NoteDiff(bot, own, library);
            return go;
        }
    }

    _fromLeader = false;
    return DecideLook();
}

bool AggressiveTargetAction::Execute(Event event)
{
    // [lead7] the decision below was issued by the leader (TakeLeaderDecision): the reports say the tank executes it
    executingLeaderDecision = _fromLeader;
    if (_fromLeader)  // [lead4c] the issued decision is taken now that it runs
        PullPlanner::ConsumeIssued(bot->GetGUID(), _fromLeaderMs);
    _fromLeader = false;
    if (!_approach.IsEmpty())
    {
        Unit* mob = botAI->GetUnit(_approach);
        _approach.Clear();
        bool const walked = mob && MoveTo(mob, APPROACH_DISTANCE);
        // [lead1] a walk of the tank's own (no pull plan): a decision line, no order
        if (walked)
            Leader::Orders::Note(bot, mob, "approach_walk", "no aggressive target: walks toward the nearest mob",
                                 executingLeaderDecision);
        return walked;
    }

    // A side of the pack that brings fewer mobs: the tank walks there first, and plans again from there.
    if (_planSide && !_planTarget.IsEmpty() && bot->GetExactDist(_sideSpot) > SIDE_PULL_ARRIVED)
    {
        _planFor.Clear();
        bool const walked =
            MoveTo(bot->GetMapId(), _sideSpot.GetPositionX(), _sideSpot.GetPositionY(), _sideSpot.GetPositionZ());
        // [lead1] the walk to the side spot is the pull order's walk (the plan goes on from there)
        if (walked)
            Leader::Orders::Report(bot, botAI->GetUnit(_planTarget), Leader::OrderState::Walking,
                                   Leader::PullMethod::Unknown, "side spot walk: " + _sideNote, _planMobs, _planCost,
                                   false, executingLeaderDecision);
        return walked;
    }

    if (PlayerbotAI::IsTank(bot))
    {
        if (_planSingle && !_planCountsSap)
            PrePullCc::Cancel(bot);  // one mob comes: no crowd control on its pack
        else
            PrePullCc::MarkPulled(bot);  // the group's crowd control stops holding the pull
    }

    if (_planTarget.IsEmpty())
    {
        // [lead3] a leader-issued attack (a BossPull order, or an attack of the leader's pick): that mob, walked in
        // for sight first as an engage is
        if (executingLeaderDecision && !_leaderAttack.IsEmpty())
        {
            Unit* attacked = botAI->GetUnit(_leaderAttack);
            Leader::PullMethod const method =
                _leaderMethod != Leader::PullMethod::Unknown ? _leaderMethod : Leader::PullMethod::Engage;
            _leaderAttack.Clear();
            _leaderMethod = Leader::PullMethod::Unknown;
            if (!attacked || !attacked->IsInWorld() || !attacked->IsAlive())
                return false;
            std::string const why = std::string(Leader::MethodName(method)) + " (the leader's order)";
            if (!attacked->IsInCombat() && !bot->IsWithinLOSInMap(attacked))
            {
                bool const walked = MoveToLOS(attacked) || ReachCombatTo(attacked, 0.0f);
                return PostCommit(bot, attacked, walked, method, why + ": walks for sight", 0, 0.0f, false);
            }
            bool const attackedOk = Attack(attacked);
            if (!attacked->IsInCombat())
                PostCommit(bot, attacked, attackedOk, method, why, 0, 0.0f, false);
            return attackedOk;
        }
        // [lead1] the tank's own attack of its aggressive target (no careful plan): an engage order
        Unit* attacked = GetTarget();
        bool const attackedOk = AttackAction::Execute(event);
        if (attacked && !attacked->IsInCombat())
            PostCommit(bot, attacked, attackedOk, Leader::PullMethod::Engage, "attack (no careful plan)", 0, 0.0f,
                       false);
        return attackedOk;
    }

    Unit* target = botAI->GetUnit(_planTarget);
    bool pullBack = _planPullBack;  // [b34_arc] no longer const: a corpse trap is walked to
    bool losPull = _planLos;
    bool const single = _planSingle;
    uint32 const commitMobs = _planMobs;  // [lead1] the plan's count before the log line below resets it
    float const commitCost = _planCost;
    std::string const commitReason = _planReason;
    _planFor.Clear();  // the next look plans afresh
    _losTempoFor.Clear();
    if (!target || !target->IsInWorld() || !target->IsAlive())
        return false;

    // [b34_arc] An Arcatraz corpse trap never comes back to the tank: the "pull back" / "los pull" shot only put it
    // in combat unengaged (arc-h 20261026-014521-arc-h-5F-31, Warder Corpse spawn 98 shot at 696 s and 1030 s, 20 min
    // of phantom combat). Walked to out of combat, it bursts by its own 8 yd rule and the spawns come to the tank.
    if ((pullBack || losPull) && Arcatraz::IsCorpseTrap(target) && !target->IsInCombat())
    {
        // [lead57] the method is the leader's task: a change of it here is a domain violation (reported)
        if (executingLeaderDecision)
            Leader::Orders::Note(bot, target, "domain_violation",
                                 "the tank changed the leader's method: walked to a corpse trap", true);
        LOG_INFO("playerbots", "[b34_arc] {}: {} {} is a rooted corpse trap: walked to (it bursts at 8 yd) instead of "
                 "the planned {}", bot->GetName(), target->GetName(), target->GetEntry(),
                 losPull ? "los pull" : "pull back");
        pullBack = false;
        losPull = false;
    }

    if (_planMobs)
    {
        bool const side = _sideFor == target->GetGUID();
        char const* const plan = single ? "one" : (losPull ? "los pull" : (pullBack ? "pull back" : "engage"));
        std::string const kind =
            std::string(plan) + (_planReason.empty() ? std::string() : " (reason: " + _planReason + ")");
        // User, Stockade 2026-10-24: «какой у него был триггер?» - the plan and why, one line per pull.
        LOG_INFO("playerbots", "[pull plan] {}: {} {} at {:.1f} yd, {} mobs, cost {:.2f}", bot->GetName(), kind,
                 target->GetName(), bot->GetExactDist(target), _planMobs, _planCost);
        PullCheck::Planned(bot, target->GetGUID(), _planMobs, _planCost, _planDrop, kind,
                           side ? _sideNote : std::string(_planPatrol ? "patrol first" : "default"));
        if (side)
            _sideFor.Clear();

        _planMobs = 0;
    }

    if (!pullBack && !losPull)
    {
        // A plan read from the list that ignores the tank's sight (a mob round a corner): it walks there first.
        // [b37_sv] MoveToLOS walks a straight-line point toward the mob, not its path: behind the rock hump that hides
        // the mob that point lies off the navmesh (navmesh_probe 545 --at -66.1 -135.6 -18: no poly holds it) and the
        // walk is refused without a word (likely MoveTo's SearchForBestPath, INVALID_HEIGHT). sv-h
        // 20261026-044253-sv-h-5B-21 (172-275 s): 30 "engage (walks in / charge)" plans on a Bog Overlord 23-25 yd
        // off, "aggressive target: Execute returned false" each time, the tank never moved. Then the navmesh path to
        // the mob (ReachCombatTo: its path end, the sight restore within range).
        if (!bot->IsWithinLOSInMap(target))
        {
            // [lead34] a led tank: the leader walks its path in (Leader MoveTo "engage path in"), the order is open
            // [lead58] LEADER_TANK_WALKS_PULL: the tank walks its own path in (the leader judged it reachable before
            // the order, lead49; its path guards keep it out of other packs' aggro); the order stays open while it
            // walks - its next look goes on once it stands
            if (Leader::LEADER_ROUTE_IS_DATA && Leader::LedBot(bot) && !Leader::LEADER_TANK_WALKS_PULL)
                return PostCommit(bot, target, true, Leader::PullMethod::Engage,
                                  "engage: the leader walks the path in for sight", commitMobs, commitCost, single);
            if (Leader::LEADER_TANK_WALKS_PULL && Leader::LedBot(bot))
            {
                bool const walked = ReachCombatTo(target, 0.0f) || MoveToLOS(target);
                return PostCommit(bot, target, walked, Leader::PullMethod::Engage,
                                  walked ? "engage: the tank walks its path in for sight"
                                         : "engage: the tank's walk in for sight refused",
                                  commitMobs, commitCost, single);
            }
            if (MoveToLOS(target))
                return PostCommit(bot, target, true, Leader::PullMethod::Engage, "engage: walks for sight",
                                  commitMobs, commitCost, single);  // [lead1]

            bool const walked = ReachCombatTo(target, 0.0f);
            thread_local std::unordered_map<uint64, uint32> engageLosLogMs;
            uint32 const logNow = getMSTime();
            uint32& loggedAt = engageLosLogMs[bot->GetGUID().GetRawValue()];
            if (!loggedAt || getMSTimeDiff(loggedAt, logNow) >= 10 * IN_MILLISECONDS)
            {
                loggedAt = logNow ? logNow : 1;
                LOG_INFO("playerbots", "[b37_sv] engage_los: {}: {} {:.1f} yd out of sight, the straight walk for "
                         "sight refused: {}", bot->GetName(), target->GetName(), bot->GetExactDist(target),
                         walked ? "walks its path in" : "its path walk refused too");
            }
            return PostCommit(bot, target, walked, Leader::PullMethod::Engage, "engage: walks its path in for sight",
                              commitMobs, commitCost, single);  // [lead1]
        }

        // An engage a mover walks into: the tank walks the fight back once it is on ("move fight spot back").
        if (_planFightFor == target->GetGUID())
            FightSpot::Plan(bot, _planFight, _planFightBack, _planFightNote);

        _planFightFor.Clear();
        return PostCommit(bot, target, Attack(target), Leader::PullMethod::Engage,
                          "engage" + (commitReason.empty() ? std::string() : " (" + commitReason + ")"), commitMobs,
                          commitCost, single);  // [lead1]
    }

    _lastPullBack = target->GetGUID();
    _lastPullBackMs = getMSTime();
    // [lead1] the ranged pull's commit: the tank's order (the pull strategy's states follow from PullActions)
    Leader::PullMethod const method =
        losPull ? Leader::PullMethod::LosPull : (single ? Leader::PullMethod::One : Leader::PullMethod::PullBack);
    std::string const why = std::string(Leader::MethodName(method)) +
                            (commitReason.empty() ? std::string() : " (" + commitReason + ")");
    return PostCommit(bot, target, losPull ? StartLosPull(target) : PullBack(target, single), method, why,
                      commitMobs, commitCost, single);
}

// The pull sequence of the "pull" and "pull back" strategies, as the "pull" command starts it: the ranged pull,
// then back to this spot, where the mob fights away from the rest of the room. `single`: the pull spell must hit the
// mob alone.
bool AggressiveTargetAction::PullBack(Unit* target, bool single)
{
    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!strategy)
        return false;

    // The careful plan's kill spot (further back, out of the mob's call for help), else where the tank stands.
    Position kill = bot->GetPosition();
    if (_planKillFor == target->GetGUID())
    {
        kill = _planKill;
        if (bot->GetExactDist(kill) > CAREFUL_AGGRO_MARGIN)
            LOG_INFO("playerbots", "[pull back] {}: fights {} at ({:.1f} {:.1f} {:.1f}), {:.1f} yd behind the tank, "
                     "where its call for help reaches fewer", bot->GetName(), target->GetName(), kill.GetPositionX(),
                     kill.GetPositionY(), kill.GetPositionZ(), bot->GetExactDist(kill));

        FightSpot::Chosen(bot);  // the plan checked this spot against the movers: no walk back once the fight is on
    }

    _planKillFor.Clear();
    if (_planFightFor == target->GetGUID())
        LOG_INFO("playerbots", "[fight spot] {} moves the fight back {:.0f} yd: {}", bot->GetName(), _planFightBack,
                 _planFightNote);

    _planFightFor.Clear();
    PositionMap& posMap = AI_VALUE(PositionMap&, "position");
    PositionInfo pullPosition = posMap["pull"];
    pullPosition.Set(kill.GetPositionX(), kill.GetPositionY(), kill.GetPositionZ(), bot->GetMapId());
    posMap["pull"] = pullPosition;

    PublishCastSpot(target);
    PullCastSpot::SetKill(bot, target->GetGUID(), kill);  // [prepull_sight_spot]
    strategy->RequestPull(target, true, single);
    bot->SetSelection(target->GetGUID());
    context->GetValue<Unit*>("current target")->Set(target);
    botAI->ChangeEngine(BOT_STATE_COMBAT);
    return true;
}

bool AggressiveTargetAction::StartLosPull(Unit* target)
{
    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!strategy || !bot->GetGroup())
        return false;

    if (!NoteLosPullStart(bot, target))  // [los_pull_plan_loop]
    {
        CarefulPlanVerdict::NoteAborted(bot, target->GetGUID());
        return false;
    }

    PositionMap& posMap = AI_VALUE(PositionMap&, "position");
    PositionInfo pullPosition = posMap["pull"];
    pullPosition.Set(_losSpot.GetPositionX(), _losSpot.GetPositionY(), _losSpot.GetPositionZ(), bot->GetMapId());
    posMap["pull"] = pullPosition;

    LosPull::Register(bot, target, _losPack, _losSpot, _losGather);
    NoteLosCorner(bot, _losSpot, _losGather);  // [b40_mgt] the corner the next pack of the hall may come round too
    PublishCastSpot(target);
    strategy->RequestPull(target);
    bot->SetSelection(target->GetGUID());
    context->GetValue<Unit*>("current target")->Set(target);
    botAI->ChangeEngine(BOT_STATE_COMBAT);
    LOG_DEBUG("playerbots", "[los pull] {} starts on {}: {} mobs, group to ({:.1f} {:.1f} {:.1f})", bot->GetName(),
              target->GetName(), _losPack.size(), _losGather.GetPositionX(), _losGather.GetPositionY(),
              _losGather.GetPositionZ());
    return true;
}

bool DropTargetAction::Execute(Event /*event*/)
{
    Unit* target = context->GetValue<Unit*>("current target")->Get();
    if (target && target->isDead())
    {
        ObjectGuid guid = target->GetGUID();
        if (guid)
            context->GetValue<LootObjectStack*>("available loot")->Get()->Add(guid);
    }

    // ObjectGuid pullTarget = context->GetValue<ObjectGuid>("pull target")->Get();
    // GuidVector possible = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();

    // if (pullTarget && find(possible.begin(), possible.end(), pullTarget) == possible.end())
    // {
    //     context->GetValue<ObjectGuid>("pull target")->Set(ObjectGuid::Empty);
    // }

    context->GetValue<Unit*>("current target")->Set(nullptr);

    // [healer_heals_first] A healer with healing to do drops the dead or invalid enemy without leaving the combat
    // engine and without spending the tick: the action reports "not done", so the engine goes on to the heal
    // triggers below it in the same tick (wipe analysis w24-25: "drop target" at 99 took 15.3 % of the healer's
    // ticks with the tank under 50 %). The heals pick their own target; the healer needs no enemy to heal.
    // [healer_audit_b30] The same while the group is under attack: PlayerbotAI::DoNextAction would only switch the
    // healer back to the combat engine on its next tick.
    if (DpsAssistAction::HealerHasHealingWork(botAI) || DpsAssistAction::HealerGroupUnderAttack(botAI))
    {
        bot->SetTarget(ObjectGuid::Empty);
        bot->AttackStop();  // a wand or melee swing on a polymorphed mob would break the sheep
        return false;
    }

    bot->SetTarget(ObjectGuid::Empty);
    bot->SetSelection(ObjectGuid());
    botAI->ChangeEngine(BOT_STATE_NON_COMBAT);
    if (bot->getClass() == CLASS_HUNTER) // Check for Hunter Class
    {
        Spell const* spell = bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL); // Get the current spell being cast by the bot
        if (spell && spell->m_spellInfo->Id == 75) //Check spell is not nullptr before accessing m_spellInfo
            bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL); // Interrupt Auto Shot
    }
    bot->AttackStop();

    // if (Pet* pet = bot->GetPet())
    // {
    //     if (CreatureAI* creatureAI = ((Creature*)pet)->AI())
    //     {
    //         pet->SetReactState(REACT_PASSIVE);
    //         pet->GetCharmInfo()->SetCommandState(COMMAND_FOLLOW);
    //         pet->GetCharmInfo()->SetIsCommandFollow(true);
    //         pet->AttackStop();
    //         pet->GetCharmInfo()->IsReturning();
    //         pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
    //     }
    // }

    return true;
}

bool TargetRequesterAction::Execute(Event event)
{
    Player* requester = event.getOwner();

    if (!requester)
        return false;

    bot->SetSelection(requester->GetGUID());

    botAI->TellMasterNoFacing(
        PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "target_requester_success",
            "Targeting %target",
            {{"%target", requester->GetName()}}));

    return true;
}

bool AttackAnythingAction::Execute(Event event)
{
    bool result = AttackAction::Execute(event);
    if (result)
    {
        if (Unit* grindTarget = GetTarget())
        {
            context->GetValue<ObjectGuid>("pull target")->Set(grindTarget->GetGUID());
            bot->GetMotionMaster()->Clear();
            // bot->StopMoving();
        }
    }

    return result;
}

bool AttackAnythingAction::isUseful()
{
    if (!bot || !botAI)  // Prevents invalid accesses
        return false;

    if (!botAI->AllowActivity(GRIND_ACTIVITY))  // Bot cannot be active
        return false;

    if (botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
        return false;

    if (bot->IsInCombat())
        return false;

    Unit* target = GetTarget();
    if (!target || !target->IsInWorld())  // Checks if the target is valid and in the world
        return false;

    std::string const name = std::string(target->GetName());
    if (!name.empty() &&
        (name.find("Dummy") != std::string::npos ||
         name.find("Charge Target") != std::string::npos ||
         name.find("Melee Target") != std::string::npos ||
         name.find("Ranged Target") != std::string::npos))
    {
        return false;
    }

    return true;
}

bool DpsAssistAction::HealerHasHealingWork(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!bot->IsInCombat() || !PlayerbotAI::IsHeal(bot))
        return false;

    // Out of mana a healer has nothing to heal with; its wand is better than standing still.
    constexpr float NO_MANA_PCT = 5.0f;
    if (bot->getPowerType() == POWER_MANA && bot->GetPowerPct(POWER_MANA) < NO_MANA_PCT)
        return false;

    // "party member to heal" is cached per tick and already ranks heals in flight; a member it returns above
    // "almost full health" is topping off, not healing work, and the healer may help kill.
    Unit* toHeal = botAI->GetAiObjectContext()->GetValue<Unit*>("party member to heal")->Get();
    if (toHeal && toHeal->IsAlive() && toHeal->GetHealthPct() < sPlayerbotAIConfig.AlmostFullHealth)
        return true;

    // [heal_triage b29] A member low in Ice Block (immune to heals) is healing work about to start: HoR escape
    // 16:58:37, the druid turned to "dps assist" with the Ice Blocked mage at 26 %.
    return HealTriage::HasImmuneMemberToHeal(botAI);
}

// [healer_audit_b30] See the header. A member only counts while something actually fights him: an idle boss held as
// the tank's target out of combat ([reach_idle_release]) must not keep the healer off the non-combat engine, which
// drinks and buffs.
bool DpsAssistAction::HealerGroupUnderAttack(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Group* group = bot->GetGroup();
    if (!group || !bot->IsAlive() || !botAI->IsHeal(bot))
        return false;

    float const reach = sPlayerbotAIConfig.HealDistance * 2.0f;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || !member->IsInMap(bot) || !member->IsInCombat())
            continue;

        if (member != bot && !bot->IsWithinDist(member, reach))
            continue;

        if (!member->getAttackers().empty() || member->GetVictim())
            return true;
    }

    return false;
}

bool DpsAssistAction::isUseful()
{
    if (PlayerHasFlag::IsCapturingFlag(bot))
        return false;

    // [healer_heals_first] "dps assist" (50) sits above every heal trigger; while the "dps target" changes the
    // healer re-targeted all tick and its heals were "not evaluated" (scholo-n w24 715 s: swiftmend and
    // rejuvenation at 22). A topped-up group still lets the healer help with damage.
    if (HealerHasHealingWork(botAI))
        return false;

    return true;
}

bool AttackRtiTargetAction::Execute(Event /*event*/)
{
    Unit* rtiTarget = AI_VALUE(Unit*, "rti target");

    // Fallback: if the "rti target" value did not resolve a valid unit yet,
    // try to resolve the raid icon directly from the group.
    if (!rtiTarget)
    {
        if (Group* group = bot->GetGroup())
        {
            std::string const rti = AI_VALUE(std::string, "rti");
            int32 const index = RtiTargetValue::GetRtiIndex(rti);
            if (index >= 0)
            {
                ObjectGuid const guid = group->GetTargetIcon(index);
                if (!guid.IsEmpty())
                    rtiTarget = botAI->GetUnit(guid);
            }
        }
    }

    if (rtiTarget && rtiTarget->IsInWorld() && rtiTarget->GetMapId() == bot->GetMapId())
    {
        botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({rtiTarget->GetGUID()});
        bool result = Attack(botAI->GetUnit(rtiTarget->GetGUID()));
        if (result)
        {
            context->GetValue<ObjectGuid>("pull target")->Set(rtiTarget->GetGUID());
            return true;
        }
    }
    else
        botAI->TellError("I dont see my rti attack target");

    return false;
}

bool AttackRtiTargetAction::isUseful()
{
    if (botAI->ContainsStrategy(STRATEGY_TYPE_HEAL))
        return false;

    return true;
}

void AggressiveTargetAction::LoadDecision(PullPlanner::Decision const& decision)
{
    _approach = decision.approach;
    _planTarget = decision.planTarget;
    _planPullBack = decision.pullBack;
    _planLos = decision.los;
    _planSingle = decision.single;
    _planCountsSap = decision.countsSap;
    _planPatrol = decision.patrolFirst;
    _planMobs = decision.mobs;
    _planCost = decision.cost;
    _planDrop = decision.drop;
    _planReason = decision.reason;
    _planSide = decision.side;
    _sideSpot = decision.sideSpot;
    _sideFor = decision.sideFor;
    _sideMs = decision.sideMs;
    _sideNote = decision.sideNote;
    _planKillFor = decision.killFor;
    _planKill = decision.kill;
    _planFightFor = decision.fightFor;
    _planFight = decision.fight;
    _planFightBack = decision.fightBack;
    _planFightNote = decision.fightNote;
    _losSpot = decision.losSpot;
    _losGather = decision.losGather;
    _losPack = decision.losPack;
    _leaderAttack = decision.attack;  // [lead3]
    _leaderMethod = decision.attack.IsEmpty() ? Leader::PullMethod::Unknown : decision.method;
    if (decision.hasCast && !decision.planTarget.IsEmpty())
        _castSpots[decision.planTarget] = {decision.castSpot, getMSTime()};
}

// LEADER_PLANNER_IN_LEADER, the tank: it asks, and executes the leader's fresh Go decision; nothing else.
bool AggressiveTargetAction::TakeLeaderDecision()
{
    _fromLeader = false;
    _planTarget.Clear();
    _approach.Clear();
    PullPlanner::Ask(bot);  // the engine looked at the tank's pull: the leader looks (in combat too: its stand clock)
    if (bot->IsInCombat())
        return false;

    PullPlanner::Decision decision;
    // [lead4c] a peek: the decision is consumed by the Execute that runs it (a skipped tick keeps it)
    if (!PullPlanner::TakeIssued(bot->GetGUID(), Leader::LEADER_ISSUE_FRESH_MS, decision, false) ||
        decision.verdict != PullPlanner::Verdict::Go)
    {
        // [lead31b] the order it runs is still open and not begun (Issued): the executor goes on with it - its next
        // step (a sight walk done: the approach, the tag) - with no new issue
        Leader::PullOrder order;
        if (!_hasActive || !Leader::Orders::GetOpen(bot->GetGUID(), order) ||
            order.state != Leader::OrderState::Issued || bot->isMoving())
            return false;
        ObjectGuid const activeMob = !_activeDecision.planTarget.IsEmpty() ? _activeDecision.planTarget
                                     : !_activeDecision.attack.IsEmpty()   ? _activeDecision.attack
                                                                           : _activeDecision.lookTarget;
        if (order.target != activeMob)
            return false;
        LoadDecision(_activeDecision);
        _fromLeader = true;
        _fromLeaderMs = 0;  // not the slot's: nothing to consume
        return true;
    }

    // An attack without a careful plan (AttackAction::Execute) attacks the aggressive target: only the leader's one.
    // [lead3] [lead5] a leader-issued attack (a boss pull, the leader's own pick) names its mob: no aggressive target
    // match needed, Execute attacks that mob
    if (decision.planTarget.IsEmpty() && decision.approach.IsEmpty() && decision.attack.IsEmpty())
    {
        // [lead27] the leader's look target is the order's mob: no match with the tank's own aggressive target (its
        // 40 yd range left it empty and the issued engage was never taken)
        if (Leader::LEADER_ONE_GATHER_OWNER && !decision.lookTarget.IsEmpty())
            decision.attack = decision.lookTarget;
        else
        {
            Unit* current = AI_VALUE(Unit*, "aggressive target");
            if (!current || current->GetGUID() != decision.lookTarget)
                return false;
        }
    }

    LoadDecision(decision);
    _fromLeader = true;
    _fromLeaderMs = decision.ms;  // [lead4c]
    _activeDecision = decision;  // [lead31b] carried on while its order stays open
    _hasActive = true;
    return true;
}
