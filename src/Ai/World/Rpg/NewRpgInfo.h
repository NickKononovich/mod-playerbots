/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGINFO_H
#define PLAYERBOTS_NEWRPGINFO_H

#include <unordered_set>

#include "Define.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "Strategy.h"
#include "Timer.h"
#include "TravelMgr.h"

#include <array>
#include "G3D/Vector3.h"
#include <vector>
#include <string>
#include <unordered_map>

using NewRpgStatusTransitionProb = std::vector<std::vector<int>>;

struct NewRpgInfo
{
    NewRpgInfo() : data(Idle{}) {}
    ~NewRpgInfo() = default;

    // RPG_GO_GRIND
    struct GoGrind
    {
        WorldPosition pos{};
    };
    // RPG_GO_CAMP
    struct GoCamp
    {
        WorldPosition pos{};
    };
    // RPG_WANDER_NPC
    struct WanderNpc
    {
        ObjectGuid npcOrGo{};
        uint32 lastReach{0};
    };
    // RPG_WANDER_RANDOM
    struct WanderRandom
    {
        WanderRandom() = default;
        uint8 level{0};  // [timers] its level when the grind began: a level-up ends it (the choice runs again)
    };
    // RPG_DO_QUEST
    struct DoQuest
    {
        Quest const* quest{nullptr};
        uint32 questId{0};
        int32 objectiveIdx{0};
        WorldPosition pos{};
        uint32 lastReachPOI{0};  // non-zero once the spot is reached ([timers] a flag, no stay time reads it)
        WorldPosition giverPos{};  // set while the quest still has to be picked up there
    };
    // RPG_TRAVEL_FLIGHT
    struct TravelFlight
    {
        uint32 flightMasterEntry{0};
        WorldPosition flightMasterPos{};
        std::vector<uint32> path;
        bool inFlight{false};
    };
    // RPG_REST
    struct Rest
    {
        Rest() = default;
    };
    // RPG_OUTDOOR_PVP
    struct OutdoorPvP
    {
        ObjectGuid::LowType capturePointSpawnId{0};
    };
    // RPG_DO_GATHER
    struct DoGather
    {
        ObjectGuid::LowType nodeSpawnId{0};
        WorldPosition nodePos{};
        std::unordered_set<ObjectGuid::LowType> visited;
        uint32 lastReach{0};
        uint32 lastPassiveCheck{0};
        ObjectGuid::LowType lastSwitchedFrom{0};
        uint8 level{0};  // [timers] its level when the run began: a level-up ends a non-guild bot's run
    };
    // RPG_DO_PROFESSION [guild20]: a walk to a profession trainer (learn a rank) or a vendor (buy a tool)
    struct DoProfession
    {
        uint8 kind{0};  // 1 train, 2 buy, 3 learn recipes, 4 skill-up crafting [guild70]
        uint32 npcEntry{0};
        WorldPosition pos{};
        uint32 spellId{0};  // trainer spell (kind 1), the recipe (kind 4)
        uint32 itemId{0};   // tool (kind 2), the vendor reagent to buy first (kind 4)
        uint32 skill{0};
        uint32 cost{0};
        ObjectGuid npc{};
        uint32 count{0};        // [guild70] kind 4: vendor lots to buy
        uint32 castSpell{0};    // [guild70] kind 4: the craft cast in progress
        uint32 skillBefore{0};  // [guild70] its skill value before the cast
        uint32 crafted{0};      // [guild70] crafts done in this visit
        float walkGap{-1.0f};   // [guild15] its distance to the npc when its last walk there was issued
    };
    struct Idle
    {
    };

    uint32 startT{0};  // start timestamp of the current status
    uint8 lastQuestLevel{0};  // level when the abandoned-quest list was last cleared
    uint32 townCheckArea{0};  // [tasks] the area DO_QUEST last asked the global leader's town tasks in

    // MOVE_FAR
    float nearestMoveFarDis{FLT_MAX};
    // [timers] the spots a walk to moveFarPos started from without getting closer (no stuck timer): a walk that
    // starts again within the progress step of one of them closes a loop - no walk from there gets closer
    std::vector<G3D::Vector3> stuckSpots;
    WorldPosition moveFarPos;
    // [guild15] perf: the far walk's last route search to moveFarPos found no walk from failedFrom; the same search
    // from the same spot gives the same answer, so it is asked again only once the bot stands elsewhere (game fact:
    // its position) or the destination changes
    bool moveFarFailed{false};
    float failedFromX{0.0f};
    float failedFromY{0.0f};
    float failedFromZ{0.0f};
    // [guild15] perf: the last complete far route to moveFarPos; the next leg is planned only to a point on it
    // two sights ahead (the danger penalties are of the mobs in sight, so a local search sees the same mobs)
    std::vector<G3D::Vector3> farRoute;
    // END MOVE_FAR

    // [guild15] the way it came: the spots it walked out of combat, one per leash radius of walk (the survival run's
    // stretch), newest last; the survival run goes back along it first (it passed there, b333 Colian's run tried five
    // straight stretches only and found none clear). Cleared on another map.
    // Capacity, from the core's chase end (Creature::CanCreatureAttack: past CreatureLeashRadius R of its reset spot,
    // plus its wander distance W, and its last leash extension older than Creature::GetLeashTimer = 11 + minlevel / 10
    // - 2 s, at least 11 s: 17 s at minlevel 80-83). The worst run: the bot's run speed with its fastest flee piece
    // (7 yd/s x 1.7, Sprint / Dash) x (17 s + the threat list's 1 s update) + R + W = 214 + R + W yd. One spot per R
    // of walk: 16 spots = 16 R = 480 yd at R = 30, enough for W up to 236 yd. A plan that needs more logs it
    // (SurvivalKit PlanFlee "the way back kept ... is shorter").
    static constexpr size_t TRAIL_SPOTS = 16;
    std::array<G3D::Vector3, TRAIL_SPOTS> trail{};
    uint8 trailCount{0};
    uint8 trailHead{0};  // the next slot to write
    uint32 trailMap{0};
    void NoteTrail(uint32 mapId, G3D::Vector3 const& at, float spacing)
    {
        if (trailMap != mapId)
        {
            trailCount = 0;
            trailHead = 0;
            trailMap = mapId;
        }
        if (trailCount)
        {
            G3D::Vector3 const& last = trail[(trailHead + TRAIL_SPOTS - 1) % TRAIL_SPOTS];
            if ((last - at).squaredLength() < spacing * spacing)
                return;
        }
        trail[trailHead] = at;
        trailHead = uint8((trailHead + 1) % TRAIL_SPOTS);
        if (trailCount < TRAIL_SPOTS)
            ++trailCount;
    }
    // the n-th spot back (0 = the newest)
    G3D::Vector3 const& TrailBack(uint8 n) const { return trail[(trailHead + TRAIL_SPOTS - 1 - n) % TRAIL_SPOTS]; }

    // Boat, zeppelin or portal leg of a far move. It spans statuses: a new destination plans it again.
    enum class TripPhase : uint8
    {
        None,
        ToStart,  // walking to the pier or the portal
        Wait,     // on the pier until the transport docks
        Board,    // stepping onto the deck
        Ride,     // aboard until it docks where the leg ends
        Leave     // walking off onto that pier
    };
    struct Trip
    {
        TripPhase phase{TripPhase::None};
        TravelRouteMgr::Leg leg{};
        uint32 phaseStart{0};
        WorldPosition dest{};
        uint32 plannedAt{0};
        // [timers] an unplanned ride: the dock it was boarded at, skipped until the transport has left it (no ride
        // timer); NO_DOCK once it left
        static constexpr uint8 NO_DOCK = 0xFF;
        uint8 boardDock{NO_DOCK};
    };
    Trip trip;
    void SetTripPhase(TripPhase phase)
    {
        trip.phase = phase;
        trip.phaseStart = getMSTime();
    }
    void ClearTrip() { trip = Trip{}; }

    // [timers] Portal: Dalaran, no cast wait / retry timer: the mage asked (waited for while it casts the portal) and
    // the mage whose cast failed for this destination (asked again only for another destination)
    ObjectGuid dalaranPortalMage{};
    ObjectGuid dalaranPortalFailedMage{};

    // taxi stall watch (#2863, #2664)
    float taxiLastX{0.0f};
    float taxiLastY{0.0f};
    float taxiLastZ{0.0f};
    uint32 taxiStillSince{0};  // the world's clock at the last position sample (TaxiStallWatch)
    uint8 taxiRecoveries{0};
    bool taxiStalled{false};  // [timers] proven by TaxiStallWatch::IsStalled this check

    uint32 groupCheckedAt{0};  // cache of HasCapableGroup
    bool capableGroup{false};

    // quest game object a group member used; this bot walks up and uses it too (#2725)
    ObjectGuid pendingQuestObject{};
    uint32 pendingQuestObjectSince{0};
    // QUEST GIVER
    ObjectGuid questGiver;  // giver the bot is walking to or talking with
    // [timers] skipped until the next IDLE, by facts (no give-up timer): no walk reaches it, or an interaction with it
    // took nothing (no quest from it the bot accepts, none it hands in)
    GuidSet ignoredQuestGivers;
    // END QUEST GIVER

    // [guild3] A walk to a vendor to sell junk (bags nearly full) or buy arrows/bullets, like a player: GO_CAMP to the
    // vendor's spawn, then WANDER_NPC on it (sell + buy). The bag fill and money at the last visit keep a visit that
    // could not help (nothing sellable left, ammo not affordable) from repeating until either changes.
    uint32 vendorRunEntry{0};
    std::string vendorRunReason;
    uint8 vendorVisitBagUsed{0};
    uint32 vendorVisitMoney{0};
    bool vendorVisited{false};
    // [regress10] the global leader's REPAIR task: a visit to a repairer for it was made at this money (tried again
    // after a money gain); cleared when the task ends
    bool repairVisited{false};
    uint32 repairVisitMoney{0};
    std::string hearthNote;    // [regress10] the HEARTH line's last "not used" / cooldown note (logged on change)
    bool loginChecked{false};  // [regress10] its login spot looked at (LoginEscape)
    uint8 loggedStatus{255};  // [guild7] the status last written to the log ([rpg] line)
    // [stuck8] a group leader waits for a member that is not ready (NewRpgGroupNotReadyTrigger, its GROUP_WAIT line)
    bool groupWaiting{false};
    // [stuck8] the last HOLD reason logged for a walk that gives no move (logged once per change)
    std::string holdNote;
    // [holds9] the "last movement" clock (LastMovement::msTime) a deliberate wait set (NewRpgBaseAction::ForceToWait:
    // the group wait, a patrol's window): a far walk honours it; any other clock with the bot standing is a walk whose
    // spline already ended (b345: 61 HOLDs "IsWaitingForLastMove" with the bot still)
    uint32 waitClockMs{0};

    using RpgData = std::variant<
        Idle,
        GoGrind,
        GoCamp,
        WanderNpc,
        WanderRandom,
        DoQuest,
        Rest,
        TravelFlight,
        OutdoorPvP,
        DoGather,
        DoProfession
    >;
    RpgData data;

    NewRpgStatus GetStatus();
    static NewRpgStatus StatusFromString(std::string const& name);
    bool HasStatusPersisted(uint32 maxDuration) { return GetMSTimeDiffToNow(startT) > maxDuration; }
    void ChangeToGoGrind(WorldPosition pos);
    void ChangeToGoCamp(WorldPosition pos);
    void ChangeToWanderNpc();
    void ChangeToWanderRandom();
    void ChangeToDoQuest(uint32 questId, Quest const* quest, WorldPosition giverPos = WorldPosition());
    void ChangeToTravelFlight(uint32 flightMasterEntry, WorldPosition flightMasterPos, std::vector<uint32> path);
    void ChangeToOutdoorPvp(ObjectGuid::LowType capturePointSpawnId = 0);
    void ChangeToDoGather();
    void ChangeToDoProfession(DoProfession const& task);
    void ChangeToRest();
    void ChangeToIdle();
    bool CanChangeTo(NewRpgStatus status);
    void Reset();
    void SetMoveFarTo(WorldPosition pos);
    std::string ToString();
};

enum class QuestDropReason : uint8
{
    Trivial,
    NotWorth,
    NotCapable,
    Failed,
    OtherZone,
    ClassOrHoliday,
    LogFull,
    Count
};

enum class QuestAbandonReason : uint8
{
    NoProgress,
    CannotTurnIn,
    Count
};

inline char const* QuestDropReasonName(QuestDropReason reason)
{
    switch (reason)
    {
        case QuestDropReason::Trivial:
            return "trivial";
        case QuestDropReason::NotWorth:
            return "not_worth";
        case QuestDropReason::NotCapable:
            return "not_capable";
        case QuestDropReason::Failed:
            return "failed";
        case QuestDropReason::OtherZone:
            return "other_zone";
        case QuestDropReason::ClassOrHoliday:
            return "class_or_holiday";
        case QuestDropReason::LogFull:
            return "log_full";
        default:
            return "unknown";
    }
}

inline char const* QuestAbandonReasonName(QuestAbandonReason reason)
{
    switch (reason)
    {
        case QuestAbandonReason::NoProgress:
            return "no_progress";
        case QuestAbandonReason::CannotTurnIn:
            return "cannot_turn_in";
        default:
            return "unknown";
    }
}

struct QuestCounters
{
    uint32 accepted{0};
    uint32 completed{0};
    uint32 rewarded{0};
    std::array<uint32, static_cast<size_t>(QuestDropReason::Count)> dropped{};
    std::array<uint32, static_cast<size_t>(QuestAbandonReason::Count)> abandoned{};
};

struct NewRpgStatistic
{
    uint32 questAccepted{0};
    uint32 questCompleted{0};
    uint32 questAbandoned{0};
    uint32 questRewarded{0};
    uint32 questDropped{0};
    // Filled only while CollectDetailedQuestStats is enabled.
    std::unordered_map<uint32, QuestCounters> byQuest;

    NewRpgStatistic& operator+=(NewRpgStatistic const& other)
    {
        questAccepted += other.questAccepted;
        questCompleted += other.questCompleted;
        questAbandoned += other.questAbandoned;
        questRewarded += other.questRewarded;
        questDropped += other.questDropped;

        for (auto const& [questId, counters] : other.byQuest)
        {
            QuestCounters& target = byQuest[questId];
            target.accepted += counters.accepted;
            target.completed += counters.completed;
            target.rewarded += counters.rewarded;
            for (size_t i = 0; i < target.dropped.size(); ++i)
                target.dropped[i] += counters.dropped[i];
            for (size_t i = 0; i < target.abandoned.size(); ++i)
                target.abandoned[i] += counters.abandoned[i];
        }
        return *this;
    }
};

#endif
