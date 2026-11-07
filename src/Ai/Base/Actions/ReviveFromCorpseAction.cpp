/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ReviveFromCorpseAction.h"
#include "BotPath.h"  // [lead109]
#include "Corpse.h"
#include "Creature.h"
#include "Event.h"
#include "FleeManager.h"
#include "GameGraveyard.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "PathGenerator.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ServerFacade.h"
#include "Timer.h"
#include "World.h"  // [guild13] CONFIG_DEATH_SICKNESS_LEVEL

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <vector>

// Top of the default start-zone brackets (Playerbots.ZoneBracket.*).
constexpr uint32 START_ZONE_GRAVEYARD_MAX_LEVEL = 12;

namespace
{
    // A spot is safe to rise on when no living hostile comes at once: none fighting or evading within
    // REVIVE_FIGHT_RADIUS, none at rest within its aggro range + REVIVE_AGGRO_MARGIN (the shaman's Reincarnation rule).
    // Stratholme wave 18: the shaman rose on its corpse among the Ravaged Cadavers that had wiped the group, 151 yd
    // from the group, and died again within 2 s.
    constexpr float REVIVE_FIGHT_RADIUS = 40.0f;
    constexpr float REVIVE_AGGRO_MARGIN = 5.0f;
    // After this long dead a ghost rises anyway, as FindCorpse already does ("walked too long").
    constexpr int64 REVIVE_UNSAFE_MAX_SEC = 8 * MINUTE;
    // The safe spot within the corpse's reclaim radius: rings of these radii, these directions, the floor within
    // SPOT_SNAP_RANGE below SPOT_SNAP_UP above the corpse, a complete walk for the best SPOT_PATH_TRIES of them.
    constexpr std::array<float, 3> SPOT_RINGS = {12.0f, 22.0f, 32.0f};
    constexpr uint32 SPOT_DIRECTIONS = 12;
    constexpr float SPOT_SNAP_UP = 5.0f;
    constexpr float SPOT_SNAP_RANGE = 15.0f;
    constexpr float SPOT_MAX_DZ = 8.0f;
    constexpr uint32 SPOT_PATH_TRIES = 4;
    constexpr uint32 SPOT_RECHECK_MS = 5 * IN_MILLISECONDS;

    // The smallest margin (yd) the point keeps from the reach of the living hostiles around; negative: one would come.
    float SpotMargin(PlayerbotAI* botAI, Player* bot, GuidVector const& hostiles, float x, float y, float z)
    {
        float margin = FLT_MAX;
        for (ObjectGuid const& guid : hostiles)
        {
            Unit* unit = botAI->GetUnit(guid);
            Creature* creature = unit ? unit->ToCreature() : nullptr;
            if (!creature || !creature->IsAlive() || creature->IsTrigger() ||
                creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) ||
                creature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NON_ATTACKABLE_2))
                continue;

            float reach = 0.0f;
            if (creature->IsInEvadeMode() || creature->IsInCombat())
                reach = REVIVE_FIGHT_RADIUS;
            else if (creature->GetReactState() == REACT_PASSIVE)
                continue;
            else
                reach = creature->GetAggroRange(bot) + REVIVE_AGGRO_MARGIN;
            margin = std::min(margin, creature->GetExactDist(x, y, z) - reach);
        }
        return margin;
    }

    // [guild13] user 2026-11-07: after a death a solo questing bot does not go straight back - it rises in town. Below
    // the level of resurrection sickness (CONFIG_DEATH_SICKNESS_LEVEL) the spirit healer costs it nothing but the
    // durability a corpse run costs too, so it takes the graveyard's healer (b300: Peny L8 rose 6 times at its corpse
    // in the Stone Cairn Lake murlocs and died again within seconds); above it the corpse run stays (a safe spot)
    bool RisesInTown(Player* bot)
    {
        Map* map = bot->GetMap();
        return map && !map->Instanceable() && !bot->GetGroup() &&
               int32(bot->GetLevel()) < sWorld->getIntConfig(CONFIG_DEATH_SICKNESS_LEVEL) &&
               GET_PLAYERBOT_AI(bot) && GET_PLAYERBOT_AI(bot)->HasStrategy("new rpg", BOT_STATE_NON_COMBAT);
    }
}

bool ReviveFromCorpseAction::Execute(Event event)
{
    // BG ghosts are revived by spirit healer waves; teleporting to a world graveyard removes them from the BG.
    if (bot->InBattleground())
        return false;

    Player* groupLeader = botAI->GetGroupLeader();
    Corpse* corpse = bot->GetCorpse();

    // follow group Leader when group Leader revives
    WorldPacket& p = event.getPacket();
    if (!p.empty() && p.GetOpcode() == CMSG_RECLAIM_CORPSE && groupLeader && !corpse && bot->IsAlive())
    {
        if (ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
                                              sPlayerbotAIConfig.FarDistance))
        {
            if (!botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT))
            {
                botAI->TellMasterNoFacing("Welcome back!");
                botAI->ChangeStrategy("+follow,-stay", BOT_STATE_NON_COMBAT);
                return true;
            }
        }
    }

    if (!corpse)
        return false;

    if (RisesInTown(bot))  // [guild13] the spirit healer, not the corpse
        return false;

    // if (corpse->GetGhostTime() + bot->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP) >
    // time(nullptr))
    //     return false;

    if (groupLeader)
    {
        if (!GET_PLAYERBOT_AI(groupLeader) && groupLeader->isDead() && groupLeader->GetCorpse() &&
            ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
                                              sPlayerbotAIConfig.FarDistance))
            return false;
    }

    if (!botAI->HasGameClientMaster())
    {
        uint32 dCount = AI_VALUE(uint32, "death count");

        // In a dungeon the corpse is the only way back: its spirit healer stands at the graveyard outside.
        if (dCount >= 5 && !bot->GetMap()->IsDungeon())
            return botAI->DoSpecificAction("spirit healer");
    }

    // Rise only where nothing comes at once; "find corpse" walks to a safe spot of the corpse's radius meanwhile.
    int64 const deadTime = time(nullptr) - corpse->GetGhostTime();
    if (deadTime < REVIVE_UNSAFE_MAX_SEC &&
        SpotMargin(botAI, bot, AI_VALUE(GuidVector, "nearest hostile npcs"), bot->GetPositionX(),
                   bot->GetPositionY(), bot->GetPositionZ()) < 0.0f)
        return false;

    // [guild9] the reclaim delay still runs: HandleReclaimCorpseOpcode refuses it (MiscHandler), so nothing is sent
    // and nothing is logged; the log showed ~100 "revives at body" per hunter death while it waited.
    if (time_t(corpse->GetGhostTime() + bot->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP)) >
        time_t(GameTime::GetGameTime().count()))
        return true;

    LOG_DEBUG("playerbots", "Bot {} {}:{} <{}> revives at body", bot->GetGUID().ToString().c_str(),
              bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str());

    if (bot->GetGroup())
        LOG_INFO("playerbots", "[corpse res] {} revives at ({:.1f},{:.1f},{:.1f}) {:.1f} yd from its corpse",
                 bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                 bot->GetExactDist(corpse));

    bot->GetMotionMaster()->Clear();
    bot->StopMoving();

    WorldPacket packet(CMSG_RECLAIM_CORPSE);
    packet << bot->GetGUID();
    bot->GetSession()->HandleReclaimCorpseOpcode(packet);

    return true;
}

bool FindCorpseAction::Execute(Event /*event*/)
{
    if (bot->InBattleground())
        return false;

    Player* groupLeader = botAI->GetGroupLeader();
    Corpse* corpse = bot->GetCorpse();
    if (!corpse)
        return false;

    // if (groupLeader)
    // {
    //     if (!GET_PLAYERBOT_AI(groupLeader) &&
    //         ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
    //         sPlayerbotAIConfig.FarDistance)) return false;
    // }

    uint32 dCount = AI_VALUE(uint32, "death count");

    // A bot that keeps dying takes the spirit healer like a player would, not a free revive - in the world. In a
    // dungeon its healer stands at the graveyard outside and the corpse is the only way back (the count grows by two
    // per death, death and release, and never resets for a bot that gains no XP: from the third death every ghost
    // that had walked back in stood at its corpse calling for a healer it could not reach).
    bool const inDungeon = bot->GetMap()->IsDungeon();
    if ((!botAI->HasGameClientMaster() && dCount >= 5 && !inDungeon) || RisesInTown(bot))  // [guild13]
        return botAI->DoSpecificAction("spirit healer", Event(), true);

    WorldPosition botPos(bot);
    WorldPosition corpsePos(corpse);
    WorldPosition moveToPos = corpsePos;
    WorldPosition leaderPos(groupLeader);

    float reclaimDist = CORPSE_RECLAIM_RADIUS - 5.0f;
    float corpseDist = botPos.distance(corpsePos);
    int64 deadTime = time(nullptr) - corpse->GetGhostTime();

    // A living leader at the corpse: rise next to him (a dead one lies among the mobs that killed him).
    bool moveToLeader = groupLeader && groupLeader != bot && groupLeader->IsAlive() &&
                        leaderPos.fDist(corpsePos) < reclaimDist;

    GuidVector const hostiles = AI_VALUE(GuidVector, "nearest hostile npcs");
    bool const safeHere =
        SpotMargin(botAI, bot, hostiles, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()) >= 0.0f;

    // Should we ressurect? If so, return false.
    if (corpseDist < reclaimDist)
    {
        if (moveToLeader)  // We are near group leader.
        {
            if (botPos.fDist(leaderPos) < sPlayerbotAIConfig.SpellDistance)
                return false;
        }
        else if (deadTime > 8 * MINUTE)  // We have walked too long already.
            return false;
        else if (safeHere)  // Nothing comes at once here: the same rule as "revive from corpse".
            return false;
    }

    // The ghost always runs back: no teleport to the corpse, also for bots with throttled activity.
    if (bot->isMoving())
        return true;

    // If we are getting close move to a save ressurrection spot instead of just the corpse.
    if (corpseDist < sPlayerbotAIConfig.ReactDistance)
    {
        G3D::Vector3 spot;
        if (moveToLeader)
            moveToPos = leaderPos;
        else if (SafeSpot(corpse, hostiles, reclaimDist, spot))
            moveToPos = WorldPosition(corpsePos.GetMapId(), spot.x, spot.y, spot.z, 0.0f);
        else if (!safeHere)
        {
            FleeManager manager(bot, reclaimDist, 0.0, urand(0, 1), moveToPos);

            if (manager.isUseful())
            {
                float rx, ry, rz;
                if (manager.CalculateDestination(&rx, &ry, &rz))
                    moveToPos = WorldPosition(moveToPos.GetMapId(), rx, ry, rz, 0.0);
                else if (!moveToPos.GetReachableRandomPointOnGround(bot, reclaimDist, urand(0, 1)))
                    moveToPos = corpsePos;
            }
        }
    }

    // Actual mobing part.
    bool moved = false;
    if (deadTime < 10 * MINUTE && (dCount < 5 || inDungeon))  // Look for corpse up to 30 minutes.
    {
        moved = MoveTo(moveToPos.GetMapId(), moveToPos.GetPositionX(), moveToPos.GetPositionY(),
                       moveToPos.GetPositionZ(), false, false);
    }

    // No spirit healer inside a dungeon: the ghost waits at its spot for the mobs to leave.
    if (!moved && !inDungeon)
        moved = botAI->DoSpecificAction("spirit healer", Event(), true);

    return moved;
}

// The spot of the corpse's reclaim radius farthest from the reach of the hostiles around, with a complete walk to it:
// a player rises out of sight of the pack that killed him. Kept for SPOT_RECHECK_MS.
bool FindCorpseAction::SafeSpot(Corpse* corpse, GuidVector const& hostiles, float radius, G3D::Vector3& spot)
{
    uint32 const now = getMSTime();
    if (_spotMs && getMSTimeDiff(_spotMs, now) < SPOT_RECHECK_MS)
    {
        spot = _spot;
        return _spotFound;
    }
    _spotMs = now;
    _spotFound = false;
    if (hostiles.empty())
        return false;

    struct Candidate
    {
        float margin;
        G3D::Vector3 point;
    };
    std::vector<Candidate> candidates;
    for (float const ring : SPOT_RINGS)
    {
        if (ring > radius)
            continue;
        for (uint32 i = 0; i < SPOT_DIRECTIONS; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * i / SPOT_DIRECTIONS;
            float const x = corpse->GetPositionX() + std::cos(angle) * ring;
            float const y = corpse->GetPositionY() + std::sin(angle) * ring;
            float const z = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, corpse->GetPositionZ() + SPOT_SNAP_UP,
                                                     true, SPOT_SNAP_RANGE);
            if (z <= INVALID_HEIGHT || std::fabs(z - corpse->GetPositionZ()) > SPOT_MAX_DZ)
                continue;
            float const margin = SpotMargin(botAI, bot, hostiles, x, y, z);
            if (margin >= 0.0f)
                candidates.push_back({margin, G3D::Vector3(x, y, z)});
        }
    }
    // On the group's side: the safe spot nearest to the living member nearest to the corpse (the ghost rises where
    // the group can take it in, not on the far side of the pack); without one, the farthest from the reaches.
    Player* nearestMember = nullptr;
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsAlive() && member->IsInWorld() &&
                member->GetMap() == bot->GetMap() &&
                (!nearestMember || corpse->GetExactDist(member) < corpse->GetExactDist(nearestMember)))
                nearestMember = member;
        }
    }

    if (nearestMember)
        std::sort(candidates.begin(), candidates.end(),
                  [nearestMember](Candidate const& a, Candidate const& b)
                  {
                      return nearestMember->GetExactDist(a.point.x, a.point.y, a.point.z) <
                             nearestMember->GetExactDist(b.point.x, b.point.y, b.point.z);
                  });
    else
        std::sort(candidates.begin(), candidates.end(),
                  [](Candidate const& a, Candidate const& b) { return a.margin > b.margin; });

    uint32 tries = 0;
    for (Candidate const& candidate : candidates)
    {
        if (++tries > SPOT_PATH_TRIES)
            break;
        BotPath const path = FindBotPath(bot, candidate.point.x, candidate.point.y, candidate.point.z);  // [lead109]
        if (path.type != PATHFIND_NORMAL)
            continue;
        _spot = candidate.point;
        _spotFound = true;
        spot = _spot;
        return true;
    }
    return false;
}

bool FindCorpseAction::isUseful()
{
    if (bot->InBattleground())
        return false;

    return bot->GetCorpse();
}

GraveyardStruct const* SpiritHealerAction::GetGrave(bool startZone)
{
    GraveyardStruct const* ClosestGrave = nullptr;
    GraveyardStruct const* NewGrave = nullptr;

    ClosestGrave = sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId());
    GraveyardStruct const* ownGrave = ClosestGrave;

    if (!startZone && ClosestGrave)
        return ClosestGrave;

    if (botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT) && botAI->GetGroupLeader() && botAI->GetGroupLeader() != bot)
    {
        Player* groupLeader = botAI->GetGroupLeader();
        if (groupLeader && groupLeader != bot)
        {
            ClosestGrave = sGraveyard->GetClosestGraveyard(groupLeader, bot->GetTeamId());

            if (ClosestGrave)
                return ClosestGrave;
        }
    }
    else if (startZone && AI_VALUE(uint8, "durability"))
    {
        TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");

        if (travelTarget->getPosition())
        {
            WorldPosition travelPos = *travelTarget->getPosition();
            if (travelPos.GetMapId() != uint32(-1))
            {
                uint32 areaId = 0;
                uint32 zoneId = 0;
                sMapMgr->GetZoneAndAreaId(bot->GetPhaseMask(), zoneId, areaId, travelPos.GetMapId(), travelPos.GetPositionX(),
                                          travelPos.GetPositionY(), travelPos.GetPositionZ());
                ClosestGrave = sGraveyard->GetClosestGraveyard(travelPos.GetMapId(), travelPos.GetPositionX(), travelPos.GetPositionY(),
                                                               travelPos.GetPositionZ(), bot->GetTeamId(), areaId, zoneId,
                                                               bot->getClass() == CLASS_DEATH_KNIGHT);

                if (ClosestGrave)
                    return ClosestGrave;
            }
        }
    }

    if (ownGrave && bot->GetLevel() > START_ZONE_GRAVEYARD_MAX_LEVEL)
        return ownGrave;

    std::vector<uint32> races;

    if (bot->GetTeamId() == TEAM_ALLIANCE)
        races = {RACE_HUMAN, RACE_DWARF, RACE_GNOME, RACE_NIGHTELF, RACE_DRAENEI};
    else
        races = {RACE_ORC, RACE_TROLL, RACE_TAUREN, RACE_UNDEAD_PLAYER, RACE_BLOODELF};

    float graveDistance = -1;

    WorldPosition botPos(bot);

    for (auto race : races)
    {
        for (uint32 cls = 0; cls < MAX_CLASSES; cls++)
        {
            PlayerInfo const* info = sObjectMgr->GetPlayerInfo(race, cls);
            if (!info)
                continue;

            uint32 areaId = 0;
            uint32 zoneId = 0;
            sMapMgr->GetZoneAndAreaId(bot->GetPhaseMask(), zoneId, areaId, info->mapId, info->positionX,
                                      info->positionY, info->positionZ);

            NewGrave = sGraveyard->GetClosestGraveyard(info->mapId, info->positionX, info->positionY, info->positionZ,
                                                       bot->GetTeamId(), areaId, zoneId, cls == CLASS_DEATH_KNIGHT);
            if (!NewGrave)
                continue;

            WorldPosition gravePos(NewGrave->Map, NewGrave->x, NewGrave->y, NewGrave->z);

            float newDist = botPos.fDist(gravePos);

            if (graveDistance < 0 || newDist < graveDistance)
            {
                ClosestGrave = NewGrave;
                graveDistance = newDist;
            }
        }
    }

    return ClosestGrave;
}

bool SpiritHealerAction::Execute(Event /*event*/)
{
    // GetGrave() picks world graveyards; teleporting there from a BG removes the bot from it.
    if (bot->InBattleground())
        return false;

    // Player::GetCorpse looks on the current map only: a ghost whose corpse lies in an instance has none here, and
    // its spirit healer is the one outside (wave 11: every dungeon corpse run reported "no spirit healer").
    if (!bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
    {
        botAI->TellError("I am not a spirit");
        return false;
    }

    Corpse* corpse = bot->GetCorpse();
    uint32 dCount = AI_VALUE(uint32, "death count");
    int64 deadTime = corpse ? time(nullptr) - corpse->GetGhostTime() : 0;

    GraveyardStruct const* ClosestGrave =
        GetGrave(dCount > 10 || deadTime > 15 * MINUTE || AI_VALUE(uint8, "durability") < 10);

    if (!ClosestGrave)
        return false;

    if (bot->GetDistance2d(ClosestGrave->x, ClosestGrave->y) < sPlayerbotAIConfig.SightDistance)
    {
        GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
        for (GuidVector::iterator i = npcs.begin(); i != npcs.end(); i++)
        {
            Unit* unit = botAI->GetUnit(*i);
            if (!unit || !unit->HasNpcFlag(UNIT_NPC_FLAG_SPIRITHEALER))
                continue;

            if (bot->GetDistance(unit) > INTERACTION_DISTANCE)
                return MoveNear(unit, INTERACTION_DISTANCE - 1.0f);

            LOG_DEBUG("playerbots", "Bot {} {}:{} <{}> revives at spirit healer", bot->GetGUID().ToString().c_str(),
                      bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());

            // The real spirit healer: resurrection sickness and durability loss, like for a player.
            WorldPacket packet(CMSG_SPIRIT_HEALER_ACTIVATE, 8);
            packet << unit->GetGUID();
            bot->GetSession()->HandleSpiritHealerActivateOpcode(packet);
            if (!bot->IsAlive())
                return false;

            context->GetValue<Unit*>("current target")->Set(nullptr);
            bot->SetTarget();
            botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault("hello", "Hello", {}));

            if (dCount > 20)
                context->GetValue<uint32>("death count")->Set(0);

            return true;
        }
    }

    bool moved = false;

    if (bot->IsWithinLOS(ClosestGrave->x, ClosestGrave->y, ClosestGrave->z))
        moved = MoveNear(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, 0.0);
    else
        moved = MoveTo(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, false, false);

    if (moved)
        return true;

    // A ghost that cannot walk there stays put: no teleport to the graveyard. A failed walk is no death: counted as one
    // it pushed every ghost past the "death count" 5 of FindCorpse / ReviveFromCorpse within a few ticks.
    return false;
}

bool SpiritHealerAction::isUseful() { return bot->HasPlayerFlag(PLAYER_FLAGS_GHOST); }
