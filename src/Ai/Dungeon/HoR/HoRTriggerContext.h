/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HORTRIGGERCONTEXT_H
#define PLAYERBOTS_HORTRIGGERCONTEXT_H

#include "HoRTriggers.h"
#include "NamedObjectContext.h"

class WotlkDungeonHoRTriggerContext : public NamedObjectContext<Trigger>
{
public:
    WotlkDungeonHoRTriggerContext()
    {
        creators["hor start intro"] = &WotlkDungeonHoRTriggerContext::start_intro;
        creators["hor wave position"] = &WotlkDungeonHoRTriggerContext::wave_position;
        creators["hor wave lost"] = &WotlkDungeonHoRTriggerContext::wave_lost;
        creators["hor wave priority"] = &WotlkDungeonHoRTriggerContext::wave_priority;
        creators["hor lich king intro"] = &WotlkDungeonHoRTriggerContext::lich_king_intro;
        creators["hor escape gather"] = &WotlkDungeonHoRTriggerContext::escape_gather;
        creators["hor start escape"] = &WotlkDungeonHoRTriggerContext::start_escape;
        creators["hor escape position"] = &WotlkDungeonHoRTriggerContext::escape_position;
        creators["hor escape priority"] = &WotlkDungeonHoRTriggerContext::escape_priority;
        creators["hor lich king targeted"] = &WotlkDungeonHoRTriggerContext::lich_king_targeted;
        creators["hor escape tank position"] = &WotlkDungeonHoRTriggerContext::escape_tank_position;
        creators["hor escape loose mob"] = &WotlkDungeonHoRTriggerContext::escape_loose_mob;
        creators["hor escape healer hold"] = &WotlkDungeonHoRTriggerContext::escape_healer_hold;
        creators["hor falric dispel"] = &WotlkDungeonHoRTriggerContext::falric_dispel;
        creators["hor shadow aura"] = &WotlkDungeonHoRTriggerContext::shadow_aura;
        creators["hor general leash"] = &WotlkDungeonHoRTriggerContext::general_leash;
        creators["hor spiritual reflection"] = &WotlkDungeonHoRTriggerContext::spiritual_reflection;
    }

private:
    static Trigger* start_intro(PlayerbotAI* botAI) { return new HoRStartIntroTrigger(botAI); }
    static Trigger* wave_position(PlayerbotAI* botAI) { return new HoRWavePositionTrigger(botAI); }
    static Trigger* wave_lost(PlayerbotAI* botAI) { return new HoRWaveLostTrigger(botAI); }
    static Trigger* wave_priority(PlayerbotAI* botAI) { return new HoRWavePriorityTrigger(botAI); }
    static Trigger* lich_king_intro(PlayerbotAI* botAI) { return new HoRLichKingIntroTrigger(botAI); }
    static Trigger* escape_gather(PlayerbotAI* botAI) { return new HoREscapeGatherTrigger(botAI); }
    static Trigger* start_escape(PlayerbotAI* botAI) { return new HoRStartEscapeTrigger(botAI); }
    static Trigger* escape_position(PlayerbotAI* botAI) { return new HoREscapePositionTrigger(botAI); }
    static Trigger* escape_priority(PlayerbotAI* botAI) { return new HoREscapePriorityTrigger(botAI); }
    static Trigger* lich_king_targeted(PlayerbotAI* botAI) { return new HoRLichKingTargetedTrigger(botAI); }
    static Trigger* escape_tank_position(PlayerbotAI* botAI) { return new HoREscapeTankPositionTrigger(botAI); }
    static Trigger* escape_loose_mob(PlayerbotAI* botAI) { return new HoREscapeLooseMobTrigger(botAI); }
    static Trigger* escape_healer_hold(PlayerbotAI* botAI) { return new HoREscapeHealerHoldTrigger(botAI); }
    static Trigger* falric_dispel(PlayerbotAI* botAI) { return new HoRFalricDispelTrigger(botAI); }
    static Trigger* shadow_aura(PlayerbotAI* botAI) { return new HoRShadowAuraTrigger(botAI); }
    static Trigger* general_leash(PlayerbotAI* botAI) { return new HoRGeneralLeashTrigger(botAI); }
    static Trigger* spiritual_reflection(PlayerbotAI* botAI) { return new HoRReflectionTrigger(botAI); }
};

#endif
