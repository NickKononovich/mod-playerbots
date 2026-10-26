/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SLTRIGGERS_H
#define PLAYERBOTS_SLTRIGGERS_H

#include "Trigger.h"

// Murmur is casting Sonic Boom and the bot is inside its reach.
class MurmurSonicBoomTrigger : public Trigger
{
public:
    MurmurSonicBoomTrigger(PlayerbotAI* botAI) : Trigger(botAI, "murmur sonic boom") {}
    bool IsActive() override;
};

// Heroic: a ranged bot or healer stands where Thundering Storm reaches (beyond 25 yd of Murmur).
class MurmurThunderingStormRangeTrigger : public Trigger
{
public:
    MurmurThunderingStormRangeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "murmur thundering storm range") {}
    bool IsActive() override;
};

// The bot carries Murmur's Touch, about to Shockwave everyone near it.
class MurmursTouchTrigger : public Trigger
{
public:
    MurmursTouchTrigger(PlayerbotAI* botAI) : Trigger(botAI, "murmurs touch") {}
    bool IsActive() override;
};

// Ambassador Hellmaw fears the whole group (33547, 45 yd); a shaman keeps a Tremor Totem down.
class HellmawFearTrigger : public Trigger
{
public:
    HellmawFearTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hellmaw fear") {}
    bool IsActive() override;
};

// Blackheart the Inciter: Incite Chaos 33684 charms every player for 15 s (first 24 s after the pull, then every
// 50-70 s, no cast bar) and the pets join in on the group (wave 10: a raptor killed the mage). Pets stay passive at
// their owner's side for the whole fight and get their stance back after it.
class BlackheartPetsTrigger : public Trigger
{
public:
    BlackheartPetsTrigger(PlayerbotAI* botAI) : Trigger(botAI, "blackheart pets", 500) {}
    bool IsActive() override;

private:
    uint32 _releaseUntilMs = 0;
};

// Ranged and healers keep apart during the fight (healers farther, BlackheartSpread): a charmed member attacks whoever
// stands nearest.
class BlackheartSpreadTrigger : public Trigger
{
public:
    BlackheartSpreadTrigger(PlayerbotAI* botAI) : Trigger(botAI, "blackheart spread", 500) {}
    bool IsActive() override;
};

#endif
