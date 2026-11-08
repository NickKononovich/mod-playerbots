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
};

struct Seek
{
    Objective obj;
    uint64 bucket = 0;
    Camp camp;
};

// the formula's pace of one objective for one bot (no measured history): the source creature it would farm (the
// objective's creature or kill credit, for an item the creature that drops it with its loot chance), its health at
// the core's base stats, the walk between the spawns it works and from the bot to the first of them
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

// a kill of `killed` counts for the objective (its entry or its kill credit; a quest drop it may loot)
bool Serves(Player* member, Objective const& o, Creature* killed)
{
    CreatureTemplate const* tmpl = killed->GetCreatureTemplate();
    if (!tmpl)
        return false;
    if (!o.item)
        return o.key == tmpl->Entry || o.key == tmpl->KillCredit[0] || o.key == tmpl->KillCredit[1];
    return tmpl->lootid && LootTemplates_Creature.HaveQuestLootForPlayer(tmpl->lootid, member);
}

// the static world data the pace formula reads, indexed once (the world thread's startup, else the first look):
// the quest items -> the creatures that drop them (creature_loot_template, the item's chance), the kill objectives
// -> the creatures whose kill credits them, those creatures -> their spawns (`creature`, multispawn alternates)
struct DropSource
{
    uint32 entry;
    float chance;  // 0..1
};
struct SpawnPoint
{
    uint16 map;
    float x, y;
    uint32 respawnSec;
};
std::once_flag indexOnce;
std::unordered_map<uint32, std::vector<DropSource>> itemSources;
std::unordered_map<uint32, std::vector<uint32>> creditSources;
std::unordered_map<uint32, std::vector<SpawnPoint>> spawnsOf;

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
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        uint32 const ids[] = {data.id, data.id2, data.id3};
        for (size_t i = 0; i < 3; ++i)
        {
            uint32 const entry = ids[i];
            if (!entry || !sources.count(entry) || (i > 0 && entry == ids[0]) || (i == 2 && entry == ids[1]))
                continue;
            spawnsOf[entry].push_back({data.mapid, data.posX, data.posY, data.spawntimesecs});
        }
    }
    LOG_INFO("playerbots", "[coop] index: {} quest items from {} creatures, {} kill objectives, {} sources spawned",
             itemSources.size(), sources.size(), creditSources.size(), spawnsOf.size());
}

void EnsureIndex() { std::call_once(indexOnce, BuildIndex); }

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

// the forecast of one pairing
struct Plan
{
    bool ok = false;
    std::string why;
    float tSoloL = 0.0f, tGroupL = 0.0f, tSoloP = 0.0f, tGroupP = 0.0f;
    float walkYd = 0.0f;
    float walkSec = 0.0f;  // P's walk to L at its run speed
    float xphSoloL = 0.0f, xphGroupL = 0.0f, xphSoloP = 0.0f, xphGroupP = 0.0f;
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
        best.why = o.item ? "no creature drops item " + std::to_string(o.key)
                          : "no creature gives the kill credit " + std::to_string(o.key);
        return best;
    }
    float const dps = DpsEstimate::EstimateDps(bot, nullptr, DpsEstimate::BuffContext::Solo);
    float const speed = std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
    float bestSec = NO_TIME;
    best.why = "no spawn of its sources on this map";
    for (DropSource const& source : candidates)
    {
        auto const spawns = spawnsOf.find(source.entry);
        if (spawns == spawnsOf.end())
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
        pace.hp = TemplateHealth(source.entry, pace.level);
        if (pace.hp <= 0.0f)
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
            reach = std::min(reach, bot->GetExactDist2d(s.x, s.y));
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

float KillXp(Player* p, uint8 mobLevel)
{
    if (!mobLevel)
        return 0.0f;
    return float(
        Acore::XP::BaseGain(p->GetLevel(), mobLevel, GetContentLevelsForMapAndZone(p->GetMapId(), p->GetZoneId())));
}

// L leads (it stays on its work), P walks to it. `partyDps`/`partyLevels`/`partySize`: the party L leads already;
// `workers`: the bots on this board (they share its spawns); `walkYd` >= 0: the length of P's route to L (else the
// straight line)
Plan Forecast(Side const& l, Side const& pr, float partyDps, uint32 partyLevels, uint32 partySize, float workers,
              float walkYd = -1.0f)
{
    Plan plan;
    Player* const lp = l.p;
    Player* const pp = pr.p;
    float const dpsP = DpsEstimate::EstimateDps(pp, nullptr, DpsEstimate::BuffContext::Group);
    float const soloDpsL = DpsEstimate::EstimateDps(lp, nullptr, DpsEstimate::BuffContext::Solo);
    float const soloDpsP = DpsEstimate::EstimateDps(pp, nullptr, DpsEstimate::BuffContext::Solo);
    float const sumDps = partyDps + dpsP;
    float const speedL = std::max(lp->GetSpeed(MOVE_RUN), 1.0f);
    float const speedP = std::max(pp->GetSpeed(MOVE_RUN), 1.0f);
    plan.walkYd = walkYd >= 0.0f ? walkYd : lp->GetExactDist2d(pp);
    plan.walkSec = plan.walkYd / speedP;
    float const walkSec = plan.walkSec;
    uint32 const rL = l.obj.Left();
    uint32 const rP = pr.obj.Left();
    uint32 const shared = std::min(rL, rP);
    if (!shared)
    {
        plan.why = "calc: a counter is closed";
        return plan;
    }

    // each side's pace by the formula (its objective's source creature at the core's base health over its damage,
    // the walk between the spawns it works, the drop chance, the respawn of spawns shared with the board): no
    // measured history
    Pace const* paceL = l.camp ? nullptr : &l.post->pace;
    Pace const* paceP = pr.camp ? nullptr : &pr.post->pace;
    for (auto const& [who, pace] : {std::make_pair(lp, paceL), std::make_pair(pp, paceP)})
        if (pace && !pace->known)
        {
            plan.why = "calc: " + who->GetName() + " has no pace on it: " +
                       (pace->why.empty() ? std::string("its objective is not on the board yet") : pace->why);
            return plan;
        }

    // the shared pace: a camp the leader could not solo: the camp's fight at the party's damage per objective unit;
    // else the leader's spawns at the party's damage (one kill credits both, each rolls its own quest drop)
    Camp const* camp = l.camp ? l.camp : pr.camp;
    float groupShared = 0.0f;
    if (l.camp)
        groupShared = float(shared) * l.camp->fightSec * l.camp->dps / std::max(sumDps, 1.0f) /
                      float(std::max<uint32>(l.camp->units, 1));
    else
        groupShared = PaceSeconds(*paceL, shared, sumDps, speedL, std::max(workers - 1.0f, 1.0f));
    uint8 const mobLevel =
        camp ? camp->topLevel : std::max<uint8>(paceL ? paceL->level : 0, paceP ? paceP->level : 0);

    // a camp the seeker could not solo: the party must win it (the pack's damage on the leader over the time the
    // party's damage needs, against the leader's health and the party's heals), and the core's con rule holds for
    // the leader (no orange / red mob is pulled)
    if (camp)
    {
        if (Acore::XP::GetColorCode(lp->GetLevel(), camp->topLevel) <= XP_ORANGE)
        {
            plan.why = Acore::StringFormat("calc: the camp's L{} is orange / red for {} L{}", camp->topLevel,
                                           lp->GetName(), lp->GetLevel());
            return plan;
        }
        Unit* mob = ObjectAccessor::GetCreature(*lp, camp->mob);
        float incoming = camp->fightSec > 0.0f ? camp->damage / camp->fightSec : 0.0f;
        if (pr.camp && mob && mob->IsAlive())
        {
            SoloSafety::Verdict const v = SoloSafety::Judge(lp, mob);  // the leader takes the hits
            if (v.fightSec > 0.0f)
                incoming = v.damage / v.fightSec;
        }
        float const seconds = camp->fightSec * camp->dps / std::max(sumDps, 1.0f);
        float heals = 0.0f;
        for (Player* m : {lp, pp})
        {
            float const rate = mob && mob->IsAlive() ? HealRate(m, mob) : 0.0f;
            plan.healRate[m->GetGUID().GetCounter()] = rate;
            if (m == lp || PlayerbotAI::IsHeal(m))
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
        plan.campFor = l.camp ? lp->GetGUID() : pp->GetGUID();
    }

    // the time to close each counter: alone (its walk to its first spawn, then its own pace), and together (P's
    // walk to L, the leader's GROUP_WAIT holds L for it; the shared units at the party's pace; the one that needs
    // more closes the rest alone, the one that needs fewer leaves when its counter closes)
    float const reachL = paceL ? paceL->reachYd / speedL : 0.0f;
    float const reachP = paceP ? paceP->reachYd / speedP : 0.0f;
    plan.tSoloL = l.camp ? NO_TIME : reachL + PaceSeconds(*paceL, rL, soloDpsL, speedL, workers);
    plan.tSoloP = pr.camp ? NO_TIME : reachP + PaceSeconds(*paceP, rP, soloDpsP, speedP, workers);
    plan.tGroupL = walkSec + reachL + groupShared +
                   (l.camp ? 0.0f : PaceSeconds(*paceL, rL - shared, soloDpsL, speedL, workers));
    plan.tGroupP = walkSec + reachL + groupShared +
                   (pr.camp ? 0.0f : PaceSeconds(*paceP, rP - shared, soloDpsP, speedP, workers));
    uint32 const doneL = l.camp ? shared : rL;
    uint32 const doneP = pr.camp ? shared : rP;

    // the XP of it per hour: the quest's XP share of the units it closes, the kill XP alone and split (the core's
    // group rate x level share)
    uint32 const count = partySize + 1;
    uint32 const levels = partyLevels + pp->GetLevel();
    float const rate = Acore::XP::xp_in_group_rate(count, false);
    auto const xph = [&](Player* m, Objective const& o, uint32 units, uint32 together, float seconds, bool alone)
    {
        if (seconds <= 0.0f || seconds >= NO_TIME)
            return 0.0f;
        Quest const* quest = sObjectMgr->GetQuestTemplate(o.questId);
        float const questXp = quest && o.need ? float(quest->XPValue(m->GetLevel())) * units / o.need : 0.0f;
        float const kill = KillXp(m, mobLevel);
        float const split = kill * rate * float(m->GetLevel()) / float(std::max<uint32>(levels, 1));
        float const xp =
            questXp + (alone ? float(units) * kill : float(together) * split + float(units - together) * kill);
        return xp / seconds * HOUR;
    };
    plan.xphSoloL = xph(lp, l.obj, rL, 0, plan.tSoloL, true);
    plan.xphSoloP = xph(pp, pr.obj, rP, 0, plan.tSoloP, true);
    plan.xphGroupL = xph(lp, l.obj, doneL, shared, plan.tGroupL, false);
    plan.xphGroupP = xph(pp, pr.obj, doneP, shared, plan.tGroupP, false);

    bool const gainL = l.camp || plan.tGroupL < plan.tSoloL;
    bool const gainP = pr.camp || plan.tGroupP < plan.tSoloP;
    auto const secs = [](float s) { return s >= NO_TIME ? std::string("never") : std::to_string(uint32(s)) + " s"; };
    plan.why = Acore::StringFormat(
        "{} {}/{} {} alone vs {} together; {} {}/{} {} alone vs {} together; walk {:.0f} "
        "yd",
        lp->GetName(), l.obj.have, l.obj.need, secs(plan.tSoloL), secs(plan.tGroupL), pp->GetName(), pr.obj.have,
        pr.obj.need, secs(plan.tSoloP), secs(plan.tGroupP), plan.walkYd);
    plan.ok = gainL && gainP;
    if (!plan.ok)
        plan.why = "calc: alone is faster for " + (gainL ? pp->GetName() : lp->GetName()) + ": " + plan.why;
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
        float const questXp =
            quest && p.partyObj.need ? float(quest->XPValue(p.level)) * units / p.partyObj.need : 0.0f;
        float const got = seconds > 0.0f ? (float(p.xp) + questXp) / seconds * HOUR : 0.0f;
        out.logs.push_back(Acore::StringFormat("[coop] exit {} group={} after={:.0f} units={} kills={}{} reason={}",
                                               p.name, party.id, seconds, units, p.sharedKills, Stamp(), why));
        // got: its XP per hour in the party (kill XP + the quest's share of the units closed); forecast: the offer's
        // XP per hour together, solo: alone (never: it cannot close it alone - no ratio)
        out.logs.push_back(Acore::StringFormat(
            "[coop] gain {} group={} got={:.0f} forecast={:.0f} solo={}{}", p.name, party.id, got, p.groupXph,
            p.soloNever ? std::string("never") : std::to_string(uint32(p.soloXph)), Stamp()));
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

// the leader's board look at one bucket (lock held): the best partner it leads; the offer
void EvaluateLocked(PlayerbotAI* /*botAI*/, Player* bot, Posting& me, uint64 key, Out& out)
{
    auto const bucket = buckets.find(key);
    if (bucket == buckets.end())
        return;
    me.seen[key] = bucket->second.version;

    // my side on this board: the objective it works, or one it looks for a party for
    Side mine;
    mine.p = bot;
    mine.post = &me;
    if (me.bucket == key)
        mine.obj = me.cur;
    else
    {
        auto const seek =
            std::find_if(me.seeks.begin(), me.seeks.end(), [key](Seek const& s) { return s.bucket == key; });
        if (seek == me.seeks.end())
            return;
        mine.obj = seek->obj;
        mine.camp = &seek->camp;
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
    float partyDps = DpsEstimate::EstimateDps(bot, nullptr, DpsEstimate::BuffContext::Group);
    uint32 partyLevels = bot->GetLevel();
    uint32 partySize = 1;
    if (led)
    {
        partyDps = 0.0f;
        partyLevels = 0;
        partySize = 0;
        for (ObjectGuid const& guid : led->members)
            if (Player* m = SameMapPlayer(bot, guid))
            {
                partyDps += DpsEstimate::EstimateDps(m, nullptr, DpsEstimate::BuffContext::Group);
                partyLevels += m->GetLevel();
                ++partySize;
            }
    }

    // the bots on this board share its spawns (their respawns bound the pace of each)
    float const workers = float(bucket->second.lows.size());
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
        if (po->second.bucket == key)
            other.obj = po->second.cur;
        else
        {
            auto const seek = std::find_if(po->second.seeks.begin(), po->second.seeks.end(),
                                           [key](Seek const& s) { return s.bucket == key; });
            if (seek == po->second.seeks.end())
                continue;
            other.obj = seek->obj;
            other.camp = &seek->camp;
        }
        if (led && mine.camp)
            continue;  // a party already formed works where it works
        std::string const busy = Busy(p);
        if (!busy.empty())
        {
            NoteRefusal(out, bot, p, "busy: " + busy);
            continue;
        }
        Plan plan = Forecast(mine, other, partyDps, partyLevels, partySize, workers);
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
            float const length =
                route.complete || route.points.empty()
                    ? route.length
                    : route.length + bot->GetExactDist(route.points.back().x, route.points.back().y,
                                                       route.points.back().z);
            best = Forecast(mine, bestSide, partyDps, partyLevels, partySize, workers, length);
            if (!best.ok)
                why = best.why + " (route " + std::to_string(uint32(length)) + " yd)";
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
    if (!led)
    {
        me.walkYd = 0.0f;
        me.walkSec = 0.0f;
        me.soloNever = best.tSoloL >= NO_TIME;
        me.soloXph = best.xphSoloL;
        me.groupXph = best.xphGroupL;
    }
    lastRefusal.erase(PairKey(bot->GetGUID().GetCounter(), bestPartner->GetGUID().GetCounter()));
    // gain / solo: the pair's XP per hour together / alone (solo=never: one of them cannot close it alone, a camp it
    // does not solo - no ratio); t_*: seconds to close each counter, walk: the route's yards, eta: its seconds
    bool const never = best.tSoloL >= NO_TIME || best.tSoloP >= NO_TIME;
    auto const secs = [](float t) { return t >= NO_TIME ? std::string("never") : std::to_string(uint32(t)); };
    out.logs.push_back(Acore::StringFormat(
        "[coop] offer {} to={} group={} quests={} gain={:.0f} solo={} t_solo={} t_group={:.0f} p_solo={} "
        "p_group={:.0f} walk={:.0f} eta={:.0f} prog={}/{} partner={}/{}{} reason={}{}",
        bot->GetName(), bestPartner->GetName(), id, QuestList(mine.obj, bestObj), best.xphGroupL + best.xphGroupP,
        never ? std::string("never") : std::to_string(uint32(best.xphSoloL + best.xphSoloP)), secs(best.tSoloL),
        best.tGroupL, secs(best.tSoloP), best.tGroupP, best.walkYd, best.walkSec, mine.obj.have, mine.obj.need,
        bestObj.have, bestObj.need, Stamp(), best.camp.known ? "a camp one cannot solo; " : "same objective; ",
        best.why));
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
        for (Seek const& seek : me.seeks)
            RemoveFrom(seek.bucket, low);
        me.seeks.clear();
        me.level = bot->GetLevel();
    }
    me.map = bot->GetMapId();
    me.zone = bot->GetZoneId();
    // a sought objective done or gone from its log leaves the board
    for (auto itr = me.seeks.begin(); itr != me.seeks.end();)
    {
        Objective o;
        if (!ReadObjective(bot, itr->obj.questId, itr->obj.index, o) || !o.Left())
        {
            RemoveFrom(itr->bucket, low);
            itr = me.seeks.erase(itr);
        }
        else
            ++itr;
    }

    Objective obj;
    bool const has = !bot->GetMap()->Instanceable() && CurrentObjective(botAI, bot, obj) && obj.Left();
    if (!has)
    {
        if (me.bucket &&
            !std::any_of(me.seeks.begin(), me.seeks.end(), [&me](Seek const& s) { return s.bucket == me.bucket; }))
            RemoveFrom(me.bucket, low);
        me.bucket = 0;
        me.cur = Objective();
        return;
    }
    uint64 const key = BucketKey(me.map, me.zone, obj);
    auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    WorldPosition const at = quest ? quest->pos : WorldPosition(bot);
    if (key != me.bucket || !obj.Same(me.cur))
    {
        if (me.bucket &&
            !std::any_of(me.seeks.begin(), me.seeks.end(), [&me](Seek const& s) { return s.bucket == me.bucket; }))
            RemoveFrom(me.bucket, low);
        AddTo(key, low);
        me.bucket = key;
        me.cur = obj;
        me.pace = ComputePace(bot, obj, at);
        return;
    }
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
        if (player == leader && camp.known && campFor == leader->GetGUID())
            if (auto* data = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data))
            {
                data->pos = camp.at;
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
    // another member's objective
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
                    !Serves(m, post->second.partyObj, killed))
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
                if (me->second.bucket)
                    RemoveFrom(me->second.bucket, low);
                for (Seek const& seek : me->second.seeks)
                    RemoveFrom(seek.bucket, low);
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
            if (mob->IsAlive() && mob->GetExactDist2d(&v.at) <= radius && Serves(bot, obj, mob))
                ++units;
    seek.camp.units = std::max<uint32>(units, 1);
    std::string line;
    {
        std::lock_guard<std::mutex> guard(lock);
        Posting& me = postings[bot->GetGUID().GetCounter()];
        if (me.guid.IsEmpty())
        {
            me.guid = bot->GetGUID();
            me.name = bot->GetName();
            me.level = bot->GetLevel();
        }
        seek.bucket = BucketKey(bot->GetMapId(), bot->GetZoneId(), obj);
        for (auto itr = me.seeks.begin(); itr != me.seeks.end();)
        {
            if (itr->obj.Same(obj))
            {
                if (itr->bucket != me.bucket)
                    RemoveFrom(itr->bucket, bot->GetGUID().GetCounter());
                itr = me.seeks.erase(itr);
            }
            else
                ++itr;
        }
        me.seeks.push_back(seek);
        AddTo(seek.bucket, bot->GetGUID().GetCounter());
        line = Acore::StringFormat(
            "[coop] seek {} quests=q{} camp={} mobs={} units={}{} reason=looks for a party: "
            "{}",
            bot->GetName(), questId, nearest->GetName(), v.mobs, seek.camp.units, Stamp(), v.why);
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
