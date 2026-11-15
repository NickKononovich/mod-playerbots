/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ChainPullJudge.h"

namespace Leader::Judge
{
    char const* GateName(Gate gate)
    {
        switch (gate)
        {
            case Gate::Boss:
                return "boss";
            case Gate::Walker:
                return "walker";
            case Gate::Opener:
                return "opener";
            case Gate::Waits:
                return "waits";
            case Gate::PackReach:
                return "pack_reach";
            case Gate::Buff:
                return "buff";
            case Gate::Dead:
                return "dead";
            case Gate::Holds:
                return "holds";
            case Gate::Overload:
                return "overload";
            case Gate::Health:
                return "health";
            case Gate::Healer:
                return "healer";
            case Gate::Mana:
                return "mana";
            case Gate::Wakes:
                return "wakes";
            case Gate::Taken:
                return "taken";
            default:
                return "undecided";
        }
    }

    ChainPullReplay ReplayChainPull(ChainPullFacts const& facts)
    {
        ChainPullReplay out;
        out.due = DueNow(facts.timing);

        // the gates in the order JudgeChainPull (PullFacts.cpp) runs them. `need` ends the replay where the fixture
        // carries no answer: an unrecorded fact is not a passed gate
        auto const need = [](Tri fact, bool& trips) -> bool
        {
            if (fact == Tri::Unknown)
                return false;
            trips = fact == Tri::No;
            return true;
        };
        // a fact whose Yes is the gate tripping (a boss in the pack, a walker's stop) read the other way round
        auto const invert = [](Tri fact)
        { return fact == Tri::Unknown ? Tri::Unknown : (fact == Tri::Yes ? Tri::No : Tri::Yes); };
        bool trips = false;

        if (!need(invert(facts.bossInPack), trips))
            return out;
        if (trips)
        {
            out.gate = Gate::Boss;
            return out;
        }
        if (!need(invert(facts.walkerStop), trips))
            return out;
        if (trips)
        {
            out.gate = Gate::Walker;
            return out;
        }
        if (!need(facts.openerReaches, trips))
            return out;
        if (trips)
        {
            out.gate = Gate::Opener;
            return out;
        }
        // the trigger: recomputed here, never read back from the fixture
        if (!out.due)
        {
            out.gate = Gate::Waits;
            return out;
        }
        if (!need(facts.packReached, trips))
            return out;
        if (trips)
        {
            out.gate = Gate::PackReach;
            return out;
        }
        if (!need(facts.groupBuffed, trips))
            return out;
        if (trips)
        {
            out.gate = Gate::Buff;
            return out;
        }
        if (!need(facts.everyoneAlive, trips))
            return out;
        if (trips)
        {
            out.gate = Gate::Dead;
            return out;
        }
        if (!need(facts.tankHolds, trips))
            return out;
        if (trips)
        {
            out.gate = Gate::Holds;
            return out;
        }
        if (!need(facts.overloadFits, trips))
            return out;
        if (trips)
        {
            out.gate = Gate::Overload;
            return out;
        }
        // the health gate: the one numeric gate after the overload model, through the production predicate
        if (facts.survival.tankHealth < 0.0f || facts.survival.drop < 0.0f)
            return out;
        if (!TankOutlastsDrop(facts.survival))
        {
            out.gate = Gate::Health;
            return out;
        }
        if (!need(facts.healerKeeps, trips))
            return out;
        if (trips)
        {
            out.gate = Gate::Healer;
            return out;
        }
        if (!need(facts.healerHasMana, trips))
            return out;
        if (trips)
        {
            out.gate = Gate::Mana;
            return out;
        }
        if (!need(facts.wayInQuiet, trips))
            return out;
        if (trips)
        {
            out.gate = Gate::Wakes;
            return out;
        }
        out.gate = Gate::Taken;
        out.taken = true;
        return out;
    }
}
