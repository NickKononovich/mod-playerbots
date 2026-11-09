/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotAI.h"
#include "AiFactory.h"
#include "BudgetValues.h"
#include "MapUpdater.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "ObjectAccessor.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "CharacterPackets.h"
#include "ChatHelper.h"
#include "CheckMountStateAction.h"
#include "ChooseTargetActions.h"  // [healer_audit_b30] DpsAssistAction::HealerGroupUnderAttack
#include "ReachTargetActions.h"  // [lead960] StopPullWalkInFight
#include "Common.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "EmoteAction.h"
#include "EscortProtect.h"
#include "Engine.h"
#include "GenericSpellActions.h"
#include "HealthTriggers.h"  // [lead15] RestState (the executor's rest report)
#include "LeaderOrders.h"    // [lead15] the leader's member commands
#include "TestRunnerStrategy.h"  // [lead17] the harness's waypoint for a member
#include "EventProcessor.h"
#include "ExternalEventHelper.h"
#include "GameObjectData.h"
#include "GameTime.h"
#include "GuildMgr.h"
#include "InterruptValues.h"
#include "LFGMgr.h"
#include "LastMovementValue.h"
#include "DetourNavMeshQuery.h"  // [nav24]
#include "MapDefines.h"         // [nav24] NAV_* flags
#include "MovementActions.h"
#include "LastSpellCastValue.h"
#include "LogLevelAction.h"
#include "LootObjectStack.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "NewRpgStrategy.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "PetsAction.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotGuildMgr.h"
#include "PlayerbotMgr.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "RBAC.h"
#include "RaceMgr.h"
#include "ReflectAuras.h"
#include "RandomPlayerbotMgr.h"
#include "ReactionEngine.h"
#include "SayAction.h"
#include "ScriptMgr.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "SmartEnum.h"  // [lead1390] EnumUtils: SpellCastResult names
#include "SocialMgr.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "StringFormat.h"
#include "Transport.h"
#include "TravelMgr.h"  // [guild15] the boat at a stop
#include "GameObjectModel.h"  // [guild15] deck vs pier
#include "ModelIgnoreFlags.h"
#include "Unit.h"
#include "UpdateTime.h"
#include "Vehicle.h"
#include "WsgRouteState.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // Temporary combat idle diagnostics: reports each executed combat action to its bot.
    class IdleDiagListener : public ActionExecutionListener
    {
    public:
        explicit IdleDiagListener(PlayerbotAI* botAI) : _botAI(botAI) {}

        bool Before(Action* /*action*/, Event /*event*/) override { return true; }
        bool AllowExecution(Action* /*action*/, Event /*event*/) override { return true; }
        bool OverrideResult(Action* /*action*/, bool executed, Event /*event*/) override { return executed; }

        void After(Action* action, bool executed, Event event) override
        {
            if (executed && action)
                _botAI->NoteIdleDiagAction(action, event.GetSource());
        }

    private:
        PlayerbotAI* _botAI;
    };
}

namespace
{
// [b44_cast] cast timeline trace (defined below CastSpell)
void B44CastTrace(Player* bot, char const* what, SpellInfo const* info, Unit* target);
}

namespace
{
constexpr uint32 SPELL_TITAN_GRIP = 49152;
constexpr uint32 SPELL_DK_FROST_PRESENCE = 48263;
constexpr uint32 SPELL_GRAVITY_LAPSE_TK = 39432;
constexpr uint32 SPELL_GRAVITY_LAPSE_MGT = 44226;
constexpr uint32 VEHICLE_FLAG_FIXED_POSITION = 0x00200000;
}

std::vector<std::string> PlayerbotAI::dispel_whitelist = {
    "mutating injection",
    "frostbolt",
};

std::vector<std::string>& split(std::string const s, char delim, std::vector<std::string>& elems);
std::vector<std::string> split(std::string const s, char delim);
char* strstri(char const* str1, char const* str2);
std::string& trim(std::string& s);

std::set<std::string> PlayerbotAI::unsecuredCommands;

PlayerbotChatHandler::PlayerbotChatHandler(Player* pMasterPlayer) : ChatHandler(pMasterPlayer->GetSession()) {}

uint32 PlayerbotChatHandler::extractQuestId(std::string const str)
{
    char* source = (char*)str.c_str();
    char* cId = extractKeyFromLink(source, "Hquest");
    return cId ? atol(cId) : 0;
}

void PacketHandlingHelper::AddHandler(uint16 opcode, std::string const handler) { handlers[opcode] = handler; }

void PacketHandlingHelper::Handle(ExternalEventHelper& helper)
{
    // [guild12] only the packets queued before this pass: a handler whose reply is handled again (the core sends
    // the next packet while the bot handles one) waits for the next tick instead of spinning here forever (b293
    // map-0 freezes 60 s, stage "pre: internal": random gatherers 1799 Gnetlibozz, 1793 Ganna)
    for (std::size_t pending = queue.size(); pending && !queue.empty(); --pending)
    {
        WorldPacket packet = queue.top();
        queue.pop(); // remove first so handling can't modify the queue while we're using it

        helper.HandlePacket(handlers, packet);
    }
}

void PacketHandlingHelper::AddPacket(WorldPacket const& packet)
{
    if (packet.empty())
        return;
    // assert(handlers);
    // assert(packet);
    // assert(packet.GetOpcode());
    if (handlers.find(packet.GetOpcode()) != handlers.end())
        queue.push(WorldPacket(packet));
}

PlayerbotAI::PlayerbotAI()
    : PlayerbotAIBase(true),
      bot(nullptr),
      master(nullptr),
      accountId(0),
      aiObjectContext(nullptr),
      currentEngine(nullptr),
      currentState(BOT_STATE_NON_COMBAT),
      chatHelper(this),
      chatFilter(this),
      security(nullptr)
{
    for (uint8 i = 0; i < BOT_STATE_MAX; i++)
        engines[i] = nullptr;

    for (uint8 i = 0; i < MAX_ACTIVITY_TYPE; i++)
    {
        allowActiveCheckTimer[i] = 0;
        allowActive[i] = false;
    }
}

PlayerbotAI::PlayerbotAI(Player* bot)
    : PlayerbotAIBase(true),
      forceRebuff(bot),
      bot(bot),
      master(nullptr),
      chatHelper(this),
      chatFilter(this),
      security(bot)  // reorder args - whipowill
{
    for (uint8 i = 0; i < MAX_ACTIVITY_TYPE; i++)
    {
        allowActiveCheckTimer[i] = 0;
        allowActive[i] = false;
    }

    accountId = bot->GetSession()->GetAccountId();
    aiObjectContext = AiFactory::createAiObjectContext(bot, this);

    engines[BOT_STATE_COMBAT] = AiFactory::createCombatEngine(bot, this, aiObjectContext);
    engines[BOT_STATE_NON_COMBAT] = AiFactory::createNonCombatEngine(bot, this, aiObjectContext);
    engines[BOT_STATE_DEAD] = AiFactory::createDeadEngine(bot, this, aiObjectContext);
    engines[BOT_STATE_REACTION] = reactionEngine = AiFactory::createReactionEngine(bot, this, aiObjectContext);
    if (engines[BOT_STATE_COMBAT])
        engines[BOT_STATE_COMBAT]->AddActionExecutionListener(new IdleDiagListener(this));

    if (sPlayerbotAIConfig.ApplyInstanceStrategies)
        ApplyInstanceStrategies(bot->GetMapId());

    currentEngine = engines[BOT_STATE_NON_COMBAT];
    currentState = BOT_STATE_NON_COMBAT;

    masterIncomingPacketHandlers.AddHandler(CMSG_GAMEOBJ_USE, "use game object");
    masterIncomingPacketHandlers.AddHandler(CMSG_AREATRIGGER, "area trigger");
    // masterIncomingPacketHandlers.AddHandler(CMSG_GAMEOBJ_USE, "use game object");
    // masterIncomingPacketHandlers.AddHandler(CMSG_LOOT_ROLL, "loot roll");
    masterIncomingPacketHandlers.AddHandler(CMSG_GOSSIP_HELLO, "gossip hello");
    masterIncomingPacketHandlers.AddHandler(CMSG_QUESTGIVER_HELLO, "gossip hello");
    masterIncomingPacketHandlers.AddHandler(CMSG_ACTIVATETAXI, "activate taxi");
    masterIncomingPacketHandlers.AddHandler(CMSG_ACTIVATETAXIEXPRESS, "activate taxi");
    masterIncomingPacketHandlers.AddHandler(CMSG_TAXICLEARALLNODES, "taxi done");
    masterIncomingPacketHandlers.AddHandler(CMSG_TAXICLEARNODE, "taxi done");
    masterIncomingPacketHandlers.AddHandler(CMSG_GROUP_UNINVITE, "uninvite");
    masterIncomingPacketHandlers.AddHandler(CMSG_GROUP_UNINVITE_GUID, "uninvite guid");
    masterIncomingPacketHandlers.AddHandler(CMSG_LFG_TELEPORT, "lfg teleport");
    masterIncomingPacketHandlers.AddHandler(CMSG_CAST_SPELL, "see spell");
    masterIncomingPacketHandlers.AddHandler(CMSG_REPOP_REQUEST, "release spirit");
    masterIncomingPacketHandlers.AddHandler(CMSG_RECLAIM_CORPSE, "revive from corpse");

    botOutgoingPacketHandlers.AddHandler(SMSG_PETITION_SHOW_SIGNATURES, "petition offer");
    botOutgoingPacketHandlers.AddHandler(SMSG_GROUP_INVITE, "group invite");
    botOutgoingPacketHandlers.AddHandler(SMSG_GUILD_INVITE, "guild invite");
    botOutgoingPacketHandlers.AddHandler(BUY_ERR_NOT_ENOUGHT_MONEY, "not enough money");
    botOutgoingPacketHandlers.AddHandler(BUY_ERR_REPUTATION_REQUIRE, "not enough reputation");
    botOutgoingPacketHandlers.AddHandler(SMSG_GROUP_SET_LEADER, "group set leader");
    botOutgoingPacketHandlers.AddHandler(SMSG_FORCE_RUN_SPEED_CHANGE, "check mount state");
    botOutgoingPacketHandlers.AddHandler(SMSG_RESURRECT_REQUEST, "resurrect request");
    botOutgoingPacketHandlers.AddHandler(SMSG_INVENTORY_CHANGE_FAILURE, "cannot equip");
    botOutgoingPacketHandlers.AddHandler(SMSG_TRADE_STATUS, "trade status");
    botOutgoingPacketHandlers.AddHandler(SMSG_TRADE_STATUS_EXTENDED, "trade status extended");
    botOutgoingPacketHandlers.AddHandler(SMSG_LOOT_RESPONSE, "loot response");
    botOutgoingPacketHandlers.AddHandler(SMSG_ITEM_PUSH_RESULT, "item push result");
    botOutgoingPacketHandlers.AddHandler(SMSG_LOOT_ROLL_WON, "loot roll won");
    botOutgoingPacketHandlers.AddHandler(SMSG_PARTY_COMMAND_RESULT, "party command");
    botOutgoingPacketHandlers.AddHandler(SMSG_LEVELUP_INFO, "levelup");
    botOutgoingPacketHandlers.AddHandler(SMSG_LOG_XPGAIN, "xpgain");
    botOutgoingPacketHandlers.AddHandler(SMSG_CAST_FAILED, "cast failed");
    botOutgoingPacketHandlers.AddHandler(SMSG_DUEL_REQUESTED, "duel requested");
    botOutgoingPacketHandlers.AddHandler(SMSG_INVENTORY_CHANGE_FAILURE, "inventory change failure");
    botOutgoingPacketHandlers.AddHandler(SMSG_BATTLEFIELD_STATUS, "bg status");
    botOutgoingPacketHandlers.AddHandler(SMSG_LFG_ROLE_CHECK_UPDATE, "lfg role check");
    botOutgoingPacketHandlers.AddHandler(SMSG_LFG_PROPOSAL_UPDATE, "lfg proposal");
    botOutgoingPacketHandlers.AddHandler(SMSG_TEXT_EMOTE, "receive text emote");
    botOutgoingPacketHandlers.AddHandler(SMSG_EMOTE, "receive emote");
    botOutgoingPacketHandlers.AddHandler(SMSG_LOOT_START_ROLL, "master loot roll");
    botOutgoingPacketHandlers.AddHandler(SMSG_ARENA_TEAM_INVITE, "arena team invite");
    botOutgoingPacketHandlers.AddHandler(SMSG_GROUP_DESTROYED, "group destroyed");
    botOutgoingPacketHandlers.AddHandler(SMSG_GROUP_LIST, "group list");

    masterOutgoingPacketHandlers.AddHandler(SMSG_PARTY_COMMAND_RESULT, "party command");
    masterOutgoingPacketHandlers.AddHandler(MSG_RAID_READY_CHECK, "ready check");
    masterOutgoingPacketHandlers.AddHandler(MSG_RAID_READY_CHECK_FINISHED, "ready check finished");
    masterOutgoingPacketHandlers.AddHandler(SMSG_QUESTGIVER_OFFER_REWARD, "questgiver quest details");

    // quest packet
    masterIncomingPacketHandlers.AddHandler(CMSG_QUESTGIVER_COMPLETE_QUEST, "complete quest");
    masterIncomingPacketHandlers.AddHandler(CMSG_QUESTGIVER_ACCEPT_QUEST, "accept quest");
    masterIncomingPacketHandlers.AddHandler(CMSG_QUEST_CONFIRM_ACCEPT, "confirm quest");
    masterIncomingPacketHandlers.AddHandler(CMSG_PUSHQUESTTOPARTY, "quest share");
    botOutgoingPacketHandlers.AddHandler(SMSG_QUESTUPDATE_COMPLETE, "quest update complete");
    botOutgoingPacketHandlers.AddHandler(SMSG_QUESTUPDATE_ADD_KILL, "quest update add kill");
    // SMSG_QUESTUPDATE_ADD_ITEM no longer used
    // botOutgoingPacketHandlers.AddHandler(SMSG_QUESTUPDATE_ADD_ITEM, "quest update add item");
    botOutgoingPacketHandlers.AddHandler(SMSG_QUEST_CONFIRM_ACCEPT, "confirm quest");
}

PlayerbotAI::~PlayerbotAI()
{
    for (uint8 i = 0; i < BOT_STATE_MAX; i++)
    {
        if (engines[i])
            delete engines[i];
    }

    if (aiObjectContext)
        delete aiObjectContext;

    if (bot)
        PlayerbotsMgr::instance().RemovePlayerBotData(bot->GetGUID(), true);
}

namespace
{
// Deeper than this under a point counts as open air. Matches the test harness' DirectLineOverGround.
constexpr float GROUND_SUPPORT_DROP = 6.0f;
// [nav24] a straight leg that rises more than this climbs: each of its samples needs a navmesh polygon within this
// height (the navmesh's walkable climb with its detail mesh; a leg through a hill or a tower wall has none)
constexpr float CLIMB_SUPPORT = 3.0f;
constexpr float CLIMB_SUPPORT_RADIUS = 1.5f;  // the polygon search box's half width round a sample
constexpr float GROUND_SAMPLE_STEP = 2.0f;
constexpr uint32 GROUND_MAX_SAMPLES = 16;
constexpr float GROUND_MIN_TRUNCATED_MOVE = 2.0f;
constexpr uint32 AIRBORNE_LOG_INTERVAL_MS = 30 * IN_MILLISECONDS;
}  // namespace

bool PlayerbotAI::HasGroundSupport(WorldObject const* object, float x, float y, float z)
{
    // Liquid counts: a bot swims or water-walks on it. Includes gameobject floors (dynamic tree).
    float const level = object->GetMapWaterOrGroundLevel(x, y, z);
    if (level > INVALID_HEIGHT && z - level <= GROUND_SUPPORT_DROP)
        return true;
    // [stuck8] Map::GetWaterOrGroundLevel looks the liquid up at the floor's height; over a deep sea floor that
    // lookup can miss the water (GridTerrainData: a z under the grid's ground is "no water") and returns the floor
    // tens of yards down. The liquid at the point itself: b341/b343 Shalaeth, Relara L1 in the sea off Auberdine,
    // (6664,662,-1.9), every 100 yd swim to the pier "refused a straight move over open air", 1184 s STUCK
    LiquidData const liquid =
        object->GetMap()->GetLiquidData(object->GetPhaseMask(), x, y, z, object->GetCollisionHeight(), {});
    return liquid.Status != LIQUID_MAP_NO_WATER && liquid.Level > INVALID_HEIGHT &&
           z - liquid.Level <= GROUND_SUPPORT_DROP;
}

bool PlayerbotAI::StraightLineSupported(WorldObject const* object, float fromX, float fromY, float fromZ, float toX,
                                        float toY, float toZ)
{
    G3D::Vector3 const start(fromX, fromY, fromZ);
    G3D::Vector3 const end(toX, toY, toZ);
    float const length = (end - start).length();
    uint32 const samples =
        std::min(GROUND_MAX_SAMPLES, std::max<uint32>(1, static_cast<uint32>(std::ceil(length / GROUND_SAMPLE_STEP))));
    for (uint32 i = 1; i <= samples; ++i)
    {
        G3D::Vector3 const point = start + (end - start) * (static_cast<float>(i) / static_cast<float>(samples));
        if (!HasGroundSupport(object, point.x, point.y, point.z))
            return false;
    }
    return true;
}

float PlayerbotAI::GroundSampleStep() { return GROUND_SAMPLE_STEP; }

bool PlayerbotAI::IsSubjectToGravity() const
{
    return bot->IsAlive() && !bot->GetVehicle() && !bot->GetTransport() && !bot->IsInFlight() && !bot->CanFly() &&
           !bot->HasFlyAura() && !bot->HasIncreaseMountedFlightSpeedAura();
}

// PathGenerator gives a Player a straight shortcut when the destination has no navmesh polygon under it
// (PathGenerator::BuildPolyPath: "creature ? creature->CanFly() : true"), because a client applies gravity to it.
// A bot has no client: it walked that line through the air (Oculus wave 7: Drakos' island z 360 to Ring 2 z 433,
// wave 6: up to a flying Azure Ring Guardian), and the next dismount let it fall to the bottom of the instance.
void PlayerbotAI::GuardAirborneSpline()
{
    if (master == bot)
        return;  // a SelfBot has a game client, which applies gravity itself

    Movement::MoveSpline const* spline = bot->movespline;
    if (!spline->Initialized() || spline->Finalized() || spline->GetId() == checkedSplineId)
        return;

    checkedSplineId = spline->GetId();
    if (spline->onTransport || spline->isFalling() || spline->isBoarding() || spline->isCyclic() ||
        fallPending || !IsSubjectToGravity() ||
        bot->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE)  // jumps, fear, ...
        return;

    // A navmesh path of several points stays on the mesh; only a single straight segment can leave the ground.
    // [b30_topped_up] ... and the first leg of a path from a spot off the navmesh: PathGenerator snaps the start to
    // the nearest polygon, the spline runs from where the bot stands straight to it. dmw-n 20261025-184229-dmw-n-5C-16:
    // the healer on a perch over Immol'thar's spawn (-39.4,812.5,-11.6, no polygon; his Portal of Immol'thar) had
    // every drop refused by the ledge guard for 4 minutes, then a follow to the leader 380 yd off (higher up: no
    // ledge check) took a path whose first leg glided 51 yd through the air into his pit (a navmesh region with no
    // way out), stranded for good. The first leg is sampled when it descends more than GROUND_SUPPORT_DROP.
    Movement::MoveSpline::MySpline const& path = spline->_Spline();
    if (path.last() - path.first() < 1)
        return;
    bool const single = path.last() - path.first() == 1;
    G3D::Vector3 start = path.getPoint(path.first());
    G3D::Vector3 end = path.getPoint(single ? path.last() : path.first() + 1);
    // [nav24] the navmesh owner 2026-11-09: a straight leg that CLIMBS was never checked - the guard below looks only
    // at descents and asks for ground within 6 yd under each sample, which a leg rising through a hillside or a
    // tower always has. The server spline has no collision: the bot walked up through the hill onto the Shadowglen
    // ridge, through the Dolanaar inn tower onto its roof (46.9 yd over the terrain), up into the Exodar. A climbing
    // single leg (the core's straight shortcut), a climbing tail (an incomplete path's straight end) or a climbing
    // first leg from a spot off the navmesh needs a navmesh polygon under every sample; it is cut at the last one
    if (dtNavMeshQuery const* query = bot->GetMap()->GetMapCollisionData().GetMMapData().GetNavMeshQuery())
    {
        dtQueryFilter filter;
        filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP));
        auto const onMesh = [&](G3D::Vector3 const& p, G3D::Vector3* at)
        {
            float const point[3] = {p.y, p.z, p.x};
            float const extents[3] = {CLIMB_SUPPORT_RADIUS, CLIMB_SUPPORT, CLIMB_SUPPORT_RADIUS};
            float nearest[3] = {0.0f, 0.0f, 0.0f};
            dtPolyRef ref = 0;
            if (dtStatusFailed(query->findNearestPoly(point, extents, &filter, &ref, nearest)) || !ref)
                return false;
            if (at)
                *at = G3D::Vector3(nearest[2], nearest[0], nearest[1]);
            return true;
        };
        // [swim28] a point under a liquid's surface is swum, not climbed: the navmesh holds water as its surface
        // polygons only, so a dive's way up (the sea floor to the surface) has no polygon under its samples. b377
        // Dekorhan L12 killed a crawler on the sea floor off Auberdine (6419,634,-8.3): every leg up to the surface
        // was refused "a climbing straight leg ... no navmesh polygon at (6419.1,634.1,-6.6)", MOVE_FAR_LOOP, HEARTH
        auto const InLiquid = [this](G3D::Vector3 const& p)
        {
            LiquidData const liquid =
                bot->GetMap()->GetLiquidData(bot->GetPhaseMask(), p.x, p.y, p.z, bot->GetCollisionHeight(), {});
            return liquid.Status != LIQUID_MAP_NO_WATER && liquid.Level > INVALID_HEIGHT && liquid.Level >= p.z;
        };
        G3D::Vector3 a, b;
        bool check = false;
        bool climbTail = false;
        if (single)
        {
            a = start;
            b = end;
            check = b.z - a.z > CLIMB_SUPPORT;
        }
        else
        {
            G3D::Vector3 const tailA = path.getPoint(path.last() - 1);
            G3D::Vector3 const tailB = path.getPoint(path.last());
            if (end.z - start.z > CLIMB_SUPPORT && !onMesh(start, nullptr))
            {
                a = start;
                b = end;
                check = true;
            }
            else if (tailB.z - tailA.z > CLIMB_SUPPORT)
            {
                a = tailA;
                b = tailB;
                check = true;
                climbTail = true;
            }
        }
        if (check)
        {
            float const length = (b - a).length();
            uint32 const samples = std::min(GROUND_MAX_SAMPLES,
                                            std::max<uint32>(1, static_cast<uint32>(std::ceil(length / GROUND_SAMPLE_STEP))));
            G3D::Vector3 lastOn = a;
            bool cut = false;
            bool swum = false;
            G3D::Vector3 bad;
            for (uint32 i = 1; i <= samples && !cut; ++i)
            {
                G3D::Vector3 const point = a + (b - a) * (static_cast<float>(i) / static_cast<float>(samples));
                G3D::Vector3 snapped;
                if (onMesh(point, &snapped))
                    lastOn = snapped;
                else if (InLiquid(point))
                {
                    if (!swum)
                        LOG_INFO("playerbots", "[swim28] {} swims up on map {} from ({:.1f},{:.1f},{:.1f}) to "
                                 "({:.1f},{:.1f},{:.1f}): under the water at ({:.1f},{:.1f},{:.1f}), no polygon needed",
                                 bot->GetName(), bot->GetMapId(), a.x, a.y, a.z, b.x, b.y, b.z, point.x, point.y,
                                 point.z);
                    swum = true;
                    lastOn = point;  // [swim28] swims up through the water
                }
                else
                {
                    cut = true;
                    bad = point;
                }
            }
            if (cut)
            {
                uint32 const now = getMSTime();
                if (!lastAirborneLogMs || getMSTimeDiff(lastAirborneLogMs, now) > AIRBORNE_LOG_INTERVAL_MS)
                {
                    lastAirborneLogMs = now;
                    LOG_INFO("playerbots", "[nav24] {} refused a climbing straight leg on map {} from ({:.1f},{:.1f},"
                             "{:.1f}) to ({:.1f},{:.1f},{:.1f}) (+{:.1f} yd): no navmesh polygon at ({:.1f},{:.1f},{:.1f})",
                             bot->GetName(), bot->GetMapId(), a.x, a.y, a.z, b.x, b.y, b.z, b.z - a.z, bad.x, bad.y,
                             bad.z);
                }
                bot->GetMotionMaster()->Clear();
                bot->StopMoving();
                if (climbTail || (lastOn - a).length() >= GROUND_MIN_TRUNCATED_MOVE)
                    bot->GetMotionMaster()->MovePoint(0, lastOn.x, lastOn.y, lastOn.z, FORCED_MOVEMENT_NONE, 0.0f,
                                                      0.0f, climbTail, true);
                return;
            }
        }
    }
    // [nav29] a straight leg under the surface: the server spline has no collision, so a straight leg that keeps
    // its height where the ground rises (a slope, a cliff, a floor up a step) runs inside the terrain or under the
    // floor - b377 Aestymro L13 7 yd inside a Westfall cliff ("ground -200000"), b378 Relara L10 2.5-3.5 yd under the
    // Dolanaar inn's floor. A leg whose sample lies more than the navmesh's walkableClimb under the polygon over it
    // (none nearer below; water is swum) is refused as [nav24] refuses a climbing one, cut at its last sample above
    if (dtNavMeshQuery const* query = bot->GetMap()->GetMapCollisionData().GetMMapData().GetNavMeshQuery())
    {
        dtNavMesh const* mesh = query->getAttachedNavMesh();
        dtQueryFilter filter;
        filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP));
        auto const underSurface = [&](G3D::Vector3 const& p) -> bool
        {
            LiquidData const liquid =
                bot->GetMap()->GetLiquidData(bot->GetPhaseMask(), p.x, p.y, p.z, bot->GetCollisionHeight(), {});
            if (liquid.Status != LIQUID_MAP_NO_WATER && liquid.Level > INVALID_HEIGHT && liquid.Level >= p.z)
                return false;  // swims
            // [nav31] the surface heights exactly at the sample's x, y (each polygon of the column that holds the
            // point, its detail height): under the surface only when one lies more than walkableClimb over it and
            // none at or under that line. [nav29] took the nearest polygon point in a 1.5 yd box - on a slope its
            // height is the slope's 1.5 yd further on (b384: 47 refusals of plain slope steps, Gnetlibozz
            // (-5993.5,-2770.8,405.2), Septimon (-5651.5,-76.9,418.1) twice)
            float const point[3] = {p.y, p.z, p.x};
            float const extents[3] = {0.1f, 30.0f, 0.1f};
            constexpr int COLUMN_POLYS = 16;  // search bound, not a behaviour rule
            dtPolyRef refs[COLUMN_POLYS];
            int count = 0;
            if (!mesh || dtStatusFailed(query->queryPolygons(point, extents, &filter, refs, &count, COLUMN_POLYS)) ||
                count <= 0)
                return false;  // no polygon round it: the off-navmesh guards' case
            float climb = 0.0f;
            bool over = false;
            bool atOrUnder = false;
            for (int i = 0; i < count; ++i)
            {
                float height = 0.0f;
                if (dtStatusFailed(query->getPolyHeight(refs[i], point, &height)))
                    continue;  // the column box touches it, the point lies off it
                dtMeshTile const* tile = nullptr;
                dtPoly const* poly = nullptr;
                if (dtStatusFailed(mesh->getTileAndPolyByRef(refs[i], &tile, &poly)) || !tile || !tile->header)
                    continue;
                climb = tile->header->walkableClimb;
                if (height - p.z > climb)
                    over = true;
                else
                    atOrUnder = true;
            }
            return over && !atOrUnder;
        };
        auto const checkLeg = [&](G3D::Vector3 const& a, G3D::Vector3 const& b) -> bool
        {
            // a leg from a spot already under the surface is its way out: not judged (it would hold it there);
            // [nav31] nor a step of a sample or two (the spline's own step onto the next point of a slope)
            if ((b - a).length() <= 2.0f * GROUND_SAMPLE_STEP || underSurface(a))
                return true;
            float const length = (b - a).length();
            uint32 const samples = std::min(
                GROUND_MAX_SAMPLES, std::max<uint32>(1, static_cast<uint32>(std::ceil(length / GROUND_SAMPLE_STEP))));
            G3D::Vector3 lastAbove = a;
            for (uint32 i = 1; i <= samples; ++i)
            {
                G3D::Vector3 const point = a + (b - a) * (static_cast<float>(i) / static_cast<float>(samples));
                if (!underSurface(point))
                {
                    lastAbove = point;
                    continue;
                }
                std::string mover;
                if (Engine* engine = currentEngine)
                {
                    std::string const actions = engine->GetLastAction();
                    size_t const bar = actions.rfind('|');
                    mover = bar == std::string::npos ? actions : actions.substr(bar + 1);
                }
                LOG_INFO("playerbots", "[nav29] {} refused a leg under the surface at ({:.1f},{:.1f},{:.1f}) on map {}: "
                         "from ({:.1f},{:.1f},{:.1f}) to ({:.1f},{:.1f},{:.1f}), mover {}",
                         bot->GetName(), point.x, point.y, point.z, bot->GetMapId(), a.x, a.y, a.z, b.x, b.y, b.z,
                         mover.empty() ? std::string("?") : mover);
                bot->GetMotionMaster()->Clear();
                bot->StopMoving();
                if ((lastAbove - a).length() >= GROUND_MIN_TRUNCATED_MOVE)
                    bot->GetMotionMaster()->MovePoint(0, lastAbove.x, lastAbove.y, lastAbove.z, FORCED_MOVEMENT_NONE,
                                                      0.0f, 0.0f, false, true);
                return false;
            }
            return true;
        };
        if (single)
        {
            if (!checkLeg(start, end))
                return;
        }
        else
        {
            // a navmesh path's own legs lie on its polygons; its last leg may be an incomplete path's straight tail
            G3D::Vector3 const tailA = path.getPoint(path.last() - 1);
            G3D::Vector3 const tailB = path.getPoint(path.last());
            if (!checkLeg(tailA, tailB))
                return;
        }
    }
    // [b32_occ] ... and the last leg: a path whose end polygon is not the destination's (PATHFIND_INCOMPLETE to a
    // spot over open air) runs on the mesh to the rim, then the core's straight tail to the destination. occ-h
    // 20261025-221216-occ-h-5G-22 (496 s): the tank's walk from Ring 2 (z 433) ended at (1171.1,1041.2,105.1),
    // the floor under a flying Azure Ring Guardian; the first leg stayed on the ring, the tail took it down
    // through the air to z 363, 70 yd under its group. The tail is sampled when it descends more than
    // GROUND_SUPPORT_DROP; a cut walks the path's own (pathed) way to the rim.
    bool tail = false;
    if (!single && start.z - end.z <= GROUND_SUPPORT_DROP)
    {
        G3D::Vector3 const tailStart = path.getPoint(path.last() - 1);
        G3D::Vector3 const tailEnd = path.getPoint(path.last());
        if (tailStart.z - tailEnd.z <= GROUND_SUPPORT_DROP ||
            !HasGroundSupport(bot, tailStart.x, tailStart.y, tailStart.z))
            return;
        start = tailStart;
        end = tailEnd;
        tail = true;
    }
    if (!HasGroundSupport(bot, start.x, start.y, start.z))
    {
        // Already in the air (a corpse revived in mid-air, a seat left above the void): a client would fall.
        // [navgap] user 2026-11-09: the alarm, with what stood under it (a transport gone, no ground)
        {
            GameObject* car = bot->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_TRANSPORT, 30.0f);
            LOG_ERROR("playerbots", "[navgap] {} L{} map {} ({:.1f},{:.1f},{:.1f}) zone {} in the air at a walk's "
                      "start: falls; cause: ground {:.1f}{}",
                      bot->GetName(), bot->GetLevel(), bot->GetMapId(), start.x, start.y, start.z, bot->GetZoneId(),
                      bot->GetMapWaterOrGroundLevel(start.x, start.y, start.z),
                      car ? Acore::StringFormat(", by moving transport {} {:.0f} yd off, not its passenger",
                                                car->GetEntry(), bot->GetExactDist(car))
                          : std::string());
        }
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        StartFall();
        return;
    }

    float const length = (end - start).length();
    uint32 const samples =
        std::min(GROUND_MAX_SAMPLES, std::max<uint32>(1, static_cast<uint32>(std::ceil(length / GROUND_SAMPLE_STEP))));
    G3D::Vector3 lastSupported = start;
    for (uint32 i = 1; i <= samples; ++i)
    {
        G3D::Vector3 const point = start + (end - start) * (static_cast<float>(i) / static_cast<float>(samples));
        if (HasGroundSupport(bot, point.x, point.y, point.z))
        {
            lastSupported = point;
            continue;
        }

        uint32 const now = getMSTime();
        if (!lastAirborneLogMs || getMSTimeDiff(lastAirborneLogMs, now) > AIRBORNE_LOG_INTERVAL_MS)
        {
            lastAirborneLogMs = now;
            // [stuck8] and the sample without support, with the level found under it (the cause in the log)
            LOG_INFO("playerbots", "{} refused a straight move over open air on map {} from ({:.1f}, {:.1f}, {:.1f}) "
                     "to ({:.1f}, {:.1f}, {:.1f}): no ground at ({:.1f}, {:.1f}, {:.1f}), water or ground level {:.1f}",
                     bot->GetName(), bot->GetMapId(), start.x, start.y, start.z, end.x, end.y, end.z, point.x,
                     point.y, point.z, bot->GetMapWaterOrGroundLevel(point.x, point.y, point.z));
        }

        // Walk as far as the ground goes, onto the ground, like a player stopping at the edge.
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        if (tail || (lastSupported - start).length() >= GROUND_MIN_TRUNCATED_MOVE)
        {
            // lastSupported has ground or liquid within GROUND_SUPPORT_DROP, so the level is valid.
            float const groundZ = bot->GetMapWaterOrGroundLevel(lastSupported.x, lastSupported.y, lastSupported.z);
            // [b32_occ] a cut tail: the mesh path to its rim point (the legs before it bend), not a straight line
            bot->GetMotionMaster()->MovePoint(0, lastSupported.x, lastSupported.y, groundZ, FORCED_MOVEMENT_NONE,
                                              0.0f, 0.0f, tail, true);
        }
        return;
    }
}

// Player::HandleFall runs on MSG_MOVE_FALL_LAND, a client opcode bots never send: the fall damage is applied here,
// where the fall spline ends, so the bot dies (and leaves its corpse) on the ground like a player.
void PlayerbotAI::StartFall()
{
    if (fallPending || !IsSubjectToGravity())
        return;

    float const startZ = bot->GetPositionZ();
    bot->GetMotionMaster()->MoveFall();  // for a player: MOVEMENTFLAG_FALLING and m_lastFallZ, then the spline
    if (!bot->HasUnitMovementFlag(MOVEMENTFLAG_FALLING))
        return;  // the ground is right here, or no ground at all below (MoveFall gave up)

    bot->SetFallInformation(0, startZ);
    fallPending = true;
    // [nav24] a drop under the core's fall-damage height (Player::HandleFall MIN_FALL_DMG_DIST: 13.48 yd, plus a Safe Fall aura's
    // yards) is a step, not a fall: b350-b372's "repeated falls" were dismount micro-falls of 0.1-1.3 yd onto the
    // floor under the mount (CheckMountStateAction -> RequestFallCheck), counted as falls and navmesh gaps
    constexpr float FALL_DAMAGE_HEIGHT = 13.48f;
    float const ground = bot->GetMapWaterOrGroundLevel(bot->GetPositionX(), bot->GetPositionY(), startZ);
    float const safe = float(bot->GetTotalAuraModifier(SPELL_AURA_SAFE_FALL));
    if (ground > INVALID_HEIGHT && startZ - ground < FALL_DAMAGE_HEIGHT + safe)
        return;
    LOG_INFO("playerbots", "{} falls from ({:.1f}, {:.1f}, {:.1f}) on map {} ({:.1f} yd)", bot->GetName(),
             bot->GetPositionX(), bot->GetPositionY(), startZ, bot->GetMapId(),
             ground > INVALID_HEIGHT ? startZ - ground : 0.0f);
}

void PlayerbotAI::UpdateFall()
{
    if (master == bot)
        return;  // a SelfBot's game client reports its own falls (MSG_MOVE_FALL_LAND)

    // Leaving a seat puts the passenger at the vehicle's position, in the air for a flying vehicle.
    bool const onVehicle = bot->GetVehicle() != nullptr;
    if (wasOnVehicle && !onVehicle)
        fallCheckRequested = true;
    wasOnVehicle = onVehicle;

    Movement::MoveSpline const* spline = bot->movespline;
    bool const splineActive = spline->Initialized() && !spline->Finalized();
    if (fallCheckRequested && !fallPending && !(splineActive && spline->isBoarding()))
    {
        fallCheckRequested = false;
        StartFall();
        return;  // the fall spline (if any) is checked from the next tick on
    }

    if (!fallPending)
        return;

    if (!bot->IsAlive())
    {
        fallPending = false;
        bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
        return;
    }

    if (splineActive && spline->isFalling())
        return;

    // Landed, or the fall was cut short by another movement: the damage counts from m_lastFallZ to here.
    fallPending = false;
    MovementInfo landing = bot->m_movementInfo;
    landing.pos.Relocate(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    // A client landing in water takes no fall damage (it sends no MSG_MOVE_FALL_LAND damage there either).
    // Player::IsInWater is the client's swim flag, never set for a bot: the liquid at the landing spot decides.
    LiquidData const& liquid = bot->GetLiquidData();
    bool const swimming = (liquid.Status & MAP_LIQUID_STATUS_SWIMMING) &&
                          liquid.Level - bot->GetPositionZ() >= bot->GetCollisionHeight() * 0.5f;
    if (!swimming)
        bot->HandleFall(landing);
    bot->SetFallInformation(0, bot->GetPositionZ());
    bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
}

void PlayerbotAI::UpdateAI(uint32 elapsed, bool minimal)
{
    // Handle the AI check delay
    if (nextAICheckDelay > elapsed)
        nextAICheckDelay -= elapsed;
    else
        nextAICheckDelay = 0;

    // Early return if bot is in invalid state
    if (!bot || !bot->GetSession() || !bot->IsInWorld() || bot->IsBeingTeleported() ||
        bot->GetSession()->IsLoggingOut() || bot->IsDuringRemoveFromWorld())
        return;

    MapWorkerTrace::SetBot(bot->GetGUID().GetRawValue());

    // Bots send no movement opcodes, so m_lastFallZ stays frozen and Player::IsFalling() (a Z test
    // against it) blocks LFG teleports. Unit::IsFalling() is the flag test, so real falls keep theirs.
    if (!bot->Unit::IsFalling())
        bot->SetFallInformation(0, bot->GetPositionZ());

    // [guild8] stages before the engine, so a freeze trace names the step (b288 18:29: 60 s hang, stage empty)
    MapWorkerTrace::SetStage("pre: fall", nullptr);
    UpdateFall();
    GuardAirborneSpline();

    // [b44_st] Charmed by an enemy (Hex of Jammal'an, Mind Control, Incite Chaos): the core's charm AI drives the bot
    // as it drives a player's character, who has no control either; the bot's own engine would cast and move against
    // it (backlog wave 11 05:35; st-h 20261026-075135-st-h-5F-7: Rthunac ran "dps assist" / "chase" under the charm).
    if (!bot->GetVehicle() && bot->IsCharmed() &&
        (bot->HasAuraType(SPELL_AURA_MOD_CHARM) || bot->HasAuraType(SPELL_AURA_AOE_CHARM) ||
         bot->HasAuraType(SPELL_AURA_MOD_POSSESS)))
        return;

    // [guild15] the way it came, for a survival run (one squared distance per tick)
    if (!bot->IsInCombat() && bot->IsAlive() && !bot->GetTransport() && !bot->IsInFlight())
        rpgInfo.NoteTrail(bot->GetMapId(), G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()),
                          sWorld->getFloatConfig(CONFIG_CREATURE_LEASH_RADIUS));

    // No health/mana/power refills: bots regenerate like players (user decision 2026-10-23, no cheats).

    AllowActivity();

    // Wake up if combat state changed (unless casting).
    bool isCasting = bot->IsNonMeleeSpellCast(true);
    if (bot->IsInCombat())
    {
        if (!inCombat)
        {
            // "attackers" is cached for 1 s; refresh it so the assist actions see the new attacker
            // this tick.
            aiObjectContext->GetValue<GuidVector>("attackers")->Reset();
            if (!isCasting)
                ResetActionDuration();
        }

        inCombat = true;
    }
    else
    {
        if (inCombat && !isCasting)
            ResetActionDuration();

        inCombat = false;
    }

    bool doMinimalReaction = minimal || !AllowActivity(REACT_ACTIVITY);
    MapWorkerTrace::SetStage("pre: reaction", nullptr);
    if (UpdateAIReaction(elapsed, doMinimalReaction, !bot->IsInFlight()))
        return;

    MapWorkerTrace::SetStage("pre: spell", nullptr);
    if (!CanUpdateAI())
        return;

    // Handle a spell that is still in its preparing phase (including channeled spells).
    Spell* currentSpell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!currentSpell)
        currentSpell = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);

    if (currentSpell)
    {
        // [lead573] b215 rfc-h 5D 512.9-514.1 s: the leader moved the arcane mage's Attack from a Searing Blade Cultist
        // it had no more threat headroom on to an Enforcer; its Arcane Blast on the Cultist was still casting, landed
        // and took it (5F 38.1-38.4 s the warlock's Shadow Bolt on a Molten Elemental the same way). The leader names
        // the mobs a led member may hit (its Attack target, the kill order it has headroom on, its interrupt / CC /
        // flee stop / pick-up / dispel targets) and its DontAttack mob is hit by nobody: a hostile cast or channel of
        // a led member (not the tank, which owns its threat work) on a mob outside that, or on the DontAttack mob,
        // stops - the command is the leader's, an older cast does not outlive it
        if (Leader::LEADER_ATTACK_EVENTS && bot->IsInCombat() && Leader::ExecutorMember(bot))
        {
            Leader::MemberEvents events;
            SpellInfo const* castInfo = currentSpell->GetSpellInfo();
            Unit* castOn = currentSpell->m_targets.GetUnitTarget();
            if (castInfo && castOn && castOn != bot && !castOn->GetGUID().IsPlayer() && castOn->IsAlive() &&
                !castInfo->IsPositive() && bot->IsHostileTo(castOn) && castOn->GetOwnerGUID() != bot->GetGUID() &&
                Leader::Events::Get(bot->GetGUID(), events))
            {
                ObjectGuid const on = castOn->GetGUID();
                bool const named = on == events.target || on == events.interrupt || on == events.cc ||
                                   on == events.fleeStop || on == events.threatTarget || on == events.dispelTarget ||
                                   std::find(events.killOrder.begin(), events.killOrder.end(), on) !=
                                       events.killOrder.end();
                bool const barred = (!events.dontAttack.IsEmpty() && on == events.dontAttack && on != events.cc &&
                                     on != events.interrupt) ||
                                    (events.attack == Leader::AttackEvent::Attack && !named);
                if (barred)
                {
                    B44CastTrace(bot, "stop:leader target", castInfo, castOn);  // [b44_cast]
                    LOG_INFO("playerbots", "[lead573] {} stops {} on {} {}: {}", bot->GetName(),
                             castInfo->SpellName[0], castOn->GetName(), castOn->GetEntry(),
                             on == events.dontAttack ? "the leader's DontAttack mob"
                                                     : "the leader's Attack no longer names it");
                    bot->InterruptSpell(currentSpell->GetCurrentContainer());
                    YieldThread(bot, GetReactDelay());
                    return;
                }
            }
        }

        // [lead870] b224 rfc-h: 53 of the healers' 65 "no tank threat on it" / "heal threat over the tank's" losses
        // came within 4 s of the leader's Heal gate closing (triage heal_gate: "cast in flight"; 5B 602.9 s: Heal
        // "every mob the heals reach is on the tank" with 0 mobs, 603.2 DontAttack "Searing Blade Cultist is not on
        // the tank with the tank's threat", the holy paladin's heal begun at 602.9 landed after the pull started and
        // its threat - split by the game over every mob with the healed member on its threat list
        // (ThreatManager::ForwardThreatForAssistingMe) - put it at 34.3 on four mobs the warrior had 0 on: all four
        // went to it). The leader's Heal is the only command a led healer heals under in a fight (lead61, the member
        // gate blocks every new heal without it); a heal cast or channel begun under it does not outlive it either -
        // like lead573's hostile cast, it stops when the command no longer allows it (the leader's emergency heal is
        // a Heal, an External it names stays)
        if (Leader::LEADER_HEAL_AFTER_THREAT && bot->IsInCombat() && Leader::ExecutorMember(bot) && IsHeal(bot))
        {
            Leader::MemberEvents events;
            SpellInfo const* castInfo = currentSpell->GetSpellInfo();
            auto const heals = [](SpellInfo const* info)
            {
                for (SpellEffectInfo const& effect : info->Effects)
                    if (effect.Effect == SPELL_EFFECT_HEAL || effect.Effect == SPELL_EFFECT_HEAL_PCT ||
                        effect.Effect == SPELL_EFFECT_HEAL_MAX_HEALTH ||
                        (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL))
                        return true;
                return false;
            };
            bool healCast = castInfo && castInfo->IsPositive() && heals(castInfo);
            // a channel heals through the spell its ticks trigger (Penance, Divine Hymn, Tranquility)
            if (castInfo && castInfo->IsPositive() && !healCast)
                for (SpellEffectInfo const& effect : castInfo->Effects)
                    if (effect.Effect == SPELL_EFFECT_APPLY_AURA &&
                        effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL && effect.TriggerSpell)
                        if (SpellInfo const* triggered = sSpellMgr->GetSpellInfo(effect.TriggerSpell);
                            triggered && heals(triggered))
                            healCast = true;
            if (healCast && Leader::Events::Get(bot->GetGUID(), events) &&
                events.attack != Leader::AttackEvent::Heal && castInfo->Id != events.externalSpell)
            {
                Unit* castOn = currentSpell->m_targets.GetUnitTarget();
                B44CastTrace(bot, "stop:leader heal", castInfo, castOn);  // [b44_cast]
                LOG_INFO("playerbots", "[lead870] {} stops {} on {}: the leader's heals wait (no Heal command)",
                         bot->GetName(), castInfo->SpellName[0], castOn ? castOn->GetName() : std::string("-"));
                bot->InterruptSpell(currentSpell->GetCurrentContainer());
                YieldThread(bot, GetReactDelay());
                return;
            }
        }

        if (currentSpell->getState() == SPELL_STATE_PREPARING)
        {
            // Allow external scripts to interrupt a cast in progress
            if (spellInterruptRequested)
            {
                spellInterruptRequested = false;
                B44CastTrace(bot, "stop:requested", currentSpell->GetSpellInfo(), nullptr);  // [b44_cast]
                bot->InterruptSpell(currentSpell->GetCurrentContainer());
                YieldThread(bot, GetReactDelay());
                return;
            }

            SpellInfo const* spellInfo = currentSpell->GetSpellInfo();
            // Stop-cast to kick: an enemy cast this bot was assigned to interrupt outranks finishing its own,
            // so the engine runs this tick and the assigned interrupt goes out.
            bool const stopCastToInterrupt = InterruptCoordination::ShouldStopCastToInterrupt(this);
            if (stopCastToInterrupt)
            {
                B44CastTrace(bot, "stop:kick", spellInfo, nullptr);  // [b44_cast]
                bot->InterruptSpell(currentSpell->GetCurrentContainer());
            }

            // Standing in a ground effect (Flamestrike) outranks finishing the cast: the engine runs this tick and
            // "avoid aoe" walks the bot out. At most once per DODGE_STOP_CAST_INTERVAL_MS.
            constexpr uint32 DODGE_STOP_CAST_INTERVAL_MS = 2500;
            bool stopCastToDodge = false;
            bool const dodgeAllowed = !lastDodgeStopCastMs ||
                                      getMSTimeDiff(lastDodgeStopCastMs, getMSTime()) >= DODGE_STOP_CAST_INTERVAL_MS;
            if (!stopCastToInterrupt && dodgeAllowed &&
                currentSpell->GetCurrentContainer() == CURRENT_GENERIC_SPELL &&
                AvoidAoeAction::ShouldStopCastToDodge(this))
            {
                stopCastToDodge = true;
                lastDodgeStopCastMs = getMSTime();
                B44CastTrace(bot, "stop:dodge", spellInfo, nullptr);  // [b44_cast]
                bot->InterruptSpell(CURRENT_GENERIC_SPELL);
            }

            if (spellInfo && !stopCastToInterrupt && !stopCastToDodge)
            {
                Unit* spellTarget = currentSpell->m_targets.GetUnitTarget();
                // Interrupt if target is dead or spell can't target dead units
                if (spellTarget && !spellTarget->IsAlive() && !spellInfo->IsAllowingDeadTarget())
                {
                    B44CastTrace(bot, "stop:target dead", spellInfo, spellTarget);  // [b44_cast]
                    bot->InterruptSpell(currentSpell->GetCurrentContainer());
                    YieldThread(bot, GetReactDelay());
                    return;
                }

                GameObject* goSpellTarget = currentSpell->m_targets.GetGOTarget();

                if (goSpellTarget && !goSpellTarget->isSpawned())
                {
                    bot->InterruptSpell(currentSpell->GetCurrentContainer());
                    YieldThread(bot, GetReactDelay());
                    return;
                }

                bool isHeal = false;
                bool isSingleTarget = true;

                for (uint8 i = 0; i < 3; ++i)
                {
                    if (!spellInfo->Effects[i].Effect)
                        continue;

                    // Check if spell is a heal
                    if (spellInfo->Effects[i].Effect == SPELL_EFFECT_HEAL ||
                        spellInfo->Effects[i].Effect == SPELL_EFFECT_HEAL_MAX_HEALTH ||
                        spellInfo->Effects[i].Effect == SPELL_EFFECT_HEAL_MECHANICAL)
                        isHeal = true;

                    // Check if spell is single-target
                    if ((spellInfo->Effects[i].TargetA.GetTarget() &&
                         spellInfo->Effects[i].TargetA.GetTarget() != TARGET_UNIT_TARGET_ALLY) ||
                        (spellInfo->Effects[i].TargetB.GetTarget() &&
                         spellInfo->Effects[i].TargetB.GetTarget() != TARGET_UNIT_TARGET_ALLY))
                    {
                        isSingleTarget = false;
                    }
                }

                // Interrupt if target ally has full health (heal by other member)
                if (isHeal && isSingleTarget && spellTarget && spellTarget->IsFullHealth())
                {
                    bot->InterruptSpell(currentSpell->GetCurrentContainer());
                    YieldThread(bot, GetReactDelay());
                    return;
                }

                // Ensure bot is facing target if necessary
                if (spellTarget && !bot->HasInArc(CAST_ANGLE_IN_FRONT, spellTarget) &&
                    (spellInfo->FacingCasterFlags & SPELL_FACING_FLAG_INFRONT))
                {
                    ServerFacade::instance().SetFacingTo(bot, spellTarget);
                }

                // Wait for spell cast. In combat the bot looks again the moment the cast ends: a react delay of
                // 500-700 ms after every cast left the caster idle a quarter of the fight (C-I-C, wave 2026-10-24).
                UpdateIdleDiag(true);
                uint32 const reactDelay = GetReactDelay();
                int32 const remaining = currentSpell->GetCastTimeRemaining();
                if (bot->IsInCombat() && remaining >= 0 && static_cast<uint32>(remaining) < reactDelay)
                {
                    SetNextCheckDelay(static_cast<uint32>(remaining));
                    return;
                }

                YieldThread(bot, reactDelay);
                return;
            }
        }
    }

    if (spellInterruptRequested)
    {
        // At this point the preparing-cast branch above did not consume the request.
        // Interrupt a current channel if one still exists; otherwise, clear the stale request.
        if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        {
            spellInterruptRequested = false;
            bot->InterruptSpell(CURRENT_CHANNELED_SPELL);
            YieldThread(bot, GetReactDelay());
            return;
        }

        spellInterruptRequested = false;
    }

    MapWorkerTrace::SetStage("pre: transport", nullptr);
    // Handle transport check delay
    if (nextTransportCheck > elapsed)
        nextTransportCheck -= elapsed;
    else
        nextTransportCheck = 0;

    if (!nextTransportCheck)
    {
        nextTransportCheck = 1000;
        Transport* newTransport = bot->GetMap()->GetTransportForPos(bot->GetPhaseMask(), bot->GetPositionX(),
                                                                    bot->GetPositionY(), bot->GetPositionZ(), bot);
        // [guild15] the ray down meets the boat's hull under a pier too: a bot standing on the pier (static ground
        // above the deck the ray found) is off the boat, as a client knows the surface it stands on (b327: bots at
        // the Rut'theran / Auberdine piers stayed passengers and rode back, 10+ round trips)
        // (only while the boat stands at a stop: under way the ray met piers and roots it passed, b329 Bierya was
        // dropped into the sea north of Rut'theran)
        bool docked = false;
        if (MotionTransport* const boat = newTransport ? newTransport->ToMotionTransport() : nullptr)
            if (TravelRouteMgr::Line const* line = sTravelRouteMgr.GetLine(boat->GetEntry()))
                for (TravelRouteMgr::Dock const& dock : line->docks)
                {
                    uint32 remaining = 0;
                    docked = docked || TravelRouteMgr::IsDocked(boat, dock, remaining);
                }
        if (docked && newTransport->m_model)
        {
            float const x = bot->GetPositionX(), y = bot->GetPositionY(), z = bot->GetPositionZ();
            G3D::Ray const ray(G3D::Vector3(x, y, z + 2.0f), G3D::Vector3(0.0f, 0.0f, -1.0f));
            float dist = 30.0f;
            if (newTransport->m_model->intersectRay(ray, dist, false, bot->GetPhaseMask(),
                                                    VMAP::ModelIgnoreFlags::Nothing))
            {
                float const deckZ = z + 2.0f - dist;
                float const groundZ = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, z + 2.0f, true);
                if (groundZ != INVALID_HEIGHT && groundZ != VMAP_INVALID_HEIGHT_VALUE && groundZ > deckZ)
                    newTransport = nullptr;
            }
        }

        // A static transport (an elevator: GAMEOBJECT_TYPE_TRANSPORT) is not in the grid and GetTransportForPos
        // never finds it (it searches 75 yd around the GO origin): the bot riding one is left on it, its own lift
        // action boards and leaves (Mechanar wave 33).
        Transport* const current = bot->GetTransport();
        bool const onElevator = current && current->GetGoType() == GAMEOBJECT_TYPE_TRANSPORT && !newTransport;
        // [guild15] stepping off at a stop (the RPG trip's Leave phase): the boat under its feet is not boarded again
        // on the way to the pier - each re-board stopped the walk (b331 Eraelor stood at the Auberdine deck's edge,
        // 11 yd from the pier, until the boat left)
        if (newTransport && !bot->GetTransport() && rpgInfo.trip.phase == NewRpgInfo::TripPhase::Leave)
            newTransport = nullptr;
        if (!onElevator && newTransport != bot->GetTransport())
        {
            LOG_DEBUG("playerbots", "Bot {} is on a transport", bot->GetName());

            if (bot->GetTransport())
                bot->GetTransport()->RemovePassenger(bot, true);

            if (newTransport)
                newTransport->AddPassenger(bot, true);

            bot->StopMovingOnCurrentPos();
        }
    }

    // Update the bot's group status (moved to helper function)
    MapWorkerTrace::SetStage("pre: group master", nullptr);
    UpdateAIGroupMaster();
    MapWorkerTrace::SetStage("pre: internal", nullptr);

    // Update internal AI
    idleDiag.tickCast = false;
    idleDiag.tickAction.clear();
    UpdateAIInternal(elapsed, minimal);
    MapWorkerTrace::SetStage("after ai", nullptr);
    UpdateIdleDiag(false);
    UpdateHealDiag();

    uint32 const reactDelay = GetReactDelay();
    YieldThread(bot, reactDelay);

    // In combat the next look comes when the global cooldown ends rather than up to a react delay later, the way
    // a player presses the next button (the cast-time branch above does the same for casts).
    if (bot->IsInCombat() && !bot->IsNonMeleeSpellCast(false, false, true))
    {
        uint32 const gcd = GetGlobalCooldownRemaining();
        if (gcd && gcd < nextAICheckDelay && nextAICheckDelay <= reactDelay + 200)
            SetNextCheckDelay(gcd);
    }
}

bool PlayerbotAI::UpdateAIReaction(uint32 elapsed, bool minimal, bool canControlSelf)
{
    if (!reactionEngine)
        return false;

    bool reactionFound = false;
    bool const reactionInProgress = reactionEngine->Update(elapsed, minimal, canControlSelf, reactionFound);

    if (reactionFound)
    {
        Reaction const* reaction = reactionEngine->GetReaction();
        if (reaction)
        {
            if (reaction->ShouldInterruptCast())
                bot->InterruptNonMeleeSpells(true);

            if (reaction->ShouldInterruptMovement())
                bot->StopMoving();
        }
    }

    return reactionInProgress;
}

void PlayerbotAI::SetActionDuration(Action const* action)
{
    if (!action)
        return;

    if (action->IsReaction())
    {
        if (reactionEngine)
            reactionEngine->SetReactionDuration(action);
    }
    else
        PlayerbotAIBase::SetActionDuration(action->GetDuration());
}

// Helper function for UpdateAI to check group membership and handle removal if necessary
void PlayerbotAI::UpdateAIGroupMaster()
{
    if (!bot)
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    Group* group = bot->GetGroup();

    // If bot is not in group verify that for is RandomBot before clearing  master and resetting.
    if (!group)
    {
        if (master && sRandomPlayerbotMgr.IsRandomBot(bot))
        {
            SetMaster(nullptr);
            Reset(true);
            ResetStrategies();
        }
        return;
    }

    // Bot in BG, but master no longer part of a group: release master
    // Exclude alt and addclass bots as they rely on current (real player) master, security-wise.
    if (bot->InBattleground() && sRandomPlayerbotMgr.IsRandomBot(bot) && master && !master->GetGroup())
        SetMaster(nullptr);

    PlayerbotAI* masterBotAI = nullptr;
    if (master)
        masterBotAI = GET_PLAYERBOT_AI(master);

    if (!master || (masterBotAI && !IsSelfBot(master)))
    {
        Player* newMaster = FindNewMaster();
        if (newMaster)
        {
            master = newMaster;
            botAI->SetMaster(newMaster);
            botAI->ResetStrategies();

            if (!bot->InBattleground())
            {
                botAI->ChangeStrategy("+follow", BOT_STATE_NON_COMBAT);

                if (botAI->GetMaster() == botAI->GetGroupLeader())
                    botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                        "hello_follow", "Hello, I follow you!", {}));
                else
                    botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                        "hello", "Hello!", {}));
            }
            else
            {
                // we're in a battleground, stay with the pack and focus on objective
                botAI->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);
            }
        }
    }
}

void PlayerbotAI::UpdateAIInternal([[maybe_unused]] uint32 elapsed, bool minimal)
{

    if (!bot || !bot->GetSession())
        return;

    if (!bot->IsInWorld() || bot->IsBeingTeleported() || bot->IsDuringRemoveFromWorld())
        return;

    if (!bot->GetMap())
        return; // instances are created and destroyed on demand

    // Only build the metric name when the profiler is enabled; start() returns nullptr otherwise.
    PerfMonitorOperation* pmo = nullptr;
    if (sPerfMonitor.IsEnabled())
    {
        std::string const mapString = WorldPosition(bot).isOverworld() ? std::to_string(bot->GetMapId()) : "I";
        pmo = sPerfMonitor.start(PERF_MON_TOTAL, "PlayerbotAI::UpdateAIInternal " + mapString);
    }

    ExternalEventHelper helper(aiObjectContext);

    // chat replies: take the due ones under the lock, answer them outside it
    std::list<ChatQueuedReply> dueReplies;
    {
        std::lock_guard<std::mutex> lock(chatRepliesLock);
        time_t const now = time(nullptr);
        for (auto it = chatReplies.begin(); it != chatReplies.end();)
        {
            if (it->m_time && now < it->m_time)
            {
                ++it;
                continue;
            }

            auto const due = it++;
            dueReplies.splice(dueReplies.end(), chatReplies, due);
        }
    }
    for (ChatQueuedReply& reply : dueReplies)
        ChatReplyAction::ChatReplyDo(bot, reply.m_type, reply.m_guid1, reply.m_msg, reply.m_chanName, reply.m_name);

    HandleCommands();

    // logout if logout timer is ready or if instant logout is possible
    if (bot->GetSession()->IsLoggingOut())
    {
        WorldSession* botWorldSessionPtr = bot->GetSession();
        bool logout = botWorldSessionPtr->ShouldLogOut(time(nullptr));

        if (!master || !master->IsInWorld() || !master->GetSession() || !master->GetSession()->GetPlayer())
            logout = true;

        if (bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) || bot->HasUnitState(UNIT_STATE_IN_FLIGHT) ||
            botWorldSessionPtr->HasPermission(rbac::RBAC_PERM_INSTANT_LOGOUT))
        {
            logout = true;
        }

        if (master && master->IsInWorld() &&
            (master->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) || master->HasUnitState(UNIT_STATE_IN_FLIGHT) ||
             (master->GetSession() &&
              master->GetSession()->HasPermission(rbac::RBAC_PERM_INSTANT_LOGOUT))))
        {
            logout = true;
        }

        if (logout)
        {
            PlayerbotMgr* masterBotMgr = nullptr;
            if (master)
                masterBotMgr = GET_PLAYERBOT_MGR(master);
            if (masterBotMgr)
            {
                masterBotMgr->LogoutPlayerBot(bot->GetGUID());
            }
            else
            {
                sRandomPlayerbotMgr.LogoutPlayerBot(bot->GetGUID());
            }
            return;
        }

        SetNextCheckDelay(sPlayerbotAIConfig.ReactDelay);
        return;
    }

    botOutgoingPacketHandlers.Handle(helper);
    masterIncomingPacketHandlers.Handle(helper);
    masterOutgoingPacketHandlers.Handle(helper);

    DoNextAction(minimal);

    // pets in a dungeon group: passive, on the owner's target once the tank holds it, else at the owner's heel. After
    // the tick's actions, so a guardian summoned by one of them (it starts aggressive) is passive before its first
    // PetAI update.
    DungeonPetControl::Update(this);

    if (pmo)
        pmo->finish();
}

void PlayerbotAI::HandleCommands()
{
    ExternalEventHelper helper(aiObjectContext);

    for (auto it = chatCommands.begin(); it != chatCommands.end();)
    {
        time_t& checkTime = it->GetTime();
        if (checkTime && time(nullptr) < checkTime)
        {
            ++it;
            continue;
        }

        // The sender may have logged out or lost permission while the command waited; drop it silently.
        Player* owner = ObjectAccessor::FindPlayer(it->GetOwnerGuid());
        if (!owner || !GetSecurity()->CheckLevelFor(it->GetRequiredLevel(), true, owner))
        {
            it = chatCommands.erase(it);
            continue;
        }

        std::string const& command = it->GetCommand();
        if (command.empty())
        {
            it = chatCommands.erase(it);
            continue;
        }

        if (!helper.ParseChatCommand(command, owner) && it->GetType() == CHAT_MSG_WHISPER)
        {
            // ostringstream out; out << "Unknown command " << command;
            // TellPlayer(out);
            // helper.ParseChatCommand("help");
        }

        it = chatCommands.erase(it);
    }
}

namespace
{
    // UTF-8 Cyrillic capitals (and ASCII) to lower case; the byte length stays the same.
    std::string LowerCaseUtf8Cyrillic(std::string const& text)
    {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            unsigned char const c = text[i];
            unsigned char const next = i + 1 < text.size() ? text[i + 1] : 0;
            if (c == 0xD0 && next >= 0x90 && next <= 0x9F)  // A..P
            {
                out += char(0xD0);
                out += char(next + 0x20);
                ++i;
            }
            else if (c == 0xD0 && next >= 0xA0 && next <= 0xAF)  // R..Ya
            {
                out += char(0xD1);
                out += char(next - 0x20);
                ++i;
            }
            else if (c == 0xD0 && next == 0x81)  // Yo
            {
                out += char(0xD1);
                out += char(0x91);
                ++i;
            }
            else
                out += c < 0x80 ? char(std::tolower(c)) : char(c);
        }
        return out;
    }

    // Short Russian chat orders (RussianCommandSynonyms) as the bot commands they stand for; the rest of the
    // line stays as the parameter ("resni Fitse" -> "res Fitse"). Bytes are escaped: the source stays ASCII.
    std::string TranslateRussianCommand(std::string const& text)
    {
        static std::vector<std::pair<std::string, std::string>> const synonyms = {
            {"\xD1\x80\xD0\xB5\xD1\x81\xD0\xBD\xD0\xB8\x20\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x8F", "res me"},  // resni menya
            {"\xD0\xB7\xD0\xB0\x20\xD0\xBC\xD0\xBD\xD0\xBE\xD0\xB9", "follow"},  // za mnoy
            {"\xD1\x8F\x20\xD1\x82\xD1\x83\xD1\x82", "resume"},  // ya tut
            {"\xD0\xB6\xD0\xB4\xD0\xB8", "stay"},  // zhdi
            {"\xD1\x81\xD1\x82\xD0\xBE\xD0\xB9", "stay"},  // stoy
            {"\xD1\x81\xD1\x82\xD0\xBE\xD0\xBF", "stay"},  // stop
            {"\xD1\x81\xD1\x82\xD0\xBE\xD1\x8F\xD1\x82\xD1\x8C", "stay"},  // stoyat
            {"\xD0\xBF\xD0\xBE\xD1\x88\xD0\xBB\xD0\xB8", "follow"},  // poshli
            {"\xD0\xB8\xD0\xB4\xD1\x91\xD0\xBC", "follow"},  // idyom
            {"\xD0\xB8\xD0\xB4\xD0\xB5\xD0\xBC", "follow"},  // idem
            {"\xD0\xBA\xD0\xBE\x20\xD0\xBC\xD0\xBD\xD0\xB5", "follow"},  // ko mne
            {"\xD0\xB1\xD0\xB5\xD0\xB9", "attack"},  // bey
            {"\xD0\xB0\xD1\x82\xD0\xB0\xD0\xBA\xD1\x83\xD0\xB9", "attack"},  // atakuy
            {"\xD0\xB0\xD1\x82\xD0\xB0\xD0\xBA\xD0\xB0", "attack"},  // ataka
            {"\xD0\xBF\xD1\x83\xD0\xBB\xD0\xBB", "pull"},  // pull
            {"\xD0\xBF\xD1\x83\xD0\xBB", "pull"},  // pul
            {"\xD0\xBE\xD1\x82\xD1\x85\xD0\xBE\xD0\xB4", "flee"},  // otkhod
            {"\xD0\xBD\xD0\xB0\xD0\xB7\xD0\xB0\xD0\xB4", "flee"},  // nazad
            {"\xD0\xB1\xD1\x83\xD1\x80\xD1\x81\xD1\x82", "burst"},  // burst
            {"\xD1\x80\xD0\xB5\xD1\x81\xD0\xBD\xD0\xB8", "res"},  // resni
            {"\xD0\xBF\xD0\xB0\xD1\x83\xD0\xB7\xD0\xB0", "pause"},  // pauza
            {"\xD0\xB0\xD1\x84\xD0\xBA", "pause"},  // afk
            {"\xD0\xBF\xD1\x80\xD0\xBE\xD0\xB4\xD0\xBE\xD0\xBB\xD0\xB6\xD0\xB0\xD0\xB5\xD0\xBC", "resume"},
            {"\xD1\x82\xD1\x83\xD1\x82", "resume"},  // tut
            {"\xD0\xBE\xD1\x82\xD0\xB4\xD1\x8B\xD1\x85", "rest"},  // otdykh
            {"\xD0\xBE\xD1\x82\xD0\xB4\xD1\x8B\xD1\x85\xD0\xB0\xD0\xB5\xD0\xBC", "rest"},  // otdykhaem
            {"\xD0\xBA\xD0\xB8\xD0\xBA", "interrupt"},  // kik
            {"\xD1\x81\xD0\xB1\xD0\xB5\xD0\xB9", "interrupt"},  // sbey
            {"\xD0\xBF\xD1\x80\xD0\xB5\xD1\x80\xD0\xB2\xD0\xB8", "interrupt"},  // prervi
            {"\xD0\xBF\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB1\xD0\xB0\xD1\x84", "rebuff"},  // perebaf
            {"\xD0\xBF\xD0\xB8\xD1\x82\xD1\x8C", "drink"},  // pit
        };

        if (text.empty() || static_cast<unsigned char>(text[0]) < 0x80)
            return text;  // not Cyrillic: nothing to translate

        std::string const lower = LowerCaseUtf8Cyrillic(text);
        for (auto const& [phrase, command] : synonyms)
        {
            if (lower.compare(0, phrase.size(), phrase) != 0)
                continue;

            if (lower.size() == phrase.size())
                return command;

            if (lower[phrase.size()] == ' ')
                return command + text.substr(phrase.size());
        }

        return text;
    }
}

std::map<std::string, ChatMsg> chatMap;
void PlayerbotAI::HandleCommand(uint32 type, std::string const& text, Player& fromPlayer, const uint32 lang)
{
    if (!bot)
        return;

    std::string filtered = text;

    if (!IsAllowedCommand(filtered) && !GetSecurity()->CheckLevelFor(PlayerbotSecurityLevel::PLAYERBOT_SECURITY_INVITE,
                                                                     type != CHAT_MSG_WHISPER, &fromPlayer))
        return;

    if (type == CHAT_MSG_ADDON)
        return;

    if (filtered.find("BOT\t") == 0)  // Mangosbot has BOT prefix so we remove that.
        filtered = filtered.substr(4);
    else if (lang == LANG_ADDON)  // Other addon messages should not command bots.
        return;

    if (type == CHAT_MSG_SYSTEM)
        return;

    if (filtered.find(sPlayerbotAIConfig.CommandSeparator) != std::string::npos)
    {
        std::vector<std::string> commands;
        split(commands, filtered, sPlayerbotAIConfig.CommandSeparator.c_str());
        for (std::vector<std::string>::iterator i = commands.begin(); i != commands.end(); ++i)
        {
            HandleCommand(type, *i, fromPlayer);
        }
        return;
    }

    if (!sPlayerbotAIConfig.CommandPrefix.empty())
    {
        if (filtered.find(sPlayerbotAIConfig.CommandPrefix) != 0)
            return;

        filtered = filtered.substr(sPlayerbotAIConfig.CommandPrefix.size());
    }

    if (chatMap.empty())
    {
        chatMap["#w "] = CHAT_MSG_WHISPER;
        chatMap["#p "] = CHAT_MSG_PARTY;
        chatMap["#r "] = CHAT_MSG_RAID;
        chatMap["#a "] = CHAT_MSG_ADDON;
        chatMap["#g "] = CHAT_MSG_GUILD;
    }
    currentChat = std::pair<ChatMsg, time_t>(CHAT_MSG_WHISPER, 0);
    for (std::map<std::string, ChatMsg>::iterator i = chatMap.begin(); i != chatMap.end(); ++i)
    {
        if (filtered.find(i->first) == 0)
        {
            filtered = filtered.substr(3);
            currentChat = std::pair<ChatMsg, time_t>(i->second, time(0) + 2);
            break;
        }
    }

    filtered = chatFilter.Filter(trim((std::string&)filtered));
    if (filtered.empty())
        return;

    if (sPlayerbotAIConfig.RussianCommandSynonyms)
        filtered = TranslateRussianCommand(filtered);
    // Security gate for all commands, including "debug" (same rule as the Player* overload).
    if (!IsAllowedCommand(filtered) &&
        !GetSecurity()->CheckLevelFor(PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL,
                                      type != CHAT_MSG_WHISPER, &fromPlayer))
        return;

    if (filtered.substr(0, 6) == "debug ")
    {
        std::string response = HandleRemoteCommand(filtered.substr(6));
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_ADDON, LANG_ADDON, bot->GetGUID(), {}, response, CHAT_TAG_NONE,
                                     bot->GetName());
        ServerFacade::instance().SendPacket(&fromPlayer, &data);
        return;
    }

    bool const allowedCommand = IsAllowedCommand(filtered);

    PlayerbotSecurityLevel const requiredLevel =
        allowedCommand ? PLAYERBOT_SECURITY_DENY_ALL : PLAYERBOT_SECURITY_ALLOW_ALL;

    if (type == CHAT_MSG_RAID_WARNING && filtered.find(bot->GetName()) != std::string::npos &&
        filtered.find("award") == std::string::npos)
    {
        chatCommands.push_back(ChatCommandHolder("warning", fromPlayer.GetGUID(), requiredLevel, type));
        return;
    }

    if ((filtered.size() > 2 && filtered.substr(0, 2) == "d ") ||
        (filtered.size() > 3 && filtered.substr(0, 3) == "do "))
    {
        Event event("do", "", &fromPlayer);
        std::string action = filtered.substr(filtered.find(" ") + 1);
        DoSpecificAction(action, event);
    }

    if (ChatHelper::parseValue("command", filtered).substr(0, 3) == "do ")
    {
        Event event("do", "", &fromPlayer);
        std::string action = ChatHelper::parseValue("command", filtered);
        action = action.substr(3);
        DoSpecificAction(action, event);
    }
    else if (type != CHAT_MSG_WHISPER && filtered.size() > 6 && filtered.substr(0, 6) == "queue ")
    {
        std::string remaining = filtered.substr(filtered.find(" ") + 1);
        int index = 1;
        Group* group = bot->GetGroup();
        if (group)
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                if (ref->GetSource() == master)
                    continue;

                if (ref->GetSource() == bot)
                    break;

                index++;
            }
        }

        chatCommands.push_back(
            ChatCommandHolder(remaining, fromPlayer.GetGUID(), requiredLevel, type, time(0) + index));
    }
    else if (filtered == "reset")
    {
        Reset(true);
    }

    // TODO: missing implementation to port
    /*else if (filtered == "logout")
    {
        if (!(bot->IsStunnedByLogout() || bot->GetSession()->IsLoggingOut()))
        {
            if (type == CHAT_MSG_WHISPER)
                TellPlayer(&fromPlayer, BOT_TEXT("logout_start"));

            if (master && master->GetPlayerbotMgr())
                SetShouldLogOut(true);
        }
    }
    else if (filtered == "logout cancel")
    {
        if (bot->IsStunnedByLogout() || bot->GetSession()->IsLoggingOut())
        {
            if (type == CHAT_MSG_WHISPER)
                TellPlayer(&fromPlayer, BOT_TEXT("logout_cancel"));

            WorldPacket p;
            bot->GetSession()->HandleLogoutCancelOpcode(p);
            SetShouldLogOut(false);
        }
    }
    else if ((filtered.size() > 5) && (filtered.substr(0, 5) == "wait ") && (filtered.find("wait for attack") ==
    std::string::npos))
    {
        std::string remaining = filtered.substr(filtered.find(" ") + 1);
        uint32 delay = atof(remaining.c_str()) * IN_MILLISECONDS;
        if (delay > 20000)
        {
            bot->TellMaster(&fromPlayer, "Max wait time is 20 seconds!");
            return;
        }

        IncreaseAIInternalUpdateDelay(delay);
        isWaiting = true;
        TellPlayer(&fromPlayer, "Waiting for " + remaining + " seconds!");
        return;
    }*/

    else
    {
        chatCommands.push_back(ChatCommandHolder(filtered, fromPlayer.GetGUID(), requiredLevel, type));
    }
}

void PlayerbotAI::HandleTeleportAck()
{
    if (!bot || !bot->GetSession())
        return;

    // Skip acknowledgment for SelfBots. The player's client handles that.
    if (IsSelfBot(bot))
        return;

    /*
     * FAR TELEPORT (worldport / map change)
     * Player may NOT be in world or grid here.
     * Handle this FIRST.
     */
    if (bot->IsBeingTeleportedFar())
    {
        bot->GetSession()->HandleMoveWorldportAck();

        // after worldport ACK the player should be in a valid map
        if (!bot->GetMap())
        {
            LOG_ERROR("playerbot", "Bot {} has no map after worldport ACK", bot->GetGUID().ToString());
            return;
        }

        // apply instance-related strategies after map attach
        if (sPlayerbotAIConfig.ApplyInstanceStrategies)
            ApplyInstanceStrategies(bot->GetMapId(), true);

        if (sPlayerbotAIConfig.RestrictHealerDPS)
            EvaluateHealerDpsStrategy();

        // reset AI state after teleport
        Reset(true);

        // clear movement only AFTER teleport is finalized and bot is in world. Skip during a taxi map-transfer
        if (bot->IsInWorld() && bot->GetMotionMaster() && !bot->IsInFlight())
        {
            bot->GetMotionMaster()->Clear(true);
            bot->StopMoving();
        }

        // simulate far teleport latency (cmangos-style)
        SetNextCheckDelay(urand(2000, 5000));
        return;
    }

    /*
     * NEAR TELEPORT (same map / instance)
     * Player MUST be in world (and in grid).
     */
    if (bot->IsBeingTeleportedNear())
    {
        if (!bot->IsInWorld())
            return;

        // [lead1302] b252 rfc-h 5D-12 585-826 s: Rtlocaa accepted Ancestral Spirit with its mover not itself; the
        // core's ack reads _player->m_mover, so every ack was dropped, the body never rose and the run stalled.
        // A dead player moves nothing but itself
        if (!bot->IsAlive() && bot->m_mover && bot->m_mover != bot)
        {
            LOG_INFO("playerbots", "[lead1302] {}: dead with mover {}, the mover is set back to itself for the ack",
                     bot->GetName(), bot->m_mover->GetGUID().ToString());
            bot->SetMover(bot);
        }

        Player* plMover = bot->m_mover ? bot->m_mover->ToPlayer() : nullptr;
        if (!plMover)
            return;

        WorldPacket p(MSG_MOVE_TELEPORT_ACK, 20);
        p << plMover->GetPackGUID();
        p << uint32(0);  // flags
        p << uint32(0);  // time

        bot->GetSession()->HandleMoveTeleportAck(p);

        // clear movement after successful relocation
        if (bot->GetMotionMaster())
        {
            bot->GetMotionMaster()->Clear(true);
            bot->StopMoving();
        }

        // simulate near teleport latency
        SetNextCheckDelay(urand(1000, 2000));
        return;
    }
}

void PlayerbotAI::Reset(bool full)
{
    if (bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
        return;

    WorldSession* botWorldSessionPtr = bot->GetSession();
    bool logout = botWorldSessionPtr->ShouldLogOut(time(nullptr));

    // cancel logout
    if (!logout && bot->GetSession()->IsLoggingOut())
    {
        WorldPackets::Character::LogoutCancel data = WorldPacket(CMSG_LOGOUT_CANCEL);
        bot->GetSession()->HandleLogoutCancelOpcode(data);
        TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "logout_cancel", "Logout cancelled!", {}));
    }

    currentEngine = engines[BOT_STATE_NON_COMBAT];
    currentState = BOT_STATE_NON_COMBAT;
    nextAICheckDelay = 0;
    if (reactionEngine)
        reactionEngine->ResetReactions();
    whispers.clear();

    aiObjectContext->GetValue<Unit*>("old target")->Set(nullptr);
    aiObjectContext->GetValue<Unit*>("current target")->Set(nullptr);
    aiObjectContext->GetValue<GuidVector>("prioritized targets")->Reset();
    aiObjectContext->GetValue<ObjectGuid>("pull target")->Set(ObjectGuid::Empty);
    aiObjectContext->GetValue<ObjectGuid>("pull strategy target")->Set(ObjectGuid::Empty);
    aiObjectContext->GetValue<GuidPosition>("rpg target")->Set(GuidPosition());
    aiObjectContext->GetValue<LootObject>("loot target")->Set(LootObject());
    aiObjectContext->GetValue<uint32>("lfg proposal")->Set(0);
    bot->SetTarget();

    LastSpellCast& lastSpell = aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get();
    lastSpell.Reset();

    if (full)
    {
        aiObjectContext->GetValue<LastMovement&>("last movement")->Get().Set(nullptr);
        aiObjectContext->GetValue<LastMovement&>("last area trigger")->Get().Set(nullptr);
        aiObjectContext->GetValue<LastMovement&>("last taxi")->Get().Set(nullptr);
        aiObjectContext->GetValue<TravelTarget*>("travel target")
            ->Get()
            ->setTarget(TravelMgr::instance().nullTravelDestination, TravelMgr::instance().nullWorldPosition, true);
        aiObjectContext->GetValue<TravelTarget*>("travel target")->Get()->setStatus(TRAVEL_STATUS_EXPIRED);
        aiObjectContext->GetValue<TravelTarget*>("travel target")->Get()->setExpireIn(1000);
        rpgInfo = NewRpgInfo();
    }

    aiObjectContext->GetValue<GuidSet&>("ignore rpg target")->Get().clear();

    bot->GetMotionMaster()->Clear();
    bot->CastStop();

    if (full)
    {
        for (uint8 i = 0; i < BOT_STATE_MAX; i++)
        {
            engines[i]->Init();
        }
    }
}

void PlayerbotAI::LeaveOrDisbandGroup()
{
    if (!bot || !bot->GetGroup() || IsSelfBot(bot))
        return;

    WorldPacket* packet = new WorldPacket(CMSG_GROUP_DISBAND);
    bot->GetSession()->QueuePacket(packet);
}

bool PlayerbotAI::IsAllowedCommand(std::string const text)
{
    if (unsecuredCommands.empty())
    {
        unsecuredCommands.insert("who");
        unsecuredCommands.insert("wts");
        unsecuredCommands.insert("sendmail");
        unsecuredCommands.insert("invite");
        unsecuredCommands.insert("leave");
        unsecuredCommands.insert("lfg");
        unsecuredCommands.insert("pvp stats");
        unsecuredCommands.insert("rpg status");
    }

    for (std::set<std::string>::iterator i = unsecuredCommands.begin(); i != unsecuredCommands.end(); ++i)
    {
        if (text.find(*i) == 0)
        {
            return true;
        }
    }

    return false;
}

void PlayerbotAI::HandleCommand(uint32 type, std::string const text, Player* fromPlayer)
{
    if (!GetSecurity()->CheckLevelFor(PLAYERBOT_SECURITY_INVITE, type != CHAT_MSG_WHISPER, fromPlayer))
        return;

    if (type == CHAT_MSG_ADDON)
        return;

    if (type == CHAT_MSG_SYSTEM)
        return;

    if (text.find(sPlayerbotAIConfig.CommandSeparator) != std::string::npos)
    {
        std::vector<std::string> commands;
        split(commands, text, sPlayerbotAIConfig.CommandSeparator.c_str());
        for (std::vector<std::string>::iterator i = commands.begin(); i != commands.end(); ++i)
        {
            HandleCommand(type, *i, fromPlayer);
        }

        return;
    }

    std::string filtered = text;
    if (!sPlayerbotAIConfig.CommandPrefix.empty())
    {
        if (filtered.find(sPlayerbotAIConfig.CommandPrefix) != 0)
            return;

        filtered = filtered.substr(sPlayerbotAIConfig.CommandPrefix.size());
    }

    if (chatMap.empty())
    {
        chatMap["#w "] = CHAT_MSG_WHISPER;
        chatMap["#p "] = CHAT_MSG_PARTY;
        chatMap["#r "] = CHAT_MSG_RAID;
        chatMap["#a "] = CHAT_MSG_ADDON;
        chatMap["#g "] = CHAT_MSG_GUILD;
    }

    currentChat = std::pair<ChatMsg, time_t>(CHAT_MSG_WHISPER, 0);
    for (std::map<std::string, ChatMsg>::iterator i = chatMap.begin(); i != chatMap.end(); ++i)
    {
        if (filtered.find(i->first) == 0)
        {
            filtered = filtered.substr(3);
            currentChat = std::pair<ChatMsg, time_t>(i->second, time(nullptr) + 2);
            break;
        }
    }

    filtered = chatFilter.Filter(trim(filtered));
    if (filtered.empty())
        return;

    if (sPlayerbotAIConfig.RussianCommandSynonyms)
        filtered = TranslateRussianCommand(filtered);
    // Gate "debug" to prevent AI state leaking to INVITE-level players.
    if (!IsAllowedCommand(filtered) &&
        (!GetSecurity()->CheckLevelFor(PLAYERBOT_SECURITY_ALLOW_ALL, type != CHAT_MSG_WHISPER, fromPlayer)))
        return;

    if (filtered.substr(0, 6) == "debug ")
    {
        std::string const response = HandleRemoteCommand(filtered.substr(6));
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, (ChatMsg)type, type == CHAT_MSG_ADDON ? LANG_ADDON : LANG_UNIVERSAL, bot,
                                     nullptr, response.c_str());
        fromPlayer->SendDirectMessage(&data);
        return;
    }
    bool const allowedCommand = IsAllowedCommand(filtered);

    PlayerbotSecurityLevel const requiredLevel =
        allowedCommand ? PLAYERBOT_SECURITY_INVITE : PLAYERBOT_SECURITY_ALLOW_ALL;

    if (type == CHAT_MSG_RAID_WARNING && filtered.find(bot->GetName()) != std::string::npos &&
        filtered.find("award") == std::string::npos)
    {
        chatCommands.push_back(ChatCommandHolder("warning", fromPlayer->GetGUID(), requiredLevel, type));
        return;
    }

    if ((filtered.size() > 2 && filtered.substr(0, 2) == "d ") ||
        (filtered.size() > 3 && filtered.substr(0, 3) == "do "))
    {
        std::string const action = filtered.substr(filtered.find(" ") + 1);
        DoSpecificAction(action);
    }
    else if (type != CHAT_MSG_WHISPER && filtered.size() > 6 && filtered.substr(0, 6) == "queue ")
    {
        std::string const remaining = filtered.substr(filtered.find(" ") + 1);
        uint32 index = 1;

        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                if (ref->GetSource() == master)
                    continue;

                if (ref->GetSource() == bot)
                    break;

                ++index;
            }
        }

        chatCommands.push_back(
            ChatCommandHolder(remaining, fromPlayer->GetGUID(), requiredLevel, type, time(nullptr) + index));
    }
    else if (filtered == "reset")
    {
        Reset(true);
    }
    else if (filtered == "logout")
    {
        if (bot->GetSession()->IsLoggingOut())
            return;

        // Verify the command came from this bot's master. Also handles nullptr
        if (fromPlayer != master)
        {
            if (type == CHAT_MSG_WHISPER)
            {
                std::string message = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "bot_not_your_master", "You are not my master!", {});
                bot->Whisper(message, LANG_UNIVERSAL, fromPlayer);
            }
            return;
        }

        PlayerbotMgr* masterBotMgr = GET_PLAYERBOT_MGR(master);
        if (!masterBotMgr)
            return;

        // Only respond if this bot is in master's collection (alt/addclass)
        if (masterBotMgr->GetPlayerBot(bot->GetGUID()))
        {
            if (type == CHAT_MSG_WHISPER)
            {
                std::string message = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "logout_start", "I'm logging out!", {});
                TellMaster(message);
            }

            masterBotMgr->LogoutPlayerBot(bot->GetGUID());
        }
        else if (type == CHAT_MSG_WHISPER)
        {
            std::string message = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "bot_rndbot_no_logout", "You can't command me to logout!", {});
            TellMaster(message);
        }
    }
    else if (filtered == "logout cancel")
    {
        if (!bot->GetSession()->IsLoggingOut())
            return;

        if (type == CHAT_MSG_WHISPER)
        {
            std::string message = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "logout_cancel", "Logout cancelled!", {});
            TellMaster(message);
        }

        WorldPackets::Character::LogoutCancel data = WorldPacket(CMSG_LOGOUT_CANCEL);
        bot->GetSession()->HandleLogoutCancelOpcode(data);
    }
    else
    {
        chatCommands.push_back(ChatCommandHolder(filtered, fromPlayer->GetGUID(), requiredLevel, type));
    }
}

void PlayerbotAI::HandleBotOutgoingPacket(WorldPacket const& packet)
{
    if (packet.empty())
        return;

    if (!bot || !bot->IsInWorld() || bot->IsDuringRemoveFromWorld())
        return;

    switch (packet.GetOpcode())
    {
        case SMSG_SPELL_FAILURE:
        {
            WorldPacket p(packet);
            p.rpos(0);
            ObjectGuid casterGuid;
            p >> casterGuid.ReadAsPacked();
            if (casterGuid != bot->GetGUID())
                return;
            uint8 count, result;
            uint32 spellId;
            p >> count >> spellId >> result;
            SpellInterrupted(spellId);
            return;
        }
        case SMSG_SPELL_DELAYED:
        {
            WorldPacket p(packet);
            p.rpos(0);
            ObjectGuid casterGuid;
            p >> casterGuid.ReadAsPacked();
            if (casterGuid != bot->GetGUID())
                return;

            uint32 delaytime;
            p >> delaytime;
            if (delaytime <= 1000)
                IncreaseNextCheckDelay(delaytime);
            return;
        }
        case SMSG_EMOTE:  // do not react to NPC emotes
        {
            WorldPacket p(packet);
            ObjectGuid source;
            uint32 emoteId;
            p.rpos(0);
            p >> emoteId >> source;
            if (source.IsPlayer())
                botOutgoingPacketHandlers.AddPacket(packet);

            return;
        }
        case SMSG_MESSAGECHAT:  // do not react to self or if not ready to reply
        {
            if (!sPlayerbotAIConfig.RandomBotTalk)
                return;

            if (!AllowActivity())
                return;

            WorldPacket p(packet);
            if (!p.empty() && (p.GetOpcode() == SMSG_MESSAGECHAT || p.GetOpcode() == SMSG_GM_MESSAGECHAT))
            {
                p.rpos(0);
                uint8 msgtype, chatTag;
                uint32 lang, textLen, unused;
                ObjectGuid guid1, guid2;
                std::string name = "";
                std::string chanName = "";
                std::string message = "";

                p >> msgtype >> lang;
                p >> guid1 >> unused;
                if (guid1.IsEmpty() || p.size() > p.DEFAULT_SIZE)
                    return;

                if (lang == LANG_ADDON)
                        return;

                if (p.GetOpcode() == SMSG_GM_MESSAGECHAT)
                {
                    p >> textLen;
                    p >> name;
                }

                switch (msgtype)
                {
                    case CHAT_MSG_CHANNEL:
                        p >> chanName;
                        [[fallthrough]];
                    case CHAT_MSG_SAY:
                    case CHAT_MSG_PARTY:
                    case CHAT_MSG_YELL:
                    case CHAT_MSG_WHISPER:
                    case CHAT_MSG_GUILD:
                        p >> guid2;
                        p >> textLen >> message >> chatTag;
                        break;
                    default:
                        return;
                }

                if (chanName == "World")
                    return;

                // This runs in the SPEAKER's thread (another map's worker for a whisper across maps, any thread of a
                // map that is not this bot's), and GetValue creates the value in this bot's context map on first use:
                // a cross-thread insert that corrupted the heap (four 60 s freezes 2026-10-24 in the chat reply path,
                // two instances of one map each time). Dungeon bots skip chat replies; the full fix defers this block
                // to the bot's own thread.
                if (bot->GetMap() && bot->GetMap()->IsDungeon())
                    return;

                // do not reply to self but always try to reply to real player
                if (guid1 != bot->GetGUID())
                {
                    time_t lastChat = GetAiObjectContext()->GetValue<time_t>("last said", "chat")->Get();
                    bool isPaused = time(0) < lastChat;
                    bool isFromFreeBot = false;
                    sCharacterCache->GetCharacterNameByGuid(guid1, name);
                    uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(guid1);
                    isFromFreeBot = sPlayerbotAIConfig.IsInRandomAccountList(accountId);
                    bool isMentioned = message.find(bot->GetName()) != std::string::npos;

                    // ChatChannelSource chatChannelSource = GetChatChannelSource(bot, msgtype, chanName);

                    // random bot speaks, chat CD
                    if (isFromFreeBot && isPaused)
                        return;

                    // BG: react only if mentioned or if not channel and real player spoke
                    if (bot->InBattleground() && !(isMentioned || (msgtype != CHAT_MSG_CHANNEL && !isFromFreeBot)))
                        return;

                    if (HasGameClientMaster() && guid1 != GetMaster()->GetGUID())
                        return;

                    auto itemIds = GetChatHelper()->ExtractAllItemIds(message);
                    if (message.starts_with(sPlayerbotAIConfig.ToxicLinksPrefix) &&
                        (itemIds.size() > 0 || GetChatHelper()->ExtractAllQuestIds(message).size() > 0) &&
                        sPlayerbotAIConfig.ToxicLinksRepliesChance)
                    {
                        if (urand(0, 50) > 0 || urand(1, 100) > sPlayerbotAIConfig.ToxicLinksRepliesChance)
                            return;
                    }
                    else if (itemIds.count(19019) && sPlayerbotAIConfig.ThunderfuryRepliesChance)
                    {
                        if (urand(0, 60) > 0 || urand(1, 100) > sPlayerbotAIConfig.ThunderfuryRepliesChance)
                            return;
                    }
                    else
                    {
                        if (isFromFreeBot && urand(0, 20))
                            return;

                        // if (msgtype == CHAT_MSG_GUILD && (!sPlayerbotAIConfig.GuildRepliesRate || urand(1, 100) >=
                        // sPlayerbotAIConfig.GuildRepliesRate)) return;

                        if (!isFromFreeBot)
                        {
                            if (!isMentioned && urand(0, 4))
                                return;
                        }
                        else
                        {
                            if (urand(0, 20 + 10 * isMentioned))
                                return;
                        }
                    }

                    QueueChatResponse(ChatQueuedReply{msgtype, guid1.GetCounter(), guid2.GetCounter(), message,
                                                      chanName, name,
                                                      time(nullptr) + urand(inCombat ? 10 : 5, inCombat ? 25 : 15)});
                    GetAiObjectContext()->GetValue<time_t>("last said", "chat")->Set(time(0) + urand(5, 25));
                    return;
                }
            }

            return;
        }
        case SMSG_FORCE_MOVE_ROOT:      // CMSG_FORCE_MOVE_ROOT_ACK
        case SMSG_FORCE_MOVE_UNROOT:    // CMSG_FORCE_MOVE_UNROOT_ACK
        {
            // Quick fix for CMSG_FORCE_MOVE_ROOT_ACK and CMSG_FORCE_MOVE_UNROOT_ACK:
            // this should resolve issues with MOVEMENTFLAG_ROOT being permanently set
            // when rooted during lost client control (charm + root effects)
            // @see https://github.com/azerothcore/azerothcore-wotlk/pull/23147
            bool forceRoot = (packet.GetOpcode() == SMSG_FORCE_MOVE_ROOT);
            if (forceRoot)
            {
                bot->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_MASK_MOVING_FLY);
                bot->m_movementInfo.AddMovementFlag(MOVEMENTFLAG_ROOT);
                bot->StopMoving();
            }
            else
                bot->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_ROOT);

            return;
        }
        case SMSG_MOVE_KNOCK_BACK:      // CMSG_MOVE_KNOCK_BACK_ACK
        {
            WorldPacket p(packet);
            p.rpos(0);

            ObjectGuid guid;
            uint32 counter;
            float vcos, vsin, horizontalSpeed, verticalSpeed = 0.f;

            p >> guid.ReadAsPacked() >> counter >> vcos >> vsin >> horizontalSpeed >> verticalSpeed;
            if (horizontalSpeed <= 0.1f)
                horizontalSpeed = 0.11f;
            verticalSpeed = -verticalSpeed;

            bot->CastStop();
            bot->StopMoving();
            bot->GetMotionMaster()->Clear();

            // Unit* currentTarget = GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
            // Bots are client-controlled players, so opt past the guard that protects real clients.
            bot->GetMotionMaster()->MoveKnockbackFrom(bot->GetPositionX() - vcos, bot->GetPositionY() - vsin,
                                                      horizontalSpeed, verticalSpeed, true);

            // bot->AddUnitMovementFlag(MOVEMENTFLAG_FALLING);
            // bot->AddUnitMovementFlag(MOVEMENTFLAG_FORWARD);
            // bot->m_movementInfo.AddMovementFlag(MOVEMENTFLAG_PENDING_STOP);
            // if (bot->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_SPLINE_ELEVATION))
            //     bot->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_SPLINE_ELEVATION);
            // bot->GetMotionMaster()->MoveIdle();
            // Position dest = bot->GetPosition();
            // float moveTimeHalf = verticalSpeed / Movement::gravity;
            // float dist = 2 * moveTimeHalf * horizontalSpeed;
            // float max_height = -Movement::computeFallElevation(moveTimeHalf, false, -verticalSpeed);

            // Use a mmap raycast to get a valid destination.
            // bot->GetMotionMaster()->MoveKnockbackFrom(fx, fy, horizontalSpeed, verticalSpeed);

            // // set delay based on actual distance
            // float newdis = sqrt(ServerFacade::instance().GetDistance2d(bot, fx, fy));
            // SetNextCheckDelay((uint32)((newdis / dis) * moveTimeHalf * 4 * IN_MILLISECONDS));

            // // add moveflags

            // // copy MovementInfo
            // MovementInfo movementInfo = bot->m_movementInfo;

            // // send ack
            // WorldPacket ack(CMSG_MOVE_KNOCK_BACK_ACK);
            // // movementInfo.jump.cosAngle = vcos;
            // // movementInfo.jump.sinAngle = vsin;
            // // movementInfo.jump.zspeed = -verticalSpeed;
            // // movementInfo.jump.xyspeed = horizontalSpeed;
            // ack << bot->GetGUID().WriteAsPacked();
            // // bot->m_mover->BuildMovementPacket(&ack);
            // ack << (uint32)0;
            // bot->BuildMovementPacket(&ack);
            // // ack << movementInfo.jump.sinAngle;
            // // ack << movementInfo.jump.cosAngle;
            // // ack << movementInfo.jump.xyspeed;
            // // ack << movementInfo.jump.zspeed;
            // bot->GetSession()->HandleMoveKnockBackAck(ack);

            // // // set jump destination for MSG_LAND packet
            // SetJumpDestination(Position(x, y, z, bot->GetOrientation()));

            // bot->Heart();

            // */
            return;
        }
        case SMSG_DISMOUNT:
        {
            WorldPacket p(packet);
            p.rpos(0);
            ObjectGuid guid;
            p >> guid.ReadAsPacked();
            if (guid != bot->GetGUID())
                return;
            CheckMountStateAction::CompleteDismount(bot);
            return;
        }
        default:
            botOutgoingPacketHandlers.AddPacket(packet);
    }
}

void PlayerbotAI::SpellInterrupted(uint32 spellid)
{
    for (uint8 type = CURRENT_MELEE_SPELL; type <= CURRENT_CHANNELED_SPELL; type++)
    {
        Spell* spell = bot->GetCurrentSpell((CurrentSpellTypes)type);
        if (!spell)
            continue;

        SpellInfo const* spellInfo = spell->GetSpellInfo();
        if (!spellInfo)
            continue;

        if (spellInfo->Id == spellid)
            bot->InterruptSpell((CurrentSpellTypes)type);
    }
    // LastSpellCast& lastSpell = aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get();
    // lastSpell.id = 0;
}

int32 PlayerbotAI::CalculateGlobalCooldown(uint32 spellid)
{
    if (!spellid)
        return 0;

    if (bot->HasSpellCooldown(spellid))
        return sPlayerbotAIConfig.GlobalCoolDown;

    return sPlayerbotAIConfig.ReactDelay;
}

void PlayerbotAI::HandleMasterIncomingPacket(WorldPacket const& packet)
{
    masterIncomingPacketHandlers.AddPacket(packet);
}

void PlayerbotAI::HandleMasterOutgoingPacket(WorldPacket const& packet)
{
    masterOutgoingPacketHandlers.AddPacket(packet);
}

void PlayerbotAI::ChangeEngine(BotState type)
{
    Engine* engine = engines[type];

    if (currentEngine != engine)
    {
        currentEngine = engine;
        currentState = type;
        ReInitCurrentEngine();

        switch (type)
        {
            case BOT_STATE_COMBAT:
                ChangeEngineOnCombat();
                break;
            case BOT_STATE_NON_COMBAT:
                ChangeEngineOnNonCombat();
                break;
            case BOT_STATE_DEAD:
                // LOG_DEBUG("playerbots",  "=== {} DEAD ===", bot->GetName().c_str());
                break;
            default:
                break;
        }
    }
}

void PlayerbotAI::ChangeEngineOnCombat()
{
    if (HasStrategy("wait for attack", BOT_STATE_COMBAT))
        aiObjectContext->GetValue<time_t>("combat start time")->Set(time(nullptr));

    if (HasStrategy("stay", BOT_STATE_COMBAT))
    {
        aiObjectContext->GetValue<PositionInfo>("pos", "stay")
            ->Set(PositionInfo(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId()));
    }
}

void PlayerbotAI::ChangeEngineOnNonCombat()
{
    if (HasStrategy("wait for attack", BOT_STATE_COMBAT))
        aiObjectContext->GetValue<time_t>("combat start time")->Set(0);

    if (HasStrategy("stay", BOT_STATE_NON_COMBAT))
    {
        aiObjectContext->GetValue<PositionInfo>("pos", "stay")->Reset();
    }
}

void PlayerbotAI::DoNextAction(bool min)
{
    if (!bot->IsInWorld() || bot->IsBeingTeleported() || (GetMaster() && GetMaster()->IsBeingTeleported()))
    {
        SetNextCheckDelay(sPlayerbotAIConfig.GlobalCoolDown);
        return;
    }

    // Change engine if just died
    bool isBotAlive = bot->IsAlive();
    if (currentEngine != engines[BOT_STATE_DEAD] && !isBotAlive)
    {
        // Death Count to prevent skeleton piles
        // Player* master = GetMaster();  // warning here - whipowill
        if (!IsRealPlayer(master) && !bot->InBattleground())
        {
            uint32 dCount = aiObjectContext->GetValue<uint32>("death count")->Get();
            aiObjectContext->GetValue<uint32>("death count")->Set(++dCount);
        }

        aiObjectContext->GetValue<Unit*>("current target")->Set(nullptr);
        aiObjectContext->GetValue<Unit*>("enemy player target")->Set(nullptr);
        aiObjectContext->GetValue<ObjectGuid>("pull target")->Set(ObjectGuid::Empty);
        aiObjectContext->GetValue<ObjectGuid>("pull strategy target")->Set(ObjectGuid::Empty);
        aiObjectContext->GetValue<LootObject>("loot target")->Set(LootObject());

        // The dead engine has no battleground strategy, so its "dead" trigger cannot reset this route.
        if (Battleground* bg = bot->GetBattleground())
            if (bg->GetBgTypeID(true) == BATTLEGROUND_WS)
                aiObjectContext->GetValue<WsgRouteState>("wsg route")->Reset();

        ChangeEngine(BOT_STATE_DEAD);
        return;
    }

    // Change engine if just ressed (no movement update when rooted)
    if (currentEngine == engines[BOT_STATE_DEAD] && isBotAlive && !bot->IsRooted())
    {
        bot->SendMovementFlagUpdate();

        ChangeEngine(BOT_STATE_NON_COMBAT);
        return;
    }

    // [healer_audit_b30] A healer joins the combat engine as soon as it or a group member near it is under attack,
    // not only through "dps assist" (blocked while it has healing work): ok-h-5G-37 the holy paladin healed from the
    // non-combat engine (no Holy Shock, no Lay on Hands, no emergency band) while the tank died. See
    // DpsAssistAction::HealerGroupUnderAttack.
    if (currentEngine == engines[BOT_STATE_NON_COMBAT] && DpsAssistAction::HealerGroupUnderAttack(this))
        ChangeEngine(BOT_STATE_COMBAT);
    // [lead112] a tank joins the combat engine as soon as a mob it can take back is on a member (TankIncomingMob):
    // its take-back ("tank open incoming") is a combat action; b147 rfc-h the tank stood 18 s in its non-combat
    // engine (its last action a blessing) while the healer and the shaman died
    // [lead114] b149 rfc-h: the combat engine entered with no current target ran "invalid target" -> "drop target"
    // on its first tick and went back to the non-combat engine, every tick, and the tank never fought - the mob is
    // its target first (as the tank's pull / assist set it), then the engine
    else if (currentEngine == engines[BOT_STATE_NON_COMBAT] && isBotAlive && PlayerbotAI::IsTank(bot) &&
             bot->GetGroup())
        if (Unit* incoming = TankIncomingMob(this, bot))
        {
            bot->SetSelection(incoming->GetGUID());
            aiObjectContext->GetValue<Unit*>("current target")->Set(incoming);
            ChangeEngine(BOT_STATE_COMBAT);
        }

    // Clear targets if in combat but sticking with old data
    if (currentEngine == engines[BOT_STATE_NON_COMBAT] && bot->IsInCombat())
    {
        Unit* currentTarget = aiObjectContext->GetValue<Unit*>("current target")->Get();
        if (currentTarget != nullptr)
        {
            aiObjectContext->GetValue<Unit*>("current target")->Set(nullptr);
        }
    }

    // [lead15] LEADER_IDLE_WITHOUT_COMMAND: a member the dungeon leader leads executes its commands only; with no
    // command and out of combat it does not wake up to look for something to do (the leader's event wakes it,
    // TestHarnessMgr::TickLeaderEvents). A buff pass the harness opened (forceRebuff) is the leader's command too.
    // [lead16] a member tick in a fight without a fresh Attack / Heal: counted (stale_command, should be 0)
    if (isBotAlive && bot->IsInCombat() && Leader::LEADER_ATTACK_EVENTS && Leader::ExecutorMember(bot))
    {
        Leader::MemberEvents events;
        Leader::Events::Get(bot->GetGUID(), events);
        if (events.attack == Leader::AttackEvent::None)
            Leader::Events::NoteStale(bot->GetGUID());
    }

    // [lead960] a led tank's pull walk in ends once its order's fight is on (b227 5C-11: ~80 yd on with the mob in
    // its melee, the next pack woken); no action of the combat engine judges a walk with its target in reach
    if (isBotAlive && bot->IsInCombat())
        StopPullWalkInFight(this, bot);

    if (Leader::LEADER_IDLE_WITHOUT_COMMAND && isBotAlive && !bot->IsInCombat() &&
        currentEngine == engines[BOT_STATE_NON_COMBAT] && !forceRebuff.IsPending() && !bot->IsInWater() &&
        Leader::ExecutorMember(bot))
    {
        Leader::MemberEvents events;
        TestRunner::Waypoint waypoint;
        bool const harnessWalk = TestRunner::GetWaypoint(bot->GetGUID(), waypoint);  // [lead17] the leader's walk
        if (!Leader::Events::Get(bot->GetGUID(), events) && !harnessWalk)
            return;
        // the executions that end with a report: rested to full, standing ready on the spot
        if (events.ready == Leader::ReadyEvent::Rest && bot->IsStandState() && !RestState::WantsDrink(bot) &&
            !RestState::WantsFood(bot))
            Leader::Events::Ack(bot->GetGUID(), events.seq, "rest", "rested: ready");
        if (events.ready == Leader::ReadyEvent::ReadyCheck && bot->IsStandState() &&
            (events.move != Leader::MoveEvent::StandAt ||
             bot->GetExactDist2d(events.spot.GetPositionX(), events.spot.GetPositionY()) <= events.tolerance))
            Leader::Events::Ack(bot->GetGUID(), events.seq, "ready", "ready on its spot");
    }

    bool minimal = !this->AllowActivity();

    currentEngine->DoNextAction(nullptr, 0, (minimal || min));

    if (minimal)
    {
        if (!bot->isAFK() && !bot->InBattleground() && !HasGameClientMaster())
            bot->ToggleAFK();

        SetNextCheckDelay(sPlayerbotAIConfig.PassiveDelay);
        return;
    }
    else if (bot->isAFK() && !IsSelfBot(bot))
        bot->ToggleAFK();

    // [lead1552] b276 lead1544: 39 of 194 drinks ended below full mana with no stand-up of the rest code ("[rest] ...
    // stands up") - this master mimic stands a sitter on any tick its actions set no long delay. A led bot's rest is
    // the leader's Rest (its StandUpAction / RestDone end it: the line, a fight, an open pull order): no stand-up here
    // while it eats or drinks under it
    auto const mimicStands = [&]() -> bool
    {
        if (!bot->IsSitState() || !RestState::LeaderRests(bot) ||
            !(RestState::IsDrinking(bot) || RestState::IsEating(bot)))
            return true;
        if (!mimicStandLogged)
            LOG_INFO("playerbots", "[lead1552] {}: no master-mimic stand-up under the leader's Rest (mana {:.1f} %, "
                     "health {:.1f} %)", bot->GetName(),
                     bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 100.0f, bot->GetHealthPct());
        mimicStandLogged = true;
        return false;
    };
    if (bot->IsStandState())
        mimicStandLogged = false;
    if (master && master->IsInWorld())
    {
        float distance = ServerFacade::instance().GetDistance2d(bot, master);

        if (master->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_WALKING) && distance < 20.0f)
            bot->m_movementInfo.AddMovementFlag(MOVEMENTFLAG_WALKING);
        else
            bot->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_WALKING);

        if (master->IsSitState() && nextAICheckDelay < 1000)
        {
            if (!bot->isMoving() && distance < 10.0f)
                bot->SetStandState(UNIT_STAND_STATE_SIT);
        }
        else if (nextAICheckDelay < 1000 && mimicStands())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
    }
    else if (bot->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_WALKING))
        bot->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_WALKING);
    else if ((nextAICheckDelay < 1000) && bot->IsSitState() && mimicStands())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    bool hasMountAura = bot->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED) ||
                        bot->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED);
    if (hasMountAura && !bot->IsMounted())
    {
        bot->RemoveAurasByType(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED);
        bot->RemoveAurasByType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED);
    }
}

void PlayerbotAI::ReInitCurrentEngine()
{
    currentEngine->Init();
}

void PlayerbotAI::ChangeStrategy(std::string const names, BotState type)
{
    Engine* e = engines[type];
    if (!e)
        return;

    e->ChangeStrategy(names);
}

void PlayerbotAI::ClearStrategies(BotState type)
{
    Engine* e = engines[type];
    if (!e)
        return;

    e->removeAllStrategies();
}

// Resets only the combat or non-combat engine: wipe strategies, repopulate with class/spec defaults,
// re-apply current map's instance strategy (if any), and call Init() to rebuild trigger/action lists.
void PlayerbotAI::SelectiveResetStrategies(BotState type)
{
    Engine* e = engines[type];
    if (!e)
        return;

    e->removeAllStrategies();

    if (type == BOT_STATE_COMBAT)
        AiFactory::AddDefaultCombatStrategies(bot, this, e);
    else if (type == BOT_STATE_NON_COMBAT)
        AiFactory::AddDefaultNonCombatStrategies(bot, this, e);

    if (sPlayerbotAIConfig.ApplyInstanceStrategies)
        ApplyInstanceStrategies(bot->GetMapId());

    e->Init();
}

std::vector<std::string> PlayerbotAI::GetStrategies(BotState type)
{
    Engine* e = engines[type];
    if (!e)
        return std::vector<std::string>();

    return e->GetStrategies();
}

void PlayerbotAI::ApplyInstanceStrategies(uint32 mapId, bool tellMaster)
{
    static const std::vector<std::string> allInstanceStrategies =
    {
        "classic-brs", "classic-gnomer", "classic-mara", "classic-scholo", "classic-sm", "classic-st", "classic-strat",
        "classic-zf", "classic-brd", "dungeon-escort", "tbc-ohb", "tbc-bm",
        "aq20", "blacktemple", "bwl", "gruulslair", "hyjal", "icc", "karazhan", "magtheridon",
        "moltencore", "naxx", "onyxia", "rs", "ssc", "tbc-ac", "tbc-arc", "tbc-bf", "tbc-mech", "tbc-mgt",
        "tbc-mt", "tbc-ramp", "tbc-seth", "tbc-sh", "tbc-sl", "tbc-sp", "tbc-sv", "tbc-ub", "tempestkeep", "ulduar",
        "voa", "wotlk-an", "wotlk-cos", "wotlk-dtk", "wotlk-eoe", "wotlk-fos", "wotlk-gd", "wotlk-hol",
        "wotlk-hor", "wotlk-hos", "wotlk-nex", "wotlk-occ", "wotlk-ok", "wotlk-os", "wotlk-pos", "wotlk-toc",
        "wotlk-uk", "wotlk-up", "wotlk-vh",
        "zulaman", "zulgurub", "world-boss"
    };

    for (std::string const& strat : allInstanceStrategies)
    {
        engines[BOT_STATE_COMBAT]->removeStrategy(strat);
        engines[BOT_STATE_NON_COMBAT]->removeStrategy(strat);
    }

    std::string strategyName;
    bool quiet = false;  // [world_boss]
    switch (mapId)
    {
        case 90:
            strategyName = "classic-gnomer";  // Gnomeregan
            break;
        case 189:
            strategyName = "classic-sm";  // Scarlet Monastery
            break;
        case 349:
            strategyName = "classic-mara";  // Maraudon
            break;
        case 229:
            strategyName = "classic-brs";  // Blackrock Spire (Rookery eggs, Hatchers first, Father Flame)
            break;
        case 109:
            strategyName = "classic-st";  // Sunken Temple (Atal'ai statues)
            break;
        case 289:
            strategyName = "classic-scholo";  // Scholomance (caster packs: Mana Burn, kill and kick order)
            break;
        case 209:
            strategyName = "classic-zf";  // Zul'Farrak (troll cages, Weegli's door, gong)
            break;
        case 230:
            strategyName = "classic-brd";  // Blackrock Depths (kill order, Gout of Flames, Incendius at a wall)
            break;
        case 329:
            strategyName = "classic-strat";  // Stratholme (street packs: aggro edge, kill and kick order, cleaves)
            break;
        case 560:
            strategyName = "tbc-ohb";  // Old Hillsbrad Foothills (bombs, barrels, Thrall)
            break;
        case 269:
            strategyName = "tbc-bm";  // The Black Morass (Medivh defense, rift keepers, Chrono-beacon)
            break;
        case 249:
            strategyName = "onyxia";  // Onyxia's Lair
            break;
        case 309:
            strategyName = "zulgurub";  // Zul'Gurub
            break;
        case 409:
            strategyName = "moltencore";  // Molten Core
            break;
        case 469:
            strategyName = "bwl";  // Blackwing Lair
            break;
        case 509:
            strategyName = "aq20";  // Ruins of Ahn'Qiraj
            break;
        case 532:
            strategyName = "karazhan";  // Karazhan
            break;
        case 533:
            strategyName = "naxx";  // Naxxramas
            break;
        case 534:
            strategyName = "hyjal";  // The Battle for Mount Hyjal (Hyjal Summit)
            break;
        case 540:
            strategyName = "tbc-sh";  // Hellfire Citadel: The Shattered Halls
            break;
        case 542:
            strategyName = "tbc-bf";  // Hellfire Citadel: The Blood Furnace
            break;
        case 543:
            strategyName = "tbc-ramp";  // Hellfire Citadel: Hellfire Ramparts
            break;
        case 544:
            strategyName = "magtheridon";  // Magtheridon's Lair
            break;
        case 545:
            strategyName = "tbc-sv";  // Coilfang Reservoir: The Steamvault
            break;
        case 546:
            strategyName = "tbc-ub";  // Coilfang Reservoir: The Underbog
            break;
        case 547:
            strategyName = "tbc-sp";  // Coilfang Reservoir: The Slave Pens
            break;
        case 548:
            strategyName = "ssc";  // Serpentshrine Cavern
            break;
        case 550:
            strategyName = "tempestkeep";  // Tempest Keep: The Eye
            break;
        case 552:
            strategyName = "tbc-arc";  // Tempest Keep: The Arcatraz
            break;
        case 555:
            strategyName = "tbc-sl";  // Auchindoun: Shadow Labyrinth
            break;
        case 556:
            strategyName = "tbc-seth";  // Auchindoun: Sethekk Halls
            break;
        case 557:
            strategyName = "tbc-mt";  // Auchindoun: Mana-Tombs
            break;
        case 554:
            strategyName = "tbc-mech";  // Tempest Keep: The Mechanar
            break;
        case 558:
            strategyName = "tbc-ac";  // Auchindoun: Auchenai Crypts
            break;
        case 564:
            strategyName = "blacktemple";  // Black Temple
            break;
        case 565:
            strategyName = "gruulslair";  // Gruul's Lair
            break;
        case 568:
            strategyName = "zulaman";  // Zul'Aman
            break;
        case 574:
            strategyName = "wotlk-uk";  // Utgarde Keep
            break;
        case 575:
            strategyName = "wotlk-up";  // Utgarde Pinnacle
            break;
        case 576:
            strategyName = "wotlk-nex";  // The Nexus
            break;
        case 578:
            strategyName = "wotlk-occ";  // The Oculus
            break;
        case 585:
            strategyName = "tbc-mgt";  // Magisters' Terrace
            break;
        case 595:
            strategyName = "wotlk-cos";  // The Culling of Stratholme
            break;
        case 599:
            strategyName = "wotlk-hos";  // Halls of Stone
            break;
        case 600:
            strategyName = "wotlk-dtk";  // Drak'Tharon Keep
            break;
        case 601:
            strategyName = "wotlk-an";  // Azjol-Nerub
            break;
        case 602:
            strategyName = "wotlk-hol";  // Halls of Lightning
            break;
        case 603:
            strategyName = "ulduar";  // Ulduar
            break;
        case 604:
            strategyName = "wotlk-gd";  // Gundrak
            break;
        case 608:
            strategyName = "wotlk-vh";  // Violet Hold
            break;
        case 615:
            strategyName = "wotlk-os";  // Obsidian Sanctum
            break;
        case 616:
            strategyName = "wotlk-eoe";  // Eye Of Eternity
            break;
        case 619:
            strategyName = "wotlk-ok";  // Ahn'kahet: The Old Kingdom
            break;
        case 624:
            strategyName = "voa";  // Vault of Archavon
            break;
        case 631:
            strategyName = "icc";  // Icecrown Citadel
            break;
        case 632:
            strategyName = "wotlk-fos";  // The Forge of Souls
            break;
        case 650:
            strategyName = "wotlk-toc";  // Trial of the Champion
            break;
        case 658:
            strategyName = "wotlk-pos";  // Pit of Saron
            break;
        case 668:
            strategyName = "wotlk-hor";  // Halls of Reflection
            break;
        case 724:
            strategyName = "rs";  // Ruby Sanctum
            break;
        case 0:
        case 1:
        case 530:
            // [world_boss] the world boss ladder (docs/WORLD_BOSS_PLAN.md): its triggers return at once out of a
            // world boss fight; not told to the master (every continent teleport would announce it)
            strategyName = "world-boss";
            quiet = true;
            break;
        default:
            break;
    }

    // Escorted NPCs (Gnomeregan's Emi Shortfuse, the Disciple of Naralex): next to the map's own strategy
    if (EscortProtect::MapHasEscort(mapId))
    {
        engines[BOT_STATE_COMBAT]->addStrategy("dungeon-escort");
        engines[BOT_STATE_NON_COMBAT]->addStrategy("dungeon-escort");
        // [w24b-escort-strategy] once per bot and map: its own context now, kept next to the map strategy
        LOG_INFO("playerbots", "[w24b-escort-strategy] {}: dungeon-escort on map {} next to its map strategy",
                 bot->GetName(), mapId);
    }

    if (strategyName.empty())
        return;

    engines[BOT_STATE_COMBAT]->addStrategy(strategyName);
    engines[BOT_STATE_NON_COMBAT]->addStrategy(strategyName);

    if (tellMaster && !strategyName.empty() && !quiet)
    {
        std::ostringstream out;
        out << "Added " << strategyName << " instance strategy";
        TellMasterNoFacing(out.str());
    }
}

bool PlayerbotAI::IsInNonRaidDungeon() const
{
    MapEntry const* mapEntry = sMapStore.LookupEntry(bot->GetMapId());
    return mapEntry && mapEntry->IsNonRaidDungeon();
}

bool PlayerbotAI::HasTargetExclusions() const
{
    return engines[BOT_STATE_COMBAT] && engines[BOT_STATE_COMBAT]->HasTargetExclusions();
}

bool PlayerbotAI::DoSpecificAction(std::string const name, Event event, bool silent, std::string const qualifier)
{
    std::ostringstream out;

    for (uint8 i = 0; i < BOT_STATE_MAX; i++)
    {
        ActionResult res = engines[i]->ExecuteAction(name, event, qualifier);
        switch (res)
        {
            case ACTION_RESULT_UNKNOWN:
                continue;
            case ACTION_RESULT_OK:
                if (!silent)
                {
                    PlaySound(TEXT_EMOTE_NOD);
                }
                return true;
            case ACTION_RESULT_IMPOSSIBLE:
                out << name << ": impossible";
                if (!silent)
                {
                    TellError(out.str());
                    PlaySound(TEXT_EMOTE_NO);
                }
                return false;
            case ACTION_RESULT_USELESS:
                out << name << ": useless";
                if (!silent)
                {
                    TellError(out.str());
                    PlaySound(TEXT_EMOTE_NO);
                }
                return false;
            case ACTION_RESULT_FAILED:
                if (!silent)
                {
                    out << name << ": failed";
                    TellError(out.str());
                }
                return false;
        }
    }

    if (!silent)
    {
        out << name << ": unknown action";
        TellError(out.str());
    }

    return false;
}

bool PlayerbotAI::PlaySound(uint32 emote)
{
    if (EmotesTextSoundEntry const* soundEntry = FindTextSoundEmoteFor(emote, bot->getRace(), bot->getGender()))
    {
        bot->PlayDistanceSound(soundEntry->SoundId);
        return true;
    }

    return false;
}

bool PlayerbotAI::PlayEmote(uint32 emote)
{
    WorldPacket data(SMSG_TEXT_EMOTE);
    data << (TextEmotes)emote;
    data << EmoteAction::GetNumberOfEmoteVariants((TextEmotes)emote, bot->getRace(), bot->getGender());
    data << ((master && (ServerFacade::instance().GetDistance2d(bot, master) < 30.0f) && urand(0, 1)) ? master->GetGUID()
             : (bot->GetTarget() && urand(0, 1))                                            ? bot->GetTarget()
                                                                                            : ObjectGuid::Empty);
    bot->GetSession()->HandleTextEmoteOpcode(data);

    return false;
}

bool PlayerbotAI::ContainsStrategy(StrategyType type)
{
    for (uint8 i = 0; i < BOT_STATE_MAX; i++)
    {
        if (engines[i]->HasStrategyType(type))
            return true;
    }

    return false;
}

bool PlayerbotAI::HasStrategy(std::string const name, BotState type) { return engines[type]->HasStrategy(name); }

Strategy* PlayerbotAI::GetStrategy(std::string const name, BotState type)
{
    return engines[type] ? engines[type]->GetStrategy(name) : nullptr;
}

void PlayerbotAI::ResetStrategies(bool /*load*/)
{
    for (uint8 i = 0; i < BOT_STATE_MAX; i++)
        engines[i]->removeAllStrategies();

    AiFactory::AddDefaultCombatStrategies(bot, this, engines[BOT_STATE_COMBAT]);
    AiFactory::AddDefaultNonCombatStrategies(bot, this, engines[BOT_STATE_NON_COMBAT]);
    AiFactory::AddDefaultDeadStrategies(bot, this, engines[BOT_STATE_DEAD]);
    AiFactory::AddDefaultReactionStrategies(bot, this, reactionEngine);
    if (sPlayerbotAIConfig.ApplyInstanceStrategies)
        ApplyInstanceStrategies(bot->GetMapId());

    for (uint8 i = 0; i < BOT_STATE_MAX; i++)
        engines[i]->Init();

    // if (load)
    //     PlayerbotRepository::instance().Load(this);
}

bool PlayerbotAI::IsRanged(Player* player, bool bySpec)
{
    PlayerbotAI* botAi = GET_PLAYERBOT_AI(player);
    if (!bySpec && botAi)
        return botAi->ContainsStrategy(STRATEGY_TYPE_RANGED);

    int tab = AiFactory::GetPlayerSpecTab(player);
    switch (player->getClass())
    {
        case CLASS_DEATH_KNIGHT:
        case CLASS_WARRIOR:
        case CLASS_ROGUE:
            return false;
            break;
        case CLASS_DRUID:
            if (tab == 1)
            {
                return false;
            }
            break;
        case CLASS_PALADIN:
            if (tab != 0)
            {
                return false;
            }
            break;
        case CLASS_SHAMAN:
            if (tab == 1)
            {
                return false;
            }
            break;
    }

    return true;
}

bool PlayerbotAI::IsMelee(Player* player, bool bySpec) { return !IsRanged(player, bySpec); }

bool PlayerbotAI::IsCaster(Player* player, bool bySpec)
{
    return IsRanged(player, bySpec) && player->getClass() != CLASS_HUNTER;
}

bool PlayerbotAI::IsCombo(Player* player)
{
    // int tab = AiFactory::GetPlayerSpecTab(player);
    return player->getClass() == CLASS_ROGUE ||
           (player->getClass() == CLASS_DRUID && player->HasAura(768));  // cat druid
}

bool PlayerbotAI::IsRangedDps(Player* player, bool bySpec)
{
    return IsRanged(player, bySpec) && IsDps(player, bySpec);
}

// If true, indexLivingOnly excludes dead group members from the index, meaning living members shift
// up to fill the gap. Example: if assist heal 0 dies, then assist heal 1 becomes assist heal 0.
// By default, it is false, meaning roles remain stable across deaths.
bool PlayerbotAI::IsAssistHealOfIndex(Player* player, uint8 index, bool indexLivingOnly)
{
    if (!IsHeal(player))
        return false;

    if (indexLivingOnly && !player->IsAlive())
        return false;

    Group* group = player->GetGroup();
    if (!group)
        return false;

    uint8 totalAssistants = 0;
    uint8 assistantsBeforePlayer = 0;
    uint8 nonAssistantsBeforePlayer = 0;
    bool playerFound = false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || (indexLivingOnly && !member->IsAlive()) || !IsHeal(member))
            continue;

        bool isAssistant = group->IsAssistant(member->GetGUID());

        if (isAssistant)
            totalAssistants++;

        if (member == player)
            playerFound = true;
        else if (!playerFound)
        {
            if (isAssistant)
                assistantsBeforePlayer++;
            else
                nonAssistantsBeforePlayer++;
        }
    }

    if (!playerFound)
        return false;

    // If the player is an assistant, their index is just the number of assistants before them.
    // If they are a non-assistant, their index is shifted by the total number of assistants.
    uint8 playerIndex = group->IsAssistant(player->GetGUID())
        ? assistantsBeforePlayer : (totalAssistants + nonAssistantsBeforePlayer);

    return playerIndex == index;
}

// If true, indexLivingOnly excludes dead group members from the index, meaning living members shift
// up to fill the gap. Example: if assist ranged dps 0 dies, then assist ranged dps 1 becomes assist
// ranged dps 0. By default, it is false, meaning roles remain stable across deaths.
bool PlayerbotAI::IsAssistRangedDpsOfIndex(Player* player, uint8 index, bool indexLivingOnly)
{
    if (!IsRangedDps(player))
        return false;

    if (indexLivingOnly && !player->IsAlive())
        return false;

    Group* group = player->GetGroup();
    if (!group)
        return false;

    uint8 totalAssistants = 0;
    uint8 assistantsBeforePlayer = 0;
    uint8 nonAssistantsBeforePlayer = 0;
    bool playerFound = false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || (indexLivingOnly && !member->IsAlive()) || !IsRangedDps(member))
            continue;

        bool isAssistant = group->IsAssistant(member->GetGUID());

        if (isAssistant)
            totalAssistants++;

        if (member == player)
            playerFound = true;
        else if (!playerFound)
        {
            if (isAssistant)
                assistantsBeforePlayer++;
            else
                nonAssistantsBeforePlayer++;
        }
    }

    if (!playerFound)
        return false;

    // If the player is an assistant, their index is just the number of assistants before them.
    // If they are a non-assistant, their index is shifted by the total number of assistants.
    uint8 playerIndex = group->IsAssistant(player->GetGUID())
        ? assistantsBeforePlayer : (totalAssistants + nonAssistantsBeforePlayer);

    return playerIndex == index;
}

bool PlayerbotAI::HasAggro(Unit* unit)
{
    if (!IsValidUnit(unit))
        return false;

    bool isMT = IsExplicitMainTank(bot);
    Unit* victim = unit->GetVictim();
    if (victim && (victim->GetGUID() == bot->GetGUID() || (!isMT && victim->ToPlayer() && IsTank(victim->ToPlayer()))))
    {
        return true;
    }
    return false;
}

bool PlayerbotAI::IsMovementImpaired(Unit* unit)
{
    return unit && (unit->HasAuraType(SPELL_AURA_MOD_ROOT) || unit->IsRooted() || unit->GetSpeedRate(MOVE_RUN) < 1.0f);
}

int32 PlayerbotAI::GetAssistTankIndex(Player* player)
{
    Group* group = player->GetGroup();
    if (!group)
    {
        return -1;
    }

    int counter = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member)
        {
            continue;
        }

        if (player == member)
        {
            return counter;
        }

        if (IsTank(member, true) && group->IsAssistant(member->GetGUID()))
        {
            counter++;
        }
    }

    return 0;
}

int32 PlayerbotAI::GetGroupSlotIndex(Player* player)
{
    Group* group = bot->GetGroup();
    if (!group)
    {
        return -1;
    }
    int counter = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();

        if (!member)
        {
            continue;
        }

        if (player == member)
        {
            return counter;
        }
        counter++;
    }
    return 0;
}

int32 PlayerbotAI::GetRangedIndex(Player* player)
{
    if (!IsRanged(player))
    {
        return -1;
    }
    Group* group = bot->GetGroup();
    if (!group)
    {
        return -1;
    }
    int counter = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();

        if (!member)
        {
            continue;
        }

        if (player == member)
        {
            return counter;
        }
        if (IsRanged(member))
        {
            counter++;
        }
    }
    return 0;
}

int32 PlayerbotAI::GetClassIndex(Player* player, uint8 cls)
{
    if (player->getClass() != cls)
    {
        return -1;
    }
    Group* group = bot->GetGroup();
    if (!group)
    {
        return -1;
    }
    int counter = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();

        if (!member)
        {
            continue;
        }

        if (player == member)
        {
            return counter;
        }
        if (member->getClass() == cls)
        {
            counter++;
        }
    }
    return 0;
}
int32 PlayerbotAI::GetRangedDpsIndex(Player* player)
{
    if (!IsRangedDps(player))
    {
        return -1;
    }
    Group* group = bot->GetGroup();
    if (!group)
    {
        return -1;
    }
    int counter = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();

        if (!member)
        {
            continue;
        }

        if (player == member)
        {
            return counter;
        }
        if (IsRangedDps(member))
        {
            counter++;
        }
    }
    return 0;
}

int32 PlayerbotAI::GetMeleeIndex(Player* player)
{
    if (IsRanged(player))
    {
        return -1;
    }
    Group* group = bot->GetGroup();
    if (!group)
    {
        return -1;
    }
    int counter = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();

        if (!member)
        {
            continue;
        }

        if (player == member)
        {
            return counter;
        }
        if (!IsRanged(member))
        {
            counter++;
        }
    }
    return 0;
}

bool PlayerbotAI::IsTank(Player* player, bool bySpec)
{
    PlayerbotAI* botAi = GET_PLAYERBOT_AI(player);
    if (!bySpec && botAi)
        return botAi->ContainsStrategy(STRATEGY_TYPE_TANK);

    int tab = AiFactory::GetPlayerSpecTab(player);
    switch (player->getClass())
    {
        case CLASS_DEATH_KNIGHT:
            if (tab == DEATH_KNIGHT_TAB_BLOOD || player->HasAura(SPELL_DK_FROST_PRESENCE))
            {
                return true;
            }
            break;
        case CLASS_PALADIN:
            if (tab == PALADIN_TAB_PROTECTION)
            {
                return true;
            }
            break;
        case CLASS_WARRIOR:
            if (tab == WARRIOR_TAB_PROTECTION)
            {
                return true;
            }
            break;
        case CLASS_DRUID:
            // [b31_sd] any Thick Hide rank: bear templates have rank 1 at 20, rank 2 at 21
            if (tab == DRUID_TAB_FERAL && (player->GetShapeshiftForm() == FORM_BEAR ||
                                           player->GetShapeshiftForm() == FORM_DIREBEAR || player->HasAura(16929) ||
                                           player->HasAura(16930) || player->HasAura(16931)))
            {
                return true;
            }
            break;
    }
    return false;
}

bool PlayerbotAI::IsHeal(Player* player, bool bySpec)
{
    PlayerbotAI* botAi = GET_PLAYERBOT_AI(player);
    if (!bySpec && botAi)
        return botAi->ContainsStrategy(STRATEGY_TYPE_HEAL);

    int tab = AiFactory::GetPlayerSpecTab(player);
    switch (player->getClass())
    {
        case CLASS_PRIEST:
            if (tab == PRIEST_TAB_DISCIPLINE || tab == PRIEST_TAB_HOLY)
            {
                return true;
            }
            break;
        case CLASS_DRUID:
            if (tab == DRUID_TAB_RESTORATION)
            {
                return true;
            }
            break;
        case CLASS_SHAMAN:
            if (tab == SHAMAN_TAB_RESTORATION)
            {
                return true;
            }
            break;
        case CLASS_PALADIN:
            if (tab == PALADIN_TAB_HOLY)
            {
                return true;
            }
            break;
    }
    return false;
}

bool PlayerbotAI::IsDps(Player* player, bool bySpec)
{
    PlayerbotAI* botAi = GET_PLAYERBOT_AI(player);
    if (!bySpec && botAi)
        return botAi->ContainsStrategy(STRATEGY_TYPE_DPS);

    int tab = AiFactory::GetPlayerSpecTab(player);
    switch (player->getClass())
    {
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        case CLASS_HUNTER:
        case CLASS_ROGUE:
            return true;
        case CLASS_PRIEST:
            if (tab == PRIEST_TAB_SHADOW)
            {
                return true;
            }
            break;
        case CLASS_DRUID:
            if (tab == DRUID_TAB_BALANCE)
            {
                return true;
            }
            if (tab == DRUID_TAB_FERAL && !IsTank(player, bySpec))
            {
                return true;
            }
            break;
        case CLASS_SHAMAN:
            if (tab != SHAMAN_TAB_RESTORATION)
            {
                return true;
            }
            break;
        case CLASS_PALADIN:
            if (tab == PALADIN_TAB_RETRIBUTION)
            {
                return true;
            }
            break;
        case CLASS_DEATH_KNIGHT:
            if (tab != DEATH_KNIGHT_TAB_BLOOD)
            {
                return true;
            }
            break;
        case CLASS_WARRIOR:
            if (tab != WARRIOR_TAB_PROTECTION)
            {
                return true;
            }
            break;
    }
    return false;
}

ObjectGuid PlayerbotAI::GetMainTankGuid(Group* group)
{
    if (!group)
        return ObjectGuid::Empty;

    Group::MemberSlotList const& slots = group->GetMemberSlots();
    for (Group::member_citerator itr = slots.begin(); itr != slots.end(); ++itr)
    {
        if (itr->flags & MEMBER_FLAG_MAINTANK)
            return itr->guid;
    }

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && IsTank(member) && member->IsAlive())
            return member->GetGUID();
    }

    return ObjectGuid::Empty;
}

bool PlayerbotAI::IsMainTank(Player* player)
{
    Group* group = player->GetGroup();
    if (!group)
        return IsTank(player);

    ObjectGuid const mainTankGuid = GetMainTankGuid(group);
    return !mainTankGuid.IsEmpty() && player->GetGUID() == mainTankGuid;
}

bool PlayerbotAI::IsExplicitMainTank(Player* player)
{
    Group* group = player->GetGroup();
    if (!group)
        return false;

    for (Group::member_citerator itr = group->GetMemberSlots().begin(); itr != group->GetMemberSlots().end(); ++itr)
    {
        if (itr->flags & MEMBER_FLAG_MAINTANK)
            return player->GetGUID() == itr->guid;
    }
    return false;
}

bool PlayerbotAI::IsBotMainTank(Player* player)
{
    if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld())
        return false;

    WorldSession* session = player->GetSession();
    if (!session || !session->IsHeadless())
        return false;

    if (!IsTank(player))
        return false;

    if (IsMainTank(player))
        return true;

    Group* group = player->GetGroup();
    if (!group)
        return true;

    int32 botAssistTankIndex = GetAssistTankIndex(player);
    if (botAssistTankIndex == -1)
        return false;

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* member = gref->GetSource();
        if (!member)
            continue;

        int32 memberAssistTankIndex = GetAssistTankIndex(member);
        if (memberAssistTankIndex == -1)
            continue;

        if (memberAssistTankIndex == botAssistTankIndex && player == member)
            return true;

        if (memberAssistTankIndex < botAssistTankIndex && member->GetSession()->IsHeadless())
            return false;
    }

    return false;
}

uint32 PlayerbotAI::GetGroupTankNum(Player* player)
{
    Group* group = player->GetGroup();
    if (!group)
        return 0;

    uint32 result = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();

        if (!member)
            continue;

        if (IsTank(member) && member->IsAlive())
            result++;
    }

    return result;
}

bool PlayerbotAI::IsAssistTank(Player* player)
{
    if (!IsTank(player))
        return false;

    Group* group = player->GetGroup();
    if (!group)
        return false;

    return player->GetGUID() != GetMainTankGuid(group);
}

// If true, indexLivingOnly excludes dead group members from the index, meaning living members shift
// up to fill the gap. Example: if assist tank 0 dies, then assist tank 1 becomes assist tank 0.
// By default, it is false, meaning roles remain stable across deaths.
bool PlayerbotAI::IsAssistTankOfIndex(Player* player, uint8 index, bool indexLivingOnly)
{
    if (!IsTank(player))
        return false;

    Group* group = player->GetGroup();
    if (!group)
        return false;

    ObjectGuid const mainTankGuid = GetMainTankGuid(group);

    if (player->GetGUID() == mainTankGuid)
        return false;

    if (indexLivingOnly && !player->IsAlive())
        return false;

    uint8 totalAssistants = 0;
    uint8 assistantsBeforePlayer = 0;
    uint8 nonAssistantsBeforePlayer = 0;
    bool playerFound = false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || (indexLivingOnly && !member->IsAlive()) || !IsTank(member) ||
            member->GetGUID() == mainTankGuid)
        {
            continue;
        }

        bool isAssistant = group->IsAssistant(member->GetGUID());

        if (isAssistant)
            totalAssistants++;

        if (member == player)
            playerFound = true;
        else if (!playerFound)
        {
            if (isAssistant)
                assistantsBeforePlayer++;
            else
                nonAssistantsBeforePlayer++;
        }
    }

    if (!playerFound)
        return false;

    // If the player is an assistant, their index is just the number of assistants before them.
    // If they are a non-assistant, their index is shifted by the total number of assistants.
    uint8 playerIndex = group->IsAssistant(player->GetGUID())
        ? assistantsBeforePlayer : (totalAssistants + nonAssistantsBeforePlayer);

    return playerIndex == index;
}

namespace acore
{
class UnitByGuidInRangeCheck
{
public:
    UnitByGuidInRangeCheck(WorldObject const* obj, ObjectGuid guid, float range)
        : i_obj(obj), i_range(range), i_guid(guid)
    {
    }
    WorldObject const& GetFocusObject() const { return *i_obj; }
    bool operator()(Unit* u) { return u->GetGUID() == i_guid && i_obj->IsWithinDistInMap(u, i_range); }

private:
    WorldObject const* i_obj;
    float i_range;
    ObjectGuid i_guid;
};

class GameObjectByGuidInRangeCheck
{
public:
    GameObjectByGuidInRangeCheck(WorldObject const* obj, ObjectGuid guid, float range)
        : i_obj(obj), i_range(range), i_guid(guid)
    {
    }
    WorldObject const& GetFocusObject() const { return *i_obj; }
    bool operator()(GameObject* u)
    {
        if (u && i_obj->IsWithinDistInMap(u, i_range) && u->isSpawned() && u->GetGOInfo() && u->GetGUID() == i_guid)
            return true;

        return false;
    }

private:
    WorldObject const* i_obj;
    float i_range;
    ObjectGuid i_guid;
};
};  // namespace acore

Unit* PlayerbotAI::GetUnit(ObjectGuid guid)
{
    if (!guid)
        return nullptr;

    return ObjectAccessor::GetUnit(*bot, guid);
}

Player* PlayerbotAI::GetPlayer(ObjectGuid guid)
{
    Unit* unit = GetUnit(guid);
    return unit ? unit->ToPlayer() : nullptr;
}

uint32 GetCreatureIdForCreatureTemplateId(uint32 creatureTemplateId)
{
    // Map-thread safe: resolve from memory, never the world DB.
    for (auto const& [spawnId, creatureData] : sObjectMgr->GetAllCreatureData())
    {
        if (creatureData.id == creatureTemplateId)
            return spawnId;
    }

    return 0;
}

Unit* PlayerbotAI::GetUnit(CreatureData const* creatureData)
{
    if (!creatureData)
        return nullptr;

    Map* map = sMapMgr->FindMap(creatureData->mapid, 0);
    if (!map)
        return nullptr;

    uint32 spawnId = creatureData->spawnId;
    if (!spawnId)  // workaround for CreatureData with missing spawnId (this just uses first matching creatureId in DB,
                   // but thats ok this method is only used for battlemasters and theres only 1 of each type)
        spawnId = GetCreatureIdForCreatureTemplateId(creatureData->id);
    auto creatureBounds = map->GetCreatureBySpawnIdStore().equal_range(spawnId);
    if (creatureBounds.first == creatureBounds.second)
        return nullptr;

    return creatureBounds.first->second;
}

Creature* PlayerbotAI::GetCreature(ObjectGuid guid)
{
    if (!guid)
        return nullptr;

    return ObjectAccessor::GetCreature(*bot, guid);
}

GameObject* PlayerbotAI::GetGameObject(ObjectGuid guid)
{
    if (!guid)
        return nullptr;

    return ObjectAccessor::GetGameObject(*bot, guid);
}

// GameObject* PlayerbotAI::GetGameObject(GameObjectData const* gameObjectData)
// {
//     if (!gameObjectData)
//         return nullptr;

//     Map* map = sMapMgr->FindMap(gameObjectData->mapid, 0);
//     if (!map)
//         return nullptr;

//     auto gameobjectBounds = map->GetGameObjectBySpawnIdStore().equal_range(gameObjectData->spawnId);
//     if (gameobjectBounds.first == gameobjectBounds.second)
//         return nullptr;

//     return gameobjectBounds.first->second;
// }

WorldObject* PlayerbotAI::GetWorldObject(ObjectGuid guid)
{
    if (!guid)
        return nullptr;

    return ObjectAccessor::GetWorldObject(*bot, guid);
}

AreaTableEntry const* PlayerbotAI::GetCurrentArea()
{
    return sAreaTableStore.LookupEntry(
        bot->GetMap()->GetAreaId(bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()));
}

AreaTableEntry const* PlayerbotAI::GetCurrentZone()
{
    return sAreaTableStore.LookupEntry(
        bot->GetMap()->GetZoneId(bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()));
}

std::string PlayerbotAI::GetLocalizedAreaName(AreaTableEntry const* entry)
{
    std::string name;
    if (entry)
    {
        name = entry->area_name[sWorld->GetDefaultDbcLocale()];
        if (name.empty())
            name = entry->area_name[LOCALE_enUS];
    }

    return name;
}

std::string PlayerbotAI::GetLocalizedCreatureName(uint32 entry)
{
    std::string name;
    CreatureLocale const* cl = sObjectMgr->GetCreatureLocale(entry);
    if (cl)
        ObjectMgr::GetLocaleString(cl->Name, sWorld->GetDefaultDbcLocale(), name);
    if (name.empty())
    {
        CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(entry);
        if (ct)
            name = ct->Name;
    }
    return name;
}

std::string PlayerbotAI::GetLocalizedGameObjectName(uint32 entry)
{
    std::string name;
    GameObjectLocale const* gl = sObjectMgr->GetGameObjectLocale(entry);
    if (gl)
        ObjectMgr::GetLocaleString(gl->Name, sWorld->GetDefaultDbcLocale(), name);
    if (name.empty())
    {
        GameObjectTemplate const* gt = sObjectMgr->GetGameObjectTemplate(entry);
        if (gt)
            name = gt->name;
    }
    return name;
}

std::vector<Player*> PlayerbotAI::GetRealPlayersInGroup()
{
    std::vector<Player*> members;

    Group* group = bot->GetGroup();

    if (!group)
        return members;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();

        if (!member)
        {
            continue;
        }

        if (GET_PLAYERBOT_AI(member) && !IsSelfBot(member))
            continue;

        members.push_back(ref->GetSource());
    }

    return members;
}

std::vector<Player*> PlayerbotAI::GetAllPlayersInGroup()
{
    std::vector<Player*> members;

    Group* group = bot->GetGroup();

    if (!group)
        return members;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();

        if (!member)
        {
            continue;
        }

        members.push_back(ref->GetSource());
    }

    return members;
}

namespace
{
// [guild40] Channel and guild chat reach players on every map: run on a map thread, the broadcast walked
// Channel::playersStore while another map thread joined that channel (Player::UpdateLocalChannels on a zone
// change) and delivered packets into bots that another thread was updating (60 s freeze 2026-11-06 20:33,
// Channel::Say -> SendToAll looping). The core handles this chat on the world thread (CMSG_MESSAGECHAT,
// CMSG_JOIN_CHANNEL are PROCESS_THREADUNSAFE); bots do the same through the world thread processor.

// Channel lookups for map threads: a copy of both ChannelMgr maps refreshed on the world thread. Channels are
// never deleted before shutdown, so the pointers stay valid; the name and channel id never change.
struct BotChannelEntry
{
    ChannelMgr const* mgr;
    Channel* channel;
    uint32 channelId;
    std::string name;
};

std::shared_mutex botChannelDirLock;
std::vector<BotChannelEntry> botChannelDir;
std::size_t botChannelDirSize[2] = {SIZE_MAX, SIZE_MAX};

class BotChannelSayOperation : public PlayerbotOperation
{
public:
    BotChannelSayOperation(ObjectGuid botGuid, Channel* channel, std::string msg)
        : m_botGuid(botGuid), m_channel(channel), m_msg(std::move(msg))
    {
    }

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindPlayer(m_botGuid);
        if (!bot || !bot->IsInWorld() || !m_channel)
            return false;

        m_channel->Say(m_botGuid, m_msg, LANG_UNIVERSAL);
        return true;
    }

    ObjectGuid GetBotGuid() const override { return m_botGuid; }
    std::string GetName() const override { return "BotChannelSay"; }

private:
    ObjectGuid m_botGuid;
    Channel* m_channel;
    std::string m_msg;
};

class BotGuildSayOperation : public PlayerbotOperation
{
public:
    BotGuildSayOperation(ObjectGuid botGuid, std::string msg) : m_botGuid(botGuid), m_msg(std::move(msg)) {}

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindPlayer(m_botGuid);
        if (!bot || !bot->IsInWorld() || !bot->GetGuildId())
            return false;

        Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId());
        if (!guild || !guild->HasRankRight(bot, GR_RIGHT_GCHATSPEAK))
            return false;

        guild->BroadcastToGuild(bot->GetSession(), false, m_msg, LANG_UNIVERSAL);
        return true;
    }

    ObjectGuid GetBotGuid() const override { return m_botGuid; }
    std::string GetName() const override { return "BotGuildSay"; }

private:
    ObjectGuid m_botGuid;
    std::string m_msg;
};

void SayInChannel(Player* bot, Channel* channel, std::string const& msg)
{
    if (PlayerbotWorldThreadProcessor::IsWorldThread())
    {
        channel->Say(bot->GetGUID(), msg, LANG_UNIVERSAL);
        return;
    }

    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<BotChannelSayOperation>(bot->GetGUID(), channel, msg));
}
}  // namespace

void PlayerbotAI::RefreshChannelDirectory()
{
    ChannelMgr* const mgrs[2] = {ChannelMgr::forTeam(TEAM_ALLIANCE), ChannelMgr::forTeam(TEAM_HORDE)};
    bool changed = false;
    for (uint8 i = 0; i < 2; ++i)
        if (mgrs[i] && mgrs[i]->GetChannels().size() != botChannelDirSize[i])
            changed = true;

    if (!changed)
        return;

    std::vector<BotChannelEntry> dir;
    std::size_t sizes[2] = {0, 0};
    for (uint8 i = 0; i < 2; ++i)
    {
        if (!mgrs[i])
            continue;

        sizes[i] = mgrs[i]->GetChannels().size();
        if (i == 1 && mgrs[1] == mgrs[0])  // cross-faction channels: one manager for both teams
            continue;

        for (auto const& [key, channel] : mgrs[i]->GetChannels())
            if (channel)
                dir.push_back({mgrs[i], channel, channel->GetChannelId(), channel->GetName()});
    }

    std::unique_lock<std::shared_mutex> lock(botChannelDirLock);
    botChannelDir = std::move(dir);
    botChannelDirSize[0] = sizes[0];
    botChannelDirSize[1] = sizes[1];
}

bool PlayerbotAI::SayToGuild(std::string const& msg)
{
    if (msg.empty())
    {
        return false;
    }

    if (bot->GetGuildId())
    {
        if (Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId()))
        {
            if (!guild->HasRankRight(bot, GR_RIGHT_GCHATSPEAK))
            {
                return false;
            }
            // [guild40] the broadcast reaches members on every map: world thread only
            if (PlayerbotWorldThreadProcessor::IsWorldThread())
                guild->BroadcastToGuild(bot->GetSession(), false, msg.c_str(), LANG_UNIVERSAL);
            else
                PlayerbotWorldThreadProcessor::instance().QueueOperation(
                    std::make_unique<BotGuildSayOperation>(bot->GetGUID(), msg));
            return true;
        }
    }

    return false;
}

bool PlayerbotAI::SayToWorld(std::string const& msg)
{
    if (msg.empty())
    {
        return false;
    }

    ChannelMgr* cMgr = ChannelMgr::forTeam(bot->GetTeamId());
    if (!cMgr)
        return false;

    // no zone
    Channel* worldChannel = nullptr;
    {
        std::shared_lock<std::shared_mutex> lock(botChannelDirLock);
        for (BotChannelEntry const& entry : botChannelDir)
            if (entry.mgr == cMgr && StringEqualI(entry.name, "World"))
            {
                worldChannel = entry.channel;
                break;
            }
    }

    if (!worldChannel)
        return false;

    SayInChannel(bot, worldChannel, msg);
    return true;
}

bool PlayerbotAI::SayToChannel(std::string const& msg, ChatChannelId const& chanId)
{
    // Checks whether the message or ChannelMgr is valid
    if (msg.empty())
        return false;

    ChannelMgr* cMgr = ChannelMgr::forTeam(bot->GetTeamId());
    if (!cMgr)
        return false;

    AreaTableEntry const* current_zone = GetCurrentZone();
    if (!current_zone)
        return false;

    const auto current_str_zone = GetLocalizedAreaName(current_zone);

    Channel* target = nullptr;
    {
        std::shared_lock<std::shared_mutex> lock(botChannelDirLock);
        for (BotChannelEntry const& entry : botChannelDir)
        {
            // Checks if the channel matches the specified ChatChannelId
            if (entry.mgr != cMgr || entry.channelId != uint32(chanId))
                continue;

            // If the channel name is empty, skip it to avoid access problems
            if (entry.name.empty())
                continue;

            // Checks if the channel name contains the current zone
            bool const zoneless =
                chanId == ChatChannelId::LOOKING_FOR_GROUP || chanId == ChatChannelId::WORLD_DEFENSE;
            if (!zoneless && entry.name.find(current_str_zone) == std::string::npos)
                continue;

            target = entry.channel;
            break;
        }
    }

    if (!target)
        return false;

    SayInChannel(bot, target, msg);
    return true;
}

bool PlayerbotAI::SayToParty(std::string const& msg)
{
    if (!bot->GetGroup())
        return false;

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, LANG_UNIVERSAL, bot->GetGUID(), {}, msg, CHAT_TAG_NONE,
                                 bot->GetName());

    for (auto receiver : GetRealPlayersInGroup())
    {
        ServerFacade::instance().SendPacket(receiver, &data);
    }

    return true;
}

bool PlayerbotAI::SayToRaid(std::string const& msg)
{
    if (!bot->GetGroup() || !bot->GetGroup()->isRaidGroup())
        return false;

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID, LANG_UNIVERSAL, bot->GetGUID(), {}, msg, CHAT_TAG_NONE,
                                 bot->GetName());

    for (auto receiver : GetRealPlayersInGroup())
    {
        ServerFacade::instance().SendPacket(receiver, &data);
    }

    return true;
}

bool PlayerbotAI::Yell(std::string const& msg)
{
    if (bot->GetTeamId() == TeamId::TEAM_ALLIANCE)
    {
        bot->Yell(msg, LANG_COMMON);
    }
    else
    {
        bot->Yell(msg, LANG_ORCISH);
    }

    return true;
}

bool PlayerbotAI::Say(std::string const& msg)
{
    if (bot->GetTeamId() == TeamId::TEAM_ALLIANCE)
    {
        bot->Say(msg, LANG_COMMON);
    }
    else
    {
        bot->Say(msg, LANG_ORCISH);
    }

    return true;
}

bool PlayerbotAI::Whisper(std::string const& msg, std::string const& receiverName)
{
    const auto receiver = ObjectAccessor::FindPlayerByName(receiverName);
    if (!receiver)
    {
        return false;
    }

    if (bot->GetTeamId() == TeamId::TEAM_ALLIANCE)
    {
        bot->Whisper(msg, LANG_COMMON, receiver);
    }
    else
    {
        bot->Whisper(msg, LANG_ORCISH, receiver);
    }

    return true;
}

bool PlayerbotAI::TellMasterNoFacing(std::ostringstream& stream, PlayerbotSecurityLevel securityLevel)
{
    return TellMasterNoFacing(stream.str(), securityLevel);
}

bool PlayerbotAI::TellMasterNoFacing(std::string const text, PlayerbotSecurityLevel securityLevel)
{
    Player* master = GetMaster();
    PlayerbotAI* masterBotAI = nullptr;
    if (master)
        masterBotAI = GET_PLAYERBOT_AI(master);

    if ((!master || (masterBotAI && !IsSelfBot(master))) &&
        (sPlayerbotAIConfig.RandomBotSayWithoutMaster || HasStrategy("debug", BOT_STATE_NON_COMBAT)))
    {
        bot->Say(text, (bot->GetTeamId() == TEAM_ALLIANCE ? LANG_COMMON : LANG_ORCISH));
        return true;
    }

    if (!IsTellAllowed(securityLevel))
        return false;

    time_t lastSaid = whispers[text];

    if (!lastSaid || (time(nullptr) - lastSaid) >= sPlayerbotAIConfig.RepeatDelay / 1000)
    {
        whispers[text] = time(nullptr);

        ChatMsg type = CHAT_MSG_WHISPER;
        if (currentChat.second - time(nullptr) >= 1)
            type = currentChat.first;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, type == CHAT_MSG_ADDON ? CHAT_MSG_PARTY : type,
                                     type == CHAT_MSG_ADDON ? LANG_ADDON : LANG_UNIVERSAL, bot, nullptr, text.c_str());
        master->SendDirectMessage(&data);
    }

    return true;
}

bool PlayerbotAI::TellError(std::string const text, PlayerbotSecurityLevel securityLevel)
{
    Player* master = GetMaster();
    if (!IsTellAllowed(securityLevel) || !master || GET_PLAYERBOT_AI(master))
        return false;

    if (PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(master))
        mgr->TellError(bot->GetName(), text);

    return false;
}

bool PlayerbotAI::IsTellAllowed(PlayerbotSecurityLevel securityLevel)
{
    Player* master = GetMaster();
    if (!master || master->IsBeingTeleported())
        return false;

    if (!GetSecurity()->CheckLevelFor(securityLevel, true, master))
        return false;

    if (sPlayerbotAIConfig.WhisperDistance && !bot->GetGroup() && sRandomPlayerbotMgr.IsRandomBot(bot) &&
        !master->CanBeGameMaster() &&
        (bot->GetMapId() != master->GetMapId() ||
         ServerFacade::instance().GetDistance2d(bot, master) > sPlayerbotAIConfig.WhisperDistance))
        return false;

    return true;
}

bool PlayerbotAI::TellMaster(std::ostringstream& stream, PlayerbotSecurityLevel securityLevel)
{
    return TellMaster(stream.str(), securityLevel);
}

bool PlayerbotAI::TellMaster(std::string const text, PlayerbotSecurityLevel securityLevel)
{
    if (!master)
    {
        if (sPlayerbotAIConfig.RandomBotSayWithoutMaster)
            return TellMasterNoFacing(text, securityLevel);

        return false;
    }
    if (!TellMasterNoFacing(text, securityLevel))
        return false;

    if (!bot->isMoving() && !bot->IsInCombat() && bot->GetMapId() == master->GetMapId() &&
        !bot->HasUnitState(UNIT_STATE_IN_FLIGHT) && !bot->IsFlying())
    {
        if (!bot->HasInArc(EMOTE_ANGLE_IN_FRONT, master, sPlayerbotAIConfig.SightDistance))
            bot->SetFacingToObject(master);

        bot->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
    }

    return true;
}

bool IsRealAura(Player* bot, AuraEffect const* aurEff, Unit const* unit)
{
    if (!unit || !unit->IsInWorld() || unit->IsDuringRemoveFromWorld())
        return false;

    if (!aurEff)
        return false;

    if (!unit->IsHostileTo(bot))
        return true;

    SpellInfo const* spellInfo = aurEff->GetSpellInfo();
    if (!spellInfo)
        return false;

    uint32 stacks = aurEff->GetBase()->GetStackAmount();
    if (stacks >= spellInfo->StackAmount)
        return true;

    if (aurEff->GetCaster() == bot || spellInfo->IsPositive() ||
        spellInfo->Effects[aurEff->GetEffIndex()].IsAreaAuraEffect())
        return true;

    return false;
}

bool PlayerbotAI::HasAura(std::string const name, Unit* unit, bool maxStack, bool checkIsOwner, int maxAuraAmount,
                          bool checkDuration)
{
    if (!IsValidUnit(unit))
        return false;

    std::wstring wnamepart;
    if (!Utf8toWStr(name, wnamepart))
        return false;

    wstrToLower(wnamepart);

    int auraAmount = 0;

    // Iterate through all aura types
    for (uint32 auraType = SPELL_AURA_BIND_SIGHT; auraType < TOTAL_AURAS; auraType++)
    {
        Unit::AuraEffectList const& auras = unit->GetAuraEffectsByType((AuraType)auraType);
        if (auras.empty())
            continue;

        // Iterate through each aura effect
        for (AuraEffect const* aurEff : auras)
        {
            if (!aurEff)
                continue;

            SpellInfo const* spellInfo = aurEff->GetSpellInfo();
            if (!spellInfo)
                continue;

            // Check if the aura name matches
            std::string_view const auraName = spellInfo->SpellName[0];
            if (auraName.empty() || auraName.length() != wnamepart.length() || !Utf8FitTo(auraName, wnamepart))
                continue;

            // Check if this is a valid aura for the bot
            if (IsRealAura(bot, aurEff, unit))
            {
                // Check caster if necessary
                if (checkIsOwner && aurEff->GetCasterGUID() != bot->GetGUID())
                    continue;

                // Check aura duration if necessary
                if (checkDuration && aurEff->GetBase()->GetDuration() == -1)
                    continue;

                // Count stacks and charges
                uint32 maxStackAmount = spellInfo->StackAmount;
                uint32 maxProcCharges = spellInfo->ProcCharges;

                // Count the aura based on max stack and proc charges
                if (maxStack)
                {
                    if (maxStackAmount && aurEff->GetBase()->GetStackAmount() >= maxStackAmount)
                        auraAmount++;

                    if (maxProcCharges && aurEff->GetBase()->GetCharges() >= maxProcCharges)
                        auraAmount++;
                }
                else
                {
                    auraAmount++;
                }

                // Early exit if maxAuraAmount is reached
                if (maxAuraAmount < 0 && auraAmount > 0)
                    return true;
            }
        }
    }

    // Return based on the maximum aura amount conditions
    if (maxAuraAmount >= 0)
    {
        return auraAmount == maxAuraAmount || (auraAmount > 0 && auraAmount <= maxAuraAmount);
    }

    return false;
}

bool PlayerbotAI::HasSpell(std::string const spellName) const
{
    uint32 const spellId = aiObjectContext->GetValue<uint32>("spell id", spellName)->Get();
    return spellId && bot->HasSpell(spellId);
}

Aura* PlayerbotAI::GetAura(std::string const name, Unit* unit, bool checkIsOwner, bool checkDuration, int checkStack)
{
    if (!IsValidUnit(unit))
        return nullptr;

    std::wstring wnamepart;
    if (!Utf8toWStr(name, wnamepart))
        return nullptr;

    wstrToLower(wnamepart);

    for (uint32 auraType = SPELL_AURA_BIND_SIGHT; auraType < TOTAL_AURAS; ++auraType)
    {
        Unit::AuraEffectList const& auras = unit->GetAuraEffectsByType((AuraType)auraType);
        if (auras.empty())
            continue;

        for (AuraEffect const* aurEff : auras)
        {
            SpellInfo const* spellInfo = aurEff->GetSpellInfo();
            if (!spellInfo)
                continue;

            std::string const& auraName = spellInfo->SpellName[0];

            // Directly skip if name mismatch (both length and content)
            if (auraName.empty() || auraName.length() != wnamepart.length() || !Utf8FitTo(auraName, wnamepart))
                continue;

            if (!IsRealAura(bot, aurEff, unit))
                continue;

            // Check owner if necessary
            if (checkIsOwner && aurEff->GetCasterGUID() != bot->GetGUID())
                continue;

            // Check duration if necessary
            if (checkDuration && aurEff->GetBase()->GetDuration() == -1)
                continue;

            // Check stack if necessary
            if (checkStack != -1 && aurEff->GetBase()->GetStackAmount() < checkStack)
                continue;

            return aurEff->GetBase();
        }
    }

    return nullptr;
}

bool PlayerbotAI::HasAnyAuraOf(Unit* player, ...)
{
    if (!player)
        return false;

    va_list vl;
    va_start(vl, player);

    char const* cur;
    while ((cur = va_arg(vl, char const*)) != nullptr)
    {
        if (HasAura(cur, player))
        {
            va_end(vl);
            return true;
        }
    }

    va_end(vl);
    return false;
}

bool PlayerbotAI::CanCastSpell(std::string const name, Unit* target, Item* itemTarget)
{
    return CanCastSpell(aiObjectContext->GetValue<uint32>("spell id", name)->Get(), target, true, itemTarget);
}

// [cc_break] A spell the core lets a player cast under its current loss of control: Every Man for Himself and the
// PvP trinkets (stun, fear, charm), Berserker Rage and Will of the Forsaken (fear). Spell::CheckCasterAuras reads the
// same SPELL_ATTR5_ALLOW_WHILE_* flags. hor-h build 29: the paladin tank stood feared 5 s by a Ghostly Priest with
// "every man for himself (isPossible false: lost control)" while the mob it should take stayed on the shaman.
static bool CastableUnderLossOfControl(Player* bot, uint32 spellId)
{
    if (bot->HasUnitState(UNIT_STATE_JUMPING | UNIT_STATE_CHARGING))
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return false;

    if (bot->HasUnitState(UNIT_STATE_STUNNED) && !spellInfo->HasAttribute(SPELL_ATTR5_ALLOW_WHILE_STUNNED))
        return false;

    if (bot->HasUnitState(UNIT_STATE_FLEEING) && !spellInfo->HasAttribute(SPELL_ATTR5_ALLOW_WHILE_FLEEING))
        return false;

    if (bot->HasUnitState(UNIT_STATE_CONFUSED) && !spellInfo->HasAttribute(SPELL_ATTR5_ALLOW_WHILE_CONFUSED))
        return false;

    return true;
}

bool PlayerbotAI::CanCastSpell(uint32 spellid, Unit* target, bool checkHasSpell, Item* itemTarget, Item* castItem)
{
    if (!spellid)
    {
        if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && HasGameClientMaster()))
            LOG_DEBUG("playerbots", "Can cast spell failed. No spellid. - spellid: {}, bot name: {}", spellid, bot->GetName());

        return false;
    }

    if (bot->HasUnitState(UNIT_STATE_LOST_CONTROL) && !CastableUnderLossOfControl(bot, spellid))
    {
        if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && HasGameClientMaster()))
            LOG_DEBUG("playerbots", "Can cast spell failed. Unit state lost control. - spellid: {}, bot name: {}", spellid, bot->GetName());

        return false;
    }

    if (!target)
        target = bot;

     if (!IsValidUnit(target))
        return false;

    if (Pet* pet = bot->GetPet())
        if (pet->HasSpell(spellid))
            return true;

    if (checkHasSpell && !bot->HasSpell(spellid))
    {
        if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && HasGameClientMaster()))
            LOG_DEBUG("playerbots", "Can cast spell failed. Bot not has spell. - target name: {}, spellid: {}, bot name: {}",
                target->GetName(), spellid, bot->GetName());

        return false;
    }

    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr)
    {
        if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && HasGameClientMaster()))
            LOG_DEBUG("playerbots", "CanCastSpell() target name: {}, spellid: {}, bot name: {}, failed because has current channeled spell",
                target->GetName(), spellid, bot->GetName());

        return false;
    }

    if (bot->HasSpellCooldown(spellid))
    {
        if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && HasGameClientMaster()))
            LOG_DEBUG("playerbots", "Can cast spell failed. Spell not has cooldown. - target name: {}, spellid: {}, bot name: {}",
                target->GetName(), spellid, bot->GetName());

        return false;
    }

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellid);
    if (!spellInfo)
    {
        if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && HasGameClientMaster()))
            LOG_DEBUG("playerbots", "Can cast spell failed. No spellInfo. - target name: {}, spellid: {}, bot name: {}",
                target->GetName(), spellid, bot->GetName());

        return false;
    }

    // A reflected spell lands on the bot (a mage killed by its own Deep Freeze), and a counterattack aura strikes
    // back at every melee hit; the bot waits it out or another target is picked (DpsTargetValue).
    if (target != bot)
    {
        bool const countered = spellInfo->DmgClass == SPELL_DAMAGE_CLASS_MELEE &&
                               ReflectAuras::CountersMelee(target, bot) && !IsTank(bot);
        if ((countered || ReflectAuras::ReflectsSpell(target, spellInfo)) && bot->IsHostileTo(target))
        {
            if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && HasGameClientMaster()))
                LOG_DEBUG("playerbots", "Can cast spell failed. Target turns it back. - target name: {}, spellid: {}, "
                          "bot name: {}", target->GetName(), spellid, bot->GetName());

            return false;
        }
    }

    if ((bot->GetShapeshiftForm() == FORM_FLIGHT || bot->GetShapeshiftForm() == FORM_FLIGHT_EPIC) && !bot->IsInCombat())
    {
        if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && HasGameClientMaster()))
            LOG_DEBUG("playerbots", "Can cast spell failed. In flight form (not in combat). - target name: {}, spellid: {}, bot name: {}",
                target->GetName(), spellid, bot->GetName());

        return false;
    }

    uint32 CastingTime = !spellInfo->IsChanneled() ? spellInfo->CalcCastTime(bot) : spellInfo->GetDuration();
    // bool interruptOnMove = spellInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_MOVEMENT;
    if ((CastingTime || spellInfo->IsAutoRepeatRangedSpell()) && bot->isMoving())
    {
        if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && HasGameClientMaster()))
            LOG_DEBUG("playerbots", "Casting time and bot is moving - target name: {}, spellid: {}, bot name: {}",
                target->GetName(), spellid, bot->GetName());

        return false;
    }

    if (!itemTarget)
    {
        // Exception for Deep Freeze (44572) - allow cast for damage on immune targets (e.g., bosses)
        // [lead1230] Banish on a target the bot banished is a cancel, not a refusal: the immune hit removes it
        // (core spell_warl_banish, -710); b245 rfc-h 5F-14 t=433-517: the check refused it until the Banish ran out,
        // and the cast then landed a fresh 30 s one (twice: 64 s of the whole group on StopDps)
        bool const banishCancel =
            sSpellMgr->GetFirstSpellInChain(spellid) == 710 && target->GetAura(spellid, bot->GetGUID());
        if (target->IsImmunedToSpell(spellInfo) && !banishCancel)
        {
            if (spellid != 44572)  // Deep Freeze
            {
                if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && HasGameClientMaster()))
                    LOG_DEBUG("playerbots", "target is immuned to spell - target name: {}, spellid: {}, bot name: {}",
                        target->GetName(), spellid, bot->GetName());

                return false;
            }
            // Otherwise, allow Deep Freeze even if immune
        }

        if (bot != target && ServerFacade::instance().GetDistance2d(bot, target) > sPlayerbotAIConfig.SightDistance)
        {
            if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && HasGameClientMaster()))
                LOG_DEBUG("playerbots", "target is out of sight distance - target name: {}, spellid: {}, bot name: {}",
                    target->GetName(), spellid, bot->GetName());

            return false;
        }
    }

    if (itemTarget == nullptr)
        itemTarget = aiObjectContext->GetValue<Item*>("item for spell", spellid)->Get();

    // TRIGGERED_IGNORE_POWER_AND_REAGENT_COST for not calling CheckPower in check
    // which avoids buff charge to be ineffectively reduced (e.g. dk freezing fog for howling blast)
    /// @TODO: Fix all calls to ApplySpellMod
    // [lead1390] the rotation keeps its own resource rules (the power is its triggers', see RageForStrikeSoon)
    TriggerCastFlags const flags = TRIGGERED_IGNORE_POWER_AND_REAGENT_COST;
    CastRun const first = RunCheckCast(spellInfo, target, itemTarget, castItem, flags);
    SpellCastResult result = first.cast;
    // [lead1390] an answer its prerequisites fix (the form left: "caster form"; the walk into range: "reach spell")
    // or a passing state passes, but Spell::CheckCast stopped at it and the checks after it never ran: b259 rfc-h
    // 5H-8 the cat's Revive (NOT_SHAPESHIFT), b260 5E-13 the Shadowfiend out of range. The checks behind the form
    // (TRIGGERED_IGNORE_SHAPESHIFT) and the caster's auras (Spell::CheckCasterAuras: silenced, pacified) answer too
    auto const passes = [](SpellCastResult answer)
    {
        switch (answer)
        {
            case SPELL_FAILED_NOT_INFRONT:
            case SPELL_FAILED_NOT_STANDING:
            case SPELL_FAILED_UNIT_NOT_INFRONT:
            case SPELL_FAILED_MOVING:
            case SPELL_FAILED_TRY_AGAIN:
            case SPELL_CAST_OK:
            case SPELL_FAILED_NOT_SHAPESHIFT:
            case SPELL_FAILED_OUT_OF_RANGE:
                return true;
            default:
                return false;
        }
    };
    if (passes(result) && result != SPELL_CAST_OK)
    {
        CastRun const behind = result == SPELL_FAILED_NOT_SHAPESHIFT
                                   ? RunCheckCast(spellInfo, target, itemTarget, castItem,
                                                  TriggerCastFlags(flags | TRIGGERED_IGNORE_SHAPESHIFT))
                                   : first;
        if (!passes(behind.cast))
            result = behind.cast;
        else if (behind.cast != SPELL_CAST_OK && behind.auras != SPELL_CAST_OK)
            result = behind.auras;
    }

    if (passes(result))
        return true;

    if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && HasGameClientMaster()))
        LOG_DEBUG("playerbots", "CanCastSpell Check Failed. - target name: {}, spellid: {}, bot name: {}, result: {}",
                  target->GetName(), spellid, bot->GetName(), result);

    return false;
}

PlayerbotAI::CastRun PlayerbotAI::RunCheckCast(SpellInfo const* spellInfo, Unit* target, Item* itemTarget,
                                               Item* castItem, TriggerCastFlags flags)
{
    CastRun run;
    Unit* oldSel = bot->GetSelectedUnit();
    Spell* spell = new Spell(bot, spellInfo, flags);
    spell->m_targets.SetUnitTarget(target);
    spell->m_CastItem = castItem;
    spell->m_targets.SetItemTarget(itemTarget);
    run.cast = spell->CheckCast(true);
    if (run.cast != SPELL_CAST_OK && !(flags & TRIGGERED_IGNORE_CASTER_AURAS))
        run.auras = spell->CheckCasterAuras(false);
    if (spellInfo->PowerType == POWER_RUNE && !castItem && !(flags & TRIGGERED_IGNORE_POWER_AND_REAGENT_COST))
        run.runes = spell->CheckRuneCost(spellInfo->RuneCostID);
    delete spell;

    if (oldSel)
        bot->SetSelection(oldSel->GetGUID());

    return run;
}

bool PlayerbotAI::IsPassingCastAnswer(SpellCastResult result, SpellInfo const* spellInfo, Item* castItem) const
{
    switch (result)
    {
        case SPELL_FAILED_MOVING:             // the walk stops (the cast waits for it)
        case SPELL_FAILED_NOT_STANDING:       // CastSpell stands the bot up and casts on the next tick
        case SPELL_FAILED_UNIT_NOT_INFRONT:   // CastSpell turns the bot to its target
        case SPELL_FAILED_TRY_AGAIN:          // a lockpick roll
        case SPELL_FAILED_SPELL_IN_PROGRESS:  // a cast or channel under way ends
        case SPELL_FAILED_INTERRUPTED:
        // [lead1482] b268ac ac-h: 27 orders / credits refused SPELL_FAILED_CONFUSED under Exarch Maladaar's Soul
        // Scream (32421, SPELL_AURA_MOD_CONFUSE, its own duration): Spell::CheckCasterAuras refuses only while the
        // confusion holds - it runs out, the cast is possible again
        case SPELL_FAILED_CONFUSED:
            return true;
        case SPELL_FAILED_NOT_READY:
            // the global cooldown only: no cooldown of its own, no potion of this combat holding the item
            return spellInfo && !bot->HasSpellCooldown(spellInfo->Id) &&
                   !(castItem && bot->GetLastPotionId() &&
                     (castItem->IsPotion() || spellInfo->IsCooldownStartedOnEvent())) &&
                   bot->GetGlobalCooldownMgr().HasGlobalCooldown(spellInfo);
        default:
            return false;
    }
}

std::string PlayerbotAI::CastResultName(SpellCastResult result) { return EnumUtils::ToConstant(result); }

PlayerbotAI::CastCheck PlayerbotAI::CheckCastNow(uint32 spellId, Unit* target, Item* castItem, TriggerCastFlags flags)
{
    CastCheck check;
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!spellInfo)
    {
        check.result = check.refusal = check.behindForm = SPELL_FAILED_UNKNOWN;
        return check;
    }

    if (!target)
        target = bot;

    if (!IsValidUnit(target))
    {
        check.result = check.refusal = check.behindForm = SPELL_FAILED_BAD_TARGETS;
        return check;
    }

    // the pet's own spell: its owner toggles it, the pet's cast check is its own (Spell::CheckPetCast)
    if (Pet* pet = bot->GetPet())
        if (pet->HasSpell(spellId))
            return check;

    bool const own = !castItem && flags == TRIGGERED_NONE;
    // the answers in the game's order; formAt: the NOT_SHAPESHIFT the form's leave removes (-1: none)
    std::vector<SpellCastResult> answers;
    int32 formAt = -1;
    if (own && !bot->HasSpell(spellId))
        answers.push_back(SPELL_FAILED_NOT_KNOWN);

    if (!(flags & TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD) && bot->HasSpellCooldown(spellId))
        answers.push_back(SPELL_FAILED_NOT_READY);

    // a channel under way: the bot's cast waits for its end (PlayerbotAI::CastSpell does not break it)
    if (!(flags & TRIGGERED_IGNORE_CAST_IN_PROGRESS) && bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        answers.push_back(SPELL_FAILED_SPELL_IN_PROGRESS);

    Item* itemTarget = own ? aiObjectContext->GetValue<Item*>("item for spell", spellId)->Get() : nullptr;
    CastRun run = RunCheckCast(spellInfo, target, itemTarget, castItem, flags);
    answers.push_back(run.cast);
    if (run.cast == SPELL_FAILED_NOT_SHAPESHIFT && !(flags & TRIGGERED_IGNORE_SHAPESHIFT))
    {
        formAt = int32(answers.size()) - 1;
        run = RunCheckCast(spellInfo, target, itemTarget, castItem,
                           TriggerCastFlags(flags | TRIGGERED_IGNORE_SHAPESHIFT));
        answers.push_back(run.cast);
    }
    if (run.cast != SPELL_CAST_OK)
        answers.push_back(run.auras);

    // Spell::m_powerCost is computed in Spell::prepare: the cast check alone never refuses for power
    if (!castItem && !(flags & TRIGGERED_IGNORE_POWER_AND_REAGENT_COST) && !bot->GetCommandStatus(CHEAT_POWER))
    {
        // Spell::CheckPower's order: health as power, the runes, the power amount
        if (spellInfo->PowerType == POWER_HEALTH)
        {
            if (int32(bot->GetHealth()) <= spellInfo->CalcPowerCost(bot, spellInfo->GetSchoolMask()))
                answers.push_back(SPELL_FAILED_CASTER_AURASTATE);
        }
        else if (spellInfo->PowerType == POWER_RUNE)
            answers.push_back(run.runes);
        else if (spellInfo->PowerType < MAX_POWERS &&
                 int32(bot->GetPower(Powers(spellInfo->PowerType))) <
                     spellInfo->CalcPowerCost(bot, spellInfo->GetSchoolMask()))
            answers.push_back(SPELL_FAILED_NO_POWER);
    }

    bool refusalFound = false;
    bool behindFound = false;
    for (int32 i = 0; i < int32(answers.size()); ++i)
    {
        SpellCastResult const answer = answers[i];
        if (answer == SPELL_CAST_OK)
            continue;

        if (check.result == SPELL_CAST_OK)
            check.result = answer;

        if (IsPassingCastAnswer(answer, spellInfo, castItem))
            continue;

        if (!refusalFound)
        {
            check.refusal = answer;
            refusalFound = true;
        }

        if (!behindFound && i != formAt)
        {
            check.behindForm = answer;
            behindFound = true;
        }
    }

    return check;
}

bool PlayerbotAI::CanCastSpell(uint32 spellid, GameObject* goTarget, bool checkHasSpell, Item* castItem)
{
    if (!spellid)
        return false;

    if (bot->HasUnitState(UNIT_STATE_LOST_CONTROL))
        return false;

    Pet* pet = bot->GetPet();
    if (pet && pet->HasSpell(spellid))
        return true;

    if (checkHasSpell && !bot->HasSpell(spellid))
        return false;

    if (bot->HasSpellCooldown(spellid))
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellid);
    if (!spellInfo)
        return false;

    int32 CastingTime = !spellInfo->IsChanneled() ? spellInfo->CalcCastTime(bot) : spellInfo->GetDuration();
    if (CastingTime > 0 && bot->isMoving())
        return false;

    if (ServerFacade::instance().GetDistance2d(bot, goTarget) > sPlayerbotAIConfig.SightDistance)
        return false;

    // ObjectGuid oldSel = bot->GetTarget();
    // bot->SetTarget(goTarget->GetGUID());
    Spell* spell = new Spell(bot, spellInfo, TRIGGERED_NONE);

    spell->m_CastItem = castItem;
    spell->m_targets.SetGOTarget(goTarget);
    Item* item = aiObjectContext->GetValue<Item*>("item for spell", spellid)->Get();
    spell->m_targets.SetItemTarget(item);

    SpellCastResult result = spell->CheckCast(true);
    delete spell;
    // if (oldSel)
    //     bot->SetTarget(oldSel);

    switch (result)
    {
        case SPELL_FAILED_NOT_INFRONT:
        case SPELL_FAILED_NOT_STANDING:
        case SPELL_FAILED_UNIT_NOT_INFRONT:
        case SPELL_FAILED_MOVING:
        case SPELL_FAILED_TRY_AGAIN:
        case SPELL_CAST_OK:
            return true;
        default:
            break;
    }

    return false;
}

bool PlayerbotAI::CanCastSpell(uint32 spellid, float x, float y, float z, bool checkHasSpell, Item* itemTarget)
{
    if (!spellid)
        return false;

    Pet* pet = bot->GetPet();
    if (pet && pet->HasSpell(spellid))
        return true;

    if (checkHasSpell && !bot->HasSpell(spellid))
        return false;

    if (bot->HasSpellCooldown(spellid))
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellid);
    if (!spellInfo)
        return false;

    if (!itemTarget)
    {
        if (bot->GetDistance(x, y, z) > sPlayerbotAIConfig.SightDistance)
            return false;
    }

    Spell* spell = new Spell(bot, spellInfo, TRIGGERED_NONE);

    spell->m_targets.SetDst(x, y, z, 0.f);

    Item* item = itemTarget ? itemTarget : aiObjectContext->GetValue<Item*>("item for spell", spellid)->Get();
    spell->m_targets.SetItemTarget(item);

    SpellCastResult result = spell->CheckCast(true);
    delete spell;

    switch (result)
    {
        case SPELL_FAILED_NOT_INFRONT:
        case SPELL_FAILED_NOT_STANDING:
        case SPELL_FAILED_UNIT_NOT_INFRONT:
        case SPELL_FAILED_MOVING:
        case SPELL_FAILED_TRY_AGAIN:
        case SPELL_CAST_OK:
            return true;
        default:
            return false;
    }
}

namespace
{
// [b44_cast] Opt-in cast timeline for the caster-filler audit (ROTATION_AUDIT_NIGHT.md, b44_cast): environment variable
// PLAYERBOTS_B44_CAST = "*" or a comma-separated list of bot names, read once. Only a bot in combat inside a dungeon
// or raid. One line per cast start (spell, cast time with haste, GCD) and per cast this file stops (reason); offline:
// next start - (start + castMs) = the gap after a cast, negative = the cast ended early (cancelled / interrupted).
bool B44CastTraced(Player* bot)
{
    static std::string const filter = []()
    {
        char const* value = std::getenv("PLAYERBOTS_B44_CAST");
        return std::string(value ? value : "");
    }();
    if (filter.empty() || !bot || !bot->IsInCombat() || !bot->GetMap() || !bot->GetMap()->Instanceable())
        return false;

    if (filter == "*")
        return true;

    std::string const name = "," + bot->GetName() + ",";
    return ("," + filter + ",").find(name) != std::string::npos;
}

void B44CastTrace(Player* bot, char const* what, SpellInfo const* info, Unit* target)
{
    if (!info || !B44CastTraced(bot))
        return;

    uint32 const castMs = info->IsChanneled() ? uint32(std::max(info->GetDuration(), 0)) : info->CalcCastTime(bot);
    LOG_DEBUG("playerbots", "[b44_cast] {} {} {} {} ms={} cast={} gcd={} moving={} target={}", bot->GetName(), what,
              info->Id, info->SpellName[LOCALE_enUS] ? info->SpellName[LOCALE_enUS] : "", getMSTime(), castMs,
              bot->GetGlobalCooldownMgr().GetGlobalCooldown(info), bot->isMoving(),
              target ? target->GetName() : std::string());
}
}  // namespace

bool PlayerbotAI::CastSpell(std::string const name, Unit* target, Item* itemTarget)
{
    if (!IsValidUnit(target))
        return false;

    bool result = CastSpell(aiObjectContext->GetValue<uint32>("spell id", name)->Get(), target, itemTarget);
    if (result)
    {
        aiObjectContext->GetValue<time_t>("last spell cast time", name)->Set(time(nullptr));
    }

    return result;
}

bool PlayerbotAI::CastSpell(uint32 spellId, Unit* target, Item* itemTarget)
{
    if (!spellId)
        return false;

    if (!target)
        target = bot;

    if (!IsValidUnit(target))
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return false;

    Pet* pet = bot->GetPet();
    if (pet && pet->HasSpell(spellId))
    {
        // List of spell IDs for which we do NOT want to toggle auto-cast or send message
        // We are excluding Spell Lock and Devour Magic because they are casted in the GenericWarlockStrategy
        // Without this exclusion, the skill would be togged for auto-cast and the player would
        // be spammed with messages about enabling/disabling auto-cast
        switch (spellId)
        {
            case 19244:  // Spell Lock rank 1
            case 19647:  // Spell Lock rank 2
            case 19505:  // Devour Magic rank 1
            case 19731:  // Devour Magic rank 2
            case 19734:  // Devour Magic rank 3
            case 19736:  // Devour Magic rank 4
            case 27276:  // Devour Magic rank 5
            case 27277:  // Devour Magic rank 6
            case 48011:  // Devour Magic rank 7
                // No message - just break out of the switch and let normal cast logic continue
                break;
            default:
                bool autocast = false;
                for (unsigned int& m_autospell : pet->m_autospells)
                {
                    if (m_autospell == spellId)
                    {
                        autocast = true;
                        break;
                    }
                }

                pet->ToggleAutocast(spellInfo, !autocast);
                std::ostringstream out;
                out << (autocast ? "|cffff0000|Disabling" : "|cFF00ff00|Enabling") << " pet auto-cast for ";
                out << chatHelper.FormatSpell(spellInfo);
                TellMaster(out);
                return true;
        }
    }

    // aiObjectContext->GetValue<LastMovement&>("last movement")->Get().Set(nullptr);
    // aiObjectContext->GetValue<time_t>("stay time")->Set(0);

    if ((bot->IsFlying() && !bot->HasAura(SPELL_GRAVITY_LAPSE_TK) && !bot->HasAura(SPELL_GRAVITY_LAPSE_MGT)) ||
        bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
    {
        return false;
    }

    // bot->ClearUnitState(UNIT_STATE_CHASE);
    // bot->ClearUnitState(UNIT_STATE_FOLLOW);

    bool failWithDelay = false;
    if (!bot->IsStandState())
    {
        bot->SetStandState(UNIT_STAND_STATE_STAND);
        failWithDelay = true;
    }

    ObjectGuid oldSel = bot->GetSelectedUnit() ? bot->GetSelectedUnit()->GetGUID() : ObjectGuid();
    bot->SetSelection(target->GetGUID());
    WorldObject* faceTo = target;
    if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, faceTo) && (spellInfo->FacingCasterFlags & SPELL_FACING_FLAG_INFRONT))
    {
        ServerFacade::instance().SetFacingTo(bot, faceTo);
        // failWithDelay = true;
    }

    if (failWithDelay)
    {
        SetNextCheckDelay(sPlayerbotAIConfig.ReactDelay);
        return false;
    }

    Spell* spell = new Spell(bot, spellInfo, TRIGGERED_NONE);

    bool const isOpeningSpell =
        spellInfo->HasEffect(SPELL_EFFECT_OPEN_LOCK) || spellInfo->HasEffect(SPELL_EFFECT_SKINNING);
    SpellCastTargets targets;
    if (!isOpeningSpell && (spellInfo->Targets & TARGET_FLAG_ITEM || spellInfo->Targets & TARGET_FLAG_GAMEOBJECT_ITEM))
    {
        Item* item = itemTarget ? itemTarget : aiObjectContext->GetValue<Item*>("item for spell", spellId)->Get();
        targets.SetItemTarget(item);

        if (bot->GetTradeData())
        {
            bot->GetTradeData()->SetSpell(spellId);
            delete spell;
            // if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && HasGameClientMaster()))
            //     LOG_DEBUG("playerbots", "Spell cast no item - target name: {}, spellid: {}, bot name: {}",
            //         target->GetName(), spellId, bot->GetName());

            return true;
        }
    }
    else if (spellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
    {
        // WorldLocation aoe = aiObjectContext->GetValue<WorldLocation>("aoe position")->Get();
        // targets.SetDst(aoe);
        targets.SetDst(*target);
    }
    else if (spellInfo->Targets & TARGET_FLAG_SOURCE_LOCATION)
    {
        targets.SetDst(*bot);
    }
    else
    {
        targets.SetUnitTarget(target);
    }

    if (isOpeningSpell)
    {
        LootObject loot = *aiObjectContext->GetValue<LootObject>("loot target");
        GameObject* go = GetGameObject(loot.guid);
        // Use the loot-target object if it exists and is spawned, and either no item was given or the item is its key.
        if (go && go->isSpawned() && (!itemTarget || itemTarget->GetEntry() == loot.reqItem))
        {
            switch (go->GetGoType())
            {
                case GAMEOBJECT_TYPE_DOOR:
                case GAMEOBJECT_TYPE_BUTTON:
                case GAMEOBJECT_TYPE_QUESTGIVER:
                case GAMEOBJECT_TYPE_CHEST:
                case GAMEOBJECT_TYPE_GENERIC:
                case GAMEOBJECT_TYPE_SPELL_FOCUS:
                case GAMEOBJECT_TYPE_GOOBER:
                case GAMEOBJECT_TYPE_FLAGSTAND:
                    break;
                default:
                {
                    WorldPacket packetgouse(CMSG_GAMEOBJ_USE, 8);
                    packetgouse << loot.guid;
                    bot->GetSession()->HandleGameObjectUseOpcode(packetgouse);
                    break;
                }
            }

            targets.SetGOTarget(go);
            faceTo = go;
            ServerFacade::instance().SetFacingTo(bot, go);
            if (itemTarget && spellInfo->HasEffect(SPELL_EFFECT_OPEN_LOCK) && itemTarget->GetEntry() == loot.reqItem)
                spell->m_CastItem = itemTarget;
        }
        else if (itemTarget)
        {
            Player* trader = bot->GetTrader();
            if (trader)
            {
                targets.SetTradeItemTarget(bot);
                targets.SetUnitTarget(bot);
                faceTo = trader;
            }
            else
            {
                targets.SetItemTarget(itemTarget);
            }
        }
        else
        {
            if (Unit* creature = GetUnit(loot.guid))
            {
                targets.SetUnitTarget(creature);
                faceTo = creature;
            }
        }
    }

    // Before prepare(): afterwards the event owns it and GetCastTime() becomes valid; a
    // CanPrepare veto returns before the event exists, so the spell would leak instead.
    if (bot->isMoving() && (spellInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_MOVEMENT) &&
        spellInfo->CalcCastTime(bot) > 0)
    {
        // bot->StopMoving();
        SetNextCheckDelay(sPlayerbotAIConfig.ReactDelay);
        delete spell;
        return false;
    }

    // spell->m_targets.SetUnitTarget(target);
    // SpellCastResult spellSuccess = spell->CheckCast(true);
    // if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && HasGameClientMaster()))
    //     LOG_DEBUG("playerbots", "Spell cast result - target name: {}, spellid: {}, bot name: {}, result: {}",
    //         target->GetName(), spellId, bot->GetName(), spellSuccess);

    // if (spellSuccess != SPELL_CAST_OK)
    //     return false;

    SpellCastResult result = spell->prepare(&targets);

    if (result != SPELL_CAST_OK)
    {
        // if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && HasGameClientMaster()))
        //     LOG_DEBUG("playerbots", "Spell cast failed. - target name: {}, spellid: {}, bot name: {}, result: {}",
        //         target->GetName(), spellId, bot->GetName(), result);

        if (HasStrategy("debug spell", BOT_STATE_NON_COMBAT))
        {
            std::ostringstream out;
            out << "Spell cast failed - ";
            out << "Spell ID: " << spellId << " (" << ChatHelper::FormatSpell(spellInfo) << "), ";
            out << "Error Code: " << static_cast<int>(result) << " (0x" << std::hex << static_cast<int>(result)
                << std::dec << "), ";
            out << "Bot: " << bot->GetName() << ", ";

            // Check spell target type
            if (targets.GetUnitTarget())
            {
                out << "Target: Unit (" << targets.GetUnitTarget()->GetName()
                    << ", Low GUID: " << targets.GetUnitTarget()->GetGUID().GetCounter()
                    << ", High GUID: " << static_cast<uint32>(targets.GetUnitTarget()->GetGUID().GetHigh()) << "), ";
            }

            if (targets.GetGOTarget())
            {
                out << "Target: GameObject (Low GUID: " << targets.GetGOTarget()->GetGUID().GetCounter()
                    << ", High GUID: " << static_cast<uint32>(targets.GetGOTarget()->GetGUID().GetHigh()) << "), ";
            }

            if (targets.GetItemTarget())
            {
                out << "Target: Item (Low GUID: " << targets.GetItemTarget()->GetGUID().GetCounter()
                    << ", High GUID: " << static_cast<uint32>(targets.GetItemTarget()->GetGUID().GetHigh()) << "), ";
            }

            // Check if bot is in trade mode
            if (bot->GetTradeData())
            {
                out << "Trade Mode: Active, ";
                Item* tradeItem = bot->GetTradeData()->GetTraderData()->GetItem(TRADE_SLOT_NONTRADED);
                if (tradeItem)
                {
                    out << "Trade Item: " << tradeItem->GetEntry()
                        << " (Low GUID: " << tradeItem->GetGUID().GetCounter()
                        << ", High GUID: " << static_cast<uint32>(tradeItem->GetGUID().GetHigh()) << "), ";
                }
                else
                {
                    out << "Trade Item: None, ";
                }
            }
            else
            {
                out << "Trade Mode: Inactive, ";
            }

            TellMasterNoFacing(out);
        }

        return false;
    }
    // if (spellInfo->Effects[0].Effect == SPELL_EFFECT_OPEN_LOCK || spellInfo->Effects[0].Effect ==
    // SPELL_EFFECT_SKINNING)
    // {
    //     LootObject loot = *aiObjectContext->GetValue<LootObject>("loot target");
    //     if (!loot.IsLootPossible(bot))
    //     {
    //         spell->cancel();
    //         delete spell;
    //         if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && HasGameClientMaster()))
    //             LOG_DEBUG("playerbots", "Spell cast loot - target name: {}, spellid: {}, bot name: {}",
    //                 target->GetName(), spellId, bot->GetName());

    //         return false;
    //     }
    // }

    // WaitForSpellCast(spell);

    aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get().Set(spellId, target->GetGUID(), time(nullptr));

    aiObjectContext->GetValue<PositionMap&>("position")->Get()["random"].Reset();

    if (oldSel)
        bot->SetSelection(oldSel);

    if (HasStrategy("debug spell", BOT_STATE_NON_COMBAT))
    {
        std::ostringstream out;
        out << "Casting " << ChatHelper::FormatSpell(spellInfo);
        TellMasterNoFacing(out);
    }

    forceRebuff.NoteCast(spellInfo);
    lastCastSpellId = spellId;
    B44CastTrace(bot, "start", spellInfo, target);  // [b44_cast]

    return true;
}

bool PlayerbotAI::CastSpell(uint32 spellId, float x, float y, float z, Item* itemTarget)
{
    if (!spellId)
        return false;

    Pet* pet = bot->GetPet();
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (pet && pet->HasSpell(spellId))
    {
        bool autocast = false;
        for (unsigned int& m_autospell : pet->m_autospells)
        {
            if (m_autospell == spellId)
            {
                autocast = true;
                break;
            }
        }

        pet->ToggleAutocast(spellInfo, !autocast);
        std::ostringstream out;
        out << (autocast ? "|cffff0000|Disabling" : "|cFF00ff00|Enabling") << " pet auto-cast for ";
        out << chatHelper.FormatSpell(spellInfo);
        TellMaster(out);
        return true;
    }

    // aiObjectContext->GetValue<LastMovement&>("last movement")->Get().Set(nullptr);
    // aiObjectContext->GetValue<time_t>("stay time")->Set(0);

    // MotionMaster& mm = *bot->GetMotionMaster();

    if ((bot->IsFlying() && !bot->HasAura(SPELL_GRAVITY_LAPSE_TK) && !bot->HasAura(SPELL_GRAVITY_LAPSE_MGT)) ||
        bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
    {
        return false;
    }

    // bot->ClearUnitState(UNIT_STATE_CHASE);
    // bot->ClearUnitState(UNIT_STATE_FOLLOW);

    bool failWithDelay = false;
    if (!bot->IsStandState())
    {
        bot->SetStandState(UNIT_STAND_STATE_STAND);
        failWithDelay = true;
    }

    ObjectGuid oldSel = bot->GetSelectedUnit() ? bot->GetSelectedUnit()->GetGUID() : ObjectGuid();

    if (!bot->isMoving())
        bot->SetFacingTo(bot->GetAngle(x, y));

    if (failWithDelay)
    {
        SetNextCheckDelay(sPlayerbotAIConfig.GlobalCoolDown);
        return false;
    }

    Spell* spell = new Spell(bot, spellInfo, TRIGGERED_NONE);

    SpellCastTargets targets;
    if (spellInfo->Targets & TARGET_FLAG_ITEM || spellInfo->Targets & TARGET_FLAG_GAMEOBJECT_ITEM)
    {
        Item* item = itemTarget ? itemTarget : aiObjectContext->GetValue<Item*>("item for spell", spellId)->Get();
        targets.SetItemTarget(item);

        if (bot->GetTradeData())
        {
            bot->GetTradeData()->SetSpell(spellId);
            delete spell;
            return true;
        }
    }
    else if (spellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
    {
        // WorldLocation aoe = aiObjectContext->GetValue<WorldLocation>("aoe position")->Get();
        targets.SetDst(x, y, z, 0.f);
    }
    else if (spellInfo->Targets & TARGET_FLAG_SOURCE_LOCATION)
    {
        targets.SetDst(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), 0.f);
    }
    else
    {
        delete spell;
        return false;
    }

    bool const isOpeningSpell =
        spellInfo->HasEffect(SPELL_EFFECT_OPEN_LOCK) || spellInfo->HasEffect(SPELL_EFFECT_SKINNING);
    if (isOpeningSpell)
    {
        delete spell;
        return false;
    }

    // Before prepare(): afterwards the event owns and deletes the spell (no double free).
    // A CanPrepare veto returns before the event exists, so the spell would leak instead.
    if (bot->isMoving() && (spellInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_MOVEMENT) &&
        spellInfo->CalcCastTime(bot) > 0)
    {
        // bot->StopMoving();
        SetNextCheckDelay(sPlayerbotAIConfig.ReactDelay);
        delete spell;
        return false;
    }

    SpellCastResult result = spell->prepare(&targets);
    if (result != SPELL_CAST_OK)
        return false;

    // WaitForSpellCast(spell);
    aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get().Set(spellId, bot->GetGUID(), time(nullptr));
    lastCastSpellId = spellId;
    aiObjectContext->GetValue<PositionMap&>("position")->Get()["random"].Reset();

    if (oldSel)
        bot->SetSelection(oldSel);

    if (HasStrategy("debug spell", BOT_STATE_NON_COMBAT))
    {
        std::ostringstream out;
        out << "Casting " << ChatHelper::FormatSpell(spellInfo);
        TellMasterNoFacing(out);
    }

    return true;
}

// Implemented for IoC (extend for SotA if needed): building-damaging missile at the siege position, or Ram at the gate
static bool IsSiegeShot(Player* bot, AiObjectContext* context, uint32 spellId)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    PositionInfo siege = context->GetValue<PositionMap&>("position")->Get()["bg siege"];
    if (!bot->GetVehicle() || !info || !siege.isSet())
        return false;
    if (info->Targets & TARGET_FLAG_DEST_LOCATION)
    {
        // a gate shot launches a missile that damages buildings (Boulder, Glaive, Cannon; not Napalm or rockets)
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == SPELL_EFFECT_TRIGGER_MISSILE)
                if (SpellInfo const* missile = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                    if (missile->HasEffect(SPELL_EFFECT_GAMEOBJECT_DAMAGE))
                        return true;
        return false;
    }
    Unit* base = bot->GetVehicleBase();
    return !info->Targets && info->HasEffect(SPELL_EFFECT_GAMEOBJECT_DAMAGE) && base &&
           base->GetExactDist2d(siege.x, siege.y) < 15.0f;
}

bool PlayerbotAI::CanCastVehicleSpell(uint32 spellId, Unit* target)
{
    if (!spellId)
        return false;

    if (IsSiegeShot(bot, aiObjectContext, spellId))
        target = nullptr;  // the gate at the siege position, not the current target
    else if (!IsValidUnit(target))
        return false;

    Vehicle* vehicle = bot->GetVehicle();
    if (!vehicle)
        return false;

    // do not allow if no spells
    VehicleSeatEntry const* seat = vehicle->GetSeatForPassenger(bot);
    if (!seat || !(seat->m_flags & VEHICLE_SEAT_FLAG_CAN_CAST))
        return false;

    Unit* vehicleBase = vehicle->GetBase();
    Unit* spellTarget = target;

    if (!spellTarget)
        spellTarget = vehicleBase;

    if (!IsValidUnit(spellTarget))
        return false;

    if (vehicleBase->HasSpellCooldown(spellId))
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return false;

    // check BG siege position set in BG Tactics
    PositionInfo siegePos = GetAiObjectContext()->GetValue<PositionMap&>("position")->Get()["bg siege"];

    // do not cast spell on self if spell is location based
    if (!(siegePos.isSet() || spellTarget != vehicleBase) && spellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
        return false;

    uint32 CastingTime = !spellInfo->IsChanneled() ? spellInfo->CalcCastTime(vehicleBase) : spellInfo->GetDuration();
    if (CastingTime && vehicleBase->isMoving())
        return false;

    if (vehicleBase != spellTarget && ServerFacade::instance().GetDistance2d(vehicleBase, spellTarget) > 120.0f)
        return false;

    if (!target && siegePos.isSet())
    {
        if (ServerFacade::instance().GetDistance2d(vehicleBase, siegePos.x, siegePos.y) > 120.0f)
            return false;
    }

    Spell* spell = new Spell(vehicleBase, spellInfo, TRIGGERED_NONE);

    WorldLocation dest;
    if (siegePos.isSet())
        dest = WorldLocation(bot->GetMapId(), siegePos.x, siegePos.y, siegePos.z, 0);
    else if (spellTarget != vehicleBase)
        dest = WorldLocation(spellTarget->GetMapId(), spellTarget->GetPosition());

    if (spellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
        spell->m_targets.SetDst(dest);
    else if (spellTarget != vehicleBase)
        spell->m_targets.SetUnitTarget(spellTarget);

    SpellCastResult result = spell->CheckCast(true);
    delete spell;

    switch (result)
    {
        case SPELL_FAILED_NOT_INFRONT:
        case SPELL_FAILED_NOT_STANDING:
        case SPELL_FAILED_UNIT_NOT_INFRONT:
        case SPELL_FAILED_MOVING:
        case SPELL_FAILED_TRY_AGAIN:
        case SPELL_CAST_OK:
            return true;
        default:
            return false;
    }

    return false;
}

bool PlayerbotAI::CastVehicleSpell(uint32 spellId, Unit* target)
{
    if (!spellId)
        return false;

    if (IsSiegeShot(bot, aiObjectContext, spellId))
        target = nullptr;  // the gate at the siege position, not the current target
    else if (!IsValidUnit(target))
        return false;

    Vehicle* vehicle = bot->GetVehicle();
    if (!vehicle)
        return false;

    // do not allow if no spells
    VehicleSeatEntry const* seat = vehicle->GetSeatForPassenger(bot);
    if (!seat || !(seat->m_flags & VEHICLE_SEAT_FLAG_CAN_CAST))
        return false;

    Unit* vehicleBase = vehicle->GetBase();
    Unit* spellTarget = target;

    if (!spellTarget)
        spellTarget = vehicleBase;

    if (!IsValidUnit(spellTarget))
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return false;

    // check BG siege position set in BG Tactics
    PositionInfo siegePos = GetAiObjectContext()->GetValue<PositionMap&>("position")->Get()["bg siege"];
    if (!target && siegePos.isSet())
    {
        if (ServerFacade::instance().GetDistance2d(vehicleBase, siegePos.x, siegePos.y) > 120.0f)
            return false;
    }

    // do not cast spell on self if spell is location based
    if (!(siegePos.isSet() || spellTarget != vehicleBase) && (spellInfo->Targets & TARGET_FLAG_DEST_LOCATION) != 0)
        return false;

    if (seat->CanControl())
    {
        // aiObjectContext->GetValue<LastMovement&>("last movement")->Get().Set(nullptr);
        // aiObjectContext->GetValue<time_t>("stay time")->Set(0);
    }

    // bot->clearUnitState(UNIT_STAT_CHASE);
    // bot->clearUnitState(UNIT_STAT_FOLLOW);

    // ObjectGuid oldSel = bot->GetSelectionGuid();
    // bot->SetSelectionGuid(target->GetGUID());

    // turn vehicle if target is not in front
    bool failWithDelay = false;
    if (spellTarget != vehicleBase && (seat->CanControl() || (seat->m_flags & VEHICLE_SEAT_FLAG_ALLOW_TURNING)))
    {
        if (!vehicleBase->HasInArc(CAST_ANGLE_IN_FRONT, spellTarget, 100.0f))
        {
            vehicleBase->SetFacingToObject(spellTarget);
            failWithDelay = true;
        }
    }

    if (siegePos.isSet() && (seat->CanControl() || (seat->m_flags & VEHICLE_SEAT_FLAG_ALLOW_TURNING)))
    {
        vehicleBase->SetFacingTo(vehicleBase->GetAngle(siegePos.x, siegePos.y));
    }

    if (failWithDelay)
    {
        SetNextCheckDelay(sPlayerbotAIConfig.ReactDelay);
        return false;
    }

    Spell* spell = new Spell(vehicleBase, spellInfo, TRIGGERED_NONE);

    SpellCastTargets targets;
    if ((spellTarget != vehicleBase || siegePos.isSet()) && (spellInfo->Targets & TARGET_FLAG_DEST_LOCATION))
    {
        WorldLocation dest;
        if (spellTarget != vehicleBase)
            dest = WorldLocation(spellTarget->GetMapId(), spellTarget->GetPosition());
        else if (siegePos.isSet())
            dest = WorldLocation(bot->GetMapId(), siegePos.x + frand(-2.0f, 2.0f), siegePos.y + frand(-2.0f, 2.0f),
                                 siegePos.z, 0.0f);
        else
            return false;

        targets.SetDst(dest);
        targets.SetSpeed(30.0f);
        float dist = vehicleBase->GetPosition().GetExactDist(dest);
        // very much an approximation of the real projectile arc
        float elev = dist >= 110.0f ? 1.0f : pow(((dist + 10.0f) / 120.0f), 2.0f);
        targets.SetElevation(elev);
    }

    if (spellInfo->Targets & TARGET_FLAG_SOURCE_LOCATION)
    {
        targets.SetSrc(vehicleBase->GetPositionX(), vehicleBase->GetPositionY(), vehicleBase->GetPositionZ());
    }

    if (target && !(spellInfo->Targets & TARGET_FLAG_DEST_LOCATION))
    {
        targets.SetUnitTarget(spellTarget);
    }

    spell->prepare(&targets);

    if (seat->CanControl() && vehicleBase->isMoving() && spell->GetCastTime())
    {
        vehicleBase->StopMoving();
        SetNextCheckDelay(sPlayerbotAIConfig.GlobalCoolDown);
        spell->cancel();
        // delete spell;
        return false;
    }

    // WaitForSpellCast(spell);

    // aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get().Set(spellId, target->GetGUID(), time(0));
    // aiObjectContext->GetValue<botAI::PositionMap&>("position")->Get()["random"].Reset();

    if (HasStrategy("debug spell", BOT_STATE_NON_COMBAT))
    {
        std::ostringstream out;
        out << "Casting Vehicle Spell" << ChatHelper::FormatSpell(spellInfo);
        TellMasterNoFacing(out);
    }

    return true;
}

bool PlayerbotAI::IsInVehicle(bool canControl, bool canCast, bool canAttack, bool canTurn, bool fixed)
{
    Vehicle* vehicle = bot->GetVehicle();
    if (!vehicle)
        return false;

    // get vehicle
    Unit* vehicleBase = vehicle->GetBase();
    if (!vehicleBase || !vehicleBase->IsAlive())
        return false;

    if (!vehicle->GetVehicleInfo())
        return false;

    // get seat
    VehicleSeatEntry const* seat = vehicle->GetSeatForPassenger(bot);
    if (!seat)
        return false;

    if (!(canControl || canCast || canAttack || canTurn || fixed))
        return true;

    if (canControl)
        return seat->CanControl() && !(vehicle->GetVehicleInfo()->m_flags & VEHICLE_FLAG_FIXED_POSITION);

    if (canCast)
        return (seat->m_flags & VEHICLE_SEAT_FLAG_CAN_CAST) != 0;

    if (canAttack)
        return (seat->m_flags & VEHICLE_SEAT_FLAG_CAN_ATTACK) != 0;

    if (canTurn)
        return (seat->m_flags & VEHICLE_SEAT_FLAG_ALLOW_TURNING) != 0;

    if (fixed)
        return (vehicle->GetVehicleInfo()->m_flags & VEHICLE_FLAG_FIXED_POSITION) != 0;

    return false;
}

void PlayerbotAI::WaitForSpellCast(Spell* spell)
{
    if (!spell)
        return;

    SpellInfo const* spellInfo = spell->GetSpellInfo();
    uint32 castTime = spell->GetCastTime();

    if (spellInfo && spellInfo->IsChanneled())
    {
        int32 duration = spellInfo->GetDuration();
        bot->ApplySpellMod(spellInfo->Id, SPELLMOD_DURATION, duration);
        if (duration > 0)
            castTime += duration;
    }

    SetNextCheckDelay(castTime + sPlayerbotAIConfig.ReactDelay);
}

void PlayerbotAI::RemoveAura(std::string const name)
{
    uint32 spellid = aiObjectContext->GetValue<uint32>("spell id", name)->Get();
    if (spellid && bot->HasAura(spellid))
        bot->RemoveAurasDueToSpell(spellid);
}

void PlayerbotAI::RequestSpellInterrupt()
{
    Spell* currentSpell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (currentSpell && currentSpell->getState() == SPELL_STATE_PREPARING)
    {
        spellInterruptRequested = true;
        return;
    }

    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        spellInterruptRequested = true;
}

bool PlayerbotAI::IsInterruptableSpellCasting(Unit* target, std::string const spell)
{
    if (!IsValidUnit(target))
        return false;

    uint32 spellid = aiObjectContext->GetValue<uint32>("spell id", spell)->Get();
    if (!spellid || !target->IsNonMeleeSpellCast(true))
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellid);
    if (!spellInfo)
        return false;

    // The enemy cast itself must be one a kick can stop (the core's EffectInterruptCast rule); spending an
    // interrupt on an uninterruptible cast wastes it.
    bool castInterruptible = false;
    if (Spell* cast = target->GetCurrentSpell(CURRENT_GENERIC_SPELL))
    {
        SpellInfo const* castInfo = cast->GetSpellInfo();
        castInterruptible = castInfo && castInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE &&
                            (castInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT);
    }

    if (Spell* cast = target->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
    {
        SpellInfo const* castInfo = cast->GetSpellInfo();
        castInterruptible = castInterruptible ||
                            (castInfo && castInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE &&
                             (castInfo->ChannelInterruptFlags & CHANNEL_INTERRUPT_FLAG_INTERRUPT));
    }

    if (!castInterruptible)
        return false;

    for (uint8 i = EFFECT_0; i <= EFFECT_2; i++)
    {
        if ((spellInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT) &&
            spellInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
            return true;

        if (spellInfo->Effects[i].Effect == SPELL_EFFECT_INTERRUPT_CAST &&
            !target->IsImmunedToSpellEffect(spellInfo, i))
            return true;

        if ((spellInfo->Effects[i].Effect == SPELL_EFFECT_APPLY_AURA) &&
            spellInfo->Effects[i].ApplyAuraName == SPELL_AURA_MOD_SILENCE)
            return true;

        // A stun stops any cast too (Hammer of Justice, Bash, War Stomp), unless the target shrugs stuns off.
        if (spellInfo->Effects[i].Effect == SPELL_EFFECT_APPLY_AURA &&
            spellInfo->Effects[i].ApplyAuraName == SPELL_AURA_MOD_STUN && !target->IsImmunedToSpellEffect(spellInfo, i))
            return true;
    }

    return false;
}

bool PlayerbotAI::HasAuraToDispel(Unit* target, uint32 dispelType)
{
    if (!IsValidUnit(target) || !target->IsAlive())
        return false;

    if (!IsValidPlayer(bot))
        return false;

    bool isFriend = bot->IsFriendlyTo(target);

    Unit::VisibleAuraMap const* visibleAuras = target->GetVisibleAuras();
    if (!visibleAuras)
        return false;

    for (Unit::VisibleAuraMap::const_iterator itr = visibleAuras->begin(); itr != visibleAuras->end(); ++itr)
    {
        if (!itr->second)
            continue;

        Aura* aura = itr->second->GetBase();
        if (!aura || aura->IsPassive() || aura->IsRemoved())
            continue;

        if (sPlayerbotAIConfig.DispelAuraDuration && aura->GetDuration() &&
            aura->GetDuration() < (int32)sPlayerbotAIConfig.DispelAuraDuration)
            continue;

        SpellInfo const* spellInfo = aura->GetSpellInfo();
        if (!spellInfo)
            continue;

        bool isPositiveSpell = spellInfo->IsPositive();
        if (isPositiveSpell && isFriend)
            continue;

        if (!isPositiveSpell && !isFriend)
            continue;

        if (canDispel(spellInfo, dispelType))
            return true;
    }

    return false;
}

#ifndef WIN32
inline int strcmpi(char const* s1, char const* s2)
{
    for (; *s1 && *s2 && (toupper(*s1) == toupper(*s2)); ++s1, ++s2)
    {
    }
    return *s1 - *s2;
}
#endif

bool PlayerbotAI::canDispel(SpellInfo const* spellInfo, uint32 dispelType)
{
    if (spellInfo->Dispel != dispelType)
        return false;

    if (!spellInfo->SpellName[0])
    {
        return true;
    }

    for (std::string& wl : dispel_whitelist)
    {
        if (strcmpi((char const*)spellInfo->SpellName[0], wl.c_str()) == 0)
        {
            return false;
        }
    }

    return !spellInfo->SpellName[0] || (strcmpi((char const*)spellInfo->SpellName[0], "demon skin") &&
                                        strcmpi((char const*)spellInfo->SpellName[0], "mage armor") &&
                                        strcmpi((char const*)spellInfo->SpellName[0], "frost armor") &&
                                        strcmpi((char const*)spellInfo->SpellName[0], "wavering will") &&
                                        strcmpi((char const*)spellInfo->SpellName[0], "chilled") &&
                                        strcmpi((char const*)spellInfo->SpellName[0], "ice armor"));
}

bool IsRealPlayer(Player* player)
{
    // No PlayerbotAI attached means this is not a bot of any kind, including SelfBots. This is an actual person
    // controlling their character manually through the client.
    // "player" check needed, otherwise GET_PLAYERBOT_AI(nullptr) reads as a "real player".
    return player && !GET_PLAYERBOT_AI(player);
}

bool IsSelfBot(Player* player)
{
    // SelfBot: "player" has PlayerbotAI attached, and it has a master who is itself (player).
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    return botAI && botAI->GetMaster() == player;
}

// Same source as GetTeamId(true); TeamIdForRace() logs an error for non-playable races.
bool IsAlliance(uint8 race)
{
    if (!race || race > 32 || !(sRaceMgr->GetPlayableRaceMask() & (1u << (race - 1))))
        return false;

    return Player::TeamIdForRace(race) == TEAM_ALLIANCE;
}

Player* PlayerbotAI::FindNewMaster()
{
    // Ideally we want to have the leader as master.
    Group* group = bot->GetGroup();
    // Only allow real players as masters unless in battleground.
    if (!group)
        return nullptr;

    Player* groupLeader = GetGroupLeader();
    PlayerbotAI* leaderBotAI = GET_PLAYERBOT_AI(groupLeader);
    if (!leaderBotAI || IsSelfBot(groupLeader))
        return groupLeader;

    // Find the real player in group
    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* member = gref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || !member->IsInSameRaidWith(bot))
            continue;

        PlayerbotAI* memberBotAI = GET_PLAYERBOT_AI(member);
        if ((!memberBotAI || IsSelfBot(member)) && !bot->InBattleground())
            return member;

        if (bot->InBattleground() && bot->GetBattleground() &&
            bot->GetBattleground()->GetBgTypeID(true) == BATTLEGROUND_AV && !GET_PLAYERBOT_AI(member) &&
            member->InBattleground() && bot->GetMapId() == member->GetMapId())
        {
            // Skip if same BG but same subgroup or lower level
            if (!group->SameSubGroup(bot, member) || member->GetLevel() < bot->GetLevel())
                continue;

            // Follow real player only if higher honor points
            uint32 honorpts = member->GetHonorPoints();
            if (bot->GetHonorPoints() && honorpts < bot->GetHonorPoints())
                continue;

            return member;
        }
    }
    return nullptr;
}

// An altbot is a bot whose master is client-based (a regular player or a SelfBot), and is not a randombot, and is not a SelfBot.
// For the purpose of this bool, all addclassbots return true for IsAltBot, but not all altbots return true for IsAddClassBot, since
// IsAddClassBot requires the bot to come from a type 2 account in playerbots_account_type.
bool PlayerbotAI::IsAltBot() { return HasGameClientMaster() && !sRandomPlayerbotMgr.IsRandomBot(bot) && !IsSelfBot(bot); }

// True when the bot's master is driven by a player with a game client: a regular player (no bot AI) or a SelfBot player.
bool PlayerbotAI::HasGameClientMaster() { return IsRealPlayer(master) || IsSelfBot(master); }

Player* PlayerbotAI::GetGroupLeader()
{
    if (!bot->InBattleground())
        if (Group* group = bot->GetGroup())
            if (Player* player = ObjectAccessor::FindPlayer(group->GetLeaderGUID()))
                return player;

    return master;
}

uint32 PlayerbotAI::GetFixedBotNumber(uint32 maxNum)
{
    if (maxNum == 0)
        return 0;

    // Deterministic pseudo-random hash based on the bot GUID evenly distributed across active slots
    uint32 id = bot->GetGUID().GetCounter();
    uint32 h = id;
    h ^= h >> 16;
    h *= 0x7feb352d;
    h ^= h >> 15;
    h *= 0x846ca68b;
    h ^= h >> 16;

    // Current time slot
    uint32 timeSlot = (getMSTime() / 1000) / sPlayerbotAIConfig.BotActiveAloneDurationSeconds;

    // Mix timeSlot into the hash to reshuffle every rotation window
    uint32 mixed = h ^ (timeSlot * 0x9e3779b9);  // with multiplicative constant

    return mixed % maxNum;
}

/*
enum GrouperType
{
    SOLO = 0,
    MEMBER = 1,
    LEADER_2 = 2,
    LEADER_3 = 3,
    LEADER_4 = 4,
    LEADER_5 = 5
};
*/

GrouperType PlayerbotAI::GetGrouperType()
{
    uint32 grouperNumber = GetFixedBotNumber(100);

    if (grouperNumber < 20 && !HasGameClientMaster())
        return GrouperType::SOLO;

    if (grouperNumber < 80)
        return GrouperType::MEMBER;

    if (grouperNumber < 85)
        return GrouperType::LEADER_2;

    if (grouperNumber < 90)
        return GrouperType::LEADER_3;

    if (grouperNumber < 95)
        return GrouperType::LEADER_4;

    return GrouperType::LEADER_5;
}

GuilderType PlayerbotAI::GetGuilderType()
{
    uint32 grouperNumber = GetFixedBotNumber(100);

    if (grouperNumber < 20 && !HasGameClientMaster())
        return GuilderType::SOLO;

    if (grouperNumber < 30)
        return GuilderType::TINY;

    if (grouperNumber < 40)
        return GuilderType::SMALL;

    if (grouperNumber < 60)
        return GuilderType::MEDIUM;

    if (grouperNumber < 80)
        return GuilderType::LARGE;

    return GuilderType::VERY_LARGE;
}

bool PlayerbotAI::HasPlayerNearby(WorldPosition* pos, float range)
{
    float sqRange = range * range;
    for (auto& player : sRandomPlayerbotMgr.GetPlayers())
    {
        if (!player->IsGameMaster() || player->isGMVisible())
        {
            if (player->GetMapId() != bot->GetMapId())
                continue;

            if (pos->sqDistance(WorldPosition(player)) < sqRange)
                return true;

            WorldObject* viewObj = player->GetViewpoint();
            if (viewObj && viewObj != player)
            {
                if (pos->sqDistance(WorldPosition(viewObj)) < sqRange)
                    return true;
            }
        }
    }

    return false;
}

bool PlayerbotAI::HasPlayerNearby(float range)
{
    WorldPosition botPos(bot);
    return HasPlayerNearby(&botPos, range);
};

bool PlayerbotAI::AllowActive(ActivityType activityType)
{
    // bot is in an invalid state, not safe to process
    if (!bot || !bot->GetSession() || !bot->IsInWorld() || bot->IsBeingTeleported() ||
        bot->GetSession()->IsLoggingOut() || bot->IsDuringRemoveFromWorld())
        return false;

    // always allow packet handling (e.g. group invites, trade, loot, friend requests etc)
    if (activityType == PACKET_ACTIVITY)
        return true;

    // all bots forced active, no rotation or scaling needed
    if (sPlayerbotAIConfig.BotActiveAlone >= 100 && !sPlayerbotAIConfig.BotActiveAloneSmartScale)
        return true;

    // bot is in combat, always defend yourself
    if (activityType != OUT_OF_PARTY_ACTIVITY && activityType != PACKET_ACTIVITY)
    {
        if (bot->IsInCombat())
            return true;
    }

    // bot is inside a BG, dungeon, or raid — always active
    if (!WorldPosition(bot).isOverworld())
        return true;

    // bot is waiting in a BG queue — stay active to speed up join
    if (bot->InBattlegroundQueue())
        return true;

    // bot is in a guild that contains a real player
    if (sPlayerbotAIConfig.BotActiveAloneForceWhenInGuild)
    {
        if (IsInRealGuild())  // checks cache list
            return true;
    }

    // a real player is in the same zone (e.g. Elwynn Forest), same continent or within configured yard radius
    // combined into a single loop to multiple iterations since this function is called so often
    bool checkMap = sPlayerbotAIConfig.BotActiveAloneForceWhenInMap;
    bool checkZone = sPlayerbotAIConfig.BotActiveAloneForceWhenInZone;
    bool checkRadius = sPlayerbotAIConfig.BotActiveAloneForceWhenInRadius > 0;
    if (checkMap || checkZone || checkRadius)
    {
        uint32 botMapId = bot->GetMapId();
        uint32 botZoneId = checkZone ? bot->GetZoneId() : 0;
        float sqRange = 0.0f;
        WorldPosition botPos(bot);
        if (checkRadius)
        {
            float range = static_cast<float>(sPlayerbotAIConfig.BotActiveAloneForceWhenInRadius);
            sqRange = range * range;
        }

        for (auto& player : sRandomPlayerbotMgr.GetPlayers())
        {
            if (!player || player->GetMapId() != botMapId)
                continue;

            bool isGM = player->IsGameMaster();

            // map check
            if (checkMap && !(isGM && !player->IsVisible()))
                return true;

            // zone check
            if (checkZone && !(isGM && !player->IsVisible()) && player->GetZoneId() == botZoneId)
                return true;

            // radius check
            if (checkRadius && (!isGM || player->isGMVisible()))
            {
                if (botPos.sqDistance(WorldPosition(player)) < sqRange)
                    return true;

                WorldObject* viewObj = player->GetViewpoint();
                if (viewObj && viewObj != player && botPos.sqDistance(WorldPosition(viewObj)) < sqRange)
                    return true;
            }
        }
    }

    // reaction engine: always active when a real player is within react distance (no config gate)
    if (activityType == REACT_ACTIVITY && HasPlayerNearby())
        return true;

    // bot has a real player master (not another bot)
    if (GetMaster())
    {
        PlayerbotAI* masterBotAI = GET_PLAYERBOT_AI(GetMaster());
        if (!masterBotAI || IsSelfBot(GetMaster()))
            return true;
    }

    // bot is grouped with a real player (or a bot owned by one)
    Group* group = bot->GetGroup();
    if (group)
    {
        for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->GetSource();
            if (!member || !member->IsInWorld() || member->GetMapId() != bot->GetMapId())
                continue;

            if (member == bot)
                continue;

            PlayerbotAI* memberBotAI = GET_PLAYERBOT_AI(member);

            // group member is a real player or owned by one — stay active
            if (!memberBotAI || memberBotAI->HasGameClientMaster())
                return true;

            // if group leader (bot) is inactive, follow suit
            if (group->IsLeader(member->GetGUID()))
            {
                if (!memberBotAI->AllowActivity(PARTY_ACTIVITY))
                    return false;
            }
        }
    }

    // bot is in LFG queue — stay active
    bool isLFG = false;
    if (group)
    {
        if (sLFGMgr->GetState(group->GetGUID()) != lfg::LFG_STATE_NONE)
            isLFG = true;
    }
    if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
        isLFG = true;

    if (isLFG)
        return true;

    // a real player has this bot on their friends list
    if (sPlayerbotAIConfig.BotActiveAloneForceWhenIsFriend)
    {
        // shouldnt be needed analyse in future
        if (!bot->GetGUID())
            return false;

        for (auto& player : sRandomPlayerbotMgr.GetPlayers())
        {
            if (!player || !player->GetSession() || !player->IsInWorld() || player->IsDuringRemoveFromWorld() ||
                player->GetSession()->IsLoggingOut())
                continue;

            PlayerbotAI* playerAI = GET_PLAYERBOT_AI(player);
            if (!playerAI || !IsSelfBot(player))
                continue;

            PlayerSocial* social = player->GetSocial();
            if (social && social->HasFriend(bot->GetGUID()))
                return true;
        }
    }

    // pathfinding only runs for bots forced active by the rules above —
    // skip it for bots that would only be active via random rotation
    if (activityType == DETAILED_MOVE_ACTIVITY)
        return false;

    // #######################################################################################
    // Acitivity throttling logic
    // #######################################################################################
    if (sPlayerbotAIConfig.BotActiveAlone <= 0)
        return false;

    // base threshold capped at 100
    uint32 mod = sPlayerbotAIConfig.BotActiveAlone > 100 ? 100 : sPlayerbotAIConfig.BotActiveAlone;

    // reduce threshold based on server tick time when SmartScale is enabled
    if (sPlayerbotAIConfig.BotActiveAloneSmartScale &&
        bot->GetLevel() >= sPlayerbotAIConfig.BotActiveAloneSmartScaleWhenMinLevel &&
        bot->GetLevel() <= sPlayerbotAIConfig.BotActiveAloneSmartScaleWhenMaxLevel)
    {
        mod = AutoScaleActivity(mod);
    }

    // deterministic rotation — bot is active if its hash falls below the threshold
    uint32 ActivityNumber = GetFixedBotNumber(100);
    return ActivityNumber < mod;
}

bool PlayerbotAI::AllowActivity(ActivityType activityType, bool checkNow)
{
    const int activityIndex = static_cast<int>(activityType);

    if (!allowActiveCheckTimer[activityIndex])
        allowActiveCheckTimer[activityIndex] = getMSTime();

    // 4500ms base + 0–499ms per-bot offset = 4500–4999ms, capping at just under 5 seconds
    uint32 offset = bot->GetGUID().GetCounter() % 500;

    if (!checkNow && getMSTime() < (allowActiveCheckTimer[activityIndex] + 4500 + offset))
        return allowActive[activityIndex];

    const bool allowed = AllowActive(activityType);
    allowActive[activityIndex] = allowed;
    allowActiveCheckTimer[activityIndex] = getMSTime();

    return allowed;
}

uint32 PlayerbotAI::AutoScaleActivity(uint32 mod)
{
    // Current max server update time (ms), and the configured floor/ceiling values for bot scaling
    uint32 maxDiff = sWorldUpdateTime.GetMaxUpdateTimeOfCurrentTable();
    uint32 diffLimitFloor = sPlayerbotAIConfig.BotActiveAloneSmartScaleDiffLimitFloor;
    uint32 diffLimitCeiling = sPlayerbotAIConfig.BotActiveAloneSmartScaleDiffLimitCeiling;

    if (diffLimitCeiling <= diffLimitFloor)
    {
        // Perfrom binary decision if ceiling <= floor: Either all bots are active or none are
        return (maxDiff > diffLimitCeiling) ? 0 : mod;
    }

    if (maxDiff > diffLimitCeiling)
        return 0;

    if (maxDiff <= diffLimitFloor)
        return mod;

    // Calculate lag progress from floor to ceiling (0 to 1)
    double lagProgress = (maxDiff - diffLimitFloor) / (double)(diffLimitCeiling - diffLimitFloor);

    // Apply the percentage of active bots (the complement of lag progress) to the mod value
    return static_cast<uint32>(mod * (1 - lagProgress));
}

bool PlayerbotAI::IsOpposing(Player* player) { return IsOpposing(player->getRace(), bot->getRace()); }

bool PlayerbotAI::IsOpposing(uint8 race1, uint8 race2)
{
    return (IsAlliance(race1) && !IsAlliance(race2)) || (!IsAlliance(race1) && IsAlliance(race2));
}

void PlayerbotAI::RemoveShapeshift()
{
    RemoveAura("bear form");
    RemoveAura("dire bear form");
    RemoveAura("moonkin form");
    RemoveAura("travel form");
    RemoveAura("cat form");
    RemoveAura("flight form");
    bot->RemoveAura(33943);  // The latter added for now as RemoveAura("flight form") currently does not work.
    RemoveAura("swift flight form");
    RemoveAura("aquatic form");
    RemoveAura("ghost wolf");
    // RemoveAura("tree of life");
}

// Mirrors Blizzard’s GetAverageItemLevel rules :
// https://wowpedia.fandom.com/wiki/API_GetAverageItemLevel
uint32 PlayerbotAI::GetEquipGearScore(Player* player)
{
    constexpr uint8 TOTAL_SLOTS = 17;  // every slot except Body & Tabard
    uint32 sumLevel = 0;

    /* ---------- 0.  Detect “ignore off-hand” situations --------- */
    Item* main = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    Item* off = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);

    bool ignoreOffhand = false;  // true → divisor = 16
    if (main)
    {
        bool twoHand = (main->GetTemplate()->InventoryType == INVTYPE_2HWEAPON);
        if (twoHand && !player->HasAura(SPELL_TITAN_GRIP))
            ignoreOffhand = true;  // classic 2-hander
    }
    else if (!off)  // both hands empty
        ignoreOffhand = true;

    /* ---------- 1.  Sum up item-levels -------------------------- */
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
            continue;  // Blizzard never counts these

        if (ignoreOffhand && slot == EQUIPMENT_SLOT_OFFHAND)
            continue;  // skip off-hand in 2-H case

        if (Item* it = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            sumLevel += it->GetTemplate()->ItemLevel;  // missing items add 0
    }

    /* ---------- 2.  Divide by 17 or 16 -------------------------- */
    const uint8 divisor = ignoreOffhand ? TOTAL_SLOTS - 1 : TOTAL_SLOTS;  // 16 or 17
    return sumLevel / divisor;
}

// NOTE : function rewritten as flags "withBags" and "withBank" not used, and _fillGearScoreData sometimes attribute
// one-hand/2H Weapon in wrong slots
/*uint32 PlayerbotAI::GetEquipGearScore(Player* player)
{
    // This function aims to calculate the equipped gear score

    uint32 sum = 0;
    uint8 count = EQUIPMENT_SLOT_END - 2;  // ignore body and tabard slots
    uint8 mh_type = 0;

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        Item* item =player->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
        if (item && i != EQUIPMENT_SLOT_BODY && i != EQUIPMENT_SLOT_TABARD)
        {
            ItemTemplate const* proto = item->GetTemplate();
            sum += proto->ItemLevel;

            // If character is not warfury and have 2 hand weapon equipped, main hand will be counted twice
            if (i == SLOT_MAIN_HAND)
                mh_type = item->GetTemplate()->InventoryType;
            if (!player->HasAura(SPELL_TITAN_GRIP) && mh_type == INVTYPE_2HWEAPON && i == SLOT_MAIN_HAND)
                sum += item->GetTemplate()->ItemLevel;
        }
    }

    uint32 gs = uint32(sum / count);
    return gs;
}*/

/*uint32 PlayerbotAI::GetEquipGearScore(Player* player, bool withBags, bool withBank)
{
    std::vector<uint32> gearScore(EQUIPMENT_SLOT_END);
    uint32 twoHandScore = 0;

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            _fillGearScoreData(player, item, &gearScore, twoHandScore);
    }

    if (withBags)
    {
        // check inventory
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                _fillGearScoreData(player, item, &gearScore, twoHandScore);
        }

        // check bags
        for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
        {
            if (Bag* pBag = (Bag*)player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                {
                    if (Item* item2 = pBag->GetItemByPos(j))
                        _fillGearScoreData(player, item2, &gearScore, twoHandScore);
                }
            }
        }
    }

    if (withBank)
    {
        for (uint8 i = BANK_SLOT_ITEM_START; i < BANK_SLOT_ITEM_END; ++i)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                _fillGearScoreData(player, item, &gearScore, twoHandScore);
        }

        for (uint8 i = BANK_SLOT_BAG_START; i < BANK_SLOT_BAG_END; ++i)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                if (item->IsBag())
                {
                    Bag* bag = (Bag*)item;
                    for (uint8 j = 0; j < bag->GetBagSize(); ++j)
                    {
                        if (Item* item2 = bag->GetItemByPos(j))
                            _fillGearScoreData(player, item2, &gearScore, twoHandScore);
                    }
                }
            }
        }
    }

    uint8 count = EQUIPMENT_SLOT_END - 2;  // ignore body and tabard slots
    uint32 sum = 0;

    // check if 2h hand is higher level than main hand + off hand
    if (gearScore[EQUIPMENT_SLOT_MAINHAND] + gearScore[EQUIPMENT_SLOT_OFFHAND] < twoHandScore * 2)
    {
        gearScore[EQUIPMENT_SLOT_OFFHAND] = 0;  // off hand is ignored in calculations if 2h weapon has higher score
        --count;
        gearScore[EQUIPMENT_SLOT_MAINHAND] = twoHandScore;
    }

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        sum += gearScore[i];
    }

    if (count)
    {
        uint32 res = uint32(sum / count);
        return res;
    }

    return 0;
}*/
uint32 PlayerbotAI::GetMixedGearScore(Player* player, bool withBags, bool withBank, uint32 topN)
{
    std::vector<uint32> gearScore(EQUIPMENT_SLOT_END);
    uint32 twoHandScore = 0;

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            _fillGearScoreData(player, item, &gearScore, twoHandScore, true);
    }

    if (withBags)
    {
        // check inventory
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                _fillGearScoreData(player, item, &gearScore, twoHandScore, true);
        }

        // check bags
        for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
        {
            if (Bag* pBag = (Bag*)player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                {
                    if (Item* item2 = pBag->GetItemByPos(j))
                        _fillGearScoreData(player, item2, &gearScore, twoHandScore, true);
                }
            }
        }
    }

    if (withBank)
    {
        for (uint8 i = BANK_SLOT_ITEM_START; i < BANK_SLOT_ITEM_END; ++i)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                _fillGearScoreData(player, item, &gearScore, twoHandScore, true);
        }

        for (uint8 i = BANK_SLOT_BAG_START; i < BANK_SLOT_BAG_END; ++i)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                if (item->IsBag())
                {
                    Bag* bag = (Bag*)item;
                    for (uint8 j = 0; j < bag->GetBagSize(); ++j)
                    {
                        if (Item* item2 = bag->GetItemByPos(j))
                            _fillGearScoreData(player, item2, &gearScore, twoHandScore, true);
                    }
                }
            }
        }
    }
    if (!topN)
    {
        uint8 count = EQUIPMENT_SLOT_END - 2;  // ignore body and tabard slots
        uint32 sum = 0;

        // check if 2h hand is higher level than main hand + off hand
        if (gearScore[EQUIPMENT_SLOT_MAINHAND] + gearScore[EQUIPMENT_SLOT_OFFHAND] < twoHandScore * 2)
        {
            gearScore[EQUIPMENT_SLOT_OFFHAND] = 0;  // off hand is ignored in calculations if 2h weapon has higher score
            --count;
            gearScore[EQUIPMENT_SLOT_MAINHAND] = twoHandScore;
        }

        for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
        {
            sum += gearScore[i];
        }

        if (count)
        {
            uint32 res = uint32(sum / count);
            return res;
        }

        return 0;
    }
    // topN != 0
    if (gearScore[EQUIPMENT_SLOT_MAINHAND] + gearScore[EQUIPMENT_SLOT_OFFHAND] < twoHandScore * 2)
    {
        gearScore[EQUIPMENT_SLOT_OFFHAND] = twoHandScore;
        gearScore[EQUIPMENT_SLOT_MAINHAND] = twoHandScore;
    }
    std::vector<uint32> topGearScore;
    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        topGearScore.push_back(gearScore[i]);
    }
    std::sort(topGearScore.begin(), topGearScore.end(), [&](const uint32 lhs, const uint32 rhs) { return lhs > rhs; });
    uint32 sum = 0;
    for (uint32 i = 0; i < std::min((uint32)topGearScore.size(), topN); i++)
    {
        sum += topGearScore[i];
    }
    return sum / topN;
}

void PlayerbotAI::_fillGearScoreData(Player* player, Item* item, std::vector<uint32>* gearScore, uint32& twoHandScore,
                                     bool mixed)
{
    if (!item)
        return;

    ItemTemplate const* proto = item->GetTemplate();
    if (player->CanUseItem(proto) != EQUIP_ERR_OK)
        return;

    uint8 type = proto->InventoryType;
    uint32 level = mixed ? proto->ItemLevel * PlayerbotAI::GetItemScoreMultiplier(ItemQualities(proto->Quality))
                         : proto->ItemLevel;

    switch (type)
    {
        case INVTYPE_2HWEAPON:
            twoHandScore = std::max(twoHandScore, level);
            break;
        case INVTYPE_WEAPON:
        case INVTYPE_WEAPONMAINHAND:
            (*gearScore)[SLOT_MAIN_HAND] = std::max((*gearScore)[SLOT_MAIN_HAND], level);
            break;
        case INVTYPE_SHIELD:
        case INVTYPE_WEAPONOFFHAND:
            (*gearScore)[EQUIPMENT_SLOT_OFFHAND] = std::max((*gearScore)[EQUIPMENT_SLOT_OFFHAND], level);
            break;
        case INVTYPE_THROWN:
        case INVTYPE_RANGEDRIGHT:
        case INVTYPE_RANGED:
        case INVTYPE_QUIVER:
        case INVTYPE_RELIC:
            (*gearScore)[EQUIPMENT_SLOT_RANGED] = std::max((*gearScore)[EQUIPMENT_SLOT_RANGED], level);
            break;
        case INVTYPE_HEAD:
            (*gearScore)[EQUIPMENT_SLOT_HEAD] = std::max((*gearScore)[EQUIPMENT_SLOT_HEAD], level);
            break;
        case INVTYPE_NECK:
            (*gearScore)[EQUIPMENT_SLOT_NECK] = std::max((*gearScore)[EQUIPMENT_SLOT_NECK], level);
            break;
        case INVTYPE_SHOULDERS:
            (*gearScore)[EQUIPMENT_SLOT_SHOULDERS] = std::max((*gearScore)[EQUIPMENT_SLOT_SHOULDERS], level);
            break;
        case INVTYPE_BODY:  // Shouldn't be considered when calculating average ilevel
            (*gearScore)[EQUIPMENT_SLOT_BODY] = std::max((*gearScore)[EQUIPMENT_SLOT_BODY], level);
            break;
        case INVTYPE_CHEST:
            (*gearScore)[EQUIPMENT_SLOT_CHEST] = std::max((*gearScore)[EQUIPMENT_SLOT_CHEST], level);
            break;
        case INVTYPE_WAIST:
            (*gearScore)[EQUIPMENT_SLOT_WAIST] = std::max((*gearScore)[EQUIPMENT_SLOT_WAIST], level);
            break;
        case INVTYPE_LEGS:
            (*gearScore)[EQUIPMENT_SLOT_LEGS] = std::max((*gearScore)[EQUIPMENT_SLOT_LEGS], level);
            break;
        case INVTYPE_FEET:
            (*gearScore)[EQUIPMENT_SLOT_FEET] = std::max((*gearScore)[EQUIPMENT_SLOT_FEET], level);
            break;
        case INVTYPE_WRISTS:
            (*gearScore)[EQUIPMENT_SLOT_WRISTS] = std::max((*gearScore)[EQUIPMENT_SLOT_WRISTS], level);
            break;
        case INVTYPE_HANDS:
            (*gearScore)[EQUIPMENT_SLOT_HANDS] = std::max((*gearScore)[EQUIPMENT_SLOT_HANDS], level);
            break;
        // equipped gear score check uses both rings and trinkets for calculation, assume that for bags/banks it is the
        // same with keeping second highest score at second slot
        case INVTYPE_FINGER:
        {
            if ((*gearScore)[EQUIPMENT_SLOT_FINGER1] < level)
            {
                (*gearScore)[EQUIPMENT_SLOT_FINGER2] = (*gearScore)[EQUIPMENT_SLOT_FINGER1];
                (*gearScore)[EQUIPMENT_SLOT_FINGER1] = level;
            }
            else if ((*gearScore)[EQUIPMENT_SLOT_FINGER2] < level)
                (*gearScore)[EQUIPMENT_SLOT_FINGER2] = level;
            break;
        }
        case INVTYPE_TRINKET:
        {
            if ((*gearScore)[EQUIPMENT_SLOT_TRINKET1] < level)
            {
                (*gearScore)[EQUIPMENT_SLOT_TRINKET2] = (*gearScore)[EQUIPMENT_SLOT_TRINKET1];
                (*gearScore)[EQUIPMENT_SLOT_TRINKET1] = level;
            }
            else if ((*gearScore)[EQUIPMENT_SLOT_TRINKET2] < level)
                (*gearScore)[EQUIPMENT_SLOT_TRINKET2] = level;
            break;
        }
        case INVTYPE_CLOAK:
            (*gearScore)[EQUIPMENT_SLOT_BACK] = std::max((*gearScore)[EQUIPMENT_SLOT_BACK], level);
            break;
        default:
            break;
    }
}

std::string const PlayerbotAI::HandleRemoteCommand(std::string const command)
{
    if (command == "state")
    {
        switch (currentState)
        {
            case BOT_STATE_COMBAT:
                return "combat";
            case BOT_STATE_DEAD:
                return "dead";
            case BOT_STATE_NON_COMBAT:
                return "non-combat";
            default:
                return "unknown";
        }
    }
    else if (command == "position")
    {
        std::ostringstream out;
        out << bot->GetPositionX() << " " << bot->GetPositionY() << " " << bot->GetPositionZ() << " " << bot->GetMapId()
            << " " << bot->GetOrientation();

        if (AreaTableEntry const* zoneEntry = sAreaTableStore.LookupEntry(bot->GetZoneId()))
            out << " |" << zoneEntry->area_name[0] << "|";

        return out.str();
    }
    else if (command == "tpos")
    {
        Unit* target = *GetAiObjectContext()->GetValue<Unit*>("current target");
        if (!target)
        {
            return "";
        }

        std::ostringstream out;
        out << target->GetPositionX() << " " << target->GetPositionY() << " " << target->GetPositionZ() << " "
            << target->GetMapId() << " " << target->GetOrientation();
        return out.str();
    }
    else if (command == "movement")
    {
        LastMovement& data = *GetAiObjectContext()->GetValue<LastMovement&>("last movement");
        std::ostringstream out;
        out << data.lastMoveShort.GetPositionX() << " " << data.lastMoveShort.GetPositionY() << " "
            << data.lastMoveShort.GetPositionZ() << " " << data.lastMoveShort.GetMapId() << " "
            << data.lastMoveShort.GetOrientation();
        return out.str();
    }
    else if (command == "target")
    {
        Unit* target = *GetAiObjectContext()->GetValue<Unit*>("current target");
        if (!target)
        {
            return "";
        }

        return target->GetName();
    }
    else if (command == "hp")
    {
        uint32 pct = static_cast<uint32>(bot->GetHealthPct());
        std::ostringstream out;
        out << pct << "%";

        Unit* target = *GetAiObjectContext()->GetValue<Unit*>("current target");
        if (!target)
        {
            return out.str();
        }

        pct = static_cast<uint32>(target->GetHealthPct());
        out << " / " << pct << "%";
        return out.str();
    }
    else if (command == "strategy")
    {
        return currentEngine->ListStrategies();
    }
    else if (command == "action")
    {
        return currentEngine->GetLastAction();
    }
    else if (command == "values")
    {
        return GetAiObjectContext()->FormatValues();
    }
    else if (command == "travel")
    {
        std::ostringstream out;

        TravelTarget* target = GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
        if (target->getDestination())
        {
            out << "Destination = " << target->getDestination()->getName();

            out << ": " << target->getDestination()->getTitle();

            out << " v: " << target->getDestination()->getVisitors();

            if (!(*target->getPosition() == WorldPosition()))
            {
                out << "(" << target->getPosition()->getAreaName() << ")";
                out << " distance: " << target->getPosition()->distance(bot) << "y";
                out << " v: " << target->getPosition()->getVisitors();
            }
        }

        out << " Status = ";

        if (target->getStatus() == TRAVEL_STATUS_NONE)
            out << " none";
        else if (target->getStatus() == TRAVEL_STATUS_PREPARE)
            out << " prepare";
        else if (target->getStatus() == TRAVEL_STATUS_TRAVEL)
            out << " travel";
        else if (target->getStatus() == TRAVEL_STATUS_WORK)
            out << " work";
        else if (target->getStatus() == TRAVEL_STATUS_COOLDOWN)
            out << " cooldown";
        else if (target->getStatus() == TRAVEL_STATUS_EXPIRED)
            out << " expired";

        if (target->getStatus() != TRAVEL_STATUS_EXPIRED)
            out << " Expire in " << (target->getTimeLeft() / 1000) << "s";

        out << " Retry " << target->getRetryCount(true) << "/" << target->getRetryCount(false);

        return out.str();
    }
    else if (command == "budget")
    {
        std::ostringstream out;

        AiObjectContext* context = GetAiObjectContext();

        out << "Current money: " << ChatHelper::formatMoney(bot->GetMoney()) << " free to use:"
            << ChatHelper::formatMoney(AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::anything)) << "\n";
        out << "Purpose | Available / Needed \n";

        for (uint32 i = 1; i < (uint32)NeedMoneyFor::anything; i++)
        {
            NeedMoneyFor needMoneyFor = NeedMoneyFor(i);

            switch (needMoneyFor)
            {
                case NeedMoneyFor::none:
                    out << "nothing";
                    break;
                case NeedMoneyFor::repair:
                    out << "repair";
                    break;
                case NeedMoneyFor::ammo:
                    out << "ammo";
                    break;
                case NeedMoneyFor::spells:
                    out << "spells";
                    break;
                case NeedMoneyFor::travel:
                    out << "travel";
                    break;
                case NeedMoneyFor::consumables:
                    out << "consumables";
                    break;
                case NeedMoneyFor::gear:
                    out << "gear";
                    break;
                case NeedMoneyFor::guild:
                    out << "guild";
                    break;
                default:
                    break;
            }

            out << " | " << ChatHelper::formatMoney(AI_VALUE2(uint32, "free money for", i)) << " / "
                << ChatHelper::formatMoney(AI_VALUE2(uint32, "money needed for", i)) << "\n";
        }

        return out.str();
    }

    std::ostringstream out;
    out << "invalid command: " << command;
    return out.str();
}

bool PlayerbotAI::HasSkill(SkillType skill) { return bot->HasSkill(skill) && bot->GetSkillValue(skill) > 0; }

float PlayerbotAI::GetRange(std::string const type)
{
    float val = 0;
    if (aiObjectContext)
        val = aiObjectContext->GetValue<float>("range", type)->Get();

    if (abs(val) >= 0.1f)
        return val;

    // [b32_engage] A hunter's "spell" range is its weapon's: Auto Shot (35 yd plus Hawk Eye), not SpellDistance
    // 28.5. At 29-35 yd it shot from where it stood instead of walking in (engage_delay.md, "G ranged
    // repositioning": 2269 hunter pulls, 20263 s lost).
    if ((type == "spell" || type == "shoot") && bot && bot->getClass() == CLASS_HUNTER)
        if (float const weapon = GetHunterWeaponRange(); weapon > 0.0f)
            return weapon;

    if (type == "spell")
        return sPlayerbotAIConfig.SpellDistance;

    if (type == "shoot")
        return sPlayerbotAIConfig.ShootDistance;

    if (type == "flee")
        return sPlayerbotAIConfig.FleeDistance;

    if (type == "heal")
        return sPlayerbotAIConfig.HealDistance;

    if (type == "melee")
        return sPlayerbotAIConfig.MeleeDistance;

    return 0;
}

// [b32_engage] Auto Shot's range with the bot's own range mods (Hawk Eye), less a margin for a moving target; 0
// without a ranged weapon or the spell.
float PlayerbotAI::GetHunterWeaponRange()
{
    constexpr uint32 AUTO_SHOT = 75;
    constexpr float AUTO_SHOT_MARGIN = 2.0f;
    if (!bot || !bot->GetWeaponForAttack(RANGED_ATTACK, true))
        return 0.0f;

    SpellInfo const* autoShot = sSpellMgr->GetSpellInfo(AUTO_SHOT);
    float const range = autoShot ? autoShot->GetMaxRange(false, bot) : 0.0f;
    return range > sPlayerbotAIConfig.SpellDistance + AUTO_SHOT_MARGIN ? range - AUTO_SHOT_MARGIN : 0.0f;
}

void PlayerbotAI::Ping(float x, float y)
{
    WorldPacket data(MSG_MINIMAP_PING, (8 + 4 + 4));
    data << bot->GetGUID();
    data << x;
    data << y;

    if (bot->GetGroup())
    {
        bot->GetGroup()->BroadcastPacket(&data, true, -1, bot->GetGUID());
    }
    else
    {
        bot->GetSession()->SendPacket(&data);
    }
}

// Helper function to iterate through items in the inventory and bags
Item* PlayerbotAI::FindItemInInventory(std::function<bool(ItemTemplate const*)> checkItem) const
{
    // List out items in the main backpack
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        if (Item* const pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            ItemTemplate const* pItemProto = pItem->GetTemplate();
            if (pItemProto && bot->CanUseItem(pItemProto) == EQUIP_ERR_OK && checkItem(pItemProto))
                return pItem;
        }
    }

    // List out items in other removable backpacks
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        if (Bag const* const pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag))
        {
            for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
            {
                if (Item* const pItem = bot->GetItemByPos(bag, slot))
                {
                    ItemTemplate const* pItemProto = pItem->GetTemplate();
                    if (pItemProto && bot->CanUseItem(pItemProto) == EQUIP_ERR_OK && checkItem(pItemProto))
                        return pItem;
                }
            }
        }
    }

    return nullptr;
}

// Find Poison
Item* PlayerbotAI::FindPoison() const
{
    return FindItemInInventory([](ItemTemplate const* pItemProto) -> bool
                               { return pItemProto->Class == ITEM_CLASS_CONSUMABLE && pItemProto->SubClass == 6; });
}

// Find Ammo
Item* PlayerbotAI::FindAmmo() const
{
    // Get equipped ranged weapon
    if (Item* rangedWeapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
    {
        uint32 weaponSubClass = rangedWeapon->GetTemplate()->SubClass;
        uint32 requiredAmmoType = 0;

        // Determine the correct ammo type based on the weapon
        switch (weaponSubClass)
        {
            case ITEM_SUBCLASS_WEAPON_GUN:
                requiredAmmoType = ITEM_SUBCLASS_BULLET;
                break;
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                requiredAmmoType = ITEM_SUBCLASS_ARROW;
                break;
            default:
                return nullptr;  // Not a ranged weapon that requires ammo
        }

        // Search inventory for the correct ammo type
        return FindItemInInventory(
            [requiredAmmoType](ItemTemplate const* pItemProto) -> bool
            { return pItemProto->Class == ITEM_CLASS_PROJECTILE && pItemProto->SubClass == requiredAmmoType; });
    }

    return nullptr;  // No ranged weapon equipped
}

// Find Consumable
Item* PlayerbotAI::FindConsumable(uint32 itemId) const
{
    return FindItemInInventory(
        [itemId](ItemTemplate const* pItemProto) -> bool
        {
            return (pItemProto->Class == ITEM_CLASS_CONSUMABLE || pItemProto->Class == ITEM_CLASS_TRADE_GOODS) &&
                   pItemProto->ItemId == itemId;
        });
}

// Find Bandage
Item* PlayerbotAI::FindBandage() const
{
    return FindItemInInventory(
        [](ItemTemplate const* pItemProto) -> bool
        { return pItemProto->Class == ITEM_CLASS_CONSUMABLE && pItemProto->SubClass == ITEM_SUBCLASS_BANDAGE; });
}

Item* PlayerbotAI::FindOpenableItem() const
{
    return FindItemInInventory(
        [this](ItemTemplate const* itemTemplate) -> bool
        {
            return itemTemplate->HasFlag(ITEM_FLAG_HAS_LOOT) &&
                   (itemTemplate->LockID == 0 || !this->bot->GetItemByEntry(itemTemplate->ItemId)->IsLocked());
        });
}

Item* PlayerbotAI::FindLockedItem() const
{
    return FindItemInInventory(
        [this](ItemTemplate const* itemTemplate) -> bool
        {
            if (!this->bot->HasSkill(SKILL_LOCKPICKING))  // Ensure bot has Lockpicking skill
                return false;

            if (itemTemplate->LockID == 0)  // Ensure the item is actually locked
                return false;

            Item* item = this->bot->GetItemByEntry(itemTemplate->ItemId);
            if (!item || !item->IsLocked())  // Ensure item instance is locked
                return false;

            // Check if bot has enough Lockpicking skill
            LockEntry const* lockInfo = sLockStore.LookupEntry(itemTemplate->LockID);
            if (!lockInfo)
                return false;

            for (uint8 j = 0; j < 8; ++j)
            {
                if (lockInfo->Type[j] == LOCK_KEY_SKILL)
                {
                    uint32 skillId = SkillByLockType(LockType(lockInfo->Index[j]));
                    if (skillId == SKILL_LOCKPICKING)
                    {
                        uint32 requiredSkill = lockInfo->Skill[j];
                        uint32 botSkill = this->bot->GetSkillValue(SKILL_LOCKPICKING);
                        return botSkill >= requiredSkill;
                    }
                }
            }

            return false;
        });
}

Item* PlayerbotAI::FindStoneFor(Item* weapon) const
{
    if (!weapon)
        return nullptr;

    ItemTemplate const* item_template = weapon->GetTemplate();
    if (!item_template)
        return nullptr;

    // [b34_gnomer] Only a weapon takes a stone. The subclass tests below never looked at the item class, and
    // ITEM_SUBCLASS_ARMOR_SHIELD (6) == ITEM_SUBCLASS_WEAPON_POLEARM (6): a tank's shield got a sharpening stone,
    // the cast failed, and "apply stone" repeated on every tick (gnomer-n 5B 20261026-014521, Rtwarbd).
    if (item_template->Class != ITEM_CLASS_WEAPON)
        return nullptr;

    static const std::vector<uint32_t> uPrioritizedSharpStoneIds = {
        ADAMANTITE_SHARPENING_STONE, FEL_SHARPENING_STONE,   ELEMENTAL_SHARPENING_STONE, DENSE_SHARPENING_STONE,
        SOLID_SHARPENING_STONE,      HEAVY_SHARPENING_STONE, COARSE_SHARPENING_STONE,    ROUGH_SHARPENING_STONE};

    // Elemental Sharpening Stone also works on maces/staves/fists, so it is a valid fallback.
    static const std::vector<uint32_t> uPrioritizedWeightStoneIds = {
        ADAMANTITE_WEIGHTSTONE, FEL_WEIGHTSTONE,    ELEMENTAL_SHARPENING_STONE, DENSE_WEIGHTSTONE, SOLID_WEIGHTSTONE,
        HEAVY_WEIGHTSTONE,      COARSE_WEIGHTSTONE, ROUGH_WEIGHTSTONE};

    Item* stone = nullptr;
    ItemTemplate const* pProto = weapon->GetTemplate();
    if (pProto && (pProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD || pProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2 ||
                   pProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE || pProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                   pProto->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER || pProto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM))
    {
        for (uint8 i = 0; i < std::size(uPrioritizedSharpStoneIds); ++i)
        {
            stone = FindConsumable(uPrioritizedSharpStoneIds[i]);
            if (stone)
            {
                return stone;
            }
        }
    }
    else if (pProto &&
             (pProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE || pProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2 ||
              pProto->SubClass == ITEM_SUBCLASS_WEAPON_STAFF || pProto->SubClass == ITEM_SUBCLASS_WEAPON_FIST))
    {
        for (uint8 i = 0; i < std::size(uPrioritizedWeightStoneIds); ++i)
        {
            stone = FindConsumable(uPrioritizedWeightStoneIds[i]);
            if (stone)
            {
                return stone;
            }
        }
    }

    return stone;
}

Item* PlayerbotAI::FindOilFor(Item* weapon) const
{
    if (!weapon)
        return nullptr;

    ItemTemplate const* item_template = weapon->GetTemplate();
    if (!item_template || item_template->Class != ITEM_CLASS_WEAPON)  // [b34_gnomer] a weapon only, as FindStoneFor
        return nullptr;

    static const std::vector<uint32_t> uPrioritizedWizardOilIds = {
        BRILLIANT_WIZARD_OIL, SUPERIOR_WIZARD_OIL, WIZARD_OIL,      LESSER_WIZARD_OIL, MINOR_WIZARD_OIL,
        BRILLIANT_MANA_OIL,   SUPERIOR_MANA_OIL,   LESSER_MANA_OIL, MINOR_MANA_OIL};

    static const std::vector<uint32_t> uPrioritizedManaOilIds = {
        BRILLIANT_MANA_OIL,  SUPERIOR_MANA_OIL, LESSER_MANA_OIL,   MINOR_MANA_OIL,  BRILLIANT_WIZARD_OIL,
        SUPERIOR_WIZARD_OIL, WIZARD_OIL,        LESSER_WIZARD_OIL, MINOR_WIZARD_OIL};

    Item* oil = nullptr;
    int botClass = bot->getClass();
    int specTab = AiFactory::GetPlayerSpecTab(bot);

    std::vector<uint32_t> const* prioritizedOils = nullptr;
    switch (botClass)
    {
        case CLASS_PRIEST:
            prioritizedOils = (specTab == 2) ? &uPrioritizedWizardOilIds : &uPrioritizedManaOilIds;
            break;
        case CLASS_MAGE:
            prioritizedOils = &uPrioritizedWizardOilIds;
            break;
        case CLASS_DRUID:
            if (specTab == 0)  // Balance
                prioritizedOils = &uPrioritizedWizardOilIds;
            else if (specTab == 1)  // Feral
                prioritizedOils = nullptr;
            else  // Restoration (specTab == 2) or any other/unspecified spec
                prioritizedOils = &uPrioritizedManaOilIds;
            break;
        case CLASS_HUNTER:
            prioritizedOils = &uPrioritizedManaOilIds;
            break;
        case CLASS_PALADIN:
            if (specTab == 1)  // Protection
                prioritizedOils = &uPrioritizedWizardOilIds;
            else if (specTab == 2)  // Retribution
                prioritizedOils = nullptr;
            else  // Holy (specTab == 0) or any other/unspecified spec
                prioritizedOils = &uPrioritizedManaOilIds;
            break;
        default:
            prioritizedOils = &uPrioritizedManaOilIds;
            break;
    }

    if (prioritizedOils)
    {
        for (auto const& id : *prioritizedOils)
        {
            oil = FindConsumable(id);
            if (oil)
            {
                return oil;
            }
        }
    }

    return oil;
}

std::vector<Item*> PlayerbotAI::GetInventoryAndEquippedItems()
{
    std::vector<Item*> items;

    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
            {
                if (Item* pItem = pBag->GetItemByPos(j))
                {
                    items.push_back(pItem);
                }
            }
        }
    }

    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            items.push_back(pItem);
        }
    }

    for (int i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            items.push_back(pItem);
        }
    }

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; slot++)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            items.push_back(pItem);
        }
    }

    return items;
}

std::vector<Item*> PlayerbotAI::GetInventoryItems()
{
    std::vector<Item*> items;

    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
            {
                if (Item* pItem = pBag->GetItemByPos(j))
                {
                    items.push_back(pItem);
                }
            }
        }
    }

    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            items.push_back(pItem);
        }
    }

    for (int i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            items.push_back(pItem);
        }
    }

    return items;
}

uint32 PlayerbotAI::GetInventoryItemsCountWithId(uint32 itemId)
{
    uint32 count = 0;

    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
            {
                if (Item* pItem = pBag->GetItemByPos(j))
                {
                    if (pItem->GetTemplate()->ItemId == itemId)
                    {
                        count += pItem->GetCount();
                    }
                }
            }
        }
    }

    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pItem->GetTemplate()->ItemId == itemId)
            {
                count += pItem->GetCount();
            }
        }
    }

    for (int i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pItem->GetTemplate()->ItemId == itemId)
            {
                count += pItem->GetCount();
            }
        }
    }

    return count;
}

void PlayerbotAI::ConsolidateItems()
{
    if (!bot || !bot->IsAlive())
        return;

    // Only entries with two or more stacks in the bags can be consolidated.
    std::map<uint32, uint32> entryCounts;
    for (Item* item : GetInventoryItems())
        if (item && item->GetCount() > 0)
            ++entryCounts[item->GetEntry()];

    for (auto const& [entry, stackCount] : entryCounts)
    {
        if (stackCount < 2)
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto || proto->GetMaxStackSize() <= 1)
            continue;

        // Merge into the fullest partial stack through the core's drag-to-stack path, one move
        // per pass. Each successful move removes one partial stack (a target fills or a source
        // empties), so the initial stack count bounds the passes.
        for (uint32 pass = 0; pass < stackCount; ++pass)
        {
            std::vector<Item*> stacks;
            for (Item* item : GetInventoryItems())
                if (item && item->GetEntry() == entry && item->GetCount() > 0)
                    stacks.push_back(item);

            if (stacks.size() < 2)
                break;

            // Target: the fullest stack that still has space.
            Item* target = nullptr;
            for (Item* item : stacks)
                if (item->GetCount() < proto->GetMaxStackSize() && (!target || item->GetCount() > target->GetCount()))
                    target = item;
            if (!target)
                break;  // all stacks are full

            // Source: the largest other partial stack. A full stack must never be a source.
            Item* source = nullptr;
            for (Item* item : stacks)
                if (item != target && item->GetCount() < proto->GetMaxStackSize() &&
                    (!source || item->GetCount() > source->GetCount()))
                    source = item;
            if (!source)
                break;

            std::vector<uint32> before;
            before.reserve(stacks.size());
            for (Item* item : stacks)
                before.push_back(item->GetCount());
            std::sort(before.begin(), before.end());

            // The core merges as much as fits and leaves any remainder in the source slot.
            bot->SwapItem(source->GetPos(), target->GetPos());

            // SwapItem can refuse (dead, busy) or move without merging; stop instead of retrying
            // the same pair for the rest of the pass budget.
            std::vector<uint32> after;
            for (Item* item : GetInventoryItems())
                if (item && item->GetEntry() == entry && item->GetCount() > 0)
                    after.push_back(item->GetCount());
            std::sort(after.begin(), after.end());
            if (after == before)
                break;
        }
    }
}

bool PlayerbotAI::HasItemInInventory(uint32 itemId)
{
    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
            {
                if (Item* pItem = pBag->GetItemByPos(j))
                {
                    if (pItem->GetTemplate()->ItemId == itemId)
                    {
                        return true;
                    }
                }
            }
        }
    }

    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pItem->GetTemplate()->ItemId == itemId)
            {
                return true;
            }
        }
    }

    for (int i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pItem->GetTemplate()->ItemId == itemId)
            {
                return true;
            }
        }
    }

    return false;
}

std::vector<std::pair<Quest const*, uint32>> PlayerbotAI::GetCurrentQuestsRequiringItemId(uint32 itemId)
{
    std::vector<std::pair<Quest const*, uint32>> result;

    if (!itemId)
    {
        return result;
    }

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        // QuestStatus status = bot->GetQuestStatus(questId);
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        for (uint8 i = 0; i < std::size(quest->RequiredItemId); ++i)
        {
            if (quest->RequiredItemId[i] == itemId)
            {
                result.push_back(std::pair(quest, quest->RequiredItemId[i]));
                break;
            }
        }
    }

    return result;
}

//  on self
void PlayerbotAI::ImbueItem(Item* item) { ImbueItem(item, TARGET_FLAG_NONE, ObjectGuid::Empty); }

//  item on unit
void PlayerbotAI::ImbueItem(Item* item, Unit* target)
{
    if (!IsValidUnit(target))
        return;

    ImbueItem(item, TARGET_FLAG_UNIT, target->GetGUID());
}

//  item on equipped item
void PlayerbotAI::ImbueItem(Item* item, uint8 targetInventorySlot)
{
    if (targetInventorySlot >= EQUIPMENT_SLOT_END)
        return;

    Item* const targetItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, targetInventorySlot);
    if (!targetItem)
        return;

    ImbueItem(item, TARGET_FLAG_ITEM, targetItem->GetGUID());
}

// generic item use method
void PlayerbotAI::ImbueItem(Item* item, uint32 targetFlag, ObjectGuid targetGUID)
{
    if (!item)
        return;

    uint32 glyphIndex = 0;
    uint8 castFlags = 0;
    uint8 bagIndex = item->GetBagSlot();
    uint8 slot = item->GetSlot();
    uint8 cast_count = 0;
    ObjectGuid item_guid = item->GetGUID();

    uint32 spellId = 0;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (item->GetTemplate()->Spells[i].SpellId > 0)
        {
            spellId = item->GetTemplate()->Spells[i].SpellId;
            break;
        }
    }

    WorldPacket* packet = new WorldPacket(CMSG_USE_ITEM);
    *packet << bagIndex;
    *packet << slot;
    *packet << cast_count;
    *packet << spellId;
    *packet << item_guid;
    *packet << glyphIndex;
    *packet << castFlags;
    *packet << targetFlag;

    if (targetFlag & (TARGET_FLAG_UNIT | TARGET_FLAG_ITEM | TARGET_FLAG_GAMEOBJECT))
        *packet << targetGUID.WriteAsPacked();

    bot->GetSession()->QueuePacket(packet);
}

void PlayerbotAI::EnchantItemT(uint32 spellid, uint8 slot)
{
    Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!pItem || !pItem->IsInWorld() || !pItem->GetOwner() || !pItem->GetOwner()->IsInWorld() ||
        !pItem->GetOwner()->GetSession())
        return;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellid);
    if (!spellInfo)
        return;

    uint32 enchantid = spellInfo->Effects[0].MiscValue;
    if (!enchantid)
    {
        // LOG_ERROR("playerbots", "{}: Invalid enchantid ", enchantid, " report to devs", bot->GetName().c_str());
        return;
    }

    if (!((1 << pItem->GetTemplate()->SubClass) & spellInfo->EquippedItemSubClassMask) &&
        !((1 << pItem->GetTemplate()->InventoryType) & spellInfo->EquippedItemInventoryTypeMask))
    {
        // LOG_ERROR("playerbots", "{}: items could not be enchanted, wrong item type equipped",
        // bot->GetName().c_str());
        return;
    }

    bot->ApplyEnchantment(pItem, PERM_ENCHANTMENT_SLOT, false);
    pItem->SetEnchantment(PERM_ENCHANTMENT_SLOT, enchantid, 0, 0);
    bot->ApplyEnchantment(pItem, PERM_ENCHANTMENT_SLOT, true);

    LOG_INFO("playerbots", "{}: items was enchanted successfully!", bot->GetName().c_str());
}

int32 PlayerbotAI::GetNearGroupMemberCount(float dis)
{
    int count = 1;  // yourself
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->GetSource();
            if (member == bot)  // calculated
                continue;

            if (!member || !member->IsInWorld())
                continue;

            if (member->GetMapId() != bot->GetMapId())
                continue;

            if (member->GetExactDist(bot) > dis)
                continue;

            count++;
        }
    }
    return count;
}

bool PlayerbotAI::CanMove()
{
    // Most common checks: confused, stunned, fleeing, jumping, charging. All these
    // states are set when handling certain aura effects. We don't check against
    // UNIT_STATE_ROOT here, because this state is used by vehicles.
    if (bot->HasUnitState(UNIT_STATE_LOST_CONTROL))
        return false;

    // Death state (w/o spirit release) and Spirit of Redemption aura (priest)
    if ((bot->isDead() && !bot->HasPlayerFlag(PLAYER_FLAGS_GHOST)) || bot->HasSpiritOfRedemptionAura())
        return false;

    // Common CC effects, ordered by frequency: rooted > charmed > frozen > polymorphed.
    // NOTE: Can't find proper way to check if bot is rooted or charmed w/o additional
    // vehicle check -- when a passenger is added, they become rooted and charmed.
    if (!bot->GetVehicle() && (bot->IsRooted() || bot->IsCharmed()))
        return false;
    if (bot->isFrozen() || bot->IsPolymorphed())
        return false;

    // Check for the MM controlled slot types: feared, confused, fleeing, etc.
    if (bot->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE)
        return false;

    // Traveling state: taxi flight and being teleported (relatively rare)
    if (bot->IsInFlight() || bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE ||
        bot->IsBeingTeleported())
        return false;

    // Vehicle state: is in the vehicle and can control it (rare, content-specific)
    if ((bot->GetVehicle() && !IsInVehicle(true)))
        return false;

    return true;
}

bool PlayerbotAI::IsInRealGuild()
{
    if (!bot->GetGuildId())
        return false;

    return PlayerbotGuildMgr::instance().IsRealGuild(bot->GetGuildId());
}

void PlayerbotAI::QueueChatResponse(const ChatQueuedReply chatReply)
{
    std::lock_guard<std::mutex> lock(chatRepliesLock);
    chatReplies.push_back(std::move(chatReply));
}

bool PlayerbotAI::EqualLowercaseName(std::string s1, std::string s2)
{
    if (s1.length() != s2.length())
    {
        return false;
    }
    for (std::string::size_type i = 0; i < s1.length(); i++)
    {
        if (tolower(s1[i]) != tolower(s2[i]))
        {
            return false;
        }
    }
    return true;
}

// A custom CanEquipItem (remove AutoUnequipOffhand in FindEquipSlot to prevent unequip on `item usage` calculation)
InventoryResult PlayerbotAI::CanEquipItem(uint8 slot, uint16& dest, Item* pItem, bool swap, bool not_loading) const
{
    dest = 0;
    if (pItem)
    {
        LOG_DEBUG("entities.player.items", "STORAGE: CanEquipItem slot = {}, item = {}, count = {}", slot,
                  pItem->GetEntry(), pItem->GetCount());
        ItemTemplate const* pProto = pItem->GetTemplate();
        if (pProto)
        {
            if (!sScriptMgr->OnPlayerCanEquipItem(bot, slot, dest, pItem, swap, not_loading))
                return EQUIP_ERR_CANT_DO_RIGHT_NOW;

            // item used
            if (pItem->m_lootGenerated)
                return EQUIP_ERR_ALREADY_LOOTED;

            if (pItem->IsBindedNotWith(bot))
                return EQUIP_ERR_DONT_OWN_THAT_ITEM;

            InventoryResult res = bot->CanTakeMoreSimilarItems(pItem);
            if (res != EQUIP_ERR_OK)
                return res;

            ScalingStatDistributionEntry const* ssd =
                pProto->ScalingStatDistribution
                    ? sScalingStatDistributionStore.LookupEntry(pProto->ScalingStatDistribution)
                    : 0;
            // check allowed level (extend range to upper values if MaxLevel more or equal max player level, this let GM
            // set high level with 1...max range items)
            if (ssd && ssd->MaxLevel < DEFAULT_MAX_LEVEL && ssd->MaxLevel < bot->GetLevel())
                return EQUIP_ERR_ITEM_CANT_BE_EQUIPPED;

            uint8 eslot = FindEquipSlot(pProto, slot, swap);
            if (eslot == NULL_SLOT)
                return EQUIP_ERR_ITEM_CANT_BE_EQUIPPED;

            // Xinef: dont allow to equip items on disarmed slot
            if (!bot->CanUseAttackType(bot->GetAttackBySlot(eslot)))
                return EQUIP_ERR_NOT_WHILE_DISARMED;

            res = bot->CanUseItem(pItem, not_loading);
            if (res != EQUIP_ERR_OK)
                return res;

            if (!swap && bot->GetItemByPos(INVENTORY_SLOT_BAG_0, eslot))
                return EQUIP_ERR_NO_EQUIPMENT_SLOT_AVAILABLE;

            // if we are swapping 2 equiped items, CanEquipUniqueItem check
            // should ignore the item we are trying to swap, and not the
            // destination item. CanEquipUniqueItem should ignore destination
            // item only when we are swapping weapon from bag
            uint8 ignore = uint8(NULL_SLOT);
            switch (eslot)
            {
                case EQUIPMENT_SLOT_MAINHAND:
                    ignore = EQUIPMENT_SLOT_OFFHAND;
                    break;
                case EQUIPMENT_SLOT_OFFHAND:
                    ignore = EQUIPMENT_SLOT_MAINHAND;
                    break;
                case EQUIPMENT_SLOT_FINGER1:
                    ignore = EQUIPMENT_SLOT_FINGER2;
                    break;
                case EQUIPMENT_SLOT_FINGER2:
                    ignore = EQUIPMENT_SLOT_FINGER1;
                    break;
                case EQUIPMENT_SLOT_TRINKET1:
                    ignore = EQUIPMENT_SLOT_TRINKET2;
                    break;
                case EQUIPMENT_SLOT_TRINKET2:
                    ignore = EQUIPMENT_SLOT_TRINKET1;
                    break;
            }

            if (ignore == uint8(NULL_SLOT) || pItem != bot->GetItemByPos(INVENTORY_SLOT_BAG_0, ignore))
                ignore = eslot;

            InventoryResult res2 = bot->CanEquipUniqueItem(pItem, swap ? ignore : uint8(NULL_SLOT));
            if (res2 != EQUIP_ERR_OK)
                return res2;

            // check unique-equipped special item classes
            if (pProto->Class == ITEM_CLASS_QUIVER)
                for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
                    if (Item* pBag = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                        if (pBag != pItem)
                            if (ItemTemplate const* pBagProto = pBag->GetTemplate())
                                if (pBagProto->Class == pProto->Class && (!swap || pBag->GetSlot() != eslot))
                                    return (pBagProto->SubClass == ITEM_SUBCLASS_AMMO_POUCH)
                                               ? EQUIP_ERR_CAN_EQUIP_ONLY1_AMMOPOUCH
                                               : EQUIP_ERR_CAN_EQUIP_ONLY1_QUIVER;

            uint32 type = pProto->InventoryType;

            if (eslot == EQUIPMENT_SLOT_OFFHAND)
            {
                // Do not allow polearm to be equipped in the offhand (rare case for the only 1h polearm 41750)
                // xinef: same for fishing poles
                if (type == INVTYPE_WEAPON && (pProto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM ||
                                               pProto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE))
                    return EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;

                else if (type == INVTYPE_WEAPON || type == INVTYPE_WEAPONOFFHAND)
                {
                    if (!bot->CanDualWield())
                        return EQUIP_ERR_CANT_DUAL_WIELD;
                }
                else if (type == INVTYPE_2HWEAPON)
                {
                    if (!bot->CanDualWield() || !bot->CanTitanGrip())
                        return EQUIP_ERR_CANT_DUAL_WIELD;
                }

                if (bot->IsTwoHandUsed())
                    return EQUIP_ERR_CANT_EQUIP_WITH_TWOHANDED;
            }

            // equip two-hand weapon case (with possible unequip 2 items)
            if (type == INVTYPE_2HWEAPON)
            {
                if (eslot == EQUIPMENT_SLOT_OFFHAND)
                {
                    if (!bot->CanTitanGrip())
                        return EQUIP_ERR_ITEM_CANT_BE_EQUIPPED;
                }
                else if (eslot != EQUIPMENT_SLOT_MAINHAND)
                    return EQUIP_ERR_ITEM_CANT_BE_EQUIPPED;

                if (!bot->CanTitanGrip())
                {
                    // offhand item must can be stored in inventory for offhand item and it also must be unequipped
                    Item* offItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
                    ItemPosCountVec off_dest;
                    if (offItem && (!not_loading ||
                                    bot->CanUnequipItem(uint16(INVENTORY_SLOT_BAG_0) << 8 | EQUIPMENT_SLOT_OFFHAND,
                                                        false) != EQUIP_ERR_OK ||
                                    bot->CanStoreItem(NULL_BAG, NULL_SLOT, off_dest, offItem, false) != EQUIP_ERR_OK))
                        return swap ? EQUIP_ERR_ITEMS_CANT_BE_SWAPPED : EQUIP_ERR_INVENTORY_FULL;
                }
            }
            dest = ((INVENTORY_SLOT_BAG_0 << 8) | eslot);
            return EQUIP_ERR_OK;
        }
    }

    return !swap ? EQUIP_ERR_ITEM_NOT_FOUND : EQUIP_ERR_ITEMS_CANT_BE_SWAPPED;
}

uint8 PlayerbotAI::FindEquipSlot(ItemTemplate const* proto, uint32 slot, bool swap) const
{
    uint8 slots[4];
    slots[0] = NULL_SLOT;
    slots[1] = NULL_SLOT;
    slots[2] = NULL_SLOT;
    slots[3] = NULL_SLOT;
    switch (proto->InventoryType)
    {
        case INVTYPE_HEAD:
            slots[0] = EQUIPMENT_SLOT_HEAD;
            break;
        case INVTYPE_NECK:
            slots[0] = EQUIPMENT_SLOT_NECK;
            break;
        case INVTYPE_SHOULDERS:
            slots[0] = EQUIPMENT_SLOT_SHOULDERS;
            break;
        case INVTYPE_BODY:
            slots[0] = EQUIPMENT_SLOT_BODY;
            break;
        case INVTYPE_CHEST:
        case INVTYPE_ROBE:
            slots[0] = EQUIPMENT_SLOT_CHEST;
            break;
        case INVTYPE_WAIST:
            slots[0] = EQUIPMENT_SLOT_WAIST;
            break;
        case INVTYPE_LEGS:
            slots[0] = EQUIPMENT_SLOT_LEGS;
            break;
        case INVTYPE_FEET:
            slots[0] = EQUIPMENT_SLOT_FEET;
            break;
        case INVTYPE_WRISTS:
            slots[0] = EQUIPMENT_SLOT_WRISTS;
            break;
        case INVTYPE_HANDS:
            slots[0] = EQUIPMENT_SLOT_HANDS;
            break;
        case INVTYPE_FINGER:
            slots[0] = EQUIPMENT_SLOT_FINGER1;
            slots[1] = EQUIPMENT_SLOT_FINGER2;
            break;
        case INVTYPE_TRINKET:
            slots[0] = EQUIPMENT_SLOT_TRINKET1;
            slots[1] = EQUIPMENT_SLOT_TRINKET2;
            break;
        case INVTYPE_CLOAK:
            slots[0] = EQUIPMENT_SLOT_BACK;
            break;
        case INVTYPE_WEAPON:
        {
            slots[0] = EQUIPMENT_SLOT_MAINHAND;

            // suggest offhand slot only if know dual wielding
            // (this will be replace mainhand weapon at auto equip instead unwonted "you don't known dual wielding" ...
            if (bot->CanDualWield())
                slots[1] = EQUIPMENT_SLOT_OFFHAND;
            break;
        }
        case INVTYPE_SHIELD:
        case INVTYPE_WEAPONOFFHAND:
        case INVTYPE_HOLDABLE:
            slots[0] = EQUIPMENT_SLOT_OFFHAND;
            break;
        case INVTYPE_RANGED:
        case INVTYPE_RANGEDRIGHT:
        case INVTYPE_THROWN:
            slots[0] = EQUIPMENT_SLOT_RANGED;
            break;
        case INVTYPE_2HWEAPON:
            slots[0] = EQUIPMENT_SLOT_MAINHAND;
            if (Item* mhWeapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
            {
                if (ItemTemplate const* mhWeaponProto = mhWeapon->GetTemplate())
                {
                    if (mhWeaponProto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM ||
                        mhWeaponProto->SubClass == ITEM_SUBCLASS_WEAPON_STAFF)
                    {
                        bot->AutoUnequipOffhandIfNeed(true);
                        break;
                    }
                }
            }

            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
            {
                if (proto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM || proto->SubClass == ITEM_SUBCLASS_WEAPON_STAFF)
                {
                    break;
                }
            }
            if (bot->CanDualWield() && bot->CanTitanGrip() && proto->SubClass != ITEM_SUBCLASS_WEAPON_POLEARM &&
                proto->SubClass != ITEM_SUBCLASS_WEAPON_STAFF && proto->SubClass != ITEM_SUBCLASS_WEAPON_FISHING_POLE)
                slots[1] = EQUIPMENT_SLOT_OFFHAND;
            break;
        case INVTYPE_TABARD:
            slots[0] = EQUIPMENT_SLOT_TABARD;
            break;
        case INVTYPE_WEAPONMAINHAND:
            slots[0] = EQUIPMENT_SLOT_MAINHAND;
            break;
        case INVTYPE_BAG:
            slots[0] = INVENTORY_SLOT_BAG_START + 0;
            slots[1] = INVENTORY_SLOT_BAG_START + 1;
            slots[2] = INVENTORY_SLOT_BAG_START + 2;
            slots[3] = INVENTORY_SLOT_BAG_START + 3;
            break;
        case INVTYPE_RELIC:
        {
            switch (proto->SubClass)
            {
                case ITEM_SUBCLASS_ARMOR_LIBRAM:
                    if (bot->IsClass(CLASS_PALADIN, CLASS_CONTEXT_EQUIP_RELIC))
                        slots[0] = EQUIPMENT_SLOT_RANGED;
                    break;
                case ITEM_SUBCLASS_ARMOR_IDOL:
                    if (bot->IsClass(CLASS_DRUID, CLASS_CONTEXT_EQUIP_RELIC))
                        slots[0] = EQUIPMENT_SLOT_RANGED;
                    break;
                case ITEM_SUBCLASS_ARMOR_TOTEM:
                    if (bot->IsClass(CLASS_SHAMAN, CLASS_CONTEXT_EQUIP_RELIC))
                        slots[0] = EQUIPMENT_SLOT_RANGED;
                    break;
                case ITEM_SUBCLASS_ARMOR_MISC:
                    if (bot->IsClass(CLASS_WARLOCK, CLASS_CONTEXT_EQUIP_RELIC))
                        slots[0] = EQUIPMENT_SLOT_RANGED;
                    break;
                case ITEM_SUBCLASS_ARMOR_SIGIL:
                    if (bot->IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_EQUIP_RELIC))
                        slots[0] = EQUIPMENT_SLOT_RANGED;
                    break;
            }
            break;
        }
        default:
            return NULL_SLOT;
    }

    if (slot != NULL_SLOT)
    {
        if (swap || !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            for (uint8 i = 0; i < 4; ++i)
                if (slots[i] == slot)
                    return slot;
    }
    else
    {
        // search free slot at first
        for (uint8 i = 0; i < 4; ++i)
            if (slots[i] != NULL_SLOT && !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slots[i]))
                // in case 2hand equipped weapon (without titan grip) offhand slot empty but not free
                if (slots[i] != EQUIPMENT_SLOT_OFFHAND || !bot->IsTwoHandUsed())
                    return slots[i];

        // if not found free and can swap return first appropriate from used
        for (uint8 i = 0; i < 4; ++i)
            if (slots[i] != NULL_SLOT && swap)
                return slots[i];
    }

    // no free position
    return NULL_SLOT;
}

bool PlayerbotAI::IsSafe(Player* player)
{
    return player && player->GetMapId() == bot->GetMapId() && player->GetInstanceId() == bot->GetInstanceId() &&
           !player->IsBeingTeleported();
}
bool PlayerbotAI::IsSafe(WorldObject* obj)
{
    return obj && obj->GetMapId() == bot->GetMapId() && obj->GetInstanceId() == bot->GetInstanceId() &&
           (!obj->IsPlayer() || !((Player*)obj)->IsBeingTeleported());
}
ChatChannelSource PlayerbotAI::GetChatChannelSource(Player* bot, uint32 type, std::string channelName)
{
    if (type == CHAT_MSG_CHANNEL)
    {
        if (channelName == "World")
            return ChatChannelSource::SRC_WORLD;
        else
        {
            ChannelMgr* cMgr = ChannelMgr::forTeam(bot->GetTeamId());
            if (!cMgr)
            {
                return ChatChannelSource::SRC_UNDEFINED;
            }

            Channel const* channel = cMgr->GetChannel(channelName, bot);
            if (channel)
            {
                switch (channel->GetChannelId())
                {
                    case ChatChannelId::GENERAL:
                    {
                        return ChatChannelSource::SRC_GENERAL;
                    }
                    case ChatChannelId::TRADE:
                    {
                        return ChatChannelSource::SRC_TRADE;
                    }
                    case ChatChannelId::LOCAL_DEFENSE:
                    {
                        return ChatChannelSource::SRC_LOCAL_DEFENSE;
                    }
                    case ChatChannelId::WORLD_DEFENSE:
                    {
                        return ChatChannelSource::SRC_WORLD_DEFENSE;
                    }
                    case ChatChannelId::LOOKING_FOR_GROUP:
                    {
                        return ChatChannelSource::SRC_LOOKING_FOR_GROUP;
                    }
                    case ChatChannelId::GUILD_RECRUITMENT:
                    {
                        return ChatChannelSource::SRC_GUILD_RECRUITMENT;
                    }
                    default:
                    {
                        return ChatChannelSource::SRC_UNDEFINED;
                    }
                }
            }
        }
    }
    else
    {
        switch (type)
        {
            case CHAT_MSG_WHISPER:
            {
                return ChatChannelSource::SRC_WHISPER;
            }
            case CHAT_MSG_SAY:
            {
                return ChatChannelSource::SRC_SAY;
            }
            case CHAT_MSG_YELL:
            {
                return ChatChannelSource::SRC_YELL;
            }
            case CHAT_MSG_GUILD:
            {
                return ChatChannelSource::SRC_GUILD;
            }
            case CHAT_MSG_PARTY:
            {
                return ChatChannelSource::SRC_PARTY;
            }
            case CHAT_MSG_RAID:
            {
                return ChatChannelSource::SRC_RAID;
            }
            case CHAT_MSG_EMOTE:
            {
                return ChatChannelSource::SRC_EMOTE;
            }
            case CHAT_MSG_TEXT_EMOTE:
            {
                return ChatChannelSource::SRC_TEXT_EMOTE;
            }
            default:
            {
                return ChatChannelSource::SRC_UNDEFINED;
            }
        }
    }
    return ChatChannelSource::SRC_UNDEFINED;
}

bool PlayerbotAI::StarterLevelDistanceCheck(Player* player, WorldLocation const& loc, bool fromStartUp)
{
    if (player->GetLevel() > 16)
        return true;

    float dis = 0.0f;
    if (fromStartUp)
    {
        PlayerInfo const* pInfo = sObjectMgr->GetPlayerInfo(player->getRace(true), player->getClass());
        if (loc.GetMapId() != pInfo->mapId)
            return false;
        dis = loc.GetExactDist(pInfo->positionX, pInfo->positionY, pInfo->positionZ);
    }
    else
    {
        if (loc.GetMapId() != player->GetMapId())
            return false;
        dis = loc.GetExactDist(player);
    }

    float bound = 10000.0f;
    if (player->GetLevel() <= 4)
        bound = 500.0f;
    else if (player->GetLevel() <= 10)
        bound = 2500.0f;

    return dis <= bound;
}

std::vector<Quest const*> PlayerbotAI::GetAllCurrentQuests()
{
    std::vector<Quest const*> result;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
        {
            continue;
        }

        result.push_back(sObjectMgr->GetQuestTemplate(questId));
    }

    return result;
}

std::vector<Quest const*> PlayerbotAI::GetCurrentIncompleteQuests()
{
    std::vector<Quest const*> result;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
        {
            continue;
        }

        QuestStatus status = bot->GetQuestStatus(questId);
        if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_NONE)
        {
            result.push_back(sObjectMgr->GetQuestTemplate(questId));
        }
    }

    return result;
}

std::set<uint32> PlayerbotAI::GetAllCurrentQuestIds()
{
    std::set<uint32> result;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
        {
            continue;
        }

        result.insert(questId);
    }

    return result;
}

std::set<uint32> PlayerbotAI::GetCurrentIncompleteQuestIds()
{
    std::set<uint32> result;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
        {
            continue;
        }

        QuestStatus status = bot->GetQuestStatus(questId);
        if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_NONE)
        {
            result.insert(questId);
        }
    }

    return result;
}

uint32 PlayerbotAI::GetGlobalCooldownRemaining()
{
    if (!lastCastSpellId)
        return 0;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(lastCastSpellId);
    if (!spellInfo || !spellInfo->StartRecoveryCategory)
        return 0;

    return bot->GetGlobalCooldownMgr().GetGlobalCooldown(spellInfo);
}

namespace
{
    constexpr uint32 IDLE_DIAG_WINDOW_MS = 15 * IN_MILLISECONDS;
    constexpr uint32 IDLE_DIAG_MAX_GAP_MS = 4 * IN_MILLISECONDS;
    constexpr uint32 IDLE_DIAG_SLOW_GAP_MS = 150;
    constexpr uint32 IDLE_DIAG_VERY_SLOW_GAP_MS = 400;
    constexpr uint8 IDLE_DIAG_THREAT_HOLD = 80;  // ThreatMultiplier's single-target limit
    constexpr float IDLE_DIAG_RANGED_REACH = 30.0f;
}

void PlayerbotAI::NoteIdleDiagAction(Action* action, std::string const& source)
{
    if (!bot->GetGroup() || !bot->GetMap() || !bot->GetMap()->Instanceable())
        return;

    idleDiag.tickAction = action->getName();
    if (!dynamic_cast<CastSpellAction*>(action))
        return;

    TraceRogueGcd(action, source);
    TraceDkGcd(action, source);
    idleDiag.tickCast = true;
    uint32 const now = getMSTime();
    if (idleDiag.busyEndMs && now >= idleDiag.busyEndMs)
    {
        uint32 const gap = now - idleDiag.busyEndMs;
        if (gap < IDLE_DIAG_MAX_GAP_MS)
        {
            ++idleDiag.gapCount;
            idleDiag.gapSumMs += gap;
            idleDiag.gapMaxMs = std::max(idleDiag.gapMaxMs, gap);
            idleDiag.gapOver150 += gap > IDLE_DIAG_SLOW_GAP_MS ? 1 : 0;
            idleDiag.gapOver400 += gap > IDLE_DIAG_VERY_SLOW_GAP_MS ? 1 : 0;
        }
    }

    uint32 busy = GetGlobalCooldownRemaining();
    for (CurrentSpellTypes type : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
        if (Spell* spell = bot->GetCurrentSpell(type))
            busy = std::max(busy, static_cast<uint32>(std::max(spell->GetCastTimeRemaining(), 0)));

    idleDiag.busyEndMs = std::max(idleDiag.busyEndMs, now + busy);
}

// [rot171] b215 rfc-h 5E: the assassination rogue's energy use did not add up (~200 energy a minute unexplained by
// the casts / regen in the harness numbers). One debug line per cast on a dungeon boss: energy and combo points right
// after the cast, the action and its trigger, the ms since the previous cast and the energy then, so the energy
// budget (regen, Relentless Strikes / Focused Attacks / Overkill returns, pooling) can be summed from the log.
// [rot321] b220 rfc-h 5H Rtdruaf: Shred 12.6 / 19.0, Rip 1.65 / 2.14 and Rake 4.7 / 5.8 a minute of the sim with no
// per-cast times in the logs: the cat (a druid in Cat Form) gets the same line, with Clearcasting, Berserk, Tiger's
// Fury, its own Rip / Rake (seconds) and Savage Roar left and whether it stands behind the target.
void PlayerbotAI::TraceRogueGcd(Action* action, std::string const& source)
{
    bool const cat = bot->getClass() == CLASS_DRUID && bot->GetShapeshiftForm() == FORM_CAT;
    if ((bot->getClass() != CLASS_ROGUE && !cat) || !sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;

    Unit* target = aiObjectContext->GetValue<Unit*>("current target")->Get();
    Creature* boss = target ? target->ToCreature() : nullptr;
    if (!boss || !boss->IsDungeonBoss())
    {
        rogueGcdLastMs = 0;
        return;
    }

    uint32 const now = getMSTime();
    uint32 const energy = bot->GetPower(POWER_ENERGY);
    uint32 const sinceMs = rogueGcdLastMs ? getMSTimeDiff(rogueGcdLastMs, now) : 0;
    if (cat)
    {
        Aura const* rip = GetAura("rip", boss, true);
        Aura const* rake = GetAura("rake", boss, true);
        Aura const* roar = GetAura("savage roar", bot);
        constexpr uint32 SPELL_DRUID_BERSERK = 50334;  // [rot430] the Berserking enchant's proc is "Berserk" too
        LOG_DEBUG("playerbots",
                  "[cat gcd] {} on {}: after the cast energy {} cp {} -> {} (trigger {}) | {} ms after the last cast "
                  "(energy {}) | clearcasting {}, berserk {}, tiger's fury {} | rip {} s, rake {} s, savage roar {} s "
                  "| melee {}, behind {}, swinging {}",
                  bot->GetName(), boss->GetName(), energy, bot->GetComboPoints(target), action->getName(),
                  source.empty() ? "default" : source, sinceMs, rogueGcdLastEnergy, HasAura("clearcasting", bot),
                  bot->HasAura(SPELL_DRUID_BERSERK), HasAura("tiger's fury", bot),
                  rip ? rip->GetDuration() / IN_MILLISECONDS : 0, rake ? rake->GetDuration() / IN_MILLISECONDS : 0,
                  roar ? roar->GetDuration() / IN_MILLISECONDS : 0, bot->IsWithinMeleeRange(target),
                  boss->isInBack(bot), bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING));
        rogueGcdLastMs = now;
        rogueGcdLastEnergy = energy;
        return;
    }

    Aura const* snd = GetAura("slice and dice", bot);
    // [rot320] b220 5E: Deadly Poison 69 % and Envenom 72 % a cast of the sim with Master Poisoner (core613) in, which
    // only fits fewer than 5 doses on the boss; the doses (own Deadly Poison, SpellFamilyFlags 0x10000 as
    // spell_rog_deadly_poison), the Envenom buff (57993: +75 % Instant Poison) and Overkill (58427) are logged too
    AuraEffect const* deadly = boss->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_ROGUE, 0x10000, 0, 0,
                                                   bot->GetGUID());
    Aura const* envenom = bot->GetAura(57993);
    Aura const* overkill = bot->GetAura(58427);
    LOG_DEBUG("playerbots",
              "[rogue gcd] {} on {}: after the cast energy {} cp {} -> {} (trigger {}) | {} ms after the last cast "
              "(energy {}) | slice and dice {} s, melee {}, swinging {} | deadly poison {} doses {} s, envenom {} ms, "
              "overkill {} s",
              bot->GetName(), boss->GetName(), energy, bot->GetComboPoints(target), action->getName(),
              source.empty() ? "default" : source, sinceMs, rogueGcdLastEnergy,
              snd ? snd->GetDuration() / IN_MILLISECONDS : 0, bot->IsWithinMeleeRange(target),
              bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING), deadly ? deadly->GetBase()->GetStackAmount() : 0,
              deadly ? deadly->GetBase()->GetDuration() / IN_MILLISECONDS : 0, envenom ? envenom->GetDuration() : 0,
              overkill ? overkill->GetDuration() / IN_MILLISECONDS : 0);
    rogueGcdLastMs = now;
    rogueGcdLastEnergy = energy;
}

// [rot181] b217 rfc-h: the Blood DK tank at 76 % of the sim in 5C against 92.5 % in 5G with the same gear, talents and
// code; the rot51 state lines carry no runes and no time. One debug line per death knight cast on a dungeon boss: the
// six runes (type and seconds left, 0 = ready) and runic power right after the cast, the action and its trigger, the ms
// since the previous cast, our diseases on the target and the attackers, so rune idle time (ready runes x the gap to
// the next cast), runic power capping and the area spells' share can be summed from the log.
void PlayerbotAI::TraceDkGcd(Action* action, std::string const& source)
{
    if (bot->getClass() != CLASS_DEATH_KNIGHT || !sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;

    Unit* target = aiObjectContext->GetValue<Unit*>("current target")->Get();
    Creature* boss = target ? target->ToCreature() : nullptr;
    if (!boss || !boss->IsDungeonBoss())
    {
        dkGcdLastMs = 0;
        return;
    }

    // RuneType order: blood, unholy, frost, death
    static char const RUNE_LETTER[NUM_RUNE_TYPES] = {'B', 'U', 'F', 'D'};
    std::string runes;
    for (uint8 i = 0; i < MAX_RUNES; ++i)
    {
        RuneType const type = bot->GetCurrentRune(i);
        runes += Acore::StringFormat("{}{}{:.1f}", i ? " " : "", type < NUM_RUNE_TYPES ? RUNE_LETTER[type] : '?',
                                     bot->GetRuneCooldown(i) / float(IN_MILLISECONDS));
    }

    uint32 const now = getMSTime();
    uint32 const sinceMs = dkGcdLastMs ? getMSTimeDiff(dkGcdLastMs, now) : 0;
    Aura const* plague = GetAura("blood plague", target, true, true);
    Aura const* fever = GetAura("frost fever", target, true, true);
    LOG_DEBUG("playerbots",
              "[dk gcd] {} on {}: after the cast runes {} rp {} -> {} (trigger {}) | {} ms after the last cast | "
              "blood plague {} s, frost fever {} s, attackers {}, melee {}, swinging {}",
              bot->GetName(), boss->GetName(), runes, bot->GetPower(POWER_RUNIC_POWER) / 10, action->getName(),
              source.empty() ? "default" : source, sinceMs, plague ? plague->GetDuration() / IN_MILLISECONDS : 0,
              fever ? fever->GetDuration() / IN_MILLISECONDS : 0,
              aiObjectContext->GetValue<GuidVector>("attackers")->Get().size(), bot->IsWithinMeleeRange(target),
              bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING));
    dkGcdLastMs = now;
}

void PlayerbotAI::UpdateHealDiag()
{
    constexpr uint32 HEAL_DIAG_CHECK_MS = 1000;
    constexpr uint32 HEAL_DIAG_LOG_MS = 3000;
    constexpr float HEAL_DIAG_HEALTH_PCT = 50.0f;

    Group* group = bot->GetGroup();
    Map* map = bot->GetMap();
    if (!group || !map || !map->Instanceable() || !bot->IsAlive() || !IsHeal(bot))
        return;

    uint32 const now = getMSTime();
    if (healDiagCheckMs && getMSTimeDiff(healDiagCheckMs, now) < HEAL_DIAG_CHECK_MS)
        return;

    healDiagCheckMs = now;
    if (healDiagLogMs && getMSTimeDiff(healDiagLogMs, now) < HEAL_DIAG_LOG_MS)
        return;

    if (bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) || bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        return;

    Player* hurt = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member->GetMap() != map || !member->IsAlive() || !member->IsInCombat() ||
            member->GetHealthPct() >= HEAL_DIAG_HEALTH_PCT)
            continue;

        if (!hurt || member->GetHealthPct() < hurt->GetHealthPct())
            hurt = member;
    }

    if (!hurt)
        return;

    healDiagLogMs = now;
    LOG_DEBUG("playerbots",
              "[heal diag] {} (combat {}, engine {}, {:.0f}% hp, {:.0f}% mana, moving {}): {} at {:.0f}% {:.1f} yd, "
              "sight {} | {}",
              bot->GetName(), bot->IsInCombat(), uint32(currentState), bot->GetHealthPct(),
              bot->GetPowerPct(POWER_MANA), bot->isMoving(), hurt->GetName(), hurt->GetHealthPct(),
              bot->GetExactDist(hurt), bot->IsWithinLOSInMap(hurt),
              currentEngine ? currentEngine->GetLastAction() : std::string());
}

void PlayerbotAI::UpdateIdleDiag(bool casting)
{
    if (!bot->IsInCombat() || !bot->GetGroup() || !bot->GetMap() || !bot->GetMap()->Instanceable() ||
        IsTank(bot) || IsHeal(bot))
    {
        idleDiag.busyEndMs = 0;
        return;
    }

    uint32 const now = getMSTime();
    if (!idleDiag.windowStartMs)
        idleDiag.windowStartMs = now;

    ++idleDiag.ticks;
    if (casting)
        ++idleDiag.castWait;
    else if (idleDiag.tickCast)
        ++idleDiag.casts;
    else if (!idleDiag.tickAction.empty())
    {
        ++idleDiag.nonCast;
        ++idleDiag.actions[idleDiag.tickAction];
    }
    else
    {
        Unit* target = aiObjectContext->GetValue<Unit*>("current target")->Get();
        bool const outOfReach = target && (IsRanged(bot) ? bot->GetDistance(target) > IDLE_DIAG_RANGED_REACH
                                                         : !bot->IsWithinMeleeRange(target));
        if (!target || !target->IsAlive())
            ++idleDiag.noTarget;
        else if (outOfReach || !bot->IsWithinLOSInMap(target))
            ++idleDiag.rangeLos;
        else if (bot->isMoving())
            ++idleDiag.moving;
        else if (GetGlobalCooldownRemaining())
            ++idleDiag.gcd;
        else if (aiObjectContext->GetValue<uint8>("threat", "current target")->Get() >= IDLE_DIAG_THREAT_HOLD)
            ++idleDiag.threat;
        else
            ++idleDiag.other;
    }

    if (getMSTimeDiff(idleDiag.windowStartMs, now) < IDLE_DIAG_WINDOW_MS)
        return;

    std::vector<std::pair<std::string, uint32>> top(idleDiag.actions.begin(), idleDiag.actions.end());
    std::sort(top.begin(), top.end(), [](auto const& left, auto const& right) { return left.second > right.second; });
    std::string actions;
    for (size_t i = 0; i < top.size() && i < 6; ++i)
        actions += Acore::StringFormat(" {}:{}", top[i].first, top[i].second);

    LOG_DEBUG("playerbots",
              "[idle diag] {} {}s ticks {} cast {} castWait {} nonCast {} | idle: noTarget {} rangeLos {} moving {} "
              "gcd {} threat {} other {} | gap n {} avg {} max {} >150 {} >400 {} | actions{}",
              bot->GetName(), getMSTimeDiff(idleDiag.windowStartMs, now) / IN_MILLISECONDS, idleDiag.ticks,
              idleDiag.casts, idleDiag.castWait, idleDiag.nonCast, idleDiag.noTarget, idleDiag.rangeLos,
              idleDiag.moving, idleDiag.gcd, idleDiag.threat, idleDiag.other, idleDiag.gapCount,
              idleDiag.gapCount ? idleDiag.gapSumMs / idleDiag.gapCount : 0, idleDiag.gapMaxMs, idleDiag.gapOver150,
              idleDiag.gapOver400, actions);

    uint32 const busyEnd = idleDiag.busyEndMs;
    idleDiag = IdleDiag();
    idleDiag.busyEndMs = busyEnd;
    idleDiag.windowStartMs = now;
}

void PlayerbotAI::RecordQuestAccepted(uint32 questId)
{
    ++rpgStatistic.questAccepted;
    if (sPlayerbotAIConfig.CollectDetailedQuestStats)
        ++rpgStatistic.byQuest[questId].accepted;
}

void PlayerbotAI::RecordQuestCompleted(uint32 questId)
{
    // Each completion counts once per bot and quest, until the quest is rewarded or dropped.
    if (!countedQuestCompletions.insert(questId).second)
        return;

    ++rpgStatistic.questCompleted;
    if (sPlayerbotAIConfig.CollectDetailedQuestStats)
        ++rpgStatistic.byQuest[questId].completed;
}

void PlayerbotAI::RecordQuestRewarded(uint32 questId)
{
    RecordQuestCompleted(questId);
    countedQuestCompletions.erase(questId);
    ++rpgStatistic.questRewarded;
    if (sPlayerbotAIConfig.CollectDetailedQuestStats)
        ++rpgStatistic.byQuest[questId].rewarded;
}

void PlayerbotAI::RecordQuestDropped(uint32 questId, QuestDropReason reason)
{
    countedQuestCompletions.erase(questId);
    ++rpgStatistic.questDropped;
    if (sPlayerbotAIConfig.CollectDetailedQuestStats)
        ++rpgStatistic.byQuest[questId].dropped[static_cast<size_t>(reason)];
}

void PlayerbotAI::RecordQuestAbandoned(uint32 questId, QuestAbandonReason reason)
{
    ++rpgStatistic.questAbandoned;
    if (sPlayerbotAIConfig.CollectDetailedQuestStats)
        ++rpgStatistic.byQuest[questId].abandoned[static_cast<size_t>(reason)];
}

uint32 PlayerbotAI::GetReactDelay()
{
    uint32 base = sPlayerbotAIConfig.ReactDelay;  // Default 100(ms)

    // If dynamic react delay is disabled, use a static calculation
    if (!sPlayerbotAIConfig.DynamicReactDelay)
    {
        if (HasGameClientMaster())
            return base;

        bool inBG = bot->InBattleground() || bot->InArena();

        if (sPlayerbotAIConfig.FastReactInBG && inBG)
            return base;

        bool inCombat = bot->IsInCombat();

        if (!inCombat)
            return base * 10;

        else if (inCombat)
            return static_cast<uint32>(base * 2.5f);

        return base;
    }

    // Dynamic react delay calculation:

    if (HasGameClientMaster())
        return base;

    bool inBG = bot->InBattleground() || bot->InArena();

    if (inBG)
    {
        if (bot->IsInCombat() || currentState == BOT_STATE_COMBAT)
        {
            return static_cast<uint32>(base * (sPlayerbotAIConfig.FastReactInBG ? 2.5f : 5.0f));
        }
        else
        {
            return static_cast<uint32>(base * (sPlayerbotAIConfig.FastReactInBG ? 1.0f : 10.0f));
        }
    }

    // When in combat, return 5 times the base
    if (bot->IsInCombat() || currentState == BOT_STATE_COMBAT)
    {
        // A group in a dungeon or raid: at five times the base a member got two or three ticks per GCD and spent
        // them turning and stepping (tank: a cast every 3 s; dps: busy 56-63 %, wave 2026-10-24).
        if (bot->GetGroup() && bot->GetMap() && bot->GetMap()->Instanceable())
            return base * 2;

        return base * 5;
    }

    // [lead15] LEADER_FAST_REACT: a member of a led dungeon group runs fast only while it executes a leader command
    // (the event itself woke it at once); idle it keeps the slow random 10-30 x base (the random staggers the
    // members' idle ticks, and an idle member skips its engine anyway, DoNextAction)
    if (Leader::LEADER_FAST_REACT && Leader::ExecutorMember(bot))
    {
        Leader::MemberEvents events;
        if (Leader::Events::Get(bot->GetGUID(), events))
        {
            // [lead27] Follow: fast while the tank walks or the member is behind
            Unit* followed = events.move == Leader::MoveEvent::Follow ? GetUnit(events.followTarget) : nullptr;
            bool const following = followed && (followed->isMoving() ||
                                                 bot->GetExactDist(followed) > Leader::LEADER_FOLLOW_KEEP_YD);
            bool const walking = following || (events.move == Leader::MoveEvent::MoveTo ||
                                  events.move == Leader::MoveEvent::StandAt) &&
                                 (bot->GetExactDist2d(events.spot.GetPositionX(), events.spot.GetPositionY()) >
                                      events.tolerance ||
                                  !bot->IsStandState());
            if (walking || events.attack != Leader::AttackEvent::None ||
                events.ready == Leader::ReadyEvent::Prepare || events.ready == Leader::ReadyEvent::ReadyCheck)
                return base * 2;
        }
    }

    // [lead171] a led tank with an open pull order runs its steps fast (the walk in, the opener at range, the tag):
    // b180 pull_trace, every step of an order waited for the idle 10-30 x base
    if (Leader::LEADER_FAST_REACT && Leader::Orders::IsTracked(bot->GetGUID()) &&
        Leader::Orders::IsOpen(bot->GetGUID()))
        return base * 2;

    // [pace23] coordinator 2026-11-09: a questing bot out in the world looks again when its walk needs it, not 1-3 s
    // (10-30 x base) later, nor 2-20 s in a town (the resting flag below). b372: the bots covered 4.3-5.3 yd a second
    // of their walking states against their 7 yd/s run - every leg of a far walk (up to its sight, 100 yd, 14 s)
    // ended in a stand until the random idle tick, and every step of a hand-in, a pickup, a vendor visit (one state
    // change a tick) waited one; in towns (the resting flag) 2-20 s each. While it moves it sleeps until its spline
    // nears the end its walk re-plans at (MoveFarToDirect: REPLAN_YD left), never longer than the old idle tick's top;
    // standing between the steps of a walking / working state it looks at twice the base (its own stops - eating,
    // a rest, a flight, a cast - keep the idle tick)
    if (!bot->IsInCombat() && bot->GetMap() && !bot->GetMap()->Instanceable() &&
        HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
    {
        constexpr float REPLAN_YD = 10.0f;  // NewRpgBaseAction::MoveFarToDirect's "remaining > 10" walk on
        NewRpgStatus const status = rpgInfo.GetStatus();
        if (bot->isMoving() && bot->movespline && !bot->movespline->Finalized())
        {
            float const speed = std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
            int32 const wake = bot->movespline->timeElapsed() - int32(REPLAN_YD / speed * IN_MILLISECONDS);
            return std::clamp<uint32>(uint32(std::max<int32>(wake, 0)), base, base * 30);
        }
        // a hold it set itself (ForceToWait / HoldPosition: the "last movement" wait) is slept through to its end
        LastMovement& last = aiObjectContext->GetValue<LastMovement&>("last movement")->Get();
        uint32 const now = getMSTime();
        if (!bot->isMoving() && last.msTime + uint32(last.lastdelayTime) > now)
            return std::clamp<uint32>(last.msTime + uint32(last.lastdelayTime) - now, base, base * 30);
        // [pace25] ... while its looks change something: a look that left the bot where it stood in the same state
        // (nothing to step to: a regeneration with nothing to eat, a wait on a spawn) twice in a row goes back to
        // the idle tick until it moves or its state changes. b373: tick 1.57 -> 1.95 ms, AI max 162 -> 268 ms with
        // pace23's twice-the-base looks at every standing bot
        uint64 const spot = (uint64(uint32(int32(bot->GetPositionX()))) << 32) ^ uint32(int32(bot->GetPositionY())) ^
                            (uint64(status) << 56);
        if (spot != paceSpot)
        {
            paceSpot = spot;
            paceSameLooks = 0;
        }
        else if (paceSameLooks < PACE_SAME_LOOKS)
            ++paceSameLooks;
        if (paceSameLooks < PACE_SAME_LOOKS && status != RPG_REST && status != RPG_TRAVEL_FLIGHT &&
            status != RPG_OUTDOOR_PVP && !bot->IsSitState() && !bot->IsNonMeleeSpellCast(false) && !bot->IsInFlight())
            return base * 2;
    }

    // When not resting, return 10-30 times the base
    if (!bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING))
        return base * urand(10, 30);

    // In other cases, return 20-200 times the base
    return base * urand(20, 200);
}

void PlayerbotAI::PetFollow()
{
    Pet* pet = bot->GetPet();
    if (!pet)
        return;
    pet->AttackStop();
    pet->CastStop();
    pet->ClearInPetCombat();
    pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
    if (pet->ToPet())
        pet->ToPet()->ClearCastWhenWillAvailable();
    CharmInfo* charmInfo = pet->GetCharmInfo();
    if (!charmInfo)
        return;
    charmInfo->SetCommandState(COMMAND_FOLLOW);

    charmInfo->SetIsCommandAttack(false);
    charmInfo->SetIsAtStay(false);
    charmInfo->SetIsReturning(true);
    charmInfo->SetIsCommandFollow(true);
    charmInfo->SetIsFollowing(false);
    charmInfo->RemoveStayPosition();
    charmInfo->SetForcedSpell(0);
    charmInfo->SetForcedTargetGUID();
}

float PlayerbotAI::GetItemScoreMultiplier(ItemQualities quality)
{
    switch (quality)
    {
        // each quality increase 1.1x
        case ITEM_QUALITY_POOR:
            return 1.0f;
            break;
        case ITEM_QUALITY_NORMAL:
            return 1.1f;
            break;
        case ITEM_QUALITY_UNCOMMON:
            return 1.21f;
            break;
        case ITEM_QUALITY_RARE:
            return 1.331f;
            break;
        case ITEM_QUALITY_EPIC:
            return 1.4641f;
            break;
        case ITEM_QUALITY_LEGENDARY:
            return 1.61051f;
            break;
        default:
            break;
    }
    return 1.0f;
}

bool PlayerbotAI::IsHealingSpell(uint32 spellFamilyName, flag96 spellFamilyFlags)
{
    if (!spellFamilyName)
        return false;
    flag96 healingFlags;
    switch (spellFamilyName)
    {
        case SPELLFAMILY_DRUID:
        {
            uint32 healingFlagsA = 0x10 | 0x40 | 0x20 | 0x80;  // rejuvenation | regrowth | healing touch | tranquility
            uint32 healingFlagsB = 0x4000000 | 0x2000000 | 0x2 | 0x10;  // wild growth | nourish | swiftmend | lifebloom
            uint32 healingFlagsC = 0x0;
            healingFlags = {healingFlagsA, healingFlagsB, healingFlagsC};
            break;
        }
        case SPELLFAMILY_PALADIN:
        {
            uint32 healingFlagsA = 0x80000000 | 0x40000000 | 0x8000 |
                                   0x80000;  // holy light | flash of light | lay on hands | judgement of light
            uint32 healingFlagsB = 0x10000;  // holy shock
            uint32 healingFlagsC = 0x0;
            healingFlags = {healingFlagsA, healingFlagsB, healingFlagsC};
            break;
        }
        case SPELLFAMILY_SHAMAN:
        {
            uint32 healingFlagsA = 0x80 | 0x40 | 0x100;  // lesser healing wave | healing wave | chain heal
            uint32 healingFlagsB = 0x400;                // earth shield
            uint32 healingFlagsC = 0x10;                 // riptide
            healingFlags = {healingFlagsA, healingFlagsB, healingFlagsC};
            break;
        }
        case SPELLFAMILY_PRIEST:
        {
            uint32 healingFlagsA = 0x40 | 0x200 | 0x40000 | 0x1000 | 0x800 | 0x400 |
                                   0x10000000;  // renew | prayer of healing | lesser heal | greater heal | flash heal |
                                                // heal | circle of healing
            uint32 healingFlagsB = 0x800000 | 0x20 | 0x4;  // penance | prayer of mending | binding heal
            uint32 healingFlagsC = 0x0;
            healingFlags = {healingFlagsA, healingFlagsB, healingFlagsC};
            break;
        }
        default:
            break;
    }
    return spellFamilyFlags & healingFlags;
}

SpellFamilyNames PlayerbotAI::Class2SpellFamilyName(uint8 cls)
{
    switch (cls)
    {
        case CLASS_WARRIOR:
            return SPELLFAMILY_WARRIOR;
        case CLASS_PALADIN:
            return SPELLFAMILY_PALADIN;
        case CLASS_HUNTER:
            return SPELLFAMILY_HUNTER;
        case CLASS_ROGUE:
            return SPELLFAMILY_ROGUE;
        case CLASS_PRIEST:
            return SPELLFAMILY_PRIEST;
        case CLASS_DEATH_KNIGHT:
            return SPELLFAMILY_DEATHKNIGHT;
        case CLASS_SHAMAN:
            return SPELLFAMILY_SHAMAN;
        case CLASS_MAGE:
            return SPELLFAMILY_MAGE;
        case CLASS_WARLOCK:
            return SPELLFAMILY_WARLOCK;
        case CLASS_DRUID:
            return SPELLFAMILY_DRUID;
        default:
            break;
    }
    return SPELLFAMILY_GENERIC;
}

void PlayerbotAI::AddTimedEvent(std::function<void()> callback, uint32 delayMs)
{
    class LambdaEvent final : public BasicEvent
    {
        std::function<void()> _cb;

    public:
        explicit LambdaEvent(std::function<void()> cb) : _cb(std::move(cb)) {}
        bool Execute(uint64 /*execTime*/, uint32 /*diff*/) override
        {
            _cb();
            return true;  // remove after execution
        }
    };

    // Every Player already owns an EventMap called m_Events
    bot->m_Events.AddEvent(new LambdaEvent(std::move(callback)), bot->m_Events.CalculateTime(delayMs));
}

void PlayerbotAI::EvaluateHealerDpsStrategy()
{
    if (!IsHeal(bot, true))
        return;

    if (sPlayerbotAIConfig.IsRestrictedHealerDPSMap(bot->GetMapId()))
        ChangeStrategy("-healer dps", BOT_STATE_COMBAT);
    else
        ChangeStrategy("+healer dps", BOT_STATE_COMBAT);
}
