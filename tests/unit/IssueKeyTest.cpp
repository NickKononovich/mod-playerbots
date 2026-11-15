/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [jrn41] THE DEDUPLICATION KEY, BOTH WAYS. Every snapshot below is a real row of pb_journal_issue /
// pb_journal_idle taken from the live base of b501, not an invented string: bot Kylantino, map 369, zone 2257, the
// same position to four decimals (39.8389, 19.7113), one unbroken episode of standing from 10.005 s to 60.206 s.
// Six samples of ONE episode became FIVE rows in the problems table because the engine's action trace is in the
// key, and the trace changes every sample.
//
// BOTH DIRECTIONS ARE ASSERTED, because only one of them is the defect that was reported and the other is the
// worse one: a key wide enough to swallow the trace is also wide enough to swallow a different cause, and then a
// fix for the first cause turns this test green while the second cause lives on underneath it.

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "IssueKey.h"

namespace
{
    // the six samples of Kylantino's one episode, in the order they were written (pb_journal_idle.doing)
    std::vector<std::string> const KYLANTINO_EPISODE = {
        "|A:create soulstone - PREREQ|A:create soulstone - FAILED|A:new rpg status update - PREREQ|A:new rpg "
        "status update - FAILED|A:add gathering loot - PREREQ|A:add gathering loot - FAILED|A:drink - "
        "USELESS|A:food - USELESS|A:attack anything - USELESS|A:new rpg do quest - PREREQ|A:new rpg do quest - OK",
        "|PUSH:food - 4.100000 (default)|PUSH:new rpg status update - 11.000000 (default)|A:loot roll - "
        "PREREQ|A:loot roll - FAILED|A:toggle pet spell - PREREQ|A:toggle pet spell - FAILED|A:create soul shard - "
        "USELESS|A:create soulstone - PREREQ|A:create soulstone - FAILED|A:new rpg do quest - OK",
        "",  // the sample in which the engine had no action to name at all - "no action" in the row
        "|A:attack anything - USELESS|A:new rpg do quest - PREREQ|A:new rpg do quest - OK",
        "|A:create soulstone - PREREQ|A:create soulstone - FAILED|A:new rpg status update - PREREQ|A:new rpg "
        "status update - FAILED|A:add gathering loot - PREREQ|A:add gathering loot - FAILED|A:drink - "
        "USELESS|A:food - USELESS|A:attack anything - USELESS|A:new rpg do quest - PREREQ|A:new rpg do quest - OK",
        "|A:create soulstone - FAILED|A:new rpg status update - PREREQ|A:new rpg status update - FAILED|A:add "
        "gathering loot - PREREQ|A:add gathering loot - FAILED|A:drink - USELESS|A:food - USELESS|A:attack "
        "anything - USELESS|A:new rpg do quest - PREREQ|A:new rpg do quest - OK"};
}  // namespace

// ONE episode of ONE bot in ONE place is ONE problem, whatever the engine tried in each second of it.
TEST(IssueKeyStanding, OneEpisodeIsOneKeyWhateverTheEngineTried)
{
    std::set<std::string> keys;
    for (std::string const& trace : KYLANTINO_EPISODE)
        keys.insert(Journal::Key::Standing("dps", false, 1, "", trace));
    EXPECT_EQ(keys.size(), 1u) << "six samples of one standing episode gave " << keys.size()
                               << " keys: the engine's action trace is in the key";
}

// THE OTHER DIRECTION. Standing through a fight and standing in peace are different problems, and so are two
// different AI states; a key that merges them would hide one fix behind another's green test.
TEST(IssueKeyStanding, DifferentCausesAreDifferentKeys)
{
    std::string const trace = KYLANTINO_EPISODE.front();
    EXPECT_NE(Journal::Key::Standing("dps", true, 1, "", trace), Journal::Key::Standing("dps", false, 1, "", trace));
    EXPECT_NE(Journal::Key::Standing("dps", false, 0, "", trace), Journal::Key::Standing("dps", false, 1, "", trace));
    EXPECT_NE(Journal::Key::Standing("dps", true, 0, "", trace), Journal::Key::Standing("dps", false, 1, "", trace));
    // the role is a cause here as it is in the sibling kinds: a healer that stands is not a tank that stands, and
    // the live rows separate into 42 dps, 18 tank and 3 healer episodes out of combat
    EXPECT_NE(Journal::Key::Standing("healer", false, 1, "", trace),
              Journal::Key::Standing("dps", false, 1, "", trace));
    EXPECT_NE(Journal::Key::Standing("tank", false, 1, "", trace), Journal::Key::Standing("dps", false, 1, "", trace));
}

// [jrn42] THE CAUSE FIELD. A member standing exactly where its leader ordered it to stand is not the problem a
// member standing with no order at all is, and until the hold entered the key both were one row. The numbers that
// make this the biggest key in the table: of 1089 out-of-combat standing rows inside Wailing Caverns on b502, 931
// (85.5 %) fell in a window in which the leader of that very run had recorded a `pull` verdict of `held` - so one
// leader hold was counted again as a problem of each member waiting under it. Joined by run_id (identity) and by
// the overlap in time (marked as a time join, as the project's slice rule requires).
TEST(IssueKeyStanding, TheLegitimateHoldIsPartOfTheCause)
{
    std::string const trace = KYLANTINO_EPISODE.front();
    std::string const unheld = Journal::Key::Standing("dps", false, 1, "", trace);
    EXPECT_NE(Journal::Key::Standing("dps", false, 1, "its leader ordered it to stand", trace), unheld)
        << "a member standing under its leader's own order shares a key with a member standing under no order";
    EXPECT_NE(Journal::Key::Standing("dps", false, 1, "waits for a transport to dock", trace), unheld);
    EXPECT_NE(Journal::Key::Standing("dps", false, 1, "waits for a transport to dock", trace),
              Journal::Key::Standing("dps", false, 1, "waits for its group", trace))
        << "two different legitimate waits share one key";
}

// ... and the other direction: the hold is a closed set of names read as facts, so one hold through one episode
// is still one key however the engine's trace moves under it.
TEST(IssueKeyStanding, OneHeldEpisodeIsStillOneKey)
{
    std::set<std::string> keys;
    for (std::string const& trace : KYLANTINO_EPISODE)
        keys.insert(Journal::Key::Standing("dps", false, 1, "waits for a transport to dock", trace));
    EXPECT_EQ(keys.size(), 1u) << "six samples of one held episode gave " << keys.size() << " keys";
}

// The same trap in the second emitter that holds a trace: these two are real stuck_target keys of b501, one of a
// dps and one of a tank, and the tail of each is the engine's strategy trace of that instant.
TEST(IssueKeyStuckTarget, TheCauseIsTheRoleAndWhetherAnythingRan)
{
    std::string const dpsTrace =
        "|S:+cure|S:+cc|S:+aoe|S:+ranged position|S:+threat|S:-avoid aoe|S:+avoid aoe|S:+dungeon-escort|S:-breathe"
        "|S:+breathe|S:-dungeon-escort|S:+dungeon-escort|S:-dungeon-escort|S:+world-boss";
    std::string const otherDpsTrace =
        "|S:+cure|S:+cc|S:+aoe|S:+ranged position|S:+threat|S:-avoid aoe|S:+avoid aoe|S:+world-boss";
    EXPECT_EQ(Journal::Key::StuckTarget("dps", dpsTrace), Journal::Key::StuckTarget("dps", otherDpsTrace))
        << "two samples of the same deadlock gave two keys: the engine's trace is in the key";
    // and the other direction: an engine with nothing to run is the point of this problem, and a role is a cause
    EXPECT_NE(Journal::Key::StuckTarget("dps", dpsTrace), Journal::Key::StuckTarget("dps", ""));
    EXPECT_NE(Journal::Key::StuckTarget("tank", dpsTrace), Journal::Key::StuckTarget("dps", dpsTrace));
}
