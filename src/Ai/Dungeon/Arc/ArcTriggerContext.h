/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ARCTRIGGERCONTEXT_H
#define PLAYERBOTS_ARCTRIGGERCONTEXT_H

#include "ArcTriggers.h"
#include "NamedObjectContext.h"

class TbcDungeonArcatrazTriggerContext : public NamedObjectContext<Trigger>
{
public:
    TbcDungeonArcatrazTriggerContext()
    {
        creators["dalliah whirlwind"] = &TbcDungeonArcatrazTriggerContext::dalliah_whirlwind;
        creators["arcatraz warden room"] = &TbcDungeonArcatrazTriggerContext::arcatraz_warden_room;
        creators["arcatraz prisoner"] = &TbcDungeonArcatrazTriggerContext::arcatraz_prisoner;
        creators["abyssal meteor"] = &TbcDungeonArcatrazTriggerContext::abyssal_meteor;
    }

private:
    static Trigger* dalliah_whirlwind(PlayerbotAI* botAI) { return new DalliahWhirlwindTrigger(botAI); }
    static Trigger* arcatraz_warden_room(PlayerbotAI* botAI) { return new ArcWardenRoomTrigger(botAI); }
    static Trigger* arcatraz_prisoner(PlayerbotAI* botAI) { return new ArcPrisonerTrigger(botAI); }
    static Trigger* abyssal_meteor(PlayerbotAI* botAI) { return new AbyssalMeteorTrigger(botAI); }
};

#endif
