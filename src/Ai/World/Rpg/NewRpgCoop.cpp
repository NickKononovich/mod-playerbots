/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgCoop.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Action.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DpsEstimate.h"
#include "Formulas.h"
#include "Group.h"
#include "GroupMgr.h"
#include "GuildCoordinator.h"
#include "LeaderFightChecks.h"
#include "LeaderOrders.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestStats.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"

namespace
{
constexpr float NO_TIME = std::numeric_limits<float>::max();

// one quest objective the bot works (New RPG DoQuest::objectiveIdx: 0-3 kill / object, 4-9 item)
struct Objective
{
    uint32 questId = 0;
    int32 index = -1;
    uint32 key = 0;  // the creature entry to kill, the item to loot
    bool item = false;
    uint32 have = 0;
    uint32 need = 0;
    uint32 Left() const { return need > have ? need - have : 0; }
    bool Same(Objective const& o) const { return questId == o.questId && index == o.index; }
};

// the formula's pace of one objective for one bot (no measured history): the source creature it would farm (the
// objective's creature or kill credit, for an item the creature that drops it with its loot chance, or the chest
// that holds it: no fight, its walk only), its health at the core's base stats, the walk between the spawns it works
// and from the bot to the first of them
struct Pace
{
    bool known = false;
    std::string why;  // not known: the data fact that is missing
    uint32 entry = 0;
    uint8 level = 0;
    float hp = 0.0f;
    float chance = 1.0f;     // objective units per kill
    float stepYd = 0.0f;     // walk per kill: the mean gap to the nearest other spawn among those it works
    float reachYd = 0.0f;    // from the bot to the nearest of them
    float spawns = 0.0f;     // its spawns on the map
    float respawnSec = 0.0f; // their mean respawn
    WorldPosition at{};      // where the bot works the objective (its New RPG spot)
    uint32 left = 0;         // the counter it was computed for
    bool chest = false;      // the source is a chest game object (no fight, no kill XP)
    Position first{};        // the nearest spawn it works (the offer's route to it)
};

// the camp its objective lies in, judged when its walk there was refused (SoloSafety::Judge, the bot solo)
struct Camp
{
    bool known = false;
    WorldPosition at{};
    ObjectGuid mob{};
    uint32 entry = 0;
    uint8 topLevel = 0;
    uint32 mobs = 0;
    uint32 units = 1;     // objective units one clear of the pack gives
    float damage = 0.0f;  // on the bot over fightSec at its damage
    float fightSec = 0.0f;
    float dps = 0.0f;  // its damage per second then
    // the pack's own mobs serve the objective (its kill credit, a drop of its item); else the pack only stands on the
    // way and the objective is worked after the clear at `pace` (its sources nearest the spot)
    bool serves = false;
    Pace pace;
};

struct Seek
{
    Objective obj;
    uint64 bucket = 0;
    Camp camp;
};

// one open objective of its quest log (the board knows every guild bot's log, as the global leader does): a bot that
// looks for a party for a camp finds the members that hold the same objective, not only the one working it now
struct Hold
{
    uint64 bucket = 0;
    Objective obj;
};

struct Posting
{
    ObjectGuid guid{};
    std::string name;
    uint32 map = 0;
    uint32 zone = 0;
    uint8 level = 0;
    Objective cur;
    uint64 bucket = 0;
    Pace pace;  // the formula's pace of `cur` (recomputed when its counter, its spot or its level changes)
    uint8 paceLevel = 0;
    std::vector<Seek> seeks;
    std::vector<Hold> holds;  // every open kill / loot objective of its log in this zone
    uint64 logSign = 0;       // the log (quests, counters), map and zone the holds were read for
    std::unordered_set<uint64> on;  // the buckets it stands on: cur, seeks, holds
    float questRate = 1.0f;         // the core's quest XP rate for it (Player::GetQuestRate)
    float questRateDf = 1.0f;       // the same for a Dungeon Finder quest
    std::unordered_map<uint64, uint32> seen;  // bucket -> the board version it last looked at
    // in a party
    uint32 party = 0;
    Objective partyObj;
    uint32 joinedAt = 0;  // ms of the accept
    uint32 haveAtJoin = 0;
    uint64 xp = 0;  // kill XP in the party
    float walkYd = 0.0f;   // the offer's walk to the leader (its route's length)
    float walkSec = 0.0f;  // the offer's eta of that walk at its run speed
    WorldPosition walkTo{};  // the leader's spot its far walk heads for
    bool arrived = false;
    bool soloNever = false;   // the offer's forecast: alone it cannot close it (a camp it does not solo)
    float soloXph = 0.0f;     // the offer's solo forecast
    float groupXph = 0.0f;    // the offer's forecast together
    uint32 sharedKills = 0;
};

struct Bucket
{
    std::vector<uint32> lows;
    uint32 version = 0;
};

struct Party
{
    uint32 id = 0;
    ObjectGuid leader{};
    std::vector<ObjectGuid> members;  // the leader included
    ObjectGuid group{};
    bool formed = false;
    std::unordered_map<uint32, float> healRate;  // member -> heals per fight second (its solo judge)
    Camp camp;                                   // the camp it was formed for (a seeker's objective)
    ObjectGuid campFor{};                        // the seeker whose camp it is
    WorldPosition spot{};                        // a holder leads: the nearest spawn of the objective it held
    bool spotSet = false;
};

std::mutex lock;
std::unordered_map<uint32, Posting> postings;
std::unordered_map<uint64, Bucket> buckets;
std::map<uint32, Party> parties;
uint32 nextParty = 1;
std::unordered_map<uint64, std::string> lastRefusal;  // pair -> the last refusal logged (one line per change)

// the members of formed parties, behind a lock of its own that never waits on another one: the global leader may ask
// InParty while it holds its own lock, and the board's lock is held while it asks the global leader
std::mutex partyLowsLock;
std::unordered_set<uint32> partyLows;

void MarkParty(uint32 low, bool in)
{
    std::lock_guard<std::mutex> guard(partyLowsLock);
    if (in)
        partyLows.insert(low);
    else
        partyLows.erase(low);
}

// what a locked section leaves to do after the lock: log lines and world-thread operations
struct Out
{
    std::vector<std::string> logs;
    std::vector<std::unique_ptr<PlayerbotOperation>> ops;
    std::vector<std::function<void()>> after;  // calls to the global leader, made without the board's lock
    void Flush()
    {
        for (std::string const& line : logs)
            LOG_INFO("playerbots", "{}", line);
        for (auto& op : ops)
            PlayerbotWorldThreadProcessor::instance().QueueOperation(std::move(op));
        for (auto const& call : after)
            call();
    }
};

// the global leader's side (NewRpgCoop::SetCoordinatorHooks), set once at its Load before any bot ticks
NewRpgCoop::CoordinatorHooks hooks;

// the co-op task enters the bot's task list (the global leader's list once its hook is set; a [tasks] line until then)
void AddTask(std::string const& name, uint32 low, std::string const& partner, uint32 partnerLow, uint32 questId,
             bool leads, std::string const& why)
{
    if (hooks.addTask)
    {
        hooks.addTask(low, partnerLow, questId, leads, why);
        return;
    }
    LOG_INFO("playerbots", "[tasks] {} add COOP [помощь, поле, from global]: {} q{} с {} — {}", name,
             leads ? "ведёт кооп" : "помогает", questId, partner, why);
}

void EndTask(std::string const& name, uint32 low, uint32 questId, std::string const& why)
{
    if (hooks.endTask)
    {
        hooks.endTask(low, questId, why);
        return;
    }
    LOG_INFO("playerbots", "[tasks] {} done COOP: q{} ({})", name, questId, why);
}

std::string Stamp() { return " t=" + std::to_string(QuestStats::NowMs()); }

uint64 PairKey(uint32 a, uint32 b) { return (uint64(std::min(a, b)) << 32) | std::max(a, b); }

// the board: map, zone, objective kind and key (a creature entry or an item id)
uint64 BucketKey(uint32 map, uint32 zone, Objective const& o)
{
    return (uint64(map & 0xFFFF) << 48) | (uint64(zone & 0xFFFF) << 32) | (uint64(o.item ? 1 : 0) << 31) |
           (o.key & 0x7FFFFFFF);
}

void AddTo(uint64 key, uint32 low)
{
    Bucket& b = buckets[key];
    if (std::find(b.lows.begin(), b.lows.end(), low) == b.lows.end())
        b.lows.push_back(low);
    ++b.version;
}

void RemoveFrom(uint64 key, uint32 low)
{
    auto const itr = buckets.find(key);
    if (itr == buckets.end())
        return;
    itr->second.lows.erase(std::remove(itr->second.lows.begin(), itr->second.lows.end(), low), itr->second.lows.end());
    ++itr->second.version;
    if (itr->second.lows.empty())
        buckets.erase(itr);
}

void Bump(uint64 key)
{
    auto const itr = buckets.find(key);
    if (itr != buckets.end())
        ++itr->second.version;
}

// the bot stands on the buckets of its current objective, of the objectives it looks for a party for and of every open
// objective of its log (lock held): the board's lists follow that set (a bucket's version moves when a bot comes or
// goes)
void SyncBuckets(Posting& me, uint32 low)
{
    std::unordered_set<uint64> want;
    if (me.bucket)
        want.insert(me.bucket);
    for (Seek const& seek : me.seeks)
        want.insert(seek.bucket);
    for (Hold const& hold : me.holds)
        want.insert(hold.bucket);
    for (uint64 key : me.on)
        if (!want.count(key))
            RemoveFrom(key, low);
    for (uint64 key : want)
        if (!me.on.count(key))
            AddTo(key, low);
    me.on = std::move(want);
}

// the bot works this bucket's objective now or looks for a party for it (not only holds it in its log)
bool WorksOn(Posting const& p, uint64 key)
{
    return p.bucket == key ||
           std::any_of(p.seeks.begin(), p.seeks.end(), [key](Seek const& s) { return s.bucket == key; });
}

// the objective's counter from the quest log; false: not a killing / looting objective of an incomplete quest
bool ReadObjective(Player* bot, uint32 questId, int32 index, Objective& out)
{
    Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
    if (!quest || bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
        return false;
    auto const status = bot->getQuestStatusMap().find(questId);
    if (status == bot->getQuestStatusMap().end())
        return false;
    out = Objective();
    out.questId = questId;
    out.index = index;
    if (index >= 0 && index < QUEST_OBJECTIVES_COUNT)
    {
        // a game object to use is collected by each for itself: no co-op
        if (quest->RequiredNpcOrGo[index] <= 0 || !quest->RequiredNpcOrGoCount[index])
            return false;
        out.key = uint32(quest->RequiredNpcOrGo[index]);
        out.need = quest->RequiredNpcOrGoCount[index];
        out.have = std::min<uint32>(status->second.CreatureOrGOCount[index], out.need);
        return true;
    }
    int32 const item = index - QUEST_OBJECTIVES_COUNT;
    if (item < 0 || item >= QUEST_ITEM_OBJECTIVES_COUNT || !quest->RequiredItemId[item] ||
        !quest->RequiredItemCount[item])
        return false;
    out.item = true;
    out.key = quest->RequiredItemId[item];
    out.need = quest->RequiredItemCount[item];
    out.have = std::min<uint32>(status->second.ItemCount[item], out.need);
    return true;
}

bool CurrentObjective(PlayerbotAI* botAI, Player* bot, Objective& out)
{
    auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    if (!quest || quest->pos == WorldPosition())
        return false;
    return ReadObjective(bot, quest->questId, quest->objectiveIdx, out);
}

// the open kill / loot objectives of its whole log in its zone (lock held), read again only when its log, its map or
// its zone changed (a quest taken, a counter, a hand-in): true when they were read again
bool ReadHolds(Player* bot, Posting& me)
{
    uint64 sign = (uint64(bot->GetMapId()) << 32) ^ uint64(bot->GetZoneId()) ^ 0x9E3779B97F4A7C15ull;
    for (auto const& [questId, data] : bot->getQuestStatusMap())
    {
        if (data.Status != QUEST_STATUS_INCOMPLETE)
            continue;
        uint64 h = questId;
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            h = h * 1000003ull + data.CreatureOrGOCount[i];
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            h = h * 1000003ull + data.ItemCount[i];
        sign += h * 0xBF58476D1CE4E5B9ull;  // the map's order does not matter
    }
    if (sign == me.logSign)
        return false;
    me.logSign = sign;
    me.holds.clear();
    if (bot->GetMap()->Instanceable())
        return true;
    for (auto const& [questId, data] : bot->getQuestStatusMap())
    {
        if (data.Status != QUEST_STATUS_INCOMPLETE)
            continue;
        for (int32 index = 0; index < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT; ++index)
        {
            Objective o;
            if (!ReadObjective(bot, questId, index, o) || !o.Left())
                continue;
            Hold hold;
            hold.obj = o;
            hold.bucket = BucketKey(bot->GetMapId(), bot->GetZoneId(), o);
            me.holds.push_back(hold);
        }
    }
    return true;
}

// the static world data the pace formula reads, indexed once (the world thread's startup, else the first look):
// the quest items -> the creatures that drop them (creature_loot_template, the item's chance), the kill objectives
// -> the creatures whose kill credits them, those creatures -> their spawns (`creature`, multispawn alternates)
struct DropSource
{
    uint32 entry;
    float chance;        // 0..1
    bool chest = false;  // a chest game object (gameobject_loot_template), else a creature
};
struct SpawnPoint
{
    uint16 map;
    float x, y, z;
    uint32 respawnSec;
};
std::once_flag indexOnce;
std::unordered_map<uint32, std::vector<DropSource>> itemSources;
std::unordered_map<uint32, std::vector<uint32>> creditSources;
std::unordered_map<uint32, std::vector<SpawnPoint>> spawnsOf;
std::unordered_map<uint32, std::vector<SpawnPoint>> chestSpawnsOf;  // chest game objects that hold a quest item

void BuildIndex()
{
    std::unordered_set<uint32> items, credits, sources;
    for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
    {
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredNpcOrGo[i] > 0)
                credits.insert(uint32(quest->RequiredNpcOrGo[i]));
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredItemId[i])
                items.insert(quest->RequiredItemId[i]);
    }
    for (auto const& [entry, tmpl] : *sObjectMgr->GetCreatureTemplates())
    {
        for (uint32 credit : {tmpl.Entry, tmpl.KillCredit[0], tmpl.KillCredit[1]})
        {
            if (!credit || !credits.count(credit))
                continue;
            std::vector<uint32>& list = creditSources[credit];
            if (std::find(list.begin(), list.end(), entry) == list.end())
                list.push_back(entry);
            sources.insert(entry);
        }
        LootTemplate const* loot = tmpl.lootid ? LootTemplates_Creature.GetLootFor(tmpl.lootid) : nullptr;
        if (!loot)
            continue;
        std::unordered_map<uint32, float> best;
        loot->VisitEntries(
            [&](LootStoreItem const& item, float chance)
            {
                if (!item.reference && items.count(item.itemid))
                    best[item.itemid] = std::max(best[item.itemid], std::abs(chance) / 100.0f);
            });
        for (auto const& [item, chance] : best)
            if (chance > 0.0f)
            {
                itemSources[item].push_back({entry, std::min(chance, 1.0f)});
                sources.insert(entry);
            }
    }
    // the chests (a quest item one opens, no fight): their loot tables and their spawns
    std::unordered_set<uint32> chests;
    if (GameObjectTemplateContainer const* gos = sObjectMgr->GetGameObjectTemplates())
        for (auto const& [entry, tmpl] : *gos)
        {
            if (tmpl.type != GAMEOBJECT_TYPE_CHEST || !tmpl.chest.lootId)
                continue;
            LootTemplate const* loot = LootTemplates_Gameobject.GetLootFor(tmpl.chest.lootId);
            if (!loot)
                continue;
            std::unordered_map<uint32, float> best;
            loot->VisitEntries(
                [&](LootStoreItem const& item, float chance)
                {
                    if (!item.reference && items.count(item.itemid))
                        best[item.itemid] = std::max(best[item.itemid], std::abs(chance) / 100.0f);
                });
            for (auto const& [item, chance] : best)
                if (chance > 0.0f)
                {
                    itemSources[item].push_back({entry, std::min(chance, 1.0f), true});
                    chests.insert(entry);
                }
        }
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
        if (chests.count(data.id))
            chestSpawnsOf[data.id].push_back(
                {data.mapid, data.posX, data.posY, data.posZ, uint32(std::abs(data.spawntimesecs))});
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        uint32 const ids[] = {data.id, data.id2, data.id3};
        for (size_t i = 0; i < 3; ++i)
        {
            uint32 const entry = ids[i];
            if (!entry || !sources.count(entry) || (i > 0 && entry == ids[0]) || (i == 2 && entry == ids[1]))
                continue;
            spawnsOf[entry].push_back({data.mapid, data.posX, data.posY, data.posZ, data.spawntimesecs});
        }
    }
    LOG_INFO("playerbots",
             "[coop] index: {} quest items from {} creatures and {} chests, {} kill objectives, {} sources spawned, {} "
             "chests spawned",
             itemSources.size(), sources.size(), chests.size(), creditSources.size(), spawnsOf.size(),
             chestSpawnsOf.size());
}

void EnsureIndex() { std::call_once(indexOnce, BuildIndex); }

// a kill of `killed` counts for the objective: its entry or its kill credit; for an item, the creature drops that very
// item (the index of the loot tables), not any quest item of the member's log
bool Serves(Objective const& o, Creature* killed)
{
    CreatureTemplate const* tmpl = killed->GetCreatureTemplate();
    if (!tmpl)
        return false;
    if (!o.item)
        return o.key == tmpl->Entry || o.key == tmpl->KillCredit[0] || o.key == tmpl->KillCredit[1];
    EnsureIndex();
    auto const itr = itemSources.find(o.key);
    return itr != itemSources.end() &&
           std::any_of(itr->second.begin(), itr->second.end(),
                       [tmpl](DropSource const& d) { return !d.chest && d.entry == tmpl->Entry; });
}

// the creature's health at its middle level (the core's base stats)
float TemplateHealth(uint32 entry, uint8& level)
{
    CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(entry);
    if (!tmpl)
        return 0.0f;
    level = uint8((uint32(tmpl->minlevel) + uint32(tmpl->maxlevel)) / 2);
    CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(level, tmpl->unit_class);
    return stats ? float(stats->GenerateHealth(tmpl)) : 0.0f;
}

float PetHealth(Player* p, bool full)
{
    if (Pet* pet = p->GetPet())
        if (pet->IsAlive())
            return float(full ? pet->GetMaxHealth() : pet->GetHealth());
    return 0.0f;
}

// its heals per fight second as its own solo judge counts them (its measured heals on itself, as far as its mana
// lasts); 0 without a judge
float HealRate(Player* p, Unit* sample)
{
    if (!sample)
        return 0.0f;
    SoloSafety::Verdict const v = SoloSafety::Judge(p, sample);
    if (v.fightSec <= 0.0f)
        return 0.0f;
    return std::max(0.0f, v.ehpNow - float(p->GetHealth()) - PetHealth(p, false)) / v.fightSec;
}

// the party's leader: the tank, else the highest level, else the lowest guid
bool Outranks(Player* a, Player* b)
{
    bool const ta = PlayerbotAI::IsTank(a);
    bool const tb = PlayerbotAI::IsTank(b);
    if (ta != tb)
        return ta;
    if (a->GetLevel() != b->GetLevel())
        return a->GetLevel() > b->GetLevel();
    return a->GetGUID().GetCounter() < b->GetGUID().GetCounter();
}

Player* SameMapPlayer(Player* bot, ObjectGuid guid)
{
    Player* p = ObjectAccessor::FindPlayer(guid);
    if (!p || !p->IsInWorld() || p->GetMap() != bot->GetMap())
        return nullptr;
    return p;
}

char const* StatusName(NewRpgStatus status)
{
    static char const* const names[] = {"IDLE",        "GO_GRIND",  "GO_CAMP",       "WANDER_RANDOM",
                                        "WANDER_NPC",  "DO_QUEST",  "TRAVEL_FLIGHT", "REST",
                                        "OUTDOOR_PVP", "DO_GATHER", "DO_PROFESSION"};
    return status < RPG_STATUS_END ? names[status] : "?";
}

// why the bot is not free for co-op now ("" = free): the global leader's work comes first (a call, a walk of its
// own errand), a fight, another group
// why the bot cannot be in a co-op party now ("" = it can). Only game facts (dead, in flight, another group, an
// instance) and an URGENT task of the global leader block it; a convenient task (town errands, a profession buy /
// craft / train that waits for the town) stays queued, a fight ends by itself (the invite works in combat)
std::string Busy(Player* p)
{
    PlayerbotAI* ai = GET_PLAYERBOT_AI(p);
    if (!ai)
        return "no bot";
    if (!p->IsAlive())
        return "dead";
    if (p->IsInFlight())
        return "in flight";
    if (p->GetGroup())
        return "in another group";
    if (p->GetMap()->Instanceable())
        return "in an instance";
    std::string why;
    if (hooks.urgentTask)
        return hooks.urgentTask(p, &why) ? "an urgent task of the global leader: " + why : "";
    // until the global leader's hook is set: what its API shows. A gathering call is its URGENT profession kind
    // (GuildCoordinator::ClassifyTask), DO_GATHER runs only an urgent gather; a guild walk (GO_CAMP, no vendor run
    // of its own) may be a five's or the founding's urgent walk and its urgency is not readable yet
    if (sGuildCoordinator.ProfessionCall(p) == GuildProfessionKind::GATHER)
        return "an urgent task of the global leader: profession gathering";
    NewRpgStatus const status = ai->rpgInfo.GetStatus();
    if (status == RPG_DO_GATHER)
        return "an urgent task of the global leader: gathering";
    if (status == RPG_GO_CAMP && !ai->rpgInfo.vendorRunEntry)
        return "a walk of the global leader (its urgency is not readable before its hook)";
    return "";
}

// the forecast of one pairing (tSoloL / xphSoloL: L alone, or its party without P when it leads one already)
struct Plan
{
    bool ok = false;
    std::string why;
    float tSoloL = 0.0f, tGroupL = 0.0f, tSoloP = 0.0f, tGroupP = 0.0f;
    std::vector<std::pair<uint32, float>> othersXph;  // the members of L's party: their XP per hour with P
    float walkYd = 0.0f;
    float walkSec = 0.0f;  // P's walk to L at its run speed
    float xphSoloL = 0.0f, xphGroupL = 0.0f, xphSoloP = 0.0f, xphGroupP = 0.0f;
    float leadWalkYd = 0.0f;  // L's walk to the spot the party works first
    std::unordered_map<uint32, float> healRate;
    Camp camp;
    ObjectGuid campFor{};
};

struct Side
{
    Player* p = nullptr;
    Posting const* post = nullptr;
    Objective obj;
    Camp const* camp = nullptr;  // a seeker: the camp it cannot solo
    bool hold = false;           // it holds the objective in its log but works another one now
    Pace const* pace = nullptr;  // a holder: the pace of this objective from where it stands (else its posting's)
};

Pace const* PaceOf(Side const& s) { return s.pace ? s.pace : (s.post ? &s.post->pace : nullptr); }

// the offer's walks by route (the leader route search; < 0: not looked at, the straight line): P to L, L to the spot
// the party works first (its camp, its nearest spawn), P to its own nearest spawn (its time alone)
struct Walks
{
    float pToL = -1.0f;
    float lToSpot = -1.0f;
    float pToSpot = -1.0f;
};

// the seconds `units` objective units take at the pace (the formula, no history): per kill the source's health at
// `dps` plus the walk to the next spawn; 1 / chance kills per unit; the spawns shared with `workers` bots on the
// same objective, every kill beyond one per spawn waits for its respawn (the core's respawn time)
float PaceSeconds(Pace const& pace, uint32 units, float dps, float speed, float workers)
{
    if (!pace.known || !units)
        return 0.0f;
    float const kills = float(units) / std::max(pace.chance, 0.0001f);
    float const perKill = pace.hp / std::max(dps, 1.0f) + pace.stepYd / std::max(speed, 1.0f);
    float const spawns = std::max(pace.spawns / std::max(workers, 1.0f), 1.0f);
    if (kills <= spawns)
        return kills * perKill;
    return spawns * perKill + (kills - spawns) * std::max(perKill, pace.respawnSec / spawns);
}

// the formula's pace of the bot's objective `o` worked at `at`: of the creatures that serve it (kill credit, or the
// drop with its chance) the one spawned on its map that closes it fastest at its solo damage
Pace ComputePace(Player* bot, Objective const& o, WorldPosition const& at)
{
    EnsureIndex();
    Pace best;
    best.at = at;
    best.left = o.Left();
    std::vector<DropSource> candidates;
    if (o.item)
    {
        auto const itr = itemSources.find(o.key);
        if (itr != itemSources.end())
            candidates = itr->second;
    }
    else if (auto const itr = creditSources.find(o.key); itr != creditSources.end())
        for (uint32 entry : itr->second)
            candidates.push_back({entry, 1.0f});
    if (candidates.empty())
    {
        best.why = o.item ? "no creature or chest holds item " + std::to_string(o.key)
                          : "no creature gives the kill credit " + std::to_string(o.key);
        return best;
    }
    float const dps = DpsEstimate::EstimateDps(bot, nullptr, DpsEstimate::BuffContext::Solo);
    float const speed = std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
    float bestSec = NO_TIME;
    best.why = "no spawn of its sources on this map";
    for (DropSource const& source : candidates)
    {
        auto const& index = source.chest ? chestSpawnsOf : spawnsOf;
        auto const spawns = index.find(source.entry);
        if (spawns == index.end())
            continue;
        std::vector<std::pair<float, SpawnPoint const*>> around;
        for (SpawnPoint const& s : spawns->second)
            if (s.map == bot->GetMapId())
                around.push_back({at.GetExactDist2d(s.x, s.y), &s});
        if (around.empty())
            continue;
        Pace pace;
        pace.known = true;
        pace.entry = source.entry;
        pace.chance = source.chance;
        pace.chest = source.chest;
        // a chest is opened, not fought: no health, no kill XP (level 0), its walk only
        pace.hp = source.chest ? 0.0f : TemplateHealth(source.entry, pace.level);
        if (!source.chest && pace.hp <= 0.0f)
            continue;
        pace.at = at;
        pace.left = best.left;
        pace.spawns = float(around.size());
        // the spawns it works: as many as the kills it needs, the nearest to its spot
        size_t const kills = size_t(std::ceil(float(std::max<uint32>(best.left, 1)) / pace.chance));
        size_t const worked = std::min(around.size(), std::max<size_t>(kills, 1));
        std::partial_sort(around.begin(), around.begin() + worked, around.end(),
                          [](auto const& a, auto const& b) { return a.first < b.first; });
        float reach = NO_TIME;
        float respawn = 0.0f;
        float steps = 0.0f;
        for (size_t i = 0; i < worked; ++i)
        {
            SpawnPoint const& s = *around[i].second;
            float const gapBot = bot->GetExactDist2d(s.x, s.y);
            if (gapBot < reach)
            {
                reach = gapBot;
                pace.first.Relocate(s.x, s.y, s.z);
            }
            respawn += float(s.respawnSec);
            float gap = NO_TIME;
            for (size_t j = 0; j < worked; ++j)
                if (j != i)
                    gap = std::min(gap, std::hypot(s.x - around[j].second->x, s.y - around[j].second->y));
            steps += gap < NO_TIME ? gap : 0.0f;
        }
        pace.reachYd = reach;
        pace.respawnSec = respawn / float(worked);
        pace.stepYd = steps / float(worked);
        float const seconds = pace.reachYd / speed + PaceSeconds(pace, best.left, dps, speed, 1.0f);
        if (seconds < bestSec)
        {
            bestSec = seconds;
            best = pace;
        }
    }
    return best;
}

// how one kill's objective units reach a party (the core): a kill credit reaches every member at the group reward
// distance (KillRewarder::_RewardKillCredit); a quest drop is rolled once per corpse (LootTemplate::Process) and is one
// copy the first looter takes (Loot::FillQuestLoot), unless the item is MULTI_DROP (freeforall: a copy for each)
bool EachGetsIt(Objective const& o)
{
    if (!o.item)
        return true;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(o.key);
    return proto && proto->HasFlag(ITEM_FLAG_MULTI_DROP);
}

// the core's XP for one kill of `entry` at `mobLevel` (Acore::XP::Gain: elite x2, ModExperience, the realm's rate;
// KillRewarder::_InitXP:
// x ModHealth at 0.75 or less) for `m` alone (`party` of one) or with the alive `party` at the reward distance
// (KillRewarder: the base of its highest member the mob is not gray for x the group rate x its level share; half when
// the mob is gray for one of them; none for a member above that level)
float KillXp(Player* m, uint32 entry, uint8 mobLevel, std::vector<Player*> const& party)
{
    if (!mobLevel)
        return 0.0f;
    float mod = 1.0f;
    if (CreatureTemplate const* tmpl = entry ? sObjectMgr->GetCreatureTemplate(entry) : nullptr)
    {
        if (tmpl->rank != CREATURE_ELITE_NORMAL && tmpl->rank != CREATURE_ELITE_RARE)
            mod *= 2.0f;
        mod *= tmpl->ModExperience;
        if (tmpl->ModHealth <= 0.75f && tmpl->ModHealth >= 0.0f)
            mod *= tmpl->ModHealth;
    }
    // the realm's kill XP rate (Acore::XP::Gain: Rate.XP.Kill outside a battleground)
    mod *= sWorld->getRate(RATE_XP_KILL);
    ContentLevels const content = GetContentLevelsForMapAndZone(m->GetMapId(), m->GetZoneId());
    if (party.size() < 2)
        return float(Acore::XP::BaseGain(m->GetLevel(), mobLevel, content)) * mod;
    uint8 maxLevel = 0;
    uint8 maxNotGray = 0;
    uint32 sumLevel = 0;
    for (Player* p : party)
    {
        maxLevel = std::max(maxLevel, p->GetLevel());
        sumLevel += p->GetLevel();
        if (mobLevel > Acore::XP::GetGrayLevel(p->GetLevel()))
            maxNotGray = std::max(maxNotGray, p->GetLevel());
    }
    if (!maxNotGray || m->GetLevel() > maxNotGray)
        return 0.0f;
    float const xp = float(Acore::XP::BaseGain(maxNotGray, mobLevel, content)) * mod *
                     Acore::XP::xp_in_group_rate(uint32(party.size()), false) * float(m->GetLevel()) /
                     float(std::max<uint32>(sumLevel, 1));
    return maxLevel == maxNotGray ? xp : xp / 2.0f + 1.0f;
}

// one member of a forecast party
struct Member
{
    Side side;
    uint32 left = 0;
    float dpsGroup = 0.0f;
    float dpsSolo = 0.0f;
    float speed = 1.0f;
};

// what the party run gives one member: it closes its counter (or, a camp it does not solo, leaves with the party's
// end) after `seconds`, with `done` units and `xp` kill XP
struct Outcome
{
    float seconds = 0.0f;
    uint32 done = 0;
    float xp = 0.0f;
};

// the party `ms` (ms[0] leads: its spots, or the camp it was formed for) works the objective from `startSec` on. Each
// member leaves when its counter closes (the one that needs fewer never waits); with one left the party ends and it
// closes the rest alone at its own pace. `each`: every kill credits every member (kill credit, a MULTI_DROP drop);
// else one drop per corpse for the whole party, shared out evenly among the members that still need it (the core
// gives it to whoever loots first): the party then has to make the sum of the counters
std::vector<Outcome> PartyRun(std::vector<Member> const& ms, Camp const* camp, float startSec, bool each, float workers)
{
    size_t const n = ms.size();
    std::vector<Outcome> out(n);
    if (!n)
        return out;
    Member const& lead = ms.front();
    Pace const* pace = PaceOf(lead.side);
    float const partyWorkers = std::max(workers - float(n - 1), 1.0f);
    // PaceSeconds on whole units: interpolated between them
    auto const paceSec = [&](Pace const& at, float units, float dps)
    {
        if (!at.known || units <= 0.0f)
            return 0.0f;
        uint32 const whole = uint32(units);
        float const lo = PaceSeconds(at, whole, dps, lead.speed, partyWorkers);
        float const hi = PaceSeconds(at, whole + 1, dps, lead.speed, partyWorkers);
        return lo + (hi - lo) * (units - float(whole));
    };
    // a camp that only stands on the way: one clear of it, then the objective's sources past it
    bool const pastCamp = camp && !camp->serves && camp->pace.known;
    // the seconds the party needs for `units` of its yield at `dps` (the camp's fight, else the leader's spawns)
    auto const yieldSec = [&](float units, float dps)
    {
        if (units <= 0.0f)
            return 0.0f;
        float const clear = camp ? camp->fightSec * camp->dps / std::max(dps, 1.0f) : 0.0f;
        if (pastCamp)
            return clear + paceSec(camp->pace, units, dps);
        if (camp)
            return units * clear / float(std::max<uint32>(camp->units, 1));
        return pace ? paceSec(*pace, units, dps) : 0.0f;
    };
    Pace const* workPace = pastCamp ? &camp->pace : (camp ? nullptr : pace);
    float const killsPerUnit = workPace && workPace->known
                                   ? 1.0f / std::max(workPace->chance, 0.0001f)
                                   : (camp ? float(std::max<uint32>(camp->mobs, 1)) /
                                                 float(std::max<uint32>(camp->units, 1))
                                           : 1.0f);
    uint32 const entry = workPace ? workPace->entry : (camp ? camp->entry : 0);
    uint8 const mobLevel = workPace ? workPace->level : (camp ? camp->topLevel : 0);  // a chest: 0, no kill XP

    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&ms](size_t a, size_t b) { return ms[a].left < ms[b].left; });
    std::vector<size_t> active = order;
    float t = startSec;
    float cum = 0.0f;
    uint32 prevLeft = 0;
    float dps = 0.0f;
    for (Member const& m : ms)
        dps += m.dpsGroup;
    size_t next = 0;
    bool first = true;
    while (active.size() >= 2)
    {
        uint32 const target = ms[order[next]].left;
        float const yield = float(target - prevLeft) * (each ? 1.0f : float(active.size()));
        float const dt = yieldSec(cum + yield, dps) - yieldSec(cum, dps);
        std::vector<Player*> players;
        for (size_t i : active)
            players.push_back(ms[i].side.p);
        for (size_t i : active)
        {
            out[i].xp += yield * killsPerUnit * KillXp(ms[i].side.p, entry, mobLevel, players);
            if (first && pastCamp)  // the clear's own kills
                out[i].xp += float(camp->mobs) * KillXp(ms[i].side.p, camp->entry, camp->topLevel, players);
        }
        first = false;
        t += dt;
        cum += yield;
        prevLeft = target;
        while (next < n && ms[order[next]].left == target)
        {
            size_t const i = order[next++];
            out[i].seconds = t;
            out[i].done = ms[i].left;
            dps -= ms[i].dpsGroup;
            active.erase(std::find(active.begin(), active.end(), i));
        }
    }
    if (active.empty())
        return out;
    // the last one: the party has ended
    size_t const i = active.front();
    Member const& m = ms[i];
    uint32 const rest = m.left - prevLeft;
    Pace const* mine = PaceOf(m.side);
    Pace const* own = mine && mine->known ? mine : workPace;
    if (m.side.camp || !own || !own->known)
    {
        out[i].seconds = t;
        out[i].done = prevLeft;
        return out;
    }
    out[i].seconds = t + PaceSeconds(*own, rest, m.dpsSolo, m.speed, workers);
    out[i].done = m.left;
    out[i].xp += float(rest) / std::max(own->chance, 0.0001f) * KillXp(m.side.p, own->entry, own->level, {m.side.p});
    return out;
}

// the XP per hour of `units` units of `o` and `killXp` over `seconds` (the quest's XP share of the units closed)
float XpPerHour(Player* m, Objective const& o, uint32 units, float killXp, float seconds)
{
    if (seconds <= 0.0f || seconds >= NO_TIME)
        return 0.0f;
    Quest const* quest = sObjectMgr->GetQuestTemplate(o.questId);
    // the reward as the core gives it (Player::RewardQuest: XPValue x the quest rate)
    float const questXp = quest && o.need ? float(quest->XPValue(m->GetLevel())) * m->GetQuestRate(quest->IsDFQuest()) *
                                                units / o.need
                                          : 0.0f;
    return (questXp + killXp) / seconds * HOUR;
}

// L leads (it stays on its work), P walks to it; `others`: the members of the party L leads already (none: a new
// party). `workers`: the bots working this board (they share its spawns); `walks`: the routes the offer looked at (P to
// L, L to its spot, P to its own spawns; else the straight lines). Every member must gain: L and P against alone, the
// members already in L's party against that party without P
Plan Forecast(Side const& l, std::vector<Side> const& others, Side const& pr, float workers, Walks const& walks = {})
{
    Plan plan;
    Player* const lp = l.p;
    Player* const pp = pr.p;
    float const speedL = std::max(lp->GetSpeed(MOVE_RUN), 1.0f);
    float const speedP = std::max(pp->GetSpeed(MOVE_RUN), 1.0f);
    plan.walkYd = walks.pToL >= 0.0f ? walks.pToL : lp->GetExactDist2d(pp);
    plan.walkSec = plan.walkYd / speedP;
    uint32 const rL = l.obj.Left();
    uint32 const rP = pr.obj.Left();
    if (!rL || !rP)
    {
        plan.why = "calc: a counter is closed";
        return plan;
    }

    // each side's pace by the formula (its objective's source creature at the core's base health over its damage,
    // the walk between the spawns it works, the drop chance, the respawn of spawns shared with the board): no
    // measured history
    Pace const* paceL = l.camp ? nullptr : PaceOf(l);
    Pace const* paceP = pr.camp ? nullptr : PaceOf(pr);
    for (auto const& [who, pace] : {std::make_pair(lp, paceL), std::make_pair(pp, paceP)})
        if (pace && !pace->known)
        {
            plan.why = "calc: " + who->GetName() + " has no pace on it: " +
                       (pace->why.empty() ? std::string("its objective is not on the board yet") : pace->why);
            return plan;
        }

    auto const member = [](Side const& s)
    {
        Member m;
        m.side = s;
        m.left = s.obj.Left();
        m.dpsGroup = DpsEstimate::EstimateDps(s.p, nullptr, DpsEstimate::BuffContext::Group);
        m.dpsSolo = DpsEstimate::EstimateDps(s.p, nullptr, DpsEstimate::BuffContext::Solo);
        m.speed = std::max(s.p->GetSpeed(MOVE_RUN), 1.0f);
        return m;
    };
    std::vector<Member> before{member(l)};
    for (Side const& s : others)
        if (s.obj.Left())
            before.push_back(member(s));
    std::vector<Member> with = before;
    with.push_back(member(pr));
    float sumDps = 0.0f;
    for (Member const& m : with)
        sumDps += m.dpsGroup;

    // the party works the leader's objective; the core's share of one kill among its members
    bool const each = EachGetsIt(l.obj);
    Camp const* camp = l.camp ? l.camp : pr.camp;
    for (Side const& s : others)
        if (!camp && s.camp)
            camp = s.camp;

    // a camp the seeker could not solo: the party must win it (the pack's damage on the leader over the time the
    // party's damage needs, against the leader's health and the party's heals), and the core's con rule holds for
    // the leader (no orange / red mob is pulled)
    if (camp)
    {
        // a pack that only stands on the way: the party still has to work the objective past it, at the pace of its
        // sources there (no pace, no forecast)
        if (l.camp && !l.camp->serves && !l.camp->pace.known)
        {
            plan.why = "calc: the camp's mobs do not serve the objective and its sources past the camp have no pace: " +
                       (l.camp->pace.why.empty() ? std::string("not computed") : l.camp->pace.why);
            return plan;
        }
        if (Acore::XP::GetColorCode(lp->GetLevel(), camp->topLevel) <= XP_ORANGE)
        {
            plan.why = Acore::StringFormat("calc: the camp's L{} is orange / red for {} L{}", camp->topLevel,
                                           lp->GetName(), lp->GetLevel());
            return plan;
        }
        Unit* mob = ObjectAccessor::GetCreature(*lp, camp->mob);
        float incoming = camp->fightSec > 0.0f ? camp->damage / camp->fightSec : 0.0f;
        if (!l.camp && mob && mob->IsAlive())
        {
            SoloSafety::Verdict const v = SoloSafety::Judge(lp, mob);  // the leader takes the hits
            if (v.fightSec > 0.0f)
                incoming = v.damage / v.fightSec;
        }
        float const seconds = camp->fightSec * camp->dps / std::max(sumDps, 1.0f);
        float heals = 0.0f;
        for (Member const& m : with)
        {
            float const rate = mob && mob->IsAlive() ? HealRate(m.side.p, mob) : 0.0f;
            plan.healRate[m.side.p->GetGUID().GetCounter()] = rate;
            if (m.side.p == lp || PlayerbotAI::IsHeal(m.side.p))
                heals += rate * seconds;
        }
        float const damage = incoming * seconds;
        float const ehp = float(lp->GetMaxHealth()) + PetHealth(lp, true) + heals;
        if (damage >= ehp)
        {
            plan.why = Acore::StringFormat(
                "calc: the party loses the camp too: {:.0f} dmg on {} over {:.0f} s, "
                "ehp {:.0f}",
                damage, lp->GetName(), seconds, ehp);
            return plan;
        }
        plan.camp = *camp;
        plan.campFor = l.camp ? lp->GetGUID() : (pr.camp ? pp->GetGUID() : ObjectGuid::Empty);
    }
    // the party's spots: the camp of its leader, else the leader's own spawns
    Camp const* workCamp = l.camp;

    // alone: its walk to its first spawn (its route when the offer looked at it), then its own pace, its kill XP alone
    auto const solo = [&](Member const& m, float& xph)
    {
        Pace const* pace = m.side.camp ? nullptr : PaceOf(m.side);
        if (!pace || !pace->known)
        {
            xph = 0.0f;
            return NO_TIME;
        }
        float const routed = m.side.p == lp ? walks.lToSpot : (m.side.p == pp ? walks.pToSpot : -1.0f);
        float const reach = routed >= 0.0f ? routed : pace->reachYd;
        float const seconds = reach / m.speed + PaceSeconds(*pace, m.left, m.dpsSolo, m.speed, workers);
        float const kills = float(m.left) / std::max(pace->chance, 0.0001f);
        xph = XpPerHour(m.side.p, m.side.obj, m.left,
                        kills * KillXp(m.side.p, pace->entry, pace->level, {m.side.p}), seconds);
        return seconds;
    };

    // together: P's walk to L first (the leader's GROUP_WAIT holds the party for it), the leader's walk to its spot:
    // its camp, else its nearest spawn (its route when the offer looked at it, else the straight line)
    float const reachYdL = walks.lToSpot >= 0.0f ? walks.lToSpot
                           : l.camp              ? lp->GetExactDist(&l.camp->at)
                           : paceL               ? paceL->reachYd
                                                 : 0.0f;
    float const reachL = reachYdL / speedL;
    plan.leadWalkYd = reachYdL;
    std::vector<Outcome> const runWith = PartyRun(with, workCamp, plan.walkSec + reachL, each, workers);
    std::vector<Outcome> runBefore;
    if (before.size() >= 2)
        runBefore = PartyRun(before, workCamp, reachL, each, workers);

    std::string loser;
    auto const judge = [&](size_t i, float baseSec, float& tBase, float& tWith, float& xphWith)
    {
        Outcome const& o = runWith[i];
        tBase = baseSec;
        tWith = o.seconds;
        xphWith = XpPerHour(with[i].side.p, with[i].side.obj, o.done, o.xp, o.seconds);
        bool const gains = with[i].side.camp ? o.done > 0 : (baseSec >= NO_TIME || o.seconds < baseSec);
        if (!gains && loser.empty())
            loser = with[i].side.p->GetName();
    };
    // L: alone (a new party) or its party without P
    float baseL = NO_TIME;
    if (runBefore.empty())
        baseL = solo(with[0], plan.xphSoloL);
    else
    {
        baseL = runBefore[0].seconds;
        plan.xphSoloL = XpPerHour(lp, l.obj, runBefore[0].done, runBefore[0].xp, runBefore[0].seconds);
    }
    if (l.camp && runBefore.empty())
        baseL = NO_TIME;
    judge(0, baseL, plan.tSoloL, plan.tGroupL, plan.xphGroupL);
    for (size_t i = 1; i + 1 < with.size(); ++i)
    {
        float tBase = 0.0f, tWith = 0.0f, xphWith = 0.0f;
        judge(i, runBefore.size() > i ? runBefore[i].seconds : NO_TIME, tBase, tWith, xphWith);
        plan.othersXph.push_back({with[i].side.p->GetGUID().GetCounter(), xphWith});
    }
    size_t const iP = with.size() - 1;
    float const baseP = pr.camp ? NO_TIME : solo(with[iP], plan.xphSoloP);
    judge(iP, baseP, plan.tSoloP, plan.tGroupP, plan.xphGroupP);

    auto const secs = [](float s) { return s >= NO_TIME ? std::string("never") : std::to_string(uint32(s)) + " s"; };
    plan.why = Acore::StringFormat(
        "{} {}/{} {} {} vs {} together; {} {}/{} {} alone vs {} together; walk {:.0f} yd; {}, a party of {}",
        lp->GetName(), l.obj.have, l.obj.need, secs(plan.tSoloL), runBefore.empty() ? "alone" : "in its party",
        secs(plan.tGroupL), pp->GetName(), pr.obj.have, pr.obj.need, secs(plan.tSoloP), secs(plan.tGroupP),
        plan.walkYd, each ? "every kill credits each" : "one drop per corpse for the party", uint32(with.size()));
    plan.ok = loser.empty();
    if (!plan.ok)
        plan.why = "calc: alone is faster for " + loser + ": " + plan.why;
    return plan;
}

std::string QuestList(Objective const& a, Objective const& b)
{
    return "q" + std::to_string(a.questId) + ",q" + std::to_string(b.questId);
}

void NoteRefusal(Out& out, Player* leader, Player* partner, std::string const& why)
{
    uint64 const key = PairKey(leader->GetGUID().GetCounter(), partner->GetGUID().GetCounter());
    // one line per kind of reason: the forecast's numbers change with every counter step
    std::string kind;
    for (char c : why)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            kind += c;
    std::string& last = lastRefusal[key];
    if (last == kind)
        return;
    last = kind;
    out.logs.push_back(Acore::StringFormat("[coop] refuse {} from={}{} reason={}", partner->GetName(),
                                           leader->GetName(), Stamp(), why));
}

// world thread: the leader invites, the member accepts (WorldSession::HandleGroupInviteOpcode /
// HandleGroupAcceptOpcode), then both take the party's objective as their quest
class CoopFormOperation : public PlayerbotOperation
{
public:
    CoopFormOperation(uint32 partyId, ObjectGuid leader, ObjectGuid member)
        : _party(partyId), _leader(leader), _member(member)
    {
    }

    bool Execute() override;
    ObjectGuid GetBotGuid() const override { return _leader; }
    uint32 GetPriority() const override { return 50; }
    std::string GetName() const override { return "CoopForm"; }

private:
    uint32 _party;
    ObjectGuid _leader;
    ObjectGuid _member;
};

// world thread: the member leaves (with one left the core disbands the group); a new leader takes the lead
class CoopLeaveOperation : public PlayerbotOperation
{
public:
    CoopLeaveOperation(ObjectGuid group, ObjectGuid member, ObjectGuid newLeader)
        : _group(group), _member(member), _newLeader(newLeader)
    {
    }

    bool Execute() override
    {
        Group* group = sGroupMgr->GetGroupByGUID(_group.GetCounter());
        if (!group || !group->IsMember(_member))
            return false;
        group->RemoveMember(_member);
        group = sGroupMgr->GetGroupByGUID(_group.GetCounter());
        if (group && _newLeader && group->IsMember(_newLeader) && !group->IsLeader(_newLeader))
            group->ChangeLeader(_newLeader);
        return true;
    }
    ObjectGuid GetBotGuid() const override { return _member; }
    uint32 GetPriority() const override { return 50; }
    std::string GetName() const override { return "CoopLeave"; }

private:
    ObjectGuid _group;
    ObjectGuid _member;
    ObjectGuid _newLeader;
};

// the bot leaves its party (lock held): exit + gain lines, the core leave; with one left the party ends
void LeaveLocked(Posting& me, std::string const& reason, uint32 now, Out& out)
{
    auto const itr = parties.find(me.party);
    me.party = 0;
    MarkParty(me.guid.GetCounter(), false);
    if (itr == parties.end())
        return;
    Party& party = itr->second;
    auto const finish = [&](Posting& p, std::string const& why)
    {
        float const seconds = float(getMSTimeDiff(p.joinedAt, now)) / IN_MILLISECONDS;
        // its counter as its own party tick last read it (no read of another map thread's player)
        uint32 const units = p.partyObj.have > p.haveAtJoin ? p.partyObj.have - p.haveAtJoin : 0;
        Quest const* quest = sObjectMgr->GetQuestTemplate(p.partyObj.questId);
        float const questXp = quest && p.partyObj.need ? float(quest->XPValue(p.level)) *
                                                             (quest->IsDFQuest() ? p.questRateDf : p.questRate) *
                                                             units / p.partyObj.need
                                                       : 0.0f;
        float const got = seconds > 0.0f ? (float(p.xp) + questXp) / seconds * HOUR : 0.0f;
        out.logs.push_back(Acore::StringFormat("[coop] exit {} group={} after={:.0f} units={} kills={}{} reason={}",
                                               p.name, party.id, seconds, units, p.sharedKills, Stamp(), why));
        // got: its XP per hour in the party (kill XP as the core gave it + the quest's share of the units closed at
        // the core's quest rate, the forecast's units); forecast: the offer's
        // XP per hour together, solo: alone (never: it cannot close it alone - no ratio); s: the seconds it was in the
        // party (to weigh the rates by); share: each (every kill credits each) or pooled (one drop per corpse)
        out.logs.push_back(Acore::StringFormat(
            "[coop] gain {} group={} got={:.0f} forecast={:.0f} solo={} s={:.0f} share={}{}", p.name, party.id, got,
            p.groupXph, p.soloNever ? std::string("never") : std::to_string(uint32(p.soloXph)), seconds,
            EachGetsIt(p.partyObj) ? "each" : "pooled", Stamp()));
        out.after.push_back([name = p.name, low = p.guid.GetCounter(), q = p.partyObj.questId, why]()
                            { EndTask(name, low, q, why); });
        p.sharedKills = 0;
        p.xp = 0;
    };
    if (party.formed)
        finish(me, reason);
    party.members.erase(std::remove(party.members.begin(), party.members.end(), me.guid), party.members.end());
    bool const wasLeader = party.leader == me.guid;
    if (party.members.size() <= 1)
    {
        for (ObjectGuid const& guid : party.members)
        {
            auto const other = postings.find(guid.GetCounter());
            if (other == postings.end())
                continue;
            if (party.formed)
                finish(other->second, "partner left (" + me.name + ": " + reason + ")");
            other->second.party = 0;
            MarkParty(guid.GetCounter(), false);
        }
        if (party.formed)
            out.ops.push_back(std::make_unique<CoopLeaveOperation>(party.group, me.guid, ObjectGuid::Empty));
        parties.erase(itr);
        return;
    }
    ObjectGuid newLeader{};
    if (wasLeader)
    {
        Player* best = nullptr;
        for (ObjectGuid const& guid : party.members)
            if (Player* p = ObjectAccessor::FindPlayer(guid))
                if (!best || Outranks(p, best))
                    best = p;
        party.leader = best ? best->GetGUID() : party.members.front();
        newLeader = party.leader;
    }
    if (party.formed)
        out.ops.push_back(std::make_unique<CoopLeaveOperation>(party.group, me.guid, newLeader));
}

// a route's yards to `to`: its walk, a partial one plus the straight rest
float RouteYd(Leader::RouteResult const& route, Position const& to)
{
    if (route.complete || route.points.empty())
        return route.length;
    G3D::Vector3 const& last = route.points.back();
    return route.length + to.GetExactDist(last.x, last.y, last.z);
}

// the bot's side on the bucket `key` (lock held): the objective it works there, one it looks for a party for (its
// camp), or one it holds in its log (read again from its log; the caller computes its pace from where it stands)
bool ResolveSide(Posting const& post, Player* p, uint64 key, Side& side)
{
    if (post.bucket == key)
    {
        side.obj = post.cur;
        return true;
    }
    auto const seek =
        std::find_if(post.seeks.begin(), post.seeks.end(), [key](Seek const& s) { return s.bucket == key; });
    if (seek != post.seeks.end())
    {
        side.obj = seek->obj;
        side.camp = &seek->camp;
        return true;
    }
    auto const hold =
        std::find_if(post.holds.begin(), post.holds.end(), [key](Hold const& h) { return h.bucket == key; });
    if (hold == post.holds.end())
        return false;
    if (!ReadObjective(p, hold->obj.questId, hold->obj.index, side.obj) || !side.obj.Left())
        return false;
    side.hold = true;
    return true;
}

// the leader's board look at one bucket (lock held): the best partner it leads; the offer
void EvaluateLocked(PlayerbotAI* /*botAI*/, Player* bot, Posting& me, uint64 key, Out& out)
{
    auto const bucket = buckets.find(key);
    if (bucket == buckets.end())
        return;
    me.seen[key] = bucket->second.version;

    // my side on this board: the objective it works, one it looks for a party for, or one it only holds in its log
    // (then only a seeker on the board is worth a look: a party for a camp one cannot solo)
    Side mine;
    mine.p = bot;
    mine.post = &me;
    Pace minePace;
    if (!ResolveSide(me, bot, key, mine))
        return;
    if (mine.hold)
    {
        if (me.party || std::none_of(bucket->second.lows.begin(), bucket->second.lows.end(),
                                     [key](uint32 low)
                                     {
                                         auto const po = postings.find(low);
                                         return po != postings.end() && !po->second.party &&
                                                std::any_of(po->second.seeks.begin(), po->second.seeks.end(),
                                                            [key](Seek const& s) { return s.bucket == key; });
                                     }))
            return;
        minePace = ComputePace(bot, mine.obj, WorldPosition(bot));
        mine.pace = &minePace;
    }

    // a party it leads takes more members, up to the core's party size
    Party* led = nullptr;
    if (me.party)
    {
        auto const itr = parties.find(me.party);
        if (itr == parties.end() || !itr->second.formed || itr->second.leader != bot->GetGUID() ||
            itr->second.members.size() >= MAXGROUPSIZE)
            return;
        led = &itr->second;
    }
    // the global leader's work comes first for the leader too
    if (!led && !Busy(bot).empty())
        return;
    // the members of the party it leads already: each must still gain with one more
    std::vector<Side> others;
    if (led)
        for (ObjectGuid const& guid : led->members)
        {
            if (guid == bot->GetGUID())
                continue;
            Player* m = SameMapPlayer(bot, guid);
            auto const post = postings.find(guid.GetCounter());
            if (!m || post == postings.end())
                continue;
            Side side;
            side.p = m;
            side.post = &post->second;
            side.obj = post->second.partyObj;
            side.camp = led->camp.known && led->campFor == guid ? &led->camp : nullptr;
            others.push_back(side);
        }

    // the bots on this board that work it share its spawns (their respawns bound the pace of each); a holder works
    // another objective now
    float const workers = std::max(1.0f, float(std::count_if(bucket->second.lows.begin(), bucket->second.lows.end(),
                                                             [key](uint32 low)
                                                             {
                                                                 auto const po = postings.find(low);
                                                                 return po != postings.end() &&
                                                                        WorksOn(po->second, key);
                                                             })));
    std::unordered_map<uint32, Pace> holdPaces;  // the holders' paces of this objective (the lows' entries)
    Plan best;
    Player* bestPartner = nullptr;
    Side bestSide;
    Objective bestObj;
    for (uint32 low : bucket->second.lows)
    {
        if (low == bot->GetGUID().GetCounter())
            continue;
        auto const po = postings.find(low);
        if (po == postings.end() || po->second.party)
            continue;
        Player* p = SameMapPlayer(bot, po->second.guid);
        if (!p || p->GetZoneId() != me.zone)
            continue;
        // the one that would lead offers; the other waits for its look
        if (!led && Outranks(p, bot))
            continue;
        Side other;
        other.p = p;
        other.post = &po->second;
        if (!ResolveSide(po->second, p, key, other))
            continue;
        if (led && (mine.camp || other.hold))
            continue;  // a party already formed works where it works
        // a holder pairs only with a seeker (a camp one cannot solo): the board's pairs of the same work stay as
        // they were
        if ((mine.hold || other.hold) && !(mine.camp || other.camp))
            continue;
        std::string const busy = Busy(p);
        if (!busy.empty())
        {
            NoteRefusal(out, bot, p, "busy: " + busy);
            continue;
        }
        if (other.hold)
        {
            Pace& paceP = holdPaces[low];
            paceP = ComputePace(p, other.obj, WorldPosition(p));
            other.pace = &paceP;
        }
        Plan plan = Forecast(mine, others, other, workers);
        if (!plan.ok)
        {
            NoteRefusal(out, bot, p, plan.why);
            continue;
        }
        float const saved = (plan.tSoloP >= NO_TIME ? HOUR : plan.tSoloP - plan.tGroupP);
        float const bestSaved = bestPartner ? (best.tSoloP >= NO_TIME ? HOUR : best.tSoloP - best.tGroupP) : -1.0f;
        if (!bestPartner || saved > bestSaved)
        {
            best = std::move(plan);
            bestPartner = p;
            bestSide = other;
            bestObj = other.obj;
        }
    }
    if (!bestPartner)
        return;

    // the partner's walk to the leader as its own walk would go (the leader route search, its solo pricing of the
    // packs on the way): the reach is checked before the offer, and the forecast takes the route's length
    {
        Leader::RouteResult const route = Leader::Route(bestPartner, bot->GetPosition());
        std::string why;
        if (!route.found)
            why = "calc: no walk from " + bestPartner->GetName() + " to " + bot->GetName() + " (" + route.Describe() +
                  ")";
        else if (route.throughRefused)
            why = "calc: " + bestPartner->GetName() + "'s way to " + bot->GetName() +
                  " passes a pack it does not fight and no walk goes round it";
        else
        {
            Walks walks;
            walks.pToL = RouteYd(route, *bot);
            // a new party: the leader's own walk to the spot the party works first (its camp, its nearest spawn) by
            // route, checked like the partner's (the camp's own pack is the party's fight, not a refusal); the
            // partner's walk to its own nearest spawn prices its time alone
            if (!led)
            {
                Pace const* paceL = mine.camp ? nullptr : PaceOf(mine);
                bool const spotKnown = mine.camp || (paceL && paceL->known);
                Position const spot = mine.camp ? Position(mine.camp->at) : (paceL ? paceL->first : Position());
                if (spotKnown)
                {
                    Leader::RouteResult const lead = Leader::Route(bot, spot);
                    if (!lead.found)
                        why = "calc: no walk from " + bot->GetName() + " to the spot it works (" + lead.Describe() +
                              ")";
                    else if (lead.throughRefused && !mine.camp)
                        why = "calc: " + bot->GetName() +
                              "'s way to the spot it works passes a pack it does not fight and no walk goes round it";
                    else
                        walks.lToSpot = RouteYd(lead, spot);
                }
                Pace const* paceP = bestSide.camp ? nullptr : PaceOf(bestSide);
                if (why.empty() && paceP && paceP->known)
                    walks.pToSpot = Leader::WalkYd(bestPartner, paceP->first);
            }
            if (why.empty())
            {
                best = Forecast(mine, others, bestSide, workers, walks);
                if (!best.ok)
                    why = best.why + " (route " + std::to_string(uint32(walks.pToL)) + " yd)";
            }
        }
        if (!why.empty())
        {
            NoteRefusal(out, bot, bestPartner, why);
            return;
        }
    }

    uint32 const id = led ? led->id : nextParty++;
    Party& party = led ? *led : parties[id];
    if (!led)
    {
        party.id = id;
        party.leader = bot->GetGUID();
        party.members = {bot->GetGUID()};
        party.camp = best.camp;
        party.campFor = best.campFor;
        // a leader that held the objective but worked another one: the party's spot is its nearest spawn
        if (mine.hold && !mine.camp)
            if (Pace const* paceL = PaceOf(mine); paceL && paceL->known)
            {
                party.spot = WorldPosition(bot->GetMapId(), paceL->first.GetPositionX(), paceL->first.GetPositionY(),
                                           paceL->first.GetPositionZ());
                party.spotSet = true;
            }
        me.party = id;
        me.partyObj = mine.obj;
    }
    for (auto const& [low, rate] : best.healRate)
        party.healRate[low] = rate;
    party.members.push_back(bestPartner->GetGUID());
    Posting& other = postings[bestPartner->GetGUID().GetCounter()];
    other.party = id;
    other.partyObj = bestObj;
    other.walkYd = best.walkYd;
    other.walkSec = best.walkSec;
    other.walkTo = WorldPosition();
    other.soloNever = best.tSoloP >= NO_TIME;
    other.soloXph = best.xphSoloP;
    other.groupXph = best.xphGroupP;
    for (auto const& [low, xph] : best.othersXph)
        if (auto const m = postings.find(low); m != postings.end())
            m->second.groupXph = xph;
    if (!led)
    {
        me.walkYd = 0.0f;
        me.walkSec = 0.0f;
        me.soloNever = best.tSoloL >= NO_TIME;
        me.soloXph = best.xphSoloL;
        me.groupXph = best.xphGroupL;
    }
    else
        me.groupXph = best.xphGroupL;
    lastRefusal.erase(PairKey(bot->GetGUID().GetCounter(), bestPartner->GetGUID().GetCounter()));
    // gain / solo: the pair's XP per hour together / alone (solo=never: one of them cannot close it alone, a camp it
    // does not solo - no ratio); t_*: seconds to close each counter, walk: the route's yards, eta: its seconds
    bool const never = best.tSoloL >= NO_TIME || best.tSoloP >= NO_TIME;
    auto const secs = [](float t) { return t >= NO_TIME ? std::string("never") : std::to_string(uint32(t)); };
    out.logs.push_back(Acore::StringFormat(
        "[coop] offer {} to={} group={} quests={} gain={:.0f} solo={} t_solo={} t_group={:.0f} p_solo={} "
        "p_group={:.0f} walk={:.0f} eta={:.0f} lead_walk={:.0f} prog={}/{} partner={}/{} holder={}{} reason={}{}",
        bot->GetName(), bestPartner->GetName(), id, QuestList(mine.obj, bestObj), best.xphGroupL + best.xphGroupP,
        never ? std::string("never") : std::to_string(uint32(best.xphSoloL + best.xphSoloP)), secs(best.tSoloL),
        best.tGroupL, secs(best.tSoloP), best.tGroupP, best.walkYd, best.walkSec, best.leadWalkYd, mine.obj.have,
        mine.obj.need, bestObj.have, bestObj.need,
        mine.hold ? bot->GetName() : (bestSide.hold ? bestPartner->GetName() : std::string("-")), Stamp(), best.camp.known ? "a camp one cannot solo; " : "same objective; ", best.why));
    out.ops.push_back(std::make_unique<CoopFormOperation>(id, bot->GetGUID(), bestPartner->GetGUID()));
}

// the bot's own entry on the board (lock held): its objective and counter; its solo pace by the formula
void UpdatePostingLocked(PlayerbotAI* botAI, Player* bot, Posting& me)
{
    uint32 const low = bot->GetGUID().GetCounter();
    if (me.guid.IsEmpty())
    {
        me.guid = bot->GetGUID();
        me.name = bot->GetName();
    }
    // a level gained: the solo judge looks at its camps again (the postponed quests come back with the level)
    if (me.level != bot->GetLevel())
    {
        me.seeks.clear();
        me.level = bot->GetLevel();
        me.questRate = bot->GetQuestRate(false);
        me.questRateDf = bot->GetQuestRate(true);
    }
    me.map = bot->GetMapId();
    me.zone = bot->GetZoneId();
    // a sought objective done or gone from its log leaves the board
    for (auto itr = me.seeks.begin(); itr != me.seeks.end();)
    {
        Objective o;
        if (!ReadObjective(bot, itr->obj.questId, itr->obj.index, o) || !o.Left())
            itr = me.seeks.erase(itr);
        else
            ++itr;
    }
    // the open objectives of its log: a seeker of one of them finds it on that board
    ReadHolds(bot, me);

    Objective obj;
    bool const has = !bot->GetMap()->Instanceable() && CurrentObjective(botAI, bot, obj) && obj.Left();
    if (!has)
    {
        if (me.bucket)
            Bump(me.bucket);  // it no longer works it: the board's forecasts change
        me.bucket = 0;
        me.cur = Objective();
        SyncBuckets(me, low);
        return;
    }
    uint64 const key = BucketKey(me.map, me.zone, obj);
    auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    WorldPosition const at = quest ? quest->pos : WorldPosition(bot);
    if (key != me.bucket || !obj.Same(me.cur))
    {
        if (me.bucket)
            Bump(me.bucket);
        me.bucket = key;
        me.cur = obj;
        me.pace = ComputePace(bot, obj, at);
        SyncBuckets(me, low);
        Bump(key);  // it works it now (it may have stood on it as a holder before)
        return;
    }
    SyncBuckets(me, low);
    // the formula's pace follows its counter, its spot and its level (its damage); nothing measured
    bool const counter = obj.have != me.cur.have || obj.need != me.cur.need;
    if (counter || me.pace.at != at || me.pace.left != obj.Left() || me.paceLevel != bot->GetLevel())
    {
        me.cur = obj;
        me.pace = ComputePace(bot, obj, at);
        me.paceLevel = bot->GetLevel();
        if (counter)
            Bump(key);
    }
}

// a party member's tick (lock held): its exits, the leader's word
NewRpgCoop::Order PartyTickLocked(PlayerbotAI* botAI, Player* bot, Posting& me, uint32 now, Out& out)
{
    NewRpgCoop::Order order;
    auto const itr = parties.find(me.party);
    if (itr == parties.end())
    {
        me.party = 0;
        return order;
    }
    Party& party = itr->second;
    if (!party.formed)
        return order;  // the invite is on its way: its own plan goes on
    bool const leader = party.leader == bot->GetGUID();

    std::string exit;
    Objective now_;
    auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    bool const open = ReadObjective(bot, me.partyObj.questId, me.partyObj.index, now_) && now_.Left();
    me.partyObj.have = open ? now_.have : me.partyObj.need;
    if (!open)
        exit = "counter closed";
    else if (!bot->GetGroup() || bot->GetGroup()->GetGUID() != party.group)
        exit = "its group is gone";
    else if (bot->GetMap()->Instanceable())
        exit = "in an instance";
    else if (!quest || quest->questId != me.partyObj.questId)
        exit = std::string(leader ? "its own plan moved on: "
                                  : "its state left the quest (the global leader, a refused walk): ") +
               StatusName(botAI->rpgInfo.GetStatus()) + (quest ? " q" + std::to_string(quest->questId) : "");
    Player* lead = leader ? bot : SameMapPlayer(bot, party.leader);
    if (exit.empty() && !lead)
        exit = "its leader is gone";
    if (!exit.empty())
    {
        LeaveLocked(me, exit, now, out);
        return order;
    }
    if (leader)
        return order;

    // arrived: within the core's group reward distance of the leader (its kill credit and XP share reach), the
    // criterion the leader's GROUP_WAIT holds for; the line carries the offer's eta of that walk
    float const distance = bot->GetExactDist(lead);
    if (!me.arrived && bot->IsAtGroupRewardDistance(lead))
    {
        me.arrived = true;
        out.logs.push_back(Acore::StringFormat(
            "[coop] wait {} group={} s={:.0f} eta={:.0f} walked={:.0f}{} reason=walk to the leader {}", me.name,
            party.id, float(getMSTimeDiff(me.joinedAt, now)) / IN_MILLISECONDS, me.walkSec, me.walkYd, Stamp(),
            lead->GetName()));
    }
    // the member keeps within its loot reach of the leader: the corpses it loots and the leader's pulls are in reach
    if (distance <= sPlayerbotAIConfig.LootDistance)
    {
        me.walkTo = WorldPosition();
        order.kind = NewRpgCoop::Order::Kind::Stay;
        return order;
    }
    order.kind = NewRpgCoop::Order::Kind::Follow;
    order.leader = lead;
    order.farWalk = distance > sPlayerbotAIConfig.SightDistance;
    // the far walk heads for one spot of the leader and keeps it while the leader stays within its group reward
    // distance of it (a spot that still gives the credit): a fresh spot every tick would start a new route search
    // and a new walk every tick
    if (me.walkTo == WorldPosition() || me.walkTo.GetMapId() != lead->GetMapId() ||
        lead->GetExactDist(me.walkTo) > sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE))
        me.walkTo = WorldPosition(lead);
    order.pos = order.farWalk ? me.walkTo : WorldPosition(lead);
    return order;
}

bool CoopFormOperation::Execute()
{
    std::string refuse;
    Player* leader = ObjectAccessor::FindConnectedPlayer(_leader);
    Player* member = ObjectAccessor::FindConnectedPlayer(_member);
    Objective leaderObj, memberObj;
    Camp camp;
    ObjectGuid campFor;
    WorldPosition spot;
    bool spotSet = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto const party = parties.find(_party);
        auto const lp = postings.find(_leader.GetCounter());
        auto const mp = postings.find(_member.GetCounter());
        if (party == parties.end() || lp == postings.end() || mp == postings.end() || mp->second.party != _party)
            return false;
        leaderObj = lp->second.partyObj;
        memberObj = mp->second.partyObj;
        camp = party->second.camp;
        campFor = party->second.campFor;
        spot = party->second.spot;
        spotSet = party->second.spotSet;
    }
    if (!leader || !member || !leader->IsInWorld() || !member->IsInWorld())
        refuse = "offline";
    else if (leader->GetMap() != member->GetMap())
        refuse = "another map";
    else if (!member->IsAlive() || !leader->IsAlive())
        refuse = "dead";
    else if (member->GetGroup())
        refuse = "in another group";
    else if (member->GetGroupInvite())
        refuse = "holds another invite";
    Group* group = leader ? leader->GetGroup() : nullptr;
    if (refuse.empty() && group && !NewRpgCoop::InParty(_leader.GetCounter()))
        refuse = "its leader is in another group";

    bool fresh = false;
    if (refuse.empty())
    {
        // the invite (HandleGroupInviteOpcode)
        if (!group)
        {
            group = new Group();
            fresh = true;
            if (!group->AddLeaderInvite(leader))
            {
                delete group;
                group = nullptr;
                refuse = "the leader cannot invite";
            }
        }
        if (group && !group->AddInvite(member))
        {
            if (fresh)
            {
                group->RemoveAllInvites();
                delete group;
            }
            group = nullptr;
            refuse = "the invite failed";
        }
    }
    if (refuse.empty())
    {
        // the accept (HandleGroupAcceptOpcode)
        group->RemoveInvite(member);
        if (!sScriptMgr->OnPlayerCanGroupAccept(member, group))
            refuse = "the accept was blocked";
        else if (group->IsFull())
            refuse = "the party is full";
        else
        {
            if (!group->IsCreated())
            {
                group->RemoveInvite(leader);
                group->Create(leader);
                sGroupMgr->AddGroup(group);
            }
            if (!group->AddMember(member))
                refuse = "the core did not add it";
            else
                group->BroadcastGroupUpdate();
        }
        if (!refuse.empty() && fresh && !group->IsCreated())
        {
            group->RemoveAllInvites();
            delete group;
            group = nullptr;
        }
    }

    Out out;
    uint32 const now = getMSTime();
    {
        std::lock_guard<std::mutex> guard(lock);
        auto const party = parties.find(_party);
        auto const mpi = postings.find(_member.GetCounter());
        auto const lpi = postings.find(_leader.GetCounter());
        if (party == parties.end() || mpi == postings.end() || lpi == postings.end())
            return false;
        Posting& mp = mpi->second;
        Posting& lp = lpi->second;
        if (!refuse.empty())
        {
            out.logs.push_back(
                Acore::StringFormat("[coop] refuse {} from={}{} reason={}", mp.name, lp.name, Stamp(), refuse));
            mp.party = 0;
            party->second.members.erase(
                std::remove(party->second.members.begin(), party->second.members.end(), _member),
                party->second.members.end());
            if (!party->second.formed)
            {
                lp.party = 0;
                parties.erase(party);
            }
        }
        else
        {
            bool const first = !party->second.formed;
            party->second.formed = true;
            party->second.group = group->GetGUID();
            std::string with;
            for (ObjectGuid const& guid : party->second.members)
                if (auto const m = postings.find(guid.GetCounter()); guid != _member && m != postings.end())
                    with += (with.empty() ? "" : ",") + m->second.name;
            out.logs.push_back(
                Acore::StringFormat("[coop] accept {} from={}{} reason={}", mp.name, leader->GetName(), Stamp(),
                                    camp.known ? "a party for a camp one cannot solo" : "together is faster"));
            out.logs.push_back(
                Acore::StringFormat("[coop] join {} group={} with={}{}", mp.name, _party, with, Stamp()));
            mp.joinedAt = now;
            MarkParty(_member.GetCounter(), true);
            mp.haveAtJoin = memberObj.have;
            mp.arrived = false;
            mp.xp = 0;
            mp.sharedKills = 0;
            // the party is a task in each one's list, above its convenient tasks and below the urgent ones
            std::string const why = camp.known ? "a camp one cannot solo" : "together is faster";
            out.after.push_back([name = mp.name, low = _member.GetCounter(), partner = lp.name,
                                 partnerLow = _leader.GetCounter(), q = memberObj.questId, why]()
                                { AddTask(name, low, partner, partnerLow, q, false, why); });
            if (first)
            {
                out.logs.push_back(
                    Acore::StringFormat("[coop] join {} group={} with={}{}", lp.name, _party, mp.name, Stamp()));
                lp.joinedAt = now;
                MarkParty(_leader.GetCounter(), true);
                lp.haveAtJoin = leaderObj.have;
                lp.arrived = true;
                lp.xp = 0;
                lp.sharedKills = 0;
                out.after.push_back([name = lp.name, low = _leader.GetCounter(), partner = mp.name,
                                     partnerLow = _member.GetCounter(), q = leaderObj.questId, why]()
                                    { AddTask(name, low, partner, partnerLow, q, true, why); });
            }
        }
    }
    out.Flush();
    if (!refuse.empty())
        return false;

    // both work the party's objective: a quest postponed for its camp comes back, the seeker's leader walks to
    // that camp (its danger mark there stays for its solo walks)
    for (auto const& [player, obj] : {std::make_pair(leader, leaderObj), std::make_pair(member, memberObj)})
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        Quest const* quest = sObjectMgr->GetQuestTemplate(obj.questId);
        if (!ai || !quest)
            continue;
        ai->lowPriorityQuest.erase(obj.questId);
        auto const* doing = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data);
        if (!doing || doing->questId != obj.questId)
            ai->rpgInfo.ChangeToDoQuest(obj.questId, quest);
        // the leader works the party's objective at the party's spot: the seeker's camp, or (a holder that worked
        // another objective) the nearest spawn of the one it held
        bool const atCamp = camp.known && campFor == leader->GetGUID();
        if (player == leader && (atCamp || spotSet))
            if (auto* data = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data))
            {
                data->pos = atCamp ? camp.at : spot;
                data->objectiveIdx = obj.index;
                data->lastReachPOI = 0;
            }
    }
    return true;
}

class NewRpgCoopPlayerScript : public PlayerScript
{
public:
    NewRpgCoopPlayerScript()
        : PlayerScript("NewRpgCoopPlayerScript", {PLAYERHOOK_ON_CREATURE_KILL_CREDIT, PLAYERHOOK_ON_GIVE_EXP,
                                                  PLAYERHOOK_ON_PLAYER_JUST_DIED, PLAYERHOOK_ON_LOGOUT})
    {
    }

    // the kill credit the core shares with the party at its reward distance: one line per kill that served
    // another member's objective (a kill credit or a MULTI_DROP drop; one quest drop per corpse serves one looter)
    void OnPlayerCreatureKillCredit(Player* player, Creature* killed) override
    {
        if (!player || !killed || !sGuildCoordinator.IsPlanned(player->GetGUID().GetCounter()))
            return;
        std::string line;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const me = postings.find(player->GetGUID().GetCounter());
            if (me == postings.end())
                return;
            auto const party = parties.find(me->second.party);
            if (!me->second.party || party == parties.end() || !party->second.formed)
                return;
            std::string quests;
            uint32 served = 0;
            for (ObjectGuid const& guid : party->second.members)
            {
                Player* m = guid == player->GetGUID() ? player : SameMapPlayer(player, guid);
                auto const post = postings.find(guid.GetCounter());
                if (!m || post == postings.end() || !m->IsAtGroupRewardDistance(killed) ||
                    !EachGetsIt(post->second.partyObj) || !Serves(post->second.partyObj, killed))
                    continue;
                quests += (quests.empty() ? "q" : ",q") + std::to_string(post->second.partyObj.questId);
                ++post->second.sharedKills;
                ++served;
            }
            if (served < 2)
                return;
            line = Acore::StringFormat("[coop] shared-kill {} group={} entry={} quests={}{}", player->GetName(),
                                       party->second.id, killed->GetEntry(), quests, Stamp());
        }
        LOG_INFO("playerbots", "{}", line);
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 xpSource) override
    {
        if (!player || xpSource != XPSOURCE_KILL)
            return;
        std::lock_guard<std::mutex> guard(lock);
        auto const me = postings.find(player->GetGUID().GetCounter());
        if (me != postings.end() && me->second.party)
            me->second.xp += amount;
    }

    void OnPlayerJustDied(Player* player) override { Leave(player, "death", false); }

    void OnPlayerLogout(Player* player) override { Leave(player, "logout", true); }

private:
    static void Leave(Player* player, std::string const& reason, bool forget)
    {
        if (!player)
            return;
        Out out;
        {
            std::lock_guard<std::mutex> guard(lock);
            uint32 const low = player->GetGUID().GetCounter();
            auto const me = postings.find(low);
            if (me == postings.end())
                return;
            if (me->second.party)
                LeaveLocked(me->second, reason, getMSTime(), out);
            if (forget)
            {
                me->second.bucket = 0;
                me->second.seeks.clear();
                me->second.holds.clear();
                SyncBuckets(me->second, low);
                postings.erase(me);
            }
        }
        out.Flush();
    }
};
}  // namespace

namespace NewRpgCoop
{
Order Tick(PlayerbotAI* botAI, Player* bot)
{
    Order order;
    if (!bot || !botAI || !bot->IsInWorld() || !sGuildCoordinator.IsPlanned(bot->GetGUID().GetCounter()))
        return order;
    EnsureIndex();  // built at startup; never under the board's lock
    uint32 const now = getMSTime();
    Out out;
    {
        std::lock_guard<std::mutex> guard(lock);
        Posting& me = postings[bot->GetGUID().GetCounter()];
        UpdatePostingLocked(botAI, bot, me);
        if (me.party)
            order = PartyTickLocked(botAI, bot, me, now, out);
        // the board of an objective it has changed since its last look: a counter, a bot that came or went
        bool const free = !me.party ? !bot->GetGroup() && bot->IsAlive() && !bot->IsInCombat()
                                    : parties.count(me.party) && parties[me.party].formed &&
                                          parties[me.party].leader == bot->GetGUID();
        if (free && (order.kind == Order::Kind::None))
        {
            std::vector<uint64> keys;
            if (me.bucket)
                keys.push_back(me.bucket);
            for (Seek const& seek : me.seeks)
                keys.push_back(seek.bucket);
            // the objectives it holds: a seeker there may want it (it leads when it outranks the seeker)
            for (Hold const& hold : me.holds)
                if (hold.bucket != me.bucket)
                    keys.push_back(hold.bucket);
            for (uint64 key : keys)
            {
                auto const bucket = buckets.find(key);
                if (bucket == buckets.end())
                    continue;
                auto const seen = me.seen.find(key);
                if (seen != me.seen.end() && seen->second == bucket->second.version)
                    continue;
                uint32 const before = me.party;
                EvaluateLocked(botAI, bot, me, key, out);
                if (!before && me.party)
                    break;  // one offer at a time
            }
        }
    }
    out.Flush();
    return order;
}

void NoWalk(Player* bot, std::string const& why)
{
    if (!bot)
        return;
    Out out;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto const me = postings.find(bot->GetGUID().GetCounter());
        if (me != postings.end() && me->second.party)
            LeaveLocked(me->second, "no walk to its leader: " + why, getMSTime(), out);
    }
    out.Flush();
}

void OnSpotRefused(PlayerbotAI* botAI, Player* bot, uint32 questId, int32 objectiveIdx, WorldPosition const& dest)
{
    if (!bot || !botAI || !sGuildCoordinator.IsPlanned(bot->GetGUID().GetCounter()))
        return;
    // every postponed spot ends in a seek or in one line that says why not (quest_metrics "groups")
    auto const noSeek = [&](std::string const& why)
    {
        LOG_INFO("playerbots", "[coop] no-seek {} quests=q{}{} reason={}", bot->GetName(), questId, Stamp(), why);
    };
    if (bot->GetGroup())
        return noSeek("in a group");
    if (dest.GetMapId() != bot->GetMapId())
        return noSeek("its spot is on another map");
    Objective obj;
    if (!ReadObjective(bot, questId, objectiveIdx, obj))
        return noSeek("no kill / loot objective open (a hand-in spot, a game object, the quest done)");
    if (!obj.Left())
        return noSeek("its counter is closed");
    // the camp: the idle mob whose reach covers the spot nearest to it (as DestinationRefused looks); when none does
    // (the refusal was a danger zone it remembers or a pack on the way there), the idle mob nearest to the spot that
    // its solo judge refuses - the pack that stands between the bot and its objective
    GuidVector const targets = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();
    Creature* nearest = nullptr;
    float nearestGap = std::numeric_limits<float>::max();
    std::vector<std::pair<float, Creature*>> idle;
    for (ObjectGuid const& guid : targets)
    {
        Creature* mob = botAI->GetCreature(guid);
        if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot) ||
            !mob->HasReactState(REACT_AGGRESSIVE))
            continue;
        float reach = mob->GetAggroRange(bot);
        Position centre = mob->GetPosition();
        if (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
        {
            centre = mob->GetHomePosition();
            reach += mob->GetWanderDistance();
        }
        float const gap = centre.GetExactDist2d(dest.GetPositionX(), dest.GetPositionY());
        idle.push_back({gap, mob});
        if (reach > 0.0f && gap <= reach && gap < nearestGap)
        {
            nearestGap = gap;
            nearest = mob;
        }
    }
    SoloSafety::Verdict v;
    if (nearest)
        v = SoloSafety::Judge(bot, nearest);
    else
    {
        std::sort(idle.begin(), idle.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        for (auto const& [gap, mob] : idle)
        {
            SoloSafety::Verdict const judged = SoloSafety::Judge(bot, mob);
            if (judged.kind == SoloSafety::Verdict::Kind::Refuse)
            {
                nearest = mob;
                v = judged;
                break;
            }
        }
    }
    if (!nearest)
        return noSeek(Acore::StringFormat("no idle mob in sight that its solo judge refuses ({} idle mobs looked at)",
                                          idle.size()));
    if (v.fightSec <= 0.0f)
        return noSeek("its judge gives no fight time for " + nearest->GetName());
    Seek seek;
    seek.obj = obj;
    seek.camp.known = true;
    seek.camp.at = dest;
    seek.camp.mob = nearest->GetGUID();
    seek.camp.entry = nearest->GetEntry();
    seek.camp.topLevel = std::max<uint8>(v.topLevel, nearest->GetLevel());
    seek.camp.mobs = v.mobs;
    seek.camp.damage = v.damage;
    seek.camp.fightSec = v.fightSec;
    seek.camp.dps = DpsEstimate::EstimateDps(bot, nullptr, DpsEstimate::BuffContext::Group);
    // the objective units one clear of the pack gives: its mobs that serve the objective
    uint32 units = 0;
    float const radius = std::max(v.radius, nearest->GetAggroRange(bot));
    for (ObjectGuid const& guid : targets)
        if (Creature* mob = botAI->GetCreature(guid))
            if (mob->IsAlive() && mob->GetExactDist2d(&v.at) <= radius && Serves(obj, mob))
                ++units;
    seek.camp.units = std::max<uint32>(units, 1);
    // a pack that only stands on the way (none of its mobs serves the objective): the party works the objective past
    // it at the pace of its sources nearest the spot
    seek.camp.serves = units > 0;
    if (!seek.camp.serves)
        seek.camp.pace = ComputePace(bot, obj, dest);
    std::string line;
    {
        std::lock_guard<std::mutex> guard(lock);
        Posting& me = postings[bot->GetGUID().GetCounter()];
        if (me.guid.IsEmpty())
        {
            me.guid = bot->GetGUID();
            me.name = bot->GetName();
            me.level = bot->GetLevel();
            me.questRate = bot->GetQuestRate(false);
            me.questRateDf = bot->GetQuestRate(true);
        }
        seek.bucket = BucketKey(bot->GetMapId(), bot->GetZoneId(), obj);
        me.seeks.erase(std::remove_if(me.seeks.begin(), me.seeks.end(),
                                      [&obj](Seek const& s) { return s.obj.Same(obj); }),
                       me.seeks.end());
        me.seeks.push_back(seek);
        SyncBuckets(me, bot->GetGUID().GetCounter());
        Bump(seek.bucket);  // a seeker came: the holders on this board look again
        line = Acore::StringFormat(
            "[coop] seek {} quests=q{} camp={} mobs={} units={} serves={}{} reason=looks for a party: "
            "{}{}",
            bot->GetName(), questId, nearest->GetName(), v.mobs, units, seek.camp.serves ? 1 : 0, Stamp(), v.why,
            seek.camp.serves ? std::string()
            : seek.camp.pace.known
                ? Acore::StringFormat("; past the camp: {} {} ({} spawns)", seek.camp.pace.chest ? "chest" : "creature",
                                      seek.camp.pace.entry, uint32(seek.camp.pace.spawns))
                : "; past the camp: no pace (" + seek.camp.pace.why + ")");
    }
    LOG_INFO("playerbots", "{}", line);
}

bool IsMember(Player* bot)
{
    if (!bot)
        return false;
    std::lock_guard<std::mutex> guard(lock);
    auto const me = postings.find(bot->GetGUID().GetCounter());
    if (me == postings.end() || !me->second.party)
        return false;
    auto const party = parties.find(me->second.party);
    return party != parties.end() && party->second.formed && party->second.leader != bot->GetGUID();
}

bool JudgesPulls(Player* bot)
{
    if (!bot)
        return false;
    std::lock_guard<std::mutex> guard(lock);
    auto const me = postings.find(bot->GetGUID().GetCounter());
    if (me == postings.end() || !me->second.party)
        return false;
    auto const party = parties.find(me->second.party);
    return party != parties.end() && party->second.formed && party->second.leader == bot->GetGUID();
}

void SetCoordinatorHooks(CoordinatorHooks const& coordinatorHooks) { hooks = coordinatorHooks; }

bool InParty(uint32 low)
{
    std::lock_guard<std::mutex> guard(partyLowsLock);
    return partyLows.count(low) != 0;
}

SoloSafety::Verdict JudgePull(Player* leader, Unit* target)
{
    SoloSafety::Verdict verdict;
    verdict.kind = SoloSafety::Verdict::Kind::Refuse;
    PlayerbotAI* botAI = leader ? GET_PLAYERBOT_AI(leader) : nullptr;
    if (!botAI || !target)
        return verdict;
    std::vector<ObjectGuid> members;
    std::unordered_map<uint32, float> healRate;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto const me = postings.find(leader->GetGUID().GetCounter());
        auto const party = me != postings.end() ? parties.find(me->second.party) : parties.end();
        if (party == parties.end())
            return verdict;
        members = party->second.members;
        healRate = party->second.healRate;
    }

    // the party's damage: its members in sight of the leader (each the formula's, [dps1]); its heals: the
    // leader's own and the healers', per fight second, as far as their mana lasts
    float sumDps = 0.0f;
    float healNow = 0.0f;
    float healFull = 0.0f;
    for (ObjectGuid const& guid : members)
    {
        Player* m = SameMapPlayer(leader, guid);
        if (!m || !m->IsAlive() || leader->GetExactDist(m) > sPlayerbotAIConfig.SightDistance)
            continue;
        sumDps += DpsEstimate::EstimateDps(m, target, DpsEstimate::BuffContext::Group);  // [dps1] the formula
        if (m != leader && !PlayerbotAI::IsHeal(m))
            continue;
        auto const rate = healRate.find(guid.GetCounter());
        if (rate == healRate.end())
            continue;
        float const share =
            m->GetMaxPower(POWER_MANA) ? float(m->GetPower(POWER_MANA)) / float(m->GetMaxPower(POWER_MANA)) : 1.0f;
        healNow += rate->second * share;
        healFull += rate->second;
    }
    sumDps = std::max(sumDps, 1.0f);

    // the pack the pull wakes: the target, the idle mobs whose aggro reach covers the fight at the target, its
    // friends within the core's assistance radius
    struct Mob
    {
        Unit* unit;
        float hp;
        float dps;
    };
    std::vector<Mob> pack;
    float spell = 0.0f;
    pack.push_back({target, float(target->GetHealth()), Leader::Fight::AttackerDps(target, leader, spell)});
    float const assist = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
    GuidVector const targets = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();
    verdict.radius = 0.0f;
    for (ObjectGuid const& guid : targets)
    {
        Creature* mob = botAI->GetCreature(guid);
        if (!mob || mob == target || !mob->IsAlive() || !mob->IsHostileTo(leader))
            continue;
        float const gap = mob->GetExactDist(target);
        bool const joins = mob->IsInCombat()
                               ? mob->GetVictim() && mob->GetVictim()->IsInRaidWith(leader)
                               : (mob->HasReactState(REACT_AGGRESSIVE) && gap <= mob->GetAggroRange(leader)) ||
                                     (gap <= assist && mob->IsFriendlyTo(target));
        if (!joins)
            continue;
        pack.push_back({mob, float(mob->GetHealth()), Leader::Fight::AttackerDps(mob, leader, spell)});
        verdict.radius = std::max(verdict.radius, gap);
    }
    std::sort(pack.begin() + 1, pack.end(),
              [](Mob const& a, Mob const& b) { return a.dps / std::max(a.hp, 1.0f) > b.dps / std::max(b.hp, 1.0f); });
    float seconds = 0.0f;
    float damage = 0.0f;
    for (Mob const& mob : pack)
    {
        seconds += mob.hp / sumDps;
        damage += mob.dps * seconds;
        if (mob.unit->GetLevel() > leader->GetLevel())
            ++verdict.above;
        verdict.topLevel = std::max<uint8>(verdict.topLevel, mob.unit->GetLevel());
    }
    verdict.mobs = pack.size();
    verdict.damage = damage;
    verdict.fightSec = seconds;
    verdict.at = target->GetPosition();
    verdict.ehpNow = float(leader->GetHealth()) + PetHealth(leader, false) + healNow * seconds;
    verdict.ehpFull = float(leader->GetMaxHealth()) + PetHealth(leader, true) + healFull * seconds;
    if (damage < verdict.ehpNow)
        verdict.kind = SoloSafety::Verdict::Kind::Win;
    else if (damage < verdict.ehpFull)
    {
        verdict.kind = SoloSafety::Verdict::Kind::RestFirst;
        verdict.why = "the party wins only at full: ";
    }
    else
        verdict.why = "the party loses even at full: ";
    verdict.why += Acore::StringFormat(
        "{} mobs, {:.0f} dmg on {} over {:.0f} s at {:.1f} party dps, ehp {:.0f} now / "
        "{:.0f} full",
        verdict.mobs, damage, leader->GetName(), seconds, sumDps, verdict.ehpNow, verdict.ehpFull);
    return verdict;
}

void RestParty(Player* leader, std::string const& why)
{
    if (!leader)
        return;
    std::vector<ObjectGuid> members;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto const me = postings.find(leader->GetGUID().GetCounter());
        auto const party = me != postings.end() ? parties.find(me->second.party) : parties.end();
        if (party == parties.end())
            return;
        members = party->second.members;
    }
    for (ObjectGuid const& guid : members)
        if (Player* m = SameMapPlayer(leader, guid))
            if (m->GetHealth() < m->GetMaxHealth() ||
                (m->GetMaxPower(POWER_MANA) && m->GetPower(POWER_MANA) < m->GetMaxPower(POWER_MANA)))
                SoloSafety::RequestRest(m, "the party's pull: " + why);
}
}  // namespace NewRpgCoop

float NewRpgCoopMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;
    uint32 const now = getMSTime();
    if (!_checked || now != _checkedAt)  // one look per tick
    {
        _member = NewRpgCoop::IsMember(bot);
        _checkedAt = now;
        _checked = true;
    }
    if (!_member)
        return 1.0f;
    // the member's own plan and its own pulls wait while it goes with its leader
    static std::unordered_set<std::string> const ownPlan = {"new rpg go grind",
                                                            "new rpg go camp",
                                                            "new rpg wander random",
                                                            "new rpg move npcs",
                                                            "new rpg do quest",
                                                            "new rpg travel flight",
                                                            "new rpg outdoor pvp",
                                                            "new rpg do gather",
                                                            "new rpg do profession",
                                                            "new rpg rest at inn",
                                                            "new rpg discover flight master",
                                                            "new rpg wait for group",
                                                            "attack anything"};
    return ownPlan.count(action->getName()) ? 0.0f : 1.0f;
}

namespace
{
// the pace formula's world data index is built once at startup, after the loot and spawn tables are loaded
class NewRpgCoopWorldScript : public WorldScript
{
public:
    NewRpgCoopWorldScript() : WorldScript("NewRpgCoopWorldScript", {WORLDHOOK_ON_STARTUP}) {}

    void OnStartup() override { EnsureIndex(); }
};
}  // namespace

void AddNewRpgCoopScripts()
{
    new NewRpgCoopPlayerScript();
    new NewRpgCoopWorldScript();
}
