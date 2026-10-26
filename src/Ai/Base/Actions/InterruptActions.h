/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_INTERRUPTACTIONS_H
#define PLAYERBOTS_INTERRUPTACTIONS_H

#include "Action.h"

class PlayerbotAI;
class SpellInfo;
class Unit;

// Casts the interrupt the group rotation assigned to this bot on the enemy whose cast it was assigned.
class InterruptAssignedCastAction : public Action
{
public:
    InterruptAssignedCastAction(PlayerbotAI* botAI) : Action(botAI, "interrupt assigned cast") {}

    bool isUseful() override;
    bool isPossible() override;
    bool Execute(Event event) override;

private:
    Unit* GetCaster();
    SpellInfo const* GetAssignedSpell();
};

// [rot2] The dungeon leader's assigned casts (LeaderCommand.h). Each does exactly what the command names and
// reports it to the leader (Leader::Events::Ack); with no fresh command for the bot each is useless.

// The external save / buff the leader assigns (spell id, on whom): Innervate, Hymn of Hope, Divine Hymn, Pain
// Suppression, Power Infusion, the Hand spells, Misdirection, Tricks, Bloodlust / Heroism.
class LeaderExternalAction : public Action
{
public:
    LeaderExternalAction(PlayerbotAI* botAI) : Action(botAI, "leader external") {}

    bool isUseful() override;
    bool isPossible() override;
    bool Execute(Event event) override;
};

// The tank takes back the mob the leader names: its taunt or AoE threat spell, else just attacks it.
class LeaderThreatAction : public Action
{
public:
    LeaderThreatAction(PlayerbotAI* botAI) : Action(botAI, "leader threat") {}

    bool isUseful() override;
    bool Execute(Event event) override;
};

// The crowd control the leader assigns: the bot's own hold spell for that mob (FindCrowdControl).
class LeaderCcAction : public Action
{
public:
    LeaderCcAction(PlayerbotAI* botAI) : Action(botAI, "leader cc") {}

    bool isUseful() override;
    bool isPossible() override;
    bool Execute(Event event) override;
};

// [rot3] The leader's PreSave (a big hit foreseen on this bot): the bot's best personal save it has ready.
class LeaderPreSaveAction : public Action
{
public:
    LeaderPreSaveAction(PlayerbotAI* botAI) : Action(botAI, "leader pre save") {}

    bool isUseful() override;
    bool Execute(Event event) override;

private:
    char const* PickSave();
};

#endif
