/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [test1] The stand, over the whole recorded corpus: every chain pull judgement the journal holds, read from files,
// put back through the judge's own predicates. One fixture answers "did this verdict turn over"; a hundred answer
// "where did the verdicts turn over", which is the question a change to the judge actually raises.
//
// What is asserted here is deliberately NOT "this one should have decided otherwise" - that would nail the author's
// guess down as truth and then guard it. The assertions are refutable without knowing who is to blame:
//   - the judge's own trigger must follow from the judge's own numbers (its bookkeeping agrees with itself);
//   - a fight it took must have had a positive modelled survival margin (a death is an error of this calculation);
//   - a fight it took must have counted at least the mobs it already had plus the pack it was adding;
//   - a fight it took must have named the healer that keeps the tank.
// Each of those goes red without anyone having to say which line of the judge broke.
//
// Run by hand:  pb_unit_tests --gtest_filter=ChainPullCorpus.*
// Another corpus: PB_FIXTURE_DIR=<dir> pb_unit_tests --gtest_filter=ChainPullCorpus.*

#include "ChainPullJudge.h"
#include "Fixture.h"

#include <cmath>
#include <cstdio>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace Leader::Judge;

namespace
{
    std::string CorpusDir() { return PbFixture::Root() + "/chain_pull"; }

    struct Loaded
    {
        PbFixture::Flat file;
        ChainPullFacts facts;
        std::string recordedKind;
        bool recordedDue = false;
        bool recordedTaken = false;
        double precision = 0.1;  // the journal prints these numbers to one decimal
        std::string name;
    };

    // the facts as the file carries them. A number the file does not carry stays negative, a flag it does not carry
    // stays Unknown: the replay must be able to say "I cannot decide this" instead of inventing it.
    Loaded Read(std::string const& path)
    {
        Loaded out;
        std::string error;
        EXPECT_TRUE(out.file.Load(path, error)) << error;
        out.name = path.substr(path.find_last_of("/\\") + 1);
        out.facts.nowMs = uint32_t(out.file.Int("now_ms", 0));
        out.facts.timing = {float(out.file.Num("fight_sec_left", -1.0)), float(out.file.Num("walk_sec", -1.0)),
                            float(out.file.Num("run_in_sec", -1.0)), float(out.file.Num("open_sec", -1.0))};
        out.facts.survival = {float(out.file.Num("tank_health", -1.0)), float(out.file.Num("drop", -1.0))};
        out.facts.distance = float(out.file.Num("distance", -1.0));
        out.facts.walkYd = float(out.file.Num("walk_yd", -1.0));
        out.facts.overload = float(out.file.Num("overload", -1.0));
        out.facts.unionSec = float(out.file.Num("union_sec", -1.0));
        out.facts.groupDps = float(out.file.Num("group_dps", -1.0));
        out.facts.healerSec = float(out.file.Num("healer_sec", -1.0));
        out.facts.packMobs = int32_t(out.file.Int("pack_mobs", -1));
        out.facts.fightMobs = int32_t(out.file.Int("fight_mobs", -1));
        out.facts.unionMobs = int32_t(out.file.Int("union_mobs", -1));
        out.facts.stop = int32_t(out.file.Int("stop", -1));
        out.facts.walk = out.file.Bool("walk", false);
        out.facts.healerWalks = out.file.Bool("healer_walks", false);
        out.facts.hasHealer = !out.file.Str("healer").empty();
        out.facts.due = out.file.Bool("recorded_due", false);
        out.facts.taken = out.file.Bool("recorded_taken", false);
        out.recordedKind = out.file.Str("recorded_kind");
        out.recordedDue = out.facts.due;
        out.recordedTaken = out.facts.taken;
        out.precision = out.file.Num("number_precision", 0.1);
        return out;
    }

    std::vector<Loaded> Corpus()
    {
        std::vector<Loaded> out;
        for (std::string const& path : PbFixture::ListJson(CorpusDir()))
            out.push_back(Read(path));
        return out;
    }

    bool HasTiming(ChainPullFacts const& facts)
    {
        return facts.timing.fightSecLeft >= 0.0f && facts.timing.walkSec >= 0.0f && facts.timing.runInSec >= 0.0f &&
               facts.timing.openSec >= 0.0f;
    }
}

TEST(ChainPullCorpus, TheCorpusIsThere)
{
    // a rig that found no fixtures has proved nothing; it must fail loudly rather than pass on an empty sweep.
    std::vector<std::string> const files = PbFixture::ListJson(CorpusDir());
    ASSERT_FALSE(files.empty()) << "no fixtures in " << CorpusDir()
                                << " - export them with tests/tools/export_fixture.py chain-pull";
    std::printf("[corpus] %zu chain pull fixtures in %s\n", files.size(), CorpusDir().c_str());
}

TEST(ChainPullCorpus, TheTriggerFollowsFromTheJudgesOwnNumbers)
{
    std::vector<Loaded> const corpus = Corpus();
    ASSERT_FALSE(corpus.empty());
    size_t agreed = 0;
    size_t inBand = 0;
    for (Loaded const& one : corpus)
    {
        if (!HasTiming(one.facts))
            continue;
        double const cost = double(one.facts.timing.walkSec) + one.facts.timing.runInSec + one.facts.timing.openSec;
        // the journal prints one decimal, so a case this close to the boundary cannot be settled from the file: the
        // true numbers may fall either side of it. Saying so is the honest answer; a tolerance that swallowed the
        // boundary would make the test agree with anything.
        if (std::fabs(double(one.facts.timing.fightSecLeft) - cost) <= 2.0 * one.precision)
        {
            ++inBand;
            continue;
        }
        EXPECT_EQ(DueNow(one.facts.timing), one.recordedDue)
            << one.name << ": fight left " << one.facts.timing.fightSecLeft << " s against walk "
            << one.facts.timing.walkSec << " + run in " << one.facts.timing.runInSec << " + opening "
            << one.facts.timing.openSec << " s, recorded due=" << one.recordedDue;
        ++agreed;
    }
    std::printf("[corpus] trigger: %zu judged, %zu inside the one-decimal rounding band\n", agreed, inBand);
    EXPECT_GT(agreed, 0u) << "every fixture fell inside the rounding band: the corpus proves nothing about the trigger";
}

TEST(ChainPullCorpus, NoTakenFightHadANonPositiveSurvivalMargin)
{
    std::vector<Loaded> const corpus = Corpus();
    ASSERT_FALSE(corpus.empty());
    size_t taken = 0;
    for (Loaded const& one : corpus)
    {
        if (!one.recordedTaken)
            continue;
        ++taken;
        ASSERT_GE(one.facts.survival.tankHealth, 0.0f) << one.name << ": a taken fight recorded no tank health";
        ASSERT_GE(one.facts.survival.drop, 0.0f) << one.name << ": a taken fight recorded no modelled drop";
        EXPECT_TRUE(TankOutlastsDrop(one.facts.survival))
            << one.name << ": the judge took a fight with the tank at " << one.facts.survival.tankHealth
            << " of its health against a modelled drop of " << one.facts.survival.drop;
    }
    std::printf("[corpus] %zu taken fights checked for their survival margin\n", taken);
    EXPECT_GT(taken, 0u) << "the corpus holds no taken fight: it cannot say anything about the margin";
}

TEST(ChainPullCorpus, ATakenFightCountedAtLeastTheMobsItAlreadyHadAndTheOnesItAdded)
{
    // "more mobs arrived than it counted" is the shape of the failure this guards: the union it priced must be at
    // least the fight it was in plus the pack it was adding.
    std::vector<Loaded> const corpus = Corpus();
    ASSERT_FALSE(corpus.empty());
    for (Loaded const& one : corpus)
    {
        if (!one.recordedTaken)
            continue;
        ASSERT_GE(one.facts.unionMobs, 0) << one.name << ": a taken fight counted no union";
        EXPECT_GE(one.facts.unionMobs, one.facts.packMobs + one.facts.fightMobs)
            << one.name << ": priced " << one.facts.unionMobs << " mobs for a fight of " << one.facts.fightMobs
            << " plus a pack of " << one.facts.packMobs;
    }
}

TEST(ChainPullCorpus, ATakenFightNamedTheHealerThatKeepsTheTank)
{
    std::vector<Loaded> const corpus = Corpus();
    ASSERT_FALSE(corpus.empty());
    for (Loaded const& one : corpus)
        if (one.recordedTaken)
            EXPECT_TRUE(one.facts.hasHealer)
                << one.name << ": taken with no healer named, though the healer gate precedes the verdict";
}

TEST(ChainPullCorpus, Summary)
{
    // the sweep the stand is for: one line per fixture, then the counts. Nothing is asserted beyond the corpus
    // being there - this is the form to read after changing the judge, to see WHERE the verdicts moved.
    std::vector<Loaded> const corpus = Corpus();
    ASSERT_FALSE(corpus.empty());
    size_t undecided = 0;
    size_t takenNow = 0;
    size_t takenThen = 0;
    std::printf("%-34s %-10s %-6s %-6s %-9s %s\n", "fixture", "recorded", "due", "replay", "replayDue", "numbers");
    for (Loaded const& one : corpus)
    {
        ChainPullReplay const replay = ReplayChainPull(one.facts);
        if (replay.gate == Gate::Undecided)
            ++undecided;
        if (replay.taken)
            ++takenNow;
        if (one.recordedTaken)
            ++takenThen;
        std::printf("%-34s %-10s %-6d %-6s %-9d left %.1f walk %.1f in %.1f open %.1f hp %.2f drop %.2f\n",
                    one.name.c_str(), one.recordedKind.c_str(), int(one.recordedDue), GateName(replay.gate),
                    int(replay.due), one.facts.timing.fightSecLeft, one.facts.timing.walkSec,
                    one.facts.timing.runInSec, one.facts.timing.openSec, one.facts.survival.tankHealth,
                    one.facts.survival.drop);
    }
    std::printf("[corpus] %zu fixtures, %zu taken when recorded, %zu taken on replay, %zu undecided (a fact the "
                "fixture does not carry)\n",
                corpus.size(), takenThen, takenNow, undecided);
}
