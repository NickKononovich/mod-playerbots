/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SETCRAFTACTION_H
#define PLAYERBOTS_SETCRAFTACTION_H

#include "Action.h"
#include "CraftValue.h"

#include <unordered_map>
#include <vector>

class PlayerbotAI;

struct SkillLineAbilityEntry;

class SetCraftAction : public Action
{
public:
    SetCraftAction(PlayerbotAI* botAI) : Action(botAI, "craft") {}

    bool Execute(Event event) override;

    static uint32 GetCraftFee(CraftData& craftData);

private:
    void TellCraft();

    static std::map<uint32, SkillLineAbilityEntry const*> skillSpells;
};

// Crafts, on its own, the known tradeskill recipe that still raises the skill (orange or yellow) and whose
// reagents are already in the bags. A glyph that a group member's premade list needs comes first, whatever its
// colour.
class AutoCraftAction : public Action
{
public:
    AutoCraftAction(PlayerbotAI* botAI) : Action(botAI, "auto craft") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    uint32 ChooseRecipe();
    bool HasReagents(uint32 spellId);
    void RefreshNeededGlyphs();

    // When the core last refused a recipe (no forge, missing tool, full bags); it is skipped for a while.
    std::unordered_map<uint32, uint32> _failedAt;
    // Glyph items that nearby group members need (Inscription only), refreshed every minute.
    std::vector<uint32> _neededGlyphs;
    uint32 _neededGlyphsAt = 0;
};

#endif
