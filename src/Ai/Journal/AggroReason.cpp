/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AggroReason.h"

namespace Journal::AggroReason
{
    bool Expected(Facts const& facts)
    {
        return facts.pullOrderOpen && facts.onThePlansTank && facts.thePlansMob;
    }

    char const* Of(Facts const& facts)
    {
        // An ADDITION, and the word now means what it says: the bot was already fighting SOMEBODY ELSE when this mob
        // engaged it. The mob itself is excluded from that test, which is the whole difference from the dead version.
        if (facts.inCombatWithOthers)
            return "add";

        // A judged pull was open. Either it arrived as the plan said, or the plan was wrong - and the second case is
        // the one the user asked this table to carry.
        if (facts.pullOrderOpen)
            return Expected(facts) ? "came" : "estimate";

        // No order named it. Walking into it and standing still while it walks into you are different problems, and
        // the leader's route is answerable for only one of them.
        return facts.moving ? "path" : "came";
    }
}  // namespace Journal::AggroReason
