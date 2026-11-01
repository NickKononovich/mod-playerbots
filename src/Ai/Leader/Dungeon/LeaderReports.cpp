/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "LeaderReports.h"

#include "AggressiveTargetValue.h"
#include "AiObjectContext.h"
#include "AKTriggers.h"
#include "ANAddTactics.h"
#include "Bag.h"
#include "BotMoveEvents.h"
#include "CellImpl.h"
#include "Chat.h"
#include "CoSTriggers.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "DBCStores.h"
#include "Engine.h"
#include "EscortProtect.h"  // [b36_ohb] EscortGossipHeld
#include "EstimatedLifetimeValue.h"
#include "GameObject.h"
#include "GameObjectModel.h"
#include "GameTime.h"
#include "GDTriggers.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "HealthTriggers.h"
#include "RaidLeaderState.h"
#include "HoRTriggers.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MechShared.h"
#include "MgTShared.h"
#include "ClassicShared.h"  // [b32_smpull] Mograine's linked chapel trash
#include "MotionMaster.h"
#include "OCTriggers.h"
#include "MoveSpline.h"
#include "AttackerCountValues.h"
#include "MovementActions.h"
#include "LeaderOrders.h"  // [lead1] the tank's pull orders
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "LeaderKnowledge.h"
#include "RouteSearch.h"  // [lead99] the leader's route search
#include "PullFacts.h"    // [lead104] the leader's pull / fight facts
#include "LeaderFightChecks.h"  // [lead107] the leader's checks
#include "LeaderPlan.h"         // [lead107] the leader's planner tick
#include "Pet.h"  // [lead38] the pet redirect
#include <source_location>  // [lead76]
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSpellRepository.h"
#include "Playerbots.h"
#include "PullStrategy.h"
#include "PullActions.h"  // [b39_strat] FightHold: the opening fight held on the use side of a key gate
#include "ChooseTargetActions.h"  // [b47_door] PullReadiness::GroupPreparing
#include "SmartAI.h"
#include "SmartScriptMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StuckWatchdog.h"
#include "DamageTaken.h"
#include "DungeonHazard.h"   // [lead16] the leader's hazard view
#include "LeaderActions.h"   // [lead19] the member guard model (LeaderMoveOrderAction::WalkRefusal)
#include "RtiValue.h"        // [lead16] the leader's CC assignments
#include "DungeonTactics.h"  // [lead15] the leader's cleave dodge (InFrontalCleave)
#include "InstanceScript.h"
#include "InstanceSaveMgr.h"
#include "SelfTravelActions.h"
#include "TemporarySummon.h"
#include "TerrainHazard.h"
#include "TestRunnerStrategy.h"
#include "Transport.h"
#include "UKTriggers.h"
#include "UPTriggers.h"
#include "Timer.h"
#include <chrono>  // [lead15] the leader cadence cost
#include <functional>
#include <optional>  // [lead18]
#include "WaypointMgr.h"
#include "WorldPacket.h"
#include "World.h"
#include "WorldSession.h"
#include "G3D/Vector4.h"  // [b32_brs] HopelessDetourLeg discs
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Leader::Dungeon
{
    using namespace Leader::Fight;

    // What holds the tank's pull, for the pull_hold / pull_hold_stall lines.
    std::string TankPullState(Player* tank)
    {
        PlayerbotAI* botAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        if (!botAI)
            return "no ai";
        std::vector<std::string> parts;
        if (CarefulPlanVerdict::RecentlyAborted(tank->GetGUID(), PULL_ABORT_HOLD_MS))
            parts.push_back("recently aborted");
        if (PullStrategy const* pull = PullStrategy::Get(botAI))
        {
            if (pull->IsPullPendingToStart())
                parts.push_back("pending to start");
            if (pull->HasPullStarted())
                parts.push_back(Acore::StringFormat("started {} s ago",
                                                    int64(time(nullptr) - pull->GetPullStartTime())));
            if (Unit* target = pull->GetTarget())
                parts.push_back(Acore::StringFormat("target {} {:.1f} yd", target->GetName(),
                                                    tank->GetExactDist(target)));
        }
        LosPull::Plan plan;
        if (LosPull::Get(tank, plan) && plan.tank == tank->GetGUID() && !plan.released)
            parts.push_back(Acore::StringFormat("los pull plan {} ({} s, gathered {}, cast {})", plan.id,
                                                getMSTimeDiff(plan.plannedMs, getMSTime()) / IN_MILLISECONDS,
                                                plan.gathered, plan.castMs != 0));
        std::string text;
        for (std::string const& part : parts)
            text += (text.empty() ? "" : ", ") + part;
        return text.empty() ? std::string("none") : text;
    }

    // [lead1] C:/WoW/LEADER_REFACTOR_PLAN.md step 1: the tank's order reports become decision lines ("tank:<name>"),
    // the leader writes the states it observes itself ("leader"): the order's fight is on (Fighting), the fight is
    // over (Done: no engaged mob), the tank is gone (Aborted); and one pull_start line per pull naming its owner - the
    // tank's open order, the leader's body pull (group.BodyPull()), or "none". [lead2] Step 2: order_orphan.
    void DrainOrderReports(LeaderGroup& group, Player* leader)
    {
        if (!leader || group.LeaderGuid().IsEmpty())
            return;

        DecisionRun const run = group.Run();
        std::string const tankWho = "tank:" + leader->GetName();
        Leader::OrderReport report;
        while (Leader::Orders::TakeReport(group.LeaderGuid(), report))
        {
            JsonObject fields;
            fields.Num("order", report.orderId).Str("state", Leader::StateName(report.state))
                .Str("method", Leader::MethodName(report.method)).Str("why", report.why);
            if (report.mobs)
                fields.Num("expected", report.mobs).Real("cost", report.cost);
            fields.Real("x", report.at.GetPositionX()).Real("y", report.at.GetPositionY())
                .Real("z", report.at.GetPositionZ());
            Decisions().Write(run, tankWho, report.what, report.target, report.targetName, report.decides,
                                          fields, report.ms);
            // [lead69] b114 376-427 s: 13 commit_failed "the tank's walk in for sight refused" with the order left
            // Issued. The tank's refusal is the walk verdict: the mob is unreachable from here (lead.unreachable,
            // logged once), the order ends and the commitment with it, the route moves on
            if (report.what == "commit_failed" && !report.target.IsEmpty() &&
                !group.State().unreachable.count(report.target))
            {
                Creature* refusedMob = leader->GetMap()->GetCreature(report.target);
                group.State().unreachable[report.target] = {leader->GetPosition(),
                                                         refusedMob ? refusedMob->GetPosition() : leader->GetPosition(),
                                                         "the tank's walk refused: " + report.why};
                Leader::Orders::Close(group.LeaderGuid(), Leader::OrderState::Aborted,
                                      "the tank's walk refused: " + report.why, leader->GetPosition(), nullptr);
                // [lead109] b142 rfc-h 381.4-383 s: the leader had committed and issued the same mob again in the
                // tick before this report came; the tank opened that order (19) and sat in it for 70 s. An issued
                // decision for the refused mob is withdrawn with the order
                PullPlanner::Decision waiting;
                if (PullPlanner::TakeIssued(group.LeaderGuid(), 0, waiting, false) &&
                    (waiting.planTarget == report.target || waiting.attack == report.target ||
                     waiting.lookTarget == report.target))
                    PullPlanner::CancelIssued(group.LeaderGuid());
                Decisions().Write(run, "leader", "pick_unreachable", report.target, report.targetName, true,
                                              JsonObject().Str("why", report.why).Str("pick", "the tank's walk"));
            }
            // [lead57] one owner per decision: the tank decided a pull (which mob / the method / when) itself - the
            // leader's domain (should be 0)
            if (report.decides)
                Decisions().Write(run, "leader", "domain_violation", report.target, report.targetName,
                                              false,
                                              JsonObject().Str("owner", "leader").Str("by", "tank")
                                                  .Str("what", report.what).Str("why", report.why),
                                              report.ms);
        }

        // [lead15] the members' acks of the leader's events (executions, decides false)
        for (LeaderGroup::MemberInfo const& member : group.Members())
        {
            Leader::MemberAck ack;
            while (Leader::Events::TakeAck(member.guid, ack))
            {
                Decisions().Write(run, "member:" + member.name, "member_ack", member.guid, member.name,
                                              false,
                                              JsonObject().Str("event", ack.what).Str("why", ack.why)
                                                  .Num("seq", ack.seq),
                                              ack.ms);
                // [lead18] a refused leader walk: the leader issued an invalid move
                // [lead29] member_refusal: the member's guard and the leader's model disagree (a blocker, goal 0)
                if (ack.why.rfind("member_refusal", 0) == 0)
                {
                    Decisions().CountConflict(run, "member_refusal");
                    ++group.State().memberRefusals[member.name];
                    if (ack.why.find("to range of the engage") != std::string::npos)
                        group.State().engageRefused.insert(member.guid);
                }
                // [lead56] the tank's pick-up cast went (or was tried): the one-shot threat command ends
                if (ack.what == "threat")
                {
                    Leader::MemberEvents threatEvents;
                    if (Leader::Events::Get(member.guid, threatEvents) && !threatEvents.threatTarget.IsEmpty())
                        group.State().threatDoneFor[threatEvents.threatTarget] = threatEvents.threatSpell;
                }
                // [lead42] a refused Attack: the target the member could not attack (the leader picks another)
                if (ack.why.rfind("the attack refused", 0) == 0 &&
                    ack.why.find("already attacking") == std::string::npos)
                {
                    Leader::MemberEvents refusedEvents;
                    if (Leader::Events::Get(member.guid, refusedEvents) && !refusedEvents.target.IsEmpty())
                        group.State().attackRefused[refusedEvents.target].insert(member.guid);
                }
                if (ack.why.rfind("the walk refused", 0) == 0)
                {
                    Decisions().Write(run, "leader", "invalid_spot", member.guid, member.name, false,
                                                  JsonObject().Str("member", member.name).Str("why", ack.why),
                                                  ack.ms);
                    // [lead29] a refused range walk is not repeated: the member follows the tank instead
                    if (ack.why.find("to range of the engage") != std::string::npos)
                        group.State().engageRefused.insert(member.guid);
                }
            }
            // [lead18] vetoed_eval: a vetoed strategy evaluated by the member (a blocker, goal 0)
            std::string vetoed;
            uint32 vetoedCount = 0;
            while (Leader::Events::TakeVetoedEval(member.guid, vetoed, vetoedCount))
            {
                group.State().vetoedEvals[member.name + "|" + vetoed] += vetoedCount;
                for (uint32 i = 0; i < vetoedCount; ++i)
                    Decisions().CountConflict(run, "vetoed_eval");
            }
            // [lead15] LEADER_OWN_DECISION_CHECK: the member's own decisions (the split's work list)
            Leader::OwnDecision own;
            while (Leader::Events::TakeOwn(member.guid, own))
            {
                if (own.kind == "blocked_move" || own.kind == "blocked_attack")
                {
                    // [lead17b] the whitelist blocked it: no decision (decides false), the step-6 work list
                    Decisions().Write(run, "member:" + member.name, own.kind, member.guid, member.name,
                                                  false,
                                                  JsonObject().Str("action", own.action).Str("dungeon", own.why)
                                                      .Str("content", group.ContentKey()),
                                                  own.ms);
                    ++group.State().blockedMoves[own.kind + "|" + own.action];
                    continue;
                }
                Decisions().Write(run, "member:" + member.name, "own_decision", member.guid, member.name,
                                              true,
                                              JsonObject().Str("action", own.action).Str("kind", own.kind)
                                                  .Str("why", own.why),
                                              own.ms);
                Decisions().CountConflict(run, "own_decision");
                ++group.State().ownDecisions[member.name][own.action];
            }
            if (uint32 const stale = Leader::Events::TakeStale(member.guid))
            {
                group.State().staleCommands[member.name] += stale;
                for (uint32 i = 0; i < stale; ++i)
                    Decisions().CountConflict(run, "stale_command");
            }
        }

        // [lead7] LEADER_PLANNER_DIFF: where the planner library's shadow look disagreed with the tank's own look
        PullPlanner::DiffLine diff;
        while (PullPlanner::TakeDiff(group.LeaderGuid(), diff))
        {
            Unit* mob = diff.mob.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*leader, diff.mob);
            Decisions().Write(run, "leader", "plan_diff", diff.mob, mob ? mob->GetName() : std::string(),
                                          false,
                                          JsonObject().Str("kinds", diff.kinds).Str("tank", diff.own)
                                              .Str("library", diff.library).Num("repeats", diff.repeats),
                                          diff.ms);
        }

        struct OrderWatch
        {
            uint32 pullSeq = 0;
            uint32 orderId = 0;
            uint32 changedMs = 0;
            uint32 idleSinceMs = 0;
            Position anchor;
        };
        static std::unordered_map<uint32, OrderWatch> watches;  // party seq -> watch (world thread)
        OrderWatch& watch = watches[group.Run().runSeq];

        Leader::PullOrder order;
        bool const open = Leader::Orders::GetOpen(group.LeaderGuid(), order);
        if (group.InCombat() && watch.pullSeq != group.PullSeq())
        {
            watch.pullSeq = group.PullSeq();
            JsonObject fields;
            fields.Num("pull", group.PullSeq()).Str("starter", group.PullStarter()).Num("order", open ? order.id : 0)
                .Bool("bodyPull", group.BodyPull());
            if (open)
                fields.Str("state", Leader::StateName(order.state)).Str("method", Leader::MethodName(order.method))
                    .Bool("orderTargetIsRouteTarget", order.target == group.RouteTarget());
            // [lead1] a start the dungeon's script made is "event", not "none" (Party::pullScript)
            if (!open && !group.BodyPull() && !group.PullScript().empty())
                fields.Str("script", group.PullScript());
            Decisions().Write(run,
                                          open ? tankWho
                                               : (group.BodyPull() ? "leader"
                                                                 : (group.PullScript().empty() ? "none" : "event")),
                                          "pull_start", open ? order.target : group.RouteTarget(),
                                          open ? order.targetName : "", false, fields);
        }
        // [lead8] the spot the group fought and cleared (the tank at each fight's end): a fight spot candidate of the
        // leader's planner (LeaderInputs::cleared)
        {
            static std::unordered_map<uint32, uint32> clearedEnds;  // party seq -> the last fight end seen
            uint32& seenEnd = clearedEnds[group.Run().runSeq];
            if (Leader::Leads(Leader::LEADER_FIGHT_SPOT_COUNT) && group.LastPullEndMs() &&
                seenEnd != group.LastPullEndMs() && !group.InCombat() && leader->IsAlive() && leader->IsInWorld())
            {
                constexpr size_t CLEARED_SPOTS_MAX = 32;
                constexpr float CLEARED_SPOT_APART = 8.0f;
                Position const here = leader->GetPosition();
                auto& cleared = group.State().cleared;
                if (std::none_of(cleared.begin(), cleared.end(),
                                 [&here](Position const& at) { return at.GetExactDist(here) < CLEARED_SPOT_APART; }))
                {
                    if (cleared.size() >= CLEARED_SPOTS_MAX)
                        cleared.erase(cleared.begin());
                    cleared.push_back(here);
                }
            }
            seenEnd = group.LastPullEndMs();
        }
        if (!open)
            return;

        uint32 const now = getMSTime();
        Position const here = leader->GetPosition();
        auto const closeOrder = [&](Leader::OrderState state, std::string const& why, std::string const& what)
        {
            Leader::PullOrder closed;
            if (!Leader::Orders::Close(group.LeaderGuid(), state, why, here, &closed))
                return;
            Decisions().Write(
                run, "leader", what, closed.target, closed.targetName, false,
                JsonObject().Num("order", closed.id).Str("state", Leader::StateName(closed.state))
                    .Str("method", Leader::MethodName(closed.method)).Str("why", why)
                    .Num("openSec", getMSTimeDiff(closed.openedMs, now) / IN_MILLISECONDS)
                    .Real("x", here.GetPositionX()).Real("y", here.GetPositionY()).Real("z", here.GetPositionZ()));
        };

        if (!leader->IsAlive() || !leader->IsInWorld())
        {
            closeOrder(Leader::OrderState::Aborted, "the tank is dead or gone", "order_close");
            return;
        }
        if (group.InCombat() || leader->IsInCombat())
        {
            if (Leader::Orders::Advance(group.LeaderGuid(), Leader::OrderState::Fighting, "the fight is on", here))
                Decisions().Write(run, "leader", "order_fighting", order.target, order.targetName, false,
                                              JsonObject().Num("order", order.id)
                                                  .Str("method", Leader::MethodName(order.method))
                                                  .Bool("tankInCombat", leader->IsInCombat()));
            watch.orderId = 0;
            return;
        }
        // [lead109] an open order on a mob the leader judged unreachable (the tank's walk refused it) while that
        // verdict's facts hold - the tank and the mob where they were judged - is closed: the tank does not sit in it
        if (order.state != Leader::OrderState::Fighting)
            if (auto const judged = group.State().unreachable.find(order.target); judged != group.State().unreachable.end())
                if (Creature* mob = leader->GetMap()->GetCreature(order.target);
                    mob && leader->GetExactDist(judged->second.tankAt) <= Leader::LEADER_COMMIT_MOVED_YD &&
                    mob->GetExactDist(judged->second.mobAt) <= Leader::LEADER_COMMIT_MOVED_YD)
                {
                    closeOrder(Leader::OrderState::Aborted, "the walk to it is " + judged->second.why, "order_close");
                    PullPlanner::Decision waiting;
                    if (PullPlanner::TakeIssued(group.LeaderGuid(), 0, waiting, false) &&
                        (waiting.planTarget == order.target || waiting.attack == order.target ||
                         waiting.lookTarget == order.target))
                        PullPlanner::CancelIssued(group.LeaderGuid());
                    watch.orderId = 0;
                    return;
                }
        if (order.state == Leader::OrderState::Fighting)
        {
            closeOrder(Leader::OrderState::Done, "no engaged mob: the fight is over", "order_done");
            // [lead101] the order's own mob alive and in no fight (another pack came instead): it stays the leader's
            // pick, the next pull goes for it from where the group stands now (released by the kept pick's facts)
            // [lead108] only one the tank has a walk to
            if (Creature* own = leader->GetMap()->GetCreature(order.target);
                Leader::Fight::OrderMobStaysPick(leader, own))
            {
                group.State().keptPick = order.target;
                group.State().keptPickWhy = "its order ended with it out of the fight";
                Decisions().Write(run, "leader", "order_mob_kept", order.target, order.targetName, true,
                                              JsonObject().Num("order", order.id)
                                                  .Real("distance", leader->GetExactDist(own)));
            }
            return;
        }

        // [lead2] order_orphan (C:/WoW/LEADER_REFACTOR_PLAN.md step 2 "Risk"): the order open, the tank out of combat
        // within ORDER_ORPHAN_RANGE of one spot, no state change and no group preparation (a sitter / buff hold the
        // tank waits for) for ORDER_ORPHAN_MS - a report was lost. A bug event about a broken invariant, not a
        // handover by clock: logged with the tank's pull state, closed, and the leader plans again.
        if (watch.orderId != order.id || watch.changedMs != order.changedMs || PullReadiness::GroupPreparing(leader) ||
            leader->GetExactDist(&watch.anchor) > Leader::ORDER_ORPHAN_RANGE)
        {
            watch.orderId = order.id;
            watch.changedMs = order.changedMs;
            watch.idleSinceMs = now ? now : 1;
            watch.anchor = here;
            return;
        }
        uint32 const idleMs = getMSTimeDiff(watch.idleSinceMs, now);
        if (idleMs < Leader::ORDER_ORPHAN_MS)
            return;

        std::string const pullState = TankPullState(leader);
        std::string const why =
            Acore::StringFormat("order_orphan: {} {} {} s without a state change ({}), tank idle {} s; pull: {}",
                                Leader::MethodName(order.method), order.targetName,
                                getMSTimeDiff(order.changedMs, now) / IN_MILLISECONDS, Leader::StateName(order.state),
                                idleMs / IN_MILLISECONDS, pullState);
        LOG_INFO("playerbots", "[lead2] {}: {} order {}: {}", group.Run().runId, leader->GetName(), order.id, why);
        group.LogEvent("order_orphan", JsonObject().Num("order", order.id).Str("name", order.targetName)
                                            .Num("entry", order.targetEntry)
                                            .Str("method", Leader::MethodName(order.method))
                                            .Str("state", Leader::StateName(order.state)).Str("origin", order.origin)
                                            .Num("openSec", getMSTimeDiff(order.openedMs, now) / IN_MILLISECONDS)
                                            .Num("idleSec", idleMs / IN_MILLISECONDS).Str("pull", pullState)
                                            .Real("x", here.GetPositionX()).Real("y", here.GetPositionY())
                                            .Real("z", here.GetPositionZ()));
        Decisions().NoteConflict(run, "order_orphan", order.target, why,
                                             JsonObject().Num("order", order.id));
        // [lead30] LEADER_NO_FORCING_TIMERS: no abort because time passed - the order stays, the stall is visible
        if (Leader::LEADER_NO_FORCING_TIMERS && group.LeaderDecides())
        {
            WaitedLong(group, "order_orphan", idleMs, why);
            watch.idleSinceMs = now ? now : 1;
            return;
        }
        closeOrder(Leader::OrderState::Aborted, why, "order_orphan");
        watch.orderId = 0;
    }
}
