/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DTKMULTIPLIERS_H
#define PLAYERBOTS_DTKMULTIPLIERS_H

#include "Multiplier.h"

class NovosMultiplier : public Multiplier
{
    public:
        NovosMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "novos the summoner") {}

    public:
        float GetValue(Action* action) override;
};

class TharonjaMultiplier : public Multiplier
{
    public:
        TharonjaMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "the prophet tharon'ja") {}

    public:
        float GetValue(Action* action) override;
};

// Around Bellowing Roar the shaman's other earth totems would replace Tremor Totem.
class KingDredMultiplier : public Multiplier
{
public:
    KingDredMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "king dred") {}
    float GetValue(Action* action) override;
};

// Before Trollgore is down, out of combat, only the tank opens: no other bot attacks or casts on a hostile mob that
// is not yet in combat with the group (a mob the group already fights is not held).
class DTKTankOpensMultiplier : public Multiplier
{
public:
    DTKTankOpensMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "dtk tank opens") {}
    float GetValue(Action* action) override;
};

// Nobody opens on an idle Trollgore while an idle trash mob stands within 30 yd of him: his engage pulls the room
// (SetInCombatWithZone) and the trash joins his fight. Released after a while so a stuck pack cannot hold him forever.
class TrollgoreBodyPullMultiplier : public Multiplier
{
public:
    TrollgoreBodyPullMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "trollgore body pull") {}
    float GetValue(Action* action) override;

private:
    uint32 _holdSinceMs = 0;
};

#endif
