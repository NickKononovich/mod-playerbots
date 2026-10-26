/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CLASSICACTIONCONTEXT_H
#define PLAYERBOTS_CLASSICACTIONCONTEXT_H

#include "ClassicActions.h"
#include "NamedObjectContext.h"

class ClassicDungeonActionContext : public NamedObjectContext<Action>
{
public:
    ClassicDungeonActionContext()
    {
        creators["doan escape detonation"] = &ClassicDungeonActionContext::doan_escape_detonation;
        creators["doan keep range"] = &ClassicDungeonActionContext::doan_keep_range;
        creators["dungeon event step"] = &ClassicDungeonActionContext::dungeon_event_step;
        creators["scholo avoid mana burn"] = &ClassicDungeonActionContext::scholo_avoid_mana_burn;
        creators["mograine keep away"] = &ClassicDungeonActionContext::mograine_keep_away;
        creators["scholo hall rally"] = &ClassicDungeonActionContext::scholo_hall_rally;
        creators["brs leave drake pen"] = &ClassicDungeonActionContext::brs_leave_drake_pen;  // [b31_ubrs_door]
        creators["scholo attack viewing room boss"] =
            &ClassicDungeonActionContext::scholo_attack_viewing_room_boss;  // [b32_brs]
        creators["st attack suppressor"] = &ClassicDungeonActionContext::st_attack_suppressor;  // [b32_st]
    }

private:
    static Action* doan_escape_detonation(PlayerbotAI* botAI) { return new DoanEscapeDetonationAction(botAI); }
    static Action* doan_keep_range(PlayerbotAI* botAI) { return new DoanKeepRangeAction(botAI); }
    static Action* dungeon_event_step(PlayerbotAI* botAI) { return new DungeonEventStepAction(botAI); }
    static Action* scholo_avoid_mana_burn(PlayerbotAI* botAI) { return new ScholoAvoidManaBurnAction(botAI); }
    static Action* mograine_keep_away(PlayerbotAI* botAI) { return new MograineKeepAwayAction(botAI); }
    static Action* scholo_hall_rally(PlayerbotAI* botAI) { return new ScholoHallRallyAction(botAI); }
    static Action* brs_leave_drake_pen(PlayerbotAI* botAI) { return new BrsLeaveDrakePenAction(botAI); }
    static Action* scholo_attack_viewing_room_boss(PlayerbotAI* botAI)
    {
        return new ScholoAttackViewingRoomBossAction(botAI);
    }
    static Action* st_attack_suppressor(PlayerbotAI* botAI) { return new StAttackSuppressorAction(botAI); }
};

#endif
