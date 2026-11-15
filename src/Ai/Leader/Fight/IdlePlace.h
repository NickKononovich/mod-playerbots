/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_FIGHT_IDLEPLACE_H
#define PLAYERBOTS_LEADER_FIGHT_IDLEPLACE_H

// [spot90] WHETHER AN IDLE MOB'S AGGRO REACH FORBIDS A PLACE, AS A SEAM: the facts go in, the answer comes out,
// and nothing here reads the world, the clock or the database. Header-only, so the unit stand compiles it alone.
//
// THE RULE IS TWO EXISTING RULES READ TOGETHER, and it was already the code's: "do not step into a mob's reach
// without wanting it" forbids an entry WITHOUT the wanting, and when the leader has taken that mob into this
// fight the wanting exists and is expressed by that decision ([lead600]: a pack the leader decided into the
// fight blocks nobody's place or walk). What this file adds is that the rule now has ONE statement, which a test
// can assert in every combination of its facts - it lived as a condition inside IdleMobCovers and nowhere else.
//
// THE COMBINATION THAT MUST KEEP BLOCKING, and it is not a weakening of the rule but part of it ([lead1140],
// measured on b239: 49 of 92 "the tank never had it" happened in pulls with a joined pack, a member walking into
// it): a joined mob the TANK HAS STILL TO OPEN ON from where it stands is in combat with nobody yet, so a member
// other than the tank that steps into its reach wakes it BEFORE the tank's opener. It goes on blocking that
// member - and blocks the tank itself not at all, because the tank is the one who pulls it.
//
// MEASURED, which is why the seam exists (b502, one window): of 387 `combat_spot_none` events 257 (66 %) were a
// melee member whose `at target` stage refused all 16 places round the target as "an idle mob's aggro reach"
// (4 067 candidates), and in 252 of those 257 that was its ONLY refusal there - so those places pass every other
// check and exist the moment the blocker stops being a mob to keep away from. WHICH of the two cases those 257
// are - a mob nobody decided to fight, or a joined one the tank had still to open on - the record could not say,
// and pb_journal_spot.idle_taken / idle_tank_opens are the two columns that will.
//
// ONE JUDGE, AND IT IS NOT HERE. `takenIntoThisFight` and `tankStillToOpenOnIt` are READ from the leader's own
// records of those decisions (joinedPack / joinedHeld, written by judgeBlockingPack out of
// PullPlanner::JudgePack). This seam does not re-decide whether a pack joins, and no second place may. "It will
// probably be taken", "it is near" and "it would wake anyway" are not those records and are not these facts.
//
// NO CLOCK: every fact is a state, read at the instant the place is judged.

namespace Leader::Fight::IdlePlace
{
    struct Facts
    {
        // the guard's own model says this mob's reach covers the place (IdleMobCovers)
        bool coversThePlace = false;
        // the leader's record holds a decision that this very mob is taken into THIS fight (joinedPack)
        bool takenIntoThisFight = false;
        // of those: the tank opens on it from where it stands and has not got it yet ([lead1140] joinedHeld)
        bool tankStillToOpenOnIt = false;
        // the member whose place is judged IS the tank - the one that pulls the mob
        bool memberIsTheTank = false;
    };

    inline bool Blocks(Facts const& facts)
    {
        if (!facts.coversThePlace)
            return false;
        if (!facts.takenIntoThisFight)
            return true;
        return facts.tankStillToOpenOnIt && !facts.memberIsTheTank;
    }
}

#endif
