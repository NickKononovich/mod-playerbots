/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GDTRIGGERCONTEXT_H
#define PLAYERBOTS_GDTRIGGERCONTEXT_H

#include "GDTriggers.h"
#include "NamedObjectContext.h"

class WotlkDungeonGDTriggerContext : public NamedObjectContext<Trigger>
{
    public:
        WotlkDungeonGDTriggerContext()
        {
            creators["poison nova"] = &WotlkDungeonGDTriggerContext::poison_nova;
            creators["snake wrap"] = &WotlkDungeonGDTriggerContext::snake_wrap;
            creators["slad'ran stack on tank"] = &WotlkDungeonGDTriggerContext::sladran_stack_on_tank;
            creators["slad'ran tank hold"] = &WotlkDungeonGDTriggerContext::sladran_tank_hold;
            creators["slad'ran tank park"] = &WotlkDungeonGDTriggerContext::sladran_tank_park;
            creators["whirling slash"] = &WotlkDungeonGDTriggerContext::whirling_slash;
            creators["gundrak altar ready"] = &WotlkDungeonGDTriggerContext::gundrak_altar_ready;
            creators["drakkari colossus dormant"] = &WotlkDungeonGDTriggerContext::drakkari_colossus_dormant;
            creators["gundrak bridge formed"] = &WotlkDungeonGDTriggerContext::gundrak_bridge_formed;
            creators["gundrak circle route"] = &WotlkDungeonGDTriggerContext::gundrak_circle_route;
            creators["gundrak moat fight"] = &WotlkDungeonGDTriggerContext::gundrak_moat_fight;  // [b34_gd]
            creators["gundrak moat exit"] = &WotlkDungeonGDTriggerContext::gundrak_moat_exit;  // [b34_gd]
        }
    private:
        static Trigger* poison_nova(PlayerbotAI* botAI) { return new SladranPoisonNovaTrigger(botAI); }
        static Trigger* snake_wrap(PlayerbotAI* botAI) { return new SladranSnakeWrapTrigger(botAI); }
        static Trigger* sladran_stack_on_tank(PlayerbotAI* botAI) { return new SladranStackOnTankTrigger(botAI); }
        static Trigger* sladran_tank_hold(PlayerbotAI* botAI) { return new SladranTankHoldTrigger(botAI); }
        static Trigger* sladran_tank_park(PlayerbotAI* botAI) { return new SladranTankParkTrigger(botAI); }
        static Trigger* whirling_slash(PlayerbotAI* botAI) { return new GaldarahWhirlingSlashTrigger(botAI); }
        static Trigger* gundrak_altar_ready(PlayerbotAI* botAI) { return new GundrakAltarReadyTrigger(botAI); }
        static Trigger* drakkari_colossus_dormant(PlayerbotAI* botAI)
        {
            return new DrakkariColossusDormantTrigger(botAI);
        }
        static Trigger* gundrak_bridge_formed(PlayerbotAI* botAI) { return new GundrakBridgeFormedTrigger(botAI); }
        static Trigger* gundrak_circle_route(PlayerbotAI* botAI) { return new GundrakCircleRouteTrigger(botAI); }
        static Trigger* gundrak_moat_fight(PlayerbotAI* botAI) { return new GundrakMoatFightTrigger(botAI); }
        static Trigger* gundrak_moat_exit(PlayerbotAI* botAI) { return new GundrakMoatExitTrigger(botAI); }
};

#endif
