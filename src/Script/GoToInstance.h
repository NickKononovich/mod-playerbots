/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SCRIPT_GOTOINSTANCE_H
#define PLAYERBOTS_SCRIPT_GOTOINSTANCE_H

// [tele20] GOING TO A PLAYER MEANS GOING INTO HIS COPY OF THE MAP, AS A SEAM: the five numbers that decide it go
// in, the three steps come out, and nothing here reads the world, the clock or the database.
//
// THE DEFECT THIS SEAM EXISTS FOR. Every five runs its own copy of the dungeon, so a destination is a map AND an
// instance id. A teleport by coordinates (map + x/y/z) names only the map, and the instance the player lands in is
// not the one he aimed at: MapInstanced::CreateInstanceForPlayer asks InstanceSaveMgr::PlayerGetDestinationInstanceId,
// which answers with the PLAYER'S OWN bind (his group leader's, if he is in a group) and creates a fresh empty copy
// when there is none. The user's words for the result: "I teleport into dungeons to the people, but I do not see
// them. I am in some other mirror."
//
// WHY `.appear` ALONE IS NOT THE ANSWER, which is the whole reason this file is not three lines in the command.
// cs_misc.cpp HandleAppearCommand binds the GM to the target's instance ONLY when he has no bind for that map and
// difficulty yet - "if the GM is bound to another instance, he will not be bound to another one". A bind to an
// older copy therefore survives the command, PlayerGetDestinationInstanceId answers with it, and `.appear`
// reports "Appearing at ..." while landing him in his own empty mirror, with no error anywhere. Measured, not
// supposed: on 2026-11-15 the user's character Watcher (guid 1756) held character_instance rows for instance 2 of
// map 43 and instance 8 of map 36, while the fives inside Wailing Caverns at that moment were in instances 5, 6
// and 7 of map 43. The stale bind is the empty mirror.
//
// SO THE STALE BIND HAS TO GO FIRST, and that is a decision with three edges, which is what the test asserts:
//   - a bind that already points at HIS copy must not be touched (dropping and re-taking it would reset nothing
//     but would churn character_instance and the calendar lockout for a permanent one);
//   - a bind to ANOTHER copy must be dropped and retaken, or the teleport lands in the mirror;
//   - a mover standing in another copy of that same dungeon cannot have that bind dropped under his feet
//     (cs_instance.cpp HandleInstanceUnbindCommand keeps the bind of the map the character is on for the same
//     reason), so he leaves the dungeon first and the command is run again.
// Outside a dungeon there is no instance to pick and nothing to bind: the coordinates are the whole destination,
// and this seam says so by returning the empty plan.

#include <cstdint>

namespace GoToInstance
{
    // The facts of one "go to him", as the caller reads them off the two players. `boundInstance` is the mover's
    // bind for the TARGET's map and difficulty, 0 meaning "no bind" - which is not the same as "bound to 0" and
    // never arises, because an instance id of 0 is the open world.
    struct Facts
    {
        bool targetInDungeon = false;  // the target stands in an instanceable map (dungeon or raid)
        uint32_t targetMap = 0;
        uint32_t targetInstance = 0;
        uint32_t moverMap = 0;
        uint32_t moverInstance = 0;
        uint32_t boundInstance = 0;  // 0 = the mover has no bind for the target's map and difficulty
        // In a group the destination is the GROUP LEADER'S bind, never the mover's own
        // (InstanceSaveMgr::PlayerGetDestinationInstanceId), so nothing done to the mover's bind decides anything:
        // the caller lets the core do what ".appear" does for a group member and says whose bind it will be.
        bool moverGroupedWithTarget = false;
    };

    // What the caller has to do before it teleports. All three false = teleport to his coordinates and nothing else.
    struct Plan
    {
        bool leaveFirst = false;  // the mover is in another copy of this very dungeon: out of it first, then again
        bool unbind = false;      // drop the mover's bind to another copy
        bool bind = false;        // bind the mover to the target's copy

        bool operator==(Plan const& other) const
        {
            return leaveFirst == other.leaveFirst && unbind == other.unbind && bind == other.bind;
        }
    };

    inline Plan PlanGoTo(Facts const& f)
    {
        if (!f.targetInDungeon)
            return {};  // the open world: one copy, the coordinates are the whole destination

        if (f.moverGroupedWithTarget)
            return {};  // the leader's bind decides; the mover's own is not read at all

        if (f.moverMap == f.targetMap)
        {
            if (f.moverInstance == f.targetInstance)
                return {};  // already in his copy: only the coordinates are left to cover

            Plan plan;
            plan.leaveFirst = true;  // the bind of the map he stands in cannot go while he stands in it
            return plan;
        }

        if (f.boundInstance == f.targetInstance)
            return {};  // bound to exactly his copy already (0 never happens here: 0 is the open world)

        Plan plan;
        plan.unbind = f.boundInstance != 0;  // the stale bind is what took the user into an empty mirror
        plan.bind = true;
        return plan;
    }
}  // namespace GoToInstance

#endif
