/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgQuestRoute.h"

#include "GuildCoordinator.h"
#include "Log.h"
#include "NewRpgBaseAction.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "QuestStats.h"
#include "StringFormat.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <limits>
#include <mutex>
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
        std::vector<std::pair<size_t, uint32>> merges;  // an existing hub (index) hands in this quest too
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
                    if (hub.kind == StopKind::Ender &&
                        Dist(hub.x, hub.y, take->ender.x, take->ender.y) <= in.hubRadius)
                    {
                        if (t.order[k] < all.size())
                            t.merges.emplace_back(t.order[k], take->questId);
                        else
                            t.added[t.order[k] - all.size()].quests.push_back(take->questId);
                        merged = true;
                    }
                }
                if (!merged)
                {
                    t.added.push_back(take->ender);
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
    return found != known_.end() ? found->second.walkYd : Straight(a, b) * WalkRatio();
}

float QuestRoute::LegTable::Seconds(uint32 a, uint32 b) const
{
    if (a == b)
        return 0.0f;
    auto const found = known_.find(Key(a, b));
    if (found != known_.end())
        return found->second.walkYd / speed_ + found->second.fightSec;
    float const straight = Straight(a, b);
    return straight * WalkRatio() / speed_ + straight * FightPerYd();
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
            // no corridor (no polygon at an end): the straight line as the build's other legs show it
            fact.walkYd = straight * WalkRatio();
            fact.fightSec = straight * FightPerYd();
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
    float const yd = straight * plan.walkRatio;
    if (walkYd)
        *walkYd = yd;
    return yd / std::max(plan.speed, 1.0f) + straight * plan.fightPerYd;
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
            float const ratio = trial.xp / std::max(added, SECONDS_EPSILON);
            if (added <= trial.alone + SECONDS_EPSILON && ratio > bestRatio)
            {
                bestRatio = ratio;
                bestOffer = o;
                best = std::move(trial);
            }
        }
        if (bestOffer == offers.size())
            break;
        used[bestOffer] = true;
        for (auto const& [hub, quest] : best.merges)
            all[hub].quests.push_back(quest);
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

    uint32 prev = 0;
    for (size_t i : order)
    {
        Stop s = all[i];
        s.leg = legs.Seconds(prev, s.node);
        s.legYd = legs.Yards(prev, s.node);
        plan.eta += s.leg + s.work;
        plan.walkYd += s.legYd;
        prev = s.node;
        plan.stops.push_back(std::move(s));
    }
    plan.xp = xp;
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
    return Mix(Mix(0, bot->GetLevel()), bot->GetMapId());
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

    bool TownListed(std::vector<std::pair<WorldPosition, std::string>> const& towns, float x, float y)
    {
        uint64 const key = QuestRoute::SpotKey(x, y);
        return std::any_of(towns.begin(), towns.end(), [&](auto const& t)
                           { return QuestRoute::SpotKey(t.first.GetPositionX(), t.first.GetPositionY()) == key; });
    }
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
        LogStopEnd(bot, r, "done", 0, "");
    else if (done && done + postponed + gone == s.quests.size())  // [routeplan] a hub, a giver: with the last
        LogStopEnd(bot, r, "done", s.kind == StopKind::Ender ? done : 0, "");
    else if (!s.quests.empty() && postponed + gone == s.quests.size())
        LogStopEnd(bot, r, "left", 0, postponed ? "postponed" : "left the log");
    else if (r.invalid)
        LogStopEnd(bot, r, "left", 0, r.invalidWhy);
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
    return "";
}

QuestRoute::BotRoute& NewRpgBaseAction::CurrentRoute()
{
    QuestRoute::BotRoute& r = QuestRoute::Of(bot->GetGUID().GetCounter());
    std::vector<std::pair<WorldPosition, std::string>> towns;
    sGuildCoordinator.TownStops(bot, towns);
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
        why = "the level or map changed";
    else if (r.respawnDue && now >= r.respawnDue)
        why = "a left-out quest's camp respawned";
    else
    {
        why = TrimRoute(r, towns);
        if (why.empty())
            why = RouteMissingStop(r, towns);
    }
    if (!why.empty())
    {
        CloseRouteStop(r, towns);
        QuestRoute::Input in;
        CollectRouteStops(in, towns, r.visitedTown);
        r.plan = QuestRoute::Solve(in);
        r.key = RouteKey();
        r.built = true;
        r.invalid = false;
        r.respawnDue = in.respawnDue;
        r.leftOutQuests = std::move(in.leftOutQuests);
        r.townsSeen = std::move(in.townsSeen);
        std::string stops;
        for (Stop const& s : r.plan.stops)
            stops += " " + QuestRoute::Describe(s);
        size_t const logQuests = std::count_if(in.stops.begin(), in.stops.end(),
                                               [](Stop const& s) { return s.kind == StopKind::Objective; });
        LOG_INFO("playerbots",
                 "{} stops={} eta={} walk={} xp={} open={} takes={} left_out={} offers={} legs={} walk_ratio={:.2f} "
                 "fight_s_per_100yd={:.1f} why={} |{}",
                 RouteHead(bot, "plan"), r.plan.stops.size(), uint32(r.plan.eta), uint32(r.plan.walkYd),
                 uint32(r.plan.xp), logQuests, r.plan.takes, in.leftOut, in.offers.size(), r.plan.measured,
                 r.plan.walkRatio, r.plan.fightPerYd * 100.0f, why, stops);
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
        LogStopEnd(bot, r, "left", 0, "replanned: " + (why.empty() ? std::string("the next stop changed") : why));
    r.hasCurrent = true;
    r.current = next;
    r.currentSig = sig;
    r.currentSinceMs = QuestStats::NowMs();
    r.currentDoneMs = 0;
    // [routeplan] the plan's own seconds for it: its leg (the real walk and the fights on it) and its work
    r.currentPlanned = next.leg + next.work;
    return r;
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
