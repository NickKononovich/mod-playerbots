/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GlyphActions.h"

#include "Bag.h"
#include "DBCStores.h"
#include "Event.h"
#include "Group.h"
#include "GuildHonest.h"
#include "ObjectMgr.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>

namespace
{
constexpr uint32 GLYPH_SCAN_INTERVAL_MS = 30 * IN_MILLISECONDS;
constexpr float GLYPH_HANDOVER_DISTANCE = 20.0f;
constexpr uint8 GLYPH_SLOT_MIN_LEVEL[MAX_GLYPH_SLOT_INDEX] = { 15, 15, 50, 30, 70, 80 };  // Spell::EffectApplyGlyph

// Stops at the first item for which visit() returns true.
template <typename Visitor>
void ForEachBagItem(Player* player, Visitor visit)
{
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (visit(item))
                return;
    }

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = player->GetBagByPos(bagSlot);
        if (!bag)
            continue;

        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
        {
            if (Item* item = bag->GetItemByPos(uint8(slot)))
                if (visit(item))
                    return;
        }
    }
}

uint32 GetGlyphSpell(ItemTemplate const* proto)
{
    for (uint32 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(proto->Spells[i].SpellId);
        if (spellInfo && spellInfo->HasEffect(SPELL_EFFECT_APPLY_GLYPH))
            return spellInfo->Id;
    }

    return 0;
}

bool HasCustomGlyphs(Player* player)
{
    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    return ai && ai->GetAiObjectContext()->GetValue<bool>("custom_glyphs")->Get();
}

// Random bots still get their glyphs from the factory; the self-bot is the user's own character.
// [honest] The guild-plan bots (groups B / C are random bots in code) use glyph items only.
bool UsesHonestGlyphs(Player* player)
{
    return (!sRandomPlayerbotMgr.IsRandomBot(player) || GuildHonest::IsHonestBot(player)) && !IsSelfBot(player);
}
}  // namespace

namespace GlyphUtil
{
uint32 GetItemGlyphId(ItemTemplate const* proto)
{
    if (!proto || proto->Class != ITEM_CLASS_GLYPH)
        return 0;

    for (uint32 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(proto->Spells[i].SpellId);
        if (!spellInfo)
            continue;

        for (uint8 effect = EFFECT_0; effect <= EFFECT_2; ++effect)
        {
            if (spellInfo->Effects[effect].Effect == SPELL_EFFECT_APPLY_GLYPH && spellInfo->Effects[effect].MiscValue)
                return uint32(spellInfo->Effects[effect].MiscValue);
        }
    }

    return 0;
}

std::vector<uint32> const& GetWantedGlyphItems(Player* player)
{
    static std::vector<uint32> const empty;
    uint8 const cls = player->getClass();
    uint8 const specIndex = PlayerbotFactory::GetGlyphSpecIndex(player);
    if (cls >= MAX_CLASSES || specIndex >= MAX_SPECNO)
        return empty;

    return sPlayerbotAIConfig.ParsedSpecGlyph[cls][specIndex];
}

int32 FindGlyphSlot(Player* player, uint32 glyphId, bool keepCurrent)
{
    GlyphPropertiesEntry const* glyph = sGlyphPropertiesStore.LookupEntry(glyphId);
    if (!glyph)
        return -1;

    for (uint8 slot = 0; slot < MAX_GLYPH_SLOT_INDEX; ++slot)
    {
        if (player->GetGlyph(slot) == glyphId)
            return -1;
    }

    std::vector<uint32> wantedGlyphs;
    for (uint32 itemId : GetWantedGlyphItems(player))
    {
        if (uint32 wanted = GetItemGlyphId(sObjectMgr->GetItemTemplate(itemId)))
            wantedGlyphs.push_back(wanted);
    }

    int32 replaceable = -1;
    for (uint8 slot = 0; slot < MAX_GLYPH_SLOT_INDEX; ++slot)
    {
        if (player->GetLevel() < GLYPH_SLOT_MIN_LEVEL[slot])
            continue;

        GlyphSlotEntry const* slotEntry = sGlyphSlotStore.LookupEntry(player->GetGlyphSlot(slot));
        if (!slotEntry || slotEntry->TypeFlags != glyph->TypeFlags)
            continue;

        uint32 const current = player->GetGlyph(slot);
        if (!current)
            return slot;

        if (!keepCurrent && replaceable < 0 &&
            std::find(wantedGlyphs.begin(), wantedGlyphs.end(), current) == wantedGlyphs.end())
            replaceable = slot;
    }

    return replaceable;
}

bool NeedsGlyphItem(Player* player, uint32 itemId, bool countBags)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    uint32 const glyphId = GetItemGlyphId(proto);
    if (!glyphId || (proto->AllowableClass & player->getClassMask()) == 0 || proto->RequiredLevel > player->GetLevel())
        return false;

    std::vector<uint32> const& wanted = GetWantedGlyphItems(player);
    if (std::find(wanted.begin(), wanted.end(), itemId) == wanted.end())
        return false;

    if (countBags && player->HasItemCount(itemId, 1))
        return false;

    return FindGlyphSlot(player, glyphId, HasCustomGlyphs(player)) >= 0;
}
}  // namespace GlyphUtil

bool LearnGlyphAction::isUseful()
{
    if (bot->GetLevel() < GLYPH_SLOT_MIN_LEVEL[0] || bot->IsInCombat() || !bot->IsAlive() || bot->isMoving() ||
        bot->IsMounted() || bot->IsNonMeleeSpellCast(true) || !UsesHonestGlyphs(bot))
        return false;

    uint32 const now = getMSTime();
    if (_lastScan && getMSTimeDiff(_lastScan, now) < GLYPH_SCAN_INTERVAL_MS)
        return !_glyphItem.IsEmpty();

    _lastScan = now;
    _glyphItem.Clear();
    _glyphSlot = -1;

    bool const keepCurrent = HasCustomGlyphs(bot);
    std::vector<uint32> const& wanted = GlyphUtil::GetWantedGlyphItems(bot);
    if (wanted.empty())
        return false;

    ForEachBagItem(bot, [&](Item* item)
    {
        uint32 const itemId = item->GetEntry();
        if (std::find(wanted.begin(), wanted.end(), itemId) == wanted.end())
            return false;

        uint32 const glyphId = GlyphUtil::GetItemGlyphId(item->GetTemplate());
        if (!glyphId || bot->CanUseItem(item) != EQUIP_ERR_OK)
            return false;

        int32 const slot = GlyphUtil::FindGlyphSlot(bot, glyphId, keepCurrent);
        if (slot < 0)
            return false;

        _glyphItem = item->GetGUID();
        _glyphSlot = slot;
        return true;
    });

    return !_glyphItem.IsEmpty();
}

bool LearnGlyphAction::Execute(Event /*event*/)
{
    Item* item = bot->GetItemByGuid(_glyphItem);
    int32 const slot = _glyphSlot;
    _glyphItem.Clear();
    _glyphSlot = -1;
    _lastScan = 0;  // the next glyph item is looked for on the next check

    if (!item || slot < 0 || slot >= MAX_GLYPH_SLOT_INDEX || bot->CanUseItem(item) != EQUIP_ERR_OK)
        return false;

    uint32 const spellId = GetGlyphSpell(item->GetTemplate());
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return false;

    bot->ClearUnitState(UNIT_STATE_CHASE);
    bot->ClearUnitState(UNIT_STATE_FOLLOW);

    // Same packet as a client: the core casts the item spell, checks the slot type and consumes the glyph.
    std::string const glyphName = item->GetTemplate()->Name1;  // [insc145] the item is consumed by the use
    WorldPacket packet(CMSG_USE_ITEM);
    packet << uint8(item->GetBagSlot()) << uint8(item->GetSlot()) << uint8(1) << spellId << item->GetGUID()
           << uint32(slot) << uint8(0);
    packet << uint32(TARGET_FLAG_NONE);
    bot->GetSession()->HandleUseItemOpcode(packet);

    botAI->SetNextCheckDelay(spellInfo->CalcCastTime() + sPlayerbotAIConfig.ReactDelay);
    // [insc145] the last step of the glyph chain (made by a scribe, moved by the guild's transfer)
    LOG_INFO("playerbots", "[insc145] {} L{} uses {} in glyph slot {}", bot->GetName(), bot->GetLevel(),
             glyphName, slot);
    return true;
}

bool GiveGlyphAction::isUseful()
{
    // [honest] a guild-plan bot does not hand glyphs out on its own: the global leader moves glyph items (GLYPH task)
    if (bot->IsInCombat() || !bot->IsAlive() || !bot->GetGroup() || !UsesHonestGlyphs(bot) ||
        GuildHonest::IsHonestBot(bot))
        return false;

    uint32 const now = getMSTime();
    if (_lastScan && getMSTimeDiff(_lastScan, now) < GLYPH_SCAN_INTERVAL_MS)
        return _itemId != 0;

    _lastScan = now;
    _itemId = 0;
    _receiver.Clear();

    std::vector<uint32> glyphItems;
    ForEachBagItem(bot, [&](Item* item)
    {
        uint32 const itemId = item->GetEntry();
        if (item->GetTemplate()->Class == ITEM_CLASS_GLYPH &&
            std::find(glyphItems.begin(), glyphItems.end(), itemId) == glyphItems.end() &&
            !GlyphUtil::NeedsGlyphItem(bot, itemId, false))
            glyphItems.push_back(itemId);

        return false;
    });

    if (glyphItems.empty())
        return false;

    Group* group = bot->GetGroup();
    for (GroupReference* ref = group->GetFirstMember(); ref && !_itemId; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || member->GetMap() != bot->GetMap() ||
            !member->IsAlive() || sRandomPlayerbotMgr.IsRandomBot(member) ||
            !bot->IsWithinDistInMap(member, GLYPH_HANDOVER_DISTANCE))
            continue;

        for (uint32 itemId : glyphItems)
        {
            if (GlyphUtil::NeedsGlyphItem(member, itemId))
            {
                _itemId = itemId;
                _receiver = member->GetGUID();
                break;
            }
        }
    }

    return _itemId != 0;
}

bool GiveGlyphAction::Execute(Event /*event*/)
{
    uint32 const itemId = _itemId;
    Player* receiver = ObjectAccessor::FindPlayer(_receiver);
    _itemId = 0;
    _receiver.Clear();
    _lastScan = 0;

    if (!itemId || !receiver || !receiver->IsInWorld() || receiver->GetMap() != bot->GetMap() ||
        !bot->IsWithinDistInMap(receiver, GLYPH_HANDOVER_DISTANCE) || !bot->HasItemCount(itemId, 1) ||
        !GlyphUtil::NeedsGlyphItem(receiver, itemId))
        return false;

    ItemPosCountVec dest;
    if (receiver->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, 1) != EQUIP_ERR_OK)
        return false;

    // One glyph out of a possible stack: the giver loses exactly the copy the receiver gets.
    bot->DestroyItemCount(itemId, 1, true);
    if (Item* given = receiver->StoreNewItem(dest, itemId, true))
        receiver->SendNewItem(given, 1, true, false);

    LOG_DEBUG("playerbots", "Bot {} gives glyph item {} to {}", bot->GetName(), itemId, receiver->GetName());
    return true;
}
