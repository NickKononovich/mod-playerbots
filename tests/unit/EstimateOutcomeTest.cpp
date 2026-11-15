/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [test1] The stand's main quantity: how far the judge's ESTIMATE fell from the FACT.
//
// Why not speed: once a changed judge decides differently the world diverges, and the recorded future no longer
// belongs to the new decision - "it got faster" cannot be measured offline at all. The estimate can: the judge
// prices every option (its cost, its mob count, its seconds), the journal keeps that prediction, and the run keeps
// the outcome. Their divergence is measurable with no server, and it is the divergence that produces the deaths and
// the stalls - a death counts here as an error of this calculation.
//
// THE OUTCOME SIDE CANNOT BE JOINED YET, and the gap is named rather than papered over:
//   - playerbots_fight_history holds the measured fights (actual_dps, fight_s, ...) but carries NO run_id and no
//     timestamp of any kind - only an auto-increment id. There is no key to a run and none to a decision. Joining it
//     by time is not possible and would not be honest if it were: two fives measure fights in the same second.
//   - guild_project_runs is run-level (its `at` is the END of the run, with deaths, wipes and duration for the whole
//     run). It cannot say which decision the deaths belong to.
//   - pb_journal_fight is being created by another session (src/Ai/Journal/**, read-only here). It is the table that
//     will carry the fight's outcome WITH the decision_id, which is what closes this.
// So the paired test skips with that reason, and what can be checked today - the estimate against itself - is
// checked today.

#include "ChainPullJudge.h"
#include "Fixture.h"
#include "KillRate.h"

#include <cmath>
#include <cstdio>
#include <gtest/gtest.h>

TEST(EstimateOutcome, TheJoinedFightIsNeverPricedShorterThanTheFightAlreadyRunning)
{
    // the one accuracy property the record can settle on its own: the fight the judge prices for the two packs
    // joined must take at least as long as the fight it is already in. Refutable without a culprit, and it goes red
    // on any change that lets the model price a joined fight away.
    std::vector<std::string> const files = PbFixture::ListJson(PbFixture::Root() + "/chain_pull");
    ASSERT_FALSE(files.empty());
    size_t checked = 0;
    for (std::string const& path : files)
    {
        PbFixture::Flat file;
        std::string error;
        ASSERT_TRUE(file.Load(path, error)) << error;
        double const unionSec = file.Num("union_sec", -1.0);
        double const fightSecLeft = file.Num("fight_sec_left", -1.0);
        if (unionSec <= 0.0 || fightSecLeft < 0.0)
            continue;  // the judge never got that far: absence is not zero
        ++checked;
        double const band = 2.0 * file.Num("number_precision", 0.1);
        EXPECT_GE(unionSec, fightSecLeft - band)
            << path << ": the union priced at " << unionSec << " s while the fight already running had "
            << fightSecLeft << " s left";
    }
    std::printf("[estimate] %zu fixtures carried a priced union\n", checked);
    EXPECT_GT(checked, 0u) << "no fixture carried a priced union: nothing was checked";
}

TEST(EstimateOutcome, TheEstimateDidNotDivergeFromTheFactByMoreThanTheBand)
{
    std::string const dir = PbFixture::Root() + "/estimate";
    std::vector<std::string> const files = PbFixture::ListJson(dir);
    if (files.empty())
        GTEST_SKIP() << "no estimate/outcome pairs in " << dir
                     << ": the outcome side has no key to a decision. playerbots_fight_history carries no run_id "
                        "and no timestamp, guild_project_runs is run-level only, and pb_journal_fight (which is to "
                        "carry the fight's outcome with its decision_id) is still being created. Waiting on that "
                        "column, not on a join by time.";
    // when the pairs exist: for each, |estimate - fact| <= the band the user sets, per quantity (the fight's
    // seconds, the mob count, the tank's drop). The band is a number the user approves - this rig does not invent
    // thresholds.
    FAIL() << "estimate/outcome pairs exist (" << files.size() << ") but the band is not agreed with the user yet";
}

// [dpszero1] THE ESTIMATE THAT DOES NOT EXIST, AND THE ONE THAT IS SMALL. Nine judge sites read the bot's own
// damage through `std::max(own.burstDps, 1.0f)`: one floor standing in for a missing estimate, for a measured zero
// and for an honest fraction of a dps at once. The fixtures are the b502 hour's own rows - the 37 judge lines that
// print exactly 1.0 dps, against a journal in which no line prints 0.1-0.9 or 1.1-4.3.
TEST(OwnDamageFloor, AnEstimateThatDoesNotExistIsNotAMeasuredZero)
{
    KillRate::Own const none;                      // nobody ran the formula
    KillRate::Own const measured{true, 0.0f};      // it ran, and the bot deals nothing
    EXPECT_EQ(KillRate::StateOf(none), KillRate::State::NoEstimate);
    EXPECT_EQ(KillRate::StateOf(measured), KillRate::State::DealsNothing);
    EXPECT_NE(KillRate::StateOf(none), KillRate::StateOf(measured));
    // the one a verdict may rest on is the one that exists
    EXPECT_FALSE(KillRate::Judgeable(none));
    EXPECT_TRUE(KillRate::Judgeable(measured));
}

TEST(OwnDamageFloor, AnEstimateThatDoesNotExistNeverTurnsIntoASpeed)
{
    // Irrua L25's recorded refusal: "118135 dmg expected over 3048 s at 1.0 dps". 3048 is the Barrens Guard's
    // health divided by the floor, so the whole line is the floor's arithmetic. An absent estimate must not be
    // able to produce a finite number of seconds at all.
    constexpr float GUARD_HEALTH = 3048.0f;
    KillRate::Own const none;
    EXPECT_FALSE(std::isfinite(KillRate::KillSec(none, GUARD_HEALTH)))
        << "an estimate nobody computed produced " << KillRate::KillSec(none, GUARD_HEALTH)
        << " s of fight: that is the invented 1.0 dps wearing a kill time";
    // a measured zero answers about the FIGHT, and the answer is that it does not end
    EXPECT_FALSE(std::isfinite(KillRate::KillSec({true, 0.0f}, GUARD_HEALTH)));
}

TEST(OwnDamageFloor, ATinyEstimateIsUsedAsItIsAndNotRaisedToOne)
{
    // Why those 37 lines were small, and it is no defect: at eleven levels and more above the bot the core's own
    // spell-hit table bottoms out at 1 % (94 - (levelDiff - 2) * 11, clamped), and 75 % resistance caps it further.
    // Every floored line is a caster against a mob 11 to 42 levels above - never ten or less. So the honest reading
    // is BELOW 1 dps, and the floor raised it: the fight looked shorter and the damage taken smaller than the
    // formula said.
    constexpr float GUARD_HEALTH = 3048.0f;
    constexpr float ONE_PERCENT_HIT = 0.04f;  // a L25 caster's dps at the table's floor, order of magnitude
    float const honest = KillRate::KillSec({true, ONE_PERCENT_HIT}, GUARD_HEALTH);
    float const floored = KillRate::KillSec({true, 1.0f}, GUARD_HEALTH);
    ASSERT_TRUE(std::isfinite(honest));
    EXPECT_GT(honest, floored) << "a fraction of a dps was answered with the same kill time as 1.0 dps: the floor "
                                  "is still there, and it points the optimistic way";
    EXPECT_NEAR(honest, GUARD_HEALTH / ONE_PERCENT_HIT, 1.0f);
    // and nothing left to kill is nothing to wait for, in every state
    EXPECT_FLOAT_EQ(KillRate::KillSec(KillRate::Own{}, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(KillRate::KillSec({true, ONE_PERCENT_HIT}, -5.0f), 0.0f);
}
