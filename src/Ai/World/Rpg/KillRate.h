/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_KILL_RATE_H
#define PLAYERBOTS_KILL_RATE_H

// [dpszero1] THE BOT'S OWN DAMAGE AS A JUDGE MAY USE IT - three states where the code had one number, and a seam.
//
// THE DEFECT IT CLOSES, in the numbers of the b502 hour. Nine places read `DpsEstimate::Result::burstDps` through
// `std::max(own.burstDps, 1.0f)`. The floor was put there against a division by zero and it did two other things
// nobody asked for: it turned "nobody computed this" into "1 dps", and it turned a SMALL honest estimate into
// "1 dps" as well. 37 of the 1988 judge lines that carry a dps number sit on exactly 1.0, and not one line in the
// whole journal reads 0.1-0.9 or 1.1-4.3 dps: the next value above the floor is 4.4, which is the proof that the
// 37 are the floor and not a measurement.
//
// WHY THOSE 37 WERE SMALL, and it is no defect of the formula. Every one of them is a caster rotation (mage,
// warlock, shaman, balance druid - the warrior and the rogue of the same hour never reach the floor, their white
// swings carry them) against a mob ELEVEN OR MORE LEVELS ABOVE IT: 11, 12, 13, 14, 15, 18, 19 and once 42, never
// ten or less. Eleven is exactly where the core's own spell-hit table bottoms out:
// `WorldObject::MagicSpellHitResult` gives 94 - (levelDiff - 2) * Rate.MissChanceMultiplier.TargetCreature, the
// server runs that rate at its default 11, and at levelDiff 11 that is 94 - 99 = -5, clamped to the table's
// minimum of 1 %. One per cent of landed casts, times the 75 % resistance cap on top, is a real fraction of a dps -
// so the honest reading is BELOW the floor, the floor raised it, and the floor was therefore OPTIMISTIC: it made
// the fight look 25 times shorter and the damage taken 25 times smaller than the formula said. Nothing is "feared
// away" by it. What it did do is print "118135 dmg over 3048 s at 1.0 dps" - a line whose every number is a
// consequence of an invented one.
//
// SO THERE ARE THREE STATES AND NOT ONE NUMBER (rule 7, absence is not zero):
//   - NoEstimate   nobody ran the formula (no bot, it or its target out of the world). No verdict about a fight may
//                  rest on this: there is no number, and inventing one is what the floor did.
//   - DealsNothing the formula RAN and said zero. That is an answer about the fight, and the answer is that the
//                  fight never ends - said by returning an infinity, not by picking a small number instead.
//   - Deals        the formula ran and said something above zero, however small. A hundredth of a dps is used as a
//                  hundredth of a dps.
//
// WHAT IT IS NOT. No threshold and no new default (rule 3): the only constant here is zero, which is the boundary
// the three states are defined by and not a number to tune. No rule of elapsed time (rule 2): nothing reads a
// clock. One judge of "how long does the bot need to take this health down": `KillSec`.
//
// Nothing here reads the world, the clock or the database.

#include <cstdint>
#include <limits>

namespace KillRate
{
    // what the damage formula said about the bot's own damage, with the one bit `DpsEstimate::Result` used not to
    // carry: WHETHER IT RAN AT ALL
    struct Own
    {
        bool estimated = false;  // the formula ran. false: there is no estimate and `dps` below is untouched default
        float dps = 0.0f;        // what it said. 0 together with `estimated`: measured - the bot deals nothing
    };

    enum class State : uint8_t
    {
        NoEstimate,
        DealsNothing,
        Deals
    };

    inline State StateOf(Own const& own)
    {
        if (!own.estimated)
            return State::NoEstimate;
        return own.dps > 0.0f ? State::Deals : State::DealsNothing;
    }

    // may a verdict about a fight rest on this reading? Only a reading that exists: the caller of a `false` here
    // has no number and must not judge the fight at all, which is a different thing from judging it lost
    inline bool Judgeable(Own const& own) { return StateOf(own) != State::NoEstimate; }

    // the seconds the bot needs to take `health` down at its own damage. Infinite when it deals nothing (the fight
    // does not end) and infinite when there is no estimate - but an infinity read off NoEstimate is not an answer
    // about the fight, so a caller asks `Judgeable` first and does not reach this. Health at or below zero is
    // nothing left to kill: zero seconds, whatever the state.
    inline float KillSec(Own const& own, float health)
    {
        if (health <= 0.0f)
            return 0.0f;
        if (StateOf(own) != State::Deals)
            return std::numeric_limits<float>::infinity();
        return health / own.dps;
    }

    // [dpszero2] MAY THE WALK ENTER A MOB'S AGGRO ZONE WITHOUT FIGHTING IT - the route's own question, answered by
    // the fight judge that already exists and by nothing else.
    //
    // THE USER'S RULE (2026-11-15): a bot does not enter the aggro zone of a mob whose fight is lost. NOT "a mob
    // eleven levels above": the level gap is one of the reasons a fight comes out lost, and keying on it would
    // catch one case of many and plant a threshold where none is needed. There is no level check anywhere here.
    //
    // WHAT WAS WRONG. `RouteCosts` offered an orange / red / elite mob a RUN PAST first and, when the chase priced
    // as survivable, cleared the wall - so the fight was never judged at all and its aggro disc never reached the
    // search's refusals. Measured over the recorded b502 hour: 19 588 run-pasts priced as survivable across 73 of
    // the 75 bots, and about half of them (9 601) against a mob eleven or more levels above the bot, i.e. a fight
    // the formula gives hundredths of a dps. Lyleria L24 priced a run past Horde Guard L40 8 358 times in that one
    // hour. Running past IS entering the zone, and once inside there is no way out: the chase estimate of
    // [runpast89] is that a mob of equal speed is never outrun.
    //
    // SO THE FIGHT'S VERDICT COMES FIRST, and it is the SAME judge (rule 5: one quantity, one judge) - nothing
    // here recomputes damage, health or a chase. A fight NOBODY judged leaves the answer to the chase exactly as
    // before: no verdict rests on a judgement that did not happen (rule 7).
    struct Zone
    {
        bool judged = false;         // the fight judge ran on this mob's pack
        bool fightLost = false;      // ... and said the bot loses it even at full
        bool chaseSurvived = false;  // the run past was priced and the bot lives through the chase
    };

    inline bool MayRunPast(Zone const& zone)
    {
        if (zone.judged && zone.fightLost)
            return false;
        return zone.chaseSurvived;
    }
}

#endif
