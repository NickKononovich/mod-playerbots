/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SetCraftAction.h"
#include "ChatHelper.h"
#include "CraftValue.h"
#include "Event.h"
#include "GlyphActions.h"
#include "Group.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"

#include <algorithm>

std::map<uint32, SkillLineAbilityEntry const*> SetCraftAction::skillSpells;

bool SetCraftAction::Execute(Event event)
{
    Player* master = GetMaster();
    if (!master)
        return false;

    std::string const link = event.getParam();

    CraftData& data = AI_VALUE(CraftData&, "craft");
    if (link == "reset")
    {
        data.Reset();
        botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "craft_reset", "I will not craft anything", {}));
        return true;
    }

    if (link == "?")
    {
        TellCraft();
        return true;
    }

    ItemIds itemIds = chat->parseItems(link);
    if (itemIds.empty())
    {
        botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "craft_usage", "Usage: 'craft [itemId]' or 'craft reset'", {}));
        return false;
    }

    uint32 itemId = *itemIds.begin();
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
        return false;

    if (skillSpells.empty())
    {
        for (SkillLineAbilityEntry const* skillLine : sSkillLineAbilityStore)
            skillSpells[skillLine->Spell] = skillLine;
    }

    data.required.clear();
    data.obtained.clear();

    for (PlayerSpellMap::iterator itr = bot->GetSpellMap().begin(); itr != bot->GetSpellMap().end(); ++itr)
    {
        uint32 spellId = itr->first;

        if (itr->second->State == PLAYERSPELL_REMOVED || !itr->second->Active)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        SkillLineAbilityEntry const* skillLine = skillSpells[spellId];
        if (skillLine != nullptr)
        {
            for (uint8 i = 0; i < 3; ++i)
            {
                if (spellInfo->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM &&
                    itemId == spellInfo->Effects[i].ItemType)
                {
                    for (uint32 x = 0; x < MAX_SPELL_REAGENTS; ++x)
                    {
                        if (spellInfo->Reagent[x] <= 0)
                            continue;

                        uint32 itemid = spellInfo->Reagent[x];
                        uint32 reagentsRequired = spellInfo->ReagentCount[x];
                        if (itemid)
                        {
                            data.required[itemid] = reagentsRequired;
                            data.obtained[itemid] = 0;
                        }
                    }
                }
            }
        }
    }

    if (data.required.empty())
    {
        botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "craft_cannot_craft", "I cannot craft this", {}));
        return false;
    }

    data.itemId = itemId;

    TellCraft();
    return true;
}

void SetCraftAction::TellCraft()
{
    CraftData& data = AI_VALUE(CraftData&, "craft");
    if (data.IsEmpty())
    {
        botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "craft_reset", "I will not craft anything", {}));
        return;
    }

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(data.itemId);
    if (!proto)
        return;

    std::ostringstream reagentsOut;

    bool first = true;
    for (std::map<uint32, uint32>::iterator i = data.required.begin(); i != data.required.end(); ++i)
    {
        uint32 item = i->first;
        uint32 required = i->second;

        if (ItemTemplate const* reagent = sObjectMgr->GetItemTemplate(item))
        {
            if (first)
                first = false;
            else
                reagentsOut << ", ";

            reagentsOut << chat->FormatItem(reagent, required);

            uint32 given = data.obtained[item];
            if (given)
                reagentsOut << "|cffffff00(x" << given << " given)|r ";
        }
    }

    botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "craft_summary",
        "I will craft %item using reagents: %reagents (craft fee: %money)",
        {{"%item", chat->FormatItem(proto)},
         {"%reagents", reagentsOut.str()},
         {"%money", chat->formatMoney(GetCraftFee(data))}}));
}

uint32 SetCraftAction::GetCraftFee(CraftData& data)
{
    if (data.IsEmpty())
        return 0;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(data.itemId);
    if (!proto)
        return 0;

    uint32 level = std::max(proto->ItemLevel, proto->RequiredLevel);
    return level * level / 40;
}

namespace
{
    constexpr uint32 AUTO_CRAFT_RETRY_MS = 10 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 NEEDED_GLYPHS_REFRESH_MS = MINUTE * IN_MILLISECONDS;

    uint32 GetCreatedItem(SpellInfo const* spellInfo)
    {
        for (uint8 i = EFFECT_0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM && spellInfo->Effects[i].ItemType)
                return spellInfo->Effects[i].ItemType;
        }

        return 0;
    }

    // Tradeskills that level by casting a recipe from reagents. Mining is here for Smelting;
    // Enchanting is left out on purpose: its recipes consume the item they enchant.
    bool IsAutoCraftSkill(uint32 skillLine)
    {
        switch (skillLine)
        {
            case SKILL_ALCHEMY:
            case SKILL_BLACKSMITHING:
            case SKILL_COOKING:
            case SKILL_ENGINEERING:
            case SKILL_FIRST_AID:
            case SKILL_INSCRIPTION:
            case SKILL_JEWELCRAFTING:
            case SKILL_LEATHERWORKING:
            case SKILL_MINING:
            case SKILL_TAILORING:
                return true;
            default:
                return false;
        }
    }
}

bool AutoCraftAction::isUseful()
{
    return !bot->IsInCombat() && !bot->isMoving() && !bot->IsMounted() && !bot->IsNonMeleeSpellCast(true) &&
           bot->IsAlive();
}

bool AutoCraftAction::HasReagents(uint32 spellId)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return false;

    for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
    {
        if (spellInfo->Reagent[i] <= 0)
            continue;

        if (!bot->HasItemCount(spellInfo->Reagent[i], spellInfo->ReagentCount[i]))
            return false;
    }

    return true;
}

void AutoCraftAction::RefreshNeededGlyphs()
{
    uint32 const now = getMSTime();
    if (_neededGlyphsAt && getMSTimeDiff(_neededGlyphsAt, now) < NEEDED_GLYPHS_REFRESH_MS)
        return;

    _neededGlyphsAt = now;
    _neededGlyphs.clear();

    Group* group = bot->GetGroup();
    if (!group || !bot->HasSkill(SKILL_INSCRIPTION))
        return;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        // Same map only: other maps run on other threads. Random bots get their glyphs from the factory.
        if (!member || !member->IsInWorld() || member->GetMap() != bot->GetMap() ||
            sRandomPlayerbotMgr.IsRandomBot(member))
            continue;

        for (uint32 itemId : GlyphUtil::GetWantedGlyphItems(member))
        {
            if (std::find(_neededGlyphs.begin(), _neededGlyphs.end(), itemId) == _neededGlyphs.end() &&
                GlyphUtil::NeedsGlyphItem(member, itemId))
                _neededGlyphs.push_back(itemId);
        }
    }
}

uint32 AutoCraftAction::ChooseRecipe()
{
    uint32 const now = getMSTime();
    uint32 bestSpell = 0;
    uint32 bestScore = 0;

    RefreshNeededGlyphs();

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;

        auto retry = _failedAt.find(spellId);
        if (retry != _failedAt.end() && getMSTimeDiff(retry->second, now) < AUTO_CRAFT_RETRY_MS)
            continue;

        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            SkillLineAbilityEntry const* ability = itr->second;
            if (!IsAutoCraftSkill(ability->SkillLine) || !ability->TrivialSkillLineRankHigh)
                continue;

            // A glyph someone in the group is waiting for; one copy at a time, "give glyph" hands it over.
            if (ability->SkillLine == SKILL_INSCRIPTION && !_neededGlyphs.empty())
            {
                SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
                uint32 const createdItem = spellInfo ? GetCreatedItem(spellInfo) : 0;
                if (createdItem &&
                    std::find(_neededGlyphs.begin(), _neededGlyphs.end(), createdItem) != _neededGlyphs.end() &&
                    !bot->HasItemCount(createdItem, 1) && HasReagents(spellId))
                    return spellId;
            }

            uint16 const skill = bot->GetSkillValue(ability->SkillLine);
            if (!skill || skill >= bot->GetMaxSkillValue(ability->SkillLine))
                continue;

            // Orange until the "low" rank, yellow until halfway to grey; green and grey rarely raise the skill.
            uint32 const yellowFrom = ability->TrivialSkillLineRankLow;
            uint32 const greenFrom = (ability->TrivialSkillLineRankLow + ability->TrivialSkillLineRankHigh) / 2;
            if (skill >= greenFrom)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo || !spellInfo->HasEffect(SPELL_EFFECT_CREATE_ITEM) || !HasReagents(spellId))
                continue;

            uint32 const score = skill < yellowFrom ? 2 : 1;
            if (score > bestScore)
            {
                bestScore = score;
                bestSpell = spellId;
            }
        }
    }

    return bestSpell;
}

bool AutoCraftAction::Execute(Event /*event*/)
{
    uint32 const spellId = ChooseRecipe();
    if (!spellId)
        return false;

    if (!botAI->CanCastSpell(spellId, bot, true) || !botAI->CastSpell(spellId, bot))
    {
        _failedAt[spellId] = getMSTime();
        return false;
    }

    return true;
}
