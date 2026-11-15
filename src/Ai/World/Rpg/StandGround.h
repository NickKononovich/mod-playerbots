/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_STAND_GROUND_H
#define PLAYERBOTS_STAND_GROUND_H

// [dpszero3] STANDING STILL INSIDE THE AGGRO ZONE OF A FIGHT THAT IS LOST - the THIRD reader of the zone rule, and
// a seam.
//
// THE USER'S WORD (2026-11-15), verbatim: "Стоять в зоне агро тоже нельзя, конечно же" - standing in an aggro zone
// is not allowed either. He was told the price (some quest spots become unreachable until the bot levels) and took
// it. So this is a rule he granted, and NOT a number: there is no threshold here, no second's worth of patience and
// no "not more often than" - the whole rule is a COUNT of reaches and one verdict asked of the judge that already
// answers it.
//
// THE HOLE IT CLOSES, and it is a hole in the ORDER of readers, not in any number. [dpszero2] gave the zone rule
// exactly two readers:
//   - the walk (`KillRate::MayRunPast`): it does not ENTER the zone of a lost fight;
//   - the rest ([death472], `RestEscape`): it does not SIT DOWN in one.
// A bot that already stands inside the disc and is doing neither was covered by neither. The third reader was
// nominally there - `SoloSafety::InRefusedReach` feeding `[reach66]` - but it asked about ONE remembered mob (the
// target of the bot's own last logged verdict) instead of about the GROUND, so a mob that walked up without ever
// being judged was invisible to it. Measured on the closed b504 corpus: `[reach66]` printed **0** lines in the whole
// hour, while 937 lines of the same hour say "loses even at full" and 371 fights opened that no judge opened at all.
// Of those 371, the bot was INSIDE the first mob's own aggro disc in 311 (83.8 %) and was STANDING STILL inside it
// in 162 (43.7 %).
//
// AND THE DEATHS SAY THE SAME FROM THE OTHER END. 7 deaths in b504, every one of them "no judged pull (attacked)".
// Four carry the route search's own reading of the mobs that then hit the bot, and THREE of those four name a mob
// the search had REFUSED - i.e. the judge had already called that fight lost and the bot went on standing in its
// reach: Kianey L26 (4 of 4 hitters refused), Rollothan L20 (3 of 5 refused), Baridrol L27 (Mor'Ladim refused).
// The fourth, Kylantino L23, stood on a route that ran THROUGH a refused reach. 42 s, 27 s and 42 s respectively
// had passed since that reading - and the answer to a stale reading is not a fresher clock but asking the judge
// about the ground the bot is on, which is what this reader does.
//
// WHY IT IS NOT "DO NOT STAND IN ANY REACH". A bot standing in the disc of a mob it BEATS is doing nothing wrong,
// and that is the common case: of the 371 unjudged fights of b504 the bot's own projection came out WON in 318
// (85.7 %) and LOST in 2. A rule that emptied every disc would move bots off most of the world's ground for no
// measured reason - the twin of the 85.5 % and 87.5 % shares this project twice nearly "fixed". So `Leaves` keys
// on the LOST reaches alone, and the test holds the inverse assertion: a stand covered only by winnable fights is
// not left. Without it, "forbid everything" would pass green.
//
// WHERE IT GOES, and nothing new is measured for it. `[death472]` already made `StandsInReach` answer with
// `CoveredGround`: how many reaches cover the bot where it stands, and every point its 16 rays leave a reach at,
// with the count over each, fewest first. This reader takes the same list: a spot out of EVERY reach first (the
// walk the step-out already took), then ground covered by FEWER reaches than the stand, and nothing else. "Fewer"
// is a measured difference against `reachesHere`, never an opinion.
//
// AND IF THERE IS NOWHERE TO GO AT ALL, that is a WRITTEN row and not a reason to stay - the same answer
// `rest_no_ground` gives for the rest, keyed on the facts (which reaches, how many) so that it is one row per fact
// and never one per tick.
//
// WHAT IT IS NOT. No clock is read and no elapsed time decides anything (rule 2): every input is a state - how
// many reaches cover the ground, how many of them are lost, whether a clear spot exists. No new threshold or
// default (rule 3): the only numbers are counts of reaches, and they are compared with each other. No second judge
// of any quantity (rule 5): "which ground does this mob's aggro cover" stays `AggroReach`, "is this fight lost"
// stays the solo judge's own verdict, and this seam adds neither.
//
// Nothing here reads the world, the clock or the database.

#include <cstdint>
#include <vector>

namespace StandGround
{
    // the ground the bot stands on, as the two judges that already exist report it
    struct Stand
    {
        // the aggro reaches that cover the bot WHERE IT STANDS (SoloSafety::CoveredGround::reachesHere)
        uint32_t reachesHere = 0;
        // ... of which this many belong to a mob whose fight the solo judge refuses even at full. 0 with
        // `reachesHere` above zero is an ANSWER - every reach over the bot is a fight it takes - and it is not the
        // same as `reachesHere == 0`, where no reach covers it at all
        uint32_t lostReaches = 0;
        // a spot out of every reach within sight whose walk the bot does not refuse: the step-out's own ground
        bool clearSpot = false;
    };

    // a point one of the 16 rays left a reach at, with how many reaches cover it
    struct Candidate
    {
        uint32_t reaches = 0;
    };

    enum class Answer : uint8_t
    {
        Stays,        // nothing that covers this ground is a fight it loses: it stands where it stands
        ClearSpot,    // out of every reach - the caller's own step-out walk
        LessCovered,  // `index` names the ray point fewer reaches cover than cover the stand
        Nowhere       // it is left, and there is no ground to leave to: a RECORDED problem, never a reason to stay
    };

    // The whole rule. `index` is set only for `LessCovered`.
    inline Answer Of(Stand const& stand, std::vector<Candidate> const& spots, uint32_t& index)
    {
        index = 0;
        if (stand.lostReaches == 0)
            return Answer::Stays;
        if (stand.clearSpot)
            return Answer::ClearSpot;
        for (uint32_t i = 0; i < uint32_t(spots.size()); ++i)
            if (spots[i].reaches < stand.reachesHere)
            {
                index = i;
                return Answer::LessCovered;
            }
        return Answer::Nowhere;
    }

    // does this stand have to be left at all
    inline bool Leaves(Stand const& stand)
    {
        uint32_t ignored = 0;
        return Of(stand, {}, ignored) != Answer::Stays;
    }
}

#endif
