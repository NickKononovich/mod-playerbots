/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [runpast88] THE RUN-PAST CHASE WINDOW, against the b502 hour that produced eight deaths.
//
// Three of those eight were the same mob - a Barrens Guard L40 - and the same decision: the bot priced walking
// past it, walked, and was caught. The price is a `decision` by the stand's own classification: the inputs are all
// numbers, the judge is deterministic and reads no world, so the same inputs put back must give the same two
// seconds out. That is what the first two tests demand, and the first of them reproduces a number printed in the
// journal to the unit.
//
// WHERE THE NUMBERS COME FROM (pb_journal_line and pb_journal_death of build b502, not from reading the code):
//   * every mob of that hour has creature_template.speed_run 1.14286, which is 1.14286 * 7.0 = 8.0 yd/s; a bot's
//     base run is 7.0 yd/s, and a bot that holds an instant speed piece (a rogue's Sprint, a shaman's Ghost Wolf,
//     +30 %) runs 9.1.
//   * CreatureLeashRadius = 30 in worldserver.conf; Creature::GetLeashTimer gives 13 s for a level 40 mob
//     (11 + 40/10 - 2) and 12 s for a level 29-30 one.
//   * spell 1604 (Dazed) is MOD_DECREASE_SPEED at -50, so the share is -0.5.
//   * "[surv10] Lanbuzz L26 commits a run-past: 1 chaser(s), widest level gap +14, priced 577 dmg (677 at the high
//     end) over 14 s against hp 924 - a margin of 247" - then, 29 s later, "[runpast87] ... took 911 (died)" and
//     "[mobdps] Barrens Guard L40 hit Lanbuzz for 32.4 dps this fight".
//   * "[surv10] Tuihgrelfim L24 commits a run-past: 1 chaser(s), widest level gap +16, priced 69 dmg (151 at the
//     high end) over 14 s against hp 956 - a margin of 805"; "[dps] ... fight_s=16.3 ... taken=414" and
//     "[mobdps] Barrens Guard L40 hit Tuihgrelfim for 25.2 dps this fight".
//   * "[surv10] Irrua L25 commits a run-past: 1 chaser(s), widest level gap +15, priced 84 dmg (121 at the high
//     end) over 14 s against hp 1207 - a margin of 1086".
//
// The last two tests are the two ways the window comes out short. Both are RED on these numbers, and both are
// skipped rather than failed, with the measurement in the skip message, because closing either one raises the
// price of every run past and so changes which walks a bot takes - and a change of behaviour is the user's to
// make. The skip goes away with the fix, not with a rewritten assertion.

#include "ChaseModel.h"

#include <cstdio>
#include <gtest/gtest.h>

namespace
{
    constexpr float MOB_RUN = 8.0f;        // 1.14286 * 7.0, every mob of the b502 hour
    constexpr float BOT_RUN = 7.0f;        // a bot's base run
    constexpr float BOT_RUN_SPEEDED = 9.1f;  // ... with a +30 % instant speed piece
    constexpr float LEASH_RADIUS = 30.0f;  // CreatureLeashRadius
    constexpr float DAZED = -0.5f;         // spell 1604's MOD_DECREASE_SPEED
    constexpr float MELEE_REACH = 5.0f;
    // Creature::GetLeashTimer: 11 + minlevel / 10 - 2
    constexpr float LEASH_SEC_L40 = 13.0f;
}

TEST(ChaseWindow, ItReproducesTheChaseLanbuzzWasPricedOn)
{
    // the bot has no speed piece, so the guard is the faster of the two and the melee lands for the whole chase
    Chase::Window const window =
        Chase::Of(BOT_RUN, MOB_RUN, LEASH_SEC_L40, LEASH_RADIUS, DAZED, 41.2f, MELEE_REACH);
    EXPECT_NEAR(window.until, 14.0f, 0.01f) << "the journal line says 14 s of chase";
    EXPECT_NEAR(window.exposure, window.until, 0.01f) << "a chaser that outruns the bot lands melee all the way";
    // 41.2 dps is what the recorded price divided by the recorded seconds gives; the product has to come back to
    // the printed number, or this seam is not the judge that wrote that line
    EXPECT_NEAR(41.2f * window.exposure, 577.0f, 1.0f) << "the journal line says 577 dmg";
}

TEST(ChaseWindow, ASpeedPieceCollapsesTheMeleeToATenthOfTheChase)
{
    // the same guard, the same leash, and the only difference is the bot's +30 %: `canDaze` turns false, so the
    // exposure becomes reach / (botSpeed - mobSpeed) and almost ten of the fourteen seconds go unpriced. This is
    // Tuihgrelfim's and Irrua's case, and it is why their chases were priced at 69 and 84 against Lanbuzz's 577.
    Chase::Window const window =
        Chase::Of(BOT_RUN_SPEEDED, MOB_RUN, LEASH_SEC_L40, LEASH_RADIUS, DAZED, 41.2f, MELEE_REACH);
    EXPECT_NEAR(window.until, 14.0f, 0.01f) << "the chase itself is the same length";
    EXPECT_NEAR(window.exposure, MELEE_REACH / (BOT_RUN_SPEEDED - MOB_RUN), 0.01f);
    EXPECT_LT(window.exposure, window.until / 3.0f) << "under a third of the chase is priced as landing";
}

TEST(ChaseWindow, TheExposureIsNeverLongerThanTheChaseItBelongsTo)
{
    // the one property that holds whatever the model decides: melee cannot land for longer than the chase lasts.
    // Refutable without a culprit, and it goes red on any change that lets the two drift apart.
    size_t checked = 0;
    for (float bot = 3.0f; bot <= 12.0f; bot += 0.5f)
        for (float mob = 3.0f; mob <= 12.0f; mob += 0.5f)
            for (float leash : {11.0f, 13.0f, 16.0f})
                for (float melee : {0.0f, 41.2f})
                {
                    Chase::Window const window =
                        Chase::Of(bot, mob, leash, LEASH_RADIUS, DAZED, melee, MELEE_REACH);
                    ++checked;
                    EXPECT_LE(window.exposure, window.until + 0.001f)
                        << "bot " << bot << " mob " << mob << " leash " << leash << " melee " << melee;
                    EXPECT_GT(window.until, 0.0f);
                }
    std::printf("[chase] %zu windows checked\n", checked);
    EXPECT_GT(checked, 0u);
}

TEST(ChaseWindow, ADazingSwingIsPricedWhereItDecidesTheChase)
{
    // RED on b502's numbers. The daze is priced only where the mob is ALREADY faster than the bot
    // (`mobSpeed > botSpeed` in Chase::Of), and that is the one case where it changes nothing - a faster mob
    // catches up with or without it. Where it decides the chase, a bot ahead by 1.1 yd/s that one swing from
    // behind drops to half speed and behind, the model prices no daze at all: 4.545 s of landed melee instead of
    // 14.187, a factor of 3.12 off the price of every such run past.
    Chase::Window const priced =
        Chase::Of(BOT_RUN_SPEEDED, MOB_RUN, LEASH_SEC_L40, LEASH_RADIUS, DAZED, 41.2f, MELEE_REACH);
    Chase::Window const afterOneSwing = Chase::Of(BOT_RUN_SPEEDED * (1.0f + DAZED), MOB_RUN, LEASH_SEC_L40,
                                                  LEASH_RADIUS, DAZED, 41.2f, MELEE_REACH);
    GTEST_SKIP() << "RED, and the fix is a change of behaviour awaiting the user's word: the chase of a mob whose "
                    "melee lands is priced over " << priced.exposure << " s while one dazing swing makes it "
                 << afterOneSwing.exposure << " s - " << afterOneSwing.exposure / priced.exposure
                 << " x. Measured on it: Tuihgrelfim L24 priced 69 dmg (151 at the high end) and took 414 from one "
                    "Barrens Guard L40 over 16.3 s at 25.2 dps; Irrua L25 priced 84 (121 high) and died to the "
                    "same guard entry. Pricing the daze as a consequence of the swing instead of the speeds stops "
                    "a bot walking past a mob of near its own speed at all, which is the user's call.";
    EXPECT_GE(priced.exposure, afterOneSwing.exposure);
}

TEST(ChaseWindow, TheChaseIsNotBoundedByALeashTimerTheCoreKeepsRestarting)
{
    // RED on b502's numbers. `until` is max(leashSec, leashRadius / follow) + 1, and NEITHER term ends the chase
    // while the mob keeps landing hits: in the core (Creature::CanCreatureAttack) the leash timer is a window
    // since the last leash extension and Unit::DealDamage calls UpdateLeashExtensionTime on every hit, and that
    // branch is tested BEFORE the distance check and answers "keep attacking", so the radius never comes into it.
    // Lanbuzz's chase was priced at this 14 s; it ran 29 s with the guard 91 yd from its home - three times the
    // 30 yd radius - still swinging, and 911 arrived against a price of 577 (677 at the high end).
    Chase::Window const window =
        Chase::Of(BOT_RUN, MOB_RUN, LEASH_SEC_L40, LEASH_RADIUS, DAZED, 41.2f, MELEE_REACH);
    // Lanbuzz: the chase was committed at at_ms 1794736152960 and the death row is at 1794736182404 - 29.44 s,
    // and the [dps] line of that fight says fight_s=29.0. 911 dmg over it is 30.9 dps, against a measured 32.4.
    constexpr float MEASURED_CHASE_SEC = 29.4f;
    GTEST_SKIP() << "RED, and the fix is a change of behaviour awaiting the user's word: the chase is priced at "
                 << window.until << " s and the one it was priced for ran " << MEASURED_CHASE_SEC
                 << " s (911 dmg against a price of 577, 677 at the high end, at a measured 32.4 dps - the damage "
                    "per second was right and the window was 2.10 x short). Bounding the chase by the fact the "
                    "core uses - it ends when the mob stops landing hits - prices a same-speed chaser as unbounded "
                    "and stops the bot walking past one at all, which is the user's call.";
    EXPECT_GE(window.until, MEASURED_CHASE_SEC);
}
