/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoSActions.h"
#include "DungeonInteract.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "Map.h"  // [b44_cos] isInLineOfSight (EpochStackAction)
#include "PathGenerator.h"
#include "Playerbots.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>  // [b44_cos]
#include <unordered_map>

using namespace CoSIds;

namespace
{
constexpr float CHROMIE_SEARCH_RANGE = 300.0f;
constexpr float NPC_TALK_DISTANCE = INTERACTION_DISTANCE - 1.0f;
constexpr float CRATE_USE_DISTANCE = 6.0f;  // Arcane Disruption 49590 reaches the crate helper within 8 yd
constexpr float CRATE_APPROACH_DISTANCE = 25.0f;
// CoS-n wave 11: the talk ran every tick (StopMoving included) without the Disruptor showing up, and the leader stood
// at Chromie for the whole run.
constexpr uint32 CHROMIE_RETRY_MS = 15 * IN_MILLISECONDS;
Position const CHROMIE_MIDDLE_POS = Position(1813.298f, 1283.578f, 142.326f);  // EventPos[EVENT_POS_CHROMIE]

// [b35_cos4] Festival Lane Gate 188685 (a closed DB door, no script opens it) shuts the Town Hall's front square off
// the Corruptor's street; the navmesh does not know it and its path from the Town Hall to the Corruptor / to Arthas
// runs through it. cos-h 20261026-024524-cos-h-5H-19: Rtdruaj, still on the Town Hall floor when the detour began
// (03:13:15, 53 yd behind, dz 10), walked "cos engage event target" out of the Town Hall's front ("[closed door] ...
// runs through Festival Lane Gate 188685 at 230.2 yd -> shorter (2350.5,1223.8,130.1)") and then stood at the gate
// (2354,1228,130.5) for the rest of the run: 291 "[closed door] ... -> stay" lines, 175-210 yd from the group, the
// tank walked back for it (meet_stuck). Such a walk goes by the secret passage instead: to its foot below the drop
// (shortcut_drop landing, cos-h 5B-21 02:13:29), from there the street path no longer touches the gate.
constexpr uint32 GO_FESTIVAL_LANE_GATE = 188685;
constexpr float FESTIVAL_GATE_SEARCH = 250.0f;
constexpr uint32 FESTIVAL_GATE_SEARCH_MS = 10 * IN_MILLISECONDS;
constexpr float FESTIVAL_GATE_PATH_NEAR = 6.0f;   // a path point this close (2D) to the gate runs through it
constexpr float FESTIVAL_GATE_PATH_DZ = 10.0f;
constexpr float FESTIVAL_GATE_SAMPLE = 2.0f;
constexpr float PASSAGE_FOOT_ARRIVED = 8.0f;
constexpr uint32 FESTIVAL_GATE_LOG_MS = 10 * IN_MILLISECONDS;
Position const SECRET_PASSAGE_FOOT = Position(2493.5f, 1122.2f, 141.0f);

// True (and x, y, z set to the passage foot) when the bot's path to (x, y, z) runs through the closed gate.
bool RerouteAroundFestivalGate(Player* bot, char const* by, float& x, float& y, float& z)
{
    uint32 const progress = CoSHelpers::GetProgress(bot);
    if (progress < COS_PROGRESS_LAST_CITY || progress >= COS_PROGRESS_FINISHED ||
        bot->GetExactDist(SECRET_PASSAGE_FOOT) <= PASSAGE_FOOT_ARRIVED)
        return false;

    struct GateCache
    {
        ObjectGuid guid;
        uint32 searchMs = 0;
    };
    thread_local std::unordered_map<uint32, GateCache> gates;
    GateCache& entry = gates[bot->GetInstanceId()];
    GameObject* gate = entry.guid ? bot->GetMap()->GetGameObject(entry.guid) : nullptr;
    if (!gate)
    {
        uint32 const now = getMSTime();
        if (entry.searchMs && getMSTimeDiff(entry.searchMs, now) < FESTIVAL_GATE_SEARCH_MS)
            return false;
        entry.searchMs = now ? now : 1;
        gate = bot->FindNearestGameObject(GO_FESTIVAL_LANE_GATE, FESTIVAL_GATE_SEARCH);
        if (!gate)
            return false;
        entry.guid = gate->GetGUID();
    }
    if (gate->GetGoState() == GO_STATE_ACTIVE || bot->GetExactDist2d(gate) > FESTIVAL_GATE_SEARCH)
        return false;

    PathGenerator path(bot);
    path.CalculatePath(x, y, z, false);
    std::vector<G3D::Vector3> const& points = path.GetPath();
    bool crosses = false;
    for (size_t i = 1; i < points.size() && !crosses; ++i)
    {
        G3D::Vector3 const& from = points[i - 1];
        G3D::Vector3 const& to = points[i];
        uint32 const steps = std::max<uint32>(1, uint32(std::ceil((to - from).length() / FESTIVAL_GATE_SAMPLE)));
        for (uint32 step = 0; step <= steps && !crosses; ++step)
        {
            G3D::Vector3 const point = from + (to - from) * (float(step) / float(steps));
            crosses = gate->GetExactDist2d(point.x, point.y) <= FESTIVAL_GATE_PATH_NEAR &&
                      std::fabs(point.z - gate->GetPositionZ()) <= FESTIVAL_GATE_PATH_DZ;
        }
    }
    if (!crosses)
        return false;

    thread_local std::unordered_map<uint64, uint32> logMs;
    uint32 const now = getMSTime();
    uint32& at = logMs[bot->GetGUID().GetRawValue()];
    if (!at || getMSTimeDiff(at, now) >= FESTIVAL_GATE_LOG_MS)
    {
        at = now ? now : 1;
        LOG_INFO("playerbots",
                 "[b35_cos4] {}: {} to ({:.1f},{:.1f},{:.1f}) runs through the closed Festival Lane Gate ({:.0f} yd "
                 "away): walks by the secret passage, to ({:.1f},{:.1f},{:.1f}) first (progress {})",
                 bot->GetName(), by, x, y, z, bot->GetExactDist(gate), SECRET_PASSAGE_FOOT.GetPositionX(),
                 SECRET_PASSAGE_FOOT.GetPositionY(), SECRET_PASSAGE_FOOT.GetPositionZ(), progress);
    }
    x = SECRET_PASSAGE_FOOT.GetPositionX();
    y = SECRET_PASSAGE_FOOT.GetPositionY();
    z = SECRET_PASSAGE_FOOT.GetPositionZ();
    return true;
}

// Walks a DB gossip story line: on each listed page picks the story option until the final page was answered.
bool SelectStoryPages(Player* bot, Creature* npc, std::initializer_list<uint32> pages, uint32 finalPage)
{
    if (!DungeonInteract::OpenGossip(bot, npc))
        return false;

    bool done = false;
    for (size_t step = 0; step < pages.size() && !done; ++step)
    {
        uint32 const menuId = bot->PlayerTalkClass->GetGossipMenu().GetMenuId();
        bool known = false;
        for (uint32 page : pages)
            known = known || page == menuId;

        if (!known || !DungeonInteract::SelectGossipOption(bot, npc, static_cast<int32>(GOSSIP_OPTION_STORY)))
            break;

        done = menuId == finalPage;
    }

    bot->PlayerTalkClass->SendCloseGossip();
    return done;
}
}  // namespace

bool ExplodeGhoulSpreadAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "salramm the fleshcrafter");
    if (!boss) { return false; }

    float distance = 10.0f;
    float distanceExtra = 2.0f;
    GuidVector npcs = AI_VALUE(GuidVector, "nearest hostile npcs");
    for (ObjectGuid const& guid : npcs)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsAlive() && unit->GetEntry() == NPC_GHOUL_MINION)
        {
            float currentDistance = bot->GetExactDist2d(unit);
            if (currentDistance < distance + distanceExtra)
            {
                return MoveAway(unit, distance + distanceExtra - currentDistance);
            }
        }
    }
    return false;
}

// [b44_cos] Time Step (boss_epoch.cpp: Time Warp, then up to three chained Time Steps to random players within 50
// yd) charges ranged members; standing close in keeps the charges short. The stack used to measure the current
// target (not the boss) and walk a straight line onto his centre: inside his melee reach, where "ranged in melee
// danger" (ACTION_MOVE + 6) stepped the bot 7 yd out again (cos-h: 25 move_conflicts in 12 runs), and through walls,
// where Move() failed every tick and the bot idled the whole kill out of sight (Rtmagah 5A-18 at (2424.0,1135.6),
// 14.8 yd from Epoch, "epoch stack: Execute returned false", nothing ran). Now: a navmesh-pathed walk to a spot of
// one shared band (CoSHelpers::EpochStackMinDistance .. Max, just outside the danger edge) beside the tank that sees
// the boss; no such spot: the path towards the boss up to its first point in sight, then to the tank. The band
// starts at 8 yd, past the hunters' dead zone, so hunters stack too.
bool EpochStackAction::isUseful()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "chrono-lord epoch");
    return boss && boss->IsAlive() && boss->IsInCombat() && boss->GetMap() == bot->GetMap() &&
           !CoSHelpers::IsEpochStacked(bot, boss);
}

bool EpochStackAction::FindStackSpot(Unit* boss, Unit* tank, float& outX, float& outY, float& outZ)
{
    constexpr float EYE = 2.0f;
    constexpr float STACK_MAX_DZ = 4.0f;     // a spot on another level is no stack spot (IsEpochStacked)
    constexpr float PATH_END_SLACK = 2.0f;   // the path has to end this close to the spot
    constexpr float ANGLE_COST = 3.0f;       // yards of walk one radian further from the tank's flank is worth
    // turns from the boss -> tank line: the tank's flanks first, then behind the boss, last the tank's own side
    static float const offsets[] = {1.2f, -1.2f, 0.8f, -0.8f, 1.6f, -1.6f, 0.4f, -0.4f, 2.1f, -2.1f, 2.6f, -2.6f,
                                    static_cast<float>(M_PI)};

    Map* map = bot->GetMap();
    uint32 const phase = bot->GetPhaseMask();
    float const minDistance = CoSHelpers::EpochStackMinDistance(bot, boss);
    float const distances[] = {minDistance + 1.5f, minDistance + 3.0f};
    float const baseAngle = tank ? boss->GetAngle(tank) : boss->GetAngle(bot);
    // members split to both flanks by their guid, so the band does not bunch up on one spot
    float const side = (bot->GetGUID().GetCounter() & 1) ? 1.0f : -1.0f;
    float const bossX = boss->GetPositionX();
    float const bossY = boss->GetPositionY();
    float const bossZ = boss->GetPositionZ();

    float bestCost = std::numeric_limits<float>::max();
    bool found = false;
    for (float const distance : distances)
        for (float const offset : offsets)
        {
            float const angleCost = ANGLE_COST * std::fabs(offset);
            float const angle = Position::NormalizeOrientation(baseAngle + side * offset);
            float x = bossX + std::cos(angle) * distance;
            float y = bossY + std::sin(angle) * distance;
            float z = bossZ;
            // the walk is at least the straight line: no better than the best already found
            if (bot->GetExactDist2d(x, y) + angleCost >= bestCost)
                continue;

            bot->UpdateAllowedPositionZ(x, y, z);
            if (std::fabs(z - bossZ) > STACK_MAX_DZ)
                continue;
            if (!map->isInLineOfSight(x, y, z + EYE, bossX, bossY, bossZ + EYE, phase, LINEOFSIGHT_ALL_CHECKS,
                                      VMAP::ModelIgnoreFlags::Nothing))
                continue;

            PathGenerator path(bot);
            path.CalculatePath(x, y, z, false);
            if (path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
                continue;
            Movement::PointsArray const& points = path.GetPath();
            if (points.empty())
                continue;
            G3D::Vector3 const& end = points.back();
            if (std::hypot(end.x - x, end.y - y) > PATH_END_SLACK || std::fabs(end.z - z) > STACK_MAX_DZ)
                continue;

            float walk = 0.0f;
            for (size_t i = 1; i < points.size(); ++i)
                walk += (points[i] - points[i - 1]).length();
            float const cost = walk + angleCost;
            if (cost >= bestCost)
                continue;

            bestCost = cost;
            outX = x;
            outY = y;
            outZ = z;
            found = true;
        }

    return found;
}

bool EpochStackAction::Execute(Event /*event*/)
{
    constexpr uint32 SPOT_KEEP_MS = 3 * IN_MILLISECONDS;
    constexpr float SPOT_BOSS_MOVED = 3.0f;   // the boss walked this far: a new spot
    constexpr float SPOT_REACHED = 1.5f;
    constexpr uint32 FAIL_LOG_MS = 10 * IN_MILLISECONDS;

    Unit* boss = AI_VALUE2(Unit*, "find target", "chrono-lord epoch");
    if (!boss || !boss->IsAlive() || boss->GetMap() != bot->GetMap())
        return false;

    Unit* tank = AI_VALUE(Unit*, "main tank");
    if (!tank || !tank->IsAlive() || tank->GetMap() != bot->GetMap())
        tank = boss->GetVictim();
    if (tank == bot)
        tank = nullptr;

    uint32 const now = getMSTime();
    bool const keep = _spotMs && getMSTimeDiff(_spotMs, now) < SPOT_KEEP_MS &&
                      boss->GetExactDist2d(_spotBossX, _spotBossY) <= SPOT_BOSS_MOVED;
    // At the spot and still not stacked (it sees the boss from a little higher than the bot): look again, and the
    // same spot found again is left for the fallback below.
    bool const reachedUnstacked = keep && bot->GetExactDist2d(_spotX, _spotY) <= SPOT_REACHED;
    if (!keep || reachedUnstacked)
    {
        _spotMs = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (FindStackSpot(boss, tank, x, y, z) &&
            !(reachedUnstacked && std::hypot(x - _spotX, y - _spotY) <= SPOT_REACHED))
        {
            _spotMs = now ? now : 1;
            _spotX = x;
            _spotY = y;
            _spotZ = z;
            _spotBossX = boss->GetPositionX();
            _spotBossY = boss->GetPositionY();
        }
    }

    if (_spotMs)
    {
        NoteMoveReason("epoch stack: beside the tank, outside the boss' reach, in his sight");
        if (MoveTo(bot->GetMapId(), _spotX, _spotY, _spotZ, false, false, false, false,
                   MovementPriority::MOVEMENT_COMBAT))
            return true;
        // the same walk already under way (a duplicate order is refused): keep walking it, no cast stops it
        if (bot->isMoving())
            return true;
    }

    // No spot that sees the boss and has a path (or the walk failed): never stand idle out of sight. The navmesh
    // path towards the boss up to its first point in sight within the band, else the path to the tank.
    float const maxDistance = CoSHelpers::EpochStackMaxDistance(bot, boss);
    float const minDistance = CoSHelpers::EpochStackMinDistance(bot, boss);
    bool const sight = bot->IsWithinLOSInMap(boss);
    bool moved = !sight && MoveToSight(boss, maxDistance, "epoch stack: into the boss' sight");
    // in his sight but inside his reach: straight out to the band, not to the tank
    float const distance = bot->GetExactDist2d(boss);
    if (!moved && sight && distance < minDistance)
        moved = MoveAway(boss, minDistance + 1.5f - distance);
    Unit* walkTo = tank ? tank : boss;
    if (!moved && !(sight && distance < minDistance))
    {
        NoteMoveReason("epoch stack: no stack spot, to the tank");
        moved = MoveTo(bot->GetMapId(), walkTo->GetPositionX(), walkTo->GetPositionY(), walkTo->GetPositionZ(),
                       false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
    }

    if (!_failLogMs || getMSTimeDiff(_failLogMs, now) >= FAIL_LOG_MS)
    {
        _failLogMs = now ? now : 1;
        LOG_INFO("playerbots",
                 "[b44_cos-epoch-stack] {}: stack spot {}, {:.1f} yd from Epoch, sight {}, fallback (sight walk, "
                 "else to ({:.1f},{:.1f},{:.1f})) {}",
                 bot->GetName(), _spotMs ? "walk refused" : "none found", distance, sight ? "yes" : "no",
                 walkTo->GetPositionX(), walkTo->GetPositionY(),
                 walkTo->GetPositionZ(), moved ? "walks" : "FAILED");
    }
    return moved;
}

bool CoSGetArcaneDisruptorAction::Execute(Event /*event*/)
{
    Creature* chromie = bot->FindNearestCreature(NPC_CHROMIE_START, CHROMIE_SEARCH_RANGE);
    if (!chromie || !chromie->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
        return false;

    if (!bot->IsWithinDistInMap(chromie, NPC_TALK_DISTANCE))
        return MoveNear(chromie, 2.0f);

    bot->StopMoving();
    // npc_cos_chromie_start::OnGossipSelect casts Summon Arcane Disruptor when page 9595 is answered.
    bool const talked =
        SelectStoryPages(bot, chromie,
                         {GOSSIP_MENU_CHROMIE_START, GOSSIP_MENU_CHROMIE_DECISION, GOSSIP_MENU_CHROMIE_INTERFERE},
                         GOSSIP_MENU_CHROMIE_INTERFERE);
    if (!bot->HasItemCount(ITEM_ARCANE_DISRUPTOR, 1))
    {
        _failedTalkMs = getMSTime();
        LOG_INFO("playerbots", "Culling: {} talked to Chromie (story {}), no Arcane Disruptor yet", bot->GetName(),
                 talked ? "done" : "cut short");
    }
    return talked;
}

bool CoSGetArcaneDisruptorAction::isUseful()
{
    return !_failedTalkMs || getMSTimeDiff(_failedTalkMs, getMSTime()) >= CHROMIE_RETRY_MS;
}

bool CoSDispelCrateAction::Execute(Event /*event*/)
{
    // A dispelled crate is deleted and replaced by a Plagued Crate, so every Suspicious Crate left needs the
    // Disruptor; instances keep all grids loaded, the spawn store holds all of them.
    GameObject* crate = nullptr;
    for (auto const& [spawnId, go] : bot->GetMap()->GetGameObjectBySpawnIdStore())
    {
        if (!go || go->GetEntry() != GO_SUSPICIOUS_CRATE || !go->isSpawned())
            continue;

        if (!crate || bot->GetExactDist(go) < bot->GetExactDist(crate))
            crate = go;
    }

    if (!crate)
        return false;

    // The crates stand in the city, hundreds of yards from Chromie: MoveNear only accepts spots in line of sight of
    // the bot and fails from that far, so walk the navmesh path first.
    if (!bot->IsWithinDistInMap(crate, CRATE_APPROACH_DISTANCE))
        return MoveTo(bot->GetMapId(), crate->GetPositionX(), crate->GetPositionY(), crate->GetPositionZ(), false,
                      false, false, false, MovementPriority::MOVEMENT_NORMAL);

    if (!bot->IsWithinDistInMap(crate, CRATE_USE_DISTANCE))
        return MoveNear(crate, 2.0f);

    Item* disruptor = bot->GetItemByEntry(ITEM_ARCANE_DISRUPTOR);
    if (!disruptor || bot->CanUseItem(disruptor) != EQUIP_ERR_OK || bot->IsNonMeleeSpellCast(false))
        return false;

    ItemTemplate const* proto = disruptor->GetTemplate();
    if (!proto || !proto->Spells[0].SpellId || bot->HasSpellCooldown(proto->Spells[0].SpellId))
        return false;

    // A player gets off the mount before using the item (the cast fails with SPELL_FAILED_NOT_MOUNTED otherwise,
    // wave 10: the action answered OK every tick while nothing was cast).
    if (bot->IsMounted())
    {
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        return true;
    }

    bot->StopMoving();

    // CMSG_USE_ITEM without a target, as the client sends it: Arcane Disruption picks the crate helper itself.
    uint8 const bagIndex = disruptor->GetBagSlot();
    uint8 const slot = disruptor->GetSlot();
    uint8 const castCount = 1;
    uint32 const spellId = proto->Spells[0].SpellId;
    uint32 const glyphIndex = 0;
    uint8 const castFlags = 0;
    WorldPacket packet(CMSG_USE_ITEM);
    packet << bagIndex << slot << castCount << spellId << disruptor->GetGUID() << glyphIndex << castFlags;
    packet << uint32(TARGET_FLAG_NONE);
    bot->GetSession()->HandleUseItemOpcode(packet);
    // Only a cast that started (or put the item on cooldown) counts; a refused cast lets other actions run.
    return bot->IsNonMeleeSpellCast(false) || bot->HasSpellCooldown(spellId);
}

bool CoSStartArthasIntroAction::Execute(Event /*event*/)
{
    // Chromie appears 20 s after the fifth crate (instance UpdateEventState); wait for her at her spot.
    Creature* chromie = bot->FindNearestCreature(NPC_CHROMIE_MIDDLE, CHROMIE_SEARCH_RANGE);
    if (!chromie || !chromie->IsVisible() || !chromie->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
    {
        if (bot->GetExactDist(CHROMIE_MIDDLE_POS) < 10.0f)
            return false;

        return MoveTo(bot->GetMapId(), CHROMIE_MIDDLE_POS.GetPositionX(), CHROMIE_MIDDLE_POS.GetPositionY(),
                      CHROMIE_MIDDLE_POS.GetPositionZ());
    }

    if (!bot->IsWithinDistInMap(chromie, NPC_TALK_DISTANCE))
        return MoveNear(chromie, 2.0f);

    bot->StopMoving();
    // npc_cos_chromie_middle::OnGossipSelect starts the intro when page 9612 is answered.
    return SelectStoryPages(bot, chromie,
                            {GOSSIP_MENU_CHROMIE_MIDDLE, GOSSIP_MENU_CHROMIE_MIDDLE_2, GOSSIP_MENU_CHROMIE_MIDDLE_3},
                            GOSSIP_MENU_CHROMIE_MIDDLE_3);
}

bool CoSArthasGossipAction::Execute(Event /*event*/)
{
    Creature* arthas = CoSHelpers::GetArthas(bot);
    if (!arthas || !arthas->IsAlive() || !arthas->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
        return false;

    if (!bot->IsWithinDistInMap(arthas, NPC_TALK_DISTANCE))
        return MoveNear(arthas, 2.0f);

    bot->StopMoving();
    if (!DungeonInteract::OpenGossip(bot, arthas))
        return false;

    // npc_arthas lists one option per event step; the Town Hall step opens one more page first.
    auto const isArthasStep = [](GossipMenuItem const& item)
    {
        return item.OptionType >= GOSSIP_ACTION_ARTHAS_FIRST && item.OptionType <= GOSSIP_ACTION_ARTHAS_LAST;
    };

    bool selected = false;
    for (uint8 page = 0; page < 2; ++page)
    {
        int32 const option = DungeonInteract::FindGossipOption(bot, isArthasStep);
        if (option < 0)
            break;

        GossipMenuItem const* item = bot->PlayerTalkClass->GetGossipMenu().GetItem(static_cast<uint32>(option));
        bool const opensPage = item && item->OptionType == GOSSIP_ACTION_ARTHAS_TOWN_HALL_PAGE;
        if (!DungeonInteract::SelectGossipOption(bot, arthas, option))
            break;

        selected = !opensPage;
        if (selected)
            break;
    }

    bot->PlayerTalkClass->SendCloseGossip();
    return selected;
}

bool CoSEngageEventTargetAction::Execute(Event /*event*/)
{
    constexpr float TANK_PULL_RANGE = 30.0f;
    constexpr float FOLLOW_UP_DISTANCE = 15.0f;
    constexpr float CORRUPTOR_PATH_RANGE = 40.0f;

    // Wave mobs stand where they spawn (no movement script, AC #21766): the group goes to them.
    bool waveSpot = false;  // [b33_cos2] the target is a wave's (MovementAction::HealerLeash for the tank's walk)
    Unit* target = CoSHelpers::GetCorruptorDetourTarget(bot);
    if (target)
    {
        // The Corruptor is a few hundred yards of streets away (from Arthas' last-city stand); MoveNear only takes
        // spots in sight of the bot, so walk the navmesh path until it is close and in sight.
        if (!bot->IsWithinDistInMap(target, CORRUPTOR_PATH_RANGE) || !bot->IsWithinLOSInMap(target))
        {
            float goX = target->GetPositionX();
            float goY = target->GetPositionY();
            float goZ = target->GetPositionZ();
            RerouteAroundFestivalGate(bot, getName().c_str(), goX, goY, goZ);  // [b35_cos4]
            return MoveTo(bot->GetMapId(), goX, goY, goZ, false, false, false, false,
                          MovementPriority::MOVEMENT_NORMAL);
        }
    }
    else
    {
        // What fights Arthas first: his escort stands while he fights.
        target = CoSHelpers::GetArthasAttacker(bot);
        if (target && PlayerbotAI::IsTank(bot))
        {
            uint32 const now = getMSTime();
            if (!_defendLogMs || getMSTimeDiff(_defendLogMs, now) >= 10 * IN_MILLISECONDS)
            {
                _defendLogMs = now ? now : 1;
                LOG_INFO("playerbots", "[cos-defend-arthas] {}: takes {} ({}) off Arthas, {:.0f} yd (progress {})",
                         bot->GetName(), target->GetName(), target->GetEntry(), bot->GetExactDist(target),
                         CoSHelpers::GetProgress(bot));
            }
        }
        if (!target)
        {
            target = CoSHelpers::GetWaveTarget(bot, !PlayerbotAI::IsTank(bot));  // [b44_cos] as the trigger
            waveSpot = target != nullptr;  // [b33_cos2]
        }
    }
    if (!target)
        return false;

    if (!PlayerbotAI::IsTank(bot))
        return MoveNear(target, FOLLOW_UP_DISTANCE);

    // In range and in sight: pull. Otherwise walk the navmesh path on (cos-h 20261025-163739: the wave-7 Patchwork
    // Construct at 2145,1355 stood 29.8 yd away, 3.4 yd higher; Attack refused every tick (no current target, so
    // most likely line of sight), the action returned false and the tank stood 2 minutes until the stall).
    // [b33_cos2] A wave is gone for with the healer: the walk stays within the healer's range (HealerLeash cuts it),
    // no pull while the healer is farther behind (cos-h 20261026-000434-cos-h-5H-22: the wave pulled 153+ yd ahead of
    // the drinking / fighting priest, 4 deaths). Out of its range the tank walks back to it; at the edge it stands
    // (the action holds it: the harness route's "test runner move" would carry it on), at most LEASH_HOLD_MAX_MS.
    constexpr uint32 LEASH_HOLD_MAX_MS = 30 * IN_MILLISECONDS;
    float walkX = target->GetPositionX();
    float walkY = target->GetPositionY();
    float walkZ = target->GetPositionZ();
    HealerLeashStep const leash = waveSpot ? HealerLeash(walkX, walkY, walkZ) : HealerLeashStep::Walk;
    if (leash != HealerLeashStep::Hold)
        _leashHoldSinceMs = 0;
    if (leash == HealerLeashStep::Hold)
    {
        uint32 const now = getMSTime();
        if (!_leashHoldSinceMs)
            _leashHoldSinceMs = now ? now : 1;
        return getMSTimeDiff(_leashHoldSinceMs, now) < LEASH_HOLD_MAX_MS;
    }
    bool const leashCut = waveSpot && (walkX != target->GetPositionX() || walkY != target->GetPositionY() ||
                                       walkZ != target->GetPositionZ());
    if (leashCut)
        return MoveTo(bot->GetMapId(), walkX, walkY, walkZ, false, false, false, false,
                      MovementPriority::MOVEMENT_NORMAL);

    bool const inSight = bot->IsWithinLOSInMap(target);
    if (inSight && bot->IsWithinDistInMap(target, TANK_PULL_RANGE))
    {
        if (AI_VALUE(Unit*, "current target") == target)
            return false;
        if (Attack(target))
            return true;
    }

    if (!inSight || !bot->IsWithinDistInMap(target, CORRUPTOR_PATH_RANGE))
    {
        float goX = target->GetPositionX();
        float goY = target->GetPositionY();
        float goZ = target->GetPositionZ();
        RerouteAroundFestivalGate(bot, getName().c_str(), goX, goY, goZ);  // [b35_cos4]
        return MoveTo(bot->GetMapId(), goX, goY, goZ, false, false, false, false, MovementPriority::MOVEMENT_NORMAL);
    }

    return MoveNear(target, 5.0f);
}

bool CoSFollowArthasAction::Execute(Event /*event*/)
{
    constexpr float ARTHAS_PATH_RANGE = 40.0f;
    constexpr uint32 REGROUP_LOG_INTERVAL_MS = 10 * IN_MILLISECONDS;

    Creature* arthas = CoSHelpers::GetArthas(bot);
    if (!arthas || !arthas->IsAlive())
        return false;

    // Far behind (the heroic Corruptor detour ends ~135 yd from his Mal'Ganis stand): MoveNear only takes spots in
    // sight of the bot, so walk the navmesh path first.
    if (!bot->IsWithinDistInMap(arthas, ARTHAS_PATH_RANGE) || !bot->IsWithinLOSInMap(arthas))
    {
        uint32 const now = getMSTime();
        if (!arthas->isMoving() && (!_regroupLogMs || getMSTimeDiff(_regroupLogMs, now) >= REGROUP_LOG_INTERVAL_MS))
        {
            _regroupLogMs = now;
            LOG_INFO("playerbots", "[cos-arthas-regroup] {}: walks back to the waiting Arthas, {:.0f} yd (progress {})",
                     bot->GetName(), bot->GetDistance(arthas), CoSHelpers::GetProgress(bot));
        }
        // [obstacle_sidestep] the Hallow's End pumpkins on the city road (cos-h 20261025-153805: three members held
        // at 1918.4,1301.1 by G_Pumpkin 68303, "MoveTo starts no move" for two minutes).
        float goX = arthas->GetPositionX();
        float goY = arthas->GetPositionY();
        float goZ = arthas->GetPositionZ();
        RerouteAroundFestivalGate(bot, getName().c_str(), goX, goY, goZ);  // [b35_cos4]
        if (SidestepObstacle(bot->GetMapId(), goX, goY, goZ))
            return true;
        return MoveTo(bot->GetMapId(), goX, goY, goZ, false, false, false, false, MovementPriority::MOVEMENT_NORMAL);
    }

    return MoveNear(arthas, 6.0f);
}
