/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [death475] AN ITEM USE REPORTS WHAT THE CORE DID WITH IT, over REAL rows of b503.
//
// The defect, in numbers. Selalleas L26 (resto druid, Angerfang Encampment) in the 32 s before its death at
// 1794752301078: 46 `[death385]` lines "run past: heals on the move (item 929, NNN hp)" against ONE `[eco194]`
// Healing Potion actually used. Health 271 -> 26 over those 32 s, standing still the whole time, because the caller
// is `if (SurvivalKit::RunPastHeal(...)) return true;` - a claimed heal eats the flee tick. `cast` was set
// unconditionally for an item (Player::CastItemUseSpell returns void) and the chooser asked an item nothing about
// being ready at all.
//
// The whole corpus of that run: 252 `[death385]` lines from 8 bots (Orenem 50, Selalleas 47, Ryarii 34,
// Maronic 32, ...), 83 of them also carrying float's maximum as a damage figure.
//
// BOTH DIRECTIONS ARE ASSERTED, because each rule alone passes one of them: a use the core did not take must not be
// reported, and a use it DID take must be.

#include <gtest/gtest.h>

#include "ItemUse.h"

namespace
{
    using Rpg::ItemUse::Facts;

    // Healing Potion, item 929: a potion, so its combat use is recorded in Player::m_lastPotionId and NOT cooled
    // down ([lead481]). This is the state before Selalleas' first drink of that fight.
    Facts HealingPotionFirstDrink()
    {
        Facts facts;
        facts.isPotion = true;
        facts.potionAlreadyDrunk = false;
        facts.lastPotionIsThisItem = false;
        facts.spellOnCooldown = false;
        return facts;
    }

    // the state right after that first drink went through: the core recorded item 929 as the potion of this combat
    Facts HealingPotionJustAccepted()
    {
        Facts facts = HealingPotionFirstDrink();
        facts.potionAlreadyDrunk = true;
        facts.lastPotionIsThisItem = true;
        return facts;
    }

    // and the state of the next 45 ticks: the potion is still in the bags, its spell carries NO cooldown (the core
    // writes that only when the combat ends), and the core has already refused every further potion
    Facts HealingPotionRetryInTheSameFight()
    {
        Facts facts = HealingPotionFirstDrink();
        facts.potionAlreadyDrunk = true;
        facts.lastPotionIsThisItem = true;  // the recorded potion IS this one - it was drunk once and refused since
        return facts;
    }
}  // namespace

// THE DEFECT ITSELF: 45 of Selalleas' 46 lines are this. Nothing in the core changed on those ticks, so nothing may
// be reported as a heal - and with the report false the tick goes on to the run.
TEST(ItemUseSeam, APotionTheCoreWillNotTakeIsNotUsableNow)
{
    EXPECT_FALSE(Rpg::ItemUse::UsableNow(HealingPotionRetryInTheSameFight()))
        << "the potion already drunk in this combat is offered again: 46 [death385] lines against one [eco194] "
           "Healing Potion, Selalleas L26 hp 271 -> 26 standing still (b503, 1794752269..1794752301)";
}

// ... and the use that DID go through is reported, so the single real drink is not lost
TEST(ItemUseSeam, TheUseTheCoreRecordedIsReported)
{
    EXPECT_TRUE(Rpg::ItemUse::Accepted(HealingPotionJustAccepted()))
        << "the one drink the core accepted is reported as not having happened";
}

// THE OTHER DIRECTION of the report: before anything was asked, nothing has been accepted. A rule that answers
// "accepted" here is the old unconditional `cast = true`.
TEST(ItemUseSeam, NothingIsAcceptedBeforeTheCoreRecordsAnything)
{
    EXPECT_FALSE(Rpg::ItemUse::Accepted(HealingPotionFirstDrink()))
        << "an item use is reported as a heal although the core recorded neither a drink nor a cooldown: this is "
           "the unconditional cast = true that ate 45 flee ticks";
}

// the first drink of the fight must still be offered: a rule that simply refuses potions in combat would pass the
// test above and starve the bot of its kit
TEST(ItemUseSeam, TheFirstPotionOfTheFightIsUsable)
{
    EXPECT_TRUE(Rpg::ItemUse::UsableNow(HealingPotionFirstDrink()));
}

// a NON-potion item (a healthstone, a trinket) is cooled down normally, so the cooldown is both its block and the
// record of its use
TEST(ItemUseSeam, ANonPotionItemIsJudgedByItsCooldown)
{
    Facts ready;
    ready.isPotion = false;
    ready.spellOnCooldown = false;
    EXPECT_TRUE(Rpg::ItemUse::UsableNow(ready));
    EXPECT_FALSE(Rpg::ItemUse::Accepted(ready));

    Facts used = ready;
    used.spellOnCooldown = true;
    EXPECT_FALSE(Rpg::ItemUse::UsableNow(used));
    EXPECT_TRUE(Rpg::ItemUse::Accepted(used));
}

// a potion whose on-use spell IS on cooldown (out of combat, after Player::UpdatePotionCooldown wrote it) is not
// usable either: the cooldown question is not skipped for items any more
TEST(ItemUseSeam, APotionOnCooldownOutOfCombatIsNotUsable)
{
    Facts facts = HealingPotionFirstDrink();
    facts.spellOnCooldown = true;
    EXPECT_FALSE(Rpg::ItemUse::UsableNow(facts))
        << "the chooser skipped the cooldown question for every item (`piece.item || !HasSpellCooldown(...)`)";
}
