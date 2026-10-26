/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CLASSICTRIGGERCONTEXT_H
#define PLAYERBOTS_CLASSICTRIGGERCONTEXT_H

#include "ClassicTriggers.h"
#include "NamedObjectContext.h"

class ClassicDungeonTriggerContext : public NamedObjectContext<Trigger>
{
public:
    ClassicDungeonTriggerContext()
    {
        creators["doan detonation"] = &ClassicDungeonTriggerContext::doan_detonation;
        creators["doan arcane explosion"] = &ClassicDungeonTriggerContext::doan_arcane_explosion;
        creators["dungeon event step"] = &ClassicDungeonTriggerContext::dungeon_event_step;
        creators["scholo mana burn"] = &ClassicDungeonTriggerContext::scholo_mana_burn;
        creators["mograine too close"] = &ClassicDungeonTriggerContext::mograine_too_close;
        creators["scholo hall rally"] = &ClassicDungeonTriggerContext::scholo_hall_rally;
        creators["brs drake pen"] = &ClassicDungeonTriggerContext::brs_drake_pen;  // [b31_ubrs_door]
        creators["scholo viewing room boss"] = &ClassicDungeonTriggerContext::scholo_viewing_room_boss;  // [b32_brs]
        creators["st nightmare suppressor"] = &ClassicDungeonTriggerContext::st_nightmare_suppressor;  // [b32_st]
    }

private:
    static Trigger* doan_detonation(PlayerbotAI* botAI) { return new DoanDetonationTrigger(botAI); }
    static Trigger* doan_arcane_explosion(PlayerbotAI* botAI) { return new DoanArcaneExplosionTrigger(botAI); }
    static Trigger* dungeon_event_step(PlayerbotAI* botAI) { return new DungeonEventStepTrigger(botAI); }
    static Trigger* scholo_mana_burn(PlayerbotAI* botAI) { return new ScholoManaBurnTrigger(botAI); }
    static Trigger* mograine_too_close(PlayerbotAI* botAI) { return new MograineTooCloseTrigger(botAI); }
    static Trigger* scholo_hall_rally(PlayerbotAI* botAI) { return new ScholoHallRallyTrigger(botAI); }
    static Trigger* brs_drake_pen(PlayerbotAI* botAI) { return new BrsDrakePenTrigger(botAI); }
    static Trigger* scholo_viewing_room_boss(PlayerbotAI* botAI) { return new ScholoViewingRoomBossTrigger(botAI); }
    static Trigger* st_nightmare_suppressor(PlayerbotAI* botAI) { return new StSuppressorTrigger(botAI); }
};

#endif
