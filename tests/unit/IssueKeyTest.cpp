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

// [supply131] THE GROUP GRAPH, AND THE MEASUREMENT THAT CHOSE IT. Inputs are b504's unheld standing samples:
// the largest key `standing|dps|peace|state 1` held 1956 of them over 43 bots, 1284 out of any group and 672 in
// one, and the 2601 s of #42 Cassand and #44 Blahadrin - which blocked a whole certification batch of wc-n - were
// the out-of-group nature. Two natures under one name, and a fix for either would have turned a test green over
// the other.
TEST(IssueKeyStanding, StandingWithNoGroupAndStandingInOneAreTwoKeys)
{
    std::string const trace = KYLANTINO_EPISODE.front();
    EXPECT_NE(Journal::Key::Standing("dps", false, 1, "", trace, false),
              Journal::Key::Standing("dps", false, 1, "", trace, true))
        << "a member standing with no group at all shares a key with a member standing in a group";
}

// THE DECIDING OTHER DIRECTION: "split on everything" would pass the test above and give us a staircase instead
// of a deduplication. Two samples of ONE nature must stay ONE key however the engine's trace moves under them -
// that is what the graph was measured for (in_group turned over inside 5 of 686 episodes, 0.7 %, while "the
// engine executed nothing" turned over inside 181 of 686, 26.4 %, and is therefore a column and never a key).
TEST(IssueKeyStanding, TwoSamplesOfOneNatureAreStillOneKey)
{
    std::set<std::string> alone, grouped;
    for (std::string const& trace : KYLANTINO_EPISODE)
    {
        alone.insert(Journal::Key::Standing("dps", false, 1, "", trace, false));
        grouped.insert(Journal::Key::Standing("dps", false, 1, "", trace, true));
    }
    EXPECT_EQ(alone.size(), 1u) << "six samples of one out-of-group episode gave " << alone.size() << " keys";
    EXPECT_EQ(grouped.size(), 1u) << "six samples of one in-group episode gave " << grouped.size() << " keys";
}

// ... and the counts already measured keep meaning what they meant: the graph is named by the whole and never
// instead of it. An emitter that does not hold the fact, and the in-group case, both write the key unchanged -
// the rule of [jrn45]'s place suffix and [jrn54]'s walking part.
TEST(IssueKeyStanding, TheKeysMeasuredBeforeTheGraphAreUnchangedByteForByte)
{
    std::string const trace = KYLANTINO_EPISODE.front();
    // the four live texts of b502-b504 this kind was counted on
    EXPECT_EQ(Journal::Key::Standing("dps", false, 1, "", trace), "standing|dps|peace|state 1");
    EXPECT_EQ(Journal::Key::Standing("healer", true, 0, "", trace), "standing|healer|combat|state 0");
    EXPECT_EQ(Journal::Key::Standing("healer", false, 0, "waits for its group", trace),
              "standing|healer|peace|state 0|held: waits for its group");
    EXPECT_EQ(Journal::Key::Standing("dps", false, 0, "dead", trace), "standing|dps|peace|state 0|held: dead");
    // unstated and in-group are the same key as before; only the out-of-group case adds its graph
    EXPECT_EQ(Journal::Key::Standing("dps", false, 1, "", trace, std::nullopt), "standing|dps|peace|state 1");
    EXPECT_EQ(Journal::Key::Standing("dps", false, 1, "", trace, true), "standing|dps|peace|state 1");
    EXPECT_EQ(Journal::Key::Standing("dps", false, 1, "", trace, false), "standing|dps|peace|state 1|alone");
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

// [jrn54] WALKING AND STANDING WERE ONE KEY, AND THAT WAS THE LAST MERGED CAUSE IN THE TABLE. `noTarget` was the
// largest of b503's ten unexplained dungeon causes - 548 of 1 435 cases - and it was not one cause: the bucket
// chain asked `!target` and then "out of reach or sight" BEFORE `isMoving()`, so a bot WALKING to the next pack
// was counted as "it had no target" and a bot RUNNING AT its mob as "out of range", while the `moving` bucket
// could only ever catch the residue where the target was already in reach and in sight (6 002 ticks of the
// build's 96 070 dungeon combat ticks, 6.2 %, against 46 119 in `noTarget` and 14 851 in `rangeLos`).
//
// Measured from the positions the table already carried (LAG over the same bot on the same map), of the 548
// noTarget-dominant problem windows 373 (68.1 %) belong to a bot that covered 20 yd or more during the window -
// average displacement 32.4 yd - and only 48 (8.8 %) to one that stood inside 5 yd with no named hold. One key
// over the two means a fix for either turns a test green while the other lives on beneath it, which is the wide
// direction this seam exists to assert; [jrn52] added the counts (no_target_moving, range_los_moving) and this is
// the key catching up with them.
//
// BOTH DIRECTIONS ARE ASSERTED HERE, and the second is the one that would let a careless "split everything" pass:
// two windows of the SAME nature must stay ONE key, whatever their counts, or the split becomes trap #2 again.
namespace
{
    // a dps that walked through almost the whole window with nothing to attack yet - the 68.1 % shape
    Journal::Key::IdleBuckets WalkedWithNothingToAttack()
    {
        Journal::Key::IdleBuckets b;
        b.noTarget = 28;
        b.noTargetMoving = 25;
        b.gcd = 3;
        return b;
    }

    // the same shape in another window of another bot: different counts, same nature
    Journal::Key::IdleBuckets WalkedWithNothingToAttackOtherBot()
    {
        Journal::Key::IdleBuckets b;
        b.noTarget = 54;
        b.noTargetMoving = 41;
        b.gcd = 9;
        return b;
    }

    // a dps that STOOD with nothing to attack - the 8.8 % shape, and the only one of the two that is a defect of
    // idleness rather than a question about a walk
    Journal::Key::IdleBuckets StoodWithNothingToAttack()
    {
        Journal::Key::IdleBuckets b;
        b.noTarget = 28;
        b.gcd = 3;
        return b;
    }

    Journal::Key::IdleBuckets RanAtATargetOutOfReach()
    {
        Journal::Key::IdleBuckets b;
        b.rangeLos = 20;
        b.rangeLosMoving = 18;
        return b;
    }

    Journal::Key::IdleBuckets StoodShortOfATargetOutOfReach()
    {
        Journal::Key::IdleBuckets b;
        b.rangeLos = 20;
        return b;
    }
}  // namespace

TEST(IssueKeyIdle, AWindowSpentWalkingAndAWindowSpentStandingAreTwoKeys)
{
    EXPECT_NE(Journal::Key::Idle("dps", WalkedWithNothingToAttack(), true),
              Journal::Key::Idle("dps", StoodWithNothingToAttack(), true))
        << "a bot walking to the next pack and a bot standing with nothing to attack share one key: a fix for "
           "either would turn a test green while the other lived on under it";
    EXPECT_NE(Journal::Key::Idle("dps", RanAtATargetOutOfReach(), true),
              Journal::Key::Idle("dps", StoodShortOfATargetOutOfReach(), true))
        << "running at a mob and stopping short of one share one key";
}

// THE OTHER DIRECTION. Which buckets fired is the cause; how many times is the value range, as it has been since
// [jrn40]. Two windows of the same nature and different counts are ONE problem.
TEST(IssueKeyIdle, TwoWindowsOfTheSameNatureAreStillOneKey)
{
    EXPECT_EQ(Journal::Key::Idle("dps", WalkedWithNothingToAttack(), true),
              Journal::Key::Idle("dps", WalkedWithNothingToAttackOtherBot(), true))
        << "two windows of one nature with different counts gave two keys - the staircase this project has "
           "already measured twice";
}

// A window with NO walking part keeps its key character for character: the counts taken on b501, b502 and b503 go
// on meaning what they meant, exactly as [jrn45] added its place suffix only for the case that had never been
// measured. This is the assertion that makes the split safe to compare across builds.
TEST(IssueKeyIdle, AWindowWithoutAWalkingPartKeepsTheKeyItHad)
{
    EXPECT_EQ(Journal::Key::Idle("dps", StoodWithNothingToAttack(), true), "idle|dps|noTarget+gcd");
    EXPECT_EQ(Journal::Key::Idle("tank", StoodShortOfATargetOutOfReach(), false), "idle|tank|rangeLos|world");
    // and the walking part is named after the bucket it is a part of, never in place of it
    EXPECT_EQ(Journal::Key::Idle("dps", WalkedWithNothingToAttack(), true), "idle|dps|noTarget+noTargetMoving+gcd");
    Journal::Key::IdleBuckets onlyWalked;
    onlyWalked.noTarget = 25;
    onlyWalked.noTargetMoving = 25;
    EXPECT_EQ(Journal::Key::Idle("dps", onlyWalked, true), "idle|dps|noTargetMoving");
}

// THE PART CANNOT EXCEED THE WHOLE, and if an emitter ever passes numbers that say it does, the key must not read
// an unsigned wrap-around as "it also stood for four billion ticks". Absence is not zero and nonsense is not a
// count: the standing part of such a window is nothing, and the walking part is what was passed.
TEST(IssueKeyIdle, AWalkingPartLargerThanItsWholeDoesNotInventAStandingPart)
{
    Journal::Key::IdleBuckets impossible;
    impossible.noTarget = 5;
    impossible.noTargetMoving = 9;
    impossible.rangeLos = 2;
    impossible.rangeLosMoving = 7;
    EXPECT_EQ(Journal::Key::Idle("dps", impossible, true), "idle|dps|noTargetMoving+rangeLosMoving");
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

// [jrn52] THE ENGINE'S OWN WORDS FOR "I NAMED NOTHING" WERE KEYED AS "IT NAMED ACTIONS". The trail is not a
// property of this event at all: Engine::LogAction (src/Bot/Engine/Engine.cpp:932-939) never clears lastAction, so
// it is a cross-tick trail of up to 512 characters, and it is empty only when the engine has logged nothing since
// the bot logged in. Measured on b503's 199 live rows of this kind: 5 rows carry an empty trail, and bot 1814 is
// among them with 3 empty-trail rows beside 24 rows of its own with a full trail - so emptiness is a fact about
// that bot's login, not about this stuck target. Meanwhile 10 rows (5 dps, 5 healer) carry a trail that is exactly
// Engine's own sentence for having named nothing - "no actions executed", src/Bot/Engine/Engine.cpp:367 - and the
// key filed all ten under "its engine named actions and none ran". One fact, two keys, and the label on the larger
// of the two says the opposite of the row it labels.
//
// WHAT IS NOT CLAIMED HERE: a trail of "|A:reach spell - USELESS|no actions executed" DID name an action, and it
// keeps the label it has. Only Engine's bare sentence, alone, means no action was named.
TEST(IssueKeyStuckTarget, TheEnginesOwnWordForNamingNothingIsNotNamingActions)
{
    EXPECT_EQ(Journal::Key::StuckTarget("dps", "|no actions executed"), Journal::Key::StuckTarget("dps", ""))
        << "the engine named nothing and the key says it named actions: "
        << Journal::Key::StuckTarget("dps", "|no actions executed");
    EXPECT_EQ(Journal::Key::StuckTarget("healer", "no actions executed"), Journal::Key::StuckTarget("healer", ""))
        << "the same sentence without the leading separator must read the same";
    // and the other direction stays: a trail that does name an action is a different key
    EXPECT_NE(Journal::Key::StuckTarget("dps", "|A:reach spell - USELESS|no actions executed"),
              Journal::Key::StuckTarget("dps", ""))
        << "a named action was folded into the label for naming none";
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
// [supply130] THE FIVE'S DRINK: THE GATE AND THE SHARE OF A SHORT POOL (GuildSupply, SupplyShare.h).
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
    using GuildSupply::Member;
    using GuildSupply::Role;

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
    EXPECT_EQ(GuildSupply::PoolNeed(WC_FIVE1_AT_THE_GATE), 36u)
        << "the gate asked for less than the run needs, by exactly the conjuring mages' lines";
    EXPECT_EQ(GuildSupply::PoolDrinks(WC_FIVE1_AT_THE_GATE), 29u)
        << "a drink the pool does not count is a drink it cannot hand to anybody";
    // 29 of 36: the five waits, its mages conjure into their bags, and the pool the gate reads rises
    EXPECT_FALSE(GuildSupply::Pooled(WC_FIVE1_AT_THE_GATE, true))
        << "five 1 was let in on 15 against 15 and its tank ran dry 8 min in";
    // and the same five once the water is actually held: both mages conjured up to their lines
    std::vector<Member> conjured = WC_FIVE1_AT_THE_GATE;
    conjured[0].held = 7;
    conjured[2].held = 11;
    conjured[3].held = 10;
    EXPECT_TRUE(GuildSupply::Pooled(conjured, true));
    // no free bag slot for a stack is the hand-over's own fact and holds the five on its own ([supply104])
    EXPECT_FALSE(GuildSupply::Pooled(conjured, false));
}

// THE SHARE OF A SHORT POOL GOES TO THE TANK BEFORE THE DPS: the leader holds the pull on the tank's mana.
TEST(SupplyShareShortPool, TheTankIsFilledBeforeTheDps)
{
    std::vector<uint32_t> const three = GuildSupply::ShortPoolTargets(WC_FIVE1_POOL_THREE, 3);
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
    std::vector<uint32_t> const twentyNine = GuildSupply::ShortPoolTargets(WC_FIVE1_AT_THE_GATE, 29);
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

// ---------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------
// [spot90] AN IDLE MOB'S REACH AND A PLACE TO FIGHT FROM. The inputs are the 257 melee events of b502 (Januel 62,
// Ginde 58, Tuihgrelfim 34 the most frequent): in every one of them the `at target` stage refused all 16 places
// round the target as "an idle mob's aggro reach", 4 067 candidates, and in 252 of the 257 that was its only
// refusal there.
//
// EVERY COMBINATION, and the ones that must keep BLOCKING are the point: the first version of this seam read
// "taken into the fight" alone and would have let a melee member step into the reach of a joined mob the tank had
// still to open on - which is [lead1140]'s case, measured on b239 as 49 of 92 "the tank never had it". A rule
// that only ever says yes is the same defect as the one it replaced, wearing the other sign.
// ---------------------------------------------------------------------------------------------------------------
#include "../../src/Ai/Leader/Fight/IdlePlace.h"

namespace
{
    using Leader::Fight::IdlePlace::Blocks;
    using Facts = Leader::Fight::IdlePlace::Facts;
}  // namespace

TEST(IdlePlaceReach, AMobTheFightHasTakenForbidsNoPlace)
{
    // the tank has it already (it is in combat, so nothing is "still to open"): the wanting is the decision
    EXPECT_FALSE(Blocks({true, true, false, false}))
        << "a place was refused for the reach of a mob the fight had already taken and the tank had got";
    // and the tank itself is never kept off the mob it pulls
    EXPECT_FALSE(Blocks({true, true, true, true}));
}

TEST(IdlePlaceReach, AMobNobodyDecidedToFightStillForbidsThePlace)
{
    // the same geometry with no such decision: "do not step into a mob's reach without wanting it" stands
    EXPECT_TRUE(Blocks({true, false, false, false}))
        << "a melee member would step into the reach of a mob nobody decided to fight";
    EXPECT_TRUE(Blocks({true, false, true, false}));
    // the tank is no exception while nobody has decided to take the mob
    EXPECT_TRUE(Blocks({true, false, false, true}));
}

TEST(IdlePlaceReach, AJoinedMobTheTankHasStillToOpenOnKeepsTheOthersOut)
{
    // [lead1140] b239: 49 of 92 "the tank never had it" were a member walking into a joined pack before the
    // tank's opener. The mob is taken into the fight AND still idle, so a member other than the tank wakes it
    EXPECT_TRUE(Blocks({true, true, true, false}))
        << "a member other than the tank would wake the joined pack before the tank's opener";
}

TEST(IdlePlaceReach, AReachThatDoesNotCoverThePlaceWasNeverTheQuestion)
{
    for (bool taken : {false, true})
        for (bool toOpen : {false, true})
            for (bool isTank : {false, true})
                EXPECT_FALSE(Blocks({false, taken, toOpen, isTank}))
                    << "a mob whose reach does not cover the place refused it";
}

// ---------------------------------------------------------------------------------------------------------------
// [pick90] AND THE OTHER HALF OF THE SAME EVENT: the target the group cannot stand at must not be CHOSEN.
//
// Live numbers (pb_journal_spot and the problems table over the same window): 66 of 107 `combat_spot_none` events
// are a melee member whose `at target` stage refused all 16 places for an idle mob's aggro reach and for nothing
// else; 64 of the 71 rows that named the blocker carry `idle_taken = 0` (90 %) - nobody had decided to fight it.
// Four Deviate Lashers stand 0.3-8.5 yd from the target (acore_world), and in 9 of 9 cases the fallback to the
// tank's own place was impossible too (the tank was out of melee reach of the target). So the member stands.
//
// Both directions are asserted, because "refuse every such pick" is the easy half-fix and it would make a five
// skip a boss (rule 11) and would also drop a target whose blocking pack the fight has already taken.
// ---------------------------------------------------------------------------------------------------------------
namespace
{
    using Leader::Fight::IdlePlace::PickRefused;
    using PickFacts = Leader::Fight::IdlePlace::PickFacts;

    // the Deviate Lasher case: a melee member with no place, the blocker idle, its pack not joined, no boss
    PickFacts DeviateLasherRoundTheTarget()
    {
        PickFacts facts;
        facts.meleeHasNoPlaceForTheIdleReach = true;
        facts.blockerStillIdle = true;
        facts.blockerPackNotJoined = true;
        facts.targetIsABoss = false;
        return facts;
    }
}  // namespace

TEST(IdlePlacePick, ATargetNoMeleeCanStandAtIsNotChosen)
{
    // THE RED ONE: before this the pick stood, the melee member found no place every tick, and the only answer
    // was the place rule saying "no" over and over - 66 of 107 events
    EXPECT_TRUE(PickRefused(DeviateLasherRoundTheTarget()))
        << "the leader kept a target whose every melee place lies in an unfought pack's reach";
}

TEST(IdlePlacePick, APackTheFightHasTakenLeavesThePickAlone)
{
    // the join is the other answer the user named, and it is the mechanism that already exists
    // (judgeBlockingPack / PullPlanner::JudgePack): once the pack is in the fight, the places exist again and the
    // pick must NOT be dropped - otherwise the gate would refuse the very targets it has just made reachable
    PickFacts joined = DeviateLasherRoundTheTarget();
    joined.blockerPackNotJoined = false;
    EXPECT_FALSE(PickRefused(joined));
}

TEST(IdlePlacePick, ABossIsNeverDropped)
{
    // rule 11: no five skips a boss. Unreachable means the navmesh or the plan is fixed, never that the boss is
    // left standing - so every fact above can hold and the pick still stands
    PickFacts boss = DeviateLasherRoundTheTarget();
    boss.targetIsABoss = true;
    EXPECT_FALSE(PickRefused(boss));
}

TEST(IdlePlacePick, NothingIsRefusedWithoutTheMeasuredRefusal)
{
    // the pick stands while the place search has not said "no place at all for the idle reach", and while the
    // blocker is no longer there to block: a refusal that outlives its own cause is the defect this replaced
    PickFacts hasPlace = DeviateLasherRoundTheTarget();
    hasPlace.meleeHasNoPlaceForTheIdleReach = false;
    EXPECT_FALSE(PickRefused(hasPlace));

    PickFacts gone = DeviateLasherRoundTheTarget();
    gone.blockerStillIdle = false;
    EXPECT_FALSE(PickRefused(gone));
}

// ---------------------------------------------------------------------------------------------------
// [supply131] THE FIVE'S WAIT FOR A MEMBER'S SUPPLY RUN. Inputs are the real rows of b504 named in
// SupplyWait.h: five 2 of the wc-n certification batch, #42 Cassand and #44 Blahadrin standing 2601 s
// each at 0.0 yd with pb_journal_idle.held_by empty, their task label frozen at "waits for its supply
// run before the entrance" through all 45 min, and Cassand's one live vendor run named by its own task
// line as "BUY ... Glorious Shoulders for itself - vendor Nizzik 3357 yd".
//
// These tests live here and not in a file of their own because tests/CMakeLists.txt is held by another
// agent in this same tree (two agents never edit one file); the seam is header-only, so the include
// below is the whole wiring. MOVE TO tests/unit/SupplyWaitTest.cpp once the stand's list is free.
#include "../../src/Ai/Guild/SupplyWait.h"

namespace
{
    // Cassand as b504 recorded it: its five asked it to restock, the wait never concluded, a vendor run
    // IS live - and it is the run for a pair of shoulders, not for its drink
    GuildSupplyWait::Facts CassandAtTheGate()
    {
        GuildSupplyWait::Facts f;
        f.asked = true;
        f.done = false;
        f.started = false;
        f.yielded = true;  // the wait had already given it its one pass
        f.restockRunLive = GuildSupplyWait::IsRestock("buy Glorious Shoulders for itself");
        return f;
    }
}  // namespace

TEST(SupplyWait, AVendorRunForSomethingElseIsNotTheMembersRestock)
{
    using namespace GuildSupplyWait;
    EXPECT_EQ(ReasonFor("buy drink"), Kind::Drink);
    EXPECT_EQ(ReasonFor("sell junk + buy food"), Kind::Food);
    // the three live runs b504 held fives on, none of them a restock
    EXPECT_EQ(ReasonFor("buy Glorious Shoulders for itself"), Kind::None) << "the shoulders run at Nizzik 3357 yd";
    EXPECT_EQ(ReasonFor("repair"), Kind::None);
    EXPECT_EQ(ReasonFor("sell junk"), Kind::None);
    EXPECT_FALSE(IsRestock("buy Glorious Shoulders for itself"));
}

TEST(SupplyWait, AWaitOnARunThatIsNotTheRestockEnds)
{
    // the defect: five 2 stood at the gate on a flag that is set by ANY vendor run, so the wait had no
    // end. 2601 s standing, 0.0 yd, and FiveStep's own conclusion line never written for either member
    EXPECT_EQ(GuildSupplyWait::Of(CassandAtTheGate()), GuildSupplyWait::Verdict::Concluded)
        << "a run for a pair of shoulders 3357 yd away held a certification batch at the entrance for 45 min";
}

TEST(SupplyWait, ARealRestockRunStillHoldsTheFive)
{
    // the other side: "end the wait always" would pass the test above and break the design. A member
    // actually on its way to buy its drink keeps its five at the gate, exactly as before
    GuildSupplyWait::Facts onItsWay = CassandAtTheGate();
    onItsWay.restockRunLive = GuildSupplyWait::IsRestock("goes to vendor Gruham Rumdnul (397 yd): buy drink");
    EXPECT_EQ(GuildSupplyWait::Of(onItsWay), GuildSupplyWait::Verdict::Running);
}

TEST(SupplyWait, TheFirstPassStillYieldsSoARunMayStart)
{
    // and "end the wait at once" would break the two-pass shape the rule is written in: the first pass
    // yields, and only a second pass with nothing live concludes
    GuildSupplyWait::Facts firstPass = CassandAtTheGate();
    firstPass.yielded = false;
    EXPECT_EQ(GuildSupplyWait::Of(firstPass), GuildSupplyWait::Verdict::Yield);
}

TEST(SupplyWait, ARunSeenLiveAndNowOverEndsTheWait)
{
    GuildSupplyWait::Facts over = CassandAtTheGate();
    over.started = true;
    over.yielded = false;
    over.restockRunLive = false;
    EXPECT_EQ(GuildSupplyWait::Of(over), GuildSupplyWait::Verdict::Concluded);
}

TEST(SupplyWait, AMemberNobodyAskedAndAWaitAlreadyOverAreNotWaits)
{
    GuildSupplyWait::Facts unasked;
    EXPECT_EQ(GuildSupplyWait::Of(unasked), GuildSupplyWait::Verdict::NoWait);
    GuildSupplyWait::Facts concluded = CassandAtTheGate();
    concluded.done = true;
    concluded.restockRunLive = true;  // even a live restock does not re-open a concluded wait
    EXPECT_EQ(GuildSupplyWait::Of(concluded), GuildSupplyWait::Verdict::NoWait);
}

TEST(SupplyWait, NothingInTheFactsIsAClockSoNoWaitCanBeSatisfiedByTime)
{
    // rule 2 asserted structurally: the facts carry no duration, age or timestamp, so the verdict for one
    // set of facts is the same answer however long the member has been standing
    GuildSupplyWait::Facts const cassand = CassandAtTheGate();
    EXPECT_EQ(GuildSupplyWait::Of(cassand), GuildSupplyWait::Of(cassand));
    EXPECT_EQ(sizeof(GuildSupplyWait::Facts), 5u) << "a field was added to the facts: if it is a time, it must not be";
}

// ---------------------------------------------------------------------------------------------------
// [supply132] THE SECOND WAITER OF THE SAME SHAPE: "IT IS A MAGE AND IT KNOWS CONJURE WATER".
//
// Inputs are b504's own rows for #42 Cassand, mage L25 of wc-n five 2 - the same five [supply131] was
// measured on: pb_journal_actor at the gate "mana 178 of 1611 = 11.049 %, drinks 0, drinks_need 27",
// the [dun199] order "Cassand conjures water up to 29 before the run (its own line + 2 the others
// lack)" written 3 times in the whole build and only for this five, and over 176 pb_journal_idle
// windows of b504 - 1 297 ticks - casts 0 and non-cast actions 0.
//
// The bite is NOT measured and is not claimed: five 2 was held at the gate by the shoulders run
// ([supply131]), not by this branch, because st.supplyDone was false and holdForPool was never
// reached. This is rule 5 - a second judge of "is this member's water on its way" is a defect named
// before it bites.
namespace
{
    // Cassand at the gate, as the row reads: it knows Conjure Water, it holds nothing, it was ordered
    // to reach 29 - and nothing it could cast went off in 45 minutes
    GuildSupplyWait::ConjurerFacts CassandTheConjurer()
    {
        GuildSupplyWait::ConjurerFacts f;
        f.knowsConjure = true;
        f.held = 0;
        f.goal = 29;
        f.canCastNow = false;
        return f;
    }
}  // namespace

TEST(ConjurerInFlight, AnAbilityIsNotWaterOnItsWay)
{
    // the defect: the five's shortfall counted as "in flight" because a member of it is a mage. Knowing
    // how to make water is constant by definition, so that reading could never come out false and the
    // wait it feeds could never end
    EXPECT_FALSE(GuildSupplyWait::ConjurerFilling(CassandTheConjurer()))
        << "Cassand knew Conjure Water for all 45 min at 178 mana of 1611 and cast nothing: 0 casts over "
           "176 windows, 0 of its 29 drinks made";
}

TEST(ConjurerInFlight, AConjurerBelowItsLineThatTheGameWouldTakeNowIsInMotion)
{
    // the reverse side: "a conjurer is never in motion" would pass the test above and break the design -
    // a mage with the mana for the cast IS filling the pool, and the five waits on that as it always did
    GuildSupplyWait::ConjurerFacts able = CassandTheConjurer();
    able.canCastNow = true;
    EXPECT_TRUE(GuildSupplyWait::ConjurerFilling(able));
}

TEST(ConjurerInFlight, AConjurerAtItsLineHasNothingLeftToMake)
{
    // and the other reverse side: a mage already at the line it was ordered to reach is not "in motion"
    // either, however well it can cast - its bags already answer the five's requirement
    GuildSupplyWait::ConjurerFacts filled = CassandTheConjurer();
    filled.held = 29;
    filled.canCastNow = true;
    EXPECT_FALSE(GuildSupplyWait::ConjurerFilling(filled));
    filled.held = 30;  // over the line is not under it
    EXPECT_FALSE(GuildSupplyWait::ConjurerFilling(filled));
}

TEST(ConjurerInFlight, AMemberThatCannotConjureIsNeverAConjurer)
{
    // the guard the ability still earns: it is necessary, it is simply never sufficient
    GuildSupplyWait::ConjurerFacts notAMage = CassandTheConjurer();
    notAMage.knowsConjure = false;
    notAMage.canCastNow = true;
    EXPECT_FALSE(GuildSupplyWait::ConjurerFilling(notAMage));
}

TEST(ConjurerInFlight, NothingInTheConjurerFactsIsAClock)
{
    // rule 2 asserted structurally, as the wait's own facts are: no duration, age or timestamp here, so
    // no assertion above can be satisfied by the member standing longer
    EXPECT_EQ(sizeof(GuildSupplyWait::ConjurerFacts), 16u)  // two bools, two uint32, padded
        << "a field was added to the conjurer's facts: if it is a time, it must not be";
}
