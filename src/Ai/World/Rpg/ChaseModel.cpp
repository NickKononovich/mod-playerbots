/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ChaseModel.h"

namespace Chase
{
    Window Of(float botSpeed, float mobSpeed, float leashSec, float leashRadius, float dazedShare, float melee,
              float reach)
    {
        Window out;
        // The daze is priced only where the mob is ALREADY faster, and that is the one case where it changes
        // nothing: a faster mob catches up with or without it. Where it decides the chase - a bot ahead by a hair,
        // which one dazing swing drops to half speed and behind - `canDaze` is false and the exposure below
        // collapses. b502: Tuihgrelfim L24 at 9.1 yd/s against a Barrens Guard L40 at 8.0 was priced 69 dmg over
        // 4.5 s of landed melee - under a third of the 14 s chase - and took 414 over 16.3 s. One swing from behind
        // makes that exposure 14.2 s, 3.12 x the priced one. Making the daze a consequence of the swing rather than
        // of the speeds raises the price of every run past, which is a change of behaviour, so it is a question for
        // the user and not a change made here.
        bool const canDaze = melee > 0.0f && mobSpeed > botSpeed && botSpeed * (1.0f + dazedShare) <= mobSpeed;
        float const follow = std::max(std::min(mobSpeed, canDaze ? botSpeed * (1.0f + dazedShare) : botSpeed), 0.1f);
        // + ThreatManager's 1 s, as the flee.
        //
        // NEITHER TERM OF THIS MAX IS THE CHASE'S END while the mob keeps landing hits. In the core
        // (Creature::CanCreatureAttack, Creature.cpp) the leash TIMER is a window since the last leash extension,
        // and Unit::DealDamage calls UpdateLeashExtensionTime on every hit - so a chaser that is hitting restarts
        // its own timer. That branch is tested BEFORE the distance check and returns "keep attacking", so the
        // radius never comes into it either. b502: Lanbuzz's chase was priced at this 14 s and ran 29 s, with the
        // guard 91 yd from its home - three times the 30 yd radius - still swinging. Bounding the chase by the fact
        // instead (it ends when the mob stops landing hits) prices such a chase far higher and would stop the bot
        // walking past a same-speed mob at all: the user's decision, named here, not taken here.
        out.until = std::max(leashSec, leashRadius / follow) + 1.0f;
        out.exposure = out.until;
        if (mobSpeed < botSpeed && !canDaze)
            out.exposure = std::min(out.until, reach / (botSpeed - mobSpeed));
        return out;
    }
}
