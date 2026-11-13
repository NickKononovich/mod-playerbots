/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_REVIVEFROMCORPSEACTION_H
#define PLAYERBOTS_REVIVEFROMCORPSEACTION_H

#include "MovementActions.h"
#include "ObjectGuid.h"

#include <G3D/Vector3.h>

#include <vector>

class PlayerbotAI;
class Corpse;

struct GraveyardStruct;

class ReviveFromCorpseAction : public MovementAction
{
public:
    ReviveFromCorpseAction(PlayerbotAI* botAI) : MovementAction(botAI, "revive from corpse") {}

    bool Execute(Event event) override;
};

class FindCorpseAction : public MovementAction
{
public:
    FindCorpseAction(PlayerbotAI* botAI) : MovementAction(botAI, "find corpse") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    // window: a spot only the mobs that stay keep out of reach of (the wanderers / patrols leave it)
    bool SafeSpot(Corpse* corpse, GuidVector const& hostiles, float radius, G3D::Vector3& spot, bool window = false);

    // [death458] what the last walk this action started was for: its end is judged as a rise spot's
    enum class Walk : uint8
    {
        None,
        CorpseFar,  // to the corpse from beyond ReactDistance: decided again once inside it
        Corpse,
        Spot
    };
    void DropSpot(std::string const& why);

    uint32 _spotMs = 0;
    bool _spotWindow = false;
    bool _spotFound = false;
    G3D::Vector3 _spot;
    // [death458] per corpse: its reach searched once, the rise spots that did not hold, the walk on
    ObjectGuid _corpseGuid;
    bool _reachSearched = false;
    std::vector<G3D::Vector3> _droppedSpots;
    Walk _walk = Walk::None;
    G3D::Vector3 _walkTo;
};

class SpiritHealerAction : public MovementAction
{
public:
    SpiritHealerAction(PlayerbotAI* botAI, std::string const name = "spirit healer") : MovementAction(botAI, name) {}

    GraveyardStruct const* GetGrave(bool startZone);
    bool Execute(Event event) override;
    bool isUseful() override;
};

// [grave44] a death: when the bot died near the graveyard its spirit healer last raised it (at its level), that
// graveyard is unsafe for it until its level changes - the next ghost takes another healer or its corpse
void NoteGhostDeath(Player* bot);

#endif
