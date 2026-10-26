/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_STRATTRIGGERCONTEXT_H
#define PLAYERBOTS_STRATTRIGGERCONTEXT_H

#include "NamedObjectContext.h"
#include "StratTriggers.h"

class ClassicDungeonStratholmeTriggerContext : public NamedObjectContext<Trigger>
{
public:
    ClassicDungeonStratholmeTriggerContext()
    {
        creators["strat aggro edge"] = &ClassicDungeonStratholmeTriggerContext::strat_aggro_edge;
        creators["strat member swarmed"] = &ClassicDungeonStratholmeTriggerContext::strat_member_swarmed;
    }

private:
    static Trigger* strat_aggro_edge(PlayerbotAI* botAI) { return new StratAggroEdgeTrigger(botAI); }
    static Trigger* strat_member_swarmed(PlayerbotAI* botAI) { return new StratMemberSwarmedTrigger(botAI); }
};

#endif
