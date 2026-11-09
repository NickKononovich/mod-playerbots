/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildCoordinator.h"  // [group43]
#include "FollowActions.h"
#include "BotPath.h"  // [lead109]
#include "LeaderOrders.h"  // [lead15]
#include "GuildHonest.h"  // [wait16]
#include "PlayerbotAIConfig.h"  // [wait16]
#include "SoloSafety.h"  // [wait16] StandsInReach
#include "NewRpgCoop.h"  // [group18]
#include <mutex>          // [group18]
#include <unordered_set>  // [group18]
#include "CheckMountStateAction.h"
#include "Creature.h"
#include "EscortProtect.h"
#include "Event.h"
#include "Formations.h"
#include "LastMovementValue.h"
#include "Map.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "Transport.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    Transport* GetTransportForPosTolerant(Map* map, WorldObject* ref, uint32 phaseMask, float x, float y, float z)
    {
        if (!map || !ref)
            return nullptr;

        std::array<float, 4> const probes = { z, z + 0.5f, z + 1.5f, z - 0.5f };
        for (float const pz : probes)
        {
            if (Transport* t = map->GetTransportForPos(phaseMask, x, y, pz, ref))
                return t;
        }

        return nullptr;
    }

    // Attempts to find a point on the leader's transport that is closer to the bot,
    // by probing along the segment from master -> bot and returning the last point
    // that is still detected as being on the expected transport.
    bool FindBoardingPointOnTransport(Map* map, Transport* expectedTransport, WorldObject* ref,
        float masterX, float masterY, float masterZ,
        float botX, float botY, float botZ,
        float& outX, float& outY, float& outZ)
    {
        if (!map || !expectedTransport || !ref)
            return false;

        uint32 const phaseMask = ref->GetPhaseMask();

        // Ensure master is actually detected on that transport (tolerant).
        if (GetTransportForPosTolerant(map, ref, phaseMask, masterX, masterY, masterZ) != expectedTransport)
            return false;

        // The raycast in GetTransportForPos starts at (z + 2). Probe with a safe Z.
        float const probeZ = std::max(masterZ, botZ);

        // Adaptive step count: small platforms need tighter sampling.
        float const dx2 = botX - masterX;
        float const dy2 = botY - masterY;
        float const dist2d = std::sqrt(dx2 * dx2 + dy2 * dy2);
        int32 const steps = std::clamp(static_cast<int32>(dist2d / 0.75f), 10, 28);

        float const dx = (botX - masterX) / static_cast<float>(steps);
        float const dy = (botY - masterY) / static_cast<float>(steps);

        // Master must actually be on the expected transport for this to work.
        if (map->GetTransportForPos(ref->GetPhaseMask(), masterX, masterY, probeZ, ref) != expectedTransport)
            return false;

        float lastX = masterX;
        float lastY = masterY;
        bool found = false;

        for (int32 i = 1; i <= steps; ++i)
        {
            float const px = masterX + dx * i;
            float const py = masterY + dy * i;

            Transport* const t = GetTransportForPosTolerant(map, ref, phaseMask, px, py, probeZ);
            if (t != expectedTransport)
                break;

            lastX = px;
            lastY = py;
            found = true;
        }

        if (!found)
            return false;

        outX = lastX;
        outY = lastY;
        outZ = masterZ; // keep deck-level Z to encourage stepping onto the platform/boat
        return true;
    }
}

bool FollowAction::Execute(Event /*event*/)
{
    // [escort_anchor] A leashed escort under way (Old Hillsbrad's Thrall) is the anchor of every member, the group
    // leader included: the group walks with the NPC it protects, not after the leader on the harness route (ohb-h
    // 163739: "follow" to the leader against "escort stay close" to Thrall, 78 move_conflict flips).
    // [b42_ohb] ... except while the NPC waits and the leader is not with it yet: the members follow the leader
    // back (EscortProtect::EscortFollowAnchor).
    if (Creature* escort = EscortProtect::EscortFollowAnchor(botAI))
        return Follow(escort);

    Formation* formation = AI_VALUE(Formation*, "formation");
    std::string const target = formation->GetTargetName();

    // Transport handling for moving transports only (boats/zeppelins).
    Player* master = botAI->GetMaster();
    if (master && master->IsInWorld() && bot->IsInWorld() && bot->GetMapId() == master->GetMapId())
    {
        Map* map = master->GetMap();
        uint32 const mapId = bot->GetMapId();
        Transport* transport = nullptr;
        bool masterOnTransport = false;

        if (master->GetTransport())
        {
            transport = master->GetTransport();
            masterOnTransport = true;
        }
        else if (map)
        {
            transport = GetTransportForPosTolerant(map, master, master->GetPhaseMask(),
                master->GetPositionX(), master->GetPositionY(), master->GetPositionZ());
            masterOnTransport = (transport != nullptr);
        }

        // Ignore static transports (elevators/trams): only keep boats/zeppelins here.
        if (transport && transport->IsStaticTransport())
            transport = nullptr;

        if (transport && map && bot->GetTransport() != transport)
        {
            float const botProbeZ = std::max(bot->GetPositionZ(), transport->GetPositionZ());
            Transport* botSurfaceTransport = GetTransportForPosTolerant(map, bot, bot->GetPhaseMask(),
                bot->GetPositionX(), bot->GetPositionY(), botProbeZ);

            if (botSurfaceTransport == transport)
            {
                transport->AddPassenger(bot, true);
                bot->StopMovingOnCurrentPos();
                return true;
            }

            float const boardingAssistDistance = 60.0f;
            float const dist2d = ServerFacade::instance().GetDistance2d(bot, master);
            bool const inAssist = ServerFacade::instance().IsDistanceLessOrEqualThan(dist2d, boardingAssistDistance);

            if (inAssist)
            {
                float destX = masterOnTransport ? master->GetPositionX() : transport->GetPositionX();
                float destY = masterOnTransport ? master->GetPositionY() : transport->GetPositionY();
                float destZ = masterOnTransport ? master->GetPositionZ() : transport->GetPositionZ();
                float edgeX = 0.0f;
                float edgeY = 0.0f;
                float edgeZ = 0.0f;

                if (masterOnTransport &&
                    FindBoardingPointOnTransport(map, transport, master,
                        master->GetPositionX(), master->GetPositionY(), master->GetPositionZ(),
                        bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                        edgeX, edgeY, edgeZ))
                {
                    destX = edgeX;
                    destY = edgeY;
                    destZ = edgeZ;
                }

                MovementPriority const priority = botAI->GetState() == BOT_STATE_COMBAT
                    ? MovementPriority::MOVEMENT_COMBAT
                    : MovementPriority::MOVEMENT_NORMAL;

                bool const movingAllowed = IsMovingAllowed();
                bool const dupMove = IsDuplicateMove(destX, destY, destZ);
                bool const waiting = IsWaitingForLastMove(priority);

                if (movingAllowed && !dupMove && !waiting)
                {
                    if (bot->IsSitState())
                        bot->SetStandState(UNIT_STAND_STATE_STAND);

                    bot->CastStop();

                    if (MotionMaster* mm = bot->GetMotionMaster())
                    {
                        mm->MovePoint(
                            /*id*/ 0,
                            /*coords*/ destX, destY, destZ,
                            /*forcedMovement*/ FORCED_MOVEMENT_NONE,
                            /*speed*/ 0.0f,
                            /*orientation*/ 0.0f,
                            /*generatePath*/ false,
                            /*forceDestination*/ false);
                    }
                    else
                        return false;

                    float delay = IN_MILLISECONDS * MoveDelay(bot->GetExactDist(destX, destY, destZ));
                    delay = std::clamp(delay, 0.0f, static_cast<float>(sPlayerbotAIConfig.MaxWaitForMove));

                    AI_VALUE(LastMovement&, "last movement")
                        .Set(mapId, destX, destY, destZ, bot->GetOrientation(), delay, priority);
                    ClearIdleState();
                    return true;
                }
            }
        }
    }
    // end unified transport handling

    bool moved = false;
    // [group18] a group the session restored (the core keeps groups across a restart) whose purpose is gone: no
    // co-op / help party holds the follower or its guild leader (that registry starts empty with the session), the
    // leader is no human and stands in another zone. Looked at once per session, once its leader is in the world.
    // [group19] ... and any time a party holds the follower whose leader is not its group's leader (a help party it
    // was assigned to while a restored group held it: the invite could not take it). b363: group18 never fired -
    // the follow's formation target is empty for a group follower (it follows the formation's spot, the leader
    // read from the group now), the first follow came before the leader was in the world, and InParty held Bami
    // (help party of Irorm) in Sehdrurmund's restored group 3549 yd away; Ainne 1949 yd behind Kianey, Kylantino 1561
    if (Group* group = bot->GetGroup();
        group && GuildHonest::IsHonestBot(bot) && !bot->GetMap()->Instanceable() &&
        group->GetLeaderGUID() != bot->GetGUID())
    {
        Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());
        if (leader && GuildHonest::IsHonestBot(leader))
        {
            uint32 const low = bot->GetGUID().GetCounter();
            ObjectGuid partyLeader;
            if (NewRpgCoop::InParty(low) && NewRpgCoop::PartyLeaderOf(low, partyLeader) && partyLeader &&
                partyLeader != leader->GetGUID() &&
                NewRpgCoop::LeaveStaleGroup(
                    bot, Acore::StringFormat("[group19] its party's leader is not its group's leader {} ({:.0f} yd)",
                                             leader->GetName(), bot->GetExactDist2d(leader))))
                return true;
            static std::mutex staleLock;
            static std::unordered_set<uint32> staleChecked;
            bool first = false;
            {
                std::lock_guard<std::mutex> guard(staleLock);
                first = staleChecked.insert(low).second;
            }
            // [group43] ... in its leader's zone too: a restored group no party holds is no plan of the global
            // leader's, and its member runs no New RPG of its own (AiFactory gives "new rpg" to a group's leader
            // only) - no sickness hold, no login escape, no rest out of reach. b390: Bami (in Kianey's), Peny
            // (Guinudrond's), Feette, Mytaeleas, Nelaeleas never logged a status all run; Bami 13 deaths, most under
            // Resurrection Sickness following its leader into the Westfall camps
            if (first && !NewRpgCoop::InParty(low) && !NewRpgCoop::InParty(leader->GetGUID().GetCounter()) &&
                !sGuildCoordinator.InSameActiveFive(low, leader->GetGUID().GetCounter()) &&
                NewRpgCoop::LeaveStaleGroup(
                    bot, Acore::StringFormat("[group43] a restored group no party holds (its leader {} {:.0f} yd, {})",
                                             leader->GetName(), bot->GetExactDist2d(leader),
                                             leader->GetZoneId() == bot->GetZoneId() ? "same zone"
                                                                                     : "another zone")))
                return true;
        }
    }
    // [wait16] coordinator 2026-11-09: a guild follower far from its leader (beyond its sight) and standing - its
    // follow makes no progress over that distance, the leader walks to meet it ([wait14]) - stands out of every idle
    // mob's reach (wander area, patrol beat) meanwhile, as a resting bot does (RestEscape). b355 Febineysh L8 stood
    // 437 yd behind Orton for minutes ("stuck watchdog: no progress"), wolves and murlocs walked into it, a Murloc
    // pack killed it
    if (!target.empty())
        if (Unit* leader = AI_VALUE(Unit*, target);
            leader && GuildHonest::IsHonestBot(bot) && !bot->IsInCombat() && !bot->isMoving() &&
            !bot->GetMap()->Instanceable() && leader->GetMapId() == bot->GetMapId() &&
            bot->GetExactDist2d(leader) > sPlayerbotAIConfig.SightDistance)
        {
            std::string who;
            std::vector<G3D::Vector3> spots;
            if (SoloSafety::StandsInReach(bot, who, spots))
                for (G3D::Vector3 const& at : spots)
                {
                    Leader::RouteResult const route = Leader::Route(bot, Position(at.x, at.y, at.z));
                    if (!route.found || !route.complete || route.throughRefused)
                        continue;
                    if (!MoveTo(bot->GetMapId(), at.x, at.y, at.z, false, false, false, true))
                        continue;
                    LOG_INFO("playerbots", "[wait16] {} L{} {:.0f} yd behind its leader {}, standing within the reach "
                             "of {}: waits at ({:.0f},{:.0f}), {:.0f} yd walk",
                             bot->GetName(), bot->GetLevel(), bot->GetExactDist2d(leader), leader->GetName(), who,
                             at.x, at.y, route.length);
                    return true;
                }
        }
    // [quest17] a guild follower hands in its complete quests where its party stands at their ender ([quest18]:
    // MemberHandIn, also from isUseful - the follow is not run while the follower stands by its leader)
    if (MemberHandIn(true))
        return true;
    if (!target.empty())
    {
        moved = Follow(AI_VALUE(Unit*, target));
    }

    else
    {
        WorldLocation loc = formation->GetLocation();
        if (Formation::IsNullLocation(loc) || loc.GetMapId() == MAPID_INVALID)
            return false;

        MovementPriority priority = botAI->GetState() == BOT_STATE_COMBAT ? MovementPriority::MOVEMENT_COMBAT : MovementPriority::MOVEMENT_NORMAL;
        moved = MoveTo(loc.GetMapId(), loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ(), false, false, false,
                       true, priority, true);
    }

    // This section has been commented out because it was forcing the pet to
    // follow the bot on every "follow" action tick, overriding any attack or
    // stay commands that might have been issued by the player.
    // if (Pet* pet = bot->GetPet())
    // {
    //     botAI->PetFollow();
    // }
    // if (moved)
    // botAI->SetNextCheckDelay(sPlayerbotAIConfig.ReactDelay);

    return moved;
}

// [quest17] a guild follower's complete quest at an ender its leader stands by: its leader's route takes the stop
// ([quest16] "HAND_IN ... (for <member>)"), a follower runs this follow, not the rpg status. [quest18] Asked from
// isUseful too: within its follow distance the follow is not run at all - b359 Arrengyl stood 14-21 yd from Bierya
// at each of its 4 hand-in stops, no hand-in. `act`: walk there / hand in; else only whether there is one
// [group23] the quests the member tried to take at a giver (the core refused it or took it): one try each
bool FollowAction::TakeTried(uint32 questId, bool mark)
{
    static std::mutex triedLock;
    static std::unordered_map<uint32, std::unordered_set<uint32>> tried;
    std::lock_guard<std::mutex> guard(triedLock);
    auto& mine = tried[bot->GetGUID().GetCounter()];
    return mark ? !mine.insert(questId).second : mine.count(questId) != 0;
}

bool FollowAction::MemberHandIn(bool act)
{
    if (!GuildHonest::IsHonestBot(bot) || bot->IsInCombat() || bot->GetMap()->Instanceable())
        return false;
    // [group23] the leader: the formation's target, else the group's leader (a group follower's formation has no
    // target name: it follows the formation's spot - the hand-in never ran for it)
    Formation* formation = AI_VALUE(Formation*, "formation");
    std::string const target = formation ? formation->GetTargetName() : std::string();
    Unit* leader = !target.empty() ? AI_VALUE(Unit*, target)
                   : bot->GetGroup() ? ObjectAccessor::FindPlayer(bot->GetGroup()->GetLeaderGUID())
                                     : nullptr;
    if (!leader || leader == bot || leader->GetMapId() != bot->GetMapId())
        return false;
    Player* const leaderPlayer = leader->ToPlayer();
    bool complete = false;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE && !complete; ++slot)
        if (uint32 const questId = bot->GetQuestSlotQuestId(slot))
            complete = bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest npcs"))
    {
        Creature* npc = botAI->GetCreature(guid);
        if (!npc || !npc->IsAlive() || !npc->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER) ||
            leader->GetExactDist2d(npc) > sPlayerbotAIConfig.SightDistance)
            continue;
        uint32 ends = 0;
        if (complete)
        {
            QuestRelationBounds const bounds = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(npc->GetEntry());
            for (auto itr = bounds.first; itr != bounds.second && !ends; ++itr)
                if (bot->GetQuestStatus(itr->second) == QUEST_STATUS_COMPLETE)
                    ends = itr->second;
        }
        // [group23] a quest its leader took at this giver (it stands by it) that the member may take too: the
        // member takes it there as a player of the group does (a shareable one comes by the leader's share)
        uint32 takes = 0;
        if (!ends && leaderPlayer && leader->GetExactDist2d(npc) <= INTERACTION_DISTANCE * 2.0f)
        {
            QuestRelationBounds const bounds = sObjectMgr->GetCreatureQuestRelationBounds(npc->GetEntry());
            for (auto itr = bounds.first; itr != bounds.second && !takes; ++itr)
                if (Quest const* quest = sObjectMgr->GetQuestTemplate(itr->second);
                    quest && leaderPlayer->GetQuestStatus(itr->second) != QUEST_STATUS_NONE &&
                    !leaderPlayer->GetQuestRewardStatus(itr->second) &&
                    bot->GetQuestStatus(itr->second) == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) &&
                    bot->CanAddQuest(quest, false) && !TakeTried(itr->second, false))
                    takes = itr->second;
        }
        if (!ends && !takes)
            continue;
        if (!act)
            return true;
        if (npc->IsWithinDistInMap(bot, INTERACTION_DISTANCE))
        {
            if (takes)
            {
                LOG_INFO("playerbots", "[group23] {} L{} takes q{} at {} with its leader {}", bot->GetName(),
                         bot->GetLevel(), takes, npc->GetName(), leader->GetName());
                TakeTried(takes, true);  // one try per quest: the giver's own checks refuse it else (no retry)
                WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST);
                p << npc->GetGUID() << takes << uint32(0);
                p.rpos(0);
                bot->GetSession()->HandleQuestgiverAcceptQuestOpcode(p);
                return true;
            }
            LOG_INFO("playerbots", "[quest17] {} L{} hands in q{} at {} with its leader {} by it", bot->GetName(),
                     bot->GetLevel(), ends, npc->GetName(), leader->GetName());
            return botAI->DoSpecificAction("talk to quest giver", Event("member hand-in", npc->GetGUID()), true);
        }
        if (MoveTo(npc->GetMapId(), npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ(), false, false,
                   false, true))
            return true;
    }
    return false;
}

bool FollowAction::isUseful()
{
    // move from group takes priority over follow as it's added and removed automatically
    // (without removing/adding follow)
    if (botAI->HasStrategy("move from group", BOT_STATE_COMBAT) ||
        botAI->HasStrategy("move from group", BOT_STATE_NON_COMBAT))
        return false;

    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr)
        return false;

    // Following stops casts, which would cancel the mount we start when the leader rides off.
    if (CheckMountStateAction::IsCastingMount(bot))
        return false;

    if (MemberHandIn(false))  // [quest18] its hand-in by its leader runs within the follow distance too
        return true;

    Formation* formation = AI_VALUE(Formation*, "formation");
    if (!formation)
        return false;

    // [escort_anchor] the escort NPC instead of the formation's target (see Execute; [b42_ohb] EscortFollowAnchor).
    if (Creature* escort = EscortProtect::EscortFollowAnchor(botAI))
    {
        // Keep eating / drinking next to it.
        if (!bot->IsInCombat() && !bot->IsStandState() && bot->GetDistance(escort) < 40.0f)
            return false;

        return ServerFacade::instance().IsDistanceGreaterThan(bot->GetExactDist2d(escort),
                                                              formation->GetMaxDistance());
    }

    std::string const target = formation->GetTargetName();

    Unit* fTarget = nullptr;
    if (!target.empty())
        fTarget = AI_VALUE(Unit*, target);
    else if (botAI->GetMaster())
        fTarget = botAI->GetMaster();
    else
        fTarget = AI_VALUE(Unit*, "group leader");

    if (fTarget)
    {
        if (fTarget->HasUnitState(UNIT_STATE_IN_FLIGHT))
            return false;

        if (!CanDeadFollow(fTarget))
            return false;

        if (fTarget->GetGUID() == bot->GetGUID())
            return false;

        // Keep eating/drinking while the one we follow is still close; a selfbot leader waits for us.
        // [lead15] LEADER_ONE_REST_RULE: not in a group the dungeon leader leads (the leader waits for a sitter and
        // moves the members itself; no own 40 yd rule)
        if (!bot->IsInCombat() && !bot->IsStandState() && fTarget->GetMapId() == bot->GetMapId() &&
            bot->GetDistance(fTarget) < 40.0f && !(Leader::LEADER_ONE_REST_RULE && Leader::LedGroup(bot)))
            return false;
    }

    float distance = 0.f;
    if (!target.empty())
    {
        distance = AI_VALUE2(float, "distance", target);
    }
    else
    {
        WorldLocation loc = formation->GetLocation();
        if (Formation::IsNullLocation(loc) || bot->GetMapId() != loc.GetMapId())
            return false;

        distance = bot->GetDistance(loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ());
    }
    if (botAI->HasStrategy("master fishing", BOT_STATE_NON_COMBAT))
        return ServerFacade::instance().IsDistanceGreaterThan(distance, sPlayerbotAIConfig.FishingDistanceFromMaster);

    return ServerFacade::instance().IsDistanceGreaterThan(distance, formation->GetMaxDistance());
}

bool FollowAction::CanDeadFollow(Unit* target)
{
    // In battleground, wait for spirit healer
    if (bot->InBattleground() && !bot->IsAlive())
        return false;

    // Move to corpse when dead and player is alive or not a ghost.
    if (!bot->IsAlive() && (target->IsAlive() || !target->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST)))
        return false;

    // [ledge_drop_false] a living bot does not follow a dead leader's body far below it that no ground path
    // reaches (bot-n 20261025-184229-56: the tank fell through the floor to 85 yd under it, the members walked
    // "follow" at the ledge guard 144 times); it holds, and the resurrection logic takes the body. Only for a dead
    // target more than DEAD_FOLLOW_MAX_DZ below, so the path query stays off the per-tick path.
    constexpr float DEAD_FOLLOW_MAX_DZ = 10.0f;
    if (bot->IsAlive() && !target->IsAlive() && target->GetMapId() == bot->GetMapId() &&
        bot->GetPositionZ() - target->GetPositionZ() > DEAD_FOLLOW_MAX_DZ)
    {
        BotPath const path =  // [lead109]
            FindBotPath(bot, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
        PathType const type = path.type;
        if (!(type & PATHFIND_NORMAL) ||
            (type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE | PATHFIND_NOT_USING_PATH)))
            return false;
    }

    return true;
}
