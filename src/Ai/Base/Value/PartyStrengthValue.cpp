/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PartyStrengthValue.h"

#include "DBCStores.h"
#include "Group.h"
#include "Item.h"
#include "Map.h"
#include "Player.h"
#include "Playerbots.h"
#include "Timer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>

namespace
{
    constexpr uint32 STRENGTH_REFRESH_MS = 10 * IN_MILLISECONDS;   // gear changes between pulls, not during them
    constexpr uint32 THREAT_SCARE_MS = 60 * IN_MILLISECONDS;       // a near aggro pull keeps the dps careful this long
    constexpr uint32 REGISTRY_STALE_MS = 5 * MINUTE * IN_MILLISECONDS;
    constexpr size_t REGISTRY_SWEEP_SIZE = 256;

    constexpr float FACTOR_MIN = 0.6f;
    constexpr float FACTOR_MAX = 1.5f;
    // Item budget grows about linearly with item level and stats multiply into damage and survival, so a group's
    // power goes with the square of its item level ratio: 13 levels over a 200 heroic is about +14%.
    constexpr float ILVL_EXPONENT = 2.0f;
    // dps whose gear factor is this much over the tank's (about 10 item levels at 200) out-threat it
    constexpr float OUTGEAR_FACTOR_RATIO = 1.1f;
    // A geared tank has about half again the health of the rest of the group; a tank far below that is soft.
    constexpr float TANK_HEALTH_RATIO = 1.5f;
    constexpr float TANK_HEALTH_ADJUST_MIN = 0.9f;
    constexpr float TANK_HEALTH_ADJUST_MAX = 1.1f;
    constexpr float WEIGHT_TANK = 0.35f;
    constexpr float WEIGHT_HEALER = 0.35f;
    constexpr float WEIGHT_DPS = 0.30f;
    constexpr float CAREFUL_TANK_FACTOR = 0.9f;
    // Below this level empty slots (no ring, no trinket yet) are normal and the average skips them; from here on
    // an empty slot counts as 0 like the game's own average item level.
    constexpr uint8 FULL_SLOTS_LEVEL = 60;
    constexpr uint8 CLASSIC_MAX_LEVEL = 60;
    constexpr uint8 TBC_MAX_LEVEL = 70;

    // Tank kit: mobs held by hand, one more with a taunt, and what each multi-target threat spell adds.
    constexpr float TANK_KIT_BASE_MOBS = 2.0f;
    constexpr float TANK_KIT_TAUNT_MOBS = 1.0f;
    constexpr float TANK_KIT_MAX_MOBS = 8.0f;

    struct KitSpell
    {
        char const* name;
        float mobs;
    };

    // [b32_smpull] Swipe (Bear) 2 -> 3: in 3.3.5 it hits the enemies in front with no target cap, and since
    // [b32_tank_aoe] the bear opens and holds every 3+ mob pack with it ("tank aoe pack" above Mangle). At 2 the bear's
    // kit was 5 against the paladin's 7 (lbrs-n 5D-10 "hold 5" vs 5A "hold 8"), so the hold term read the same chain
    // 1.6x over for the bear; it now counts as the warrior's Thunder Clap + Shockwave (6).
    constexpr KitSpell TANK_KIT_SPELLS[] = {
        { "consecration", 2.0f },     { "avenger's shield", 1.0f }, { "hammer of the righteous", 1.0f },
        { "thunder clap", 2.0f },     { "shockwave", 1.0f },        { "swipe (bear)", 3.0f },
        { "death and decay", 2.0f },  { "blood boil", 1.0f },
    };

    constexpr char const* TANK_TAUNTS[] = {
        "hand of reckoning", "righteous defense", "taunt", "growl", "dark command",
    };

    // Item level a group geared for the content wears: the content's own normal drop level, so a group in the
    // gear the place drops plays it as designed. Raids of the raid_rescale ladder (C:\WoW\raid_rescale\PLAN.md
    // section 1) are level 80 content now; their normal and hard columns are used. WotLK 5-man heroics are 200,
    // Classic and TBC heroics follow the 200-251 ladder of section 12.
    struct RaidItemLevel
    {
        uint32 mapId;
        float normal10;
        float normal25;
        float heroic10;
        float heroic25;
    };

    constexpr RaidItemLevel RAID_ITEM_LEVELS[] = {
        // WotLK. [ladder_v3] heroics: boss ilvl of the v3 configs (endgame/uld*h.json, wotlk_heroic/*.json);
        // Naxx-25H stays 277. Raids without a heroic repeat their normal values.
        { 533, 200.0f, 213.0f, 200.0f, 277.0f },  // Naxxramas
        { 615, 200.0f, 213.0f, 200.0f, 213.0f },  // The Obsidian Sanctum
        { 616, 213.0f, 226.0f, 213.0f, 226.0f },  // The Eye of Eternity
        { 624, 213.0f, 226.0f, 213.0f, 226.0f },  // Vault of Archavon
        { 603, 219.0f, 226.0f, 328.0f, 338.0f },  // Ulduar
        { 249, 232.0f, 245.0f, 232.0f, 245.0f },  // Onyxia's Lair
        { 649, 232.0f, 245.0f, 318.0f, 334.0f },  // Trial of the Crusader
        { 631, 251.0f, 264.0f, 328.0f, 344.0f },  // Icecrown Citadel
        { 724, 258.0f, 271.0f, 334.0f, 350.0f },  // The Ruby Sanctum
        // raid_rescale ladder v2.1 (C:\WoW\RAID_LADDER_V2.md, configs old_raids/*.json). ZG / AQ20 / Kara / ZA have
        // all four modes; the 25-man-only raids run normal at difficulty 0 and hard at 3, both columns alike.
        { 532, 232.0f, 250.0f, 267.0f, 285.0f },  // Karazhan
        { 568, 242.0f, 257.0f, 278.0f, 293.0f },  // Zul'Aman
        { 565, 244.0f, 244.0f, 280.0f, 280.0f },  // Gruul's Lair
        { 544, 245.0f, 245.0f, 282.0f, 282.0f },  // Magtheridon's Lair
        { 548, 258.0f, 258.0f, 294.0f, 294.0f },  // Serpentshrine Cavern
        { 550, 257.0f, 257.0f, 289.0f, 289.0f },  // Tempest Keep: The Eye
        { 534, 264.0f, 264.0f, 297.0f, 297.0f },  // Battle for Mount Hyjal
        { 564, 264.0f, 264.0f, 301.0f, 301.0f },  // Black Temple
        { 580, 271.0f, 271.0f, 303.0f, 303.0f },  // Sunwell Plateau
        { 309, 219.0f, 237.0f, 251.0f, 269.0f },  // Zul'Gurub
        { 509, 219.0f, 237.0f, 253.0f, 271.0f },  // Ruins of Ahn'Qiraj
        { 409, 226.0f, 226.0f, 256.0f, 256.0f },  // Molten Core
        { 469, 232.0f, 232.0f, 265.0f, 265.0f },  // Blackwing Lair
        { 531, 239.0f, 239.0f, 274.0f, 274.0f },  // Temple of Ahn'Qiraj
    };

    struct DungeonItemLevel
    {
        uint32 mapId;
        float normal;   // 0: by the dungeon's level
        float heroic;
    };

    constexpr float WOTLK_HEROIC_ITEM_LEVEL = 200.0f;
    constexpr DungeonItemLevel DUNGEON_ITEM_LEVELS[] = {
        { 650, 200.0f, 219.0f },  // Trial of the Champion
        { 632, 219.0f, 232.0f },  // The Forge of Souls
        { 658, 219.0f, 232.0f },  // Pit of Saron
        { 668, 219.0f, 232.0f },  // Halls of Reflection
        { 574, 0.0f, WOTLK_HEROIC_ITEM_LEVEL },  // Utgarde Keep
        { 575, 0.0f, WOTLK_HEROIC_ITEM_LEVEL },  // Utgarde Pinnacle
        { 576, 0.0f, WOTLK_HEROIC_ITEM_LEVEL },  // The Nexus
        { 578, 0.0f, WOTLK_HEROIC_ITEM_LEVEL },  // The Oculus
        { 595, 0.0f, WOTLK_HEROIC_ITEM_LEVEL },  // The Culling of Stratholme
        { 599, 0.0f, WOTLK_HEROIC_ITEM_LEVEL },  // Halls of Stone
        { 600, 0.0f, WOTLK_HEROIC_ITEM_LEVEL },  // Drak'Tharon Keep
        { 601, 0.0f, WOTLK_HEROIC_ITEM_LEVEL },  // Azjol-Nerub
        { 602, 0.0f, WOTLK_HEROIC_ITEM_LEVEL },  // Halls of Lightning
        { 604, 0.0f, WOTLK_HEROIC_ITEM_LEVEL },  // Gundrak
        { 608, 0.0f, WOTLK_HEROIC_ITEM_LEVEL },  // The Violet Hold
        { 619, 0.0f, WOTLK_HEROIC_ITEM_LEVEL },  // Ahn'kahet: The Old Kingdom
        // raid_rescale heroic ladder (PLAN.md section 12): Classic and TBC heroics are level 80 content of this
        // item level. Without these rows a Classic heroic read as level 80 quest greens (187) and a TBC heroic as
        // level 70 blues (115), and every group "outgeared" it at the 1.5 cap.
        { 389, 0.0f, 200.0f },  // Ragefire Chasm
        { 43, 0.0f, 200.0f },   // Wailing Caverns
        { 36, 0.0f, 200.0f },   // The Deadmines
        { 33, 0.0f, 200.0f },   // Shadowfang Keep
        { 48, 0.0f, 200.0f },   // Blackfathom Deeps
        { 34, 0.0f, 200.0f },   // The Stockade
        { 90, 0.0f, 200.0f },   // Gnomeregan
        { 47, 0.0f, 200.0f },   // Razorfen Kraul
        { 189, 0.0f, 213.0f },  // Scarlet Monastery
        { 129, 0.0f, 213.0f },  // Razorfen Downs
        { 70, 0.0f, 213.0f },   // Uldaman
        { 209, 0.0f, 213.0f },  // Zul'Farrak
        { 349, 0.0f, 226.0f },  // Maraudon
        { 109, 0.0f, 226.0f },  // Sunken Temple
        { 230, 0.0f, 226.0f },  // Blackrock Depths
        { 229, 0.0f, 239.0f },  // Blackrock Spire
        { 429, 0.0f, 239.0f },  // Dire Maul
        { 289, 0.0f, 245.0f },  // Scholomance
        { 329, 0.0f, 245.0f },  // Stratholme
        { 543, 0.0f, 200.0f },  // Hellfire Ramparts
        { 542, 0.0f, 200.0f },  // The Blood Furnace
        { 547, 0.0f, 213.0f },  // The Slave Pens
        { 546, 0.0f, 213.0f },  // The Underbog
        { 557, 0.0f, 213.0f },  // Mana-Tombs
        { 558, 0.0f, 226.0f },  // Auchenai Crypts
        { 556, 0.0f, 226.0f },  // Sethekk Halls
        { 560, 0.0f, 226.0f },  // Old Hillsbrad Foothills
        { 545, 0.0f, 239.0f },  // The Steamvault
        { 554, 0.0f, 239.0f },  // The Mechanar
        { 553, 0.0f, 239.0f },  // The Botanica
        { 540, 0.0f, 239.0f },  // The Shattered Halls
        { 555, 0.0f, 245.0f },  // Shadow Labyrinth
        { 269, 0.0f, 245.0f },  // The Black Morass
        { 552, 0.0f, 245.0f },  // The Arcatraz
        { 585, 0.0f, 251.0f },  // Magisters' Terrace
    };

    // Item level of level-appropriate gear (quest greens and dungeon blues), per expansion: classic gear runs a few
    // levels over the character, Outland greens start near 85 at 58 and reach the 115 blues at 70, Northrend greens
    // start near 130 and reach the 187 dungeon blues at 80. A rough curve: it only has to tell a group dressed for
    // the place from one that is not.
    float ExpectedItemLevelForLevel(uint32 level, uint32 expansion)
    {
        if (expansion == 0 || (expansion > 2 && level <= CLASSIC_MAX_LEVEL))
            return float(std::min<uint32>(level, CLASSIC_MAX_LEVEL)) + 5.0f;

        if (expansion == 1 || (expansion > 2 && level <= TBC_MAX_LEVEL))
            return 85.0f + 2.5f * float(std::clamp<uint32>(level, 58, TBC_MAX_LEVEL) - 58);

        return 130.0f + (57.0f / 9.0f) * float(std::clamp<uint32>(level, 71, 80) - 71);
    }

    // The dungeon finder's own level for a map and difficulty, with its expansion; built once from the DBC.
    struct ContentLevel
    {
        uint32 level = 0;
        uint32 expansion = 0;
    };

    ContentLevel LookupContentLevel(uint32 mapId, uint32 difficulty)
    {
        static std::unordered_map<uint32, ContentLevel> const levels = []
        {
            std::unordered_map<uint32, ContentLevel> table;
            for (uint32 index = 0; index < sLFGDungeonStore.GetNumRows(); ++index)
            {
                LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(index);
                if (!dungeon || !dungeon->TargetLevel)
                    continue;

                MapEntry const* mapEntry = sMapStore.LookupEntry(dungeon->MapID);
                if (!mapEntry || !mapEntry->IsDungeon())
                    continue;

                ContentLevel level;
                level.level = dungeon->TargetLevel;
                level.expansion = dungeon->ExpansionLevel;
                table.emplace((dungeon->MapID << 4) | (dungeon->Difficulty & 0xF), level);
            }

            return table;
        }();

        auto const found = levels.find((mapId << 4) | (difficulty & 0xF));
        return found != levels.end() ? found->second : ContentLevel();
    }

    // Expected item level of the map the group stands in, or 0 when only the members' own levels can tell.
    float ExpectedItemLevelForMap(Map* map)
    {
        uint32 const mapId = map->GetId();
        bool const heroic = map->IsHeroic();
        if (map->IsRaid())
        {
            for (RaidItemLevel const& raid : RAID_ITEM_LEVELS)
            {
                if (raid.mapId != mapId)
                    continue;

                bool const large = map->Is25ManRaid();
                if (heroic)
                    return large ? raid.heroic25 : raid.heroic10;

                return large ? raid.normal25 : raid.normal10;
            }
        }
        else
        {
            for (DungeonItemLevel const& dungeon : DUNGEON_ITEM_LEVELS)
            {
                if (dungeon.mapId != mapId)
                    continue;

                float const itemLevel = heroic ? dungeon.heroic : dungeon.normal;
                if (itemLevel > 0.0f)
                    return itemLevel;

                break;
            }
        }

        ContentLevel const content = LookupContentLevel(mapId, uint32(map->GetDifficulty()));
        return content.level ? ExpectedItemLevelForLevel(content.level, content.expansion) : 0.0f;
    }

    float AverageItemLevel(PlayerbotAI* botAI, Player* member)
    {
        if (member->GetLevel() >= FULL_SLOTS_LEVEL)
            return float(botAI->GetEquipGearScore(member));

        uint32 sum = 0;
        uint32 count = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
                continue;

            if (Item* item = member->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                sum += item->GetTemplate()->ItemLevel;
                ++count;
            }
        }

        return count ? float(sum) / float(count) : 0.0f;
    }

    float GearFactor(float itemLevel, float expected)
    {
        if (expected <= 0.0f)
            return 1.0f;

        return std::pow(std::max(itemLevel, 1.0f) / expected, ILVL_EXPONENT);
    }

    float Clamp(float factor) { return std::clamp(factor, FACTOR_MIN, FACTOR_MAX); }

    struct RoleSum
    {
        float gear = 0.0f;     // sum of gear factors
        float health = 0.0f;   // sum of max health
        uint32 count = 0;

        void Add(float gearFactor, uint32 maxHealth)
        {
            gear += gearFactor;
            health += float(maxHealth);
            ++count;
        }

        float Gear() const { return count ? gear / float(count) : 1.0f; }
        float Health() const { return count ? health / float(count) : 0.0f; }
    };

    PartyStrength Compute(PlayerbotAI* botAI, uint32 scareMs, uint32 now)
    {
        PartyStrength result;
        Player* bot = botAI->GetBot();
        Group* group = bot->GetGroup();
        Map* map = bot->GetMap();
        if (!group || !map)
            return result;

        float const mapExpected = map->IsDungeon() ? ExpectedItemLevelForMap(map) : 0.0f;
        RoleSum tanks;
        RoleSum healers;
        RoleSum dps;
        bool tankLacksAreaThreat = false;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            // Members on another map are read by another thread; they also take no part in this fight.
            if (!member || !member->IsInWorld() || member->GetMap() != map)
                continue;

            float const expected = mapExpected > 0.0f ? mapExpected
                                                      : ExpectedItemLevelForLevel(member->GetLevel(), UINT32_MAX);
            float const gear = GearFactor(AverageItemLevel(botAI, member), expected);
            if (PlayerbotAI::IsTank(member))
            {
                tanks.Add(gear, member->GetMaxHealth());
                if (map->IsDungeon() && !PartyStrengthShared::GetTankKit(member).areaThreat)
                    tankLacksAreaThreat = true;
            }
            else if (PlayerbotAI::IsHeal(member))
                healers.Add(gear, member->GetMaxHealth());
            else
                dps.Add(gear, member->GetMaxHealth());
        }

        if (!tanks.count && !healers.count && !dps.count)
            return result;

        bool const scared = scareMs && getMSTimeDiff(scareMs, now) < THREAT_SCARE_MS;
        bool const outgeared = tanks.count && dps.count && dps.Gear() >= tanks.Gear() * OUTGEAR_FACTOR_RATIO;
        result.dpsOutgearsTank = scared || outgeared;

        // The open world is not sized for a group: only the dps-against-tank comparison means anything there.
        if (!map->IsDungeon())
        {
            result.measured = result.dpsOutgearsTank;
            return result;
        }

        float tankFactor = tanks.Gear();
        uint32 const others = healers.count + dps.count;
        if (tanks.count && others)
        {
            float const othersHealth = (healers.health + dps.health) / float(others);
            if (othersHealth > 0.0f)
                tankFactor *= std::clamp(tanks.Health() / othersHealth / TANK_HEALTH_RATIO, TANK_HEALTH_ADJUST_MIN,
                                         TANK_HEALTH_ADJUST_MAX);
        }

        result.tank = Clamp(tankFactor);
        result.healer = Clamp(healers.Gear());
        result.dps = Clamp(dps.Gear());

        float weight = 0.0f;
        float sum = 0.0f;
        auto const addRole = [&](RoleSum const& role, float factor, float roleWeight)
        {
            if (!role.count)
                return;

            sum += factor * roleWeight;
            weight += roleWeight;
        };
        addRole(tanks, result.tank, WEIGHT_TANK);
        addRole(healers, result.healer, WEIGHT_HEALER);
        addRole(dps, result.dps, WEIGHT_DPS);
        result.overall = weight > 0.0f ? Clamp(sum / weight) : 1.0f;
        result.tankLacksAreaThreat = tankLacksAreaThreat;
        result.measured = true;
        return result;
    }

    struct RegistryEntry
    {
        uint32 computedMs = 0;
        uint32 scareMs = 0;
        bool computed = false;
        PartyStrength strength;
    };

    using RegistryKey = std::tuple<uint32, uint32, uint32>;  // group, map, instance

    std::mutex registryLock;
    std::map<RegistryKey, RegistryEntry> registry;

    bool MakeKey(Player* bot, RegistryKey& key)
    {
        Group* group = bot->GetGroup();
        Map* map = bot->GetMap();
        if (!group || !map)
            return false;

        key = RegistryKey(group->GetGUID().GetCounter(), map->GetId(), map->GetInstanceId());
        return true;
    }

    void SweepStale(uint32 now)
    {
        if (registry.size() < REGISTRY_SWEEP_SIZE)
            return;

        for (auto it = registry.begin(); it != registry.end();)
        {
            uint32 const last = std::max(it->second.computedMs, it->second.scareMs);
            if (getMSTimeDiff(last, now) > REGISTRY_STALE_MS)
                it = registry.erase(it);
            else
                ++it;
        }
    }
}

float PartyStrength::PullScale() const
{
    return std::clamp(std::min(std::min(tank, healer), overall), FACTOR_MIN, FACTOR_MAX);
}

// A tank without area threat holds a pack only mob by mob: the dps give it the same margin as a soft tank.
bool PartyStrength::CarefulThreat() const
{
    return dpsOutgearsTank || tank < CAREFUL_TANK_FACTOR || tankLacksAreaThreat;
}

namespace PartyStrengthShared
{
    TankKit GetTankKit(Player* tank)
    {
        TankKit kit;
        PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        if (!tankAI)
        {
            kit.holdMobs = TANK_KIT_MAX_MOBS;
            kit.areaThreat = true;
            return kit;
        }

        // "spell id" resolves any known rank by name and is cached by the tank's own context
        AiObjectContext* context = tankAI->GetAiObjectContext();
        auto const knows = [context](char const* name)
        { return context->GetValue<uint32>("spell id", name)->Get() != 0; };

        kit.holdMobs = TANK_KIT_BASE_MOBS;
        for (char const* taunt : TANK_TAUNTS)
        {
            if (knows(taunt))
            {
                kit.holdMobs += TANK_KIT_TAUNT_MOBS;
                break;
            }
        }

        for (KitSpell const& spell : TANK_KIT_SPELLS)
        {
            if (!knows(spell.name))
                continue;

            kit.holdMobs += spell.mobs;
            kit.areaThreat = true;
        }

        kit.holdMobs = std::min(kit.holdMobs, TANK_KIT_MAX_MOBS);
        return kit;
    }

    PartyStrength Get(PlayerbotAI* botAI)
    {
        RegistryKey key;
        if (!botAI || !MakeKey(botAI->GetBot(), key))
            return PartyStrength();

        uint32 const now = getMSTime();
        uint32 scareMs = 0;
        {
            std::lock_guard<std::mutex> guard(registryLock);
            auto const found = registry.find(key);
            if (found != registry.end())
            {
                if (found->second.computed && getMSTimeDiff(found->second.computedMs, now) < STRENGTH_REFRESH_MS)
                    return found->second.strength;

                scareMs = found->second.scareMs;
            }
        }

        // Every member read here is on this map, so this thread owns them; the lock only guards the registry.
        PartyStrength const strength = Compute(botAI, scareMs, now);

        std::lock_guard<std::mutex> guard(registryLock);
        SweepStale(now);
        RegistryEntry& entry = registry[key];
        entry.computedMs = now;
        entry.computed = true;
        entry.strength = strength;
        return strength;
    }

    void NoteThreatScare(Player* bot)
    {
        RegistryKey key;
        if (!bot || !MakeKey(bot, key))
            return;

        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> guard(registryLock);
        RegistryEntry& entry = registry[key];
        bool const wasScared = entry.scareMs && getMSTimeDiff(entry.scareMs, now) < THREAT_SCARE_MS;
        entry.scareMs = now;
        // the next reader recomputes at once instead of waiting for the refresh
        if (!wasScared)
            entry.computed = false;
    }
}

PartyStrengthValue::PartyStrengthValue(PlayerbotAI* botAI)
    : CalculatedValue<PartyStrength>(botAI, "party strength", 2 * IN_MILLISECONDS)
{
}

PartyStrength PartyStrengthValue::Calculate() { return PartyStrengthShared::Get(botAI); }

std::string const PartyStrengthValue::Format()
{
    PartyStrength const strength = Calculate();
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "tank %.2f healer %.2f dps %.2f overall %.2f pull %.2f%s%s%s",
                  strength.tank, strength.healer, strength.dps, strength.overall, strength.PullScale(),
                  strength.dpsOutgearsTank ? " dps-outgear" : "", strength.tankLacksAreaThreat ? " no-aoe-tank" : "",
                  strength.measured ? "" : " (neutral)");
    return buffer;
}

PullGraceStartValue::PullGraceStartValue(PlayerbotAI* botAI) : CalculatedValue<time_t>(botAI, "pull grace start") {}

time_t PullGraceStartValue::Calculate()
{
    // The value is read while the bot picks targets, which it does all fight long: a gap this long between reads
    // means the last fight ended (loot, rest) and this is a new pull. Chain pulls closer than that count as one
    // fight and get no new grace.
    constexpr time_t NEW_FIGHT_GAP_SECONDS = 8;

    // A dps standing back is not in combat itself yet while the tank picks the pack up; the group's attackers
    // already say the pull is on.
    if (!bot->IsInCombat() && context->GetValue<GuidVector>("attackers")->Get().empty())
    {
        _start = 0;
        _lastSeen = 0;
        return 0;
    }

    time_t const now = time(nullptr);
    if (!_start || now - _lastSeen > NEW_FIGHT_GAP_SECONDS)
    {
        _start = now;
        _startMs = getMSTime();  // [b32_engage]
    }

    _lastSeen = now;
    return _start;
}

UntypedValue* CreatePartyStrengthValue(PlayerbotAI* botAI) { return new PartyStrengthValue(botAI); }

UntypedValue* CreatePullGraceStartValue(PlayerbotAI* botAI) { return new PullGraceStartValue(botAI); }
