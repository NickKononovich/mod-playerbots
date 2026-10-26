/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SVACTIONS_H
#define PLAYERBOTS_SVACTIONS_H

#include "Action.h"
#include "AttackAction.h"

// Warlord Kalithresh: damage dealers kill the distiller that is channelling Warlord's Rage into him.
class KalithreshKillDistillerAction : public AttackAction
{
public:
    KalithreshKillDistillerAction(PlayerbotAI* botAI) : AttackAction(botAI, "kalithresh kill distiller") {}
    bool Execute(Event event) override;
    bool isUseful() override;

private:
    Unit* FindActiveDistiller();
};

// The tank takes a loose Bog Overlord back: its taunt (Righteous Defense on the victim for a paladin), then attacks it.
class SvTauntBogOverlordAction : public AttackAction
{
public:
    SvTauntBogOverlordAction(PlayerbotAI* botAI) : AttackAction(botAI, "sv taunt bog overlord") {}
    bool Execute(Event event) override;
};

// Warlord Kalithresh: drop a cast aimed at him while Spell Reflection is up.
class KalithreshStopCastingAction : public Action
{
public:
    KalithreshStopCastingAction(PlayerbotAI* botAI) : Action(botAI, "kalithresh stop casting") {}
    bool Execute(Event event) override;
};

#endif
