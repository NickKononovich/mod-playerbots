/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SPTRIGGERS_H
#define PLAYERBOTS_SPTRIGGERS_H

#include "Trigger.h"

// Mennu the Betrayer

// The bot stands inside the Fire Nova radius of a live Corrupted Nova Totem.
class MennuNovaTotemTrigger : public Trigger
{
public:
    MennuNovaTotemTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mennu nova totem") {}
    bool IsActive() override;
};

// A damage dealer in the Mennu fight: totems and the healing ward go down first.
class MennuTotemsUpTrigger : public Trigger
{
public:
    MennuTotemsUpTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mennu totems up") {}
    bool IsActive() override;
};

#endif
