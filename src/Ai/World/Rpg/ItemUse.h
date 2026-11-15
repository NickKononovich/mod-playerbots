/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AI_WORLD_RPG_ITEMUSE_H
#define PLAYERBOTS_AI_WORLD_RPG_ITEMUSE_H

// [death475] WAS THE ITEM USE ACCEPTED BY THE CORE, AS A SEAM: the facts go in, the answer comes out, and nothing
// here reads the world, the clock or the database. Header-only, so the unit stand compiles it alone.
//
// WHAT WAS WRONG, measured. `Player::CastItemUseSpell` returns `void`, so the run-past heal set its own success
// unconditionally for an item - `cast = true` whenever the item was merely in the bags. And the chooser
// (SurvivalKit::BestOnTheMove) asked an item NOTHING about being ready, although its own comment promises
// "castable now": the test read `piece.item || !bot->HasSpellCooldown(piece.spell)`, so the cooldown question was
// skipped for every item there is. Selalleas L26 in the 32 s before its death: 46 `[death385]` lines claiming a heal
// on the move against ONE `[eco194]` Healing Potion (item 929) actually used, health 271 -> 26, standing still -
// because the caller does `if (RunPastHeal(...)) return true;` and so a claimed heal ate the flee tick every tick.
//
// WHY A POTION'S COOLDOWN IS NOT THE QUESTION, and this is the project's own measured fact ([lead481], b208 rfc-h:
// 526 "mana potion" Consumes, a fresh one every ~0.3 s): a potion drunk IN COMBAT gets no cooldown at all.
// `Spell::SendSpellCooldown` sets `Player::m_lastPotionId` to the item's entry instead and returns, the real
// cooldown is only written when the combat ends (`Player::UpdatePotionCooldown`), and `Spell::CheckCast` refuses
// every further potion while that id is set. So `HasSpellCooldown` sees nothing and a chooser that asks only it
// will offer the same potion for ever. `GetLastPotionId()` is the fact that answers both questions - and it is a
// STATE, cleared by the core when the combat ends, never a clock (rule 2).
//
// NO NEW RULE OF CHOICE between healing and running: a use the core did not accept simply reports false, and the
// tick then goes on to the run exactly as it does when there is no heal to cast at all.

namespace Rpg::ItemUse
{
    // one item of the kit, as the core already knows it. `UsableNow` reads them BEFORE the use is asked for and
    // `Accepted` reads them again AFTER - two moments, the same four facts.
    struct Facts
    {
        bool isPotion = false;  // ItemTemplate: a potion (its combat use is recorded instead of cooled down)
        // Player::GetLastPotionId() names a potion already drunk in this combat (any potion: the core's block is
        // not per item)
        bool potionAlreadyDrunk = false;
        // ... and the id it names is THIS item: the core's own record that THIS use went through
        bool lastPotionIsThisItem = false;
        bool spellOnCooldown = false;  // Player::HasSpellCooldown of the item's on-use spell
    };

    // the core will accept a use of it right now. For a potion the block is the recorded drink and not a cooldown,
    // which is why asking the cooldown alone offered the same potion on every tick
    inline bool UsableNow(Facts const& facts)
    {
        if (facts.spellOnCooldown)
            return false;
        return !(facts.isPotion && facts.potionAlreadyDrunk);
    }

    // the core ACCEPTED the use just asked for: it recorded this potion as drunk, or it put the on-use spell on
    // cooldown. Neither is an assumption - both are the core's own state. When neither appears, the use did not
    // happen and must not be reported as a heal
    inline bool Accepted(Facts const& facts)
    {
        if (facts.isPotion)
            return facts.lastPotionIsThisItem;
        return facts.spellOnCooldown;
    }
}

#endif
