/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TestHarnessData.h"

#include "SharedDefines.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace TestHarness
{
    namespace
    {
        constexpr uint32 CAPS_PALADIN = CAP_BLESSING | CAP_DISPEL_MAGIC | CAP_DISPEL_POISON | CAP_DISPEL_DISEASE;
        constexpr uint32 CAPS_PRIEST = CAP_FORTITUDE | CAP_DISPEL_MAGIC | CAP_DISPEL_DISEASE;
        constexpr uint32 CAPS_SHAMAN = CAP_HEROISM | CAP_INTERRUPT | CAP_DISPEL_POISON | CAP_DISPEL_DISEASE;
        constexpr uint32 CAPS_MAGE = CAP_ARCANE_INTELLECT | CAP_INTERRUPT | CAP_DISPEL_CURSE;
        constexpr uint32 CAPS_WARLOCK = CAP_SPELL_DAMAGE_TAKEN | CAP_MINOR_ARMOR;
        constexpr uint32 CAPS_DRUID = CAP_MARK_OF_THE_WILD | CAP_BATTLE_RES | CAP_DISPEL_CURSE | CAP_DISPEL_POISON;
        constexpr uint32 CAPS_DK = CAP_INTERRUPT | CAP_BATTLE_RES;
        constexpr uint32 CAPS_ROGUE = CAP_INTERRUPT | CAP_MAJOR_ARMOR;
        constexpr uint32 CAPS_WARRIOR = CAP_MAJOR_ARMOR | CAP_INTERRUPT;

        std::vector<SpecDef> const specs = {
            {"arms-warrior", CLASS_WARRIOR, "arms pve", Role::Melee, "Arms Warrior", CAPS_WARRIOR},
            {"fury-warrior", CLASS_WARRIOR, "fury pve", Role::Melee, "Fury Warrior", CAPS_WARRIOR},
            {"prot-warrior", CLASS_WARRIOR, "prot pve", Role::Tank, "Prot Warrior", CAPS_WARRIOR},
            {"holy-paladin", CLASS_PALADIN, "holy pve", Role::Healer, "Holy Paladin", CAPS_PALADIN},
            {"prot-paladin", CLASS_PALADIN, "prot pve", Role::Tank, "Prot Paladin", CAPS_PALADIN},
            {"ret-paladin", CLASS_PALADIN, "ret pve", Role::Melee, "Retribution Paladin",
             CAPS_PALADIN | CAP_REPLENISHMENT | CAP_CRIT_TAKEN},
            {"bm-hunter", CLASS_HUNTER, "bm pve", Role::Ranged, "Beast Mastery Hunter", 0},
            {"mm-hunter", CLASS_HUNTER, "mm pve", Role::Ranged, "Marksmanship Hunter", CAP_INTERRUPT},
            {"surv-hunter", CLASS_HUNTER, "surv pve", Role::Ranged, "Survival Hunter", CAP_REPLENISHMENT},
            {"assa-rogue", CLASS_ROGUE, "as pve", Role::Melee, "Assassination Rogue", CAPS_ROGUE | CAP_CRIT_TAKEN},
            {"combat-rogue", CLASS_ROGUE, "combat pve", Role::Melee, "Combat Rogue", CAPS_ROGUE},
            // [harness10] user: specs nobody plays in PvE (Subtlety Rogue, PvP-only in 3.3.5) are kept out of the
            // compositions; the entry stays for a manual test
            {"sub-rogue", CLASS_ROGUE, "subtlety pve", Role::Melee, "Subtlety Rogue", CAPS_ROGUE},
            {"disc-priest", CLASS_PRIEST, "disc pve", Role::Healer, "Discipline Priest", CAPS_PRIEST},
            {"holy-priest", CLASS_PRIEST, "holy pve", Role::Healer, "Holy Priest", CAPS_PRIEST},
            {"shadow-priest", CLASS_PRIEST, "shadow pve", Role::Caster, "Shadow Priest",
             CAPS_PRIEST | CAP_REPLENISHMENT | CAP_SPELL_HIT | CAP_INTERRUPT},
            {"blood-dk", CLASS_DEATH_KNIGHT, "blood pve", Role::Tank, "Blood DK", CAPS_DK},
            {"frost-dk", CLASS_DEATH_KNIGHT, "frost pve", Role::Melee, "Frost DK", CAPS_DK},
            {"unholy-dk", CLASS_DEATH_KNIGHT, "unholy pve", Role::Melee, "Unholy DK", CAPS_DK | CAP_SPELL_DAMAGE_TAKEN},
            {"ele-shaman", CLASS_SHAMAN, "ele pve", Role::Caster, "Elemental Shaman", CAPS_SHAMAN | CAP_CRIT_TAKEN},
            {"enh-shaman", CLASS_SHAMAN, "enh pve", Role::Melee, "Enhancement Shaman", CAPS_SHAMAN},
            {"resto-shaman", CLASS_SHAMAN, "resto pve", Role::Healer, "Restoration Shaman",
             CAPS_SHAMAN | CAP_DISPEL_CURSE},
            {"arcane-mage", CLASS_MAGE, "arcane pve", Role::Caster, "Arcane Mage", CAPS_MAGE},
            {"fire-mage", CLASS_MAGE, "fire pve", Role::Caster, "Fire Mage", CAPS_MAGE},
            {"frost-mage", CLASS_MAGE, "frost pve", Role::Caster, "Frost Mage", CAPS_MAGE | CAP_REPLENISHMENT},
            {"affli-warlock", CLASS_WARLOCK, "affli pve", Role::Caster, "Affliction Warlock", CAPS_WARLOCK},
            {"demo-warlock", CLASS_WARLOCK, "demo pve", Role::Caster, "Demonology Warlock", CAPS_WARLOCK},
            {"destro-warlock", CLASS_WARLOCK, "destro pve", Role::Caster, "Destruction Warlock",
             CAPS_WARLOCK | CAP_REPLENISHMENT},
            {"balance-druid", CLASS_DRUID, "balance pve", Role::Caster, "Balance Druid",
             CAPS_DRUID | CAP_SPELL_DAMAGE_TAKEN | CAP_SPELL_HIT | CAP_MINOR_ARMOR},
            {"cat-druid", CLASS_DRUID, "cat pve", Role::Melee, "Feral Druid (cat)", CAPS_DRUID | CAP_MINOR_ARMOR},
            {"bear-druid", CLASS_DRUID, "bear pve", Role::Tank, "Feral Druid (bear)", CAPS_DRUID | CAP_MINOR_ARMOR},
            {"resto-druid", CLASS_DRUID, "resto pve", Role::Healer, "Restoration Druid", CAPS_DRUID},
        };

        constexpr uint8 MT = SLOT_MAIN_TANK;
        constexpr uint8 OT = SLOT_OFF_TANK;

        // 5A, 10A and 25A are the user's optimal compositions; the other variants rotate every spec through, and
        // every tank and healer spec through its role.
        std::vector<Composition> const compositions = {
            {"5A", 5, true, "optimal",
             {{"prot-paladin", 1, MT}, {"resto-druid", 1, 0}, {"frost-mage", 1, 0}, {"mm-hunter", 1, 0},
              {"enh-shaman", 1, 0}}},
            {"5B", 5, false, "rotation",
             {{"prot-warrior", 1, MT}, {"holy-paladin", 1, 0}, {"shadow-priest", 1, 0}, {"combat-rogue", 1, 0},
              {"ele-shaman", 1, 0}}},
            {"5C", 5, false, "rotation",
             {{"blood-dk", 1, MT}, {"disc-priest", 1, 0}, {"fire-mage", 1, 0}, {"fury-warrior", 1, 0},
              {"balance-druid", 1, 0}}},
            {"5D", 5, false, "rotation",
             {{"bear-druid", 1, MT}, {"resto-shaman", 1, 0}, {"arcane-mage", 1, 0}, {"affli-warlock", 1, 0},
              {"unholy-dk", 1, 0}}},
            {"5E", 5, false, "rotation",
             {{"prot-paladin", 1, MT}, {"holy-priest", 1, 0}, {"destro-warlock", 1, 0}, {"bm-hunter", 1, 0},
              {"assa-rogue", 1, 0}}},
            {"5F", 5, false, "rotation",
             {{"prot-warrior", 1, MT}, {"resto-druid", 1, 0}, {"demo-warlock", 1, 0}, {"surv-hunter", 1, 0},
              {"combat-rogue", 1, 0}}},
            {"5G", 5, false, "rotation",
             {{"blood-dk", 1, MT}, {"holy-paladin", 1, 0}, {"ret-paladin", 1, 0}, {"arms-warrior", 1, 0},
              {"frost-dk", 1, 0}}},
            {"5H", 5, false, "rotation",
             {{"bear-druid", 1, MT}, {"disc-priest", 1, 0}, {"cat-druid", 1, 0}, {"fire-mage", 1, 0},
              {"mm-hunter", 1, 0}}},
            // [lead53] 5A with a protection warrior tank (the user's threat tests)
            {"5W", 5, false, "5A with a prot warrior",
             {{"prot-warrior", 1, MT}, {"resto-druid", 1, 0}, {"frost-mage", 1, 0}, {"mm-hunter", 1, 0},
              {"enh-shaman", 1, 0}}},
            // 5A with Horde races (Blood Elf paladin, Tauren druid, Undead mage, Orc hunter and shaman).
            {"5AH", 5, false, "optimal, Horde",
             {{"prot-paladin", 1, MT}, {"resto-druid", 1, 0}, {"frost-mage", 1, 0}, {"mm-hunter", 1, 0},
              {"enh-shaman", 1, 0}},
             true},

            {"10A", 10, true, "optimal",
             {{"prot-paladin", 1, MT}, {"blood-dk", 1, OT}, {"holy-paladin", 1, 0}, {"disc-priest", 1, 0},
              {"resto-druid", 1, 0}, {"ele-shaman", 2, 0}, {"arcane-mage", 2, 0}, {"affli-warlock", 2, 0},
              {"fury-warrior", 2, 0}, {"combat-rogue", 2, 0}}},
            {"10B", 10, false, "rotation",
             {{"prot-warrior", 1, MT}, {"bear-druid", 1, OT}, {"holy-priest", 1, 0}, {"resto-shaman", 1, 0},
              {"holy-paladin", 1, 0}, {"ret-paladin", 2, 0}, {"shadow-priest", 2, 0}, {"frost-mage", 2, 0},
              {"destro-warlock", 2, 0}, {"mm-hunter", 2, 0}}},
            {"10C", 10, false, "rotation",
             {{"blood-dk", 1, MT}, {"prot-paladin", 1, OT}, {"resto-druid", 1, 0}, {"disc-priest", 1, 0},
              {"resto-shaman", 1, 0}, {"enh-shaman", 2, 0}, {"balance-druid", 2, 0}, {"fire-mage", 2, 0},
              {"demo-warlock", 2, 0}, {"assa-rogue", 2, 0}}},
            {"10D", 10, false, "rotation",
             {{"prot-warrior", 1, MT}, {"bear-druid", 1, OT}, {"holy-paladin", 1, 0}, {"holy-priest", 1, 0},
              {"resto-druid", 1, 0}, {"arms-warrior", 2, 0}, {"cat-druid", 2, 0}, {"bm-hunter", 2, 0},
              {"surv-hunter", 2, 0}, {"unholy-dk", 2, 0}}},
            {"10E", 10, false, "rotation",
             {{"prot-paladin", 1, MT}, {"blood-dk", 1, OT}, {"holy-paladin", 1, 0}, {"disc-priest", 1, 0},
              {"resto-shaman", 1, 0}, {"frost-dk", 2, 0}, {"combat-rogue", 2, 0}, {"ele-shaman", 2, 0},
              {"arcane-mage", 2, 0}, {"affli-warlock", 2, 0}}},

            {"25A", 25, true, "optimal",
             {{"prot-paladin", 1, MT}, {"prot-warrior", 1, OT}, {"holy-paladin", 1, 0}, {"holy-paladin", 1, 0},
              {"disc-priest", 1, 0},
              {"enh-shaman", 2, 0}, {"fury-warrior", 2, 0}, {"arms-warrior", 2, 0}, {"cat-druid", 2, 0},
              {"combat-rogue", 2, 0},
              {"ele-shaman", 3, 0}, {"balance-druid", 3, 0}, {"arcane-mage", 3, 0}, {"fire-mage", 3, 0},
              {"shadow-priest", 3, 0},
              {"resto-shaman", 4, 0}, {"resto-druid", 4, 0}, {"holy-priest", 4, 0}, {"ret-paladin", 4, 0},
              {"demo-warlock", 4, 0},
              {"mm-hunter", 5, 0}, {"surv-hunter", 5, 0}, {"unholy-dk", 5, 0}, {"frost-dk", 5, 0},
              {"affli-warlock", 5, 0}}},
            {"25B", 25, false, "rotation",
             {{"blood-dk", 1, MT}, {"bear-druid", 1, OT}, {"holy-paladin", 1, 0}, {"holy-paladin", 1, 0},
              {"disc-priest", 1, 0},
              {"enh-shaman", 2, 0}, {"fury-warrior", 2, 0}, {"arms-warrior", 2, 0}, {"assa-rogue", 2, 0},
              {"combat-rogue", 2, 0},
              {"ele-shaman", 3, 0}, {"balance-druid", 3, 0}, {"frost-mage", 3, 0}, {"arcane-mage", 3, 0},
              {"shadow-priest", 3, 0},
              {"resto-shaman", 4, 0}, {"resto-druid", 4, 0}, {"holy-priest", 4, 0}, {"ret-paladin", 4, 0},
              {"destro-warlock", 4, 0},
              {"bm-hunter", 5, 0}, {"mm-hunter", 5, 0}, {"unholy-dk", 5, 0}, {"frost-dk", 5, 0},
              {"affli-warlock", 5, 0}}},
        };

        // Normal 5-man dungeons at the level of their mobs (mod-raid-rescale leaves difficulty 0 untouched).
        // level: median level of the dungeon's elite / boss spawns of normal mode (acore_world creature x
        // creature_template, ranks 1-3, per wing by the nearest boss), at least 15 and at least the
        // dungeon_access_template / LFGDungeons.dbc minimum. Event dungeons with script-spawned mobs (Black Morass,
        // Trial of the Champion) take the LFGDungeons.dbc target level.
        // ilvl = gear target + 10 (target: classic boss loot - 10, 100..115 in TBC, 150..180 in WotLK; Trial of the
        // Champion and the Icecrown halls need an average item level of 180 to enter, their target is 195).
        // User rule (2026-10-24): prepared gear is always below what the dungeon drops - ilvl is the real boss-loot
        // item level (acore_world creature_loot_template of the bosses) where it was measured: DM West / North and
        // Stratholme 62, UBRS 63, Ramparts / Blood Furnace 85, Slave Pens 91, Underbog 94, Mana-Tombs 97, Auchenai
        // Crypts / Old Hillsbrad 100, Sethekk ~105 (its loot is required level 68), Nexus 159, Azjol-Nerub 163,
        // Ahn'kahet 166, Gundrak / Halls of Stone ~180. The invented curve above made the target unreachable (the
        // gear filters have nothing at it) and the groups came out below it. Utgarde Keep at 70: 125 for a target of
        // 115, fresh from Outland (its own loot, ~159, needs Northrend gear the level-70 group does not wear yet).
        // Classic normals (gear agent 2026-10-24): median item level of the bosses' blue / epic loot, references
        // followed (acore_world creature_loot_template + reference_loot_template; wings by their bossEntries, the
        // other maps by their rank-3 / instance_encounters creatures). The old level + 17 curve (Scholomance 76,
        // Zul'Farrak 62) put the target at level + 7, above anything wearable at that level (blues and greens of
        // RequiredLevel <= level stop at about level + 5, rings / neck / trinkets lower), and the groups came out
        // 4-5 below it: Scholomance 61.0 for 66, Razorfen Kraul 28.3 for 33.
        // run_start logs "gear_above_content" when target or group average exceed ilvl - 10 (TestHarnessReport).
        std::vector<ContentDef> const contents = {
            // Classic normals
            {"rfc-n", 389, 0, 5, 18, false, "Ragefire Chasm", "classic-n", 15},
            {"wc-n", 43, 0, 5, 23, false, "Wailing Caverns", "classic-n", 19},
            {"dm-n", 36, 0, 5, 22, false, "The Deadmines", "classic-n", 19},
            {"sfk-n", 33, 0, 5, 26, false, "Shadowfang Keep", "classic-n", 19},
            {"bfd-n", 48, 0, 5, 25, false, "Blackfathom Deeps", "classic-n", 23},
            {"stocks-n", 34, 0, 5, 29, false, "The Stockade", "classic-n", 24},
            {"rfk-n", 47, 0, 5, 32, false, "Razorfen Kraul", "classic-n", 26},
            {"gnomer-n", 90, 0, 5, 29, false, "Gnomeregan", "classic-n", 27},
            {"sm-gy-n", 189, 0, 5, 34, false, "Scarlet Monastery - Graveyard", "classic-n", 31, {45},
             {3983, 4543}},
            {"sm-lib-n", 189, 0, 5, 38, false, "Scarlet Monastery - Library", "classic-n", 34, {614}, {3974, 6487}},
            {"sm-arm-n", 189, 0, 5, 42, false, "Scarlet Monastery - Armory", "classic-n", 36, {612}, {3975}},
            {"sm-cath-n", 189, 0, 5, 44, false, "Scarlet Monastery - Cathedral", "classic-n", 39, {610},
             {4542, 3976, 3977}},
            {"rfd-n", 129, 0, 5, 38, false, "Razorfen Downs", "classic-n", 36},
            {"ulda-n", 70, 0, 5, 41, false, "Uldaman", "classic-n", 39},
            {"zf-n", 209, 0, 5, 47, false, "Zul'Farrak", "classic-n", 45},
            {"mara-n", 349, 0, 5, 53, false, "Maraudon", "classic-n", 45},
            {"st-n", 109, 0, 5, 54, false, "Sunken Temple", "classic-n", 48},
            {"brd-n", 230, 0, 5, 55, false, "Blackrock Depths", "classic-n", 51},
            {"dme-n", 429, 0, 5, 59, false, "Dire Maul - East", "classic-n", 56, {3183, 3184, 3185},
             {11490, 13280, 14327, 11492}},
            {"lbrs-n", 229, 0, 5, 60, false, "Lower Blackrock Spire", "classic-n", 57, {1468},
             {9196, 9236, 9237, 10596, 10584, 9736, 10220, 10268, 9568}},
            {"dmw-n", 429, 0, 5, 62, false, "Dire Maul - West", "classic-n", 58, {3186, 3187},
             {11489, 11488, 11487, 11496, 11486}},
            {"dmn-n", 429, 0, 5, 62, false, "Dire Maul - North", "classic-n", 58, {3189},
             {14326, 14322, 14321, 14323, 14325, 14324, 11501}},
            {"strat-n", 329, 0, 5, 62, false, "Stratholme", "classic-n", 58},
            {"scholo-n", 289, 0, 5, 61, false, "Scholomance", "classic-n", 59},
            // Upper Blackrock Spire is a 10-player dungeon
            {"ubrs-n", 229, 0, 10, 63, false, "Upper Blackrock Spire", "classic-n", 60, {1468},
             {9816, 10264, 10429, 10430, 10363}},
            // TBC normals
            {"ramps-n", 543, 0, 5, 85, false, "Hellfire Ramparts", "tbc-n", 61},
            {"bf-n", 542, 0, 5, 85, false, "The Blood Furnace", "tbc-n", 61},
            {"sp-n", 547, 0, 5, 91, false, "The Slave Pens", "tbc-n", 63},
            {"ub-n", 546, 0, 5, 94, false, "The Underbog", "tbc-n", 63},
            {"mt-n", 557, 0, 5, 97, false, "Mana-Tombs", "tbc-n", 65},
            {"ac-n", 558, 0, 5, 100, false, "Auchenai Crypts", "tbc-n", 66},
            {"ohb-n", 560, 0, 5, 100, false, "Old Hillsbrad Foothills", "tbc-n", 66},
            {"seth-n", 556, 0, 5, 105, false, "Sethekk Halls", "tbc-n", 67},
            {"sv-n", 545, 0, 5, 125, false, "The Steamvault", "tbc-n", 70},
            {"mech-n", 554, 0, 5, 125, false, "The Mechanar", "tbc-n", 70},
            {"bot-n", 553, 0, 5, 125, false, "The Botanica", "tbc-n", 70},
            {"sh-n", 540, 0, 5, 125, false, "The Shattered Halls", "tbc-n", 70},
            {"slabs-n", 555, 0, 5, 125, false, "Shadow Labyrinth", "tbc-n", 70},
            {"bm-n", 269, 0, 5, 125, false, "The Black Morass", "tbc-n", 70},
            {"arc-n", 552, 0, 5, 125, false, "The Arcatraz", "tbc-n", 70},
            {"mgt-n", 585, 0, 5, 125, false, "Magisters' Terrace", "tbc-n", 70},
            // WotLK normals (level 70: no Northrend gear yet, LimitGearExpansion)
            {"uk-n", 574, 0, 5, 125, false, "Utgarde Keep", "wotlk-n", 70},
            {"nex-n", 576, 0, 5, 159, false, "The Nexus", "wotlk-n", 71},
            {"an-n", 601, 0, 5, 163, false, "Azjol-Nerub", "wotlk-n", 72},
            {"ok-n", 619, 0, 5, 166, false, "Ahn'kahet: The Old Kingdom", "wotlk-n", 73},
            {"dtk-n", 600, 0, 5, 176, false, "Drak'Tharon Keep", "wotlk-n", 76},
            {"vh-n", 608, 0, 5, 176, false, "The Violet Hold", "wotlk-n", 76},
            {"gd-n", 604, 0, 5, 180, false, "Gundrak", "wotlk-n", 77},
            {"hos-n", 599, 0, 5, 180, false, "Halls of Stone", "wotlk-n", 77},
            {"occ-n", 578, 0, 5, 186, false, "The Oculus", "wotlk-n", 79},
            {"hol-n", 602, 0, 5, 190, false, "Halls of Lightning", "wotlk-n", 80},
            {"cos-n", 595, 0, 5, 190, false, "The Culling of Stratholme", "wotlk-n", 80},
            {"up-n", 575, 0, 5, 190, false, "Utgarde Pinnacle", "wotlk-n", 80},
            {"toc5-n", 650, 0, 5, 205, false, "Trial of the Champion", "wotlk-n", 80},
            {"fos-n", 632, 0, 5, 205, false, "The Forge of Souls", "wotlk-n", 80},
            {"pos-n", 658, 0, 5, 205, false, "Pit of Saron", "wotlk-n", 80},
            {"hor-n", 668, 0, 5, 205, false, "Halls of Reflection", "wotlk-n", 80},
            // Classic heroics (mod-raid-rescale, PLAN.md section 12)
            {"rfc-h", 389, 1, 5, 200, false, "Ragefire Chasm", "classic-h"},
            {"wc-h", 43, 1, 5, 200, false, "Wailing Caverns", "classic-h"},
            {"dm-h", 36, 1, 5, 200, false, "The Deadmines", "classic-h"},
            {"sfk-h", 33, 1, 5, 200, false, "Shadowfang Keep", "classic-h"},
            {"bfd-h", 48, 1, 5, 200, false, "Blackfathom Deeps", "classic-h"},
            {"stocks-h", 34, 1, 5, 200, false, "The Stockade", "classic-h"},
            {"gnomer-h", 90, 1, 5, 200, false, "Gnomeregan", "classic-h"},
            {"rfk-h", 47, 1, 5, 200, false, "Razorfen Kraul", "classic-h"},
            {"sm-h", 189, 1, 5, 213, false, "Scarlet Monastery", "classic-h"},
            {"rfd-h", 129, 1, 5, 213, false, "Razorfen Downs", "classic-h"},
            {"ulda-h", 70, 1, 5, 213, false, "Uldaman", "classic-h"},
            {"zf-h", 209, 1, 5, 213, false, "Zul'Farrak", "classic-h"},
            {"mara-h", 349, 1, 5, 226, false, "Maraudon", "classic-h"},
            {"st-h", 109, 1, 5, 226, false, "Sunken Temple", "classic-h"},
            {"brd-h", 230, 1, 5, 226, false, "Blackrock Depths", "classic-h"},
            {"brs-h", 229, 1, 5, 239, false, "Blackrock Spire", "classic-h"},
            {"dme-h", 429, 1, 5, 239, false, "Dire Maul", "classic-h"},
            {"scholo-h", 289, 1, 5, 245, false, "Scholomance", "classic-h"},
            {"strat-h", 329, 1, 5, 245, false, "Stratholme", "classic-h"},
            // TBC heroics
            {"ramps-h", 543, 1, 5, 200, false, "Hellfire Ramparts", "tbc-h"},
            {"bf-h", 542, 1, 5, 200, false, "The Blood Furnace", "tbc-h"},
            {"sp-h", 547, 1, 5, 213, false, "The Slave Pens", "tbc-h"},
            {"ub-h", 546, 1, 5, 213, false, "The Underbog", "tbc-h"},
            {"mt-h", 557, 1, 5, 213, false, "Mana-Tombs", "tbc-h"},
            {"ac-h", 558, 1, 5, 226, false, "Auchenai Crypts", "tbc-h"},
            {"seth-h", 556, 1, 5, 226, false, "Sethekk Halls", "tbc-h"},
            {"ohb-h", 560, 1, 5, 226, false, "Old Hillsbrad Foothills", "tbc-h"},
            {"sv-h", 545, 1, 5, 239, false, "The Steamvault", "tbc-h"},
            {"mech-h", 554, 1, 5, 239, false, "The Mechanar", "tbc-h"},
            {"bot-h", 553, 1, 5, 239, false, "The Botanica", "tbc-h"},
            {"sh-h", 540, 1, 5, 239, false, "The Shattered Halls", "tbc-h"},
            {"slabs-h", 555, 1, 5, 245, false, "Shadow Labyrinth", "tbc-h"},
            {"bm-h", 269, 1, 5, 245, false, "The Black Morass", "tbc-h"},
            {"arc-h", 552, 1, 5, 245, false, "The Arcatraz", "tbc-h"},
            {"mgt-h", 585, 1, 5, 251, false, "Magisters' Terrace", "tbc-h"},
            // WotLK heroics (unchanged item levels)
            {"uk-h", 574, 1, 5, 200, false, "Utgarde Keep", "wotlk-h"},
            {"nex-h", 576, 1, 5, 200, false, "The Nexus", "wotlk-h"},
            {"an-h", 601, 1, 5, 200, false, "Azjol-Nerub", "wotlk-h"},
            {"ok-h", 619, 1, 5, 200, false, "Ahn'kahet: The Old Kingdom", "wotlk-h"},
            {"dtk-h", 600, 1, 5, 200, false, "Drak'Tharon Keep", "wotlk-h"},
            {"vh-h", 608, 1, 5, 200, false, "The Violet Hold", "wotlk-h"},
            {"gd-h", 604, 1, 5, 200, false, "Gundrak", "wotlk-h"},
            {"hos-h", 599, 1, 5, 200, false, "Halls of Stone", "wotlk-h"},
            {"hol-h", 602, 1, 5, 200, false, "Halls of Lightning", "wotlk-h"},
            {"cos-h", 595, 1, 5, 200, false, "The Culling of Stratholme", "wotlk-h"},
            {"up-h", 575, 1, 5, 200, false, "Utgarde Pinnacle", "wotlk-h"},
            {"occ-h", 578, 1, 5, 200, false, "The Oculus", "wotlk-h"},
            {"toc5-h", 650, 1, 5, 219, false, "Trial of the Champion", "wotlk-h"},
            {"fos-h", 632, 1, 5, 232, false, "The Forge of Souls", "wotlk-h"},
            {"pos-h", 658, 1, 5, 232, false, "Pit of Saron", "wotlk-h"},
            {"hor-h", 668, 1, 5, 232, false, "Halls of Reflection", "wotlk-h"},
            // Classic raids (rescaled). [ladder_v2] ilvl = RAID_LADDER_V2.md; ZG / AQ20 also 25N / 25H
            {"zg-n", 309, 0, 10, 219, true, "Zul'Gurub", "classic-raid"},
            {"zg-h", 309, 2, 10, 251, true, "Zul'Gurub (Hard)", "classic-raid"},
            {"zg25-n", 309, 1, 25, 237, true, "Zul'Gurub 25", "classic-raid"},
            {"zg25-h", 309, 3, 25, 269, true, "Zul'Gurub 25 (Hard)", "classic-raid"},
            {"aq20-n", 509, 0, 10, 219, true, "Ruins of Ahn'Qiraj", "classic-raid"},
            {"aq20-h", 509, 2, 10, 253, true, "Ruins of Ahn'Qiraj (Hard)", "classic-raid"},
            {"aq20-25-n", 509, 1, 25, 237, true, "Ruins of Ahn'Qiraj 25", "classic-raid"},
            {"aq20-25-h", 509, 3, 25, 271, true, "Ruins of Ahn'Qiraj 25 (Hard)", "classic-raid"},
            {"mc-n", 409, 0, 25, 226, true, "Molten Core", "classic-raid"},
            {"mc-h", 409, 3, 25, 256, true, "Molten Core (Hard)", "classic-raid"},
            {"bwl-n", 469, 0, 25, 232, true, "Blackwing Lair", "classic-raid"},
            {"bwl-h", 469, 3, 25, 265, true, "Blackwing Lair (Hard)", "classic-raid"},
            {"aq40-n", 531, 0, 25, 239, true, "Temple of Ahn'Qiraj", "classic-raid"},
            {"aq40-h", 531, 3, 25, 274, true, "Temple of Ahn'Qiraj (Hard)", "classic-raid"},
            // TBC raids (rescaled). [ladder_v2] Karazhan / Zul'Aman also 25N / 25H
            {"kara-n", 532, 0, 10, 232, true, "Karazhan", "tbc-raid"},
            {"kara-h", 532, 2, 10, 267, true, "Karazhan (Hard)", "tbc-raid"},
            {"kara25-n", 532, 1, 25, 250, true, "Karazhan 25", "tbc-raid"},
            {"kara25-h", 532, 3, 25, 285, true, "Karazhan 25 (Hard)", "tbc-raid"},
            {"za-n", 568, 0, 10, 242, true, "Zul'Aman", "tbc-raid"},
            {"za-h", 568, 2, 10, 278, true, "Zul'Aman (Hard)", "tbc-raid"},
            {"za25-n", 568, 1, 25, 257, true, "Zul'Aman 25", "tbc-raid"},
            {"za25-h", 568, 3, 25, 293, true, "Zul'Aman 25 (Hard)", "tbc-raid"},
            {"gruul-n", 565, 0, 25, 244, true, "Gruul's Lair", "tbc-raid"},
            {"gruul-h", 565, 3, 25, 280, true, "Gruul's Lair (Hard)", "tbc-raid"},
            {"mag-n", 544, 0, 25, 245, true, "Magtheridon's Lair", "tbc-raid"},
            {"mag-h", 544, 3, 25, 282, true, "Magtheridon's Lair (Hard)", "tbc-raid"},
            {"ssc-n", 548, 0, 25, 258, true, "Serpentshrine Cavern", "tbc-raid"},
            {"ssc-h", 548, 3, 25, 294, true, "Serpentshrine Cavern (Hard)", "tbc-raid"},
            {"tk-n", 550, 0, 25, 257, true, "Tempest Keep: The Eye", "tbc-raid"},
            {"tk-h", 550, 3, 25, 289, true, "Tempest Keep: The Eye (Hard)", "tbc-raid"},
            {"hyjal-n", 534, 0, 25, 264, true, "Battle for Mount Hyjal", "tbc-raid"},
            {"hyjal-h", 534, 3, 25, 297, true, "Battle for Mount Hyjal (Hard)", "tbc-raid"},
            {"bt-n", 564, 0, 25, 264, true, "Black Temple", "tbc-raid"},
            {"bt-h", 564, 3, 25, 301, true, "Black Temple (Hard)", "tbc-raid"},
            {"swp-n", 580, 0, 25, 271, true, "Sunwell Plateau", "tbc-raid"},
            {"swp-h", 580, 3, 25, 303, true, "Sunwell Plateau (Hard)", "tbc-raid"},
            // WotLK raids (normal item levels)
            {"ony10", 249, 0, 10, 232, true, "Onyxia's Lair 10", "wotlk-raid"},
            {"ony25", 249, 1, 25, 245, true, "Onyxia's Lair 25", "wotlk-raid"},
            {"naxx10", 533, 0, 10, 200, true, "Naxxramas 10", "wotlk-raid"},
            {"naxx25", 533, 1, 25, 213, true, "Naxxramas 25", "wotlk-raid"},
            {"naxx25h", 533, 3, 25, 277, true, "Naxxramas 25 Heroic", "wotlk-raid"}, // [ladder_v2]
            {"os10", 615, 0, 10, 200, true, "The Obsidian Sanctum 10", "wotlk-raid"},
            {"os25", 615, 1, 25, 213, true, "The Obsidian Sanctum 25", "wotlk-raid"},
            {"eoe10", 616, 0, 10, 213, true, "The Eye of Eternity 10", "wotlk-raid"},
            {"eoe25", 616, 1, 25, 226, true, "The Eye of Eternity 25", "wotlk-raid"},
            {"voa10", 624, 0, 10, 200, true, "Vault of Archavon 10", "wotlk-raid"},
            {"voa25", 624, 1, 25, 213, true, "Vault of Archavon 25", "wotlk-raid"},
            {"uld10", 603, 0, 10, 219, true, "Ulduar 10", "wotlk-raid"},
            {"uld25", 603, 1, 25, 226, true, "Ulduar 25", "wotlk-raid"},
            {"uld10h", 603, 2, 10, 322, true, "Ulduar 10 Heroic", "wotlk-raid"}, // [ladder_v3] 322-331
            {"uld25h", 603, 3, 25, 338, true, "Ulduar 25 Heroic", "wotlk-raid"}, // [ladder_v3] 338-347
            {"toc10", 649, 0, 10, 232, true, "Trial of the Crusader 10", "wotlk-raid"},
            {"toc25", 649, 1, 25, 245, true, "Trial of the Crusader 25", "wotlk-raid"},
            {"toc10h", 649, 2, 10, 318, true, "Trial of the Crusader 10 Heroic", "wotlk-raid"}, // [ladder_v3]
            {"toc25h", 649, 3, 25, 334, true, "Trial of the Crusader 25 Heroic", "wotlk-raid"}, // [ladder_v3]
            {"icc10", 631, 0, 10, 251, true, "Icecrown Citadel 10", "wotlk-raid"},
            {"icc25", 631, 1, 25, 264, true, "Icecrown Citadel 25", "wotlk-raid"},
            {"icc10h", 631, 2, 10, 328, true, "Icecrown Citadel 10 Heroic", "wotlk-raid"}, // [ladder_v3] 328-334
            {"icc25h", 631, 3, 25, 344, true, "Icecrown Citadel 25 Heroic", "wotlk-raid"}, // [ladder_v3] 344-350
            {"rs10", 724, 0, 10, 258, true, "The Ruby Sanctum 10", "wotlk-raid"},
            {"rs25", 724, 1, 25, 271, true, "The Ruby Sanctum 25", "wotlk-raid"},
            {"rs10h", 724, 2, 10, 334, true, "The Ruby Sanctum 10 Heroic", "wotlk-raid"}, // [ladder_v3]
            {"rs25h", 724, 3, 25, 350, true, "The Ruby Sanctum 25 Heroic", "wotlk-raid"}, // [ladder_v3]
        };

        // Level tiers: the first entry whose minimum level the character reaches, highest first.
        struct LevelItem
        {
            uint8 minLevel;
            uint32 item;
        };

        uint32 ItemForLevel(std::vector<LevelItem> const& tiers, uint8 level)
        {
            for (LevelItem const& tier : tiers)
                if (level >= tier.minLevel)
                    return tier.item;
            return 0;
        }

        // Item ids and required levels verified against acore_world.item_template.
        std::vector<LevelItem> const healingPotions = {{70, 33447}, {55, 22829}, {45, 13446}, {35, 3928},
                                                       {21, 1710},  {12, 929},   {3, 858},    {1, 118}};
        std::vector<LevelItem> const manaPotions = {{70, 33448}, {55, 22832}, {49, 13444}, {41, 13443},
                                                    {31, 6149},  {22, 3827},  {14, 3385},  {5, 2455}};
        std::vector<LevelItem> const drinks = {{75, 33445}, {65, 27860}, {60, 28399}, {45, 8766}, {35, 1645},
                                               {25, 1708},  {15, 1205},  {5, 1179},   {1, 159}};
        std::vector<LevelItem> const regenFoods = {{70, 43268}, {65, 29451}, {55, 27855}, {45, 8952}, {35, 4599},
                                                   {25, 3771},  {15, 3770},  {5, 2287},   {1, 117}};

        // "Well Fed" food per stat.
        std::vector<LevelItem> const staminaFood = {{70, ITEM_DRAGONFIN_FILET}, {65, 33052}, {55, 27667},
                                                    {40, 12218}, {35, 17222}, {25, 12210}, {15, 3727}, {1, 2680}};
        std::vector<LevelItem> const bearFood = {{70, ITEM_BLACKENED_DRAGONFIN}, {65, 33052}, {55, 27667},
                                                 {40, 12218}, {35, 17222}, {25, 12210}, {15, 3727}, {1, 2680}};
        std::vector<LevelItem> const manaFood = {{70, ITEM_FIRECRACKER_SALMON}, {55, 27666}, {35, 13931},
                                                 {30, 21217}, {10, 21072}, {1, 6888}};
        std::vector<LevelItem> const spellFood = {{70, ITEM_FIRECRACKER_SALMON}, {55, 27657}, {45, 18254},
                                                  {30, 21217}, {10, 21072}, {1, 6888}};
        std::vector<LevelItem> const agilityFood = {{70, ITEM_BLACKENED_DRAGONFIN}, {55, 27659}, {35, 13928},
                                                    {25, 12210}, {15, 3727}, {1, 2680}};
        std::vector<LevelItem> const strengthFood = {{70, ITEM_DRAGONFIN_FILET}, {55, 27658}, {45, 20452},
                                                     {25, 12210}, {15, 3727}, {1, 2680}};
        std::vector<LevelItem> const attackPowerFood = {{70, 34754}, {55, 27658}, {45, 20452}, {25, 12210},
                                                        {15, 3727},  {1, 2680}};

        // Flasks from 50 (Classic), 65 (TBC), 75 (WotLK); none below 50. No Classic flask for physical damage.
        std::vector<LevelItem> const tankFlasks = {{75, ITEM_FLASK_STONEBLOOD}, {65, 22851}, {50, 13510}};
        std::vector<LevelItem> const casterFlasks = {{75, ITEM_FLASK_FROST_WYRM}, {65, 22866}, {50, 13512}};
        std::vector<LevelItem> const healerFlasks = {{75, ITEM_FLASK_PURE_MOJO}, {65, 22853}, {50, 13511}};
        std::vector<LevelItem> const paladinHealerFlasks = {{75, ITEM_FLASK_FROST_WYRM}, {65, 22853}, {50, 13511}};
        std::vector<LevelItem> const physicalFlasks = {{75, ITEM_FLASK_ENDLESS_RAGE}, {65, 22854}};

        // Pre-pots: the WotLK set at 70, TBC combat potions from 60, mana potions for mana users below.
        std::vector<LevelItem> const tankPots = {{70, ITEM_INDESTRUCTIBLE_POTION}, {60, 22849}, {46, 13455}};
        std::vector<LevelItem> const casterPots = {{70, ITEM_POTION_OF_WILD_MAGIC}, {60, 22839}};
        std::vector<LevelItem> const physicalPots = {{70, ITEM_POTION_OF_SPEED}, {60, 22838}};
    }

    char const* RoleName(Role role)
    {
        switch (role)
        {
            case Role::Tank:
                return "tank";
            case Role::Healer:
                return "healer";
            case Role::Melee:
                return "melee";
            case Role::Ranged:
                return "ranged";
            case Role::Caster:
                return "caster";
        }
        return "?";
    }

    bool UsesMana(Role role, uint8 cls)
    {
        if (cls == CLASS_WARRIOR || cls == CLASS_ROGUE || cls == CLASS_DEATH_KNIGHT)
            return false;

        // Feral druids and hunters carry mana but do not drink for it in a fight.
        return role == Role::Healer || role == Role::Caster || cls == CLASS_PALADIN || cls == CLASS_SHAMAN ||
               cls == CLASS_HUNTER;
    }

    std::vector<SpecDef> const& Specs() { return specs; }

    SpecDef const* FindSpec(std::string const& key)
    {
        for (SpecDef const& spec : specs)
            if (key == spec.key)
                return &spec;
        return nullptr;
    }

    std::vector<Composition> const& Compositions() { return compositions; }

    Composition const* FindComposition(std::string const& id)
    {
        for (Composition const& comp : compositions)
            if (id == comp.id)
                return &comp;
        return nullptr;
    }

    Composition const* OptimalComposition(uint8 size)
    {
        for (Composition const& comp : compositions)
            if (comp.size == size && comp.optimal)
                return &comp;
        return nullptr;
    }

    std::vector<Composition const*> CompositionsOfSize(uint8 size)
    {
        std::vector<Composition const*> result;
        for (Composition const& comp : compositions)
            if (comp.size == size && !comp.horde)
                result.push_back(&comp);
        return result;
    }

    std::vector<ContentDef> const& Contents() { return contents; }

    ContentDef const* FindContent(std::string const& key)
    {
        for (ContentDef const& content : contents)
            if (key == content.key)
                return &content;
        return nullptr;
    }

    uint32 RunBudgetMinutes(char const* contentKey)
    {
        // [hleft-run-budget] user decision 2026-10-25: Blackrock Depths stays one run, 180 minutes.
        struct RunBudgetDef
        {
            char const* key;
            uint32 minutes;
        };
        static RunBudgetDef const budgets[] = {
            {"brd-n", 180},
            {"brd-h", 180},
            // [b32_timeout] Dire Maul heroic is all three wings in one key (16 bosses). dme-h 20261025-221216-5E-11:
            // East 45.7 min, West to 4/5 generators at 88.7 min, North untouched; projected ~141 min at that pace,
            // ~120 min net of waits / sitting / idle; budget = net x 1.3.
            {"dme-h", 160},
        };
        if (!contentKey)
            return 0;
        for (RunBudgetDef const& def : budgets)
            if (std::strcmp(def.key, contentKey) == 0)
                return def.minutes;
        return 0;
    }

    RoleConsumables ConsumablesFor(SpecDef const& spec, uint8 level)
    {
        uint32 const manaPot = ItemForLevel(manaPotions, level);
        switch (spec.role)
        {
            case Role::Tank:
                return {ItemForLevel(tankFlasks, level),
                        ItemForLevel(spec.cls == CLASS_DRUID ? bearFood : staminaFood, level),
                        ItemForLevel(tankPots, level)};
            case Role::Healer:
            {
                bool const mojo = spec.cls == CLASS_PRIEST || spec.cls == CLASS_DRUID;
                return {ItemForLevel(mojo ? healerFlasks : paladinHealerFlasks, level), ItemForLevel(manaFood, level),
                        manaPot};
            }
            case Role::Caster:
            {
                uint32 const pot = ItemForLevel(casterPots, level);
                return {ItemForLevel(casterFlasks, level), ItemForLevel(spellFood, level), pot ? pot : manaPot};
            }
            case Role::Ranged:
                return {ItemForLevel(physicalFlasks, level), ItemForLevel(agilityFood, level),
                        ItemForLevel(physicalPots, level)};
            case Role::Melee:
            {
                // Fish Feast needs Cooking 425 to put down; the Enhancement shaman eats Mega Mammoth Meal (AP).
                std::vector<LevelItem> const* food = &strengthFood;
                if (spec.cls == CLASS_ROGUE || spec.cls == CLASS_DRUID)
                    food = &agilityFood;
                else if (spec.cls == CLASS_SHAMAN)
                    food = &attackPowerFood;
                return {ItemForLevel(physicalFlasks, level), ItemForLevel(*food, level),
                        ItemForLevel(physicalPots, level)};
            }
        }
        return {0, ItemForLevel(staminaFood, level), 0};
    }

    LevelSupplies SuppliesFor(uint8 level)
    {
        return {ItemForLevel(healingPotions, level), ItemForLevel(manaPotions, level), ItemForLevel(drinks, level),
                ItemForLevel(regenFoods, level)};
    }

    uint32 DefaultTargetIlvl(uint8 level)
    {
        if (level >= MAX_TEST_LEVEL)
            return 200;
        if (level > 70)
            return 150 + (uint32(level) - 71) * 30 / 9;
        if (level > 60)
            return 100 + (uint32(level) - 61) * 15 / 9;
        // Quest greens and dungeon blues a levelling character really wears (the level + 7 curve was above what the
        // gear filters find, wave 10 gear investigation).
        return uint32(level) + 4;
    }

    float ExpectedMemberDps(Role role, uint8 level, float avgIlvl, uint8 size)
    {
        auto interp = [](std::vector<std::pair<float, float>> const& curve, float x)
        {
            if (x <= curve.front().first)
                return curve.front().second;
            for (size_t index = 1; index < curve.size(); ++index)
                if (x <= curve[index].first)
                {
                    auto const& [x0, y0] = curve[index - 1];
                    auto const& [x1, y1] = curve[index];
                    return y0 + (y1 - y0) * (x - x0) / (x1 - x0);
                }
            return curve.back().second;
        };

        static std::vector<std::pair<float, float>> const levelDps = {
            {10.0f, 18.0f}, {20.0f, 45.0f}, {30.0f, 100.0f}, {40.0f, 170.0f},
            {50.0f, 270.0f}, {60.0f, 450.0f}, {70.0f, 1000.0f}, {80.0f, 1800.0f}};
        static std::vector<std::pair<float, float>> const ilvlDps = {
            {150.0f, 1100.0f}, {172.0f, 1700.0f}, {187.0f, 2300.0f}, {200.0f, 3000.0f}, {216.0f, 3900.0f},
            {229.0f, 4900.0f}, {245.0f, 6200.0f}, {258.0f, 7600.0f}, {264.0f, 8400.0f}, {277.0f, 10500.0f}};

        float dps = level < MAX_TEST_LEVEL ? interp(levelDps, float(level))
                                           : interp(ilvlDps, avgIlvl) * (size <= 5 ? 0.80f : size <= 10 ? 0.92f : 1.0f);
        switch (role)
        {
            case Role::Tank:
                dps *= 0.6f;
                break;
            case Role::Healer:
                dps *= 0.1f;
                break;
            default:
                break;
        }
        return dps;
    }

    uint16 ProfessionSkillFor(uint8 level)
    {
        if (level >= MAX_TEST_LEVEL)
            return 450;
        return uint16(std::min<uint32>(450, uint32(level) * 5));
    }

    std::pair<uint16, uint16> ProfessionPairFor(Role role, uint32 indexInRole)
    {
        static std::vector<std::pair<uint16, uint16>> const tank = {
            {SKILL_JEWELCRAFTING, SKILL_BLACKSMITHING}, {SKILL_JEWELCRAFTING, SKILL_MINING},
            {SKILL_BLACKSMITHING, SKILL_MINING}};
        static std::vector<std::pair<uint16, uint16>> const physical = {
            {SKILL_JEWELCRAFTING, SKILL_BLACKSMITHING}, {SKILL_JEWELCRAFTING, SKILL_ENGINEERING},
            {SKILL_LEATHERWORKING, SKILL_JEWELCRAFTING}, {SKILL_BLACKSMITHING, SKILL_ENGINEERING},
            {SKILL_ENGINEERING, SKILL_LEATHERWORKING}, {SKILL_JEWELCRAFTING, SKILL_SKINNING}};
        static std::vector<std::pair<uint16, uint16>> const caster = {
            {SKILL_TAILORING, SKILL_ENCHANTING}, {SKILL_TAILORING, SKILL_JEWELCRAFTING},
            {SKILL_ENCHANTING, SKILL_JEWELCRAFTING}, {SKILL_TAILORING, SKILL_ENGINEERING}};
        static std::vector<std::pair<uint16, uint16>> const healer = {
            {SKILL_ALCHEMY, SKILL_INSCRIPTION}, {SKILL_INSCRIPTION, SKILL_HERBALISM},
            {SKILL_ALCHEMY, SKILL_HERBALISM}, {SKILL_TAILORING, SKILL_ENCHANTING},
            {SKILL_INSCRIPTION, SKILL_JEWELCRAFTING}};

        std::vector<std::pair<uint16, uint16>> const* table = &physical;
        if (role == Role::Tank)
            table = &tank;
        else if (role == Role::Caster)
            table = &caster;
        else if (role == Role::Healer)
            table = &healer;

        return (*table)[indexInRole % table->size()];
    }

    char const* ProfessionName(uint16 skillId)
    {
        switch (skillId)
        {
            case SKILL_ALCHEMY:
                return "Alchemy";
            case SKILL_BLACKSMITHING:
                return "Blacksmithing";
            case SKILL_ENCHANTING:
                return "Enchanting";
            case SKILL_ENGINEERING:
                return "Engineering";
            case SKILL_HERBALISM:
                return "Herbalism";
            case SKILL_INSCRIPTION:
                return "Inscription";
            case SKILL_JEWELCRAFTING:
                return "Jewelcrafting";
            case SKILL_LEATHERWORKING:
                return "Leatherworking";
            case SKILL_MINING:
                return "Mining";
            case SKILL_SKINNING:
                return "Skinning";
            case SKILL_TAILORING:
                return "Tailoring";
            default:
                return "?";
        }
    }

    std::vector<std::string> CoverageGaps(Composition const& comp)
    {
        std::map<uint32, uint32> providers;
        uint32 paladins = 0;
        for (CompSlot const& slot : comp.slots)
        {
            SpecDef const* spec = FindSpec(slot.spec);
            if (!spec)
                continue;

            for (uint32 bit = 1; bit <= CAP_DISPEL_DISEASE; bit <<= 1)
                if (spec->caps & bit)
                    ++providers[bit];

            if (spec->cls == CLASS_PALADIN)
                ++paladins;
        }

        bool const big = comp.size >= 25;
        struct Rule
        {
            uint32 cap;
            uint32 need;
            char const* name;
        };
        std::vector<Rule> const rules = {
            {CAP_HEROISM, 1, "Heroism/Bloodlust"},
            {CAP_REPLENISHMENT, big ? 2u : 1u, "Replenishment"},
            {CAP_FORTITUDE, 1, "Fortitude/Spirit/Shadow Protection"},
            {CAP_MARK_OF_THE_WILD, 1, "Mark of the Wild"},
            {CAP_ARCANE_INTELLECT, 1, "Arcane Intellect"},
            {CAP_MAJOR_ARMOR, 1, "major armor (Sunder/Expose)"},
            {CAP_MINOR_ARMOR, 1, "minor armor (Faerie Fire/Curse of Weakness)"},
            {CAP_SPELL_DAMAGE_TAKEN, 1, "spell damage taken (CoE/Ebon Plague/Earth and Moon)"},
            {CAP_CRIT_TAKEN, 1, "crit taken (Heart of the Crusader/Totem of Wrath/Master Poisoner)"},
            {CAP_SPELL_HIT, 1, "3% spell hit (Misery/Improved Faerie Fire)"},
            {CAP_BATTLE_RES, big ? 2u : 1u, "battle res"},
            {CAP_INTERRUPT, 3, "interrupts"},
            {CAP_DISPEL_MAGIC, 1, "dispel magic"},
            {CAP_DISPEL_CURSE, 1, "dispel curse"},
            {CAP_DISPEL_POISON, 1, "dispel poison"},
            {CAP_DISPEL_DISEASE, 1, "dispel disease"},
        };

        std::vector<std::string> gaps;
        for (Rule const& rule : rules)
        {
            uint32 const have = providers[rule.cap];
            if (have < rule.need)
                gaps.push_back(std::string(rule.name) + ": " + std::to_string(have) + "/" + std::to_string(rule.need));
        }

        if (paladins < 3)
            gaps.push_back("Kings/Might/Wisdom: " + std::to_string(paladins) + "/3 paladins");

        return gaps;
    }

    char const* const HORDE_COMPOSITION = "5AH";

    bool HordeRunContent(char const* key)
    {
        return key && (std::strcmp(key, "ulda-n") == 0 || std::strcmp(key, "ulda-h") == 0);
    }

    uint8 RaceForClass(uint8 cls, bool horde)
    {
        if (horde)
            switch (cls)
            {
                case CLASS_PALADIN:
                    return RACE_BLOODELF;
                case CLASS_DRUID:
                    return RACE_TAUREN;
                case CLASS_PRIEST:
                case CLASS_MAGE:
                    return RACE_UNDEAD_PLAYER;
                default:
                    return RACE_ORC;  // warrior, hunter, rogue, death knight, shaman, warlock
            }
        switch (cls)
        {
            case CLASS_HUNTER:
                return RACE_DWARF;
            case CLASS_SHAMAN:
                return RACE_DRAENEI;
            case CLASS_DRUID:
                return RACE_NIGHTELF;
            default:
                return RACE_HUMAN;
        }
    }

    char const* ClassName(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:
                return "warrior";
            case CLASS_PALADIN:
                return "paladin";
            case CLASS_HUNTER:
                return "hunter";
            case CLASS_ROGUE:
                return "rogue";
            case CLASS_PRIEST:
                return "priest";
            case CLASS_DEATH_KNIGHT:
                return "deathknight";
            case CLASS_SHAMAN:
                return "shaman";
            case CLASS_MAGE:
                return "mage";
            case CLASS_WARLOCK:
                return "warlock";
            case CLASS_DRUID:
                return "druid";
            default:
                return "?";
        }
    }
}
