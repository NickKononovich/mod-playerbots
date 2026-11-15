/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_JOURNAL_IDLEWINDOW_H
#define PLAYERBOTS_JOURNAL_IDLEWINDOW_H

// [jrn51] THE IDLE WINDOW'S VERDICT, AS A SEAM: the window's own counters go in as numbers, the answer "is this a
// problem, and why in words" comes out, and nothing here reads the world, the clock or the database.
// PlayerbotAI::UpdateIdleDiag counts the ticks from the map as it always did and calls these with what it counted.
//
// WHY THE CRITERION MOVED HERE, measured on b503's 522 unexplained idle keys (3 888 cases):
//
//   * THE WINDOW CAN CONTRADICT ITSELF, AND 829 OF THE 2 453 WORLD CASES (33.8 %) DID. A global-cooldown tick
//     exists only because a spell was cast, so a window that counts `gcd` while recording no cast, no executed
//     action and no cast in progress states two things that cannot both be true. Inside a dungeon that shape
//     appears in 0 of the build's 2 301 windows - a clean separation, not a tendency. The cause was an
//     instrument, not a bot: PlayerbotAI::NoteIdleDiagAction still carried the gate [jrn45] removed from the
//     window itself (`!bot->GetGroup() || !bot->GetMap()->Instanceable()`), so outside a dungeon NOTHING the bot
//     successfully did was ever recorded - 0 casts and 0 executed actions over 19 574 world windows, against
//     15 205 casts over 7 068 dungeon windows - while the engine's REFUSALS were recorded everywhere
//     (Engine::DoNextAction calls NoteIdleDiagRefusal unconditionally). Every productive tick of a bot in the
//     world therefore fell through the bucket chain into `gcd` or `other`, and the key named a cause that may not
//     have existed. The gate is gone; the contradiction stays as a verdict, because an instrument that can be
//     half-blind once can be half-blind again, and a row that says so is worth more than a row that insists.
//
//   * NOT EVERY IDLE TICK IS A DEFECT. A tick spent on the game's own global cooldown is the game's mechanic, and
//     a tick the leader's command zeroed is the leader's order, which by the project's rule nobody argues with.
//     Measured on the 1 435 dungeon cases: 13 614 of 64 112 idle ticks (21.2 %) were those two plus the threat
//     hold, and the old criterion ("at least one idle bucket is non-zero") called them problems all the same.
//
// THE COUNTS ONLY. No role, no place, no bot: those are columns of the row and parts of the key, and this seam
// deliberately cannot see them, so its answer cannot quietly become a second judge of where the bot was.

#include <cstdint>
#include <string>

namespace Journal::IdleWindow
{
    // One diagnostics window's tally, exactly the columns pb_journal_idle already carries. `ticks` is the whole
    // window; the rest are its parts, and the idle buckets sum to the ticks that produced nothing.
    struct Counts
    {
        uint32_t ticks{0};
        uint32_t casts{0};     // ticks in which the engine cast a spell
        uint32_t castWait{0};  // ticks spent waiting on a cast already in progress
        uint32_t nonCast{0};   // ticks in which the engine ran an action that was not a cast
        uint32_t noTarget{0};
        uint32_t rangeLos{0};
        uint32_t moving{0};
        uint32_t gcd{0};
        uint32_t threat{0};      // the bot's own threat on the target held it back
        uint32_t leaderHold{0};  // the leader's command zeroed its actions
        uint32_t other{0};
    };

    // The ticks that produced nothing: the buckets, which is what the `why` sentence and `value` already count.
    uint32_t IdleTicks(Counts const& counts);

    // Of those, the ones named by the game's own mechanic or by the leader's order - legitimate, not a defect.
    // The threat hold is in here with them: holding damage below the tank's threat is the project's own rule.
    uint32_t MechanicTicks(Counts const& counts);

    // Did this window record ANY activity at all? Three different things count as activity on purpose: a cast the
    // engine issued, an action it ran that was not a cast, and a cast already in flight. A window with none of the
    // three is either a bot that truly did nothing or an instrument that could not see - which is why the next
    // question exists and this one is not the verdict.
    bool ActivityRecorded(Counts const& counts);

    // The window's own numbers disagree: a global cooldown was running, so a spell HAD been cast, yet nothing was
    // recorded as done. Not "suspicious" - impossible, inside one instant's own tally.
    bool ContradictsItself(Counts const& counts);

    // "yes" / "no" for pb_journal_issue.is_real, by the criterion above. Never "unknown": every fact this verdict
    // rests on is in the window, so the emitter can answer, and leaving it open would be the blank the column's
    // third value is NOT for.
    char const* Real(Counts const& counts);

    // The verdict in words, for real_why - the whole sentence, numbers of this very window included.
    std::string RealWhy(Counts const& counts);

    // The criterion itself, for real_when: what was checked, not what was found.
    char const* RealWhen();
}  // namespace Journal::IdleWindow

#endif
