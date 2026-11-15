/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LAST_WAY_H
#define PLAYERBOTS_LAST_WAY_H

// [lastway1] A LOST FIGHT WITH NO CLEAN WAY OUT RUNS ANYWAY, TOWARDS THE LEAST COVERED GROUND - the user's own
// words (2026-11-15), translated: "he should still try to run away by the shortest point if he loses the
// fight".
//
// WHAT WAS ALREADY THERE, and it is most of this - said with numbers so that nobody "fixes" it again.
// `SurvivalKit::FleeWay` does NOT search one direction. It runs THREE searches, each priced over every chaser's
// leash, speed, daze and casts (`[guild15]`, `[deaths]`):
//   1. straight away from the chasers' reset spots, then the same sweep to the sides;
//   2. back the way it came, stretch by stretch, cut at the last clear spot;
//   3. the maze search round the blockers (`MazeWays`), many corners, each stretch re-checked as the bot walks it.
// And the kit above it is not a plain rotation either: `[guild80]` prices a reset (Vanish, Shadowmeld, Feign
// Death), heals and heal items, absorbs, damage-taken buffs, STUNS on the attackers, silences and control
// windows, each from its own spell data against the projection's shortfall, shortest cooldowns first. The b504
// hour shows all of it alive: 44 survival lines - 21 heals, 4 resets, 3 damage-taken buffs, 8 flees taken.
// So "the bot cannot run" and "the bot does not fight with what it has" were both FALSE, and no part of that is
// touched here.
//
// THE ONE THING THAT WAS MISSING, in the numbers of the closed b504 corpus. All three searches are gated on the
// way being CLEAR OF OTHER MOBS (`StretchBlocker` must find nothing on any stretch). When all three fail, the
// kit answered `survival none` and the bot STOOD STILL until it died. Seven such lines, and all seven carry the
// same two facts: "no run: no way out clear of other mobs" AND "0 castable" - nothing else was left either:
//   Ginde L25     7 attackers 155.0 dps  2569 dmg to come at hp 777   (3.3x short)
//   Kylantino L23 5 attackers  95.5 dps  1711 dmg to come at hp 432   (4.0x)
//   Baridrol L27  5 attackers  54.3 dps  2063 dmg to come at hp 757   (2.7x)
//   Kianey L26    5 attackers  87.3 dps   952 dmg to come at hp 709   (1.34x)
//   Kianey L26    4 attackers  65.3 dps   769 dmg to come at hp 686   (1.12x)
//   Baridrol L27 15 attackers 134.7 dps  9275 dmg to come at hp 734   (12.6x)
//   Baridrol L27  1 attacker   31.6 dps  1583 dmg to come at hp 780   (2.0x)
// THREE OF THE SEVEN DEATHS of b504 (Baridrol, Kianey, Kylantino) carry one of these lines. In two of the seven
// the shortfall is 1.12x and 1.34x of the body - the margin a single stretch of distance decides. Standing still
// is the one choice with no upside at all.
//
// "NO CLEAN WAY OUT" IS THEREFORE NOT A REASON TO STAND - the same answer `[death472]` gave the rest
// ("nowhere to go is a RECORDED row, not a reason to sit down") and `[dpszero3]` gave the stand. Running
// without a clean way is NOT a
// promise of survival and this seam never pretends it is: it names a DIRECTION, and the direction is the one the
// project already computes.
//
// NO SECOND JUDGE AND NO NEW GEOMETRY (rule 5). The direction is not decided here: it is delegated to
// `StandGround::Of` over `SoloSafety::CoveredGround`, i.e. the SAME 16 rays, the same "fewest reaches first"
// order and the same measured comparison against `reachesHere` that `[death472]` and `[dpszero3]` already use.
// "Which ground does a mob's aggro cover" stays `AggroReach`; "is this fight lost" stays `JudgeAlone`. This seam
// owns exactly one thing: WHETHER the door opens.
//
// WHAT IT IS NOT. No clock and no elapsed time (rule 2): every input is a state - the judge's verdict, whether a
// clear way was found, whether a reset or a piece is castable, and counts of reaches. No new threshold or default
// (rule 3): there is no number in this file at all. The order is strict and the better option always wins - a
// reset that ends the fight outright, then a kit piece that is due, then any priced clean way, and only then
// this. Open the door wider and the bot runs from fights it wins or throws away a Vanish; the tests hold both
// inverses, and without them "always run" would pass green.
//
// Nothing here reads the world, the clock or the database.

#include <cstdint>
#include <vector>

#include "StandGround.h"

namespace LastWay
{
    // what the kit knows at the moment it would otherwise report "survival none"
    struct Exits
    {
        // the solo judge refuses this fight even at full (`JudgeAlone`'s own verdict, never recomputed). false:
        // the fight is not lost and nothing here applies - the bot is fighting, not leaving
        bool fightLost = false;
        // a reset ends the fight outright (SPELL_EFFECT_SANCTUARY, feign death) and is castable now
        bool resetDue = false;
        // a kit piece is castable and its own rule says it is due (`[guild80]`'s priced pieces)
        bool pieceDue = false;
        // one of the three flee searches found a way CLEAR of other mobs, priced and taken
        bool clearWay = false;
    };

    enum class Answer : uint8_t
    {
        Fights,        // the fight is not lost: nothing is being left
        ResetFirst,    // a reset ends the fight: strictly better than any run
        PieceFirst,    // a priced kit piece is due: tried before a run with no clean way
        ClearWay,      // a clean, priced way exists - the run the kit already planned
        LeastCovered,  // no clean way: `index` names the ray point FEWER reaches cover than cover the stand
        Nowhere        // no clean way and no less covered ground: a RECORDED problem, never a reason to stand
    };

    // The whole rule. `stand` and `spots` are passed straight to `StandGround::Of`, which owns the direction;
    // `index` is set only for `LeastCovered`.
    inline Answer Of(Exits const& exits, StandGround::Stand const& stand,
                     std::vector<StandGround::Candidate> const& spots, uint32_t& index)
    {
        index = 0;
        if (!exits.fightLost)
            return Answer::Fights;
        if (exits.resetDue)
            return Answer::ResetFirst;
        if (exits.pieceDue)
            return Answer::PieceFirst;
        if (exits.clearWay)
            return Answer::ClearWay;
        // the door this seam exists for. The ground's own judge answers where to: a spot out of every reach is
        // the step-out the kit already walks, so only `LessCovered` is a NEW direction here.
        switch (StandGround::Of(stand, spots, index))
        {
            case StandGround::Answer::ClearSpot:
                return Answer::LeastCovered;  // out of every reach: the least covered ground there is
            case StandGround::Answer::LessCovered:
                return Answer::LeastCovered;
            default:
                return Answer::Nowhere;
        }
    }

    // does the bot move at all on this answer
    inline bool Leaves(Answer answer)
    {
        return answer == Answer::ClearWay || answer == Answer::LeastCovered;
    }
}

#endif
