/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SLACTIONCONTEXT_H
#define PLAYERBOTS_SLACTIONCONTEXT_H

#include "NamedObjectContext.h"
#include "SLActions.h"

class TbcDungeonShadowLabyrinthActionContext : public NamedObjectContext<Action>
{
public:
    TbcDungeonShadowLabyrinthActionContext()
    {
        creators["murmur leave sonic boom"] = &TbcDungeonShadowLabyrinthActionContext::murmur_leave_sonic_boom;
        creators["murmur avoid thundering storm"] =
            &TbcDungeonShadowLabyrinthActionContext::murmur_avoid_thundering_storm;
        creators["murmurs touch spread"] = &TbcDungeonShadowLabyrinthActionContext::murmurs_touch_spread;
        creators["blackheart pets"] = &TbcDungeonShadowLabyrinthActionContext::blackheart_pets;
        creators["blackheart spread"] = &TbcDungeonShadowLabyrinthActionContext::blackheart_spread;
    }

private:
    static Action* murmur_leave_sonic_boom(PlayerbotAI* botAI) { return new MurmurLeaveSonicBoomAction(botAI); }
    static Action* murmur_avoid_thundering_storm(PlayerbotAI* botAI)
    {
        return new MurmurAvoidThunderingStormAction(botAI);
    }
    static Action* murmurs_touch_spread(PlayerbotAI* botAI) { return new MurmursTouchSpreadAction(botAI); }
    static Action* blackheart_pets(PlayerbotAI* botAI) { return new BlackheartPetsAction(botAI); }
    static Action* blackheart_spread(PlayerbotAI* botAI) { return new BlackheartSpreadAction(botAI); }
};

#endif
