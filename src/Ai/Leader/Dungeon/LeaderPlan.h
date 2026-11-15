/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_LEADERPLAN_H
#define PLAYERBOTS_LEADER_LEADERPLAN_H

// [lead107] user: production code does not live in the test harness. The dungeon leader's planner tick (its pick,
// its inputs to the planner, the commitment and the order it issues, the pull spots, the readiness verdict, the
// whole-pack / boss-pull requests, planner_no_option), moved out of TestHarnessRunner.cpp unchanged; it reads the
// group through its view (LeaderGroup) and logs through the leader's sinks.

#include "LeaderGroup.h"
#include "LeaderLog.h"

#include "Optional.h"

#include <string>
#include <vector>

class Creature;
class Map;
class Player;
class Unit;

namespace PullPlanner
{
    struct Decision;  // ChooseTargetActions.h
}

namespace Leader::Dungeon
{
    // [lead128] user rule: no elapsed-time rule changes a bot decision. A careful refusal's skip therefore carries
    // NO deadline - the value stored in GroupRouteState::skipUntil is the skip's STAMP (which facts it was made on),
    // not a time to wait out, and every decision path reads the entry's presence (skipUntil.count), never its value.
    // What lifts the skip is the fact sweep in Retarget: the leader moved off where it judged
    // (UNREACHABLE_RECHECK_DISTANCE / _HEIGHT), the object moved off where it was judged (LEADER_COMMIT_MOVED_YD), a
    // door of the instance opened or closed, or the object is dead or gone ([lead132], the user's own rule: "a skip
    // lasts until a fact changes, not for its duration"). It was 3 minutes, and a five whose route target was
    // refused once stood looking busy for three minutes with nowhere to go.
    constexpr uint32 CAREFUL_REFUSED_SKIP_NO_DEADLINE = 0;

    // [lead7] the leader plans the tank's pull (PullPlanner::Decide on the world thread, every world update, when the
    // tank asked) and issues the decision; a hopeless refusal is the planner_no_option bug event.
    void TickLeaderPlanner(LeaderGroup& group);

    // [jrn50] THE STATE OF EVERY PARTICIPANT OF A DECISION, for the journal's participant rows (pb_journal_actor,
    // which had the columns and no values: not one bot row, and NULL for every mob's level, health and mana - so
    // "put the same conditions back and run the judge again" had nothing to put back).
    //
    // They are shared rather than private to LeaderPlan.cpp because three emitters write the same participants and
    // one shape of row is the point: the planner's answer, the issued order, and the chain pull's verdict.
    // Everything is read in the EMITTER'S OWN PASS, under the same lock as the rest of the decision; the journal
    // never looks a unit up, because that would be a second reading of the world at a second moment.
    //
    // `tankOpenerReady` is handed in by whoever holds that fact (JudgeChainPull's ChainPull::openerReady) and is
    // left empty by whoever does not: an empty one writes NO key, so the column stays NULL - "at that moment the
    // quantity did not exist", which is not "its opener was not ready". The planner's own answer runs out of
    // combat, where no opener gate has run, so it passes nothing.
    // `note` says why these participants are in the record, and it is what keeps two instants apart: one decision
    // is written from several lines (the answer, the order, a chain pull's verdict), the journal keeps one row per
    // distinct participant plus note, and merging a fight's reading into an out-of-combat answer's row would splice
    // two moments into one state. Pass nullptr to take the journal's default ("five" / "look target").
    std::string FiveFacts(LeaderGroup& group, Player* tank, Unit* look, Optional<bool> tankOpenerReady,
                          char const* note = nullptr);
    // One creature as the judge saw it: identity, place, level, health beside its pool, combat, wanderer.
    std::string MobFacts(Creature* mob, Player* tank, char const* note = nullptr);
    // [lead6] the leader's one readiness verdict ("" = ready): the group here and topped up, nobody buffing,
    // sitting or preparing (PullReadiness::GroupPreparing)
    std::string LeaderReadyWhy(LeaderGroup& group, Player* leader);
    // [lead3] the pack the leader must clear: asked of its planner whole (walk-in priced by the planner); the
    // leader holds where it stands meanwhile. `required`: no option for it is planner_no_option with a stop.
    void LeaderWholePack(LeaderGroup& group, Player* leader, Creature* pack, std::string const& why, bool walkIn,
                         bool required);
    // [lead3] a boss pull: a BossPull order at the tank's next ask (PullPlanner::BossPullDecision)
    void LeaderBossPull(LeaderGroup& group, Player* leader, Creature* boss, std::string const& why);
    // [lead3] user decision 1: planner_no_option, once per pack; `stops` ends the run (LEADER_STOP_ON_NO_OPTION)
    // true: reported now (a pack reported before is not again)
    bool LeaderNoOption(LeaderGroup& group, Player* leader, Unit* mob, std::vector<ObjectGuid> const& pack,
                        std::string const& why, bool stops, JsonObject fields);
    // [lead15] the spots of a pull (false: no gate for it) and what the pull still waits for ("" = everyone there)
    bool LeaderAssignSpots(LeaderGroup& group, Player* tank, PullPlanner::Decision const& decision);
    std::string LeaderSpotsWait(LeaderGroup& group, Player* tank);
    // [lead541] the boss a pull is for: its mob, or a boss the plan's pack counts; nullptr: a trash pull
    Creature* PullBoss(Map* map, Unit* mob, PullPlanner::Decision const& decision);
    // [lead24] a former forcing timer's diagnostic: `kind` waited `ms` (once per kind per 30 s)
    void WaitedLong(LeaderGroup& group, std::string const& kind, uint32 ms, std::string const& why);
    // [lead1390] the leader's invariants, every leader tick; each violation is one invariant_violation decision line
    // per state change (its kind, member and why): wait_without_order (the pull waits on a member that no order moves
    // or readies), order_impossible (a member's order the game refuses now, LeaderOrderCheck), credit_unguaranteed (a
    // mana line's tool or potion counted ready now whose use the game refuses now, PlayerbotAI::CheckCastNow)
    void CheckLeaderInvariants(LeaderGroup& group, Player* tank);
}

#endif
