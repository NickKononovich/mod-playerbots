/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [test1] The death tests, left as a shape with nothing in it yet, on purpose. A death counts here as an error of
// the leader's calculation, so each one earns a test that would have gone red before it - but there is nothing to
// feed one yet:
//
//   - [jrn50] THIS HALF IS DONE, and the note is corrected rather than left standing (a comment about what the code
//     no longer does costs more than a wrong number): the emitters now write the five's bots and each mob's level,
//     health and mana into the line themselves, in the judge's own pass (LeaderPlan: `five`, `lookAt`, the enriched
//     `packAt`), the journal turns them into pb_journal_actor rows (ActorFacts, covered by ActorFactsTest), and the
//     member's drinks / foods / drinks_need are columns too. The journal still does not ask a unit anything - that
//     would be a second reading of the world at a different moment, a second judge of one quantity. What these
//     tests wait for now is a RUN on a build carrying those emitters, so the rows exist to export.
//   - pb_journal_death / pb_journal_fight / pb_journal_aggro are being created by another session right now
//     (src/Ai/Journal/**). Their columns are read here, never written.
//
// So these tests SKIP, and each says what it waits for. They are not disabled: a skip is reported every run, which
// is how the gap keeps asking. The moment the tables carry rows, tests/tools/export_fixture.py death --id <n>
// writes the fixture and the body below replaces the skip.
//
// What the first death test will assert, so it is refutable without knowing the culprit (the same rule the pull
// corpus follows):
//   - the fight the bot died in was taken with a modelled survival margin that was not positive, or
//   - more mobs reached the group than the judge priced into the fight it took, or
//   - the healer that the judge counted on was farther from the landing than the judge's own rule allows.
// Any of those is a calculation the record itself refutes; which line of code produced it is found afterwards, once
// the test is already red.

#include "ChainPullJudge.h"
#include "Fixture.h"

#include <gtest/gtest.h>

TEST(DeathReplay, ADeathIsAnErrorOfTheSurvivalCalculation)
{
    std::string const dir = PbFixture::Root() + "/death";
    std::vector<std::string> const files = PbFixture::ListJson(dir);
    if (files.empty())
        GTEST_SKIP() << "no death fixtures in " << dir
                     << ": waiting on a run of a build carrying the [jrn50] emitters (the five's rows and the "
                        "mobs' health are written now, but no run has produced them yet) and on a death row "
                        "carrying the decision_id of the pull that took the fight. The query that says the first "
                        "half has arrived: SELECT COUNT(*) FROM pb_journal_actor WHERE side = 0;";
    // the body lands here together with the first exported fixture: load it, put the facts through
    // Leader::Judge::ReplayChainPull, and assert the margin the record refutes.
    FAIL() << "death fixtures exist (" << files.size() << ") but the replay body is not written yet";
}

TEST(DeathReplay, ADelayedEffectIsSoughtInTheWindowBeforeTheDeath)
{
    // the user's point: a death may be the effect of a decision taken earlier, so the stand must be able to load a
    // WINDOW of decisions before the event, not one row. The window is a query - run_id plus at_ms - and the
    // exporter writes it as a numbered series of fixtures in one directory.
    //
    // What is NOT invented here: no criterion saying "this earlier decision caused that death". The journal holds
    // none, and making one up would turn an author's guess into the test's truth. The window is loaded and shown;
    // the link is a finding a human makes.
    std::string const dir = PbFixture::Root() + "/death_window";
    std::vector<std::string> const files = PbFixture::ListJson(dir);
    if (files.empty())
        GTEST_SKIP() << "no death windows in " << dir
                     << ": waiting on pb_journal_death (for the event's run_id and at_ms) so that "
                        "export_fixture.py death-window --id <n> --before-ms 60000 can cut the window.";
    EXPECT_GE(files.size(), 2u) << "a window of one decision is not a window";
}
