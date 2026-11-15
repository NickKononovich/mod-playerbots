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
    EXPECT_NE(Journal::Key::Idle("dps", UselessAndFailed(), true), Journal::Key::Idle("dps", ImpossibleOnly(), true))
        << "two windows whose only idle buckets are gcd and `other` share one key although the engine named "
           "different refusals: `other` is not a cause, it is the hole where the cause goes";
}

// THE OTHER DIRECTION, and it is the one that matters more here: the refusal is a closed set of names the engine
// answers with, never a count and never a trace, so two windows of two bots with the same refusal are ONE key.
// Without this, a key per combination of counts would fan one cause into a staircase - trap #2.
TEST(IssueKeyIdle, TheSameRefusalInTwoWindowsIsOneKey)
{
    EXPECT_EQ(Journal::Key::Idle("dps", ImpossibleOnly(), true),
              Journal::Key::Idle("dps", ImpossibleOnlyOtherBot(), true))
        << "two windows with the same buckets and the same refusal gave two keys";
    // ... and the buckets and the role go on being causes, as they were
    EXPECT_NE(Journal::Key::Idle("healer", ImpossibleOnly(), true), Journal::Key::Idle("dps", ImpossibleOnly(), true));
    Journal::Key::IdleBuckets moving = ImpossibleOnly();
    moving.moving = 4;
    EXPECT_NE(Journal::Key::Idle("dps", moving, true), Journal::Key::Idle("dps", ImpossibleOnly(), true));
}

// A window with no `other` at all has no refusal to name, and its key must stay exactly what it was: the keys
// already measured on b502 go on meaning what they meant (the same rule the empty hold name follows).
TEST(IssueKeyIdle, AWindowWithoutOtherKeepsTheKeyItHad)
{
    Journal::Key::IdleBuckets plain;
    plain.gcd = 18;
    plain.moving = 3;
    EXPECT_EQ(Journal::Key::Idle("dps", plain, true), "idle|dps|moving+gcd");
}

// [jrn45] WHERE IT IDLED IS A CAUSE, now that the window is measured outside instances too. Until this build the
// emitter returned at once unless the bot was in a group inside an instance (`in_combat` was 1 in all 3 923 windows
// of b502's hour, which is a fact about the gate and not about bots), so this field could not differ in any row the
// key ever wrote. A dungeon five idling on the global cooldown and a bot levelling alone in the world idling on it
// are two different defects.
TEST(IssueKeyIdle, TheSameBucketsInADungeonAndInTheWorldAreTwoKeys)
{
    Journal::Key::IdleBuckets plain;
    plain.gcd = 18;
    plain.moving = 3;
    EXPECT_NE(Journal::Key::Idle("dps", plain, true), Journal::Key::Idle("dps", plain, false));
    // ... and the dungeon key keeps its exact text, so the keys counted on b501 and b502 go on meaning what they
    // meant: the suffix is added only for the case that was never measured (the same rule the empty hold name of
    // Standing() follows).
    EXPECT_EQ(Journal::Key::Idle("dps", plain, true), "idle|dps|moving+gcd");
    EXPECT_EQ(Journal::Key::Idle("dps", plain, false), "idle|dps|moving+gcd|world");
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

// [jrn46] `run_failed`: THE RUN'S OWN RECORD. The four runs below are real rows of guild_project_runs on b502,
// the two `failed` ones being the biggest problem of that batch: wc-n five 1 and five 2 of the 10:30 certification
// wave ended at 3 of 8 bosses with NOT ONE death - their paladin tanks ran dry of water 8 min in (five 1: the
// in-run share gave the tank its last drink at 10:37:54 and the pool held 2 of the lines' 36 after it), and the
// leader held the pull "the tank rests" for a measured 1762 s of the 3033 s run. Four fives of the SAME wave with
// the same route finished 8 of 8.
namespace
{
    // guild_project_runs id 681 (five 1) and 679 (five 2): the same shape, the same dungeon, two different runs
    Journal::Key::RunFacts const WC_FIVE1_FAILED{"wc-n", "failed", 3, 8, 0, 0, 3033};
    Journal::Key::RunFacts const WC_FIVE2_FAILED{"wc-n", "failed", 3, 8, 0, 0, 2778};
    // id 686: stalled at 7 of 8 after 2745 s, and id 690: stopped by the server shutdown at 0 of 8 after 206 s
    Journal::Key::RunFacts const WC_STALLED{"wc-n", "stalled", 7, 8, 0, 0, 2745};
    Journal::Key::RunFacts const WC_STOPPED{"wc-n", "stopped", 0, 8, 0, 0, 206};
    // id 685: 8 of 8, no death, no wipe - the run that writes nothing
    Journal::Key::RunFacts const WC_DONE{"wc-n", "done", 8, 8, 0, 0, 2570};
}  // namespace

// The keys as the live table holds them, character for character: moving them into the seam must not re-fan the
// rows that already exist.
TEST(IssueKeyRunFailed, TheKeysAreTheOnesTheLiveTableHolds)
{
    EXPECT_EQ(Journal::Key::RunFailed(WC_FIVE1_FAILED), "run_failed|wc-n|failed|not done+boss left");
    EXPECT_EQ(Journal::Key::RunFailed(WC_FIVE2_FAILED), "run_failed|wc-n|failed|not done+boss left");
    EXPECT_EQ(Journal::Key::RunFailed(WC_STALLED), "run_failed|wc-n|stalled|not done+boss left");
    EXPECT_EQ(Journal::Key::RunFailed(WC_STOPPED), "run_failed|wc-n|stopped|not done+boss left");
    // one problem, not one row per run: the two failed runs of the same wave share their key (occurrences = 2)
    EXPECT_EQ(Journal::Key::RunFailed(WC_FIVE1_FAILED), Journal::Key::RunFailed(WC_FIVE2_FAILED));
    // and a result that differs is a different problem, so the shutdown never merges with a failure
    EXPECT_NE(Journal::Key::RunFailed(WC_STOPPED), Journal::Key::RunFailed(WC_FIVE1_FAILED));
    EXPECT_TRUE(Journal::Key::RunClean(WC_DONE)) << "a run that finished 8 of 8 with no death writes no problem";
    EXPECT_FALSE(Journal::Key::RunClean(WC_FIVE1_FAILED));
    EXPECT_FALSE(Journal::Key::RunClean({"wc-n", "done", 8, 8, 1, 0, 2570})) << "a death is a problem of its own";
}

// THE RANGED VALUE MUST BE A QUANTITY THE KEY LEAVES FREE. The shape carries whether there were deaths, so for
// `not done+boss left` the deaths are 0 by construction - a range of 0..0 in every row that shape can write.
TEST(IssueKeyRunFailed, TheRangedValueCanComeOutDifferentUnderItsOwnKey)
{
    Journal::Key::RangedValue const five1 = Journal::Key::RunFailedValue(WC_FIVE1_FAILED);
    Journal::Key::RangedValue const stalled = Journal::Key::RunFailedValue(WC_STALLED);
    Journal::Key::RangedValue const stopped = Journal::Key::RunFailedValue(WC_STOPPED);
    // the five bosses wc-n five 1 left standing, not the zero deaths it cost
    EXPECT_EQ(five1.value, 5.0) << "the row ranged over " << five1.unit << ", which its own key had already fixed";
    EXPECT_EQ(five1.unit, "boss");
    // and under ONE key the value does come out different: 5 bosses left, 1, 8
    EXPECT_EQ(stalled.value, 1.0);
    EXPECT_EQ(stopped.value, 8.0);
    EXPECT_EQ(Journal::Key::RunFailedValue(WC_FIVE2_FAILED).value, 5.0);

    // a shape WITHOUT `boss left` leaves the deaths free, and then they are what the row ranges over
    Journal::Key::RunFacts twoDeaths{"wc-n", "done", 8, 8, 2, 0, 2570};
    EXPECT_EQ(Journal::Key::RunFailedValue(twoDeaths).value, 2.0);
    EXPECT_EQ(Journal::Key::RunFailedValue(twoDeaths).unit, "death");
    // nor may the wipes be ranged over by a shape that already fixed them at zero
    Journal::Key::RunFacts wiped{"wc-n", "done", 8, 8, 0, 3, 2570};
    EXPECT_EQ(Journal::Key::RunFailedValue(wiped).value, 3.0);
    EXPECT_EQ(Journal::Key::RunFailedValue(wiped).unit, "wipe");
    // and a run that failed with every boss down and nobody dead: its seconds are all the shape leaves open
    Journal::Key::RunFacts stoppedClean{"wc-n", "stopped", 8, 8, 0, 0, 940};
    EXPECT_EQ(Journal::Key::RunFailedValue(stoppedClean).value, 940.0);
    EXPECT_EQ(Journal::Key::RunFailedValue(stoppedClean).unit, "s");
}

// ---------------------------------------------------------------------------------------------------------------
// [supply130] THE FIVE'S DRINK: THE GATE AND THE SHARE OF A SHORT POOL (Guild::Supply, SupplyShare.h).
//
// THESE TESTS LIVE IN THIS FILE AND NOT IN A SUPPLYSHARE TEST OF THEIR OWN because tests/CMakeLists.txt was being
// edited by another agent at the time and two agents may not touch one file here; the stand registers this file
// already. Move them to tests/unit/SupplyShareTest.cpp as soon as that list is free - nothing about them depends
// on this file.
//
// The rows are wc-n five 1 of the certification batch (run 681), as [dun208] printed them at 10:30:34.
// ---------------------------------------------------------------------------------------------------------------
// by path, not by an include directory, for the same reason: the stand's directory list is in that same file
#include "../../src/Ai/Guild/SupplyShare.h"

namespace
{
    using Guild::Supply::Member;
    using Guild::Supply::Role;

    // "pool 29 of the lines' 36, short, [dun230] the healer Rumanatol first: Bami 6/7, Rumanatol 3/2, Orton 4/11,
    // Thamesh 10/10, Arrengyl 6/6". Orton and Thamesh are mages of level 26 and 25: both conjure water.
    std::vector<Member> const WC_FIVE1_AT_THE_GATE = {{6, 7, Role::Tank, false},
                                                      {3, 2, Role::Healer, false},
                                                      {4, 11, Role::Other, true},
                                                      {10, 10, Role::Other, true},
                                                      {6, 6, Role::Other, false}};

    // the same five 13 minutes later, at "pool 3 of the lines' 36": the tank dry, the healer at its line
    std::vector<Member> const WC_FIVE1_POOL_THREE = {{0, 7, Role::Tank, false},
                                                     {2, 2, Role::Healer, false},
                                                     {0, 11, Role::Other, true},
                                                     {1, 10, Role::Other, true},
                                                     {0, 6, Role::Other, false}};
}  // namespace

// THE ORDER IS NOT GIVEN WHILE THE POOL IS SHORT OF THE REQUIREMENT, and the requirement is the run's: every mana
// user, its full priced line, the conjurers among them.
TEST(SupplyShareGate, AConjurerIsNotExcusedFromTheFivesOwnRequirement)
{
    EXPECT_EQ(Guild::Supply::PoolNeed(WC_FIVE1_AT_THE_GATE), 36u)
        << "the gate asked for less than the run needs, by exactly the conjuring mages' lines";
    EXPECT_EQ(Guild::Supply::PoolDrinks(WC_FIVE1_AT_THE_GATE), 29u)
        << "a drink the pool does not count is a drink it cannot hand to anybody";
    // 29 of 36: the five waits, its mages conjure into their bags, and the pool the gate reads rises
    EXPECT_FALSE(Guild::Supply::Pooled(WC_FIVE1_AT_THE_GATE, true))
        << "five 1 was let in on 15 against 15 and its tank ran dry 8 min in";
    // and the same five once the water is actually held: both mages conjured up to their lines
    std::vector<Member> conjured = WC_FIVE1_AT_THE_GATE;
    conjured[0].held = 7;
    conjured[2].held = 11;
    conjured[3].held = 10;
    EXPECT_TRUE(Guild::Supply::Pooled(conjured, true));
    // no free bag slot for a stack is the hand-over's own fact and holds the five on its own ([supply104])
    EXPECT_FALSE(Guild::Supply::Pooled(conjured, false));
}

// THE SHARE OF A SHORT POOL GOES TO THE TANK BEFORE THE DPS: the leader holds the pull on the tank's mana.
TEST(SupplyShareShortPool, TheTankIsFilledBeforeTheDps)
{
    std::vector<uint32_t> const three = Guild::Supply::ShortPoolTargets(WC_FIVE1_POOL_THREE, 3);
    ASSERT_EQ(three.size(), 5u);
    EXPECT_EQ(three[1], 2u) << "the healer keeps its line first ([dun230])";
    EXPECT_EQ(three[0], 1u) << "the last drink of the pool went to a dps while the tank held 0 of its 7";
    EXPECT_EQ(three[2], 0u);
    EXPECT_EQ(three[3], 0u);
    EXPECT_EQ(three[4], 0u);
    // the share always hands out the whole pool and never more
    uint32_t sum = 0;
    for (uint32_t t : three)
        sum += t;
    EXPECT_EQ(sum, 3u);

    // a bigger short pool: the healer's 2 and the tank's 7 first, the remaining 20 in proportion to 11, 10 and 6
    std::vector<uint32_t> const twentyNine = Guild::Supply::ShortPoolTargets(WC_FIVE1_AT_THE_GATE, 29);
    EXPECT_EQ(twentyNine[1], 2u);
    EXPECT_EQ(twentyNine[0], 7u) << "the tank is filled to its line while the pool can still do it";
    uint32_t rest = 0;
    for (uint32_t t : twentyNine)
        rest += t;
    EXPECT_EQ(rest, 29u);
    // nobody is left at nothing while the pool holds one a member
    EXPECT_GT(twentyNine[2], 0u);
    EXPECT_GT(twentyNine[3], 0u);
    EXPECT_GT(twentyNine[4], 0u);
}

// ---------------------------------------------------------------------------------------------------------------
// [jrn47] `combat_spot_none`: ONE CAUSE IS ONE ROW, BOTH WAYS. Every line below is a real [lead463] line of b502,
// out of 387 in one window. The key is the role plus the SET of checks that refused, and the numbers that make
// the other candidates for a key wrong are in the comment of IssueKey.h: 219 keys on the printed line, 154 on the
// reasons with their counts, 91 on the mob's name beside the reasons (over 16 names), 77 on the (stage, reason)
// pairs - against 52 reason sets.
// ---------------------------------------------------------------------------------------------------------------
namespace
{
    // the healer family, 55 of the 387: the band and the heal reach refuse every candidate
    std::vector<std::string> const HEALER_REASONS = {"heal reach", "out of the healer's band"};
    std::string const AEVMAE_TRACE = "near: heal reach x43; near: inside its minimum gap to the target x6; "
                                     "ring: heal reach x15; ring: out of the healer's band x33; "
                                     "walk: out of the healer's band x8";
    // the same cause for another healer on another mob, with other counts and one other stage
    std::string const PHERUA_TRACE = "near: heal reach x49; ring: heal reach x25; "
                                     "ring: out of the healer's band x23; walk: heal reach x3; "
                                     "walk: out of the healer's band x9";
    // the melee family, 189 of the 387 (the largest single cause): every melee place at the target lies inside an
    // idle mob's aggro reach
    std::vector<std::string> const MELEE_REASONS = {"out of melee reach of the target", "an idle mob's aggro reach",
                                                    "no melee place in reach and sight in front of the target",
                                                    "out of melee reach"};
    std::string const TUIHGRELFIM_TRACE = "at target: an idle mob's aggro reach x16; "
                                          "front: no melee place in reach and sight in front of the target x1; "
                                          "near: out of melee reach of the target x49; "
                                          "ring: an idle mob's aggro reach x7; tank's spot: out of melee reach x1";
}  // namespace

TEST(IssueKeyCombatSpotNone, OneCauseIsOneKeyWhateverTheCountsAndTheMob)
{
    // Aevmae on Deviate Adder and Pherua on Deviate Lasher: the same two checks refused, so ONE problem
    EXPECT_EQ(Journal::Key::CombatSpotNone("healer", HEALER_REASONS, AEVMAE_TRACE),
              Journal::Key::CombatSpotNone("healer", HEALER_REASONS, PHERUA_TRACE))
        << "the counts and the stages of one cause made two rows of it";
    // and the order the reasons were gathered in is no cause either
    std::vector<std::string> reversed(HEALER_REASONS.rbegin(), HEALER_REASONS.rend());
    EXPECT_EQ(Journal::Key::CombatSpotNone("healer", HEALER_REASONS, AEVMAE_TRACE),
              Journal::Key::CombatSpotNone("healer", reversed, PHERUA_TRACE));
    // a repeated reason is the same set (the same check refusing at two stages)
    std::vector<std::string> twice = HEALER_REASONS;
    twice.push_back("heal reach");
    EXPECT_EQ(Journal::Key::CombatSpotNone("healer", twice, AEVMAE_TRACE),
              Journal::Key::CombatSpotNone("healer", HEALER_REASONS, AEVMAE_TRACE));
    // nothing of the trace, and no count, reaches the key
    std::string const key = Journal::Key::CombatSpotNone("healer", HEALER_REASONS, AEVMAE_TRACE);
    EXPECT_EQ(key.find("x43"), std::string::npos) << key;
    EXPECT_EQ(key.find("near"), std::string::npos) << "the stage belongs to the child row, not to the key: " << key;
    for (char const c : key)
        EXPECT_FALSE(c >= '0' && c <= '9') << "a number in the key: " << key;
}

TEST(IssueKeyCombatSpotNone, TwoCausesAreTwoKeys)
{
    // the healer's band against the melee geometry: one fix for either must not turn a test green over the other
    EXPECT_NE(Journal::Key::CombatSpotNone("healer", HEALER_REASONS, AEVMAE_TRACE),
              Journal::Key::CombatSpotNone("dps", MELEE_REASONS, TUIHGRELFIM_TRACE));
    // the role is part of the cause: the same refusal means a different search for a healer and for a dps
    EXPECT_NE(Journal::Key::CombatSpotNone("healer", HEALER_REASONS, AEVMAE_TRACE),
              Journal::Key::CombatSpotNone("dps", HEALER_REASONS, AEVMAE_TRACE));
    // and one check more is one cause more: the idle mob's reach beside the band is not the band alone
    std::vector<std::string> withIdle = HEALER_REASONS;
    withIdle.push_back("an idle mob's aggro reach");
    EXPECT_NE(Journal::Key::CombatSpotNone("healer", withIdle, AEVMAE_TRACE),
              Journal::Key::CombatSpotNone("healer", HEALER_REASONS, AEVMAE_TRACE));
}
