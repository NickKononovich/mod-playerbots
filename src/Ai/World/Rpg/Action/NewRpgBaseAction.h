/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGBASEACTION_H
#define PLAYERBOTS_NEWRPGBASEACTION_H

#include "LastMovementValue.h"
#include "MovementActions.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "TravelMgr.h"

class Creature;
class GameObject;
struct CreatureTemplate;
class MotionTransport;
class WorldPacket;

struct POIInfo
{
    G3D::Vector2 pos;
    int32 objectiveIdx;
};

/// A base (composition) class for all new rpg actions
/// All functions that may be shared by multiple actions should be declared here
/// And we should make all actions composable instead of inheritable
class NewRpgBaseAction : public MovementAction
{
public:
    NewRpgBaseAction(PlayerbotAI* botAI, std::string name) : MovementAction(botAI, name) {}

    // [guild12] the nearest spawned object (within QUEST_OBJECT_RANGE, in sight, lootable by the loot pipeline)
    // that drops an item a quest in the bot's log still needs (gameobject_loot_template / gameobject_questitem);
    // nullptr none. The grind target prefers it over mobs the quest does not need.
    static GameObject* FindNeededQuestObject(Player* bot);
    // [guild12] a hostile creature spawn of orange or red con for the bot (core Acore::XP::GetColorCode) whose aggro
    // radius (core Creature::GetAggroRange: detection_range plus the level gap) covers the straight walk from -> to
    // on the bot's map; nullptr none
    static CreatureTemplate const* DangerOnWay(Player* bot, float fromX, float fromY, float toX, float toY);
    // [guild12] a quest the bot failed (timed, escort) is not taken again
    static bool HasFailedQuest(Player* bot, uint32 questId);
    // [guild12] builds the shared quest spawn / starter indexes (world DB reads) once at server startup, never on a
    // map thread in a bot's tick
    static void BuildQuestIndexes();
    // [guild12] a bot killed by a creature remembers the spot (killer's and its own) as dangerous until it outlevels
    // the killer: DangerOnWay reports it, so objectives and walks there are skipped
    static void RecordDeath(Player* bot, Creature* killer);

protected:
    // [guild3] full bags or low ammo: GO_CAMP to the nearest friendly vendor; true when the walk starts
    bool StartVendorRun();

    /* MOVEMENT RELATED */
    bool MoveFarTo(WorldPosition dest);
    bool MoveWorldObjectTo(ObjectGuid guid, float distance = INTERACTION_DISTANCE);
    bool MoveRandomNear(float moveStep = 50.0f, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL, WorldObject* center = nullptr);
    bool ForceToWait(uint32 duration, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);

    /* TRAVEL: boats, zeppelins, portals, Dalaran */
    bool MoveFarToDirect(WorldPosition dest);
    bool WalkTo(WorldPosition const& dest);
    bool TravelTrip(WorldPosition const& dest);
    bool TravelByTransport(NewRpgInfo::Trip& trip);
    bool TravelByPortal(NewRpgInfo::Trip& trip);
    bool RideTransport(MotionTransport* transport);
    bool BoardTransport(MotionTransport* transport);
    bool FindDeckPoint(MotionTransport* transport, float& x, float& y, float& z);
    MotionTransport* FindTransport(uint32 entry);
    bool HoldPosition(uint32 duration = 2 * IN_MILLISECONDS);
    void MovePointDirect(float x, float y, float z);
    bool IsGroupGathered(float distance);
    bool DalaranStep(WorldPosition const& dest);
    bool UseDalaranPortal();
    bool UseDalaranCrystal(uint32 entry);
    bool TeleportByDalaranEnvoy();
    bool FlyTowards(uint32 toNode);
    bool UseGameObjectWithGroup(GameObject* go);
    void GossipTeleportToDalaran(Player* player, Creature* envoy);
    bool HasDalaranTeleportQuest(Player* player);

    /* QUEST RELATED CHECK */
    bool InteractWithNpcOrGameObjectForQuest(ObjectGuid guid);
    bool IsWithinInteractionDist(Object* object);
    uint32 BestRewardIndex(Quest const* quest);
    bool IsQuestWorthDoing(Quest const* quest);
    bool IsQuestCapableDoing(Quest const* quest);
    // [guild12] a failed quest (timed / escort) leaves the log for good: logged, never retaken
    bool AbandonFailedQuest(uint32 questId);
    // [guild12] a timed quest: the walk to its nearest ender at the bot's run speed fits its time
    bool TimedQuestFits(Quest const* quest);
    bool HasCapableGroup();
    bool UseQuestGameObject(uint32 questId);
    bool GetDalaranQuestTarget(uint32 questId, WorldPosition& pos);

    /* QUEST RELATED ACTION */
    bool SearchQuestGiverAndAcceptOrReward();
    bool AcceptQuest(Quest const* quest, ObjectGuid guid);
    bool TurnInQuest(Quest const* quest, ObjectGuid guid);
    bool OrganizeQuestLog();
    void RelayPacketToGroupBots(WorldPacket const& packet);
    void RelayQuestPacketToGroupBots(uint16 opcode, ObjectGuid guid, uint32 questId);
    void RelayInteractPacketToGroupBots(ObjectGuid guid);

protected:
    bool GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete = false);
    static WorldPosition SelectRandomGrindPos(Player* bot);
    static WorldPosition SelectRandomCampPos(Player* bot);
    bool SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path);
    bool RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus);
    uint32 ChooseBestQuest(WorldPosition& giverPos);
    // [guild11] the quest the bot is closest to being allowed: lowest min level above its own, nearest giver;
    // grindPos = the grind spot of its level nearest that giver
    uint32 ChooseNextLevelQuest(WorldPosition& giverPos, WorldPosition& grindPos);
    // [guild11] hook for the dungeon step: logs the normal dungeons of its level
    void LogDungeonCandidates();
    float GetQuestWeight(Quest const* quest, bool rested);
    uint32 GetQuestProgress(uint32 questId);
    bool CheckRpgStatusAvailable(NewRpgStatus status);

protected:
    /* FOR MOVE FAR */
    const float pathFinderDis = 70.0f;
    // Time without real progress toward dest before MoveFarTo
    // falls back to teleport recovery. Kept short enough that a
    // bot truly oscillating around an unreachable destination
    // (mmap returning non-progressing partial paths, or NOPATH +
    // cone fallback wandering) doesn't spin for 5 minutes before
    // the teleport fires, but long enough that a genuine long
    // walk that is slowly making progress never triggers it.
    const uint32 stuckTime = 90 * IN_MILLISECONDS;

    /* FOR TRAVEL */
    static constexpr float TRIP_PLAN_DISTANCE = 1200.0f;            // closer destinations are always walked to
    static constexpr uint32 TRIP_REPLAN_MS = 30 * IN_MILLISECONDS;
    static constexpr float PIER_REACHED = 6.0f;
    static constexpr uint32 BOARD_MIN_REMAINING_MS = 15 * IN_MILLISECONDS;  // later the group would miss it
    static constexpr uint32 BOARD_TIMEOUT_MS = 25 * IN_MILLISECONDS;
    static constexpr uint32 WAIT_SPARE_MS = 2 * MINUTE * IN_MILLISECONDS;   // on top of one full loop
    static constexpr float GROUP_GATHER_DISTANCE = 30.0f;
    static constexpr float GROUP_USE_DISTANCE = 20.0f;              // members this close use a portal with us
    static constexpr uint32 PORTAL_CAST_WAIT_MS = 20 * IN_MILLISECONDS;
    static constexpr uint32 PORTAL_RETRY_MS = 5 * MINUTE * IN_MILLISECONDS;
    static constexpr float QUEST_OBJECT_RANGE = 60.0f;

    /* FOR GROUP QUESTS */
    static constexpr uint32 MAX_GROUP_QUEST_PLAYERS = 5;
    static constexpr uint32 MIN_GROUP_FOR_ELITE = 3;     // members on the map within GROUP_LEVEL_SPREAD levels
    static constexpr int32 GROUP_LEVEL_SPREAD = 3;
    static constexpr uint32 GROUP_CHECK_MS = 10 * IN_MILLISECONDS;
    /* FOR QUEST GIVER */
    // Time on one giver, walking and interacting, after which it is skipped until the next IDLE.
    const uint32 questGiverGiveUpTime = 60 * IN_MILLISECONDS;
    // A longer pause (combat, eating) starts the timer over.
    const uint32 questGiverRetryGap = 5 * IN_MILLISECONDS;
};

#endif
