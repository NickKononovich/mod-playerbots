/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildGatherPlan.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>
#include <iterator>
#include <map>
#include <tuple>

#include "Creature.h"
#include "DpsEstimate.h"  // [questrate] the kill time of a skinning place
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Formulas.h"
#include "GameObject.h"
#include "Log.h"
#include "LootMgr.h"
#include "MapMgr.h"
#include "NewRpgFishing.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"

namespace
{
    constexpr uint32 SPELL_FISHING = 7620;
    constexpr uint32 SPELL_MILLING = 51005;
    constexpr uint32 SPELL_PROSPECTING = 31252;
    constexpr uint32 SPELL_DISENCHANT = 13262;
    // Spell::EffectMilling / EffectProspecting: one cast takes a stack of five
    constexpr float CONVERSION_STACK = 5.0f;
    // Spell::EffectTransmitted (fishing bobber): the bite comes 3, 7 or 13 s (equal odds) before the channel ends
    constexpr float FISHING_LAST_SECONDS[] = {3.0f, 7.0f, 13.0f};
    // GameObject::Use (fishing node): the catch never misses at the zone skill + 95
    constexpr int32 FISHING_NO_MISS_OVER_ZONE = 95;
    constexpr uint32 MAX_REFERENCE_DEPTH = 4;
    constexpr float COORD_OFFSET = 20000.0f;  // map coordinates lie within +-17066 (MAP_HALFSIZE)
    constexpr uint8 NO_SAFE_LEVEL = 255;
    constexpr uint8 ELITE_BIT = 0x80;
    constexpr uint8 LEVEL_MASK = 0x7F;
    // [gather] the continent rule (user 2026-11-08): Outland from 58 (the Dark Portal's level), Northrend from 68;
    // below 58 a gatherer stays on its own continent
    constexpr uint8 OUTLAND_LEVEL = 58;
    constexpr uint8 NORTHREND_LEVEL = 68;

    // the continent a spot is on: a map, map 530 split into Outland and the two starting isles (Azuremyst /
    // Bloodmyst, Eversong / Ghostlands), as TravelRouteMgr::Landmass splits it
    uint32 ContinentOf(uint32 mapId, WorldPosition const& pos)
    {
        if (mapId != MAP_OUTLAND)
            return mapId * 4;
        return TravelRouteMgr::Landmass(
            WorldPosition(mapId, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ()));
    }

    // the safety of an area by its own level (AreaTable area_level, the game's exploration level of the sub-area;
    // 0 = none given): a solo bot below orange con of it
    uint8 AreaSafeLevel(uint32 areaId);

    uint64 CellKey(uint32 mapId, float x, float y, float cell)
    {
        uint64 const cx = uint64((x + COORD_OFFSET) / cell);
        uint64 const cy = uint64((y + COORD_OFFSET) / cell);
        return (uint64(mapId) << 40) | (cx << 20) | cy;
    }

    float Dist(WorldPosition const& a, WorldPosition const& b)
    {
        float const dx = a.GetPositionX() - b.GetPositionX(), dy = a.GetPositionY() - b.GetPositionY(),
                    dz = a.GetPositionZ() - b.GetPositionZ();
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // expected items of one roll of a loot template (references unrolled, quest-only and junk-only entries skipped)
    void Expected(LootTemplate const* tmpl, std::map<uint32, float>& out, float scale, uint32 depth = 0)
    {
        if (!tmpl || depth > MAX_REFERENCE_DEPTH)
            return;
        tmpl->VisitEntries(
            [&](LootStoreItem const& e, float chance)
            {
                if (e.needs_quest || !(e.lootmode & LOOT_MODE_DEFAULT))
                    return;
                float const p = std::min(100.0f, chance) / 100.0f;
                if (e.reference)
                {
                    // LootTemplate::Process: a reference entry is processed maxcount times
                    Expected(LootTemplates_Reference.GetLootFor(uint32(std::abs(e.reference))), out,
                             scale * p * float(std::max<uint8>(1, e.maxcount)), depth + 1);
                    return;
                }
                out[e.itemid] += scale * p * (float(e.mincount) + float(e.maxcount)) / 2.0f;
            });
    }

    std::string AreaName(uint32 id)
    {
        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(id))
            return area->area_name[LOCALE_enUS];
        return std::to_string(id);
    }

    std::string ItemName(uint32 itemId)
    {
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId))
            return proto->Name1;
        return std::to_string(itemId);
    }

    char const* SkillName(uint32 skill)  // [craft432] the gathering skills of a climb (the decision lines' names)
    {
        switch (skill)
        {
            case SKILL_MINING:
                return "Mine";
            case SKILL_HERBALISM:
                return "Herb";
            case SKILL_SKINNING:
                return "Skin";
            default:
                return "skill";
        }
    }

    // the lowest bot level for which a spot is safe: hostiles of normal rank below orange con, elites gray
    uint8 MinSafeBotLevel(uint8 normal, uint8 elite)
    {
        uint32 const maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
        for (uint32 level = 1; level <= maxLevel; ++level)
            if (GuildGatherPlan::MaxSafeMobLevel(uint8(level)) >= normal &&
                (!elite || Acore::XP::GetGrayLevel(uint8(level)) >= elite))
                return uint8(level);
        return NO_SAFE_LEVEL;
    }

    uint8 AreaSafeLevel(uint32 areaId)
    {
        AreaTableEntry const* area = areaId ? sAreaTableStore.LookupEntry(areaId) : nullptr;
        if (!area || area->area_level <= 0)
            return 1;
        return MinSafeBotLevel(uint8(std::min<int32>(area->area_level, LEVEL_MASK)), 0);
    }

    // [stuck8] a zone's own floor: the lowest exploration level among its sub-areas (a zone's row has none; Darkshore
    // 11, Teldrassil 5); a bot walks into another zone only at a level that floor is safe for. b338 Shalaeth L1 was
    // sent from Teldrassil to Darkshore's Mist's Edge (area level 0) for Tangy Clam Meat, rode the boat to Auberdine
    // and was left in the sea there for three runs
    uint8 ZoneSafeLevel(uint32 zoneId)
    {
        static std::unordered_map<uint32, int32> floor;
        static std::once_flag built;
        std::call_once(built, []()
        {
            for (uint32 i = 0; i < sAreaTableStore.GetNumRows(); ++i)
            {
                AreaTableEntry const* area = sAreaTableStore.LookupEntry(i);
                if (!area || !area->zone || area->area_level <= 0)
                    continue;
                auto const [itr, added] = floor.emplace(area->zone, area->area_level);
                if (!added)
                    itr->second = std::min(itr->second, area->area_level);
            }
        });
        auto const itr = floor.find(zoneId);
        if (itr == floor.end())
            return 1;
        return MinSafeBotLevel(uint8(std::min<int32>(itr->second, LEVEL_MASK)), 0);
    }

    bool HostileToAlliance(CreatureTemplate const* tmpl)
    {
        FactionTemplateEntry const* ft = sFactionTemplateStore.LookupEntry(tmpl->faction);
        return ft && (ft->IsHostileToAlliancePlayers() || ft->IsHostileToPlayers());
    }

    bool Elite(CreatureTemplate const* tmpl)
    {
        return tmpl->rank == CREATURE_ELITE_ELITE || tmpl->rank == CREATURE_ELITE_RAREELITE ||
               tmpl->rank == CREATURE_ELITE_WORLDBOSS;
    }

    bool Attackable(CreatureTemplate const* tmpl)
    {
        return !(tmpl->unit_flags & (UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE)) &&
               !(tmpl->flags_extra & CREATURE_FLAG_EXTRA_CIVILIAN) && tmpl->type != CREATURE_TYPE_CRITTER;
    }

    // the map a spawn stands on is a world the guild can gather on: continents and dungeons / raids (no
    // battleground or arena: no PvP for these bots)
    bool GatherMap(uint32 mapId, bool& instance)
    {
        MapEntry const* map = sMapStore.LookupEntry(mapId);
        if (!map)
            return false;
        instance = map->IsDungeon();
        return map->IsContinent() || instance;
    }

    // the creature's base health at its middle level (CreatureBaseStats); 0: no stats for it
    float MobHp(CreatureTemplate const* tmpl)
    {
        uint8 const level = uint8((uint32(tmpl->minlevel) + uint32(tmpl->maxlevel)) / 2);
        CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(level, tmpl->unit_class);
        return stats ? float(stats->GenerateHealth(tmpl)) : 0.0f;
    }

    struct SpawnZone
    {
        uint32 zone{0};
        uint32 area{0};
    };

    void ReadZones(char const* table, std::unordered_map<uint32, SpawnZone>& out)
    {
        if (QueryResult result = WorldDatabase.Query("SELECT guid, zoneId, areaId FROM {} WHERE zoneId <> 0", table))
        {
            do
            {
                Field* f = result->Fetch();
                out[f[0].Get<uint32>()] = {f[1].Get<uint32>(), f[2].Get<uint32>()};
            } while (result->NextRow());
        }
    }

    SpawnZone ZoneOf(std::unordered_map<uint32, SpawnZone> const& db, uint32 spawnId, SpawnData const& data)
    {
        if (auto const itr = db.find(spawnId); itr != db.end())
            return itr->second;
        SpawnZone z;
        sMapMgr->GetZoneAndAreaId(PHASEMASK_NORMAL, z.zone, z.area, data.mapid, data.posX, data.posY, data.posZ);
        return z;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// static helpers
// ---------------------------------------------------------------------------------------------------------------------
char const* GuildGatherPlan::KindName(GatherKind kind)
{
    switch (kind)
    {
        case GatherKind::NODE:
            return "node";
        case GatherKind::SKIN:
            return "skin";
        case GatherKind::FISH:
            return "fish";
        case GatherKind::CHEST:
            return "chest";
        case GatherKind::DROP:
            return "drop";
    }
    return "?";
}

int32 GuildGatherPlan::SkinningReq(uint8 mobLevel)
{
    // Spell::CheckCast SPELL_EFFECT_SKINNING: below skill 100 the requirement is (level - 10) x 10, from 100 it is
    // level x 5; the lowest skill that passes is therefore below 100 only for mobs under level 20
    int32 const low = (int32(mobLevel) - 10) * 10;
    if (low < 100)
        return std::max(0, low);
    return int32(mobLevel) * 5;
}

// XPColorChar runs XP_RED (0) .. XP_GRAY: "below orange" is a greater value
uint8 GuildGatherPlan::MaxSafeMobLevel(uint8 botLevel)
{
    uint8 level = botLevel;
    while (level < NO_SAFE_LEVEL - 1 && Acore::XP::GetColorCode(botLevel, uint8(level + 1)) > XP_ORANGE)
        ++level;
    return level;
}

// ---------------------------------------------------------------------------------------------------------------------
// startup
// ---------------------------------------------------------------------------------------------------------------------
void GuildGatherPlan::Load()
{
    uint32 const start = getMSTime();
    places_.clear();
    placesByItem_.clear();
    gatherSkill_.clear();
    harvest_.clear();
    supply_.clear();  // [gather42]
    LoadPools();
    LoadHostiles();
    LoadNodesAndChests();
    LoadSkinning();
    LoadFishing();
    LoadDrops();
    // [ui11] the open-world harvest first (nodes, skinning, fishing): a chest never stands in for it. b-live: «Сбор:
    // Silverleaf x364 @ chest Tattered Chest» for every bot without Herbalism - the chest's loot group gives one of
    // seven herbs (~0.14 Silverleaf a chest), it opens without a skill, so it was the only place they could "gather" a
    // common herb at; and its skill 0 made Silverleaf read as gatherable by anyone (gatherSkill_)
    for (GatherPlace const& place : places_)
    {
        if (place.instance || place.kind == GatherKind::CHEST)
            continue;
        // [gather42] the same walk fills supply_: a mob's own drop is a farm the guild can work ([ench164] made it
        // one), a chest is not, so an item whose only places are chests is in neither set
        bool const drop = place.kind == GatherKind::DROP;
        for (auto const* yield : {&place.yield, &place.poolYield})
            for (auto const& [item, amount] : *yield)
                if (amount > 0.0f)
                {
                    supply_.insert(item);
                    if (!drop)
                        harvest_.insert(item);
                }
    }
    for (size_t i = 0; i < places_.size(); ++i)
    {
        places_[i].id = uint32(i + 1);
        std::map<uint32, float> all = places_[i].yield;
        for (auto const& [item, amount] : places_[i].poolYield)
            all[item] += amount;
        for (auto const& [item, amount] : all)
        {
            if (amount <= 0.0f)
                continue;
            // [ench164] a mob's own drop never stands in for them either (a humanoid's Light Leather)
            bool const chest = places_[i].kind == GatherKind::CHEST || places_[i].kind == GatherKind::DROP;
            if (chest && !places_[i].instance && harvest_.count(item))
                continue;  // [ui11] a herb / ore / leather / fish: its nodes, mobs and waters only
            placesByItem_[item].push_back(uint32(i));
            if (places_[i].instance)
                continue;
            auto const known = gatherSkill_.find(item);
            if (known == gatherSkill_.end() || places_[i].reqSkill < known->second.second)
                gatherSkill_[item] = {places_[i].skill, places_[i].reqSkill};
        }
    }
    LoadConversions();
    pools_.clear();
    goPool_.clear();
    creaturePool_.clear();
    hostileCells_.clear();

    std::map<GatherKind, std::pair<uint32, uint32>> counts;  // kind -> (open world, instance)
    for (GatherPlace const& p : places_)
        (p.instance ? counts[p.kind].second : counts[p.kind].first)++;
    for (auto const& [kind, n] : counts)
        LOG_INFO("playerbots", "[gather] index: {} places of kind {} ({} in instances)", n.first + n.second,
                 KindName(kind), n.second);
    LOG_INFO("playerbots", "[gather] index: {} gatherable items, {} converted reagents, {} disenchant mats in {} ms",
             gatherSkill_.size(), conversions_.size(), disenchantMats_.size(), GetMSTimeDiffToNow(start));
    loaded_ = true;
}

void GuildGatherPlan::LoadPools()
{
    if (QueryResult result = WorldDatabase.Query("SELECT entry, max_limit FROM pool_template"))
    {
        do
        {
            Field* f = result->Fetch();
            pools_[f[0].Get<uint32>()].maxLimit = f[1].Get<uint32>();
        } while (result->NextRow());
    }
    auto const members = [&](char const* sql, std::unordered_map<uint32, uint32>* spawnPool)
    {
        if (QueryResult result = WorldDatabase.Query(sql))
        {
            do
            {
                Field* f = result->Fetch();
                uint32 const child = f[0].Get<uint32>(), pool = f[1].Get<uint32>();
                ++pools_[pool].members;
                if (spawnPool)
                    (*spawnPool)[child] = pool;
                else
                    pools_[child].mother = pool;
            } while (result->NextRow());
        }
    };
    members("SELECT guid, pool_entry FROM pool_gameobject", &goPool_);
    members("SELECT guid, pool_entry FROM pool_creature", &creaturePool_);
    members("SELECT pool_id, mother_pool FROM pool_pool", nullptr);
}

float GuildGatherPlan::SpawnWeight(uint32 spawnId, bool gameObject) const
{
    auto const& index = gameObject ? goPool_ : creaturePool_;
    auto const itr = index.find(spawnId);
    if (itr == index.end())
        return 1.0f;
    // PoolMgr: a pool keeps max_limit of its members spawned (0: all of them); a member pool of a mother pool is
    // itself one of the mother's members
    float weight = 1.0f;
    uint32 pool = itr->second;
    for (uint32 depth = 0; pool && depth < MAX_REFERENCE_DEPTH; ++depth)
    {
        auto const p = pools_.find(pool);
        if (p == pools_.end())
            break;
        if (p->second.maxLimit && p->second.members)
            weight *= std::min(1.0f, float(p->second.maxLimit) / float(p->second.members));
        pool = p->second.mother;
    }
    return weight;
}

void GuildGatherPlan::LoadHostiles()
{
    std::vector<std::pair<uint32, Hostile>> all;
    std::vector<float> ranges;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        bool instance = false;
        if (!GatherMap(data.mapid, instance))
            continue;
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
        if (!tmpl || !HostileToAlliance(tmpl) || !Attackable(tmpl) || tmpl->detection_range <= 0.0f)
            continue;
        Hostile h{data.posX, data.posY, data.posZ, tmpl->detection_range, tmpl->maxlevel};
        // elites are kept apart by the top bit of the stored level (see DangerAt)
        if (Elite(tmpl))
            h.level = uint8(ELITE_BIT | std::min<uint8>(tmpl->maxlevel, LEVEL_MASK));
        ranges.push_back(tmpl->detection_range);
        all.emplace_back(data.mapid, h);
    }
    if (ranges.empty())
        return;
    // grid of the median detection range: each hostile is filed in every cell its range reaches, a spot then reads
    // its own cell only (a few huge ranges do not blow up every lookup)
    std::nth_element(ranges.begin(), ranges.begin() + ranges.size() / 2, ranges.end());
    float const cell = ranges[ranges.size() / 2];
    hostileCellSize_ = cell;
    for (auto const& [mapId, h] : all)
        for (float x = h.x - h.range; x <= h.x + h.range + cell; x += cell)
            for (float y = h.y - h.range; y <= h.y + h.range + cell; y += cell)
            {
                uint64 const key = CellKey(mapId, std::min(x, h.x + h.range), std::min(y, h.y + h.range), cell);
                auto& list = hostileCells_[key];
                if (list.empty() || list.back().x != h.x || list.back().y != h.y)
                    list.push_back(h);
            }
}

uint8 GuildGatherPlan::DangerAt(uint32 mapId, float x, float y, float z) const
{
    if (hostileCellSize_ <= 0.0f)
        return 1;
    uint8 normal = 0, elite = 0;
    auto const itr = hostileCells_.find(CellKey(mapId, x, y, hostileCellSize_));
    if (itr != hostileCells_.end())
        for (Hostile const& h : itr->second)
        {
            float const dx = h.x - x, dy = h.y - y, dz = h.z - z;
            if (dx * dx + dy * dy + dz * dz > h.range * h.range)
                continue;
            if (h.level & ELITE_BIT)
                elite = std::max<uint8>(elite, h.level & LEVEL_MASK);
            else
                normal = std::max(normal, h.level);
        }
    return MinSafeBotLevel(normal, elite);
}

void GuildGatherPlan::Finish(GatherPlace& place, std::vector<WorldPosition> const& points)
{
    if (points.empty())
        return;
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    for (WorldPosition const& p : points)
    {
        cx += p.GetPositionX();
        cy += p.GetPositionY();
        cz += p.GetPositionZ();
    }
    WorldPosition const centre(place.mapId, cx / float(points.size()), cy / float(points.size()),
                               cz / float(points.size()));
    float best = FLT_MAX;
    for (WorldPosition const& p : points)
        if (float const d = Dist(p, centre); d < best)
        {
            best = d;
            place.spot = p;
        }
    std::sort(place.spawnReq.begin(), place.spawnReq.end());
    // [questrate] the box its spawns spread over (the walk between the live ones, Rate)
    float minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX;
    for (WorldPosition const& p : points)
    {
        minX = std::min(minX, p.GetPositionX());
        maxX = std::max(maxX, p.GetPositionX());
        minY = std::min(minY, p.GetPositionY());
        maxY = std::max(maxY, p.GetPositionY());
    }
    place.areaYd2 = (maxX - minX) * (maxY - minY);
}

void GuildGatherPlan::LoadNodesAndChests()
{
    std::unordered_map<uint32, SpawnZone> zones;
    ReadZones("gameobject", zones);
    // (map, zone, entry) -> place index, and the spawn points of each
    std::map<std::tuple<uint32, uint32, uint32>, size_t> index;
    std::vector<std::vector<WorldPosition>> points;
    // [guild15] crash fix: places_ is shared by every loader; points / respawnSum are this loader's, from base on
    size_t const base = places_.size();
    std::vector<float> respawnSum;
    std::unordered_map<uint32, std::map<uint32, float>> lootCache;

    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        bool instance = false;
        if (!GatherMap(data.mapid, instance))
            continue;
        GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(data.id);
        if (!go || go->type != GAMEOBJECT_TYPE_CHEST || go->chest.questId)
            continue;
        GatherKind kind = GatherKind::CHEST;
        uint32 skill = 0;
        int32 req = 0;
        bool keyOnly = false;
        if (LockEntry const* lock = go->GetLockId() ? sLockStore.LookupEntry(go->GetLockId()) : nullptr)
        {
            bool skillCase = false, keyCase = false;
            for (uint8 j = 0; j < MAX_LOCK_CASE; ++j)
            {
                if (lock->Type[j] == LOCK_KEY_ITEM && lock->Index[j])
                    keyCase = true;
                if (lock->Type[j] != LOCK_KEY_SKILL)
                    continue;
                uint32 const s = SkillByLockType(LockType(lock->Index[j]));
                if (s == SKILL_HERBALISM || s == SKILL_MINING)
                {
                    kind = GatherKind::NODE;
                    skill = s;
                    req = int32(std::max(1u, lock->Skill[j]));  // GatherNodeMgr: a 0 lock skill still needs 1
                    skillCase = true;
                    break;
                }
                if (s == SKILL_LOCKPICKING)
                {
                    skill = s;
                    req = int32(lock->Skill[j]);
                    skillCase = true;
                }
                else if (s == SKILL_NONE)  // LOCKTYPE_OPEN and the like: any character opens it
                    skillCase = true;
            }
            keyOnly = keyCase && !skillCase;
        }
        if (keyOnly)
            continue;
        uint32 const lootId = go->GetLootId();
        if (!lootId)
            continue;
        auto cached = lootCache.find(lootId);
        if (cached == lootCache.end())
        {
            std::map<uint32, float> yield;
            Expected(LootTemplates_Gameobject.GetLootFor(lootId), yield, 1.0f);
            cached = lootCache.emplace(lootId, std::move(yield)).first;
        }
        if (cached->second.empty())
            continue;
        SpawnZone const z = ZoneOf(zones, spawnId, data);
        auto const key = std::make_tuple(uint32(data.mapid), z.zone, data.id);
        auto itr = index.find(key);
        if (itr == index.end())
        {
            GatherPlace place;
            place.kind = kind;
            place.mapId = data.mapid;
            place.zoneId = z.zone;
            place.areaId = z.area;
            place.instance = instance;
            place.skill = skill;
            place.reqSkill = req;
            place.entries.push_back(data.id);
            place.yield = cached->second;
            itr = index.emplace(key, places_.size()).first;
            places_.push_back(std::move(place));
            points.emplace_back();
            respawnSum.push_back(0.0f);
        }
        GatherPlace& place = places_[itr->second];
        float const weight = SpawnWeight(spawnId, true);
        ++place.spawns;
        place.liveShare += weight;
        respawnSum[itr->second - base] += float(std::max(0, data.spawntimesecs));
        // [gather] the spot's hostiles and the area's own level (b337: L1 gatherers sent into L5-10 areas)
        place.spawnReq.emplace_back(
            std::max(DangerAt(data.mapid, data.posX, data.posY, data.posZ), AreaSafeLevel(z.area)), req, weight);
        points[itr->second - base].emplace_back(data.mapid, data.posX, data.posY, data.posZ);
    }
    for (auto const& [key, i] : index)
    {
        GatherPlace& place = places_[i];
        place.respawnSec = place.spawns ? respawnSum[i - base] / float(place.spawns) : 0.0f;
        Finish(place, points[i - base]);
    }
}

void GuildGatherPlan::LoadSkinning()
{
    std::unordered_map<uint32, SpawnZone> zones;
    ReadZones("creature", zones);
    std::map<std::tuple<uint32, uint32, uint32>, size_t> index;
    std::vector<std::vector<WorldPosition>> points;
    // [guild15] crash fix: places_ is shared by every loader; points / respawnSum are this loader's, from base on
    size_t const base = places_.size();
    std::vector<float> respawnSum;
    std::unordered_map<uint32, std::map<uint32, float>> lootCache;

    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        bool instance = false;
        if (!GatherMap(data.mapid, instance))
            continue;
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
        if (!tmpl || !tmpl->SkinLootId || !Attackable(tmpl))
            continue;
        auto cached = lootCache.find(tmpl->SkinLootId);
        if (cached == lootCache.end())
        {
            std::map<uint32, float> yield;
            Expected(LootTemplates_Skinning.GetLootFor(tmpl->SkinLootId), yield, 1.0f);
            cached = lootCache.emplace(tmpl->SkinLootId, std::move(yield)).first;
        }
        if (cached->second.empty())
            continue;
        SpawnZone const z = ZoneOf(zones, spawnId, data);
        auto const key = std::make_tuple(uint32(data.mapid), z.zone, data.id);
        auto itr = index.find(key);
        if (itr == index.end())
        {
            GatherPlace place;
            place.kind = GatherKind::SKIN;
            place.mapId = data.mapid;
            place.zoneId = z.zone;
            place.areaId = z.area;
            place.instance = instance;
            // CreatureTemplate::GetRequiredLootSkill: herbalism / mining / engineering by type flags, else skinning
            place.skill = tmpl->GetRequiredLootSkill();
            place.reqSkill = SkinningReq(tmpl->minlevel);
            place.levelMin = tmpl->minlevel;
            place.levelMax = tmpl->maxlevel;
            place.killHp = MobHp(tmpl);
            place.entries.push_back(data.id);
            place.yield = cached->second;
            itr = index.emplace(key, places_.size()).first;
            places_.push_back(std::move(place));
            points.emplace_back();
            respawnSum.push_back(0.0f);
        }
        GatherPlace& place = places_[itr->second];
        float const weight = SpawnWeight(spawnId, false);
        // the skinned mob is itself the fight: it counts like a hostile at its spot (its highest level)
        uint8 const around = DangerAt(data.mapid, data.posX, data.posY, data.posZ);
        uint8 const self = Elite(tmpl) ? MinSafeBotLevel(0, tmpl->maxlevel) : MinSafeBotLevel(tmpl->maxlevel, 0);
        ++place.spawns;
        place.liveShare += weight;
        respawnSum[itr->second - base] += float(data.spawntimesecs);
        place.spawnReq.emplace_back(std::max({around, self, AreaSafeLevel(z.area)}), SkinningReq(tmpl->maxlevel),
                                    weight);
        points[itr->second - base].emplace_back(data.mapid, data.posX, data.posY, data.posZ);
    }
    for (auto const& [key, i] : index)
    {
        GatherPlace& place = places_[i];
        place.respawnSec = place.spawns ? respawnSum[i - base] / float(place.spawns) : 0.0f;
        Finish(place, points[i - base]);
    }
}

// [ench164] coordinator 2026-11-11: the trade goods a mob drops itself (meat, cloth) are a gathering source like a
// skinnable mob - b442 the cooks', tailors' and blacksmiths' skill-up walks stopped at "Coyote Meat x1 / Crawler Meat /
// Wool Cloth / Mageweave Cloth: neither held nor gathered in the guild" (196 such lines) while the members killed those
// mobs. A place per map, zone and creature of the open world (no instance: a five's run is no solo grind), its
// creature_loot_template's trade goods per kill (Expected: references unrolled, quest-only drops skipped); not a mob
// friendly to the Alliance (its guards and townsfolk)
void GuildGatherPlan::LoadDrops()
{
    std::unordered_map<uint32, SpawnZone> zones;
    ReadZones("creature", zones);
    std::map<std::tuple<uint32, uint32, uint32>, size_t> index;
    std::vector<std::vector<WorldPosition>> points;
    size_t const base = places_.size();
    std::vector<float> respawnSum;
    std::unordered_map<uint32, std::map<uint32, float>> lootCache;

    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        bool instance = false;
        if (!GatherMap(data.mapid, instance) || instance)
            continue;
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
        if (!tmpl || !tmpl->lootid || !Attackable(tmpl))
            continue;
        if (FactionTemplateEntry const* ft = sFactionTemplateStore.LookupEntry(tmpl->faction);
            !ft || ((ft->ourMask | ft->friendlyMask) & FACTION_MASK_ALLIANCE))
            continue;
        auto cached = lootCache.find(tmpl->lootid);
        if (cached == lootCache.end())
        {
            std::map<uint32, float> all, goods;
            Expected(LootTemplates_Creature.GetLootFor(tmpl->lootid), all, 1.0f);
            // [ench167] a dropped container opened by its use spell (the clams: Thick-shelled Clam's 58168 gives
            // Tangy Clam Meat by spell_loot_template, SPELL_EFFECT_CREATE_RANDOM_ITEM) gives its contents per kill
            // too - b443 the cooks' walk to 175 priced Tangy Clam Meat (Goblin Deviled Clams, the trainer's recipe
            // from 160) at the 0.02-0.1 % of its direct drops: "to 175 in 150616 min"
            std::map<uint32, float> contents;
            for (auto const& [item, amount] : all)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
                    proto && proto->Spells[0].SpellId > 0 &&
                    proto->Spells[0].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE &&
                    LootTemplates_Spell.HaveLootFor(uint32(proto->Spells[0].SpellId)))
                    Expected(LootTemplates_Spell.GetLootFor(uint32(proto->Spells[0].SpellId)), contents, amount);
            for (auto const& [item, amount] : contents)
                all[item] += amount;
            for (auto const& [item, amount] : all)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
                    proto && proto->Class == ITEM_CLASS_TRADE_GOODS && proto->Quality >= ITEM_QUALITY_NORMAL &&
                    amount > 0.0f)
                    goods[item] = amount;
            cached = lootCache.emplace(tmpl->lootid, std::move(goods)).first;
        }
        if (cached->second.empty())
            continue;
        float const hp = MobHp(tmpl);
        if (hp <= 0.0f)
            continue;
        SpawnZone const z = ZoneOf(zones, spawnId, data);
        auto const key = std::make_tuple(uint32(data.mapid), z.zone, data.id);
        auto itr = index.find(key);
        if (itr == index.end())
        {
            GatherPlace place;
            place.kind = GatherKind::DROP;
            place.mapId = data.mapid;
            place.zoneId = z.zone;
            place.areaId = z.area;
            place.levelMin = tmpl->minlevel;
            place.levelMax = tmpl->maxlevel;
            place.killHp = hp;
            place.entries.push_back(data.id);
            place.yield = cached->second;
            itr = index.emplace(key, places_.size()).first;
            places_.push_back(std::move(place));
            points.emplace_back();
            respawnSum.push_back(0.0f);
        }
        GatherPlace& place = places_[itr->second];
        float const weight = SpawnWeight(spawnId, false);
        // the mob is itself the fight: it counts like a hostile at its spot (its highest level), as a skinned mob
        uint8 const around = DangerAt(data.mapid, data.posX, data.posY, data.posZ);
        uint8 const self = Elite(tmpl) ? MinSafeBotLevel(0, tmpl->maxlevel) : MinSafeBotLevel(tmpl->maxlevel, 0);
        ++place.spawns;
        place.liveShare += weight;
        respawnSum[itr->second - base] += float(data.spawntimesecs);
        place.spawnReq.emplace_back(std::max({around, self, AreaSafeLevel(z.area)}), 0, weight);
        points[itr->second - base].emplace_back(data.mapid, data.posX, data.posY, data.posZ);
    }
    for (auto const& [key, i] : index)
    {
        GatherPlace& place = places_[i];
        place.respawnSec = place.spawns ? respawnSum[i - base] / float(place.spawns) : 0.0f;
        Finish(place, points[i - base]);
    }
}

void GuildGatherPlan::LoadFishing()
{
    // water markers: fishing pools (gameobject type 25) and creatures that can only swim
    std::unordered_map<uint32, SpawnZone> goZones, creatureZones;
    ReadZones("gameobject", goZones);
    ReadZones("creature", creatureZones);
    struct Marker
    {
        WorldPosition pos;
        uint32 zone;
        uint32 area;
        bool instance;
        uint32 poolLoot;  // a pool's own loot, 0: open water marker
        float poolCatches;
    };
    std::vector<Marker> markers;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        bool instance = false;
        GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(data.id);
        if (!go || go->type != GAMEOBJECT_TYPE_FISHINGHOLE || !GatherMap(data.mapid, instance))
            continue;
        SpawnZone const z = ZoneOf(goZones, spawnId, data);
        float const catches =
            (float(go->fishinghole.minSuccessOpens) + float(go->fishinghole.maxSuccessOpens)) / 2.0f *
            SpawnWeight(spawnId, true);
        markers.push_back({WorldPosition(data.mapid, data.posX, data.posY, data.posZ), z.zone, z.area, instance,
                           go->fishinghole.lootId, catches});
    }
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        bool instance = false;
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
        if (!tmpl || !tmpl->Movement.IsSwimAllowed() || tmpl->Movement.IsGroundAllowed() ||
            tmpl->Movement.IsFlightAllowed() || !GatherMap(data.mapid, instance))
            continue;
        SpawnZone const z = ZoneOf(creatureZones, spawnId, data);
        markers.push_back({WorldPosition(data.mapid, data.posX, data.posY, data.posZ), z.zone, z.area, instance, 0, 0});
    }

    // GameObject::GetFishLoot: the area's loot, else the zone's, else zone 1; the skill: the area's, else the zone's
    std::map<std::pair<uint32, uint32>, size_t> index;  // (map, loot entry) -> place
    std::vector<std::vector<WorldPosition>> points;
    // [guild15] crash fix: places_ is shared by every loader; points / respawnSum are this loader's, from base on
    size_t const base = places_.size();
    std::map<std::pair<size_t, uint32>, float> poolLoot;  // (place, pool loot) -> catches per pool round
    for (Marker const& m : markers)
    {
        uint32 lootEntry = 0;
        for (uint32 id : {m.area, m.zone})
            if (id && LootTemplates_Fishing.HaveLootFor(id))
            {
                lootEntry = id;
                break;
            }
        if (!lootEntry)
            continue;
        int32 zoneSkill = sObjectMgr->GetFishingBaseSkillLevel(m.area);
        if (!zoneSkill)
            zoneSkill = sObjectMgr->GetFishingBaseSkillLevel(m.zone);
        auto const key = std::make_pair(m.pos.GetMapId(), lootEntry);
        auto itr = index.find(key);
        if (itr == index.end())
        {
            GatherPlace place;
            place.kind = GatherKind::FISH;
            place.mapId = m.pos.GetMapId();
            place.zoneId = m.zone;
            place.areaId = lootEntry == m.area ? m.area : 0;
            place.instance = m.instance;
            place.skill = SKILL_FISHING;
            place.reqSkill = zoneSkill;
            place.entries.push_back(lootEntry);
            Expected(LootTemplates_Fishing.GetLootFor(lootEntry), place.yield, 1.0f);
            itr = index.emplace(key, places_.size()).first;
            places_.push_back(std::move(place));
            points.emplace_back();
        }
        GatherPlace& place = places_[itr->second];
        ++place.spawns;
        place.spawnReq.emplace_back(std::max(DangerAt(m.pos.GetMapId(), m.pos.GetPositionX(), m.pos.GetPositionY(),
                                                      m.pos.GetPositionZ()),
                                             AreaSafeLevel(m.area)),
                                    place.reqSkill, 1.0f);
        points[itr->second - base].push_back(m.pos);
        if (m.poolLoot)
            poolLoot[{itr->second, m.poolLoot}] += m.poolCatches;
    }
    // a pool's catches are on top of the open water's (every cast into it catches: GameObject::Use)
    for (auto const& [key, catches] : poolLoot)
    {
        GatherPlace& place = places_[key.first];
        std::map<uint32, float> pool;
        Expected(LootTemplates_Gameobject.GetLootFor(key.second), pool, 1.0f);
        for (auto const& [item, n] : pool)
            place.poolYield[item] += n * catches;
    }
    for (auto const& [key, i] : index)
        Finish(places_[i], points[i - base]);
}

void GuildGatherPlan::LoadConversions()
{
    conversions_.clear();
    convertedFrom_.clear();
    disenchantMats_.clear();
    auto const add = [&](LootStore const& store, uint32 skillLine, uint32 spellId)
    {
        for (auto const& [rawId, tmpl] : store.GetTemplates())
        {
            ItemTemplate const* raw = sObjectMgr->GetItemTemplate(rawId);
            if (!raw)
                continue;
            std::map<uint32, float> out;
            Expected(tmpl, out, 1.0f);
            for (auto const& [item, n] : out)
                if (n > 0.0f)
                    conversions_[item].push_back(
                        {rawId, CONVERSION_STACK / n, skillLine, raw->RequiredSkillRank, spellId});
        }
    };
    add(LootTemplates_Milling, SKILL_INSCRIPTION, SPELL_MILLING);
    add(LootTemplates_Prospecting, SKILL_JEWELCRAFTING, SPELL_PROSPECTING);
    for (auto const& [deId, tmpl] : LootTemplates_Disenchant.GetTemplates())
    {
        std::map<uint32, float> out;
        Expected(tmpl, out, 1.0f);
        for (auto const& [item, n] : out)
            if (n > 0.0f && disenchantMats_.insert(item).second)
                conversions_[item].push_back({0, 0.0f, SKILL_ENCHANTING, 0, SPELL_DISENCHANT});
    }
    // best first: a raw the guild can gather, then the fewest raw per unit, then the lowest crafter skill
    for (auto& [item, list] : conversions_)
    {
        std::sort(list.begin(), list.end(),
                  [&](GatherConversion const& a, GatherConversion const& b)
                  {
                      bool const ga = a.raw && gatherSkill_.count(a.raw), gb = b.raw && gatherSkill_.count(b.raw);
                      if (ga != gb)
                          return ga;
                      if ((a.raw != 0) != (b.raw != 0))
                          return a.raw != 0;
                      if (a.rawPerUnit != b.rawPerUnit)
                          return a.rawPerUnit < b.rawPerUnit;
                      return a.reqSkill < b.reqSkill;
                  });
        if (list.front().raw)
            convertedFrom_[item] = {{list.front().raw, list.front().rawPerUnit}};
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// lookups
// ---------------------------------------------------------------------------------------------------------------------
GatherPlace const* GuildGatherPlan::Place(uint32 placeId) const
{
    return placeId && placeId <= places_.size() ? &places_[placeId - 1] : nullptr;
}

bool GuildGatherPlan::Gatherable(uint32 itemId, uint32* skill, int32* reqSkill) const
{
    auto const itr = gatherSkill_.find(itemId);
    if (itr == gatherSkill_.end())
        return false;
    if (skill)
        *skill = itr->second.first;
    if (reqSkill)
        *reqSkill = itr->second.second;
    return true;
}

bool GuildGatherPlan::Supplied(uint32 itemId) const
{
    return supply_.count(itemId) != 0;  // [gather42]
}

std::vector<GatherPlace const*> GuildGatherPlan::PlacesFor(uint32 itemId) const
{
    std::vector<GatherPlace const*> out;
    auto const itr = placesByItem_.find(itemId);
    if (itr != placesByItem_.end())
        for (uint32 i : itr->second)
            if (i < places_.size())
                out.push_back(&places_[i]);
    return out;
}

GatherConversion const* GuildGatherPlan::ConversionFor(uint32 itemId) const
{
    auto const itr = conversions_.find(itemId);
    return itr == conversions_.end() || itr->second.empty() ? nullptr : &itr->second.front();
}

std::vector<GatherConversion> const* GuildGatherPlan::Conversions(uint32 itemId) const
{
    auto const itr = conversions_.find(itemId);
    return itr == conversions_.end() ? nullptr : &itr->second;
}

std::vector<std::pair<uint32, float>> const* GuildGatherPlan::ConvertedFrom(uint32 itemId) const
{
    auto const itr = convertedFrom_.find(itemId);
    return itr == convertedFrom_.end() ? nullptr : &itr->second;
}

std::map<uint32, float> GuildGatherPlan::DisenchantYield(ItemTemplate const* proto) const
{
    std::map<uint32, float> out;
    if (proto && proto->DisenchantID)
        Expected(LootTemplates_Disenchant.GetLootFor(proto->DisenchantID), out, 1.0f);
    return out;
}

std::vector<GatherPlace const*> GuildGatherPlan::InstancePlaces(uint32 mapId) const
{
    std::vector<GatherPlace const*> out;
    for (GatherPlace const& p : places_)
        if (p.instance && p.mapId == mapId)
            out.push_back(&p);
    return out;
}

std::string GuildGatherPlan::PlaceName(GatherPlace const& place) const
{
    std::string what;
    if (!place.entries.empty())
    {
        if (place.kind == GatherKind::SKIN || place.kind == GatherKind::DROP)
        {
            if (CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(place.entries.front()))
                what = t->Name;
        }
        else if (place.kind == GatherKind::FISH)
            what = "water";
        else if (GameObjectTemplate const* t = sObjectMgr->GetGameObjectTemplate(place.entries.front()))
            what = t->name;
    }
    std::string text = std::string(KindName(place.kind)) + " " + what + " @ " + AreaName(place.zoneId);
    if (place.areaId && place.areaId != place.zoneId)
        text += "/" + AreaName(place.areaId);
    return text + " #" + std::to_string(place.id);
}

bool GuildGatherPlan::Assigned(uint32 botLow, GatherAssignment& out) const
{
    std::lock_guard<std::mutex> guard(lock_);
    auto const itr = assigned_.find(botLow);
    if (itr == assigned_.end())
        return false;
    out = itr->second;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// the assignment (world thread)
// ---------------------------------------------------------------------------------------------------------------------
float GuildGatherPlan::Rate(GatherPlace const& place, uint32 itemId, Player* bot, uint32 others) const
{
    auto const y = place.yield.find(itemId);
    auto const py = place.poolYield.find(itemId);
    float const perUnit = y != place.yield.end() ? y->second : 0.0f;
    float const perPool = py != place.poolYield.end() ? py->second : 0.0f;
    if (perUnit <= 0.0f && perPool <= 0.0f)
        return 0.0f;
    if (place.skill && !bot->HasSkill(place.skill))
        return 0.0f;
    int32 const skill = place.skill ? int32(bot->GetSkillValue(place.skill)) : 0;
    uint8 const level = bot->GetLevel();

    if (place.kind == GatherKind::FISH)
    {
        // [gather] the bot's own Fishing Pole (bought at a vendor, NewRpgProfession): NewRpgFishing gives the task
        // back without one
        if (!NewRpgFishing::HasPole(bot))
            return 0.0f;
        // a cast lasts until the bite (the channel minus 3, 7 or 13 s); GameObject::Use: the catch chance is
        // (skill / (zone skill + 95))^2, at least 1 %, 100 % into a pool
        bool const safe = std::any_of(place.spawnReq.begin(), place.spawnReq.end(),
                                      [level](auto const& s) { return std::get<0>(s) <= level; });
        if (!safe)
            return 0.0f;
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(SPELL_FISHING);
        float const channel = spell ? float(spell->GetDuration()) / 1000.0f : 0.0f;
        float bite = 0.0f;
        for (float s : FISHING_LAST_SECONDS)
            bite += s;
        float const castSec = channel - bite / float(std::size(FISHING_LAST_SECONDS));
        if (castSec <= 0.0f)
            return 0.0f;
        int32 const noMiss = place.reqSkill + FISHING_NO_MISS_OVER_ZONE;
        float const casts = 3600.0f / castSec;
        // [econ1] b350: Fishing 1/75 on 21 members with a pole and nobody fished - the rate took the catch chance
        // at today's skill for the whole hour ((1/25)^2 -> the 1 % floor at the starting zones' -70: ~2 fish / h,
        // never worth a task). The core raises the skill at every bobber click before the catch roll
        // (GameObject::Use -> Player::UpdateFishingSkill, getProbabilityOfLevelUp: 100 % below 115, then 1/2 ...
        // 1/12, up to the rank cap), so the hour's chance is the mean over its casts as the skill grows
        static constexpr uint32 LEVEL_UP_BOUNDS[] = {115, 135, 160, 190, 215, 295, 315, 355, 425, 450};
        static constexpr float LEVEL_UP_DENS[] = {1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 1};
        float const cap = float(bot->GetMaxSkillValue(SKILL_FISHING));
        float const gain = float(std::max<uint32>(1, sWorld->getIntConfig(CONFIG_SKILL_GAIN_GATHERING)));
        float s = float(skill), chanceSum = 0.0f;
        uint32 const n = uint32(casts);
        // the same (skill, cap, zone) gives the same mean: the greedy assignment asks it many times per update
        thread_local std::map<std::tuple<int32, uint32, int32, uint32>, float> memo;
        auto const key = std::make_tuple(skill, uint32(cap), noMiss, n);
        if (auto const hit = memo.find(key); hit != memo.end())
            return casts * hit->second * perUnit + perPool * 3600.0f / castSec /
                                                       float(std::max<uint32>(1, place.spawns)) / float(others + 1);
        for (uint32 i = 0; i < n; ++i)
        {
            float const c = noMiss > 0 && s < float(noMiss) ? std::pow(s / float(noMiss), 2.0f) : 1.0f;
            chanceSum += std::max(c, 0.01f);
            if (s < cap && s > 0.0f)
            {
                size_t k = 0;
                while (k < std::size(LEVEL_UP_BOUNDS) && uint32(s) > LEVEL_UP_BOUNDS[k])
                    ++k;
                s = std::min(cap, s + gain / LEVEL_UP_DENS[k]);
            }
        }
        float const chance = n ? chanceSum / float(n) : 0.01f;
        if (memo.size() > 4096)
            memo.clear();
        memo[key] = chance;
        // pools are shared like nodes; open water is not
        return casts * chance * perUnit + perPool * 3600.0f / castSec / float(std::max<uint32>(1, place.spawns)) /
                                              float(others + 1);
    }

    return Visits(place, bot, others) * perUnit;
}

float GuildGatherPlan::Visits(GatherPlace const& place, Player* bot, uint32 others) const
{
    if (place.kind == GatherKind::FISH || (place.skill && !bot->HasSkill(place.skill)))
        return 0.0f;
    int32 const skill = place.skill ? int32(bot->GetSkillValue(place.skill)) : 0;
    uint8 const level = bot->GetLevel();
    // nodes, chests, skinnable mobs: what respawns at the spawns this bot may work (level safety, skill)
    float live = 0.0f;
    for (auto const& [minLevel, req, weight] : place.spawnReq)
        if (minLevel <= level && req <= skill)
            live += weight;
    if (live <= 0.0f || place.respawnSec <= 0.0f)
        return 0.0f;
    float const rate = live * 3600.0f / place.respawnSec / float(others + 1);
    // [questrate] what the bot itself works per hour, by formula (b338: the supply alone - every live spawn of a whole
    // zone, capped only by a measured gathering pace - gave one L8 657 Light Leather / h at Elder Crag Boars): per
    // spawn the walk to the next live one of its share (spread over the spawns' box: the 2D mean nearest distance
    // 0.5 sqrt(area / live)), the kill of a skinned mob (its base health over the bot's EstimateDps), the gather cast
    float const mine = live / float(others + 1);
    float const step = place.areaYd2 > 0.0f ? 0.5f * std::sqrt(place.areaYd2 / mine) : 0.0f;
    float work = step / std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
    uint32 castSpell = 0;
    if (place.kind == GatherKind::SKIN)
        castSpell = SPELL_SKINNING_BASE;
    else if (place.skill == SKILL_HERBALISM)
        castSpell = SPELL_HERB_GATHERING_BASE;
    else if (place.skill == SKILL_MINING)
        castSpell = SPELL_MINING_BASE;
    if (SpellInfo const* cast = castSpell ? sSpellMgr->GetSpellInfo(castSpell) : nullptr)
        work += float(cast->CalcCastTime()) / 1000.0f;
    // the kill of a skinned mob, [ench164] of a mob whose own loot it is (a drop place: the loot chance per kill x the
    // kills - Expected's chance x (min + max) / 2 per spawn): its base health over the bot's dps
    if (place.kind == GatherKind::SKIN || place.kind == GatherKind::DROP)
    {
        float const dps = DpsEstimate::EstimateDps(bot, nullptr, DpsEstimate::BuffContext::Solo);
        if (place.killHp <= 0.0f || dps <= 0.0f)
            return 0.0f;
        work += place.killHp / dps;
    }
    return work > 0.0f ? std::min(rate, 3600.0f / work) : rate;
}

float GuildGatherPlan::SkillUpChance(uint32 skill, int32 value, int32 req)
{
    if (value < req)
        return 0.0f;
    // SkillGainChance(value, req + 100, req + 50, req + 25): per mille, rolled against irand(1, 1000)
    ServerConfigs const band = value >= req + 100  ? CONFIG_SKILL_CHANCE_GREY
                                 : value >= req + 50 ? CONFIG_SKILL_CHANCE_GREEN
                                 : value >= req + 25 ? CONFIG_SKILL_CHANCE_YELLOW
                                                     : CONFIG_SKILL_CHANCE_ORANGE;
    uint32 chance = sWorld->getIntConfig(band) * 10;
    // mining and skinning lose half the chance per step of skill when the steps are set
    uint32 const steps = skill == SKILL_MINING     ? sWorld->getIntConfig(CONFIG_SKILL_CHANCE_MINING_STEPS)
                         : skill == SKILL_SKINNING ? sWorld->getIntConfig(CONFIG_SKILL_CHANCE_SKINNING_STEPS)
                                                   : 0;
    if (steps)
    {
        uint32 const shift = uint32(value) / steps;
        chance = shift >= 32 ? 0 : chance >> shift;
    }
    return float(std::min<uint32>(chance, 1000)) / 1000.0f;
}

bool GuildGatherPlan::ClimbLeg(GatherPlace const& place, Player* bot, uint32 others, int32 target, float& points,
                               float& hours) const
{
    points = hours = 0.0f;
    if (!place.skill || !bot->HasSkill(place.skill))
        return false;
    int32 const start = int32(bot->GetPureSkillValue(place.skill));
    int32 const end = std::min(target, int32(bot->GetPureMaxSkillValue(place.skill)));
    if (start >= end)
        return false;
    float const visits = Visits(place, bot, others);
    if (visits <= 0.0f)
        return false;
    // the spawns it works now (Visits' set), each roll at the core's chance for the skill it holds by then
    int32 const workSkill = int32(bot->GetSkillValue(place.skill));
    uint8 const level = bot->GetLevel();
    int32 const gain = int32(std::max<uint32>(1, sWorld->getIntConfig(CONFIG_SKILL_GAIN_GATHERING)));
    std::map<int32, float> byReq;  // the worked spawns' weight per skill requirement (a place: one or a few)
    float weights = 0.0f;
    for (auto const& [minLevel, req, weight] : place.spawnReq)
        if (minLevel <= level && req <= workSkill)
        {
            byReq[req] += weight;
            weights += weight;
        }
    if (weights <= 0.0f)
        return false;
    for (int32 v = start; v < end; v += gain)
    {
        float chance = 0.0f;
        for (auto const& [req, weight] : byReq)
            chance += weight * SkillUpChance(place.skill, v, req);
        if (chance <= 0.0f)
            break;
        hours += weights / (chance * visits);  // the visits one skill-up takes at this skill
        points += float(std::min(gain, end - v));
    }
    return points > 0.0f;
}

float GuildGatherPlan::TravelSeconds(Player* bot, GatherPlace const& place) const
{
    float const speed = bot->GetSpeed(MOVE_RUN);
    if (bot->GetMapId() == place.mapId)
        return Dist(WorldPosition(bot), place.spot) / (speed > 0.0f ? speed : 1.0f);
    TravelRouteMgr::Leg leg;
    float seconds = 0.0f;
    if (!TravelRouteMgr::instance().FindRoute(bot, place.spot, leg, seconds))
        return FLT_MAX;
    return seconds;
}

bool GuildGatherPlan::ContinentAllowed(Player* bot, uint32 mapId, WorldPosition const& spot)
{
    uint8 const level = bot->GetLevel();
    uint32 const there = ContinentOf(mapId, spot);
    if (there == MAP_OUTLAND * 4 && level < OUTLAND_LEVEL)
        return false;
    if (mapId == MAP_NORTHREND && level < NORTHREND_LEVEL)
        return false;
    // b337: below 58 the portals / boats to another continent sent L1 gatherers from Teldrassil and Azuremyst to the
    // Eastern Kingdoms (MOVE_FAR_LOOP at the Darnassus and Exodar portals, deaths on the walks there)
    return level >= OUTLAND_LEVEL || there == ContinentOf(bot->GetMapId(), WorldPosition(bot));
}

void GuildGatherPlan::Refuse(Player* bot, uint32 placeId, std::string const& why)
{
    if (!bot || !placeId)
        return;
    GatherPlace const* place = Place(placeId);
    {
        std::lock_guard<std::mutex> guard(lock_);
        refused_[{bot->GetGUID().GetCounter(), placeId}] = bot->GetLevel();
    }
    LOG_INFO("playerbots", "[gather] refuse {} L{} place {}: {} (off until its next level)", bot->GetName(),
             bot->GetLevel(), place ? PlaceName(*place) : std::to_string(placeId), why);
}

void GuildGatherPlan::Unrefuse(uint32 botLow, uint32 placeId)
{
    std::lock_guard<std::mutex> guard(lock_);
    refused_.erase({botLow, placeId});
}

std::vector<GatherAssignment> GuildGatherPlan::Assign(std::map<uint32, uint32> const& shortfall,
                                                      std::vector<Player*> const& gatherers,
                                                      std::map<uint32, float> const& questPace,
                                                      std::map<uint32, float> const& unitValue,
                                                      std::map<uint32, std::string> const& questWhy,
                                                      std::set<uint32> const& roleFishers,
                                                      std::vector<GatherClimb> const& climbs)
{
    std::vector<GatherAssignment> out;
    if (!loaded_)
        return out;

    std::map<std::pair<uint32, uint32>, uint8> refused;
    std::unordered_map<uint32, GatherAssignment> previous;  // [craft432] last update's assignments (the keep pass)
    {
        std::lock_guard<std::mutex> guard(lock_);
        previous = assigned_;
        // a level-up (or a logout) gives every refused place another try
        for (auto itr = refused_.begin(); itr != refused_.end();)
        {
            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(itr->first.first));
            if (!bot || bot->GetLevel() != itr->second)
                itr = refused_.erase(itr);
            else
                ++itr;
        }
        refused = refused_;
    }

    struct Option
    {
        size_t place{0};
        float travel{0.0f};
    };
    struct Skipped
    {
        uint32 continent{0};
        uint32 refused{0};
        uint32 unsafeOrSkill{0};
        uint32 noRoute{0};
    };
    // per bot and item: the places it can work (travel computed once per place)
    std::map<std::pair<size_t, uint32>, std::vector<Option>> options;
    std::map<std::pair<size_t, uint32>, float> routeCache;  // (bot, map) -> seconds (cross-map routes only)
    std::vector<Skipped> skipped(gatherers.size());
    for (size_t b = 0; b < gatherers.size(); ++b)
    {
        Player* bot = gatherers[b];
        uint32 const low = bot->GetGUID().GetCounter();
        for (auto const& [item, shortBy] : shortfall)
        {
            if (!shortBy)
                continue;
            auto const list = placesByItem_.find(item);
            if (list == placesByItem_.end())
                continue;
            for (uint32 i : list->second)
            {
                GatherPlace const& place = places_[i];
                if (place.instance)  // a five's run gathers these (InstancePlaces), never a solo walk
                    continue;
                if (!ContinentAllowed(bot, place.mapId, place.spot))
                {
                    ++skipped[b].continent;
                    continue;
                }
                // [stuck8] no place in the other faction's zone (b343 Shalaeth planned to a Battered Chest in
                // Mulgore), none in another zone whose floor is above its level
                if (TravelRouteMgr::IsEnemyZone(bot, place.zoneId))
                {
                    ++skipped[b].continent;
                    continue;
                }
                if (place.zoneId != bot->GetZoneId() && ZoneSafeLevel(place.zoneId) > bot->GetLevel())
                {
                    ++skipped[b].unsafeOrSkill;
                    continue;
                }
                if (refused.count({low, place.id}))
                {
                    ++skipped[b].refused;
                    continue;
                }
                // Rate is 0 when no spawn of the place is safe for its level (the spot's hostiles, the area's own
                // level) or its skill is short
                if (Rate(place, item, bot, 0) <= 0.0f)
                {
                    ++skipped[b].unsafeOrSkill;
                    continue;
                }
                float travel = 0.0f;
                if (place.mapId == bot->GetMapId())
                    travel = TravelSeconds(bot, place);
                else
                {
                    auto const key = std::make_pair(b, place.mapId);
                    auto cached = routeCache.find(key);
                    if (cached == routeCache.end())
                        cached = routeCache.emplace(key, TravelSeconds(bot, place)).first;
                    travel = cached->second;
                }
                if (travel == FLT_MAX)
                {
                    ++skipped[b].noRoute;
                    continue;
                }
                options[{b, item}].push_back({i, travel});
            }
        }
    }

    // the item's value per unit for the leader's comparison with questing: what it is wanted for (the leader's
    // wanted list: skill points, upgrade / bag crafts, quests, shared over their materials); not a vendor price
    auto const valueOf = [&](uint32 itemId)
    {
        auto const v = unitValue.find(itemId);
        return v == unitValue.end() ? 0.0f : v->second;
    };
    auto const questOf = [&](Player* bot)
    {
        auto const q = questPace.find(bot->GetGUID().GetCounter());
        return q == questPace.end() ? -1.0f : q->second;
    };

    // greedy makespan: each step adds the bot that shortens the longest remaining work the most (an item nobody
    // gathers yet first: the longest one), at the place where it brings the most per second, travel included.
    // Phase 1 takes only the gathers that pay more per minute than the bot's questing (URGENT tasks); phase 2 gives
    // every other bot its best place as a convenient task (questing goes on; what it picks on the way counts)
    std::map<uint32, float> remaining;
    for (auto const& [item, n] : shortfall)
        if (n)
            remaining[item] = float(n);
    std::map<uint32, float> itemRate;    // item -> units per hour of the bots already on it (worth only)
    std::map<size_t, uint32> placeLoad;  // place -> bots already there (worth only)
    std::map<uint32, uint32> itemBots;   // item -> bots already on it (worth only)
    std::vector<bool> taken(gatherers.size(), false);
    std::vector<GatherAssignment> picks;
    auto const placeAllowed = [&](Player* bot, GatherPlace const& place)
    {
        return !place.instance && ContinentAllowed(bot, place.mapId, place.spot) &&
               !TravelRouteMgr::IsEnemyZone(bot, place.zoneId) &&
               (place.zoneId == bot->GetZoneId() || ZoneSafeLevel(place.zoneId) <= bot->GetLevel()) &&
               !refused.count({bot->GetGUID().GetCounter(), place.id});
    };

    // [craft433] b465: every update (5 s) planned the URGENT gathers anew and 147 GATHER tasks were dropped before
    // they started - Hobliir's Light Hide went Rabid Dire Wolf / Great Goretusk / Young Black Ravager and back 24
    // times, Laaral's Copper Ore Redridge -> Wetlands -> Hillsbrad on walks of 600-1100 s: no assigned gatherer
    // reached its place in 37 min. A bot keeps the place of its last URGENT gather while that still yields the item
    // and still beats its questing; on a transit map (the Deeprun Tram: every place "another continent") as it was
    std::vector<GatherAssignment> keep(gatherers.size());
    std::vector<bool> keeps(gatherers.size(), false);
    for (size_t b = 0; b < gatherers.size(); ++b)
    {
        Player* bot = gatherers[b];
        auto const old = previous.find(bot->GetGUID().GetCounter());
        if (old == previous.end() || !old->second.worth || old->second.climbSkill)  // climbs: judged below
            continue;
        GatherAssignment a = old->second;
        auto const left = remaining.find(a.itemId);
        if (left == remaining.end())
            continue;
        MapEntry const* map = sMapStore.LookupEntry(bot->GetMapId());
        if (!map || map->IsContinent() || map->Instanceable())
        {
            auto const opts = options.find({b, a.itemId});
            if (opts == options.end())
                continue;
            auto const o = std::find_if(opts->second.begin(), opts->second.end(),
                                        [&](Option const& x) { return x.place + 1 == a.placeId; });
            if (o == opts->second.end())
                continue;
            GatherPlace const& place = places_[o->place];
            float const rate = Rate(place, a.itemId, bot, 0);
            if (rate <= 0.0f)
                continue;
            float const quest = questOf(bot);
            float const effective = left->second / (left->second / rate + o->travel / 3600.0f);
            float const value = effective * valueOf(a.itemId) / 60.0f;
            bool const fisher = place.kind == GatherKind::FISH && roleFishers.count(a.botLow);
            if (!fisher && (quest < 0.0f || value <= quest))
                continue;
            a.need = uint32(std::ceil(left->second));
            a.ratePerHour = effective;
            a.travelSec = o->travel;
            a.valuePerMin = value;
            a.questPerMin = quest;
        }
        keep[b] = a;
        keeps[b] = true;
    }

    // [craft432] the tiers the crafters' walks wait for: per tier the gatherer whose climb pays the most per minute
    // (the tier's value is gained once, by the first to reach it - one climber), at a place of its level band whose
    // spawns still give it skill-ups by the core's chance, plus what they yield of the guild's shortfall. A climb
    // runs when it beats the bot's questing and the gather it holds; the climber of the last update keeps it while
    // it still does
    for (GatherClimb const& c : climbs)
    {
        if (c.value <= 0.0f || c.target <= 0)
            continue;
        struct ClimbPick
        {
            float perMin{-1.0f};
            size_t bot{0}, place{0};
            float points{0.0f}, hours{0.0f}, travel{0.0f}, units{0.0f}, worth{0.0f}, output{0.0f};
            uint32 item{0};
            bool wasClimber{false};
        } pick;
        std::string none = "no gatherer with the skill below the tier";
        for (size_t b = 0; b < gatherers.size(); ++b)
        {
            Player* bot = gatherers[b];
            if (taken[b] || !bot->HasSkill(c.skill))
                continue;
            int32 const skill = int32(bot->GetPureSkillValue(c.skill));
            if (skill >= c.target)
                continue;
            float const quest = questOf(bot);
            if (quest < 0.0f)  // [lag333] below the median level, or no quest pace: it levels first
            {
                none = "its gatherers level first (no quest pace to beat)";
                continue;
            }
            if (int32(bot->GetPureMaxSkillValue(c.skill)) <= skill)
            {
                none = "its gatherers wait for the next rank at the trainer";
                continue;
            }
            float const floor = std::max(quest, keeps[b] ? keep[b].valuePerMin : 0.0f);
            auto const old = previous.find(bot->GetGUID().GetCounter());
            bool const wasClimber = old != previous.end() && old->second.climbSkill == c.skill;
            for (size_t i = 0; i < places_.size(); ++i)
            {
                GatherPlace const& place = places_[i];
                if (place.skill != c.skill || (place.kind != GatherKind::NODE && place.kind != GatherKind::SKIN) ||
                    !placeAllowed(bot, place))
                    continue;
                float points = 0.0f, hours = 0.0f;
                if (!ClimbLeg(place, bot, placeLoad[i], c.target, points, hours))
                    continue;
                float travel = 0.0f;
                if (place.mapId == bot->GetMapId())
                    travel = TravelSeconds(bot, place);
                else
                {
                    auto const key = std::make_pair(b, place.mapId);
                    auto cached = routeCache.find(key);
                    if (cached == routeCache.end())
                        cached = routeCache.emplace(key, TravelSeconds(bot, place)).first;
                    travel = cached->second;
                }
                if (travel == FLT_MAX)
                    continue;
                float const worth = c.value * points / float(c.target - skill);
                float const visits = Visits(place, bot, placeLoad[i]);
                float output = 0.0f, itemValue = -1.0f, itemUnits = 0.0f;
                uint32 item = 0;
                for (auto const& [yieldItem, perVisit] : place.yield)
                {
                    float const units = visits * perVisit * hours;
                    auto const short_ = remaining.find(yieldItem);
                    float const v =
                        short_ == remaining.end() ? 0.0f : std::min(units, short_->second) * valueOf(yieldItem);
                    output += v;
                    if (v > itemValue || (v == itemValue && units > itemUnits))
                    {
                        itemValue = v;
                        itemUnits = units;
                        item = yieldItem;
                    }
                }
                float const minutes = travel / 60.0f + hours * 60.0f;
                float const perMin = minutes > 0.0f ? (worth + output) / minutes : 0.0f;
                if (perMin <= floor)
                {
                    none = "no climb beats its gatherers' questing";
                    continue;
                }
                bool const better = wasClimber != pick.wasClimber ? wasClimber : perMin > pick.perMin;
                if (pick.perMin >= 0.0f && !better)
                    continue;
                pick = {perMin, b, i, points, hours, travel, itemUnits, worth, output, item, wasClimber};
            }
        }
        std::string const climbKey = std::to_string(c.skill) + ":" + std::to_string(c.target);
        if (pick.perMin < 0.0f)
        {
            if (climbWhy_[climbKey] != none)
            {
                climbWhy_[climbKey] = none;
                LOG_INFO("playerbots", "[craft432] climb {} {} for {} ({}, worth {:.0f} c): nobody climbs - {}",
                         SkillName(c.skill), c.target, ItemName(c.itemId), c.forWhat, c.value, none);
            }
            continue;
        }
        climbWhy_.erase(climbKey);
        Player* bot = gatherers[pick.bot];
        GatherPlace const& place = places_[pick.place];
        uint32 const skill = bot->GetPureSkillValue(c.skill);
        taken[pick.bot] = true;
        ++placeLoad[pick.place];
        GatherAssignment a;
        a.botLow = bot->GetGUID().GetCounter();
        a.itemId = pick.item;
        a.placeId = place.id;
        a.kind = place.kind;
        a.spot = place.spot;
        a.need = uint32(std::ceil(pick.points));
        a.share = uint32(std::ceil(pick.units));
        a.ratePerHour = pick.hours > 0.0f ? pick.points / pick.hours : 0.0f;
        a.travelSec = pick.travel;
        a.valuePerMin = pick.perMin;
        a.questPerMin = questOf(bot);
        a.worth = true;
        a.climbSkill = c.skill;
        a.climbTo = c.target;
        a.placeName = PlaceName(place);
        auto const qw = questWhy.find(a.botLow);
        a.why = Acore::StringFormat(
            "[craft432] climb {} {} -> {} for {} ({}): +{:.0f} points here at {:.1f}/h (skill-up {:.0f} % now), walk "
            "{:.0f} s; the tier {:.0f} c x {:.0f}/{} points + its yield the guild lacks {:.0f} c = {:.1f} c/min > "
            "quests {:.1f} c/min ({})",
            SkillName(c.skill), skill, c.target, ItemName(c.itemId), c.forWhat, pick.points, a.ratePerHour,
            [&]
            {
                float chance = 0.0f, weights = 0.0f;
                int32 const work = int32(bot->GetSkillValue(c.skill));
                for (auto const& [minLevel, req, weight] : place.spawnReq)
                    if (minLevel <= bot->GetLevel() && req <= work)
                    {
                        chance += weight * SkillUpChance(c.skill, int32(skill), req);
                        weights += weight;
                    }
                return weights > 0.0f ? 100.0f * chance / weights : 0.0f;
            }(),
            pick.travel, c.value, pick.points, c.target - int32(skill), pick.output, pick.perMin, a.questPerMin,
            qw == questWhy.end() ? std::string("formula") : qw->second);
        picks.push_back(a);
    }
    for (size_t b = 0; b < gatherers.size(); ++b)
        if (keeps[b] && !taken[b])
        {
            size_t const place = keep[b].placeId - 1;
            taken[b] = true;
            ++placeLoad[place];
            itemRate[keep[b].itemId] += keep[b].ratePerHour;
            ++itemBots[keep[b].itemId];
            picks.push_back(keep[b]);
        }

    for (bool const worthPhase : {true, false})
        for (size_t round = 0; round < gatherers.size(); ++round)
        {
            float bestGain = -1.0f;
            GatherAssignment best;
            size_t bestBot = 0, bestPlace = 0;
            for (auto const& [key, list] : options)
            {
                auto const& [b, item] = key;
                if (taken[b])
                    continue;
                float const left = remaining[item];
                if (left <= 0.0f)
                    continue;
                Player* bot = gatherers[b];
                float const quest = questOf(bot);
                float const already = itemRate[item];
                float const share = left / float(itemBots[item] + 1);  // its part of what is left, by head count
                for (Option const& o : list)
                {
                    GatherPlace const& place = places_[o.place];
                    float const rate = Rate(place, item, bot, placeLoad[o.place]);
                    if (rate <= 0.0f)
                        continue;
                    // units per hour once the walk there is paid for over the bot's share of the work
                    float const workHours = share / rate;
                    float const effective = share / (workHours + o.travel / 3600.0f);
                    float const value = effective * valueOf(item) / 60.0f;
                    // [fish110] user 2026-11-10: a guild fisher's fishing is its guild role - it levels Fishing and
                    // brings the fives' cooked supplies their fish
                    bool const fisher =
                        place.kind == GatherKind::FISH && roleFishers.count(bot->GetGUID().GetCounter());
                    bool const worth = fisher || (quest >= 0.0f && value > quest);
                    if (worthPhase && !worth)
                        continue;
                    float const before = already > 0.0f ? left / already : FLT_MAX;
                    float const after = left / (already + effective);
                    float const gain = before == FLT_MAX ? FLT_MAX / 2.0f + after : before - after;
                    if (gain > bestGain)
                    {
                        bestGain = gain;
                        bestBot = b;
                        bestPlace = o.place;
                        best.botLow = bot->GetGUID().GetCounter();
                        best.itemId = item;
                        best.placeId = place.id;
                        best.kind = place.kind;
                        best.spot = place.spot;
                        best.need = uint32(std::ceil(left));
                        best.ratePerHour = effective;
                        best.travelSec = o.travel;
                        best.valuePerMin = value;
                        best.questPerMin = quest;
                        best.worth = worth;
                    }
                }
            }
            if (bestGain < 0.0f)
                break;
            taken[bestBot] = true;
            if (worthPhase)
            {
                ++placeLoad[bestPlace];
                itemRate[best.itemId] += best.ratePerHour;
                ++itemBots[best.itemId];
            }
            picks.push_back(best);
        }

    // each bot's share of its item: in proportion to its rate among the worth bots on the item; a convenient task
    // keeps the shortfall (what it picks on its quests counts toward it)
    for (GatherAssignment& a : picks)
    {
        if (a.climbSkill)  // [craft432] its leg's yield and its own reason
            continue;
        GatherPlace const& place = places_[a.placeId - 1];
        uint32 const shortBy = shortfall.at(a.itemId);
        float const total = itemRate[a.itemId];
        a.share = !a.worth ? shortBy : total > 0.0f ? uint32(std::ceil(float(shortBy) * a.ratePerHour / total)) : 0;
        a.placeName = PlaceName(place);
        auto const qw = questWhy.find(a.botLow);
        std::string const pace =
            a.questPerMin < 0.0f
                ? Acore::StringFormat(
                      "gathering {:.1f} c/min ({:.1f} c/unit), quests without a value (no quest with data)",
                      a.valuePerMin, valueOf(a.itemId))
                : Acore::StringFormat("gathering {:.1f} c/min ({:.1f} c/unit) {} quests {:.1f} c/min ({})",
                                      a.valuePerMin, valueOf(a.itemId), a.worth ? ">" : "≤", a.questPerMin,
                                      qw == questWhy.end() ? std::string("formula") : qw->second);
        a.why = Acore::StringFormat("short {}, here {:.1f}/h, walk {:.0f} s, bots at the spot {}; {}{}", a.need,
                                    a.ratePerHour, a.travelSec, a.worth ? placeLoad[a.placeId - 1] : 0, pace,
                                    a.kind == GatherKind::FISH && roleFishers.count(a.botLow)
                                        ? "; [fish110] a guild fisher: its role"
                                        : "");
    }

    std::lock_guard<std::mutex> guard(lock_);
    std::unordered_map<uint32, GatherAssignment> next;
    for (GatherAssignment const& a : picks)
    {
        auto const old = assigned_.find(a.botLow);
        if (old == assigned_.end() || old->second.itemId != a.itemId || old->second.placeId != a.placeId ||
            old->second.worth != a.worth || old->second.climbSkill != a.climbSkill)
        {
            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(a.botLow));
            LOG_INFO("playerbots",
                     "[gather] plan {} L{} item {} {} need {} share {} place {} kind {} map {} rate {:.1f}/h "
                     "travel {:.0f}s {} because {}",
                     bot ? bot->GetName() : std::to_string(a.botLow), bot ? uint32(bot->GetLevel()) : 0u, a.itemId,
                     ItemName(a.itemId), a.need, a.share, a.placeName, KindName(a.kind), a.spot.GetMapId(),
                     a.ratePerHour, a.travelSec, a.worth ? "URGENT" : "QUESTS_ON", a.why);
            if (a.kind == GatherKind::FISH && bot)  // [econ1] the rate with the skill growing over the hour
                LOG_INFO("playerbots", "[econ1] fishing plan {} L{} fishing {}/{}: {} {:.1f}/h at {} ({})",
                         bot->GetName(), uint32(bot->GetLevel()), bot->GetPureSkillValue(SKILL_FISHING),
                         bot->GetPureMaxSkillValue(SKILL_FISHING), ItemName(a.itemId), a.ratePerHour, a.placeName,
                         a.worth ? "URGENT" : "QUESTS_ON");
            if (old == assigned_.end() || old->second.itemId != a.itemId ||
                old->second.climbSkill != a.climbSkill)
                sessions_[a.botLow] = {a.itemId, getMSTime(), 0,
                                       a.climbSkill && bot ? uint32(bot->GetPureSkillValue(a.climbSkill)) : 0u};
        }
        next[a.botLow] = a;
        out.push_back(a);
    }
    for (auto const& [low, a] : assigned_)
        if (!next.count(low))
            LOG_INFO("playerbots", "[gather] plan {} done item {} {} place {}", low, a.itemId, ItemName(a.itemId),
                     a.placeName);
    // the gatherers with no place at all, and why (logged on change)
    for (size_t b = 0; b < gatherers.size(); ++b)
    {
        uint32 const low = gatherers[b]->GetGUID().GetCounter();
        if (next.count(low))
        {
            idleWhy_.erase(low);
            continue;
        }
        Skipped const& s = skipped[b];
        std::string const why = Acore::StringFormat(
            "no place: other continent {}, refused {}, unsafe for L{} or skill short {}, no route {}", s.continent,
            s.refused, uint32(gatherers[b]->GetLevel()), s.unsafeOrSkill, s.noRoute);
        if (idleWhy_[low] != why)
        {
            idleWhy_[low] = why;
            LOG_INFO("playerbots", "[gather] plan {} L{} quests: {}", gatherers[b]->GetName(),
                     uint32(gatherers[b]->GetLevel()), why);
        }
    }
    assigned_ = std::move(next);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// metrics
// ---------------------------------------------------------------------------------------------------------------------
void GuildGatherPlan::OnGathered(Player* bot, uint32 itemId, uint32 count)
{
    if (!bot || !count)
        return;
    uint32 const low = bot->GetGUID().GetCounter();
    // [gather] b337: chest loot made every drop "gathered" (Frayed Gloves, Worn Leather Boots: 339 of 376 got lines);
    // a got line is a node / skin / fish item, a converted reagent, or the item of the bot's assignment
    bool assignedItem = false;
    uint32 climbSkill = 0, climbStart = 0, climbMs = 0;
    int32 climbTo = 0;
    {
        std::lock_guard<std::mutex> guard(lock_);
        auto const a = assigned_.find(low);
        assignedItem = a != assigned_.end() && a->second.itemId == itemId;
        if (a != assigned_.end() && a->second.climbSkill && harvest_.count(itemId))
            if (auto const s = sessions_.find(low); s != sessions_.end())
            {
                climbSkill = a->second.climbSkill;
                climbTo = a->second.climbTo;
                climbStart = s->second.startSkill;
                climbMs = s->second.startMs;
            }
    }
    if (climbSkill)  // [craft432] the climb's progress at each harvest
    {
        uint32 const now = bot->GetPureSkillValue(climbSkill);
        float const hours = float(GetMSTimeDiffToNow(climbMs)) / float(HOUR * IN_MILLISECONDS);
        LOG_INFO("playerbots", "[craft432] climb {} {} {}/{} toward {}: +{} since it began ({:.1f}/h), harvest {} x{}",
                 bot->GetName(), SkillName(climbSkill), now, bot->GetPureMaxSkillValue(climbSkill), climbTo,
                 now > climbStart ? now - climbStart : 0,
                 hours > 0.0f ? float(now > climbStart ? now - climbStart : 0) / hours : 0.0f, ItemName(itemId),
                 count);
    }
    if (!assignedItem && !harvest_.count(itemId) && !disenchantMats_.count(itemId) && !conversions_.count(itemId))
        return;
    std::string place = AreaName(bot->GetZoneId());
    uint32 units = 0;
    float rate = -1.0f;
    {
        std::lock_guard<std::mutex> guard(lock_);
        auto const a = assigned_.find(low);
        if (a != assigned_.end() && a->second.itemId == itemId)
        {
            place = a->second.placeName;
            Session& s = sessions_[low];
            if (s.itemId != itemId)
                s = {itemId, getMSTime(), 0};
            s.units += count;
            units = s.units;
            float const hours = float(GetMSTimeDiffToNow(s.startMs)) / float(HOUR * IN_MILLISECONDS);
            if (hours > 0.0f)
                rate = float(s.units) / hours;
        }
    }
    if (rate >= 0.0f)
        LOG_INFO("playerbots", "[gather] got {} item {} {} count {} place {} total {} rate {:.1f}/h", bot->GetName(),
                 itemId, ItemName(itemId), count, place, units, rate);
    else
        LOG_INFO("playerbots", "[gather] got {} item {} {} count {} place {} total - rate -", bot->GetName(), itemId,
                 ItemName(itemId), count, place);
}

void GuildGatherPlan::LogStock(std::map<uint32, std::pair<uint32, uint32>> const& needHave) const
{
    for (auto const& [item, nh] : needHave)
    {
        uint32 skill = 0;
        int32 req = 0;
        std::string source = "craft/vendor/drop";
        // [gather42] the line says only what it checked here: that a gather place exists and the skill it asks, plus
        // whether that place is a supply line at all or only chests. Whether a vendor sells the item is NOT checked
        // here, and the crafter's walk reads VendorSource BEFORE any gather source - so Empty Vial (20 c at several
        // vendors, and genuinely 9 % in fishing reference table 11026 of an area whose zone asks Fishing 430) is
        // bought, not fished, however this line reads. Naming a source this function never verified is the same
        // silent assertion that cost us [ui11]'s chest case; pulling VendorSource into the gathering plan for a log
        // line is not worth the coupling, so the line states its own evidence instead
        if (Gatherable(item, &skill, &req))
            source = "gather skill " + std::to_string(skill) + " >= " + std::to_string(req) +
                     (Supplied(item) ? "" : ", chests only") + "; vendor not checked here, the walk buys first";
        else if (GatherConversion const* c = ConversionFor(item))
            source = c->raw ? "convert " + ItemName(c->raw) + " x" + std::to_string(c->rawPerUnit) : "disenchant";
        LOG_INFO("playerbots", "[gather] stock item {} {} need {} have {} short {} source {}", item, ItemName(item),
                 nh.first, nh.second, nh.first > nh.second ? nh.first - nh.second : 0, source);
    }
}

float GuildGatherPlan::BestRate(uint32 itemId, Player* bot) const
{
    float best = 0.0f;
    if (!loaded_ || !bot)
        return best;
    for (GatherPlace const* place : PlacesFor(itemId))
        if (place && !place->instance)
            best = std::max(best, Rate(*place, itemId, bot, 0));
    return best;
}
