/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SVTRIGGERS_H
#define PLAYERBOTS_SVTRIGGERS_H

#include "Trigger.h"

class PlayerbotAI;
class Unit;

namespace Steamvault
{
// A Bog Overlord in the bot's fight hitting a member who is not a tank, or nullptr.
Unit* LooseBogOverlord(PlayerbotAI* botAI);
}

// The tank: a Bog Overlord is loose on a non-tank (SVShared.h).
class SvBogOverlordLooseTrigger : public Trigger
{
public:
    SvBogOverlordLooseTrigger(PlayerbotAI* botAI) : Trigger(botAI, "sv bog overlord loose") {}
    bool IsActive() override;
};

// Warlord Kalithresh: a damage dealer in the fight (a channelling distiller may be up).
class KalithreshDistillerTrigger : public Trigger
{
public:
    KalithreshDistillerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "kalithresh distiller") {}
    bool IsActive() override;
};

// Warlord Kalithresh: Spell Reflection is up and the bot is casting at him.
class KalithreshSpellReflectionTrigger : public Trigger
{
public:
    KalithreshSpellReflectionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "kalithresh spell reflection") {}
    bool IsActive() override;
};

#endif
