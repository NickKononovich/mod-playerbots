/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ARCTRIGGERS_H
#define PLAYERBOTS_ARCTRIGGERS_H

#include "Trigger.h"

// Dalliah the Doomsayer is whirlwinding and the bot stands within reach of it.
class DalliahWhirlwindTrigger : public Trigger
{
public:
    DalliahWhirlwindTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dalliah whirlwind") {}
    bool IsActive() override;
};

// Prison event: a bot has strayed out of the warden's room (chasing, or the follow route pulling it on).
class ArcWardenRoomTrigger : public Trigger
{
public:
    ArcWardenRoomTrigger(PlayerbotAI* botAI) : Trigger(botAI, "arcatraz warden room") {}
    bool IsActive() override;
};

// Prison event: a released prisoner is up and the bot is on something else.
class ArcPrisonerTrigger : public Trigger
{
public:
    ArcPrisonerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "arcatraz prisoner") {}
    bool IsActive() override;
};

// A Gargantuan Abyssal is casting Meteor and the bot is not stacked on its landing point.
class AbyssalMeteorTrigger : public Trigger
{
public:
    AbyssalMeteorTrigger(PlayerbotAI* botAI) : Trigger(botAI, "abyssal meteor") {}
    bool IsActive() override;
};

#endif
