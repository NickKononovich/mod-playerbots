/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MANA_LINE_H
#define PLAYERBOTS_MANA_LINE_H

// [pull84] THE MANA LINE A PULL WAITS AT, and whether it is a line anybody can ever stand at - a seam.
//
// THE RUN IT COMES FROM, in its own numbers. wc-n run 691 (five 6, leader Bierya, 2026-11-15): 0 deaths, 0 wipes,
// and `stalled 3/8` with five bosses "alive, not engaged" - the five did not die, it did not arrive. Its last 237
// seconds are 75 identical holds:
//
//   "Aestymro at 20...90 % mana (needs 100 % = 7.07 %/s (spell data) x 10 s + 41 reserve
//    (one more mob, 3.0 s + 17.7 heal floor (Holy Light's cost)) - 0 cooldowns;
//    heals 38.0 % per tank health x 0.25 cost)"
//
// Two numbers in that one line answer each other. `7.07 %/s x 10 s` = 70.7 % of the pool is the mana the healer
// would spend casting its best heal BACK TO BACK for the whole fight. `heals 38.0 % per tank health x 0.25 cost`
// = 9.5 % is the mana the fight's damage actually asks it to heal. A healer does not chain-cast; it heals the
// damage the fight deals, so the second number is the formula and the first is an upper bound of a different
// question - a factor of 7.4 between a model and the measurement standing side by side in the same sentence.
// [pull80] had already taken the nonstop term out of a healer's TRASH line and left it on the boss line, saying
// that was "a decision of its own". This is that decision, made: the witness formula is the line on a boss too,
// and the caution a boss deserves stays where it can be read - the explicit reserve beside it (one more mob's
// fight, plus [lead1400]'s floor of one whole cast of the heal), never as a multiplier nobody can audit.
//
// AND THE SECOND DEFECT, which is the one that cost the run. 70.7 + 41 = 111.7 % of a pool that holds 100. The
// clamp turned that into "needs 100 %" and the gate read "needs 100 %" as a line to WAIT at - so an impossible
// requirement became an endless wait. No drink, no regeneration and no standing still reaches 111 % of a pool;
// the healer's last Melon Juice went into chasing it, and once its bags were empty it stood on spirit regen at
// 0.33 %/s while the leader held every pull. A raw line above the pool is not a wait, it is a FACT ABOUT THE
// PLAN: the plan costs more mana than the member owns. So `Read` gives the clamped line AND says plainly that the
// line is not one to wait at, and the caller must not wait on it for a single second. What to do about the pack
// instead is the planner's decision, not this seam's - this seam only refuses to hide the overflow from it.
//
// WHAT IT IS NOT. No clock is read and no elapsed time decides anything (rule 2). No threshold is introduced
// (rule 3): every field of `Terms` is a quantity the caller already has from the game's own data - the member's
// rate from its spell data, its heal's mana per tank health from the same, the fight's seconds and cost from the
// judge that priced the pull, the mana its own cooldowns return. The only constants here are the size of the
// pool, which is 100 % by definition, and the half-percent of slack that keeps float arithmetic from calling
// 100.0000001 an overflow.
//
// Nothing here reads the world, the clock or the database. One judge of "what mana does this fight ask for, and
// can the member ever hold it": these two functions.

#include <algorithm>

namespace ManaLine
{
    // A pool holds 100 % of itself. This is not a tunable: it is the definition of the unit every term is in.
    constexpr float POOL_PCT = 100.0f;
    // float slack, so that a line that lands exactly on a full pool is a full pool and not an overflow
    constexpr float POOL_EPS_PCT = 0.5f;

    // Everything the line is built from, all of it in percent of the member's own maximum mana (or percent per
    // second), so that no term can be compared against a pool it is not measured in.
    struct Terms
    {
        bool healer = false;    // a healer's spend is its heals; everybody else's is its own rotation
        float fightSec = 0.0f;  // the judge's own estimate of how long this fight runs
        // the member's rate cast NONSTOP: what it would spend with no pause at all. For a healer this is an upper
        // bound of a different question (see the header note); for a tank's upkeep or a damage caster's casts it
        // is the formula, because their spend really is their rotation.
        float nonstopPctPerSec = 0.0f;
        float oneMobSec = 0.0f;  // one more mob's fight: the reserve's length, from the judge, not a constant
        // THE WITNESS FORMULA (healer): the mana its own heal costs per tank health healed, from its spell data.
        // 0 means it is not known yet (no heal read), which is NOT "it costs nothing".
        float healPerTankHealthPct = 0.0f;
        float cost = 0.0f;                // the tank health this plan costs, the unit the term above is per
        float upkeepPctPerSec = 0.0f;     // its heals-over-time upkeep while the fight runs
        float regenPctPerSec = 0.0f;      // the mana it gets back casting through the fight
        float healFloorPct = 0.0f;        // [lead1400] one whole cast of its heal: the boss caution, named apart
        float returnedPct = 0.0f;         // the mana its own ready cooldowns give back inside the fight
        float potionFloorPct = 0.0f;      // [lead502] the floor a credited potion puts under the line
        float cooldownFloorPct = 0.0f;    // [lead634] the floor a cooldown back inside the fight puts under it
    };

    struct Composed
    {
        float needPct = 0.0f;     // the fight's own spend
        float reservePct = 0.0f;  // what is held back on top of it, one more mob's fight and the heal floor
        bool witness = false;     // the need came from the witness formula rather than the nonstop bound
    };

    // The fight's own spend and the reserve held on top of it. Three steps and not one, because the caller needs
    // the spend and the reserve BEFORE it knows what its potion and its cooldowns floor the line at (it prices the
    // drink against them), and a quantity computed in two places is a quantity with two judges.
    Composed Of(Terms const& terms);

    // The line before any clamp: the spend and its reserve, less what the member's own cooldowns give back, never
    // under a floor that a credit only is a credit above. THIS is the number the clamp used to hide.
    float Raw(Composed const& composed, Terms const& terms);

    struct Verdict
    {
        float linePct = 0.0f;  // the line to compare the member's mana against, clamped into the pool
        // the raw line asked for more than the pool holds: the plan costs more mana than this member owns
        bool overPool = false;
        // ... and therefore this is NOT a line to wait at. Waiting is only ever right while the member can still
        // get there; above the pool it cannot, not in any number of seconds.
        bool isWait = true;
    };

    // `floorPct`: the low line that guards a role's estimate while nothing is measured (config LowMana); it never
    // raises a line over the pool, because a pool that cannot pay it is still a pool that holds 100.
    Verdict Read(float rawPct, float floorPct);
}

#endif
