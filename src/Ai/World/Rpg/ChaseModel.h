/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CHASE_MODEL_H
#define PLAYERBOTS_CHASE_MODEL_H

// [runpast88] THE CHASE WINDOW, AS A SEAM. How long a mob chases a bot that walks past it, and how long of that
// chase its melee lands, used to live inside `ChaseTimes` in SoloSafety.cpp - a function that takes a `Player*` and
// reads the world (its speed, whether it swims, the daze spell's data, the leash radius config). The arithmetic
// itself needs none of that: it is six numbers in and two out. It is here, depending on nothing but <algorithm>,
// so a chase recorded in a real death can be put back and the same two numbers demanded of it with no server.
//
// WHY IT IS WORTH A SEAM, in the numbers of the b502 hour (three of the eight deaths):
//   * Lanbuzz L26 (demo warlock, no instant speed piece) ran past a Barrens Guard L40. Both run at the speeds the
//     data gives - the bot 7.0 yd/s, every mob in that hour 1.14286 * 7 = 8.0 yd/s - so the chase was priced at the
//     WHOLE window: `until` = max(leashSec 13, leashRadius 30 / dazed follow 3.5) + 1 = 14 s, exposure the same 14 s,
//     577 dmg at 41 dps against hp 924. The chase ran 29.4 s and took 911: the damage per second was right
//     (measured 32.4), the WINDOW was 2.10 x longer than priced.
//   * Tuihgrelfim L24 (rogue, Sprint) and Irrua L25 (shaman, Ghost Wolf) ran past Barrens Guards too. With the
//     speed piece the bot is FASTER than the chaser (9.1 against 8.0), so `canDaze` below is false and the exposure
//     collapses to `reach / (vb - vm)` - 4.5 s of the same 14 s chase, under a third of it. Priced 69 and 84 dmg.
//     Tuihgrelfim took 414 from that one guard over 16.3 s at a measured 25.2 dps.
//
// The two cases are the two branches of `Window`, and both come out low. What the model does NOT carry is named in
// `Window`'s own comments rather than fixed here, because changing either one changes which walks a bot takes, and
// that is the user's decision and not this seam's.
//
// Nothing here reads the world, the clock or the database. One judge of "how long is this chase": this function.

#include <algorithm>

namespace Chase
{
    // how long the chase lasts (`until`) and how much of it the mob's melee lands (`exposure`), both in seconds
    struct Window
    {
        float until = 0.0f;
        float exposure = 0.0f;
    };

    // `botSpeed`: the bot's run (or swim) speed WITH whatever instant speed piece the run-past executor casts first
    //     (SurvivalKit::RunPastSpeed) - the caller applies it, this function does not know about spells.
    // `mobSpeed`: the chaser's run (or swim) speed, the same way.
    // `leashSec`: Creature::GetLeashTimer - 11 s for a level 1-5 mob rising to 16 s at level 70+.
    // `leashRadius`: CONFIG_CREATURE_LEASH_RADIUS (CreatureLeashRadius in worldserver.conf, 30 on this server).
    // `dazedShare`: the share of the bot's speed the daze (spell 1604) takes off, as the spell's data gives it -
    //     a NEGATIVE number (-0.5 for the stock -50 %), or 0 when daze is turned off in the config.
    // `melee`: the chaser's melee damage per second against this bot; 0 means its swings land nothing.
    // `reach`: the chaser's melee range against this bot, in yards.
    Window Of(float botSpeed, float mobSpeed, float leashSec, float leashRadius, float dazedShare, float melee,
              float reach);
}

#endif
