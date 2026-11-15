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

// [jrn43] THE `idle` KEY AND THE HOLE IN ITS LIST OF CAUSES. Every window below is a real row of pb_journal_idle
// from b502, by its id, and `other` is the bucket for "the engine turned this tick's action down and the list of
// causes has no name for that". Since b502 the engine's own answer IS recorded (ref_*), and the breakdown sums
// exactly to `other` over the whole hour - 30 842 ticks against 30 842 - so it is a fact of the same instant.
//
// THE NUMBER THAT MAKES THIS THE WORSE DIRECTION: 109 keys over the 3 665 windows with a non-zero `other`, against
// 336 distinct refusal combinations inside them. One key, `idle|dps|gcd+other`, holds 306 windows of 19 bots and
// 14 of those combinations.
namespace
{
    // id 7708327910943569239, Tuihgrelfim, dps: 73 ticks, 7 casts, gcd 18, other 47 = useless 40 + failed 7
    Journal::Key::IdleBuckets UselessAndFailed()
    {
        Journal::Key::IdleBuckets b;
        b.gcd = 18;
        b.other = 47;
        b.refUseless = 40;
        b.refFailed = 7;
        return b;
    }

    // id 7708327910943281755, Feeli, dps: 48 ticks, 6 casts, gcd 6, other 26 = impossible 26
    Journal::Key::IdleBuckets ImpossibleOnly()
    {
        Journal::Key::IdleBuckets b;
        b.gcd = 6;
        b.other = 26;
        b.refImpossible = 26;
        return b;
    }

    // id 7708327910943329936, Idunk, dps: 49 ticks, 5 casts, gcd 15, other 25 = impossible 25 - another bot, another
    // window, the same cause as Feeli's above
    Journal::Key::IdleBuckets ImpossibleOnlyOtherBot()
    {
        Journal::Key::IdleBuckets b;
        b.gcd = 15;
        b.other = 25;
        b.refImpossible = 25;
        return b;
    }
}  // namespace

// THE DEFECT: two different refusals under one key. An action its own usefulness check turned down is not an action
// that could not be cast, and a fix for one of them would turn a test over this key green while the other lived on.
TEST(IssueKeyIdle, TheRefusalBehindOtherIsPartOfTheCause)
{
    EXPECT_NE(Journal::Key::Idle("dps", UselessAndFailed()), Journal::Key::Idle("dps", ImpossibleOnly()))
        << "two windows whose only idle buckets are gcd and `other` share one key although the engine named "
           "different refusals: `other` is not a cause, it is the hole where the cause goes";
}

// THE OTHER DIRECTION, and it is the one that matters more here: the refusal is a closed set of names the engine
// answers with, never a count and never a trace, so two windows of two bots with the same refusal are ONE key.
// Without this, a key per combination of counts would fan one cause into a staircase - trap #2.
TEST(IssueKeyIdle, TheSameRefusalInTwoWindowsIsOneKey)
{
    EXPECT_EQ(Journal::Key::Idle("dps", ImpossibleOnly()), Journal::Key::Idle("dps", ImpossibleOnlyOtherBot()))
        << "two windows with the same buckets and the same refusal gave two keys";
    // ... and the buckets and the role go on being causes, as they were
    EXPECT_NE(Journal::Key::Idle("healer", ImpossibleOnly()), Journal::Key::Idle("dps", ImpossibleOnly()));
    Journal::Key::IdleBuckets moving = ImpossibleOnly();
    moving.moving = 4;
    EXPECT_NE(Journal::Key::Idle("dps", moving), Journal::Key::Idle("dps", ImpossibleOnly()));
}

// A window with no `other` at all has no refusal to name, and its key must stay exactly what it was: the keys
// already measured on b502 go on meaning what they meant (the same rule the empty hold name follows).
TEST(IssueKeyIdle, AWindowWithoutOtherKeepsTheKeyItHad)
{
    Journal::Key::IdleBuckets plain;
    plain.gcd = 18;
    plain.moving = 3;
    EXPECT_EQ(Journal::Key::Idle("dps", plain), "idle|dps|moving+gcd");
}

// [jrn43] `stuck_target`: THE KEY SAID SOMETHING THAT CANNOT BE TRUE. The detector is called with the engine's own
// answer - ReleaseIdleTargetNobodyReaches(acted) returns at once when `acted` - so NO action ran in any row it ever
// writes, and the sentence in every row says exactly that ("with no action that runs"). The key, meanwhile, read a
// non-empty action TRACE as "an action ran": 121 of the 123 live occurrences carried that label. A row that
// contradicts itself answers a query, which is worse than a row that is missing.
TEST(IssueKeyStuckTarget, NoKeyClaimsAnActionRan)
{
    std::string const trace = "|A:claw - USELESS|A:melee - USELESS|no actions executed";
    for (std::string const& key : {Journal::Key::StuckTarget("healer", trace),
                                   Journal::Key::StuckTarget("dps", trace), Journal::Key::StuckTarget("tank", "")})
        EXPECT_EQ(key.find("an action ran"), std::string::npos)
            << "the key says an action ran: " << key
            << " - the detector only fires when the engine executed nothing";
}

// [jrn43] `wait_behind`: THE CAUSE FIELD COULD NOT DIFFER. The emitter's guard requires the member to be on the
// bot's own map, so "same map" was the answer in all 43 live rows and in every row the kind could ever write,
// which left 84 yd behind in one zone and 6 894 yd behind across the map sharing one row.
TEST(IssueKeyWaitBehind, TheCauseFieldCanComeOutBothWays)
{
    EXPECT_NE(Journal::Key::WaitBehind(true), Journal::Key::WaitBehind(false))
        << "the two values of the cause field give one key: it cannot come out false";
    EXPECT_EQ(Journal::Key::WaitBehind(true).find("map"), std::string::npos)
        << "the key is keyed on the map, which the emitter's own guard makes constant: "
        << Journal::Key::WaitBehind(true);
}

// [jrn43] `stale_victim`: WHICH REASONS ARE A PROBLEM. 178 of the 182 live rows are the mob the group had just
// killed, dropped on the first tick the drop was possible, with no clock and nothing waiting on it.
TEST(IssueRealStaleVictim, TheNormalEndOfAFightIsNotAProblem)
{
    EXPECT_FALSE(Journal::Real::StaleVictim("dead")) << "the victim the group had just killed is a problem";
    EXPECT_FALSE(Journal::Real::StaleVictim("gone"));
    EXPECT_FALSE(Journal::Real::StaleVictim("out of the map"));
    // and the two that stay real, plus an unknown reason: better to over-report than to miss
    EXPECT_TRUE(Journal::Real::StaleVictim("evading"));
    EXPECT_TRUE(Journal::Real::StaleVictim("an idle mob no pull order is for"));
    EXPECT_TRUE(Journal::Real::StaleVictim("a reason nobody has written yet"));
}

// [jrn43] THE ALERT THAT IS A SECOND ROW FOR ONE EVENT: 9 deaths in pb_journal_death, 9 DEATH alerts 1-2 ms after
// them. The other three kinds are honestly `unknown` - the funnel cannot tell whether the state under them is a
// defect - and must not be marked as duplicates of anything.
TEST(IssueRealAlert, OnlyTheDeathAlertRepeatsADomainRow)
{
    EXPECT_TRUE(Journal::Real::AlertKindDuplicatesDomainRow("DEATH"))
        << "the DEATH alert is counted as a problem of its own beside the death row it follows";
    EXPECT_TRUE(Journal::Real::AlertKindDuplicatesDomainRow("DEATHS"));
    EXPECT_FALSE(Journal::Real::AlertKindDuplicatesDomainRow("LOOP"));
    EXPECT_FALSE(Journal::Real::AlertKindDuplicatesDomainRow("STUCK"));
    EXPECT_FALSE(Journal::Real::AlertKindDuplicatesDomainRow("STUCK_COMBAT"));
}
