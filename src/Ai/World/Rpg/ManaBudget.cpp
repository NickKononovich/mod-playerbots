/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ManaBudget.h"

namespace ManaBudget
{
    Answer Of(Fight const& fight)
    {
        Answer out;
        float const fightSec = std::max(fight.fightSec, 0.0f);
        float const dps = std::max(fight.incomingDps, 0.0f);
        float const perMana = std::max(fight.healPerMana, 0.0f);
        // A bot that cannot turn mana into health is not killed by running out of it: its damage falls off and the
        // fight runs longer, which the fight's own estimate answers, but its body is the only thing between it and
        // the mobs either way. The mana is a bound on SURVIVING a fight only where it buys survival. Without this
        // the budget would refuse a mage every long fight on its rotation's spend alone - a refusal about damage
        // wearing a death's clothes.
        if (perMana <= 0.0f)
            return out;
        // the seconds its own body pays for before one point of mana has to be turned into health
        float const bodySec =
            dps > 0.0f ? std::max(fight.health, 0.0f) / dps : std::numeric_limits<float>::infinity();
        // the drain while the body still covers the damage, and the drain once it does not. Mana GAINED is not
        // credited (there is no maximum here to store it up to), so a regen above the spend only means "no drain".
        float const beforeHeals = std::max(fight.manaPerSec - std::max(fight.regenPerSec, 0.0f), 0.0f);
        float const underHeals = beforeHeals + (perMana > 0.0f ? dps / perMana : 0.0f);
        float const span = std::min(bodySec, fightSec);
        float const rest = std::max(fightSec - span, 0.0f);
        out.needMana = beforeHeals * span + underHeals * rest;
        float mana = std::max(fight.mana, 0.0f);
        if (beforeHeals > 0.0f && mana < beforeHeals * span)
        {
            out.holdsSec = mana / beforeHeals;
            out.holds = false;
            return out;
        }
        mana -= beforeHeals * span;
        if (underHeals <= 0.0f)
        {
            out.holdsSec = std::numeric_limits<float>::infinity();  // nothing drains it: it holds any fight
            out.holds = true;
            return out;
        }
        out.holdsSec = span + mana / underHeals;
        out.holds = out.holdsSec >= fightSec;
        return out;
    }
}
