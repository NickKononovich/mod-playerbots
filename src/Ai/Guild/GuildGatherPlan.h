/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUILDGATHERPLAN_H
#define PLAYERBOTS_GUILDGATHERPLAN_H

#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Define.h"
#include "TravelMgr.h"

class Player;
struct ItemTemplate;

// [gather] Gathering by need. The global leader (GuildCoordinator) stays the single decider: it hands this plan the
// wanted shortfalls and the gatherers, and gets back one assignment per gatherer ("gather X at place P because need
// N"), which it issues as a GATHER task. Everything here is built once at startup from the world DB and the core's
// loot stores; Assign runs on the world thread, the lookups marked "map thread" are thread-safe.
enum class GatherKind : uint8
{
    NODE,   // herb / ore spawns (lock skill of the node)
    SKIN,   // skinnable mobs (creature_template skinloot; skill by the core's skinning formula)
    FISH,   // fishable water of an area (fishing_loot_template by area, then zone; skill_fishing_base_level)
    CHEST   // lockable or plain chests (only when their loot is on the wanted list)
};

struct GatherPlace
{
    uint32 id{0};
    GatherKind kind{GatherKind::NODE};
    uint32 mapId{0};
    uint32 zoneId{0};
    uint32 areaId{0};
    bool instance{false};  // inside a dungeon / raid: a five gathers it on its run, never a solo gatherer
    WorldPosition spot{};  // the spawn nearest the spawns' centre (fish: a water marker - a pool or a swimming mob)
    uint32 skill{0};       // SKILL_HERBALISM / SKILL_MINING / SKILL_SKINNING / SKILL_FISHING / SKILL_LOCKPICKING / 0
    int32 reqSkill{0};     // the lowest skill that works here (node lock, skinning of the lowest mob, zone fishing)
    uint32 spawns{0};
    float liveShare{0.0f};   // spawns alive at once (pool max_limit / members, nested pools multiplied)
    float respawnSec{0.0f};  // mean spawntimesecs of the spawns
    std::vector<uint32> entries;  // gameobject or creature entries
    // per spawn (node, mob, chest) or per catch (fish): item -> expected count
    std::map<uint32, float> yield;
    std::map<uint32, float> poolYield;  // FISH: item -> expected count over one round of all the area's pools
    // per spawn (fish: per water marker): (the lowest bot level for which it is safe - hostiles of normal rank in
    // detection range below orange con, elites gray; SKIN: the skinned mob itself counts -, the skill it needs,
    // its pool weight); sorted
    std::vector<std::tuple<uint8, int32, float>> spawnReq;
    uint8 levelMin{0}, levelMax{0};  // SKIN: the mob's levels (display)
};

struct GatherAssignment
{
    uint32 botLow{0};
    uint32 itemId{0};
    uint32 placeId{0};
    GatherKind kind{GatherKind::NODE};
    WorldPosition spot{};
    uint32 need{0};        // the guild shortfall of the item
    uint32 share{0};       // what this gatherer is expected to bring
    float ratePerHour{0.0f};  // units per hour at the place for this bot (shared supply, catch chance)
    float travelSec{0.0f};
    // [gather] the leader's value test: the gather's copper per minute (the item's wanted value x the rate, the walk
    // paid for over the share) against the bot's measured questing copper per minute (GuildCoordinator::QuestPace).
    // worth = the gather pays more: an URGENT task; else a convenient one that never takes the bot off its quests
    float valuePerMin{0.0f};
    float questPerMin{-1.0f};  // < 0: no questing samples yet (the bot quests)
    bool worth{false};
    std::string placeName;
    std::string why;
};

// A crafter's conversion that turns a raw material into the reagent (milling, prospecting, disenchanting)
struct GatherConversion
{
    uint32 raw{0};       // herb / ore (disenchant: 0, any spare item of the band)
    float rawPerUnit{0};  // raw items per unit of the reagent
    uint32 skillLine{0};  // SKILL_INSCRIPTION / SKILL_JEWELCRAFTING / SKILL_ENCHANTING
    uint32 reqSkill{0};   // the raw's RequiredSkillRank (the core's milling / prospecting / disenchant check)
    uint32 spellId{0};    // 51005 Milling / 31252 Prospecting / 13262 Disenchant
};

class GuildGatherPlan
{
public:
    static GuildGatherPlan& instance()
    {
        static GuildGatherPlan instance;
        return instance;
    }

    // world thread, startup (after the core's loot stores and spawns are loaded)
    void Load();

    // world thread: the shortfalls (item -> units still needed) over the gatherers online; returns one assignment per
    // gatherer that can bring any of them (others: none, they quest). `measuredPace`: skill -> units per minute of
    // gathering the leader measured (GuildCoordinator::GatherPace), < 0 unknown.
    // `questPace`: bot low guid -> its questing copper per minute (missing: no samples); `unitValue`: item -> copper
    // one unit is worth to the guild (GuildCoordinator::UpdateWanted: what it is wanted for)
    std::vector<GatherAssignment> Assign(std::map<uint32, uint32> const& shortfall,
                                         std::vector<Player*> const& gatherers,
                                         std::map<uint32, float> const& measuredPace,
                                         std::map<uint32, float> const& questPace,
                                         std::map<uint32, float> const& unitValue);
    // map thread: the bot's walk to its place was refused by the solo judge (TripRefused) or ended no nearer than the
    // last one (the walk gave the place up): the place is off for this bot until its next level, Assign picks again
    void Refuse(Player* bot, uint32 placeId, std::string const& why);
    // [gather] the continent rule (user): Outland from level 58, Northrend from 68, no other continent below 58
    static bool ContinentAllowed(Player* bot, uint32 mapId, WorldPosition const& spot);

    // map thread
    bool Assigned(uint32 botLow, GatherAssignment& out) const;
    GatherPlace const* Place(uint32 placeId) const;  // read-only after Load
    // the item can be gathered somewhere (any kind) and with which skill; false: craft / vendor / drop only
    bool Gatherable(uint32 itemId, uint32* skill = nullptr, int32* reqSkill = nullptr) const;
    // the reagent comes from a crafter's conversion of a raw material (pigment <- herb, gem <- ore): the cheapest one
    // by raw per unit; disenchant mats return raw 0 (spare items only)
    GatherConversion const* ConversionFor(uint32 itemId) const;
    // for GuildCoordinator::ExpandReagent: the raw per unit of a milled / prospected reagent (nullptr: none)
    std::vector<std::pair<uint32, float>> const* ConvertedFrom(uint32 itemId) const;
    // the expected disenchant mats of a spare item (empty: cannot be disenchanted)
    std::map<uint32, float> DisenchantYield(ItemTemplate const* proto) const;
    // instance places of a map (a five's run): the nodes / skinnable mobs its members can take on the way
    std::vector<GatherPlace const*> InstancePlaces(uint32 mapId) const;

    // map thread: a gathered stack reached the bags (GuildCoordinator::OnLoot) - the [gather] got line and the rate
    void OnGathered(Player* bot, uint32 itemId, uint32 count);
    // world thread: need vs stock lines for the metrics (one per item per call)
    void LogStock(std::map<uint32, std::pair<uint32, uint32>> const& needHave) const;

    static char const* KindName(GatherKind kind);
    static int32 SkinningReq(uint8 mobLevel);  // the core's Spell::CheckCast skinning requirement
    static uint8 MaxSafeMobLevel(uint8 botLevel);  // the highest level below orange con (Acore::XP::GetColorCode)

private:
    GuildGatherPlan() = default;

    void LoadPools();
    void LoadHostiles();
    void LoadNodesAndChests();
    void LoadSkinning();
    void LoadFishing();
    void LoadConversions();
    void Finish(GatherPlace& place, std::vector<WorldPosition> const& points);
    uint8 DangerAt(uint32 mapId, float x, float y, float z) const;
    float SpawnWeight(uint32 spawnId, bool gameObject) const;
    std::string PlaceName(GatherPlace const& place) const;
    // units per hour this bot gets at the place (shared with `others` already sent there); 0 = cannot work there
    float Rate(GatherPlace const& place, uint32 itemId, Player* bot, uint32 others, float pace) const;
    float TravelSeconds(Player* bot, GatherPlace const& place) const;

    std::vector<GatherPlace> places_;
    std::unordered_map<uint32, std::vector<uint32>> placesByItem_;  // item -> place indexes
    std::unordered_map<uint32, std::pair<uint32, int32>> gatherSkill_;  // item -> (skill, lowest req)
    std::unordered_map<uint32, std::vector<GatherConversion>> conversions_;  // reagent -> conversions, best first
    std::unordered_map<uint32, std::vector<std::pair<uint32, float>>> convertedFrom_;
    std::unordered_set<uint32> disenchantMats_;
    std::unordered_set<uint32> harvest_;  // items of open-world nodes, skinning and fishing (the [gather] got lines)

    // load-time only
    struct PoolInfo
    {
        uint32 maxLimit{0};
        uint32 members{0};
        uint32 mother{0};
    };
    std::unordered_map<uint32, PoolInfo> pools_;
    std::unordered_map<uint32, uint32> goPool_, creaturePool_;  // spawn -> pool
    struct Hostile
    {
        float x, y, z, range;
        uint8 level;
    };
    std::unordered_map<uint64, std::vector<Hostile>> hostileCells_;  // (map, cell x, cell y) -> hostile spawns
    float hostileCellSize_{0.0f};

    mutable std::mutex lock_;
    std::unordered_map<uint32, GatherAssignment> assigned_;  // bot -> its assignment
    struct Session
    {
        uint32 itemId{0};
        uint32 startMs{0};
        uint32 units{0};
    };
    std::unordered_map<uint32, Session> sessions_;  // bot -> the units since its assignment began
    std::map<std::pair<uint32, uint32>, uint8> refused_;  // (bot, place) -> the bot level it was refused at
    std::unordered_map<uint32, std::string> idleWhy_;      // bot -> why it got no place (logged on change)
    bool loaded_{false};
};

#define sGuildGatherPlan GuildGatherPlan::instance()

#endif
