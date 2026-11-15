/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_JOURNAL_AGGROREASON_H
#define PLAYERBOTS_JOURNAL_AGGROREASON_H

// [jrn44] WHY A MOB IS ON A BOT, AS A SEAM: the facts of that instant go in, the reason code comes out, and nothing
// here reads the world, the clock or the database. The emitter (JournalFight.cpp, Journal::NoteAggro) gathers the
// facts from the map exactly as it did and calls this instead of deciding inline.
//
// WHY IT IS A SEAM NOW. The code was DEAD, and measured dead: 4 661 rows of pb_journal_aggro on b502, 4 661 of them
// `add`. Not one `came`, `path` or - the one the user asked the table to be built for - `estimate`. The cause was
// not a missing emitter but the ORDER OF THE BRANCHES: the first one read `bot->IsInCombat()`, and this hook runs
// from Creature::AtEngage, which the core reaches only AFTER it has put the two in combat. So `IsInCombat()` is true
// for the FIRST mob of every fight, and the first mob of a fight cannot be an addition by definition.
//
// THE PROOF IS IN THE DATA, not in reading the code: 2 621 of those 4 661 rows match a pb_journal_fight `start` row
// of the same bot against the same (entry, low) at the SAME at_ms - the first mob of the fight - and the fight row's
// own `mobs` count (the creatures holding the bot on their threat list at that instant) is 0 in ALL 2 621. The bot
// was in combat with nobody, and the row said a mob had joined a fight already running.
//
// So "already in combat" is a fact that must EXCLUDE THIS MOB: was the bot in combat with anybody BUT the one
// engaging it. That is `inCombatWithOthers` below, and it is the whole repair.
//
// WHAT A WRONG REASON COSTS, which is why this is asserted by a test rather than eyeballed: `WHERE reason =
// 'estimate'` answered "that never happens" when the truth was "it was never measured" - this project's trap #5
// (absence is not zero) inside the instrument itself.

namespace Journal::AggroReason
{
    // The facts of the instant Creature::AtEngage announces, and nothing else. Each one is held by the emitter at
    // that moment; none of them is read a second time anywhere.
    struct Facts
    {
        // the bot was in combat with at least one unit OTHER than the mob now engaging it. NOT IsInCombat(): see the
        // header comment - the core has already joined these two by the time this runs.
        bool inCombatWithOthers = false;
        // a pull order of this bot's five was open (Leader::Orders). Without one there is no plan to be wrong, so
        // `estimate` cannot be stated and is not guessed.
        bool pullOrderOpen = false;
        // the order named THIS bot as the one standing in front of the mob
        bool onThePlansTank = false;
        // the order was aimed at THIS mob (identity by guid, never by name)
        bool thePlansMob = false;
        // the bot was walking: what separates "hooked on the way" from "it came to me"
        bool moving = false;
    };

    // One of the codes of pb_journal_aggro.reason: "add", "estimate", "came" or "path". The returned pointer is a
    // literal and outlives any caller.
    //
    // `off_tank` is NOT returned here and cannot be: it is a change of GetVictim() between two ticks rather than an
    // engage, so no fact of this instant can state it. `unknown` is not returned either - every combination of the
    // five booleans above maps to one of the four codes, so the emitter never has to say "I could not tell". Both
    // are stated in the report rather than left to be discovered by a query that finds nothing.
    char const* Of(Facts const& facts);

    // Did the plan put this bot in front of this mob - the `expected` column, which is the SAME pair of facts the
    // reason uses, so it is answered in one place. Returns false when no order was open: the caller then writes
    // nothing at all, because "no order" is not "the plan was wrong", and a 0 in that column would say it was.
    bool Expected(Facts const& facts);
}

#endif
