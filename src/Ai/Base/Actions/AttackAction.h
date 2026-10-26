/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ATTACKACTION_H
#define PLAYERBOTS_ATTACKACTION_H

#include "MovementActions.h"

class PlayerbotAI;

class AttackAction : public MovementAction
{
public:
    AttackAction(PlayerbotAI* botAI, std::string const name) : MovementAction(botAI, name) {}

    bool Execute(Event event) override;

protected:
    bool Attack(Unit* target, bool with_pet = true);

private:
    // A dungeon tank's own target choice never opens a new pull (an idle mob) while the fight goes on or the group is
    // not ready (TankPullRules); logs "[tank target] ... skips idle ...".
    bool RefusesNewPull(Unit* target);
};

class AttackMyTargetAction : public AttackAction
{
public:
    AttackMyTargetAction(PlayerbotAI* botAI, std::string const name = "attack my target") : AttackAction(botAI, name) {}

    bool Execute(Event event) override;
};

// Tank out of combat beside its target, a boss that has not engaged ("idle boss at tank"): it swings at him, and when
// it already swings or cannot reach him in melee it opens with a ranged pull. Attack() alone refused: the boss was
// the current target and the victim already ("I am already attacking"), so nothing ever hit him (occ-h Drakos 2 min
// at 0.1 yd, VH Moragg at 6 yd).
class OpenIdleBossAction : public AttackAction
{
public:
    OpenIdleBossAction(PlayerbotAI* botAI) : AttackAction(botAI, "tank open idle boss") {}

    bool Execute(Event event) override;

private:
    // A boss that does not engage after a few tries (an event boss still spawning, out of reach in water) is left
    // alone a while: each try stops the tank's walk (zf-h Gahz'rilla pool build 17).
    ObjectGuid _boss;
    uint32 _tries = 0;
    uint32 _leaveUntilMs = 0;
};

class AttackDuelOpponentAction : public AttackAction
{
public:
    AttackDuelOpponentAction(PlayerbotAI* botAI, std::string const name = "attack duel opponent")
        : AttackAction(botAI, name)
    {
    }

public:
    bool Execute(Event event) override;
    bool isUseful() override;
};

// Tank out of combat: takes the mob off the member it hits ("defend member target") with a taunt or a gap closer when
// one is ready, then attacks it. Pull holds and plans do not stop it: the fight is already on. No gap closer onto a mob
// in an idle pack's aggro reach, and "reach melee" does not walk there either (TankWalkIntoIdlePack).
class DefendAttackedGroupMemberAction : public AttackAction
{
public:
    DefendAttackedGroupMemberAction(PlayerbotAI* botAI) : AttackAction(botAI, "tank defend on attacked group member") {}

    std::string const GetTargetName() override { return "defend member target"; }
    bool isUseful() override;
    bool Execute(Event event) override;

private:
    bool CastOpener(Unit* mob, Player* victim);
};

// [fled_survivor] Tank out of combat: a mob that fled from an earlier fight stands idle and damaged within 40 yd
// ("fled survivor"): finished now as a one-mob pull, before the group walks on - it brings a pack later or meets the
// group on its way back. Pulled from where the tank stands (Death Grip, Avenger's Shield, Heroic Throw, a taunt, Faerie
// Fire); walked to only when no idle pack stands near it (else the harness plans it: Retarget "fled survivor").
class FinishFledSurvivorAction : public AttackAction
{
public:
    static constexpr float MIN_HEALTH_PCT = 60.0f;

    FinishFledSurvivorAction(PlayerbotAI* botAI) : AttackAction(botAI, "finish fled survivor") {}

    std::string const GetTargetName() override { return "fled survivor"; }
    bool isUseful() override;
    bool Execute(Event event) override;

private:
    ObjectGuid _logged;
};

class MeleeAction : public AttackAction
{
public:
    MeleeAction(PlayerbotAI* botAI) : AttackAction(botAI, "melee") {}

    std::string const GetTargetName() override { return "current target"; }
    bool isUseful() override;
};

#endif
