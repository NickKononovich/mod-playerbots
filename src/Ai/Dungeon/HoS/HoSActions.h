/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HOSACTIONS_H
#define PLAYERBOTS_HOSACTIONS_H

#include "Action.h"
#include "AttackAction.h"
#include "HoSTriggers.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include <map>
#include <vector>

// Steps to the nearby point that takes the least Shatter damage (linear falloff, summed over every source).
class KrystallusSpreadMovementAction : public MovementAction
{
public:
    KrystallusSpreadMovementAction(PlayerbotAI* botAI, std::string const name) : MovementAction(botAI, name) {}

protected:
    bool StepAwayFromShatter(Unit* boss, std::vector<Position> const& sources, float radius, float maxBossDistance,
                             bool keepBossLos);
};

class ShatterSpreadAction : public KrystallusSpreadMovementAction
{
public:
    ShatterSpreadAction(PlayerbotAI* botAI) : KrystallusSpreadMovementAction(botAI, "shatter spread") {}
    bool Execute(Event event) override;
};

// The owner parks its pets and guardians on the spot farthest from the group before they are Stoned (pets would
// otherwise Shatter on their owner and the melee around the boss), then releases them once Shatter has passed.
// Not a MovementAction, so the Ground Slam multiplier does not drop it.
class KrystallusPetSpreadAction : public Action
{
public:
    KrystallusPetSpreadAction(PlayerbotAI* botAI) : Action(botAI, "krystallus pet spread") {}
    bool Execute(Event event) override;

private:
    bool ParkPet(Creature* pet);
    void ReleasePet(Creature* pet, ReactStates previous);
    // A parked pet that is still close to a member (its spot did not hold, or something re-engaged it).
    bool NeedsRepark(Creature* pet) const;

    std::map<ObjectGuid, ReactStates> _parked;  // pet -> the stance it had before parking
    std::map<ObjectGuid, uint32> _reparkAt;     // pet -> earliest getMSTime() of the next re-park
};

// Casts Dismiss Pet (5 s); the pet stays away until Krystallus is dead (KrystallusMultiplier holds "call pet").
class KrystallusDismissPetAction : public Action
{
public:
    KrystallusDismissPetAction(PlayerbotAI* botAI) : Action(botAI, "krystallus dismiss pet") {}
    bool Execute(Event event) override;
};

// Dismisses the bot's short-lived summons the way the pet frame's Dismiss does (CMSG_PET_ACTION COMMAND_ABANDON:
// a summoned pet is put down, a guardian unsummoned); KrystallusMultiplier keeps them from being summoned again.
class KrystallusDismissSummonAction : public Action
{
public:
    KrystallusDismissSummonAction(PlayerbotAI* botAI) : Action(botAI, "krystallus dismiss summon") {}
    bool Execute(Event event) override;
};

// Ranged and healers walk to their spoke of the star (HoSShared::StarSpot); where that spot cannot be reached they
// fall back to the step away from the nearest Shatter sources.
class KrystallusRangedSpreadAction : public KrystallusSpreadMovementAction
{
public:
    KrystallusRangedSpreadAction(PlayerbotAI* botAI)
        : KrystallusSpreadMovementAction(botAI, "krystallus ranged spread")
    {
    }
    bool isUseful() override;
    bool Execute(Event event) override;

private:
    bool MoveToStarSpot(Unit* boss);

    uint32 _nextAttemptMs = 0;
};

class AvoidLightningRingAction : public MovementAction
{
public:
    AvoidLightningRingAction(PlayerbotAI* botAI) : MovementAction(botAI, "avoid lightning ring") {}
    bool Execute(Event event) override;
};

class HoSBrannGossipAction : public MovementAction
{
public:
    HoSBrannGossipAction(PlayerbotAI* botAI) : MovementAction(botAI, "hos brann gossip") {}
    bool Execute(Event event) override;
};

class HoSStayWithBrannAction : public MovementAction
{
public:
    HoSStayWithBrannAction(PlayerbotAI* botAI) : MovementAction(botAI, "hos stay with brann") {}
    bool Execute(Event event) override;
};

class HoSProtectBrannAction : public AttackAction
{
public:
    HoSProtectBrannAction(PlayerbotAI* botAI) : AttackAction(botAI, "hos protect brann") {}
    bool Execute(Event event) override;
};

class HoSAvoidTribunalHazardAction : public MovementAction
{
public:
    HoSAvoidTribunalHazardAction(PlayerbotAI* botAI) : MovementAction(botAI, "hos avoid tribunal hazard") {}
    bool Execute(Event event) override;
};

// The tank takes a Tribunal add off another member: as its target when the add is near its hold spot, by a ranged
// taunt from the spot otherwise (no chase across the room away from the healer).
class HoSTankTribunalAddAction : public AttackAction
{
public:
    HoSTankTribunalAddAction(PlayerbotAI* botAI) : AttackAction(botAI, "hos tank tribunal add") {}
    bool Execute(Event event) override;

private:
    bool TauntFromSpot(Unit* add);
};

// The tank walks back to its Tribunal hold spot next to Brann (HoSIds::TRIBUNAL_HOLD_*).
class HoSTribunalHoldPointAction : public MovementAction
{
public:
    HoSTribunalHoldPointAction(PlayerbotAI* botAI) : MovementAction(botAI, "hos tribunal hold point") {}
    bool Execute(Event event) override;
};

// The healer closes up on the tank during the Tribunal.
class HoSTribunalHealerHoldAction : public MovementAction
{
public:
    HoSTribunalHealerHoldAction(PlayerbotAI* botAI) : MovementAction(botAI, "hos tribunal healer hold") {}
    bool Execute(Event event) override;
};

// A damage dealer controls (stun, incapacitate) and kills the Dark Rune Protector that hits the healer.
class HoSTribunalProtectorAction : public AttackAction
{
public:
    HoSTribunalProtectorAction(PlayerbotAI* botAI) : AttackAction(botAI, "hos tribunal protector") {}
    bool Execute(Event event) override;
};

#endif
