/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TESTHARNESSDATA_H
#define PLAYERBOTS_TESTHARNESSDATA_H

#include "Define.h"
#include "LeaderKnowledge.h"  // [lead35] the dungeon knowledge the leader reads (Leader::Knowledge)

#include <string>
#include <utility>
#include <vector>

// Static tables of the headless dungeon / raid test harness: specs, group compositions, content (map,
// difficulty, item level), consumables per role, profession pairs and event starters.
namespace TestHarness
{
    using namespace Leader::Knowledge;  // [lead35] the harness reads the same knowledge

    enum class Role : uint8
    {
        Tank,
        Healer,
        Melee,
        Ranged,
        Caster
    };

    constexpr uint8 MAX_TEST_LEVEL = 80;

    char const* RoleName(Role role);
    bool UsesMana(Role role, uint8 cls);

    enum Capability : uint32
    {
        CAP_HEROISM = 0x00001,
        CAP_REPLENISHMENT = 0x00002,
        CAP_BLESSING = 0x00004,  // one paladin = one of Kings / Might / Wisdom
        CAP_FORTITUDE = 0x00008,  // Fortitude, Divine Spirit, Shadow Protection
        CAP_MARK_OF_THE_WILD = 0x00010,
        CAP_ARCANE_INTELLECT = 0x00020,
        CAP_MAJOR_ARMOR = 0x00040,  // Sunder Armor / Expose Armor
        CAP_MINOR_ARMOR = 0x00080,  // Faerie Fire / Curse of Weakness
        CAP_SPELL_DAMAGE_TAKEN = 0x00100,  // Curse of the Elements / Ebon Plague / Earth and Moon
        CAP_CRIT_TAKEN = 0x00200,  // Heart of the Crusader / Totem of Wrath / Master Poisoner
        CAP_SPELL_HIT = 0x00400,  // Misery / Improved Faerie Fire
        CAP_BATTLE_RES = 0x00800,
        CAP_INTERRUPT = 0x01000,
        CAP_DISPEL_MAGIC = 0x02000,
        CAP_DISPEL_CURSE = 0x04000,
        CAP_DISPEL_POISON = 0x08000,
        CAP_DISPEL_DISEASE = 0x10000
    };

    struct SpecDef
    {
        char const* key;      // "prot-paladin"
        uint8 cls;
        char const* premade;  // Playerbots.PremadeSpecName.<class>.<n>
        Role role;
        char const* label;    // "Prot Paladin"
        uint32 caps;
    };

    enum SlotFlags : uint8
    {
        SLOT_MAIN_TANK = 0x01,
        SLOT_OFF_TANK = 0x02
    };

    struct CompSlot
    {
        char const* spec;
        uint8 subgroup;  // raid group 1..5, 1 for a party
        uint8 flags;
    };

    struct Composition
    {
        char const* id;  // "5A", "10B", "25A"
        uint8 size;
        bool optimal;
        char const* note;
        std::vector<CompSlot> slots;
        // A Horde group (Horde races, the Horde pool: 'playerbots test setup <n> [leveling] horde'). Not in
        // CompositionsOfSize (the variants), only by its id and HORDE_RUN_CONTENTS in runall.
        bool horde = false;
    };

    struct ContentDef
    {
        char const* key;   // "uk-h", "naxx25", "kara-h", "sm-gy-n"
        uint32 mapId;
        uint8 difficulty;  // Difficulty value of the map (dungeon 0/1, raid 0..3)
        uint8 size;        // 5, 10 or 25
        uint16 ilvl;       // item level of the content; the test group wears ilvl - 10 on average
        bool raid;
        char const* name;
        char const* tier;  // "classic-n", "tbc-n", "wotlk-n", "classic-h", "tbc-h", "wotlk-h", "*-raid"
        uint8 level = 80;  // level the test group is prepared to (normal dungeons: median elite mob level)
        // Wing of a map with several entrances (Scarlet Monastery, Dire Maul, Blackrock Spire): only these
        // areatrigger_teleport ids are used as entrances, and the run is done when these boss entries are dead.
        // Empty: every entrance of the map, every boss of the instance.
        std::vector<uint32> entrances = {};
        std::vector<uint32> bossEntries = {};
        // [world_boss] an open-world boss (docs/WORLD_BOSS_PLAN.md): no instance; the group is ported to a stand-off
        // spot near the boss's live copy on mapId (LoadEntrances) and the run is done when it dies (bossEntries)
        uint32 worldBoss = 0;
    };

    inline bool IsNormalDungeon(ContentDef const& content) { return !content.raid && content.difficulty == 0; }

    struct RoleConsumables
    {
        uint32 flask;    // 0 below level 50 (no flask of that role exists yet)
        uint32 food;     // "Well Fed" buff food of the level
        uint32 prePot;   // 0 when no potion of that role exists at the level
    };

    // Plain supplies of the level (healing / mana potion, water, regen food), all vendor or common items.
    struct LevelSupplies
    {
        uint32 healingPotion;
        uint32 manaPotion;
        uint32 drink;
        uint32 regenFood;
    };

    // Item ids, verified against acore_world.item_template.
    constexpr uint32 ITEM_FLASK_ENDLESS_RAGE = 46377;
    constexpr uint32 ITEM_FLASK_FROST_WYRM = 46376;
    constexpr uint32 ITEM_FLASK_STONEBLOOD = 46379;
    constexpr uint32 ITEM_FLASK_PURE_MOJO = 46378;
    constexpr uint32 ITEM_FISH_FEAST = 43015;
    constexpr uint32 ITEM_DRAGONFIN_FILET = 43000;       // strength fish
    constexpr uint32 ITEM_BLACKENED_DRAGONFIN = 42999;   // agility fish
    constexpr uint32 ITEM_FIRECRACKER_SALMON = 34767;    // spell power fish
    constexpr uint32 ITEM_IMPERIAL_MANTA_STEAK = 34769;  // haste fish
    constexpr uint32 ITEM_POTION_OF_SPEED = 40211;
    constexpr uint32 ITEM_POTION_OF_WILD_MAGIC = 40212;
    constexpr uint32 ITEM_INDESTRUCTIBLE_POTION = 40093;
    constexpr uint32 ITEM_RUNIC_HEALING_POTION = 33447;
    constexpr uint32 ITEM_RUNIC_MANA_POTION = 33448;
    constexpr uint32 ITEM_FEL_HEALTHSTONE = 36892;
    constexpr uint32 ITEM_HONEYMINT_TEA = 33445;
    constexpr uint32 ITEM_DALARAN_CLAM_CHOWDER = 43268;  // plain regen food

    constexpr uint32 FLASK_COUNT = 6;
    constexpr uint32 FISH_COUNT = 20;
    constexpr uint32 PREPOT_COUNT = 12;
    constexpr uint32 HEALING_POTION_COUNT = 10;
    constexpr uint32 MANA_POTION_COUNT = 10;
    // [b33_pos] 90-minute runs: the heaviest drinkers (hunters, healers) use 0.4-0.58 a minute (brd-h 5H 107 min:
    // hunter 60 -> 9), i.e. ~52 per 90 min; 80 leaves a margin. Harness grant at run start, replaces farming.
    constexpr uint32 DRINK_COUNT = 80;
    constexpr uint32 REGEN_FOOD_COUNT = 40;
    // [rot110] a hunter pet's food (TestHarnessPrepare PetFoodFor): Pet::GetCurrentFoodBenefitLevel's full feed, and
    // two stacks for a run (the pet is fed out of combat whenever it is not Happy)
    constexpr uint32 PET_FOOD_FULL_BENEFIT = 35000;
    constexpr uint32 PET_FOOD_COUNT = 40;

    std::vector<SpecDef> const& Specs();
    SpecDef const* FindSpec(std::string const& key);

    std::vector<Composition> const& Compositions();
    Composition const* FindComposition(std::string const& id);
    Composition const* OptimalComposition(uint8 size);
    std::vector<Composition const*> CompositionsOfSize(uint8 size);

    std::vector<ContentDef> const& Contents();
    ContentDef const* FindContent(std::string const& key);

    // [hleft-run-budget] the run time limit of a content in minutes, 0: the default (dungeon 90, raid)
    uint32 RunBudgetMinutes(char const* contentKey);

    // Consumables of the role at the character's level (level 80: the WotLK raid set above).
    RoleConsumables ConsumablesFor(SpecDef const& spec, uint8 level);
    LevelSupplies SuppliesFor(uint8 level);

    // Two primary professions for the n-th member of that role (skill ids).
    std::pair<uint16, uint16> ProfessionPairFor(Role role, uint32 indexInRole);
    char const* ProfessionName(uint16 skillId);
    // Primary profession skill a levelling player has at that level: level * 5, 450 at level 80.
    uint16 ProfessionSkillFor(uint8 level);
    // Average item level of a levelling character (quest greens, dungeon blues): level + 4 up to 60,
    // 100..115 in Outland, 150..176 in Northrend, 200 at 80 (the "test prepare" default).
    uint32 DefaultTargetIlvl(uint8 level);
    // Rough single-target DPS of one member (tools/dps_check.py curves: LEVEL_DPS below 80, REF_DPS by item level
    // at 80, size factor, role share). Only the suspicious-kill check uses it: a sanity bound, not a benchmark.
    float ExpectedMemberDps(Role role, uint8 level, float avgIlvl, uint8 size);

    // Holes in the raid-buff coverage of a composition, one line each ("heroism: none").
    std::vector<std::string> CoverageGaps(Composition const& comp);

    // Race used to create a test character of that class (all Alliance, so every class fits one group).
    uint8 RaceForClass(uint8 cls, bool horde = false);
    // Contents runall also queues with the Horde composition HORDE_COMPOSITION (user 2026-10-25: Uldaman with a Horde
    // group, the Lost Dwarves are fought; for an Alliance group they are n/a).
    extern char const* const HORDE_COMPOSITION;
    bool HordeRunContent(char const* key);
    char const* ClassName(uint8 cls);
}

#endif
