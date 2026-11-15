/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AggroReach.h"

namespace AggroReach
{
    Disc Of(Mob const& mob)
    {
        Disc out;
        float const base = std::max(mob.aggroYd, 0.0f) + std::max(mob.botReachYd, 0.0f);
        float const wander = mob.wanders ? std::max(mob.wanderYd, 0.0f) : 0.0f;
        float const gone = std::hypot(mob.x - mob.homeX, mob.y - mob.homeY);
        if (mob.busy)
        {
            // it is coming home: its ground is home and the area it wanders, never the yards it is being held in
            out = {mob.homeX, mob.homeY, mob.homeZ, base + wander};
            return out;
        }
        if (!mob.wanders)
        {
            out = {mob.x, mob.y, mob.z, base};
            return out;
        }
        // free to wander: the home circle has to hold the whole wander area AND the ground round where it stands now
        out = {mob.homeX, mob.homeY, mob.homeZ, base + std::max(wander, gone)};
        return out;
    }

    bool Covers(Disc const& disc, float x, float y)
    {
        float const dx = disc.x - x;
        float const dy = disc.y - y;
        return dx * dx + dy * dy <= disc.radius * disc.radius;
    }
}
