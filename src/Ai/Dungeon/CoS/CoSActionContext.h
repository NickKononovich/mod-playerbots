/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COSACTIONCONTEXT_H
#define PLAYERBOTS_COSACTIONCONTEXT_H

#include "Action.h"
#include "CoSActions.h"
#include "NamedObjectContext.h"

class WotlkDungeonCoSActionContext : public NamedObjectContext<Action>
{
    public:
        WotlkDungeonCoSActionContext() {
            creators["explode ghoul spread"] = &WotlkDungeonCoSActionContext::explode_ghoul_spread;
            creators["epoch stack"] = &WotlkDungeonCoSActionContext::epoch_stack;
            creators["cos get arcane disruptor"] = &WotlkDungeonCoSActionContext::cos_get_arcane_disruptor;
            creators["cos dispel crate"] = &WotlkDungeonCoSActionContext::cos_dispel_crate;
            creators["cos start arthas intro"] = &WotlkDungeonCoSActionContext::cos_start_arthas_intro;
            creators["cos arthas gossip"] = &WotlkDungeonCoSActionContext::cos_arthas_gossip;
            creators["cos engage event target"] = &WotlkDungeonCoSActionContext::cos_engage_event_target;
            creators["cos follow arthas"] = &WotlkDungeonCoSActionContext::cos_follow_arthas;
        }
    private:
        static Action* explode_ghoul_spread(PlayerbotAI* botAI) { return new ExplodeGhoulSpreadAction(botAI); }
        static Action* epoch_stack(PlayerbotAI* botAI) { return new EpochStackAction(botAI); }
        static Action* cos_get_arcane_disruptor(PlayerbotAI* botAI) { return new CoSGetArcaneDisruptorAction(botAI); }
        static Action* cos_dispel_crate(PlayerbotAI* botAI) { return new CoSDispelCrateAction(botAI); }
        static Action* cos_start_arthas_intro(PlayerbotAI* botAI) { return new CoSStartArthasIntroAction(botAI); }
        static Action* cos_arthas_gossip(PlayerbotAI* botAI) { return new CoSArthasGossipAction(botAI); }
        static Action* cos_engage_event_target(PlayerbotAI* botAI) { return new CoSEngageEventTargetAction(botAI); }
        static Action* cos_follow_arthas(PlayerbotAI* botAI) { return new CoSFollowArthasAction(botAI); }
};

#endif
