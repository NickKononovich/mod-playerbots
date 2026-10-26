/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_OCTRIGGERCONTEXT_H
#define PLAYERBOTS_OCTRIGGERCONTEXT_H

#include "NamedObjectContext.h"
#include "OCTriggers.h"

class WotlkDungeonOccTriggerContext : public NamedObjectContext<Trigger>
{
    public:
        WotlkDungeonOccTriggerContext()
        {
            creators["drake mount"] = &WotlkDungeonOccTriggerContext::drake_mount;
            creators["drake dismount"] = &WotlkDungeonOccTriggerContext::drake_dismount;
            creators["group flying"] = &WotlkDungeonOccTriggerContext::group_flying;
            creators["drake combat"] = &WotlkDungeonOccTriggerContext::drake_combat;
            creators["varos cloudstrider"] = &WotlkDungeonOccTriggerContext::varos_cloudstrider;
            creators["arcane explosion"] = &WotlkDungeonOccTriggerContext::arcane_explosion;
            creators["time bomb"] = &WotlkDungeonOccTriggerContext::time_bomb;
            creators["oc nexus portal"] = &WotlkDungeonOccTriggerContext::oc_nexus_portal;
            creators["oc pull boss"] = &WotlkDungeonOccTriggerContext::oc_pull_boss;
            creators["oc landing hold"] = &WotlkDungeonOccTriggerContext::oc_landing_hold;
            creators["oc planar anomaly"] = &WotlkDungeonOccTriggerContext::oc_planar_anomaly;  // [b36_occ]
        }
    private:
        static Trigger* drake_mount(PlayerbotAI* botAI) { return new DrakeMountTrigger(botAI); }
        static Trigger* drake_dismount(PlayerbotAI* botAI) { return new DrakeDismountTrigger(botAI); }
        static Trigger* group_flying(PlayerbotAI* botAI) { return new GroupFlyingTrigger(botAI); }
        static Trigger* drake_combat(PlayerbotAI* botAI) { return new DrakeCombatTrigger(botAI); }
        static Trigger* varos_cloudstrider(PlayerbotAI* botAI) { return new VarosCloudstriderTrigger(botAI); }
        static Trigger* arcane_explosion(PlayerbotAI* botAI) { return new UromArcaneExplosionTrigger(botAI); }
        static Trigger* time_bomb(PlayerbotAI* botAI) { return new UromTimeBombTrigger(botAI); }
        static Trigger* oc_nexus_portal(PlayerbotAI* botAI) { return new OcNexusPortalTrigger(botAI); }
        static Trigger* oc_pull_boss(PlayerbotAI* botAI) { return new OcPullBossTrigger(botAI); }
        static Trigger* oc_landing_hold(PlayerbotAI* botAI) { return new OcLandingHoldTrigger(botAI); }
        static Trigger* oc_planar_anomaly(PlayerbotAI* botAI) { return new OcPlanarAnomalyTrigger(botAI); }
};

#endif
