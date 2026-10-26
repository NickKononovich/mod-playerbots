/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SLTRIGGERCONTEXT_H
#define PLAYERBOTS_SLTRIGGERCONTEXT_H

#include "NamedObjectContext.h"
#include "SLTriggers.h"

class TbcDungeonShadowLabyrinthTriggerContext : public NamedObjectContext<Trigger>
{
public:
    TbcDungeonShadowLabyrinthTriggerContext()
    {
        creators["murmur sonic boom"] = &TbcDungeonShadowLabyrinthTriggerContext::murmur_sonic_boom;
        creators["murmur thundering storm range"] =
            &TbcDungeonShadowLabyrinthTriggerContext::murmur_thundering_storm_range;
        creators["murmurs touch"] = &TbcDungeonShadowLabyrinthTriggerContext::murmurs_touch;
        creators["hellmaw fear"] = &TbcDungeonShadowLabyrinthTriggerContext::hellmaw_fear;
        creators["blackheart pets"] = &TbcDungeonShadowLabyrinthTriggerContext::blackheart_pets;
        creators["blackheart spread"] = &TbcDungeonShadowLabyrinthTriggerContext::blackheart_spread;
    }

private:
    static Trigger* murmur_sonic_boom(PlayerbotAI* botAI) { return new MurmurSonicBoomTrigger(botAI); }
    static Trigger* murmur_thundering_storm_range(PlayerbotAI* botAI)
    {
        return new MurmurThunderingStormRangeTrigger(botAI);
    }
    static Trigger* murmurs_touch(PlayerbotAI* botAI) { return new MurmursTouchTrigger(botAI); }
    static Trigger* hellmaw_fear(PlayerbotAI* botAI) { return new HellmawFearTrigger(botAI); }
    static Trigger* blackheart_pets(PlayerbotAI* botAI) { return new BlackheartPetsTrigger(botAI); }
    static Trigger* blackheart_spread(PlayerbotAI* botAI) { return new BlackheartSpreadTrigger(botAI); }
};

#endif
