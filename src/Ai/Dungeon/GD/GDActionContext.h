/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GDACTIONCONTEXT_H
#define PLAYERBOTS_GDACTIONCONTEXT_H

#include "Action.h"
#include "GDActions.h"
#include "NamedObjectContext.h"

class WotlkDungeonGDActionContext : public NamedObjectContext<Action>
{
    public:
        WotlkDungeonGDActionContext() {
            creators["avoid poison nova"] = &WotlkDungeonGDActionContext::avoid_poison_nova;
            creators["attack snake wrap"] = &WotlkDungeonGDActionContext::attack_snake_wrap;
            creators["slad'ran stack on tank"] = &WotlkDungeonGDActionContext::sladran_stack_on_tank;
            creators["slad'ran tank hold"] = &WotlkDungeonGDActionContext::sladran_tank_hold;
            creators["slad'ran tank park"] = &WotlkDungeonGDActionContext::sladran_tank_park;
            creators["avoid whirling slash"] = &WotlkDungeonGDActionContext::avoid_whirling_slash;
            creators["gundrak use altar"] = &WotlkDungeonGDActionContext::gundrak_use_altar;
            creators["engage living mojo"] = &WotlkDungeonGDActionContext::engage_living_mojo;
            creators["gundrak cross bridge"] = &WotlkDungeonGDActionContext::gundrak_cross_bridge;
            creators["gundrak circle route"] = &WotlkDungeonGDActionContext::gundrak_circle_route;
            creators["gundrak moat drop"] = &WotlkDungeonGDActionContext::gundrak_moat_drop;  // [b34_gd]
            creators["gundrak moat exit"] = &WotlkDungeonGDActionContext::gundrak_moat_exit;  // [b34_gd]
        }
    private:
        static Action* avoid_poison_nova(PlayerbotAI* botAI) { return new AvoidPoisonNovaAction(botAI); }
        static Action* attack_snake_wrap(PlayerbotAI* botAI) { return new AttackSnakeWrapAction(botAI); }
        static Action* sladran_stack_on_tank(PlayerbotAI* botAI) { return new SladranStackOnTankAction(botAI); }
        static Action* sladran_tank_hold(PlayerbotAI* botAI) { return new SladranTankHoldAction(botAI); }
        static Action* sladran_tank_park(PlayerbotAI* botAI) { return new SladranTankParkAction(botAI); }
        static Action* avoid_whirling_slash(PlayerbotAI* botAI) { return new AvoidWhirlingSlashAction(botAI); }
        static Action* gundrak_use_altar(PlayerbotAI* botAI) { return new GundrakUseAltarAction(botAI); }
        static Action* engage_living_mojo(PlayerbotAI* botAI) { return new EngageLivingMojoAction(botAI); }
        static Action* gundrak_cross_bridge(PlayerbotAI* botAI) { return new GundrakCrossBridgeAction(botAI); }
        static Action* gundrak_circle_route(PlayerbotAI* botAI) { return new GundrakCircleRouteAction(botAI); }
        static Action* gundrak_moat_drop(PlayerbotAI* botAI) { return new GundrakMoatDropAction(botAI); }
        static Action* gundrak_moat_exit(PlayerbotAI* botAI) { return new GundrakMoatExitAction(botAI); }
};

#endif
