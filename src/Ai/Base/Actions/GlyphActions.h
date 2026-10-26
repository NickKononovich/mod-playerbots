/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GLYPHACTIONS_H
#define PLAYERBOTS_GLYPHACTIONS_H

#include "Action.h"
#include "ObjectGuid.h"

#include <vector>

class Player;
struct ItemTemplate;

// Honest glyphs: a non-random bot only gets a glyph from a glyph item (crafted by an Inscription bot, looted or
// bought), and only the glyphs of its premade spec list (Playerbots.PremadeSpecGlyph).
namespace GlyphUtil
{
// GlyphProperties.dbc id the item applies, 0 when it is not a glyph item.
uint32 GetItemGlyphId(ItemTemplate const* proto);
// Glyph item ids of the player's premade spec list, empty when none is configured.
std::vector<uint32> const& GetWantedGlyphItems(Player* player);
// Slot index for the glyph, or -1: a free slot of its type first, then one holding a glyph that is not on the
// premade list (only free slots when keepCurrent). -1 also when the glyph is already applied.
int32 FindGlyphSlot(Player* player, uint32 glyphId, bool keepCurrent);
// The player wants this glyph item from its premade list, has a slot for it and, when countBags, does not
// already carry one.
bool NeedsGlyphItem(Player* player, uint32 itemId, bool countBags = true);
}  // namespace GlyphUtil

// Uses a premade-list glyph item from the bags when a slot of its type is free or holds an off-list glyph.
class LearnGlyphAction : public Action
{
public:
    LearnGlyphAction(PlayerbotAI* botAI) : Action(botAI, "learn glyph") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    ObjectGuid _glyphItem;
    int32 _glyphSlot = -1;
    uint32 _lastScan = 0;
};

// Hands a glyph item from the bags to a nearby group member whose premade list needs it (and the bot does not).
class GiveGlyphAction : public Action
{
public:
    GiveGlyphAction(PlayerbotAI* botAI) : Action(botAI, "give glyph") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    uint32 _itemId = 0;
    ObjectGuid _receiver;
    uint32 _lastScan = 0;
};

#endif
