/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AGGRO_REACH_H
#define PLAYERBOTS_AGGRO_REACH_H

// [death470] THE GROUND ONE MOB'S AGGRO COVERS FOR A FIGHT THAT IS ABOUT TO START - one judge of it, and a seam.
//
// THE FIRST DEFECT IT CLOSES, in measured rows. `[death462]` names, for every engage the solo judge opened, the mobs
// whose reach covers the fight spot that its pack LEFT OUT, each with what it was doing at that instant. Over the
// recorded window there are 31 such mobs in 30 engages, and they divide like this:
//   18  in a fight of their own (victim: another bot of the guild)
//    8  evading home
//    5  idle - a genuine gap of the pull model
// So 26 of 31 (84 %) were left out for one reason: the judge's surroundings admit a mob only while
// `!IsInCombat()`, and a mob busy somewhere else is dropped from the count entirely. It is not gone - it returns to
// its home, its home reach covers the spot, and it joins. `[deaths139]` (141 adds that joined a judged fight) says
// the same from the other end: 60 of the 141 adds had a home whose reach "covers it", median 9.2 s into the fight.
// The deaths it bought, over the whole priced corpus: of 4 920 fights with both numbers printed, 118 had more mobs
// come than were judged - 2.4 % - but that includes ALL FIVE of the fights that were lost and 12 of the 20
// disengages. Undercounting is rare and almost perfectly predictive of a death.
//
// THE SECOND DEFECT, the other direction. The disc of a mob away from its home was widened by how far it had gone
// (max(wanderDistance, distance from home)) so that the ground round where it stands now is covered too. For a mob
// that is away because something is HOLDING it there - a fight of its own, a walk back home - that widening walls
// off every yard it was dragged across, ground its aggro will never cover again once it is free. b503 Duskwood:
// Mor'Ladim L30's refusal was recorded 103, 107, 115, 117, 128, 132, 160 and 162 yd away from the walker.
//
// WHAT IT IS NOT. No clock is read and no elapsed time decides anything (rule 2): "busy" is a state of the mob, and
// the answer changes only when that state changes. No threshold is introduced (rule 3): every number in `Mob` comes
// from the game - the creature's own aggro range on this bot, the bot's combat reach, the spawn's wander distance,
// and two positions. There is no constant to tune.
//
// Nothing here reads the world, the clock or the database.

#include <algorithm>
#include <cmath>

namespace AggroReach
{
    // one mob, as the game already knows it
    struct Mob
    {
        float homeX = 0.0f, homeY = 0.0f, homeZ = 0.0f;  // its spawn point: where it stands, or returns to
        float x = 0.0f, y = 0.0f, z = 0.0f;              // where it stands now
        float aggroYd = 0.0f;   // Creature::GetAggroRange against this bot (the game's own number)
        float botReachYd = 0.0f;  // the bot's own combat reach
        float wanderYd = 0.0f;  // the spawn's wander distance (0: it does not wander)
        bool wanders = false;   // RANDOM_MOTION_TYPE
        // it is held away from its own ground by something of its own: a fight it is in, or the walk back home after
        // one (evade). Where it stands NOW is not ground it can aggro from for our fight; where it RETURNS TO is.
        bool busy = false;
    };

    // the ground it can aggro from, as a disc (its centre and radius in yards)
    struct Disc
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float radius = 0.0f;
    };

    // One answer for all three kinds of mob, so that no caller has to decide which rule applies:
    // - standing still and free: the circle round where it stands;
    // - a wanderer, free: the circle round its home holding the whole area it may wander into, widened by how far it
    //   has actually gone, so it covers both that area AND the ground round where it stands now;
    // - busy (a fight of its own, evading home): the circle round its HOME and its wander area only. The yards
    //   between home and where it is being held are not its aggro ground - it is leaving them.
    Disc Of(Mob const& mob);

    // `at` lies on that ground (2D, as every aggro check in the project is)
    bool Covers(Disc const& disc, float x, float y);
}

#endif
