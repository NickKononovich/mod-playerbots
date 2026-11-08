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

#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Formulas.h"
#include "GameObject.h"
#include "Log.h"
#include "LootMgr.h"
#include "MapMgr.h"
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
    LoadPools();
    LoadHostiles();
    LoadNodesAndChests();
    LoadSkinning();
    LoadFishing();
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
}

void GuildGatherPlan::LoadNodesAndChests()
{
    std::unordered_map<uint32, SpawnZone> zones;
    ReadZones("gameobject", zones);
    // (map, zone, entry) -> place index, and the spawn points of each
    std::map<std::tuple<uint32, uint32, uint32>, size_t> index;
    std::vector<std::vector<WorldPosition>> points;
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
        respawnSum[itr->second] += float(std::max(0, data.spawntimesecs));
        place.spawnReq.emplace_back(DangerAt(data.mapid, data.posX, data.posY, data.posZ), req, weight);
        points[itr->second].emplace_back(data.mapid, data.posX, data.posY, data.posZ);
    }
    for (auto const& [key, i] : index)
    {
        GatherPlace& place = places_[i];
        place.respawnSec = place.spawns ? respawnSum[i] / float(place.spawns) : 0.0f;
        Finish(place, points[i]);
    }
}

void GuildGatherPlan::LoadSkinning()
{
    std::unordered_map<uint32, SpawnZone> zones;
    ReadZones("creature", zones);
    std::map<std::tuple<uint32, uint32, uint32>, size_t> index;
    std::vector<std::vector<WorldPosition>> points;
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
        respawnSum[itr->second] += float(data.spawntimesecs);
        place.spawnReq.emplace_back(std::max(around, self), SkinningReq(tmpl->maxlevel), weight);
        points[itr->second].emplace_back(data.mapid, data.posX, data.posY, data.posZ);
    }
    for (auto const& [key, i] : index)
    {
        GatherPlace& place = places_[i];
        place.respawnSec = place.spawns ? respawnSum[i] / float(place.spawns) : 0.0f;
        Finish(place, points[i]);
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
        place.spawnReq.emplace_back(DangerAt(m.pos.GetMapId(), m.pos.GetPositionX(), m.pos.GetPositionY(),
                                             m.pos.GetPositionZ()),
                                    place.reqSkill, 1.0f);
        points[itr->second].push_back(m.pos);
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
        Finish(places_[i], points[i]);
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

GatherConversion const* GuildGatherPlan::ConversionFor(uint32 itemId) const
{
    auto const itr = conversions_.find(itemId);
    return itr == conversions_.end() || itr->second.empty() ? nullptr : &itr->second.front();
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
        if (place.kind == GatherKind::SKIN)
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
float GuildGatherPlan::Rate(GatherPlace const& place, uint32 itemId, Player* bot, uint32 others, float pace) const
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
        float chance = noMiss > 0 && skill < noMiss ? std::pow(float(skill) / float(noMiss), 2.0f) : 1.0f;
        chance = std::max(chance, 0.01f);
        float const casts = 3600.0f / castSec;
        // pools are shared like nodes; open water is not
        return casts * chance * perUnit + perPool * 3600.0f / castSec / float(std::max<uint32>(1, place.spawns)) /
                                              float(others + 1);
    }

    // nodes, chests, skinnable mobs: what respawns at the spawns this bot may work (level safety, skill)
    float live = 0.0f;
    for (auto const& [minLevel, req, weight] : place.spawnReq)
        if (minLevel <= level && req <= skill)
            live += weight;
    if (live <= 0.0f || place.respawnSec <= 0.0f)
        return 0.0f;
    float rate = live * 3600.0f / place.respawnSec * perUnit / float(others + 1);
    if (pace >= 0.0f)
    {
        float total = 0.0f;
        for (auto const& [item, n] : place.yield)
            total += n;
        if (total > 0.0f)
            rate = std::min(rate, pace * 60.0f * perUnit / total);
    }
    return rate;
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

std::vector<GatherAssignment> GuildGatherPlan::Assign(std::map<uint32, uint32> const& shortfall,
                                                      std::vector<Player*> const& gatherers,
                                                      std::map<uint32, float> const& measuredPace)
{
    std::vector<GatherAssignment> out;
    if (!loaded_)
        return out;

    struct Option
    {
        size_t place{0};
        float travel{0.0f};
    };
    // per bot and item: the places it can work (travel computed once per place)
    std::map<std::pair<size_t, uint32>, std::vector<Option>> options;
    std::map<std::pair<size_t, uint32>, float> routeCache;  // (bot, map) -> seconds (cross-map routes only)
    for (size_t b = 0; b < gatherers.size(); ++b)
    {
        Player* bot = gatherers[b];
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
                auto const pace = measuredPace.find(place.skill);
                if (Rate(place, item, bot, 0, pace == measuredPace.end() ? -1.0f : pace->second) <= 0.0f)
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
                options[{b, item}].push_back({i, travel});
            }
        }
    }

    // greedy makespan: each step adds the bot that shortens the longest remaining work the most (an item nobody
    // gathers yet first: the longest one), at the place where it brings the most per second, travel included
    std::map<uint32, float> remaining;
    for (auto const& [item, n] : shortfall)
        if (n)
            remaining[item] = float(n);
    std::map<uint32, float> itemRate;      // item -> units per hour of the bots already on it
    std::map<size_t, uint32> placeLoad;    // place -> bots already there
    std::map<uint32, uint32> itemBots;     // item -> bots already on it
    std::vector<bool> taken(gatherers.size(), false);
    std::vector<GatherAssignment> picks;
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
            float const already = itemRate[item];
            float const share = left / float(itemBots[item] + 1);  // its part of what is left, by head count
            for (Option const& o : list)
            {
                GatherPlace const& place = places_[o.place];
                auto const pace = measuredPace.find(place.skill);
                float const rate = Rate(place, item, bot, placeLoad[o.place],
                                        pace == measuredPace.end() ? -1.0f : pace->second);
                if (rate <= 0.0f)
                    continue;
                // units per hour once the walk there is paid for over the bot's share of the work
                float const workHours = share / rate;
                float const effective = share / (workHours + o.travel / 3600.0f);
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
                }
            }
        }
        if (bestGain < 0.0f)
            break;
        taken[bestBot] = true;
        ++placeLoad[bestPlace];
        itemRate[best.itemId] += best.ratePerHour;
        ++itemBots[best.itemId];
        picks.push_back(best);
    }

    // each bot's share of its item: in proportion to its rate among the bots on the item
    for (GatherAssignment& a : picks)
    {
        GatherPlace const& place = places_[a.placeId - 1];
        float const total = itemRate[a.itemId];
        a.share = total > 0.0f ? uint32(std::ceil(float(shortfall.at(a.itemId)) * a.ratePerHour / total)) : 0;
        a.placeName = PlaceName(place);
        a.why = Acore::StringFormat("нужно {} (гильдии не хватает), здесь {:.0f}/ч, путь {:.0f} с, ботов на месте {}",
                                    a.need, a.ratePerHour, a.travelSec, placeLoad[a.placeId - 1]);
    }

    std::lock_guard<std::mutex> guard(lock_);
    std::unordered_map<uint32, GatherAssignment> next;
    for (GatherAssignment const& a : picks)
    {
        auto const old = assigned_.find(a.botLow);
        if (old == assigned_.end() || old->second.itemId != a.itemId || old->second.placeId != a.placeId)
        {
            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(a.botLow));
            LOG_INFO("playerbots",
                     "[gather] plan {} item {} {} need {} share {} place {} kind {} map {} rate {:.1f}/h "
                     "travel {:.0f}s because {}",
                     bot ? bot->GetName() : std::to_string(a.botLow), a.itemId, ItemName(a.itemId), a.need, a.share,
                     a.placeName, KindName(a.kind), a.spot.GetMapId(), a.ratePerHour, a.travelSec, a.why);
            sessions_[a.botLow] = {a.itemId, getMSTime(), 0};
        }
        next[a.botLow] = a;
        out.push_back(a);
    }
    for (auto const& [low, a] : assigned_)
        if (!next.count(low))
            LOG_INFO("playerbots", "[gather] plan {} done item {} {} place {}", low, a.itemId, ItemName(a.itemId),
                     a.placeName);
    assigned_ = std::move(next);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// metrics
// ---------------------------------------------------------------------------------------------------------------------
void GuildGatherPlan::OnGathered(Player* bot, uint32 itemId, uint32 count)
{
    bool const indexed = gatherSkill_.count(itemId) || disenchantMats_.count(itemId) || conversions_.count(itemId);
    if (!bot || !count || !indexed)
        return;
    uint32 const low = bot->GetGUID().GetCounter();
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
        if (Gatherable(item, &skill, &req))
            source = "gather skill " + std::to_string(skill) + " >= " + std::to_string(req);
        else if (GatherConversion const* c = ConversionFor(item))
            source = c->raw ? "convert " + ItemName(c->raw) + " x" + std::to_string(c->rawPerUnit) : "disenchant";
        LOG_INFO("playerbots", "[gather] stock item {} {} need {} have {} short {} source {}", item, ItemName(item),
                 nh.first, nh.second, nh.first > nh.second ? nh.first - nh.second : 0, source);
    }
}
