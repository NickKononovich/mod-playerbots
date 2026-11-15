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
        // [jrn52] of those two, the part in which the bot WAS WALKING. They are parts and not replacements: the
        // totals above keep the meaning they have had since [grp20], so a count taken on an earlier build still
        // means what it meant, and `noTarget - noTargetMoving` is the part in which the bot stood.
        uint32_t noTargetMoving{0};
        uint32_t rangeLosMoving{0};
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

    // [jrn52] And the ones in which the bot was WALKING. Deliberately NOT added to MechanicTicks: walking to the
    // next pack is the dungeon flow working, walking in circles is a defect, and a 15 s window cannot tell them
    // apart - whether the walk had a purpose is the route domain's question (pb_journal_route), not this one. So
    // the number is reported in words and the verdict stays "yes": calling a circle legitimate would be inventing.
    uint32_t TravelTicks(Counts const& counts);

    // [jrn55] AND THE TICKS THE CHAIN ANSWERED BEFORE THE ENGINE WAS EVER ASKED. The engine's own reason for
    // producing nothing (PlayerbotAI::NoteIdleDiagRefusal, the first refusal of the relevance-ordered queue, with
    // the name of the action it would have taken) is recorded on EVERY tick - Engine::DoNextAction runs before
    // UpdateIdleDiag and the fields are cleared only at the top of the next tick - but it is COUNTED only on a
    // tick that reaches `Other`. `noTarget`, `rangeLos` and `moving` are decided earlier, so on those ticks the
    // engine's answer is in hand and thrown away.
    //
    // THAT IS NOT A SMALL EDGE, IT IS A WHOLE ROLE. Measured over pb_journal_idle at at_ms <= 1794764400000, a
    // healer in a dungeon in combat: 1 028 windows, 45 234 ticks, 38 090 idle ticks - and all 38 090 of them in
    // `noTarget`, with `rangeLos`, `moving`, `gcd`, `threat`, `leaderHold` and `other` EXACTLY ZERO in every one
    // of the 1 028 windows, over three builds (b502 595, b503 310, b504 123). `ref_top_action` is NULL in
    // 1 028 of 1 028. The cause: a healer in a dungeon has no hostile "current target" - the leader's plan gives
    // the pull to the tank - so `!hasLiveTarget` is true on every tick it did not act, and the chain's FIRST
    // question answers the whole window. The key `idle|healer|noTarget` (923 cases, the table's largest) therefore
    // names the only bucket a healer in a dungeon can ever land in, which is not a cause.
    //
    // IT IS NOT THE ROLE ALONE, and the reverse direction is measured too: the same healers OUTSIDE a dungeon
    // reach every bucket (217 windows with rangeLos, 560 with moving, 1 357 with gcd, 1 146 with other), because
    // there they grind and do have a hostile target. So this is role x place, not a property of healers, and a
    // criterion written against the role would be wrong.
    //
    // THE VERDICT DELIBERATELY STAYS "yes" FOR SUCH A WINDOW. Unlike ContradictsItself, nothing here is
    // self-inconsistent: the bot really did idle those ticks (and really did work in them too - 1 272 casts and
    // 3 493 ticks with a cast in flight across the same windows, and only 87 of 1 028 recorded no activity at
    // all). Calling it "no" would hide a real idle behind an instrument complaint. What changes is the SENTENCE:
    // it stops implying the bucket is a cause and says the engine's own answer was never consulted.
    uint32_t UnaskedTicks(Counts const& counts);

    // Did the chain answer the WHOLE of this window's idle before the engine's own reason was ever read? Equality
    // against the window's own total, so there is no threshold here and nothing to tune.
    bool NamesNoCause(Counts const& counts);

    // [jrn52] WHICH COUNTER THIS TICK BELONGS TO - the chain itself, as a seam, because its ORDER was the defect.
    // `!target` and then "out of reach or sight" were asked BEFORE `isMoving()`, so the `moving` bucket could only
    // ever catch a tick whose target existed, was in reach AND in sight while the bot still moved: 6 002 ticks of
    // b503's 96 070 dungeon combat ticks (6.2 %), against 46 119 in `noTarget` and 14 851 in `rangeLos`. Walking
    // was therefore counted as "it had no target" or "it was out of range", and the largest of the build's ten
    // dungeon causes was not one cause but two under one name - travel and idleness - with travel the bigger
    // part: of the 548 noTarget windows, 373 (68.1 %) belong to a bot that covered 20 yd or more during them.
    //
    // THE WORLD READS STAY WITH THE CALLER, which is why this is two functions and not one. The chain asks about
    // the global cooldown only after the movement question and about the threat only after the leader's command,
    // and every one of those reads costs something per tick for every bot. `AskGcd` and `AskThreat` are the seam
    // saying "this is the next question, and it is yours to ask" - the order is asserted by a test all the same,
    // and nothing that used to be read lazily is now read eagerly.
    enum class Tick : uint8_t
    {
        CastWait,        // a cast already in flight
        Cast,            // the engine cast a spell this tick
        Action,          // the engine ran an action that was not a cast
        NoTargetMoving,  // no live target, and the bot was walking
        NoTargetStill,   // no live target, and the bot stood
        RangeLosMoving,  // a live target out of reach or sight, and the bot was walking towards it
        RangeLosStill,   // a live target out of reach or sight, and the bot stood
        Moving,          // the target is right there and in sight, and the bot still moves
        AskGcd,          // the caller's turn: is the global cooldown running?
        Gcd,
        LeaderHold,
        AskThreat,  // the caller's turn: is the bot's own threat at or over the hold?
        Threat,
        Other,  // the engine turned the tick's action down and this list has no name for the reason
    };

    // The facts of one tick that cost nothing to gather, in the order the chain asks them.
    struct TickFacts
    {
        bool casting{false};           // a cast is already in flight
        bool cast{false};              // the engine cast a spell this tick
        bool actionRan{false};         // the engine ran an action this tick
        bool hasLiveTarget{false};     // there is a target and it is alive
        bool outOfReachOrLos{false};   // ... but out of reach or out of sight
        bool moving{false};            // the bot was moving
    };

    // Returns the counter, or AskGcd when the global cooldown is the next question.
    Tick Classify(TickFacts const& facts);

    // The rest of the chain, once the caller has read the global cooldown it was asked for.
    Tick ClassifyAfterGcd(bool gcdRunning, bool leaderHold);

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
