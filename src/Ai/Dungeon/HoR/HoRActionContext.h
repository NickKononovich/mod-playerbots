/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HORACTIONCONTEXT_H
#define PLAYERBOTS_HORACTIONCONTEXT_H

#include "Action.h"
#include "HoRActions.h"
#include "NamedObjectContext.h"

class WotlkDungeonHoRActionContext : public NamedObjectContext<Action>
{
public:
    WotlkDungeonHoRActionContext()
    {
        creators["hor start intro"] = &WotlkDungeonHoRActionContext::start_intro;
        creators["hor move to wave position"] = &WotlkDungeonHoRActionContext::move_to_wave_position;
        creators["hor leave wave circle"] = &WotlkDungeonHoRActionContext::leave_wave_circle;
        creators["hor attack wave priority"] = &WotlkDungeonHoRActionContext::attack_wave_priority;
        creators["hor enter throne room"] = &WotlkDungeonHoRActionContext::enter_throne_room;
        creators["hor move to escape leader"] = &WotlkDungeonHoRActionContext::move_to_escape_leader;
        creators["hor start escape"] = &WotlkDungeonHoRActionContext::start_escape;
        creators["hor escape move to leader"] = &WotlkDungeonHoRActionContext::escape_move_to_leader;
        creators["hor attack escape priority"] = &WotlkDungeonHoRActionContext::attack_escape_priority;
        creators["hor stop attacking lich king"] = &WotlkDungeonHoRActionContext::stop_attacking_lich_king;
        creators["hor escape tank position"] = &WotlkDungeonHoRActionContext::escape_tank_position;
        creators["hor escape taunt loose mob"] = &WotlkDungeonHoRActionContext::escape_taunt_loose_mob;
        creators["hor escape healer hold"] = &WotlkDungeonHoRActionContext::escape_healer_hold;
        creators["hor falric dispel"] = &WotlkDungeonHoRActionContext::falric_dispel;
        creators["hor shadow aura"] = &WotlkDungeonHoRActionContext::shadow_aura;
        creators["hor general leash"] = &WotlkDungeonHoRActionContext::general_leash;
        creators["hor attack reflection"] = &WotlkDungeonHoRActionContext::attack_reflection;
    }

private:
    static Action* start_intro(PlayerbotAI* botAI) { return new HoRStartIntroAction(botAI); }
    static Action* move_to_wave_position(PlayerbotAI* botAI) { return new HoRMoveToWavePositionAction(botAI); }
    static Action* leave_wave_circle(PlayerbotAI* botAI) { return new HoRLeaveWaveCircleAction(botAI); }
    static Action* attack_wave_priority(PlayerbotAI* botAI) { return new HoRAttackWavePriorityAction(botAI); }
    static Action* enter_throne_room(PlayerbotAI* botAI) { return new HoREnterThroneRoomAction(botAI); }
    static Action* move_to_escape_leader(PlayerbotAI* botAI) { return new HoRMoveToEscapeLeaderAction(botAI); }
    static Action* start_escape(PlayerbotAI* botAI) { return new HoRStartEscapeAction(botAI); }
    static Action* escape_move_to_leader(PlayerbotAI* botAI) { return new HoREscapeMoveToLeaderAction(botAI); }
    static Action* attack_escape_priority(PlayerbotAI* botAI) { return new HoRAttackEscapePriorityAction(botAI); }
    static Action* stop_attacking_lich_king(PlayerbotAI* botAI) { return new HoRStopAttackingLichKingAction(botAI); }
    static Action* escape_tank_position(PlayerbotAI* botAI) { return new HoREscapeTankPositionAction(botAI); }
    static Action* escape_taunt_loose_mob(PlayerbotAI* botAI) { return new HoREscapeTauntLooseMobAction(botAI); }
    static Action* escape_healer_hold(PlayerbotAI* botAI) { return new HoREscapeHealerHoldAction(botAI); }
    static Action* falric_dispel(PlayerbotAI* botAI) { return new HoRFalricDispelAction(botAI); }
    static Action* shadow_aura(PlayerbotAI* botAI) { return new HoRShadowAuraAction(botAI); }
    static Action* general_leash(PlayerbotAI* botAI) { return new HoRGeneralLeashAction(botAI); }
    static Action* attack_reflection(PlayerbotAI* botAI) { return new HoRAttackReflectionAction(botAI); }
};

#endif
