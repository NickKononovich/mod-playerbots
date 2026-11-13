/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgQuestRoute.h"
#include "SoloSafety.h"  // [walk68]
#include "BotPath.h"    // [walk68]

#include "SlowScope.h"  // [perf26]
#include "GuildCoordinator.h"
#include "Group.h"  // [town12]
#include "Playerbots.h"  // [town12] GET_PLAYERBOT_AI
#include "Log.h"
#include "NewRpgBaseAction.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"  // [town15] SightDistance
#include "QuestDef.h"
#include "QuestStats.h"
#include "StringFormat.h"
#include "TravelMgr.h"  // [lag340] the landmasses, the routes across them

#include <algorithm>
#include <cmath>
#include <ctime>
#include <limits>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>

using QuestRoute::Stop;
using QuestRoute::StopKind;

namespace
{
    constexpr float SECONDS_EPSILON = 0.001f;    // float noise, not a preference
    constexpr float TOWN_STOP_REACHED = 10.0f;   // GO_CAMP's own reach (NewRpgStatusUpdateAction)
    // [routeplan] the compute budget of one build: the legs of the order found are measured (navmesh, spawns), the
    // order improved on them, its new legs measured - this many rounds at most (not a behaviour rule: a cost cap)
    constexpr uint32 MEASURE_ROUNDS = 3;

    float Dist(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

    // the stops of `all` (+ `added` past its end) by index
    struct Pool
    {
        std::vector<Stop> const& all;
        std::vector<Stop> const* added;
        Stop const& operator()(size_t i) const { return i < all.size() ? all[i] : (*added)[i - all.size()]; }
    };

    // [routeplan] the order's seconds from the bot's spot (node 0): each leg's walk and fights, each stop's work
    float OrderSeconds(QuestRoute::LegTable const& legs, Pool const& at, std::vector<size_t> const& order,
                       float* walkYd = nullptr)
    {
        float yd = 0.0f, seconds = 0.0f;
        uint32 prev = 0;
        for (size_t i : order)
        {
            Stop const& s = at(i);
            seconds += legs.Seconds(prev, s.node) + s.work;
            if (walkYd)
                yd += legs.Yards(prev, s.node);
            prev = s.node;
        }
        if (walkYd)
            *walkYd = yd;
        return seconds;
    }

    // every quest's giver before its objective, its objective (and giver) before its ender
    bool Feasible(Pool const& at, std::vector<size_t> const& order)
    {
        std::unordered_map<uint32, size_t> giverAt, objectiveAt;
        for (size_t k = 0; k < order.size(); ++k)
        {
            Stop const& s = at(order[k]);
            for (uint32 q : s.quests)
                if (s.kind == StopKind::Giver)
                    giverAt.emplace(q, k);
                else if (s.kind == StopKind::Objective)
                    objectiveAt.emplace(q, k);
        }
        for (size_t k = 0; k < order.size(); ++k)
        {
            Stop const& s = at(order[k]);
            if (s.kind != StopKind::Objective && s.kind != StopKind::Ender)
                continue;
            for (uint32 q : s.quests)
            {
                auto const g = giverAt.find(q);
                if (g != giverAt.end() && g->second > k)
                    return false;
                if (s.kind == StopKind::Ender)
                {
                    auto const o = objectiveAt.find(q);
                    if (o != objectiveAt.end() && o->second > k)
                        return false;
                }
            }
        }
        return true;
    }

    // nearest feasible neighbour (by the leg's seconds) from the bot's spot; `pinned` (an index, or SIZE_MAX) first
    std::vector<size_t> NearestNeighbour(QuestRoute::LegTable const& legs, std::vector<Stop> const& all,
                                         size_t pinned)
    {
        std::unordered_map<uint32, size_t> giverOf, objectiveOf;
        for (size_t i = 0; i < all.size(); ++i)
            for (uint32 q : all[i].quests)
                if (all[i].kind == StopKind::Giver)
                    giverOf.emplace(q, i);
                else if (all[i].kind == StopKind::Objective)
                    objectiveOf.emplace(q, i);
        std::vector<bool> placed(all.size(), false);
        std::vector<size_t> order;
        uint32 prev = 0;
        auto const place = [&](size_t i)
        {
            placed[i] = true;
            order.push_back(i);
            prev = all[i].node;
        };
        if (pinned < all.size())
            place(pinned);
        auto const ready = [&](size_t i)
        {
            Stop const& s = all[i];
            if (s.kind != StopKind::Objective && s.kind != StopKind::Ender)
                return true;
            for (uint32 q : s.quests)
            {
                auto const g = giverOf.find(q);
                if (g != giverOf.end() && !placed[g->second])
                    return false;
                auto const o = objectiveOf.find(q);
                if (s.kind == StopKind::Ender && o != objectiveOf.end() && !placed[o->second])
                    return false;
            }
            return true;
        };
        while (order.size() < all.size())
        {
            size_t best = all.size();
            float bestSeconds = std::numeric_limits<float>::max();
            for (size_t i = 0; i < all.size(); ++i)
                if (!placed[i] && ready(i))
                {
                    float const seconds = legs.Seconds(prev, all[i].node);
                    if (seconds < bestSeconds)
                    {
                        best = i;
                        bestSeconds = seconds;
                    }
                }
            if (best == all.size())  // a cycle in the precedence cannot happen; keep the rest in their order
            {
                for (size_t i = 0; i < all.size(); ++i)
                    if (!placed[i])
                        place(i);
                break;
            }
            place(best);
        }
        return order;
    }

    // 2-opt and relocate moves while one shortens the route and keeps it feasible; the first stop stays when pinned
    void Improve(QuestRoute::LegTable const& legs, Pool const& at, std::vector<size_t>& order, bool pinnedFirst)
    {
        size_t const n = order.size();
        size_t const from = pinnedFirst ? 1 : 0;
        if (n < from + 2)
            return;
        float best = OrderSeconds(legs, at, order);
        for (size_t pass = 0; pass < n; ++pass)  // each pass shortens the route or ends the search
        {
            bool improved = false;
            for (size_t i = from; i + 1 < n; ++i)
                for (size_t j = i + 1; j < n; ++j)
                {
                    std::reverse(order.begin() + i, order.begin() + j + 1);
                    float const cost = OrderSeconds(legs, at, order);
                    if (cost + SECONDS_EPSILON < best && Feasible(at, order))
                    {
                        best = cost;
                        improved = true;
                    }
                    else
                        std::reverse(order.begin() + i, order.begin() + j + 1);
                }
            for (size_t i = from; i < n; ++i)
                for (size_t j = from; j < n; ++j)
                {
                    if (i == j)
                        continue;
                    size_t const moved = order[i];
                    order.erase(order.begin() + i);
                    order.insert(order.begin() + j, moved);
                    float const cost = OrderSeconds(legs, at, order);
                    if (cost + SECONDS_EPSILON < best && Feasible(at, order))
                    {
                        best = cost;
                        improved = true;
                    }
                    else
                    {
                        order.erase(order.begin() + j);
                        order.insert(order.begin() + i, moved);
                    }
                }
            if (!improved)
                break;
        }
    }

    // [quest15] the order's experience-weighted hand-in seconds: each ender's arrival (the legs and work before it,
    // its own leg) times the experience handed in there. The run's measure is the hand-ins it makes: a route that
    // walks the same stops but hands in sooner gets its experience sooner (the bot builds its route again at each
    // level, a town task, a new quest - stops at the end of a long route are reached late or never)
    // [quest124] `reach`: the chance the bot gets one place further into a plan (Input::reach, measured). A hand-in
    // at place k is reached with p^k, so its lateness counts against that chance - one placed where the bot probably
    // never arrives costs what it probably loses, not merely its own seconds. reach 1.0 is the old measure exactly
    double HandInSeconds(QuestRoute::LegTable const& legs, Pool const& at, std::vector<size_t> const& order,
                         double reach)
    {
        double weighted = 0.0;
        float seconds = 0.0f;
        uint32 prev = 0;
        double share = 1.0;
        for (size_t i : order)
        {
            Stop const& s = at(i);
            seconds += legs.Seconds(prev, s.node);
            share *= reach;
            // an ender, or a town stop where its party members hand in ([quest16])
            if (s.kind == StopKind::Ender || s.kind == StopKind::Town)
                weighted += double(s.xp) * double(seconds) / std::max(share, std::numeric_limits<double>::min());
            seconds += s.work;
            prev = s.node;
        }
        return weighted;
    }

    // [quest15] user 2026-11-08: a completed quest's ender comes early in the route unless its detour costs more
    // than handing in now gains. Each ender moves to the earliest place where the experience-weighted hand-in seconds
    // drop: the experience it hands in that much sooner against the detour's seconds added to every hand-in after it
    // (HandInSeconds) - the order stays feasible (a giver and an objective before their ender), the first stop stays
    // when pinned. The rest of the order is the shortest one found (Improve)
    uint32 HandInsEarlier(QuestRoute::LegTable const& legs, Pool const& at, std::vector<size_t>& order,
                          bool pinnedFirst, double reach)
    {
        size_t const from = pinnedFirst ? 1 : 0;
        uint32 moved = 0;
        double best = HandInSeconds(legs, at, order, reach);
        for (size_t pass = 0; pass < order.size(); ++pass)  // each pass moves an ender sooner or ends the search
        {
            bool improved = false;
            for (size_t i = from + 1; i < order.size(); ++i)
            {
                if ((at(order[i]).kind != StopKind::Ender && at(order[i]).kind != StopKind::Town) ||
                    at(order[i]).xp <= 0.0f)
                    continue;
                size_t bestPos = i;
                for (size_t j = from; j < i; ++j)
                {
                    std::vector<size_t> trial = order;
                    size_t const stop = trial[i];
                    trial.erase(trial.begin() + i);
                    trial.insert(trial.begin() + j, stop);
                    double const cost = HandInSeconds(legs, at, trial, reach);
                    if (cost + SECONDS_EPSILON < best && Feasible(at, trial))
                    {
                        best = cost;
                        bestPos = j;
                    }
                }
                if (bestPos != i)
                {
                    size_t const stop = order[i];
                    order.erase(order.begin() + i);
                    order.insert(order.begin() + bestPos, stop);
                    improved = true;
                    ++moved;
                }
            }
            if (!improved)
                break;
        }
        return moved;
    }

    // the cheapest place (>= minPos) for a stop at `node` in the order: its position and the seconds it adds
    size_t CheapestInsert(QuestRoute::LegTable const& legs, Pool const& at, std::vector<size_t> const& order,
                          uint32 node, size_t minPos, float& addedSeconds)
    {
        size_t bestPos = order.size();
        addedSeconds = std::numeric_limits<float>::max();
        for (size_t k = std::min(minPos, order.size()); k <= order.size(); ++k)
        {
            uint32 const prev = k > 0 ? at(order[k - 1]).node : 0;
            float delta = legs.Seconds(prev, node);
            if (k < order.size())
            {
                uint32 const next = at(order[k]).node;
                delta += legs.Seconds(node, next) - legs.Seconds(prev, next);
            }
            if (delta < addedSeconds)
            {
                addedSeconds = delta;
                bestPos = k;
            }
        }
        return bestPos;
    }

    struct Trial
    {
        std::vector<Stop> added;
        // an existing hub (index) hands in this quest too, with its experience ([quest15])
        std::vector<std::tuple<size_t, uint32, float>> merges;
        std::vector<size_t> order;
        float xp{0.0f};
        uint32 takes{0};
        float alone{0.0f};  // [routeplan] seconds of the same quests as a trip of their own after the route
    };

    // the offer's giver, then each take's objective after it and its ender after that (an existing hub later in the
    // route when its ender stands there) - each at its cheapest place
    Trial TryOffer(QuestRoute::Input const& in, QuestRoute::LegTable const& legs, std::vector<Stop> const& all,
                   std::vector<size_t> const& order, QuestRoute::Offer const& offer, uint32 slots, bool pinnedFirst)
    {
        Trial t;
        t.order = order;
        t.added.reserve(1 + offer.takes.size() * 2);
        Pool const at{all, &t.added};
        auto const posOf = [&](size_t index)
        { return size_t(std::find(t.order.begin(), t.order.end(), index) - t.order.begin()); };
        float seconds = 0.0f;
        t.added.push_back(offer.giver);
        t.added.back().quests.clear();
        size_t const giverIndex = all.size();
        size_t const giverPos =
            CheapestInsert(legs, at, t.order, offer.giver.node, pinnedFirst ? 1 : 0, seconds);
        t.order.insert(t.order.begin() + giverPos, giverIndex);
        // [routeplan] the trip of its own: from the route's last stop to the giver, then each take's objective and
        // ender in turn
        uint32 aloneAt = order.empty() ? 0 : all[order.back()].node;
        t.alone = legs.Seconds(aloneAt, offer.giver.node);
        aloneAt = offer.giver.node;
        std::vector<QuestRoute::Offer::Take const*> takes;
        for (QuestRoute::Offer::Take const& take : offer.takes)
            takes.push_back(&take);
        std::sort(takes.begin(), takes.end(), [](auto const* a, auto const* b) { return a->xp > b->xp; });
        for (QuestRoute::Offer::Take const* take : takes)
        {
            if (t.takes >= slots)
                break;
            t.added[0].quests.push_back(take->questId);
            t.added.push_back(take->objective);
            size_t const objectiveIndex = all.size() + t.added.size() - 1;
            size_t const objectivePos =
                CheapestInsert(legs, at, t.order, take->objective.node, posOf(giverIndex) + 1, seconds);
            t.order.insert(t.order.begin() + objectivePos, objectiveIndex);
            t.alone += legs.Seconds(aloneAt, take->objective.node) + take->objective.work;
            aloneAt = take->objective.node;
            if (take->hasEnder)
            {
                t.alone += legs.Seconds(aloneAt, take->ender.node);
                aloneAt = take->ender.node;
                bool merged = false;
                for (size_t k = objectivePos + 1; k < t.order.size() && !merged; ++k)
                {
                    Stop const& hub = at(t.order[k]);
                    // [quest15] not a ready hub (its complete quests): the new quest's objective would hold it back
                    if (hub.kind == StopKind::Ender && !hub.ready &&
                        Dist(hub.x, hub.y, take->ender.x, take->ender.y) <= in.hubRadius)
                    {
                        if (t.order[k] < all.size())
                            t.merges.emplace_back(t.order[k], take->questId, take->xp);
                        else
                        {
                            t.added[t.order[k] - all.size()].quests.push_back(take->questId);
                            t.added[t.order[k] - all.size()].xp += take->xp;
                        }
                        merged = true;
                    }
                }
                if (!merged)
                {
                    t.added.push_back(take->ender);
                    t.added.back().xp = take->xp;  // [quest15]
                    t.added.back().ready = false;
                    size_t const enderIndex = all.size() + t.added.size() - 1;
                    size_t const enderPos =
                        CheapestInsert(legs, at, t.order, take->ender.node, posOf(objectiveIndex) + 1, seconds);
                    t.order.insert(t.order.begin() + enderPos, enderIndex);
                }
            }
            t.xp += take->xp;
            ++t.takes;
        }
        return t;
    }

    std::mutex routesLock;
    std::unordered_map<uint32, QuestRoute::BotRoute> routes;

    uint64 Mix(uint64 h, uint64 v) { return h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2)); }

    // the quest of a stop is done there: worked (objective), handed in (ender), taken (giver)
    bool StopQuestDone(Player* bot, StopKind kind, uint32 q)
    {
        QuestStatus const status = bot->GetQuestStatus(q);
        if (kind == StopKind::Objective)
            return status == QUEST_STATUS_COMPLETE || status == QUEST_STATUS_REWARDED;
        if (kind == StopKind::Ender)
            return status == QUEST_STATUS_REWARDED;
        if (kind == StopKind::Giver)
            return status != QUEST_STATUS_NONE;
        return false;
    }

    // [routeplan] the quests a hand-in opens: those whose previous quest it is, its next in chain
    std::vector<uint32> const& UnlockedBy(uint32 questId)
    {
        static std::unordered_map<uint32, std::vector<uint32>> unlocks;
        static std::once_flag built;
        std::call_once(built, []()
        {
            for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
            {
                if (quest->GetPrevQuestId() > 0)
                    unlocks[uint32(quest->GetPrevQuestId())].push_back(id);
                if (quest->GetNextQuestInChain())
                    unlocks[id].push_back(quest->GetNextQuestInChain());
                if (quest->GetNextQuestId())
                    unlocks[id].push_back(quest->GetNextQuestId());
            }
            for (auto& [id, list] : unlocks)
            {
                std::sort(list.begin(), list.end());
                list.erase(std::unique(list.begin(), list.end()), list.end());
            }
        });
        static std::vector<uint32> const none;
        auto const found = unlocks.find(questId);
        return found == unlocks.end() ? none : found->second;
    }
}

float QuestRoute::ChainXp(Player* bot, uint32 questId)
{
    if (!bot || !questId)
        return 0.0f;
    constexpr size_t CHAIN_QUESTS = 32;  // search bound (a chain's length), no behaviour rule
    std::vector<uint32> open{questId};
    std::set<uint32> seen{questId};
    float xp = 0.0f;
    for (size_t i = 0; i < open.size() && i < CHAIN_QUESTS; ++i)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(open[i]);
        if (!quest || bot->GetQuestRewardStatus(open[i]))
            continue;
        xp += float(quest->XPValue(bot->GetLevel()));
        // [help28] the quests this hand-in opens at once, not the whole chain behind them: each later one has its own
        // camps and walks the help does not clear (b377: q985 priced 5825 xp, q224 4600 xp - helpers sent 4991 and
        // 6323 yd at 0.7-5 xp/s; 10 help parties over 1000 yd)
        if (i == 0)
            for (uint32 next : UnlockedBy(open[i]))
                if (seen.insert(next).second)
                    open.push_back(next);
    }
    return xp;
}

uint32 QuestRoute::LegTable::Add(float x, float y, float z)
{
    points_.push_back({x, y, z});
    return uint32(points_.size() - 1);
}

float QuestRoute::LegTable::Straight(uint32 a, uint32 b) const
{
    return Dist(points_[a].x, points_[a].y, points_[b].x, points_[b].y);
}

float QuestRoute::LegTable::Yards(uint32 a, uint32 b) const
{
    if (a == b)
        return 0.0f;
    auto const found = known_.find(Key(a, b));
    // [quest131] a leg nobody searched a corridor for is its straight line, not that line times a measured
    // path-over-straight average: such an average is a rate measured from history, the thing that disqualified the
    // reach measure. Straight line under-states the walk, and the plan line says how many legs it under-states
    return found != known_.end() ? found->second.walkYd : Straight(a, b);
}

float QuestRoute::LegTable::Seconds(uint32 a, uint32 b) const
{
    if (a == b)
        return 0.0f;
    auto const found = known_.find(Key(a, b));
    if (found != known_.end())
        return found->second.walkYd / speed_ + found->second.fightSec;
    // [quest131] the straight line over the bot's speed, and no fight term: a fight-seconds-per-yard average is a
    // measured rate. An estimate honest about what it leaves out beats one that is confidently wrong
    return Straight(a, b) / speed_;
}

size_t QuestRoute::LegTable::Measure(std::vector<std::pair<uint32, uint32>> const& legs)
{
    if (!measure_)
        return 0;
    size_t added = 0;
    for (auto const& [a, b] : legs)
    {
        if (a == b || known_.count(Key(a, b)))
            continue;
        Fact fact;
        float const straight = Straight(a, b);
        if (!measure_(points_[a], points_[b], fact.walkYd, fact.fightSec))
        {
            // [quest131] no corridor (no polygon at an end): the straight line, and no fight time it cannot know
            fact.walkYd = straight;
            fact.fightSec = 0.0f;
            ++estimated_;
        }
        else
        {
            straightYd_ += straight;
            walkYd_ += fact.walkYd;
            fightSec_ += fact.fightSec;
        }
        known_.emplace(Key(a, b), fact);
        ++added;
    }
    return added;
}

QuestRoute::BotRoute& QuestRoute::Of(uint32 botLow)
{
    std::lock_guard<std::mutex> guard(routesLock);
    return routes[botLow];  // references stay valid across rehashes; entries are never erased
}

float QuestRoute::XpPerSecond(uint32 botLow)
{
    std::lock_guard<std::mutex> guard(routesLock);
    auto const found = routes.find(botLow);
    if (found == routes.end() || !found->second.built || found->second.plan.eta <= 0.0f)
        return 0.0f;
    // [help21] the open quests' experience over the route's seconds: a complete quest by its ender inflated it (b368
    // Febineysh's helpers priced 1183 xp against 850 for q412)
    return found->second.plan.openXp / found->second.plan.eta;

}

std::vector<QuestRoute::Point> QuestRoute::RouteStops(uint32 botLow)
{
    std::vector<Point> out;
    std::lock_guard<std::mutex> guard(routesLock);
    auto const found = routes.find(botLow);
    if (found == routes.end() || !found->second.built)
        return out;
    BotRoute const& r = found->second;
    size_t from = 0;
    if (r.hasCurrent)
        for (size_t i = 0; i < r.plan.stops.size(); ++i)
            if (r.plan.stops[i].x == r.current.x && r.plan.stops[i].y == r.current.y)
            {
                from = i;
                break;
            }
    for (size_t i = from; i < r.plan.stops.size(); ++i)
        if (r.plan.stops[i].kind != StopKind::Town)
            out.push_back({r.plan.stops[i].x, r.plan.stops[i].y, r.plan.stops[i].z});
    return out;
}


void QuestRoute::NoteQuestEvent(Player* bot, uint32 questId)
{
    if (!bot)
        return;
    BotRoute& r = Of(bot->GetGUID().GetCounter());
    if (!r.hasCurrent || r.currentDoneMs ||
        std::find(r.current.quests.begin(), r.current.quests.end(), questId) == r.current.quests.end())
        return;
    // a hub or a giver of several quests: done with the last of them
    for (uint32 q : r.current.quests)
        if (!StopQuestDone(bot, r.current.kind, q))
            return;
    r.currentDoneMs = QuestStats::NowMs();
}

namespace
{
    thread_local std::unordered_map<uint64, float> memberHandInXp;  // [quest16] the build on this thread
}

float QuestRoute::MemberHandInXp(uint64 spotKey)
{
    auto const found = memberHandInXp.find(spotKey);
    return found == memberHandInXp.end() ? 0.0f : found->second;
}

uint64 QuestRoute::SpotKey(float x, float y)
{
    return (uint64(uint32(int32(std::lround(x)))) << 32) | uint64(uint32(int32(std::lround(y))));
}

char const* QuestRoute::KindName(StopKind kind)
{
    switch (kind)
    {
        case StopKind::Objective:
            return "objective";
        case StopKind::Ender:
            return "handin";
        case StopKind::Giver:
            return "giver";
        case StopKind::Town:
            return "town";
    }
    return "?";
}

std::string QuestRoute::Signature(Stop const& stop)
{
    std::vector<uint32> quests = stop.quests;
    std::sort(quests.begin(), quests.end());
    std::string sig = KindName(stop.kind);
    for (uint32 q : quests)
        sig += ":" + std::to_string(q);
    if (stop.kind == StopKind::Town)
        sig += "@" + std::to_string(SpotKey(stop.x, stop.y));
    return sig;
}

std::string QuestRoute::Describe(Stop const& stop)
{
    std::string text;
    switch (stop.kind)
    {
        case StopKind::Objective:
            text = "O";
            break;
        case StopKind::Ender:
            text = "E";
            break;
        case StopKind::Giver:
            text = "G";
            break;
        case StopKind::Town:
            return "T(" + stop.what + ")";
    }
    for (size_t i = 0; i < stop.quests.size(); ++i)
        text += (i ? "+" : "") + std::to_string(stop.quests[i]);
    return text;
}

float QuestRoute::EstimateLeg(Plan const& plan, float ax, float ay, float bx, float by, float* walkYd)
{
    float const straight = Dist(ax, ay, bx, by);
    // [quest131] the straight line over the bot's speed. It used to be that line times the build's measured
    // path-over-straight average plus its measured fight seconds per yard - a rate measured from history deciding
    // where the bot goes next, which the user's rule reserves for formulas. It under-states a walk; the plan's
    // legs_straight count says by how many legs
    if (walkYd)
        *walkYd = straight;
    return straight / std::max(plan.speed, 1.0f);
}

QuestRoute::Plan QuestRoute::Solve(Input const& in)
{
    // [routeplan] every point of the build in one leg table: the bot's spot (node 0), the stops, the offers' stops
    LegTable legs(std::max(in.speed, 1.0f), in.measure);
    legs.Add(in.x, in.y, in.z);
    std::vector<Stop> all = in.stops;
    for (Stop& s : all)
        s.node = legs.Add(s.x, s.y, s.z);
    std::vector<Offer> offers = in.offers;
    for (Offer& o : offers)
    {
        o.giver.node = legs.Add(o.giver.x, o.giver.y, o.giver.z);
        for (Offer::Take& take : o.takes)
        {
            take.objective.node = legs.Add(take.objective.x, take.objective.y, take.objective.z);
            if (take.hasEnder)
                take.ender.node = legs.Add(take.ender.x, take.ender.y, take.ender.z);
        }
    }

    size_t pinned = std::numeric_limits<size_t>::max();
    if (in.pinnedQuest)
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i].kind == StopKind::Objective && !all[i].quests.empty() && all[i].quests[0] == in.pinnedQuest)
                pinned = i;
    bool const pinnedFirst = pinned < all.size();
    std::vector<size_t> order = NearestNeighbour(legs, all, pinned);
    Improve(legs, Pool{all, nullptr}, order, pinnedFirst);
    // [routeplan] the log's own order on real legs first: the offers below are priced on them
    auto const measureOrder = [&]()
    {
        std::vector<std::pair<uint32, uint32>> list;
        uint32 prev = 0;
        for (size_t i : order)
        {
            list.emplace_back(prev, all[i].node);
            prev = all[i].node;
        }
        return legs.Measure(list);
    };
    for (uint32 round = 0; round < MEASURE_ROUNDS && measureOrder(); ++round)
        Improve(legs, Pool{all, nullptr}, order, pinnedFirst);

    // givers on the way: the one whose quests add the most experience per second they add. [routeplan] Taken when
    // adding them here costs no more than the same quests as a trip of their own after the route (a quest worth doing
    // is done the cheaper way: b343's rule, a rate above the route's own, refused nearly every offer once the log's
    // done quests made the route's rate high - open objectives median 1, one hand-in per hub visit)
    Plan plan;
    float xp = in.logXp;
    uint32 slots = in.freeSlots;
    std::vector<bool> used(offers.size(), false);
    while (slots)
    {
        float const seconds = OrderSeconds(legs, Pool{all, nullptr}, order);
        float bestRatio = -1.0f;
        size_t bestOffer = offers.size();
        Trial best;
        for (size_t o = 0; o < offers.size(); ++o)
        {
            if (used[o])
                continue;
            Trial trial = TryOffer(in, legs, all, order, offers[o], slots, pinnedFirst);
            if (!trial.takes || trial.xp <= 0.0f)
                continue;
            float const added = OrderSeconds(legs, Pool{all, &trial.added}, trial.order) - seconds;
            // [quest124] the offer is worth what the bot will reach of it. Its experience arrives at its last new
            // place in the order (a quest pays at its ender), so it carries p^place of itself: an offer that would
            // sit late in an already long order is worth almost nothing and is not taken. The plan shortens itself
            // this way - no stop limit chosen by hand
            size_t place = 0;
            for (size_t k = 0; k < trial.order.size(); ++k)
                if (trial.order[k] >= all.size())
                    place = k + 1;
            float const worth = trial.xp * float(std::pow(double(in.reach), double(place)));
            float const ratio = worth / std::max(added, SECONDS_EPSILON);
            if (added <= trial.alone + SECONDS_EPSILON && worth > 0.0f && ratio > bestRatio)
            {
                bestRatio = ratio;
                bestOffer = o;
                best = std::move(trial);
            }
        }
        if (bestOffer == offers.size())
            break;
        used[bestOffer] = true;
        for (auto const& [hub, quest, questXp] : best.merges)
        {
            all[hub].quests.push_back(quest);
            all[hub].xp += questXp;  // [quest15]
        }

        for (Stop& s : best.added)
            all.push_back(std::move(s));
        order = std::move(best.order);
        xp += best.xp;
        slots -= best.takes;
        plan.takes += best.takes;
    }
    if (plan.takes)
    {
        Improve(legs, Pool{all, nullptr}, order, pinnedFirst);
        for (uint32 round = 0; round < MEASURE_ROUNDS && measureOrder(); ++round)
            Improve(legs, Pool{all, nullptr}, order, pinnedFirst);
    }

    plan.earlierHandIns = HandInsEarlier(legs, Pool{all, nullptr}, order, pinnedFirst, double(in.reach));

    uint32 prev = 0;
    // [quest132] the kills [quest131] already promised, spent as the ground sweep meets them
    std::map<uint32, uint32> groundNpcLeft = in.objectiveNpcLeft;
    for (size_t i : order)
    {
        Stop s = all[i];

        s.leg = legs.Seconds(prev, s.node);
        s.legYd = legs.Yards(prev, s.node);
        plan.eta += s.leg + s.work;
        plan.walkYd += s.legYd;
        prev = s.node;
        if (s.kind == StopKind::Ender && !s.ready)
            plan.openXp += s.xp;  // [help21]
        plan.killXp += s.killXp;  // [quest131] the kills its objectives need, worth their own experience
        plan.killCount += s.killCount;
        ++plan.totalLegs;
        // [quest132] what the ground of this leg is worth. A leg no corridor search answered is swept as its
        // straight line and counted, so the plan line can say how much of the term under-states the walk
        if (in.ground && prev != s.node)
        {
            plan.groundXp += in.ground(legs.At(prev), legs.At(s.node), groundNpcLeft, plan.groundCount);
            if (!legs.Searched(prev, s.node))
                ++plan.groundStraightLegs;
        }
        plan.stops.push_back(std::move(s));
    }
    plan.questXp = xp;
    plan.xp = xp + plan.killXp;
    plan.estimatedLegs = uint32(legs.EstimatedCount());
    plan.walkRatio = legs.WalkRatio();
    plan.fightPerYd = legs.FightPerYd();
    plan.speed = legs.Speed();
    plan.measured = uint32(legs.MeasuredCount());
    return plan;
}

// ---- the bot's route (NewRpgBaseAction) ----

// [routeplan] what makes a new build: the level (the offers, the work's prices) and the map; the rest of the log is
// compared stop by stop (RouteMissingStop, TrimRoute)
uint64 NewRpgBaseAction::RouteKey()
{
    // [quest23] ... and its landmass: a route built on Teldrassil holds no stop of Darkshore (same map 1), its
    // quests completed there were never routed to their enders (b372 Bierya: q1001 complete by Auberdine, its walk
    // went on 2000 yd south to q952's ender, "not in its 0 stops")
    return Mix(Mix(Mix(0, bot->GetLevel()), bot->GetMapId()), TravelRouteMgr::Landmass(WorldPosition(bot)));
}

// [quest123] the place a route belongs to, without the level: the map and the landmass. A plan is parked under it
// while the bot is away (a dungeon trip) and resumed on its return, so a crossing costs no rebuild
uint64 NewRpgBaseAction::RoutePlaceKey()
{
    return Mix(Mix(0, bot->GetMapId()), TravelRouteMgr::Landmass(WorldPosition(bot)));
}

void NewRpgBaseAction::InvalidateRoute(Player* bot, uint32 questId, std::string const& why)
{
    QuestRoute::BotRoute& r = QuestRoute::Of(bot->GetGUID().GetCounter());
    r.invalid = true;
    r.invalidWhy = "q" + std::to_string(questId) + " " + why;
}

namespace
{
    std::string RouteHead(Player* bot, char const* what)
    {
        return Acore::StringFormat("[route] {} L{} {} t={} at=({:.0f},{:.0f})", bot->GetName(), bot->GetLevel(),
                                   what, QuestStats::NowMs(), bot->GetPositionX(), bot->GetPositionY());
    }

    // [routeplan] actual: the seconds to the stop's quest event (accept, complete, turn-in) when it came, else to now;
    // late: from that event to this line (the bot ran other tasks before it asked its route again)
    void LogStopEnd(Player* bot, QuestRoute::BotRoute const& r, char const* what, uint32 handed,
                    std::string const& why)
    {
        uint64 const now = QuestStats::NowMs();
        uint64 const end = r.currentDoneMs && r.currentDoneMs <= now ? r.currentDoneMs : now;
        uint32 const actual = uint32((end - std::min(end, r.currentSinceMs)) / IN_MILLISECONDS);
        uint32 const late = uint32((now - end) / IN_MILLISECONDS);
        LOG_INFO("playerbots", "{} stop={} kind={} planned={} actual={} handed={} late={}{}", RouteHead(bot, what),
                 QuestRoute::Describe(r.current), QuestRoute::KindName(r.current.kind), uint32(r.currentPlanned),
                 actual, handed, late, why.empty() ? "" : " why=" + why);
    }

    // [stuck12] bot -> the town stops it left for a given-up walk (one line per stop and level)
    std::mutex townGivenUpLock;
    std::unordered_map<uint32, std::set<std::tuple<uint8, int32, int32>>> townGivenUp;

    bool TownListed(std::vector<std::pair<WorldPosition, std::string>> const& towns, float x, float y)
    {
        uint64 const key = QuestRoute::SpotKey(x, y);
        return std::any_of(towns.begin(), towns.end(), [&](auto const& t)
                           { return QuestRoute::SpotKey(t.first.GetPositionX(), t.first.GetPositionY()) == key; });
    }
}

void NewRpgBaseAction::NoteTownGivenUp(WorldPosition const& pos, std::string const& what)
{
    {
        std::lock_guard<std::mutex> guard(townGivenUpLock);
        if (!townGivenUp[bot->GetGUID().GetCounter()]
                 .insert({bot->GetLevel(), int32(pos.GetPositionX()), int32(pos.GetPositionY())})
                 .second)
            return;
    }
    LOG_INFO("playerbots", "{} town stop {} at ({:.0f},{:.0f}) {:.0f} yd left out: its walk there was given up at L{} "
             "(the global leader's task stays; another vendor / town is the leader's choice)",
             RouteHead(bot, "town-given-up"), what, pos.GetPositionX(), pos.GetPositionY(),
             bot->GetExactDist2d(pos.GetPositionX(), pos.GetPositionY()), bot->GetLevel());
}

// the stop it was on: done (logged with its planned and actual seconds), left (postponed, gone), or still open
bool NewRpgBaseAction::CloseRouteStop(QuestRoute::BotRoute& r,
                                      std::vector<std::pair<WorldPosition, std::string>> const& towns)
{
    if (!r.hasCurrent)
        return true;
    Stop const& s = r.current;
    uint32 done = 0, gone = 0, postponed = 0;
    for (uint32 q : s.quests)
    {
        QuestStatus const status = bot->GetQuestStatus(q);
        if (StopQuestDone(bot, s.kind, q))
            ++done;
        else if (botAI->lowPriorityQuest.count(q))
            ++postponed;
        else if (s.kind != StopKind::Giver && status == QUEST_STATUS_NONE)
            ++gone;
    }
    // a town stop: reached, or its task left the list (the guild's NextStep ran it there)
    if (s.kind == StopKind::Town && (!TownListed(towns, s.x, s.y) || bot->GetDistance2d(s.x, s.y) <= TOWN_STOP_REACHED))
    {
        LogStopEnd(bot, r, "done", 0, "");
        ++r.stopsDone;  // [quest122] reached: one more stop a plan of this bot gets through
    }
    else if (done && done + postponed + gone == s.quests.size())  // [routeplan] a hub, a giver: with the last
    {
        LogStopEnd(bot, r, "done", s.kind == StopKind::Ender ? done : 0, "");
        ++r.stopsDone;
        // [route23] a hand-in or a pickup done: its leg's planned seconds against the seconds to its quest event
        if ((s.kind == StopKind::Ender || s.kind == StopKind::Giver) && s.leg > 0.0f)
        {
            uint64 const now = QuestStats::NowMs();
            uint64 const end = r.currentDoneMs && r.currentDoneMs <= now ? r.currentDoneMs : now;
            r.legPlannedSec += s.leg;
            r.legActualSec += float(end - std::min(end, r.currentSinceMs)) / float(IN_MILLISECONDS);
            ++r.legStops;
        }
    }
    else if (!s.quests.empty() && postponed + gone == s.quests.size())
    {
        LogStopEnd(bot, r, "left", 0, postponed ? "postponed" : "left the log");
        ++r.stopsLeft;  // [quest122] lost: the plan paid for this stop and got nothing from it
    }
    else if (r.invalid)
    {
        LogStopEnd(bot, r, "left", 0, r.invalidWhy);
        ++r.stopsLeft;
    }
    else
        return false;
    r.hasCurrent = false;
    r.currentDoneMs = 0;
    return true;
}

// [routeplan] a stop the route lacks: a log quest open or done without its objective or hub stop (taken since the
// build, back from a postponement, its item lost), a left-out quest whose status moved, a new town task
std::string NewRpgBaseAction::RouteMissingStop(QuestRoute::BotRoute const& r,
                                               std::vector<std::pair<WorldPosition, std::string>> const& towns)
{
    auto const has = [&](StopKind kind, uint32 q)
    {
        for (Stop const& s : r.plan.stops)
            if (s.kind == kind && std::find(s.quests.begin(), s.quests.end(), q) != s.quests.end())
                return true;
        return false;
    };
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || botAI->lowPriorityQuest.count(questId))
            continue;
        QuestStatus const status = bot->GetQuestStatus(questId);
        if (status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE)
            continue;
        auto const left = r.leftOutQuests.find(questId);
        if (left != r.leftOutQuests.end())
        {
            if (left->second != uint8(status))
                return "q" + std::to_string(questId) + " left out, now " +
                       (status == QUEST_STATUS_COMPLETE ? "done" : "open");
            continue;
        }
        if (status == QUEST_STATUS_INCOMPLETE && !has(StopKind::Objective, questId))
            return "q" + std::to_string(questId) + " open, no objective stop";
        if (status == QUEST_STATUS_COMPLETE && !has(StopKind::Ender, questId))
            return "q" + std::to_string(questId) + " done, no hand-in stop";
    }
    for (auto const& [pos, what] : towns)
        if (!r.townsSeen.count(QuestRoute::SpotKey(pos.GetPositionX(), pos.GetPositionY())))
            return "town task " + what;
    return "";
}

// [routeplan] the stops done, postponed or dropped since the build are cut out of the order (the rest keeps its
// order: the route is not built again for them); a stop whose predecessor went gets its leg from the new one by the
// build's ratios. A hand-in that opens a follow-up the bot may take now: its why (the route is built again)
std::string NewRpgBaseAction::TrimRoute(QuestRoute::BotRoute& r,
                                        std::vector<std::pair<WorldPosition, std::string>> const& towns)
{
    std::set<uint32> giving;  // quests a giver stop of the route still offers
    for (Stop& s : r.plan.stops)
        if (s.kind == StopKind::Giver)
        {
            s.quests.erase(std::remove_if(s.quests.begin(), s.quests.end(),
                                          [&](uint32 q)
                                          {
                                              return bot->GetQuestStatus(q) != QUEST_STATUS_NONE ||
                                                     botAI->lowPriorityQuest.count(q);
                                          }),
                           s.quests.end());
            giving.insert(s.quests.begin(), s.quests.end());
        }
    std::vector<uint32> handedIn;
    std::string dropped;
    std::vector<Stop> kept;
    kept.reserve(r.plan.stops.size());
    bool predecessorGone = false;
    float px = bot->GetPositionX(), py = bot->GetPositionY();
    for (Stop& s : r.plan.stops)
    {
        bool keep = true;
        if (s.kind == StopKind::Town)
            keep = TownListed(towns, s.x, s.y);
        else
        {
            s.quests.erase(std::remove_if(s.quests.begin(), s.quests.end(),
                                          [&](uint32 q)
                                          {
                                              if (s.kind == StopKind::Giver)
                                                  return false;
                                              QuestStatus const status = bot->GetQuestStatus(q);
                                              if (s.kind == StopKind::Ender && status == QUEST_STATUS_REWARDED)
                                                  handedIn.push_back(q);
                                              return StopQuestDone(bot, s.kind, q) ||
                                                     botAI->lowPriorityQuest.count(q) ||
                                                     status == QUEST_STATUS_FAILED ||
                                                     (status == QUEST_STATUS_NONE && !giving.count(q));
                                          }),
                           s.quests.end());
            keep = !s.quests.empty();
        }
        if (!keep)
        {
            dropped += " " + QuestRoute::Signature(s);
            predecessorGone = true;
            continue;
        }
        if (predecessorGone)  // its leg now starts at the stop before it that stays (the bot's spot for the first)
            s.leg = QuestRoute::EstimateLeg(r.plan, px, py, s.x, s.y, &s.legYd);
        predecessorGone = false;
        px = s.x;
        py = s.y;
        kept.push_back(std::move(s));
    }
    bool const cut = kept.size() != r.plan.stops.size();
    r.plan.stops = std::move(kept);
    if (cut)
    {
        r.plan.eta = 0.0f;
        r.plan.walkYd = 0.0f;
        for (Stop const& s : r.plan.stops)
        {
            r.plan.eta += s.leg + s.work;
            r.plan.walkYd += s.legYd;
        }
        LOG_INFO("playerbots", "{} stops={} eta={} walk={} dropped={}", RouteHead(bot, "trim"),
                 r.plan.stops.size(), uint32(r.plan.eta), uint32(r.plan.walkYd), dropped.empty() ? " -" : dropped);
    }
    for (uint32 q : handedIn)
        for (uint32 next : UnlockedBy(q))
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(next);
            if (quest && bot->GetQuestStatus(next) == QUEST_STATUS_NONE && !botAI->lowPriorityQuest.count(next) &&
                bot->CanTakeQuest(quest, false))
                return "q" + std::to_string(q) + " handed in opens q" + std::to_string(next);
        }
    // [quest17] a quest complete since the build whose hub still waits for an open quest's objective: the route is
    // built again (the complete one gets its ready hub, HandInsEarlier weighs it). b357 Hobliir L13: q9641 complete
    // at +112 s in hub "E9641+9694", the hub waited for O9694 (399 s more) - 1092 s not handed in; Galiver q88 1153 s
    for (Stop const& s : r.plan.stops)
        if (s.kind == StopKind::Ender && !s.ready && s.quests.size() > 1)
        {
            uint32 done = 0;
            bool open = false;
            for (uint32 q : s.quests)
                if (bot->GetQuestStatus(q) == QUEST_STATUS_COMPLETE)
                    done = q;
                else
                    open = true;
            if (done && open)
                return "q" + std::to_string(done) + " complete, its hub waits for an open quest";
        }
    return "";
}


QuestRoute::BotRoute& NewRpgBaseAction::CurrentRoute()
{
    SlowScope slow("route build", &bot->GetName());  // [perf26]
    QuestRoute::BotRoute& r = QuestRoute::Of(bot->GetGUID().GetCounter());
    std::vector<std::pair<WorldPosition, std::string>> towns;
    sGuildCoordinator.TownStops(bot, towns);
    // [town12] the leader of a group walks its members' town stops too (econ b350: the mail pickups and smelts of
    // group followers - Lanbuzz, Manue - never ran: a member's own route is not run while it follows); the member runs
    // its task there itself (NewRpgStatusUpdateAction, TownTaskHere)
    memberHandInXp.clear();  // [quest16] this build's member hand-ins
    if (Group* group = bot->GetGroup(); group && group->GetLeaderGUID() == bot->GetGUID())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())

        {
            Player* member = ref->GetSource();
            if (!member || member == bot || member->GetMapId() != bot->GetMapId() || !GET_PLAYERBOT_AI(member))
                continue;
            // [town15] ... of a member with it (in its sight): the member runs its task only standing at the stop
            // (TownTaskHere), so a member away on its own walk gets nothing from the leader's walk there. b355
            // Gnetlibozz L11 walked 98 s to "BUY The Farstrider Lodge (for Kianey)" with Kianey 724 yd off on its
            // own route; [town12] never fired
            if (member->GetExactDist2d(bot) > sPlayerbotAIConfig.SightDistance)
                continue;
            std::vector<std::pair<WorldPosition, std::string>> theirs;
            sGuildCoordinator.TownStops(member, theirs);
            for (auto& [pos, what] : theirs)
                if (!TownListed(towns, pos.GetPositionX(), pos.GetPositionY()))
                    towns.emplace_back(pos, what + " (for " + member->GetName() + ")");
            // [quest16] coordinator decision 2026-11-09: its complete quests' enders are stops of the leader's route,
            // weighed as its own hand-ins (the experience each member hands in there); the member hands in at the
            // ender (MemberHandInHere). One its leader has complete too is the leader's own ender stop already
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 const questId = member->GetQuestSlotQuestId(slot);
                Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
                WorldPosition ender;
                if (!quest || member->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE ||
                    bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE ||
                    !NearestEnderSpot(member, questId, ender) || ender.GetMapId() != bot->GetMapId())
                    continue;
                memberHandInXp[QuestRoute::SpotKey(ender.GetPositionX(), ender.GetPositionY())] +=
                    float(quest->XPValue(member->GetLevel()));
                if (!TownListed(towns, ender.GetPositionX(), ender.GetPositionY()))
                    towns.emplace_back(ender, Acore::StringFormat("HAND_IN q{} (for {})", questId, member->GetName()));
            }
        }
    // [stuck12] a town stop whose walk the bot gave up at this level (MOVE_FAR_LOOP, the maze search: every way there
    // passes mobs it does not pull) is no stop of its route: TrimRoute drops it as a task gone. Before, the stop stayed
    // first in the route and IDLE walked to it again on every look (b350 Baridrol L10: BUY at Ratchet 3567 yd off
    // through Ashenvale's L23-26 bears, 161 TOWN -> GIVE_UP rounds, 51 STUCK alerts; Blahadrin L13 BUY Stormwind 447)
    towns.erase(std::remove_if(towns.begin(), towns.end(),
                               [this](auto const& t)
                               {
                                   if (!WalkGivenUp(bot, t.first.GetPositionX(), t.first.GetPositionY()))
                                       return false;
                                   NoteTownGivenUp(t.first, t.second);
                                   return true;
                               }),
                towns.end());
    uint64 townKey = 0;
    for (auto const& [pos, what] : towns)
        townKey = Mix(townKey, QuestRoute::SpotKey(pos.GetPositionX(), pos.GetPositionY()));
    if (townKey != r.townKey)
    {
        r.townKey = townKey;
        r.visitedTown.clear();
    }
    uint32 const now = uint32(time(nullptr));
    // [routeplan] the stop it is on closes at once (its done line), then the order loses what is done; a new build
    // only for what the order cannot hold without one
    CloseRouteStop(r, towns);
    std::string why;
    if (!r.built)
        why = "first";
    else if (r.invalid)
        why = r.invalidWhy;
    else if (r.key != RouteKey())
    {
        // [quest123] the plan it had here is parked under its place, and a plan parked for the place it now stands
        // in is resumed instead of built again (then trimmed like any other, so stops done or gone since are cut)
        if (r.placeKey)
            r.parked[r.placeKey] = {r.plan, r.key, r.respawnDue, r.leftOutQuests, r.townsSeen};
        auto const back = r.parked.find(RoutePlaceKey());
        if (back != r.parked.end() && back->second.builtKey == RouteKey())
        {
            r.plan = back->second.plan;
            r.respawnDue = back->second.respawnDue;
            r.leftOutQuests = back->second.leftOutQuests;
            r.townsSeen = back->second.townsSeen;
            r.key = RouteKey();
            r.placeKey = RoutePlaceKey();
            r.parked.erase(back);
            LOG_INFO("playerbots", "[quest123] {} L{} resumes the {} stop plan it left on map {}: no rebuild",
                     bot->GetName(), bot->GetLevel(), r.plan.stops.size(), bot->GetMapId());
            why = TrimRoute(r, towns);
            if (why.empty())
                why = RouteMissingStop(r, towns);
        }
        else
            why = "the level or map changed";
    }
    else if (r.respawnDue && now >= r.respawnDue)
        why = "a left-out quest's camp respawned";
    else
    {
        why = TrimRoute(r, towns);
        if (why.empty())
            why = RouteMissingStop(r, towns);
    }
    // [lag340] coordinator 2026-11-11: a town stop on another landmass is no stop of the landmass's route (its legs
    // are walks) - b445: Dekorhan's Virtuoso Inking Set in Darnassus, across the water from Darkshore, ran only when he
    // happened to stand in a town. Such a stop with a route there (boat / portal, TravelRouteMgr::FindRoute) is taken
    // when its tasks' worth (their game prices, [lag337]) beats what questing gives in the trip's time there (the walk,
    // the waits and rides of its route; [econ1] its quest copper per minute); the best of them by worth per second
    if (r.crossTown != WorldPosition() && !TownListed(towns, r.crossTown.GetPositionX(), r.crossTown.GetPositionY()))
    {
        r.crossTown = WorldPosition();
        r.crossWhat.clear();
    }
    if (!why.empty() || r.crossKey != townKey)
    {
        r.crossKey = townKey;
        uint32 const own = TravelRouteMgr::Landmass(WorldPosition(bot));
        float const questPerMin = sGuildCoordinator.QuestCopperPerMinute(bot);
        double bestRate = 0.0;
        WorldPosition bestPos;
        std::string bestWhat, bestLine;
        for (auto const& [pos, what] : towns)
        {
            if (pos.GetMapId() != bot->GetMapId() || TravelRouteMgr::Landmass(pos) == own)
                continue;
            TravelRouteMgr::Leg leg;
            float seconds = 0.0f;
            if (!sTravelRouteMgr.FindRoute(bot, pos, leg, seconds) || seconds <= 0.0f)
                continue;
            double const worth = sGuildCoordinator.TownStopCopper(bot, pos);
            double const questing = double(questPerMin) * double(seconds) / 60.0;
            if (worth <= questing || worth / seconds <= bestRate)
                continue;
            bestRate = worth / seconds;
            bestPos = pos;
            bestWhat = what;
            bestLine = Acore::StringFormat("worth {:.0f}c against {:.0f}c of questing in its {:.0f} s trip", worth,
                                           questing, seconds);
        }
        if (bestPos != r.crossTown)
        {
            if (bestPos != WorldPosition())
                LOG_INFO("playerbots", "[lag340] {} L{} takes the town stop {} on another landmass: {}",
                         bot->GetName(), bot->GetLevel(), bestWhat, bestLine);
            r.crossTown = bestPos;
            r.crossWhat = bestWhat;
        }
    }
    // [quest122] a new town task is no reason to throw a whole plan away while its stop is still open. b475: 89 of
    // the 138 rebuilds that were not a first build came from a town task, and a plan replaced under an open stop
    // lost it (11 of 25 hand-in stops collected nothing, 5 of them "replanned"). The task waits for a town visit
    // either way and an urgent one comes as an order, so it joins the route at the next build - the stop closing,
    // a quest opened or done, a level or map change, a trim or a camp respawn
    if (r.hasCurrent && why.rfind("town task", 0) == 0)
        why.clear();
    if (!why.empty())
    {
        CloseRouteStop(r, towns);
        QuestRoute::Input in;
        CollectRouteStops(in, towns, r.visitedTown);
        // [quest122] [quest124] the chance this bot reaches one more place of a plan, from its own plans: the mean
        // stops a plan of its reaches gives the geometric rate p = mean / (1 + mean). It is recomputed at every
        // build from counts that keep growing, so it rises as the plans start being walked - nothing is frozen.
        // With nothing measured yet it is 1.0 and the solver weighs exactly as it did before
        float const meanReached = float(r.stopsDone) / float(std::max<uint32>(r.builds, 1));
        float const reach = r.builds && r.stopsDone ? meanReached / (1.0f + meanReached) : 1.0f;
        // [quest130] b478 measured: of 131 plans, 104 carried reach=1.00 (nothing measured yet, no weighting) and
        // the 27 that were weighed came from samples like plans=3 reached=1, where a plan's expected experience fell
        // from ~16000 to 22. The denominator is rebuilds, which [quest123] deliberately prevents, and the counts die
        // with the session - so the measure cannot converge, and the guild split into 79 % planning as before and
        // 21 % with their plans priced at nothing. It is also a rate measured from history deciding a bot's route,
        // which the user's rule reserves for formulas. So it does not price anything: it is logged (would_reach) and
        // the weighting runs at 1.0, which is the pricing the solver had before [quest124], until an honest plan
        // estimate can drive it. The weighting itself stays - the fiction it was meant to discount is plan.xp / eta,
        // 201 XP/min promised in b478 against 27 XP/min the bots actually earned
        in.reach = 1.0f;
        r.plan = QuestRoute::Solve(in);
        PlaceGreyHandIns(r.plan, in);  // [quest14]
        r.key = RouteKey();
        r.placeKey = RoutePlaceKey();
        r.built = true;
        r.invalid = false;
        r.respawnDue = in.respawnDue;
        r.leftOutQuests = std::move(in.leftOutQuests);
        r.townsSeen = std::move(in.townsSeen);
        ++r.builds;
        // [quest122] what the plan is worth once that chance is priced in: stop k carries p^(k+1) of its experience,
        // so a 17-stop hour-long plan scores near its first stops alone and a short one the bot finishes scores
        // near all of it
        float expectedXp = 0.0f;
        float share = reach;
        for (Stop const& s : r.plan.stops)
        {
            expectedXp += s.xp * share;
            share *= reach;
        }
        std::string stops;
        for (Stop const& s : r.plan.stops)
            stops += " " + QuestRoute::Describe(s);
        size_t const logQuests = std::count_if(in.stops.begin(), in.stops.end(),
                                               [](Stop const& s) { return s.kind == StopKind::Objective; });
        LOG_INFO("playerbots",
                 "{} stops={} eta={} walk={} xp={} open={} takes={} left_out={} offers={} legs={} walk_ratio={:.2f} "
                 "fight_s_per_100yd={:.1f} handins_sooner={} would_reach={:.2f} exp_xp={} plans={} reached={} "
                 "lost={} xp_quest={} xp_kill={} kills={} xp_ground={} ground_mobs={} legs_ground_straight={}/{} "
                 "legs_straight={}/{} why={} |{}",
                 // [quest15] handins_sooner, [quest122] [quest130] would_reach / exp_xp, [quest131] the two terms
                 // of the promise apart (a 20x must not hide inside one number) and the legs it under-states
                 RouteHead(bot, "plan"), r.plan.stops.size(), uint32(r.plan.eta), uint32(r.plan.walkYd),
                 uint32(r.plan.xp), logQuests, r.plan.takes, in.leftOut, in.offers.size(), r.plan.measured,
                 r.plan.walkRatio, r.plan.fightPerYd * 100.0f, r.plan.earlierHandIns, reach, uint32(expectedXp),
                 r.builds, r.stopsDone, r.stopsLeft, uint32(r.plan.questXp), uint32(r.plan.killXp),
                 r.plan.killCount, uint32(r.plan.groundXp), r.plan.groundCount, r.plan.groundStraightLegs,
                 r.plan.totalLegs, r.plan.estimatedLegs, r.plan.totalLegs, why, stops);
        // [walk68] the plan's next stop is a walk priced before it is walked (TripRefused: an orange / red or elite
        // roamer's round at the stop, a walk through a reach it must never fight - the route search with the static
        // spawns beyond sight; its answer kept per stop and level): a refused one is postponed now and the route built
        // again, not found refused after the walk. b406: 19 objective stops (6809 s) ended "every way there passes
        // mobs it does not pull", 40 giver / hand-in stops (15.4k s) left postponed, Orenem L15's 4211 yd giver walk
        // closed a MOVE_FAR_LOOP after 499 s
        if (SoloSafety::Applies(bot) && !r.plan.stops.empty())
        {
            Stop const& next = r.plan.stops.front();
            bool const priced =
                next.kind == StopKind::Objective || next.kind == StopKind::Giver || next.kind == StopKind::Ender;
            float z = next.z;
            if (priced && (z == INVALID_HEIGHT || z == VMAP_INVALID_HEIGHT_VALUE))
                z = std::max(bot->GetMap()->GetHeight(next.x, next.y, MAX_HEIGHT),
                             bot->GetMap()->GetWaterLevel(next.x, next.y));
            // [walk68] b410: TripRefused (built for town trips: DangerOnWay on the straight stages between travel nodes
            // of a far walk) postponed 161 stops in 20 min - givers and objectives a median 5.4 km off that the live
            // walk reaches round those mobs; quests per hour fell 113 -> 59. The stop is refused only by the walk's own
            // verdict, the one MoveFarTo acts on at once: a walk given up at this level, or the route search's refusal
            // of every way (an area red for it, an enemy capital, static spawns it must not fight with no way round)
            bool refused = false;
            if (priced && !next.quests.empty() && z != INVALID_HEIGHT && z != VMAP_INVALID_HEIGHT_VALUE &&
                bot->GetExactDist2d(next.x, next.y) > sPlayerbotAIConfig.SightDistance)
            {
                refused = WalkGivenUp(bot, next.x, next.y);
                if (!refused)
                {
                    BotPath const walk = FindBotPath(bot, next.x, next.y, z);
                    refused = walk.leaderRoute && walk.refusedArea;
                }
            }
            if (refused)
            {
                std::string const kind = next.kind == StopKind::Objective ? "objective"
                                         : next.kind == StopKind::Giver   ? "giver"
                                                                          : "hand-in";
                std::string const reason = "[walk68] its walk to the " + kind + " stop is refused at planning";
                std::string quests;
                for (uint32 q : next.quests)
                {
                    botAI->lowPriorityQuest.insert(q);
                    NotePostpone(bot, q, reason);
                    quests += " q" + std::to_string(q);
                }
                LOG_INFO("playerbots", "[walk68] {} L{} postpones{}: its walk to the {} stop at ({:.0f},{:.0f}) "
                         "{:.0f} yd is refused (given up, or every way refused) - the route is built again",
                         bot->GetName(), bot->GetLevel(), quests, kind, next.x, next.y,
                         bot->GetExactDist2d(next.x, next.y));
                r.invalid = true;
                r.invalidWhy = reason;
            }
        }
    }
    // a town stop it stands at is visited: the guild's NextStep runs its task there
    while (!r.plan.stops.empty() && r.plan.stops.front().kind == StopKind::Town &&
           bot->GetDistance2d(r.plan.stops.front().x, r.plan.stops.front().y) <= TOWN_STOP_REACHED)
    {
        Stop const& town = r.plan.stops.front();
        r.visitedTown.insert(QuestRoute::SpotKey(town.x, town.y));
        if (r.hasCurrent && r.currentSig == QuestRoute::Signature(town))
        {
            LogStopEnd(bot, r, "done", 0, "");
            r.hasCurrent = false;
            r.currentDoneMs = 0;
        }
        r.plan.stops.erase(r.plan.stops.begin());
    }
    if (r.plan.stops.empty())
        return r;
    Stop const& next = r.plan.stops.front();
    std::string const sig = QuestRoute::Signature(next);
    // [routeplan] the same stop with fewer quests left (a hub after its first hand-in, a giver after a take) stays
    // the stop it is on
    auto const sameStop = [&]()
    {
        if (next.kind != r.current.kind || QuestRoute::SpotKey(next.x, next.y) != QuestRoute::SpotKey(r.current.x,
                                                                                                     r.current.y))
            return false;
        for (uint32 q : next.quests)
            if (std::find(r.current.quests.begin(), r.current.quests.end(), q) == r.current.quests.end())
                return false;
        return true;
    };
    if (r.hasCurrent && (r.currentSig == sig || sameStop()))
        return r;
    if (r.hasCurrent)  // still open, the new route goes elsewhere first
    {
        LogStopEnd(bot, r, "left", 0, "replanned: " + (why.empty() ? std::string("the next stop changed") : why));
        ++r.stopsLeft;  // [quest122] a plan replaced while its stop was still open: the stop is lost
    }
    r.hasCurrent = true;
    r.current = next;
    r.currentSig = sig;
    r.currentSinceMs = QuestStats::NowMs();
    r.currentDoneMs = 0;
    // [routeplan] the plan's own seconds for it: its leg (the real walk and the fights on it) and its work
    r.currentPlanned = next.leg + next.work;
    return r;
}
// [quest14] user rule 2026-11-08: a complete grey quest nobody needs is handed in only where the bot passes its
// ender anyway. The detour of its ender at its cheapest place in the solved order (the straight legs at the plan's
// measured walk ratio): within one hand-in visit (the hub radius, enders that close are one visit) it joins the plan
// (the hub there, else its own stop), else it leaves the log. Its XP counts toward no rate
void NewRpgBaseAction::PlaceGreyHandIns(QuestRoute::Plan& plan, QuestRoute::Input const& in)
{
    for (Stop const& hand : in.greyHandIns)
    {
        if (hand.quests.empty())
            continue;
        uint32 const questId = hand.quests.front();
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            continue;
        auto const dist = [](float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); };
        float best = std::numeric_limits<float>::max();
        size_t at = 0;
        for (size_t i = 0; i <= plan.stops.size(); ++i)
        {
            float const ax = i ? plan.stops[i - 1].x : in.x;
            float const ay = i ? plan.stops[i - 1].y : in.y;
            float detour = dist(ax, ay, hand.x, hand.y);
            if (i < plan.stops.size())
                detour += dist(hand.x, hand.y, plan.stops[i].x, plan.stops[i].y) -
                          dist(ax, ay, plan.stops[i].x, plan.stops[i].y);
            if (detour < best)
            {
                best = detour;
                at = i;
            }
        }
        float const detourYd = std::max(0.0f, best) * plan.walkRatio;
        if (detourYd > in.hubRadius)
        {
            uint8 const slot = uint8(bot->FindQuestSlot(questId));
            if (slot < MAX_QUEST_LOG_SIZE)
                AbandonGreyQuest(slot, quest,
                                 Acore::StringFormat("complete, not needed, the route's detour to its ender {:.0f} "
                                                     "yd > {:.0f} yd (one hand-in visit)",
                                                     detourYd, in.hubRadius));
            continue;
        }
        GuildCoordinator::LogGrey(bot, "keeps", quest,
                                  Acore::StringFormat("complete, the route passes its ender (detour {:.0f} yd <= "
                                                      "{:.0f} yd, one hand-in visit)",
                                                      detourYd, in.hubRadius));
        auto const hub = std::find_if(plan.stops.begin(), plan.stops.end(),
                                      [&](Stop const& s) {
                                          return s.kind == StopKind::Ender &&
                                                 dist(s.x, s.y, hand.x, hand.y) <= in.hubRadius;
                                      });
        if (hub != plan.stops.end())
        {
            hub->quests.push_back(questId);
            continue;
        }
        Stop stop = hand;
        float const ax = at ? plan.stops[at - 1].x : in.x;
        float const ay = at ? plan.stops[at - 1].y : in.y;
        stop.leg = QuestRoute::EstimateLeg(plan, ax, ay, stop.x, stop.y, &stop.legYd);
        float added = stop.leg, addedYd = stop.legYd;
        if (at < plan.stops.size())
        {
            Stop& after = plan.stops[at];
            added -= after.leg;
            addedYd -= after.legYd;
            after.leg = QuestRoute::EstimateLeg(plan, stop.x, stop.y, after.x, after.y, &after.legYd);
            added += after.leg;
            addedYd += after.legYd;
        }
        plan.eta += std::max(0.0f, added);
        plan.walkYd += std::max(0.0f, addedYd);
        plan.stops.insert(plan.stops.begin() + at, stop);
    }
}

uint32 NewRpgBaseAction::ChooseBestQuest(WorldPosition& giverPos)
{
    giverPos = WorldPosition();
    QuestRoute::BotRoute& r = CurrentRoute();
    for (size_t i = 0; i < r.plan.stops.size(); ++i)
    {
        Stop const& s = r.plan.stops[i];
        uint32 chosen = 0;
        for (uint32 q : s.quests)
        {
            QuestStatus const status = bot->GetQuestStatus(q);
            if ((s.kind == StopKind::Objective && status == QUEST_STATUS_INCOMPLETE) ||
                (s.kind == StopKind::Ender && status == QUEST_STATUS_COMPLETE) ||
                (s.kind == StopKind::Giver && status == QUEST_STATUS_NONE && !botAI->lowPriorityQuest.count(q)))
            {
                chosen = q;
                break;
            }
        }
        if (!chosen)
            continue;  // a town stop (IDLE walks there first), or one already done
        if (s.kind == StopKind::Giver)
            giverPos = WorldPosition(bot->GetMapId(), s.x, s.y, s.z);
        NoteRouteChoice(bot, chosen,
                        "route stop " + std::to_string(i + 1) + "/" + std::to_string(r.plan.stops.size()) + " " +
                            QuestRoute::Describe(s) + ", route " + std::to_string(uint32(r.plan.eta)) + " s");
        return chosen;
    }
    return ChooseQuestByRate(giverPos);
}

bool NewRpgBaseAction::RouteTownStop(WorldPosition& pos, std::string& what)
{
    QuestRoute::BotRoute& r = CurrentRoute();
    // [lag340] a town stop on another landmass worth its trip goes first: the walk there takes its boat / portal
    // (MoveFarTo's trip), the guild's NextStep runs the tasks on arrival
    if (r.crossTown != WorldPosition() && r.crossTown.GetMapId() == bot->GetMapId())
    {
        pos = r.crossTown;
        what = r.crossWhat + " [lag340]";
        return true;
    }
    if (r.plan.stops.empty() || r.plan.stops.front().kind != StopKind::Town)
        return false;
    Stop const& town = r.plan.stops.front();
    pos = WorldPosition(bot->GetMapId(), town.x, town.y, town.z);
    what = town.what;
    return true;
}

bool NewRpgBaseAction::RouteHandsInNow(uint32 questId)
{
    QuestRoute::BotRoute& r = CurrentRoute();
    bool routed = false;
    for (Stop const& s : r.plan.stops)
        if (s.kind == StopKind::Ender && std::find(s.quests.begin(), s.quests.end(), questId) != s.quests.end())
            routed = true;
    if (!routed)
        return true;  // its ender is off the route (another landmass): the walk there as before
    Stop const& next = r.plan.stops.front();
    return next.kind == StopKind::Ender &&
           std::find(next.quests.begin(), next.quests.end(), questId) != next.quests.end();
}
