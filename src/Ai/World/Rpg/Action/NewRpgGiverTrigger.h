/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGGIVERTRIGGER_H
#define PLAYERBOTS_NEWRPGGIVERTRIGGER_H

#include <string>
#include <vector>

#include "Define.h"

class Player;

// [giver15] quests whose every giver is a creature that gets the quest-giver flag only from its SmartAI (user
// 2026-11-08: done honestly, the way a player does, never skipped). The trigger is read once from the game data: the
// script's events that set or add UNIT_NPC_FLAG_QUESTGIVER (directly, linked, or in the timed action list they call),
// traced back to the event that starts them, with the event phase that event needs and what sets that phase:
//  - a spell hit (the bot's own spell, or the on-use spell of an item: then the item is used on it or next to it),
//  - a gossip option (or hello),
//  - a data set from another script: a creature's death near the giver (a kill), an object an item's spell summons,
//  - its health falling to a percent after the script turned it hostile (the fight it starts),
//  - its own idle clock (the flag comes back by itself).
// b354 q9612 A Hearty Thanks!: Gift of the Naaru on the Draenei Youngling (phase 1), then a Timberstrider Fledgling
// dying within 10 yd of it (its SmartAI sets data 1 1 on the closest youngling) gives the flag until its next 60 s
// out-of-combat tick.
namespace NewRpgGiverTrigger
{
    enum class StepKind : uint8
    {
        Spell,       // cast one of `spells` (ranks) on the giver
        Item,        // use `itemId` (its spell `spells[0]`) on the giver, or next to it when the spell takes no unit
        Gossip,      // gossip hello, then option `option` of menu `menu` unless `hello`
        Kill,        // kill a `killEntry` creature within `radius` yd of the giver (0: anywhere)
        Fight,       // the giver turned hostile: fight it down to `healthPct` %
        Wait,        // its script gives the flag back by itself
        Unsupported  // no step a bot can do: `why`
    };

    struct Step
    {
        StepKind kind = StepKind::Unsupported;
        std::vector<uint32> spells;
        uint32 itemId = 0;
        uint32 menu = 0;
        uint32 option = 0;
        bool hello = false;
        uint32 killEntry = 0;
        float radius = 0.0f;
        bool killSpawned = false;  // Kill: the creature has spawns of its own (a dead one comes back at its respawn)
        uint32 healthPct = 0;
        uint32 flagDelayMs = 0;  // the last step: the script's own delay from it to the flag (its action list)
        std::string why;         // Unsupported
        std::string source;      // the data that names it, for the log
    };

    // one way to get a giver's flag: its steps in order (a phase step first)
    struct Trigger
    {
        uint32 giver = 0;
        std::vector<Step> steps;
        bool retry = false;  // a kill decides it (where it dies, the phase still on): a missed flag is tried again
    };

    // builds the triggers once at startup (in-memory game data only: quests, creature and object SmartAI, items,
    // spells) and logs the [giver15] summary line
    void Build();
    // every giver of the quest gets the flag from its script
    bool IsScripted(uint32 questId);
    // its triggers (one or more per giver creature), null when it is no such quest
    std::vector<Trigger> const* For(uint32 questId);
    // the bot can do every step of this trigger (it knows the spell, holds the item, the data names a way); why not
    bool CanDo(Player* bot, Trigger const& trigger, std::string* why);
    // no scripted giver, or a trigger of it the bot can do; why not
    bool CanTrigger(Player* bot, uint32 questId, std::string* why);
    // CanTrigger, the refusal logged once per bot and quest ("[giver15] <bot> cannot trigger q<id>: <why>")
    bool Plannable(Player* bot, uint32 questId);
    // the seconds of the cheapest trigger it can do: casts and their cooldowns left, kills and the fight by the
    // creature's health at `dps`, the script's delay to the flag (0: no scripted giver)
    float Seconds(Player* bot, uint32 questId, float dps);
    // "casts Gift of the Naaru on it", "kills Timberstrider Fledgling within 10 yd of it"
    std::string Describe(Step const& step);
    std::string Describe(Trigger const& trigger);
}

#endif
