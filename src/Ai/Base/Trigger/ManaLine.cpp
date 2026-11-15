/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ManaLine.h"

namespace ManaLine
{
    Composed Of(Terms const& terms)
    {
        Composed out;
        float const nonstop = terms.nonstopPctPerSec * terms.fightSec;
        // the witness: the mana the fight's own damage asks this healer to heal, plus its upkeep less the regen it
        // earns casting through the fight. Never negative - a fight whose regen outruns its upkeep is free, not a
        // refund.
        float const witness =
            terms.healer ? std::max(terms.healPerTankHealthPct * terms.cost +
                                        (terms.upkeepPctPerSec - terms.regenPctPerSec) * terms.fightSec,
                                    0.0f)
                         : 0.0f;
        // [pull84] A healer's line is the witness, on a boss exactly as on trash. The nonstop bound stays only
        // where it IS the formula: a member whose spend really is its rotation (a tank's upkeep, a damage caster's
        // casts), and a healer whose heal is not read yet - absence is not zero, so the bound is all there is.
        if (terms.healer && terms.healPerTankHealthPct > 0.0f)
        {
            out.needPct = witness;
            out.witness = true;
        }
        else if (terms.healer)
        {
            out.needPct = std::max(nonstop, witness);
        }
        else
        {
            out.needPct = nonstop;
        }

        // the reserve is one more mob's fight at the SAME rate the need is read on, so that a need read off the
        // witness is not reserved for at a bound it never spends at
        bool const witnessReserve = terms.healer && terms.healPerTankHealthPct > 0.0f && terms.fightSec > 0.0f;
        out.reservePct = witnessReserve ? 0.0f : terms.nonstopPctPerSec * terms.oneMobSec;
        if (terms.healer && terms.fightSec > 0.0f)
            out.reservePct += terms.healPerTankHealthPct * terms.cost * terms.oneMobSec / terms.fightSec +
                              terms.upkeepPctPerSec * terms.oneMobSec;
        // [lead1400] the boss caution, named apart and added as itself: one whole cast of its heal left in the pool
        out.reservePct += terms.healFloorPct;
        return out;
    }

    float Raw(Composed const& composed, Terms const& terms)
    {
        float raw = std::max({composed.needPct + composed.reservePct - terms.returnedPct, terms.potionFloorPct,
                              terms.cooldownFloorPct});
        // [lead1430] the credits are sized to the shortfall, so without this they cancel the very floor the heal
        // floor put into the reserve: the floor is the pool's bottom and no credit covers it
        if (terms.healFloorPct > 0.0f)
            raw = std::max({raw, terms.healFloorPct, terms.potionFloorPct + terms.healFloorPct,
                            terms.cooldownFloorPct + terms.healFloorPct});
        return raw;
    }

    Verdict Read(float rawPct, float floorPct)
    {
        Verdict out;
        out.overPool = rawPct > POOL_PCT + POOL_EPS_PCT;
        out.isWait = !out.overPool;
        out.linePct = std::clamp(rawPct, floorPct, POOL_PCT);
        return out;
    }
}
