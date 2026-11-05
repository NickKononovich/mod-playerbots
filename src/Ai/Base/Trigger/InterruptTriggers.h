/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_INTERRUPTTRIGGERS_H
#define PLAYERBOTS_INTERRUPTTRIGGERS_H

#include "Trigger.h"

class PlayerbotAI;

// The group interrupt rotation gave this bot an enemy cast to stop.
class AssignedInterruptTrigger : public Trigger
{
public:
    AssignedInterruptTrigger(PlayerbotAI* botAI) : Trigger(botAI, "assigned interrupt") {}

    bool IsActive() override;
};

// [rot2] The dungeon leader assigns this bot an external save / buff now (LeaderCommand::AssignedExternal), the spell
// known and off cooldown.
class LeaderExternalTrigger : public Trigger
{
public:
    LeaderExternalTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader external") {}

    bool IsActive() override;
};

// [lead1371] The leader's External is its self-defence order (Frost Nova on the member's own attackers, Ice Block):
// the same "leader external" executes it, above the leader's move order (the cast before the run).
class LeaderSelfDefenseTrigger : public Trigger
{
public:
    LeaderSelfDefenseTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader self defense") {}

    bool IsActive() override;
};

// [rot2] The tank: the leader names a mob to take back (LeaderCommand::AssignedThreat).
class LeaderThreatTrigger : public Trigger
{
public:
    LeaderThreatTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader threat") {}

    bool IsActive() override;
};

// [rot2] The leader assigns this bot a mob to crowd-control (LeaderCommand::AssignedCc) that is not held yet.
class LeaderCcTrigger : public Trigger
{
public:
    LeaderCcTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader cc") {}

    bool IsActive() override;
};

// [rot3] The leader's PreSave for this bot (LeaderCommandState::PreSaveAssigned).
class LeaderPreSaveTrigger : public Trigger
{
public:
    LeaderPreSaveTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader pre save") {}

    bool IsActive() override;
};

// [lead400] The leader's Dispel for this bot (LeaderCommand::AssignedDispel), the spell known.
class LeaderDispelTrigger : public Trigger
{
public:
    LeaderDispelTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader dispel") {}

    bool IsActive() override;
};

// [lead402] The leader's Consume of a bag item for this bot (LeaderCommand::AssignedConsume).
class LeaderConsumeTrigger : public Trigger
{
public:
    LeaderConsumeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader consume") {}

    bool IsActive() override;
};

// [lead405] The leader's Consume of a game object for this bot.
class LeaderUseObjectTrigger : public Trigger
{
public:
    LeaderUseObjectTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader use object") {}

    bool IsActive() override;
};

#endif
