/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TOCACTIONCONTEXT_H
#define PLAYERBOTS_TOCACTIONCONTEXT_H

#include "Action.h"
#include "NamedObjectContext.h"
#include "TOCActions.h"

class WotlkDungeonToCActionContext : public NamedObjectContext<Action>
{
    public:
        WotlkDungeonToCActionContext() {
            creators["toc lance"] = &WotlkDungeonToCActionContext::toc_lance;
            creators["toc ue lance"] = &WotlkDungeonToCActionContext::toc_ue_lance;
            creators["toc mount"] = &WotlkDungeonToCActionContext::toc_mount;
            creators["toc mounted"] = &WotlkDungeonToCActionContext::toc_mounted;
            creators["toc eadric"] = &WotlkDungeonToCActionContext::toc_eadric;
            creators["toc start event"] = &WotlkDungeonToCActionContext::toc_start_event;
            creators["toc pull argent soldiers"] = &WotlkDungeonToCActionContext::toc_pull_argent_soldiers;
            creators["toc hold fire"] = &WotlkDungeonToCActionContext::toc_hold_fire;
            creators["toc interrupt"] = &WotlkDungeonToCActionContext::toc_interrupt;
            creators["toc avoid ghoul explode"] = &WotlkDungeonToCActionContext::toc_avoid_ghoul_explode;
            creators["toc dispel renew"] = &WotlkDungeonToCActionContext::toc_dispel_renew;
            creators["toc marked defensive"] = &WotlkDungeonToCActionContext::toc_marked_defensive;
            creators["toc heal marked"] = &WotlkDungeonToCActionContext::toc_heal_marked;
        }
    private:
        static Action* toc_lance(PlayerbotAI* botAI) { return new ToCLanceAction(botAI); }
        static Action* toc_ue_lance(PlayerbotAI* botAI) { return new ToCUELanceAction(botAI); }
        static Action* toc_mount(PlayerbotAI* botAI) { return new ToCMountAction(botAI); }
        static Action* toc_mounted(PlayerbotAI* botAI) { return new ToCMountedAction(botAI); }
        static Action* toc_eadric(PlayerbotAI* botAI) { return new ToCEadricAction(botAI); }
        static Action* toc_start_event(PlayerbotAI* botAI) { return new ToCStartEventAction(botAI); }
        static Action* toc_pull_argent_soldiers(PlayerbotAI* botAI) { return new ToCPullArgentSoldiersAction(botAI); }
        static Action* toc_hold_fire(PlayerbotAI* botAI) { return new ToCHoldFireAction(botAI); }
        static Action* toc_interrupt(PlayerbotAI* botAI) { return new ToCInterruptAction(botAI); }
        static Action* toc_avoid_ghoul_explode(PlayerbotAI* botAI) { return new ToCAvoidGhoulExplodeAction(botAI); }
        static Action* toc_dispel_renew(PlayerbotAI* botAI) { return new ToCDispelRenewAction(botAI); }
        static Action* toc_marked_defensive(PlayerbotAI* botAI) { return new ToCMarkedDefensiveAction(botAI); }
        static Action* toc_heal_marked(PlayerbotAI* botAI) { return new ToCHealMarkedAction(botAI); }
};

#endif
