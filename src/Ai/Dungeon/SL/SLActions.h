/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SLACTIONS_H
#define PLAYERBOTS_SLACTIONS_H

#include "ObjectGuid.h"
#include "TbcHazard.h"
#include "UnitDefines.h"
#include <map>

// Murmur: everyone (the tank too; Murmur never moves) runs out of Sonic Boom reach during its 5 s cast.
class MurmurLeaveSonicBoomAction : public TbcHazardMoveAction
{
public:
    MurmurLeaveSonicBoomAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "murmur leave sonic boom") {}
    bool Execute(Event event) override;
};

// Heroic Murmur: ranged and healers come back inside 25 yd of him so Thundering Storm finds nobody.
class MurmurAvoidThunderingStormAction : public TbcHazardMoveAction
{
public:
    MurmurAvoidThunderingStormAction(PlayerbotAI* botAI)
        : TbcHazardMoveAction(botAI, "murmur avoid thundering storm") {}
    bool Execute(Event event) override;
};

// Murmur's Touch: walk away from the group before the Shockwave goes off.
class MurmursTouchSpreadAction : public TbcHazardMoveAction
{
public:
    MurmursTouchSpreadAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "murmurs touch spread") {}
    bool Execute(Event event) override;
};

// Parks the bot's pets (passive, following) while Blackheart fights, restores their stance afterwards.
class BlackheartPetsAction : public Action
{
public:
    BlackheartPetsAction(PlayerbotAI* botAI) : Action(botAI, "blackheart pets") {}
    bool Execute(Event event) override;

private:
    std::map<ObjectGuid, ReactStates> _parked;  // pet -> the stance it had before
};

class BlackheartSpreadAction : public TbcHazardMoveAction
{
public:
    BlackheartSpreadAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "blackheart spread") {}
    bool Execute(Event event) override;
};

#endif
