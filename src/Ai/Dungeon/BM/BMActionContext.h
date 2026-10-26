/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BMACTIONCONTEXT_H
#define PLAYERBOTS_BMACTIONCONTEXT_H

#include "Action.h"
#include "BMActions.h"
#include "NamedObjectContext.h"

class TbcDungeonBlackMorassActionContext : public NamedObjectContext<Action>
{
public:
    TbcDungeonBlackMorassActionContext()
    {
        creators["bm defend medivh"] = &TbcDungeonBlackMorassActionContext::bm_defend_medivh;
        creators["bm hold near medivh"] = &TbcDungeonBlackMorassActionContext::bm_hold_near_medivh;
        creators["bm kill rift guardian"] = &TbcDungeonBlackMorassActionContext::bm_kill_rift_guardian;
        creators["bm fetch rift guardian"] = &TbcDungeonBlackMorassActionContext::bm_fetch_rift_guardian;
        creators["bm drag rift guardian"] = &TbcDungeonBlackMorassActionContext::bm_drag_rift_guardian;
        creators["bm chrono beacon from saat"] = &TbcDungeonBlackMorassActionContext::bm_chrono_beacon_from_saat;
        creators["bm use chrono beacon"] = &TbcDungeonBlackMorassActionContext::bm_use_chrono_beacon;
        creators["bm remove boss buff"] = &TbcDungeonBlackMorassActionContext::bm_remove_boss_buff;
        creators["bm fight spot at medivh"] = &TbcDungeonBlackMorassActionContext::bm_fight_spot_at_medivh;
    }

private:
    static Action* bm_defend_medivh(PlayerbotAI* botAI) { return new BlackMorassDefendMedivhAction(botAI); }
    static Action* bm_hold_near_medivh(PlayerbotAI* botAI) { return new BlackMorassHoldNearMedivhAction(botAI); }
    static Action* bm_kill_rift_guardian(PlayerbotAI* botAI)
    {
        return new BlackMorassKillRiftGuardianAction(botAI);
    }
    static Action* bm_fetch_rift_guardian(PlayerbotAI* botAI)
    {
        return new BlackMorassFetchRiftGuardianAction(botAI);
    }
    static Action* bm_drag_rift_guardian(PlayerbotAI* botAI) { return new BlackMorassDragRiftGuardianAction(botAI); }
    static Action* bm_chrono_beacon_from_saat(PlayerbotAI* botAI)
    {
        return new BlackMorassChronoBeaconFromSaatAction(botAI);
    }
    static Action* bm_use_chrono_beacon(PlayerbotAI* botAI) { return new BlackMorassUseChronoBeaconAction(botAI); }
    static Action* bm_remove_boss_buff(PlayerbotAI* botAI) { return new BlackMorassRemoveBossBuffAction(botAI); }
    static Action* bm_fight_spot_at_medivh(PlayerbotAI* botAI)
    {
        return new BlackMorassFightSpotAtMedivhAction(botAI);
    }
};

#endif
