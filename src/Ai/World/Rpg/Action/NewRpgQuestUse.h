/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGQUESTUSE_H
#define PLAYERBOTS_NEWRPGQUESTUSE_H

#include <string>
#include <vector>

#include "Define.h"

class Player;
class Quest;

// [questitems] quest objectives credited by using a quest item or a spell (a flute on rats, a disguise at a flag, a
// net at a school, a racial heal on a survivor): what to use and on what, read once from the game data - the tool's
// on-use spell, its spell focus, its target conditions, the SmartAI that credits its hit or the players under its aura
namespace NewRpgQuestUse
{
    enum class TargetKind : uint8
    {
        Creature,  // cast on a creature of `creatures`
        Focus,     // cast where a spell focus object of `objects` stands (the spell's RequiresSpellFocus)
        Near,      // cast on the bot next to a creature of `creatures` (its script credits players under the aura)
    };

    struct Plan
    {
        uint32 itemId = 0;            // the tool item (0: a spell of the bot's own)
        std::vector<uint32> spells;   // the item's on-use spell; for a spell plan its ranks (the first the bot knows)
        uint8 objective = 0;          // 0..3 creature objective, 4..9 item objective (QUEST_OBJECTIVES_COUNT + i)
        TargetKind kind = TargetKind::Creature;
        std::vector<uint32> creatures;
        std::vector<uint32> objects;
        float focusRadius = 0.0f;     // gameobject_template spellFocus.dist
        bool hitCredit = false;       // credited at the hit itself: no credit from a cast means the quest is not doable
        std::string source;           // the data that names it, for the log
    };

    // builds the plans once (in-memory game data only: quests, items, spells, conditions, SmartAI)
    void Build();
    // the plan crediting that objective of the quest; with a bot: only one it can carry out (it holds the tool, or the
    // quest gives it at accept; it knows the spell)
    Plan const* PlanFor(Player* bot, Quest const* quest, uint8 objective);
    // every objective of the quest that a tool use would credit has a plan the bot can carry out
    bool CoversToolObjectives(Player* bot, Quest const* quest);
    // the creature / object entries whose spawns are the objective's spots (empty: no plan for it)
    void SourceEntries(Quest const* quest, uint32 objective, std::vector<uint32>& creatures,
                       std::vector<uint32>& objects);
}

#endif
