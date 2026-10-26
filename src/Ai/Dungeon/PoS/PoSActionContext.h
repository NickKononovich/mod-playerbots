/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_POSACTIONCONTEXT_H
#define PLAYERBOTS_POSACTIONCONTEXT_H

#include "Action.h"
#include "NamedObjectContext.h"
#include "PoSActions.h"

class WotlkDungeonPoSActionContext : public NamedObjectContext<Action>
{
    public:
        WotlkDungeonPoSActionContext()
        {
            creators["ick and krick"] = &WotlkDungeonPoSActionContext::ick_and_krick;
            creators["tyrannus"] = &WotlkDungeonPoSActionContext::tyrannus;
            creators["pos send area trigger"] = &WotlkDungeonPoSActionContext::pos_send_area_trigger;
            creators["pos hide from permafrost"] = &WotlkDungeonPoSActionContext::pos_hide_from_permafrost;
            creators["pos dodge saronite"] = &WotlkDungeonPoSActionContext::pos_dodge_saronite;
            creators["pos dodge icicle"] = &WotlkDungeonPoSActionContext::pos_dodge_icicle;
            creators["pos brand hold fire"] = &WotlkDungeonPoSActionContext::pos_brand_hold_fire;
            creators["pos avoid hoarfrost"] = &WotlkDungeonPoSActionContext::pos_avoid_hoarfrost;
            creators["pos tyrannus position"] = &WotlkDungeonPoSActionContext::pos_tyrannus_position;  // [b33_pos]
        }
    private:
        static Action* ick_and_krick(PlayerbotAI* botAI) { return new IckAndKrickAction(botAI); }
        static Action* tyrannus(PlayerbotAI* botAI) { return new TyrannusAction(botAI); }
        static Action* pos_send_area_trigger(PlayerbotAI* botAI) { return new PoSSendAreaTriggerAction(botAI); }
        static Action* pos_hide_from_permafrost(PlayerbotAI* botAI) { return new PoSHideFromPermafrostAction(botAI); }
        static Action* pos_dodge_saronite(PlayerbotAI* botAI) { return new PoSDodgeSaroniteAction(botAI); }
        static Action* pos_dodge_icicle(PlayerbotAI* botAI) { return new PoSDodgeIcicleAction(botAI); }
        static Action* pos_brand_hold_fire(PlayerbotAI* botAI) { return new PoSBrandHoldFireAction(botAI); }
        static Action* pos_avoid_hoarfrost(PlayerbotAI* botAI) { return new PoSAvoidHoarfrostAction(botAI); }
        static Action* pos_tyrannus_position(PlayerbotAI* botAI) { return new PoSTyrannusPositionAction(botAI); }
};

#endif
