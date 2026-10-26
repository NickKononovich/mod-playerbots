/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FEARCOUNTERTRIGGERS_H
#define PLAYERBOTS_FEARCOUNTERTRIGGERS_H

#include "Trigger.h"

class PlayerbotAI;

// [fear_counter] Every trigger reads the cached "fear counter state" (FearCounterValues.h): O(1) per tick.

// In combat, a party member within Tremor reach of the bot is feared / charmed / asleep and the bot's own Tremor
// Totem does not cover him.
class PartyMemberFearedTrigger : public Trigger
{
public:
    PartyMemberFearedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "party member feared") {}
    bool IsActive() override;
};

// In combat, the fight has a fear / charm / sleep source and the bot's Tremor Totem does not cover the tank.
class FearThreatTrigger : public Trigger
{
public:
    FearThreatTrigger(PlayerbotAI* botAI) : Trigger(botAI, "fear threat") {}
    bool IsActive() override;
};

// The fight (or the tank's pull target) has a real fear source and the tank has no Fear Ward.
class FearWardNeededTrigger : public Trigger
{
public:
    FearWardNeededTrigger(PlayerbotAI* botAI) : Trigger(botAI, "fear ward needed") {}
    bool IsActive() override;
};

// A fear-type cast is in progress within reach of the bot.
class FearIncomingTrigger : public Trigger
{
public:
    FearIncomingTrigger(PlayerbotAI* botAI) : Trigger(botAI, "fear incoming") {}
    bool IsActive() override;
};

#endif
