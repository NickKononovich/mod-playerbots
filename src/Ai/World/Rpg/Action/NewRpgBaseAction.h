/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGBASEACTION_H
#define PLAYERBOTS_NEWRPGBASEACTION_H

#include "G3D/Vector4.h"  // [deaths] danger discs
#include "LastMovementValue.h"
#include "MovementActions.h"
#include "NewRpgInfo.h"
#include "NewRpgQuestRoute.h"  // [questroute]
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "TravelMgr.h"

class Creature;
class GameObject;
struct BotPath;
struct GuildRunStep;  // [stuck12]
struct CreatureTemplate;
class MotionTransport;
class Transport;
class WorldPacket;

struct POIInfo
{
    G3D::Vector2 pos;
    int32 objectiveIdx;
    // [guild90] the spawn's own height (INVALID_HEIGHT: unknown, a POI point): a camp on a lower floor (b319 Peny
    // q87 Goldtooth in Fargodeep Mine, z 12.8) is walked to there, not to the ground's top above it
    float z = INVALID_HEIGHT;
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
    // [guild15] with both heights known the core's 3D aggro distance is used (a mob under a bridge, on a ledge); a
    // danger the walk is nearest to at its very start is behind the walker (it stands there now, or the previous
    // leg counted it): the walk leaves it, it is not on the way (b321: a refused pack beside the bot blocked every
    // quest of the log, 100+ postpones "objective among <the same mob>")
    // [static39] the static spawns (the spawn index, not live units) of hostile orange / red or elite mobs whose
    // aggro reach (a wanderer's with its wander area) covers the walk `points` from index `fromIndex` on: their
    // reach discs (x, y, z, radius), each spawn once
    static std::vector<G3D::Vector4> StaticDangerDiscs(Player* bot, std::vector<G3D::Vector3> const& points,
                                                       size_t fromIndex);
    static CreatureTemplate const* DangerOnWay(Player* bot, float fromX, float fromY, float toX, float toY,
                                               float fromZ = NO_WAY_HEIGHT, float toZ = NO_WAY_HEIGHT);
    static constexpr float NO_WAY_HEIGHT = -200000.0f;
    // [guild14] a solo bot's trip (trainer, tool vendor, vendor, gather node, guild vault) to (x, y, z): the first
    // mob of orange / red con whose aggro reaches the walk it would take (its route, else the straight line; the
    // world's spawns, not only the mobs in sight) or the danger zone the trip ends in; nullptr: the trip is safe
    static CreatureTemplate const* TripDanger(Player* bot, float x, float y, float z);
    // [guild14] TripDanger once per bot, destination and bot level (the route is searched once; a danger zone is
    // looked up every time); a refusal is logged once: [solo] <bot> TRIP_REFUSED <what>
    static bool TripRefused(Player* bot, float x, float y, float z, std::string const& what);
    // [regress10] the fraction of its maximum an item loses at one death (the core's RATE_DURABILITY_LOSS_ON_DEATH,
    // plus RATE_DURABILITY_LOSS_ON_SPIRIT_RESURRECT below the death sickness level: a bot rises at the spirit healer)
    static float DeathDurabilityLoss(Player* bot);
    // [regress10] the nearest friendly repairer spawn on the bot's map (no walk judged: the global leader's task
    // place, world thread); an empty position when none
    static WorldPosition NearestRepairSpawn(Player* bot);
    // [guild15] at login: its death zones of the last session ("dz" in playerbots_db_store)
    static void LoadDeathZones(Player* bot, std::string const& value);
    // [guild15] its refused-pull zones were judged with its old power: judged again (deaths stay until outlevelled)
    static void ForgetRefusedZones(Player* bot);
    // [guild12] a quest the bot failed (timed, escort) is not taken again
    static bool HasFailedQuest(Player* bot, uint32 questId);
    // [guild12] builds the shared quest spawn / starter indexes (world DB reads) once at server startup, never on a
    // map thread in a bot's tick
    static void BuildQuestIndexes();
    // [guild12] a bot killed by a creature remembers the spot (killer's and its own) as dangerous until it outlevels
    // the killer: DangerOnWay reports it, so objectives and walks there are skipped
    static void RecordDeath(Player* bot, Creature* killer);
    // [guild13] a spot the bot keeps away from until it is above `level`: a pack its pull judge refused (SoloSafety)
    static void RememberDanger(Player* bot, float x, float y, float radius, uint32 entry, uint8 level);
    // [guild15] the zones of the pulls its judge refused (not where it died) are judged again: after a repair
    static void ForgetRefusals(Player* bot);
    // [guild13] the spot lies in one of the bot's remembered danger zones (a death or a refused pack): its creature
    static CreatureTemplate const* InDangerZone(Player* bot, float x, float y);
    // [regress10] ... where it died (a refused pull's zone left out)
    static bool InDeathZone(Player* bot, float x, float y);
    // [pull9] one of its danger zones covers both spots (the bot standing in the zone of that mob)
    static bool SameDangerZone(Player* bot, float ax, float ay, float bx, float by);
    // [deaths] the bot's danger zones on its map within `range` of (x, y): the killer's and the death spot as discs
    // (x, y, z = the bot's height, radius) - refused by its route search whether or not it sees their mobs
    static void DangerDiscs(Player* bot, float x, float y, float range, std::vector<G3D::Vector4>& out);
    // [guild13] a class quest whose purpose is a spell (its reward or its chain's: AutoLearnQuestSpells grants it):
    // never taken, abandoned when in the log. Letters to the class trainer and the like stay.
    static bool IsSpellUnlockClassQuest(Quest const* quest);
    // [guild14] every ender of the quest is an object the world keeps despawned until a script spawns it
    static bool EnderNeedsScript(uint32 questId);
    // [fish14] an item objective still open whose item only fishing gives (no creature or object drops it, the gather
    // plan's FISH places yield it): `itemId` and the nearest such water on the bot's map (none: an invalid position)
    static bool FishOnlyItem(Player* bot, Quest const* quest, uint32& itemId, WorldPosition& water);
    // [fish14] the bot can fish it: Fishing at the water's skill and a pole of its own
    static bool CanFishFor(Player* bot, uint32 itemId);
    // [guild15] a creature / object objective whose credit comes from using the quest's own tool item (its start item,
    // with an on-use spell: a flute, a disguise kit) - the bot does not use quest items, so it is not taken
    static bool NeedsItemUseCredit(Quest const* quest, std::string* why = nullptr);
    // [flit] a kill objective on a creature friendly to the bot that no kill, talk or other creature credits: a spell
    // or a script gives it (openOnly: the objectives still to do)
    static bool NeedsSpellCredit(Player* bot, Quest const* quest, bool openOnly, std::string* why = nullptr);
    // [guild13] where a bot goes after a death: the nearest ender of a quest in its log, else a travel hub (inn)
    static WorldPosition NearestHub(Player* bot);
    // [guild13] before any far walk: the revived bot's way to its hub, the regeneration sit of a bot that must rest
    // with nothing to eat, a hunter's tame of its first pet; true: the tick is taken
    bool SoloSafetyStep();
    // [guild13] objective spots of a quest the bot stood at with nothing to do (no mob it pulls, no object): skipped
    // by the next POI pick until the quest is postponed
    static void NoteSpentSpot(Player* bot, uint32 questId, WorldPosition const& pos);
    static bool IsSpentSpot(Player* bot, uint32 questId, float x, float y);
    static void ClearSpentSpots(Player* bot, uint32 questId);
    // [guild90] why the bot left or skipped a quest (shown on the planner's next QUEST line); the planner's last why
    static void NotePostpone(Player* bot, uint32 questId, std::string const& reason);
    static std::string QuestChoiceNote(Player* bot);
    // [guild90] the share (0..1) of the quest's objective counts still to do
    static float RemainingShare(Player* bot, Quest const* quest);
    // [questroute] the bot's route is built again on its next pick: its stop for the quest proved impossible
    static void InvalidateRoute(Player* bot, uint32 questId, std::string const& why);
    // [questroute] the route's pick: the active quest (claims, the QUEST line's note), a switch away from it logged
    static void NoteRouteChoice(Player* bot, uint32 chosen, std::string const& why);
    // [guild90] the camp a bot works for a quest (questId 0: none); other bots skip it as taken
    static void ClaimSpot(Player* bot, uint32 questId, WorldPosition const& pos);
    // [flit] the quest a bot leaves for another while its camps respawn: ChooseBestQuest takes it up again
    static void NoteResume(Player* bot, uint32 questId);
    // [guild90] the counts of this thread's last objective resolve (spawns, spent, taken, offered), for the log
    static std::string LastResolveStats();
    // [guild90] seconds until the first spent camp of the quest respawns (spawntimesecs); 0: none pending
    static uint32 EarliestRespawn(Player* bot, uint32 questId);
    // [guild90] the cheapest other quest in the log (travel at QUEST_TRAVEL_SPEED + the work left); -1: none
    float OtherQuestSeconds(uint32 exceptQuest, uint32& otherQuest);
    // [guild12] a timed quest: the walk to its nearest ender at the bot's run speed fits its time
    static bool TimedQuestFits(Player* bot, Quest const* quest);
    // [guild13] the spawn (with its floor's height) of the quest's ender nearest the bot on its map
    static bool NearestEnderSpot(Player* bot, uint32 questId, WorldPosition& out);
    // [guild13] a far walk that cannot start: one [rpg] WALK_FAIL line per destination (the route, the refusal)
    void NoteWalkFailure(WorldPosition const& dest, std::string const& why);
    // [guild15] perf: the far route search found no walk from where the bot stands (NewRpgInfo::moveFarFailed)
    // [guild15] b319 Selalleas stood 705 s 9 yd from her trainer (DO_PROFESSION "OK" each tick): the bot stands and
    // no walk on the navmesh ends within interaction distance of the npc - the visit cannot happen from here
    bool NpcOutOfWalk(WorldObject* npc);
    void NoteFarSearchFailed()
    {
        botAI->rpgInfo.moveFarFailed = true;
        botAI->rpgInfo.failedFromX = bot->GetPositionX();
        botAI->rpgInfo.failedFromY = bot->GetPositionY();
        botAI->rpgInfo.failedFromZ = bot->GetPositionZ();
    }
    // [guild14] no way out from where the bot stands: the core path, the reverse route, the nearest polygon in sight
    bool WalkOffIsland(WorldPosition const& dest, float disToDest);
    // [runpast11] the last WalkOffIsland found the bot on a navmesh island (its region within its sight, no step or
    // fall off it leading on): no walk from here reaches anything, a postpone of the target is no answer
    bool onIsland = false;
    // [runpast11] the walk to `refused` is walled (every way passes mobs it does not pull and does not live through
    // running past): the walk out to its bind point, else its hub, on this landmass is walled too - it stands walled
    // in, not the target. `why`: that walk's verdict
    bool WalledIn(WorldPosition const& refused, std::string& why);
    // [deaths] user 2026-11-08: the maze search of a solo bot's far walk whose route (`path`) passes a pack it does
    // not fight, ends in a dead end (`deadEnd`) or is longer than the search completes: its stages over the travel
    // nodes, another way round, a refused patrol's window, else the walk given up (RefusedWalk). true: handled
    bool WalkAround(WorldPosition const& dest, BotPath const& path, float disToDest, bool deadEnd);
    // [deaths] the walk to `dest` is refused (`why`): its quest is postponed (the co-op seek asks for a party), its
    // gather node skipped, its state ended; true: the tick is used
    void GiveUpWalkTargets(WorldPosition const& dest, std::string const& why);  // [grind25]
    bool TramTrackStep();  // [tram25] off the navmesh on a tram line's track: boards the car that passes under it
    bool RefusedWalk(WorldPosition const& dest, std::string const& why);
    // [holds9] a walk given up (refused, or proven out of walk: MOVE_FAR_LOOP, no route and no step): its spot is out
    // of the choices (TripRefused, the grind spot choice) until the bot's next level; one GIVE_UP line per spot
    static void NoteWalkGivenUp(Player* bot, WorldPosition const& dest, std::string const& why);
    static bool WalkGivenUp(Player* bot, float x, float y);
    // [help12] the camp that walled the walk is cleared: the spot is its choice again
    static void ForgetWalkGivenUp(Player* bot, WorldPosition const& dest);
    // [holds9] a GO_GRIND / GO_CAMP tick: the far walk to `pos`, else a nudge, else the walk given up or a HOLD with
    // its reason (NewRpgAction.cpp)
    bool FarWalkStep(WorldPosition const& pos, char const* status);
    // [guild13] a hunter with Tame Beast and no pet tames the nearest tameable beast of its level
    bool TameFirstPet();
    // [guild13] the walk's end lies in a danger zone or among idle mobs whose pull the judge refuses (logged, the pack
    // remembered as a danger zone): the walk is not made
    bool DestinationRefused(WorldPosition const& dest);
    // [quest13] the bot stands in an area trigger of the quest's explore objective: reported as a client reports it
    // (true); else `walkTo` gets the nearest such trigger's centre on this map
    bool ReportExploreTrigger(uint32 questId, WorldPosition* walkTo = nullptr);

protected:
    // [guild3] full bags or low ammo: GO_CAMP to the nearest friendly vendor; true when the walk starts
    bool StartVendorRun();
    // [regress10] no walk out of where it stands that it survives: its Hearthstone to its bind point (a game item and
    // mechanic), its cooldown waited out where it stands (logged with the ETA); false when it cannot (no stone, the
    // bind point here)
    // [hearth23] camp: the walled-in walk's target - on the stone's cooldown it asks for help with that camp
    bool HearthOut(std::string const& why, WorldPosition const* camp = nullptr);
    // [hold58] a walled-in bot (WalledIn) is held where it waits - out of every reach, no plan - until its help
    // arrives (a party holds it), the wall's camp is cleared or its level changes (the given-up walk is its choice
    // again), or its Hearthstone takes it out; true from WalledHoldStep: the tick is the hold's
    void NoteWalledHold(WorldPosition const& camp, std::string const& why);
    bool WalledHoldStep();
    // [vendor12] the cheapest usable stack of its ammo bought straight from the vendor's list
    void BuyAmmoDirect(Creature* vendor, uint32 subClass, uint32 have);
    // [stuck12] the guild's WALK / GRIND step goes to a spot whose walk the bot gave up at this level
    bool GuildStepGivenUp(GuildRunStep const& step);
    // [town12] a party member at one of its own town stops runs the task there (the leader's route takes them)
    bool TownTaskHere();
    bool MemberHandInHere();  // [quest16] its complete quest at the ender its leader stands at
    // [regress10] an innkeeper within reach whose inn is not its bind point: it binds there (the innkeeper's own
    // "make this inn your home"), so the Hearthstone takes it back to the town it quests from
    bool BindAtInnHere();
    // [regress10] the bot's first New RPG tick after its login: when idle mobs' reach covers its login spot, a step
    // out of every reach on a walk the route search judges (its walls round the camp); true when it walks
    bool LoginEscape();
    // [deaths13] hurt out of a fight within an idle mob's reach (a wanderer's area): the step out of every reach to
    // rest there; true when it walks
    bool RestEscape();

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
    bool RideTransport(Transport* transport);
    bool BoardTransport(Transport* transport);
    bool FindDeckPoint(Transport* transport, float& x, float& y, float& z);
    Transport* FindTransport(uint32 entry);  // [tram23] a boat / zeppelin, or a tram car in sight
    bool HoldPosition(uint32 duration = 2 * IN_MILLISECONDS);
    void MovePointDirect(float x, float y, float z);
    bool IsGroupGathered(float distance);
    // [timers] a member beyond `distance` that is coming (alive, on this map, a walk reaches the bot from it): its
    // name, "" when none is; what the leader waits for at a portal instead of a gather timer
    std::string ComingStraggler(float distance);
    // [timers] the run time in ms the bot and its gathered members need to the transport's middle (the slowest)
    uint32 BoardWalkMs(Transport* transport);
    // [timers] a member beyond GROUP_GATHER_DISTANCE that reaches the transport within `windowMs` at its run speed
    std::string StragglerInTime(Transport* transport, uint32 windowMs);
    bool DalaranStep(WorldPosition const& dest);
    bool UseDalaranPortal();
    bool UseDalaranCrystal(uint32 entry);
    bool TeleportByDalaranEnvoy();
    bool FlyTowards(uint32 toNode);
    bool FlyIfFaster(WorldPosition const& dest);  // [taxi20]
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
    // [questroute] the next stop of the bot's route over all its quests (NewRpgQuestRoute.cpp); no stop on its
    // landmass: ChooseQuestByRate (the zone change, a quest of another landmass)
    uint32 ChooseBestQuest(WorldPosition& giverPos);
    uint32 ChooseQuestByRate(WorldPosition& giverPos);
    // [questroute] the route's first stop is a town task: its spot (IDLE walks there, the guild's NextStep runs it)
    bool RouteTownStop(WorldPosition& pos, std::string& what);
    void NoteTownGivenUp(WorldPosition const& pos, std::string const& what);  // [stuck12] its [route] line
    // [questroute] the quest's ender is the route's next stop (or off the route): the walk to hand it in starts now
    bool RouteHandsInNow(uint32 questId);
    QuestRoute::BotRoute& CurrentRoute();
    bool CloseRouteStop(QuestRoute::BotRoute& route,
                        std::vector<std::pair<WorldPosition, std::string>> const& towns);
    uint64 RouteKey();
    // [navgap] user 2026-11-09: a bot off the navmesh or on an island is a navmesh / travel defect to repair, not a
    // state to hearth out of: one alarm per bot and spot with the map, the position and the facts of how it got there
    // (a transport it rides or was left by, a trip phase, standing in the air, falling, a steep polygon, a hole)
    static void NavGap(Player* bot, std::string const& event);
    // [zone23] a grind kill's whole cycle at `spot` (fight, walk at the spawn density, rest, the respawn floor)
    // -1: no mob of its level there
public:
    // [supply27] what a five member lacks of its ammo (a hunter below the vendor run's low-ammo line) and of the
    // vendor-sold reagents its known spells use (none left of one); "" = nothing. missingReagents: those item ids
    static std::string FiveGearShort(Player* bot, std::vector<uint32>* missingReagents = nullptr);
    static float GrindCycleSeconds(Player* bot, WorldPosition const& spot, float killSeconds, std::string& parts);
protected:
    // [routeplan] the stops done, postponed or dropped are cut out of the order (no new build); a hand-in that opens
    // a follow-up the bot may take: its why (the route is built again)
    std::string TrimRoute(QuestRoute::BotRoute& route, std::vector<std::pair<WorldPosition, std::string>> const& towns);
    // [routeplan] a log quest or town task the route has no stop for: its why (the route is built again), else empty
    std::string RouteMissingStop(QuestRoute::BotRoute const& route,
                                 std::vector<std::pair<WorldPosition, std::string>> const& towns);
    // [routeplan] the route's leg from `from` to `to` for the bot: the navmesh corridor's yards and the seconds of the
    // fights on it (each hostile spawn whose aggro reach the corridor crosses: its fight at `dps`, or the walk round
    // its reach when cheaper or never fought); the corridor and its spawns cached per spot pair for every bot.
    // newLegs (optional): the corridor searches it may still run (a leg not cached past it: false, estimated)
    static bool MeasureLeg(Player* bot, QuestRoute::Point const& from, QuestRoute::Point const& to, float dps,
                           float& walkYd, float& fightSec, uint32* newLegs = nullptr);
    // [questroute] the route's stops from the log, the givers of its landmass and the town tasks (at a rebuild only)
    void CollectRouteStops(QuestRoute::Input& in, std::vector<std::pair<WorldPosition, std::string>> const& towns,
                           std::set<uint64> const& visitedTown);
    // [quest14] the complete grey quests nobody needs (in.greyHandIns): each ender the solved route passes at a
    // detour within one hand-in visit (in.hubRadius) joins the plan, the others leave the log
    void PlaceGreyHandIns(QuestRoute::Plan& plan, QuestRoute::Input const& in);
    // [quest14] a grey quest leaves the log (the core's CMSG_QUESTLOG_REMOVE_QUEST), logged once
    void AbandonGreyQuest(uint8 slot, Quest const* quest, std::string const& reason);
    // [guild11] the quest the bot is closest to being allowed: lowest min level above its own, nearest giver;
    // grindPos = the grind spot of its level nearest that giver
    uint32 ChooseNextLevelQuest(WorldPosition& giverPos, WorldPosition& grindPos, bool withGrindSpot = true);
    // [guild11] hook for the dungeon step: logs the normal dungeons of its level
    void LogDungeonCandidates();
    float GetQuestWeight(Quest const* quest, bool rested);
    uint32 GetQuestProgress(uint32 questId);
    bool CheckRpgStatusAvailable(NewRpgStatus status);

protected:
    /* FOR MOVE FAR */
    const float pathFinderDis = 70.0f;
    // the progress a walk must make toward dest to count as getting closer (and the loop radius of the stuck check)
    static constexpr float MOVE_FAR_PROGRESS = 5.0f;

    /* FOR TRAVEL */
    static constexpr float TRIP_PLAN_DISTANCE = 1200.0f;            // closer destinations are always walked to
    // [timers] class (b): the route search (expensive) to the same destination is not asked again within this; the
    // same search gives the same answer, the bot walks meanwhile
    static constexpr uint32 TRIP_REPLAN_MS = 30 * IN_MILLISECONDS;
    static constexpr float PIER_REACHED = 6.0f;
    static constexpr float GROUP_GATHER_DISTANCE = 30.0f;
    static constexpr float GROUP_USE_DISTANCE = 20.0f;              // members this close use a portal with us
    static constexpr float QUEST_OBJECT_RANGE = 60.0f;

    /* FOR GROUP QUESTS */
    static constexpr uint32 MAX_GROUP_QUEST_PLAYERS = 5;
    static constexpr uint32 MIN_GROUP_FOR_ELITE = 3;     // members on the map within GROUP_LEVEL_SPREAD levels
    static constexpr int32 GROUP_LEVEL_SPREAD = 3;
    static constexpr uint32 GROUP_CHECK_MS = 10 * IN_MILLISECONDS;  // [timers] class (b): a cache of HasCapableGroup
};

#endif
