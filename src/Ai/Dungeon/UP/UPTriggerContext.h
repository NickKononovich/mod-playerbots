/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_UPTRIGGERCONTEXT_H
#define PLAYERBOTS_UPTRIGGERCONTEXT_H

#include "NamedObjectContext.h"
#include "UPTriggers.h"

class WotlkDungeonUPTriggerContext : public NamedObjectContext<Trigger>
{
    public:
        WotlkDungeonUPTriggerContext()
        {
            creators["freezing cloud"] = &WotlkDungeonUPTriggerContext::freezing_cloud;
            creators["skadi whirlwind"] = &WotlkDungeonUPTriggerContext::whirlwind;
            creators["ymiron bane"] = &WotlkDungeonUPTriggerContext::bane;
            creators["ymiron bane purge"] = &WotlkDungeonUPTriggerContext::bane_purge;
            creators["svala start ready"] = &WotlkDungeonUPTriggerContext::svala_start_ready;
            creators["svala ritual"] = &WotlkDungeonUPTriggerContext::svala_ritual;
            creators["skadi breath side"] = &WotlkDungeonUPTriggerContext::skadi_breath_side;
            creators["skadi harpooner"] = &WotlkDungeonUPTriggerContext::skadi_harpooner;
            creators["gortok start ready"] = &WotlkDungeonUPTriggerContext::gortok_start_ready;
            creators["skadi start ready"] = &WotlkDungeonUPTriggerContext::skadi_start_ready;
            creators["skadi gauntlet"] = &WotlkDungeonUPTriggerContext::skadi_gauntlet;
            creators["skadi harpoon on ground"] = &WotlkDungeonUPTriggerContext::skadi_harpoon_on_ground;
            creators["skadi harpoon in hand"] = &WotlkDungeonUPTriggerContext::skadi_harpoon_in_hand;
        }
    private:
        static Trigger* freezing_cloud(PlayerbotAI* botAI) { return new SkadiFreezingCloudTrigger(botAI); }
        static Trigger* whirlwind(PlayerbotAI* botAI) { return new SkadiWhirlwindTrigger(botAI); }
        static Trigger* bane(PlayerbotAI* botAI) { return new YmironBaneTrigger(botAI); }
        static Trigger* bane_purge(PlayerbotAI* botAI) { return new YmironBanePurgeTrigger(botAI); }
        static Trigger* svala_start_ready(PlayerbotAI* botAI) { return new SvalaStartReadyTrigger(botAI); }
        static Trigger* svala_ritual(PlayerbotAI* botAI) { return new SvalaRitualTrigger(botAI); }
        static Trigger* skadi_breath_side(PlayerbotAI* botAI) { return new SkadiBreathSideTrigger(botAI); }
        static Trigger* skadi_harpooner(PlayerbotAI* botAI) { return new SkadiHarpoonerTrigger(botAI); }
        static Trigger* gortok_start_ready(PlayerbotAI* botAI) { return new GortokStartReadyTrigger(botAI); }
        static Trigger* skadi_start_ready(PlayerbotAI* botAI) { return new SkadiStartReadyTrigger(botAI); }
        static Trigger* skadi_gauntlet(PlayerbotAI* botAI) { return new SkadiGauntletTrigger(botAI); }
        static Trigger* skadi_harpoon_on_ground(PlayerbotAI* botAI) { return new SkadiHarpoonOnGroundTrigger(botAI); }
        static Trigger* skadi_harpoon_in_hand(PlayerbotAI* botAI) { return new SkadiHarpoonInHandTrigger(botAI); }
};

#endif
