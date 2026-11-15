/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AI_WORLD_RPG_MOBSTAND_H
#define PLAYERBOTS_AI_WORLD_RPG_MOBSTAND_H

#include <algorithm>

// [stuck259] HAS THE MOB LEFT THE STAND ITS VERDICT WAS GIVEN AT, AS A SEAM: the facts go in, the answer comes out,
// and nothing here reads the world, the clock or the database. Header-only, so the unit stand compiles it alone.
//
// WHAT WAS WRONG, measured on the recorded window. A target the bot's combat engine could run nothing on is let go
// and remembered as the pair of stands it was let go at (SoloSafety::NoteUnreachable); the memo is asked again when
// the bot leaves its stand OR THE MOB LEAVES ITS OWN. The second half was measured with
// Leader::LEADER_COMMIT_MOVED_YD = 12 yd - the yards that say A LEADER'S COMMITMENT TO A PULL is no longer about the
// same arrangement. Applied to a wandering mob it is not a measure of anything the mob does: of 199 `stuck_target`
// cases 25 (12.6 %) were the same bot and the same mob again inside a minute, and in 20 of those 25 THE BOT HAD NOT
// MOVED A YARD - only the mob had. Those mobs roam `creature.wander_distance` 10 yd and 3 yd areas (Giant Moss
// Creeper 15453 / 15468, Vicious Gray Bear 15469, Torn Fin Muckdweller 15473, Syndicate Shadow Mage 15584): a mob
// can cross its WHOLE area, come back, and cross it again without ever moving the 12 yd the memo waits for, while
// every step of it is a new question for the walk. One quantity, one judge (rule 5): the leader's commitment keeps
// its 12 yd, and the mob's departure is measured by the mob's own area.
//
// THE MEASURE IS THE MOB'S OWN AREA, which is a fact of the world's data and no new threshold: its spawn point and
// `creature.wander_distance` for a mob that roams (RANDOM_MOTION_TYPE, Leader::Fight::WanderAreaOf), and the one
// spot it holds for a mob that does not. Inside that area the mob is where it was judged - it is the same
// arrangement, and this is the very model the pull judge already uses for a wanderer ([lead1331]: a wanderer is
// judged by its whole area, not by the tick it is on). Outside it, the memo is an open question again.
//
// WHY THE SLACK IS THE MOB'S COMBAT REACH and not a constant: a mob standing at the edge of its area is "outside"
// it by whatever its body is, and the reach is the body the core itself uses for every distance that matters. No
// number was invented here.
//
// NO CLOCK: every fact is a state of this instant.

namespace Rpg::MobStand
{
    struct Facts
    {
        // the mob roams an area of its own (RANDOM_MOTION_TYPE with a wander distance)
        bool wanders = false;
        // that area's radius; 0 for a mob that holds one spot - and for one that holds a spot the center IS that
        // spot, so the two cases are one formula
        float areaRadius = 0.0f;
        // how far from the center of that area the mob stands now
        float fromCenterYd = 0.0f;
        // the mob's own combat reach: its body, the core's own quantity
        float bodyYd = 0.0f;
    };

    // How far outside its own area the mob stands (0 while it is inside it)
    inline float OutsideItsAreaYd(Facts const& facts)
    {
        float const radius = facts.wanders ? facts.areaRadius : 0.0f;
        return std::max(0.0f, facts.fromCenterYd - radius);
    }

    // The mob is no longer at the stand its verdict was given at: that verdict is an open question again
    inline bool LeftItsArea(Facts const& facts) { return OutsideItsAreaYd(facts) > facts.bodyYd; }
}

#endif
