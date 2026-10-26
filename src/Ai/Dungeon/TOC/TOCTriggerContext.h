/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TOCTRIGGERCONTEXT_H
#define PLAYERBOTS_TOCTRIGGERCONTEXT_H

#include "NamedObjectContext.h"
#include "TOCTriggers.h"

class WotlkDungeonToCTriggerContext : public NamedObjectContext<Trigger>
{
    public:
        WotlkDungeonToCTriggerContext()
        {
            creators["toc lance"] = &WotlkDungeonToCTriggerContext::toc_lance;
            creators["toc ue lance"] = &WotlkDungeonToCTriggerContext::toc_ue_lance;
            creators["toc mount near"] = &WotlkDungeonToCTriggerContext::toc_mount_near;
            creators["toc mounted"] = &WotlkDungeonToCTriggerContext::toc_mounted;
            creators["toc eadric"] = &WotlkDungeonToCTriggerContext::toc_eadric;
            creators["toc start event"] = &WotlkDungeonToCTriggerContext::toc_start_event;
            creators["toc argent soldiers"] = &WotlkDungeonToCTriggerContext::toc_argent_soldiers;
            creators["toc reflective shield"] = &WotlkDungeonToCTriggerContext::toc_reflective_shield;
            creators["toc interrupt"] = &WotlkDungeonToCTriggerContext::toc_interrupt;
            creators["toc ghoul explode"] = &WotlkDungeonToCTriggerContext::toc_ghoul_explode;
            creators["toc paletress renew"] = &WotlkDungeonToCTriggerContext::toc_paletress_renew;
            creators["toc marked for death"] = &WotlkDungeonToCTriggerContext::toc_marked_for_death;
            creators["toc marked for death heal"] = &WotlkDungeonToCTriggerContext::toc_marked_for_death_heal;
        }
    private:
        static Trigger* toc_lance(PlayerbotAI* botAI) { return new ToCLanceTrigger(botAI); }
        static Trigger* toc_ue_lance(PlayerbotAI* botAI) { return new ToCUELanceTrigger(botAI); }
        static Trigger* toc_mount_near(PlayerbotAI* botAI) { return new ToCMountNearTrigger(botAI); }
        static Trigger* toc_mounted(PlayerbotAI* botAI) { return new ToCMountedTrigger(botAI); }
        static Trigger* toc_eadric(PlayerbotAI* botAI) { return new ToCEadricTrigger(botAI); }
        static Trigger* toc_start_event(PlayerbotAI* botAI) { return new ToCStartEventTrigger(botAI); }
        static Trigger* toc_argent_soldiers(PlayerbotAI* botAI) { return new ToCArgentSoldiersTrigger(botAI); }
        static Trigger* toc_reflective_shield(PlayerbotAI* botAI) { return new ToCReflectiveShieldTrigger(botAI); }
        static Trigger* toc_interrupt(PlayerbotAI* botAI) { return new ToCInterruptTrigger(botAI); }
        static Trigger* toc_ghoul_explode(PlayerbotAI* botAI) { return new ToCGhoulExplodeTrigger(botAI); }
        static Trigger* toc_paletress_renew(PlayerbotAI* botAI) { return new ToCPaletressRenewTrigger(botAI); }
        static Trigger* toc_marked_for_death(PlayerbotAI* botAI) { return new ToCMarkedForDeathTrigger(botAI); }
        static Trigger* toc_marked_for_death_heal(PlayerbotAI* botAI)
        {
            return new ToCMarkedForDeathHealTrigger(botAI);
        }
};

#endif
