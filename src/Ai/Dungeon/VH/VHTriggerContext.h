/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_VHTRIGGERCONTEXT_H
#define PLAYERBOTS_VHTRIGGERCONTEXT_H

#include "NamedObjectContext.h"
#include "VHTriggers.h"

class WotlkDungeonVHTriggerContext : public NamedObjectContext<Trigger>
{
    public:
        WotlkDungeonVHTriggerContext()
        {
            creators["erekem target"] = &WotlkDungeonVHTriggerContext::erekem_target;
            creators["ichoron target"] = &WotlkDungeonVHTriggerContext::ichoron_target;
            creators["void shift"] = &WotlkDungeonVHTriggerContext::void_shift;
            creators["shroud of darkness"] = &WotlkDungeonVHTriggerContext::shroud_of_darkness;
            creators["cyanigosa positioning"] = &WotlkDungeonVHTriggerContext::cyanigosa_positioning;
            creators["vh sinclari start"] = &WotlkDungeonVHTriggerContext::vh_sinclari_start;
            creators["vh seal under attack"] = &WotlkDungeonVHTriggerContext::vh_seal_under_attack;
            creators["vh portal keeper"] = &WotlkDungeonVHTriggerContext::vh_portal_keeper;
            creators["vh released boss"] = &WotlkDungeonVHTriggerContext::vh_released_boss;
            creators["vh rally"] = &WotlkDungeonVHTriggerContext::vh_rally;
            creators["xevozz ethereal sphere"] = &WotlkDungeonVHTriggerContext::xevozz_ethereal_sphere;
            creators["vh late join"] = &WotlkDungeonVHTriggerContext::vh_late_join;
        }
    private:
        static Trigger* erekem_target(PlayerbotAI* botAI) { return new ErekemTargetTrigger(botAI); }
        static Trigger* ichoron_target(PlayerbotAI* botAI) { return new IchoronTargetTrigger(botAI); }
        static Trigger* void_shift(PlayerbotAI* botAI) { return new VoidShiftTrigger(botAI); }
        static Trigger* shroud_of_darkness(PlayerbotAI* botAI) { return new ShroudOfDarknessTrigger(botAI); }
        static Trigger* cyanigosa_positioning(PlayerbotAI* botAI) { return new CyanigosaPositioningTrigger(botAI); }
        static Trigger* vh_sinclari_start(PlayerbotAI* botAI) { return new VHSinclariStartTrigger(botAI); }
        static Trigger* vh_seal_under_attack(PlayerbotAI* botAI) { return new VHSealUnderAttackTrigger(botAI); }
        static Trigger* vh_portal_keeper(PlayerbotAI* botAI) { return new VHPortalKeeperTrigger(botAI); }
        static Trigger* vh_released_boss(PlayerbotAI* botAI) { return new VHReleasedBossTrigger(botAI); }
        static Trigger* vh_rally(PlayerbotAI* botAI) { return new VHRallyTrigger(botAI); }
        static Trigger* xevozz_ethereal_sphere(PlayerbotAI* botAI) { return new XevozzEtherealSphereTrigger(botAI); }
        static Trigger* vh_late_join(PlayerbotAI* botAI) { return new VHLateJoinTrigger(botAI); }
};

#endif
