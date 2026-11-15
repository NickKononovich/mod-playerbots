/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [jrn44] THE REASON CODE OF pb_journal_aggro, over REAL rows of b502.
//
// The defect being held down, in numbers: 4 661 rows in the live table and 4 661 of them `add`. 2 621 of those match
// a pb_journal_fight `start` row of the same bot against the same (entry, low) at the SAME at_ms - so they are the
// FIRST mob of that fight - and the fight row's own `mobs` (creatures holding the bot on their threat list at that
// instant) is 0 in all 2 621. A fight's first mob cannot be an addition, and a bot in combat with nobody cannot have
// been "already fighting".
//
// BOTH DIRECTIONS ARE ASSERTED. Too narrow and a genuine addition stops being counted as one; too wide and every
// first mob is an addition again, which is the state this test was written against.

#include <gtest/gtest.h>

#include <string>

#include "AggroReason.h"

namespace
{
    using Journal::AggroReason::Facts;

    // pb_journal_aggro / pb_journal_fight, bot Emdyl (bot_low 1765), map 0, b502.
    //
    // at_ms 1794735677444: `start` against Forest Spider (entry 30, low 2321) with mobs = 0, and the aggro row of the
    // SAME mob at the SAME millisecond. No pull order (order_target_low 0), bot_moving 1 - Emdyl was walking its RPG
    // route. The live row says `add`.
    Facts EmdylFirstMobWhileWalking()
    {
        Facts facts;
        facts.inCombatWithOthers = false;  // the fight row's own count: 0 creatures on it
        facts.pullOrderOpen = false;       // order_target_low 0
        facts.moving = true;               // bot_moving 1
        return facts;
    }

    // at_ms 1794735846365: TWO Riverpaw Mongrels (entry 123, low 8658 and 8659) engaged Emdyl 1.998 s after low 8657
    // had started the fight at 1794735844367. These two are additions in the plain sense of the word, and they must
    // stay additions after the repair.
    Facts EmdylSecondMobOfTheSameFight()
    {
        Facts facts;
        facts.inCombatWithOthers = true;  // 8657 was still on it
        facts.pullOrderOpen = false;
        facts.moving = false;  // bot_moving 0
        return facts;
    }
}  // namespace

// THE DEFECT ITSELF. 2 621 live rows have this shape and every one of them was written `add`.
TEST(AggroReason, TheFirstMobOfAFightIsNotAnAddition)
{
    EXPECT_STRNE(Journal::AggroReason::Of(EmdylFirstMobWhileWalking()), "add")
        << "the first mob of a fight (Emdyl vs Forest Spider 2321 at 1794735677444, the fight row's own mobs = 0) "
           "is still counted as a mob joining a fight already running";
}

// ... and it is not merely "not add": a bot walking its route when a mob nobody pulled engaged it is `path`, which is
// the code that says the route walked into it.
TEST(AggroReason, WalkedIntoItWithNoOrderIsPath)
{
    EXPECT_STREQ(Journal::AggroReason::Of(EmdylFirstMobWhileWalking()), "path");
}

// THE OTHER DIRECTION, from the same fight two seconds later: a real addition stays one.
TEST(AggroReason, AMobJoiningARunningFightIsStillAnAddition)
{
    EXPECT_STREQ(Journal::AggroReason::Of(EmdylSecondMobOfTheSameFight()), "add");
}

// Standing still with no order is "it came by itself" - a wanderer, a respawn, a patrol - and not the route's fault.
TEST(AggroReason, StandingStillWithNoOrderIsCame)
{
    Facts facts;
    facts.moving = false;
    EXPECT_STREQ(Journal::AggroReason::Of(facts), "came");
}

// THE CODE THE TABLE WAS BUILT FOR. An order was open, and the mob went for somebody the plan did not put in front of
// it, or for a mob the plan was not about. 779 of the 2 621 first-mob rows of b502 had an order open, so this branch
// is reachable on the recorded data and was unreachable only because of the branch order.
TEST(AggroReason, AnOpenOrderThatDidNotArriveAsPlannedIsAnErrorOfTheEstimate)
{
    Facts notItsTank;
    notItsTank.pullOrderOpen = true;
    notItsTank.onThePlansTank = false;  // the mob went for a member who is not the order's tank
    notItsTank.thePlansMob = true;
    EXPECT_STREQ(Journal::AggroReason::Of(notItsTank), "estimate");
    EXPECT_FALSE(Journal::AggroReason::Expected(notItsTank));

    Facts notItsMob;
    notItsMob.pullOrderOpen = true;
    notItsMob.onThePlansTank = true;
    notItsMob.thePlansMob = false;  // a mob the plan did not count
    EXPECT_STREQ(Journal::AggroReason::Of(notItsMob), "estimate");
    EXPECT_FALSE(Journal::AggroReason::Expected(notItsMob));
}

// An order that arrived exactly as judged is not a problem and must not be reported as one.
TEST(AggroReason, AnOpenOrderThatArrivedAsPlannedIsCame)
{
    Facts asPlanned;
    asPlanned.pullOrderOpen = true;
    asPlanned.onThePlansTank = true;
    asPlanned.thePlansMob = true;
    EXPECT_STREQ(Journal::AggroReason::Of(asPlanned), "came");
    EXPECT_TRUE(Journal::AggroReason::Expected(asPlanned));
}

// An addition outranks the order: the bot was already fighting, so whatever the plan said about this mob, the fact of
// this instant is that it joined a fight in progress. `expected` still answers the plan's question separately - one
// quantity, one place - so the row keeps both.
TEST(AggroReason, AnAdditionDuringAnOpenOrderKeepsBothAnswers)
{
    Facts addOnThePlansTank;
    addOnThePlansTank.inCombatWithOthers = true;
    addOnThePlansTank.pullOrderOpen = true;
    addOnThePlansTank.onThePlansTank = true;
    addOnThePlansTank.thePlansMob = true;
    EXPECT_STREQ(Journal::AggroReason::Of(addOnThePlansTank), "add");
    EXPECT_TRUE(Journal::AggroReason::Expected(addOnThePlansTank));
}

// Every combination of the five facts lands on one of the four codes: the emitter never needs `unknown`, and a query
// that finds no `unknown` row is reading a code with no emitter rather than a case that does not happen.
TEST(AggroReason, EveryCombinationOfTheFactsHasACode)
{
    for (int bits = 0; bits < 32; ++bits)
    {
        Facts facts;
        facts.inCombatWithOthers = bits & 1;
        facts.pullOrderOpen = bits & 2;
        facts.onThePlansTank = bits & 4;
        facts.thePlansMob = bits & 8;
        facts.moving = bits & 16;
        std::string const reason = Journal::AggroReason::Of(facts);
        EXPECT_TRUE(reason == "add" || reason == "came" || reason == "path" || reason == "estimate")
            << "facts " << bits << " gave " << reason;
    }
}
