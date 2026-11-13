/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgInfo.h"
#include "Timer.h"
#include <cmath>

void NewRpgInfo::ChangeToGoGrind(WorldPosition pos)
{
    startT = getMSTime();
    data = GoGrind{pos};
}

void NewRpgInfo::ChangeToGoCamp(WorldPosition pos)
{
    startT = getMSTime();
    data = GoCamp{pos};
}

void NewRpgInfo::ChangeToWanderNpc()
{
    startT = getMSTime();
    data = WanderNpc{};
}

void NewRpgInfo::ChangeToWanderRandom()
{
    startT = getMSTime();
    data = WanderRandom{};
}

void NewRpgInfo::ChangeToDoQuest(uint32 questId, Quest const* quest, WorldPosition giverPos)
{
    startT = getMSTime();
    DoQuest do_quest;
    do_quest.questId = questId;
    do_quest.quest = quest;
    do_quest.giverPos = giverPos;
    data = do_quest;
}

void NewRpgInfo::ChangeToTravelFlight(uint32 flightMasterEntry, WorldPosition flightMasterPos, std::vector<uint32> path)
{
    startT = getMSTime();
    TravelFlight flight;
    flight.flightMasterEntry = flightMasterEntry;
    flight.flightMasterPos = flightMasterPos;
    flight.path = std::move(path);
    flight.inFlight = false;
    data = flight;
}

void NewRpgInfo::ChangeToOutdoorPvp(ObjectGuid::LowType capturePointSpawnId)
{
    startT = getMSTime();
    OutdoorPvP pvp;
    pvp.capturePointSpawnId = capturePointSpawnId;
    data = pvp;
}

void NewRpgInfo::ChangeToDoGather()
{
    startT = getMSTime();
    data = DoGather{};
}

void NewRpgInfo::ChangeToDoProfession(DoProfession const& task)
{
    startT = getMSTime();
    data = task;
}

void NewRpgInfo::ChangeToRest()
{
    startT = getMSTime();
    data = Rest{};
}

void NewRpgInfo::ChangeToIdle(std::string reason)
{
    startT = getMSTime();
    data = Idle{};
    idleReason = std::move(reason);  // [status40] what it waits for, in the words of the step that stopped
    idleUndecided = false;
    questGiver.Clear();
    ignoredQuestGivers.clear();
}

// [status41] no wait behind this one: the step produced no plan at all
void NewRpgInfo::ChangeToNoDecision(std::string reason)
{
    ChangeToIdle(std::move(reason));
    idleUndecided = true;
}

bool NewRpgInfo::CanChangeTo(NewRpgStatus)
{
    return true;
}

void NewRpgInfo::Reset()
{
    data = Idle{};
    startT = getMSTime();
    idleReason = "its New RPG state was reset, the next look chooses";  // [status40]
    idleUndecided = false;
    questGiver.Clear();
    ignoredQuestGivers.clear();
}

void NewRpgInfo::SetMoveFarTo(WorldPosition pos)
{
    nearestMoveFarDis = FLT_MAX;
    stuckSpots.clear();
    moveFarPos = pos;
    dalaranPortalFailedMage.Clear();  // [timers] another destination: every mage is asked again
    moveFarFailed = false;
    farRoute.clear();
    graphBans.clear();  // [nav182]
    moveFarFromSet = false;  // [nav165] the caller notes the new walk's start
}

NewRpgStatus NewRpgInfo::StatusFromString(std::string const& name)
{
    if (name == "idle")           return RPG_IDLE;
    if (name == "rest")           return RPG_REST;
    if (name == "wander random")  return RPG_WANDER_RANDOM;
    if (name == "wander npc")     return RPG_WANDER_NPC;
    if (name == "go grind")       return RPG_GO_GRIND;
    if (name == "go camp")        return RPG_GO_CAMP;
    if (name == "do quest")       return RPG_DO_QUEST;
    if (name == "travel flight")  return RPG_TRAVEL_FLIGHT;
    if (name == "outdoor pvp")    return RPG_OUTDOOR_PVP;
    if (name == "do gather")      return RPG_DO_GATHER;
    if (name == "do profession")  return RPG_DO_PROFESSION;
    return RPG_STATUS_END;
}

NewRpgStatus NewRpgInfo::GetStatus()
{
    return std::visit([](auto&& arg) -> NewRpgStatus {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, Idle>) return RPG_IDLE;
        if constexpr (std::is_same_v<T, GoGrind>) return RPG_GO_GRIND;
        if constexpr (std::is_same_v<T, GoCamp>) return RPG_GO_CAMP;
        if constexpr (std::is_same_v<T, WanderNpc>) return RPG_WANDER_NPC;
        if constexpr (std::is_same_v<T, WanderRandom>) return RPG_WANDER_RANDOM;
        if constexpr (std::is_same_v<T, Rest>) return RPG_REST;
        if constexpr (std::is_same_v<T, DoQuest>) return RPG_DO_QUEST;
        if constexpr (std::is_same_v<T, TravelFlight>) return RPG_TRAVEL_FLIGHT;
        if constexpr (std::is_same_v<T, OutdoorPvP>) return RPG_OUTDOOR_PVP;
        if constexpr (std::is_same_v<T, DoGather>) return RPG_DO_GATHER;
        if constexpr (std::is_same_v<T, DoProfession>) return RPG_DO_PROFESSION;
        return RPG_IDLE;
    }, data);
}

std::string NewRpgInfo::ToString()
{
    std::stringstream out;
    out << "Status: ";
    std::visit([&out, this](auto&& arg)
    {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, GoGrind>)
        {
            out << "GO_GRIND";
            out << "\nGrindPos: " << arg.pos.GetMapId() << " " << arg.pos.GetPositionX() << " "
                << arg.pos.GetPositionY() << " " << arg.pos.GetPositionZ();
            out << "\nlastGoGrind: " << startT;
        }
        else if constexpr (std::is_same_v<T, GoCamp>)
        {
            out << "GO_CAMP";
            out << "\nCampPos: " << arg.pos.GetMapId() << " " << arg.pos.GetPositionX() << " "
                << arg.pos.GetPositionY() << " " << arg.pos.GetPositionZ();
            out << "\nlastGoCamp: " << startT;
        }
        else if constexpr (std::is_same_v<T, WanderNpc>)
        {
            out << "WANDER_NPC";
            out << "\nnpcOrGo: entry " << arg.npcOrGo.GetEntry() << " (guid " << arg.npcOrGo.GetCounter()
                << ")";
            out << "\nlastWanderNpc: " << startT;
            out << "\nlastReachNpcOrGo: " << arg.lastReach;
        }
        else if constexpr (std::is_same_v<T, WanderRandom>)
        {
            out << "WANDER_RANDOM";
            out << "\nlastWanderRandom: " << startT;
        }
        else if constexpr (std::is_same_v<T, Idle>)
        {
            // [status40] never a bare IDLE: what it waits for, or [status41] that nothing was decided
            out << (idleUndecided ? "NO_DECISION" : "IDLE") << "("
                << (idleReason.empty() ? "no reason recorded" : idleReason) << ")";
        }
        else if constexpr (std::is_same_v<T, Rest>)
        {
            out << "REST";
            out << "\nlastRest: " << startT;
        }
        else if constexpr (std::is_same_v<T, DoQuest>)
        {
            out << "DO_QUEST";
            out << "\nquestId: " << arg.questId;
            out << "\nobjectiveIdx: " << arg.objectiveIdx;
            out << "\npoiPos: " << arg.pos.GetMapId() << " " << arg.pos.GetPositionX() << " "
                << arg.pos.GetPositionY() << " " << arg.pos.GetPositionZ();
            out << "\nlastReachPOI: " << (arg.lastReachPOI ? GetMSTimeDiffToNow(arg.lastReachPOI) : 0);
        }
        else if constexpr (std::is_same_v<T, TravelFlight>)
        {
            out << "TRAVEL_FLIGHT";
            out << "\nflightMasterEntry: " << arg.flightMasterEntry;
            out << "\nfromNode: " << arg.path[0];
            out << "\ntoNode: " << arg.path[arg.path.size() - 1];
            out << "\ninFlight: " << arg.inFlight;
        }
        else if constexpr (std::is_same_v<T, OutdoorPvP>)
        {
            out << "OUTDOOR_PVP";
            if (!arg.capturePointSpawnId)
                out << "\nNo capture point assigned.";
            else
                out << "\ncapturePointSpawnId: " << arg.capturePointSpawnId;
        }
        else if constexpr (std::is_same_v<T, DoGather>)
        {
            out << "DO_GATHER";
            out << "\nnodeSpawnId: " << arg.nodeSpawnId;
            out << "\nnodePos: " << arg.nodePos.GetMapId() << " " << arg.nodePos.GetPositionX() << " "
                << arg.nodePos.GetPositionY() << " " << arg.nodePos.GetPositionZ();
            out << "\nvisited: " << arg.visited.size();
            out << "\nlastReach: " << (arg.lastReach ? GetMSTimeDiffToNow(arg.lastReach) : 0);
        }
        else if constexpr (std::is_same_v<T, DoProfession>)
        {
            out << "DO_PROFESSION";
            char const* const what = arg.kind == 1   ? "train spell "
                                     : arg.kind == 3 ? "learn recipes "
                                     : arg.kind == 4 ? "craft "
                                                     : "buy item ";
            out << "\n"
                << what << (arg.kind == 1 || arg.kind == 4 ? arg.spellId : arg.itemId)
                << " skill " << arg.skill << " at npc " << arg.npcEntry << " cost " << arg.cost;
        }
        else
            out << "UNKNOWN";
    }, data);
    return out.str();
}
