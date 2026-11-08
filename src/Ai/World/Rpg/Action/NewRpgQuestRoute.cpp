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

    float Dist(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

    // the stops of `all` (+ `added` past its end) by index
    struct Pool
    {
        std::vector<Stop> const& all;
        std::vector<Stop> const* added;
        Stop const& operator()(size_t i) const { return i < all.size() ? all[i] : (*added)[i - all.size()]; }
    };

    float OrderSeconds(float x, float y, float speed, Pool const& at, std::vector<size_t> const& order,
                       float* walkYd = nullptr)
    {
        float yd = 0.0f, work = 0.0f;
        float px = x, py = y;
        for (size_t i : order)
        {
            Stop const& s = at(i);
            yd += Dist(px, py, s.x, s.y);
            work += s.work;
            px = s.x;
            py = s.y;
        }
        if (walkYd)
            *walkYd = yd;
        return yd / speed + work;
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

    // nearest feasible neighbour from (x, y); `pinned` (an index, or SIZE_MAX) first
    std::vector<size_t> NearestNeighbour(float x, float y, std::vector<Stop> const& all, size_t pinned)
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
        float px = x, py = y;
        auto const place = [&](size_t i)
        {
            placed[i] = true;
            order.push_back(i);
            px = all[i].x;
            py = all[i].y;
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
            float bestYd = std::numeric_limits<float>::max();
            for (size_t i = 0; i < all.size(); ++i)
                if (!placed[i] && ready(i) && Dist(px, py, all[i].x, all[i].y) < bestYd)
                {
                    best = i;
                    bestYd = Dist(px, py, all[i].x, all[i].y);
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
    void Improve(float x, float y, float speed, Pool const& at, std::vector<size_t>& order, bool pinnedFirst)
    {
        size_t const n = order.size();
        size_t const from = pinnedFirst ? 1 : 0;
        if (n < from + 2)
            return;
        float best = OrderSeconds(x, y, speed, at, order);
        for (size_t pass = 0; pass < n; ++pass)  // each pass shortens the route or ends the search
        {
            bool improved = false;
            for (size_t i = from; i + 1 < n; ++i)
                for (size_t j = i + 1; j < n; ++j)
                {
                    std::reverse(order.begin() + i, order.begin() + j + 1);
                    float const cost = OrderSeconds(x, y, speed, at, order);
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
                    float const cost = OrderSeconds(x, y, speed, at, order);
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

    // the cheapest place (>= minPos) for a stop at (sx, sy) in the order: its position and the yards it adds
    size_t CheapestInsert(float x, float y, Pool const& at, std::vector<size_t> const& order, float sx, float sy,
                          size_t minPos, float& addedYd)
    {
        size_t bestPos = order.size();
        addedYd = std::numeric_limits<float>::max();
        for (size_t k = std::min(minPos, order.size()); k <= order.size(); ++k)
        {
            float px = x, py = y;
            if (k > 0)
            {
                px = at(order[k - 1]).x;
                py = at(order[k - 1]).y;
            }
            float delta = Dist(px, py, sx, sy);
            if (k < order.size())
            {
                Stop const& next = at(order[k]);
                delta += Dist(sx, sy, next.x, next.y) - Dist(px, py, next.x, next.y);
            }
            if (delta < addedYd)
            {
                addedYd = delta;
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
    };

    // the offer's giver, then each take's objective after it and its ender after that (an existing hub later in the
    // route when its ender stands there) - each at its cheapest place
    Trial TryOffer(QuestRoute::Input const& in, std::vector<Stop> const& all, std::vector<size_t> const& order,
                   QuestRoute::Offer const& offer, uint32 slots, bool pinnedFirst)
    {
        Trial t;
        t.order = order;
        t.added.reserve(1 + offer.takes.size() * 2);
        Pool const at{all, &t.added};
        auto const posOf = [&](size_t index)
        { return size_t(std::find(t.order.begin(), t.order.end(), index) - t.order.begin()); };
        float yd = 0.0f;
        t.added.push_back(offer.giver);
        t.added.back().quests.clear();
        size_t const giverIndex = all.size();
        size_t const giverPos =
            CheapestInsert(in.x, in.y, at, t.order, offer.giver.x, offer.giver.y, pinnedFirst ? 1 : 0, yd);
        t.order.insert(t.order.begin() + giverPos, giverIndex);
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
            size_t const objectivePos = CheapestInsert(in.x, in.y, at, t.order, take->objective.x, take->objective.y,
                                                       posOf(giverIndex) + 1, yd);
            t.order.insert(t.order.begin() + objectivePos, objectiveIndex);
            if (take->hasEnder)
            {
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
                    size_t const enderPos = CheapestInsert(in.x, in.y, at, t.order, take->ender.x, take->ender.y,
                                                           posOf(objectiveIndex) + 1, yd);
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
}

QuestRoute::BotRoute& QuestRoute::Of(uint32 botLow)
{
    std::lock_guard<std::mutex> guard(routesLock);
    return routes[botLow];  // references stay valid across rehashes; entries are never erased
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

float QuestRoute::Seconds(float x, float y, float speed, std::vector<Stop> const& order, float* walkYd)
{
    std::vector<size_t> indexes(order.size());
    for (size_t i = 0; i < order.size(); ++i)
        indexes[i] = i;
    return OrderSeconds(x, y, speed, Pool{order, nullptr}, indexes, walkYd);
}

QuestRoute::Plan QuestRoute::Solve(Input const& in)
{
    std::vector<Stop> all = in.stops;
    size_t pinned = std::numeric_limits<size_t>::max();
    if (in.pinnedQuest)
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i].kind == StopKind::Objective && !all[i].quests.empty() && all[i].quests[0] == in.pinnedQuest)
                pinned = i;
    bool const pinnedFirst = pinned < all.size();
    std::vector<size_t> order = NearestNeighbour(in.x, in.y, all, pinned);
    Improve(in.x, in.y, in.speed, Pool{all, nullptr}, order, pinnedFirst);

    // givers on the way: the one whose quests add the most experience per second they add, while that beats the
    // route's own experience per second (a quest is taken when it raises the route's rate)
    Plan plan;
    float xp = in.logXp;
    uint32 slots = in.freeSlots;
    std::vector<bool> used(in.offers.size(), false);
    while (slots)
    {
        float const seconds = OrderSeconds(in.x, in.y, in.speed, Pool{all, nullptr}, order);
        float const rate = order.empty() ? 0.0f : xp / std::max(seconds, 1.0f);
        float bestRatio = -1.0f;
        size_t bestOffer = in.offers.size();
        Trial best;
        for (size_t o = 0; o < in.offers.size(); ++o)
        {
            if (used[o])
                continue;
            Trial trial = TryOffer(in, all, order, in.offers[o], slots, pinnedFirst);
            if (!trial.takes || trial.xp <= 0.0f)
                continue;
            float const added =
                OrderSeconds(in.x, in.y, in.speed, Pool{all, &trial.added}, trial.order) - seconds;
            float const ratio = trial.xp / std::max(added, SECONDS_EPSILON);
            if (ratio >= rate && ratio > bestRatio)
            {
                bestRatio = ratio;
                bestOffer = o;
                best = std::move(trial);
            }
        }
        if (bestOffer == in.offers.size())
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
        Improve(in.x, in.y, in.speed, Pool{all, nullptr}, order, pinnedFirst);

    for (size_t i : order)
        plan.stops.push_back(all[i]);
    plan.eta = Seconds(in.x, in.y, in.speed, plan.stops, &plan.walkYd);
    plan.xp = xp;
    return plan;
}

// ---- the bot's route (NewRpgBaseAction) ----

uint64 NewRpgBaseAction::RouteKey(uint64 townKey)
{
    uint64 h = Mix(0, bot->GetLevel());
    h = Mix(h, bot->GetMapId());
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (uint32 const questId = bot->GetQuestSlotQuestId(slot))
            h = Mix(h, (uint64(questId) << 8) | uint64(bot->GetQuestStatus(questId)));
    uint64 postponed = 0;
    for (uint32 q : botAI->lowPriorityQuest)
        postponed += q;
    h = Mix(h, botAI->lowPriorityQuest.size());
    h = Mix(h, postponed);
    h = Mix(h, bot->getRewardedQuests().size());
    return Mix(h, townKey);
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

    void LogStopEnd(Player* bot, QuestRoute::BotRoute const& r, char const* what, uint32 handed,
                    std::string const& why)
    {
        uint32 const actual = uint32((QuestStats::NowMs() - r.currentSinceMs) / IN_MILLISECONDS);
        LOG_INFO("playerbots", "{} stop={} kind={} planned={} actual={} handed={}{}", RouteHead(bot, what),
                 QuestRoute::Describe(r.current), QuestRoute::KindName(r.current.kind), uint32(r.currentPlanned),
                 actual, handed, why.empty() ? "" : " why=" + why);
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
        bool isDone = false;
        if (s.kind == StopKind::Objective)
            isDone = status == QUEST_STATUS_COMPLETE || status == QUEST_STATUS_REWARDED;
        else if (s.kind == StopKind::Ender)
            isDone = status == QUEST_STATUS_REWARDED;
        else if (s.kind == StopKind::Giver)
            isDone = status != QUEST_STATUS_NONE;
        if (isDone)
            ++done;
        else if (botAI->lowPriorityQuest.count(q))
            ++postponed;
        else if (s.kind != StopKind::Giver && status == QUEST_STATUS_NONE)
            ++gone;
    }
    // a town stop: reached, or its task left the list (the guild's NextStep ran it there)
    bool const townPending =
        std::any_of(towns.begin(), towns.end(), [&](auto const& t)
                    { return QuestRoute::SpotKey(t.first.GetPositionX(), t.first.GetPositionY()) ==
                             QuestRoute::SpotKey(s.x, s.y); });
    if (s.kind == StopKind::Town && (!townPending || bot->GetDistance2d(s.x, s.y) <= TOWN_STOP_REACHED))
        LogStopEnd(bot, r, "done", 0, "");
    else if (done)
        LogStopEnd(bot, r, "done", s.kind == StopKind::Ender ? done : 0, "");
    else if (!s.quests.empty() && postponed + gone == s.quests.size())
        LogStopEnd(bot, r, "left", 0, postponed ? "postponed" : "left the log");
    else if (r.invalid)
        LogStopEnd(bot, r, "left", 0, r.invalidWhy);
    else
        return false;
    r.hasCurrent = false;
    return true;
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
    uint64 const key = RouteKey(townKey);
    uint32 const now = uint32(time(nullptr));
    std::string why;
    if (!r.built)
        why = "first";
    else if (r.invalid)
        why = r.invalidWhy;
    else if (r.key != key)
        why = "the log, level or tasks changed";
    else if (r.respawnDue && now >= r.respawnDue)
        why = "a left-out quest's camp respawned";
    if (!why.empty())
    {
        CloseRouteStop(r, towns);
        QuestRoute::Input in;
        CollectRouteStops(in, towns, r.visitedTown);
        r.plan = QuestRoute::Solve(in);
        r.key = RouteKey(townKey);  // after the clean-ups and postpones of the collection
        r.built = true;
        r.invalid = false;
        r.respawnDue = in.respawnDue;
        std::string stops;
        for (Stop const& s : r.plan.stops)
            stops += " " + QuestRoute::Describe(s);
        size_t const logQuests = std::count_if(in.stops.begin(), in.stops.end(),
                                               [](Stop const& s) { return s.kind == StopKind::Objective; });
        LOG_INFO("playerbots", "{} stops={} eta={} walk={} xp={} open={} takes={} left_out={} offers={} why={} |{}",
                 RouteHead(bot, "plan"), r.plan.stops.size(), uint32(r.plan.eta), uint32(r.plan.walkYd),
                 uint32(r.plan.xp), logQuests, r.plan.takes, in.leftOut, in.offers.size(), why, stops);
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
        }
        r.plan.stops.erase(r.plan.stops.begin());
    }
    if (r.plan.stops.empty())
        return r;
    Stop const& next = r.plan.stops.front();
    std::string const sig = QuestRoute::Signature(next);
    if (r.hasCurrent && r.currentSig == sig)
        return r;
    if (r.hasCurrent)  // still open, the new route goes elsewhere first
        LogStopEnd(bot, r, "left", 0, "replanned: " + (why.empty() ? std::string("the next stop changed") : why));
    r.hasCurrent = true;
    r.current = next;
    r.currentSig = sig;
    r.currentSinceMs = QuestStats::NowMs();
    float const speed = std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
    r.currentPlanned = bot->GetDistance2d(next.x, next.y) / speed + next.work;
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
