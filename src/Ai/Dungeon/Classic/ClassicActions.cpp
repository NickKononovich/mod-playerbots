/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ClassicActions.h"
#include "ClassicShared.h"
#include "DungeonInteract.h"
#include "EscortProtect.h"  // [b36_ohb] EscortGossipHeld
#include "GameObject.h"
#include "Group.h"
#include "InstanceScript.h"
#include "LastMovementValue.h"  // [b33_ohb]
#include "Map.h"
#include "MotionMaster.h"  // [b33_ohb]
#include "MoveSpline.h"    // [b33_ohb]
#include "PathGenerator.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TargetValue.h"
#include "Timer.h"
#include "UseItemAction.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <list>
#include <utility>
#include <vector>

using namespace ScarletMonastery;

namespace
{
constexpr std::array<float, 4> ESCAPE_RINGS = {14.0f, 20.0f, 26.0f, 35.0f};
constexpr uint32 ESCAPE_DIRECTIONS = 16;
constexpr uint32 ESCAPE_MAX_PATHS = 8;
constexpr uint32 ESCAPE_SPOT_LIFETIME_MS = 6000;  // the 5 s cast plus a margin
constexpr float ESCAPE_ARRIVED = 1.5f;
constexpr float ESCAPE_MAX_DZ = 6.0f;

float PathLength(Movement::PointsArray const& points)
{
    float length = 0.0f;
    for (size_t i = 1; i < points.size(); ++i)
        length += (points[i] - points[i - 1]).length();

    return length;
}
}  // namespace

bool DoanEscapeDetonationAction::ChooseSpot(Unit* doan)
{
    std::vector<std::pair<float, std::array<float, 3>>> candidates;  // straight distance, (x, y, z)
    for (float ring : ESCAPE_RINGS)
    {
        for (uint32 i = 0; i < ESCAPE_DIRECTIONS; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * i / ESCAPE_DIRECTIONS;
            float const x = doan->GetPositionX() + std::cos(angle) * ring;
            float const y = doan->GetPositionY() + std::sin(angle) * ring;
            float const z = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, doan->GetPositionZ() + ESCAPE_MAX_DZ);
            if (z <= INVALID_HEIGHT || std::fabs(z - doan->GetPositionZ()) > ESCAPE_MAX_DZ)
                continue;

            if (!IsSafeFromDetonation(bot, doan, x, y, z))
                continue;

            candidates.push_back({bot->GetExactDist2d(x, y), {x, y, z}});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](auto const& left, auto const& right) { return left.first < right.first; });

    float bestLength = 0.0f;
    bool found = false;
    uint32 paths = 0;
    for (auto const& candidate : candidates)
    {
        if (++paths > ESCAPE_MAX_PATHS || (found && candidate.first >= bestLength))
            break;

        PathGenerator path(bot);
        path.CalculatePath(candidate.second[0], candidate.second[1], candidate.second[2], false);
        if (path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
            continue;

        float const length = PathLength(path.GetPath());
        if (!found || length < bestLength)
        {
            found = true;
            bestLength = length;
            _spotX = candidate.second[0];
            _spotY = candidate.second[1];
            _spotZ = candidate.second[2];
        }
    }

    if (found)
        _spotExpiresMs = getMSTime() + ESCAPE_SPOT_LIFETIME_MS;

    return found;
}

bool DoanEscapeDetonationAction::Execute(Event /*event*/)
{
    Unit* doan = FindDoan(botAI);
    if (!IsDetonationPending(doan))
        return false;

    if (getMSTime() > _spotExpiresMs && !ChooseSpot(doan))
        return LeaveCircle(doan->GetPosition(), DETONATION_RADIUS + bot->GetCombatReach(), true);

    if (bot->GetExactDist2d(_spotX, _spotY) < ESCAPE_ARRIVED)
        return false;

    bot->CastStop();
    return MoveTo(bot->GetMapId(), _spotX, _spotY, _spotZ, false, false, false, false,
                  MovementPriority::MOVEMENT_FORCED, true);
}

bool DoanKeepRangeAction::Execute(Event /*event*/)
{
    Unit* doan = FindDoan(botAI);
    if (!doan)
        return false;

    return LeaveCircle(doan->GetPosition(), ARCANE_EXPLOSION_RADIUS + bot->GetCombatReach(), false);
}

namespace
{
constexpr std::array<float, 4> MANA_BURN_STEP_RINGS = {5.0f, 9.0f, 13.0f, 17.0f};
constexpr uint32 MANA_BURN_STEP_DIRECTIONS = 16;
constexpr float MANA_BURN_SAFE_MARGIN = 3.0f;      // beyond the pick, as both mobs and bots move
constexpr float MANA_BURN_HEALER_TANK_MAX = 30.0f;  // inside the 33 yd healer leash of the ranged spot
constexpr float MANA_BURN_RANGED_TARGET_MAX = 28.0f;
constexpr float MANA_BURN_MELEE_GAP = 6.0f;         // never into a mob's melee
constexpr float MANA_BURN_IDLE_MARGIN = 3.0f;
constexpr float MANA_BURN_MAX_DZ = 6.0f;
constexpr float MANA_BURN_IDLE_SCAN = 45.0f;
constexpr float RESEARCHER_PENALTY = 8.0f;          // yards of walk worth leaving a Researcher's 30 yd
constexpr uint32 MANA_BURN_SPOT_LIFETIME_MS = 3000;
constexpr float MANA_BURN_ARRIVED = 1.5f;

Player* FindGroupTank(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    ObjectGuid const mainTank = PlayerbotAI::GetMainTankGuid(group);
    Player* fallback = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap())
            continue;

        if (member->GetGUID() == mainTank)
            return member;

        if (!fallback && PlayerbotAI::IsTank(member))
            fallback = member;
    }

    return fallback;
}
}  // namespace

bool ScholoAvoidManaBurnAction::SpotIsClear(float x, float y) const
{
    GuidVector const attackers = AI_VALUE(GuidVector, "attackers");
    for (ObjectGuid const& guid : attackers)
    {
        if (guid.GetEntry() != Scholomance::NPC_SCHOLOMANCE_ACOLYTE)
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || unit->GetVictim() == bot || IsCrowdControlled(unit))
            continue;

        float const reach = Scholomance::ACOLYTE_MANA_BURN_REACH + unit->GetCombatReach() + bot->GetCombatReach();
        if (unit->GetExactDist2d(x, y) <= reach + MANA_BURN_SAFE_MARGIN)
            return false;
    }

    return true;
}

bool ScholoAvoidManaBurnAction::ChooseSpot()
{
    Map* map = bot->GetMap();
    Player* tank = FindGroupTank(bot);
    bool const healer = PlayerbotAI::IsHeal(bot);
    Unit* target = AI_VALUE(Unit*, "current target");
    WorldObject const* anchor = healer ? static_cast<WorldObject const*>(tank) : target;
    if (!anchor && tank)
        anchor = tank;
    if (!map || !anchor)
        return false;

    float const anchorMax = healer ? MANA_BURN_HEALER_TANK_MAX : MANA_BURN_RANGED_TARGET_MAX;

    std::vector<Unit*> enemies;
    std::vector<Unit*> researchers;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "attackers"))
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive())
            continue;

        enemies.push_back(unit);
        if (guid.GetEntry() == Scholomance::NPC_SPECTRAL_RESEARCHER)
            researchers.push_back(unit);
    }

    // [idle_pack_no_los] Every idle mob in sight range, seen or not: scholo-h w25 #1 (20261025-132016-scholo-h-5A-29)
    // the healer's steps took it 28-30 yd from the tank by the room's next packs, which "possible targets" (the
    // bot's line of sight only) did not hold; the fight grew from 3 to 19.
    std::vector<Creature*> idle;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "possible targets no los"))
    {
        Creature* mob = botAI->GetCreature(guid);
        if (mob && mob->IsAlive() && !mob->IsInCombat() && mob->HasReactState(REACT_AGGRESSIVE) &&
            bot->IsHostileTo(mob) && bot->GetExactDist2d(mob) <= MANA_BURN_IDLE_SCAN)
            idle.push_back(mob);
    }

    bool found = false;
    float bestCost = 0.0f;
    for (float const ring : MANA_BURN_STEP_RINGS)
    {
        for (uint32 i = 0; i < MANA_BURN_STEP_DIRECTIONS; ++i)
        {
            float const angle = 2.0f * float(M_PI) * float(i) / float(MANA_BURN_STEP_DIRECTIONS);
            float x = bot->GetPositionX() + std::cos(angle) * ring;
            float y = bot->GetPositionY() + std::sin(angle) * ring;
            float z = bot->GetPositionZ();
            if (!map->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                      bot->GetPositionZ(), x, y, z, false))
                continue;

            if (std::fabs(z - bot->GetPositionZ()) > MANA_BURN_MAX_DZ || !SpotIsClear(x, y))
                continue;

            if (anchor->GetExactDist(x, y, z) > anchorMax || !anchor->IsWithinLOS(x, y, z))
                continue;

            bool blocked = false;
            for (Unit const* enemy : enemies)
            {
                if (enemy->GetVictim() != bot &&
                    enemy->GetExactDist2d(x, y) - enemy->GetCombatReach() <= MANA_BURN_MELEE_GAP)
                {
                    blocked = true;
                    break;
                }
            }

            for (Creature* mob : idle)
            {
                if (blocked)
                    break;

                // the core's proximity aggro (its Z gate, aggro range + combat distance in 3D), no line of sight:
                // the fight drifts round the corner
                if (!MovementAction::IdleAggroBlocker(mob, bot, x, y, z, MANA_BURN_IDLE_MARGIN, false))
                    blocked = true;
            }

            if (blocked)
                continue;

            float cost = bot->GetExactDist2d(x, y);
            for (Unit const* researcher : researchers)
            {
                float const reach = Scholomance::RESEARCHER_REACH + researcher->GetCombatReach() +
                                    bot->GetCombatReach();
                if (researcher->GetExactDist2d(x, y) <= reach)
                {
                    cost += RESEARCHER_PENALTY;
                    break;
                }
            }

            if (!found || cost < bestCost)
            {
                found = true;
                bestCost = cost;
                _spotX = x;
                _spotY = y;
                _spotZ = z;
            }
        }
    }

    _spotExpiresMs = found ? getMSTime() + MANA_BURN_SPOT_LIFETIME_MS : 0;
    return found;
}

bool ScholoAvoidManaBurnAction::isUseful()
{
    // a heal or a nuke under way finishes first; the step comes between casts
    return !bot->IsNonMeleeSpellCast(false, false, true) && Scholomance::ManaBurnThreat(botAI) != nullptr;
}

bool ScholoAvoidManaBurnAction::Execute(Event /*event*/)
{
    bool keep = _spotExpiresMs && getMSTime() < _spotExpiresMs && SpotIsClear(_spotX, _spotY);
    // [idle_pack_no_los] (PATCH_queue_encounters.md item 7) the spot kept up to MANA_BURN_SPOT_LIFETIME_MS is
    // dropped too once an idle pack's aggro reaches it, seen or not; ChooseSpot applies the same test to new spots.
    if (keep)
    {
        for (ObjectGuid const& guid : AI_VALUE(GuidVector, "possible targets no los"))
        {
            Creature* mob = botAI->GetCreature(guid);
            if (mob && mob->IsAlive() && !mob->IsInCombat() && mob->HasReactState(REACT_AGGRESSIVE) &&
                bot->IsHostileTo(mob) && bot->GetExactDist2d(mob) <= MANA_BURN_IDLE_SCAN &&
                !MovementAction::IdleAggroBlocker(mob, bot, _spotX, _spotY, _spotZ, MANA_BURN_IDLE_MARGIN, false))
            {
                keep = false;
                break;
            }
        }
    }

    if (!keep && !ChooseSpot())
        return false;

    if (bot->GetExactDist2d(_spotX, _spotY) <= MANA_BURN_ARRIVED)
        return false;

    return MoveTo(bot->GetMapId(), _spotX, _spotY, _spotZ, false, false, false, false,
                  MovementPriority::MOVEMENT_COMBAT, true);
}

namespace DungeonEvents
{
namespace
{
enum DungeonEventIds : uint32
{
    MAP_GNOMEREGAN = 90,
    MAP_SUNKEN_TEMPLE = 109,
    MAP_ZULFARRAK = 209,
    MAP_BLACKROCK_SPIRE = 229,
    MAP_OLD_HILLSBRAD = 560,

    // Gnomeregan: Blastmaster Emi Shortfuse (SmartAI 7998, gossip menu 1080 option 0 "I am ready to begin.") starts
    // her escort; its last wave is followed by Grubbis 7361. No quest or item.
    NPC_EMI_SHORTFUSE = 7998,
    NPC_GRUBBIS = 7361,

    // Sunken Temple (instance_sunken_temple.cpp, SmartGameObjectAI + conditions 22/1): the six Atal'ai Statues
    // 148830-148835 must be used in ascending entry order; statue N only works while GetData(DATA_STATUES) == N - 1,
    // a wrong one hits its user with a trap spell. At phase 6 the Idol of Hakkar 148838 appears. Lock 93 needs no key.
    GO_ATALAI_STATUE_1 = 148830,
    DATA_STATUES = 10,
    STATUE_COUNT = 6,

    // Zul'Farrak (zulfarrak.cpp go_troll_cage): using a troll cage while DATA_PYRAMID == 0 frees Sergeant Bly's
    // crew and starts the pyramid waves (Sezz'ziz and Nekrum come in the third). The cages are lock 420: Executioner's
    // Key 8444 (Sandfury Executioner 7274). Weegli (7607, gossip at phase >= 8) blows up the door to Chief Ukorz.
    GO_TROLL_CAGE = 141073,
    ITEM_EXECUTIONERS_KEY = 8444,
    DATA_PYRAMID = 0,
    DATA_END_DOOR = 2,
    PYRAMID_KILLED_ALL_TROLLS = 8,
    NPC_WEEGLI = 7607,
    // The Gong of Zul'Farrak 141832 (SmartAI, GossipHello) summons Gahz'rilla 7273 once. Lock 99 has no key: the
    // server does not ask for the Mallet of Zul'Farrak 9240, a player needs it, so the bot rings it only with one.
    GO_GONG_OF_ZULFARRAK = 141832,
    ITEM_MALLET_OF_ZULFARRAK = 9240,
    NPC_GAHZRILLA = 7273,
    DATA_GAHZRILLA = 1,

    // Blackrock Spire: the Father Flame 175245 (go_father_flame, a chest with lock 93) starts the Solakar Flamewreath
    // event (instance_blackrock_spire.cpp, DATA_SOLAKAR_FLAMEWREATH 23: five Guardian + Hatcher waves, then Solakar).
    GO_FATHER_FLAME = 175245,
    // Pyroguard Emberseer starts from the Blackrock Altar 175706 in his room (summoning ritual, 1 participant: the
    // user channels 16532, then 16533 wakes the Blackhand Incarcerators and him, boss_pyroguard_emberseer.cpp).
    GO_BLACKROCK_ALTAR = 175706,
    DATA_SOLAKAR_FLAMEWREATH = 23,
    DATA_PYROGUARD_EMBERSEER = 9,  // blackrock_spire.h DATA_PYROGAURD_EMBERSEER, a boss state
    DATA_DRAGONSPIRE_ROOM = 15,    // blackrock_spire.h: DONE once the seven hall runes are out; opens Emberseer In

    // Old Hillsbrad: Erozion 18723 (gossip 7769 option 0) gives the Pack of Incendiary Bombs 25853; Planting
    // Incendiary Bomb 32744 is cast from that item on each of the five Durnholde barrels 182589 (lock 1682 = the
    // item, the barrel's SmartAI counts the spell hit); DATA_ESCORT_PROGRESS 0 turns 1 after the fifth. Thrall 17876
    // (gossip 7499 at progress 1, then at each stop) leads the escape.
    NPC_EROZION = 18723,
    ITEM_INCENDIARY_BOMBS = 25853,
    GO_DURNHOLDE_BARREL = 182589,
    NPC_THRALL = 17876,
    DATA_ESCORT_PROGRESS = 0,
    ESCORT_BARRELS_DONE = 1,
    ESCORT_FINISHED = 9,
};

constexpr float STEP_SCAN_RANGE = 80.0f;
constexpr float GATHER_FIGHT = 30.0f;

using StepDue = bool (*)(Player* bot, InstanceScript* instance, uint32 entry);

bool NotFoundNearby(Player* bot, uint32 entry)
{
    return !bot->FindNearestCreature(entry, STEP_SCAN_RANGE * 2.0f, true);
}

// After Emberseer, as the dungeon's order has it (wave 13: lit every time the group passed the Rookery out of combat,
// ten times; each failed attempt reset the event, 434 Rookery Whelps, and the leader's own walk to the flame outranked
// the route to Emberseer's altar, 0/5 in 25 minutes).
bool BrsFatherFlameDue(Player* /*bot*/, InstanceScript* instance, uint32 /*entry*/)
{
    return instance->GetBossState(DATA_PYROGUARD_EMBERSEER) == DONE &&
           instance->GetData(DATA_SOLAKAR_FLAMEWREATH) == NOT_STARTED;
}

// Upper Blackrock Spire, the same map as the lower wing (TestHarnessData.cpp door gate): the Dragonspire Hall and
// Emberseer's room, and above z 85 the Rookery, Drakkisath's hall, the stadium and The Beast.
bool InUpperSpire(Player* bot)
{
    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const z = bot->GetPositionZ();
    bool const hall = x >= 97.0f && x <= 240.0f && y >= -445.0f && y <= -228.0f && z >= 63.0f && z <= 135.0f;
    bool const upper = x >= 30.0f && x <= 240.0f && y >= -600.0f && y <= -228.0f && z >= 85.0f && z <= 140.0f;
    return hall || upper;
}

// UBRS wave 15: the ptgt pass made the Father Flame wait for Emberseer, but nothing used his altar (0/5). His room
// opens (Emberseer In) only once the Dragonspire Hall runes are out: before that the step held the tank at the
// closed door (ubrs-n / lbrs-n / brs-h 2026-10-25, "[closed door] dungeon event step ... -> stay"), and in the
// lower wing it is none of the group's business.
bool BrsAltarDue(Player* bot, InstanceScript* instance, uint32 /*entry*/)
{
    return InUpperSpire(bot) && instance->GetBossState(DATA_DRAGONSPIRE_ROOM) == DONE &&
           instance->GetBossState(DATA_PYROGUARD_EMBERSEER) == NOT_STARTED;
}

bool StStatueDue(Player* /*bot*/, InstanceScript* instance, uint32 entry)
{
    return instance->GetData(DATA_STATUES) == entry - GO_ATALAI_STATUE_1;
}

bool ZfCageDue(Player* /*bot*/, InstanceScript* instance, uint32 /*entry*/)
{
    return instance->GetData(DATA_PYRAMID) == 0;
}

// [b32_zf] The pool round the gong and Gahz'rilla's fight spot (smart_scripts 7273 id 0: he walks to
// 1696.9,1216.6,8.9) holds ~20 Sandfury (Blood Drinkers flee for help at 15 %, Soul Eater / Witch Doctor patrols,
// Witch Doctors heal). The gong waits until none of them stands idle there - the same radius as the harness's
// [b30_mech_pos] starter clear, which this step bypassed: zf-h 20261025-221621-zf-h-5C-63 the tank carrying the
// Mallet rang the gong himself at 888 s between two pool pulls (starter_area_clear still on a Blood Drinker 29 yd
// away); Gahz'rilla (heroic 707273: 720 897 HP) was then fought with 6-8 Sandfury elites on the tank for 225 s, the
// Disc priest ran dry (0 % mana from ~200 s) and the group wiped. A person ringing the gong is not held by this.
bool ZfPoolHasIdleSandfury(Player* bot)
{
    constexpr float POOL_CLEAR_RADIUS = 45.0f;
    constexpr float POOL_CLEAR_DZ = 6.0f;
    Position const gahzSpot(1696.9f, 1216.6f, 8.9f);
    Position const gongSpot(1650.9f, 1171.9f, 10.9f);
    constexpr uint32 RESCAN_MS = 2 * IN_MILLISECONDS;
    Map* map = bot->GetMap();
    if (!map)
        return false;
    // The step's trigger asks every tick (only for the bot carrying the Mallet): the map scan runs every 2 s.
    struct PoolScan
    {
        uint32 instanceId = 0;
        uint32 atMs = 0;
        bool idle = false;
    };
    thread_local PoolScan cache;
    uint32 const now = getMSTime();
    if (cache.atMs && cache.instanceId == map->GetInstanceId() && getMSTimeDiff(cache.atMs, now) < RESCAN_MS)
        return cache.idle;
    cache.instanceId = map->GetInstanceId();
    cache.atMs = now ? now : 1;
    cache.idle = false;
    for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
    {
        if (!creature || !creature->IsAlive() || creature->IsInCombat() || creature->IsSummon() ||
            creature->IsDungeonBoss() || creature->IsCritter() || creature->GetEntry() == NPC_GAHZRILLA ||
            !creature->IsHostileTo(bot) || creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) ||
            creature->IsImmuneToPC() || !bot->IsValidAttackTarget(creature))
            continue;
        bool const nearSpot = creature->GetExactDist2d(&gahzSpot) <= POOL_CLEAR_RADIUS &&
                              std::fabs(creature->GetPositionZ() - gahzSpot.GetPositionZ()) <= POOL_CLEAR_DZ;
        bool const nearGong = creature->GetExactDist2d(&gongSpot) <= POOL_CLEAR_RADIUS &&
                              std::fabs(creature->GetPositionZ() - gongSpot.GetPositionZ()) <= POOL_CLEAR_DZ;
        if (nearSpot || nearGong)
        {
            cache.idle = true;
            break;
        }
    }
    return cache.idle;
}

bool ZfGongDue(Player* bot, InstanceScript* instance, uint32 /*entry*/)
{
    return instance->GetData(DATA_GAHZRILLA) == 0 && NotFoundNearby(bot, NPC_GAHZRILLA) &&
           !ZfPoolHasIdleSandfury(bot);  // [b32_zf]
}

bool ZfWeegliDue(Player* /*bot*/, InstanceScript* instance, uint32 /*entry*/)
{
    return instance->GetData(DATA_PYRAMID) >= PYRAMID_KILLED_ALL_TROLLS && instance->GetData(DATA_END_DOOR) == 0;
}

// Her escort starts at her spawn; once she walks or Grubbis is dead there is nothing to start.
bool GnomerEmiDue(Player* bot, InstanceScript* /*instance*/, uint32 /*entry*/)
{
    return bot->FindNearestCreature(NPC_GRUBBIS, 250.0f, true) != nullptr;
}

bool OhbBombsDue(Player* bot, InstanceScript* instance, uint32 /*entry*/)
{
    return instance->GetData(DATA_ESCORT_PROGRESS) == 0 && !bot->HasItemCount(ITEM_INCENDIARY_BOMBS, 1);
}

bool OhbBarrelDue(Player* /*bot*/, InstanceScript* instance, uint32 /*entry*/)
{
    return instance->GetData(DATA_ESCORT_PROGRESS) == 0;
}

bool OhbThrallDue(Player* bot, InstanceScript* instance, uint32 /*entry*/)
{
    uint32 const progress = instance->GetData(DATA_ESCORT_PROGRESS);
    if (progress < ESCORT_BARRELS_DONE || progress >= ESCORT_FINISHED)
        return false;

    // [b36_ohb] Clear ahead first, as players do: no word to a waiting Thrall while an idle pack stands on the next
    // stretch of his walk (EscortProtect::EscortGossipHeld; the planner pulls it meanwhile, capped per stop). From his
    // cell he walks the armory and the keep yard without a stop (waypoints 0-29, ambushes summoned on top).
    Creature* thrall = bot->FindNearestCreature(NPC_THRALL, STEP_SCAN_RANGE, true);
    return !(thrall && EscortProtect::EscortGossipHeld(bot, thrall));
}
}  // namespace

struct Step
{
    uint32 mapId;
    StepKind kind;
    uint32 entry;         // gameobject or creature
    uint32 requiredItem;  // the bot must carry it (0 = none)
    StepDue due;
    float gatherRadius;
    char const* what;
};

namespace
{
constexpr std::array<Step, 15> STEPS = {{
    {MAP_BLACKROCK_SPIRE, StepKind::UseObject, GO_BLACKROCK_ALTAR, 0, &BrsAltarDue, GATHER_FIGHT,
     "UBRS: Blackrock Altar (Pyroguard Emberseer)"},
    {MAP_BLACKROCK_SPIRE, StepKind::UseObject, GO_FATHER_FLAME, 0, &BrsFatherFlameDue, GATHER_FIGHT,
     "UBRS: Father Flame (Solakar Flamewreath)"},
    {MAP_SUNKEN_TEMPLE, StepKind::UseObject, GO_ATALAI_STATUE_1, 0, &StStatueDue, 0.0f, "Sunken Temple: statue 1"},
    {MAP_SUNKEN_TEMPLE, StepKind::UseObject, GO_ATALAI_STATUE_1 + 1, 0, &StStatueDue, 0.0f, "Sunken Temple: statue 2"},
    {MAP_SUNKEN_TEMPLE, StepKind::UseObject, GO_ATALAI_STATUE_1 + 2, 0, &StStatueDue, 0.0f, "Sunken Temple: statue 3"},
    {MAP_SUNKEN_TEMPLE, StepKind::UseObject, GO_ATALAI_STATUE_1 + 3, 0, &StStatueDue, 0.0f, "Sunken Temple: statue 4"},
    {MAP_SUNKEN_TEMPLE, StepKind::UseObject, GO_ATALAI_STATUE_1 + 4, 0, &StStatueDue, 0.0f, "Sunken Temple: statue 5"},
    {MAP_SUNKEN_TEMPLE, StepKind::UseObject, GO_ATALAI_STATUE_1 + 5, 0, &StStatueDue, 0.0f, "Sunken Temple: statue 6"},
    {MAP_ZULFARRAK, StepKind::UseObject, GO_TROLL_CAGE, ITEM_EXECUTIONERS_KEY, &ZfCageDue, GATHER_FIGHT,
     "Zul'Farrak: troll cage (pyramid event)"},
    {MAP_ZULFARRAK, StepKind::Gossip, NPC_WEEGLI, 0, &ZfWeegliDue, 0.0f, "Zul'Farrak: Weegli blows the door"},
    {MAP_ZULFARRAK, StepKind::UseObject, GO_GONG_OF_ZULFARRAK, ITEM_MALLET_OF_ZULFARRAK, &ZfGongDue, GATHER_FIGHT,
     "Zul'Farrak: gong (Gahz'rilla)"},
    {MAP_GNOMEREGAN, StepKind::Gossip, NPC_EMI_SHORTFUSE, 0, &GnomerEmiDue, GATHER_FIGHT,
     "Gnomeregan: Emi Shortfuse (Grubbis)"},
    {MAP_OLD_HILLSBRAD, StepKind::Gossip, NPC_EROZION, 0, &OhbBombsDue, 0.0f, "Old Hillsbrad: Erozion's bombs"},
    {MAP_OLD_HILLSBRAD, StepKind::UseItemOnObject, GO_DURNHOLDE_BARREL, ITEM_INCENDIARY_BOMBS, &OhbBarrelDue, 0.0f,
     "Old Hillsbrad: incendiary barrel"},
    {MAP_OLD_HILLSBRAD, StepKind::Gossip, NPC_THRALL, 0, &OhbThrallDue, GATHER_FIGHT, "Old Hillsbrad: Thrall"},
}};

// The nearest usable object. A planted barrel looks the same as the others (its SmartAI only counts the spell hit):
// the action remembers the ones it is done with (skip).
GameObject* FindReadyObject(Player* bot, uint32 entry, bool readyOnly, GuidSet const* skip)
{
    std::list<GameObject*> objects;
    bot->GetGameObjectListWithEntryInGrid(objects, entry, STEP_SCAN_RANGE);
    GameObject* best = nullptr;
    for (GameObject* object : objects)
    {
        if (!object->isSpawned() || object->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE) ||
            (readyOnly && object->GetGoState() != GO_STATE_READY) || (skip && skip->count(object->GetGUID())))
            continue;

        if (!best || bot->GetExactDist(object) < bot->GetExactDist(best))
            best = object;
    }
    return best;
}

// A gossip NPC that offers its menu now (the escort NPCs drop the flag while they walk).
Creature* FindGossipCreature(Player* bot, uint32 entry)
{
    Creature* creature = bot->FindNearestCreature(entry, STEP_SCAN_RANGE, true);
    return creature && !creature->IsInCombat() && creature->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP) ? creature : nullptr;
}
}  // namespace

Step const* NextStep(PlayerbotAI* botAI, WorldObject*& target, GuidSet const* skip)
{
    target = nullptr;
    Player* bot = botAI->GetBot();
    uint32 const mapId = bot->GetMapId();
    if (std::none_of(STEPS.begin(), STEPS.end(), [mapId](Step const& step) { return step.mapId == mapId; }))
        return nullptr;

    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || bot->IsInCombat())
        return nullptr;

    // A step that needs an item is done by whoever looted it, the others by the group's interactor; nobody acts while
    // a person drives a member (GetInteractor is nullptr then: that person starts the events).
    Player* groupInteractor = DungeonInteract::GetInteractor(botAI);
    if (!groupInteractor)
        return nullptr;

    bool const interactor = groupInteractor == bot;
    for (Step const& step : STEPS)
    {
        if (step.mapId != mapId || (step.requiredItem && !bot->HasItemCount(step.requiredItem, 1)) ||
            (!step.requiredItem && !interactor) || !step.due(bot, instance, step.entry))
            continue;

        if (step.kind == StepKind::Gossip)
            target = FindGossipCreature(bot, step.entry);
        else
            target = FindReadyObject(bot, step.entry, step.kind == StepKind::UseItemOnObject, skip);

        if (target)
            return &step;
    }
    return nullptr;
}

bool Perform(PlayerbotAI* botAI, Step const& step, WorldObject* target)
{
    Player* bot = botAI->GetBot();
    switch (step.kind)
    {
        case StepKind::UseObject:
            return DungeonInteract::UseGameObject(bot, target->ToGameObject());
        case StepKind::UseItemOnObject:
        {
            // As the client sends it, the object as the spell's target; the core makes every cast check. The shared
            // UseItemAction tests the spell on the bot itself first and refused Old Hillsbrad's bomb (wave 13: not
            // one barrel in 25 minutes).
            Item* item = bot->GetItemByEntry(step.requiredItem);
            GameObject* go = target->ToGameObject();
            if (!item || !go || bot->CanUseItem(item) != EQUIP_ERR_OK || bot->IsNonMeleeSpellCast(false))
                return false;

            uint32 spellId = 0;
            for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS && !spellId; ++i)
                if (item->GetTemplate()->Spells[i].SpellId > 0 &&
                    item->GetTemplate()->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                    spellId = uint32(item->GetTemplate()->Spells[i].SpellId);
            SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
            if (!info || bot->HasSpellCooldown(spellId))
                return false;

            if (bot->isMoving())
                bot->StopMoving();
            bot->SetFacingToObject(go);

            WorldPacket packet(CMSG_USE_ITEM);
            packet << item->GetBagSlot() << item->GetSlot() << uint8(1) << spellId << item->GetGUID() << uint32(0)
                   << uint8(0);
            if (info->GetExplicitTargetMask() & TARGET_FLAG_GAMEOBJECT_MASK)
                packet << uint32(TARGET_FLAG_GAMEOBJECT) << go->GetPackGUID();
            else
                packet << uint32(TARGET_FLAG_NONE);
            bot->GetSession()->HandleUseItemOpcode(packet);
            // Cast or done at once (instant: its cooldown runs); neither: the core refused it.
            return bot->IsNonMeleeSpellCast(false) || bot->HasSpellCooldown(spellId);
        }
        case StepKind::Gossip:
        {
            // Thrall's stop after Skarloc opens a submenu first: its first option is taken as well.
            if (!DungeonInteract::OpenGossip(bot, target))
                return false;

            int32 option = DungeonInteract::FindGossipOption(bot, [](GossipMenuItem const&) { return true; });
            bool selected = DungeonInteract::SelectGossipOption(bot, target, option);
            GossipMenu const& menu = bot->PlayerTalkClass->GetGossipMenu();
            if (selected && menu.GetSenderGUID() == target->GetGUID() && !menu.Empty())
            {
                option = DungeonInteract::FindGossipOption(bot, [](GossipMenuItem const&) { return true; });
                DungeonInteract::SelectGossipOption(bot, target, option);
            }
            bot->PlayerTalkClass->SendCloseGossip();
            return selected;
        }
    }
    return false;
}

float GatherRadius(Step const& step) { return step.gatherRadius; }
StepKind Kind(Step const& step) { return step.kind; }
char const* Describe(Step const& step) { return step.what; }
}  // namespace DungeonEvents

bool DungeonEventStepAction::Execute(Event /*event*/)
{
    constexpr float USE_MARGIN = 1.5f;
    constexpr uint32 UNREACHABLE_RETRY_MS = 60 * IN_MILLISECONDS;
    constexpr uint32 USED_OBJECT_PAUSE_MS = 3 * MINUTE * IN_MILLISECONDS;  // a used object starts its event once
    constexpr uint32 PATH_CHECK_MS = 10 * IN_MILLISECONDS;
    constexpr uint32 CAST_END_MARGIN_MS = 300;
    constexpr float PAUSE_NEAR_RANGE = 10.0f;
    constexpr float EVENT_ROUTE_LEG_STEP = 40.0f;  // a walk cut short goes by the complete route, leg by leg

    uint32 const now = getMSTime();

    // An item cast on an object (Old Hillsbrad's 5 s bomb): stand still until it ends; ended on time = done with it.
    if (!_castOn.IsEmpty())
    {
        if (bot->IsNonMeleeSpellCast(false))
            return true;
        if (getMSTimeDiff(_castStartMs, now) + CAST_END_MARGIN_MS >= _castMs)
            _done.insert(_castOn);
        _castOn.Clear();
    }

    GuidSet skip = _done;
    for (auto itr = _pausedUntil.begin(); itr != _pausedUntil.end();)
    {
        if (int32(now - itr->second) >= 0)
            itr = _pausedUntil.erase(itr);
        else
        {
            // A paused object the bot stands next to now is no detour any more (OHB wave 15: the leader stood 2 yd
            // from a barrel paused as "no walk" from afar, the step walked to the others and paused them in turn).
            GameObject const* paused = bot->GetMap()->GetGameObject(itr->first);
            if (!paused || !bot->IsWithinDistInMap(paused, PAUSE_NEAR_RANGE))
                skip.insert(itr->first);
            ++itr;
        }
    }

    WorldObject* target = nullptr;
    DungeonEvents::Step const* step = DungeonEvents::NextStep(botAI, target, &skip);
    if (!step || !target)
        return false;

    float const reach = std::max(2.0f, (target->ToGameObject() ? target->ToGameObject()->GetInteractionDistance()
                                                                : INTERACTION_DISTANCE) -
                                           USE_MARGIN);
    if (!bot->IsWithinDistInMap(target, reach))
    {
        // [b32_pull] The leader's walk to the step leads the group: it waits for the laggers and a healer out of
        // sight (DungeonInteract::GroupLaggers) instead of walking alone into the packs on the way; the planner pulls
        // those meanwhile. A member that does not come within GROUP_WAIT_MAX_MS no longer holds the event.
        constexpr float GROUP_LEAD_RADIUS = 20.0f;
        constexpr uint32 GROUP_WAIT_MAX_MS = 45 * IN_MILLISECONDS;
        constexpr uint32 GROUP_WAIT_LOG_MS = 10 * IN_MILLISECONDS;
        std::string const laggers = DungeonInteract::GroupLaggers(bot, target->GetPositionX(), target->GetPositionY(),
                                                                  target->GetPositionZ(), GROUP_LEAD_RADIUS);
        if (laggers.empty())
            _groupWaitSinceMs = 0;
        else
        {
            if (!_groupWaitSinceMs)
                _groupWaitSinceMs = now ? now : 1;
            if (getMSTimeDiff(_groupWaitSinceMs, now) < GROUP_WAIT_MAX_MS)
            {
                // [b33_ohb] stop the step's own walk for good (StopMoving alone let a spline go on, ohb-h 000434)
                if (_stepWalking && (bot->isMoving() || !bot->movespline->Finalized()))
                {
                    bot->StopMoving();
                    bot->GetMotionMaster()->Clear();
                    AI_VALUE(LastMovement&, "last movement").clear();
                }
                _stepWalking = false;
                if (!_groupWaitLogMs || getMSTimeDiff(_groupWaitLogMs, now) >= GROUP_WAIT_LOG_MS)
                {
                    _groupWaitLogMs = now ? now : 1;
                    LOG_INFO("playerbots", "[b32_pull] {} event step: waits for {} before the walk to {} ({:.1f} yd)",
                             bot->GetName(), laggers, DungeonEvents::Describe(*step), bot->GetExactDist(target));
                }
                return false;
            }
        }
        _stepWalking = true;

        // An object the navmesh does not lead to (a barrel below Durnholde's wall, wave 13: the bot tried a straight
        // step off the wall for minutes): another one first, this one again later.
        if (_pathChecked != target->GetGUID() || getMSTimeDiff(_pathCheckedMs, now) > PATH_CHECK_MS)
        {
            PathGenerator path(bot);
            path.CalculatePath(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), false);
            float const endLeft = target->GetExactDist(path.GetActualEndPosition().x, path.GetActualEndPosition().y,
                                                       path.GetActualEndPosition().z);
            // A path cut short (PATHFIND_SHORT: over the point limit, OHB wave 15: every barrel 27-79 yd away) or
            // incomplete is no proof of a walk: its end may be a dead end (ohb-h 2026-10-25). The complete navmesh
            // route decides then, and the walk follows it; a path that ends where the bot stands is the wall case.
            bool const complete =
                !(path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORT)) || endLeft <= reach;
            G3D::Vector3 leg;
            _routeWalk = !complete && SelfTravel::NextRouteLeg(bot, _route,
                                                              G3D::Vector3(target->GetPositionX(),
                                                                           target->GetPositionY(),
                                                                           target->GetPositionZ()),
                                                              reach, EVENT_ROUTE_LEG_STEP, leg);
            bool const walkable = complete || _routeWalk;
            _pathChecked = target->GetGUID();
            _pathCheckedMs = now;
            if (!walkable)
            {
                _pausedUntil[target->GetGUID()] = now + UNREACHABLE_RETRY_MS;
                LOG_INFO("playerbots", "{} event step: no walk to {} ({:.1f} yd), another one first", bot->GetName(),
                         DungeonEvents::Describe(*step), bot->GetExactDist(target));
                return false;
            }
        }
        if (_routeWalk)
        {
            G3D::Vector3 leg;
            if (SelfTravel::NextRouteLeg(bot, _route,
                                         G3D::Vector3(target->GetPositionX(), target->GetPositionY(),
                                                      target->GetPositionZ()),
                                         reach, EVENT_ROUTE_LEG_STEP, leg))
            {
                if (bot->GetExactDist(leg.x, leg.y, leg.z) > 1.0f &&
                    MoveTo(target->GetMapId(), leg.x, leg.y, leg.z, false, false, false, false,
                           MovementPriority::MOVEMENT_NORMAL))
                    return true;
                if (bot->isMoving())
                    return true;
            }
        }
        return MoveTo(target->GetMapId(), target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), false,
                      false, false, false, MovementPriority::MOVEMENT_NORMAL);
    }

    float const gather = DungeonEvents::GatherRadius(*step);
    if (gather > 0.0f && !DungeonInteract::IsGroupGathered(bot, target, gather))
        return false;

    if (!DungeonEvents::Perform(botAI, *step, target))
        return false;

    DungeonEvents::StepKind const kind = DungeonEvents::Kind(*step);
    if (kind == DungeonEvents::StepKind::UseItemOnObject)
    {
        if (bot->IsNonMeleeSpellCast(false))
        {
            Spell const* spell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            _castOn = target->GetGUID();
            _castStartMs = now;
            _castMs = spell && spell->GetSpellInfo() ? uint32(spell->GetSpellInfo()->CalcCastTime(bot)) : 0;
        }
        else
            _done.insert(target->GetGUID());
    }
    else if (kind == DungeonEvents::StepKind::UseObject)
        _pausedUntil[target->GetGUID()] = now + USED_OBJECT_PAUSE_MS;

    LOG_INFO("playerbots", "{} did the event step: {}", bot->GetName(), DungeonEvents::Describe(*step));
    return true;
}

bool MograineKeepAwayAction::Execute(Event /*event*/)
{
    uint32 called = 0;
    Unit* mograine = ScarletMonastery::IdleMograineWithChapel(bot, &called);
    // [b36_sm] the whole hold band, as MograineTooCloseTrigger: a bot resting at 30-33 yd is held there for good
    constexpr float holdEdge = ScarletMonastery::MOGRAINE_KEEP_AWAY + 1.0f + TbcHazard::HOLD_BUFFER;
    if (!mograine || bot->GetExactDist2d(mograine) >= holdEdge)
        return false;

    if (!bot->isMoving())
        LOG_INFO("playerbots", "[sm mograine] {} steps out of Mograine's reach ({:.1f} yd, {} chapel mobs idle)",
                 bot->GetName(), bot->GetExactDist2d(mograine), called);
    // [b32_smpull] Out past the hold band, not onto its edge: LeaveCircle exits at the radius + EXIT_MARGIN (33 yd),
    // and MograineHoldMultiplier (ShouldHoldOutside) holds every reposition within the radius + 1 + HOLD_BUFFER (33 yd)
    // while the current target stands inside - the exit spot sat on that edge. sm-h 20261025-221216-sm-h-5E-14: the
    // tank stepped out to (1145.8,1430.4), 33.0 yd from Mograine (on the pews, navmesh z 32.2, walkable), with him as
    // its target, and stood there 3497-3748 s, "own walk made no progress" x6 towards every waypoint, until the run
    // stalled. The exit now ends KEEP_AWAY_EXIT_EXTRA yd beyond the hold band.
    constexpr float KEEP_AWAY_EXIT_EXTRA = 1.5f;
    float const exitRadius = ScarletMonastery::MOGRAINE_KEEP_AWAY + 1.0f + TbcHazard::HOLD_BUFFER +
                             KEEP_AWAY_EXIT_EXTRA - TbcHazard::EXIT_MARGIN;
    return LeaveCircle(mograine->GetPosition(), exitRadius, false);
}

bool ScholoHallRallyAction::Execute(Event /*event*/)
{
    constexpr float arrived = 3.0f;
    if (bot->IsInCombat() ||
        bot->GetExactDist(Scholomance::HALL_RALLY_X, Scholomance::HALL_RALLY_Y, Scholomance::HALL_RALLY_Z) <= arrived)
        return false;

    Unit* mob = Scholomance::IdleHallMobNear(bot);
    if (!mob)
        return false;

    if (!bot->isMoving())
        LOG_INFO("playerbots", "[scholo hall] {} back to the ramp top: idle {} {:.1f} yd away on the floor",
                 bot->GetName(), mob->GetName(), bot->GetExactDist(mob));
    return MoveTo(bot->GetMapId(), Scholomance::HALL_RALLY_X, Scholomance::HALL_RALLY_Y, Scholomance::HALL_RALLY_Z,
                  false, false, false, false, MovementPriority::MOVEMENT_NORMAL, true);
}

// [b31_ubrs_door]
bool BrsLeaveDrakePenAction::Execute(Event /*event*/)
{
    using namespace BlackrockSpire;
    if (!InDrakePen(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()) || !WavePortcullisOpen(bot))
        return false;

    if (!bot->isMoving())
        LOG_INFO("playerbots", "[b31_ubrs_door] {} leaves the drake pen while the wave portcullis is up ({:.1f},{:.1f},"
                               "{:.1f}{})",
                 bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                 bot->IsInCombat() ? ", in combat" : "");
    return MoveTo(bot->GetMapId(), PEN_EXIT_X, PEN_EXIT_Y, PEN_EXIT_Z, false, false, false, false,
                  MovementPriority::MOVEMENT_FORCED, true);
}

// [b32_brs] Scholomance Viewing Room (ClassicShared.h): the tank's own attack on Vectus / Marduk after Dawn's Gambit.
bool ScholoAttackViewingRoomBossAction::isUseful() { return Scholomance::ViewingRoomBossToAttack(bot) != nullptr; }

bool ScholoAttackViewingRoomBossAction::Execute(Event /*event*/)
{
    Unit* boss = Scholomance::ViewingRoomBossToAttack(bot);
    if (!boss)
        return false;
    // [b33_harn] scholo-h 20261026-000434-scholo-h-5A-3 (2420-2600 s): Attack() at 15-18 yd set the victim but did
    // not reach him; out of combat the trigger stayed on and this action (ACTION_HIGH + 1) re-ran Attack every tick
    // above every walk ("reach melee", the harness waypoint: bot_cannot_move "own walk made no progress"), 180 s
    // idle, Vectus skipped as "route end". Already on him: the tank walks in, his students come with him (one pull,
    // the group's AoE).
    if (bot->GetVictim() == boss && !bot->IsWithinMeleeRange(boss))
        return MoveNear(boss, sPlayerbotAIConfig.ContactDistance, MovementPriority::MOVEMENT_COMBAT);
    LOG_INFO("playerbots", "[b32_brs] {}: Dawn's Gambit has run, attacks {} ({:.1f} yd)", bot->GetName(),
             boss->GetName(), bot->GetExactDist(boss));
    return Attack(boss);
}

// [b32_st] Sunken Temple: the Nightmare Suppressor first (ClassicShared.h SunkenTemple).
bool StAttackSuppressorAction::isUseful() { return SunkenTemple::SuppressorToAttack(bot) != nullptr; }

bool StAttackSuppressorAction::Execute(Event /*event*/)
{
    Unit* mob = SunkenTemple::SuppressorToAttack(bot);
    if (!mob)
        return false;
    LOG_INFO("playerbots", "[b32_st] {}: attacks the idle {} ({:.1f} yd, was on {})", bot->GetName(), mob->GetName(),
             bot->GetExactDist(mob), bot->GetVictim() ? bot->GetVictim()->GetName() : std::string("nothing"));
    return Attack(mob);
}
