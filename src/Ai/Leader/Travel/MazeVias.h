/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_MAZEVIAS_H
#define PLAYERBOTS_LEADER_MAZEVIAS_H

// [maze21] THE WAYS ROUND A REFUSED WALK, AS AN ANSWER THAT CAN SAY "I DID NOT LOOK".
//
// The maze search's step 2 asks "which places near me could I walk round through". It used to ask the stock
// TravelNodeMap, which is never loaded in this build (TravelMgr::LoadQuestTravelTable has no caller anywhere in the
// module or the core; only the `load node` debug chat command reaches loadNodeStore), so the question came back
// empty EVERY time - and the row wrote that empty answer down as `candidates = 0`, i.e. "there is nowhere to go
// round", which is a statement about the world. It was false: 75 of the 76 recorded give-ups of b504 have 1 to 8
// named walkable places of playerbots_travelnode within 400 yd of the very spot the bot gave up at.
//
// So this seam carries the source's own answer separately from the count (rule 7, absence is not zero):
//   `measured == false` -> no source answered (this map has no built graph): the count means nothing and the row
//                          must write NULL, never 0;
//   `measured == true`  -> a source answered, and `candidates` is then a measurement, 0 included.
// Nothing here reads the world, the clock or the database, and there is no number of its own: `reach` and
// `sameSpot` are the caller's existing ones (the search's reach and MOVE_FAR_PROGRESS).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Leader::Travel::Maze
{
    struct Spot
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    inline float Gap(Spot const& a, Spot const& b)
    {
        float const dx = a.x - b.x;
        float const dy = a.y - b.y;
        float const dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    struct Via
    {
        Spot at;
        float yards = 0.0f;  // from `here` through it on to `goal`: the whole way round, not the leg to it
    };

    // the ways round, the shortest way through first. `candidates` is read only when `measured`
    struct Vias
    {
        bool measured = false;
        uint32_t candidates = 0;
        std::vector<Via> order;
    };

    // `sourceAnswered`: the source said whether it holds nodes for this map at all (not whether it found any here)
    inline Vias Of(bool sourceAnswered, std::vector<Spot> const& nodes, Spot const& here, Spot const& goal,
                   float reach, float sameSpot)
    {
        Vias vias;
        if (!sourceAnswered)
            return vias;  // nothing was looked at: the count stays unmeasured
        vias.measured = true;
        for (Spot const& at : nodes)
        {
            float const toIt = Gap(at, here);
            if (toIt > reach)
                continue;        // out of the search's reach
            if (toIt <= sameSpot)
                continue;        // where the bot already stands is no way round
            vias.order.push_back({at, toIt + Gap(goal, at)});
        }
        // the shortest way through first; the leg to it breaks a tie, so the order is the same answer every look
        std::sort(vias.order.begin(), vias.order.end(),
                  [&](Via const& a, Via const& b)
                  {
                      if (a.yards != b.yards)
                          return a.yards < b.yards;
                      return Gap(a.at, here) < Gap(b.at, here);
                  });
        vias.candidates = uint32_t(vias.order.size());
        return vias;
    }
}

#endif
