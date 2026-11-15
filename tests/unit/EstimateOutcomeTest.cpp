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
