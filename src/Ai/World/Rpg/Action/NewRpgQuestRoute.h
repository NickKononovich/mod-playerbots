/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGQUESTROUTE_H
#define PLAYERBOTS_NEWRPGQUESTROUTE_H

#include "Define.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

// [questroute] one route over every quest of the bot's log (user 2026-11-08: several quests at once, done by route,
// turned in together): the stops (objective areas, hand-in hubs, givers on the way, town tasks) ordered for the least
// time - the walk at the bot's run speed plus the work at each stop by the formula (kills of the units left at
// EstimateDps, an item's source over its drop chance, the walk between the area's spawns). A heuristic tour: nearest
// feasible neighbour, then 2-opt and relocate moves that keep every quest's giver before its objective before its
// ender. [routeplan] A leg costs the real walk (the navmesh corridor's length, LegTable) and the fights on it (the
// hostile spawns whose aggro reach the corridor crosses: each the cheaper of its fight and the walk round its reach,
// the price the solo route search gives it). Built only when the route lacks a stop for the log or the town tasks
// (a quest taken, a postponed quest back, a hand-in that unlocks a follow-up), the level or map changed, a stop
// proved impossible or a spent camp's respawn is due; a done, postponed or dropped stop is only cut out of the
// order (TrimRoute). The bot takes its next stop from the cache.
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
        uint32 node{0};              // [routeplan] its point in the build's LegTable (0: the bot's spot)
        float leg{0.0f};             // [routeplan] seconds of the leg that reaches it in the plan (walk + fights)
        float legYd{0.0f};           // [routeplan] the yards walked on that leg
        // [quest15] an ender: the experience handed in there (the route's hand-in times are weighted by it); ready:
        // every quest it hands in is complete already (no objective stop before it)
        float xp{0.0f};
        bool ready{false};
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

    // [routeplan] the legs between the points of one build: the walk (yards) and the fights on it (seconds) as
    // measured (`measure`: the navmesh corridor and the spawns along it, priced for the bot), else estimated from the
    // straight line by the legs this build measured (their walk over their straight yards, their fight seconds per
    // straight yard; 1 and 0 before any). Measure(): the legs of a candidate order, only those not measured yet
    struct Point
    {
        float x{0.0f}, y{0.0f}, z{0.0f};
    };
    class LegTable
    {
    public:
        using MeasureFn = std::function<bool(Point const& from, Point const& to, float& walkYd, float& fightSec)>;
        LegTable(float speed, MeasureFn measure) : speed_(speed), measure_(std::move(measure)) {}
        uint32 Add(float x, float y, float z);
        float Seconds(uint32 a, uint32 b) const;
        float Yards(uint32 a, uint32 b) const;
        // the legs not measured yet are measured now; how many were new
        size_t Measure(std::vector<std::pair<uint32, uint32>> const& legs);
        float WalkRatio() const { return straightYd_ > 0.0f ? walkYd_ / straightYd_ : 1.0f; }
        float FightPerYd() const { return straightYd_ > 0.0f ? fightSec_ / straightYd_ : 0.0f; }
        float Speed() const { return speed_; }
        size_t MeasuredCount() const { return known_.size(); }

    private:
        struct Fact
        {
            float walkYd{0.0f}, fightSec{0.0f};
        };
        float Straight(uint32 a, uint32 b) const;
        static uint64 Key(uint32 a, uint32 b) { return (uint64(a) << 32) | b; }
        float speed_;
        MeasureFn measure_;
        std::vector<Point> points_;
        std::unordered_map<uint64, Fact> known_;
        float straightYd_{0.0f}, walkYd_{0.0f}, fightSec_{0.0f};  // the measured legs' sums
    };

    struct Input
    {
        float x{0.0f}, y{0.0f}, z{0.0f};
        float speed{1.0f};          // the bot's run speed (yd/s)
        float hubRadius{0.0f};      // enders this close are one hand-in visit
        std::vector<Stop> stops;    // the log's objectives and hubs, the town tasks
        std::vector<Offer> offers;  // givers of quests it may take on its landmass
        float logXp{0.0f};          // the experience of the log's routed quests
        uint32 freeSlots{0};        // quest log slots left
        uint32 pinnedQuest{0};      // the started quest: its objective stays the first stop
        uint32 respawnDue{0};       // unix s: a spent camp of a left-out quest respawns (0: none)
        uint32 leftOut{0};          // log quests not routed (other landmass, every camp spent, no spot)
        std::map<uint32, uint8> leftOutQuests;  // [routeplan] those quests and their status at the build
        std::set<uint64> townsSeen;             // [routeplan] the town task spots of the build, routed or not
        LegTable::MeasureFn measure;            // [routeplan] the real leg (NewRpgBaseAction::MeasureLeg); empty: none
        // [quest14] the enders of complete grey quests nobody needs: handed in only where the solved route passes
        // them at near-zero detour (PlaceGreyHandIns), else abandoned
        std::vector<Stop> greyHandIns;
    };

    struct Plan
    {
        std::vector<Stop> stops;  // in order
        float eta{0.0f};          // seconds: walks + the fights on them + work
        float walkYd{0.0f};
        float xp{0.0f};
        uint32 takes{0};          // quests it plans to take on the way
        uint32 earlierHandIns{0};  // [quest15] enders moved sooner by the hand-in weighting (HandInsEarlier)
        // [help21] the experience of the open quests' hand-ins only (no ready hub: a complete quest's hand-in comes
        // whatever the bot does) - the rate its time is worth (XpPerSecond)
        float openXp{0.0f};


        float walkRatio{1.0f};    // [routeplan] the build's measured walk over straight yards
        float fightPerYd{0.0f};   // [routeplan] the build's measured fight seconds per straight yard
        float speed{1.0f};
        uint32 measured{0};       // [routeplan] legs the build measured (the cache's or new)
    };

    Plan Solve(Input const& in);
    // [routeplan] a leg the build did not measure (a trim joins two stops): the straight line by the plan's ratios;
    // walkYd: its yards
    float EstimateLeg(Plan const& plan, float ax, float ay, float bx, float by, float* walkYd = nullptr);
    char const* KindName(StopKind kind);
    // kind + quests (+ a town spot): the same stop across rebuilds
    std::string Signature(Stop const& stop);
    // one short word per stop for the [route] line: O218 E218+219 G2:q1,q2 T(bank)
    std::string Describe(Stop const& stop);
    uint64 SpotKey(float x, float y);
    // [quest16] the experience its party members hand in at a town stop of the route being built on this thread (the
    // leader's "HAND_IN ... (for <member>)" stops, by SpotKey): weighed as an ender's (HandInsEarlier)
    float MemberHandInXp(uint64 spotKey);


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
        uint64 currentDoneMs{0};  // [routeplan] when its quest event came (NoteQuestEvent); 0: not yet
        float currentPlanned{0.0f};
        std::set<uint64> visitedTown;           // town spots reached under the current task list
        std::map<uint32, uint8> leftOutQuests;  // [routeplan] of the last build: quest -> its status then
        std::set<uint64> townsSeen;             // [routeplan] of the last build
    };
    BotRoute& Of(uint32 botLow);
    // [help20] the bot's own route: its experience over its seconds (0: no route built) - what an hour of its own plan
    // is worth, the price of the time it gives a help party
    float XpPerSecond(uint32 botLow);


    // [routeplan] QuestStats' accept / complete / turn-in event (the bot's map thread): the stop it is on is done at
    // this moment, whenever the bot next asks its route (the done line counts the stop's seconds to this event)
    void NoteQuestEvent(Player* bot, uint32 questId);
}

#endif
