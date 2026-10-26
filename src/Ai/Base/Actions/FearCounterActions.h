/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FEARCOUNTERACTIONS_H
#define PLAYERBOTS_FEARCOUNTERACTIONS_H

#include "Action.h"

class Item;
class Player;
class PlayerbotAI;

// [fear_counter] Counters to a fear / charm / sleep, all fed by the cached "fear counter state" value.

// Shaman: Tremor Totem at the shaman's feet, reactively (a party member is feared now) or before the fear when the
// fight has a known source. It replaces the earth totem in the slot; FearCounterTotemMultiplier keeps the other
// earth totems from taking the slot back until the fight is over.
class FearCounterTremorTotemAction : public Action
{
public:
    FearCounterTremorTotemAction(PlayerbotAI* botAI) : Action(botAI, "fear counter tremor totem") {}

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;

private:
    uint32 _lastDropMs = 0;
};

// Priest: Fear Ward on the tank before and during a fight with a fear source (recast once it is used and ready).
class FearCounterFearWardAction : public Action
{
public:
    FearCounterFearWardAction(PlayerbotAI* botAI) : Action(botAI, "fear counter fear ward") {}

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;
    Unit* GetTarget() override;
};

// Warrior: Berserker Rage while a fear is being cast nearby (10 s fear immunity, so the fear never lands).
class FearCounterBerserkerRageAction : public Action
{
public:
    FearCounterBerserkerRageAction(PlayerbotAI* botAI) : Action(botAI, "fear counter berserker rage") {}

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;
};

// Anyone: the equipped PvP trinket (42292 / 65547) breaks the bot's own fear / charm / sleep.
class FearCounterPvpTrinketAction : public Action
{
public:
    FearCounterPvpTrinketAction(PlayerbotAI* botAI) : Action(botAI, "fear counter pvp trinket") {}

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;

    // The equipped trinket whose on-use spell is a PvP trinket's, or nullptr; spellId receives that spell.
    static Item* FindPvpTrinket(Player* bot, uint32& spellId);
};

#endif
