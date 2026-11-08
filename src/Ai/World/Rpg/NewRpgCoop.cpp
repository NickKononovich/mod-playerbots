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

struct Posting
{
    ObjectGuid guid{};
    std::string name;
    uint32 map = 0;
    uint32 zone = 0;
    uint8 level = 0;
    Objective cur;
    uint64 bucket = 0;
    uint32 since = 0;       // ms: it took this objective (its measured solo pace counts from here)
    uint32 lastUnitAt = 0;  // ms of its last counter step alone
    uint32 units = 0;       // counter steps alone since `since`
    std::vector<Seek> seeks;
    std::unordered_map<uint64, uint32> seen;  // bucket -> the board version it last looked at
    uint32 lastKillEntry = 0;
    uint32 itemSource = 0;  // the creature whose kill last raised its item counter
    // in a party
    uint32 party = 0;
    Objective partyObj;
    uint32 joinedAt = 0;  // ms of the accept
    uint32 haveAtJoin = 0;
    uint64 xp = 0;  // kill XP in the party
    float walkYd = 0.0f;
    bool arrived = false;
    float soloXph = 0.0f;  // the offer's solo forecast
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

// the chance of `itemId` in one kill of `entry` (creature_loot_template), 1 when not found
float DropChance(uint32 entry, uint32 itemId)
{
    CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(entry);
    LootTemplate const* loot = tmpl && tmpl->lootid ? LootTemplates_Creature.GetLootFor(tmpl->lootid) : nullptr;
    if (!loot)
        return 1.0f;
    float chance = 0.0f;
    loot->VisitEntries(
        [&](LootStoreItem const& entryItem, float itemChance)
        {
            if (entryItem.itemid == itemId)
                chance = std::max(chance, itemChance);
        });
    return chance > 0.0f ? std::min(chance / 100.0f, 1.0f) : 1.0f;
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

// the forecast of one pairing
struct Plan
{
    bool ok = false;
    std::string why;
    float tSoloL = 0.0f, tGroupL = 0.0f, tSoloP = 0.0f, tGroupP = 0.0f;
    float walkYd = 0.0f;
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

// seconds per objective unit alone: measured (its counter steps since it took the objective), at least the fight
// model (the objective creature's health over its damage, over the drop chance); `fight`: the model's part
float UnitSeconds(Side const& s, float dps, float& fight, uint8& mobLevel)
{
    fight = 0.0f;
    mobLevel = 0;
    uint32 const entry = s.obj.item ? s.post->itemSource : s.obj.key;
    if (entry)
    {
        float const hp = TemplateHealth(entry, mobLevel);
        float const chance = s.obj.item ? DropChance(entry, s.obj.key) : 1.0f;
        if (hp > 0.0f && chance > 0.0f)
            fight = hp / std::max(dps, 1.0f) / chance;
    }
    float measured = 0.0f;
    if (s.post->cur.Same(s.obj) && s.post->units && s.post->lastUnitAt != s.post->since)
        measured = float(getMSTimeDiff(s.post->since, s.post->lastUnitAt)) / IN_MILLISECONDS / float(s.post->units);
    if (measured <= 0.0f)
        return fight;
    if (fight <= 0.0f)
        fight = measured;  // no model: all of it counts as fighting
    return std::max(measured, fight);
}

float KillXp(Player* p, uint8 mobLevel)
{
    if (!mobLevel)
        return 0.0f;
    return float(
        Acore::XP::BaseGain(p->GetLevel(), mobLevel, GetContentLevelsForMapAndZone(p->GetMapId(), p->GetZoneId())));
}

// L leads (it stays on its work), P walks to it. `partyDps`/`partyLevels`/`partySize`: the party L leads already
Plan Forecast(Side const& l, Side const& pr, float partyDps, uint32 partyLevels, uint32 partySize)
{
    Plan plan;
    Player* const lp = l.p;
    Player* const pp = pr.p;
    float const dpsL = DpsEstimate::EstimateDps(lp, nullptr, DpsEstimate::BuffContext::Group);
    float const dpsP = DpsEstimate::EstimateDps(pp, nullptr, DpsEstimate::BuffContext::Group);
    float const sumDps = partyDps + dpsP;
    plan.walkYd = lp->GetExactDist2d(pp);
    float const walkSec = plan.walkYd / std::max(pp->GetSpeed(MOVE_RUN), 1.0f);
    uint32 const rL = l.obj.Left();
    uint32 const rP = pr.obj.Left();
    uint32 const shared = std::min(rL, rP);
    if (!shared)
    {
        plan.why = "calc: a counter is closed";
        return plan;
    }

    float fightL = 0.0f, fightP = 0.0f;
    uint8 levelL = 0, levelP = 0;
    float const unitL = l.camp ? NO_TIME : UnitSeconds(l, dpsL, fightL, levelL);
    float const unitP = pr.camp ? NO_TIME : UnitSeconds(pr, dpsP, fightP, levelP);

    // the shared pace: where the leader works (its spots, its search between kills), the fight at the party's
    // damage; a camp the leader could not solo: the camp's fight at the party's damage per objective unit
    Camp const* camp = l.camp ? l.camp : pr.camp;
    float groupUnit = 0.0f;
    if (l.camp)
        groupUnit = l.camp->fightSec * l.camp->dps / std::max(sumDps, 1.0f) / float(std::max<uint32>(l.camp->units, 1));
    else
    {
        if (unitL <= 0.0f)
        {
            plan.why = "calc: " + lp->GetName() + " has no pace on it yet (no measured step, no model)";
            return plan;
        }
        groupUnit = fightL * dpsL / std::max(sumDps, 1.0f) + std::max(0.0f, unitL - fightL);
    }
    if (!pr.camp && unitP <= 0.0f)
    {
        plan.why = "calc: " + pp->GetName() + " has no pace on it yet (no measured step, no model)";
        return plan;
    }
    uint8 const mobLevel = camp ? camp->topLevel : std::max(levelL, levelP);

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

    // the time to close each counter: alone, and together while both need kills (the one that needs fewer leaves
    // when its counter closes), the walk to meet on both (the leader's New RPG waits for a member far away)
    plan.tSoloL = l.camp ? NO_TIME : float(rL) * unitL;
    plan.tSoloP = pr.camp ? NO_TIME : float(rP) * unitP;
    plan.tGroupL = walkSec + float(shared) * groupUnit + (l.camp ? 0.0f : float(rL - shared) * unitL);
    plan.tGroupP = walkSec + float(shared) * groupUnit + (pr.camp ? 0.0f : float(rP - shared) * unitP);
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
    me.since = now;
    me.lastUnitAt = now;
    me.units = 0;
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
        out.logs.push_back(Acore::StringFormat("[coop] gain {} group={} got={:.0f} solo={:.0f}{}", p.name, party.id,
                                               got, p.soloXph, Stamp()));
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
            other->second.since = now;
            other->second.lastUnitAt = now;
            other->second.units = 0;
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

    Plan best;
    Player* bestPartner = nullptr;
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
        Plan plan = Forecast(mine, other, partyDps, partyLevels, partySize);
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
            bestObj = other.obj;
        }
    }
    if (!bestPartner)
        return;

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
    other.soloXph = best.xphSoloP;
    if (!led)
    {
        me.walkYd = 0.0f;
        me.soloXph = best.xphSoloL;
    }
    lastRefusal.erase(PairKey(bot->GetGUID().GetCounter(), bestPartner->GetGUID().GetCounter()));
    out.logs.push_back(Acore::StringFormat(
        "[coop] offer {} to={} group={} quests={} gain={:.0f} solo={:.0f} t_solo={:.0f} t_group={:.0f} "
        "p_solo={:.0f} p_group={:.0f} walk={:.0f} prog={}/{} partner={}/{}{} reason={}{}",
        bot->GetName(), bestPartner->GetName(), id, QuestList(mine.obj, bestObj), best.xphGroupL + best.xphGroupP,
        best.xphSoloL + best.xphSoloP, std::min(best.tSoloL, 99999.0f), best.tGroupL, std::min(best.tSoloP, 99999.0f),
        best.tGroupP, best.walkYd, mine.obj.have, mine.obj.need, bestObj.have, bestObj.need, Stamp(),
        best.camp.known ? "a camp one cannot solo; " : "same objective; ", best.why));
    out.ops.push_back(std::make_unique<CoopFormOperation>(id, bot->GetGUID(), bestPartner->GetGUID()));
}

// the bot's own entry on the board (lock held): its objective and counter; its measured solo pace
void UpdatePostingLocked(PlayerbotAI* botAI, Player* bot, Posting& me, uint32 now)
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
    if (key != me.bucket || !obj.Same(me.cur))
    {
        if (me.bucket &&
            !std::any_of(me.seeks.begin(), me.seeks.end(), [&me](Seek const& s) { return s.bucket == me.bucket; }))
            RemoveFrom(me.bucket, low);
        AddTo(key, low);
        me.bucket = key;
        me.cur = obj;
        me.since = now;
        me.lastUnitAt = now;
        me.units = 0;
        return;
    }
    if (obj.have != me.cur.have || obj.need != me.cur.need)
    {
        if (obj.have > me.cur.have)
        {
            if (!me.party)
            {
                me.units += obj.have - me.cur.have;
                me.lastUnitAt = now;
            }
            if (obj.item && me.lastKillEntry)
                me.itemSource = me.lastKillEntry;
        }
        me.cur = obj;
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
        exit = std::string(leader ? "its own plan moved on: " : "the global leader moved it: ") +
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

    // the member keeps within its loot reach of the leader: the corpses it loots and the leader's pulls (the
    // core's kill credit at its group reward distance) are in reach
    float const distance = bot->GetExactDist(lead);
    if (!me.arrived && distance <= sPlayerbotAIConfig.LootDistance)
    {
        me.arrived = true;
        out.logs.push_back(
            Acore::StringFormat("[coop] wait {} group={} s={:.0f} walked={:.0f}{} reason=walk to the "
                                "leader {}",
                                me.name, party.id, float(getMSTimeDiff(me.joinedAt, now)) / IN_MILLISECONDS, me.walkYd,
                                Stamp(), lead->GetName()));
    }
    if (distance <= sPlayerbotAIConfig.LootDistance)
    {
        order.kind = NewRpgCoop::Order::Kind::Stay;
        return order;
    }
    order.kind = NewRpgCoop::Order::Kind::Follow;
    order.leader = lead;
    order.pos = WorldPosition(lead);
    order.farWalk = distance > sPlayerbotAIConfig.SightDistance;
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
            me->second.lastKillEntry = killed->GetEntry();
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
    uint32 const now = getMSTime();
    Out out;
    {
        std::lock_guard<std::mutex> guard(lock);
        Posting& me = postings[bot->GetGUID().GetCounter()];
        UpdatePostingLocked(botAI, bot, me, now);
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

void NoWalk(Player* bot)
{
    if (!bot)
        return;
    Out out;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto const me = postings.find(bot->GetGUID().GetCounter());
        if (me != postings.end() && me->second.party)
            LeaveLocked(me->second, "no walk to its leader", getMSTime(), out);
    }
    out.Flush();
}

void OnSpotRefused(PlayerbotAI* botAI, Player* bot, uint32 questId, int32 objectiveIdx, WorldPosition const& dest)
{
    if (!bot || !botAI || !sGuildCoordinator.IsPlanned(bot->GetGUID().GetCounter()) || bot->GetGroup() ||
        dest.GetMapId() != bot->GetMapId())
        return;
    Objective obj;
    if (!ReadObjective(bot, questId, objectiveIdx, obj) || !obj.Left())
        return;
    // the camp: the idle mob whose reach covers the spot nearest to it (as DestinationRefused looks), judged once
    GuidVector const targets = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();
    Creature* nearest = nullptr;
    float nearestGap = std::numeric_limits<float>::max();
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
        if (reach > 0.0f && gap <= reach && gap < nearestGap)
        {
            nearestGap = gap;
            nearest = mob;
        }
    }
    if (!nearest)
        return;
    SoloSafety::Verdict const v = SoloSafety::Judge(bot, nearest);
    if (v.fightSec <= 0.0f)
        return;
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

void AddNewRpgCoopScripts() { new NewRpgCoopPlayerScript(); }
