/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_REZZEDTANKHOLDMULTIPLIER_H
#define PLAYERBOTS_REZZEDTANKHOLDMULTIPLIER_H

#include "Action.h"
#include "AiObjectContext.h"
#include "AttackAction.h"
#include "Multiplier.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Value.h"

// A tank resurrected in the fight (Rebirth, a soulstone) comes back low and unbuffed next to the pack that killed
// it. It holds off its taunts and attacks until the healer has brought it up, then takes the mobs back. Nothing is
// held once a mob hits it: it fights back.
class RezzedTankHoldMultiplier : public Multiplier
{
public:
    static constexpr float SAFE_HEALTH_PCT = 60.0f;
    static constexpr uint32 HOLD_MAX_MS = 45000;  // the hold ends here even when the heals do not come

    RezzedTankHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "rezzed tank hold") {}

    float GetValue(Action* action) override
    {
        Player* bot = botAI->GetBot();
        if (!action || !bot || !bot->IsAlive())
            return 1.0f;

        // "death count" goes up on every death; a change seen while alive is a resurrection
        uint32 const deaths = botAI->GetAiObjectContext()->GetValue<uint32>("death count")->Get();
        uint32 const now = getMSTime();
        if (!_seen)
        {
            _seen = true;
            _deaths = deaths;
        }
        else if (deaths != _deaths)
        {
            _deaths = deaths;
            _rezzedMs = now;
        }

        if (!_rezzedMs || getMSTimeDiff(_rezzedMs, now) > HOLD_MAX_MS)
            return 1.0f;

        if (bot->IsInCombat() || bot->GetHealthPct() >= SAFE_HEALTH_PCT || !PlayerbotAI::IsTank(bot))
            return 1.0f;

        if (dynamic_cast<AttackAction*>(action))
            return 0.0f;

        // a taunt, a gap closer or an opener on a mob: anything aimed at a hostile unit waits
        Unit* target = action->GetTarget();
        return target && target != bot && bot->IsValidAttackTarget(target) ? 0.0f : 1.0f;
    }

private:
    bool _seen = false;
    uint32 _deaths = 0;
    uint32 _rezzedMs = 0;
};

#endif
