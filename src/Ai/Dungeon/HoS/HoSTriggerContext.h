/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HOSTRIGGERCONTEXT_H
#define PLAYERBOTS_HOSTRIGGERCONTEXT_H

#include "HoSTriggers.h"
#include "NamedObjectContext.h"

class WotlkDungeonHoSTriggerContext : public NamedObjectContext<Trigger>
{
    public:
        WotlkDungeonHoSTriggerContext()
        {
            creators["ground slam"] = &WotlkDungeonHoSTriggerContext::ground_slam;
            creators["krystallus ranged spread"] = &WotlkDungeonHoSTriggerContext::krystallus_ranged_spread;
            creators["krystallus pet spread"] = &WotlkDungeonHoSTriggerContext::krystallus_pet_spread;
            creators["krystallus dismiss pet"] = &WotlkDungeonHoSTriggerContext::krystallus_dismiss_pet;
            creators["krystallus dismiss summon"] = &WotlkDungeonHoSTriggerContext::krystallus_dismiss_summon;
            creators["lightning ring"] = &WotlkDungeonHoSTriggerContext::lightning_ring;
            creators["hos brann gossip"] = &WotlkDungeonHoSTriggerContext::hos_brann_gossip;
            creators["hos stay with brann"] = &WotlkDungeonHoSTriggerContext::hos_stay_with_brann;
            creators["hos brann attacked"] = &WotlkDungeonHoSTriggerContext::hos_brann_attacked;
            creators["hos tribunal hazard"] = &WotlkDungeonHoSTriggerContext::hos_tribunal_hazard;
            creators["hos tribunal loose add"] = &WotlkDungeonHoSTriggerContext::hos_tribunal_loose_add;
            creators["hos tribunal hold point"] = &WotlkDungeonHoSTriggerContext::hos_tribunal_hold_point;
            creators["hos tribunal healer hold"] = &WotlkDungeonHoSTriggerContext::hos_tribunal_healer_hold;
            creators["hos tribunal protector"] = &WotlkDungeonHoSTriggerContext::hos_tribunal_protector;
        }
    private:
        static Trigger* ground_slam(PlayerbotAI* botAI) { return new KrystallusGroundSlamTrigger(botAI); }
        static Trigger* krystallus_ranged_spread(PlayerbotAI* botAI)
        {
            return new KrystallusRangedSpreadTrigger(botAI);
        }
        static Trigger* krystallus_pet_spread(PlayerbotAI* botAI) { return new KrystallusPetSpreadTrigger(botAI); }
        static Trigger* krystallus_dismiss_pet(PlayerbotAI* botAI) { return new KrystallusDismissPetTrigger(botAI); }
        static Trigger* krystallus_dismiss_summon(PlayerbotAI* botAI)
        {
            return new KrystallusDismissSummonTrigger(botAI);
        }
        static Trigger* lightning_ring(PlayerbotAI* botAI) { return new SjonnirLightningRingTrigger(botAI); }
        static Trigger* hos_brann_gossip(PlayerbotAI* botAI) { return new HoSBrannGossipTrigger(botAI); }
        static Trigger* hos_stay_with_brann(PlayerbotAI* botAI) { return new HoSStayWithBrannTrigger(botAI); }
        static Trigger* hos_brann_attacked(PlayerbotAI* botAI) { return new HoSBrannAttackedTrigger(botAI); }
        static Trigger* hos_tribunal_hazard(PlayerbotAI* botAI) { return new HoSTribunalHazardTrigger(botAI); }
        static Trigger* hos_tribunal_loose_add(PlayerbotAI* botAI) { return new HoSTribunalLooseAddTrigger(botAI); }
        static Trigger* hos_tribunal_hold_point(PlayerbotAI* botAI) { return new HoSTribunalHoldPointTrigger(botAI); }
        static Trigger* hos_tribunal_healer_hold(PlayerbotAI* botAI)
        {
            return new HoSTribunalHealerHoldTrigger(botAI);
        }
        static Trigger* hos_tribunal_protector(PlayerbotAI* botAI) { return new HoSTribunalProtectorTrigger(botAI); }
};

#endif
