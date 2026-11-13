/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [assist1] user 2026-11-13: the guild balances itself. The global leader values what holds the guild back and
// weighs each member's experience by it; the help pricing (NewRpgCoop::AssignLocked) then sends the helpers whose
// whole trip costs least against what the help unlocks, and an escort to a gather place the guild needs.
// - Open dungeon [order360]: its uncleared members below its band. The next dungeon opens when the last of them is
//   in the band, so only the one with the most time left (experience to the band over its own rate) moves the
//   unlock. One experience point of it saves 1 / rate seconds of the unlock for every member that has cleared the
//   open dungeon and is in the next one's band; each of their seconds is priced at its own rate times the share of
//   the guild's time its fives take now (members in fives / members online): the dungeon time the wait costs them.
// - Laggard: below the guild's median level with its own progress stuck (a STUCK alert standing, two deaths within
//   the health monitor's hour, a help request its judge raised). Its experience is weighed by the guild's need of
//   its role: the fives' share of the role (tank / healer 1 of 5, dps 3 of 5) over the role's share of the roster.
// - Gather place [assist5]: a gatherer's walk to its place refused by its solo judge - an escort request priced at
//   the gather's worth (its share of units at the guild's value, the gathering plan's), in its own experience.
// Helpers are not picked here: the help pricing prices every free member's trip there and back at its own rate
// times its own weight (a blocker helping costs the guild its blocking time), and refuses when no trip pays off.
// Log: [assist1] blockers|assign lines, each on change.

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "GameTime.h"
#include "GuildCoordinator.h"
#include "GuildGatherPlan.h"
#include "Log.h"
#include "NewRpgCoop.h"
#include "NewRpgQuestRoute.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "StringFormat.h"

namespace
{
constexpr float FIVE_SIZE = 5.0f;
constexpr float FIVE_TANKS = 1.0f;    // a five's tank
constexpr float FIVE_HEALERS = 1.0f;  // a five's healer
constexpr uint32 HOUR_SECONDS = 3600;

// experience from where the member stands to the start of `level`
float XpToLevel(Player* p, uint32 level)
{
    float xp = 0.0f;
    for (uint32 l = p->GetLevel(); l < level; ++l)
        xp += float(sObjectMgr->GetXPForLevel(l));
    return std::max(0.0f, xp - float(p->GetUInt32Value(PLAYER_XP)));
}

struct Blocker
{
    uint32 low = 0;
    std::string text;  // its line part
    std::string key;   // what makes it a new line (no rates)
};
}  // namespace

float GuildCoordinator::AssistWeight(uint32 low, std::string* why) const
{
    std::lock_guard<std::mutex> guard(assistLock_);
    auto const found = assistWeight_.find(low);
    if (found == assistWeight_.end())
        return 1.0f;
    if (why)
        *why = found->second.second;
    return found->second.first;
}

std::string GuildCoordinator::FiveWanted(uint32 low) const
{
    std::lock_guard<std::mutex> guard(assistLock_);
    auto const found = fiveWanted_.find(low);
    return found == fiveWanted_.end() ? std::string() : found->second;
}

void GuildCoordinator::SetFiveWanted(std::unordered_map<uint32, std::string>&& wanted)
{
    std::lock_guard<std::mutex> guard(assistLock_);
    fiveWanted_ = std::move(wanted);
}

void GuildCoordinator::NoteGatherBlocked(Player* bot, uint32 placeId, std::string const& why)
{
    if (!bot || !placeId)
        return;
    GatherAssignment a;
    uint32 const low = bot->GetGUID().GetCounter();
    if (!sGuildGatherPlan.Assigned(low, a) || a.placeId != placeId || a.ratePerHour <= 0.0f || a.share == 0)
        return;
    // the gather's worth: its copper per minute at the place over the minutes its share takes there
    // [assist13] ... less the gatherer's own questing over those minutes (the plan's own comparison, questPerMin): the
    // escort unlocks the gain over what the gatherer does anyway, not the gather's gross worth. b470: Thokhen's
    // Thousand Needles drop place priced 128571 c gross (225 units, 73667 xp) - helpers sent 7684-7826 yd
    float const minutes = float(a.share) / (a.ratePerHour / 60.0f);
    float const copper = (a.valuePerMin - std::max(0.0f, a.questPerMin)) * minutes;
    if (copper <= 0.0f)
        return;
    std::lock_guard<std::mutex> guard(assistLock_);
    for (EscortAsk const& ask : escortAsks_)
        if (ask.low == low && ask.placeId == placeId)
            return;
    escortAsks_.push_back({low, placeId, uint8(bot->GetLevel()), a.spot, copper,
                           Acore::StringFormat("{} x{} ({}) for need {}: {}", a.placeName, a.share,
                                               GuildGatherPlan::KindName(a.kind), a.need, why)});
}

void GuildCoordinator::OnEscortEnd(uint32 low, uint32 placeId, bool done)
{
    if (done)
        sGuildGatherPlan.Unrefuse(low, placeId);
    LOG_INFO("playerbots", "[assist5] escort-end {} place={} done={} reason={}", Who(low), placeId, done ? 1 : 0,
             done ? "the place's guard is dead where it stands: the place is its choice again"
                  : "the escort ended unfinished: the place stays refused at this level");
}

void GuildCoordinator::UpdateAssist()
{
    // the board first, without lock_ (lock order: board, then lock_)
    std::vector<NewRpgCoop::HelpView> const helps = NewRpgCoop::HelpSnapshot();
    std::set<uint32> asked;
    for (NewRpgCoop::HelpView const& h : helps)
        asked.insert(h.requester);

    std::unordered_map<uint32, std::pair<float, std::string>> weights;
    std::vector<Blocker> blockers;
    std::vector<std::tuple<uint32, uint32, WorldPosition, float, std::string>> escorts;  // low, place, spot, xp, why
    std::vector<EscortAsk> asks;
    {
        std::lock_guard<std::mutex> guard(assistLock_);
        asks.swap(escortAsks_);
    }
    {
        CoordLock guard(lock_, __func__);
        std::vector<std::pair<uint32, Player*>> online;
        std::vector<uint8> levels;
        std::map<std::string, float> roleCount;
        // the fives' role of each member (the role plan's [roles59], else the plan row's; a gatherer plays dps)
        std::unordered_map<uint32, std::string> roleOf;
        for (PlanRow const& row : plan_)
        {
            if (row.grp < 'A' || row.grp > 'C')
                continue;
            std::string const planned = RoleOf(row.low);
            std::string const role = planned.empty() ? row.role : planned;
            roleOf[row.low] = role == "tank" || role == "healer" ? role : "dps";
            roleCount[roleOf[row.low]] += 1.0f;
            Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
            if (!p || !p->IsInWorld() || HumanCharacter(p))
                continue;
            online.emplace_back(row.low, p);
            levels.push_back(p->GetLevel());
        }
        if (online.empty())
            return;
        std::vector<uint8> sorted = levels;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        uint8 const median = sorted[sorted.size() / 2];
        float members = 0.0f;
        for (auto const& [role, n] : roleCount)
            members += n;

        // (a) the open dungeon's members below its band: the critical one moves the unlock
        DungeonOption const* open = nullptr;
        for (DungeonOption const& d : dungeons_)
            if (!orderOpenKey_.empty() && d.contentKey == orderOpenKey_ && (!open || d.minLevel < open->minLevel))
                open = &d;
        if (open)
        {
            std::unordered_set<uint32> below;
            {
                std::lock_guard<std::mutex> held(orderHeldLock_);
                below = orderHeldBelow_;
            }
            // the next dungeon in the order and the members that wait for it: cleared the open one, in its band
            DungeonOption const* next = nullptr;
            if (orderPos_.size() == dungeons_.size())
            {
                size_t const at = orderPos_[size_t(open - dungeons_.data())];
                for (size_t pos = at + 1; pos < orderIdx_.size() && !next; ++pos)
                    if (dungeons_[orderIdx_[pos]].contentKey != open->contentKey)
                        next = &dungeons_[orderIdx_[pos]];
            }
            float inFives = 0.0f;
            for (auto const& [id, five] : fives_)
                if (five.stage != GuildFive::Stage::Done)
                    inFives += float(five.members.size());
            float const fiveShare = std::min(1.0f, inFives / float(online.size()));
            float waitRate = 0.0f;
            uint32 waiting = 0;
            if (next)
                for (auto const& [low, p] : online)
                    if (Cleared(open->contentKey, low) && p->GetLevel() >= next->minLevel)
                    {
                        waitRate += QuestRoute::XpPerSecond(low);
                        ++waiting;
                    }
            uint32 critical = 0;
            float criticalT = 0.0f, criticalRate = 0.0f, criticalXp = 0.0f;
            for (auto const& [low, p] : online)
            {
                if (!below.count(low))
                    continue;
                float const xp = XpToLevel(p, open->minLevel);
                float const rate = QuestRoute::XpPerSecond(low);
                float const t = rate > 0.0f ? xp / rate : std::numeric_limits<float>::max();
                if (!critical || t > criticalT)
                {
                    critical = low;
                    criticalT = t;
                    criticalRate = rate;
                    criticalXp = xp;
                }
                Blocker b;
                b.low = low;
                b.key = "dungeon " + Who(low);
                b.text = Acore::StringFormat("{} L{} below {} (L{}): {:.0f} xp to the band at {:.2f} xp/s", Who(low),
                                             p->GetLevel(), open->contentKey, open->minLevel, xp, rate);
                blockers.push_back(std::move(b));
            }
            if (critical && criticalRate > 0.0f && waitRate > 0.0f && fiveShare > 0.0f)
            {
                float const term = waitRate * fiveShare / criticalRate;
                std::string const why = Acore::StringFormat(
                    "the last below {}'s band ({:.0f} xp, {:.0f} min at its rate): {} members wait for {} at {:.2f} "
                    "xp/s, {:.0f}% of the guild's time in fives",
                    open->contentKey, criticalXp, criticalT / 60.0f, waiting, next ? next->contentKey : "-", waitRate,
                    fiveShare * 100.0f);
                weights[critical] = {1.0f + term, why};
                for (Blocker& b : blockers)
                    if (b.low == critical)
                        b.text += Acore::StringFormat(" - the critical one, unlock x{:.2f} ({})", 1.0f + term, why);
            }
            // its in-band uncleared members wait for a five, not for a helper (the five planner's)
            for (auto const& [low, p] : online)
                if (!Cleared(open->contentKey, low) && !below.count(low) && p->GetLevel() <= open->maxLevel)
                    blockers.push_back({low,
                                        Acore::StringFormat("{} L{} {} first clear (in band: the fives')", Who(low),
                                                            p->GetLevel(), open->contentKey),
                                        "five " + Who(low)});
        }

        // (c) laggards: below the median, their own progress stuck; weighed by the guild's need of their role
        uint32 const nowSec = uint32(GameTime::GetGameTime().count());
        float medianRate = 0.0f;
        uint32 medianN = 0;
        for (auto const& [low, p] : online)
            if (p->GetLevel() >= median)
            {
                medianRate += QuestRoute::XpPerSecond(low);
                ++medianN;
            }
        medianRate = medianN ? medianRate / float(medianN) : 0.0f;
        for (auto const& [low, p] : online)
        {
            if (p->GetLevel() >= median)
                continue;
            auto const st = bots_.find(low);
            if (st == bots_.end())
                continue;
            uint32 deaths = 0;
            for (uint32 at : st->second.deaths)
                deaths += nowSec - at <= HOUR_SECONDS;
            std::string signal;
            if (st->second.stillWhy.rfind("STUCK", 0) == 0)
                signal = st->second.stillWhy.substr(0, 60);
            else if (deaths >= 2)
                signal = std::to_string(deaths) + " deaths in the hour";
            else if (asked.count(low))
                signal = "its judge refused a camp (a help request)";
            if (signal.empty())
                continue;
            std::string const role = roleOf[low];
            float const needShare = role == "tank" ? FIVE_TANKS / FIVE_SIZE
                                    : role == "healer" ? FIVE_HEALERS / FIVE_SIZE
                                                       : (FIVE_SIZE - FIVE_TANKS - FIVE_HEALERS) / FIVE_SIZE;
            float const have = members > 0.0f ? roleCount[role] / members : 0.0f;
            float const need = have > 0.0f ? std::max(1.0f, needShare / have) : 1.0f;
            float const rate = QuestRoute::XpPerSecond(low);
            auto& w = weights[low];
            if (w.first < 1.0f)
                w.first = 1.0f;
            w.first += need - 1.0f;
            w.second += (w.second.empty() ? "" : "; ") +
                        Acore::StringFormat("laggard L{} < median L{} ({}), {} need x{:.2f}", p->GetLevel(), median,
                                            signal, role, need);
            blockers.push_back({low,
                                Acore::StringFormat("{} L{} laggard ({}): {:.0f} xp/h behind the median's {:.0f}, {} "
                                                    "need x{:.2f}",
                                                    Who(low), p->GetLevel(), signal,
                                                    std::max(0.0f, medianRate - rate) * 3600.0f, medianRate * 3600.0f,
                                                    role, need),
                                "lag " + Who(low) + " " + signal.substr(0, 5)});
        }

        // (b) the gatherers' refused places: their worth in the gatherer's own experience
        for (EscortAsk const& ask : asks)
        {
            Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(ask.low));
            auto const st = bots_.find(ask.low);
            if (!p || !p->IsInWorld() || p->GetLevel() != ask.level || st == bots_.end() ||
                st->second.copperPerXp <= 0.0f)
                continue;
            float const xp = ask.valueCopper / st->second.copperPerXp;
            escorts.emplace_back(ask.low, ask.placeId, ask.spot, xp,
                                 Acore::StringFormat("[assist5] {:.0f} c at {:.2f} c/xp: {}", ask.valueCopper,
                                                     st->second.copperPerXp, ask.why));
        }
        for (NewRpgCoop::HelpView const& h : helps)
            if (h.goal.rfind("escort", 0) == 0)
                blockers.push_back({h.requester,
                                    Who(h.requester) + " " + h.goal + " (a gather place its judge refused)",
                                    "escort " + Who(h.requester) + h.goal});
    }
    {
        std::lock_guard<std::mutex> guard(assistLock_);
        assistWeight_.swap(weights);
    }
    for (auto const& [low, place, spot, xp, why] : escorts)
        if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low)))
            NewRpgCoop::PostEscort(p, place, spot, xp, why);

    // the lines, each on change
    std::string key, text;
    uint32 n = 0;
    for (Blocker const& b : blockers)
    {
        key += b.key + "|";
        text += Acore::StringFormat("{}{}. {}", text.empty() ? "" : "; ", ++n, b.text);
    }
    for (auto const& [low, w] : assistWeight_)
        key += Acore::StringFormat("{}:{:.0f}|", low, w.first);
    if (key != assistBlockersLogged_)
    {
        assistBlockersLogged_ = key;
        LOG_INFO("playerbots", "[assist1] guild blockers: {}", text.empty() ? "none" : text);
    }
    std::string assign;
    std::string open;
    for (NewRpgCoop::HelpView const& h : helps)
    {
        float const w = AssistWeight(h.requester, nullptr);
        if (h.helpers.empty())
        {
            if (!h.lastRefusal.empty())
                open += Acore::StringFormat("{}{} {} (x{:.2f}): {}", open.empty() ? "" : "; ", Who(h.requester),
                                            h.goal, w, h.lastRefusal.substr(0, 120));
            continue;
        }
        std::string names;
        for (uint32 helper : h.helpers)
        {
            Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(helper));
            names += (names.empty() ? "" : ", ") + Who(helper) + (p ? " L" + std::to_string(p->GetLevel()) : "");
        }
        assign += Acore::StringFormat("{}{} -> {} {} (x{:.2f})", assign.empty() ? "" : "; ", names, Who(h.requester),
                                      h.goal, w);
    }
    std::string const line = "assigned: " + (assign.empty() ? std::string("none") : assign) +
                             (open.empty() ? "" : " | unassigned: " + open);
    if (line != assistAssignLogged_)
    {
        assistAssignLogged_ = line;
        LOG_INFO("playerbots", "[assist1] {}", line);
    }
}
