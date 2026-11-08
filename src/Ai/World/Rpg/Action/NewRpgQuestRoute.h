/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGQUESTROUTE_H
#define PLAYERBOTS_NEWRPGQUESTROUTE_H

#include "Define.h"

#include <set>
#include <string>
#include <vector>

// [questroute] one route over every quest of the bot's log (user 2026-11-08: several quests at once, done by route,
// turned in together): the stops (objective areas, hand-in hubs, givers on the way, town tasks) ordered for the least
// time - the walk at the bot's run speed plus the work at each stop by the formula (kills of the units left at
// EstimateDps, an item's source over its drop chance, the walk between the area's spawns). A heuristic tour: nearest
// feasible neighbour, then 2-opt and relocate moves that keep every quest's giver before its objective before its
// ender. Built only when the log, the level, the postponed set or the town tasks change, a stop proves impossible or
// a spent camp's respawn is due; the bot takes its next stop from the cache.
namespace QuestRoute
{
    enum class StopKind : uint8
    {
        Objective,  // work a quest's objectives at its area
        Ender,      // hand in the quests whose enders stand at this hub
        Giver,      // take the quests this giver offers
        Town        // a town task of the guild's task list (vendor, bank, mailbox)
    };

    struct Stop
    {
        StopKind kind{StopKind::Objective};
        float x{0.0f}, y{0.0f}, z{0.0f};
        std::vector<uint32> quests;  // objective: its quest; ender: the quests handed in here; giver: taken here
        float work{0.0f};            // seconds at the stop: kills and loot by the formula, the walk between its camps
        std::string what;            // a town task: its name
    };

    // a giver the route may pass: the quests the bot would take there, each with its area and ender
    struct Offer
    {
        Stop giver;
        struct Take
        {
            uint32 questId{0};
            float xp{0.0f};
            Stop objective;
            bool hasEnder{false};
            Stop ender;
        };
        std::vector<Take> takes;
    };

    struct Input
    {
        float x{0.0f}, y{0.0f};
        float speed{1.0f};          // the bot's run speed (yd/s)
        float hubRadius{0.0f};      // enders this close are one hand-in visit
        std::vector<Stop> stops;    // the log's objectives and hubs, the town tasks
        std::vector<Offer> offers;  // givers of quests it may take on its landmass
        float logXp{0.0f};          // the experience of the log's routed quests
        uint32 freeSlots{0};        // quest log slots left
        uint32 pinnedQuest{0};      // the started quest: its objective stays the first stop
        uint32 respawnDue{0};       // unix s: a spent camp of a left-out quest respawns (0: none)
        uint32 leftOut{0};          // log quests not routed (other landmass, every camp spent, no spot)
    };

    struct Plan
    {
        std::vector<Stop> stops;  // in order
        float eta{0.0f};          // seconds: walks + work
        float walkYd{0.0f};
        float xp{0.0f};
        uint32 takes{0};  // quests it plans to take on the way
    };

    Plan Solve(Input const& in);
    // seconds of the route from (x, y): the walks at `speed` plus every stop's work; walkYd: the yards walked
    float Seconds(float x, float y, float speed, std::vector<Stop> const& order, float* walkYd = nullptr);
    char const* KindName(StopKind kind);
    // kind + quests (+ a town spot): the same stop across rebuilds
    std::string Signature(Stop const& stop);
    // one short word per stop for the [route] line: O218 E218+219 G2:q1,q2 T(bank)
    std::string Describe(Stop const& stop);
    uint64 SpotKey(float x, float y);

    // the bot's cached route (map thread of the bot only; the container is locked)
    struct BotRoute
    {
        bool built{false};
        uint64 key{0};
        uint64 townKey{0};
        Plan plan;
        uint32 respawnDue{0};
        bool invalid{false};
        std::string invalidWhy;
        // the stop it is on: since when and the seconds the route gave it (leg + work), for the [route] done line
        bool hasCurrent{false};
        Stop current;
        std::string currentSig;
        uint64 currentSinceMs{0};
        float currentPlanned{0.0f};
        std::set<uint64> visitedTown;  // town spots reached under the current task list
    };
    BotRoute& Of(uint32 botLow);
}

#endif
