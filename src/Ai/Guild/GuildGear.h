/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUILDGEAR_H
#define PLAYERBOTS_GUILDGEAR_H

#include <string>
#include <vector>

#include "Define.h"

class Item;
class Player;
class StatsWeightCalculator;
struct ItemTemplate;

// [guild50] The upgrade index of the global leader: for every equippable item (and every bag and every token an
// extended cost asks for), each way it can be had. Built once at startup from the core's own in-memory stores, no SQL:
// the loot stores (creature incl. difficulty entries of dungeons and the rescaled raids, gameobject chests, item
// containers, pickpocketing, skinning, fishing, mail; references expanded), the quest templates, the vendor lists
// (gold and extended cost), the craft recipes (SkillLineAbility). Compact: item -> a few source refs; a world-drop
// reference shared by many creatures is one ref with its level range. Per-member filters (class, spec, level,
// reputation, quest chain, race / class limits) are applied at plan time.
namespace GuildGear
{
    enum SourceKind : uint8
    {
        SRC_QUEST = 0,      // id = quest, level = quest level, aux = choice index (0..5) or 100 + fixed reward index
        SRC_DROP = 1,       // instance boss: id = creature entry, level = entry level of the map / difficulty, mapId
        SRC_CRAFT = 2,      // id = recipe spell, level = skill the recipe needs, aux = skill line
        SRC_VENDOR = 3,     // id = vendor creature entry, aux = ItemExtendedCost id (0 = gold)
        SRC_BANK = 4,       // the guild bank holds it (plan time only)
        SRC_WORLD = 5,      // an open-world creature's own loot: id = creature entry, level..levelMax, mapId
        SRC_REFDROP = 6,    // a shared loot reference (world drop): id = reference id, level..levelMax of its users
        SRC_OBJECT = 7,     // a gameobject (chest, fishing hole): id = gameobject entry, mapId
        SRC_CONTAINER = 8,  // opened from an item (lockbox, bag of loot): id = that item
        SRC_OTHER = 9       // pickpocketing / skinning / fishing zone / mail: id = loot id, aux = OtherStore
    };

    enum OtherStore : uint8
    {
        OTHER_PICKPOCKET = 1,
        OTHER_SKINNING = 2,
        OTHER_FISHING = 3,
        OTHER_MAIL = 4
    };

    struct Source  // 20 bytes
    {
        uint8 kind{SRC_QUEST};
        uint8 difficulty{0};
        uint16 mapId{0};
        uint32 id{0};
        uint16 level{0};
        uint16 levelMax{0};
        float chance{0.0f};
        uint32 aux{0};
    };

    struct VendorSpawn
    {
        uint32 entry{0};
        uint16 mapId{0};
        float x{0}, y{0}, z{0};
        uint32 extendedCost{0};
    };

    struct CraftRecipe
    {
        uint32 spellId{0};
        uint32 skillLine{0};
        uint32 reqSkill{0};
        uint32 itemId{0};
        uint32 made{1};
        std::vector<std::pair<uint32, uint32>> reagents;
    };

    struct RefInfo  // a shared loot reference: who uses it
    {
        uint16 minLevel{0};
        uint16 maxLevel{0};
        uint32 users{0};
        uint16 mapId{0};  // the map with most users
    };

    // startup (world thread); the lines are the craft skill lines the coordinator plans
    void Load(std::vector<uint32> const& craftLines);

    std::vector<Source> const* SourcesOf(uint32 itemId);
    // equippable items with a source, of one equip group (EquipGroup), sorted by RequiredLevel
    std::vector<uint32> const& ItemsOfGroup(uint8 group);
    std::vector<VendorSpawn> const* VendorsOf(uint32 itemId);
    CraftRecipe const* Recipe(uint32 spellId);
    std::vector<uint32> const& BagRecipes();  // recipe spells that make a generic bag
    RefInfo const* Reference(uint32 refId);
    std::string MapName(uint32 mapId);
    // "2× Item, 50 honor" of an extended cost; empty = the member has it all
    std::string ExtendedCostMissing(Player* member, uint32 extendedCost);
    std::string ExtendedCostText(uint32 extendedCost);

    // The equip group of an inventory type (one group per equipment slot family), 0xFF = not planned
    uint8 EquipGroup(uint8 inventoryType);
    constexpr uint8 GROUP_COUNT = 16;  // the slot groups EquipGroup returns: 0 .. GROUP_COUNT - 1
    char const* GroupName(uint8 group);
    // The equipment slots a member may put an item of this group into (both rings, main+off hand for dual wield)
    std::vector<uint8> SlotsFor(Player* member, ItemTemplate const* proto);

    struct Eval
    {
        float score{0.0f};
        float current{0.0f};
        float gain{0.0f};  // (score - current) / current; an empty slot = 1; a [guild12] tie-break = 0.001
        uint8 slot{0};
        uint32 worn{0};      // item entry in the weakest slot (0 = empty)
        std::string locked;  // plan mode: what the member still lacks to wear it (level, reputation), "" = nothing
    };

    // [ui11] the gain as text: "пустой слот" for an empty slot (no % of nothing), the weights for a slot whose worn
    // piece weighs next to nothing (BotControl showed +563800 % - an L74 piece over a grey one), else "+N%"
    std::string GainText(float gain, float score, float current);

    // Whether the item is an upgrade for the member in its weakest slot of the group: the core's CanUseItem, the
    // module's class armor / weapon rules, the StatsWeightCalculator score against the worn item scaled by
    // EquipUpgradeThreshold (as ItemUsageValue), equal scores broken as [guild12] (armor, item level, quality).
    // `calc` belongs to the member. plan: the item's level or reputation may still be ahead (out.locked says which).
    bool Evaluate(Player* member, StatsWeightCalculator& calc, ItemTemplate const* proto, Eval& out,
                  bool plan = false);

    // Counts, memory and build time of the index, for the startup log and the checks.
    std::string Stats();

    // [guild15] gear history (acore_playerbots.playerbots_gear_history, user 2026-11-08): every time a guild bot
    // equips a different item into a slot, a row (old / new entry and item level, where the new one came from, its
    // level) and a [gear] line. Map thread; the row is written by the async database queue.
    // the bot's equipment as it logged in (the "old" side of its first change)
    void HistoryInit(Player* bot);
    // where an item came from, noted when it reaches the bot's bags: quest reward / loot / vendor buy / guild transfer
    // / craft / bank
    void HistoryNoteSource(Player* bot, Item* item, char const* source);
    // the core equipped `item` into equipment slot `slot`: true when the slot holds a different item than before
    bool HistoryOnEquip(Player* bot, Item* item, uint8 slot);
    // who decided the bot equips `itemEntry` and why (the global leader's plan text): taken by its next equip of it
    void HistoryNoteDecision(uint32 botLow, uint32 itemEntry, std::string const& decider, std::string const& reason);
    void HistoryForget(Player* bot);
    // the same, unless a decision for that item is already pending (a leader BUY / QUEST_REWARD / transfer reason)
    void HistoryNoteDecisionIfNone(uint32 botLow, uint32 itemEntry, std::string const& decider,
                                   std::string const& reason);

    // [equip] user 2026-11-08: the global leader is the only gear decider. A guild-plan bot equips an item from its
    // bags only on the leader's yes: Evaluate's rules on the owned item (its random suffix, the same
    // StatsWeightCalculator weights and EquipUpgradeThreshold as ItemUsageValue, class armor / weapon rules, ties by
    // armor / item level / quality, a worn piece the class should not wear or a broken one is replaced), nothing the
    // leader holds for another member (GuildCoordinator::IsHeld), bags and quivers by size. `slot`: the equipment
    // slot (the weakest of the group's slots), NULL_SLOT for a bag (the equip action picks the bag slot); `gain` ranks
    // several yes answers; `decision`: the leader's text for HistoryNoteDecision. Map thread (the bot's own).
    bool ShouldEquip(Player* bot, Item* item, uint8& slot, float& gain, std::string& decision);
    // whether the leader lets the bot buy this gear at the vendor it stands at (the same verdict, not owned yet)
    bool ShouldBuy(Player* bot, ItemTemplate const* proto, std::string& decision);
}

#endif
