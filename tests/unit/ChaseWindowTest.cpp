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

#include <cmath>
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

TEST(ChaseWindow, TheChaseLanbuzzWasPricedOnHasNoEndAtAll)
{
    // THE NUMBER THIS TEST USED TO DEMAND, and it reproduced the journal line to the unit: until 14.000, exposure
    // 14.000, and 41.2 dps over it 576.8 against the printed "priced 577 dmg over 14 s". It was the wrong number.
    // The guard outruns the bot (8.0 against 7.0), it lands hits all the way, and every hit restarts its own leash -
    // so nothing in the core's rules ends that chase. The chase it was priced for ran 29.4 s and dealt 911.
    Chase::Window const window =
        Chase::Of(BOT_RUN, MOB_RUN, LEASH_SEC_L40, LEASH_RADIUS, DAZED, 41.2f, MELEE_REACH);
    EXPECT_FALSE(window.ends) << "priced at " << window.until << " s, the chase ran 29.4 s and never ended by itself";
    EXPECT_FALSE(std::isfinite(window.exposure)) << "there is no number of seconds of landed melee to price";
}

TEST(ChaseWindow, ASpeedPieceNoLongerCollapsesTheMeleeToATenthOfTheChase)
{
    // Tuihgrelfim's and Irrua's case: the same guard, the same leash, and the bot's +30 % on top. This test used to
    // demand the collapse - until 14.000 and exposure 5 / (9.1 - 8.0) = 4.545 s, under a third of the chase, which
    // is why their chases were priced at 69 and 84 against Lanbuzz's 577 and why they ran. One swing in the back
    // dazes the bot to 4.55 yd/s, below the guard's 8.0, and from there the chase has no end either.
    Chase::Window const window =
        Chase::Of(BOT_RUN_SPEEDED, MOB_RUN, LEASH_SEC_L40, LEASH_RADIUS, DAZED, 41.2f, MELEE_REACH);
    EXPECT_FALSE(window.ends);
    EXPECT_GT(window.exposure, MELEE_REACH / (BOT_RUN_SPEEDED - MOB_RUN))
        << "4.545 s of landed melee was the old price of this very chase";
}

TEST(ChaseWindow, AChaserItOutrunsEvenDazedIsShakenOffAndThenGivesUp)
{
    // the other branch, and the only one left with a finite price: a bot fast enough that the daze still leaves it
    // ahead. The melee lands until it is out of reach, and from that last hit the leash timer the core gives the mob
    // runs its course - not from the start of the chase, because every hit until then restarted it.
    constexpr float FAST_BOT = 20.0f;  // dazed 10.0, still 2.0 yd/s clear of the guard's 8.0
    Chase::Window const window = Chase::Of(FAST_BOT, MOB_RUN, LEASH_SEC_L40, LEASH_RADIUS, DAZED, 41.2f, MELEE_REACH);
    EXPECT_TRUE(window.ends);
    EXPECT_NEAR(window.exposure, MELEE_REACH / (FAST_BOT * (1.0f + DAZED) - MOB_RUN), 0.01f);
    EXPECT_NEAR(window.until, window.exposure + LEASH_SEC_L40 + 1.0f, 0.01f);
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
    // Measured on it: Tuihgrelfim L24 priced 69 dmg (151 at the high end) and took 414 from one Barrens Guard L40
    // over 16.3 s at 25.2 dps; Irrua L25 priced 84 (121 high) and died to the same guard entry. 4.545 s of landed
    // melee instead of 14.187 was a factor of 3.12 off the price of every such run past.
    EXPECT_GE(priced.exposure, afterOneSwing.exposure)
        << "the price must already carry the swing that makes the chase what it is";
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
    // 911 dmg arrived against a price of 577 (677 at the high end) at a measured 32.4 dps: the damage per second was
    // right and the window was 2.10 x short. The chase ends when the mob stops landing hits, and a chaser the bot
    // cannot outrun never stops - so the honest answer here is that the chase has no end at all.
    EXPECT_FALSE(window.ends) << "a chaser the bot cannot outrun keeps extending its own leash for ever";
    EXPECT_GE(window.until, MEASURED_CHASE_SEC);
}

// [runpast89] THE HEAL THE PRICE OF A RUN PAST MAY COUNT.
//
// `SurvivesRunPast` compares the chase's damage with `bot->GetHealth() + SurvivalKit::HealPoints(bot, seconds, mana,
// true, true)`, and that second term hands the WHOLE mana pool over as health points to come: as many casts as the
// mana pays for, each worth its full amount, with `incomingDps` 0 at every run-past call site. Measured against the
// bots' own bodies in the b502 hour (pb_journal_death.health_max against the "[solo] ... vs hp N" line):
//   * Irrua L25 (shaman)      judged against 1207, body 797  - +51 %
//   * Tuihgrelfim L24 (rogue) judged against  956, body 616  - +55 %
//   * Lanbuzz L26 (warlock)   judged against  924, body 760  - +22 %
//   * Colian L26 (warrior)    judged against  926, body 926  - exact, and it is the one bot of the four with no mana
//   * Orenem L26 (priest)     "run-past priced 931 hp vs hp 1683" against a body of 487, and the fight's own measure
//     says active_s=3.7 of 28.2, why=ran before two swings - it never cast at all.
// Colian is the proof of the mechanism: the error is the mana, and it is zero for exactly the bot where the judge
// was right to the point.
namespace
{
    // Orenem's: a free-standing instant heal of 222 points on the global cooldown, 185 mana a cast, 1300 mana in the
    // pool - seven casts the old rule credits whole, 1554 points on a body of 487
    constexpr float ORENEM_HEALTH = 487.0f;
    constexpr float ORENEM_DAMAGE = 931.0f;
    constexpr float ORENEM_CHASE_SEC = 13.0f;

    Chase::Heal OrenemHeal()
    {
        Chase::Heal heal;
        heal.amount = 222.0f;
        heal.castSec = 1.5f;  // the global cooldown the spell data gives an instant
        heal.cost = 185.0f;
        return heal;
    }
}

TEST(ChaseHeal, AHealIsNotCreditedAboveTheHealthTheChaseTookOff)
{
    // overheal is not credit: a heal landing on a bot the chase has not hurt yet puts back nothing. At full health
    // the most that can ever land over the whole chase is what the chase deals, and never one point more.
    Chase::Survival const under =
        Chase::Under(OrenemHeal(), ORENEM_CHASE_SEC, ORENEM_DAMAGE, ORENEM_HEALTH, ORENEM_HEALTH, 1300.0f);
    EXPECT_LE(under.healed, ORENEM_DAMAGE)
        << "a chase that deals " << ORENEM_DAMAGE << " cannot be met with " << under.healed << " of healing";
    EXPECT_LE(ORENEM_HEALTH + under.healed, ORENEM_HEALTH + ORENEM_DAMAGE);
}

TEST(ChaseHeal, NoHealLandsAfterTheChaseHasAlreadyKilledIt)
{
    // the same heal and the same chase against a body the first seconds already empty: 931 over 13 s is 71.6 dps, so
    // a bot of 100 health is dead at 1.4 s - before the first cast's global cooldown is even out. Nothing may be
    // credited past that moment, and the verdict has to be death.
    Chase::Survival const under = Chase::Under(OrenemHeal(), ORENEM_CHASE_SEC, ORENEM_DAMAGE, 100.0f, 100.0f, 1300.0f);
    EXPECT_FALSE(under.lives);
    EXPECT_LE(under.healed, 100.0f) << "it was dead at " << under.lostAtSec << " s and was credited " << under.healed;
}

TEST(ChaseHeal, ABotWithNoManaIsPricedAgainstItsBodyAlone)
{
    // Colian L26: no mana, no heal piece, and the judge's 926 was its body to the point. The seam has to keep that
    // one right, or it has traded one error for another.
    Chase::Heal const none;
    Chase::Survival const under = Chase::Under(none, 14.0f, 900.0f, 926.0f, 926.0f, 0.0f);
    EXPECT_FLOAT_EQ(under.healed, 0.0f);
    EXPECT_TRUE(under.lives) << "900 of damage against a body of 926";
    EXPECT_FALSE(Chase::Under(none, 14.0f, 950.0f, 926.0f, 926.0f, 0.0f).lives);
}

TEST(ChaseHeal, TheVerdictAndThePoolAreTheSameAnswer)
{
    // one quantity, one judge: the gate (`lives`) and the number the journal line prints (`health + healed`) may
    // never disagree, whatever the inputs.
    size_t checked = 0;
    for (float health : {60.0f, 487.0f, 926.0f})
        for (float damage : {0.0f, 56.0f, 500.0f, 931.0f, 4000.0f})
            for (float chaseSec : {1.0f, 12.0f, 14.0f, 29.4f})
                for (float mana : {0.0f, 185.0f, 1300.0f})
                {
                    Chase::Survival const under =
                        Chase::Under(OrenemHeal(), chaseSec, damage, health, health, mana);
                    ++checked;
                    EXPECT_EQ(under.lives, damage < health + under.healed)
                        << "health " << health << " damage " << damage << " chase " << chaseSec << " mana " << mana
                        << " healed " << under.healed;
                }
    std::printf("[chase] %zu survivals checked\n", checked);
    EXPECT_GT(checked, 0u);
}
