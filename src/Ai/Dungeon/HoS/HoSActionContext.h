/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HOSACTIONCONTEXT_H
#define PLAYERBOTS_HOSACTIONCONTEXT_H

#include "Action.h"
#include "HoSActions.h"
#include "NamedObjectContext.h"

class WotlkDungeonHoSActionContext : public NamedObjectContext<Action>
{
    public:
        WotlkDungeonHoSActionContext() {
            creators["shatter spread"] = &WotlkDungeonHoSActionContext::shatter_spread;
            creators["krystallus ranged spread"] = &WotlkDungeonHoSActionContext::krystallus_ranged_spread;
            creators["krystallus pet spread"] = &WotlkDungeonHoSActionContext::krystallus_pet_spread;
            creators["krystallus dismiss pet"] = &WotlkDungeonHoSActionContext::krystallus_dismiss_pet;
            creators["krystallus dismiss summon"] = &WotlkDungeonHoSActionContext::krystallus_dismiss_summon;
            creators["avoid lightning ring"] = &WotlkDungeonHoSActionContext::avoid_lightning_ring;
            creators["hos brann gossip"] = &WotlkDungeonHoSActionContext::hos_brann_gossip;
            creators["hos stay with brann"] = &WotlkDungeonHoSActionContext::hos_stay_with_brann;
            creators["hos protect brann"] = &WotlkDungeonHoSActionContext::hos_protect_brann;
            creators["hos avoid tribunal hazard"] = &WotlkDungeonHoSActionContext::hos_avoid_tribunal_hazard;
            creators["hos tank tribunal add"] = &WotlkDungeonHoSActionContext::hos_tank_tribunal_add;
            creators["hos tribunal hold point"] = &WotlkDungeonHoSActionContext::hos_tribunal_hold_point;
            creators["hos tribunal healer hold"] = &WotlkDungeonHoSActionContext::hos_tribunal_healer_hold;
            creators["hos tribunal protector"] = &WotlkDungeonHoSActionContext::hos_tribunal_protector;
        }
    private:
        static Action* shatter_spread(PlayerbotAI* botAI) { return new ShatterSpreadAction(botAI); }
        static Action* krystallus_ranged_spread(PlayerbotAI* botAI) { return new KrystallusRangedSpreadAction(botAI); }
        static Action* krystallus_pet_spread(PlayerbotAI* botAI) { return new KrystallusPetSpreadAction(botAI); }
        static Action* krystallus_dismiss_pet(PlayerbotAI* botAI) { return new KrystallusDismissPetAction(botAI); }
        static Action* krystallus_dismiss_summon(PlayerbotAI* botAI)
        {
            return new KrystallusDismissSummonAction(botAI);
        }
        static Action* avoid_lightning_ring(PlayerbotAI* botAI) { return new AvoidLightningRingAction(botAI); }
        static Action* hos_brann_gossip(PlayerbotAI* botAI) { return new HoSBrannGossipAction(botAI); }
        static Action* hos_stay_with_brann(PlayerbotAI* botAI) { return new HoSStayWithBrannAction(botAI); }
        static Action* hos_protect_brann(PlayerbotAI* botAI) { return new HoSProtectBrannAction(botAI); }
        static Action* hos_avoid_tribunal_hazard(PlayerbotAI* botAI) { return new HoSAvoidTribunalHazardAction(botAI); }
        static Action* hos_tank_tribunal_add(PlayerbotAI* botAI) { return new HoSTankTribunalAddAction(botAI); }
        static Action* hos_tribunal_hold_point(PlayerbotAI* botAI) { return new HoSTribunalHoldPointAction(botAI); }
        static Action* hos_tribunal_healer_hold(PlayerbotAI* botAI) { return new HoSTribunalHealerHoldAction(botAI); }
        static Action* hos_tribunal_protector(PlayerbotAI* botAI) { return new HoSTribunalProtectorAction(botAI); }
};

#endif
