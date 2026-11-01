/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EscortProtect.h"
#include "BotPath.h"  // [lead109]

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>

#include "AttackersValue.h"
#include "Creature.h"
#include "DungeonInteract.h"  // [b32_pull] GroupLaggers
#include "Group.h"            // [b32_pull]
#include "InstanceGuid.h"     // [rot44]
#include "LastMovementValue.h"  // [b33_ohb] the hold stops the walk for good
#include "MotionMaster.h"       // [b33_ohb]
#include "MoveSpline.h"         // [b33_ohb]
#include "InstanceScript.h"
#include "Playerbots.h"
#include "Timer.h"
#include "CellImpl.h"           // [b36_ohb] the way-ahead scan
#include "GridNotifiers.h"      // [b36_ohb]
#include "GridNotifiersImpl.h"  // [b36_ohb]
#include "ScriptSystem.h"       // [b36_ohb] script_waypoint of the escort
#include "StringFormat.h"       // [b36_ohb]
#include "PathGenerator.h"      // [b38_ohb] the return walk's path check

#include <cmath>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
constexpr uint32 NO_DATA_GATE = std::numeric_limits<uint32>::max();
constexpr float ESCORT_SEARCH_RANGE = 80.0f;   // the group walks with the NPC; farther is not this group's escort

struct EscortDef
{
    uint32 mapId;
    uint32 entry;
    float healBelowPct;  // healers start on the NPC below this health
    float searchRange = ESCORT_SEARCH_RANGE;
    // 0: no leash. While the NPC walks (no gossip flag) the group stays within leash of it (combatLeash in combat);
    // while it waits for the group (gossip flag up) a member out of combat farther than waitReturn walks back to it.
    float leash = 0.0f;
    float combatLeash = 0.0f;
    float waitReturn = 0.0f;
    // The escort counts as under way only while instance data dataIndex lies in [dataFrom, dataUntil).
    uint32 dataIndex = NO_DATA_GATE;
    uint32 dataFrom = 0;
    uint32 dataUntil = 0;
};

// Gnomeregan: Blastmaster Emi Shortfuse (SmartAI 7998: waypoints 1-14, creature groups 1-4 = the trogg waves from the
// two tunnels, Cave In 146086 / 146085 set open with the waves and shut by her Explosion 12158 / 12159, then Grubbis
// 7361 and Chomper). Wailing Caverns: the Disciple of Naralex 3678 (raptor ambushes on the walk, then Mutanus).
// Old Hillsbrad Foothills: Thrall 17876 (old_hillsbrad.cpp npc_thrall_old_hillsbradAI, an npc_escortAI): from the
// barrels (DATA_ESCORT_PROGRESS 0 >= 1) to the end (9) he walks Durnholde, the Skarloc road, Tarren Mill and the inn;
// every ambush and the Tarren Mill guards go for him. On his death the instance puts him back at the last
// checkpoint 10 s later (EVENT_THRALL_REPOSITION, up to 20 attempts) with his gossip up again. 2026-10-25 ohb-h
// (17/17 failed): he died up to 7 times a run with the group 60-110 yd away fighting Tarren Mill guards and
// wildlife, and after the last death the group wandered the hills for 22 min ("full clear") and never went back.
constexpr std::array<EscortDef, 3> ESCORTS = {{
    {90, 7998, 80.0f},
    {43, 3678, 80.0f},
    {560, 17876, 90.0f, 250.0f, 20.0f, 35.0f, 60.0f, 0, 1, 9},
}};

EscortDef const* FindDef(uint32 mapId, uint32 entry)
{
    auto const def = std::find_if(ESCORTS.begin(), ESCORTS.end(),
                                  [mapId, entry](EscortDef const& d) { return d.mapId == mapId && d.entry == entry; });
    return def != ESCORTS.end() ? &*def : nullptr;
}

// The instance data gate of the escort (always open without one).
bool EscortUnderWay(Player* bot, EscortDef const& def)
{
    if (def.dataIndex == NO_DATA_GATE)
        return true;

    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance)
        return false;

    uint32 const progress = instance->GetData(def.dataIndex);
    return progress >= def.dataFrom && progress < def.dataUntil;
}
constexpr float ESCORT_ATTACKER_RANGE = 40.0f;  // an attacker this close to the NPC (they all are: melee trash)
constexpr float ESCORT_HEAL_RANGE = 40.0f;

// Righteous Defense takes up to three mobs off the NPC; the others taunt the mob itself.
constexpr std::array<char const*, 4> TAUNTS = {"hand of reckoning", "taunt", "growl", "dark command"};
// Fast heals first: the NPC takes trash melee in bursts, a long cast lands after the next hits.
constexpr std::array<char const*, 10> HEALS = {"flash heal", "flash of light", "lesser healing wave", "regrowth",
                                               "renew", "rejuvenation", "greater heal", "holy light",
                                               "healing wave", "healing touch"};

// [b36_ohb] Escort pacing (EscortProtect.h EscortWayBlocker / EscortWalksIntoPack / EscortGossipHeld) and the escort
// heal gate.
constexpr float ESCORT_WAY_STEP = 3.0f;            // way samples this far apart
constexpr float ESCORT_WAY_SCAN_EXTRA = 30.0f;     // the creature scan reaches this far past the way's end (aggro)
constexpr float ESCORT_WAY_SCAN_MAX = 130.0f;
constexpr float ESCORT_WAY_AGGRO_MARGIN = 1.5f;    // MovementAction::SIGHT_IDLE_AGGRO_MARGIN
constexpr uint32 ESCORT_WAY_CACHE_MS = 2 * IN_MILLISECONDS;
constexpr uint32 ESCORT_WAY_LOS_PER_MOB = 4;       // sight tests per creature at most
constexpr float ESCORT_WALK_LOOKAHEAD = 45.0f;     // ~6 s of Thrall's run: the pack the tank must take now
constexpr float ESCORT_GOSSIP_LOOKAHEAD = 100.0f;  // from his cell: the armory and the keep yard (waypoints 0-15)
constexpr uint32 ESCORT_GOSSIP_HOLD_MAX_MS = 150 * IN_MILLISECONDS;  // a pack nobody pulls holds no stop for ever
constexpr uint32 ESCORT_GOSSIP_HOLD_GAP_MS = 10 * MINUTE * IN_MILLISECONDS;  // [b38_ohb] a stale hold (was 30 s)
constexpr float ESCORT_GOSSIP_HOLD_MOVED = 10.0f;  // the NPC this far from the hold's spot: another stop
constexpr float ESCORT_HEAL_EMERGENCY_PCT = 35.0f;  // below it the NPC is healed whatever it costs
constexpr float ESCORT_HEALER_REACH = 30.0f;        // HealerOutOfReach's HEALER_PULL_REACH
constexpr float ESCORT_HEALER_TANK_SIDE = 4.0f;     // the healer's spot: this far from the tank, on its own side
constexpr uint32 ESCORT_LOG_MS = 10 * IN_MILLISECONDS;

struct WayPoint
{
    G3D::Vector3 pos;
    float along;  // yd along the way from the NPC
};

// The escort's way ahead, at most `lookahead` yd: from where it stands along its script waypoints (script_waypoint,
// the npc_escortAI path), from the leg nearest to it (3D: Thrall's cell lies a floor under the armory). Samples
// ESCORT_WAY_STEP yd apart, the NPC's own spot first.
std::vector<WayPoint> EscortWayAhead(Creature* escort, float lookahead)
{
    std::vector<WayPoint> way;
    G3D::Vector3 const from(escort->GetPositionX(), escort->GetPositionY(), escort->GetPositionZ());
    way.push_back({from, 0.0f});
    ScriptPointVector const& points = sScriptSystemMgr->GetPointMoveList(escort->GetEntry());
    if (points.size() < 2)
        return way;

    size_t leg = 0;
    float best = std::numeric_limits<float>::max();
    for (size_t i = 0; i + 1 < points.size(); ++i)
    {
        G3D::Vector3 const a(points[i].fX, points[i].fY, points[i].fZ);
        G3D::Vector3 const ab = G3D::Vector3(points[i + 1].fX, points[i + 1].fY, points[i + 1].fZ) - a;
        float const len2 = ab.squaredLength();
        float const t = len2 > 0.0f ? std::clamp((from - a).dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
        float const d = (a + ab * t - from).squaredLength();
        if (d < best)
        {
            best = d;
            leg = i;
        }
    }

    float walked = 0.0f;
    G3D::Vector3 at = from;
    for (size_t i = leg + 1; i < points.size() && walked < lookahead; ++i)
    {
        G3D::Vector3 const next(points[i].fX, points[i].fY, points[i].fZ);
        float const len = (next - at).length();
        for (float s = ESCORT_WAY_STEP; s < len && walked + s <= lookahead; s += ESCORT_WAY_STEP)
            way.push_back({at + (next - at) * (s / len), walked + s});
        walked += len;
        if (walked > lookahead)
            break;
        way.push_back({next, walked});
        at = next;
    }
    return way;
}

// The nearest idle creature along the way whose proximity aggro reaches the NPC there (see EscortWayBlocker).
Creature* ScanEscortWay(Player* bot, Creature* escort, float lookahead, float& wayDist)
{
    wayDist = 0.0f;
    std::vector<WayPoint> const way = EscortWayAhead(escort, lookahead);
    float const radius = std::min(lookahead + ESCORT_WAY_SCAN_EXTRA, ESCORT_WAY_SCAN_MAX);
    std::list<Unit*> units;
    Acore::AnyUnitInObjectRangeCheck check(escort, radius);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(escort, units, check);
    Cell::VisitObjects(escort, searcher, radius);

    Creature* best = nullptr;
    float bestAlong = std::numeric_limits<float>::max();
    for (Unit* unit : units)
    {
        Creature* mob = unit ? unit->ToCreature() : nullptr;
        if (!mob || mob == escort || !mob->IsAlive() || mob->IsInCombat() || !mob->IsVisible() || mob->IsCritter() ||
            mob->IsTotem() || mob->IsImmuneToPC() || mob->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) ||
            mob->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) || !mob->GetCharmerOrOwnerGUID().IsEmpty() ||
            mob->HasReactState(REACT_PASSIVE) || !(mob->IsHostileTo(escort) || mob->IsHostileTo(bot)) ||
            !bot->IsValidAttackTarget(mob))
            continue;

        float const reach = MovementAction::IdleAggroReach(mob, escort, ESCORT_WAY_AGGRO_MARGIN);
        uint32 sightTests = 0;
        for (WayPoint const& point : way)
        {
            if (point.along >= bestAlong || sightTests >= ESCORT_WAY_LOS_PER_MOB)
                break;
            if (mob->GetExactDist(point.pos.x, point.pos.y, point.pos.z) >= reach)
                continue;
            char const* why = MovementAction::IdleAggroBlocker(mob, escort, point.pos.x, point.pos.y, point.pos.z,
                                                               ESCORT_WAY_AGGRO_MARGIN, true);
            if (!why)
            {
                best = mob;
                bestAlong = point.along;
                break;
            }
            if (!strcmp(why, "los"))
                ++sightTests;
        }
    }
    if (best)
        wayDist = bestAlong;
    return best;
}

// The group's living tank on the bot's map nearest to the bot (not the bot), or nullptr.
Player* GroupTankOf(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    Player* best = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
            !PlayerbotAI::IsTank(member))
            continue;
        if (!best || bot->GetExactDist(member) < bot->GetExactDist(best))
            best = member;
    }
    return best;
}

// [b36_ohb] The escort heal gate: a healer that does not see the tank does not heal the NPC while it fights a mob the
// tank has not engaged (the heal's threat brings that mob, and its pack, onto the healer), unless the NPC is below
// ESCORT_HEAL_EMERGENCY_PCT (his death sends the escort back to its last stop: dearer than a risky heal then).
// ohb-h 20261026-034959 1479 s: "renew on Thrall (87 % hp)" from 10 yd out of the tank's sight, four mobs on the
// priest, dead 4 s later. True: the heal waits (logged "[b36_ohb] escort heal hold", rate-limited per bot).
bool EscortHealHeld(Player* bot, Creature* escort)
{
    if (!escort->IsInCombat() || escort->GetHealthPct() < ESCORT_HEAL_EMERGENCY_PCT)
        return false;

    Player* tank = GroupTankOf(bot);
    if (!tank || (bot->GetExactDist(tank) <= ESCORT_HEAL_RANGE && bot->IsWithinLOSInMap(tank)))
        return false;

    Unit* loose = nullptr;
    for (Unit* attacker : escort->getAttackers())
        if (attacker && attacker->IsAlive() && !attacker->IsEngagedBy(tank))
        {
            loose = attacker;
            break;
        }
    if (!loose)
        if (Unit* victim = escort->GetVictim(); victim && victim->IsAlive() && !victim->IsEngagedBy(tank))
            loose = victim;
    if (!loose)
        return false;

    thread_local std::unordered_map<ObjectGuid, uint32> logged;
    uint32 const now = getMSTime();
    uint32& at = logged[bot->GetGUID()];
    if (!at || getMSTimeDiff(at, now) >= ESCORT_LOG_MS)
    {
        at = now ? now : 1;
        LOG_INFO("playerbots", "[b36_ohb] escort heal hold: {} holds its heal on {} ({:.0f} % hp, heals below "
                 "{:.0f} %): {} not engaged by the tank {}, the tank {:.1f} yd away{}", bot->GetName(),
                 escort->GetName(), escort->GetHealthPct(), ESCORT_HEAL_EMERGENCY_PCT, loose->GetName(),
                 tank->GetName(), bot->GetExactDist(tank), bot->IsWithinLOSInMap(tank) ? "" : ", out of sight");
    }
    return true;
}
}  // namespace

namespace EscortProtect
{
bool MapHasEscort(uint32 mapId)
{
    return std::any_of(ESCORTS.begin(), ESCORTS.end(), [mapId](EscortDef const& def) { return def.mapId == mapId; });
}

Creature* Escort(PlayerbotAI* botAI)
{
    ObjectGuid const guid = botAI->GetAiObjectContext()->GetValue<ObjectGuid>("escort npc")->Get();
    if (guid.IsEmpty())
        return nullptr;

    Creature* escort = botAI->GetCreature(guid);
    if (!escort || !escort->IsAlive() || escort->GetMapId() != botAI->GetBot()->GetMapId())
        return nullptr;

    return escort;
}

Unit* EscortAttacker(PlayerbotAI* botAI, Creature* escort)
{
    if (!escort)
        return nullptr;

    Player* bot = botAI->GetBot();
    Unit* best = nullptr;
    for (Unit* attacker : escort->getAttackers())
    {
        if (!attacker || !attacker->IsAlive() || attacker->GetVictim() != escort ||
            !escort->IsWithinDist(attacker, ESCORT_ATTACKER_RANGE) || !AttackersValue::IsValidTarget(attacker, bot))
            continue;

        if (!best || escort->GetExactDist(attacker) < escort->GetExactDist(best))
            best = attacker;
    }
    return best;
}

Creature* EscortLeash(PlayerbotAI* botAI, float& stand)
{
    stand = 0.0f;
    Creature* escort = Escort(botAI);
    if (!escort)
        return nullptr;

    Player* bot = botAI->GetBot();
    EscortDef const* def = FindDef(bot->GetMapId(), escort->GetEntry());
    if (!def || def->leash <= 0.0f || !bot->IsAlive())
        return nullptr;

    float const dist = bot->GetExactDist2d(escort);
    if (escort->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
    {
        // Waiting for the group's word: out of combat only (the planner clears the way meanwhile).
        if (def->waitReturn <= 0.0f || bot->IsInCombat() || dist <= def->waitReturn)
            return nullptr;

        stand = def->waitReturn;
        return escort;
    }

    stand = bot->IsInCombat() ? def->combatLeash : def->leash;
    return dist > stand ? escort : nullptr;
}

Creature* EscortAnchor(PlayerbotAI* botAI, bool& waiting, float& waitReturn)
{
    waiting = false;
    waitReturn = 0.0f;
    Player* bot = botAI->GetBot();
    if (!bot || !bot->IsAlive() || !MapHasEscort(bot->GetMapId()))
        return nullptr;

    Creature* escort = Escort(botAI);
    if (!escort)
        return nullptr;

    EscortDef const* def = FindDef(bot->GetMapId(), escort->GetEntry());
    if (!def || def->leash <= 0.0f)
        return nullptr;

    waiting = escort->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP);
    waitReturn = def->waitReturn;
    return escort;
}

Creature* EscortWayBlocker(Player* bot, Creature* escort, float lookahead, float* wayDist)
{
    if (wayDist)
        *wayDist = 0.0f;
    if (!bot || !escort || !escort->IsInWorld() || escort->GetMap() != bot->GetMap())
        return nullptr;

    // One scan over the longest lookahead per escort every ESCORT_WAY_CACHE_MS (per thread: map and world threads);
    // a shorter question takes the nearest blocker when it lies within its own lookahead.
    struct WayScan
    {
        uint32 atMs = 0;
        ObjectGuid blocker;
        float along = 0.0f;
    };
    thread_local std::map<InstanceGuid, WayScan> scans;  // [rot44] by instance: the escort's GUID is per map
    uint32 const now = getMSTime();
    WayScan& scan = scans[InstanceKey(escort)];
    if (!scan.atMs || getMSTimeDiff(scan.atMs, now) >= ESCORT_WAY_CACHE_MS)
    {
        scan.atMs = now ? now : 1;
        float along = 0.0f;
        Creature* found = ScanEscortWay(bot, escort, std::max(lookahead, ESCORT_GOSSIP_LOOKAHEAD), along);
        scan.blocker = found ? found->GetGUID() : ObjectGuid::Empty;
        scan.along = along;
    }

    if (scan.blocker.IsEmpty() || scan.along > lookahead)
        return nullptr;

    Creature* blocker = ObjectAccessor::GetCreature(*escort, scan.blocker);
    if (!blocker || !blocker->IsAlive() || blocker->IsInCombat())
        return nullptr;

    if (wayDist)
        *wayDist = scan.along;
    return blocker;
}

bool EscortWalksIntoPack(PlayerbotAI* botAI, std::string* why)
{
    bool waiting = false;
    float waitReturn = 0.0f;
    Creature* escort = EscortAnchor(botAI, waiting, waitReturn);
    if (!escort || waiting)
        return false;

    if (escort->IsInCombat())
        if (Unit* victim = escort->GetVictim(); victim && victim->IsAlive())
        {
            if (why)
                *why = Acore::StringFormat("{} fights {}", escort->GetName(), victim->GetName());
            return true;
        }

    float along = 0.0f;
    Creature* blocker = EscortWayBlocker(botAI->GetBot(), escort, ESCORT_WALK_LOOKAHEAD, &along);
    if (!blocker)
        return false;

    if (why)
        *why = Acore::StringFormat("{} walks into {} ({:.0f} yd on along his way)", escort->GetName(),
                                   blocker->GetName(), along);
    return true;
}

// [b42_ohb] ohb-h 20261026-063902-ohb-h-5D-16 (build 41, bear Rtdruam): after Drake (1062 s) Thrall waited in his cell
// ~120 yd up. The members held "for leader Rtdruam not at the escort yet" while their "follow" anchored on Thrall
// (EscortAnchor) and walked nowhere, the leader held "for Rtlocaf(far),Rtmagah(far),Rtshaae(far)" (laggers: not
// within 20 yd of it), so nobody came to anybody until the leader's 45 s lagger wait ran out; it then walked the
// [b38_ohb] route alone into the keep-yard pack (Warden / Veteran / Sentry / Riflemen / Hound at ~2178,142,88,
// unplanned pull 27 "tank's reach melee", healer 71.6 yd off), dead at 1116 s, then three more (death burst 1137 s).
Creature* EscortFollowAnchor(PlayerbotAI* botAI)
{
    bool waiting = false;
    float waitReturn = 0.0f;
    Creature* escort = EscortAnchor(botAI, waiting, waitReturn);
    if (!escort || !waiting)
        return escort;

    Player* bot = botAI->GetBot();
    Group* group = bot->GetGroup();
    if (!group || group->GetLeaderGUID() == bot->GetGUID())
        return escort;

    Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());
    if (leader && leader != bot && DungeonInteract::IsActiveMember(bot, leader) &&
        leader->GetExactDist2d(escort) > waitReturn)
        return nullptr;

    return escort;
}

bool EscortGossipHeld(Player* bot, Creature* escort, std::string* why)
{
    if (!bot || !escort || !escort->IsAlive() || escort->IsInCombat() || !escort->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
        return false;

    EscortDef const* def = FindDef(bot->GetMapId(), escort->GetEntry());
    if (!def || def->leash <= 0.0f)
        return false;

    struct GossipHold
    {
        uint32 sinceMs = 0;
        uint32 lastMs = 0;
        uint32 logMs = 0;
        bool gaveUp = false;
        Position spot;
    };
    thread_local std::map<InstanceGuid, GossipHold> holds;  // [rot44] by instance: the escort's GUID is per map
    uint32 const now = getMSTime();
    GossipHold& hold = holds[InstanceKey(escort)];
    // [b38_ohb] The cap counts from the first hold at this stop, whatever the bot did in between: the step asks only
    // within 80 yd of Thrall and out of combat (ohb-h 20261026-050341: two "held 0 s" lines, then the tank was out
    // of range or pulling), so a reset after a short gap let the cap never come. A new stop (the NPC moved: walked
    // on, or put back after a death) or a stale record starts a new hold.
    if (hold.sinceMs && (getMSTimeDiff(hold.lastMs, now) >= ESCORT_GOSSIP_HOLD_GAP_MS ||
                         escort->GetExactDist(&hold.spot) > ESCORT_GOSSIP_HOLD_MOVED))
        hold = GossipHold();

    float along = 0.0f;
    Creature* blocker = EscortWayBlocker(bot, escort, ESCORT_GOSSIP_LOOKAHEAD, &along);
    if (!blocker)
        return false;

    if (!hold.sinceMs)
    {
        hold.sinceMs = now ? now : 1;
        hold.spot = escort->GetPosition();
    }
    hold.lastMs = now;
    uint32 const heldMs = getMSTimeDiff(hold.sinceMs, now);
    if (heldMs >= ESCORT_GOSSIP_HOLD_MAX_MS)
    {
        if (!hold.gaveUp)
        {
            hold.gaveUp = true;
            LOG_INFO("playerbots", "[b36_ohb] escort gossip: {} gives up the wait for {} after {} s ({} still idle "
                     "{:.0f} yd on along the way), talks to it", bot->GetName(), escort->GetName(),
                     heldMs / IN_MILLISECONDS,
                     blocker->GetName(), along);
        }
        return false;
    }

    std::string const reason = Acore::StringFormat("{} idle {:.0f} yd on along {}'s way ({:.1f} yd from {}), held {} s",
                                                   blocker->GetName(), along, escort->GetName(),
                                                   blocker->GetExactDist(escort), escort->GetName(),
                                                   heldMs / IN_MILLISECONDS);
    if (!hold.logMs || getMSTimeDiff(hold.logMs, now) >= ESCORT_LOG_MS)
    {
        hold.logMs = now ? now : 1;
        LOG_INFO("playerbots", "[b36_ohb] escort gossip: {} waits before talking to {}: {}", bot->GetName(),
                 escort->GetName(), reason);
    }
    if (why)
        *why = reason;
    return true;
}
}  // namespace EscortProtect

ObjectGuid EscortNpcValue::Calculate()
{
    uint32 const mapId = bot->GetMapId();
    Creature* nearest = nullptr;
    for (EscortDef const& def : ESCORTS)
    {
        if (def.mapId != mapId)
            continue;

        if (!EscortUnderWay(bot, def))
            continue;

        Creature* escort = bot->FindNearestCreature(def.entry, def.searchRange, true);
        if (escort && (!nearest || bot->GetExactDist(escort) < bot->GetExactDist(nearest)))
            nearest = escort;
    }
    return nearest ? nearest->GetGUID() : ObjectGuid::Empty;
}

bool EscortNpcAttackedTrigger::IsActive()
{
    if (PlayerbotAI::IsHeal(bot) && !PlayerbotAI::IsTank(bot))
        return false;

    Unit* attacker = EscortProtect::EscortAttacker(botAI, EscortProtect::Escort(botAI));
    return attacker && attacker != AI_VALUE(Unit*, "current target");
}

bool EscortNpcLowHealthTrigger::IsActive()
{
    if (!PlayerbotAI::IsHeal(bot))
        return false;

    Creature* escort = EscortProtect::Escort(botAI);
    if (!escort || !bot->IsWithinDist(escort, ESCORT_HEAL_RANGE))
        return false;

    EscortDef const* def = FindDef(bot->GetMapId(), escort->GetEntry());
    // [b36_ohb] not into a pack the tank has not picked up while the healer does not see the tank (EscortHealHeld)
    return def && escort->GetHealthPct() < def->healBelowPct && !EscortHealHeld(bot, escort);
}

namespace
{
// [b36_ohb] The tank the healer must get into sight of before the escort walks into a pack (EscortHealerSight*), or
// nullptr: the escort walks into a fight, the tank stands near the NPC (within its leash, less the side step) and the
// healer sees it not or from too far.
Player* EscortHealerSightTank(PlayerbotAI* botAI, Player* bot)
{
    if (!PlayerbotAI::IsHeal(bot) || PlayerbotAI::IsTank(bot) || bot->IsInCombat() || !bot->IsAlive())
        return nullptr;

    Creature* escort = EscortProtect::Escort(botAI);
    EscortDef const* def = escort ? FindDef(bot->GetMapId(), escort->GetEntry()) : nullptr;
    if (!def || def->leash <= 0.0f || !EscortProtect::EscortWalksIntoPack(botAI))
        return nullptr;

    Player* tank = GroupTankOf(bot);
    if (!tank || tank->GetExactDist2d(escort) > def->leash - ESCORT_HEALER_TANK_SIDE)
        return nullptr;

    return bot->GetExactDist(tank) > ESCORT_HEALER_REACH || !bot->IsWithinLOSInMap(tank) ? tank : nullptr;
}
}  // namespace

bool EscortHealerSightTrigger::IsActive() { return EscortHealerSightTank(botAI, bot) != nullptr; }

bool EscortHealerSightAction::Execute(Event /*event*/)
{
    Player* tank = EscortHealerSightTank(botAI, bot);
    if (!tank)
        return false;

    // Next to the tank on the healer's side: in its sight, behind it as seen from the pack it pulls.
    float const dist = bot->GetExactDist2d(tank);
    float const share = dist > ESCORT_HEALER_TANK_SIDE ? ESCORT_HEALER_TANK_SIDE / dist : 0.0f;
    float const x = tank->GetPositionX() + (bot->GetPositionX() - tank->GetPositionX()) * share;
    float const y = tank->GetPositionY() + (bot->GetPositionY() - tank->GetPositionY()) * share;

    uint32 const now = getMSTime();
    if (!_logMs || getMSTimeDiff(_logMs, now) >= ESCORT_LOG_MS)
    {
        _logMs = now ? now : 1;
        std::string why;
        EscortProtect::EscortWalksIntoPack(botAI, &why);
        LOG_INFO("playerbots", "[b36_ohb] escort healer sight: {} moves to the tank {} ({:.1f} yd{}): {}",
                 bot->GetName(), tank->GetName(), bot->GetExactDist(tank),
                 bot->IsWithinLOSInMap(tank) ? "" : ", out of sight", why);
    }

    return MoveTo(tank->GetMapId(), x, y, tank->GetPositionZ(), false, false, false, false,
                  MovementPriority::MOVEMENT_NORMAL, true, false);
}

bool EscortNpcFarTrigger::IsActive()
{
    float stand = 0.0f;
    return EscortProtect::EscortLeash(botAI, stand) != nullptr;
}

bool EscortStayCloseAction::Execute(Event /*event*/)
{
    // Short of the NPC, on the bot's side: next to it, not in its path.
    constexpr float besideNpc = 4.0f;
    constexpr uint32 logMs = 10 * IN_MILLISECONDS;

    float stand = 0.0f;
    Creature* escort = EscortProtect::EscortLeash(botAI, stand);
    if (!escort)
        return false;

    float const dist = bot->GetExactDist2d(escort);
    float const share = dist > besideNpc ? besideNpc / dist : 0.0f;
    float const x = escort->GetPositionX() + (bot->GetPositionX() - escort->GetPositionX()) * share;
    float const y = escort->GetPositionY() + (bot->GetPositionY() - escort->GetPositionY()) * share;

    uint32 const now = getMSTime();

    // [b32_pull] The return to a WAITING escort (gossip up: in his cell, at a checkpoint, after a death) is a walk
    // through the dungeon, not a leash: the group goes together. The leader waits for the laggers and a healer out of
    // sight (DungeonInteract::GroupLaggers), the others do not walk ahead of the leader (they follow it); the planner
    // pulls the packs on the way. ohb-h 20261025-221216 1162 s: the tank's "escort return" to Thrall's cell 60 yd off
    // started the solo walk that pulled Durnholde Riflemen without the healer (pull 22). A walking escort's leash is
    // untouched: that one must keep up with the NPC.
    if (escort->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
    {
        constexpr float groupLeadRadius = 20.0f;
        constexpr uint32 groupWaitMaxMs = 45 * IN_MILLISECONDS;
        std::string holdFor;
        Group* group = bot->GetGroup();
        if (group && group->GetLeaderGUID() == bot->GetGUID())
            holdFor = DungeonInteract::GroupLaggers(bot, escort->GetPositionX(), escort->GetPositionY(),
                                                    escort->GetPositionZ(), groupLeadRadius);
        else if (group)
        {
            // [b33_ohb] A member never walks back to the waiting escort on its own while the leader lives here and is
            // not with the NPC yet (farther than its return distance): it stays with the leader (follow) and comes
            // with it. The b32 rule held only a member already 5 yd nearer the NPC than the leader, which let both
            // strays of ohb-h 20261026-000434 go: the healer at 1157 s (117 yd from Thrall, the tank 109 yd, resting
            // after Drake) walked up the keep ramp alone, 69 yd and 35 yd above the tank, and died to pull 25 at
            // 1180 s; the cat at 1185 s (4 yd nearer than the tank, which walked to res the healer) went on, its
            // [follow path] alt spot (2167.8,135.2) led it into Riflemen and it opened pull 26 alone (the wipe).
            Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());
            if (leader && leader != bot && DungeonInteract::IsActiveMember(bot, leader) &&
                leader->GetExactDist2d(escort) > stand)
                holdFor = "leader " + leader->GetName() + " not at the escort yet";
        }

        bool const member = !holdFor.empty() && group && group->GetLeaderGUID() != bot->GetGUID();
        if (holdFor.empty())
            _groupWaitSinceMs = 0;
        else
        {
            if (!_groupWaitSinceMs)
                _groupWaitSinceMs = now ? now : 1;
            // [b33_ohb] Only the leader's wait for laggers runs out; a member's wait lasts while the leader leads.
            if (member || getMSTimeDiff(_groupWaitSinceMs, now) < groupWaitMaxMs)
            {
                // [b33_ohb] Stop this action's own walk for good: a StopMoving alone left the healer's spline to
                // Thrall's cell going on (1167-1177 s: the hold logged at 87 yd, she still walked 40 yd on).
                if (_walking && (bot->isMoving() || !bot->movespline->Finalized()))
                {
                    bot->StopMoving();
                    bot->GetMotionMaster()->Clear();
                    AI_VALUE(LastMovement&, "last movement").clear();
                }
                _walking = false;
                if (!_logMs || getMSTimeDiff(_logMs, now) >= logMs)
                {
                    _logMs = now;
                    LOG_INFO("playerbots", "[escort return] {}: {} waits {:.0f} yd away, holds for {} [b32_pull]",
                             bot->GetName(), escort->GetName(), dist, holdFor);
                }
                return false;
            }
        }
    }
    _walking = true;
    if (!_logMs || getMSTimeDiff(_logMs, now) >= logMs)
    {
        _logMs = now;
        bool const waiting = escort->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP);
        LOG_INFO("playerbots", "[{}] {}: {} {} {:.0f} yd away (limit {:.0f}){}, back to it",
                 waiting ? "escort return" : "escort leash", bot->GetName(), escort->GetName(),
                 waiting ? "waits" : "walks", dist, stand, bot->IsInCombat() ? " in combat" : "");
    }

    // [b38_ohb] The return to a waiting escort far off goes by the complete route. ohb-h 20261026-050341-ohb-h-5A-16
    // (paladin tank Rtpalai, 1282-1480 s): after Drake the plain MoveTo to Thrall's cell 110-123 yd off (up the keep
    // ramp, longer than PathGenerator's corridor) walked the partial path's end straight on into a navmesh gap at
    // (2122.3,106.2,52.9) (no polygon under it; the main floor's edge 4 yd south, a 17-polygon island 8.7 yd north,
    // navmesh_probe). From there every path started on the island: "own walk made no progress", 200 s idle, while
    // the harness's short steps back onto the floor were suspended as "Thrall waits ... leads" (MoveConflict).
    if (escort->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
    {
        constexpr float ROUTE_MIN_DIST = 25.0f;  // nearer: the plain walk
        constexpr float ROUTE_LEG_STEP = 40.0f;  // DungeonEventStepAction's EVENT_ROUTE_LEG_STEP
        constexpr float ROUTE_REACH = 5.0f;
        constexpr uint32 PATH_CHECK_MS = 10 * IN_MILLISECONDS;
        constexpr uint32 OFF_MESH_RETRY_MS = 3 * IN_MILLISECONDS;
        constexpr float OFF_MESH_FLOOR_DZ = 2.0f;  // a polygon this far under / over the bot: it stands on it
        constexpr float OFF_MESH_STEP_DZ = 3.0f;   // a step back onto the floor at about the bot's height
        constexpr float OFF_MESH_RING_MIN = 2.0f;
        constexpr float OFF_MESH_RING_MAX = 10.0f;
        constexpr float OFF_MESH_RING_STEP = 2.0f;
        constexpr int OFF_MESH_ANGLES = 16;

        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        float floorZ = 0.0f;
        if (!SelfTravel::NavmeshFloor(bot, here, OFF_MESH_FLOOR_DZ, floorZ))
        {
            // Off the navmesh: the nearest floor point at about its height, in a straight line over ground (as the
            // harness's stall_short_step); the nearest ring first, on a ring the escort's direction first.
            if (!_offMeshTriedMs || getMSTimeDiff(_offMeshTriedMs, now) >= OFF_MESH_RETRY_MS)
            {
                _offMeshTriedMs = now ? now : 1;
                float const toEscort = std::atan2(escort->GetPositionY() - here.y, escort->GetPositionX() - here.x);
                for (float ring = OFF_MESH_RING_MIN; ring <= OFF_MESH_RING_MAX; ring += OFF_MESH_RING_STEP)
                    for (int step = 0; step < OFF_MESH_ANGLES; ++step)
                    {
                        // 0, +1, -1, +2, -2 ... sectors off the escort's direction
                        int const sector = (step + 1) / 2 * (step % 2 ? 1 : -1);
                        float const angle = toEscort + float(sector) * 2.0f * float(M_PI) / float(OFF_MESH_ANGLES);
                        G3D::Vector3 const probe(here.x + ring * std::cos(angle), here.y + ring * std::sin(angle),
                                                 here.z);
                        float probeZ = 0.0f;
                        if (!SelfTravel::NavmeshFloor(bot, probe, OFF_MESH_STEP_DZ, probeZ) ||
                            std::fabs(probeZ - here.z) > OFF_MESH_STEP_DZ)
                            continue;
                        if (MoveStraight(escort->GetMapId(), probe.x, probe.y, probeZ))
                        {
                            LOG_INFO("playerbots", "[b38_ohb] escort return: {} stands off the navmesh at "
                                     "({:.1f},{:.1f},{:.1f}), steps {:.0f} yd back onto it ({:.1f},{:.1f},{:.1f})",
                                     bot->GetName(), here.x, here.y, here.z, ring, probe.x, probe.y, probeZ);
                            return true;
                        }
                    }
            }
        }
        else if (dist > ROUTE_MIN_DIST)
        {
            G3D::Vector3 const goal(x, y, escort->GetPositionZ());
            if (!_pathCheckedMs || getMSTimeDiff(_pathCheckedMs, now) >= PATH_CHECK_MS)
            {
                _pathCheckedMs = now ? now : 1;
                BotPath const path = FindBotPath(bot, goal.x, goal.y, goal.z);  // [lead109]
                G3D::Vector3 const end = path.actualEnd;
                _routeWalk = (path.type & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORT)) &&
                             (end - goal).length() > ROUTE_REACH;
            }
            G3D::Vector3 leg;
            if (_routeWalk && SelfTravel::NextRouteLeg(bot, _route, goal, ROUTE_REACH, ROUTE_LEG_STEP, leg))
            {
                if (!_routeLogMs || getMSTimeDiff(_routeLogMs, now) >= logMs)
                {
                    _routeLogMs = now ? now : 1;
                    LOG_INFO("playerbots", "[b38_ohb] escort return: {} to {} {:.0f} yd by the route, leg to "
                             "({:.1f},{:.1f},{:.1f})", bot->GetName(), escort->GetName(), dist, leg.x, leg.y,
                             leg.z);
                }
                if (bot->GetExactDist(leg.x, leg.y, leg.z) > 1.0f &&
                    MoveTo(escort->GetMapId(), leg.x, leg.y, leg.z, false, false, false, false,
                           MovementPriority::MOVEMENT_NORMAL, true, false))
                    return true;
                if (bot->isMoving())
                    return true;
            }
        }
    }

    return MoveTo(escort->GetMapId(), x, y, escort->GetPositionZ(), false, false, false, false,
                  MovementPriority::MOVEMENT_NORMAL, true, false);
}

// [b42_ohb] A tank walking back to a waiting escort, out of combat: its walk through the dungeon, not a leash.
bool EscortStayCloseAction::WalksBackToWaitingEscort()
{
    if (bot->IsInCombat())
        return false;

    Creature* escort = EscortProtect::Escort(botAI);
    return escort && escort->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP);
}

bool EscortProtectAction::Execute(Event /*event*/)
{
    Creature* escort = EscortProtect::Escort(botAI);
    Unit* attacker = EscortProtect::EscortAttacker(botAI, escort);
    if (!attacker)
        return false;

    bool taunted = false;
    if (PlayerbotAI::IsTank(bot))
    {
        if (botAI->CanCastSpell("righteous defense", escort))
            taunted = botAI->CastSpell("righteous defense", escort);

        for (char const* taunt : TAUNTS)
        {
            if (taunted)
                break;
            if (botAI->CanCastSpell(taunt, attacker))
                taunted = botAI->CastSpell(taunt, attacker);
        }
    }

    LOG_INFO("playerbots", "[escort protect] {} ({}): {} on {} ({:.0f} % hp), {:.1f} yd, {}", bot->GetName(),
             PlayerbotAI::IsTank(bot) ? "tank" : "dps", attacker->GetName(), escort->GetName(),
             escort->GetHealthPct(), bot->GetExactDist(attacker), taunted ? "taunted" : "attack");

    return Attack(attacker) || taunted;
}

bool EscortHealAction::Execute(Event /*event*/)
{
    Creature* escort = EscortProtect::Escort(botAI);
    if (!escort)
        return false;

    for (char const* heal : HEALS)
    {
        // a heal over time already on the NPC is no new heal
        if ((!strcmp(heal, "renew") || !strcmp(heal, "rejuvenation")) && botAI->HasAura(heal, escort))
            continue;

        if (!botAI->CanCastSpell(heal, escort))
            continue;

        if (botAI->CastSpell(heal, escort))
        {
            LOG_INFO("playerbots", "[escort heal] {}: {} on {} ({:.0f} % hp)", bot->GetName(), heal, escort->GetName(),
                     escort->GetHealthPct());
            return true;
        }
    }
    return false;
}

void DungeonEscortStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Above the rotation and the generic target choice, below survival: losing the NPC ends the event
    triggers.push_back(new TriggerNode("escort npc attacked", {
        NextAction("escort protect", ACTION_RAID + 3) }));
    triggers.push_back(new TriggerNode("escort npc low health", {
        NextAction("escort heal", ACTION_MEDIUM_HEAL + 5) }));
    // The escorts with a leash (Thrall): the group walks with the NPC instead of fighting far from it, and goes back
    // to it when it waits at a checkpoint (after a death too).
    triggers.push_back(new TriggerNode("escort npc far", {
        NextAction("escort stay close", ACTION_MOVE + 4) }));
    // [b36_ohb] the healer into the tank's sight before the walking NPC meets the pack ahead (the tank pulls it)
    triggers.push_back(new TriggerNode("escort healer sight", {
        NextAction("escort healer sight", ACTION_MOVE + 5) }));
}

void DungeonEscortStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    Player* bot = botAI->GetBot();
    if (type != TargetValueExclusionType::DpsTarget || !bot->IsInCombat() || PlayerbotAI::IsTank(bot) ||
        PlayerbotAI::IsHeal(bot))
        return;

    Creature* escort = EscortProtect::Escort(botAI);
    if (!EscortProtect::EscortAttacker(botAI, escort))
        return;

    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const& guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || unit->GetVictim() != escort)
            exclusions.insert(guid);
    }
}
