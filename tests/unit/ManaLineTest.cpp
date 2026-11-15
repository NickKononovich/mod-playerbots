/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [pull84] THE LINE A PULL WAITS AT, against wc-n run 691 - the run that lost the Wailing Caverns certificate.
//
// The fixture is not invented. Every number below is read off the 75 holds that are the last 237 seconds of that
// run (pb_journal_decision, run_id 20261115-135852-wc-n-five6-3, judge_low 1768 = the run's own leader):
//
//   "Aestymro at 20...90 % mana (needs 100 % = 7.07 %/s (spell data) x 10 s + 41 reserve
//    (one more mob, 3.0 s + 17.7 heal floor (Holy Light's cost) [lead1400], no credit covers it [lead1430])
//    - 0 cooldowns; heals 38.0 % per tank health (per tank health healed [lead1294]) x 0.25 cost)"
//
// 7.07 %/s, 10 s, 3.0 s of one more mob, 17.7 % of heal floor, 0 cooldowns credited, and the witness formula in
// the same sentence: 38.0 % per tank health against a plan costing 0.25 tank healths = 9.5 %. The pool holds 100.
//
// Two claims, both refutable, neither of them naming a constant of mine:
//   * a line is not allowed to ask for more mana than the pool it is paid out of;
//   * a line that asks for more than the pool is not read as something to wait at.
//
// The first one failed on these inputs before the fix - 112 % of a pool that holds 100 - and that failure is the
// whole run: five bosses left alive while the five waited for a bar that cannot exist.

#include "ManaLine.h"

#include <gtest/gtest.h>

namespace
{
    // Aestymro L24, holy paladin, the boss line that held Lord Pythas
    ManaLine::Terms Aestymro()
    {
        ManaLine::Terms terms;
        terms.healer = true;
        terms.fightSec = 10.0f;            // the judge's own estimate of the boss fight
        terms.nonstopPctPerSec = 7.07f;    // its best heal cast back to back (spell data)
        terms.oneMobSec = 3.0f;            // one more mob's fight: the reserve's length
        terms.healPerTankHealthPct = 38.0f;  // THE WITNESS: its heal's mana per tank health healed
        terms.cost = 0.25f;                  // ... and the tank health this plan costs
        terms.healFloorPct = 17.7f;          // [lead1400] one whole cast of Holy Light
        return terms;                        // 0 cooldowns credited, no potion and no cooldown floor
    }
}

TEST(ManaLine, TheLineNeverAsksForMoreManaThanThePoolItIsPaidFrom)
{
    ManaLine::Terms const terms = Aestymro();
    ManaLine::Composed const composed = ManaLine::Of(terms);
    float const raw = ManaLine::Raw(composed, terms);
    EXPECT_LE(raw, ManaLine::POOL_PCT)
        << "the line asks " << raw << " % of a pool that holds " << ManaLine::POOL_PCT
        << " (need " << composed.needPct << ", reserve " << composed.reservePct
        << "): wc-n run 691 waited 237 s for that bar and left five bosses alive";
}

TEST(ManaLine, TheNeedIsTheWitnessFormulaAndNotTheNonstopBound)
{
    // 38.0 % per tank health x 0.25 cost = 9.5 %, against the nonstop bound's 7.07 x 10 = 70.7 %. Both numbers
    // stood in the same printed sentence; only one of them is what a healer spends.
    ManaLine::Composed const composed = ManaLine::Of(Aestymro());
    EXPECT_TRUE(composed.witness) << "the need came from the nonstop bound, " << composed.needPct << " %";
    EXPECT_NEAR(composed.needPct, 9.5f, 0.05f);
}

TEST(ManaLine, TheBossCautionStaysAsAnExplicitReserveAndIsStillThere)
{
    // the fix removes a multiplier, not the caution: one whole cast of its heal stays in the reserve, and so does
    // one more mob's fight, both as themselves.
    ManaLine::Terms terms = Aestymro();
    ManaLine::Composed const withFloor = ManaLine::Of(terms);
    terms.healFloorPct = 0.0f;
    ManaLine::Composed const without = ManaLine::Of(terms);
    EXPECT_NEAR(withFloor.reservePct - without.reservePct, 17.7f, 0.05f);
    EXPECT_GT(without.reservePct, 0.0f) << "one more mob's fight is still reserved for";
}

TEST(ManaLine, ALineAboveThePoolIsNotALineToWaitAt)
{
    ManaLine::Verdict const over = ManaLine::Read(111.7f, 0.0f);
    EXPECT_TRUE(over.overPool);
    EXPECT_FALSE(over.isWait) << "an impossible requirement read as a wait is the livelock itself";
    EXPECT_FLOAT_EQ(over.linePct, ManaLine::POOL_PCT) << "the clamped value is still reported, it is just not a wait";
}

TEST(ManaLine, ALineInsideThePoolIsALineToWaitAt)
{
    ManaLine::Verdict const inside = ManaLine::Read(30.1f, 0.0f);
    EXPECT_FALSE(inside.overPool);
    EXPECT_TRUE(inside.isWait);
    EXPECT_FLOAT_EQ(inside.linePct, 30.1f);
}

TEST(ManaLine, AFullPoolExactlyIsAFullPoolAndNotAnOverflow)
{
    // the member that has to come topped up is asked for exactly what it can hold: that is a wait, and it ends.
    ManaLine::Verdict const full = ManaLine::Read(ManaLine::POOL_PCT, 0.0f);
    EXPECT_FALSE(full.overPool);
    EXPECT_TRUE(full.isWait);
}

TEST(ManaLine, AMemberWithNoWitnessKeepsTheRotationBoundBecauseAbsenceIsNotZero)
{
    // a healer whose heal is not read yet (healPerTankHealthPct 0) must not be priced at 0 mana: the bound is all
    // there is, and it stays. A damage caster's spend really IS its rotation, so it keeps the bound too.
    ManaLine::Terms healerNoHeal = Aestymro();
    healerNoHeal.healPerTankHealthPct = 0.0f;
    ManaLine::Composed const blind = ManaLine::Of(healerNoHeal);
    EXPECT_FALSE(blind.witness);
    EXPECT_NEAR(blind.needPct, 70.7f, 0.05f);

    ManaLine::Terms caster;
    caster.healer = false;
    caster.fightSec = 10.0f;
    caster.nonstopPctPerSec = 2.25f;
    caster.oneMobSec = 3.0f;
    ManaLine::Composed const rotation = ManaLine::Of(caster);
    EXPECT_FALSE(rotation.witness);
    EXPECT_NEAR(rotation.needPct, 22.5f, 0.05f);
}

TEST(ManaLine, TheCooldownsItCreditsComeOffTheLineAndTheHealFloorSurvivesThem)
{
    // [lead1430] the credits are sized to the shortfall, so a big credit must not cancel the floor of one cast
    ManaLine::Terms terms = Aestymro();
    terms.returnedPct = 95.0f;
    ManaLine::Composed const composed = ManaLine::Of(terms);
    EXPECT_GE(ManaLine::Raw(composed, terms), terms.healFloorPct)
        << "the pool's bottom by the line's own math: no credit covers it";
}
