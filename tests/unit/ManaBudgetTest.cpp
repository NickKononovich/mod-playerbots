/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [mana90] THE MANA AS A BUDGET FOR THE WHOLE FIGHT, against the b502 hour that produced eight deaths.
//
// Four of the eight are "[death465] ... lost out of mana" (Selalleas, Emdyl, Orenem, Lanbuzz; 19 such lines in the
// hour). The fixture here is Selalleas L26, a resto druid, and every number of it is recorded:
//   * "[dps] Selalleas L26 11/resto ctx=solo target=1034 L23 mobs=5 formula=22.1 ideal=23.0 actual=21.1
//      fight_s=167.7 active_s=148.1 taken=2628 outcome=died valid=1" - 167.7 s of fight, 2628 taken.
//   * five "[mobdps]" lines of that fight: 11.0 + 13.3 + 9.4 + 7.0 + 9.9 = 50.6 incoming dps.
//   * "[solo] Selalleas L26 survival Healing Potion (heal, item): ... 3 attackers 37.6 dps, 24 s at its 45.9 dps:
//      683 dmg to come at hp 479 (+175 heals)" - the judge's window was TWENTY-FOUR seconds of a 167.7 s fight, and
//      683 over 24 s at hp 479 is right. Every look was right; no look was of the fight.
//   * "[death465] Selalleas L26 lost out of mana to 5 attackers (0 judged)" - and `0 judged` says no pull judge ever
//      opened it: it was attacked while wandering.
// Its body is 774 (pb_journal_death.health_max) and its druid heals give about 2.4 health per point of mana at that
// level, which is what the price of a heal and its points give.
//
// The claim is refutable and names no constant of mine: a fight the judge takes must not ask for more mana than the
// bot has, measured over the judge's OWN estimate of how long that fight runs.

#include "ManaBudget.h"

#include <cmath>
#include <cstdio>
#include <gtest/gtest.h>

namespace
{
    // Selalleas L26, resto druid, the fight it died in
    ManaBudget::Fight Selalleas()
    {
        ManaBudget::Fight fight;
        fight.fightSec = 167.7f;     // [dps] fight_s
        fight.incomingDps = 50.6f;   // the five [mobdps] lines of that fight, summed
        fight.health = 774.0f;       // its body
        fight.mana = 1500.0f;        // a L26 druid's pool
        fight.manaPerSec = 5.0f;     // what its own rotation spends while it fights back
        fight.regenPerSec = 2.0f;    // the mana it gets back in combat
        fight.healPerMana = 2.4f;    // its best heal's points per point of mana, from the spell's own data
        return fight;
    }
}

TEST(ManaBudget, TheFightSelalleasTookDidNotFitItsMana)
{
    ManaBudget::Answer const answer = ManaBudget::Of(Selalleas());
    EXPECT_FALSE(answer.holds) << "the mana was asked for " << answer.needMana << " and the bot had 1500";
    EXPECT_LT(answer.holdsSec, 167.7f)
        << "it held " << answer.holdsSec << " s of a 167.7 s fight (measured: it fought 148.1 s and died)";
    EXPECT_GT(answer.needMana, 1500.0f);
}

TEST(ManaBudget, TheEpisodeTheJudgeLookedAtFitsAndThatIsWhyItWasTaken)
{
    // the very window the judge DID look at - 24 s - fits the mana comfortably. This is the whole mechanism in one
    // assertion: the episode is affordable, the fight is not, and only the fight kills.
    ManaBudget::Fight episode = Selalleas();
    episode.fightSec = 24.0f;
    ManaBudget::Answer const answer = ManaBudget::Of(episode);
    EXPECT_TRUE(answer.holds) << "needed " << answer.needMana;
    EXPECT_LT(answer.needMana, 1500.0f);
}

TEST(ManaBudget, ABotThatSpendsNoManaIsNeverBoundByIt)
{
    // a warrior, a rogue: no pool, no spend, no heal bought with mana. The answer has to be "the mana never ends
    // this fight", not "it ends at once" - absence is not zero.
    ManaBudget::Fight fight;
    fight.fightSec = 167.7f;
    fight.incomingDps = 50.6f;
    fight.health = 926.0f;
    ManaBudget::Answer const answer = ManaBudget::Of(fight);
    EXPECT_TRUE(answer.holds);
    EXPECT_FALSE(std::isfinite(answer.holdsSec));
    EXPECT_FLOAT_EQ(answer.needMana, 0.0f);
}

TEST(ManaBudget, ARotationTheRegenPaysForIsNeverBoundEither)
{
    // the drain is what its play spends LESS what the game gives back: a rotation inside its own regen, with no
    // damage to heal, never runs the pool down however long the fight is
    ManaBudget::Fight fight;
    fight.fightSec = 600.0f;
    fight.mana = 100.0f;
    fight.manaPerSec = 2.0f;
    fight.regenPerSec = 3.0f;
    fight.healPerMana = 2.4f;
    ManaBudget::Answer const answer = ManaBudget::Of(fight);
    EXPECT_TRUE(answer.holds);
    EXPECT_FLOAT_EQ(answer.needMana, 0.0f);
}

TEST(ManaBudget, ItHoldsExactlyAsLongAsTheManaBuysBackTheDamage)
{
    // the one property that holds whatever the inputs: the seconds the mana holds are monotone in the mana, and a
    // fight no longer than those seconds is one the mana holds. Refutable without a culprit.
    size_t checked = 0;
    for (float mana : {0.0f, 200.0f, 1500.0f, 6000.0f})
        for (float dps : {0.0f, 10.0f, 50.6f, 200.0f})
            for (float fightSec : {1.0f, 24.0f, 167.7f, 600.0f})
            {
                ManaBudget::Fight fight = Selalleas();
                fight.mana = mana;
                fight.incomingDps = dps;
                fight.fightSec = fightSec;
                ManaBudget::Answer const answer = ManaBudget::Of(fight);
                ++checked;
                EXPECT_EQ(answer.holds, answer.holdsSec >= fightSec)
                    << "mana " << mana << " dps " << dps << " fight " << fightSec << " holds " << answer.holdsSec;
                ManaBudget::Fight richer = fight;
                richer.mana = mana + 500.0f;
                EXPECT_GE(ManaBudget::Of(richer).holdsSec, answer.holdsSec) << "more mana must not hold for less";
            }
    std::printf("[mana] %zu budgets checked\n", checked);
    EXPECT_GT(checked, 0u);
}

TEST(ManaBudget, ABotThatCannotBuyHealthWithManaIsNotKilledByRunningOut)
{
    // a mage: a pool, a rotation that drains it, and no heal its mana pays for. Running the pool down costs it its
    // damage, not its life - and a budget that refused it every long fight would be a refusal about damage wearing
    // a death's clothes. The fight's own length estimate answers the damage; this quantity answers survival.
    ManaBudget::Fight fight;
    fight.fightSec = 300.0f;
    fight.incomingDps = 20.0f;
    fight.health = 900.0f;
    fight.mana = 1200.0f;
    fight.manaPerSec = 30.0f;  // its pool is gone in forty seconds
    fight.regenPerSec = 2.0f;
    fight.healPerMana = 0.0f;
    ManaBudget::Answer const answer = ManaBudget::Of(fight);
    EXPECT_TRUE(answer.holds);
    EXPECT_FALSE(std::isfinite(answer.holdsSec));
}
