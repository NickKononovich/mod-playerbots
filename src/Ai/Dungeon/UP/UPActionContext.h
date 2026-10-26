/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_UPACTIONCONTEXT_H
#define PLAYERBOTS_UPACTIONCONTEXT_H

#include "NamedObjectContext.h"
#include "UPActions.h"

class WotlkDungeonUPActionContext : public NamedObjectContext<Action>
{
    public:
        WotlkDungeonUPActionContext() {
            creators["avoid freezing cloud"] = &WotlkDungeonUPActionContext::avoid_freezing_cloud;
            creators["avoid skadi whirlwind"] = &WotlkDungeonUPActionContext::avoid_whirlwind;
            creators["svala start event"] = &WotlkDungeonUPActionContext::svala_start_event;
            creators["svala attack channeler"] = &WotlkDungeonUPActionContext::svala_attack_channeler;
            creators["skadi cross lane"] = &WotlkDungeonUPActionContext::skadi_cross_lane;
            creators["skadi attack harpooner"] = &WotlkDungeonUPActionContext::skadi_attack_harpooner;
            creators["gortok start event"] = &WotlkDungeonUPActionContext::gortok_start_event;
            creators["skadi start event"] = &WotlkDungeonUPActionContext::skadi_start_event;
            creators["skadi move to harpoons"] = &WotlkDungeonUPActionContext::skadi_move_to_harpoons;
            creators["skadi pick up harpoon"] = &WotlkDungeonUPActionContext::skadi_pick_up_harpoon;
            creators["skadi fire harpoon"] = &WotlkDungeonUPActionContext::skadi_fire_harpoon;
            creators["ymiron purge bane"] = &WotlkDungeonUPActionContext::ymiron_purge_bane;
            creators["ymiron hold damage"] = &WotlkDungeonUPActionContext::ymiron_hold_damage;  // [b36_ymiron]
        }
    private:
        static Action* avoid_freezing_cloud(PlayerbotAI* botAI) { return new AvoidFreezingCloudAction(botAI); }
        static Action* avoid_whirlwind(PlayerbotAI* botAI) { return new AvoidSkadiWhirlwindAction(botAI); }
        static Action* svala_start_event(PlayerbotAI* botAI) { return new SvalaStartEventAction(botAI); }
        static Action* svala_attack_channeler(PlayerbotAI* botAI) { return new SvalaAttackChannelerAction(botAI); }
        static Action* skadi_cross_lane(PlayerbotAI* botAI) { return new SkadiCrossLaneAction(botAI); }
        static Action* skadi_attack_harpooner(PlayerbotAI* botAI) { return new SkadiAttackHarpoonerAction(botAI); }
        static Action* gortok_start_event(PlayerbotAI* botAI) { return new GortokStartEventAction(botAI); }
        static Action* skadi_start_event(PlayerbotAI* botAI) { return new SkadiStartEventAction(botAI); }
        static Action* skadi_move_to_harpoons(PlayerbotAI* botAI) { return new SkadiMoveToHarpoonsAction(botAI); }
        static Action* skadi_pick_up_harpoon(PlayerbotAI* botAI) { return new SkadiPickUpHarpoonAction(botAI); }
        static Action* skadi_fire_harpoon(PlayerbotAI* botAI) { return new SkadiFireHarpoonAction(botAI); }
        static Action* ymiron_purge_bane(PlayerbotAI* botAI) { return new YmironPurgeBaneAction(botAI); }
        static Action* ymiron_hold_damage(PlayerbotAI* botAI) { return new YmironHoldDamageAction(botAI); }
};

#endif
