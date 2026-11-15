/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_JOURNAL_ISSUEKEY_H
#define PLAYERBOTS_JOURNAL_ISSUEKEY_H

// [jrn41] THE DEDUPLICATION KEYS OF pb_journal_issue, AS A SEAM: the causal parts go in, the key comes out, and
// nothing here reads the world, the clock or the database. The emitters (JournalIdle.cpp, PlayerbotAI.cpp) gather
// the facts from the map as they always did and call these functions where they used to concatenate the key by hand.
//
// WHY A SEAM AND NOT A STRING BUILT ON THE SPOT: the key decides what counts as ONE problem, so a wrong key is
// measured in the problems table and nowhere else - and it has now been wrong twice in the same way. The key may
// therefore be asserted by a test over recorded inputs, with no server, and both directions have to be asserted:
// too narrow fans one cause into a staircase of rows, too wide merges two causes into one row and then a fix for
// the first one turns the test green while the second lives on under it.
//
// THE ENGINE'S ACTION TRACE IS AN INPUT TO THE ROW AND NEVER TO THE KEY. Engine::GetLastAction() returns the
// trace of what the engine pushed, ran and refused in that instant - "|A:create soulstone - PREREQ|A:create
// soulstone - FAILED|..." - and it changes on every sample of one standing episode. Measured on the live table:
// one bot (Kylantino, map 369, the same position to four decimals) standing from 10 s to 60 s wrote SIX samples
// that became FIVE issue rows, because the trace is in the key; and the project had already measured the same
// trap on the idle diagnostics, where keying on the whole line gave 757 "causes" over 3 900 rows and the 757 were
// a trailing list of action names. The trace stays in `why` and in the domain row (pb_journal_idle.doing,
// pb_journal_stuck.action_running), where it is read by eye in a row already found.

#include <cstdint>
#include <string>
#include <vector>  // [jrn47] the reasons of combat_spot_none come in as a list

namespace Journal::Key
{
    // `standing`: the key of the standing watch (JournalIdle.cpp). The cause as this detector can state it is the
    // bot's role in its five, whether it stood through a fight, and which state its AI was in - the same three
    // kinds of part the sibling kinds use (`idle|<role>|<buckets>`, `stuck_target|<role>|...`). `actionTrace` is
    // passed in so that the one place that must NOT use it is the one place that holds it.
    // [jrn42] `heldBy` is the cause field the comment below asked for and did not have: the name of the
    // legitimate stillness the episode stands under ("" when none, Journal::Standing::HoldName). It is in the
    // key because standing under the leader's own order is not the same problem as standing with no order at
    // all - measured on b502, 931 of 1089 out-of-combat standing rows inside Wailing Caverns fell in a window
    // in which the leader of that very run had recorded a `pull` verdict of `held` (run_id by identity, the
    // overlap by time), so without this field one leader hold was counted again as a problem of each member
    // standing under it.
    std::string Standing(std::string const& role, bool inCombat, uint32_t engineState, std::string const& heldBy,
                         std::string const& actionTrace);

    // `stuck_target`: the combat engine holds an idle mob as its target and nothing of it runs (PlayerbotAI.cpp).
    // The cause is the role plus whether the engine had anything at all to run - an empty trace is the point of
    // this problem - and never the trace's text.
    std::string StuckTarget(std::string const& role, std::string const& actionRunning);

    // `idle`: the diagnostics window of one bot (PlayerbotAI.cpp). The cause is the role plus WHICH buckets fired -
    // never how many times, the counts' spread is the value range - and, for the `other` bucket, which refusal the
    // engine gave, because `other` is not a cause but a hole in the list of causes.
    //
    // WHY THE REFUSAL BELONGS IN THE KEY, measured on b502: the key `idle|<role>|<buckets>` gave 109 keys over the
    // 3 665 windows whose `other` was non-zero, and those same windows hold 336 distinct combinations of the
    // refusal the engine named. The largest single key, `idle|dps|gcd+other` (306 windows, 19 bots), covered 14 of
    // them: an action turned down as USELESS by its own usefulness check and one turned down as IMPOSSIBLE are two
    // defects, and under one key a fix for the first one turns a test green while the second lives on beneath it.
    // That is the worse of the two directions this seam exists to assert, and `other` is where it had been left:
    // the refusal columns (ref_* in pb_journal_idle) have been filled since b502 and sum EXACTLY to `other`
    // (30 842 ticks against 30 842), so the breakdown is a fact of the same instant and not a second reading.
    //
    // WHICH refusals, never how many: the same rule as the buckets themselves. The counts stay in the row's own
    // columns and their spread in value_min / value_max.
    struct IdleBuckets
    {
        uint32_t noTarget{0};
        uint32_t rangeLos{0};
        // [jrn54] the PART of each of the two above in which the bot was WALKING. A part, never a replacement:
        // a window with no walking part keeps the key it had character for character, so the counts taken on
        // b501, b502 and b503 go on meaning what they meant.
        uint32_t noTargetMoving{0};
        uint32_t rangeLosMoving{0};
        uint32_t moving{0};
        uint32_t gcd{0};
        uint32_t threat{0};
        uint32_t leaderHold{0};
        uint32_t other{0};
        // the breakdown of `other`: the engine's own answer for a tick it produced nothing in (IDLE_REFUSAL_*)
        uint32_t refUnsaid{0};
        uint32_t refUseless{0};
        uint32_t refImpossible{0};
        uint32_t refPrereq{0};
        uint32_t refFailed{0};
        uint32_t refBlocked{0};
        uint32_t refNoCandidate{0};
    };

    std::string Idle(std::string const& role, IdleBuckets const& buckets, bool inDungeon);

    // `wait_behind`: a bot walks to meet a member its own follow does not bring within the group reward distance
    // (NewRpgAction.cpp). `sameZone` is the cause field, and it replaced one that could not differ: the emitter's
    // guard requires `member->GetMapId() == bot->GetMapId()`, so the old "same map" / "other map" field read
    // "same map" in all 43 live rows and in every row it could ever write. A statement that cannot come out false
    // is counted here as a defect of its own, and it left this kind with no cause field at all: 84 yd behind in
    // the same zone and 6 894 yd behind across the map were one row.
    std::string WaitBehind(bool sameZone);

    // [jrn47] `combat_spot_none`: a member of a five had NO place to fight its target from. By the project's rule
    // a place for a ranged member always exists and the tank's place is not a fallback (its cleave), so the cause
    // is the SET OF CHECKS THAT REFUSED the candidate places - "heal reach", "out of the healer's band", "an idle
    // mob's aggro reach", "no ground on its floor" - together with the member's role, because the checks that can
    // speak at all differ by role.
    //
    // NOT THE COUNTS, NOT THE STAGE, NOT THE MOB'S NAME, NOT THE TRACE, and each of those is measured on the 387
    // live events of b502: keying on the printed line gave 219 keys, on the reasons WITH their counts 154, on the
    // (stage, reason) pairs 77, on the mob's name beside the reasons 91 over 16 names - against 52 reason sets.
    // The counts belong in the child rows (pb_journal_spot_reject.candidates) and in the row's value range, the
    // stage in the child's own column, the mob in the event's entry and guid (a name is not an identity).
    //
    // `trace` is passed in FOR THE SAME REASON AS Standing()'s actionTrace above: the one place that must not use
    // it is the one place that holds it, and a test can then assert that it does not.
    std::string CombatSpotNone(std::string const& role, std::vector<std::string> const& reasons,
                               std::string const& trace);

    // [mount9] `mount_flap`: a bot got onto and off its mount repeatedly WITHOUT LEAVING ITS OWN RADIUS. The
    // cause this detector can honestly name is the JUSTIFICATION the repeated change stood under
    // (Journal::MountState::Justification - a closed set of facts, not a text that happens to differ), plus the
    // role, as both sibling kinds carry it, plus whether a walk was in play at all and whether the bot copies a
    // mount leader - because those three are three different judges of the same quantity and a fix for one must
    // not turn a test green over the other two:
    //   * no walk, justification `nothing` - the town case: a standing bot with no target was mounted by
    //     CheckMountStateAction because "no target" meant "mount", and whatever it was really doing (a craft, an
    //     enchant, a vendor) took the mount away again. Measured on b504: 915 standing windows over 23 bots,
    //     823 of them with in_group = 0, Emdyl 95.4 min at ONE point in Stormwind to four decimals.
    //   * a walk is on, justification `target in reach` - the walk case: CheckMountStateAction gets the bot off
    //     for a target inside the dismount band while MountForWalk puts it back on for the same walk.
    //   * `its leader rides` - the copied case (LeaderMountStateTrigger mirrors the leader's state).
    //
    // WHAT IS NOT IN THE KEY, and each part is measured rather than argued on b504's 4 212 mount casts:
    //   * THE COUNTS. The flips, mounts and dismounts of one episode climb with every sample, so they would make
    //     one episode a staircase of rows - this project's trap #2, and the counts' spread is what value_min /
    //     value_max are for.
    //   * THE MOUNT. Which beast it is is an identity of an item and no cause at all; 59 bots over their mounts
    //     would fan one cause out, and the spell and the name are columns of pb_journal_mount.
    //   * THE PLACE AND THE BOT. 23 bots over 7 zones flapped on b504 under three justifications; keying on the
    //     zone would have given at least 7 "problems" where there are 3, and on the bot 23. They are columns.
    //   * THE ENGINE'S TRACE, for the reason the whole of this file exists.
    std::string MountFlap(std::string const& role, bool hasWalk, bool hasLeader, std::string const& justification);

    // The first version of the key, kept because a test asserts the trap against it (seven rows for one
    // episode): called from MountEpisodeTest and from nowhere else. See IssueKey.cpp.
    std::string MountFlapNaive(std::string const& printedWhy);

    // [jrn46] `run_failed`: the facts of ONE finished run of a five, as the run's own record states them
    // (guild_project_runs). The key, the shape and the ranged value are built from this one struct so that the
    // three cannot disagree about the same run - the emitter used to concatenate the key by hand beside the
    // numbers it ranged over, which is how the value below came to be a number the key had already fixed.
    struct RunFacts
    {
        std::string contentKey;  // `wc-n`: the dungeon, as the five's content key names it
        std::string result;      // `done` / `failed` / `stalled` / `stopped`, the run's own word
        uint32_t bossesKilled{0};
        uint32_t bossesTotal{0};
        uint32_t deaths{0};
        uint32_t wipes{0};
        uint32_t durationSec{0};
    };

    // A run that went cleanly writes no problem at all: it finished, left no boss, cost no death and no wipe.
    bool RunClean(RunFacts const& facts);

    // The SHAPE of the failure - which of the four facts went wrong, with the numbers taken out - and the full
    // key `run_failed|<dungeon>|<result>|<shape>`. Keying on the reason text instead would fan one cause across
    // every run that hit it (this project's trap #2).
    std::string RunFailedShape(RunFacts const& facts);
    std::string RunFailed(RunFacts const& facts);

    // THE RANGED VALUE OF THE ROW, AND ITS UNIT. value_min / value_max are the spread of one quantity over the
    // occurrences the row merged, so the quantity has to be one the row's own key leaves FREE.
    //
    // WHY THIS IS A SEAM OF ITS OWN: the emitter ranged over the deaths, and the key carries whether there were
    // deaths at all. For every shape without the `deaths` part the deaths are 0 by construction - not merely
    // measured as 0, unable to be anything else - so the row's range said 0..0 in every row it could ever write.
    // Measured on b502: all three live `run_failed` rows (7 occurrences, among them the two wc-n runs that ended
    // at 3 of 8 bosses) carry value_min = value_max = 0 with unit `death`, while the bosses they left were 5, 5,
    // 1 and 8. That is the defect the sibling WaitBehind() above records in words: a statement that cannot come
    // out false. The deaths are not lost by this - a death has its own owner (pb_journal_death and the `death`
    // issue kind), and ranging over them here was a second judge of a quantity that already has one.
    //
    // The quantity is therefore the first fact the shape leaves open, in this order: the bosses the run left, the
    // deaths, the wipes, and - for a run that failed with every boss down and nobody dead - its seconds.
    struct RangedValue
    {
        double value{0.0};
        std::string unit;
    };

    RangedValue RunFailedValue(RunFacts const& facts);
}

// [jrn43] WHETHER THE ROW IS A PROBLEM AT ALL - the `is_real` column, as a seam, for the kinds whose answer is a
// closed set of names rather than a number. The precedent is Journal::Standing::HoldName: a row recognised as
// not-a-problem is MARKED and STAYS, keeping its duration and its range, and the count the user drives to zero
// (is_real IN ('yes','unknown')) stops counting it.
namespace Journal::Real
{
    // `stale_victim`: the leader held a target outside a fight and this code dropped it (LeaderCombatEnd.cpp).
    // THE REASON DECIDES, and three of the five reasons are the normal end of a fight rather than a defect: the
    // victim is dead, its guid resolves to nothing any more, or it is off the leader's map. The drop costs nothing
    // in those three - TickStaleVictim runs on the leader's own update and drops on the FIRST tick the facts
    // change, once per victim, with no clock anywhere in it, so nothing ever waited on the stale target.
    // Measured on b502: 178 rows `dead`, 3 `gone`, 1 `an idle mob no pull order is for` - 181 of 182 of them the
    // routine clearing of a mob the group had just killed (116 distinct guids, 6 leaders, 10 runs), counted as 172
    // occurrences of a problem.
    // THE TWO THAT STAY REAL: `evading` is a mob the group cannot reach (the navmesh / pathing defect this
    // project treats as high urgency), and `an idle mob no pull order is for` is somebody holding a target against
    // the leader's order, which is rule 4. An unknown reason stays real too - better to over-report than to miss.
    bool StaleVictim(std::string const& why);

    // The alert funnel (GuildCoordinator::Alert) raises four kinds, and one of them is about an event that already
    // has a row of its own with its full inputs: a death is written by the core's "killed by" hook into
    // pb_journal_death and into an issue row of kind `death`. Measured on b502: 9 deaths in pb_journal_death and 9
    // DEATH alerts, each 1-2 ms after its own death and in the same order - one event counted twice in the
    // problems table, which is this project's trap #1 at the level of the table itself. The alert row stays (it is
    // the funnel's own notice and carries the coordinator's view) and says `no` with the reason in words.
    bool AlertKindDuplicatesDomainRow(std::string const& alertKind);
}

#endif
