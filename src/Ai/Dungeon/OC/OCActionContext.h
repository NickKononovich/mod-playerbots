/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_OCACTIONCONTEXT_H
#define PLAYERBOTS_OCACTIONCONTEXT_H

#include "Action.h"
#include "NamedObjectContext.h"
#include "OCActions.h"

class WotlkDungeonOccActionContext : public NamedObjectContext<Action>
{
    public:
        WotlkDungeonOccActionContext() {
            creators["mount drake"] = &WotlkDungeonOccActionContext::mount_drake;
            creators["dismount drake"] = &WotlkDungeonOccActionContext::dismount_drake;
            creators["occ fly drake"] = &WotlkDungeonOccActionContext::occ_fly_drake;
            creators["occ drake attack"] = &WotlkDungeonOccActionContext::occ_drake_attack;
            creators["avoid arcane explosion"] = &WotlkDungeonOccActionContext::avoid_arcane_explosion;
            creators["time bomb spread"] = &WotlkDungeonOccActionContext::time_bomb_spread;
            creators["oc use nexus portal"] = &WotlkDungeonOccActionContext::oc_use_nexus_portal;
            creators["oc pull boss"] = &WotlkDungeonOccActionContext::oc_pull_boss;
            creators["avoid energize cores"] = &WotlkDungeonOccActionContext::avoid_energize_cores;
            creators["oc hold landing spot"] = &WotlkDungeonOccActionContext::oc_hold_landing_spot;
            creators["avoid planar anomaly"] = &WotlkDungeonOccActionContext::avoid_planar_anomaly;  // [b36_occ]
        }
    private:
        static Action* mount_drake(PlayerbotAI* botAI) { return new MountDrakeAction(botAI); }
        static Action* dismount_drake(PlayerbotAI* botAI) { return new DismountDrakeAction(botAI); }
        static Action* occ_fly_drake(PlayerbotAI* botAI) { return new OccFlyDrakeAction(botAI); }
        static Action* occ_drake_attack(PlayerbotAI* botAI) { return new OccDrakeAttackAction(botAI); }
        static Action* avoid_arcane_explosion(PlayerbotAI* botAI) { return new AvoidArcaneExplosionAction(botAI); }
        static Action* time_bomb_spread(PlayerbotAI* botAI) { return new TimeBombSpreadAction(botAI); }
        static Action* oc_use_nexus_portal(PlayerbotAI* botAI) { return new OcUseNexusPortalAction(botAI); }
        static Action* oc_pull_boss(PlayerbotAI* botAI) { return new OcPullBossAction(botAI); }
        static Action* avoid_energize_cores(PlayerbotAI* botAI) { return new AvoidEnergizeCoresAction(botAI); }
        static Action* oc_hold_landing_spot(PlayerbotAI* botAI) { return new OcHoldLandingSpotAction(botAI); }
        static Action* avoid_planar_anomaly(PlayerbotAI* botAI) { return new AvoidPlanarAnomalyAction(botAI); }
};

#endif
