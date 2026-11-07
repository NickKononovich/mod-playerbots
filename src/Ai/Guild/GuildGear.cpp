/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildGear.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "RandomItemMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StatsWeightCalculator.h"

namespace
{
    enum : uint8
    {
        G_HEAD = 0, G_NECK, G_SHOULDERS, G_BACK, G_CHEST, G_WRISTS, G_HANDS, G_WAIST, G_LEGS, G_FEET, G_FINGER,
        G_TRINKET, G_MAINHAND, G_TWOHAND, G_OFFHAND, G_RANGED, G_COUNT
    };

    char const* const GROUP_NAMES[G_COUNT] = {"голова", "шея", "плечи", "спина", "грудь", "запястья", "кисти",
                                              "пояс", "ноги", "ступни", "кольцо", "аксессуар", "правая рука",
                                              "двуручное", "левая рука", "дальний бой"};

    struct Index
    {
        std::unordered_map<uint32, std::vector<GuildGear::Source>> sources;
        std::vector<uint32> groups[G_COUNT];
        std::unordered_map<uint32, std::vector<GuildGear::VendorSpawn>> vendors;
        std::unordered_map<uint32, GuildGear::CraftRecipe> recipes;
        std::vector<uint32> bagRecipes;
        std::unordered_map<uint32, std::string> dungeonNames;
        uint32 counts[5]{};
        bool loaded{false};
    };

    Index index;  // built once on the world thread at startup, read-only afterwards
    std::vector<uint32> const EMPTY;

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

    void AddSource(uint32 itemId, GuildGear::Source const& src)
    {
        auto& list = index.sources[itemId];
        for (GuildGear::Source const& s : list)
            if (s.kind == src.kind && s.id == src.id)
                return;
        list.push_back(src);
        ++index.counts[src.kind];
    }

    // Loot rows: (item, reference, chance, group). Grouped rows of chance 0 share what the group's explicit chances
    // leave (LootGroup::Roll: equal chance among them).
    struct LootRow
    {
        uint32 item;
        int32 reference;
        float chance;
        uint8 group;
        uint8 maxCount;
    };

    void Normalise(std::vector<LootRow>& rows)
    {
        std::map<uint8, std::pair<float, uint32>> groups;  // group -> (explicit sum, zero-chance count)
        for (LootRow const& r : rows)
            if (r.group)
            {
                auto& g = groups[r.group];
                if (r.chance > 0.0f)
                    g.first += r.chance;
                else
                    ++g.second;
            }
        for (LootRow& r : rows)
            if (r.group && r.chance <= 0.0f)
            {
                auto const& g = groups[r.group];
                r.chance = g.second ? std::max(0.0f, 100.0f - g.first) / g.second : 0.0f;
            }
    }

    void Expand(std::unordered_map<uint32, std::vector<LootRow>> const& refs, std::vector<LootRow> const& rows,
                float scale, uint32 depth, std::map<uint32, float>& out)
    {
        for (LootRow const& r : rows)
        {
            float const chance = r.chance * scale / 100.0f;
            if (r.reference)
            {
                auto const itr = refs.find(uint32(std::abs(r.reference)));
                if (itr != refs.end() && depth < 4)
                    Expand(refs, itr->second, chance * std::max<uint8>(1, r.maxCount), depth + 1, out);
                continue;
            }
            float& c = out[r.item];
            c = std::min(100.0f, c + chance * 100.0f);
        }
    }

    void LoadQuests()
    {
        for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
        {
            if (!quest)
                continue;
            uint32 const level = quest->GetQuestLevel() > 0 ? uint32(quest->GetQuestLevel()) : quest->GetMinLevel();
            for (uint8 i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
                if (uint32 const item = quest->RewardChoiceItemId[i])
                    if (Equippable(sObjectMgr->GetItemTemplate(item)))
                        AddSource(item, {GuildGear::SRC_QUEST, questId, level, 0, 100.0f, i});
            for (uint8 i = 0; i < QUEST_REWARDS_COUNT; ++i)
                if (uint32 const item = quest->RewardItemId[i])
                    if (Equippable(sObjectMgr->GetItemTemplate(item)))
                        AddSource(item, {GuildGear::SRC_QUEST, questId, level, 0, 100.0f, 100u + i});
        }
    }

    void LoadDrops()
    {
        // the bosses of the 5-man dungeons: the core's dungeon-boss flag (instance_encounters) or a boss / rare
        // elite rank, spawned in a non-raid instance map
        std::unordered_map<uint32, std::pair<uint32, uint32>> bossLoot;  // lootid -> (boss entry, map)
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            MapEntry const* map = sMapStore.LookupEntry(data.mapid);
            if (!map || !map->IsNonRaidDungeon())
                continue;
            CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
            if (!tmpl || !tmpl->lootid)
                continue;
            bool const boss = (tmpl->flags_extra & CREATURE_FLAG_EXTRA_DUNGEON_BOSS) ||
                              tmpl->rank == CREATURE_ELITE_WORLDBOSS || tmpl->rank == CREATURE_ELITE_RARE ||
                              tmpl->rank == CREATURE_ELITE_RAREELITE;
            if (!boss)
                continue;
            bossLoot.emplace(tmpl->lootid, std::make_pair(tmpl->Entry, data.mapid));
            if (!index.dungeonNames.count(data.mapid))
                index.dungeonNames[data.mapid] = map->name[0];
        }
        if (bossLoot.empty())
            return;

        std::unordered_map<uint32, std::vector<LootRow>> refs;
        if (QueryResult result = WorldDatabase.Query(
                "SELECT Entry, Item, Reference, Chance, GroupId, MaxCount FROM reference_loot_template"))
        {
            do
            {
                Field* f = result->Fetch();
                refs[f[0].Get<uint32>()].push_back({f[1].Get<uint32>(), f[2].Get<int32>(), f[3].Get<float>(),
                                                    f[4].Get<uint8>(), f[5].Get<uint8>()});
            } while (result->NextRow());
        }
        for (auto& [id, rows] : refs)
            Normalise(rows);

        std::unordered_map<uint32, std::vector<LootRow>> loot;
        std::ostringstream ids;
        bool first = true;
        for (auto const& [lootId, boss] : bossLoot)
        {
            ids << (first ? "" : ",") << lootId;
            first = false;
        }
        if (QueryResult result = WorldDatabase.Query(
                "SELECT Entry, Item, Reference, Chance, GroupId, MaxCount FROM creature_loot_template WHERE Entry IN (" +
                ids.str() + ")"))
        {
            do
            {
                Field* f = result->Fetch();
                loot[f[0].Get<uint32>()].push_back({f[1].Get<uint32>(), f[2].Get<int32>(), f[3].Get<float>(),
                                                    f[4].Get<uint8>(), f[5].Get<uint8>()});
            } while (result->NextRow());
        }
        for (auto& [lootId, rows] : loot)
        {
            Normalise(rows);
            std::map<uint32, float> items;
            Expand(refs, rows, 100.0f, 0, items);
            auto const& [bossEntry, mapId] = bossLoot[lootId];
            uint32 level = 0;
            if (DungeonProgressionRequirements const* req = sObjectMgr->GetAccessRequirement(mapId, DUNGEON_DIFFICULTY_NORMAL))
                level = req->levelMin;
            if (!level)
                if (CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(bossEntry))
                    level = tmpl->minlevel;
            for (auto const& [item, chance] : items)
                if (chance > 0.0f && Equippable(sObjectMgr->GetItemTemplate(item)))
                    AddSource(item, {GuildGear::SRC_DROP, bossEntry, level, mapId, chance, 0});
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
                else
                    AddSource(item, {GuildGear::SRC_CRAFT, recipe.spellId, recipe.reqSkill, 0, 100.0f,
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
                if (!item || item->ExtendedCost)
                    continue;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->item);
                if (!Equippable(proto))
                    continue;
                index.vendors[item->item].push_back({data.id, data.mapid, data.posX, data.posY, data.posZ});
                if (firstSpawn)
                    AddSource(item->item, {GuildGear::SRC_VENDOR, data.id, 0, data.mapid, 100.0f, uint32(proto->BuyPrice)});
            }
        }
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
        index = Index();
        LoadQuests();
        LoadDrops();
        LoadCrafts(craftLines);
        LoadVendors();
        for (auto const& [item, list] : index.sources)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            uint8 const group = proto ? EquipGroup(proto->InventoryType) : 0xFF;
            if (group < G_COUNT)
                index.groups[group].push_back(item);
        }
        for (auto& list : index.groups)
            std::sort(list.begin(), list.end(), [](uint32 a, uint32 b)
                      {
                          uint32 const la = sObjectMgr->GetItemTemplate(a)->RequiredLevel;
                          uint32 const lb = sObjectMgr->GetItemTemplate(b)->RequiredLevel;
                          return la != lb ? la < lb : a < b;
                      });
        index.loaded = true;
        LOG_INFO("playerbots", "[guild50] upgrade index: {}", Stats());
    }

    std::string Stats()
    {
        std::ostringstream out;
        size_t grouped = 0;
        for (auto const& list : index.groups)
            grouped += list.size();
        out << index.sources.size() << " items (" << grouped << " in slot groups); sources: quest " << index.counts[0]
            << ", dungeon drop " << index.counts[1] << " (" << index.dungeonNames.size() << " dungeons), craft "
            << index.counts[2] << ", vendor " << index.counts[3] << "; bag recipes " << index.bagRecipes.size()
            << "; vendor spawns for " << index.vendors.size() << " items";
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

    std::string DungeonName(uint32 mapId)
    {
        auto const itr = index.dungeonNames.find(mapId);
        return itr == index.dungeonNames.end() ? "map " + std::to_string(mapId) : itr->second;
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

    bool Evaluate(Player* member, StatsWeightCalculator& calc, ItemTemplate const* proto, Eval& out, bool ahead)
    {
        if (!proto)
            return false;
        InventoryResult const use = member->CanUseItem(proto);
        if (use != EQUIP_ERR_OK && !(ahead && use == EQUIP_ERR_CANT_EQUIP_LEVEL_I))
            return false;
        if (proto->Class == ITEM_CLASS_WEAPON && !sRandomItemMgr.CanEquipWeapon(proto, member->getClass()))
            return false;
        if (proto->Class == ITEM_CLASS_ARMOR &&
            !sRandomItemMgr.CanEquipArmor(proto, member->getClass(), member->GetLevel()))
            return false;
        // already owned (worn or in the bags): the member's own equip logic handles it
        if (member->GetItemCount(proto->ItemId, true))
            return false;
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
        float const score = calc.CalculateItem(proto->ItemId, 0);
        out = Eval();
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
        if ((score || out.current) && score > out.current * sPlayerbotAIConfig.EquipUpgradeThreshold &&
            score > out.current)
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
        return false;
    }
}
