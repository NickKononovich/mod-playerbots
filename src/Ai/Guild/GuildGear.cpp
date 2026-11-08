/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildGear.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "PlayerbotsDatabase.h"
#include "GameObject.h"
#include "Log.h"
#include "LootMgr.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "AiFactory.h"
#include "Bag.h"
#include "GuildCoordinator.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "RandomItemMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StatsWeightCalculator.h"
#include "Timer.h"

namespace
{
    enum : uint8
    {
        G_HEAD = 0, G_NECK, G_SHOULDERS, G_BACK, G_CHEST, G_WRISTS, G_HANDS, G_WAIST, G_LEGS, G_FEET, G_FINGER,
        G_TRINKET, G_MAINHAND, G_TWOHAND, G_OFFHAND, G_RANGED, G_COUNT
    };
    static_assert(G_COUNT == GuildGear::GROUP_COUNT, "GROUP_COUNT");

    char const* const GROUP_NAMES[G_COUNT] = {"head",      "neck",     "shoulders", "back",  "chest", "wrists",
                                              "hands",     "waist",    "legs",      "feet",  "ring",  "trinket",
                                              "main hand", "two-hand", "off hand",  "ranged"};

    constexpr uint32 MAX_REFERENCE_DEPTH = 4;  // LootTemplate::Process nests references deeper than any table does

    struct Index
    {
        std::unordered_map<uint32, std::vector<GuildGear::Source>> sources;
        std::vector<uint32> groups[G_COUNT];
        std::unordered_map<uint32, std::vector<GuildGear::VendorSpawn>> vendors;
        std::unordered_map<uint32, GuildGear::CraftRecipe> recipes;
        std::vector<uint32> bagRecipes;
        std::unordered_map<uint32, GuildGear::RefInfo> refs;
        std::unordered_map<uint32, std::string> mapNames;
        uint32 counts[10]{};
        uint32 buildMs{0};
        bool loaded{false};
    };

    Index index;  // built once on the world thread at startup, read-only afterwards
    std::vector<uint32> const EMPTY;
    std::unordered_set<uint32> tokens;  // items an extended cost asks for (tier tokens, emblems, badges, marks)

    bool Equippable(ItemTemplate const* proto)
    {
        return proto && (proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON) &&
               proto->Quality >= ITEM_QUALITY_NORMAL && GuildGear::EquipGroup(proto->InventoryType) != 0xFF;
    }

    bool GenericBag(ItemTemplate const* proto)
    {
        return proto && proto->Class == ITEM_CLASS_CONTAINER && proto->SubClass == ITEM_SUBCLASS_CONTAINER &&
               proto->ContainerSlots > 0;
    }

    // the items the index keeps: gear, bags, and the tokens that buy gear
    bool Indexed(uint32 itemId)
    {
        if (tokens.count(itemId))
            return true;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        return Equippable(proto) || GenericBag(proto);
    }

    void AddSource(uint32 itemId, GuildGear::Source const& src)
    {
        auto& list = index.sources[itemId];
        for (GuildGear::Source& s : list)
            if (s.kind == src.kind && s.id == src.id && s.difficulty == src.difficulty)
            {
                s.chance = std::max(s.chance, src.chance);
                return;
            }
        list.push_back(src);
        ++index.counts[src.kind];
    }

    void NoteMap(uint32 mapId)
    {
        if (index.mapNames.count(mapId))
            return;
        if (MapEntry const* map = sMapStore.LookupEntry(mapId))
            index.mapNames[mapId] = map->name[0];
    }

    // per creature entry (difficulty entries resolved to their base): where it stands, its levels, boss or not
    struct CreatureInfo
    {
        uint16 mapId{0};
        uint16 minLevel{0};
        uint16 maxLevel{0};
        uint8 difficulty{0};
        bool instance{false};
        bool boss{false};
    };

    // the items of a reference, nested references expanded (chances multiplied), memoised
    std::unordered_map<uint32, std::vector<std::pair<uint32, float>>> refItems;

    std::vector<std::pair<uint32, float>> const& RefItems(uint32 refId, uint32 depth)
    {
        auto const itr = refItems.find(refId);
        if (itr != refItems.end())
            return itr->second;
        std::vector<std::pair<uint32, float>> items;
        if (LootTemplate const* tmpl = LootTemplates_Reference.GetLootFor(refId))
            tmpl->VisitEntries(
                [&](LootStoreItem const& e, float chance)
                {
                    if (e.reference)
                    {
                        if (depth >= MAX_REFERENCE_DEPTH)
                            return;
                        float const scale = chance / 100.0f * std::max<uint8>(1, e.maxcount);
                        for (auto const& [item, c] : RefItems(uint32(std::abs(e.reference)), depth + 1))
                            items.emplace_back(item, std::min(100.0f, c * scale));
                    }
                    else if (Indexed(e.itemid))
                        items.emplace_back(e.itemid, chance);
                });
        return refItems[refId] = std::move(items);
    }

    void NoteRefUser(uint32 refId, uint32 minLevel, uint32 maxLevel, uint32 mapId)
    {
        GuildGear::RefInfo& info = index.refs[refId];
        info.minLevel = info.users ? std::min<uint16>(info.minLevel, uint16(minLevel)) : uint16(minLevel);
        info.maxLevel = std::max<uint16>(info.maxLevel, uint16(maxLevel));
        if (!info.users)
            info.mapId = uint16(mapId);
        ++info.users;
    }

    // One loot template of an owner. Direct items -> `direct` source; a reference -> one REFDROP ref per item (shared
    // world drops), or, for an instance boss, expanded into the boss's own DROP source.
    void ScanTemplate(LootTemplate const* tmpl, GuildGear::Source const& direct, bool expandRefs)
    {
        if (!tmpl)
            return;
        tmpl->VisitEntries(
            [&](LootStoreItem const& e, float chance)
            {
                if (!e.reference)
                {
                    if (Indexed(e.itemid))
                    {
                        GuildGear::Source s = direct;
                        s.chance = chance;
                        AddSource(e.itemid, s);
                    }
                    return;
                }
                uint32 const refId = uint32(std::abs(e.reference));
                float const scale = chance / 100.0f * std::max<uint8>(1, e.maxcount);
                if (expandRefs)
                {
                    for (auto const& [item, c] : RefItems(refId, 1))
                    {
                        GuildGear::Source s = direct;
                        s.chance = std::min(100.0f, c * scale);
                        AddSource(item, s);
                    }
                    return;
                }
                NoteRefUser(refId, direct.level, direct.levelMax, direct.mapId);
                for (auto const& [item, c] : RefItems(refId, 1))
                {
                    GuildGear::Source s;
                    s.kind = GuildGear::SRC_REFDROP;
                    s.id = refId;
                    s.chance = std::min(100.0f, c * scale);
                    AddSource(item, s);
                }
            });
    }

    uint16 EntryLevel(uint32 mapId, uint8 difficulty, uint16 fallback)
    {
        DungeonProgressionRequirements const* req = sObjectMgr->GetAccessRequirement(mapId, Difficulty(difficulty));
        if (!req && difficulty)
            req = sObjectMgr->GetAccessRequirement(mapId, Difficulty(0));
        return req && req->levelMin ? uint16(req->levelMin) : fallback;
    }

    void LoadCreatures()
    {
        // the first spawn of each creature entry: its map
        std::unordered_map<uint32, uint16> spawnMap;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            for (uint32 id : {data.id, data.id2, data.id3})
                if (id)
                    spawnMap.emplace(id, data.mapid);

        std::unordered_map<uint32, CreatureInfo> info;
        CreatureTemplateContainer const* templates = sObjectMgr->GetCreatureTemplates();
        auto const describe = [&](CreatureTemplate const& tmpl, uint32 base, uint8 difficulty)
        {
            CreatureInfo ci;
            auto const map = spawnMap.find(base);
            if (map == spawnMap.end())
                return false;  // never spawned (nor its base): not reachable in the world
            ci.mapId = map->second;
            MapEntry const* entry = sMapStore.LookupEntry(ci.mapId);
            ci.instance = entry && entry->IsDungeon();
            ci.minLevel = tmpl.minlevel;
            ci.maxLevel = tmpl.maxlevel;
            ci.difficulty = difficulty;
            ci.boss = ci.instance && ((tmpl.flags_extra & CREATURE_FLAG_EXTRA_DUNGEON_BOSS) ||
                                      tmpl.rank == CREATURE_ELITE_WORLDBOSS || tmpl.rank == CREATURE_ELITE_RARE ||
                                      tmpl.rank == CREATURE_ELITE_RAREELITE);
            info[tmpl.Entry] = ci;
            return true;
        };
        for (auto const& [entry, tmpl] : *templates)
        {
            if (!describe(tmpl, entry, 0))
                continue;
            for (uint8 d = 0; d < MAX_DIFFICULTY - 1; ++d)
                if (uint32 const hard = tmpl.DifficultyEntry[d])
                    if (CreatureTemplate const* h = sObjectMgr->GetCreatureTemplate(hard))
                    {
                        describe(*h, entry, uint8(d + 1));
                        // a difficulty entry carries the base's boss flag only on the base template
                        info[hard].boss = info[entry].boss;
                    }
        }

        for (auto const& [entry, ci] : info)
        {
            CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(entry);
            if (!tmpl)
                continue;
            GuildGear::Source s;
            s.id = entry;
            s.mapId = ci.mapId;
            s.difficulty = ci.difficulty;
            s.level = ci.minLevel;
            s.levelMax = ci.maxLevel;
            if (ci.instance)
            {
                NoteMap(ci.mapId);
                s.kind = ci.boss ? GuildGear::SRC_DROP : GuildGear::SRC_WORLD;
                if (ci.boss)
                    s.level = EntryLevel(ci.mapId, ci.difficulty, ci.minLevel);
            }
            else
                s.kind = GuildGear::SRC_WORLD;
            if (tmpl->lootid)
                ScanTemplate(LootTemplates_Creature.GetLootFor(tmpl->lootid), s, ci.boss);
            GuildGear::Source other = s;
            other.kind = GuildGear::SRC_OTHER;
            if (tmpl->pickpocketLootId)
            {
                other.aux = GuildGear::OTHER_PICKPOCKET;
                ScanTemplate(LootTemplates_Pickpocketing.GetLootFor(tmpl->pickpocketLootId), other, false);
            }
            if (tmpl->SkinLootId)
            {
                other.aux = GuildGear::OTHER_SKINNING;
                ScanTemplate(LootTemplates_Skinning.GetLootFor(tmpl->SkinLootId), other, false);
            }
        }
    }

    void LoadObjects()
    {
        std::unordered_map<uint32, uint16> spawnMap;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
            spawnMap.emplace(data.id, data.mapid);
        for (auto const& [entry, tmpl] : *sObjectMgr->GetGameObjectTemplates())
        {
            uint32 const lootId = tmpl.GetLootId();
            auto const map = spawnMap.find(entry);
            if (!lootId || map == spawnMap.end())
                continue;
            GuildGear::Source s;
            s.kind = GuildGear::SRC_OBJECT;
            s.id = entry;
            s.mapId = map->second;
            if (MapEntry const* m = sMapStore.LookupEntry(s.mapId))
                if (m->IsDungeon())
                {
                    NoteMap(s.mapId);
                    s.level = EntryLevel(s.mapId, 0, 0);
                }
            ScanTemplate(LootTemplates_Gameobject.GetLootFor(lootId), s, false);
        }
    }

    void LoadOtherStores()
    {
        // items opened for their loot (lockboxes, bags of loot): keyed by the item entry
        for (auto const& [itemId, tmpl] : LootTemplates_Item.GetTemplates())
        {
            GuildGear::Source s;
            s.kind = GuildGear::SRC_CONTAINER;
            s.id = itemId;
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId))
                s.level = uint16(proto->RequiredLevel);
            ScanTemplate(tmpl, s, false);
        }
        for (auto const& [zoneId, tmpl] : LootTemplates_Fishing.GetTemplates())
        {
            GuildGear::Source s;
            s.kind = GuildGear::SRC_OTHER;
            s.id = zoneId;
            s.aux = GuildGear::OTHER_FISHING;
            ScanTemplate(tmpl, s, false);
        }
        for (auto const& [mailId, tmpl] : LootTemplates_Mail.GetTemplates())
        {
            GuildGear::Source s;
            s.kind = GuildGear::SRC_OTHER;
            s.id = mailId;
            s.aux = GuildGear::OTHER_MAIL;
            ScanTemplate(tmpl, s, false);
        }
    }

    void LoadQuests()
    {
        for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
        {
            if (!quest)
                continue;
            uint16 const level = uint16(quest->GetQuestLevel() > 0 ? quest->GetQuestLevel() : quest->GetMinLevel());
            for (uint8 i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
                if (uint32 const item = quest->RewardChoiceItemId[i])
                    if (Indexed(item))
                        AddSource(item, {GuildGear::SRC_QUEST, 0, 0, questId, level, uint16(quest->GetMinLevel()),
                                         100.0f, i});
            for (uint8 i = 0; i < QUEST_REWARDS_COUNT; ++i)
                if (uint32 const item = quest->RewardItemId[i])
                    if (Indexed(item))
                        AddSource(item, {GuildGear::SRC_QUEST, 0, 0, questId, level, uint16(quest->GetMinLevel()),
                                         100.0f, 100u + i});
        }
    }

    void LoadCrafts(std::vector<uint32> const& craftLines)
    {
        std::unordered_set<uint32> lines(craftLines.begin(), craftLines.end());
        for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
        {
            SkillLineAbilityEntry const* ability = sSkillLineAbilityStore.LookupEntry(i);
            if (!ability || !lines.count(ability->SkillLine))
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(ability->Spell);
            if (!info)
                continue;
            for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
            {
                if (info->Effects[e].Effect != SPELL_EFFECT_CREATE_ITEM || !info->Effects[e].ItemType)
                    continue;
                uint32 const item = info->Effects[e].ItemType;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
                bool const bag = GenericBag(proto);
                if (!bag && !Equippable(proto))
                    break;
                GuildGear::CraftRecipe recipe;
                recipe.spellId = ability->Spell;
                recipe.skillLine = ability->SkillLine;
                recipe.reqSkill = ability->MinSkillLineRank;
                recipe.itemId = item;
                recipe.made = uint32(std::max(1, info->Effects[e].CalcValue()));
                for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
                    if (info->Reagent[r] > 0 && info->ReagentCount[r] > 0)
                        recipe.reagents.emplace_back(uint32(info->Reagent[r]), info->ReagentCount[r]);
                index.recipes[recipe.spellId] = recipe;
                if (bag)
                    index.bagRecipes.push_back(recipe.spellId);
                AddSource(item, {GuildGear::SRC_CRAFT, 0, 0, recipe.spellId, uint16(recipe.reqSkill), 0, 100.0f,
                                 recipe.skillLine});
                break;
            }
        }
        std::sort(index.bagRecipes.begin(), index.bagRecipes.end(), [](uint32 a, uint32 b)
                  {
                      uint32 const sa = sObjectMgr->GetItemTemplate(index.recipes[a].itemId)->ContainerSlots;
                      uint32 const sb = sObjectMgr->GetItemTemplate(index.recipes[b].itemId)->ContainerSlots;
                      return sa != sb ? sa > sb : a < b;
                  });
    }

    void LoadVendors()
    {
        std::unordered_set<uint32> seen;  // vendor entry whose list was read
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
            if (!tmpl || !(tmpl->npcflag & UNIT_NPC_FLAG_VENDOR))
                continue;
            VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(data.id);
            if (!items)
                continue;
            bool const firstSpawn = seen.insert(data.id).second;
            for (VendorItem const* item : items->m_items)
            {
                if (!item || !Indexed(item->item))
                    continue;
                index.vendors[item->item].push_back(
                    {data.id, data.mapid, data.posX, data.posY, data.posZ, item->ExtendedCost});
                if (firstSpawn)
                    AddSource(item->item, {GuildGear::SRC_VENDOR, 0, data.mapid, data.id, 0, 0, 100.0f,
                                           item->ExtendedCost});
            }
        }
    }

    size_t MemoryBytes()
    {
        // vectors by capacity, hash nodes as key + value + two pointers
        size_t bytes = 0;
        for (auto const& [item, list] : index.sources)
            bytes += sizeof(item) + sizeof(list) + 2 * sizeof(void*) + list.capacity() * sizeof(GuildGear::Source);
        for (auto const& list : index.groups)
            bytes += list.capacity() * sizeof(uint32);
        for (auto const& [item, list] : index.vendors)
            bytes += sizeof(item) + sizeof(list) + 2 * sizeof(void*) +
                     list.capacity() * sizeof(GuildGear::VendorSpawn);
        for (auto const& [spell, recipe] : index.recipes)
            bytes += sizeof(spell) + sizeof(recipe) + 2 * sizeof(void*) +
                     recipe.reagents.capacity() * sizeof(std::pair<uint32, uint32>);
        bytes += index.refs.size() * (sizeof(uint32) + sizeof(GuildGear::RefInfo) + 2 * sizeof(void*));
        for (auto const& [mapId, name] : index.mapNames)
            bytes += sizeof(mapId) + sizeof(name) + name.capacity() + 2 * sizeof(void*);
        bytes += tokens.size() * (sizeof(uint32) + 2 * sizeof(void*));
        return bytes;
    }
}

namespace GuildGear
{
    uint8 EquipGroup(uint8 inventoryType)
    {
        switch (inventoryType)
        {
            case INVTYPE_HEAD: return G_HEAD;
            case INVTYPE_NECK: return G_NECK;
            case INVTYPE_SHOULDERS: return G_SHOULDERS;
            case INVTYPE_CLOAK: return G_BACK;
            case INVTYPE_CHEST:
            case INVTYPE_ROBE: return G_CHEST;
            case INVTYPE_WRISTS: return G_WRISTS;
            case INVTYPE_HANDS: return G_HANDS;
            case INVTYPE_WAIST: return G_WAIST;
            case INVTYPE_LEGS: return G_LEGS;
            case INVTYPE_FEET: return G_FEET;
            case INVTYPE_FINGER: return G_FINGER;
            case INVTYPE_TRINKET: return G_TRINKET;
            case INVTYPE_WEAPON:
            case INVTYPE_WEAPONMAINHAND: return G_MAINHAND;
            case INVTYPE_2HWEAPON: return G_TWOHAND;
            case INVTYPE_SHIELD:
            case INVTYPE_HOLDABLE:
            case INVTYPE_WEAPONOFFHAND: return G_OFFHAND;
            case INVTYPE_RANGED:
            case INVTYPE_THROWN:
            case INVTYPE_RANGEDRIGHT:
            case INVTYPE_RELIC: return G_RANGED;
            default: return 0xFF;
        }
    }

    char const* GroupName(uint8 group) { return group < G_COUNT ? GROUP_NAMES[group] : "?"; }

    void Load(std::vector<uint32> const& craftLines)
    {
        uint32 const start = getMSTime();
        index = Index();
        refItems.clear();
        tokens.clear();
        for (uint32 i = 0; i < sItemExtendedCostStore.GetNumRows(); ++i)
            if (ItemExtendedCostEntry const* cost = sItemExtendedCostStore.LookupEntry(i))
                for (uint32 r = 0; r < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++r)
                    if (cost->reqitem[r])
                        tokens.insert(cost->reqitem[r]);
        LoadQuests();
        LoadCreatures();
        LoadObjects();
        LoadOtherStores();
        LoadCrafts(craftLines);
        LoadVendors();
        refItems.clear();  // only needed while building
        for (auto& [item, list] : index.sources)
        {
            list.shrink_to_fit();
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            uint8 const group = Equippable(proto) ? EquipGroup(proto->InventoryType) : 0xFF;
            if (group < G_COUNT)
                index.groups[group].push_back(item);
        }
        for (auto& list : index.groups)
        {
            std::sort(list.begin(), list.end(), [](uint32 a, uint32 b)
                      {
                          uint32 const la = sObjectMgr->GetItemTemplate(a)->RequiredLevel;
                          uint32 const lb = sObjectMgr->GetItemTemplate(b)->RequiredLevel;
                          return la != lb ? la < lb : a < b;
                      });
            list.shrink_to_fit();
        }
        index.buildMs = getMSTimeDiff(start, getMSTime());
        index.loaded = true;
        LOG_INFO("playerbots", "[guild50] upgrade index: {}", Stats());
    }

    std::string Stats()
    {
        std::ostringstream out;
        size_t grouped = 0;
        for (auto const& list : index.groups)
            grouped += list.size();
        size_t refs = 0;
        for (auto const& [item, list] : index.sources)
            refs += list.size();
        out << index.sources.size() << " items (" << grouped << " gear in slot groups, " << tokens.size()
            << " cost tokens), " << refs << " source refs: quest " << index.counts[SRC_QUEST] << ", boss drop "
            << index.counts[SRC_DROP] << ", world mob " << index.counts[SRC_WORLD] << ", shared reference "
            << index.counts[SRC_REFDROP] << " (" << index.refs.size() << " references), object "
            << index.counts[SRC_OBJECT] << ", container " << index.counts[SRC_CONTAINER] << ", pickpocket/skin/"
            << "fish/mail " << index.counts[SRC_OTHER] << ", craft " << index.counts[SRC_CRAFT] << ", vendor "
            << index.counts[SRC_VENDOR] << "; " << index.mapNames.size() << " instance maps; bag recipes "
            << index.bagRecipes.size() << "; memory ~" << (MemoryBytes() + 1023) / 1024 << " KiB; built in "
            << index.buildMs << " ms";
        return out.str();
    }

    std::vector<Source> const* SourcesOf(uint32 itemId)
    {
        auto const itr = index.sources.find(itemId);
        return itr == index.sources.end() ? nullptr : &itr->second;
    }

    std::vector<uint32> const& ItemsOfGroup(uint8 group) { return group < G_COUNT ? index.groups[group] : EMPTY; }

    std::vector<VendorSpawn> const* VendorsOf(uint32 itemId)
    {
        auto const itr = index.vendors.find(itemId);
        return itr == index.vendors.end() ? nullptr : &itr->second;
    }

    CraftRecipe const* Recipe(uint32 spellId)
    {
        auto const itr = index.recipes.find(spellId);
        return itr == index.recipes.end() ? nullptr : &itr->second;
    }

    std::vector<uint32> const& BagRecipes() { return index.bagRecipes; }

    RefInfo const* Reference(uint32 refId)
    {
        auto const itr = index.refs.find(refId);
        return itr == index.refs.end() ? nullptr : &itr->second;
    }

    std::string MapName(uint32 mapId)
    {
        auto const itr = index.mapNames.find(mapId);
        if (itr != index.mapNames.end())
            return itr->second;
        MapEntry const* map = sMapStore.LookupEntry(mapId);
        return map ? map->name[0] : "map " + std::to_string(mapId);
    }

    std::string ExtendedCostText(uint32 extendedCost)
    {
        ItemExtendedCostEntry const* cost = sItemExtendedCostStore.LookupEntry(extendedCost);
        if (!cost)
            return "";
        std::string text;
        auto const add = [&](std::string part) { text += (text.empty() ? "" : ", ") + part; };
        for (uint32 r = 0; r < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++r)
            if (cost->reqitem[r])
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(cost->reqitem[r]);
                add(std::to_string(cost->reqitemcount[r]) + "× " + (proto ? proto->Name1 : "?"));
            }
        if (cost->reqhonorpoints)
            add(std::to_string(cost->reqhonorpoints) + " honor");
        if (cost->reqarenapoints)
            add(std::to_string(cost->reqarenapoints) + " arena points");
        if (cost->reqpersonalarenarating)
            add("rating " + std::to_string(cost->reqpersonalarenarating));
        return text;
    }

    std::string ExtendedCostMissing(Player* member, uint32 extendedCost)
    {
        ItemExtendedCostEntry const* cost = sItemExtendedCostStore.LookupEntry(extendedCost);
        if (!cost)
            return "";
        std::string text;
        auto const add = [&](std::string part) { text += (text.empty() ? "" : ", ") + part; };
        for (uint32 r = 0; r < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++r)
            if (cost->reqitem[r])
            {
                uint32 const have = member->GetItemCount(cost->reqitem[r], true);
                if (have < cost->reqitemcount[r])
                {
                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(cost->reqitem[r]);
                    add(std::to_string(cost->reqitemcount[r] - have) + "× " + (proto ? proto->Name1 : "?"));
                }
            }
        if (member->GetHonorPoints() < cost->reqhonorpoints)
            add(std::to_string(cost->reqhonorpoints - member->GetHonorPoints()) + " honor");
        if (member->GetArenaPoints() < cost->reqarenapoints)
            add(std::to_string(cost->reqarenapoints - member->GetArenaPoints()) + " arena points");
        if (cost->reqpersonalarenarating)
            add("rating " + std::to_string(cost->reqpersonalarenarating));  // no PvP for guild bots
        return text;
    }

    std::vector<uint8> SlotsFor(Player* member, ItemTemplate const* proto)
    {
        Item* mainHand = member->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        bool const twoHand = mainHand && mainHand->GetTemplate()->InventoryType == INVTYPE_2HWEAPON &&
                             !member->CanTitanGrip();
        switch (EquipGroup(proto->InventoryType))
        {
            case G_HEAD: return {EQUIPMENT_SLOT_HEAD};
            case G_NECK: return {EQUIPMENT_SLOT_NECK};
            case G_SHOULDERS: return {EQUIPMENT_SLOT_SHOULDERS};
            case G_BACK: return {EQUIPMENT_SLOT_BACK};
            case G_CHEST: return {EQUIPMENT_SLOT_CHEST};
            case G_WRISTS: return {EQUIPMENT_SLOT_WRISTS};
            case G_HANDS: return {EQUIPMENT_SLOT_HANDS};
            case G_WAIST: return {EQUIPMENT_SLOT_WAIST};
            case G_LEGS: return {EQUIPMENT_SLOT_LEGS};
            case G_FEET: return {EQUIPMENT_SLOT_FEET};
            case G_FINGER: return {EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2};
            case G_TRINKET: return {EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2};
            case G_MAINHAND:
                if (proto->InventoryType == INVTYPE_WEAPON && member->CanDualWield() && !twoHand)
                    return {EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND};
                return {EQUIPMENT_SLOT_MAINHAND};
            case G_TWOHAND: return {EQUIPMENT_SLOT_MAINHAND};
            case G_OFFHAND:
                // an off-hand beside a two-hander cannot be worn; an off-hand weapon needs dual wield
                if (twoHand || (proto->InventoryType == INVTYPE_WEAPONOFFHAND && !member->CanDualWield()))
                    return {};
                return {EQUIPMENT_SLOT_OFFHAND};
            case G_RANGED: return {EQUIPMENT_SLOT_RANGED};
            default: return {};
        }
    }

    // [equip] the evaluation behind Evaluate (an item the member does not own) and ShouldEquip (`owned`: an item in
    // its bags, with its random suffix): one rule set for the plan and for the equip itself
    static bool EvaluateFor(Player* member, StatsWeightCalculator& calc, ItemTemplate const* proto, int32 randomProp,
                            Item* owned, Eval& out, bool plan)
    {
        if (!proto)
            return false;
        out = Eval();
        InventoryResult const use = member->CanUseItem(proto);
        if (use != EQUIP_ERR_OK)
        {
            if (!plan)
                return false;
            if (use == EQUIP_ERR_CANT_EQUIP_LEVEL_I)
                out.locked = "from L" + std::to_string(proto->RequiredLevel);
            else if (use == EQUIP_ERR_CANT_EQUIP_REPUTATION)
            {
                FactionEntry const* faction = sFactionStore.LookupEntry(proto->RequiredReputationFaction);
                static char const* const ranks[] = {"hated",    "hostile", "unfriendly", "neutral",
                                                    "friendly", "honored", "revered",    "exalted"};
                out.locked = std::string("reputation ") + (faction ? faction->name[0] : "?") + ": " +
                             (proto->RequiredReputationRank < 8 ? ranks[proto->RequiredReputationRank] : "?");
            }
            else
                return false;
        }
        if (proto->Class == ITEM_CLASS_WEAPON && !sRandomItemMgr.CanEquipWeapon(proto, member->getClass()))
            return false;
        if (proto->Class == ITEM_CLASS_ARMOR &&
            !sRandomItemMgr.CanEquipArmor(proto, member->getClass(), std::max<uint32>(member->GetLevel(),
                                                                                        proto->RequiredLevel)))
            return false;
        if (!owned)
        {
            // already owned (worn or in the bags): ShouldEquip decides it when the member equips from its bags
            if (member->GetItemCount(proto->ItemId, true))
                return false;
        }
        else
        {
            // a broken piece gives nothing; a unique-equipped one may be worn once
            if (owned->GetUInt32Value(ITEM_FIELD_DURABILITY) == 0 && owned->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
                return false;
            if (proto->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE) &&
                member->GetItemCount(proto->ItemId, true) > member->GetItemCount(proto->ItemId, false))
                return false;
        }
        std::vector<uint8> const slots = SlotsFor(member, proto);
        if (slots.empty())
            return false;

        // the weakest of the slots it may go into
        float current = -1.0f;
        Item* weakest = nullptr;
        uint8 weakestSlot = slots.front();
        for (uint8 slot : slots)
        {
            Item* worn = member->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!worn)
            {
                current = 0.0f;  // an empty slot is the weakest
                weakest = nullptr;
                weakestSlot = slot;
                break;
            }
            float const s = calc.CalculateItem(worn->GetEntry(), worn->GetItemRandomPropertyId());
            if (current < 0.0f || s < current)
            {
                current = s;
                weakest = worn;
                weakestSlot = slot;
            }
        }
        float const score = calc.CalculateItem(proto->ItemId, randomProp);
        out.score = score;
        out.current = std::max(0.0f, current);
        out.slot = weakestSlot;
        out.worn = weakest ? weakest->GetEntry() : 0;
        if (!weakest)
        {
            out.gain = score > 0.0f ? 1.0f : 0.01f;  // a stat-less piece still fills the slot (armor)
            return true;
        }
        ItemTemplate const* old = weakest->GetTemplate();
        // as ItemUsageValue: a worn piece the class should not wear (module armor / weapon rules) is replaced by any
        // better one, the threshold only guards a swap between two fitting pieces; a broken worn piece by a whole one
        bool const oldFits =
            (old->Class != ITEM_CLASS_WEAPON || sRandomItemMgr.CanEquipWeapon(old, member->getClass())) &&
            (old->Class != ITEM_CLASS_ARMOR ||
             sRandomItemMgr.CanEquipArmor(old, member->getClass(), member->GetLevel()));
        bool const oldBroken = weakest->GetUInt32Value(ITEM_FIELD_DURABILITY) == 0 &&
                               weakest->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
        float const threshold = oldFits ? sPlayerbotAIConfig.EquipUpgradeThreshold : 1.0f;
        if ((score || out.current) && score > out.current * threshold && score > out.current)
        {
            out.gain = out.current > 0.0f ? (score - out.current) / out.current : 1.0f;
            return true;
        }
        // [guild12] equal weight: more armor, then the higher item level, then the better quality
        if (score == out.current && proto->Class == old->Class &&
            (proto->Class != ITEM_CLASS_ARMOR || proto->SubClass >= old->SubClass))
        {
            bool better = false;
            if (proto->Armor != old->Armor)
                better = proto->Armor > old->Armor;
            else if (proto->ItemLevel != old->ItemLevel)
                better = proto->ItemLevel > old->ItemLevel;
            else
                better = proto->Quality > old->Quality;
            if (better)
            {
                out.gain = 0.001f;
                return true;
            }
        }
        if (owned && oldBroken)
        {
            out.gain = 0.001f;  // a whole piece beats a broken one (no stats while broken), as ItemUsageValue
            return true;
        }
        return false;
    }

    bool Evaluate(Player* member, StatsWeightCalculator& calc, ItemTemplate const* proto, Eval& out, bool plan)
    {
        return EvaluateFor(member, calc, proto, 0, nullptr, out, plan);
    }
}

// [guild15] gear history
namespace GuildGear
{
    namespace
    {
        std::mutex historyLock;
        // bot guid -> the entry it wore in each equipment slot
        std::unordered_map<uint32, std::array<uint32, EQUIPMENT_SLOT_END>> worn;
        // item guid -> where the bot got it
        std::unordered_map<uint32, char const*> itemSource;
        // (bot guid, item entry) -> who decided it and why (the global leader's plan)
        std::map<std::pair<uint32, uint32>, std::pair<std::string, std::string>> decisions;

        uint32 ItemLevelOf(uint32 entry)
        {
            ItemTemplate const* proto = entry ? sObjectMgr->GetItemTemplate(entry) : nullptr;
            return proto ? proto->ItemLevel : 0;
        }

        std::string Escaped(std::string text, size_t max)
        {
            if (text.size() > max)
                text.resize(max);
            std::string out;
            for (char c : text)
            {
                if (c == '\\' || c == '\'')
                    out += '\\';
                out += c;
            }
            return out;
        }

        // one row: the change and the full equipment after it (async: the database worker writes it)
        void WriteRow(Player* bot, uint8 slot, uint32 oldEntry, uint32 newEntry, char const* source,
                      std::string const& decider, std::string const& reason)
        {
            std::string entries, levels, enchants;
            uint32 levelSum = 0, counted = 0;
            for (uint8 s = EQUIPMENT_SLOT_START; s < EQUIPMENT_SLOT_END; ++s)
            {
                Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, s);
                uint32 const entry = item ? item->GetEntry() : 0;
                uint32 const ilvl = ItemLevelOf(entry);
                entries += ", " + std::to_string(entry);
                levels += ", " + std::to_string(ilvl);
                if (item && s != EQUIPMENT_SLOT_BODY && s != EQUIPMENT_SLOT_TABARD)
                {
                    levelSum += ilvl;
                    ++counted;
                }
                if (!item)
                    continue;
                uint32 const perm = item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
                uint32 const g1 = item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT);
                uint32 const g2 = item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT_2);
                uint32 const g3 = item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT_3);
                if (perm || g1 || g2 || g3)
                    enchants += (enchants.empty() ? "" : ";") + std::to_string(s) + ":" + std::to_string(perm) + "/" +
                                std::to_string(g1) + "," + std::to_string(g2) + "," + std::to_string(g3);
            }
            float const avg = counted ? float(levelSum) / float(counted) : 0.0f;
            uint32 const score = PlayerbotAI::GetMixedGearScore(bot, false, false);
            std::string columns;
            for (uint8 s = EQUIPMENT_SLOT_START; s < EQUIPMENT_SLOT_END; ++s)
                columns += ", e" + std::to_string(s);
            for (uint8 s = EQUIPMENT_SLOT_START; s < EQUIPMENT_SLOT_END; ++s)
                columns += ", i" + std::to_string(s);
            PlayerbotsDatabase.Execute(
                "INSERT INTO playerbots_gear_history (guid, name, level, class, spec, slot, old_entry, old_ilvl, "
                "new_entry, new_ilvl, source, decider, reason{}, enchants, avg_ilvl, gear_score) VALUES ({}, '{}', {}, "
                "{}, '{}', {}, {}, {}, {}, {}, '{}', '{}', '{}'{}{}, '{}', {:.2f}, {})",
                columns, bot->GetGUID().GetCounter(), bot->GetName(), bot->GetLevel(), bot->getClass(),
                Escaped(AiFactory::GetPlayerSpecName(bot), 32), slot, oldEntry, ItemLevelOf(oldEntry), newEntry,
                ItemLevelOf(newEntry), source, Escaped(decider, 8), Escaped(reason, 255), entries, levels,
                Escaped(enchants, 1024), avg, score);
        }
    }

    void HistoryInit(Player* bot)
    {
        if (!bot)
            return;
        std::array<uint32, EQUIPMENT_SLOT_END> now{};
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                now[slot] = item->GetEntry();
        {
            std::lock_guard<std::mutex> guard(historyLock);
            worn[bot->GetGUID().GetCounter()] = now;
        }
        // its first login with the history: a baseline snapshot (login path, world thread: one indexed lookup)
        if (!PlayerbotsDatabase.Query("SELECT 1 FROM playerbots_gear_history WHERE guid = {} LIMIT 1",
                                      bot->GetGUID().GetCounter()))
        {
            WriteRow(bot, 255, 0, 0, "baseline", "baseline", "baseline");
            LOG_INFO("playerbots", "[gear] {} L{} baseline snapshot", bot->GetName(), bot->GetLevel());
        }
    }

    void HistoryForget(Player* bot)
    {
        if (!bot)
            return;
        std::lock_guard<std::mutex> guard(historyLock);
        worn.erase(bot->GetGUID().GetCounter());
    }

    void HistoryNoteSource(Player* bot, Item* item, char const* source)
    {
        if (!bot || !item || !source)
            return;
        std::lock_guard<std::mutex> guard(historyLock);
        if (worn.find(bot->GetGUID().GetCounter()) == worn.end())
            return;  // not a tracked bot
        itemSource[item->GetGUID().GetCounter()] = source;
    }

    void HistoryNoteDecision(uint32 botLow, uint32 itemEntry, std::string const& decider, std::string const& reason)
    {
        std::lock_guard<std::mutex> guard(historyLock);
        if (worn.find(botLow) == worn.end())
            return;
        decisions[{botLow, itemEntry}] = {decider, reason};
    }

    bool HistoryOnEquip(Player* bot, Item* item, uint8 slot)
    {
        if (!bot || !item || slot >= EQUIPMENT_SLOT_END)
            return false;
        uint32 const entry = item->GetEntry();
        uint32 old = 0;
        char const* source = "other";
        std::string decider = "none";
        std::string reason;
        {
            std::lock_guard<std::mutex> guard(historyLock);
            auto found = worn.find(bot->GetGUID().GetCounter());
            if (found == worn.end())
                return false;
            old = found->second[slot];
            if (old == entry)
                return false;
            found->second[slot] = entry;
            auto const src = itemSource.find(item->GetGUID().GetCounter());
            if (src != itemSource.end())
            {
                source = src->second;
                itemSource.erase(src);
            }
            auto const why = decisions.find({bot->GetGUID().GetCounter(), entry});
            if (why != decisions.end())
            {
                decider = why->second.first;
                reason = why->second.second;
                decisions.erase(why);
            }
        }
        // a crafted item carries its maker
        if (!strcmp(source, "other") && item->GetGuidValue(ITEM_FIELD_CREATOR))
            source = "craft";
        // user 2026-11-08: the global leader is the single decider for gear; an equip without its decision is a
        // second decider still at work (auto-equip, its own quest reward pick, AutoUpgradeEquip...)
        if (decider == "none")
        {
            reason = "no leader decision";
            LOG_INFO("playerbots", "[alert] DOUBLE_DECISION {} slot {} item {} ({}) source {}", bot->GetName(), slot,
                     entry, item->GetTemplate()->Name1, source);
        }
        ItemTemplate const* oldProto = old ? sObjectMgr->GetItemTemplate(old) : nullptr;
        LOG_INFO("playerbots", "[gear] {} L{} slot {}: {} (ilvl {}) -> {} (ilvl {}), from {}, {}: {}", bot->GetName(),
                 bot->GetLevel(), slot, oldProto ? oldProto->Name1 : std::string("empty"), ItemLevelOf(old),
                 item->GetTemplate()->Name1, ItemLevelOf(entry), source, decider, reason);
        WriteRow(bot, slot, old, entry, source, decider, reason);
        return true;
    }
}

// [ui11] the gain as text (GuildGear.h)
namespace GuildGear
{
    std::string GainText(float gain, float score, float current)
    {
        // [econ1] b350 plan rows "+550 %", "+400 %", "empty slot (weight 0.0)": a percentage of a slot that held
        // next to nothing says nothing (L10 whites: weight 2 -> 13 is "+550 %"). An empty slot is "new slot" with
        // the weight it brings; above +100 % the real stat weight delta is shown instead of the percentage
        constexpr float PERCENT_SHOWN_UP_TO = 1.0f;
        char buf[64];
        if (current <= 0.0f)
        {
            if (score < 0.05f)
                return "new slot";
            snprintf(buf, sizeof(buf), "new slot (+%.1f weight)", score);
            return buf;
        }
        if (gain < 0.005f)
            return "equal weight, more armor / item level / intact";
        if (gain > PERCENT_SHOWN_UP_TO)
        {
            snprintf(buf, sizeof(buf), "+%.1f weight (%.1f -> %.1f)", score - current, current, score);
            return buf;
        }
        return "+" + std::to_string(int32(gain * 100.0f + 0.5f)) + "%";
    }
}

// [equip] the global leader's equip verdict (user 2026-11-08: one decider; b337: 61 loot equips and 1 vendor buy came
// from the bot's own upgrade check, [alert] DOUBLE_DECISION)
namespace GuildGear
{
    namespace
    {

        // a generic bag or a hunter's quiver: worth a bag slot when one is free or it is bigger than the one it
        // replaces (the smallest generic bag / the worn quiver), as ItemUsageValue
        bool BagVerdict(Player* bot, ItemTemplate const* proto, std::string& why)
        {
            bool const quiver = proto->Class == ITEM_CLASS_QUIVER;
            if (quiver ? bot->getClass() != CLASS_HUNTER
                       : proto->Class != ITEM_CLASS_CONTAINER || proto->SubClass != ITEM_SUBCLASS_CONTAINER)
                return false;
            bool freeSlot = false;
            uint32 smallest = 0, quiverSlots = 0;
            bool wornQuiver = false;
            for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
            {
                Bag* bag = bot->GetBagByPos(slot);
                if (!bag)
                {
                    freeSlot = true;
                    continue;
                }
                ItemTemplate const* b = bag->GetTemplate();
                if (b->Class == ITEM_CLASS_QUIVER)
                {
                    wornQuiver = true;
                    quiverSlots = std::max<uint32>(quiverSlots, b->ContainerSlots);
                }
                else if (b->Class == ITEM_CLASS_CONTAINER && b->SubClass == ITEM_SUBCLASS_CONTAINER &&
                         (!smallest || b->ContainerSlots < smallest))
                    smallest = b->ContainerSlots;
            }
            if (quiver)
            {
                if (wornQuiver ? quiverSlots >= proto->ContainerSlots : !freeSlot)
                    return false;
                why = wornQuiver
                          ? "quiver " + std::to_string(quiverSlots) + " → " + std::to_string(proto->ContainerSlots)
                          : "quiver into a free bag slot";
                return true;
            }
            if (!freeSlot && smallest >= proto->ContainerSlots)
                return false;
            why = freeSlot
                      ? "bag " + std::to_string(proto->ContainerSlots) + " slots into a free slot"
                      : "bag " + std::to_string(smallest) + " → " + std::to_string(proto->ContainerSlots) + " slots";
            return true;
        }

        std::string GearWhy(Eval const& ev, ItemTemplate const* proto)
        {
            ItemTemplate const* was = ev.worn ? sObjectMgr->GetItemTemplate(ev.worn) : nullptr;
            if (!was)  // [ui11] an empty slot: no percentage of nothing
                return std::string(GroupName(EquipGroup(proto->InventoryType))) + " into an empty slot";
            return std::string(GroupName(EquipGroup(proto->InventoryType))) + " " +
                   GainText(ev.gain, ev.score, ev.current) + " instead of " + was->Name1;
        }
    }

    bool ShouldEquip(Player* bot, Item* item, uint8& slot, float& gain, std::string& decision)
    {
        slot = NULL_SLOT;
        gain = 0.0f;
        decision.clear();
        ItemTemplate const* proto = bot && item ? item->GetTemplate() : nullptr;
        if (!proto || proto->InventoryType == INVTYPE_NON_EQUIP || proto->InventoryType == INVTYPE_AMMO)
            return false;
        // a BoE given to another member, or not decided yet
        if (sGuildCoordinator.IsHeld(bot, item))
            return false;
        std::string why;
        if (proto->Class == ITEM_CLASS_CONTAINER || proto->Class == ITEM_CLASS_QUIVER)
        {
            if (!BagVerdict(bot, proto, why))
                return false;
            gain = 0.001f;  // the equip action picks the bag slot
        }
        else
        {
            if (EquipGroup(proto->InventoryType) == 0xFF)
                return false;
            StatsWeightCalculator calc(bot);
            calc.SetItemSetBonus(false);
            calc.SetOverflowPenalty(false);
            Eval ev;
            if (!EvaluateFor(bot, calc, proto, item->GetItemRandomPropertyId(), item, ev, false))
                return false;
            slot = ev.slot;
            gain = ev.gain;
            why = GearWhy(ev, proto);
        }
        decision = std::string(bot->GetName()) + " → EQUIP: " + proto->Name1 + " — " + why;
        return true;
    }

    bool ShouldBuy(Player* bot, ItemTemplate const* proto, std::string& decision)
    {
        decision.clear();
        if (!bot || !proto || proto->InventoryType == INVTYPE_NON_EQUIP || proto->InventoryType == INVTYPE_AMMO)
            return false;
        std::string why;
        if (proto->Class == ITEM_CLASS_CONTAINER || proto->Class == ITEM_CLASS_QUIVER)
        {
            if (bot->GetItemCount(proto->ItemId, false) || !BagVerdict(bot, proto, why))
                return false;
        }
        else
        {
            if (EquipGroup(proto->InventoryType) == 0xFF)
                return false;
            StatsWeightCalculator calc(bot);
            calc.SetItemSetBonus(false);
            calc.SetOverflowPenalty(false);
            Eval ev;
            if (!Evaluate(bot, calc, proto, ev))
                return false;
            why = GearWhy(ev, proto);
        }
        decision =
            std::string(bot->GetName()) + " → BUY: " + proto->Name1 + " — upgrade " + why + ", with its own money";
        return true;
    }

    void HistoryNoteDecisionIfNone(uint32 botLow, uint32 itemEntry, std::string const& decider,
                                   std::string const& reason)
    {
        std::lock_guard<std::mutex> guard(historyLock);
        if (worn.find(botLow) == worn.end())
            return;
        decisions.emplace(std::make_pair(botLow, itemEntry), std::make_pair(decider, reason));
    }
}
