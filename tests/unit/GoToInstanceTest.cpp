/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [tele20] GOING TO A BOT IN A DUNGEON LANDS IN HIS COPY, over the real binds of 2026-11-15.
//
// The row this test is written from, read out of acore_characters.character_instance while the user complained:
// his character Watcher (guid 1756) was bound to instance 2 of map 43 (Wailing Caverns) and instance 8 of map 36
// (Deadmines), while the three fives inside Wailing Caverns at that moment held instances 5, 6 and 7 of map 43.
// `.appear` keeps an existing bind (cs_misc.cpp: "if the GM is bound to another instance, he will not be bound to
// another one"), so the destination stayed instance 2 - an empty copy - and the command still said "Appearing at".
//
// BOTH DIRECTIONS ARE ASSERTED, because each half-rule passes one of them:
//   - a stale bind must be dropped (today's rule keeps it and the user ends up in the mirror);
//   - a bind that already points at his copy must NOT be dropped and retaken, and the open world must not be
//     touched at all, or "fix the dungeon case" would start churning binds and lockouts everywhere else.

#include <gtest/gtest.h>

#include "GoToInstance.h"

using namespace GoToInstance;

namespace
{
    // Wailing Caverns as it stood on 2026-11-15: the user bound to copy 2, the five he clicked on inside copy 6.
    Facts WailingCaverns(uint32_t moverMap, uint32_t moverInstance, uint32_t boundInstance)
    {
        Facts f;
        f.targetInDungeon = true;
        f.targetMap = 43;
        f.targetInstance = 6;
        f.moverMap = moverMap;
        f.moverInstance = moverInstance;
        f.boundInstance = boundInstance;
        return f;
    }
}  // namespace

// THE DEFECT ITSELF: standing in the open world, bound to the empty copy 2, clicking a bot of copy 6.
TEST(GoToInstance, StaleBindIsDroppedAndRetaken)
{
    Plan const plan = PlanGoTo(WailingCaverns(1, 0, 2));
    EXPECT_TRUE(plan.unbind) << "the bind to copy 2 is what takes the user into the empty mirror";
    EXPECT_TRUE(plan.bind) << "without a bind to copy 6 the core creates yet another empty copy";
    EXPECT_FALSE(plan.leaveFirst) << "he is in the open world, there is nothing to leave";
}

// The other direction: a bind that is already his copy stays as it is.
TEST(GoToInstance, BindOnHisOwnCopyIsLeftAlone)
{
    Plan const plan = PlanGoTo(WailingCaverns(1, 0, 6));
    EXPECT_EQ(plan, Plan{}) << "dropping and retaking the same bind churns character_instance for nothing";
}

// No bind at all: nothing to drop, but the bind still has to be taken, or a fresh empty copy is created.
TEST(GoToInstance, NoBindIsOnlyTaken)
{
    Plan const plan = PlanGoTo(WailingCaverns(1, 0, 0));
    EXPECT_FALSE(plan.unbind);
    EXPECT_TRUE(plan.bind);
    EXPECT_FALSE(plan.leaveFirst);
}

// Already inside his copy: only the coordinates are left, and the bind must not be touched.
TEST(GoToInstance, SameCopyIsOnlyAWalk)
{
    Plan const plan = PlanGoTo(WailingCaverns(43, 6, 6));
    EXPECT_EQ(plan, Plan{});
}

// Standing in ANOTHER copy of that same dungeon: the bind of the map under his feet cannot go, so he leaves first.
TEST(GoToInstance, OtherCopyOfTheSameDungeonLeavesFirst)
{
    Plan const plan = PlanGoTo(WailingCaverns(43, 2, 2));
    EXPECT_TRUE(plan.leaveFirst);
    EXPECT_FALSE(plan.unbind) << "unbinding the map he stands in is what cs_instance.cpp refuses to do";
    EXPECT_FALSE(plan.bind);
}

// In the target's own group nothing of the mover's bind is read: the group leader's is (PlayerGetDestinationInstanceId).
// Touching the mover's bind here would be work that decides nothing, and dropping it could cost him a raid lockout.
TEST(GoToInstance, InTheTargetsGroupNothingIsBound)
{
    Facts f = WailingCaverns(1, 0, 2);
    f.moverGroupedWithTarget = true;
    EXPECT_EQ(PlanGoTo(f), Plan{});
}

// The open world is untouched: a teleport to a bot in Durotar binds nothing and must not drop a dungeon bind.
TEST(GoToInstance, OpenWorldPlansNothing)
{
    Facts f;
    f.targetInDungeon = false;
    f.targetMap = 1;
    f.targetInstance = 0;
    f.moverMap = 0;
    f.moverInstance = 0;
    f.boundInstance = 2;  // his stale Wailing Caverns bind is none of this teleport's business
    EXPECT_EQ(PlanGoTo(f), Plan{});
}
