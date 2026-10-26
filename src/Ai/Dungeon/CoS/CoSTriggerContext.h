/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COSTRIGGERCONTEXT_H
#define PLAYERBOTS_COSTRIGGERCONTEXT_H

#include "CoSTriggers.h"
#include "NamedObjectContext.h"

class WotlkDungeonCoSTriggerContext : public NamedObjectContext<Trigger>
{
    public:
        WotlkDungeonCoSTriggerContext()
        {
            creators["explode ghoul"] = &WotlkDungeonCoSTriggerContext::explode_ghoul;
            creators["epoch ranged"] = &WotlkDungeonCoSTriggerContext::epoch_ranged;
            creators["cos chromie start"] = &WotlkDungeonCoSTriggerContext::cos_chromie_start;
            creators["cos suspicious crate"] = &WotlkDungeonCoSTriggerContext::cos_suspicious_crate;
            creators["cos chromie middle"] = &WotlkDungeonCoSTriggerContext::cos_chromie_middle;
            creators["cos arthas gossip"] = &WotlkDungeonCoSTriggerContext::cos_arthas_gossip;
            creators["cos event target"] = &WotlkDungeonCoSTriggerContext::cos_event_target;
            creators["cos follow arthas"] = &WotlkDungeonCoSTriggerContext::cos_follow_arthas;
        }
    private:
        static Trigger* explode_ghoul(PlayerbotAI* botAI) { return new ExplodeGhoulTrigger(botAI); }
        static Trigger* epoch_ranged(PlayerbotAI* botAI) { return new EpochRangedTrigger(botAI); }
        static Trigger* cos_chromie_start(PlayerbotAI* botAI) { return new CoSChromieStartTrigger(botAI); }
        static Trigger* cos_suspicious_crate(PlayerbotAI* botAI) { return new CoSSuspiciousCrateTrigger(botAI); }
        static Trigger* cos_chromie_middle(PlayerbotAI* botAI) { return new CoSChromieMiddleTrigger(botAI); }
        static Trigger* cos_arthas_gossip(PlayerbotAI* botAI) { return new CoSArthasGossipTrigger(botAI); }
        static Trigger* cos_event_target(PlayerbotAI* botAI) { return new CoSEventTargetTrigger(botAI); }
        static Trigger* cos_follow_arthas(PlayerbotAI* botAI) { return new CoSFollowArthasTrigger(botAI); }
};

#endif
