/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [mount9] THE BOT GETS ONTO ITS MOUNT AND OFF AGAIN IN A CIRCLE. The user saw it in the game ("they look stuck,
// they just stand there and keep getting off the mount and back on") and asked for it to be caught in the problems
// table instead of dissolving into the standing watch.
//
// EVERY FIXTURE HERE IS A ROW OF b504, taken from the live journal and quoted beside it. Nothing is invented: the
// positions are to four decimals as the table holds them, the walk distances are the ones the emitter printed.
//
// WHAT THESE TESTS ARE FOR, in one line each:
//   * one episode of flapping is ONE problem, whatever the counts and whatever beast it sat on (too-narrow key);
//   * a re-mount after the facts changed is NOT a flip, and the long walk that re-mounts five times proves it
//     (too-wide criterion: this is the direction that would have made the detector lie);
//   * the town case and the walk case are not one problem (too-wide key);
//   * a map change is not a flap (the lesson the standing watch already paid for).

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "IssueKey.h"
#include "MountEpisode.h"

namespace
{
    using namespace Journal::MountState;

    Sample At(uint32_t nowMs, float x, float y, float z, bool mounted)
    {
        Sample s;
        s.nowMs = nowMs;
        s.mapId = 0;
        s.x = x;
        s.y = y;
        s.z = z;
        s.mounted = mounted;
        s.radius = 1.5f;      // max(combat reach, 1.5): passed in, as the emitter passes it
        s.cadenceMs = 10000;  // the journal's one instrument window
        return s;
    }

    // EMDYL, b504, map 0 zone 1519 (Stormwind), role dps, in_group 0. 352 standing rows at this one point over
    // 95.4 minutes, `stood_yd` 0.000 in every one of them, and the longest episode the standing watch ever
    // reported there was 69.9 s because each mount cast ended the episode and restarted the duration.
    // Its engine trace in every row: "|A:check mount state - PREREQ|A:check mount state - OK".
    // No target, no walk, outdoors, not in combat: NO FACT JUSTIFIES EITHER CHANGE.
    constexpr float EMDYL_X = -8777.3105f;
    constexpr float EMDYL_Y = 665.5f;
    constexpr float EMDYL_Z = 103.1f;

    // MARONIC, b504, one walk to (-5081,-2338), five mounts: the emitter's own lines, with the yards it printed
    // as left to walk and the millisecond each was written at.
    //   1794741068304  1906 yd left  "got off for a fight: its target in reach"
    //   1794741127636  1825 yd left  idem
    //   1794741213090  1476 yd left  idem
    //   1794741250841  1432 yd left  idem
    //   1794741297010  1381 yd left  idem
    // 525 yd covered between the first and the last. Every one of those five is RIGHT.
}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// THE TOWN CASE: the flip, and why one episode is one problem
// ---------------------------------------------------------------------------------------------------------------

TEST(MountEpisode, AChangeRepeatedUnderTheSameJustificationIsAFlip)
{
    Episode e;
    Facts none;  // Emdyl in town: no target, no walk, outdoors, out of combat, standing

    // the first sample is not a change: there is no previous state to differ from
    Sample s = At(1794762768180u, EMDYL_X, EMDYL_Y, EMDYL_Z, false);
    s.facts = none;
    Look look = Observe(e, s);
    EXPECT_FALSE(look.changed) << "the first sample of an episode was reported as a change of state";
    EXPECT_FALSE(look.report);

    // on, off, on, off - at one point, with nothing changing around it
    uint32_t at = 1794762768180u;
    for (int i = 0; i < 4; ++i)
    {
        at += 2000;
        s = At(at, EMDYL_X, EMDYL_Y, EMDYL_Z, i % 2 == 0);
        s.facts = none;
        look = Observe(e, s);
        EXPECT_TRUE(look.changed);
    }

    EXPECT_EQ(2u, look.mounts);
    EXPECT_EQ(2u, look.dismounts);
    // the second mount repeated the first mount's justification, and so did the second dismount: two flips
    EXPECT_EQ(2u, look.flips);
    EXPECT_EQ("nothing", look.flipUnder) << "the worst case of all: no fact justified any of the four changes";
    EXPECT_TRUE(IsReal(e)) << "a change repeated under an identical justification was not judged real";
    EXPECT_FLOAT_EQ(0.f, look.heldYd) << "Emdyl's rows carry stood_yd 0.000 and the episode must say the same";
}

TEST(MountEpisode, OneEpisodeIsOneProblemWhateverTheCounts)
{
    // RED AGAINST THE FIRST VERSION OF THE KEY: the emitter's printed sentence carries the counts, which climb
    // with every sample of one episode - so Emdyl's single 95-minute episode becomes a row per report.
    std::vector<std::string> naive;
    std::vector<std::string> keys;
    for (uint32_t mounts = 1; mounts <= 7; ++mounts)
    {
        naive.push_back(Journal::Key::MountFlapNaive(
            "Emdyl got onto its mount " + std::to_string(mounts) + " time(s) and off it " +
            std::to_string(mounts) + " time(s) without leaving 0.0 yd, under nothing"));
        keys.push_back(Journal::Key::MountFlap("dps", false, false, "nothing"));
    }
    std::set<std::string> const naiveDistinct(naive.begin(), naive.end());
    std::set<std::string> const distinct(keys.begin(), keys.end());
    EXPECT_EQ(7u, naiveDistinct.size()) << "the first version of the key is expected to fan one episode out";
    EXPECT_EQ(1u, distinct.size()) << "seven reports of ONE episode of Emdyl's gave " << distinct.size()
                                   << " problems: the counts are in the key, which is this project's trap #2";
}

// ---------------------------------------------------------------------------------------------------------------
// THE LEGITIMATE CASE: the direction that would make the detector lie
// ---------------------------------------------------------------------------------------------------------------

TEST(MountEpisode, TheLongWalkThatRemountsFiveTimesIsNotAFlap)
{
    // Maronic's five mounts for one walk, with the same words every time ("a fight: its target in reach") and the
    // same walk target - the justification IS identical, so only the ground it covered tells them apart. The
    // positions are not in the journal, so the yards it had left to walk stand in for them: 1906 -> 1381 along
    // one line, which is 525 yd of real progress.
    Episode e;
    Facts fighting;
    fighting.inCombat = true;
    fighting.targetInReach = true;
    fighting.hasWalk = true;
    fighting.walkToX = -5081;
    fighting.walkToY = -2338;

    struct Step { uint32_t ms; float yardsLeft; };
    constexpr Step steps[] = {{1794741068304u, 1906.f}, {1794741127636u, 1825.f}, {1794741213090u, 1476.f},
                              {1794741250841u, 1432.f}, {1794741297010u, 1381.f}};

    Look look;
    for (Step const& step : steps)
    {
        // off the mount for the fight, then back on - both at the place it had reached by then
        Sample off = At(step.ms - 1000u, -5081.f + step.yardsLeft, -2338.f, 0.f, false);
        off.facts = fighting;
        Observe(e, off);
        Sample on = At(step.ms, -5081.f + step.yardsLeft, -2338.f, 0.f, true);
        on.facts = fighting;
        look = Observe(e, on);
    }

    EXPECT_EQ(0u, look.flips) << "the walk that covered 525 yd between its first and its fifth mount was called a "
                                 "flap: the anchor radius is what tells a leg of a walk from a circle in place";
    EXPECT_FALSE(IsReal(e));
}

TEST(MountEpisode, AJustificationThatChangedIsNotAFlip)
{
    // got off indoors, mounted again outdoors, got off for a fight: three changes, three different justifications
    Episode e;
    Facts indoors;
    indoors.indoors = true;
    Facts outdoors;
    Facts fight;
    fight.inCombat = true;

    Sample s = At(1000u, 100.f, 100.f, 10.f, true);
    s.facts = outdoors;
    Observe(e, s);

    s = At(2000u, 100.f, 100.f, 10.f, false);
    s.facts = indoors;
    Observe(e, s);
    s = At(3000u, 100.f, 100.f, 10.f, true);
    s.facts = outdoors;
    Observe(e, s);
    s = At(4000u, 100.f, 100.f, 10.f, false);
    s.facts = fight;
    Look const look = Observe(e, s);

    EXPECT_EQ(2u, look.dismounts);
    EXPECT_EQ(0u, look.flips) << "the second dismount was justified by a fight and the first by being indoors: "
                                 "two different facts are two decisions, not a circle";
    EXPECT_FALSE(IsReal(e));
}

// ---------------------------------------------------------------------------------------------------------------
// THE KEY, THE OTHER DIRECTION: two causes must not hide under one row
// ---------------------------------------------------------------------------------------------------------------

TEST(MountEpisodeKey, TheTownCaseAndTheWalkCaseAreNotOneProblem)
{
    // RED AGAINST A KEY THAT CARRIES ONLY THE ROLE: the town flap (no target, no walk, nobody leading) and the
    // walk flap (a walk is on and a target sits inside the dismount band) are two judges disagreeing about two
    // different quantities, and a fix for one of them must not turn a test green over the other.
    std::string const town = Journal::Key::MountFlap("dps", false, false, "nothing");
    std::string const walk =
        Journal::Key::MountFlap("dps", true, false, "in combat+target in reach+a walk is on to (-5081,-2338)");
    std::string const led = Journal::Key::MountFlap("dps", false, true, "its leader rides");

    EXPECT_NE(town, walk);
    EXPECT_NE(town, led);
    EXPECT_NE(walk, led);
    EXPECT_EQ("mount_flap|dps|no walk|solo|nothing", town);
}

TEST(MountEpisodeKey, NeitherTheMountNorThePlaceNorTheBotIsACause)
{
    // 23 bots over 7 zones flapped on b504 under three justifications. Keying on the zone would give at least 7
    // problems where there are 3, and on the bot 23: they are columns of the row, as the mount's spell and name
    // are columns of pb_journal_mount. A role IS in the key, as it is in both sibling kinds.
    EXPECT_EQ(Journal::Key::MountFlap("dps", false, false, "nothing"),
              Journal::Key::MountFlap("dps", false, false, "nothing"));
    EXPECT_NE(Journal::Key::MountFlap("dps", false, false, "nothing"),
              Journal::Key::MountFlap("healer", false, false, "nothing"));
    // and the justification is a closed set of names: the same facts always give the same string
    Journal::MountState::Facts facts;
    facts.inCombat = true;
    facts.targetInReach = true;
    EXPECT_EQ("in combat+target in reach", Journal::MountState::Justification(facts));
    EXPECT_EQ("nothing", Journal::MountState::Justification(Journal::MountState::Facts{}));
}

// ---------------------------------------------------------------------------------------------------------------
// THE LESSON ALREADY PAID FOR ONCE
// ---------------------------------------------------------------------------------------------------------------

TEST(MountEpisode, ATeleportToAnotherMapIsNotAFlap)
{
    // The standing watch reported a teleport into Wailing Caverns as an episode "within 3355.50341796875 yd"
    // because its anchor carried no map. This anchor carries one, and the counters go with the place.
    Episode e;
    Facts none;
    Sample s = At(1000u, -8777.3105f, 665.5f, 103.1f, false);
    s.facts = none;
    Observe(e, s);
    s = At(2000u, -8777.3105f, 665.5f, 103.1f, true);
    s.facts = none;
    Observe(e, s);
    s = At(3000u, -8777.3105f, 665.5f, 103.1f, false);
    s.facts = none;
    Observe(e, s);

    Sample other = At(4000u, 39.8389f, 19.7113f, -90.f, false);
    other.mapId = 43;  // Wailing Caverns
    other.facts = none;
    Look const look = Observe(e, other);

    EXPECT_FALSE(look.report) << "a teleport to another map was reported as an episode of the place left behind";
    EXPECT_EQ(0u, look.flips);
    EXPECT_FLOAT_EQ(0.f, look.heldYd) << "a position on another map is not a distance away";
}

TEST(MountEpisode, AnOpenEpisodeIsReportedOncePerWindow)
{
    // The cadence is the sampler's own and is reused, not invented: a ten-minute circle is ONE problem whose
    // value range tops out at its flips, not sixty rows.
    Episode e;
    Facts none;
    Sample s = At(1000u, 10.f, 10.f, 1.f, false);
    s.facts = none;
    Observe(e, s);
    s = At(2000u, 10.f, 10.f, 1.f, true);
    s.facts = none;
    EXPECT_TRUE(Observe(e, s).report) << "the sample that changes the state reports at once";

    int reports = 0;
    for (uint32_t at = 3000u; at <= 9000u; at += 1000u)
    {
        Sample same = At(at, 10.f, 10.f, 1.f, true);
        same.facts = none;
        if (Observe(e, same).report)
            ++reports;
    }
    EXPECT_EQ(0, reports) << "an episode with no new change wrote " << reports
                          << " rows inside one instrument window: that is the cadence talking, not the bot";
}

TEST(MountEpisode, ABotThatSimplyStaysMountedWritesNothing)
{
    // The clean result the standing watch already paid for: 406 of 3 900 idle rows had every bucket at zero. A
    // detector that reports its own clean results poisons the count the user drives to zero.
    Episode e;
    Facts riding;
    riding.moving = true;
    for (uint32_t at = 1000u; at <= 60000u; at += 1000u)
    {
        Sample s = At(at, 10.f, 10.f, 1.f, true);
        s.facts = riding;
        EXPECT_FALSE(Observe(e, s).report);
    }
    EXPECT_FALSE(IsReal(e));
}
