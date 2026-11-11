/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PETSACTION_H
#define PLAYERBOTS_PETSACTION_H

#include "Action.h"
#include <string>

class PlayerbotAI;

class PetsAction : public Action
{
public:
    PetsAction(PlayerbotAI* botAI, std::string const& defaultCmd = "") : Action(botAI, "pet"), defaultCmd(defaultCmd) {}

    bool Execute(Event event) override;

private:
    std::string defaultCmd;
};

class TogglePetSpellAutoCastAction : public Action
{
public:
    TogglePetSpellAutoCastAction(PlayerbotAI* botAI) : Action(botAI, "toggle pet spell") {}
    virtual bool Execute(Event event) override;
};

class PetAttackAction : public Action
{
public:
    PetAttackAction(PlayerbotAI* botAI) : Action(botAI, "pet attack") {}
    virtual bool Execute(Event event) override;
};

class SetPetStanceAction : public Action
{
public:
    SetPetStanceAction(PlayerbotAI* botAI) : Action(botAI, "set pet stance") {}

    bool Execute(Event event) override;
};

// Pets and guardians of a bot in a dungeon group (a raid too, outside a boss encounter) play as an expert plays them.
// A pet with a pet bar (hunter and warlock pets, the Water Elemental, a talented ghoul, Feral Spirit wolves, treants,
// the Shadowfiend) is kept passive and sent only at the mob the owner attacks once the group's tank has engaged it
// (on its threat list or attacked by it) and while it fights by the tank or in melee with a member; out of combat it
// follows at the owner's heel, inside the owner's aggro margin. A defensive pet runs at whatever hits its owner
// (PetAI::OwnerAttackedBy) and after a kill at whatever hits either of them: wave 24 Scholomance, the hunter's Crab
// fought a caster nuking from its spawn, whose assistance calls (10 yd every 3 s) brought the next packs ("[far
// engage] ... engaged Crab"). A guardian without a pet bar (Army of the Dead, Snake Trap, Mirror Image, Gargoyle,
// Dancing Rune Weapon) is stopped on a mob no player of the group fights. Called every AI tick after the tick's
// actions from PlayerbotAI::UpdateAIInternal, and by "set pet stance"; a command goes out only when the pet does
// something else. Outside such a group the stance set here is restored.
class Player;
enum ReactStates : uint8;  // Unit.h

namespace DungeonPetControl
{
void Update(PlayerbotAI* botAI);
// [dps236] the control holds this bot's pets now (a dungeon group, outside a raid boss encounter)
bool Controls(Player* bot);
// [dps236] the stance the config gives a bot's pet (Playerbots.DefaultPetStance)
ReactStates ConfigStance();
}

#endif
