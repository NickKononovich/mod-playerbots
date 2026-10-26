/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BMTRIGGERCONTEXT_H
#define PLAYERBOTS_BMTRIGGERCONTEXT_H

#include "BMTriggers.h"
#include "NamedObjectContext.h"

class TbcDungeonBlackMorassTriggerContext : public NamedObjectContext<Trigger>
{
public:
    TbcDungeonBlackMorassTriggerContext()
    {
        creators["bm medivh threatened"] = &TbcDungeonBlackMorassTriggerContext::bm_medivh_threatened;
        creators["bm hold near medivh"] = &TbcDungeonBlackMorassTriggerContext::bm_hold_near_medivh;
        creators["bm kill rift guardian"] = &TbcDungeonBlackMorassTriggerContext::bm_kill_rift_guardian;
        creators["bm fetch rift guardian"] = &TbcDungeonBlackMorassTriggerContext::bm_fetch_rift_guardian;
        creators["bm drag rift guardian"] = &TbcDungeonBlackMorassTriggerContext::bm_drag_rift_guardian;
        creators["bm chrono beacon from saat"] = &TbcDungeonBlackMorassTriggerContext::bm_chrono_beacon_from_saat;
        creators["bm use chrono beacon"] = &TbcDungeonBlackMorassTriggerContext::bm_use_chrono_beacon;
        creators["bm boss buff"] = &TbcDungeonBlackMorassTriggerContext::bm_boss_buff;
        creators["bm fight spot at medivh"] = &TbcDungeonBlackMorassTriggerContext::bm_fight_spot_at_medivh;
    }

private:
    static Trigger* bm_medivh_threatened(PlayerbotAI* botAI) { return new BlackMorassMedivhThreatenedTrigger(botAI); }
    static Trigger* bm_hold_near_medivh(PlayerbotAI* botAI) { return new BlackMorassHoldNearMedivhTrigger(botAI); }
    static Trigger* bm_kill_rift_guardian(PlayerbotAI* botAI)
    {
        return new BlackMorassKillRiftGuardianTrigger(botAI);
    }
    static Trigger* bm_fetch_rift_guardian(PlayerbotAI* botAI)
    {
        return new BlackMorassFetchRiftGuardianTrigger(botAI);
    }
    static Trigger* bm_drag_rift_guardian(PlayerbotAI* botAI)
    {
        return new BlackMorassDragRiftGuardianTrigger(botAI);
    }
    static Trigger* bm_chrono_beacon_from_saat(PlayerbotAI* botAI)
    {
        return new BlackMorassChronoBeaconFromSaatTrigger(botAI);
    }
    static Trigger* bm_use_chrono_beacon(PlayerbotAI* botAI) { return new BlackMorassUseChronoBeaconTrigger(botAI); }
    static Trigger* bm_boss_buff(PlayerbotAI* botAI) { return new BlackMorassBossBuffTrigger(botAI); }
    static Trigger* bm_fight_spot_at_medivh(PlayerbotAI* botAI)
    {
        return new BlackMorassFightSpotAtMedivhTrigger(botAI);
    }
};

#endif
