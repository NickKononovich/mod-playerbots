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
#include <map>
#include <string>
#include <utility>

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

    // [jrn56] WHICH TARGET THE CHAIN IS EVEN TALKING ABOUT - the question itself was wrong for a whole role.
    //
    // `hasLiveTarget` was read from one value, "current target", which is the bot's HOSTILE target. In a dungeon
    // the leader's plan gives the pull to the tank, so a healer has no hostile target at all, and the chain's
    // first question therefore answered every tick it did not act on. Measured over pb_journal_idle with the
    // corpus closed at at_ms <= 1794764479861, a healer in a dungeon in combat: 1 028 windows, 45 234 ticks,
    // 38 090 idle - and ALL 38 090 in `noTarget`, with `rangeLos`, `moving`, `gcd`, `threat`, `leaderHold` and
    // `other` exactly zero in every one of the 1 028 windows, over three builds. `idle|healer|noTarget` and
    // `idle|healer|noTarget+noTargetMoving` hold 1 021 of the ledger's 3 606 unexplained idle cases (28.3 %),
    // and neither names a cause: they name the only bucket such a healer can reach.
    //
    // A HEALER'S OWN TARGET IS THE ONE IT HEALS, and the code already has that value: "party member to heal",
    // cached per tick and ranking heals in flight. Asking it costs no new scan of the world. The hostile target
    // still WINS where it exists, because a healer grinding in the world is fighting that mob and the bucket must
    // keep saying so - outside a dungeon the same healers already reach every bucket (217 windows with
    // `rangeLos`, 560 with `moving`, 1 357 with `gcd`, 1 146 with `other`), which is how we know this is role x
    // place and not a property of healers.
    //
    // WHAT THIS DOES NOT DO: it does not judge whether there was healing WORK. Whether a member at full health is
    // worth a heal is already decided elsewhere (DpsAssistAction reads the same value against
    // AlmostFullHealth), and a second place deciding it here would be rule 5. The chain asks only "was there a
    // target of the kind this bot's job gives it"; WHY nothing came of it is the engine's own answer, and the
    // whole point is that such a tick now travels far enough down the chain to be asked.
    //
    // THE COST, NAMED BEFORE IT IS PAID: for a healer the MEANING of `no_target` changes. Up to b504 it meant
    // "no hostile target", which for a healer in a dungeon was true on every tick and said nothing; from b505 it
    // means "nothing of its own kind to act on". Counts taken on b502-b504 for healers therefore do not compare
    // with later ones - unlike [jrn52] and [jrn54], which added a PART and left the whole alone, this corrects a
    // question that was simply the wrong one, and a bucket that was never a measurement cannot be preserved.
    // Nothing changes by a single byte for a tank or a dps: their job target is the hostile one.
    struct TargetLook
    {
        bool heals{false};                   // this bot's job includes healing (the caller knows; the seam asks)
        bool hostileAlive{false};            // "current target" is there and alive
        bool hostileOutOfReachOrLos{false};  // ... and out of attack reach or out of sight
        bool healAlive{false};               // "party member to heal" is there and alive
        bool healOutOfReachOrLos{false};     // ... and out of a caster's reach or out of sight
    };

    // The target the chain is talking about, as the two facts the chain consumes.
    struct Target
    {
        bool live{false};
        bool outOfReachOrLos{false};
    };

    Target Looked(TargetLook const& look);

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

    // [jrn57] WAS THIS TICK DECIDED BEFORE THE ENGINE'S OWN REASON WAS EVER READ? The refusal and the name of the
    // action behind it are recorded on EVERY tick and counted only on a tick that reaches `Other`, so for these
    // the answer existed and was thrown away - which is `UnaskedTicks` seen one tick at a time instead of one
    // column at a time. The two MUST agree, and a test holds them to it (TheTwoViewsOfTheSameRuleAgree): this
    // one names Tick values, that one sums the matching columns, and a rule in two places can drift.
    //
    // THE COUNT GOES IN A COLUMN AND NOT IN THE KEY, and that is measured rather than asserted, by the rule this
    // project wrote for exactly this question: how often does the candidate flip INSIDE one episode, and what is
    // the denominator. Over consecutive windows of the same bot on the same map (gap <= 1.5 windows - a join by
    // TIME, marked as one), the refused action flips in 56 of 450 pairs (12.4 %) and its cause in 42 of 450
    // (9.3 %), against 0.7 % for the `in_group` bucket that did earn a place in a key and 26.4 % for the one
    // that did not. A grapheme that unstable would split one episode across several keys - the ladder [jrn41]
    // removed - so it stays a column, where the slice is a WHERE.
    bool DecidedBeforeTheEngineWasAsked(Tick tick);

    // [jrn58] THE BUSIEST (reason, action) PAIR OF A WINDOW - AND HOW MANY PAIRS THERE WERE AT ALL.
    //
    // The second number is the one that matters, and it is what caught the defect. `NoteIdleDiagRefusal` keeps
    // only the FIRST refusal of a tick, and the tank's `pull action` sits at relevance 105.0 against
    // ACTION_DEFAULT (~10) for the whole rest of its rotation, so it refuses first on EVERY tick and hides every
    // other refusal behind it. Measured over the closed b504 corpus (at_ms <= 1794764479861): a tank in combat
    // wrote 278 windows and they carry ONE pair between them - `useless` / `pull action` - 195 in a dungeon and
    // 83 in the world, while a dps in a dungeon shows 30 pairs over 324 windows and a healer in the world 12 over
    // 294. And `ref_top_ticks` equals `other` exactly for the tank (1 851 of 1 851 in a dungeon, 415 of 415 in
    // the world): 100 % of its unexplained ticks attributed to one pair, by construction. A value that cannot
    // come out otherwise is not a measurement (trap 5). The cost of believing it: "this is the leader's lawful
    // plan" explains 29 ticks of 2 266 (1.3 %), not the 100 % the single pair read like.
    //
    // WHY THE COUNT OF PAIRS AND NOT JUST THE BUSIEST ONE, decided by number rather than by taste: the tank's
    // `pull action` is refused on every idle tick, so it stays the busiest pair even after every refusal is
    // recorded - the busiest pair therefore CANNOT show whether the fix worked, while the count of pairs can and
    // is the readiness criterion itself ("more than one pair over the tank's windows on b505"). The magnitude
    // comes with it, because a busiest pair without its own count is a share with no denominator.
    //
    // ONE JUDGE OF "BUSIEST" (rule 5): this is called for both families, the first-refusal pairs and the
    // every-refusal pairs, so the two cannot drift. Strictly greater, so a tie keeps the first pair in the map's
    // own order rather than the last one seen - which is the same reason severity takes the worst and not the
    // latest. An empty map answers "nothing, 0 events, 0 pairs", and the caller writes NULL for that.
    struct Busiest
    {
        uint8_t cause{0};
        std::string action;
        uint32_t events{0};
        uint32_t pairs{0};  // how many distinct (reason, action) pairs the window saw at all
    };

    Busiest BusiestOf(std::map<std::pair<uint8_t, std::string>, uint32_t> const& counted);

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
