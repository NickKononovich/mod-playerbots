/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CLASSICTRIGGERS_H
#define PLAYERBOTS_CLASSICTRIGGERS_H

#include "Trigger.h"

// Arcanist Doan is bubbled / casting Detonation and the bot is still in its reach.
class DoanDetonationTrigger : public Trigger
{
public:
    DoanDetonationTrigger(PlayerbotAI* botAI) : Trigger(botAI, "doan detonation") {}
    bool IsActive() override;
};

// A ranged bot or a healer stands within Doan's Arcane Explosion.
class DoanArcaneExplosionTrigger : public Trigger
{
public:
    DoanArcaneExplosionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "doan arcane explosion") {}
    bool IsActive() override;
};

// Scholomance: a ranged bot or a healer stands inside an Acolyte's Mana Burn pick (ClassicShared.h).
class ScholoManaBurnTrigger : public Trigger
{
public:
    ScholoManaBurnTrigger(PlayerbotAI* botAI) : Trigger(botAI, "scholo mana burn") {}
    bool IsActive() override;
};

// Scarlet Monastery Cathedral: the bot stands within MOGRAINE_KEEP_AWAY of an idle Commander Mograine while his chapel
// still stands (his engage sends all of it, ClassicShared.h).
class MograineTooCloseTrigger : public Trigger
{
public:
    MograineTooCloseTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mograine too close") {}
    bool IsActive() override;
};

// Scholomance hall: shortly after a fight the bot stands on the hall floor within reach of an idle hall mob, and no
// pull of the group is under way (ClassicShared.h).
class ScholoHallRallyTrigger : public Trigger
{
public:
    ScholoHallRallyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "scholo hall rally") {}
    bool IsActive() override;

private:
    uint32 _lastCombatMs = 0;
};

// [b32_brs] Scholomance Viewing Room: the tank stands by Vectus / Marduk (neutral, npc flags) after Dawn's Gambit.
class ScholoViewingRoomBossTrigger : public Trigger
{
public:
    ScholoViewingRoomBossTrigger(PlayerbotAI* botAI) : Trigger(botAI, "scholo viewing room boss") {}
    bool IsActive() override;
};

// [b31_ubrs_door] UBRS stadium: the bot stands in the drake pen east of the wave portcullis 175185 while the gate is
// up (ClassicShared.h BlackrockSpire); it closes 10 s after each wave and stays shut after Gyth.
class BrsDrakePenTrigger : public Trigger
{
public:
    BrsDrakePenTrigger(PlayerbotAI* botAI) : Trigger(botAI, "brs drake pen") {}
    bool IsActive() override;
};

// The group's interactor has an event step to do here (ClassicShared.h DungeonEvents); a grid search, so slow.
class DungeonEventStepTrigger : public Trigger
{
public:
    DungeonEventStepTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon event step", 2000) {}
    bool IsActive() override;
};

// [b32_st] Sunken Temple: an idle Nightmare Suppressor near a damage dealer (ClassicShared.h SunkenTemple).
class StSuppressorTrigger : public Trigger
{
public:
    StSuppressorTrigger(PlayerbotAI* botAI) : Trigger(botAI, "st nightmare suppressor") {}
    bool IsActive() override;
};

#endif
