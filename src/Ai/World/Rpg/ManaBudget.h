/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MANA_BUDGET_H
#define PLAYERBOTS_MANA_BUDGET_H

// [mana90] HOW MANY SECONDS OF THIS FIGHT THE MANA HOLDS - a quantity the project did not have, and a seam.
//
// THE DEFECT IT CLOSES, in the numbers of the b502 hour. Four of its eight deaths are "lost out of mana"
// (`[death465]`: Selalleas, Emdyl, Orenem, Lanbuzz; 19 such lines in the hour). The clearest is Selalleas L26, a
// resto druid: the fight ran 167.7 s, it healed itself 2628 points against a body of 774 - three and a half bodies -
// and died to five Dragonmaw at a measured 50.6 incoming dps.
//
// AND EVERY SINGLE LOOK THE JUDGE TOOK WAS RIGHT. Its own line says
// "3 attackers 37.6 dps, 24 s at its 45.9 dps: 683 dmg to come at hp 479 (+175 heals)" - 683 over 24 s at hp 479
// with 175 of heals is arithmetic that holds. Episode after episode held. The error is that THERE WAS NO JUDGE OF
// THE FIGHT: the estimate looked 24 seconds ahead of a fight that ran almost three minutes, and nothing ever asked
// whether the mana reaches the end of it. SurvivalKit::FightHeals already walks the rotation's mana over
// `fight.seconds`, so the arithmetic of one episode was never the gap - the gap was that `fight.seconds` is the
// seconds of the episode and the mana is spent over the whole fight.
//
// WHAT IT IS NOT. It is not a timer and no rule of elapsed time (rule 2): nothing here reads a clock, and the answer
// changes only when a fact of the fight changes. It is not a threshold either (rule 3): every number in `Fight`
// comes from the game - the mana the bot has, what its rotation spends by its own play, the mana it gets back in
// combat, the health its own best heal puts back per point of mana (its spell data), the damage per second the
// fight deals it, and the judge's OWN estimate of how long the fight runs. There is no constant to tune.
//
// Nothing here reads the world, the clock or the database. One judge of "does the mana reach the end of this
// fight": this function.

#include <algorithm>
#include <limits>

namespace ManaBudget
{
    // the fight as the judge already knows it
    struct Fight
    {
        float fightSec = 0.0f;     // the judge's OWN estimate of how long this fight runs (Verdict::fightSec)
        float incomingDps = 0.0f;  // the damage the fight deals the bot per second
        float health = 0.0f;       // its health: the seconds of the fight its own body pays for before any heal
        float mana = 0.0f;         // the mana it has
        float manaPerSec = 0.0f;   // what its rotation spends per second, by its own play
        float regenPerSec = 0.0f;  // the mana it gets back WHILE IN COMBAT (the game's own number, not out of it)
        float healPerMana = 0.0f;  // health its best heal puts back per point of mana; 0: it cannot heal at all
    };

    struct Answer
    {
        // the seconds of this fight the mana holds; infinite when the fight never drains it
        float holdsSec = std::numeric_limits<float>::infinity();
        bool holds = true;      // ... and whether that reaches `fightSec`
        float needMana = 0.0f;  // the mana the whole fight asks for, whether or not the bot has it
    };

    // The fight in two spans, because a heal that is not needed yet costs nothing: while the bot's own body still
    // covers the incoming damage only its rotation spends, and from the moment the body is gone every point of
    // damage has to be bought back with mana at the rate its own heal gives. Mana GAINED is never credited (the
    // maximum it could be stored up to is not a number this function is given), so the answer errs towards
    // "it runs out" and never towards "it holds".
    Answer Of(Fight const& fight);
}

#endif
