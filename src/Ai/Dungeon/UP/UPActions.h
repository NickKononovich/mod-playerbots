/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_UPACTIONS_H
#define PLAYERBOTS_UPACTIONS_H

#include "AttackAction.h"
#include "MovementActions.h"
#include "Position.h"
#include "TbcHazard.h"

class SpellInfo;  // [b36_ymiron]

// The gauntlet staging spot next to the Harpoon Launchers, and the radius that counts as being there.
Position const& SkadiHarpoonStaging();
float SkadiStagingReachedYd();
// [b35_up2] A harpoon carrier may walk to its launcher: Skadi on a flying Grauf, no live gauntlet mob on its path or
// by the launcher (the designated harpooner may fire past adds the tank holds there), and not past the tank into adds.
bool SkadiHarpoonRunAllowed(PlayerbotAI* botAI);

class AvoidFreezingCloudAction : public MovementAction
{
public:
    AvoidFreezingCloudAction(PlayerbotAI* botAI) : MovementAction(botAI, "avoid freezing cloud") {}
    bool Execute(Event event) override;
};

class AvoidSkadiWhirlwindAction : public MovementAction
{
public:
    AvoidSkadiWhirlwindAction(PlayerbotAI* botAI) : MovementAction(botAI, "avoid skadi whirlwind") {}
    bool Execute(Event event) override;
};

// Walks into an area trigger box and sends CMSG_AREATRIGGER from inside once the group stands with the bot.
class UPAreaTriggerStartAction : public MovementAction
{
public:
    UPAreaTriggerStartAction(PlayerbotAI* botAI, std::string const name, uint32 triggerId)
        : MovementAction(botAI, name), _triggerId(triggerId)
    {
    }
    bool Execute(Event event) override;

private:
    uint32 _triggerId;
};

class SvalaStartEventAction : public UPAreaTriggerStartAction
{
public:
    SvalaStartEventAction(PlayerbotAI* botAI);
    bool isUseful() override;
};

class SkadiStartEventAction : public UPAreaTriggerStartAction
{
public:
    SkadiStartEventAction(PlayerbotAI* botAI);
    bool isUseful() override;
};

class GortokStartEventAction : public MovementAction
{
public:
    GortokStartEventAction(PlayerbotAI* botAI) : MovementAction(botAI, "gortok start event") {}
    bool Execute(Event event) override;
};

class SvalaAttackChannelerAction : public AttackAction
{
public:
    SvalaAttackChannelerAction(PlayerbotAI* botAI) : AttackAction(botAI, "svala attack channeler") {}
    bool Execute(Event event) override;
};

// Grauf's breath pass: cross to the other half of the lane (same x), the way players dodge the Freezing Cloud.
class SkadiCrossLaneAction : public TbcHazardMoveAction
{
public:
    SkadiCrossLaneAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "skadi cross lane") {}
    bool Execute(Event event) override;
};

// The damage dealers kill the Harpooners first: each drops the harpoon the launchers need.
class SkadiAttackHarpoonerAction : public AttackAction
{
public:
    SkadiAttackHarpoonerAction(PlayerbotAI* botAI) : AttackAction(botAI, "skadi attack harpooner") {}
    bool Execute(Event event) override;
};

class SkadiMoveToHarpoonsAction : public MovementAction
{
public:
    SkadiMoveToHarpoonsAction(PlayerbotAI* botAI) : MovementAction(botAI, "skadi move to harpoons") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

class SkadiPickUpHarpoonAction : public MovementAction
{
public:
    SkadiPickUpHarpoonAction(PlayerbotAI* botAI) : MovementAction(botAI, "skadi pick up harpoon") {}
    bool Execute(Event event) override;
};

class SkadiFireHarpoonAction : public MovementAction
{
public:
    SkadiFireHarpoonAction(PlayerbotAI* botAI) : MovementAction(botAI, "skadi fire harpoon") {}
    bool Execute(Event event) override;
};

class YmironPurgeBaneAction : public Action
{
public:
    YmironPurgeBaneAction(PlayerbotAI* botAI) : Action(botAI, "ymiron purge bane") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

// [b36_ymiron] A spell that may still go out while Bane is cast or up: helpful spells, interrupts, taunts (the tank
// keeps the boss without dealing damage), purges, and single-target damage on something other than Ymiron.
// Damage on Ymiron and area damage that would reach him wait.
bool YmironBaneAllowsSpell(SpellInfo const* info, Unit* target, Unit* boss);

// [b36_ymiron] From the Bane cast until the aura fades: stop melee swings, auto shot / wand, an own damaging cast
// at Ymiron and the pet's attack on him. Keeps the target so an interrupt can still land on the cast. Useful only
// while something is still hitting him, so healers and stopped bots go on with their tick.
class YmironHoldDamageAction : public Action
{
public:
    YmironHoldDamageAction(PlayerbotAI* botAI) : Action(botAI, "ymiron hold damage") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
