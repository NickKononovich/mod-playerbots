/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RTIACTION_H
#define PLAYERBOTS_RTIACTION_H

#include "Action.h"
#include "MovementActions.h"

class PlayerbotAI;
class Unit;
struct CrowdControlOption;

namespace PrePullCc
{
    struct Claim;
    struct Plan;
}

class RtiAction : public Action
{
public:
    RtiAction(PlayerbotAI* botAI) : Action(botAI, "rti") {}

    bool Execute(Event event) override;

private:
    void AppendRti(std::ostringstream& out, std::string const type);
};

class MarkRtiAction : public Action
{
public:
    MarkRtiAction(PlayerbotAI* botAI) : Action(botAI, "mark rti") {}

    bool Execute(Event event) override;
};

// Puts this bot's crowd control icon ("rti cc") on a pack mob its CC spells can hold, at the pull. Only a mob from
// the tail of the kill order (KillOrder::SortKillOrder past Skull and Cross), never one the group fights now.
class AutoCcMarkAction : public Action
{
public:
    AutoCcMarkAction(PlayerbotAI* botAI) : Action(botAI, "auto cc mark") {}

    bool isUseful() override;
    bool Execute(Event event) override;
};

// Casts this bot's crowd control on the mob it marked ("auto cc mark"), and again when it breaks: the first spell of
// its class (GetCrowdControlRole) that holds that mob and is ready, walking into range when no idle mob sees the way.
// Refuses, and gives the mark up, once the mob is a kill target of the group (IsGroupKillTarget).
class CastAutoCcAction : public MovementAction
{
public:
    CastAutoCcAction(PlayerbotAI* botAI) : MovementAction(botAI, "cast auto cc") {}

    bool isPossible() override;
    bool Execute(Event event) override;

private:
    bool CastTrap(Unit* target, CrowdControlOption const& option);
    bool CastByPet(Unit* target, CrowdControlOption const& option);
};

// [b44_st] Holds a charmed member of the group (AttackersValue::FindCharmedAlly) without damage until the charm
// fades, as players do with a mind-controlled friend: Polymorph, Hex, Repentance / Hammer of Justice, Cyclone, Blind,
// Wyvern Sting, Fear (only where the run pulls nothing). One bot per member at a time; a bot of a class without such
// a spell waits (nobody attacks it: AttackersValue::IsCharmedAlly).
class ControlCharmedMemberAction : public Action
{
public:
    ControlCharmedMemberAction(PlayerbotAI* botAI) : Action(botAI, "control charmed member") {}

    bool isUseful() override;
    bool Execute(Event event) override;
};

// Answers the tank's pre-pull crowd control plan (PrePullCc): claims a mob from the tail of the pack's kill order and
// marks it, then saps it or lays a Freezing Trap before the pull. A Polymorph, a Hex and the like wait for the pull
// ("cast auto cc"): cast before it, they would bring the pack onto the caster.
class PrePullCcAction : public MovementAction
{
public:
    PrePullCcAction(PlayerbotAI* botAI) : MovementAction(botAI, "pre pull cc") {}

    bool Execute(Event event) override;

private:
    bool ClaimTarget(PrePullCc::Plan const& plan);
    bool Place(PrePullCc::Plan const& plan, PrePullCc::Claim const& claim);
};

// A mob loose on a healer that nobody fights: the rogue blinds it, a priest it beats on screams it away (only with
// no crowd-controlled mob or idle pack near). The mage's Frost Nova for the same case is its own action.
class EmergencyCcAction : public MovementAction
{
public:
    EmergencyCcAction(PlayerbotAI* botAI) : MovementAction(botAI, "emergency cc") {}

    bool Execute(Event event) override;
};

// The main tank keeps Skull on the best kill target of the pack it holds (casters first) and Cross on the next.
class TankKillMarksAction : public Action
{
public:
    TankKillMarksAction(PlayerbotAI* botAI) : Action(botAI, "tank kill marks") {}

    bool Execute(Event event) override;
};

// Takes this bot's crowd control icon back off the unit it marked.
class ClearAutoCcMarkAction : public Action
{
public:
    ClearAutoCcMarkAction(PlayerbotAI* botAI) : Action(botAI, "clear auto cc mark") {}

    bool Execute(Event event) override;
};

#endif
