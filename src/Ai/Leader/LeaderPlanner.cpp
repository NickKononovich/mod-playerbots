/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ChooseTargetActions.h"

#include <mutex>
#include <unordered_map>
#include "AggressiveTargetValue.h"
#include "Battleground.h"
#include "ChooseRpgTargetAction.h"
#include "ClassicShared.h"
#include "CreatureGroups.h"
#include "DamageTaken.h"
#include "DBCStores.h"
#include "EscortProtect.h"  // [b36_ohb] EscortWalksIntoPack
#include "EstimatedLifetimeValue.h"
#include "Event.h"
#include "Group.h"
#include "HealthTriggers.h"
#include "InstanceScript.h"
#include "KillOrder.h"
#include "LeaderKnowledge.h"  // [lead107 final] the door-cut registry
#include "LeaderOrders.h"  // [lead1] the tank's commit as a pull order (C:/WoW/LEADER_REFACTOR_PLAN.md step 1)
#include "LootObjectStack.h"
#include "Map.h"
#include "MobRoles.h"
#include "MotionMaster.h"
#include "NewRpgStrategy.h"
#include "ObjectMgr.h"
#include "PartyMemberToHeal.h"
#include "PartyStrengthValue.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "AKTriggers.h"
#include "ArcShared.h"  // [b34_arc] Arcatraz::IsCorpseTrap
#include "ANTriggers.h"
#include "CoSTriggers.h"  // [b39_cos] CoSHelpers::IsGuardianTimerRunning
#include "MgTShared.h"
#include "PossibleRpgTargetsValue.h"
#include "PullStrategy.h"
#include "PullActions.h"  // FightHold ([drift_fight_hold])
#include "PvpTriggers.h"
#include "PvpValues.h"
#include "RtiTargetValue.h"
#include "RtiValue.h"
#include "ServerFacade.h"
#include "SmartAI.h"
#include "SmartScriptMgr.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"  // [b33_aoepull] KnowsAnyRank
#include "TargetValue.h"
#include "TerrainHazard.h"
#include "WaypointMgr.h"
#include "World.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>   // [lead7]
#include <iterator>
#include <map>
#include <memory>  // [lead7]
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// [lead99] user: no PathGenerator in the leader's planning - the planner's walk checks read the one route search
// (Leader::Route) through PathGenerator's interface: PATHFIND_NORMAL a complete walk, PATHFIND_INCOMPLETE a partial
// one, PATHFIND_NOPATH none; the walk sampled on its ground (Leader::RouteResult)
namespace
{
    class LeaderPath
    {
    public:
        explicit LeaderPath(Unit* walker) : _walker(walker) {}

        void CalculatePath(float x, float y, float z, bool /*forceDest*/)
        {
            Take(Leader::Route(_walker, Position(x, y, z)));
        }
        void CalculatePath(float fromX, float fromY, float fromZ, float x, float y, float z, bool /*forceDest*/)
        {
            Take(Leader::Route(_walker, G3D::Vector3(fromX, fromY, fromZ), G3D::Vector3(x, y, z)));
        }
        uint32 GetPathType() const { return _type; }
        float getPathLength() const { return _length; }
        Movement::PointsArray const& GetPath() const { return _points; }

    private:
        void Take(Leader::RouteResult const& route)
        {
            _type = !route.found ? uint32(PATHFIND_NOPATH)
                                 : route.complete ? uint32(PATHFIND_NORMAL) : uint32(PATHFIND_INCOMPLETE);
            _length = route.length;
            _points.assign(route.points.begin(), route.points.end());
        }

        Unit* _walker;
        uint32 _type = PATHFIND_BLANK;
        float _length = 0.0f;
        Movement::PointsArray _points;
    };
}

// [lead31a] the planner's helpers, shared with the tank's execution (ChooseTargetActions.cpp)
namespace PullPlanning
{
}
using namespace PullPlanning;

// [lead7] The leader's planner library (C:/WoW/LEADER_REFACTOR_PLAN.md step 7, PullPlanner in ChooseTargetActions.h):
// a shadow look (LEADER_PLANNER_DIFF) writes no log line. LOG_INFO / LOG_DEBUG are redefined for this file only
// (after every include), so each of its log lines is skipped while a shadow look runs on this thread.
namespace PullPlanner
{
    namespace
    {
        thread_local bool shadowLook = false;
        // [lead3] the leader inputs of the library look running on this thread (LibraryLook binds them)
        thread_local LeaderInputs const* currentInputs = nullptr;
    }

    bool IsShadow() { return shadowLook; }
    LeaderInputs const* CurrentLeaderInputs() { return currentInputs; }
}

#undef LOG_INFO
#define LOG_INFO(filterType__, ...)                                                \
    do                                                                             \
    {                                                                              \
        if (!PullPlanner::IsShadow())                                              \
            LOG_MESSAGE_BODY(filterType__, LogLevel::LOG_LEVEL_INFO, __VA_ARGS__); \
    } while (0)

#undef LOG_DEBUG
#define LOG_DEBUG(filterType__, ...)                                                \
    do                                                                              \
    {                                                                               \
        if (!PullPlanner::IsShadow())                                               \
            LOG_MESSAGE_BODY(filterType__, LogLevel::LOG_LEVEL_DEBUG, __VA_ARGS__); \
    } while (0)

namespace PullReadiness
{
// [lead17b] LEADER_ONE_GATHER_OWNER: a led tank's planner checks (group ready to pull, topped up, healer in reach,
// buffs) read the leader's one readiness verdict (Leader::Readiness, TestHarnessMgr::LeaderReadyWhy) - no readiness
// rule of their own. False: not a led tank, or no fresh verdict (the old rules then).
bool LeaderVerdict(Player* bot, bool& ready, std::string* why)
{
    if (!Leader::LEADER_ONE_GATHER_OWNER || !Leader::LEADER_PLANNER_IN_LEADER || !bot ||
        !Leader::Orders::IsTracked(bot->GetGUID()))
        return false;
    Group* group = bot->GetGroup();
    return group && Leader::Readiness::Get(group->GetGUID(), ready, why);
}

bool GroupPreparing(Player* bot, std::string* why)
{
    constexpr float PREPARING_RANGE = 50.0f;  // RestState::GROUP_RANGE: the members the next fight is fought with
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || !member->IsInWorld() ||
            member->GetMap() != bot->GetMap() || member->IsInCombat() ||
            !bot->IsWithinDistInMap(member, PREPARING_RANGE))
            continue;

        if (PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member); memberAI && memberAI->forceRebuff.IsPending())
        {
            // [lead150] b170 356-428 s: "Rthunaa buffing" for 70 s at 1-7 % mana - a member below its own line
            // rests first (the leader's Rest; no buff under it), the label says so
            if (why)
                *why = member->GetName() + (RestState::BelowOwnLine(member) ? " rests below its line" : " buffing");
            return true;
        }

        // [lead85] b128 rfc-h 1092 s: "Rtshaab rests below its line (health 100 %, mana 100 %)" - WantsDrink counts the
        // leader's Rest (to 100 %), so the hold and the Rest held each other up to full. The member's own line
        if ((RestState::IsDrinking(member) || RestState::IsEating(member)) && RestState::BelowOwnLine(member))
        {
            if (why)
                *why = Acore::StringFormat("{} rests below its line (health {:.0f} %, mana {:.0f} %)",
                                           member->GetName(), member->GetHealthPct(),
                                           member->GetMaxPower(POWER_MANA) ? member->GetPowerPct(POWER_MANA) : 100.0f);
            return true;
        }
    }

    return false;
}
}  // namespace PullReadiness

namespace PullPlanning  // [lead31a] was anonymous: shared with the tank's execution
{
    // Resources the group needs for the next pull grow with how dangerous that pull is, so the group rests
    // only as long as the next fight needs instead of to full after every mob.
    constexpr float PULL_HEALTH_BASE = 40.0f;
    constexpr float PULL_HEALTH_PER_DANGER = 6.0f;
    constexpr float PULL_HEALTH_MIN = 50.0f;
    constexpr float PULL_HEALTH_MAX = 90.0f;
    constexpr float PULL_MANA_BASE = 15.0f;
    constexpr float PULL_MANA_PER_DANGER = 8.0f;
    constexpr float PULL_MANA_MIN = 25.0f;
    constexpr float PULL_MANA_MAX = 80.0f;
    constexpr float PACK_SOCIAL_RADIUS = 12.0f;        // idle mobs this close to the target join the fight
    constexpr float PACK_SOCIAL_DRIFT = 2.0f;          // CollectPack: the assistance radius plus the fight's drift
    constexpr float PACK_LEVEL_STEP = 0.15f;           // extra danger per level the mob has over the tank
    constexpr float PACK_BASE_CAPACITY = 1.5f;         // what a tank and no one else can take
    constexpr float PACK_CAPACITY_PER_MEMBER = 0.75f;
    // Dungeon trash is built for a full group: a level-appropriate 5-man takes a pack of 4-5 elites, while
    // the open-world numbers above would allow 1.5. Measured on the first heroic wave: every pack was refused.
    constexpr float PACK_DUNGEON_CAPACITY_SCALE = 3.0f;
    // Mob count on top of the danger sum: past about five mobs every further one is more AoE damage on the healer
    // and one more the tank has to keep off the casters, so a swarm of normals weighs more than its sum. Measured
    // in wave 3: Arcatraz and Shattered Halls tanks took 10-11 mobs at once and lost them to the dps.
    constexpr float PACK_COMFORT_MOBS = 5.0f;          // for a group matching the content (pull scale 1.0)
    constexpr float PACK_COMFORT_MOBS_MIN = 3.0f;
    constexpr float PACK_COMFORT_MOBS_MAX = 7.0f;
    constexpr float PACK_EXTRA_MOB_DANGER = 0.15f;     // the whole pack is this much harder per mob over comfort
    constexpr uint32 PACK_MAX_HELPERS = 4;
    constexpr float PULL_PATH_HEIGHT = 5.0f;           // a mob this far above/below the way in is on another floor
    // Tempo (heroic or max-level 5-man): the healer's mana is a budget of packs, and the tank chains pulls while
    // that budget lasts instead of waiting for everyone to drink up.
    // [lead258] the healer mana kept back for a pull that goes wrong is the config's low mana line (LowMana)
    constexpr float TEMPO_MANA_PER_DANGER = 4.0f;      // healer mana one point of pack danger costs
    constexpr float TEMPO_HEALTH_BASE = 30.0f;
    constexpr float TEMPO_HEALTH_PER_DANGER = 4.0f;
    constexpr float TEMPO_HEALTH_MIN = 40.0f;
    constexpr float TEMPO_HEALTH_MAX = 75.0f;
    constexpr float TEMPO_CAPACITY_BONUS = 0.25f;      // a well-stocked healer lets the tank take a bit more at once
    constexpr float TEMPO_FULL_MANA = 50.0f;           // mana above this earns the capacity bonus, in full at 100
    // Party strength against the content (PartyStrengthValue): a group too soft for the place drops back to the
    // safe rules, and one that clearly outgears any 5-man keeps the pace even off heroic and below max level.
    constexpr float TEMPO_MIN_PULL_SCALE = 0.85f;
    constexpr float TEMPO_OUTGEAR_PULL_SCALE = 1.25f;
    // Careful pulls (a levelling 5-man, outside tempo). Wave 4 (levels 15-58): tanks died under 8-15 mobs that came
    // from the whole room. Everything whose aggro range reaches the fight counts, the pack is capped by what the
    // tank's spells hold and by the melee the group can heal, and a crowded spot is pulled at range instead.
    constexpr float CAREFUL_AGGRO_MARGIN = 3.0f;         // the fight drifts a few yards around the mob
    constexpr float CAREFUL_PATROL_MARGIN = 10.0f;       // a patrol this far beyond its aggro range walks in
    // [b47_plan] A pull back is fought round its kill spot, not on it: the tank's fight hold keeps 15 yd round it, the
    // ranged step out of melee and the healer walks for sight. An idle static mob whose aggro reach comes within this
    // much more of the kill spot joins (the measured fight spread). brd-h 20261026-093620-brd-h-5B-6 pull 54: "one
    // ... Anvilrage Soldier, 6 mobs, cost 1.75" fought at (612.7,46.4), 25.6 yd from the idle Anvilrage Officer
    // 45845 (reach 24 = aggro 21 + 3): its pack (2 Medics, 2 Soldiers) joined, 11 engaged, the wipe at 2196-2210 s.
    constexpr float KILL_SPOT_DRIFT = 5.0f;
    constexpr float CAREFUL_EYE_HEIGHT = 2.0f;
    constexpr size_t FLEE_CHAIN_MAX_MOBS = 16;  // CollectPack stops following a flee-for-assistance chain here
    // User, 2026-10-24 («на склоне стояло 3 моба ... почему они считаются одним паком?»): a mob that flees for
    // assistance (SMART_ACTION_FLEE_FOR_ASSIST, mostly at 15 % health) brings its helper only if it runs there before
    // the group kills the health it has left: the run (distance / its run speed) under FLEE_REACH_MARGIN times the
    // kill (FLEE_HEALTH_SHARE of its maximum health at the group's estimated dps). A Ragefire Shaman (759-903 hp) has
    // 1.1-1.4 s left at 100 dps, so a helper counts within 14-16 yd (run 8 yd/s), a Defias Miner (386-417 hp) within
    // 7 yd; the 30 yd flee radius alone brought every pack within 30 yd of the fight (RFC: 12 mobs for 3 on a slope).
    constexpr float FLEE_HEALTH_SHARE = 0.15f;
    constexpr float FLEE_REACH_MARGIN = 1.5f;
    // [plan29_audit] 24 -> 16 (PULL_ESTIMATE_AUDIT 2026-10-25: estimates of 23-29 ran into the bound, 1-10 came)
    constexpr size_t PACK_CHAIN_MAX_MOBS = 16;  // and an assistance chain here (far over any cap: refused anyway)
    // A pack mob that stands and casts or shoots this far from the spot draws the fight to it: the tank and the
    // melee go to it, and what sees them there joins (wave 13 Shattered Halls, Magisters' Terrace, Razorfen Kraul).
    constexpr float CAREFUL_DRIFT_MIN_DISTANCE = 8.0f;
    // RFC 5B-1 2026-10-24 20:24:37 (wave 44): a Voidwalker Minion was counted as 22 mobs, the whole room and Jergosh
    // 79 yd away, through a chain "reaches #N, which keeps its range" -> "assist of #N" -> ...; 1 came. A mob that
    // keeps its range stands at most CASTER_KEEP_RANGE from the real fight spot (the kill spot) - one spawned
    // further walks in to that range - and draws the fight to where it stands then, not to its spawn (with the
    // core's Z gate, ReachesSpot). Every mob that really enters the fight calls its assistance in turn, as the core
    // does (Creature::CallAssistance on engage: Deadmines foundry, 12 counted, 20 came), so the chain stays
    // transitive; PACK_MAX_ASSIST_HOPS only bounds it (assist / draws-fight links from a mob that came by itself).
    constexpr float CASTER_KEEP_RANGE = 30.0f;
    constexpr uint8 PACK_MAX_ASSIST_HOPS = 6;
    // [lead260] a pack mob's summons are counted over the estimated fight (its scripted rate x the pack's kill time);
    // a summon the group kills within one global cooldown at its damage dies as it comes: it lands its melee over
    // its own kill time, not in the queue of the pack's kills (EstimatePack)
    constexpr float CAREFUL_MIN_HOLD = 2.0f;
    constexpr float CAREFUL_MAX_HOLD = 8.0f;
    // Mobs over what the tank's spells hold that the damage dealers still kill off it (the kill order takes them
    // first). Waves 10-12 levelling normals, tank kits of 2-3: 1-7% of pulls of 1-5 mobs lost someone, 8% at 6,
    // 15% at 7, 33% at 8.
    constexpr float CAREFUL_LOOSE_MOBS = 2.0f;
    // A pack refused this long with the group topped up, after its crowd control had its chance (PrePullCc holds a
    // pull up to 1.5 s for claims and 12 s for a Sap or a trap): the smallest plan found is pulled if it is within
    // the pack-size cap and the tank carries it (TankCarries), else a lone mob at range, else the pack is left for the
    // next one (wave 12: tanks stood for minutes before 2-mob packs). Waves 10-12: 7-8 mobs lost someone in 8-15%
    // of pulls, 12 and more in 17-21%, and the wipes came from 12 mobs and more.
    constexpr uint32 CAREFUL_STALL_MS = 20 * IN_MILLISECONDS;
    constexpr uint32 CAREFUL_LAST_RESORT_DEFAULT_CAP = 6;  // with Playerbots.CarefulPullMaxMobs 0
    // [lead263] A fallback or last-resort plan is held to what the tank and the healers carry (TankCarries: its drop
    // within the most the tank may drop, its healer mana within their pool) instead of the 0.8 / 0.9 / 1.0 / 3.0 tank
    // health caps (wave 13 Razorfen Kraul: a plan of 1.08 killed the tank; RFC 7 mobs at 1.82).
    // [careful_ready_last_resort] User rule 2026-10-25 (combat time as high as possible): a ready group (topped up,
    // nobody sitting below its own rest line) takes the last resort / the hard minimum on the first refused look when
    // its smallest plan is within the pack-size cap - the CAREFUL_STALL_MS wait stays for packs over the cap only. A
    // look that built no plan at all (cost 0, "best 0 mobs": every way in woke a mob, no danger estimate) goes to the
    // hard minimum at once too. st-n 2026-10-25 08:49: Nightmare Wanderer, 3 mobs at cost 0.95, stood 20 s + 11 s and
    // the fight left the tank at 92 %; 132 of the wave's 432 refusals were "at cost 0.00".
    constexpr bool CAREFUL_READY_LAST_RESORT = true;
    constexpr float CAREFUL_TANK_MANA = 35.0f;           // a paladin tank below this cannot taunt through a pull
    constexpr float CAREFUL_MIN_PULL_RANGE = 8.0f;
    constexpr float CHARGE_MIN_RANGE = 8.0f;   // Charge (warrior): 8-25 yd, out of combat ("[pull plan] ... charge")
    constexpr float CHARGE_MAX_RANGE = 25.0f;
    constexpr uint32 CAREFUL_PLAN_CACHE_MS = 1000;
    constexpr size_t CAREFUL_MAX_ALTERNATIVES = 6;
    constexpr size_t CAREFUL_LONE_MOB_CANDIDATES = 12;  // the lone-mob minimum looks at this many mobs of the pack
    constexpr uint32 CAREFUL_PULL_RETRY_MS = 20 * IN_MILLISECONDS;  // longer than the pull sequence may take
    // Expert rule (a mob that calls or flees for help at low health): it is pulled back far enough that its call
    // reaches nobody, and killed there. The pull-back fight's spot (where the tank waits after its ranged pull, the
    // kill spot) is tried this far behind the tank, along the line from the mob through it, up to KILL_SPOT_RETREATS
    // times, for at most KILL_SPOT_SEARCHES mobs a plan. Wailing Caverns wave 2 (level 19): Lady Anacondra's five
    // Deviate Guardians call for help in 40 yd at 15% health; every pull of one was counted with the whole room.
    constexpr float KILL_SPOT_RETREAT_STEP = 10.0f;
    constexpr uint32 KILL_SPOT_RETREATS = 3;
    constexpr uint32 KILL_SPOT_SEARCHES = 8;
    // The tank's own walk (to a mob it engages, to its cast spot, back to its kill spot) follows its navmesh path,
    // not the straight line: Wailing Caverns wave 2, an engage on a Deviate Ravager 94 yd away walked through
    // Anacondra's room. Paths up to this long are checked.
    constexpr float CAREFUL_MAX_WALK_CHECK = 150.0f;
    constexpr float WALK_SAMPLE_STEP = 2.0f;
    // User rule (hard minimum: the tank always takes one mob): the lone-mob minimum waits this long for a
    // single-target ranged pull (Hand of Reckoning off cooldown), then walks in on the mob. Looks come one stall
    // period apart; a wait not looked at for CAREFUL_LONE_MOB_WAIT_GAP_MS is from an earlier stand.
    // [careful_lone_wait_trim] 6 s, not the 20 s stall (st-n 2026-10-25: the "pull one" stood 7-11 s more for a
    // ranged pull): Hand of Reckoning (8 s cooldown) or a ranged weapon is back by then, else the tank walks in.
    constexpr uint32 CAREFUL_LONE_MOB_RANGED_WAIT_MS = 6 * IN_MILLISECONDS;
    constexpr uint32 CAREFUL_LONE_MOB_WAIT_GAP_MS = 3 * CAREFUL_STALL_MS;
    // User, 2026-10-24: «раз мы зашли в данж и предметы подходят - нет смысла стоять; минимум одного моба надо всё
    // равно пулить, если все условия хороши (все вместе, хил недалеко)». A group topped up, here and nobody sitting
    // takes the lone mob after this long refused (and waits this long for a ranged pull before it walks in), if the
    // tank carries the single (TankCarries) and the healer sees its fight; else the CAREFUL_STALL_MS path.
    constexpr uint32 CAREFUL_READY_ONE_MS = 3 * IN_MILLISECONDS;
    // A patrol this close whose path runs through the spot walks into the fight before it is over.
    constexpr float CAREFUL_PATROL_RANGE = 40.0f;  // the timeless test, for a patrol whose walk cannot be forecast
    // Patrol forecast (expert rule: a pull waits for the patrol, or takes it first). A patrol's walker (the creature
    // that walks the path: itself, or the formation leader it follows) is followed along its waypoints from where it
    // is now, one sample a second; a pack counts a patrol that comes within its aggro reach of the spot in the next
    // CAREFUL_PATROL_HORIZON s. Steam Vault wave 33 (normal): a Tidal Surger patrol (1 elite, 4 Steam Surgers) came
    // within 20-23 yd of the group's spot 12-24 s after the tank's last look and walked into it, 16 mobs, wipe.
    constexpr float PATROL_SAMPLE_SEC = 1.0f;
    constexpr float CAREFUL_PATROL_HORIZON = 30.0f;  // the pull's walk plus a usual fight
    constexpr float PATROL_GROUP_HORIZON = 15.0f;    // a patrol this close in time to the group is dealt with now
    constexpr float PATROL_IMMINENT_SEC = 3.0f;      // [patrol_meet_imminent] this close it is met, not held for
    constexpr float PATROL_MAX_WAIT_SEC = 45.0f;     // a patrol is waited out for at most this long
    constexpr float PATROL_MIN_FIGHT_SEC = 10.0f;
    constexpr float PATROL_MAX_FIGHT_SEC = 60.0f;
    constexpr float PATROL_BOSS_FIGHT_SEC = 60.0f;   // a boss leaves its own fight out of the estimate
    constexpr float PATROL_FIGHT_MARGIN = 1.25f;     // fights run longer than their estimate
    constexpr float PATROL_PULL_LEAD_SEC = 4.0f;     // the pull cast and the mob's walk to the tank
    constexpr float PATROL_TRACK_SEC = 120.0f;       // PATROL_MAX_WAIT_SEC and the longest fight with its lead
    constexpr float PATROL_MAX_FOLLOW = 10.0f;       // how far a formation member walks from its leader
    constexpr float PATROL_OFF_LEG = 8.0f;           // a walker this far off its leg is matched to the nearest one
    constexpr float PATROL_GROUP_RANGE = 40.0f;      // members this near the tank are the group's spot
    constexpr float PATROL_PACER_RADIUS = 12.0f;     // a walk within this of its middle paces beside its pack
    constexpr uint32 PATROL_LOS_CHECKS = 4;          // line-of-sight checks one forecast may spend
    constexpr uint32 PATROL_ROUTE_CACHE_MS = 30 * IN_MILLISECONDS;
    constexpr uint32 PATROL_TRACK_CACHE_MS = 1000;
    constexpr size_t PATROL_CACHE_MAX = 2048;
    constexpr uint32 PATROL_MAX_HOLD_MS = 60 * IN_MILLISECONDS;  // held this long for one patrol: planned as usual
    constexpr uint32 PATROL_HOLD_GAP_MS = 10 * IN_MILLISECONDS;  // no hold for it this long: its wait starts anew
    // User rule (packs stand apart at spawn): two mobs are one pack only if they stood together at spawn (one
    // formation, or spawn points within the radius that brings one with the other; a wanderer's wander radius on
    // top). A mob that moves (a waypoint patrol, a wanderer off its bound, a mob seen walking) and stands within
    // that radius of a pack only now is another group crossing it: it is not merged into the pack (CollectPack
    // split mode), and the pull waits while it is there and until it is OVERLAP_MARGIN past the radius, at most
    // OVERLAP_MAX_HOLD_MS, then the pull is planned as before (merged) for OVERLAP_GIVE_UP_MS ("[pull overlap]").
    // Two static mobs are never split (the calibrated assistance and flee chains run as before).
    constexpr float MOTION_MIN_TRAVEL = 1.0f;                     // moved this far over the window: a mover
    constexpr uint32 MOTION_SAMPLE_MS = 1000;                     // one position sample per mob at most this often
    constexpr uint32 MOTION_WINDOW_MS = 5 * IN_MILLISECONDS;      // samples kept this long
    constexpr size_t MOTION_SAMPLES = 6;
    constexpr size_t MOTION_CACHE_MAX = 8192;                     // one cache for every map thread
    constexpr float OVERLAP_MARGIN = 5.0f;                        // a crossing group is let go this far past it
    constexpr float OVERLAP_TREND_MIN = 0.5f;                     // a gap that changed less is steady
    // A crossing group that does not walk off (a wanderer inside its bound, a mover standing or closing in) never
    // separates: judged over at least this long a window, it is counted in the pack at once (planned merged), no hold.
    // Only a waypoint patrol (it walks on) or a mover whose gap grows is waited for. SFK wave 38: a Slavering Worg
    // wandering 1-7 yd inside a Moonwalker pack's 10 yd reach held the pull 24-41 s again and again.
    constexpr float OVERLAP_TREND_MIN_SEC = 2.0f;
    constexpr uint32 OVERLAP_MAX_HOLD_MS = uint32(PATROL_MAX_WAIT_SEC) * IN_MILLISECONDS;
    constexpr uint32 OVERLAP_HOLD_GAP_MS = PATROL_HOLD_GAP_MS;    // no hold this long: the wait starts anew
    constexpr uint32 OVERLAP_GIVE_UP_MS = 60 * IN_MILLISECONDS;   // planned merged this long after a wait ran out
    // [b36_sv] sv-h-5D-36 20261026-034959: a looping Coilfang Warrior patrol re-crossed the planned pack every
    // 15-25 s; each walk-off reset the hold clock ("held 0-25 s" for 8 minutes, OVERLAP_MAX_HOLD_MS never reached).
    // Holds that start again with no fight in between are counted: from the OVERLAP_MAX_CYCLES + 1-th the waypoint
    // patrol is pulled itself (if the group takes it alone), else the wait gives up (planned merged). A gap over
    // OVERLAP_CYCLE_WINDOW_MS since the last hold's start, or a fight, starts the count anew.
    constexpr uint32 OVERLAP_MAX_CYCLES = 3;
    constexpr uint32 OVERLAP_CYCLE_WINDOW_MS = 180 * IN_MILLISECONDS;
    // Measured careful pulls (Playerbots.CarefulPullHeroic: heroic or max-level 5-man). The mobs there are rescaled
    // at runtime (raid_rescale), so their level and rank say little: the pack is sized by its real melee and health
    // against the tank's real mitigation and the group's throughput. Waves 6-8: a heroic tank chain pulled 22-32
    // mobs in Stratholme and Scholomance and the group wiped, while packs of 3-5 went through. [lead257] The melee is
    // read through the game's own attack table and armor (MeleeTakenShare, ArmorReduction) and the spells from the
    // mob's spell data (GetMobSpells); the rescale's spell factor and what the spells really land on the tank (a
    // caster casts on others too, is interrupted and controlled) come in by the measured spell ratio of the dungeon.
    // [lead258] What the healers heal (rate, health per mana, the gap between two heals) is read from their spells
    // and the dungeon's measured healing per mana (RestState::HealerThroughput, GroupSupport).
    // Measured careful pulls: [lead259] (a) the tank survives: its deepest drop over the pull (the estimate's lives
    // against the healers' rate after their first heal, DropOver, times the dungeon's measured drop ratio) leaves it
    // the health for one more gap between two heals at the pack's rate (PackEstimate::maxDrop). The size the group
    // prefers when the pack can be split: a pull within half of it, which still fits when an equal pack joins. (The
    // wave 13 power law 0.146 / 0.164 cost^0.41 / 0.66 x 2.2, the 0.6 and the 0.28 lines are gone.)
    // (b) The healers keep up: [lead258] the pack's average damage on the tank (AverageDtps: the estimate's own
    // lives) within the healers' best rate together (HealerThroughput), and the healer mana it takes (TakenCost:
    // the calibrated cost) within their pool (HealerPool).
    // A pack at most this much of what the group takes is weak: it may be pulled with a damage dealer down.
    constexpr float CAREFUL_WEAK_PACK_OVERLOAD = 0.5f;
    // The tank waits this long at most for a buff the group can give before a pull of its own.
    constexpr uint32 PULL_BUFF_WAIT_MS = 10 * IN_MILLISECONDS;
    // And this long before a boss (BossReadyGap): bf-h batch6, the prot paladin never blessed the others and Keli'dan's
    // channelers were held "missing a Blessing" until the run was stopped.
    constexpr uint32 BOSS_BUFF_WAIT_MS = 30 * IN_MILLISECONDS;
    constexpr uint32 PULL_BUFF_CHECK_MS = 2 * IN_MILLISECONDS;
    // No pick in range this long with the group topped up: the tank walks toward the nearest possible target until
    // it is this far from it (inside the dungeon pick range of AggressiveTargetValue).
    constexpr uint32 APPROACH_STALL_MS = 20 * IN_MILLISECONDS;
    constexpr float APPROACH_DISTANCE = 30.0f;
    constexpr float APPROACH_MAX_HEIGHT = 12.0f;
    // No ask for this long (a fight, a target in range): the stall is counted anew.
    constexpr uint32 APPROACH_CALL_GAP_MS = 5 * IN_MILLISECONDS;
    // The approach walk enters no idle mob's aggro reach: the nearest candidates get a path, sampled every step.
    constexpr uint32 APPROACH_MAX_PATHS = 3;
    constexpr float APPROACH_SAMPLE_STEP = 2.0f;
    constexpr float APPROACH_AGGRO_MARGIN = 5.0f;
    // Pull check: a plan whose fight never started is reported after this long; a fight's attackers are kept up to
    // PULL_CHECK_MAX_MOBS.
    constexpr uint32 PULL_CHECK_TIMEOUT_MS = 30 * IN_MILLISECONDS;
    constexpr size_t PULL_CHECK_MAX_MOBS = 64;
    // The group's measured damage over its last checked fights (the health of the mobs it killed over the fight's
    // length), which the careful planner takes over the gear-table estimate when it is higher. zf-n wave 8 (level
    // 45): "estimated group dps" 269 against 550-1100 measured, so 3-mob packs were planned at 48-118 s and 1.6-6.7
    // tank healths (came: 13-27 s, 0.1-0.5 tank healths) and refused about 140 times; the run ended 0/8.
    // [lead256] every fight of this dungeon is kept (no clock: the samples belong to the group until the dungeon
    // changes) and the planner counts on the damage the group reached over 9 of 10 seconds of its fights (the lower
    // quantile of the fights' dps, weighted by their length, MEASURED_QUANTILE): the measured spread instead of a
    // fixed trust share, and a short fight weighs only its seconds instead of a minimum length.
    // The same for the damage the tank really takes against the template of what came (LandedMeleeDps): [lead209]
    // the ratios of this dungeon's fights (NoteMeasuredTaken), [lead256] melee and spells apart (DamageTaken::Spell),
    // each fight weighted by its template damage, from the first fight on, up as well as down; no floor, no safety
    // factor, no seed from another dungeon.
    // Side pulls (user rule: from one side a pack brings fewer mobs than from the other): cast spots around the pull
    // target at the pull's range, reached by a walk that enters no other idle mob's aggro reach (the test of the
    // harness approach: aggro range or attack distance, combat distance and a margin, and the mob sees the point).
    constexpr uint32 SIDE_PULL_ANGLES = 8;
    constexpr uint32 SIDE_PULL_MAX_PATHS = 4;
    constexpr float SIDE_PULL_RANGE_MARGIN = 2.0f;
    constexpr float SIDE_PULL_MAX_WALK = 60.0f;
    constexpr float SIDE_PULL_AGGRO_MARGIN = 5.0f;
    constexpr float SIDE_PULL_SAMPLE_STEP = 2.0f;
    constexpr float SIDE_PULL_ARRIVED = 2.0f;
    constexpr uint32 SIDE_PULL_RETRY_MS = 5 * IN_MILLISECONDS;
    constexpr uint32 SIDE_PULL_WALK_MS = 15 * IN_MILLISECONDS;  // the walk to a side spot is given up after this
    // A pack that cannot be split is still pulled, at its edge or at range, once the group is topped up, if it is
    // at most this much over what the group takes (a real group does not skip the pack in its way).
    constexpr float CAREFUL_FALLBACK_OVERLOAD = 1.5f;
    // The hard minimum's stand (lead110: the lone mob is judged, no cap escalates) while the tank stands topped up; the
    // stand ends with a fight or a gap this long without such a look (the tank walked on, the group sat down).
    constexpr uint32 CAREFUL_BLOCK_ESCALATE_MS = 30 * IN_MILLISECONDS;
    constexpr uint32 CAREFUL_BLOCK_GAP_MS = 15 * IN_MILLISECONDS;
    // The stand's guarantee: topped up and blocked this long, the lone mob is pulled whatever its cost if what it
    // brings stays within the pack-size cap (the last resort's), never with a boss. The cost model overstates
    // levelling packs 3-7 times (uk-n wave 9: 3 Dragonflayers planned at 18.1 tank healths, waves 7-8 took 0.35-1.1;
    // ramps-n, zf-n the same), and the walk guard of build 9 turned such refusals into stalls of the whole run; the
    // mob count is what kept the 25-mob pulls out.
    // [careful_guarantee_trim] 30 s within the cap (was 90), 90 s over it (was 180): DOWNTIME_AUDIT 2026-10-25, the
    // stand only delayed a pull the guarantee then took anyway (bot-n 235 s, hos-h 150 s, uk-h 107 s with the group
    // ready); the hard cap's escalation (x2 at 30 s, x3 at 60 s) still comes first for a costly lone mob.
    // [plan29_audit] 30 s -> 12 s (PULL_ESTIMATE_AUDIT: 660 of 747 refusals were pulled anyway, 98.8 % clean)
    constexpr uint32 CAREFUL_BLOCK_GUARANTEE_MS = 12 * IN_MILLISECONDS;
    constexpr size_t CASTER_LONE_MAX_MOBS = 3;  // loneMob: a caster's own pack this small is still a lone-mob pull
    constexpr uint32 REFUSAL_MEMO_MS = 60 * IN_MILLISECONDS;  // the last refusal in words is kept this long
    constexpr uint32 GUARANTEE_WALK_ROUNDS = 4;  // the guarantee's way in: this many woken packs are looked for
    // A pack over the cap but within GUARANTEE_OVER_CAP_SCALE of it is no dead end: it is taken after the longer
    // stand. The planner does not know the route; a hopeless verdict made the harness route round a pack that was
    // the only way on (seth-n wave 13: "the smallest pull (Time-Lost Controller at range) brings 10 mobs, over the
    // cap 7", the run stopped; waves 7-11 pulled that hall clean). Only past the scale it is hopeless (the
    // Scholomance hall: 18-23).
    constexpr uint32 CAREFUL_BLOCK_GUARANTEE_OVER_MS = 45 * IN_MILLISECONDS;  // [plan29_audit] 90 s -> 45 s
    constexpr float GUARANTEE_OVER_CAP_SCALE = 2.0f;  // [plan29_audit] 1.5 -> 2.0
    // The pack's stand (_packBlocked) is kept across a look away this long (the harness skips a refused pack for
    // 180 s and comes back to it), and the memo is pruned past this many mobs.
    constexpr uint32 PACK_BLOCKED_GAP_MS = 5 * MINUTE * IN_MILLISECONDS;
    constexpr size_t PACK_BLOCKED_MAX = 64;
    constexpr float CAREFUL_FALLBACK_HEALTH = 80.0f;
    constexpr float CAREFUL_FALLBACK_MANA = 80.0f;
    // Playerbots.CarefulPullMaxMobs scaled by the group's strength against the content, at least this many.
    constexpr uint32 CAREFUL_MIN_MAX_MOBS = 3;
    constexpr float CAREFUL_MAX_MOBS_SCALE_MIN = 0.9f;  // [plan29_audit] 0.6 -> 0.9: the cap of 10 stays at least 9
    constexpr float CAREFUL_MAX_MOBS_SCALE_MAX = 1.25f;
    // Line-of-sight pull of a caster pack (LosPull): rings of candidate spots around the cast spot, nearest first.
    constexpr float LOS_PULL_RING_MIN = 6.0f;
    constexpr float LOS_PULL_RING_MAX = 18.0f;
    constexpr float LOS_PULL_RING_STEP = 4.0f;
    constexpr uint32 LOS_PULL_ANGLES = 16;
    constexpr float LOS_PULL_MAX_PATH = 25.0f;        // walk from the cast spot to the tank's spot
    constexpr float LOS_PULL_FLOOR = 4.0f;            // a spot this far above/below the cast spot is another floor
    constexpr float LOS_PULL_GATHER_FLOOR = 2.5f;
    constexpr float LOS_PULL_EYE_HEIGHT = 2.0f;
    constexpr uint32 LOS_PULL_MAX_PATHS = 12;         // path searches one plan may spend
    constexpr uint32 LOS_PULL_FOUND_MEMO_MS = 3 * IN_MILLISECONDS;
    constexpr uint32 LOS_PULL_FAILED_MEMO_MS = 10 * IN_MILLISECONDS;
    constexpr float LOS_PULL_MEMO_MOVE = 3.0f;        // the tank moved this far: the cast spot changed, search again
    constexpr size_t LOS_PULL_MEMO_SIZE = 8;
    constexpr uint32 LOS_PULL_TEMPO_CACHE_MS = 1000;
    constexpr uint32 PRE_PULL_CC_RECHECK_MS = 2000;   // a pack not worth pre-pull crowd control, looked at again

    float MobDanger(Unit* mob, Player* tank)
    {
        float danger = 1.0f;
        if (Creature* creature = mob->ToCreature())
        {
            switch (creature->GetCreatureTemplate()->rank)
            {
                case CREATURE_ELITE_ELITE:
                    danger = 3.0f;
                    break;
                case CREATURE_ELITE_RAREELITE:
                    danger = 4.0f;
                    break;
                case CREATURE_ELITE_WORLDBOSS:
                    return 100.0f;
                default:
                    break;
            }
        }

        int32 const levelDiff = int32(mob->GetLevel()) - int32(tank->GetLevel());
        if (levelDiff > 0)
            danger *= 1.0f + PACK_LEVEL_STEP * levelDiff;

        return danger;
    }

    // Heroic, or a max-level tank, in a 5-man: the group is geared for pace, unless it is too soft for this
    // place. A group that clearly outgears the dungeon chains pulls in a levelling dungeon too. Everywhere else
    // (raids, the open world) the safe rules stay as they are.
    bool IsTempoMode(Player* bot, PartyStrength const& strength)
    {
        Map* map = bot->GetMap();
        if (!map || !map->IsNonRaidDungeon())
            return false;

        float const pullScale = strength.PullScale();
        if (pullScale < TEMPO_MIN_PULL_SCALE)
            return false;

        return map->IsHeroic() || bot->GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL) ||
               pullScale >= TEMPO_OUTGEAR_PULL_SCALE;
    }

    // [b30_topped_up] Members the group cannot wait for. dmw-n 20261025-184229-dmw-n-5C-16: after Immol'thar the
    // healer fell into a pocket of the pit with no path out ("stuck: needs path fix", 270-330 yd off); the tank held
    // "healer not with the group" -> "not topped up" for 385 s with all five at 100 % until the run was stopped.
    // Stranded: a healer on the map, alive, farther than STRANDED_RANGE and not getting STRANDED_PROGRESS closer for
    // STRANDED_MS (one walking back covers 100 yd in ~15 s). Not regenerating: a member short of the topped-up line
    // whose short value has not risen REGEN_MIN_GAIN points in REGEN_STALL_MS out of combat. Both are left out of the
    // presence / topped-up / healer-sight / support checks (the hold resolves), one log line per episode.
    namespace ToppedUpWatch
    {
        constexpr float STRANDED_RANGE = 100.0f;  // CAREFUL_FAR_MEMBER_RANGE
        constexpr float STRANDED_PROGRESS = 10.0f;
        // past the harness's stranded re-entry (STRANDED_REENTER_MS 75 s): the re-entry comes first in a test run
        constexpr uint32 STRANDED_MS = 120 * IN_MILLISECONDS;
        constexpr float REGEN_MIN_GAIN = 2.0f;
        constexpr uint32 REGEN_STALL_MS = 60 * IN_MILLISECONDS;
        constexpr uint32 STALE_MS = 30 * IN_MILLISECONDS;  // not looked at this long: the watch starts again

        enum Kind : uint8 { KIND_STRANDED, KIND_HEALTH, KIND_MANA, KIND_COUNT };
        struct Watch
        {
            uint32 sinceMs = 0;
            uint32 lastMs = 0;
            float best = 0.0f;  // stranded: the nearest distance seen; health / mana: the value at the last change
            bool logged = false;
        };

        std::mutex mutex;
        std::map<std::pair<uint64, uint64>, Watch> watches[KIND_COUNT];

        void Clear(Player* tank, Player* member, Kind kind)
        {
            std::lock_guard<std::mutex> lock(mutex);
            watches[kind].erase({ tank->GetGUID().GetRawValue(), member->GetGUID().GetRawValue() });
        }

        // How long `value` has made no progress (KIND_STRANDED: shrinking, else growing); 0 on a fresh watch.
        uint32 Observe(Player* tank, Player* member, Kind kind, float value, uint32 limitMs, bool& firstOver)
        {
            uint32 now = getMSTime();
            now = now ? now : 1;
            std::lock_guard<std::mutex> lock(mutex);
            Watch& watch = watches[kind][{ tank->GetGUID().GetRawValue(), member->GetGUID().GetRawValue() }];
            // health / mana: a drop (a new fight came between) starts a new watch as well
            bool const progress = kind == KIND_STRANDED ? value < watch.best - STRANDED_PROGRESS
                                                        : std::abs(value - watch.best) >= REGEN_MIN_GAIN;
            if (!watch.sinceMs || getMSTimeDiff(watch.lastMs, now) > STALE_MS || progress)
                watch = Watch{ now, now, value, false };
            watch.lastMs = now;
            uint32 const held = getMSTimeDiff(watch.sinceMs, now);
            firstOver = held >= limitMs && !watch.logged;
            watch.logged = watch.logged || firstOver;
            return held;
        }

        bool Stranded(Player* tank, Player* member)
        {
            if (member == tank || !PlayerbotAI::IsHeal(member) || member->GetMap() != tank->GetMap() ||
                !member->IsAlive() || tank->GetExactDist(member) <= STRANDED_RANGE)
            {
                Clear(tank, member, KIND_STRANDED);
                return false;
            }

            float const distance = tank->GetExactDist(member);
            bool firstOver = false;
            uint32 const held = Observe(tank, member, KIND_STRANDED, distance, STRANDED_MS, firstOver);
            if (firstOver)
                LOG_INFO("playerbots", "[topped_up_skip] {}: healer {} stranded {:.0f} yd off, no closer for {} s: "
                         "the group goes on without it", tank->GetName(), member->GetName(), distance,
                         held / IN_MILLISECONDS);
            return held >= STRANDED_MS;
        }

        // `value`: the member's health or mana percent, short of the topped-up line
        bool NotRegenerating(Player* tank, Player* member, Kind kind, float value)
        {
            if (member->IsInCombat())
            {
                Clear(tank, member, kind);
                return false;
            }

            bool firstOver = false;
            uint32 const held = Observe(tank, member, kind, value, REGEN_STALL_MS, firstOver);
            if (firstOver)
                LOG_INFO("playerbots", "[topped_up_skip] {}: {} at {:.0f} % {} has not regenerated for {} s: counted "
                         "as topped up", tank->GetName(), member->GetName(), value,
                         kind == KIND_HEALTH ? "health" : "mana", held / IN_MILLISECONDS);
            return held >= REGEN_STALL_MS;
        }
    }

    // Lowest mana among the group's living mana healers (the tank counts if it heals), or a negative value when
    // the group has none.
    float LowestHealerMana(Player* bot)
    {
        float lowest = -1.0f;
        auto const consider = [&lowest](Player* member)
        {
            if (!member->IsAlive() || !PlayerbotAI::IsHeal(member) || member->getPowerType() != POWER_MANA)
                return;

            float const mana = member->GetPowerPct(POWER_MANA);
            if (lowest < 0.0f || mana < lowest)
                lowest = mana;
        };

        consider(bot);
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                // [b30_topped_up] a stranded healer heals no fight here
                if (member && member != bot && member->GetMapId() == bot->GetMapId() &&
                    !ToppedUpWatch::Stranded(bot, member))
                    consider(member);
            }
        }

        return lowest;
    }

    // How many packs of this danger the weakest healer can still carry before it has to drink.
    float HealerPackBudget(float healerMana, float danger)
    {
        return std::max(0.0f, healerMana - float(sPlayerbotAIConfig.LowMana)) /
               (TEMPO_MANA_PER_DANGER * std::max(danger, 1.0f));
    }

    // The core's proximity aggro of an idle `mob` on `bot` standing at (x, y, z): MovementAction::IdleAggroBlocker,
    // the copy of Creature::CanStartAttack (its Z gate, then GetAggroRange + combat distance in 3D, plus `margin`),
    // without the line of sight (the callers test it where they need it). A unit that is no creature reaches
    // sPlayerbotAIConfig.AggroDistance. Wave 44: the planner's own 2D aggro tests counted a mob on a ledge 9 yd above
    // the fight, the core never wakes it.
    bool InCoreAggroReach(Unit* mob, Player* bot, float x, float y, float z, float margin)
    {
        if (Creature* creature = mob->ToCreature())
            return !MovementAction::IdleAggroBlocker(creature, bot, x, y, z, margin, false);

        return mob->GetExactDist(x, y, z) < sPlayerbotAIConfig.AggroDistance + margin;
    }

    bool InCoreAggroReach(Unit* mob, Player* bot, Position const& at, float margin)
    {
        return InCoreAggroReach(mob, bot, at.GetPositionX(), at.GetPositionY(), at.GetPositionZ(), margin);
    }

    // An idle mob close enough to the tank's way in is pulled on the way and comes back with the pack, through
    // the group. Height is compared too (the core's Z gate), so a mob on the floor above the corridor does not count.
    bool IsOnPullPath(Player* bot, Position const& target, Unit* mob)
    {
        float const ax = bot->GetPositionX();
        float const ay = bot->GetPositionY();
        float const dx = target.GetPositionX() - ax;
        float const dy = target.GetPositionY() - ay;
        float const lengthSq = dx * dx + dy * dy;
        float t = 0.0f;
        if (lengthSq > 0.0f)
            t = std::clamp(((mob->GetPositionX() - ax) * dx + (mob->GetPositionY() - ay) * dy) / lengthSq, 0.0f, 1.0f);

        float const px = ax + dx * t;
        float const py = ay + dy * t;
        float const pz = bot->GetPositionZ() + (target.GetPositionZ() - bot->GetPositionZ()) * t;
        // the core's proximity aggro needs the mob to see the walker (Creature::CanStartAttack): a pack behind the
        // wall beside the straight line is no pull path (the real walk is judged by WalkEntersAggro)
        return InCoreAggroReach(mob, bot, px, py, pz, 0.0f) && mob->IsWithinLOS(px, py, pz + CAREFUL_EYE_HEIGHT);
    }

    // A stronger group keeps more mobs on its tank at once, a weaker one fewer.
    float SwarmDanger(float danger, uint32 mobs, PartyStrength const& strength)
    {
        float const comfortMobs =
            std::clamp(PACK_COMFORT_MOBS * strength.PullScale(), PACK_COMFORT_MOBS_MIN, PACK_COMFORT_MOBS_MAX);
        if (float(mobs) > comfortMobs)
            danger *= 1.0f + PACK_EXTRA_MOB_DANGER * (float(mobs) - comfortMobs);

        return danger;
    }

    struct GroupSupport
    {
        uint32 helpers = 0;  // living members on the map besides the tank
        uint32 healers = 0;
        // [lead258] the healers' facts on this tank (RestState::HealerThroughput: their spells, spell power and the
        // dungeon's measured healing per mana)
        float healHps = 0.0f;  // health per second the healers heal at their best rate together
        float strongestHps = 0.0f;
        float healMana = 0.0f;     // health their full mana heals (max mana x health per mana, summed)
        float healReserve = 0.0f;  // health of the mana each keeps back (the config's low mana line)
        float healGapSec = 0.0f;   // the shortest time between two heals of one healer landing (0: no healer)
        float tankHealth = 0.0f;   // the health the healers heal (PlanTankHealth)
        std::string healerFacts;   // per healer, logged
    };

    float PlanTankHealth(Player* tank);

    // [lead259] The group's damage the game gives without a measured fight or a gear model: every member's weapon
    // (its min-max damage over its attack time, the better of melee and ranged) - the floor of the fight's length.
    float GroupWeaponDps(Player* bot)
    {
        auto const weapon = [](Player* member)
        {
            float best = 0.0f;
            for (WeaponAttackType type : {BASE_ATTACK, RANGED_ATTACK})
            {
                uint32 const attackTime = member->GetAttackTime(type);
                if (!attackTime)
                    continue;
                float const hit =
                    type == BASE_ATTACK
                        ? (member->GetFloatValue(UNIT_FIELD_MINDAMAGE) + member->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) /
                              2.0f
                        : (member->GetFloatValue(UNIT_FIELD_MINRANGEDDAMAGE) +
                           member->GetFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE)) /
                              2.0f;
                best = std::max(best, hit * float(IN_MILLISECONDS) / float(attackTime));
            }
            return best;
        };
        float dps = weapon(bot);
        if (Group* group = bot->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource();
                    member && member != bot && member->IsAlive() && member->GetMap() == bot->GetMap())
                    dps += weapon(member);
        return std::max(dps, 1.0f);
    }

    GroupSupport CountSupport(Player* bot)
    {
        GroupSupport support;
        support.tankHealth = PlanTankHealth(bot);
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || !member->IsAlive() || member->GetMapId() != bot->GetMapId() ||
                    ToppedUpWatch::Stranded(bot, member))  // [b30_topped_up] no support from a stranded healer
                    continue;

                ++support.helpers;
                if (!PlayerbotAI::IsHeal(member))
                    continue;

                ++support.healers;
                RestState::HealerFacts const facts = RestState::HealerThroughput(member, bot);
                support.healHps += facts.hps;
                support.strongestHps = std::max(support.strongestHps, facts.hps);
                support.healMana += facts.maxMana * facts.healPerMana;
                support.healReserve += facts.maxMana * float(sPlayerbotAIConfig.LowMana) / 100.0f * facts.healPerMana;
                if (facts.gapSec > 0.0f)
                    support.healGapSec =
                        support.healGapSec > 0.0f ? std::min(support.healGapSec, facts.gapSec) : facts.gapSec;
                support.healerFacts += Acore::StringFormat(
                    "{}{} {} {:.0f}/s every {:.1f} s, {:.2f} hp per mana ({}) x {:.0f} mana",
                    support.healerFacts.empty() ? "" : "; ", member->GetName(), facts.spell, facts.hps, facts.gapSec,
                    facts.healPerMana, facts.measured ? "measured" : "spell data", facts.maxMana);
            }
        }

        return support;
    }

    // Danger points (MobDanger sum) the group takes at once.
    float PackCapacity(Player* bot, PartyStrength const& strength, GroupSupport const& support)
    {
        bool const hasHealer = support.healers > 0;
        float capacity = PACK_BASE_CAPACITY + PACK_CAPACITY_PER_MEMBER * std::min(support.helpers, PACK_MAX_HELPERS);
        if (bot->GetMap() && bot->GetMap()->IsDungeon() && hasHealer)
            capacity *= PACK_DUNGEON_CAPACITY_SCALE;
        if (!hasHealer)
            capacity /= 2.0f;
        else if (IsTempoMode(bot, strength))
        {
            float const mana = LowestHealerMana(bot);
            float const stock = std::clamp((mana - TEMPO_FULL_MANA) / (100.0f - TEMPO_FULL_MANA), 0.0f, 1.0f);
            capacity *= 1.0f + TEMPO_CAPACITY_BONUS * stock;
        }

        return capacity * strength.PullScale();
    }

    // Danger of everything that comes with this target (its idle neighbours join in, mobs next to the way in are
    // dragged along), and whether the group can take that much at all. A group weaker than the content takes
    // smaller packs, a stronger one bigger (party strength pull scale, clamped 0.6..1.5).
    bool IsPackManageable(PlayerbotAI* botAI, Player* bot, Unit* target, PartyStrength const& strength, float& danger)
    {
        danger = MobDanger(target, bot);
        uint32 mobs = 1;
        for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets")->Get())
        {
            Unit* mob = botAI->GetUnit(guid);
            // a mob held by crowd control (a Sap before the pull) does not join in
            if (!mob || mob == target || !mob->IsAlive() || mob->IsInCombat() || !bot->IsHostileTo(mob) ||
                IsCrowdControlled(mob))
                continue;

            if (mob->GetDistance(target) <= PACK_SOCIAL_RADIUS || IsOnPullPath(bot, *target, mob))
            {
                danger += MobDanger(mob, bot);
                ++mobs;
            }
        }

        danger = SwarmDanger(danger, mobs, strength);
        return danger <= PackCapacity(bot, strength, CountSupport(bot));
    }

    // What a tank checks before a pull of the given danger: nobody dead, and everyone (the tank too) has the
    // health, and the healers the mana, that this particular fight needs. Someone still eating or drinking
    // only holds the pull while they are below that need. In tempo mode the healers' mana is read as a budget
    // of packs instead: the tank keeps pulling while it covers one more pack of this danger. A group weaker than
    // the content feels the pack as more dangerous and rests longer; a stronger one rests less. A measured careful
    // pull passes the healer mana this pack costs (`healerManaNeed`, percent) instead of either mana rule.
    // Why the tank makes no pull of its own now ("[pull gate] waiting: ..."), one line per tank per 10 s.
    void LogPullGate(Player* bot, std::string const& reason)
    {
        if (PullPlanner::IsShadow())
            return;  // [lead7] no slot of the tank's own lines

        static std::mutex mutex;
        static std::unordered_map<uint64, uint32> last;
        uint32 const now = getMSTime();
        {
            std::lock_guard<std::mutex> lock(mutex);
            uint32& at = last[bot->GetGUID().GetRawValue()];
            if (at && getMSTimeDiff(at, now) < 10 * IN_MILLISECONDS)
                return;
            at = now;
        }

        LOG_INFO("playerbots", "[pull gate] {}: waiting: {}", bot->GetName(), reason);
    }

    // A member who takes part in the next pull: alive, inside the instance with the tank. No resurrection-request
    // test: Player::ResurectUsingRequestData never clears the request, so an accepted resurrection left the healer
    // "dead" for the planner until it died again (ok-h wave 7: "healer dead" with the healer alive at 100 % mana).
    bool IsPresent(Player* bot, Player* member)
    {
        return member->GetMap() == bot->GetMap() && member->IsAlive();
    }

    // [careful_far_member] A member this far from the tank is not on its way to this pull (the los pull's
    // LOS_PULL_GATHER_FAR rule, PullStrategy.cpp): its health / mana and its sitting hold no topped-up check
    // (DOWNTIME_AUDIT 2026-10-25: ok-h 222 s, bf-h 141 s, ulda-h 126 s of holds for a member stuck 279 yd off).
    constexpr float CAREFUL_FAR_MEMBER_RANGE = 100.0f;
    bool IsFarMember(Player* bot, Player* member) { return bot->GetExactDist(member) > CAREFUL_FAR_MEMBER_RANGE; }

    // User rule: the group pulls and moves on only with its tank, a healer and at least one damage dealer alive and
    // here (per role, for a group with several). A dead dps of several does not hold the group; a dead healer, tank
    // or every dps does, until they are back (then the drink and topped-up checks follow).
    // `why` ([b30_topped_up]): the [pull gate] reason, for the planner's hold line
    bool IsGroupPresent(Player* bot, std::string* why = nullptr)
    {
        char const* missing = nullptr;
        if (!bot->IsAlive())
            missing = "tank";
        else if (Group* group = bot->GetGroup())
        {
            uint32 healers = 0;
            uint32 healersHere = 0;
            uint32 dps = 0;
            uint32 dpsHere = 0;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || PlayerbotAI::IsTank(member))
                    continue;

                bool const here = IsPresent(bot, member);
                if (PlayerbotAI::IsHeal(member))
                {
                    ++healers;
                    healersHere += here ? 1 : 0;
                }
                else
                {
                    ++dps;
                    dpsHere += here ? 1 : 0;
                }
            }

            if (healers && !healersHere)
                missing = "healer";
            else if (dps && !dpsHere)
                missing = "every dps";
        }

        if (missing)
        {
            if (why)
                *why = std::string(missing) + " dead";
            LogPullGate(bot, std::string(missing) + " dead");
            return false;
        }

        // Alive in the instance is not with the group (user, Stockade 2026-10-24: «танк убегает, не ждет основную
        // группу»): the healer within GATHER_HEALER_RANGE of the tank and at least one dps within GATHER_DPS_RANGE,
        // else the tank waits where it is and the group catches up.
        // [b32_strat_gate] a member the harness found cut off behind a closed door (Leader::Knowledge::SetDoorCut) holds every
        // pull, however near it stands: strat-n 20261025-221216-strat-n-5H-2, the healer 8 yd behind the shut Service
        // Entrance Gate counted "with the group" (GATHER_HEALER_RANGE) while it could neither heal nor come.
        // [lead10] LEADER_DOOR_INPUTS: on a leader look the leader's own door cut (LeaderInputs::doorCut), not the
        // door-cut registry the leader wrote for the tank's thread.
        PullPlanner::LeaderInputs const* const doorInputs =
            Leader::Leads(Leader::LEADER_DOOR_INPUTS) ? PullPlanner::CurrentLeaderInputs() : nullptr;
        auto const doorCut = [doorInputs](Player* member)
        {
            if (doorInputs)
                return std::find(doorInputs->doorCut.begin(), doorInputs->doorCut.end(), member->GetGUID()) !=
                       doorInputs->doorCut.end();
            return Leader::Knowledge::IsDoorCut(member->GetGUID());
        };
        if (Group* group = bot->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource();
                    member && member != bot && member->IsAlive() && doorCut(member))
                {
                    std::string const reason =
                        Acore::StringFormat("{} behind a closed door", member->GetName());
                    if (why)
                        *why = reason;
                    LogPullGate(bot, reason);
                    return false;
                }

        if (Group* group = bot->GetGroup())
        {
            constexpr float GATHER_HEALER_RANGE = 30.0f;
            // A healer a little farther that sees the tank heals it from where it stands: zf-n wave 7 and others, the
            // healer held back 30-39 yd by its follow's idle-aggro guard and the tank waited for it for good.
            constexpr float GATHER_HEALER_SIGHT_RANGE = 40.0f;  // its spells' range; HealerSees judges the spot
            constexpr float GATHER_DPS_RANGE = 35.0f;
            Player* farHealer = nullptr;
            Player* nearestDps = nullptr;
            bool healerNear = false;
            bool anyDps = false;
            bool dpsNear = false;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || PlayerbotAI::IsTank(member) || !IsPresent(bot, member))
                    continue;

                float const distance = bot->GetExactDist(member);
                if (PlayerbotAI::IsHeal(member))
                {
                    // [b30_topped_up] a healer stranded far off (no path back) holds no pull: the group goes on
                    if (ToppedUpWatch::Stranded(bot, member))
                        continue;

                    if (distance <= GATHER_HEALER_RANGE ||
                        (distance <= GATHER_HEALER_SIGHT_RANGE && member->IsWithinLOSInMap(bot)))
                        healerNear = true;
                    else if (!farHealer || distance < bot->GetExactDist(farHealer))
                        farHealer = member;
                    continue;
                }

                anyDps = true;
                dpsNear = dpsNear || distance <= GATHER_DPS_RANGE;
                if (!nearestDps || distance < bot->GetExactDist(nearestDps))
                    nearestDps = member;
            }

            if (farHealer && !healerNear)
            {
                std::string const reason = Acore::StringFormat("healer {} not with the group ({:.0f} yd)",
                                                               farHealer->GetName(), bot->GetExactDist(farHealer));
                if (why)
                    *why = reason;
                LogPullGate(bot, reason);
                return false;
            }

            if (anyDps && !dpsNear && nearestDps)
            {
                std::string const reason = Acore::StringFormat("no dps with the group (nearest {} at {:.0f} yd)",
                                                               nearestDps->GetName(), bot->GetExactDist(nearestDps));
                if (why)
                    *why = reason;
                LogPullGate(bot, reason);
                return false;
            }
        }

        // A member revived by the spirit healer has 25% of its stats for up to 10 minutes: a real group waits it
        // out (Scholomance wave 18: tank at 2287 of 5237 hp, wipe).
        constexpr uint32 SPELL_RESURRECTION_SICKNESS = 15007;
        std::vector<Player*> members = { bot };
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                if (Player* member = ref->GetSource(); member && member != bot && IsPresent(bot, member))
                    members.push_back(member);
            }
        }

        for (Player* member : members)
        {
            if (member->HasAura(SPELL_RESURRECTION_SICKNESS))
            {
                if (why)
                    *why = "resurrection sickness on " + member->GetName();
                LogPullGate(bot, Acore::StringFormat("resurrection sickness on {}", member->GetName()));
                return false;
            }
        }

        return true;
    }

    // Whether the group's healers see `spot` from where they stand now (one LoS query per healer): a fight there stays
    // in their sight, no healer is left behind a wall. True without a healer here. User, Stockade 2026-10-24: «танк
    // любит забегать за повороты и становится вне видимости хила».
    bool HealerSees(Player* bot, Position const& spot, Player** blind = nullptr)
    {
        constexpr float HEALER_SIGHT_RANGE = 40.0f;
        Group* group = bot->GetGroup();
        if (!group)
            return true;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            // [b30_topped_up] a stranded healer sees no spot of this fight
            if (!member || member == bot || !PlayerbotAI::IsHeal(member) || !IsPresent(bot, member) ||
                ToppedUpWatch::Stranded(bot, member))
                continue;

            if (member->GetExactDist(spot) > HEALER_SIGHT_RANGE ||
                !member->IsWithinLOS(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ()))
            {
                if (blind)
                    *blind = member;
                return false;
            }
        }

        return true;
    }

    bool IsEveryDpsPresent(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return true;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && !PlayerbotAI::IsTank(member) && !PlayerbotAI::IsHeal(member) &&
                !IsPresent(bot, member))
            {
                LogPullGate(bot, Acore::StringFormat("{} dead (a dangerous pack waits for every dps)",
                                                     member->GetName()));
                return false;
            }
        }

        return true;
    }

    // A buff some member of the group can give (Fortitude, the Mark, Intellect, a Blessing) and the tank's own
    // Righteous Fury: what the group's buff strategies put up between pulls.
    struct GroupBuff
    {
        uint8 casterClass;
        char const* spell;           // the caster's spell, as "spell id" names it
        char const* auras[4];        // any of these on the member counts (nullptr ends the list)
        bool manaOnly;
    };

    constexpr GroupBuff GROUP_BUFFS[] = {
        { CLASS_PRIEST, "power word: fortitude", { "Power Word: Fortitude", "Prayer of Fortitude", nullptr, nullptr },
          false },
        { CLASS_DRUID, "mark of the wild", { "Mark of the Wild", "Gift of the Wild", nullptr, nullptr }, false },
        { CLASS_MAGE, "arcane intellect", { "Arcane Intellect", "Arcane Brilliance", "Dalaran Brilliance",
                                            "Fel Intelligence" }, true },
    };

    bool HasAuraNamed(Unit* unit, char const* const* names, size_t count, bool prefix)
    {
        for (auto const& [spellId, application] : unit->GetAppliedAuras())
        {
            SpellInfo const* info = application && application->GetBase() ? application->GetBase()->GetSpellInfo()
                                                                           : nullptr;
            char const* auraName = info ? info->SpellName[0] : nullptr;
            if (!auraName)
                continue;

            std::string_view const name(auraName);
            for (size_t i = 0; i < count && names[i]; ++i)
            {
                if (prefix ? name.starts_with(names[i]) : name == names[i])
                    return true;
            }
        }

        return false;
    }

    bool Knows(Player* member, char const* spell)
    {
        PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
        return memberAI && memberAI->GetAiObjectContext()->GetValue<uint32>("spell id", spell)->Get();
    }

    // "<buff> on <member>" for the first buff the group can give that a member here lacks; empty when none.
    std::string MissingGroupBuff(Player* bot)
    {
        static char const* const blessings[] = { "Blessing of", "Greater Blessing of" };
        std::vector<Player*> members;
        members.push_back(bot);
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member != bot && IsPresent(bot, member))
                    members.push_back(member);
            }
        }

        bool canBless = false;
        bool provides[std::size(GROUP_BUFFS)] = {};
        for (Player* member : members)
        {
            canBless = canBless || (member->getClass() == CLASS_PALADIN && Knows(member, "blessing of might"));
            for (size_t i = 0; i < std::size(GROUP_BUFFS); ++i)
                provides[i] = provides[i] ||
                              (member->getClass() == GROUP_BUFFS[i].casterClass && Knows(member, GROUP_BUFFS[i].spell));
        }

        for (Player* member : members)
        {
            for (size_t i = 0; i < std::size(GROUP_BUFFS); ++i)
            {
                GroupBuff const& buff = GROUP_BUFFS[i];
                if (provides[i] && (!buff.manaOnly || member->getPowerType() == POWER_MANA) &&
                    !HasAuraNamed(member, buff.auras, std::size(buff.auras), false))
                    return Acore::StringFormat("{} on {}", buff.auras[0], member->GetName());
            }

            if (canBless && !HasAuraNamed(member, blessings, std::size(blessings), true))
                return Acore::StringFormat("a Blessing on {}", member->GetName());
        }

        static char const* const fury[] = { "Righteous Fury" };
        if (bot->getClass() == CLASS_PALADIN && Knows(bot, "righteous fury") &&
            !HasAuraNamed(bot, fury, std::size(fury), false))
            return Acore::StringFormat("Righteous Fury on {}", bot->GetName());

        return std::string();
    }

    // [b32_engage] bf-h-5H-84 20261025-223902 pull 18: the harness logged pull_without_healer (the priest alive,
    // 48.6 yd off, drinking where the tank left it), the planned body pull went anyway, 7 mobs came, the healer had
    // no sight of the tank in half the samples and the tank died with the healer at 90 % mana. A pull waits while a
    // healer near the group (not stuck far off: IsFarMember) stands farther than HEALER_PULL_REACH from the tank or
    // out of its sight, at most HEALER_REACH_HOLD_MAX_MS per wait (a healer that cannot get there).
    bool HealerOutOfReach(Player* bot, std::string* why)
    {
        // [lead17b] the leader's heal-reach invariant and readiness own this
        if (bool ready = false; PullReadiness::LeaderVerdict(bot, ready, nullptr))
            return false;
        constexpr float HEALER_PULL_REACH = 30.0f;
        constexpr uint32 HEALER_REACH_HOLD_MAX_MS = 30 * IN_MILLISECONDS;
        thread_local std::unordered_map<ObjectGuid, uint32> holdStarts;  // by tank
        Group* group = bot->GetGroup();
        if (!group || bot->IsInCombat() || !PlayerbotAI::IsTank(bot) || !bot->GetMap()->IsNonRaidDungeon())
            return false;

        Player* away = nullptr;
        for (GroupReference* ref = group->GetFirstMember(); ref && !away; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && IsPresent(bot, member) && PlayerbotAI::IsHeal(member) &&
                !PlayerbotAI::IsTank(member) && !IsFarMember(bot, member) &&
                (bot->GetExactDist(member) > HEALER_PULL_REACH || !bot->IsWithinLOSInMap(member)))
                away = member;
        }

        uint32 const now = getMSTime();
        if (!away)
        {
            holdStarts.erase(bot->GetGUID());
            return false;
        }

        uint32& start = holdStarts[bot->GetGUID()];
        if (!start)
            start = now ? now : 1;
        if (getMSTimeDiff(start, now) >= HEALER_REACH_HOLD_MAX_MS)
            return false;

        // [b36_ohb] A walking escort does not wait for the healer: the tank takes the pack ahead of the NPC now, the
        // healer comes into its sight ("escort healer sight"). ohb-h 20261026-034959-ohb-h-5H-17 1476 s: "pull one
        // ... Durnholde Sentry" held on "healer Rtpriaf 10.0 yd from the tank, out of sight" while Thrall walked into
        // that pack; the healer's renew on Thrall started the fight instead (priest dead, wipe).
        if (PlayerbotAI* tankAI = GET_PLAYERBOT_AI(bot))
        {
            std::string escortWhy;
            if (EscortProtect::EscortWalksIntoPack(tankAI, &escortWhy))
            {
                static thread_local std::unordered_map<ObjectGuid, uint32> escortLogMs;
                uint32& logged = escortLogMs[bot->GetGUID()];
                if (!logged || getMSTimeDiff(logged, now) >= 10 * IN_MILLISECONDS)
                {
                    logged = now ? now : 1;
                    LOG_INFO("playerbots", "[b36_ohb] pull gate: {} does not wait for healer {} ({:.1f} yd{}): {}",
                             bot->GetName(), away->GetName(), bot->GetExactDist(away),
                             bot->IsWithinLOSInMap(away) ? "" : ", out of sight", escortWhy);
                }
                return false;
            }
        }

        if (why)
            *why = Acore::StringFormat("healer {} {:.1f} yd from the tank{}", away->GetName(), bot->GetExactDist(away),
                                       bot->IsWithinLOSInMap(away) ? "" : ", out of sight");
        return true;
    }

    bool IsGroupReadyToPull(Player* bot, float packDanger, PartyStrength const& strength,
                            float healerManaNeed = -1.0f)
    {
        // [lead17b] the leader's one readiness verdict
        if (bool ready = false; PullReadiness::LeaderVerdict(bot, ready, nullptr))
            return ready;
        // [combat_no_damage_release] a member fights and nobody has hit anything for a while: no rest hold
        if (GroupDamage::NoDamageRelease(bot))
            return true;

        if (!IsGroupPresent(bot))
            return false;

        float const danger = packDanger / strength.PullScale();
        bool const tempo = IsTempoMode(bot, strength);
        bool const measured = healerManaNeed >= 0.0f;
        float const needHealth = tempo ? std::clamp(TEMPO_HEALTH_BASE + TEMPO_HEALTH_PER_DANGER * danger,
                                                    TEMPO_HEALTH_MIN, TEMPO_HEALTH_MAX)
                                       : std::clamp(PULL_HEALTH_BASE + PULL_HEALTH_PER_DANGER * danger,
                                                    PULL_HEALTH_MIN, PULL_HEALTH_MAX);
        float const needMana = std::clamp(PULL_MANA_BASE + PULL_MANA_PER_DANGER * danger, PULL_MANA_MIN, PULL_MANA_MAX);

        if (bot->GetHealthPct() < needHealth)
        {
            LogPullGate(bot, Acore::StringFormat("tank health {:.0f} % of {:.0f} %", bot->GetHealthPct(), needHealth));
            return false;
        }

        if (measured)
        {
            // a group too soft for tempo also keeps the levelling rest rule
            // At most PULL_MANA_MAX: a healer above its drink threshold does not drink, and a need above what it
            // keeps held the group for good (Utgarde Keep wave 26: healer 84 %, a 1-mob plan, 4 minutes).
            float const need = std::min(tempo ? healerManaNeed : std::max(healerManaNeed, needMana), PULL_MANA_MAX);
            float const healerMana = LowestHealerMana(bot);
            if (healerMana >= 0.0f && healerMana < need)
            {
                LogPullGate(bot, Acore::StringFormat("healer mana {:.0f} % of {:.0f} %", healerMana, need));
                return false;
            }
        }
        else if (tempo)
        {
            float const healerMana = LowestHealerMana(bot);
            if (healerMana >= 0.0f && HealerPackBudget(healerMana, danger) < 1.0f)
                return false;
        }

        Group* group = bot->GetGroup();
        if (!group)
            return true;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            // the dead and the absent are judged by IsGroupPresent above
            if (!member || member == bot || !IsPresent(bot, member))
                continue;

            if (member->GetHealthPct() < needHealth)
            {
                LogPullGate(bot, Acore::StringFormat("{} health {:.0f} % of {:.0f} %", member->GetName(),
                                                     member->GetHealthPct(), needHealth));
                return false;
            }

            if (!tempo && !measured && PlayerbotAI::IsHeal(member) && member->getPowerType() == POWER_MANA &&
                member->GetPowerPct(POWER_MANA) < needMana)
            {
                LogPullGate(bot, Acore::StringFormat("{} mana {:.0f} % of {:.0f} %", member->GetName(),
                                                     member->GetPowerPct(POWER_MANA), needMana));
                return false;
            }
        }

        // [pull_waits_for_sitters] A member drinking or eating below its own rest line, or still buffing, is not
        // ready: the tank does not pull over it. scholo-h w25 (20261025-132016-scholo-h-5A-29, pull 6): the tank's
        // line-of-sight pull started 3 s after the last fight while the mage and the hunter sat down to drink at
        // 59 / 67 %; the drink broke after 7 s, the group went in at 61 % mana on average, the fight grew to 19 mobs.
        std::string preparing;
        if (PullReadiness::GroupPreparing(bot, &preparing))
        {
            LogPullGate(bot, preparing);
            return false;
        }

        // [b32_engage] the healer within reach and sight of the tank (HealerOutOfReach)
        if (HealerOutOfReach(bot, &preparing))
        {
            LogPullGate(bot, preparing);
            return false;
        }

        return true;
    }

    enum class PullMode : uint8
    {
        Default,   // the open world, raids, and 5-man tempo with Playerbots.CarefulPullHeroic off: as before
        Careful,   // a levelling 5-man below the pace of tempo mode, sized by rank and the levelling constants
        Measured,  // a heroic or max-level 5-man: careful planning, sized by the mobs' and the group's real numbers
    };

    // Heroic or max-level pace: a heroic or max-level 5-man, or a levelling group that outgears its dungeon. Its
    // casters are read at the rescale's spell multiplier.
    bool IsHeroicPace(Player* bot, PartyStrength const& strength)
    {
        return bot->GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL) || IsTempoMode(bot, strength);
    }

    // A levelling 5-man below the pace of tempo mode: the tank pulls the way a careful player does at that level,
    // sized by the mobs' and the group's real numbers too with Playerbots.CarefulPullMeasuredLevelling. A heroic or
    // max-level 5-man (and a levelling group that outgears its dungeon) plans its pulls the same way when
    // Playerbots.CarefulPullHeroic is on, with tempo's resting kept; never a raid (IsNonRaidDungeon).
    PullMode GetPullMode(Player* bot, PartyStrength const& strength)
    {
        Map* map = bot->GetMap();
        if (!map || !map->IsNonRaidDungeon())
            return PullMode::Default;

        if (!IsHeroicPace(bot, strength))
            return sPlayerbotAIConfig.CarefulPullMeasuredLevelling ? PullMode::Measured : PullMode::Careful;

        return sPlayerbotAIConfig.CarefulPullHeroic ? PullMode::Measured : PullMode::Default;
    }

    bool IsBossMob(Unit* unit)
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        return creature && (creature->IsDungeonBoss() || creature->isWorldBoss());
    }

    // [pack_skip_sealed_mob] An idle mob a script holds out of any fight (immune to players, not attackable, not
    // selectable: a banished boss, an encounter that has not started) neither aggroes nor answers a call for help, so
    // no pull brings it. slabs-h 2026-10-25 (20261025-115805-slabs-h-5A-38): Ambassador Hellmaw stays banished and
    // ImmuneToAll while his Cabal Ritualists live (boss_ambassador_hellmaw.cpp InitializeAI, TYPE_RITUALISTS); the
    // planner counted him in every ritualist pack ("refused for now: the pack of Cabal Ritualist brings a boss, pulled
    // as his fight") and the ritualists - the only way to free him - were never pulled: stalled 796 s.
    bool IsSealedFromFight(Unit* unit)
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        return creature && !creature->IsInCombat() &&
               (creature->IsImmuneToPC() || creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) ||
                creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE));
    }

    // [rest_topup_real_boss] A boss mob that is an encounter of its map's DungeonEncounter.dbc list (its kill credit,
    // or a row named after it: spell-credited encounters, the other faction's twin) - not a mini-boss / named trash
    // flagged as a dungeon boss. A map without encounter rows counts every flagged boss. Cached per map thread by
    // (map, entry): the lists are static data.
    bool IsEncounterBoss(Unit* unit)
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature || !IsBossMob(creature))
            return false;
        if (creature->isWorldBoss())
            return true;

        Map* map = creature->GetMap();
        if (!map)
            return true;

        uint32 const mapId = map->GetId();
        uint32 const entry = creature->GetEntry();
        uint64 const key = (uint64(mapId) << 32) | entry;
        static thread_local std::unordered_map<uint64, bool> cache;  // per map thread
        auto const cached = cache.find(key);
        if (cached != cache.end())
            return cached->second;

        bool rows = false;
        bool listed = false;
        // Classic and TBC maps run as heroics here; their encounter lists are the normal mode's.
        uint32 const difficulties[] = {uint32(map->GetDifficulty()), 0};
        for (uint32 difficulty : difficulties)
        {
            DungeonEncounterList const* list = sObjectMgr->GetDungeonEncounterList(mapId, Difficulty(difficulty));
            if (!list || list->empty())
                continue;

            rows = true;
            for (DungeonEncounter const* encounter : *list)
                listed = listed || (encounter && encounter->creditType == ENCOUNTER_CREDIT_KILL_CREATURE &&
                                    encounter->creditEntry == entry);
            if (listed)
                break;
        }

        if (!listed)
        {
            auto const lower = [](std::string text)
            {
                while (!text.empty() && text.back() == ' ')
                    text.pop_back();
                std::transform(text.begin(), text.end(), text.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return text;
            };
            std::string const name = lower(creature->GetName());
            for (uint32 row = 0; row < sDungeonEncounterStore.GetNumRows() && !listed; ++row)
            {
                DungeonEncounterEntry const* encounterRow = sDungeonEncounterStore.LookupEntry(row);
                if (!encounterRow || encounterRow->mapId != mapId)
                    continue;

                rows = true;
                char const* rowName = encounterRow->encounterName[LOCALE_enUS];
                std::string const encounterName = lower(rowName ? rowName : "");
                listed = name.size() >= 4 && encounterName.size() >= 4 &&
                         (name.find(encounterName) != std::string::npos ||
                          encounterName.find(name) != std::string::npos);
            }
        }

        bool const result = listed || !rows;
        constexpr size_t ENCOUNTER_BOSS_CACHE_MAX = 4096;
        if (cache.size() >= ENCOUNTER_BOSS_CACHE_MAX)
            cache.clear();
        cache[key] = result;
        return result;
    }

    bool PackHasEncounterBoss(PlayerbotAI* botAI, GuidVector const& mobs)
    {
        return std::any_of(mobs.begin(), mobs.end(),
                           [botAI](ObjectGuid const& guid) { return IsEncounterBoss(botAI->GetUnit(guid)); });
    }

    bool IsIdleEnemy(Player* bot, Unit* mob)
    {
        return mob && mob->IsInWorld() && mob->IsAlive() && !mob->IsInCombat() && mob->GetMap() == bot->GetMap() &&
               bot->IsHostileTo(mob);
    }

    float SegmentDistance2d(Position const& spot, float ax, float ay, float bx, float by)
    {
        float const dx = bx - ax;
        float const dy = by - ay;
        float const lengthSq = dx * dx + dy * dy;
        float t = 0.0f;
        if (lengthSq > 0.0f)
            t = std::clamp(((spot.GetPositionX() - ax) * dx + (spot.GetPositionY() - ay) * dy) / lengthSq, 0.0f, 1.0f);

        return spot.GetExactDist2d(ax + dx * t, ay + dy * t);
    }

    // A patrol's route: the path it walks itself (creature_addon path, or the one its SmartAI started), else its
    // formation leader's, which it follows. `walker`: the creature that walks it. Looked up once per creature and
    // PATROL_ROUTE_CACHE_MS on a map thread; the path itself is fetched again by id (a reload replaces it).
    struct PatrolRoute
    {
        uint32 pathId = 0;
        bool smart = false;  // a SmartAI path (sSmartWaypointMgr)
        ObjectGuid walker;
        uint32 ms = 0;
    };

    PatrolRoute FindPatrolRoute(Creature* creature)
    {
        PatrolRoute route;
        for (uint8 hop = 0; creature && hop < 2; ++hop)
        {
            // A creature_addon path is walked only by a waypoint mover (MotionMaster::InitDefault): Dire Maul East
            // wave 6, Hydrospawn (MovementType 1, wander 5, with an addon path) was counted as a patrol walking into
            // a Wildspawn Imp pack 33 yd away and held it as a boss pull for minutes.
            bool const walksOwnPath =
                creature->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE ||
                creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE;
            if (uint32 const pathId = walksOwnPath ? creature->GetWaypointPath() : 0)
            {
                if (sWaypointMgr->GetPath(pathId))
                {
                    route.pathId = pathId;
                    route.walker = creature->GetGUID();
                    return route;
                }
            }

            if (SmartAI* ai = dynamic_cast<SmartAI*>(creature->AI()))
            {
                if (uint32 const pathId = ai->GetScript()->GetPathId())
                {
                    bool const smart = sSmartWaypointMgr->GetPath(pathId) != nullptr;
                    if (smart || sWaypointMgr->GetPath(pathId))
                    {
                        route.pathId = pathId;
                        route.smart = smart;
                        route.walker = creature->GetGUID();
                        return route;
                    }
                }
            }

            // Only a member that follows its leader walks its path (CreatureGroup::LeaderStartedMoving moves members
            // with GROUP_AI_FLAG_FOLLOW_LEADER only). Ramparts wave 6 (Rlpalay, blocked 229 s): two Bonechewer
            // Destroyers linked to the pacing Beastmaster (groupAI 3, no follow) stand still, yet counted as a patrol
            // they got CAREFUL_PATROL_MARGIN on their aggro reach and joined a Scryer pack 28-31 yd away.
            CreatureGroup const* formation = creature->GetFormation();
            Creature* leader = formation ? formation->GetLeader() : nullptr;
            bool follows = false;
            if (formation && leader && leader != creature)
            {
                auto const& members = formation->GetMembers();
                auto const own = members.find(creature);
                follows = own != members.end() &&
                          own->second.HasGroupFlag(uint16(GroupAIFlags::GROUP_AI_FLAG_FOLLOW_LEADER));
            }

            creature = follows ? leader : nullptr;
        }

        return route;
    }

    // The path a patrol walks, and (optional) who walks it: the creature itself or the formation leader it follows.
    WaypointPath const* PatrolPath(Creature* creature, Creature** walkerOut = nullptr)
    {
        if (walkerOut)
            *walkerOut = nullptr;

        thread_local std::unordered_map<uint64, PatrolRoute> cache;
        uint32 const now = getMSTime();
        // [lead185] by instance too: the instances of one map share their creatures' GUIDs (8 parallel runs)
        uint64 const key =
            creature->GetGUID().GetRawValue() ^ (uint64(creature->GetInstanceId()) * 0x9E3779B97F4A7C15ULL);
        auto found = cache.find(key);
        if (found == cache.end() || getMSTimeDiff(found->second.ms, now) >= PATROL_ROUTE_CACHE_MS)
        {
            if (found == cache.end() && cache.size() >= PATROL_CACHE_MAX)
                cache.clear();

            PatrolRoute route = FindPatrolRoute(creature);
            route.ms = now;
            found = cache.insert_or_assign(key, route).first;
        }

        PatrolRoute const& route = found->second;
        if (!route.pathId)
            return nullptr;

        WaypointPath const* path =
            route.smart ? sSmartWaypointMgr->GetPath(route.pathId) : sWaypointMgr->GetPath(route.pathId);
        if (!path || !walkerOut)
            return path;

        if (route.walker == creature->GetGUID())
            *walkerOut = creature;
        else if (CreatureGroup const* formation = creature->GetFormation())
        {
            Creature* leader = formation->GetLeader();
            *walkerOut = leader && leader->GetGUID() == route.walker ? leader : nullptr;
        }

        return path;
    }

    // The patrol's path passes within `reach` of the spot, on its floor (the timeless test, for a patrol whose walk
    // cannot be forecast now).
    bool PathCrossesSpot(Creature* creature, Position const& spot, float reach)
    {
        WaypointPath const* path = PatrolPath(creature);
        if (!path || path->Nodes.empty())
            return false;

        std::vector<WaypointNode> const& nodes = path->Nodes;
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            WaypointNode const& from = nodes[i];
            WaypointNode const& to = nodes[(i + 1) % nodes.size()];
            if (std::fabs(from.Z - spot.GetPositionZ()) > PULL_PATH_HEIGHT &&
                std::fabs(to.Z - spot.GetPositionZ()) > PULL_PATH_HEIGHT)
                continue;

            if (SegmentDistance2d(spot, from.X, from.Y, to.X, to.Y) <= reach)
                return true;
        }

        return false;
    }

    // Where the walker will be, one sample every PATROL_SAMPLE_SEC from now to PATROL_TRACK_SEC: from where it stands
    // to the node after the last it reached (WaypointMovementGenerator: Creature::GetCurrentWaypointID), then node
    // after node round its path, at its walk or run speed, with each node's delay. A walker that stands at a node now
    // is read as leaving it now (it comes no later than forecast).
    void BuildPatrolTrack(Creature* walker, WaypointPath const& path, std::vector<G3D::Vector3>& out)
    {
        out.clear();
        std::vector<WaypointNode> const& nodes = path.Nodes;
        size_t const count = nodes.size();
        size_t const samples = size_t(PATROL_TRACK_SEC / PATROL_SAMPLE_SEC) + 1;
        G3D::Vector3 position(walker->GetPositionX(), walker->GetPositionY(), walker->GetPositionZ());
        if (count < 2)
        {
            out.assign(samples, position);
            return;
        }

        Position const here = walker->GetPosition();
        auto const legDistance = [&nodes, count, &here](size_t to)
        {
            WaypointNode const& from = nodes[(to + count - 1) % count];
            return SegmentDistance2d(here, from.X, from.Y, nodes[to].X, nodes[to].Y);
        };

        size_t next = count;
        uint32 const reached = walker->GetCurrentWaypointID();
        for (size_t i = 0; i < count && reached; ++i)
        {
            if (nodes[i].Id == reached)
            {
                next = (i + 1) % count;
                break;
            }
        }

        if (next == count || legDistance(next) > PATROL_OFF_LEG)
        {
            float nearest = -1.0f;
            for (size_t i = 0; i < count; ++i)
            {
                float const distance = legDistance(i);
                if (nearest < 0.0f || distance < nearest)
                {
                    nearest = distance;
                    next = i;
                }
            }
        }

        float const walk = std::max(walker->GetSpeed(MOVE_WALK), 0.5f);
        float const run = std::max(walker->GetSpeed(MOVE_RUN), 0.5f);
        float time = 0.0f;
        float sampleAt = 0.0f;
        out.reserve(samples);
        // every leg at least once round the path; a path of zero length and no delays ends the walk
        for (size_t legs = 0; out.size() < samples && legs < count * 64; ++legs)
        {
            WaypointNode const& node = nodes[next];
            G3D::Vector3 const target(node.X, node.Y, node.Z);
            float const speed = node.Velocity > 0.0f
                                    ? node.Velocity
                                    : (node.MoveType == WAYPOINT_MOVE_TYPE_RUN ? run : walk);
            float const travel = (target - position).length() / speed;
            for (; sampleAt <= time + travel && out.size() < samples; sampleAt += PATROL_SAMPLE_SEC)
                out.push_back(travel > 0.0f ? position + (target - position) * ((sampleAt - time) / travel) : target);

            time += travel;
            position = target;
            float const delay = float(node.Delay) / float(IN_MILLISECONDS);
            for (; sampleAt <= time + delay && out.size() < samples; sampleAt += PATROL_SAMPLE_SEC)
                out.push_back(position);

            time += delay;
            next = (next + 1) % count;
        }

        out.resize(samples, position);
    }

    // The walker's track (BuildPatrolTrack), kept for PATROL_TRACK_CACHE_MS per walker on a map thread; none while it
    // does not walk its path (a fight, a script that holds it). The pointer is good until the next call.
    std::vector<G3D::Vector3> const* PatrolTrack(Creature* walker, WaypointPath const* path)
    {
        if (!walker || !path || !walker->IsAlive() || walker->IsInCombat() || walker->IsInEvadeMode() ||
            walker->GetMotionMaster()->GetCurrentMovementGeneratorType() != WAYPOINT_MOTION_TYPE)
            return nullptr;

        struct Track
        {
            uint32 ms = 0;
            std::vector<G3D::Vector3> samples;
        };

        thread_local std::unordered_map<uint64, Track> cache;
        uint32 const now = getMSTime();
        uint64 const key =  // [lead185] by instance too
            walker->GetGUID().GetRawValue() ^ (uint64(walker->GetInstanceId()) * 0x9E3779B97F4A7C15ULL);
        auto found = cache.find(key);
        if (found == cache.end())
        {
            if (cache.size() >= PATROL_CACHE_MAX)
                cache.clear();

            found = cache.emplace(key, Track()).first;
        }
        else if (getMSTimeDiff(found->second.ms, now) < PATROL_TRACK_CACHE_MS)
            return &found->second.samples;

        found->second.ms = now;
        BuildPatrolTrack(walker, *path, found->second.samples);
        return &found->second.samples;
    }

    bool PatrolForecast(Creature* mob, std::vector<G3D::Vector3>& track, float& sampleSec)
    {
        Creature* walker = nullptr;
        WaypointPath const* path = mob ? PatrolPath(mob, &walker) : nullptr;
        std::vector<G3D::Vector3> const* forecast = path ? PatrolTrack(walker, path) : nullptr;
        if (!forecast || forecast->empty())
            return false;

        track = *forecast;
        sampleSec = PATROL_SAMPLE_SEC;
        return true;
    }

    // For each of the first `samples` samples of the track: the patrol is within `reach` of one of `points`, on its
    // floor, and sees it there (checked once per stretch in reach, at its closest sample, PATROL_LOS_CHECKS at most:
    // a stretch past them counts as seen).
    std::vector<bool> PatrolInReach(Map* map, uint32 phaseMask, std::vector<G3D::Vector3> const& track, float reach,
                                    std::vector<Position> const& points, size_t samples)
    {
        samples = std::min(samples, track.size());
        std::vector<bool> flags(samples, false);
        std::vector<int32> closest(samples, -1);  // the point each sample is nearest to, within reach
        std::vector<float> nearest(samples, 0.0f);
        for (size_t i = 0; i < samples; ++i)
        {
            G3D::Vector3 const& at = track[i];
            for (size_t p = 0; p < points.size(); ++p)
            {
                Position const& point = points[p];
                if (std::fabs(at.z - point.GetPositionZ()) > PULL_PATH_HEIGHT)
                    continue;

                float const distance = point.GetExactDist2d(at.x, at.y);
                if (distance <= reach && (closest[i] < 0 || distance < nearest[i]))
                {
                    closest[i] = int32(p);
                    nearest[i] = distance;
                }
            }

            flags[i] = closest[i] >= 0;
        }

        uint32 checks = PATROL_LOS_CHECKS;
        for (size_t start = 0; start < samples && map;)
        {
            if (!flags[start])
            {
                ++start;
                continue;
            }

            size_t end = start;
            size_t best = start;
            for (; end < samples && flags[end]; ++end)
            {
                if (nearest[end] < nearest[best])
                    best = end;
            }

            if (checks)
            {
                --checks;
                G3D::Vector3 const& at = track[best];
                Position const& point = points[closest[best]];
                if (!map->isInLineOfSight(at.x, at.y, at.z + CAREFUL_EYE_HEIGHT, point.GetPositionX(),
                                          point.GetPositionY(), point.GetPositionZ() + CAREFUL_EYE_HEIGHT, phaseMask,
                                          LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                    std::fill(flags.begin() + start, flags.begin() + end, false);
            }

            start = end;
        }

        return flags;
    }

    int32 FirstInReach(std::vector<bool> const& flags, size_t samples)
    {
        for (size_t i = 0; i < std::min(samples, flags.size()); ++i)
        {
            if (flags[i])
                return int32(i);
        }

        return -1;
    }

    // The fewest samples to wait (at most `maxWait`) for a stretch of `window` samples with the patrol out of reach
    // all along it; -1 when there is none within the forecast.
    int32 PatrolWait(std::vector<bool> const& flags, size_t window, size_t maxWait)
    {
        for (size_t wait = 0; wait <= maxWait && wait + window <= flags.size(); ++wait)
        {
            bool clear = true;
            for (size_t i = wait; i < wait + window && clear; ++i)
                clear = !flags[i];

            if (clear)
                return int32(wait);
        }

        return -1;
    }

    // A creature that walks a patrol (its own waypoints, or a formation leader's) or is on waypoint movement now.
    bool IsPatrolling(Creature* creature)
    {
        return creature->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE ||
               creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE ||
               PatrolPath(creature);
    }

    // An idle mob whose aggro range reaches the spot the tank fights or casts at (and sees it), a wanderer whose
    // wander radius round its spawn brings it there, or a patrol that walks into it: one already close, or one whose
    // forecast walk (PatrolTrack) comes within its aggro reach of the spot in the next CAREFUL_PATROL_HORIZON s (the
    // timeless test of a path through the spot up to CAREFUL_PATROL_RANGE away when its walk cannot be forecast).
    bool IsPacer(std::vector<G3D::Vector3> const& track);

    // `drift` ([b47_plan]): extra yards of the fight's spread round `spot` (a pull back's kill spot), for a static mob.
    bool ReachesSpot(Player* bot, Unit* mob, Position const& spot, float drift = 0.0f)
    {
        Creature* creature = mob->ToCreature();
        if (!creature || !creature->HasReactState(REACT_AGGRESSIVE))
            return false;

        // the core's reach (MovementAction::IdleAggroReach: aggro range and combat distance, 3D) and Z gate
        float const aggroReach = MovementAction::IdleAggroReach(creature, bot, CAREFUL_AGGRO_MARGIN);
        MovementGeneratorType const movement = creature->GetDefaultMovementType();
        bool const patrol = IsPatrolling(creature);
        float const distance = mob->GetExactDist(spot);
        // Ramparts wave 6: a Bonechewer Beastmaster pacing a 7 yd box (IsPacer) 20-33 yd from a Scryer pack got the
        // patrol margin wherever it stood in its box and joined the pack on most looks. A pacer's forecast track
        // (below) already says where it walks: its reach where it stands now is its aggro reach alone.
        bool const pacer = patrol && [&]()
        {
            Creature* walker = nullptr;
            WaypointPath const* path = PatrolPath(creature, &walker);
            std::vector<G3D::Vector3> const* track = PatrolTrack(walker, path);
            return track && IsPacer(*track);
        }();
        if (MovementAction::PassesAggroZGate(creature, bot, spot.GetPositionZ()))
        {
            float reach = aggroReach;
            if (patrol && !pacer)
                reach += CAREFUL_PATROL_MARGIN;
            else if (movement == RANDOM_MOTION_TYPE)
                reach += creature->GetWanderDistance();
            else
                reach += drift;  // [b47_plan]

            bool const wanders = !patrol && movement == RANDOM_MOTION_TYPE &&
                                 creature->GetHomePosition().GetExactDist2d(spot.GetPositionX(), spot.GetPositionY()) <=
                                     aggroReach + creature->GetWanderDistance();
            if ((distance <= reach || wanders) &&
                mob->IsWithinLOS(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ() + CAREFUL_EYE_HEIGHT))
                return true;
        }

        if (!patrol)
            return false;

        Creature* walker = nullptr;
        WaypointPath const* path = PatrolPath(creature, &walker);
        if (std::vector<G3D::Vector3> const* track = PatrolTrack(walker, path))
        {
            float const follow = std::min(mob->GetExactDist2d(walker), PATROL_MAX_FOLLOW);
            std::vector<bool> const flags =
                PatrolInReach(bot->GetMap(), bot->GetPhaseMask(), *track, aggroReach + follow, {spot},
                              size_t(CAREFUL_PATROL_HORIZON / PATROL_SAMPLE_SEC) + 1);
            return FirstInReach(flags, flags.size()) >= 0;
        }

        return std::fabs(mob->GetPositionZ() - spot.GetPositionZ()) <= PULL_PATH_HEIGHT &&
               distance <= CAREFUL_PATROL_RANGE && PathCrossesSpot(creature, spot, aggroReach);
    }

    // The idle members of one patrol, by the creature that walks its path, with its forecast walk.
    struct PatrolGroup
    {
        Creature* walker = nullptr;
        std::vector<Unit*> members;
        Unit* nearest = nullptr;  // the member nearest to the tank
        float reach = 0.0f;       // its members' aggro reach, plus how far they walk from the walker
        std::vector<G3D::Vector3> track;
    };

    // A track that stays within PATROL_PACER_RADIUS of its middle: a mob pacing beside its pack, not a patrol that
    // walks in from elsewhere (it is counted with the pack where it stands).
    bool IsPacer(std::vector<G3D::Vector3> const& track)
    {
        if (track.empty())
            return true;

        G3D::Vector3 middle(0.0f, 0.0f, 0.0f);
        for (G3D::Vector3 const& at : track)
            middle += at;

        middle /= float(track.size());
        for (G3D::Vector3 const& at : track)
        {
            if ((at - middle).length() > PATROL_PACER_RADIUS)
                return false;
        }

        return true;
    }

    // The patrols among the possible targets that walk in from elsewhere, each with its idle members.
    std::vector<PatrolGroup> FindPatrols(PlayerbotAI* botAI, Player* bot, GuidVector const& possible)
    {
        std::vector<PatrolGroup> patrols;
        std::vector<Creature*> pacers;
        for (ObjectGuid const& guid : possible)
        {
            Creature* mob = botAI->GetCreature(guid);
            if (!mob || !IsIdleEnemy(bot, mob) || !mob->HasReactState(REACT_AGGRESSIVE) || IsCrowdControlled(mob) ||
                IsBossMob(mob))
                continue;

            Creature* walker = nullptr;
            WaypointPath const* path = PatrolPath(mob, &walker);
            if (!walker || std::find(pacers.begin(), pacers.end(), walker) != pacers.end())
                continue;

            auto group = std::find_if(patrols.begin(), patrols.end(),
                                      [walker](PatrolGroup const& patrol) { return patrol.walker == walker; });
            if (group == patrols.end())
            {
                std::vector<G3D::Vector3> const* track = PatrolTrack(walker, path);
                if (!track)
                    continue;

                if (IsPacer(*track))
                {
                    pacers.push_back(walker);
                    continue;
                }

                PatrolGroup patrol;
                patrol.walker = walker;
                patrol.track = *track;
                patrols.push_back(std::move(patrol));
                group = patrols.end() - 1;
            }

            group->members.push_back(mob);
            group->reach = std::max(group->reach, MovementAction::IdleAggroReach(mob, bot, CAREFUL_AGGRO_MARGIN) +
                                                      std::min(mob->GetExactDist2d(walker), PATROL_MAX_FOLLOW));
            if (!group->nearest || bot->GetExactDist(mob) < bot->GetExactDist(group->nearest))
                group->nearest = mob;
        }

        return patrols;
    }

    // Where the group stands: the tank and every member present near it.
    std::vector<Position> GroupPoints(Player* bot)
    {
        std::vector<Position> points = {bot->GetPosition()};
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member != bot && member->GetMap() == bot->GetMap() && member->IsAlive() &&
                    bot->GetExactDist(member) <= PATROL_GROUP_RANGE)
                    points.push_back(member->GetPosition());
            }
        }

        return points;
    }

    // "[pull hold] <tank>: patrol <name> approaches (<yd>, eta <s>) ..." and "[pull patrol] ...", one line per tank
    // per 5 s; `channel` 1: the "[pull overlap]" lines, limited on their own.
    void LogPatrol(Player* bot, std::string const& line, uint32 channel = 0)
    {
        if (PullPlanner::IsShadow())
            return;  // [lead7] no slot of the tank's own lines

        static std::mutex mutex;
        static std::unordered_map<uint64, uint32> last;
        uint32 const now = getMSTime();
        {
            std::lock_guard<std::mutex> lock(mutex);
            uint32& at = last[(bot->GetGUID().GetRawValue() << 1) | (channel & 1)];
            if (at && getMSTimeDiff(at, now) < 5 * IN_MILLISECONDS)
                return;
            at = now;
        }

        LOG_INFO("playerbots", "{}", line);
    }

    // How a mob moves out of combat (user rule: packs stand apart at spawn, see MOTION_MIN_TRAVEL).
    enum class MobMotion : uint8
    {
        Static,
        Patrol,    // walks waypoints (its own, or a formation leader's)
        Wanderer,  // random movement round its spawn point
        Moving     // none of these, but seen walking (a script, a return home)
    };

    char const* MobMotionName(MobMotion motion)
    {
        switch (motion)
        {
            case MobMotion::Patrol:
                return "patrol";
            case MobMotion::Wanderer:
                return "wanderer";
            case MobMotion::Moving:
                return "moving";
            default:
                return "static";
        }
    }

    // The positions a mob was seen at over the last MOTION_WINDOW_MS, one sample per MOTION_SAMPLE_MS at most,
    // oldest first (the careful planner samples its possible targets once per plan). An entry from another map or
    // instance (the same guid elsewhere) starts anew. One cache for every map thread, behind a lock: a map is updated
    // by whichever worker thread is free, and the per-thread caches it had saw a mob once in a few ticks - SFK wave
    // 38 logged "steady (+0.0 yd in 0 s)" for a wanderer and a patrol, their trend never had two samples. Only
    // positions are stored (read on the caller's own map thread); the copy returned holds no reference to the cache.
    struct MotionTrack
    {
        uint32 mapId = 0;
        uint32 instanceId = 0;
        std::vector<std::pair<uint32, G3D::Vector3>> samples;
    };

    std::mutex motionLock;
    std::unordered_map<uint64, MotionTrack> motionCache;

    MotionTrack SampleMotion(Creature* creature)
    {
        std::lock_guard<std::mutex> guard(motionLock);
        std::unordered_map<uint64, MotionTrack>& cache = motionCache;
        uint32 const now = getMSTime();
        // the same guid in two copies of one instance (two test groups) is two tracks
        uint64 const key =
            creature->GetGUID().GetRawValue() ^ (uint64(creature->GetInstanceId()) * 0x9E3779B97F4A7C15ULL);
        auto found = cache.find(key);
        if (found == cache.end())
        {
            if (cache.size() >= MOTION_CACHE_MAX)
                cache.clear();

            found = cache.emplace(key, MotionTrack()).first;
        }

        MotionTrack& track = found->second;
        if (track.mapId != creature->GetMapId() || track.instanceId != creature->GetInstanceId())
        {
            track.samples.clear();
            track.mapId = creature->GetMapId();
            track.instanceId = creature->GetInstanceId();
        }

        if (!track.samples.empty() && getMSTimeDiff(track.samples.back().first, now) < MOTION_SAMPLE_MS)
            return track;

        auto const stale = std::find_if(track.samples.begin(), track.samples.end(),
                                        [now](std::pair<uint32, G3D::Vector3> const& sample)
                                        { return getMSTimeDiff(sample.first, now) <= MOTION_WINDOW_MS; });
        track.samples.erase(track.samples.begin(), stale);
        if (track.samples.size() >= MOTION_SAMPLES)
            track.samples.erase(track.samples.begin());

        track.samples.emplace_back(
            now, G3D::Vector3(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ()));
        return track;
    }

    MobMotion MotionOf(Creature* creature)
    {
        if (IsPatrolling(creature))
            return MobMotion::Patrol;

        if (creature->GetDefaultMovementType() == RANDOM_MOTION_TYPE && creature->GetWanderDistance() > 0.0f)
            return MobMotion::Wanderer;

        MotionTrack const track = SampleMotion(creature);
        G3D::Vector3 const& now = track.samples.back().second;
        for (auto const& sample : track.samples)
        {
            if ((sample.second - now).length() > MOTION_MIN_TRAVEL)
                return MobMotion::Moving;
        }

        return MobMotion::Static;
    }

    // How the mob's distance to `anchor` changed over its samples (negative: it closes in), and over how long.
    float DistanceTrend(Creature* creature, Position const& anchor, float& seconds)
    {
        MotionTrack const track = SampleMotion(creature);
        auto const& [oldMs, old] = track.samples.front();
        seconds = float(getMSTimeDiff(oldMs, track.samples.back().first)) / float(IN_MILLISECONDS);
        return creature->GetExactDist2d(anchor.GetPositionX(), anchor.GetPositionY()) -
               anchor.GetExactDist2d(old.x, old.y);
    }

    // Where the mob was spawned (its creature row; a summon: its home), with its wander radius.
    Position SpawnPoint(Creature* creature, float* wander = nullptr)
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float distance = 0.0f;
        creature->GetRespawnPosition(x, y, z, nullptr, &distance);
        if (wander)
            *wander = creature->GetDefaultMovementType() == RANDOM_MOTION_TYPE ? distance : 0.0f;

        return Position(x, y, z);
    }

    // A group that crosses a pack now: its mob, the pack side it is near (a mob of the pack, or the spot) within
    // `reach` (the radius that would bring it), and which of the two moves.
    struct PackOverlap
    {
        Unit* outside = nullptr;    // the mob of the other group
        Unit* member = nullptr;     // nullptr: the spot
        Creature* mover = nullptr;  // `outside` or `member`
        Position anchor;            // where the side that does not move stands
        float reach = 0.0f;
        MobMotion motion = MobMotion::Static;
    };

    // `mob` is near `bringer` (a pack mob; nullptr: the spot at `anchor`) within `radius` now, one of them moves,
    // and they did not stand together at spawn: not one formation, nor spawn points within `radius` (a wanderer's
    // wander radius on top: within its bound it may stand there at any time, and counts as before).
    bool SeparateAtSpawn(Unit* bringer, Position const& anchor, Unit* mob, float radius, PackOverlap& out)
    {
        Creature* creature = mob->ToCreature();
        Creature* other = bringer ? bringer->ToCreature() : nullptr;
        if (!creature || radius <= 0.0f || (bringer && !other))
            return false;

        MobMotion const motion = MotionOf(creature);
        MobMotion const otherMotion = other ? MotionOf(other) : MobMotion::Static;
        if (motion == MobMotion::Static && otherMotion == MobMotion::Static)
            return false;

        if (other)
        {
            CreatureGroup const* formation = creature->GetFormation();
            if (formation && formation == other->GetFormation())
                return false;
        }

        float wander = 0.0f;
        float otherWander = 0.0f;
        Position const spawn = SpawnPoint(creature, &wander);
        Position const otherSpawn = other ? SpawnPoint(other, &otherWander) : anchor;
        if (std::fabs(spawn.GetPositionZ() - otherSpawn.GetPositionZ()) <= PULL_PATH_HEIGHT &&
            spawn.GetExactDist2d(otherSpawn) <= radius + wander + otherWander)
            return false;

        bool const moves = motion != MobMotion::Static;
        out.outside = mob;
        out.member = bringer;
        out.mover = moves ? creature : other;
        out.anchor = moves ? anchor : mob->GetPosition();
        out.reach = radius;
        out.motion = moves ? motion : otherMotion;
        return true;
    }

    // The tank's walk from `from` to `to`, along the navmesh path it walks, enters the aggro reach (+ `margin`) of an
    // idle aggressive mob that is not in `pack` and sees it there: the first such mob, else nullptr. Patrols are left
    // to the plan's patrol forecast. `walkable`: false when no complete path of at most `maxLength` yd was found
    // (nothing judged).
    Creature* WalkEntersAggro(PlayerbotAI* botAI, Player* bot, GuidVector const& possible, Position const& from,
                              Position const& to, std::vector<Unit*> const& pack, float maxLength, float margin,
                              bool& walkable)
    {
        walkable = false;
        LeaderPath path(bot);  // [lead99]
        path.CalculatePath(from.GetPositionX(), from.GetPositionY(), from.GetPositionZ(), to.GetPositionX(),
                           to.GetPositionY(), to.GetPositionZ(), false);
        if (path.GetPathType() != PATHFIND_NORMAL || path.getPathLength() > maxLength)
            return nullptr;

        walkable = true;
        float const walked = path.getPathLength();
        std::vector<std::pair<Creature*, float>> reaches;
        for (ObjectGuid const& guid : possible)
        {
            Creature* mob = botAI->GetCreature(guid);
            if (!mob || !IsIdleEnemy(bot, mob) || !mob->HasReactState(REACT_AGGRESSIVE) || IsPatrolling(mob) ||
                IsCrowdControlled(mob) || std::find(pack.begin(), pack.end(), mob) != pack.end())
                continue;

            float const reach = MovementAction::IdleAggroReach(mob, bot, margin);  // the core's (CanStartAttack)
            // nowhere on the walk can be nearer to it than this
            if (mob->GetExactDist(from) > walked + reach)
                continue;

            reaches.emplace_back(mob, reach);
        }

        if (reaches.empty())
            return nullptr;

        Movement::PointsArray const& points = path.GetPath();
        float const eye = bot->GetCollisionHeight();
        for (size_t i = 1; i < points.size(); ++i)
        {
            G3D::Vector3 const segment = points[i] - points[i - 1];
            float const length = segment.length();
            for (float along = 0.0f; along <= length; along += WALK_SAMPLE_STEP)
            {
                G3D::Vector3 const point = points[i - 1] + (length > 0.0f ? segment * (along / length) : segment);
                for (auto const& [mob, reach] : reaches)
                {
                    if (mob->GetExactDist(point.x, point.y, point.z) < reach &&
                        MovementAction::PassesAggroZGate(mob, bot, point.z) &&
                        mob->IsWithinLOS(point.x, point.y, point.z + eye))
                        return mob;
                }
            }
        }

        return nullptr;
    }

    // The creature's SmartAI flees for assistance at low health (SMART_ACTION_FLEE_FOR_ASSIST: most humanoid trash).
    // Read once per entry and map thread.
    bool FleesForHelp(Creature* creature)
    {
        if (creature->GetAIName() != "SmartAI")
            return false;

        thread_local std::unordered_map<uint32, bool> cache;
        uint32 const entry = creature->GetEntry();
        auto const found = cache.find(entry);
        if (found != cache.end())
            return found->second;

        bool flees = false;
        for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
        {
            if (holder.action.type == SMART_ACTION_FLEE_FOR_ASSIST)
            {
                flees = true;
                break;
            }
        }

        cache.emplace(entry, flees);
        return flees;
    }

    // The creature stands where it is and casts or shoots once it fights (MobRoles casters, and SmartAI ranged
    // attacks cast with SMARTCAST_COMBAT_MOVE, such as a Shoot). Read once per entry and map thread.
    bool KeepsRange(Creature* creature)
    {
        // [b33_st] a ranged-caster-by-class mob measured to chase and melee (ClassicShared.h FightsInMelee)
        if (SunkenTemple::FightsInMelee(creature))
            return false;
        if (MobRoles::HoldsRange(creature))  // [b33_brs] was IsCaster: a SmartAI class caster that chases holds none
            return true;

        if (creature->GetAIName() != "SmartAI")
            return false;

        thread_local std::unordered_map<uint32, bool> cache;
        uint32 const entry = creature->GetEntry();
        auto const found = cache.find(entry);
        if (found != cache.end())
            return found->second;

        bool keeps = false;
        for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
        {
            if (holder.GetActionType() != SMART_ACTION_CAST || !(holder.action.cast.castFlags & SMARTCAST_COMBAT_MOVE))
                continue;

            SpellInfo const* info = sSpellMgr->GetSpellInfo(holder.action.cast.spell);
            if (info && !info->IsPositive())
            {
                keeps = true;
                break;
            }
        }

        cache.emplace(entry, keeps);
        return keeps;
    }

    // What the creature's SmartAI does in a fight to bring more: the mobs it summons while fighting (a Gnomeregan
    // Mobile Alert System summons a Leprous Defender and an Assistant every 10 s) and the radius it calls for help
    // in (SMART_ACTION_CALL_FOR_HELP). [lead260] A summon on a repeating in-combat timer is a rate (one per mean
    // repeat), counted over the estimated fight (EstimatePack); any other in-combat summon comes once; out-of-combat
    // summons (pets) are already standing there. Read once per entry and map thread.
    struct ScriptedHelp
    {
        float summons = 0.0f;        // summons that come once per fight
        float summonsPerSec = 0.0f;  // [lead260] summons on a repeating timer, per second of fight
        float callRadius = 0.0f;
        // [b36_scholo] a summon's health and damage against its summoner's (creature_template ModHealth /
        // DamageModifier, the summons' average; 1 when the summoned entry is unknown). scholo-h 20261026-034959-
        // scholo-h-5A-3: Dark Summoners (Summon Risen Lackey every 5 s: 7 counted each) had each Risen Lackey
        // (ModHealth 0.1 against 3) read as an average elite, "13 mobs at cost 143.10 / 191.00".
        float summonHealth = 1.0f;
        float summonDamage = 1.0f;
    };

    ScriptedHelp const& GetScriptedHelp(Creature* creature)
    {
        static ScriptedHelp const none;
        if (creature->GetAIName() != "SmartAI")
            return none;

        thread_local std::unordered_map<uint32, ScriptedHelp> cache;
        uint32 const entry = creature->GetEntry();
        auto found = cache.find(entry);
        if (found != cache.end())
            return found->second;

        ScriptedHelp help;
        float summonHealthSum = 0.0f;  // [b36_scholo]
        float summonDamageSum = 0.0f;
        SmartAIEventList const scripts = sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE);
        for (SmartScriptHolder const& holder : scripts)
        {
            if (holder.GetActionType() == SMART_ACTION_CALL_FOR_HELP)
                help.callRadius = std::max(help.callRadius, float(holder.action.callHelp.range));

            // a summon by a spell counts too: Deadmines' Goblin Engineer casts Summon Remote-Controlled Golem (3605)
            // in combat (wave 44 foundry: 12 counted, 20 came with the golems)
            bool spellSummon = false;
            uint32 summonEntry = 0;  // [b36_scholo] what comes (0: unknown, read as an average mob)
            if (holder.GetActionType() == SMART_ACTION_CAST)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(holder.action.cast.spell);
                for (uint8 i = 0; info && i < MAX_SPELL_EFFECTS && !spellSummon; ++i)
                {
                    spellSummon = info->Effects[i].Effect == SPELL_EFFECT_SUMMON;
                    if (spellSummon && info->Effects[i].MiscValue > 0)
                        summonEntry = uint32(info->Effects[i].MiscValue);
                }
            }
            else if (holder.GetActionType() == SMART_ACTION_SUMMON_CREATURE)
                summonEntry = holder.action.summonCreature.creature;

            if (holder.GetActionType() != SMART_ACTION_SUMMON_CREATURE &&
                holder.GetActionType() != SMART_ACTION_SUMMON_CREATURE_GROUP && !spellSummon)
                continue;

            // a linked action runs on the event that links to it
            SmartScriptHolder const* trigger = &holder;
            for (uint8 hop = 0; trigger && trigger->GetEventType() == SMART_EVENT_LINK && hop < 4; ++hop)
            {
                SmartScriptHolder const* source = nullptr;
                for (SmartScriptHolder const& other : scripts)
                {
                    if (other.link == trigger->event_id)
                    {
                        source = &other;
                        break;
                    }
                }

                trigger = source;
            }

            if (!trigger)
                continue;

            uint32 const event = trigger->GetEventType();
            if (event == SMART_EVENT_UPDATE_OOC || event == SMART_EVENT_RESET || event == SMART_EVENT_RESPAWN ||
                event == SMART_EVENT_LINK)
                continue;

            // [lead260] the first one comes once, the repeats at the timer's mean (summons over the fight's seconds)
            float const times = 1.0f;
            if (event == SMART_EVENT_UPDATE_IC && trigger->event.minMaxRepeat.repeatMax)
            {
                float const everySec =
                    float(trigger->event.minMaxRepeat.repeatMin + trigger->event.minMaxRepeat.repeatMax) / 2.0f /
                    float(IN_MILLISECONDS);
                help.summonsPerSec += 1.0f / std::max(everySec, 0.001f);
            }
            help.summons += times;
            // [b36_scholo] the summon's own stats against the summoner's (see ScriptedHelp)
            float healthShare = 1.0f;
            float damageShare = 1.0f;
            CreatureTemplate const* own = creature->GetCreatureTemplate();
            CreatureTemplate const* summon = summonEntry ? sObjectMgr->GetCreatureTemplate(summonEntry) : nullptr;
            if (own && summon && own->ModHealth > 0.0f && own->DamageModifier > 0.0f)
            {
                healthShare = summon->ModHealth / own->ModHealth;  // [lead260] the template's own ratio
                damageShare = summon->DamageModifier / own->DamageModifier;
            }
            summonHealthSum += healthShare * times;
            summonDamageSum += damageShare * times;
        }

        if (help.summons > 0.0f)
        {
            help.summonHealth = summonHealthSum / help.summons;
            help.summonDamage = summonDamageSum / help.summons;
        }
        return cache.emplace(entry, help).first->second;
    }

    // Creature::CallForHelp of `caller` fighting at `from` (CallOfHelpCreatureInRangeDo): `mob` comes if it is
    // within `radius` of it there, friendly, may assist it, and sees its enemy (the tank there). The call comes late
    // in the fight: a wanderer counts anywhere within its wander radius round its spawn.
    bool CalledForHelp(Creature* caller, Position const& from, float radius, Creature* mob, Player* bot)
    {
        if (radius <= 0.0f)
            return false;

        bool inReach = mob->GetExactDist(from) <= radius;
        if (!inReach && mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
            inReach = mob->GetHomePosition().GetExactDist(from) <= radius + mob->GetWanderDistance();

        return inReach && mob->IsFriendlyTo(caller) && mob->CanAssistTo(caller, bot, false) &&
               mob->IsWithinLOS(from.GetPositionX(), from.GetPositionY(), from.GetPositionZ() + CAREFUL_EYE_HEIGHT);
    }

    // Spawns this one's own SmartAI script calls in during its fight (SMART_ACTION_SET_DATA on a creature guid): the
    // Shattered Halls Legionnaires call a sparring orc each time one of theirs dies, and it fights the whole zone.
    // Read once per spawn and map thread.
    std::vector<ObjectGuid::LowType> const& ScriptedAdds(Creature* creature)
    {
        thread_local std::unordered_map<ObjectGuid::LowType, std::vector<ObjectGuid::LowType>> cache;
        ObjectGuid::LowType const spawnId = creature->GetSpawnId();
        auto found = cache.find(spawnId);
        if (found != cache.end())
            return found->second;

        std::vector<ObjectGuid::LowType> adds;
        if (spawnId && creature->GetAIName() == "SmartAI")
        {
            for (SmartScriptHolder const& holder :
                 sSmartScriptMgr->GetScript(-int32(spawnId), SMART_SCRIPT_TYPE_CREATURE))
            {
                if (holder.GetActionType() == SMART_ACTION_SET_DATA &&
                    holder.GetTargetType() == SMART_TARGET_CREATURE_GUID && holder.target.unitGUID.dbGuid &&
                    std::find(adds.begin(), adds.end(), holder.target.unitGUID.dbGuid) == adds.end())
                    adds.push_back(holder.target.unitGUID.dbGuid);
            }
        }

        return cache.emplace(spawnId, std::move(adds)).first->second;
    }

    // User, 2026-10-24: «как считается количество мобов в паке? ... почему они считаются одним паком?» Why each mob
    // is in a CollectPack pack ("[pack why]"): diagnostics only, recorded while the careful planner runs with the
    // playerbots log at info (PackWhyScope), one record per mob, parallel to the pack.
    enum class PackWhyKind : uint8
    {
        Start,       // the candidate
        Social,      // within PACK_SOCIAL_RADIUS of the candidate
        PullPath,    // on the tank's way to the spot
        SpotAggro,   // its aggro range reaches the spot
        SpotWander,  // its wander radius brings it to the spot
        SpotPatrol,  // a patrol that walks into the spot
        KillPath,    // on the tank's way back to the kill spot
        KillSpot,    // reaches the kill spot
        Formation,   // a formation member of the source
        Assist,      // the source's assistance call (in its radius, in its sight)
        CallForHelp, // the source's scripted call for help
        DrawsFight,  // reaches the source, a mob that keeps its range
        ScriptedAdd, // an add the source's script calls
        Scripted,    // a scripted call (Scarlet Monastery Cathedral)
        FleeHelper,  // the source flees to it at low health
        FleeTooFar,  // the source would flee to it, but dies before it gets there (not in the pack)
        PatrolAdded  // counted by the plan's patrol forecast after the pack
    };

    struct PackWhy
    {
        ObjectGuid mob;
        ObjectGuid source;
        PackWhyKind kind = PackWhyKind::Start;
        float distance = 0.0f;  // to the source (or to the spot)
        float radius = 0.0f;    // the call's radius, where there is one
        uint8 step = 0;         // flee-for-assistance steps from the candidate's own pack (0: no flight)
        float reachSec = 0.0f;  // a flight: the run to the helper, and what the fleer has left to live
        float killSec = 0.0f;
    };

    // One CollectPack call: why each pack mob is in it (parallel to the pack), and the helpers a flight misses.
    struct PackWhyRecord
    {
        std::vector<PackWhy> mobs;
        std::vector<PackWhy> skipped;
    };

    constexpr size_t PACK_WHY_MAX_PACKS = 64;      // CollectPack calls recorded per plan
    constexpr size_t PACK_WHY_MAX_LISTED = 25;     // mobs listed on one "[pack why]" line
    constexpr uint32 PACK_WHY_THROTTLE_MS = 10 * IN_MILLISECONDS;  // per tank and candidate
    constexpr size_t PACK_WHY_MIN_PLAN_MOBS = 6;   // a plan this big gets its line too

    struct PackWhyRecorder
    {
        bool on = false;
        std::vector<PackWhyRecord> packs;  // one per CollectPack call, in call order
    };

    thread_local PackWhyRecorder packWhyRecorder;

    // Records while it lives (when the playerbots log writes info), and forgets on the way out.
    struct PackWhyScope
    {
        PackWhyScope()
        {
            packWhyRecorder.packs.clear();
            packWhyRecorder.on = sLog->ShouldLog("playerbots", LOG_LEVEL_INFO);
            if (packWhyRecorder.on)
                packWhyRecorder.packs.reserve(PACK_WHY_MAX_PACKS);
        }

        ~PackWhyScope()
        {
            packWhyRecorder.on = false;
            packWhyRecorder.packs.clear();
        }

        PackWhyScope(PackWhyScope const&) = delete;
        PackWhyScope& operator=(PackWhyScope const&) = delete;
    };

    // Wave 44: a Sap a rogue of the group has claimed on the tank's pre-pull crowd control plan counts as applied
    // while the planner asks whether the pack fits (CollectPack leaves the mob out, as a mob crowd control holds):
    // the rogue claims it on the refused pack (PrePullCcAction), the Sap goes out once the tank accepts the plan,
    // and the tank holds its pull until it has landed (IsHolding). A failed claim no longer counts. Per map thread,
    // for one PlanCarefulPull (SapClaimScope).
    thread_local GuidVector sapClaimed;

    bool IsSapClaimed(Unit* mob)
    {
        return !sapClaimed.empty() &&
               std::find(sapClaimed.begin(), sapClaimed.end(), mob->GetGUID()) != sapClaimed.end();
    }

    struct SapClaimScope
    {
        SapClaimScope(PlayerbotAI* botAI, Player* tank)
        {
            sapClaimed.clear();
            PrePullCc::Plan plan;
            if (!PrePullCc::Get(tank, plan) || plan.tank != tank->GetGUID() || plan.pulledMs)
                return;

            for (PrePullCc::Claim const& claim : plan.claims)
            {
                if (claim.target.IsEmpty() || claim.failed)
                    continue;

                Unit* member = botAI->GetUnit(claim.bot);
                Player* controller = member ? member->ToPlayer() : nullptr;
                if (!controller || !controller->IsAlive())
                    continue;

                if (controller->getClass() == CLASS_ROGUE)
                {
                    sapClaimed.push_back(claim.target);
                    continue;
                }

                // [rot_b31_cc] A Polymorph, Hex, Shackle, Repentance, Hibernate, Banish or trap claimed on a caster or
                // healer of the pack (ready: it is cast as the pack comes, the trap is down) counts as applied too:
                // the pack refused as it stands is taken with its most dangerous mob held, the way a party pulls it
                // (brs-h 20261025: the Smolderthorn Mystic packs stayed refused, only a Sap ever counted).
                Unit* mob = claim.ready ? botAI->GetUnit(claim.target) : nullptr;
                if (mob && (MobRoles::IsHealer(mob) || MobRoles::IsCaster(mob)))
                    sapClaimed.push_back(claim.target);
            }
        }

        ~SapClaimScope() { sapClaimed.clear(); }

        SapClaimScope(SapClaimScope const&) = delete;
        SapClaimScope& operator=(SapClaimScope const&) = delete;
    };

    // The record of a pack: the latest recorded CollectPack whose mobs open `pack` in its order (the mobs after it
    // were added by the plan's patrol forecast). Null when nothing was recorded.
    PackWhyRecord const* FindPackWhy(std::vector<Unit*> const& pack)
    {
        // the whole pack first, then a record the patrol forecast added to
        for (bool const exact : {true, false})
        {
            for (auto itr = packWhyRecorder.packs.rbegin(); itr != packWhyRecorder.packs.rend(); ++itr)
            {
                std::vector<PackWhy> const& why = itr->mobs;
                if (why.empty() || why.size() > pack.size() || (exact && why.size() != pack.size()))
                    continue;

                bool same = true;
                for (size_t i = 0; i < why.size() && same; ++i)
                    same = pack[i] && pack[i]->GetGUID() == why[i].mob;

                if (same)
                    return &*itr;
            }
        }

        return nullptr;
    }

    // Everything that fights once `target` is engaged with the tank at `spot`: the target's social neighbours and
    // formation (linked packs, patrol groups), the mobs on the tank's way there, and every mob that sees the spot.
    // `assistChain` (measured careful pulls, read from a list that ignores the tank's line of sight): the social
    // neighbours must see the target, and every mob that fights calls the idle mobs that see it within the core's
    // assistance radius: when it engages and again every few seconds (Creature::CallAssistance,
    // CreatureFamilyAssistancePeriod), so an assistant calls in turn and the chain runs from pack to pack. A pack
    // mob that keeps its range draws the fight to it (the tank and the melee go there), what its script calls in
    // comes too, and a fleeing mob brings its helper's chain. Wave 10: plans of 2-4 mobs brought 8 (Stockade),
    // 12-13 (Steamvault) and 16 (Scholomance); wave 13: 5 brought 18 (Shattered Halls), 5 brought 15 (Razorfen Kraul).
    // `losPull` (a pull round a corner, measured): the casters walk to the tank instead of keeping their range, so
    // they draw no fight to their spawn, and only what engages on its own calls its neighbours there. An assistant
    // engages with its call switched off (AssistDelayEvent: SetNoCallAssistance) and calls next 3 s later
    // (CreatureFamilyAssistancePeriod) from where it is then: on its way to the tank, unless it stands (rooted).
    // Scholomance hall (wave 27): its 33 casters are one chain through their aggro ranges (spawn gaps of 13-18 yd,
    // more than the 10 yd assistance radius), so it comes whole when its casters stand, and 4-9 at a time round
    // a corner.
    // `overlaps` (split mode, the careful planner): a group that did not stand with the pack at spawn and crosses it
    // now (SeparateAtSpawn) is noted there instead of merged by the social, assistance, call-for-help and flee
    // tests; what the spot test (ReachesSpot) or the pull path brings is counted as before, but for a forecast patrol
    // of another spawn group (walksBy below).
    // `killSpot` (a pull back): where the fight stays once the tank has stepped back to it after its ranged pull at
    // `spot`, so where a pack mob drops to the health its script calls or flees for help at (default: `spot`). The
    // calls and flights are counted from there, and what reaches it or the tank's way back to it comes too.
    // The core's formation aggro link (CreatureGroup::MemberEngagingTarget): the leader's engage brings the rest only
    // with GROUP_AI_FLAG_MEMBER_ASSIST_LEADER in its groupAI, a member's engage brings the rest (the leader too) only
    // with GROUP_AI_FLAG_LEADER_ASSIST_MEMBER in the member's. A group with neither only walks together; what stands
    // near is brought by CallAssistance, which CollectPack counts on its own. Before, every formation member came
    // whatever its flags (world DB: 213 formation rows have no assist flag, 1252 only one of the two).
    bool FormationBrings(Creature* engaged, CreatureGroup const* formation)
    {
        auto const& members = formation->GetMembers();
        auto const found = members.find(engaged);
        if (found == members.end())
            return false;

        GroupAIFlags const needed = engaged == formation->GetLeader() ? GroupAIFlags::GROUP_AI_FLAG_MEMBER_ASSIST_LEADER
                                                                     : GroupAIFlags::GROUP_AI_FLAG_LEADER_ASSIST_MEMBER;
        return found->second.HasGroupFlag(std::underlying_type_t<GroupAIFlags>(needed));
    }

    // [chain_step_weight] PULL_ESTIMATE_AUDIT 2026-10-25: a refused pack's estimate ran median x1.36, p75 x3, p90
    // x4.5 over what came, all of it in the chain links (assist / draws-fight hops). A mob one or more such links
    // away counts CHAIN_STEP_CAP_WEIGHT against the size cap and the proven strength (its cost stays full). The
    // last CollectPack of this map thread leaves its hops here; EstimatePack reads them for the same pack.
    constexpr float CHAIN_STEP_CAP_WEIGHT = 0.5f;
    thread_local std::vector<std::pair<ObjectGuid, uint8>> lastPackHops;

    float ChainStepWeight(std::vector<Unit*> const& pack, Unit* mob)
    {
        if (lastPackHops.empty() || pack.empty() || !pack.front() ||
            lastPackHops.front().first != pack.front()->GetGUID())
            return 1.0f;

        for (auto const& [guid, hop] : lastPackHops)
        {
            if (guid == mob->GetGUID())
                return hop ? CHAIN_STEP_CAP_WEIGHT : 1.0f;
        }

        return 1.0f;
    }

    // [b33_brs] The core's engage call (Unit::Attack -> Creature::CallAssistance) is skipped for a creature with
    // CREATURE_FLAG_EXTRA_DONT_CALL_ASSISTANCE; only its periodic call (Creature::Update, every
    // CreatureFamilyAssistancePeriod, from where it fights) reaches its neighbours. ac-h 20261026-002247-ac-h-5E-70:
    // the Reanimated Bones 18700 (flags_extra 0x02000000, 47 spawns in six Auchenai Necromancer formations) were
    // chained formation to formation by engage calls they never make: "39 mobs, over 18", Maladaar never engaged.
    bool CallsOnEngage(Unit* mob)
    {
        Creature* creature = mob ? mob->ToCreature() : nullptr;
        return !creature || !creature->HasFlagsExtra(CREATURE_FLAG_EXTRA_DONT_CALL_ASSISTANCE);
    }

    // [lead8] LEADER_REFACTOR_PLAN.md step 8 / 2.5: where the pack's fight is, a required argument of every count.
    // FightHere: the fight is at `spot` itself (an engage at the mob, a stand where the tank pulls and fights);
    // FightAt: a separate fight spot (a pull back's kill spot, a line-of-sight corner, a sight fight spot), from which
    // the calls for help, the social / formation links and the "draws the fight" chains are counted (KILL_SPOT_DRIFT
    // round it), the walk's own aggro along the way to `spot`.
    struct PackFight
    {
        Position at;
        bool separate = false;
    };
    PackFight FightHere(Position const& spot) { return PackFight{spot, false}; }
    PackFight FightAt(Position const& kill) { return PackFight{kill, true}; }

    void CollectPack(PlayerbotAI* botAI, Player* bot, Unit* target, Position const& spot, GuidVector const& possible,
                     std::vector<Unit*>& pack, PackFight const& fight, bool assistChain = false, bool losPull = false,
                     std::vector<PackOverlap>* overlaps = nullptr)
    {
        Position const* const killSpot = fight.separate ? &fight.at : nullptr;  // [lead8]
        Position const kill = killSpot ? *killSpot : spot;
        bool const killApart = killSpot && kill.GetExactDist(spot) > CAREFUL_AGGRO_MARGIN;
        // [b47_plan] the fight's spread round a pull back's kill spot (the spot itself when they coincide)
        float const killDrift = killSpot ? KILL_SPOT_DRIFT : 0.0f;
        pack.clear();
        pack.push_back(target);
        // parallel to `pack`: how a mob joins - 0 by itself (the target, its neighbours, what reaches the spot, a
        // flee helper), n a mob n assist / draws-fight links away from those
        std::vector<uint8> hops = {0};
        // [chain_fight_position] parallel to `pack`: 1 for a mob an assistance call brought (AssistDelayEvent: it
        // engages with its own call switched off and calls next from where it fights, not from its spawn)
        std::vector<uint8> assisted = {0};
        // [chain_step_weight] on every way out: the hops of this pack for EstimatePack
        struct NoteHops
        {
            std::vector<Unit*> const& pack;
            std::vector<uint8> const& hops;

            ~NoteHops()
            {
                lastPackHops.clear();
                for (size_t i = 0; i < pack.size() && i < hops.size(); ++i)
                {
                    if (pack[i])
                        lastPackHops.emplace_back(pack[i]->GetGUID(), hops[i]);
                }
            }
        } const noteHops{pack, hops};
        auto const hopOf = [&](Unit* mob) -> uint8
        {
            for (size_t i = 0; i < pack.size(); ++i)
            {
                if (pack[i] == mob)
                    return hops[i];
            }

            return 0;
        };
        if (overlaps)
            overlaps->clear();

        // "[pack why]": why each mob joins, parallel to `pack` (only while the planner records).
        PackWhyRecord* why = nullptr;
        if (packWhyRecorder.on && packWhyRecorder.packs.size() < PACK_WHY_MAX_PACKS)
        {
            why = &packWhyRecorder.packs.emplace_back();
            why->mobs.push_back({target->GetGUID(), ObjectGuid::Empty, PackWhyKind::Start});
        }

        // Where a mob that reaches `at` comes from: its aggro range, its wander radius, or its patrol.
        auto const spotKind = [](Unit* mob) -> PackWhyKind
        {
            Creature* creature = mob->ToCreature();
            if (creature && IsPatrolling(creature))
                return PackWhyKind::SpotPatrol;

            if (creature && creature->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
                return PackWhyKind::SpotWander;

            return PackWhyKind::SpotAggro;
        };

        auto const note = [overlaps](PackOverlap const& overlap)
        {
            if (std::none_of(overlaps->begin(), overlaps->end(),
                             [&overlap](PackOverlap const& seen) { return seen.outside == overlap.outside; }))
                overlaps->push_back(overlap);
        };

        // Split mode: `mob`, near `bringer` (nullptr: the spot) within `radius` now, is a crossing group (noted).
        auto const crossing = [&](Unit* bringer, Position const& anchor, Unit* mob, float radius) -> bool
        {
            PackOverlap overlap;
            if (!overlaps || !SeparateAtSpawn(bringer, anchor, mob, radius, overlap))
                return false;

            note(overlap);
            return true;
        };

        // On the way out: a crossing group that came in another way after all (it reaches the spot) is in the
        // pack, and one noted against a mob that is not in it (a skipped helper) crosses nothing that fights.
        struct DropMerged
        {
            std::vector<PackOverlap>* overlaps;
            std::vector<Unit*> const& pack;

            bool InPack(Unit* mob) const { return std::find(pack.begin(), pack.end(), mob) != pack.end(); }

            ~DropMerged()
            {
                if (!overlaps)
                    return;

                overlaps->erase(std::remove_if(overlaps->begin(), overlaps->end(),
                                               [this](PackOverlap const& overlap)
                                               {
                                                   return InPack(overlap.outside) ||
                                                          (overlap.member && !InPack(overlap.member));
                                               }),
                                overlaps->end());
            }
        } const dropMerged{overlaps, pack};

        // Split mode: a patrol of another spawn group that the spot test counts for where it walks (near the spot
        // now, or forecast to come) is left to the plan's patrol forecast (waited out, pulled first, or counted:
        // PlanCarefulPull), and one within its aggro reach of the spot now is noted as crossing it. A patrol whose
        // walk cannot be forecast, or that paces beside its pack, is counted as before.
        auto const walksBy = [&](Unit* mob) -> bool
        {
            Creature* creature = mob->ToCreature();
            if (!overlaps || !creature || IsBossMob(creature))
                return false;

            Creature* walker = nullptr;
            WaypointPath const* path = PatrolPath(creature, &walker);
            std::vector<G3D::Vector3> const* track = PatrolTrack(walker, path);
            if (!track || IsPacer(*track))
                return false;

            float const reach = MovementAction::IdleAggroReach(creature, bot, CAREFUL_AGGRO_MARGIN);
            PackOverlap overlap;
            if (!SeparateAtSpawn(nullptr, spot, mob, reach, overlap))
                return false;

            if (InCoreAggroReach(mob, bot, spot, CAREFUL_AGGRO_MARGIN))
                note(overlap);

            return true;
        };
        // Mobs that engage by themselves (the target, its neighbours, what sees the spot): they call for assistance
        // where they stand the moment they engage (Unit::Attack -> CallAssistance).
        std::vector<Unit*> direct = {target};
        // A mob brings its formation (a linked pack, the rest of a patrol group) with it, and each member its own.
        std::function<void(Unit*)> addFormation;
        // A mob held by crowd control (a Sap before the pull) stays where it is while the rest fights.
        // `kind`, `source`, `distance` (below 0: to `source`) and `radius`: why it comes ("[pack why]" only).
        auto const add = [&](Unit* mob, PackWhyKind kind, Unit* source, float distance = -1.0f, float radius = 0.0f)
        {
            if (IsCrowdControlled(mob) || IsSapClaimed(mob) || std::find(pack.begin(), pack.end(), mob) != pack.end())
                return;

            // [pack_skip_sealed_mob] a banished / immune idle mob joins no fight
            if (mob != target && IsSealedFromFight(mob))
                return;

            // an assistant, or a mob a ranged mob's fight draws, is one link further than its source; a formation
            // member and a called or scripted add come as their source does
            uint8 const sourceHop = source ? hopOf(source) : 0;
            uint8 hop = 0;
            if (kind == PackWhyKind::Assist || kind == PackWhyKind::DrawsFight)
                hop = uint8(std::min<uint32>(sourceHop + 1u, 255u));
            else if (kind == PackWhyKind::Formation || kind == PackWhyKind::CallForHelp ||
                     kind == PackWhyKind::ScriptedAdd)
                hop = sourceHop;

            pack.push_back(mob);
            hops.push_back(hop);
            // [b35_lbrs2] round a corner a social neighbour came by the target's engage call (AssistDelayEvent:
            // SetNoCallAssistance), so it makes no engage call of its own at its spawn: only the periodic one, on its
            // walk and at the kill spot (brs-h 20261026-024524-brs-h-5D-8: a Firebrand Invoker's phantom spawn call
            // chained the Firebrand hall pair to pair)
            assisted.push_back(kind == PackWhyKind::Assist || (losPull && kind == PackWhyKind::Social) ? 1 : 0);
            if (why)
            {
                uint8 step = 0;
                if (source)
                {
                    for (PackWhy const& record : why->mobs)
                    {
                        if (record.mob == source->GetGUID())
                        {
                            step = record.step;
                            break;
                        }
                    }
                }

                if (kind == PackWhyKind::FleeHelper && step < 255)
                    ++step;

                if (distance < 0.0f)
                    distance = source ? mob->GetExactDist(source) : 0.0f;

                why->mobs.push_back({mob->GetGUID(), source ? source->GetGUID() : ObjectGuid::Empty, kind,
                                     distance, radius, step});
            }

            // [b51_strat] A formation member engages by itself where it stands: CreatureGroup::MemberEngagingTarget
            // -> EngageWithTarget -> AtEngage -> AttackStart -> Unit::Attack -> CallAssistance, with no
            // SetNoCallAssistance (only AssistDelayEvent sets that). Round a corner its spawn call was dropped (not in
            // `direct`), so a split of one linked formation counted 7 while its engage calls woke the next one:
            // strat-h 20261026-130307-5H-2 pull 2 (Mangled Cadaver of formation 52157, "7 come", 19 engaged: 52157 +
            // formation 52158 + Mangled Cadaver 52189), strat-n 122322-5B-3 (7 -> 17), strat-h 093620-5E-2 (7 -> 32).
            if (kind == PackWhyKind::Formation && std::find(direct.begin(), direct.end(), mob) == direct.end())
                direct.push_back(mob);

            addFormation(mob);
        };

        addFormation = [&](Unit* mob)
        {
            Creature* creature = mob->ToCreature();
            CreatureGroup const* formation = creature ? creature->GetFormation() : nullptr;
            if (!formation || !FormationBrings(creature, formation))
                return;

            for (auto const& member : formation->GetMembers())
            {
                if (member.first != creature && IsIdleEnemy(bot, member.first))
                    add(member.first, PackWhyKind::Formation, mob);
            }
        };

        addFormation(target);
        // The target's engage call (Creature::CallAssistance, AnyAssistCreatureInRangeCheck): an idle mob that may
        // assist it (CanAssistTo: its faction, it answers calls, aggressive, sees the enemy) within the assistance
        // radius, plus the drift of the fight, that the target sees. A mob that only stands near is no neighbour:
        // it comes if it sees the spot (ReachesSpot) or another member calls it.
        float const socialAssistRadius = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        float const socialRadius =
            socialAssistRadius > 0.0f ? std::min(PACK_SOCIAL_RADIUS, socialAssistRadius + PACK_SOCIAL_DRIFT) : 0.0f;
        Creature* const targetCreature = target->ToCreature();
        for (ObjectGuid const& guid : possible)
        {
            Unit* mob = botAI->GetUnit(guid);
            if (!mob || mob == target || !IsIdleEnemy(bot, mob))
                continue;

            Creature* const mobCreature = mob->ToCreature();
            bool const social = socialRadius > 0.0f && CallsOnEngage(target) &&  // [b33_brs]
                                mob->GetDistance(target) <= socialRadius &&
                                (!mobCreature || !targetCreature || mobCreature->CanAssistTo(targetCreature, bot)) &&
                                target->IsWithinLOSInMap(mob) &&
                                !crossing(target, target->GetPosition(), mob, socialRadius);
            // the same tests in the same order as one condition, the first that holds named for "[pack why]"
            bool joins = true;
            PackWhyKind kind = PackWhyKind::Social;
            Position const* from = target;
            if (social)
                kind = PackWhyKind::Social;
            else if (IsOnPullPath(bot, spot, mob))
            {
                kind = PackWhyKind::PullPath;
                from = &spot;
            }
            else if (ReachesSpot(bot, mob, spot, killApart ? 0.0f : killDrift) && !walksBy(mob))
            {
                kind = why ? spotKind(mob) : PackWhyKind::SpotAggro;
                from = &spot;
            }
            else if (killApart && IsOnPullPath(bot, kill, mob))
            {
                kind = PackWhyKind::KillPath;
                from = &kill;
            }
            else if (killApart && ReachesSpot(bot, mob, kill, killDrift) && !walksBy(mob))
            {
                kind = PackWhyKind::KillSpot;
                from = &kill;
            }
            else
                joins = false;

            if (joins)
            {
                add(mob, kind, kind == PackWhyKind::Social ? target : nullptr, why ? mob->GetExactDist(*from) : 0.0f);
                direct.push_back(mob);
            }
        }

        float const assistRadius = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        Map* map = bot->GetMap();
        // Where a mob that keeps its range fights from: where it stands, if that is within CASTER_KEEP_RANGE of the
        // real fight spot (the kill spot), else at that range from it on the line to where it stands (it walks in).
        auto const standPoint = [&kill](Unit* caller) -> Position
        {
            float const toKill = caller->GetExactDist(kill);
            if (toKill <= CASTER_KEEP_RANGE || toKill <= 0.0f)
                return caller->GetPosition();

            float const share = CASTER_KEEP_RANGE / toKill;
            return Position(kill.GetPositionX() + (caller->GetPositionX() - kill.GetPositionX()) * share,
                            kill.GetPositionY() + (caller->GetPositionY() - kill.GetPositionY()) * share,
                            kill.GetPositionZ() + (caller->GetPositionZ() - kill.GetPositionZ()) * share);
        };
        // What each pack mob from `from` on brings (and what that brings in turn), in measured careful pulls.
        auto const spread = [&](size_t from)
        {
            if (!assistChain)
                return;

            for (size_t i = from; i < pack.size() && pack.size() < PACK_CHAIN_MAX_MOBS; ++i)
            {
                Creature* caller = pack[i]->ToCreature();
                // the safety bound of the chain (PACK_MAX_ASSIST_HOPS links from a mob that came by itself)
                if (!caller || hops[i] >= PACK_MAX_ASSIST_HOPS)
                    continue;

                // Round a corner a caster comes too: only a rooted mob stays and keeps calling from its spawn,
                // and a mob that walks to the tank calls only once it engages by itself.
                bool const rooted = caller->IsRooted();
                bool const stays = rooted || (!losPull && KeepsRange(caller));
                // Stratholme wave 6 (Rlpalbh, 15 mobs for a 7-mob formation): a Plague Ghoul patrol forecast to walk
                // into the fight was counted with the assistance of the pack it passed 60 yd away. A patrol that is not
                // at the fight yet engages (and calls) where it arrives, which the spot tests count; not from here.
                bool const arriving = [&]()
                {
                    if (!IsPatrolling(caller) || IsBossMob(caller))
                        return false;

                    Creature* walker = nullptr;
                    WaypointPath const* path = PatrolPath(caller, &walker);
                    std::vector<G3D::Vector3> const* track = PatrolTrack(walker, path);
                    float const reach = MovementAction::IdleAggroReach(caller, bot, CAREFUL_AGGRO_MARGIN);
                    return track && !IsPacer(*track) && caller->GetExactDist(kill) > reach + CAREFUL_PATROL_MARGIN;
                }();
                bool const callsHere = !arriving && (!losPull || stays ||
                                                     std::find(direct.begin(), direct.end(), caller) != direct.end());
                // where it fights: a rooted mob where it stands, a ranged one within its range of the fight spot
                Position const stand = rooted ? caller->GetPosition() : standPoint(caller);
                // apart from where the fight is (the kill spot; default: the spot), the tank and the melee go to it
                // Only a mob that came by itself draws the fight (hop 0): the group fights at the kill spot and goes to
                // the casters there, not on from caster to caster across a hall. Build 20 counted chains of them
                // (Scholomance upper hall 20, Sethekk 13, a Stratholme Cadaver formation 26) where the passing waves
                // pulled 2-16 there without a death; the DB groups are 2-7.
                bool const drawsFight = !arriving && stays && hops[i] == 0 &&
                                        stand.GetExactDist(kill) > CAREFUL_DRIFT_MIN_DISTANCE;
                // [chain_melee_edge] the melee that go to it stand no further from the fight spot than the fight
                // hold lets them (FightHold: RADIUS + MELEE_SLACK, PATCH_pets_rally 7), so what sees them there is
                // looked for at that edge, on the line to the caster.
                Position drawAt = stand;
                if (float const toStand = stand.GetExactDist(kill);
                    toStand > FightHold::RADIUS + FightHold::MELEE_SLACK && toStand > 0.0f)
                {
                    float const share = (FightHold::RADIUS + FightHold::MELEE_SLACK) / toStand;
                    drawAt.Relocate(kill.GetPositionX() + (stand.GetPositionX() - kill.GetPositionX()) * share,
                                    kill.GetPositionY() + (stand.GetPositionY() - kill.GetPositionY()) * share,
                                    kill.GetPositionZ() + (stand.GetPositionZ() - kill.GetPositionZ()) * share);
                }
                // [chain_fight_position] User 2026-10-25 (PATCH_pets_rally 5): the engage call (Unit::Attack ->
                // Creature::CallAssistance) comes from where the mob stands when it engages, but only from a mob
                // that engaged by itself; an assistant engages with its call off (AssistDelayEvent:
                // SetNoCallAssistance) and calls next by the periodic call (Creature::Update, every
                // CreatureFamilyAssistancePeriod, CanPeriodicallyCallForAssistance) from where it FIGHTS then: a
                // melee mob by the tank (the kill spot), a mob that keeps its range where it stands and casts. The
                // chain counted every assistant's call from its spawn, pack to pack (scholo-h 153805: "16 mobs,
                // hopeless" -> 1 came, "lone Adept brings 10").
                bool const engageCall = callsHere && !assisted[i] && CallsOnEngage(caller);  // [b33_brs]
                Position const fightAt = stays ? stand : kill;
                float const callerSize = caller->GetObjectSize();
                float const callRadius = GetScriptedHelp(caller).callRadius;
                // It calls at low health where it fights then: the kill spot, or where it stands and casts.
                Position const callFrom = stays ? stand : kill;
                for (ObjectGuid const& guid : possible)
                {
                    Creature* mob = botAI->GetCreature(guid);
                    if (!mob || mob == caller || !IsIdleEnemy(bot, mob) ||
                        std::find(pack.begin(), pack.end(), mob) != pack.end())
                        continue;

                    // its engage call reaches from where it stands when it engages (CallAssistance), its periodic
                    // call from where it fights ([chain_fight_position])
                    bool const engageAssists = engageCall && assistRadius > 0.0f &&
                                               caller->IsWithinDistInMap(mob, assistRadius) &&
                                               mob->CanAssistTo(caller, bot) && caller->IsWithinLOSInMap(mob) &&
                                               !crossing(caller, caller->GetPosition(), mob, assistRadius);
                    bool const fightAssists =
                        !engageAssists && !arriving && assistRadius > 0.0f &&
                        mob->GetExactDist(fightAt) <= assistRadius + callerSize + mob->GetObjectSize() &&
                        mob->CanAssistTo(caller, bot) &&
                        mob->IsWithinLOS(fightAt.GetPositionX(), fightAt.GetPositionY(),
                                         fightAt.GetPositionZ() + CAREFUL_EYE_HEIGHT) &&
                        !crossing(nullptr, fightAt, mob, assistRadius);
                    bool const assists = engageAssists || fightAssists;
                    // Creature::CallForHelp: any friendly idle mob in the radius that sees the fight
                    bool const called = CalledForHelp(caller, callFrom, callRadius, mob, bot) &&
                                        !crossing(nullptr, callFrom, mob, callRadius);
                    // the core's aggro of the mob on the tank and melee where the ranged mob fights (ReachesSpot)
                    // a patrol of another spawn group that walks by is left to the plan's patrol forecast, as at the
                    // spot (walksBy)
                    if (assists || called || (drawsFight && ReachesSpot(bot, mob, drawAt) && !walksBy(mob)))
                    {
                        if (assists)
                            add(mob, PackWhyKind::Assist, caller, -1.0f, assistRadius);
                        else if (called)
                            add(mob, PackWhyKind::CallForHelp, caller, why ? mob->GetExactDist(callFrom) : 0.0f,
                                callRadius);
                        else
                            add(mob, PackWhyKind::DrawsFight, caller);

                        addFormation(mob);
                    }
                }

                // [b34_est] The periodic call (Creature::Update, every CreatureFamilyAssistancePeriod) comes from
                // where the mob is then, so also on its walk to the fight spot round the corner. ohb-h
                // 20261026-014521-ohb-h-5F-19 pull 25 (wipe): a los pull counted at 3 brought 13, three Durnholde
                // formations (4 + 5 + 4); the tank's spot stood 11.6 yd up a ramp, the walk to it ran 5.7 yd past
                // the next formation's Sentry (it engaged at once), and that one's walk past the third. A mob that
                // walks round the corner calls along its navmesh walk there, at the assistance radius (an assist).
                if (losPull && !stays && !arriving && assistRadius > 0.0f && caller->GetExactDist(kill) > assistRadius)
                {
                    LeaderPath walk(caller);  // [lead99]
                    walk.CalculatePath(kill.GetPositionX(), kill.GetPositionY(), kill.GetPositionZ(), false);
                    if (walk.GetPathType() & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE))
                    {
                        Movement::PointsArray const& points = walk.GetPath();
                        // [b35_lbrs2] Only where a call can come from: the first periodic call fires a whole
                        // CreatureFamilyAssistancePeriod after the mob engaged (Unit::Attack -> SetAssistanceTimer),
                        // so the first LOS_WALK_CALL_FREE_SHARE of that period's run is call-free (its spawn call, if
                        // any, is the engage call above). Then every stretch of the walk, not only its navmesh corners
                        // (a 20 yd straight leg passed a pack between two corners unseen). brs-h
                        // 20261026-024524-brs-h-5D-8: the walk's first point is the mob's own spawn, so every Firebrand
                        // pair 8-12 yd from the next joined "on the walk" and the hall chained whole (15 mobs, cost
                        // 30.16) where real players peel it pair by pair round the corner.
                        constexpr float LOS_WALK_CALL_FREE_SHARE = 0.5f;
                        float const periodMs = float(sWorld->getIntConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_PERIOD));
                        float const callFree =
                            LOS_WALK_CALL_FREE_SHARE * periodMs / float(IN_MILLISECONDS) * caller->GetSpeed(MOVE_RUN);
                        std::vector<std::pair<G3D::Vector3, G3D::Vector3>> legs;
                        float walked = 0.0f;
                        for (size_t p = 1; p < points.size(); ++p)
                        {
                            G3D::Vector3 const& a = points[p - 1];
                            G3D::Vector3 const& b = points[p];
                            float const length = (b - a).length();
                            if (length > 0.0f && walked + length > callFree)
                            {
                                float const skip = std::max(callFree - walked, 0.0f);
                                legs.emplace_back(a + (b - a) * (skip / length), b);
                            }
                            walked += length;
                        }

                        for (ObjectGuid const& guid : possible)
                        {
                            if (legs.empty())
                                break;  // it reaches the kill spot before its first call: the kill spot's call only

                            Creature* mob = botAI->GetCreature(guid);
                            if (!mob || mob == caller || !IsIdleEnemy(bot, mob) ||
                                std::find(pack.begin(), pack.end(), mob) != pack.end() ||
                                !mob->CanAssistTo(caller, bot))
                                continue;

                            float const reach = assistRadius + callerSize + mob->GetObjectSize();
                            G3D::Vector3 const at(mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ());
                            for (auto const& [from, to] : legs)
                            {
                                // the leg's point nearest the mob
                                G3D::Vector3 const leg = to - from;
                                float const squared = leg.dot(leg);
                                float const share =
                                    squared > 0.0f ? std::clamp((at - from).dot(leg) / squared, 0.0f, 1.0f) : 0.0f;
                                G3D::Vector3 const point = from + leg * share;
                                Position const closest(point.x, point.y, point.z);
                                if (mob->GetExactDist(closest) <= reach &&
                                    mob->IsWithinLOS(point.x, point.y, point.z + CAREFUL_EYE_HEIGHT) &&
                                    !crossing(nullptr, closest, mob, assistRadius))
                                {
                                    add(mob, PackWhyKind::Assist, caller, -1.0f, assistRadius);
                                    break;
                                }
                            }
                        }
                    }
                }

                if (!map)
                    continue;

                for (ObjectGuid::LowType const spawnId : ScriptedAdds(caller))
                {
                    auto const range = map->GetCreatureBySpawnIdStore().equal_range(spawnId);
                    for (auto itr = range.first; itr != range.second; ++itr)
                    {
                        // a called add may wait at a neutral faction until its script sets it hostile
                        Creature* called = itr->second;
                        if (called && called->IsInWorld() && called->IsAlive() && !called->IsInCombat())
                            add(called, PackWhyKind::ScriptedAdd, caller);
                    }
                }
            }
        };

        spread(0);

        // Call for help in every careful mode (measured: in the chain above). A pack mob whose script calls at low
        // health brings every mob CalledForHelp finds round the spot where it fights then (the kill spot; a rooted
        // mob: where it stands), not round its spawn, each with its formation and the neighbours its engage calls
        // (CallAssistance); a called mob that calls in turn does so from there too.
        auto const callForHelp = [&](size_t from)
        {
            if (assistChain)
                return;

            for (size_t i = from; i < pack.size() && pack.size() < PACK_CHAIN_MAX_MOBS; ++i)
            {
                Creature* caller = pack[i]->ToCreature();
                float const callRadius = caller ? GetScriptedHelp(caller).callRadius : 0.0f;
                if (callRadius <= 0.0f)
                    continue;

                Position const callFrom = caller->IsRooted() ? caller->GetPosition() : kill;
                for (ObjectGuid const& guid : possible)
                {
                    Creature* mob = botAI->GetCreature(guid);
                    if (!mob || mob == caller || !IsIdleEnemy(bot, mob) ||
                        std::find(pack.begin(), pack.end(), mob) != pack.end() ||
                        !CalledForHelp(caller, callFrom, callRadius, mob, bot) ||
                        crossing(nullptr, callFrom, mob, callRadius))
                        continue;

                    add(mob, PackWhyKind::CallForHelp, caller, why ? mob->GetExactDist(callFrom) : 0.0f, callRadius);
                    if (assistRadius <= 0.0f || !CallsOnEngage(mob))  // [b33_brs]
                        continue;

                    for (ObjectGuid const& other : possible)
                    {
                        Creature* neighbour = botAI->GetCreature(other);
                        if (neighbour && neighbour != mob && IsIdleEnemy(bot, neighbour) &&
                            mob->IsWithinDistInMap(neighbour, assistRadius) && neighbour->CanAssistTo(mob, bot) &&
                            mob->IsWithinLOSInMap(neighbour) &&
                            !crossing(mob, mob->GetPosition(), neighbour, assistRadius))
                            add(neighbour, PackWhyKind::Assist, mob, -1.0f, assistRadius);
                    }
                }
            }
        };

        callForHelp(0);

        // A scripted call (Scarlet Monastery Cathedral, ClassicShared.h): Commander Mograine's engage sends his
        // whole chapel at his victim, wherever its mobs stand and whatever sees what. Wave 34: a fight 34 yd from him
        // drew a hunter to 12 yd of him, and the rest of the chapel came at once.
        if (Unit* mograine = ScarletMonastery::IdleMograineWithChapel(bot))
        {
            bool const engaged = std::find(pack.begin(), pack.end(), mograine) != pack.end() ||
                                 (IsIdleEnemy(bot, mograine) &&
                                  (ReachesSpot(bot, mograine, spot) ||
                                   mograine->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY()) <
                                       ScarletMonastery::MOGRAINE_KEEP_AWAY));
            if (engaged)
            {
                add(mograine, PackWhyKind::Scripted, nullptr, why ? mograine->GetExactDist(spot) : 0.0f);
                std::vector<Unit*> called;
                ScarletMonastery::CollectMograineCall(mograine, called);
                for (Unit* mob : called)
                {
                    if (IsIdleEnemy(bot, mob))
                        add(mob, PackWhyKind::Scripted, mograine);
                }
            }
        }

        // Flee for assistance (Creature::DoFleeToGetAssistance), in every careful mode: a pack mob that flees at low
        // health runs to the nearest idle mob within the flee radius that sees it, which comes with its formation and
        // calls its neighbours (CallAssistance); a helper that flees fetches the next. Counted from the spot, where
        // the fight stays, or (measured) from a mob that keeps its range, where the tank goes to fight it. Wave 11b
        // Scarlet Monastery Library: a 2-mob plan, the next pack 27 yd away, 13 came.
        float const fleeRadius = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS);
        if (fleeRadius <= 0.0f)
            return;

        float fleeDps = -1.0f;  // the group's estimated dps, read at the first flight
        for (size_t i = 0; i < pack.size() && pack.size() < FLEE_CHAIN_MAX_MOBS; ++i)
        {
            Creature* fleer = pack[i]->ToCreature();
            if (!fleer || IsBossMob(fleer) || !FleesForHelp(fleer))
                continue;

            Position const from = assistChain && !losPull && KeepsRange(fleer) ? standPoint(fleer) : kill;
            Creature* helper = nullptr;
            float nearest = fleeRadius;
            // Split mode: a crossing group nearer than the helper is where the fleer would run now (noted); once it
            // has walked on, the fleer runs to the helper.
            PackOverlap passing;
            float passingAt = fleeRadius;
            for (ObjectGuid const& guid : possible)
            {
                Creature* mob = botAI->GetCreature(guid);
                if (!mob || !IsIdleEnemy(bot, mob) || IsCrowdControlled(mob) || IsSapClaimed(mob) ||
                    std::find(pack.begin(), pack.end(), mob) != pack.end())
                    continue;

                float const distance = mob->GetExactDist(from);
                if (distance > nearest || !mob->CanAssistTo(fleer, bot) ||
                    !mob->IsWithinLOS(from.GetPositionX(), from.GetPositionY(),
                                      from.GetPositionZ() + CAREFUL_EYE_HEIGHT))
                    continue;

                PackOverlap overlap;
                if (overlaps && SeparateAtSpawn(nullptr, from, mob, fleeRadius, overlap))
                {
                    if (!passing.outside || distance < passingAt)
                    {
                        passing = overlap;
                        passingAt = distance;
                    }

                    continue;
                }

                helper = mob;
                nearest = distance;
            }

            if (passing.outside && (!helper || passingAt < nearest))
                note(passing);

            if (!helper)
                continue;

            // It dies on the way: the helper never hears of the fight.
            if (fleeDps < 0.0f)
                fleeDps = std::max(botAI->GetAiObjectContext()->GetValue<float>("estimated group dps")->Get(),
                                   GroupWeaponDps(bot));  // [lead259]

            float const reachSec = nearest / std::max(fleer->GetSpeed(MOVE_RUN), 0.1f);
            float const killSec = float(fleer->GetMaxHealth()) * FLEE_HEALTH_SHARE / fleeDps;
            if (reachSec >= killSec * FLEE_REACH_MARGIN)
            {
                if (why)
                {
                    uint8 step = 0;
                    for (PackWhy const& record : why->mobs)
                    {
                        if (record.mob == fleer->GetGUID())
                        {
                            step = record.step;
                            break;
                        }
                    }

                    why->skipped.push_back({helper->GetGUID(), fleer->GetGUID(), PackWhyKind::FleeTooFar, nearest,
                                            fleeRadius, step, reachSec, killSec});
                }

                continue;
            }

            size_t const before = pack.size();
            add(helper, PackWhyKind::FleeHelper, fleer, nearest, fleeRadius);
            if (why && why->mobs.size() > before)
            {
                why->mobs[before].reachSec = reachSec;
                why->mobs[before].killSec = killSec;
            }

            addFormation(helper);
            direct.push_back(helper);  // the fleeing mob's call brings it, and it calls its neighbours where it stands
            if (assistChain)
            {
                spread(before);
                continue;
            }

            if (assistRadius <= 0.0f || !CallsOnEngage(helper))  // [b33_brs]
                continue;

            for (ObjectGuid const& guid : possible)
            {
                Creature* mob = botAI->GetCreature(guid);
                if (mob && mob != helper && IsIdleEnemy(bot, mob) && helper->IsWithinDistInMap(mob, assistRadius) &&
                    mob->CanAssistTo(helper, bot) && helper->IsWithinLOSInMap(mob) &&
                    !crossing(helper, helper->GetPosition(), mob, assistRadius))
                    add(mob, PackWhyKind::Assist, helper, -1.0f, assistRadius);
            }

            callForHelp(before);
        }
    }

    // "[b43_plan] ..." lines, one per tank per 5 s, throttled apart from LogPatrol (so the "[pull patrol]" and
    // "[patrol_meet_imminent]" lines that follow them still log)
    void LogPatrolSurroundings(Player* bot, std::string const& line)
    {
        if (PullPlanner::IsShadow())
            return;  // [lead7] no slot of the tank's own lines

        static std::mutex mutex;
        static std::unordered_map<uint64, uint32> last;
        uint32 const now = getMSTime();
        {
            std::lock_guard<std::mutex> lock(mutex);
            uint32& at = last[bot->GetGUID().GetRawValue()];
            if (at && getMSTimeDiff(at, now) < 5 * IN_MILLISECONDS)
                return;
            at = now ? now : 1;
        }

        LOG_INFO("playerbots", "{}", line);
    }

    // [b43_plan] The idle mobs a patrol's fight brings from beside it. hos-h 20261026-071042-hos-h-5A-21 pull 7
    // (292-332 s): "[pull patrol] patrol Dark Rune Theurgist comes to the group (22.1 yd, eta 0 s): pulls it first,
    // 2 mobs" counted the patrol alone (its plan is cast at the cast spot, and a mob that walks by is "arriving":
    // CollectPack counts no call from where it stands). It walked 13-15 yd beside an idle pack; its Theurgists cast
    // from there (they keep their range) and called it: 7-8 engaged, three deaths. Counted here: every idle static
    // mob within the assistance radius (Creature::CallAssistance, AnyAssistCreatureInRangeCheck: it may assist, in
    // line of sight) of a patrol member where it is now and along its next `seconds` of walk (the walker's track,
    // each member at its offset; the closest point of each stretch, as the [b34_est] walk call), and of where each
    // of its casters stands and casts once the fight is at `fight` (where it engages, or CASTER_KEEP_RANGE from the
    // fight on the line to it: CollectPack's standPoint), with the pack each such mob brings in turn (CollectPack);
    // plus what each caster's stand draws (CollectPack of the caster at it: what sees the melee there, its calls,
    // as addCasterStands counts a pulled-back pack's casters). `known` (the plan's own pack) is left out. `nowAt`: the first mob found beside the patrol where it stands now
    // or would cast from now; `aheadAt`: one found only along its next seconds of walk.
    struct PatrolSurroundings
    {
        std::vector<Unit*> mobs;
        Unit* nowAt = nullptr;
        Unit* aheadAt = nullptr;
    };

    PatrolSurroundings CollectPatrolSurroundings(PlayerbotAI* botAI, Player* bot, GuidVector const& possible,
                                                 PatrolGroup const& patrol, float seconds, Position const& fight,
                                                 std::vector<Unit*> const& known, bool assistChain)
    {
        PatrolSurroundings out;
        float const assistRadius = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        if (assistRadius <= 0.0f || patrol.members.empty() || patrol.track.empty())
            return out;

        G3D::Vector3 const origin = patrol.track.front();
        size_t const samples =
            std::clamp(size_t(std::max(seconds, 0.0f) / PATROL_SAMPLE_SEC) + 1, size_t(1), patrol.track.size());
        // where a caster stands and casts with the fight at `fight`
        auto const standFor = [&fight](G3D::Vector3 const& at) -> G3D::Vector3
        {
            G3D::Vector3 const kill(fight.GetPositionX(), fight.GetPositionY(), fight.GetPositionZ());
            float const toKill = (at - kill).length();
            if (toKill <= CASTER_KEEP_RANGE || toKill <= 0.0f)
                return at;

            return kill + (at - kill) * (CASTER_KEEP_RANGE / toKill);
        };

        struct Caller
        {
            Creature* mob;
            std::vector<G3D::Vector3> now;                             // points of the fight opened now
            std::vector<std::pair<G3D::Vector3, G3D::Vector3>> ahead;  // stretches of its next walk
            std::vector<G3D::Vector3> aheadStands;                     // its cast stand at the walk's end
        };
        std::vector<Caller> callers;
        for (Unit* member : patrol.members)
        {
            Creature* creature = member ? member->ToCreature() : nullptr;
            if (!creature || !creature->IsAlive())
                continue;

            G3D::Vector3 const position(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
            G3D::Vector3 const offset = position - origin;  // a formation member walks at its place by the walker
            Caller caller{creature, {position}, {}, {}};
            bool const caster = KeepsRange(creature);
            if (caster)
                caller.now.push_back(standFor(position));

            for (size_t i = 1; i < samples; ++i)
                caller.ahead.emplace_back(patrol.track[i - 1] + offset, patrol.track[i] + offset);

            if (caster && samples > 1)
                caller.aheadStands.push_back(standFor(patrol.track[samples - 1] + offset));

            callers.push_back(std::move(caller));
        }

        // another patrol walks by on its own forecast; a mob pacing beside its pack stands with it
        auto const walksOn = [](Creature* mob) -> bool
        {
            if (!IsPatrolling(mob))
                return false;

            Creature* walker = nullptr;
            WaypointPath const* path = PatrolPath(mob, &walker);
            std::vector<G3D::Vector3> const* track = PatrolTrack(walker, path);
            return track && !IsPacer(*track);
        };
        std::vector<Unit*> seeds;
        for (ObjectGuid const& guid : possible)
        {
            Creature* mob = botAI->GetCreature(guid);
            if (!mob || !IsIdleEnemy(bot, mob) || IsCrowdControlled(mob) || walksOn(mob) ||
                std::find(patrol.members.begin(), patrol.members.end(), mob) != patrol.members.end() ||
                std::find(known.begin(), known.end(), mob) != known.end())
                continue;

            G3D::Vector3 const at(mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ());
            auto const reachesFrom = [&](Creature* caller, G3D::Vector3 const& point) -> bool
            {
                return (at - point).length() <= assistRadius + caller->GetObjectSize() + mob->GetObjectSize() &&
                       mob->IsWithinLOS(point.x, point.y, point.z + CAREFUL_EYE_HEIGHT);
            };
            bool now = false;
            bool ahead = false;
            for (Caller const& caller : callers)
            {
                if (!mob->CanAssistTo(caller.mob, bot))
                    continue;

                for (G3D::Vector3 const& point : caller.now)
                {
                    if (reachesFrom(caller.mob, point))
                    {
                        now = true;
                        break;
                    }
                }

                if (now)
                    break;

                for (auto const& [from, to] : caller.ahead)
                {
                    if (ahead)
                        break;

                    // the stretch's point nearest the mob ([b34_est] walk call)
                    G3D::Vector3 const leg = to - from;
                    float const squared = leg.dot(leg);
                    float const share = squared > 0.0f ? std::clamp((at - from).dot(leg) / squared, 0.0f, 1.0f) : 0.0f;
                    ahead = reachesFrom(caller.mob, from + leg * share);
                }

                for (G3D::Vector3 const& point : caller.aheadStands)
                {
                    if (ahead)
                        break;

                    ahead = reachesFrom(caller.mob, point);
                }
            }

            if (!now && !ahead)
                continue;

            if (now && !out.nowAt)
                out.nowAt = mob;
            else if (!now && !out.aheadAt)
                out.aheadAt = mob;

            seeds.push_back(mob);
        }

        // What CollectPack of `source` at `spot` brings past the patrol and the plan's pack; the first such mob.
        std::vector<Unit*> around;
        auto const merge = [&](Unit* source, Position const& spot) -> Unit*
        {
            Unit* first = nullptr;
            if (out.mobs.size() >= PACK_CHAIN_MAX_MOBS)
                return first;

            CollectPack(botAI, bot, source, spot, possible, around, FightHere(spot), assistChain, false, nullptr);
            for (Unit* joined : around)
            {
                if (joined && std::find(out.mobs.begin(), out.mobs.end(), joined) == out.mobs.end() &&
                    std::find(patrol.members.begin(), patrol.members.end(), joined) == patrol.members.end() &&
                    std::find(known.begin(), known.end(), joined) == known.end())
                {
                    out.mobs.push_back(joined);
                    if (!first)
                        first = joined;
                }
            }

            return first;
        };

        // each one brings its own pack where it stands (its engage call, its formation, what it calls in turn)
        for (Unit* seed : seeds)
        {
            if (std::find(out.mobs.begin(), out.mobs.end(), seed) == out.mobs.end())
                merge(seed, seed->GetPosition());
        }

        // [b32_pull] a caster's stand draws the fight (the tank and the melee go to it): what sees it there, and its
        // own calls from there, as addCasterStands counts a pulled-back pack's casters
        for (Caller const& caller : callers)
        {
            if (!KeepsRange(caller.mob))
                continue;

            for (G3D::Vector3 const& point : caller.now)
            {
                Unit* const joined = merge(caller.mob, Position(point.x, point.y, point.z));
                if (joined && !out.nowAt)
                    out.nowAt = joined;
            }

            for (G3D::Vector3 const& point : caller.aheadStands)
            {
                Unit* const joined = merge(caller.mob, Position(point.x, point.y, point.z));
                if (joined && !out.nowAt && !out.aheadAt)
                    out.aheadAt = joined;
            }
        }

        return out;
    }

    // "[pack why] <tank>: <candidate> (x y z) N mobs = <counts>; near R, flee-chain only F: #1 <mob> start; ...":
    // why each mob of `pack` comes (its record `why`, null: not recorded), the flights that miss their helper after
    // the list. `what` names the line (a refusal, a plan).
    std::string DescribePackWhy(Player* bot, Unit* candidate, std::vector<Unit*> const& pack, PackWhyRecord const* why,
                                size_t mobs, char const* what)
    {
        std::string line = Acore::StringFormat("[pack why] {}: {} {} ({:.1f} {:.1f} {:.1f}) {} mobs", bot->GetName(),
                                               what, candidate->GetName(), candidate->GetPositionX(),
                                               candidate->GetPositionY(), candidate->GetPositionZ(), mobs);
        if (!why)
            return line + ": not recorded (the pack record limit, or a pack built outside the plan)";

        std::vector<PackWhy> records(why->mobs);
        for (size_t i = records.size(); i < pack.size(); ++i)
            records.push_back({pack[i]->GetGUID(), ObjectGuid::Empty, PackWhyKind::PatrolAdded});

        auto const index = [&records](ObjectGuid guid) -> size_t
        {
            for (size_t i = 0; i < records.size(); ++i)
            {
                if (records[i].mob == guid)
                    return i + 1;
            }

            return 0;
        };

        // the summary: what came near (no flight on the way), and what only a flight chain brings
        static char const* const groups[] = {"start", "social", "pull path", "aggro reaches spot", "wanders to spot",
                                             "patrol walks in", "kill spot path", "kill spot", "formation", "assist",
                                             "call for help", "draws fight", "scripted add", "scripted call",
                                             "flee", "flee miss", "patrol forecast"};
        static_assert(std::size(groups) == size_t(PackWhyKind::PatrolAdded) + 1);
        size_t counts[std::size(groups)] = {};
        size_t closeBy = 0;
        size_t chained = 0;
        uint8 maxStep = 0;
        for (PackWhy const& record : records)
        {
            if (record.step)
            {
                ++chained;
                maxStep = std::max(maxStep, record.step);
            }
            else
            {
                ++closeBy;
                ++counts[size_t(record.kind)];
            }
        }

        std::string summary;
        for (size_t i = 0; i < std::size(groups); ++i)
        {
            if (counts[i])
                summary += Acore::StringFormat("{}{} {}", summary.empty() ? "" : " + ", counts[i], groups[i]);
        }

        if (chained)
            summary += Acore::StringFormat(" + {} flee chain (max step {})", chained, maxStep);

        line += Acore::StringFormat(" = {}; near {}, flee-chain only {}:", summary, closeBy, chained);
        for (size_t i = 0; i < records.size(); ++i)
        {
            if (i >= PACK_WHY_MAX_LISTED)
            {
                line += Acore::StringFormat(" ... +{} more", records.size() - i);
                break;
            }

            PackWhy const& record = records[i];
            Unit* mob = i < pack.size() ? pack[i] : nullptr;
            size_t const source = index(record.source);
            std::string const of = source ? Acore::StringFormat("#{}", source) : std::string("?");
            std::string reason;
            switch (record.kind)
            {
                case PackWhyKind::Start:
                    reason = "start";
                    break;
                case PackWhyKind::Social:
                    reason = Acore::StringFormat("social {:.1f} yd of {}", record.distance, of);
                    break;
                case PackWhyKind::PullPath:
                    reason = Acore::StringFormat("on the pull path ({:.1f} yd from the spot)", record.distance);
                    break;
                case PackWhyKind::SpotAggro:
                    reason = Acore::StringFormat("aggro reaches the spot ({:.1f} yd)", record.distance);
                    break;
                case PackWhyKind::SpotWander:
                    reason = Acore::StringFormat("wanders to the spot ({:.1f} yd)", record.distance);
                    break;
                case PackWhyKind::SpotPatrol:
                    reason = Acore::StringFormat("patrol walks in ({:.1f} yd)", record.distance);
                    break;
                case PackWhyKind::KillPath:
                    reason = Acore::StringFormat("on the way back to the kill spot ({:.1f} yd)", record.distance);
                    break;
                case PackWhyKind::KillSpot:
                    reason = Acore::StringFormat("reaches the kill spot ({:.1f} yd)", record.distance);
                    break;
                case PackWhyKind::Formation:
                    reason = Acore::StringFormat("formation of {} ({:.1f} yd)", of, record.distance);
                    break;
                case PackWhyKind::Assist:
                    reason = Acore::StringFormat("assist {:.1f} yd of {} (LoS, radius {:.0f})", record.distance, of,
                                                 record.radius);
                    break;
                case PackWhyKind::CallForHelp:
                    reason = Acore::StringFormat("call for help {:.0f} from {} ({:.1f} yd)", record.radius, of,
                                                 record.distance);
                    break;
                case PackWhyKind::DrawsFight:
                    reason = Acore::StringFormat("reaches {}, which keeps its range ({:.1f} yd)", of, record.distance);
                    break;
                case PackWhyKind::ScriptedAdd:
                    reason = Acore::StringFormat("scripted add of {}", of);
                    break;
                case PackWhyKind::Scripted:
                    reason = source ? Acore::StringFormat("scripted call of {}", of)
                                    : Acore::StringFormat("scripted call ({:.1f} yd)", record.distance);
                    break;
                case PackWhyKind::FleeHelper:
                    reason = Acore::StringFormat("flee step {} from {} ({:.1f} yd: reaches in {:.1f} s vs dies in "
                                                 "{:.1f} s)", record.step, of, record.distance, record.reachSec,
                                                 record.killSec);
                    break;
                case PackWhyKind::FleeTooFar:
                    break;
                case PackWhyKind::PatrolAdded:
                    reason = "patrol walks in (the plan's patrol forecast)";
                    break;
            }

            if (record.step && record.kind != PackWhyKind::FleeHelper)
                reason += Acore::StringFormat(" [flee step {}]", record.step);

            line += Acore::StringFormat("{} #{} {} @{:.0f}: {}", i ? ";" : "", i + 1,
                                        mob ? mob->GetName() : std::string("?"),
                                        mob ? mob->GetExactDist(candidate) : 0.0f, reason);
        }

        // the flights that die before they get there (their helpers are not in the pack)
        for (PackWhy const& miss : why->skipped)
        {
            size_t const source = index(miss.source);
            line += Acore::StringFormat("; flee from #{} to {:.1f} yd: reaches in {:.1f} s vs dies in {:.1f} s -> not "
                                        "counted", source, miss.distance, miss.reachSec, miss.killSec);
        }

        return line;
    }

    // One "[pack why]" line per tank and candidate per PACK_WHY_THROTTLE_MS: true (and stamped) when due.
    bool PackWhyDue(std::unordered_map<ObjectGuid, uint32>& memo, ObjectGuid candidate, uint32 now)
    {
        if (memo.size() > 32)
        {
            for (auto itr = memo.begin(); itr != memo.end();)
                itr = getMSTimeDiff(itr->second, now) >= PACK_WHY_THROTTLE_MS ? memo.erase(itr) : std::next(itr);
        }

        auto const found = memo.find(candidate);
        if (found != memo.end() && getMSTimeDiff(found->second, now) < PACK_WHY_THROTTLE_MS)
            return false;

        memo[candidate] = now;
        return true;
    }

    // What the tank's own stats take off a melee hit: the miss, dodge, parry and block chances of its character
    // sheet (percent, as Unit::RollMeleeOutcomeAgainst reads them) and the block value of its shield.
    struct TankMitigation
    {
        float miss = 0.0f;
        float dodge = 0.0f;
        float parry = 0.0f;
        float block = 0.0f;
        float blockValue = 0.0f;
        float avoidance = 0.0f;  // miss + dodge + parry against a mob of the tank's level, share (logged)
    };

    // [b32_smpull] A bear tank is planned with its bear's stats. The estimate read the druid as it stood: out of
    // (Dire) Bear Form while it drank or walked, "tank 2366 hp, armor 1670" (81 of 124 looks at the Firebrand Grunt
    // chain, lbrs-n 20261025-221216-lbrs-n-5D-10) against "2676 hp, armor 6632" in form - the same pack cost
    // the bear far more than it does, and refusals of such looks skipped packs it takes. The last stats seen in form
    // (per tank, the map thread's own cache) stand for the bear while it is out of form; none seen yet: as it stands.
    struct BearFormStats
    {
        float health = 0.0f;
        float armor = 0.0f;
        float miss = 0.0f;  // percent, as the character sheet
        float dodge = 0.0f;
        float parry = 0.0f;
    };

    bool InBearForm(Player* tank)
    {
        return tank->GetShapeshiftForm() == FORM_BEAR || tank->GetShapeshiftForm() == FORM_DIREBEAR;
    }

    // The bear's stats to plan with, nullptr when `tank` is no druid tank out of form with a bear seen before.
    BearFormStats const* BearStatsOutOfForm(Player* tank)
    {
        if (!tank || tank->getClass() != CLASS_DRUID || !PlayerbotAI::IsTank(tank))
            return nullptr;

        static thread_local std::unordered_map<uint64, BearFormStats> seen;  // per map thread
        uint64 const key = tank->GetGUID().GetRawValue();
        if (InBearForm(tank))
        {
            BearFormStats& stats = seen[key];
            stats.health = float(tank->GetMaxHealth());
            stats.armor = float(tank->GetArmor());
            stats.miss = tank->GetUnitMissChance(BASE_ATTACK);
            stats.dodge = tank->GetUnitDodgeChance();
            stats.parry = tank->GetUnitParryChance();
            return nullptr;
        }

        auto const found = seen.find(key);
        return found != seen.end() && found->second.health > float(tank->GetMaxHealth()) ? &found->second : nullptr;
    }

    float PlanTankHealth(Player* tank)
    {
        BearFormStats const* bear = BearStatsOutOfForm(tank);
        return bear ? bear->health : float(tank->GetMaxHealth());
    }

    float PlanTankArmor(Player* tank)
    {
        BearFormStats const* bear = BearStatsOutOfForm(tank);
        return bear ? std::max(bear->armor, float(tank->GetArmor())) : float(tank->GetArmor());
    }

    TankMitigation GetTankMitigation(Player* tank)
    {
        TankMitigation mitigation;
        BearFormStats const* bear = BearStatsOutOfForm(tank);  // [b32_smpull]
        mitigation.miss = bear ? bear->miss : tank->GetUnitMissChance(BASE_ATTACK);
        mitigation.dodge = bear ? bear->dodge : tank->GetUnitDodgeChance();
        mitigation.parry = bear ? bear->parry : tank->GetUnitParryChance();
        mitigation.block = bear ? 0.0f : tank->GetUnitBlockChance();
        mitigation.blockValue = mitigation.block > 0.0f ? float(tank->GetShieldBlockValue()) : 0.0f;
        mitigation.avoidance = std::min(mitigation.miss + mitigation.dodge + mitigation.parry, 100.0f) / 100.0f;
        return mitigation;
    }

    // [lead257] The share of a swing's armored damage the tank takes from this mob: the game's attack table
    // (Unit::RollMeleeOutcomeAgainst, one roll over miss, dodge, parry, block, crushing, crit, hit in that order,
    // each chance moved by the weapon skill against the tank's defense, 0.04 % a point; the table ends at 100 %),
    // a blocked hit less the shield's block value, a crushing blow at 150 %, a crit at 200 % (melee crit damage).
    // `outcome` (optional): the table, logged.
    float MeleeTakenShare(Unit* mob, Player* tank, TankMitigation const& mitigation, float armoredHit,
                          std::string* outcome = nullptr)
    {
        Creature const* creature = mob->ToCreature();
        int32 const weaponSkill = int32(mob->GetWeaponSkillValue(BASE_ATTACK, tank));
        int32 const tankMaxSkill = int32(static_cast<Unit const*>(tank)->GetMaxSkillValueForLevel(mob));
        float const skillBonus = 0.04f * float(weaponSkill - tankMaxSkill);  // percent
        // the core's miss chance with the skill difference (Unit::MeleeSpellMissChance), from the plan's base miss
        float const miss = std::max(mob->MeleeSpellMissChance(tank, BASE_ATTACK, weaponSkill - tankMaxSkill, 0) -
                                        tank->GetUnitMissChance(BASE_ATTACK) + mitigation.miss,
                                    0.0f);
        float const expertise = float(mob->GetTotalAuraModifier(SPELL_AURA_MOD_EXPERTISE)) * 0.25f;
        float const dodge = std::max(mitigation.dodge - expertise - skillBonus, 0.0f);
        float const parry = std::max(mitigation.parry - expertise - skillBonus, 0.0f);
        float const block = std::max(mitigation.block - skillBonus, 0.0f);
        float crush = 0.0f;
        if (int32(mob->getLevelForTarget(tank)) >= int32(tank->getLevelForTarget(mob)) + 4 &&
            !(creature && creature->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_CRUSHING_BLOWS)))
        {
            int32 const lacking = int32(mob->GetMaxSkillValueForLevel(tank)) -
                                  std::min(int32(tank->GetDefenseSkillValue(mob)), tankMaxSkill);
            if (lacking >= 15)
                crush = float(lacking) * 2.0f - 15.0f;
        }
        float crit = std::max(mob->GetUnitCriticalChance(BASE_ATTACK, tank), 0.0f);
        if (creature && creature->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_CRIT))
            crit = 0.0f;

        float left = 100.0f;  // the attack table: each outcome takes what the ones before it left
        auto const take = [&left](float chance)
        {
            float const taken = std::clamp(chance, 0.0f, left);
            left -= taken;
            return taken / 100.0f;
        };
        take(miss);
        take(dodge);
        take(parry);
        float const blocked = take(block);
        float const crushing = take(crush);
        float const crits = take(crit);
        float const hits = left / 100.0f;
        float const blockKeeps = armoredHit > 0.0f ? 1.0f - std::min(mitigation.blockValue / armoredHit, 1.0f) : 1.0f;
        float const share = hits + blocked * blockKeeps + 1.5f * crushing + 2.0f * crits;
        if (outcome)
            *outcome = Acore::StringFormat(
                "miss {:.1f} dodge {:.1f} parry {:.1f} block {:.1f} crush {:.1f} crit "
                "{:.1f} %: {:.2f} of a hit",
                miss, dodge, parry, block, crush, crit, share);
        return share;
    }

    // [lead257] The armor reduction of this mob's melee on the tank: the core's (Unit::CalcArmorReducedDamage, with
    // its 75 % cap), on the bear's armor while a bear is out of form.
    float ArmorReduction(Unit* mob, Player* tank)
    {
        constexpr uint32 PROBE_DAMAGE = 1000000;  // a hit large enough that the core's rounding does not count
        float const armor = PlanTankArmor(tank);
        if (armor <= float(tank->GetArmor()))
            return 1.0f - float(Unit::CalcArmorReducedDamage(mob, tank, PROBE_DAMAGE, nullptr)) / float(PROBE_DAMAGE);

        constexpr float CORE_ARMOR_REDUCTION_CAP = 0.75f;  // Unit::CalcArmorReducedDamage
        float level = float(mob->GetLevel());
        if (level > 59.0f)
            level += 4.5f * (level - 59.0f);
        float const value = 0.1f * armor / (8.5f * level + 40.0f);
        return std::clamp(value / (1.0f + value), 0.0f, CORE_ARMOR_REDUCTION_CAP);
    }

    // [lead257] What a mob's spells land per second from its spell data: the damage spells of its SmartAI script
    // (SMART_ACTION_CAST on a repeating in-combat timer, at the timer's mean) or, without one, of its template's
    // spell list (at the fastest the spell's own cooldown or cast lets it), each cast's school damage and periodic
    // damage at the mob's level (SpellEffectInfo::CalcValue) in a magic school (a physical one is melee: armor
    // reduces it). Spells of a fixed count per fight (once, at a health percent) do not repeat and are left out; the
    // measured spell ratio of this dungeon (LandedCalibration) corrects the rest, the rescale's spell factor too.
    struct MobSpells
    {
        float dps = 0.0f;
        float castShare = 0.0f;  // the share of the fight it spends casting: no swing meanwhile
    };

    MobSpells const& GetMobSpells(Creature* creature)
    {
        thread_local std::unordered_map<uint64, MobSpells> cache;  // by entry and level, per map thread
        uint64 const key = (uint64(creature->GetEntry()) << 8) | creature->GetLevel();
        if (auto const found = cache.find(key); found != cache.end())
            return found->second;

        MobSpells spells;
        auto const add = [&](uint32 spellId, float intervalMs)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || info->IsPositive() || (info->GetSchoolMask() & SPELL_SCHOOL_MASK_NORMAL))
                return;

            float damage = 0.0f;
            for (SpellEffectInfo const& effect : info->GetEffects())
            {
                if (effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
                    damage += float(std::max(effect.CalcValue(creature), 0));
                else if (effect.Effect == SPELL_EFFECT_APPLY_AURA &&
                         effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE && effect.Amplitude)
                    damage += float(std::max(effect.CalcValue(creature), 0)) *
                              float(std::max(info->GetMaxDuration(), 0)) / float(effect.Amplitude);
            }
            if (damage <= 0.0f)
                return;

            float const castMs = float(info->CalcCastTime(creature));
            float const everyMs = std::max({intervalMs, castMs, float(info->StartRecoveryTime), 1.0f});
            spells.dps += damage * float(IN_MILLISECONDS) / everyMs;
            spells.castShare += castMs / everyMs;
        };

        if (creature->GetAIName() == "SmartAI")
        {
            SmartAIEventList const scripts =
                sSmartScriptMgr->GetScript(int32(creature->GetEntry()), SMART_SCRIPT_TYPE_CREATURE);
            for (SmartScriptHolder const& holder : scripts)
                if (holder.GetActionType() == SMART_ACTION_CAST && holder.GetEventType() == SMART_EVENT_UPDATE_IC &&
                    holder.event.minMaxRepeat.repeatMax)
                    add(holder.action.cast.spell,
                        float(holder.event.minMaxRepeat.repeatMin + holder.event.minMaxRepeat.repeatMax) / 2.0f);
        }
        else if (CreatureTemplate const* proto = creature->GetCreatureTemplate())
        {
            for (uint32 spellId : proto->spells)
            {
                SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
                if (info)
                    add(spellId, float(std::max(info->RecoveryTime, info->CategoryRecoveryTime)));
            }
        }

        spells.castShare = std::min(spells.castShare, 1.0f);
        return cache.emplace(key, spells).first->second;
    }

    // Melee and spells the tank takes per second from this mob: [lead257] its weapon (min-max damage over its
    // attack time) after the core's armor reduction and the game's attack table against the tank (MeleeTakenShare),
    // in the share of the fight it does not cast, plus its spells from its spell data (GetMobSpells). A caster
    // (MobRoles: it stands and casts) only casts, unless it was pulled round a corner (`casterInMelee`): then it
    // swings between its casts. A paladin class mob with mana is not a caster for that alone: wave 12 read Arcatraz
    // Warders, Sunblade Mage Guards and Bloodwarder Protectors (melee guards) as casters. A caster whose spells its
    // data does not give (a C++ script) is read by its weapon, the only damage the game gives for it, until the
    // dungeon's measured ratio corrects it. `spellPart` (optional): the part of the result that is spells, which
    // armor and avoidance do not reduce; `why` (optional): the inputs.
    float LandedMeleeDps(Unit* mob, Player* tank, bool casterInMelee = false, float* spellPart = nullptr,
                         std::string* why = nullptr)
    {
        if (spellPart)
            *spellPart = 0.0f;

        Creature* creature = mob->ToCreature();
        MobSpells const spells = creature ? GetMobSpells(creature) : MobSpells();
        if (spellPart)
            *spellPart = spells.dps;
        bool const caster = MobRoles::IsCaster(mob) && spells.dps > 0.0f;
        uint32 const attackTime = mob->GetAttackTime(BASE_ATTACK);
        if ((caster && !casterInMelee) || !attackTime)
        {
            if (why)
                *why = Acore::StringFormat("spells {:.0f}/s{}", spells.dps, caster ? " (caster at range)" : "");
            return spells.dps;
        }

        float const hit = (mob->GetFloatValue(UNIT_FIELD_MINDAMAGE) + mob->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2.0f;
        float const reduction = ArmorReduction(mob, tank);
        float const armoredHit = hit * (1.0f - reduction);
        std::string outcome;
        float const share = MeleeTakenShare(mob, tank, GetTankMitigation(tank), armoredHit, why ? &outcome : nullptr);
        float const melee = armoredHit * share * float(IN_MILLISECONDS) / float(attackTime) * (1.0f - spells.castShare);
        if (why)
            *why = Acore::StringFormat(
                "swing {:.0f} / {:.1f} s, armor {:.2f}, {}, casting {:.2f}: melee {:.0f}/s + "
                "spells {:.0f}/s",
                hit, float(attackTime) / float(IN_MILLISECONDS), reduction, outcome, spells.castShare, melee,
                spells.dps);
        return melee + spells.dps;
    }

    struct PackEstimate
    {
        float danger = 0.0f;     // MobDanger sum, as IsPackManageable counts it
        float landedDps = 0.0f;  // on the tank, from everything but a boss (the encounter is built for the group)
        float cost = 0.0f;       // tank healths the pack lands on it over the fight
        float spellCost = 0.0f;  // the part of `cost` that is spells (casters)
        float fightSec = 0.0f;   // the pack's health over the group's damage
        uint32 casters = 0;
        uint32 mobs = 0;
        // The count held against the size cap (withinCap): a weak swarm add (CapWeight) counts less than a full mob;
        // the cost above still has every mob in full.
        float capMobs = 0.0f;
        uint32 rangedMobs = 0;  // [adaptive_cap_b29] mobs that keep their range (KeepsRange)
        bool boss = false;
        // [b44_packs] the pack fought as one stack under the group's AoE (EstimateMode::aoeTargetDps > 0): its kill
        // time and the tank healths it lands meanwhile (every mob alive most of that time); 0: not estimated
        float aoeSec = 0.0f;
        float aoeCost = 0.0f;
        float aoeSpellCost = 0.0f;
        // [lead259] the tank's deepest health drop over the fight (share of its health): what the pack lands before
        // the healers' rate catches up (DropOver), times the dungeon's measured drop ratio (EstimateMode::dropScale);
        // `maxDrop`: the most it may drop and still live through one more gap between two heals at the pack's rate
        float dropRaw = 0.0f;
        float drop = 0.0f;
        float maxDrop = 1.0f;
        float aoeDrop = 0.0f;
    };

    // [b44_packs] User 2026-10-26: «некоторые паки из 20 мобов - это мобы с одного удара, их надо просто АОЕшить».
    // A pack that dies to the group's AoE in a few seconds is pulled whole, as a real group does, if the tank and the
    // healers carry what it lands meanwhile. PACK_AUDIT 2026-10-26 (pull_end of the 20261026-04..07 runs, 24 contents,
    // 2600 pulls of 3+ kills): the AoE spells' damage per second per mob alive (alive: (kills + 1) / 2) is 4-14 % of
    // the group's damage per second on heroic, median 8.4 % (strat-h 4.2, scholo-h 4.3, mgt-h 4.9, dme-h 8.1, sv-h
    // 8.6, sm-h 8.9, mara-h 10.7, brd-h 14.3), with a tank with AoE threat and 1-3 AoE damage dealers;
    // [lead261] The group's AoE on each stacked mob is its members' area spells from their spell data
    // (MemberAreaDps, with the spells' own target caps), and the stack's fight is played out kill by kill
    // (EstimatePack): the focus target takes the group's damage and the AoE, the other stacked mobs the AoE, the
    // mobs that keep their range none of it. That replaces the 8 % share of the group's dps, the 10-target cap, the
    // 0.75 alive share, the 3-mob minimum and the 25 % ranged share; the 12 s kill time ("dies in 10-15 s to AoE")
    // is now "AoE kills the stack faster than one by one".

    // CAREFUL_SWARM_CAP_WEIGHT (wave 21, *.bak_w21idle): a non-elite mob with less than CAREFUL_SWARM_HEALTH_SHARE of
    // the tank's health counts this much of a mob against the pull's size cap. The encounter gates of weak adds (the
    // Crazed Mana-Wraith cloud before Anomalus "15 > 14", the Twilight Initiates before Jedoga "15 > 10") were refused
    // as full packs and skipped ("route end"), the boss never came. Their cost is still estimated in full.
    constexpr float CAREFUL_SWARM_CAP_WEIGHT = 0.5f;
    constexpr float CAREFUL_SWARM_HEALTH_SHARE = 0.3f;

    float CapWeight(Unit* mob, Player* tank)
    {
        Creature* creature = mob->ToCreature();
        if (!creature || creature->isElite() || creature->IsDungeonBoss() || !tank->GetMaxHealth())
            return 1.0f;
        return float(mob->GetMaxHealth()) < CAREFUL_SWARM_HEALTH_SHARE * PlanTankHealth(tank)  // [b32_smpull]
                   ? CAREFUL_SWARM_CAP_WEIGHT
                   : 1.0f;
    }

    // [lead256] the measured ratio real / template of a pack's melee and of its spells (LandedCalibration)
    struct LandedScale
    {
        float melee = 1.0f;
        float spell = 1.0f;
    };

    // How the pack is read: `measured` - a measured careful pull (heroic or max-level content), [lead257] every
    // mode reads the tank's own attack table and the mob's spell data (LandedMeleeDps).
    struct EstimateMode
    {
        bool measured = false;
        LandedScale landedScale;  // the tank's measured calibration in this dungeon (LandedCalibration)
        // [lead209] the measured ratio of a mob entry measured on its own in this dungeon (else landedScale)
        std::unordered_map<uint32, LandedScale> const* landedByEntry = nullptr;
        float aoeTargetDps = 0.0f;  // [b44_packs] the group's AoE damage per second on each stacked mob (0: none)
        uint32 aoeMaxTargets = 0;   // [lead261] the fewest mobs one of the group's area spells hits (0: all)
        bool withBoss = false;      // [lead201] a boss's own health and damage are in the estimate (the boss judge)
        float healHps = 0.0f;       // [lead259] the healers' best rate together (GroupSupport::healHps)
        float healGapSec = 0.0f;    // [lead259] the time between two heals landing (GroupSupport::healGapSec)
        float dropScale = 1.0f;     // [lead259] the dungeon's measured drop ratio (DropCalibration)
    };

    // [lead259] The deepest the tank's health falls (health units) under a damage profile - each life lands its rate
    // until it dies - against healing at `healHps` from the first heal on (`gapSec` in: the first heal lands one gap
    // into the fight): the loss less the healing at each point the profile changes, the highest of them.
    float DropOver(std::vector<std::pair<float, float>> lives, float healHps, float gapSec)
    {
        std::sort(lives.begin(), lives.end(),
                  [](std::pair<float, float> const& left, std::pair<float, float> const& right)
                  { return left.second < right.second; });  // by when it dies
        float rate = 0.0f;
        for (auto const& [landed, life] : lives)
            rate += landed;
        std::vector<float> points{gapSec};
        for (auto const& life : lives)
            points.push_back(life.second);
        std::sort(points.begin(), points.end());

        float deepest = 0.0f;
        float taken = 0.0f;
        float at = 0.0f;
        size_t next = 0;
        for (float point : points)
        {
            while (next < lives.size() && lives[next].second <= at)  // the ones dead by now land nothing more
                rate -= lives[next++].first;
            taken += std::max(rate, 0.0f) * (point - at);
            at = point;
            deepest = std::max(deepest, taken - healHps * std::max(at - gapSec, 0.0f));
        }
        return deepest;
    }

    // The game's global cooldown: the tank's own reaction when no healer heals it.
    constexpr float GCD_SEC = 1.5f;

    // `groupDps` > 0: the fight's cost and length are read from the mobs' health too. The group kills one mob after
    // another in no known order: a mob lives its own kill time plus, on average, half of the others', and lands its
    // melee on the tank all that time.
    PackEstimate EstimatePack(std::vector<Unit*> const& pack, Player* tank, float groupDps, EstimateMode const& mode,
                              bool losPull = false)
    {
        PackEstimate estimate;
        float totalHealth = 0.0f;
        float landedSpell = 0.0f;
        std::vector<std::pair<float, float>> lives;  // landed melee, health
        std::vector<float> spells;                   // the spell part of each life's landed damage
        float summonedOnce = 0.0f;                   // [lead260] summons that come once
        float summonRate = 0.0f;                     // [lead260] and per second of fight
        float summonHealthSum = 0.0f;  // [b36_scholo] the summons' shares of an average mob (ScriptedHelp)
        float summonDamageSum = 0.0f;
        std::vector<bool> quick;  // [b36_scholo] a weak summon: lives its own kill time only
        std::vector<bool> apart;  // [lead261] the life keeps its range: out of the stack's AoE
        for (Unit* mob : pack)
        {
            estimate.danger += MobDanger(mob, tank);
            ++estimate.mobs;
            float const chainWeight = ChainStepWeight(pack, mob);  // [chain_step_weight]
            estimate.capMobs += CapWeight(mob, tank) * chainWeight;
            if (Creature* ranged = mob->ToCreature(); ranged && KeepsRange(ranged))
                ++estimate.rangedMobs;
            if (IsBossMob(mob))
            {
                estimate.boss = true;
                if (!mode.withBoss)
                    continue;
            }

            if (Creature* creature = mob->ToCreature())
            {
                ScriptedHelp const& help = GetScriptedHelp(creature);
                summonedOnce += help.summons;
                summonRate += help.summonsPerSec;
                float const each = help.summons + help.summonsPerSec;  // the shares, by the entry's summons
                summonHealthSum += help.summonHealth * each;           // [b36_scholo]
                summonDamageSum += help.summonDamage * each;
            }

            float spell = 0.0f;
            LandedScale scale = mode.landedScale;  // [lead209] the mob's own entry when measured, else the trash
            if (mode.landedByEntry)
                if (auto const own = mode.landedByEntry->find(mob->GetEntry()); own != mode.landedByEntry->end())
                    scale = own->second;
            float const templateDps = LandedMeleeDps(mob, tank, losPull, &spell);
            // [lead256] melee and spells by their own measured ratios
            float const landed = (templateDps - spell) * scale.melee + spell * scale.spell;
            spell *= scale.spell;
            if (spell > 0.0f)
                ++estimate.casters;

            estimate.landedDps += landed;
            landedSpell += spell;
            float const health = float(mob->GetHealth());
            totalHealth += health;
            lives.emplace_back(landed, health);
            spells.push_back(spell);
            quick.push_back(false);
            Creature* keeps = mob->ToCreature();
            apart.push_back(keeps && KeepsRange(keeps));
        }

        // What the pack summons while it fights, each read as an average mob of the pack, [b36_scholo] scaled by the
        // summon's own health and damage against its summoner's (ScriptedHelp).
        // [lead260] the summons over the pack's own kill time at the group's damage (no damage known: the ones that
        // come once)
        float const baseSec = groupDps > 0.0f ? totalHealth / groupDps : 0.0f;
        uint32 const summoned = uint32(std::lround(summonedOnce + summonRate * baseSec));
        if (summoned && !lives.empty() && summonedOnce + summonRate > 0.0f)
        {
            float const healthShare = summonHealthSum / (summonedOnce + summonRate);
            float const damageShare = summonDamageSum / (summonedOnce + summonRate);
            float const share = 1.0f / float(lives.size());
            float const landed = estimate.landedDps * share * damageShare;
            float const spell = landedSpell * share * damageShare;
            float const health = totalHealth * share * healthShare;
            float const danger = estimate.danger / float(estimate.mobs);
            bool const diesAsItComes = groupDps > 0.0f && health <= groupDps * GCD_SEC;  // [lead260]
            for (uint32 i = 0; i < summoned; ++i)
            {
                lives.emplace_back(landed, health);
                spells.push_back(spell);
                quick.push_back(diesAsItComes);
                apart.push_back(false);
                estimate.landedDps += landed;
                landedSpell += spell;
                totalHealth += health;
                estimate.danger += danger;
                ++estimate.mobs;
                estimate.capMobs += 1.0f;
            }
        }

        if (groupDps > 0.0f && tank->GetMaxHealth())
        {
            float damage = 0.0f;
            float spellDamage = 0.0f;
            for (size_t i = 0; i < lives.size(); ++i)
            {
                auto const& [landed, health] = lives[i];
                float const life =
                    (quick[i] ? health : health + (totalHealth - health) / 2.0f) / groupDps;  // [b36_scholo]
                damage += landed * life;
                spellDamage += spells[i] * life;
            }

            float const tankHealth = PlanTankHealth(tank);  // [b32_smpull] a bear out of form: its bear's health
            estimate.cost = damage / tankHealth;
            estimate.spellCost = spellDamage / tankHealth;
            estimate.fightSec = totalHealth / groupDps;
            // [lead259] the drop: every life's rate until it dies against the healers' rate after their first heal
            std::vector<std::pair<float, float>> profile;
            for (size_t i = 0; i < lives.size(); ++i)
                profile.emplace_back(
                    lives[i].first,
                    (quick[i] ? lives[i].second : lives[i].second + (totalHealth - lives[i].second) / 2.0f) / groupDps);
            float const gapSec = mode.healHps > 0.0f ? mode.healGapSec : estimate.fightSec;
            estimate.dropRaw = std::min(DropOver(profile, mode.healHps, gapSec) / tankHealth, estimate.cost);
            estimate.drop = estimate.dropRaw * mode.dropScale;
            estimate.maxDrop =
                std::clamp(1.0f - estimate.landedDps * std::max(mode.healGapSec, GCD_SEC) / tankHealth, 0.0f, 1.0f);
        }

        // [b44_packs] The same pack under the group's AoE, [lead261] played out kill by kill: the focus target (the
        // first one alive) takes the group's damage plus the AoE, every other stacked mob (up to the area spells'
        // target cap) the AoE; a mob that keeps its range takes none until it is the focus. Each mob lands its rate
        // until it dies.
        if (groupDps > 0.0f && mode.aoeTargetDps > 0.0f && !estimate.boss && tank->GetMaxHealth() &&
            totalHealth > 0.0f)
        {
            std::vector<float> left;
            for (auto const& life : lives)
                left.push_back(life.second);
            std::vector<float> died(lives.size(), 0.0f);
            std::vector<bool> alive(lives.size(), true);
            float now = 0.0f;
            for (size_t kill = 0; kill < lives.size(); ++kill)
            {
                std::vector<float> rate(lives.size(), 0.0f);
                size_t focus = lives.size();
                uint32 hit = 0;
                for (size_t i = 0; i < lives.size(); ++i)
                {
                    if (!alive[i])
                        continue;
                    if (focus == lives.size())
                    {
                        focus = i;
                        rate[i] += groupDps;
                    }
                    if (!apart[i] && (!mode.aoeMaxTargets || hit < mode.aoeMaxTargets))
                    {
                        rate[i] += mode.aoeTargetDps;
                        ++hit;
                    }
                }
                float step = -1.0f;
                for (size_t i = 0; i < lives.size(); ++i)
                    if (alive[i] && rate[i] > 0.0f && (step < 0.0f || left[i] / rate[i] < step))
                        step = left[i] / rate[i];
                if (step < 0.0f)
                    break;
                now += step;
                for (size_t i = 0; i < lives.size(); ++i)
                {
                    if (!alive[i])
                        continue;
                    left[i] -= rate[i] * step;
                    if (left[i] <= rate[i] * step * 1e-4f)
                    {
                        alive[i] = false;
                        died[i] = now;
                    }
                }
            }

            float const tankHealth = PlanTankHealth(tank);
            estimate.aoeSec = now;
            if (tankHealth > 0.0f)
            {
                float damage = 0.0f;
                float spellDamage = 0.0f;
                std::vector<std::pair<float, float>> profile;
                for (size_t i = 0; i < lives.size(); ++i)
                {
                    damage += lives[i].first * died[i];
                    spellDamage += spells[i] * died[i];
                    profile.emplace_back(lives[i].first, died[i]);
                }
                estimate.aoeCost = damage / tankHealth;
                estimate.aoeSpellCost = spellDamage / tankHealth;
                float const gapSec = mode.healHps > 0.0f ? mode.healGapSec : estimate.aoeSec;
                estimate.aoeDrop =
                    std::min(DropOver(profile, mode.healHps, gapSec) / tankHealth, estimate.aoeCost) * mode.dropScale;
            }
        }

        return estimate;
    }

    // Tank healths the pull really costs the healers: [lead258] the planned cost itself - the dungeon's measured
    // landed ratios (LandedCalibration) already make it what the tank really takes (the wave 13 power law
    // 0.33 cost^0.73 read the uncalibrated plan).
    float TakenCost(PackEstimate const& estimate) { return std::max(estimate.cost, 0.0f); }

    // Tank healths the group's healers restore from full mana, above the mana each keeps back: [lead258] their max
    // mana times their health per mana (spell data, or this dungeon's measured healing per mana spent), less their
    // low mana line (the config's), over the tank's health.
    float HealerPool(GroupSupport const& support)
    {
        return support.tankHealth > 0.0f ? std::max(support.healMana - support.healReserve, 0.0f) / support.tankHealth
                                         : 0.0f;
    }

    // Healer mana (percent) a measured careful pull of this cost needs before it starts: [lead258] the low mana line
    // plus the mana its cost takes at the healers' health per mana; -1 without a healer.
    float HealerManaNeed(PackEstimate const& estimate, GroupSupport const& support)
    {
        if (support.healMana <= 0.0f)
            return -1.0f;
        return std::min(100.0f * (support.healReserve + TakenCost(estimate) * support.tankHealth) / support.healMana,
                        100.0f);
    }

    // [lead258] The pack's average damage on the tank per second: what it lands over the fight (the estimate's
    // lives: each mob its kill time plus half the others') over the fight's length.
    float AverageDtps(PackEstimate const& estimate, float tankHealth);

    // [lead263] The tank and the healers carry the plan: the tank's drop (DropP90) within the most it may drop
    // (PackEstimate::maxDrop) and the healer mana it takes within their pool (no healer: no mana term). `why`: the
    // terms.
    bool TankCarries(PackEstimate const& estimate, GroupSupport const& support, std::string* why = nullptr)
    {
        float const pool = HealerPool(support);
        bool const drop = std::max(estimate.drop, 0.0f) <= estimate.maxDrop;
        bool const mana = pool <= 0.0f || TakenCost(estimate) <= pool;
        if (why)
            *why = Acore::StringFormat("drop {:.2f} of at most {:.2f}, healer mana {:.2f} of {:.2f} tank hp",
                                       estimate.drop, estimate.maxDrop, TakenCost(estimate), pool);
        return drop && mana;
    }

    float AverageDtps(PackEstimate const& estimate, float tankHealth)
    {
        return estimate.fightSec > 0.0f ? estimate.cost * tankHealth / estimate.fightSec : estimate.landedDps;
    }

    // Each limit a pack is held against, as a share of it (1.0: just fits).
    struct OverloadTerms
    {
        float size = 0.0f;     // danger against the group's capacity (levelling) or the healers' mana (measured)
        float hold = 0.0f;     // mobs against what the tank's spells hold, plus the loose mobs the dps kill off it
        float peak = 0.0f;     // melee on the tank before a mob dies (levelling) or its average against what the
                               // healers heal (measured)
        float drain = 0.0f;    // the fight's melee against what the healers heal over it plus half the tank's health
                               // (levelling) or the tank's 90th percentile health drop against its most
                               // (PackEstimate::maxDrop)
        float comfort = 0.0f;  // measured: the drop against the size the group prefers (half the most)
        float drop = 0.0f;     // measured: the tank's 90th percentile health drop
        float holdMobs = 0.0f;

        // What the group can take at all; `withComfort`: and prefers when it has a choice.
        float Max(bool withComfort = false) const
        {
            return std::max({size, hold, peak, drain, withComfort ? comfort : 0.0f});
        }
    };

    // Measured careful pulls: the tank's health drop over the fight in 9 pulls of 10: [lead259] the estimate's own
    // drop (DropOver) times the upper quantile of this dungeon's measured drop ratios (DropCalibration); nothing
    // measured: the drop as the game's numbers give it.
    float DropP90(PackEstimate const& estimate) { return std::max(estimate.drop, 0.0f); }

    // How far the pack is over what the group takes (1.0: just fits): the group's danger capacity (levelling) or the
    // healers' mana for the fight (measured), the tank's spells hold the pack but for a couple of loose mobs, and the
    // tank's health carries the melee it takes, at once and over the fight. About 4-5 mobs for a tank without area
    // threat, more with Consecration or Thunder Clap, fewer for a soft group. Measured: (a) the tank's drop in 9 pulls
    // of 10 leaves it one more heal gap at the pack's rate (PackEstimate::maxDrop) and (b) the healers keep up with the
    // pack's average damage and have the mana for it; the tank's own health, armor and avoidance are in the cost, the
    // healers' gear scales what they heal.
    float CarefulOverload(Player* bot, PackEstimate const& estimate, PartyStrength const& strength,
                          GroupSupport const& support, TankKit const& kit, bool measured,
                          OverloadTerms* out = nullptr)
    {
        OverloadTerms terms;
        // [lead258] measured without a healer: no healer mana to run out of (the drop term holds the tank's health)
        float const pool = HealerPool(support);
        terms.size = measured
                         ? (pool > 0.0f ? TakenCost(estimate) / pool : 0.0f)
                         : SwarmDanger(estimate.danger, estimate.mobs, strength) / PackCapacity(bot, strength, support);
        // [lead259] a lone mob's drop too: it is no limit there, but its fight measures the drop ratio
        terms.drop = DropP90(estimate);
        if (estimate.mobs > 1)
        {
            terms.holdMobs =
                std::clamp(std::round(kit.holdMobs * strength.PullScale()), CAREFUL_MIN_HOLD, CAREFUL_MAX_HOLD) +
                CAREFUL_LOOSE_MOBS;
            terms.hold = float(estimate.mobs) / terms.holdMobs;

            float const maxHealth = PlanTankHealth(bot);  // [b32_smpull]
            // [lead259] the drop against the most the tank may drop (EstimatePack::maxDrop); the comfort (measured):
            // against half of it, a pull that still fits when an equal pack joins it. [lead266] Levelling reads the
            // same two terms (the 7 % / 4.5 % of the tank's health per second and the half tank health pool of
            // waves 4-12 are gone).
            terms.drain = estimate.maxDrop > 0.0f ? terms.drop / estimate.maxDrop : 99.0f;
            if (measured)
                terms.comfort = 2.0f * terms.drain;
            // [lead258] the pack's average damage per second against the healers' best rate together (their spells
            // and spell power: the gear is in it); without a healer the drop term alone
            if (support.healHps > 0.0f)
                terms.peak = AverageDtps(estimate, maxHealth) / support.healHps;
        }

        if (out)
            *out = terms;

        return terms.Max();
    }

    // Everyone alive and topped up: what a real group waits for before it takes a pack bigger than it likes.
    // `why`: what is missing, for the hold line (wave 7 logged "not topped up (tank 100 % hp, lowest healer 100 %
    // mana)" for a healer 30-39 yd away and for a stale "healer dead").
    bool IsGroupToppedUp(Player* bot, std::string* why = nullptr)
    {
        // [lead17b] the leader's one readiness verdict (its rest rule tops the group up)
        if (bool ready = false; PullReadiness::LeaderVerdict(bot, ready, why))
            return ready;
        // [b30_topped_up] a member short of the line that has not regenerated for REGEN_STALL_MS counts as topped up
        auto const toppedUp = [why, bot](Player* member)
        {
            if (!member->IsAlive() ||
                (member->GetHealthPct() < CAREFUL_FALLBACK_HEALTH &&
                 !ToppedUpWatch::NotRegenerating(bot, member, ToppedUpWatch::KIND_HEALTH, member->GetHealthPct())))
            {
                if (why)
                    *why = Acore::StringFormat("{} at {:.0f} % health", member->GetName(), member->GetHealthPct());
                return false;
            }

            if (PlayerbotAI::IsHeal(member) && member->getPowerType() == POWER_MANA &&
                member->GetPowerPct(POWER_MANA) < CAREFUL_FALLBACK_MANA &&
                !ToppedUpWatch::NotRegenerating(bot, member, ToppedUpWatch::KIND_MANA,
                                                member->GetPowerPct(POWER_MANA)))
            {
                if (why)
                    *why = Acore::StringFormat("{} at {:.0f} % mana", member->GetName(),
                                               member->GetPowerPct(POWER_MANA));
                return false;
            }

            return true;
        };

        if (!toppedUp(bot))
            return false;

        std::string absent;
        if (!IsGroupPresent(bot, &absent))
        {
            if (why)
                *why = "the group is not all here: " + absent;
            return false;
        }

        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member != bot && IsPresent(bot, member) && !IsFarMember(bot, member) &&
                    !toppedUp(member))
                    return false;
            }
        }

        return true;
    }

    // [careful_sit_below_line] A member that sits (eats, drinks) below its own rest line (RestState::WantsDrink /
    // WantsFood): the group is not ready for the quick lone mob / the ready last resort. One that sits above its line
    // (a drink "for company", topping up to full) holds no pull: it stands up when the fight starts (RestDone).
    // DOWNTIME_AUDIT 2026-10-25: after every pull somebody sat, and any sitter turned the 3 s quick path off.
    bool SitsBelowOwnLine(Player* member)
    {
        if (!member->IsSitState())
            return false;

        bool const eating = RestState::IsEating(member);
        bool const drinking = RestState::IsDrinking(member);
        if (!eating && !drinking)
            return true;  // sits without food or drink (a cast, an emote): counted as before

        return (drinking && RestState::WantsDrink(member)) || (eating && RestState::WantsFood(member));
    }

    bool IsAnyoneSittingBelowLine(Player* bot)
    {
        if (SitsBelowOwnLine(bot))
            return true;

        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member != bot && IsPresent(bot, member) && !IsFarMember(bot, member) &&
                    SitsBelowOwnLine(member))
                    return true;
            }
        }

        return false;
    }

    // [b33_brd] brd-n 20261026-000434-brd-n-5F-6 pull #60: a hunter's pet (Gorilla) was engaged through the closed Bar
    // Door by a Fireguard Destroyer that could only reach it the long way round, through Fineous Darkvire's room
    // (z -71): its assistance calls woke a Blazing Fireguard and four Warbringer Constructs there ([far engage] 52-89
    // yd, dz 15-22, all on the Gorilla). The core keeps only the pet in combat (CombatManager::UpdateOwnerCombatState
    // sets UNIT_FLAG_PET_IN_COMBAT on the owner, not UNIT_FLAG_IN_COMBAT), so the hunter dropped combat while the train
    // still walked in, "a group member still fights" let go 3 s after the empty pull #59, and the planner pulled one
    // Construct ("expects 2") into 9 mobs: 3 deaths. A member's pet with a live, non-evading attacker is a fight too
    // (a mob still coming). PET_FIGHT_MAX_MS: a pet held in combat longer than that by a mob that never arrives (an
    // unreachable chaser) stops holding the group, as before.
    constexpr uint32 PET_FIGHT_MAX_MS = 60 * IN_MILLISECONDS;

    bool IsPetFighting(Player* owner)
    {
        Pet* pet = owner ? owner->GetPet() : nullptr;
        static std::mutex petFightLock;
        static std::unordered_map<uint64, uint32> petFightSince;  // pet guid -> first seen fighting, under the lock
        bool attacked = false;
        if (pet && pet->IsInWorld() && pet->IsAlive() && pet->IsInCombat())
        {
            for (Unit* attacker : pet->getAttackers())
            {
                Creature* creature = attacker ? attacker->ToCreature() : nullptr;
                if (creature && creature->IsAlive() && !creature->IsInEvadeMode())
                {
                    attacked = true;
                    break;
                }
            }
        }

        if (!pet)
            return false;

        uint64 const key =  // [lead185] by instance too (a guardian's GUID is the map's)
            pet->GetGUID().GetRawValue() ^ (uint64(pet->GetInstanceId()) * 0x9E3779B97F4A7C15ULL);
        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> guard(petFightLock);
        if (!attacked)
        {
            petFightSince.erase(key);
            return false;
        }

        if (petFightSince.size() > 512)
            petFightSince.clear();
        auto const [it, inserted] = petFightSince.try_emplace(key, now ? now : 1);
        if (inserted)
            return true;

        return getMSTimeDiff(it->second, now) < PET_FIGHT_MAX_MS;
    }

    bool IsGroupFighting(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->GetMap() == bot->GetMap() && member->IsAlive() &&
                member->IsInCombat())
                return true;
            // [b33_brd] a pet fought for its owner (the bot's own pet too: callers test bot->IsInCombat() only)
            if (member && member->GetMap() == bot->GetMap() && member->IsAlive() && !member->IsInCombat() &&
                IsPetFighting(member))
            {
                static thread_local uint32 petHoldLogMs = 0;  // per map thread
                uint32 const now = getMSTime();
                if (!petHoldLogMs || getMSTimeDiff(petHoldLogMs, now) > 10 * IN_MILLISECONDS)
                {
                    petHoldLogMs = now ? now : 1;
                    LOG_INFO("playerbots", "[b33_brd] pet_fight_hold: {}: {}'s pet is attacked while its owner is out "
                             "of combat (a mob still coming): the group counts as fighting",
                             bot->GetName(), member->GetName());
                }
                return true;
            }
        }

        return false;
    }

    // Sweep batch4, Drak'Tharon Keep (dtk-n 21:29 and 22:12, dtk-h 22:15: 3 wipes). Trollgore fights by himself every
    // ~38 s (boss_trollgore.cpp: his invader waves, boss state IN_PROGRESS for 3-5 s each), 57-80 yd from the trash
    // of his room (z +15, no aggro or assistance link to it); each trash fight there that overlapped a wave had him
    // on the group 3-4 s after it started. A dungeon boss that fights without the group runs his encounter on its own
    // clock, and a fight near him while it runs draws him in. Per boss (instance and guid, every map thread under the
    // lock): when his last fight without the group started, and the gap between two such starts.
    constexpr float BOSS_ROOM_REACH = 90.0f;  // a fight this near (3D) a boss that fights by himself draws him
    constexpr uint32 BOSS_CYCLE_MIN_MS = 10 * IN_MILLISECONDS;  // a gap between two of his own fights: his cycle
    constexpr uint32 BOSS_CYCLE_MAX_MS = 180 * IN_MILLISECONDS;
    constexpr uint32 BOSS_CYCLE_MEMO_MS = 5 * MINUTE * IN_MILLISECONDS;  // not looked at this long: forgotten
    constexpr float BOSS_CYCLE_MARGIN_SEC = 5.0f;  // the trash fight ends this long before his next own fight
    constexpr uint32 BOSS_WINDOW_MAX_HOLD_MS = 90 * IN_MILLISECONDS;  // held this long: taken as his fight
    constexpr uint32 BOSS_HOLD_GAP_MS = 10 * IN_MILLISECONDS;          // no hold this long: the wait starts anew
    constexpr size_t BOSS_CYCLE_CACHE_MAX = 512;
    constexpr float BOSS_READY_HEALTH = 95.0f;  // a pull that brings a boss: the group as ready as for the boss
    constexpr float BOSS_READY_MANA = 90.0f;
    // [rest_topup_real_boss] STN92_EFFICIENCY 2026-10-25: a mini-boss (flagged a dungeon boss, no DungeonEncounter.dbc
    // encounter of its map: IsEncounterBoss) is no full boss top-up; the group is as ready as for a hard trash pull,
    // just under the rest line the members drink to before it (RestState::MINI_BOSS_TOPUP_PCT), or nobody drinks for
    // it and the tank waits for good.
    constexpr float MINI_BOSS_READY_HEALTH = 80.0f;
    constexpr float MINI_BOSS_READY_MANA = RestState::MINI_BOSS_TOPUP_PCT - 5.0f;
    // [chain_pull] STN92_EFFICIENCY 2026-10-25 (43 pulls with a single kill, post-fight settle 127 s, median gap
    // 7.5 s): within CHAIN_PULL_WINDOW_MS of the group's last fight a plan of at most the cap's mobs whose pull mob is
    // within CHAIN_PULL_RANGE is pulled at once when every member stands above its own rest lines (RestState) and the
    // pack is cheap (the quick single's cost cap): no wait for the danger gate's health top-up. false: as before.
    constexpr bool CHAIN_PULL = true;
    constexpr uint32 CHAIN_PULL_WINDOW_MS = 15 * IN_MILLISECONDS;
    constexpr float CHAIN_PULL_RANGE = 30.0f;
    constexpr float CHAIN_PULL_TANK_HEALTH = 60.0f;  // the tank's own floor for a chained pull
    // [plan_route_blocker] STN92_EFFICIENCY 2026-10-25 (st-n-92 Mijan / Hukku: "[pull walk] the walk ... for Mijan
    // wakes Atal'ai Slave: not this way" -> "no manageable pull around Mijan (best 0 mobs, cost 0.00)" -> refused,
    // edge hold, skip of every troll): when no plan reaches the target because the way in wakes another idle pack,
    // that pack is planned instead (its own plan, fallback, lone mob, guarantee), the route target's look answered with it. A pack
    // on the only way is never skipped (user rule; the harness's [route_blocker_body_pull]). false: refused as before.
    constexpr bool PLAN_ROUTE_BLOCKER = true;

    struct BossCycle
    {
        uint32 startMs = 0;     // his last fight without the group began (0: none seen)
        uint32 periodMs = 0;    // the gap between his last two such starts (0: not known)
        uint32 seenMs = 0;
        bool fighting = false;  // he fought without the group at the last look
    };

    std::mutex bossCycleLock;
    std::map<std::pair<uint32, uint64>, BossCycle> bossCycles;  // by instance id and boss guid

    // The boss fights, and neither the bot nor anyone of its group (nor a pet) is on his threat list.
    bool FightsWithoutGroup(Player* bot, Unit* boss)
    {
        if (!boss->IsInCombat() || boss->IsEngagedBy(bot) || (bot->GetPet() && boss->IsEngagedBy(bot->GetPet())))
            return false;

        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || member->GetMap() != bot->GetMap())
                    continue;

                if (boss->IsEngagedBy(member) || (member->GetPet() && boss->IsEngagedBy(member->GetPet())))
                    return false;
            }
        }

        return true;
    }

    // One look at every living dungeon boss of `possible`: the start of each fight he has without the group.
    void SampleBossCycles(PlayerbotAI* botAI, Player* bot, GuidVector const& possible)
    {
        uint32 const now = getMSTime();
        uint32 const instance = bot->GetInstanceId();
        std::lock_guard<std::mutex> lock(bossCycleLock);
        if (bossCycles.size() >= BOSS_CYCLE_CACHE_MAX)
            bossCycles.clear();

        for (ObjectGuid const& guid : possible)
        {
            Unit* unit = botAI->GetUnit(guid);
            if (!unit || !unit->IsAlive() || !IsBossMob(unit))
                continue;

            BossCycle& cycle = bossCycles[{instance, guid.GetRawValue()}];
            if (cycle.seenMs && getMSTimeDiff(cycle.seenMs, now) > BOSS_CYCLE_MEMO_MS)
                cycle = BossCycle();

            bool const fighting = FightsWithoutGroup(bot, unit);
            if (fighting && !cycle.fighting)
            {
                uint32 const gap = cycle.startMs ? getMSTimeDiff(cycle.startMs, now) : 0;
                if (gap >= BOSS_CYCLE_MIN_MS && gap <= BOSS_CYCLE_MAX_MS)
                    cycle.periodMs = gap;

                cycle.startMs = now;
            }

            cycle.fighting = fighting;
            cycle.seenMs = now;
        }
    }

    // What brings `boss` into a fight at `spot` that lasts about `fightSec` with `pack` (nullptr: he stays out):
    // - his own encounter runs and the spot is within BOSS_ROOM_REACH of him: he fights without the group now, or
    //   the next fight of his cycle starts before this one ends (`waitSec`: how long the pull waits for it to fit);
    // - idle: the core's aggro of him at the spot (InCoreAggroReach, he sees it), a pack mob of his formation, or a
    //   pack mob's engage call for assistance (Creature::CallAssistance: its radius, he sees it and may assist).
    char const* BossDrawnIn(Player* bot, Unit* boss, Position const& spot, float fightSec,
                            std::vector<Unit*> const& pack, float& waitSec)
    {
        waitSec = 0.0f;
        Creature* creature = boss->ToCreature();
        if (!creature || boss->GetExactDist(spot) > BOSS_ROOM_REACH)
            return nullptr;

        // [pack_skip_sealed_mob] a banished / immune boss (Hellmaw under his ritualists' channel) is drawn by nothing
        if (IsSealedFromFight(creature))
            return nullptr;

        if (FightsWithoutGroup(bot, boss))
        {
            waitSec = 1.0f;
            return "he fights by himself now";
        }

        {
            uint32 const now = getMSTime();
            std::lock_guard<std::mutex> lock(bossCycleLock);
            auto const found = bossCycles.find({bot->GetInstanceId(), boss->GetGUID().GetRawValue()});
            if (found != bossCycles.end() && found->second.periodMs && found->second.startMs)
            {
                float const period = float(found->second.periodMs) / float(IN_MILLISECONDS);
                float const since = float(getMSTimeDiff(found->second.startMs, now)) / float(IN_MILLISECONDS);
                float const next = period - std::fmod(since, period);
                if (fightSec + BOSS_CYCLE_MARGIN_SEC > next)
                {
                    waitSec = next;
                    return "his next fight of his own starts before this one ends";
                }
            }
        }

        if (!IsIdleEnemy(bot, boss))
            return nullptr;

        if (InCoreAggroReach(boss, bot, spot, CAREFUL_AGGRO_MARGIN) &&
            boss->IsWithinLOS(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ() + CAREFUL_EYE_HEIGHT))
            return "his aggro reaches the fight";

        CreatureGroup const* formation = creature->GetFormation();
        float const assistRadius = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        for (Unit* mob : pack)
        {
            Creature* member = mob->ToCreature();
            if (formation && member && member->GetFormation() == formation)
                return "a pack mob is of his formation";

            if (assistRadius > 0.0f && mob->IsWithinDistInMap(boss, assistRadius) && creature->CanAssistTo(mob, bot) &&
                mob->IsWithinLOSInMap(boss))
                return "a pack mob's call for assistance reaches him";
        }

        return nullptr;
    }

    // A pull that brings a boss is the boss fight: everyone here alive, at BOSS_READY_HEALTH, the mana users at
    // BOSS_READY_MANA, nobody eating or drinking, no group buff missing (`askBuffs`). Empty: ready; else what is
    // missing.
    // `realBoss` false ([rest_topup_real_boss]): a mini-boss, held to MINI_BOSS_READY_HEALTH / MINI_BOSS_READY_MANA.
    // [lead51] the seconds a boss fight lasts: his health over the group's damage (the "estimated group dps" value)
    float BossFightSec(Player* bot, Unit* boss)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        // [lead86] b129 rfc-h: Taragaman's fight was estimated at 100 s (the rolling dps, pulled down by short trash
        // pulls) and died in 54 s in b128 (11988 dps): the group's sustained measured dps when it is higher
        // [lead234] b188 rfc-h: every boss estimated at 101-106 s, killed in 36-64 s - the run's sustained dps is
        // the trash pulls' (ramp-up, few targets in reach); the group's measured dps on its earlier boss kills when
        // there is one. The boss lines (mana x seconds) asked 100 % of the hunters and the paladin tank
        float const groupDps =
            std::max({botAI ? botAI->GetAiObjectContext()->GetValue<float>("estimated group dps")->Get() : 0.0f,
                      GroupDamage::SustainedDps(bot),
                      bot->GetGroup() ? Leader::Knowledge::BossDps(bot->GetGroup()->GetGUID()) : 0.0f});
        return boss ? float(boss->GetHealth()) / std::max(groupDps, 1.0f) : 0.0f;
    }

    // [lead78] ONE fight length for the next pull, the same for the boss gate, the planner's hold and the members'
    // rest (RestState::PredictNextFight's published fight carries it): the boss's own fight when the pull brings
    // him, else the trash fight
    float NextPullSec(Player* bot, Unit* boss)
    {
        RestState::NextFight next;
        float const published = RestState::PredictNextFight(bot, next) ? next.fightSec : 0.0f;
        return std::max(published, BossFightSec(bot, boss));
    }

    std::string BossReadyGap(Player* bot, bool askBuffs, bool realBoss = true, Unit* boss = nullptr)
    {
        float const readyHealth = realBoss ? BOSS_READY_HEALTH : MINI_BOSS_READY_HEALTH;
        float const readyMana = realBoss ? BOSS_READY_MANA : MINI_BOSS_READY_MANA;
        float const bossSec = NextPullSec(bot, boss);  // [lead51] [lead78] the one next-pull length
        auto const gap = [readyHealth, readyMana, boss, bossSec](Player* member) -> std::string
        {
            if (!member->IsAlive())
                return member->GetName() + " dead";

            if (member->GetHealthPct() < readyHealth)
                return Acore::StringFormat("{} at {:.0f} % health", member->GetName(), member->GetHealthPct());

            // [lead51] the mana THIS boss fight takes from this member (FightManaLine), not a fixed line
            float const manaLine = boss ? RestState::FightManaLine(member, bossSec, 0.0f, nullptr, true) : readyMana;
            if (member->getPowerType() == POWER_MANA && member->GetPowerPct(POWER_MANA) < manaLine)
                return Acore::StringFormat("{} at {:.0f} % mana (needs {:.0f} %)", member->GetName(),
                                           member->GetPowerPct(POWER_MANA), manaLine);

            if (member->IsSitState())
                return member->GetName() + " sits";

            // [boss_pull_ready_gate] a member still casting its pre-boss rebuff round (the leader's ready check
            // answers "buffing" for it the same way) is not ready: sh-h w24 pulled Kargath 9 s into the buffing
            if (PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member); memberAI && memberAI->forceRebuff.IsPending())
                return member->GetName() + " buffing";

            return std::string();
        };

        if (!IsGroupPresent(bot))
            return "the group is not all here";

        std::string missing = gap(bot);
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref && missing.empty(); ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member != bot && IsPresent(bot, member))
                    missing = gap(member);
            }
        }

        // [lead164] b178 rfc-h 5D (20261101-171156) 303-413 s: "held for now: missing Mark of the Wild on Rtdekab"
        // 110 s before Oggleflint - this gate counts a buff as missing whenever a member of its class knows the spell,
        // while the leader's buff pass (LeaderBuffing, the casters' facts: in range, castable, reagents) had ended
        // with it missing; nothing could end the wait. In a led group the buffs are the leader's buff pass alone
        if (missing.empty() && askBuffs && !Leader::LedBot(bot))
        {
            std::string const buff = MissingGroupBuff(bot);
            if (!buff.empty())
                missing = "missing " + buff;
        }

        return missing;
    }

    uint32 CountCasters(std::vector<Unit*> const& pack)
    {
        uint32 casters = 0;
        for (Unit* mob : pack)
        {
            if (MobRoles::IsCaster(mob))
                ++casters;
        }

        return casters;
    }

    // The pack's mobs that stand off and cast or shoot (a caster, a SmartAI ranged attack: KeepsRange): what a pull
    // round a corner brings into melee. A pack without them gains nothing from it.
    uint32 CountRanged(std::vector<Unit*> const& pack)
    {
        uint32 ranged = 0;
        for (Unit* mob : pack)
        {
            Creature* creature = mob->ToCreature();
            if (MobRoles::IsCaster(mob) || (creature && KeepsRange(creature)))
                ++ranged;
        }

        return ranged;
    }

    // The ground at (x, y) on the floor of `floorZ`, within `maxDiff` yards of it.
    bool GroundNear(Player* bot, float x, float y, float floorZ, float maxDiff, float& z)
    {
        float const ground = bot->GetMapHeight(x, y, floorZ + maxDiff, true, maxDiff * 2.0f + 1.0f);
        if (ground <= INVALID_HEIGHT || std::fabs(ground - floorZ) > maxDiff)
            return false;

        z = ground;
        return true;
    }

    // No mob of the pack sees the spot (at eye height of whoever stands there).
    bool HiddenFromPack(std::vector<Unit*> const& pack, float x, float y, float z)
    {
        for (Unit* mob : pack)
        {
            if (mob->IsWithinLOS(x, y, z))
                return false;
        }

        return true;
    }

    // No mob of the pack notices someone standing at the spot.
    bool OutOfPackAggro(Player* bot, std::vector<Unit*> const& pack, float x, float y, float z)
    {
        for (Unit* mob : pack)
        {
            if (InCoreAggroReach(mob, bot, x, y, z, CAREFUL_AGGRO_MARGIN))
                return false;
        }

        return true;
    }

    bool ReachedByOthers(Player* bot, std::vector<Unit*> const& others, Position const& spot)
    {
        for (Unit* mob : others)
        {
            if (ReachesSpot(bot, mob, spot))
                return true;
        }

        return false;
    }

    // A walkable path of at most `maxLength` yards; on a map without navmesh there is no telling, so no.
    bool ShortWalk(Player* bot, Position const& from, Position const& to, float maxLength, uint32& budget)
    {
        if (!budget)
            return false;

        --budget;
        LeaderPath path(bot);  // [lead99]
        path.CalculatePath(from.GetPositionX(), from.GetPositionY(), from.GetPositionZ(), to.GetPositionX(),
                           to.GetPositionY(), to.GetPositionZ(), false);
        // [b34_ss] The navmesh has no doors: a walk through a closed one is no walk. scholo-n 20261026-014521-5E-4
        // pull 25: the los pull spot / gather (203.6,121.9) lay behind the closed Iron Gate 175615 - "hidden from the
        // pack" by the gate's own bars - the healer ended up behind it (door_cut 1942 s), the tank died alone at
        // 1978 s, and the healer's walk back to it after the corpse run went through the bone pits (3 deaths).
        return path.GetPathType() == PATHFIND_NORMAL && path.getPathLength() <= maxLength &&
               !PathThroughClosedDoor(bot, path.GetPath()).Found();
    }

    // Where the group waits: 3-6 yards behind the tank's spot, away from the pack, hidden from it too, and in sight
    // of the tank so the healer reaches it.
    bool FindLosPullGatherSpot(Player* bot, std::vector<Unit*> const& pack, std::vector<Unit*> const& others,
                               Position const& spot, float centerX, float centerY, uint32& budget, Position& gather)
    {
        static float const distances[] = {4.5f, 3.0f, 6.0f};
        static float const offsets[] = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f};  // radians off straight away from the pack
        Map* map = bot->GetMap();
        if (!map)
            return false;

        float const away = std::atan2(spot.GetPositionY() - centerY, spot.GetPositionX() - centerX);
        for (float const distance : distances)
        {
            for (float const offset : offsets)
            {
                float const x = spot.GetPositionX() + distance * std::cos(away + offset);
                float const y = spot.GetPositionY() + distance * std::sin(away + offset);
                float z = 0.0f;
                if (!GroundNear(bot, x, y, spot.GetPositionZ(), LOS_PULL_GATHER_FLOOR, z) ||
                    !HiddenFromPack(pack, x, y, z))
                    continue;

                if (!map->isInLineOfSight(spot.GetPositionX(), spot.GetPositionY(),
                                          spot.GetPositionZ() + LOS_PULL_EYE_HEIGHT, x, y, z + LOS_PULL_EYE_HEIGHT,
                                          bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                          VMAP::ModelIgnoreFlags::Nothing))
                    continue;

                Position const candidate(x, y, z);
                if (ReachedByOthers(bot, others, candidate))
                    continue;

                if (!ShortWalk(bot, spot, candidate, distance * 1.5f + 1.0f, budget))
                {
                    if (!budget)
                        return false;

                    continue;
                }

                gather = candidate;
                return true;
            }
        }

        return false;
    }

    // The tank's spot for a line-of-sight pull: near the cast spot, on its floor, out of the pack's aggro range and
    // sight, reached by no other idle mob, a short walk from the cast spot, and with a group spot behind it.
    bool FindLosPullSpot(Player* bot, std::vector<Unit*> const& pack, std::vector<Unit*> const& others,
                         Position const& cast, Position& spot, Position& gather)
    {
        float centerX = 0.0f;
        float centerY = 0.0f;
        for (Unit* mob : pack)
        {
            centerX += mob->GetPositionX();
            centerY += mob->GetPositionY();
        }

        centerX /= float(pack.size());
        centerY /= float(pack.size());

        // Angles step out both ways from straight away from the pack: behind the cast spot first.
        float const away = std::atan2(cast.GetPositionY() - centerY, cast.GetPositionX() - centerX);
        float const angleStep = 2.0f * float(M_PI) / float(LOS_PULL_ANGLES);
        uint32 budget = LOS_PULL_MAX_PATHS;
        for (float radius = LOS_PULL_RING_MIN; radius <= LOS_PULL_RING_MAX + 0.1f; radius += LOS_PULL_RING_STEP)
        {
            for (uint32 i = 0; i < LOS_PULL_ANGLES; ++i)
            {
                int32 const step = int32((i + 1) / 2) * ((i % 2) ? 1 : -1);
                float const angle = away + float(step) * angleStep;
                float const x = cast.GetPositionX() + radius * std::cos(angle);
                float const y = cast.GetPositionY() + radius * std::sin(angle);
                float z = 0.0f;
                if (!GroundNear(bot, x, y, cast.GetPositionZ(), LOS_PULL_FLOOR, z) ||
                    !OutOfPackAggro(bot, pack, x, y, z) || !HiddenFromPack(pack, x, y, z))
                    continue;

                Position const candidate(x, y, z);
                if (ReachedByOthers(bot, others, candidate))
                    continue;

                if (!ShortWalk(bot, cast, candidate, LOS_PULL_MAX_PATH, budget))
                {
                    if (!budget)
                        return false;

                    continue;
                }

                if (FindLosPullGatherSpot(bot, pack, others, candidate, centerX, centerY, budget, gather))
                {
                    spot = candidate;
                    return true;
                }

                if (!budget)
                    return false;
            }
        }

        return false;
    }

    // [b40_mgt] The corner the group already pulled round. mgt-n 20261026-060218-mgt-n-5D-13, the Sunblade hall before
    // Delrissa: five formations of 4-5 (groupAI 3, + the warlocks' imps), 20-28 yd apart in an open hall, no SmartAI
    // call for help; counted where the casters stand ("reaches #5, which keeps its range", one 10 yd assist) as one
    // 16-17-mob chain. Pull 20 peeled the near pack round the hall's way in (tank spot 14 yd from its cast spot);
    // for the far packs that corner is 35-50 yd from the cast spot, past LOS_PULL_RING_MAX / LOS_PULL_MAX_PATH, the
    // open hall hides nothing nearer: "no hidden spot near the cast spot" for every candidate, "no split in 26
    // searches", the run stalled at 2/4. Real groups pull every pack of such a hall back to the same corner. Each
    // los pull started notes its tank / group spots (NoteLosCorner); the split round a corner tries them, and a
    // small ring round each, when the near ring finds nothing, under the same tests: out of the pack's aggro and
    // sight, reached by no other idle mob, farther from the pack than the cast spot, a walk of at most
    // LOS_CORNER_MAX_PATH from it, a group spot behind it. Nothing leashes in a dungeon (Creature::CanCreatureAttack):
    // the longer run back loses nobody.
    constexpr size_t LOS_CORNER_MEMO_SIZE = 4;
    constexpr uint32 LOS_CORNER_TTL_MS = 30 * MINUTE * IN_MILLISECONDS;
    constexpr float LOS_CORNER_MAX_DIST = 55.0f;  // straight from the cast spot
    constexpr float LOS_CORNER_MAX_PATH = 65.0f;  // walked from the cast spot
    constexpr float LOS_CORNER_FLOOR = 8.0f;      // a corner up or down a stair, not another floor
    constexpr float LOS_CORNER_SAME = 3.0f;       // a spot this close to a noted one is that one
    constexpr float LOS_CORNER_RING = 5.0f;
    constexpr uint32 LOS_CORNER_RING_ANGLES = 8;
    constexpr uint32 LOS_CORNER_MAX_PATHS = 8;
    struct LosCorner
    {
        Position spot;
        Position gather;
        uint32 mapId = 0;
        uint32 instanceId = 0;
        uint32 ms = 0;
    };
    std::mutex losCornerLock;
    std::unordered_map<uint64, std::vector<LosCorner>> losCorners;  // by tank, the newest last

    void NoteLosCorner(Player* tank, Position const& spot, Position const& gather)
    {
        if (!tank)
            return;

        std::lock_guard<std::mutex> guard(losCornerLock);
        std::vector<LosCorner>& corners = losCorners[tank->GetGUID().GetRawValue()];
        uint32 const mapId = tank->GetMapId();
        uint32 const instanceId = tank->GetInstanceId();
        corners.erase(std::remove_if(corners.begin(), corners.end(),
                                     [&](LosCorner const& corner)
                                     {
                                         return corner.mapId != mapId || corner.instanceId != instanceId ||
                                                corner.spot.GetExactDist(spot) <= LOS_CORNER_SAME;
                                     }),
                      corners.end());
        if (corners.size() >= LOS_CORNER_MEMO_SIZE)
            corners.erase(corners.begin());

        LosCorner corner;
        corner.spot = spot;
        corner.gather = gather;
        corner.mapId = mapId;
        corner.instanceId = instanceId;
        corner.ms = getMSTime();
        corners.push_back(corner);
    }

    bool FindLosPullCornerSpot(Player* bot, std::vector<Unit*> const& pack, std::vector<Unit*> const& others,
                               Position const& cast, Position& spot, Position& gather)
    {
        Map* map = bot ? bot->GetMap() : nullptr;
        if (!map || pack.empty())
            return false;

        std::vector<LosCorner> corners;
        {
            std::lock_guard<std::mutex> guard(losCornerLock);
            auto const itr = losCorners.find(bot->GetGUID().GetRawValue());
            if (itr == losCorners.end())
                return false;

            uint32 const now = getMSTime();
            for (LosCorner const& corner : itr->second)
            {
                if (corner.mapId == bot->GetMapId() && corner.instanceId == bot->GetInstanceId() &&
                    getMSTimeDiff(corner.ms, now) <= LOS_CORNER_TTL_MS)
                    corners.push_back(corner);
            }
        }

        if (corners.empty())
            return false;

        float centerX = 0.0f;
        float centerY = 0.0f;
        for (Unit* mob : pack)
        {
            centerX += mob->GetPositionX();
            centerY += mob->GetPositionY();
        }

        centerX /= float(pack.size());
        centerY /= float(pack.size());
        float const castFromPack = cast.GetExactDist2d(centerX, centerY);
        uint32 budget = LOS_CORNER_MAX_PATHS;
        for (auto corner = corners.rbegin(); corner != corners.rend(); ++corner)  // the newest first
        {
            for (uint32 i = 0; i <= LOS_CORNER_RING_ANGLES; ++i)
            {
                // the noted spot itself, then a ring round it
                float x = corner->spot.GetPositionX();
                float y = corner->spot.GetPositionY();
                float z = corner->spot.GetPositionZ();
                if (i)
                {
                    float const angle = 2.0f * float(M_PI) * float(i - 1) / float(LOS_CORNER_RING_ANGLES);
                    x += LOS_CORNER_RING * std::cos(angle);
                    y += LOS_CORNER_RING * std::sin(angle);
                    if (!GroundNear(bot, x, y, corner->spot.GetPositionZ(), LOS_PULL_FLOOR, z))
                        continue;
                }

                Position const candidate(x, y, z);
                if (std::fabs(z - cast.GetPositionZ()) > LOS_CORNER_FLOOR ||
                    cast.GetExactDist2d(x, y) > LOS_CORNER_MAX_DIST ||
                    candidate.GetExactDist2d(centerX, centerY) <= castFromPack ||
                    !OutOfPackAggro(bot, pack, x, y, z) || !HiddenFromPack(pack, x, y, z) ||
                    ReachedByOthers(bot, others, candidate))
                    continue;

                if (!ShortWalk(bot, cast, candidate, LOS_CORNER_MAX_PATH, budget))
                {
                    if (!budget)
                        return false;

                    continue;
                }

                // the group spot noted with it, when it still is one; else one behind the candidate
                Position const& noted = corner->gather;
                bool const notedFits =
                    !i && noted.GetExactDist(candidate) <= 8.0f &&
                    HiddenFromPack(pack, noted.GetPositionX(), noted.GetPositionY(), noted.GetPositionZ()) &&
                    !ReachedByOthers(bot, others, noted) &&
                    map->isInLineOfSight(x, y, z + LOS_PULL_EYE_HEIGHT, noted.GetPositionX(), noted.GetPositionY(),
                                         noted.GetPositionZ() + LOS_PULL_EYE_HEIGHT, bot->GetPhaseMask(),
                                         LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
                if (notedFits)
                    gather = noted;
                else if (!FindLosPullGatherSpot(bot, pack, others, candidate, centerX, centerY, budget, gather))
                {
                    if (!budget)
                        return false;

                    continue;
                }

                spot = candidate;
                LOG_DEBUG("playerbots", "[b40_mgt] los_corner: {}: the corner pulled round before ({:.1f} {:.1f} "
                          "{:.1f}) for {} mobs, tank spot ({:.1f} {:.1f} {:.1f}) {:.0f} yd from the cast spot",
                          bot->GetName(), corner->spot.GetPositionX(), corner->spot.GetPositionY(),
                          corner->spot.GetPositionZ(), pack.size(), x, y, z, cast.GetExactDist(candidate));
                return true;
            }
        }

        return false;
    }

    // [b47_plan] The way in as the corner: players clear a caster hall by pulling each group back to the doorway or the
    // corridor they came through, out of the casters' sight, and fight there. lbrs-n 20261026-093620-lbrs-n-5G-11 /
    // brs-h 093620-5H-10, the Firebrand hall by Voone (Invokers / Darkweavers cast Fireball / Shadow Bolt where they
    // stand, SmartAI cast flag 64): every split search ended "round a corner 4 tried, fewest 0" - "no hidden spot near
    // the cast spot" (the ring of FindLosPullSpot lies in the open hall) and no corner noted yet (a first los pull of
    // the run) - so "no split in 19 searches", the run stalled 4 min at (-63,-405.7). The tank's own trail (the way
    // it walked in, cleared) is walked back from the newest point: the first point that is hidden from the pack, out
    // of its aggro, reached by no other idle mob, farther from the pack than the cast spot, on its floor, a walk of at
    // most LOS_CORNER_MAX_PATH from the cast spot, with a group spot behind it.
    constexpr float LOS_TRAIL_MAX_BACK = 70.0f;  // along the trail from the tank
    constexpr float LOS_TRAIL_SAMPLE = 4.0f;

    // [b51_strat] User 2026-10-26 (strat-h 130307-5H-2 street wipe): pull back to a part of the street the group has
    // ALREADY cleared, so a mob that flees or calls for help there finds nobody. A spot is clear when every idle mob
    // outside the pack is farther from it than its aggro reach (wander radius included) plus `margin` (the core's
    // CreatureFamilyAssistanceRadius: an engage / periodic call from a pack mob fighting there reaches nobody).
    // Line of sight is not asked: a call through a corner is the cheaper mistake to avoid.
    bool ClearOfIdle(Player* bot, std::vector<Unit*> const& others, Position const& at, float margin)
    {
        for (Unit* mob : others)
        {
            Creature* creature = mob ? mob->ToCreature() : nullptr;
            if (!creature || !creature->HasReactState(REACT_AGGRESSIVE))
                continue;

            float reach = MovementAction::IdleAggroReach(creature, bot, CAREFUL_AGGRO_MARGIN) + margin;
            if (creature->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
                reach += creature->GetWanderDistance();

            if (mob->GetExactDist(at) <= reach)
                return false;
        }

        return true;
    }

    // `clearMargin` above 0 ([b51_strat]): only a trail point (and its group spot) ClearOfIdle by that margin.
    bool FindLosPullTrailSpot(Player* bot, std::vector<Unit*> const& pack, std::vector<Unit*> const& others,
                              Position const& cast, Position& spot, Position& gather, float clearMargin = 0.0f)
    {
        if (!bot || pack.empty())
            return false;

        std::vector<G3D::Vector3> const trail = MovementAction::TankTrailPoints(bot);
        if (trail.size() < 2)
            return false;

        float centerX = 0.0f;
        float centerY = 0.0f;
        for (Unit* mob : pack)
        {
            centerX += mob->GetPositionX();
            centerY += mob->GetPositionY();
        }

        centerX /= float(pack.size());
        centerY /= float(pack.size());
        float const castFromPack = cast.GetExactDist2d(centerX, centerY);
        uint32 budget = LOS_CORNER_MAX_PATHS;
        float back = 0.0f;
        float tried = -LOS_TRAIL_SAMPLE;
        G3D::Vector3 previous(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        for (auto point = trail.rbegin(); point != trail.rend(); ++point)
        {
            back += (*point - previous).length();
            previous = *point;
            if (back > LOS_TRAIL_MAX_BACK)
                break;

            if (back < tried + LOS_TRAIL_SAMPLE)  // one look every LOS_TRAIL_SAMPLE yd of the way back
                continue;

            tried = back;
            float const x = point->x;
            float const y = point->y;
            float z = point->z;
            if (!GroundNear(bot, x, y, point->z, LOS_PULL_FLOOR, z))
                continue;

            Position const candidate(x, y, z);
            if (std::fabs(z - cast.GetPositionZ()) > LOS_CORNER_FLOOR ||
                candidate.GetExactDist2d(centerX, centerY) <= castFromPack || !OutOfPackAggro(bot, pack, x, y, z) ||
                !HiddenFromPack(pack, x, y, z) || ReachedByOthers(bot, others, candidate) ||
                (clearMargin > 0.0f && !ClearOfIdle(bot, others, candidate, clearMargin)))  // [b51_strat]
                continue;

            if (!ShortWalk(bot, cast, candidate, LOS_CORNER_MAX_PATH, budget))
            {
                if (!budget)
                    return false;

                continue;
            }

            if (!FindLosPullGatherSpot(bot, pack, others, candidate, centerX, centerY, budget, gather))
            {
                if (!budget)
                    return false;

                continue;
            }

            if (clearMargin > 0.0f && !ClearOfIdle(bot, others, gather, clearMargin))  // [b51_strat]
                continue;

            spot = candidate;
            LOG_DEBUG("playerbots", "[b47_plan] los_trail: {}: the way in hides the pack ({} mobs): tank spot ({:.1f} "
                      "{:.1f} {:.1f}), {:.0f} yd back along its trail, {:.0f} yd from the cast spot{}", bot->GetName(),
                      pack.size(), x, y, z, back, cast.GetExactDist(candidate),
                      clearMargin > 0.0f ? " ([b51_strat] cleared: no idle mob within its reach + assist radius)" : "");
            return true;
        }

        return false;
    }

    // User, Ragefire Chasm 2026-10-24: «он сагрил одного моба и просто стоит на месте ... некоторые мобы где-то ходят,
    // пока не в радиусе, но зайдут и заагрятся на танка ... танк, заагрив моба, иногда просто отводит его назад, где он
    // раньше был, чтобы те, которые подходят, не заагрились». The fight's spot is judged against the mobs that walk:
    // one that can come within its aggro reach of it during the fight moves the fight back, along the way the group
    // came (cleared, in the healer's sight), FIGHT_SPOT_STEP at a time, to where none can.
    constexpr float FIGHT_SPOT_SCAN = 80.0f;  // a mover farther than this from the spot is not looked at
    constexpr float FIGHT_SPOT_STEP = 10.0f;  // the fight moves back in steps of this
    constexpr uint32 FIGHT_SPOT_STEPS = 3;
    constexpr float FIGHT_SPOT_COMBAT_SEC = 20.0f;                    // what is left of a fight already on, about
    constexpr uint32 FIGHT_SPOT_REVIEW_MS = 8 * IN_MILLISECONDS;      // a fight on is looked at in its first seconds
    constexpr uint32 FIGHT_SPOT_LOOK_MS = 1000;                       // one look a second at most
    constexpr uint32 FIGHT_SPOT_WALK_MS = 12 * IN_MILLISECONDS;       // the walk back is given up after this
    constexpr uint32 FIGHT_SPOT_PLAN_MS = 20 * IN_MILLISECONDS;       // a plan's spot: for a fight on by then
    constexpr uint32 FIGHT_SPOT_COMBAT_GAP_MS = 5 * IN_MILLISECONDS;  // not seen fighting this long: a new fight
    constexpr float FIGHT_SPOT_ARRIVED = 2.0f;

    // How long a fight with this pack lasts, its pull included (the patrol forecast's window).
    float FightWindowSec(PackEstimate const& estimate)
    {
        return PATROL_PULL_LEAD_SEC +
               (estimate.boss ? PATROL_BOSS_FIGHT_SEC
                              : std::clamp(estimate.fightSec, PATROL_MIN_FIGHT_SEC, PATROL_MAX_FIGHT_SEC)) *
                   PATROL_FIGHT_MARGIN;
    }

    // A mover that can walk into a fight: the mob, how it moves, and how far from the spot it is now.
    // [lead26] the pull-together overhead of a separate pull (the walk, the tag, the group's settle) and the
    // ranged placement range of the fight-spot check
    constexpr float PULL_TOGETHER_OVERHEAD_SEC = 12.0f;
    constexpr float PLACEMENT_RANGED_YD = 25.0f;

    struct FightIntruder
    {
        Creature* mob = nullptr;
        MobMotion motion = MobMotion::Static;
        float distance = 0.0f;
    };

    std::string FightIntruderNote(FightIntruder const& intruder)
    {
        return Acore::StringFormat("{} {} ({:.0f} yd) can reach the fight spot", MobMotionName(intruder.motion),
                                   intruder.mob->GetName(), intruder.distance);
    }

    // `mob` comes with a pull of `target` wherever it is fought: its formation, or a spawn point within the social
    // radius of the target's (each one's wander radius on top).
    bool SpawnsWith(Creature* mob, Unit* target)
    {
        Creature* other = target ? target->ToCreature() : nullptr;
        if (!other)
            return false;

        if (mob == other)
            return true;

        CreatureGroup const* formation = mob->GetFormation();
        if (formation && formation == other->GetFormation())
            return true;

        float wander = 0.0f;
        float otherWander = 0.0f;
        Position const spawn = SpawnPoint(mob, &wander);
        Position const otherSpawn = SpawnPoint(other, &otherWander);
        return std::fabs(spawn.GetPositionZ() - otherSpawn.GetPositionZ()) <= PULL_PATH_HEIGHT &&
               spawn.GetExactDist2d(otherSpawn) <= PACK_SOCIAL_RADIUS + wander + otherWander;
    }

    // An idle aggressive mob that walks (MotionOf) and can come within its aggro reach (+ CAREFUL_AGGRO_MARGIN) of
    // `spot` over the next `seconds`, and see it: a patrol by its forecast walk (PatrolTrack; the timeless path test
    // when it cannot be forecast), a wanderer anywhere on the disc of its wander radius round its spawn point, any
    // other mob seen walking by where its trend takes it. What comes with `target` anyway (SpawnsWith), a boss and a
    // mob held by crowd control are left out; a standing mob is the spot test's (ReachesSpot). The nearest, in `out`.
    bool WalksIntoFight(PlayerbotAI* botAI, Player* bot, GuidVector const& possible, Unit* target, Position const& spot,
                        float seconds, FightIntruder& out)
    {
        out = FightIntruder();
        Map* map = bot->GetMap();
        if (!map)
            return false;

        size_t const samples = size_t(std::max(seconds, 0.0f) / PATROL_SAMPLE_SEC) + 1;
        for (ObjectGuid const& guid : possible)
        {
            Creature* mob = botAI->GetCreature(guid);
            if (!mob || !IsIdleEnemy(bot, mob) || !mob->HasReactState(REACT_AGGRESSIVE) || IsCrowdControlled(mob) ||
                IsBossMob(mob))
                continue;

            float const distance = mob->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY());
            if (distance > FIGHT_SPOT_SCAN || (out.mob && distance >= out.distance) || SpawnsWith(mob, target))
                continue;

            MobMotion const motion = MotionOf(mob);
            if (motion == MobMotion::Static)
                continue;

            float const reach = MovementAction::IdleAggroReach(mob, bot, CAREFUL_AGGRO_MARGIN);
            bool const sameFloor = std::fabs(mob->GetPositionZ() - spot.GetPositionZ()) <= PULL_PATH_HEIGHT;
            bool reaches = false;
            if (motion == MobMotion::Patrol)
            {
                Creature* walker = nullptr;
                WaypointPath const* path = PatrolPath(mob, &walker);
                if (std::vector<G3D::Vector3> const* track = PatrolTrack(walker, path))
                {
                    float const follow = std::min(mob->GetExactDist2d(walker), PATROL_MAX_FOLLOW);
                    std::vector<bool> const flags =
                        PatrolInReach(map, bot->GetPhaseMask(), *track, reach + follow, {spot}, samples);
                    reaches = FirstInReach(flags, flags.size()) >= 0;
                }
                else
                    reaches = sameFloor && distance <= CAREFUL_PATROL_RANGE && PathCrossesSpot(mob, spot, reach);
            }
            else if (motion == MobMotion::Wanderer)
            {
                float wander = 0.0f;
                Position const home = SpawnPoint(mob, &wander);
                reaches = std::fabs(home.GetPositionZ() - spot.GetPositionZ()) <= PULL_PATH_HEIGHT &&
                          home.GetExactDist2d(spot) <= reach + wander &&
                          map->isInLineOfSight(home.GetPositionX(), home.GetPositionY(),
                                               home.GetPositionZ() + CAREFUL_EYE_HEIGHT, spot.GetPositionX(),
                                               spot.GetPositionY(), spot.GetPositionZ() + CAREFUL_EYE_HEIGHT,
                                               bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                               VMAP::ModelIgnoreFlags::Nothing);
            }
            else
            {
                float trendSec = 0.0f;
                float const change = DistanceTrend(mob, spot, trendSec);
                float projected = distance;
                if (trendSec >= OVERLAP_TREND_MIN_SEC && change < -OVERLAP_TREND_MIN)
                    projected = distance + change / trendSec * seconds;

                reaches = sameFloor && projected <= reach &&
                          mob->IsWithinLOS(spot.GetPositionX(), spot.GetPositionY(),
                                           spot.GetPositionZ() + CAREFUL_EYE_HEIGHT);
            }

            if (reaches)
                out = {mob, motion, distance};
        }

        return out.mob != nullptr;
    }

    // A fight spot back from `from` along (`dirX`, `dirY`) (2d, unit), FIGHT_SPOT_STEP apart from step `firstStep`
    // (0: `from` itself) to FIGHT_SPOT_STEPS, on its floor, that the healer sees and no mover walks into over
    // `seconds`, and that the tank's walk from `walkFrom` reaches by its navmesh path waking nobody outside `pack`.
    bool FindFightSpotBack(PlayerbotAI* botAI, Player* bot, GuidVector const& possible, Unit* target,
                           Position const& from, float dirX, float dirY, Position const& walkFrom,
                           std::vector<Unit*> const& pack, float seconds, uint32 firstStep, Position& out)
    {
        for (uint32 i = firstStep; i <= FIGHT_SPOT_STEPS; ++i)
        {
            float const back = FIGHT_SPOT_STEP * float(i);
            float const x = from.GetPositionX() + dirX * back;
            float const y = from.GetPositionY() + dirY * back;
            float z = from.GetPositionZ();
            if (i && !GroundNear(bot, x, y, from.GetPositionZ(), PULL_PATH_HEIGHT, z))
                break;  // the way back ends here

            Position const option(x, y, z);
            // no fight spot by lava, slime or a drop-off (wave 44: the whole group fought on the lava edge)
            if (bot->GetMap() &&
                NearHazard(bot->GetMap(), bot->GetPhaseMask(), x, y, z, TERRAIN_HAZARD_FIGHT_RADIUS))
                continue;

            FightIntruder intruder;
            if (!HealerSees(bot, option) || WalksIntoFight(botAI, bot, possible, target, option, seconds, intruder))
                continue;

            float const straight = walkFrom.GetExactDist(option);
            if (straight > CAREFUL_AGGRO_MARGIN)
            {
                bool walkable = false;
                if (WalkEntersAggro(botAI, bot, possible, walkFrom, option, pack, straight * 2.0f + FIGHT_SPOT_STEP,
                                    CAREFUL_AGGRO_MARGIN, walkable) ||
                    !walkable)
                    continue;
            }

            out = option;
            return true;
        }

        return false;
    }

    // [b44_gerst] The boss fight spot. User, brd-h 20261026-060218-brd-h-5F-5 (High Interrogator Gerstahn): a boss
    // that summons adds or spams something, or has packs near him, is dragged to the middle of cleared ground, not
    // fought at the edge of the uncleared room. The tank held its pull at the brink of his aggro (324.6,-131.3), 21 yd
    // from him and 13-19 yd from the idle Warden / Bloodhounds / Torturer of his room, and the fight (boss + 4) was
    // fought there. A spot qualifies when the fight has room round it (BOSS_SPOT_ROOM: open floor in most directions,
    // no hazard or drop-off, no doorway), no idle mob outside the fight reaches it (ReachesSpot: aggro, wander,
    // patrol) or stands within its aggro reach + BOSS_SPOT_IDLE_MARGIN (adds and the group spread round the tank),
    // the boss's spawn is at most BOSS_SPOT_MAX_FROM_HOME away (leash, script areas), and the tank walks there waking
    // nobody (WalkEntersAggro). Candidates: the asked spot, the tank, then the tank's own trail walking back - ground
    // the group came by, cleared - every BOSS_SPOT_TRAIL_STEP up to BOSS_SPOT_MAX_BACK; the healer's sight first, then
    // without (the healer follows the fight). The planner calls it once per plan, only for a boss fight.
    constexpr float BOSS_SPOT_ROOM = 10.0f;
    constexpr float BOSS_SPOT_IDLE_MARGIN = 8.0f;
    constexpr float BOSS_SPOT_MAX_BACK = 40.0f;
    constexpr float BOSS_SPOT_TRAIL_STEP = 4.0f;
    constexpr size_t BOSS_SPOT_MAX_CANDIDATES = 12;
    constexpr float BOSS_SPOT_MAX_FROM_HOME = 50.0f;
    constexpr float BOSS_SPOT_DOOR_RADIUS = 8.0f;
    constexpr float BOSS_SPOT_FLOOR_DZ = 3.0f;
    constexpr float BOSS_SPOT_TRAIL_DZ = 8.0f;  // a trail point this far above / below the tank is another floor
    constexpr uint32 BOSS_SPOT_ROOM_DIRECTIONS = 8;
    constexpr uint32 BOSS_SPOT_ROOM_MIN_OPEN = 5;

    // Why `spot` is no fight spot for the boss fight `fight` (empty: it is one). `blocker`: the idle mob, if one.
    std::string BossSpotRejection(PlayerbotAI* botAI, Player* bot, GuidVector const& possible, Unit* boss,
                                  std::vector<Unit*> const& fight, Position const& spot, bool healerSight)
    {
        Map* map = bot->GetMap();
        if (!map)
            return "no map";

        float const x = spot.GetPositionX();
        float const y = spot.GetPositionY();
        float const z = spot.GetPositionZ();
        if (Creature* creature = boss->ToCreature())
            if (creature->GetHomePosition().GetExactDist(spot) > BOSS_SPOT_MAX_FROM_HOME)
                return Acore::StringFormat("{:.0f} yd from his spawn", creature->GetHomePosition().GetExactDist(spot));

        if (NearHazard(map, bot->GetPhaseMask(), x, y, z, BOSS_SPOT_ROOM))
            return "a hazard or drop-off within the fight's room";

        if (DoorNear(bot, x, y, z, BOSS_SPOT_DOOR_RADIUS))
            return "a doorway";

        uint32 open = 0;
        for (uint32 i = 0; i < BOSS_SPOT_ROOM_DIRECTIONS; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * i / BOSS_SPOT_ROOM_DIRECTIONS;
            float const px = x + std::cos(angle) * BOSS_SPOT_ROOM;
            float const py = y + std::sin(angle) * BOSS_SPOT_ROOM;
            float pz = z;
            if (GroundNear(bot, px, py, z, BOSS_SPOT_FLOOR_DZ, pz) &&
                map->isInLineOfSight(x, y, z + CAREFUL_EYE_HEIGHT, px, py, pz + CAREFUL_EYE_HEIGHT,
                                     bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                ++open;
        }

        if (open < BOSS_SPOT_ROOM_MIN_OPEN)
            return Acore::StringFormat("no room for the fight ({} of {} sides open)", open, BOSS_SPOT_ROOM_DIRECTIONS);

        for (ObjectGuid const& guid : possible)
        {
            Unit* unit = botAI->GetUnit(guid);
            Creature* mob = unit ? unit->ToCreature() : nullptr;
            if (!mob || mob == boss || !IsIdleEnemy(bot, mob) || !mob->HasReactState(REACT_AGGRESSIVE) ||
                IsCrowdControlled(mob) || std::find(fight.begin(), fight.end(), unit) != fight.end())
                continue;

            if (ReachesSpot(bot, mob, spot))
                return Acore::StringFormat("idle {} reaches it ({:.1f} yd)", mob->GetName(), mob->GetExactDist(spot));

            // no line of sight asked: bars and doors pass aggro, and the fight spreads round the spot
            if (MovementAction::PassesAggroZGate(mob, bot, z) &&
                mob->GetExactDist(spot) < MovementAction::IdleAggroReach(mob, bot, BOSS_SPOT_IDLE_MARGIN))
                return Acore::StringFormat("idle {} {:.1f} yd off, within its reach + {:.0f} yd", mob->GetName(),
                                           mob->GetExactDist(spot), BOSS_SPOT_IDLE_MARGIN);
        }

        if (healerSight && !HealerSees(bot, spot))
            return "out of the healer's sight";

        return "";
    }

    // The boss fight's spot (see BossSpotRejection): `asked` itself when it qualifies, else the first qualifying
    // candidate in `out` (true); false when none does (`note`: why the asked spot does not).
    bool FindBossFightSpot(PlayerbotAI* botAI, Player* bot, Unit* boss, std::vector<Unit*> const& fight,
                           Position const& asked, Position& out, std::string& note)
    {
        GuidVector const possible =
            botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();
        std::vector<Position> candidates = {asked};
        if (asked.GetExactDist(bot) > FIGHT_SPOT_ARRIVED)
            candidates.push_back(bot->GetPosition());

        // the trail is oldest first: walk it back from its newest point
        std::vector<G3D::Vector3> const trail = MovementAction::TankTrailPoints(bot);
        G3D::Vector3 last(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        float walked = 0.0f;
        float nextAt = BOSS_SPOT_TRAIL_STEP;
        for (auto itr = trail.rbegin(); itr != trail.rend() && candidates.size() < BOSS_SPOT_MAX_CANDIDATES; ++itr)
        {
            walked += (*itr - last).length();
            last = *itr;
            if (walked > BOSS_SPOT_MAX_BACK)
                break;

            if (walked < nextAt || std::fabs(itr->z - bot->GetPositionZ()) > BOSS_SPOT_TRAIL_DZ)
                continue;

            nextAt = walked + BOSS_SPOT_TRAIL_STEP;
            candidates.emplace_back(itr->x, itr->y, itr->z);
        }

        for (bool const healerSight : {true, false})
        {
            for (size_t i = 0; i < candidates.size(); ++i)
            {
                Position const& option = candidates[i];
                std::string const why = BossSpotRejection(botAI, bot, possible, boss, fight, option, healerSight);
                if (i == 0 && healerSight)
                    note = why;

                if (!why.empty())
                    continue;

                float const straight = bot->GetExactDist(option);
                if (straight > CAREFUL_AGGRO_MARGIN)
                {
                    bool walkable = false;
                    if (WalkEntersAggro(botAI, bot, possible, bot->GetPosition(), option, fight,
                                        straight * 2.0f + BOSS_SPOT_TRAIL_STEP, CAREFUL_AGGRO_MARGIN, walkable) ||
                        !walkable)
                        continue;
                }

                out = option;
                return true;
            }
        }

        return false;
    }

    // The fight spot of each tank (FightSpot in ChooseTargetActions.h), behind one lock: written by the tank's own
    // plan, pull and fight, on its map thread.
    struct FightSpotEntry
    {
        Position spot;
        std::string reason;
        float back = 0.0f;
        uint32 planMs = 0;    // a body pull's spot from the plan, for the fight that starts next
        uint32 chosenMs = 0;  // the next fight's spot was checked already (a pull back's kill spot)
        uint32 combatMs = 0;  // since when this fight is on
        uint32 seenMs = 0;    // its last look (0: the next look starts a new fight)
        uint32 lookMs = 0;    // its last search
        uint32 walkMs = 0;    // the walk back started (0: none)
        bool done = false;    // this fight's spot is settled: no further walk
        uint32 cornerLookMs = 0;  // [drift_caster_corner] its last look for casters that keep their range
        bool cornerDone = false;  // this fight went round a corner once already
    };

    std::mutex fightSpotLock;
    std::unordered_map<uint64, FightSpotEntry> fightSpots;
}

bool IdleMobReachesSpot(Player* bot, Unit* mob, Position const& spot)
{
    return mob && IsIdleEnemy(bot, mob) && ReachesSpot(bot, mob, spot);
}

bool HealerSeesSpot(Player* bot, Position const& spot) { return HealerSees(bot, spot); }

std::string PullReadiness::BossNotReady(Player* bot, Unit* boss)
{
    return BossReadyGap(bot, true, IsEncounterBoss(boss), boss);
}

std::string PullReadiness::MemberBossGap(Player* member, Unit* boss)
{
    bool const realBoss = IsEncounterBoss(boss);
    float const readyHealth = realBoss ? BOSS_READY_HEALTH : MINI_BOSS_READY_HEALTH;
    // [lead51] the mana THIS boss fight takes from this member
    if (!member || !member->IsAlive())
        return std::string();
    std::string terms;
    float const readyMana = RestState::FightManaLine(member, NextPullSec(member, boss), 0.0f, &terms, true);
    if (member->GetHealthPct() < readyHealth)
        return Acore::StringFormat("health {:.0f} % < {:.0f} % for {}", member->GetHealthPct(), readyHealth,
                                   boss->GetName());
    if (member->getPowerType() == POWER_MANA && member->GetPowerPct(POWER_MANA) < readyMana)
        return Acore::StringFormat("mana {:.0f} % < {} for {}", member->GetPowerPct(POWER_MANA), terms,
                                   boss->GetName());
    return std::string();
}

float PullReadiness::MemberBossManaLine(Player* member, Unit* boss)
{
    return member && boss ? RestState::FightManaLine(member, NextPullSec(member, boss), 0.0f, nullptr, true) : 0.0f;
}

namespace PullPlanning  // [lead31a] was anonymous: shared with the tank's execution
{
    // [drift_caster_corner] scholo-n 20261025-150737-7 pull 12: Adepts and Necromancers cast from range and never
    // came to the tank; the hunter died 44-48 yd off. Casters that keep their range in a fight already on are broken
    // by line of sight: the tank steps round the nearest corner (FindLosPullSpot), the group gathers behind it
    // (LosPull: its spot and hold), the casters walk round into melee. Once per fight, through FightSpot::Due.
    namespace CasterCorner
    {
        constexpr float OFF_TANK = 12.0f;      // yd from the tank: one that stays out there casts from range
        constexpr uint32 FIGHT_MIN_MS = 4000;  // the fight is this old (the casters had time to come)
        constexpr float MAX_WALK = 25.0f;      // the corner is at most this far from the tank
    }

    // [lead206] the tank's ranged threat tools by class, best first (lead202's list)
    std::vector<char const*> TankRangedTools(Player* tank)
    {
        static char const* const PALADIN[] = {"avenger's shield", "hand of reckoning"};
        static char const* const WARRIOR[] = {"heroic throw", "taunt"};
        static char const* const DRUID[] = {"faerie fire (feral)", "growl"};
        static char const* const DEATH_KNIGHT[] = {"death grip", "dark command"};
        std::vector<char const*> tools;
        switch (tank->getClass())
        {
            case CLASS_PALADIN:
                tools.assign(std::begin(PALADIN), std::end(PALADIN));
                break;
            case CLASS_WARRIOR:
                tools.assign(std::begin(WARRIOR), std::end(WARRIOR));
                break;
            case CLASS_DRUID:
                tools.assign(std::begin(DRUID), std::end(DRUID));
                break;
            case CLASS_DEATH_KNIGHT:
                tools.assign(std::begin(DEATH_KNIGHT), std::end(DEATH_KNIGHT));
                break;
            default:
                break;
        }
        return tools;
    }

    // [lead202] The tank's tool that takes `mob` from where the tank stands, ready and castable on it now: Death Grip
    // brings it in; the ranged threat spells (Avenger's Shield, Heroic Throw, Faerie Fire (Feral), the taunts) hold
    // it at range. nullptr: none - the mob stays out of the tank's threat reach.
    char const* TankRangedTool(PlayerbotAI* tankAI, Player* tank, Unit* mob)
    {
        for (char const* tool : TankRangedTools(tank))
            if (tankAI->CanCastSpell(tool, mob))
                return tool;
        return nullptr;
    }

    // [lead206] [lead215] the tank's ranged tools as spells: TankRangedTools by name, and a warrior's ranged weapon
    // (Shoot with a bow / gun / crossbow, Throw with a thrown weapon: CastShootAction's spells) when one is equipped.
    // readySec: its cooldown left now; againSec: the wait for its next cast once used (its cooldown, the global
    // cooldown, its cast)
    struct RangedToolSpell
    {
        SpellInfo const* info = nullptr;
        float range = 0.0f;
        uint32 targets = 1;
        float readySec = 0.0f;
        float againSec = 0.0f;
    };

    std::vector<RangedToolSpell> TankRangedToolSpells(PlayerbotAI* tankAI, Player* tank)
    {
        std::vector<uint32> ids;
        for (char const* tool : TankRangedTools(tank))
            if (uint32 const spellId = tankAI->GetAiObjectContext()->GetValue<uint32>("spell id", tool)->Get())
                ids.push_back(spellId);
        constexpr uint32 SPELL_SHOOT = 3018;  // CastShootAction's ids
        constexpr uint32 SPELL_THROW = 2764;
        if (tank->getClass() == CLASS_WARRIOR)
            if (Item* const ranged = tank->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
                switch (ranged->GetTemplate()->SubClass)
                {
                    case ITEM_SUBCLASS_WEAPON_GUN:
                    case ITEM_SUBCLASS_WEAPON_BOW:
                    case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                        ids.push_back(SPELL_SHOOT);
                        break;
                    case ITEM_SUBCLASS_WEAPON_THROWN:
                        ids.push_back(SPELL_THROW);
                        break;
                    default:
                        break;
                }
        std::vector<RangedToolSpell> tools;
        for (uint32 const spellId : ids)
        {
            RangedToolSpell tool;
            tool.info = sSpellMgr->GetSpellInfo(spellId);
            if (!tool.info)
                continue;
            tool.range = tool.info->GetMaxRange(false);
            for (uint8 effect = 0; effect < MAX_SPELL_EFFECTS; ++effect)
                tool.targets = std::max(tool.targets, tool.info->Effects[effect].ChainTarget);
            tool.readySec = float(tank->GetSpellCooldownDelay(spellId)) / IN_MILLISECONDS;
            tool.againSec = float(std::max({tool.info->RecoveryTime, tool.info->CategoryRecoveryTime,
                                            tool.info->StartRecoveryTime, uint32(tool.info->CalcCastTime(tank))})) /
                            IN_MILLISECONDS;
            tools.push_back(tool);
        }
        return tools;
    }

    // [lead206] [lead215] The casters / shooters of `ranged` against the tank's ranged tools from `from` (`gathered`:
    // every one comes to `from`, round a corner). Each tool ready now takes the nearest ones in its range, as many as
    // the spell jumps to (the chain targets), else one: held. Every other one hits a member until the first tool in
    // its range is ready again or it dies (its health over the group's damage): its damage meanwhile
    // (LandedMeleeDps: a caster's spells) over the healers' healing rate is the seconds it costs. Its kill time is
    // in the fight already; this is the threat loss only.
    struct RangedLoss
    {
        uint32 held = 0;
        float lossSec = 0.0f;
    };

    RangedLoss TankRangedHolds(PlayerbotAI* tankAI, Player* tank, Position const& from, std::vector<Unit*> ranged,
                               bool gathered, float groupDps, float healRate)
    {
        auto const gap = [&](Unit* mob) { return gathered ? 0.0f : from.GetExactDist(mob); };
        std::sort(ranged.begin(), ranged.end(), [&](Unit* a, Unit* b) { return gap(a) < gap(b); });
        RangedLoss loss;
        std::vector<RangedToolSpell> tools = TankRangedToolSpells(tankAI, tank);
        for (RangedToolSpell& tool : tools)
        {
            if (tool.readySec > 0.0f)
                continue;
            uint32 targets = tool.targets;
            for (auto mob = ranged.begin(); mob != ranged.end() && targets;)
            {
                if (gap(*mob) <= tool.range + (*mob)->GetCombatReach())
                {
                    mob = ranged.erase(mob);
                    ++loss.held;
                    --targets;
                }
                else
                    ++mob;
            }
            if (targets < tool.targets)
                tool.readySec = tool.againSec;  // used now
        }
        for (Unit* mob : ranged)
        {
            float looseSec = groupDps > 0.0f ? float(mob->GetHealth()) / groupDps : 0.0f;
            for (RangedToolSpell const& tool : tools)
                if (gap(mob) <= tool.range + mob->GetCombatReach())
                    looseSec = std::min(looseSec, tool.readySec);
            float const damage = LandedMeleeDps(mob, tank) * looseSec;
            loss.lossSec += healRate > 0.0f ? damage / healRate : looseSec;
        }
        return loss;
    }

    // [lead206] the farthest walk of a living member (not the tank) to `to`, straight
    float GroupWalkTo(Player* tank, Position const& to)
    {
        float farthest = 0.0f;
        if (Group* group = tank->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource();
                    member && member != tank && member->IsAlive() && member->GetMap() == tank->GetMap())
                    farthest = std::max(farthest, member->GetExactDist(to));
        return farthest;
    }

    bool RangedMob(Unit* mob)
    {
        Creature* creature = mob->ToCreature();
        return MobRoles::IsCaster(mob) || (creature && KeepsRange(creature));
    }

    // [lead206] user: a corner pull only when it is cheaper. Both ways in seconds of the group's time, each to the
    // pack's spot, where the route goes on: the corner - the tank's walk to the cast spot and on to the corner or the
    // group's walk to its spot behind it (the longer), the casters' walk round it, the fight of the pack that comes,
    // the walk back to the pack's spot (lead212) and the threat loss of the casters that come round (lead215);
    // standing - the walk in, the fight where the pack stands and the threat loss there; [lead215] a pull back - the
    // walk to its cast spot, the pack's walk to the kill spot, the fight there, the walk on to the pack's spot and
    // the threat loss of the casters that keep their range from the kill spot. The threat loss: TankRangedHolds.
    struct CornerCost
    {
        float cornerSec = 0.0f;
        float cornerLossSec = 0.0f;
        float standSec = 0.0f;
        float lossSec = 0.0f;
        uint32 ranged = 0;
        uint32 held = 0;
    };

    CornerCost CornerVsStand(PlayerbotAI* tankAI, Player* tank, Unit* candidate, Position const& castSpot,
                             Position const& corner, Position const& gather, std::vector<Unit*> const& losPack,
                             float losFightSec, std::vector<Unit*> const& standPack, float standFightSec,
                             float groupDps, float healRate, Position const* pullBackKill)
    {
        CornerCost cost;
        float const speed = std::max(tank->GetSpeed(MOVE_RUN), 0.1f);
        float const tankCorner = tank->GetExactDist(castSpot) + castSpot.GetExactDist(corner);
        float casterSec = 0.0f;
        std::vector<Unit*> losRanged;
        for (Unit* mob : losPack)
            if (RangedMob(mob))
            {
                losRanged.push_back(mob);
                casterSec = std::max(casterSec, mob->GetExactDist(corner) / std::max(mob->GetSpeed(MOVE_RUN), 0.1f));
            }
        // [lead212] ... and the walk back from the corner to where the pack stood: the route goes on from there, where
        // the fight without the corner ends (b184 rfc-h 5F: the corner cost ended at the corner)
        float const walkBack = corner.GetExactDist(candidate->GetPosition());
        // [lead215] b184 rfc-h 5D: 99 s of aggro off the tank in corner pulls - the casters that come round are the
        // tank's tools' too
        cost.cornerLossSec = TankRangedHolds(tankAI, tank, corner, losRanged, true, groupDps, healRate).lossSec;
        cost.cornerSec = (std::max(tankCorner, GroupWalkTo(tank, gather)) + walkBack) / speed + casterSec +
                         losFightSec + cost.cornerLossSec;

        std::vector<Unit*> ranged;
        for (Unit* mob : standPack)
            if (RangedMob(mob))
                ranged.push_back(mob);
        cost.ranged = uint32(ranged.size());
        Position const fight = pullBackKill ? *pullBackKill : candidate->GetPosition();
        RangedLoss const loss = TankRangedHolds(tankAI, tank, fight, ranged, false, groupDps, healRate);
        cost.held = loss.held;
        cost.lossSec = loss.lossSec;
        if (pullBackKill)
        {
            float mobSec = 0.0f;
            for (Unit* mob : standPack)
                mobSec = std::max(mobSec, mob->GetExactDist(*pullBackKill) / std::max(mob->GetSpeed(MOVE_RUN), 0.1f));
            float const walk = std::max(tank->GetExactDist(castSpot), GroupWalkTo(tank, castSpot)) +
                               pullBackKill->GetExactDist(candidate->GetPosition());
            cost.standSec = walk / speed + mobSec + standFightSec + cost.lossSec;
        }
        else
        {
            float const standWalk =
                std::max(tank->GetExactDist(candidate), GroupWalkTo(tank, candidate->GetPosition()));
            cost.standSec = standWalk / speed + standFightSec + cost.lossSec;
        }
        return cost;
    }

    // [lead199] b181 rfc-h 5B / 5F: the prot warrior opened 37 / 48 and 33 / 41 pulls without a hit - Heroic Throw
    // (1 min) on cooldown, the engage opened with Taunt, which gives no threat on a mob that fights nobody. Per class
    // the damage openers first, the gap closer (rage and a stun, no walk), the ranged weapon, the taunt last.
    std::vector<char const*> TankEngageOpeners(Player* tank, bool gapClosers)
    {
        std::vector<char const*> openers;
        switch (tank->getClass())
        {
            case CLASS_PALADIN:
                openers = {"avenger's shield", "hand of reckoning"};
                break;
            case CLASS_WARRIOR:
                openers = {"heroic throw"};
                if (gapClosers)
                    openers.push_back("charge");
                openers.insert(openers.end(), {"shoot", "throw", "taunt"});
                break;
            case CLASS_DRUID:
                openers = {"faerie fire (feral)"};
                if (gapClosers)
                    openers.push_back("feral charge - bear");
                openers.push_back("growl");
                break;
            case CLASS_DEATH_KNIGHT:
                openers = {"death grip", "icy touch", "dark command"};  // the grip brings the mob, no walk
                break;
            default:
                break;
        }
        return openers;
    }

    // [drift_caster_corner] The casters and shooters of the fight on that stand off the tank and cast at it from
    // there (in its sight, not walking), no boss among the attackers. [lead202] b181 rfc-h: lone Searing Blade
    // Warlocks / Cultists and Ragefire Shamans at range took mobs off bear / warrior tanks (the corner wanted two):
    // every one the tank has no ranged tool for now (TankRangedTool) counts, even one. True with the tank's corner
    // `spot` and the group's `gather` spot behind it (FindLosPullSpot, at most MAX_WALK away), and the casters in
    // `casters`.
    bool FindCasterCorner(PlayerbotAI* botAI, Player* tank, Position& spot, Position& gather,
                          std::vector<Unit*>& casters)
    {
        casters.clear();
        AiObjectContext* context = botAI->GetAiObjectContext();
        for (ObjectGuid const& guid : context->GetValue<GuidVector>("attackers")->Get())
        {
            Unit* unit = botAI->GetUnit(guid);
            Creature* mob = unit ? unit->ToCreature() : nullptr;
            if (!mob || !mob->IsAlive())
                continue;

            if (IsBossMob(mob))
                return false;  // an encounter keeps its own positions

            if (!(MobRoles::IsCaster(mob) || KeepsRange(mob)) || IsCrowdControlled(mob) || mob->isMoving() ||
                mob->IsRooted() || tank->GetExactDist(mob) <= CasterCorner::OFF_TANK || !mob->IsWithinLOSInMap(tank))
                continue;

            if (TankRangedTool(botAI, tank, mob))
                continue;  // the tank takes it from where it stands (a grip brings it in)

            casters.push_back(mob);
        }

        if (casters.empty())
            return false;

        std::vector<Unit*> others;
        for (ObjectGuid const& guid : context->GetValue<GuidVector>("possible targets no los")->Get())
        {
            Unit* mob = botAI->GetUnit(guid);
            if (mob && IsIdleEnemy(tank, mob) && std::find(casters.begin(), casters.end(), mob) == casters.end())
                others.push_back(mob);
        }

        if (!FindLosPullSpot(tank, casters, others, tank->GetPosition(), spot, gather))
            return false;

        return tank->GetExactDist(spot) <= CasterCorner::MAX_WALK;
    }
}

namespace FightSpot
{
    void Plan(Player* tank, Position const& spot, float back, std::string const& reason)
    {
        std::lock_guard<std::mutex> guard(fightSpotLock);
        FightSpotEntry& entry = fightSpots[tank->GetGUID().GetRawValue()];
        entry.spot = spot;
        entry.back = back;
        entry.reason = reason;
        entry.planMs = getMSTime();
        entry.chosenMs = 0;
        entry.seenMs = 0;
    }

    void Chosen(Player* tank)
    {
        std::lock_guard<std::mutex> guard(fightSpotLock);
        FightSpotEntry& entry = fightSpots[tank->GetGUID().GetRawValue()];
        entry.chosenMs = getMSTime();
        entry.planMs = 0;
        entry.seenMs = 0;
    }

    bool Review(PlayerbotAI* botAI, Player* tank, Unit* target, Position& spot)
    {
        if (!target || !tank->GetMap() || !tank->GetMap()->IsDungeon())
            return false;

        GuidVector const possible = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets")->Get();
        float const seconds = PATROL_PULL_LEAD_SEC + FIGHT_SPOT_COMBAT_SEC;
        FightIntruder intruder;
        if (!WalksIntoFight(botAI, tank, possible, target, spot, seconds, intruder))
            return false;

        // back along the line from the mob through the spot: the way the group came
        float const dx = spot.GetPositionX() - target->GetPositionX();
        float const dy = spot.GetPositionY() - target->GetPositionY();
        float const length = std::sqrt(dx * dx + dy * dy);
        std::string const reason = FightIntruderNote(intruder);
        Position back;
        if (length < 1.0f || !FindFightSpotBack(botAI, tank, possible, target, spot, dx / length, dy / length,
                                                tank->GetPosition(), {target}, seconds, 1, back))
        {
            LOG_DEBUG("playerbots", "[fight spot] {}: {}, no spot back from the pull position", tank->GetName(),
                      reason);
            return false;
        }

        LOG_INFO("playerbots", "[fight spot] {} moves the fight back {:.0f} yd: {} (the pull of {} lands)",
                 tank->GetName(), spot.GetExactDist(back), reason, target->GetName());
        spot = back;
        Chosen(tank);
        return true;
    }

    bool Due(PlayerbotAI* botAI, Player* tank, Position& spot)
    {
        uint32 const now = getMSTime();
        uint64 const key = tank->GetGUID().GetRawValue();
        bool cornerDue = false;
        {
            std::lock_guard<std::mutex> guard(fightSpotLock);
            FightSpotEntry& entry = fightSpots[key];
            if (!entry.seenMs || getMSTimeDiff(entry.seenMs, now) > FIGHT_SPOT_COMBAT_GAP_MS)
            {
                // a new fight: the plan's walk back, or the plan's checked spot, or a look of its own
                entry.combatMs = now;
                entry.lookMs = 0;
                entry.walkMs = 0;
                entry.done = entry.chosenMs && getMSTimeDiff(entry.chosenMs, now) <= FIGHT_SPOT_PLAN_MS;
                entry.cornerLookMs = 0;
                entry.cornerDone = false;
                if (entry.planMs && getMSTimeDiff(entry.planMs, now) <= FIGHT_SPOT_PLAN_MS)
                {
                    entry.walkMs = now;
                    FightHold::Move(tank, entry.spot);  // [drift_fight_hold] the hold goes with the fight
                    LOG_INFO("playerbots", "[fight spot] {} moves the fight back {:.0f} yd: {}", tank->GetName(),
                             entry.back, entry.reason);
                }

                entry.planMs = 0;
                entry.chosenMs = 0;
            }

            entry.seenMs = now;
            if (entry.walkMs)
            {
                if (getMSTimeDiff(entry.walkMs, now) < FIGHT_SPOT_WALK_MS &&
                    tank->GetExactDist(entry.spot) > FIGHT_SPOT_ARRIVED)
                {
                    spot = entry.spot;
                    return true;
                }

                entry.walkMs = 0;
                entry.done = true;
            }

            if (entry.done || getMSTimeDiff(entry.combatMs, now) > FIGHT_SPOT_REVIEW_MS ||
                (entry.lookMs && getMSTimeDiff(entry.lookMs, now) < FIGHT_SPOT_LOOK_MS))
            {
                // [drift_caster_corner] past the movers' review: one look a second for casters that keep their range
                if (entry.cornerDone || getMSTimeDiff(entry.combatMs, now) < CasterCorner::FIGHT_MIN_MS ||
                    (entry.cornerLookMs && getMSTimeDiff(entry.cornerLookMs, now) < FIGHT_SPOT_LOOK_MS))
                    return false;

                entry.cornerLookMs = now ? now : 1;
                cornerDue = true;
            }
            else
                entry.lookMs = now;
        }

        // A fight already on (a body pull, a mob that came by itself): the tank's own spot against the movers.
        Map* map = tank->GetMap();
        if (!map || !map->IsDungeon() || tank->IsRooted())
            return false;

        if (cornerDue)
        {
            // [drift_caster_corner] casters that cast from range: round the corner, the group gathers behind it
            LosPull::Plan plan;
            if (!map->IsNonRaidDungeon() || LosPull::Get(tank, plan))
                return false;

            Position corner;
            Position gather;
            std::vector<Unit*> casters;
            if (!FindCasterCorner(botAI, tank, corner, gather, casters))
                return false;

            GuidVector pack;
            for (Unit* caster : casters)
                pack.push_back(caster->GetGUID());

            LosPull::Register(tank, casters.front(), pack, corner, gather);
            LosPull::MarkCast(tank);  // the casters are pulled already: the group holds behind until they come
            float const distance = tank->GetExactDist(corner);
            LOG_INFO("playerbots",
                     "[caster corner] {}: {} casters cast from range ({} first, {:.0f} yd): the fight goes round the "
                     "corner to ({:.1f},{:.1f},{:.1f}) {:.0f} yd away, the group behind at ({:.1f},{:.1f},{:.1f})",
                     tank->GetName(), casters.size(), casters.front()->GetName(), tank->GetExactDist(casters.front()),
                     corner.GetPositionX(), corner.GetPositionY(), corner.GetPositionZ(), distance,
                     gather.GetPositionX(), gather.GetPositionY(), gather.GetPositionZ());
            std::lock_guard<std::mutex> guard(fightSpotLock);
            FightSpotEntry& entry = fightSpots[key];
            entry.spot = corner;
            entry.back = distance;
            entry.reason = "casters keep their range";
            entry.walkMs = now ? now : 1;
            entry.cornerDone = true;
            FightHold::Move(tank, corner);
            spot = corner;
            return true;
        }

        AiObjectContext* context = botAI->GetAiObjectContext();
        GuidVector const possible = context->GetValue<GuidVector>("possible targets")->Get();
        Unit* target = tank->GetVictim();
        FightIntruder intruder;
        if (!WalksIntoFight(botAI, tank, possible, target, tank->GetPosition(), FIGHT_SPOT_COMBAT_SEC, intruder))
            return false;

        // Back toward the group (the way it came, cleared), else away from the mob through the tank.
        float sumX = 0.0f;
        float sumY = 0.0f;
        uint32 members = 0;
        if (Group* group = tank->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member != tank && IsPresent(tank, member) &&
                    tank->GetExactDist(member) <= PATROL_GROUP_RANGE)
                {
                    sumX += member->GetPositionX();
                    sumY += member->GetPositionY();
                    ++members;
                }
            }
        }

        float dx = 0.0f;
        float dy = 0.0f;
        if (members)
        {
            dx = sumX / float(members) - tank->GetPositionX();
            dy = sumY / float(members) - tank->GetPositionY();
        }

        if (std::sqrt(dx * dx + dy * dy) < FIGHT_SPOT_STEP * 0.5f && target)
        {
            dx = tank->GetPositionX() - target->GetPositionX();
            dy = tank->GetPositionY() - target->GetPositionY();
        }

        float const length = std::sqrt(dx * dx + dy * dy);
        std::string const reason = FightIntruderNote(intruder);
        std::vector<Unit*> fighting;
        for (ObjectGuid const& guid : context->GetValue<GuidVector>("attackers")->Get())
        {
            if (Unit* unit = botAI->GetUnit(guid))
                fighting.push_back(unit);
        }

        Position back;
        if (length < 1.0f || !FindFightSpotBack(botAI, tank, possible, target, tank->GetPosition(), dx / length,
                                                dy / length, tank->GetPosition(), fighting, FIGHT_SPOT_COMBAT_SEC, 1,
                                                back))
        {
            LOG_DEBUG("playerbots", "[fight spot] {}: {}, no spot back from here", tank->GetName(), reason);
            return false;
        }

        float const distance = tank->GetExactDist(back);
        LOG_INFO("playerbots", "[fight spot] {} moves the fight back {:.0f} yd: {}", tank->GetName(), distance,
                 reason);
        std::lock_guard<std::mutex> guard(fightSpotLock);
        FightSpotEntry& entry = fightSpots[key];
        entry.spot = back;
        entry.back = distance;
        entry.reason = reason;
        entry.walkMs = now;
        FightHold::Move(tank, back);  // [drift_fight_hold]
        spot = back;
        return true;
    }

    bool Walking(Player* tank, Position& spot)
    {
        std::lock_guard<std::mutex> guard(fightSpotLock);
        auto const found = fightSpots.find(tank->GetGUID().GetRawValue());
        if (found == fightSpots.end() || !found->second.walkMs ||
            getMSTimeDiff(found->second.walkMs, getMSTime()) >= FIGHT_SPOT_WALK_MS)
            return false;

        spot = found->second.spot;
        return true;
    }

    void Arrived(Player* tank)
    {
        std::lock_guard<std::mutex> guard(fightSpotLock);
        auto const found = fightSpots.find(tank->GetGUID().GetRawValue());
        if (found == fightSpots.end())
            return;

        found->second.walkMs = 0;
        found->second.done = true;
    }
}

bool AggressiveTargetAction::HoldForPrePullCc(Unit* pullTarget, bool heavy, bool refused, uint32 plannedMobs)
{
    constexpr uint32 DANGEROUS_PACK_SIZE = 3;  // a smaller pack with a healer or two casters, as AutoCcHardPackValue
    if (!sPlayerbotAIConfig.AutoCc || !sPlayerbotAIConfig.AutoCcPrePull || !sPlayerbotAIConfig.AutoCcMarkMinPackSize ||
        !pullTarget || pullTarget->IsInCombat() || IsBossMob(pullTarget))
        return false;

    // [b39_cos] Heroic Culling while the Guardian of Time runs: no pre-pull crowd control and no hold for it
    // (CoSTimedRunCcMultiplier; cos-h 20261026-050341-cos-h-5H-18 wave 6: Polymorph + Freezing Arrow on the wave,
    // 71 s for 6 kills).
    if (bot->GetMapId() == CoSIds::MAP_CULLING_OF_STRATHOLME && CoSHelpers::IsGuardianTimerRunning(bot))
        return false;

    Group* group = bot->GetGroup();
    Map* map = bot->GetMap();
    if (!group || !map || !map->IsNonRaidDungeon() || bot->InBattleground() || IsGroupFighting(bot))
        return false;

    // [lead39] b88 rfc-h 22 s: "held for pre-pull cc" on a pull of lone wanderers - a wanderer walks out of any
    // crowd control plan and needs none: no hold for it
    // [lead43] ... nor does a patrol (b92 rfc-h 387 s: the patrol Shaman "held for pre-pull cc")
    if (Creature const* creature = pullTarget->ToCreature();
        creature && (creature->GetDefaultMovementType() == RANDOM_MOTION_TYPE ||
                     creature->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE))
        return false;

    bool const measured = GetPullMode(bot, PlanStrength()) == PullMode::Measured;  // [lead7] the look's snapshot
    GuidVector const possible = PlanPossible(measured);
    uint32 const now = getMSTime();
    // Sweep batch4 (Scholomance Risen Guard "pull back 1 -> came 4" 3/3, Gnomeregan "pull back 2 -> came 4", BRS
    // "los pull 5 -> came 11", all "via cc"): the plan was registered on the pack as it stands where its pull target
    // is engaged (4 mobs), and a claim cast on the pull (Freezing Arrow, Polymorph, Hex) at a mob the pull back left
    // standing engaged it there: its neighbours answered its call for assistance and came. An accepted plan that
    // splits the pack (`plannedMobs` fewer than that pack) gets no crowd control opener at the pack; a claimed Sap the
    // plan counts as applied (_planCountsSap) is kept. Looked at once per PRE_PULL_CC_RECHECK_MS per mob.
    if (plannedMobs && !_planCountsSap)
    {
        bool splits = _ccSplitFor == pullTarget->GetGUID() && getMSTimeDiff(_ccSplitMs, now) < PRE_PULL_CC_RECHECK_MS;
        if (!splits)
        {
            std::vector<Unit*> atSpot;
            CollectPack(botAI, bot, pullTarget, pullTarget->GetPosition(), possible, atSpot,
                        FightHere(pullTarget->GetPosition()), measured);
            splits = plannedMobs < atSpot.size();
            if (splits)
            {
                _ccSplitFor = pullTarget->GetGUID();
                _ccSplitMs = now;
                LOG_DEBUG("playerbots", "[pre-pull cc] {} skips {}: the plan splits its pack ({} of {} come), an "
                          "opener at the pack would bring the rest", bot->GetName(), pullTarget->GetName(),
                          plannedMobs, atSpot.size());
            }
        }

        if (splits)
        {
            PrePullCc::Plan current;
            if (!_shadow && PrePullCc::Get(bot, current) && current.tank == bot->GetGUID() && !current.pulledMs)
                PrePullCc::Cancel(bot);  // [lead7] a shadow look cancels nothing

            return false;
        }
    }

    // A plan registered while the tank refused the pack, and the verdict is accepted now: its `heavy` and the
    // declines of the refusal are stale (wave 44: they stuck, a Sap the accepted pull needed never came). Reopened.
    PrePullCc::Plan current;
    bool const reopen = !refused && _ccRefusedPlanId && PrePullCc::Get(bot, current) &&
                        current.id == _ccRefusedPlanId && current.tank == bot->GetGUID() && !current.pulledMs;
    if (PrePullCc::HasPlan(bot, pullTarget->GetGUID()))
    {
        if (reopen)
        {
            if (!_shadow)  // [lead7] a shadow look registers nothing
                PrePullCc::Register(bot, pullTarget, current.pack, current.controllers, heavy, true);
            _ccRefusedPlanId = 0;
        }
        else if (refused && PrePullCc::Get(bot, current) && current.tank == bot->GetGUID())
            _ccRefusedPlanId = current.id;

        return PrePullCc::IsHolding(bot);
    }

    // A pack found not worth it is looked at again only after a while: the pack search is the expensive part.
    if (_ccSkipFor == pullTarget->GetGUID() && getMSTimeDiff(_ccSkipMs, now) < PRE_PULL_CC_RECHECK_MS)
        return false;

    _ccSkipFor = pullTarget->GetGUID();
    _ccSkipMs = now;

    // Everything that fights once the pull target is engaged where it stands, counted as the planner counts it (a
    // measured careful pull follows the assistance chain): Scholomance wave 27 read the 27-mob hall as 4-5 here,
    // so a trap was laid at its edge and the whole hall came.
    std::vector<Unit*> pack;
    CollectPack(botAI, bot, pullTarget, pullTarget->GetPosition(), possible, pack, FightHere(pullTarget->GetPosition()),
                measured);
    uint32 healers = 0;
    uint32 casters = 0;
    for (Unit* mob : pack)
    {
        if (IsBossMob(mob))
            return false;

        if (MobRoles::IsHealer(mob))
            ++healers;
        else if (MobRoles::IsCaster(mob))
            ++casters;
    }

    uint32 const mobs = uint32(pack.size());
    uint32 const minPack = sPlayerbotAIConfig.AutoCcMarkMinPackSize;
    uint32 const smallPack = std::min(minPack, DANGEROUS_PACK_SIZE);
    if (mobs < minPack && (mobs < smallPack || (!heavy && !healers && casters < 2)))
        return false;

    // The kill order the tank opens with: the pull target, then the rest as the tank's kill marks will rank them.
    std::vector<Unit*> rest(pack.begin() + 1, pack.end());
    KillOrder::SortKillOrder(rest, pullTarget, nullptr);
    // [rot_b31_cc] A pack too big as it stands, or one with two casters or healers, holds its casters and healers
    // past the pull target, not its melee: the claims come from the tail (KILL_FIRST on), and the kill order's
    // casters-first ([b30_mech_pos]) left only melee there (brs-h 20261025: "plans Smolderthorn Mystic (4 mobs,
    // refused as it stands)", every claim on an Axe Thrower, the Mystics never held). The melee ones move up to the
    // head; the live kill marks skip whatever ends up held (RankTankPack), so kill order and crowd control agree.
    if (heavy || healers + casters >= 2)
        std::stable_partition(rest.begin(), rest.end(),
                              [](Unit* mob) { return !MobRoles::IsHealer(mob) && !MobRoles::IsCaster(mob); });

    GuidVector order;
    order.push_back(pullTarget->GetGUID());
    for (Unit* mob : rest)
        order.push_back(mob->GetGUID());

    // [prepull_cc_capable] STN92_EFFICIENCY 2026-10-25 (st-n-92: "3 crowd control members asked" with a Polymorph and
    // a trap in the group, the Enhancement shaman without Hex; 3 of 21 claims placed, 77 s of crowd control waits):
    // only a member that has a crowd control spell usable on a mob of the claimable tail (PrePullCcAction's
    // candidates: past the kill order's head, elite in a dungeon) is asked, so the claim window waits for nobody
    // who can only decline.
    std::vector<Unit*> tail;
    for (size_t i = PrePullCc::KILL_FIRST; i < order.size(); ++i)
    {
        Unit* mob = botAI->GetUnit(order[i]);
        Creature* creature = mob ? mob->ToCreature() : nullptr;
        if (creature && creature->IsAlive() && !creature->IsInCombat() && !IsCrowdControlled(creature) &&
            (!map->IsDungeon() || creature->isElite()))
            tail.push_back(creature);
    }

    if (tail.empty())
        return false;

    // Who may answer: the crowd control dps, or the healer of a group that has none (AutoCcMarkAction::isUseful).
    GuidVector dps;
    GuidVector healerControllers;
    std::string incapable;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        PlayerbotAI* memberAI = member ? GET_PLAYERBOT_AI(member) : nullptr;
        if (!member || member == bot || !member->IsAlive() || member->GetMap() != map ||
            PlayerbotAI::IsTank(member) || !memberAI || !GetCrowdControlRole(member->getClass()))
            continue;

        bool const capable = std::any_of(tail.begin(), tail.end(), [memberAI](Unit* mob)
                                         { return FindCrowdControl(memberAI, mob, true) != nullptr; });
        if (!capable)
        {
            incapable += (incapable.empty() ? "" : ", ") + member->GetName();
            continue;
        }

        (PlayerbotAI::IsHeal(member) ? healerControllers : dps).push_back(member->GetGUID());
    }

    GuidVector const& controllers = dps.empty() ? healerControllers : dps;
    if (!incapable.empty())
        LOG_DEBUG("playerbots", "[prepull_cc_capable] {} on {}: not asked, no crowd control for its mobs: {}; asked {}",
                  bot->GetName(), pullTarget->GetName(), incapable, controllers.size());

    if (controllers.empty())
        return false;

    // A refused pack that stays too big with every controller placed is not made pullable by crowd control, and the
    // first Polymorph / Trap on it brings the whole pack (Scholomance wave 16: 24 linked mobs, 2 of 3 claims, wipe).
    // The planner's lone-mob minimum takes it apart instead.
    constexpr uint32 HEAVY_CC_REMAINING_MAX = 4;
    if (heavy && mobs > uint32(controllers.size()) + HEAVY_CC_REMAINING_MAX)
    {
        LOG_DEBUG("playerbots", "[pre-pull cc] {} skips {}: {} mobs, {} crowd control members, too many left",
                  bot->GetName(), pullTarget->GetName(), mobs, controllers.size());
        return false;
    }

    _ccSkipFor.Clear();
    // [lead7] a shadow look registers nothing: a plan registered now would hold the pull (its claims are pending)
    if (_shadow)
        return true;

    PrePullCc::Register(bot, pullTarget, order, controllers, heavy, reopen);
    if (reopen)
        _ccRefusedPlanId = 0;

    PrePullCc::Plan registered;
    if (refused && PrePullCc::Get(bot, registered) && registered.tank == bot->GetGUID())
        _ccRefusedPlanId = registered.id;

    return PrePullCc::IsHolding(bot);
}

namespace PullPlanning  // [lead31a] was anonymous: shared with the tank's execution
{
    struct PullRecord
    {
        uint32 mobs = 0;
        float cost = 0.0f;
        float drop = 0.0f;
        std::string kind;
        std::string method;
        std::string spot;
        ObjectGuid target;
        uint32 plannedMs = 0;
        uint32 startMs = 0;      // first sample in a fight
        uint32 deaths = 0;       // most members dead at once
        GuidVector came;         // every mob seen fighting the group
        std::vector<uint32> cameHealth;  // parallel to `came`: its maximum health when first seen
        uint64 takenAtStart = 0;         // the tank's DamageTaken total when the fight began
        uint64 spellAtStart = 0;         // [lead256] ... and its DamageTaken::Spell part
        struct HealerStart
        {
            ObjectGuid healer;
            uint64 healed = 0;  // HealingDone::Effective
            float mana = 0.0f;
        };
        std::vector<HealerStart> healers;  // [lead258] the healers present when the fight began
        float dropScale = 1.0f;            // [lead259] the measured drop ratio `drop` was planned with
        float tankStartPct = 0.0f;         // [lead259] the tank's health when the fight began
        float tankLowPct = 100.0f;         // [lead259] ... and the lowest in it
    };

    std::mutex pullCheckLock;
    std::unordered_map<uint64, PullRecord> pullChecks;  // by tank
    std::unordered_map<uint64, uint32> lastFights;     // [chain_pull] by tank: its group's last fight (Sample)

    // [lead256] one measured sample: a value over a weight (a fight's killed health over its seconds; the damage the
    // tank took over the template's)
    struct MeasuredSample
    {
        float value = 0.0f;
        float weight = 0.0f;
    };

    struct MeasuredDps
    {
        std::vector<MeasuredSample> fights;  // killed health over the fight's seconds
        uint32 instanceId = 0;
    };
    std::unordered_map<uint64, MeasuredDps> measuredDps;  // by tank, under pullCheckLock

    // [lead209] user (approved, "only by facts"): the landed estimate is the template (LandedMeleeDps) times the
    // ratio real / template measured in this dungeon - per mob entry where a fight of that entry alone measured it,
    // else the dungeon's trash ratio - and its spread is the upper quantile of the same measured ratios (no fixed
    // floor or safety factor). Nothing measured: the template.
    constexpr float LANDED_RATIO_QUANTILE = 0.9f;  // the upper quantile of the measured ratios (user-approved spread)
    constexpr float MEASURED_QUANTILE = LANDED_RATIO_QUANTILE;  // [lead256] the same spread for every measured fact

    // [lead256] the ratios real / template of melee and of spells apart (each sample: real over template damage)
    struct TakenRatios
    {
        std::vector<MeasuredSample> melee;
        std::vector<MeasuredSample> spell;
    };

    // [lead259] the tank's real deepest drop against the planned one (DropOver), this dungeon's trash fights
    struct MeasuredDrop
    {
        std::vector<MeasuredSample> fights;  // real drop over the planned drop
        uint32 instanceId = 0;
    };
    std::unordered_map<uint64, MeasuredDrop> measuredDrops;  // by tank, under pullCheckLock

    struct MeasuredTaken
    {
        TakenRatios trash;                              // every fight without a boss
        std::unordered_map<uint32, TakenRatios> entry;  // fights of one mob entry alone
        uint32 instanceId = 0;
    };
    std::unordered_map<uint64, MeasuredTaken> measuredTaken;  // by tank, under pullCheckLock

    // [lead256] the weighted quantile of samples (value / weight each): the ratio below which `share` of the weight
    // lies; -1 without a sample
    float WeightedQuantile(std::vector<MeasuredSample> samples, float share)
    {
        float total = 0.0f;
        for (MeasuredSample const& sample : samples)
            total += sample.weight;
        if (samples.empty() || total <= 0.0f)
            return -1.0f;

        std::sort(samples.begin(), samples.end(), [](MeasuredSample const& left, MeasuredSample const& right)
                  { return left.value * right.weight < right.value * left.weight; });
        float seen = 0.0f;
        for (MeasuredSample const& sample : samples)
        {
            seen += sample.weight;
            if (seen >= share * total && sample.weight > 0.0f)
                return sample.value / sample.weight;
        }
        MeasuredSample const& last = samples.back();
        return last.weight > 0.0f ? last.value / last.weight : -1.0f;
    }

    // The fight is over: what the tank took against the template of what came (each mob's LandedMeleeDps over its
    // life in the planner's model: its own kill time plus half the others', at the fight's real kill rate), melee
    // and spells apart. Under pullCheckLock.
    void NoteMeasuredTaken(PlayerbotAI* botAI, Player* tank, PullRecord const& record, uint32 now)
    {
        uint32 const fightMs = record.startMs ? getMSTimeDiff(record.startMs, now) : 0;
        if (!fightMs || record.deaths || record.came.empty() || record.cameHealth.size() != record.came.size())
            return;

        uint64 const total = DamageTaken::Total(tank->GetGUID());
        uint64 const spellTotal = DamageTaken::Spell(tank->GetGUID());
        if (total < record.takenAtStart || spellTotal < record.spellAtStart)
            return;

        float totalHealth = 0.0f;
        for (uint32 health : record.cameHealth)
            totalHealth += float(health);
        if (totalHealth <= 0.0f)
            return;

        float const fightSec = float(fightMs) / float(IN_MILLISECONDS);
        float templateMelee = 0.0f;
        float templateSpell = 0.0f;
        bool boss = false;
        uint32 oneEntry = 0;
        bool singleEntry = true;
        for (size_t i = 0; i < record.came.size(); ++i)
        {
            Unit* mob = botAI->GetUnit(record.came[i]);
            if (!mob || !mob->ToCreature())
                return;  // a mob the template cannot read: no measurement
            boss = boss || IsBossMob(mob);
            if (!oneEntry)
                oneEntry = mob->GetEntry();
            singleEntry = singleEntry && mob->GetEntry() == oneEntry;
            float const health = float(record.cameHealth[i]);
            float const life = (health + (totalHealth - health) / 2.0f) / totalHealth * fightSec;
            float spell = 0.0f;
            float const landed = LandedMeleeDps(mob, tank, false, &spell);
            templateMelee += (landed - spell) * life;
            templateSpell += spell * life;
        }

        float realSpell = float(spellTotal - record.spellAtStart);
        float realMelee = float(total - record.takenAtStart) - realSpell;
        if (templateSpell <= 0.0f)  // spells no template casts: the melee side carries them
        {
            realMelee += realSpell;
            realSpell = 0.0f;
        }
        else if (templateMelee <= 0.0f)  // melee no template swings: the spell side carries it
        {
            realSpell += std::max(realMelee, 0.0f);
            realMelee = 0.0f;
        }
        if (templateMelee <= 0.0f && templateSpell <= 0.0f)
            return;

        MeasuredTaken& entry = measuredTaken[tank->GetGUID().GetRawValue()];
        if (entry.instanceId != tank->GetInstanceId())
            entry = MeasuredTaken();  // [lead209] this dungeon's fights only
        entry.instanceId = tank->GetInstanceId();
        auto const add = [&](TakenRatios& ratios)
        {
            if (templateMelee > 0.0f)
                ratios.melee.push_back({std::max(realMelee, 0.0f), templateMelee});
            if (templateSpell > 0.0f)
                ratios.spell.push_back({realSpell, templateSpell});
        };
        if (!boss)
            add(entry.trash);
        if (singleEntry)
            add(entry.entry[oneEntry]);
        LOG_DEBUG("playerbots",
                  "[lead256] {}: taken {:.0f} melee / {:.0f} spell against the template's {:.0f} / {:.0f} over "
                  "{:.1f} s ({} mobs{})",
                  tank->GetName(), realMelee, realSpell, templateMelee, templateSpell, fightSec, record.came.size(),
                  boss ? ", a boss" : "");
    }

    // [lead259] The fight is over: the tank's deepest drop in it against the drop the plan expected before the
    // dungeon's ratio (DropOver alone). Under pullCheckLock.
    void NoteMeasuredDrop(PlayerbotAI* botAI, Player* tank, PullRecord const& record)
    {
        if (!record.startMs || record.drop <= 0.0f || record.dropScale <= 0.0f || record.tankStartPct <= 0.0f)
            return;
        for (ObjectGuid const& guid : record.came)  // a boss is healed as the fight goes: trash fights only
            if (Unit* mob = botAI->GetUnit(guid); mob && IsBossMob(mob))
                return;
        float const planned = record.drop / record.dropScale;
        float const real = std::max(record.tankStartPct - record.tankLowPct, 0.0f) / 100.0f;
        MeasuredDrop& entry = measuredDrops[tank->GetGUID().GetRawValue()];
        if (entry.instanceId != tank->GetInstanceId())
            entry = MeasuredDrop();  // this dungeon's fights only
        entry.instanceId = tank->GetInstanceId();
        entry.fights.push_back({real, planned});
        LOG_DEBUG("playerbots", "[lead259] {}: drop {:.2f} against the planned {:.2f} ({} fights)", tank->GetName(),
                  real, planned, entry.fights.size());
    }

    // [lead259] the upper quantile (MEASURED_QUANTILE, each fight weighted by its planned drop) of the dungeon's
    // real / planned drops; nothing measured: 1 (the drop as the game's numbers give it). Under pullCheckLock.
    float DropScaleLocked(Player* tank)
    {
        auto const found = measuredDrops.find(tank->GetGUID().GetRawValue());
        if (found == measuredDrops.end() || found->second.instanceId != tank->GetInstanceId())
            return 1.0f;
        float const scale = WeightedQuantile(found->second.fights, MEASURED_QUANTILE);
        return scale > 0.0f ? scale : 1.0f;
    }

    float DropCalibration(Player* tank)
    {
        std::lock_guard<std::mutex> guard(pullCheckLock);
        return DropScaleLocked(tank);
    }

    // [lead258] The fight is over: each healer's effective healing and net mana spent over it, its measured health
    // per mana and best rate in this dungeon (RestState::NoteHealing). Under pullCheckLock.
    void NoteMeasuredHealing(Player* tank, PullRecord const& record, uint32 now)
    {
        uint32 const fightMs = record.startMs ? getMSTimeDiff(record.startMs, now) : 0;
        if (!fightMs)
            return;

        for (PullRecord::HealerStart const& start : record.healers)
        {
            Player* healer = ObjectAccessor::FindPlayer(start.healer);
            if (!healer || healer->GetMap() != tank->GetMap())
                continue;
            uint64 const healed = HealingDone::Effective(start.healer);
            if (healed < start.healed)
                continue;
            float const spent = start.mana - float(healer->GetPower(POWER_MANA));
            RestState::NoteHealing(healer, float(healed - start.healed), spent,
                                   float(fightMs) / float(IN_MILLISECONDS));
            LOG_DEBUG("playerbots", "[lead258] {}: healer {} healed {} with {:.0f} mana over {:.1f} s", tank->GetName(),
                      healer->GetName(), healed - start.healed, spent, float(fightMs) / float(IN_MILLISECONDS));
        }
    }

    // [lead209] the landed scales of this tank in this dungeon: the trash ratio and the entries measured alone (the
    // upper quantile of each one's measured ratios); nothing measured: 1 (the template). [lead256] melee and spells
    // apart, up as well as down, from the first fight on.
    struct LandedRatios
    {
        LandedScale trash;
        std::unordered_map<uint32, LandedScale> byEntry;
        uint32 meleeFights = 0;  // the trash fights behind it (logged)
        uint32 spellFights = 0;
    };

    LandedScale ScaleOf(TakenRatios const& ratios, LandedScale fallback)
    {
        float const melee = WeightedQuantile(ratios.melee, LANDED_RATIO_QUANTILE);
        float const spell = WeightedQuantile(ratios.spell, LANDED_RATIO_QUANTILE);
        return {melee >= 0.0f ? melee : fallback.melee, spell >= 0.0f ? spell : fallback.spell};
    }

    LandedRatios LandedCalibration(Player* tank)
    {
        LandedRatios out;
        std::lock_guard<std::mutex> guard(pullCheckLock);
        auto const found = measuredTaken.find(tank->GetGUID().GetRawValue());
        if (found == measuredTaken.end() || found->second.instanceId != tank->GetInstanceId())
            return out;

        out.trash = ScaleOf(found->second.trash, LandedScale());
        out.meleeFights = uint32(found->second.trash.melee.size());
        out.spellFights = uint32(found->second.trash.spell.size());
        for (auto const& [mobEntry, ratios] : found->second.entry)
            out.byEntry[mobEntry] = ScaleOf(ratios, out.trash);
        return out;
    }

    // The fight is over: the health of what it killed over its length (corpses still in the world), kept as the
    // tank's measured group damage. Under pullCheckLock.
    void NoteMeasuredDps(PlayerbotAI* botAI, Player* tank, PullRecord const& record, uint32 now)
    {
        uint32 const fightMs = record.startMs ? getMSTimeDiff(record.startMs, now) : 0;
        if (!fightMs || record.deaths)
            return;

        float killed = 0.0f;
        for (size_t i = 0; i < record.came.size() && i < record.cameHealth.size(); ++i)
        {
            Unit* mob = botAI->GetUnit(record.came[i]);
            if (mob && !mob->IsAlive())
                killed += float(record.cameHealth[i]);
        }

        if (killed <= 0.0f)
            return;

        MeasuredDps& entry = measuredDps[tank->GetGUID().GetRawValue()];
        if (entry.instanceId != tank->GetInstanceId())
            entry = MeasuredDps();  // this dungeon's fights only
        entry.instanceId = tank->GetInstanceId();
        entry.fights.push_back({killed, float(fightMs) / float(IN_MILLISECONDS)});
    }

    // The tank's measured group damage in this instance: the dps it reached over all but the slowest tenth of its
    // fights' seconds (MEASURED_QUANTILE); `mean`: over all of them. 0 when none is known.
    float MeasuredGroupDps(Player* tank, bool mean = false)
    {
        std::lock_guard<std::mutex> guard(pullCheckLock);
        auto const found = measuredDps.find(tank->GetGUID().GetRawValue());
        if (found == measuredDps.end() || found->second.instanceId != tank->GetInstanceId())
            return 0.0f;

        if (mean)
        {
            float killed = 0.0f;
            float sec = 0.0f;
            for (MeasuredSample const& fight : found->second.fights)
            {
                killed += fight.value;
                sec += fight.weight;
            }
            return sec > 0.0f ? killed / sec : 0.0f;
        }
        return std::max(WeightedQuantile(found->second.fights, 1.0f - MEASURED_QUANTILE), 0.0f);
    }

    bool IsAnyoneFighting(Player* bot)
    {
        return bot->IsInCombat() || IsGroupFighting(bot);
    }

    uint32 CountDeadMembers(Player* bot)
    {
        uint32 dead = bot->IsAlive() ? 0 : 1;
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member != bot && member->GetMap() == bot->GetMap() && !member->IsAlive())
                    ++dead;
            }
        }

        return dead;
    }

    // [adaptive_cap] User 2026-10-25 (scholo-h 20261025-153805-scholo-h-5A-4, «тупят, излишне ждут»): the group
    // killed 26 and 16 mobs per pull without a death while the planner held the cap at 9 (x1.5 = 14) and stood 64 s
    // before a lone Adept. Every fight of the tank's group (planned or not) is recorded here: what came, the most
    // mobs (and ranged mobs) on the group at once, the deaths, the tank's health and the lowest healer mana at its
    // start and end, and the damage per second. [lead262] A clean fight proves the group takes that much here times
    // what the fight left: the healers' mana above their low line over the mana the fight took, and the tank's health
    // at the start over the health it dropped - the fewer of the two is the damage the group could carry (k); a pack's
    // damage grows with its mobs times their kill time, so its mobs grow by sqrt(k) and its cost by k. A fight that
    // used neither proves itself (k 1). A fight with a death caps the proof one mob under its peak. The proof is
    // scaled by the group's damage now against that fight's (sqrt of the ratio, never above 1). Per tank and
    // instance, under pullCheckLock.
    constexpr size_t PROVEN_MAX_TRACKED = 96;  // mobs one fight record keeps
    // [adaptive_cap_b29] Build-29 wipe (slabs-h 5A, pull 10 at 681 s, Rtpalba): "[adaptive cap] 14 mobs (14.9
    // weighted, cost 5.63) within the proven 21.7 weighted / 17.2 mobs: taken" - 9 attackers at +2 s, 15 at +10 s,
    // the healer died to Cabal Acolyte melee at +5 s, then all. The proof came from fights whose mobs arrived one by
    // one (a los pull, the Hellmaw event), counted by their total, and it overrode the cost model. Now:
    // - only an ordinary planned trash pull (plan "engage" or "pull back", no encounter in progress, no boss) proves
    //   anything, and it proves its PEAK: the most mobs on the group at once;
    // - the proof never overrides the cost: a pack within it is taken only at its measured cost cap (k);
    // - [lead262] a pack with more mobs keeping their range than a proving fight had on the group at once keeps the
    //   planner's own los / split plan whatever the proof (Cabal Shadow Priests / Acolytes / Deathsworn).

    struct FightTrack
    {
        uint32 startMs = 0;
        uint32 lastMs = 0;
        GuidVector came;
        float health = 0.0f;  // the maximum health of what came
        uint32 deadAtStart = 0;
        uint32 deaths = 0;  // most members dead at once, over those dead at the start
        float lowestTankHealth = 100.0f;
        float tankStartHealth = 100.0f;  // [lead262]
        float manaStart = -1.0f;         // [lead262] the lowest healer mana when it began (-1: no healer)
        bool boss = false;
        bool encounter = false;  // [adaptive_cap_b29] an encounter of the instance ran during it
        uint32 peakMobs = 0;     // [adaptive_cap_b29] the most mobs on the group at once
        uint32 peakRanged = 0;   // [lead262] ... and of them keeping their range
    };
    std::unordered_map<uint64, FightTrack> fightTracks;  // by tank

    struct ProvenEntry
    {
        uint32 instanceId = 0;
        float mobs = 0.0f;      // the biggest clean fight's peak, its headroom applied
        float dps = 0.0f;       // that fight's health per second
        float deathCap = 0.0f;  // 0: no death yet
        uint32 cleanFights = 0;
        float cost = 0.0f;  // [lead262] the highest planned cost of a proving fight times its k
        uint32 ranged = 0;  // [lead262] the most ranged mobs a proving fight had on the group at once
    };
    std::unordered_map<uint64, ProvenEntry> provenStrength;  // by tank

    // [pull_estimate_vs_actual] the last finished planned pull per tank, for the test harness (PullEstimate::Take)
    std::unordered_map<uint64, PullEstimate::Result> pullEstimates;
    uint32 pullEstimateSeq = 0;
    std::unordered_map<uint64, uint32> lastPlannedMs;  // [pull_label] by tank: PullCheck::Planned

    // Under pullCheckLock. `fighting`: this sample saw the group fight; `attackers`: what fights it.
    void TrackFight(PlayerbotAI* botAI, Player* tank, bool fighting, GuidVector const& attackers, uint32 now)
    {
        if (!PlayerbotAI::IsTank(tank))
            return;

        uint64 const key = tank->GetGUID().GetRawValue();
        if (fighting)
        {
            FightTrack& track = fightTracks[key];
            if (!track.startMs)
            {
                track = FightTrack();
                track.startMs = now ? now : 1;
                track.deadAtStart = CountDeadMembers(tank);
                track.tankStartHealth = tank->GetHealthPct();
                track.manaStart = LowestHealerMana(tank);
            }

            track.lastMs = now;
            if (InstanceScript* script = tank->GetInstanceScript(); script && script->IsEncounterInProgress())
                track.encounter = true;
            // [adaptive_cap_b29] the mobs on the group now (alive): the fight's peak
            uint32 nowMobs = 0;
            uint32 nowRanged = 0;
            for (ObjectGuid const& guid : attackers)
            {
                Unit* mob = botAI->GetUnit(guid);
                if (!mob || !mob->IsAlive())
                    continue;
                ++nowMobs;
                if (Creature* creature = mob->ToCreature(); creature && KeepsRange(creature))
                    ++nowRanged;
            }
            track.peakMobs = std::max(track.peakMobs, nowMobs);
            track.peakRanged = std::max(track.peakRanged, nowRanged);
            for (ObjectGuid const& guid : attackers)
            {
                if (track.came.size() >= PROVEN_MAX_TRACKED ||
                    std::find(track.came.begin(), track.came.end(), guid) != track.came.end())
                    continue;

                Unit* mob = botAI->GetUnit(guid);
                if (!mob)
                    continue;

                track.came.push_back(guid);
                track.health += float(mob->GetMaxHealth());
                if (IsBossMob(mob))
                    track.boss = true;
            }

            uint32 const dead = CountDeadMembers(tank);
            track.deaths = std::max(track.deaths, dead > track.deadAtStart ? dead - track.deadAtStart : 0u);
            track.lowestTankHealth = std::min(track.lowestTankHealth, tank->GetHealthPct());
            return;
        }

        auto const found = fightTracks.find(key);
        if (found == fightTracks.end())
            return;

        FightTrack const track = found->second;
        fightTracks.erase(found);
        uint32 const fightMs = track.lastMs && track.startMs ? getMSTimeDiff(track.startMs, track.lastMs) : 0;
        if (!fightMs || track.boss || track.came.empty())
            return;

        // [adaptive_cap_b29] what may prove: the tank's own planned pull
        auto const planned = pullChecks.find(key);
        bool const plannedPull = planned != pullChecks.end() && planned->second.startMs &&
                                 (planned->second.kind.rfind("engage", 0) == 0 ||
                                  planned->second.kind.rfind("pull back", 0) == 0);
        std::string notProving;
        if (track.encounter)
            notProving = "an encounter was in progress";
        else if (!plannedPull)
            notProving = planned == pullChecks.end() || !planned->second.startMs
                             ? "no planned pull"
                             : "plan " + planned->second.kind.substr(0, planned->second.kind.find(" ("));

        // [lead262] what the fight left: the healers' mana and the tank's health against what it took of each
        float const mana = LowestHealerMana(tank);
        float k = -1.0f;
        if (track.manaStart >= 0.0f && mana >= 0.0f && track.manaStart > mana)
            k = std::max(track.manaStart - float(sPlayerbotAIConfig.LowMana), 0.0f) / (track.manaStart - mana);
        if (track.tankStartHealth > track.lowestTankHealth)
        {
            float const byHealth = track.tankStartHealth / (track.tankStartHealth - track.lowestTankHealth);
            k = k < 0.0f ? byHealth : std::min(k, byHealth);
        }
        if (k < 0.0f)
            k = 1.0f;  // it took neither: it proves itself
        float const headroom = std::sqrt(k);
        ProvenEntry& entry = provenStrength[key];
        if (entry.instanceId != tank->GetInstanceId())
        {
            entry = ProvenEntry();
            entry.instanceId = tank->GetInstanceId();
        }

        if (!track.deaths && notProving.empty())
        {
            ++entry.cleanFights;
            entry.cost = std::max(entry.cost, planned->second.cost * k);
            entry.ranged = std::max(entry.ranged, track.peakRanged);
            if (float(track.peakMobs) * headroom > entry.mobs)
            {
                entry.mobs = float(track.peakMobs) * headroom;
                entry.dps = track.health * float(IN_MILLISECONDS) / float(fightMs);
            }
        }
        // [b38_mgt] only a fight with a death caps it: a clean fight that proves nothing (no planned pull, a los pull,
        // staggered) set it too - mgt-n 20261026-050341-mgt-n-5E-14 (Rlpalam, 0 deaths all run) ended at "death cap
        // 0.8" after a clean 1-mob fight with "no planned pull", crushing the proven 2.2 to 0.8.
        else if (track.deaths)
        {
            float const cap = std::max(float(track.peakMobs) - 1.0f, 0.0f);  // [lead262] one mob under its peak
            entry.deathCap = entry.deathCap > 0.0f ? std::min(entry.deathCap, cap) : cap;
        }

        LOG_INFO("playerbots",
                 "[adaptive cap] {}: fight of {} mobs (peak {}, {} ranged) in {} s, deaths {}, tank {:.0f} -> low "
                 "{:.0f} %, healer mana {:.0f} -> {:.0f} %: k {:.2f}, headroom {:.2f}{}; proven {:.1f} mobs, cost "
                 "{:.2f}, {} ranged, over {} clean fights{}",
                 tank->GetName(), track.came.size(), track.peakMobs, track.peakRanged, fightMs / IN_MILLISECONDS,
                 track.deaths, track.tankStartHealth, track.lowestTankHealth, track.manaStart, mana, k, headroom,
                 notProving.empty() ? std::string() : " (proves nothing: " + notProving + ")", entry.mobs, entry.cost,
                 entry.ranged, entry.cleanFights,
                 entry.deathCap > 0.0f ? Acore::StringFormat(", death cap {:.1f}", entry.deathCap) : std::string());
    }

    struct ProvenCap
    {
        float mobs = 0.0f;  // 0: nothing proven
        float cost = 0.0f;  // [lead262] the cost the proving fights carried, times what they left (k)
        uint32 ranged = 0;  // [lead262] the most ranged mobs a proving fight had at once
    };

    // The tank's proven strength in this instance, scaled by its group's damage now against the proving fight's.
    ProvenCap ProvenFor(Player* tank)
    {
        ProvenEntry entry;
        {
            std::lock_guard<std::mutex> guard(pullCheckLock);
            auto const found = provenStrength.find(tank->GetGUID().GetRawValue());
            if (found == provenStrength.end() || found->second.instanceId != tank->GetInstanceId())
                return ProvenCap();
            entry = found->second;
        }

        ProvenCap cap;
        if (entry.mobs <= 0.0f || entry.cost <= 0.0f)
            return cap;

        float scale = 1.0f;  // [lead262] the mobs a group carries grow with the root of its damage (see above)
        float const current = MeasuredGroupDps(tank, true);
        if (current > 0.0f && entry.dps > 0.0f)
            scale = std::sqrt(std::min(current / entry.dps, 1.0f));

        cap.mobs = entry.mobs * scale;
        cap.cost = entry.cost * scale * scale;
        cap.ranged = entry.ranged;
        if (entry.deathCap > 0.0f && cap.mobs > entry.deathCap)
            cap.mobs = entry.deathCap;

        return cap;
    }

    // A pack no bigger than what the group proved it takes in this run, at no more than the cost it proved it
    // carries ([adaptive_cap_b29]: the proof lifts the size cap, never the cost model). Never a boss, never more
    // ranged mobs than a proving fight had (their los / split plan stays).
    bool ProvenFits(PackEstimate const& estimate, ProvenCap const& proven)
    {
        if (estimate.boss || proven.mobs <= 0.0f || proven.cost <= 0.0f || estimate.rangedMobs > proven.ranged)
            return false;
        return estimate.capMobs <= proven.mobs + 0.5f && estimate.cost <= proven.cost;
    }

    void LogPullCheck(Player* tank, PullRecord const& record, uint32 now)
    {
        uint32 const duration = record.startMs ? getMSTimeDiff(record.startMs, now) / IN_MILLISECONDS : 0;
        LOG_INFO("playerbots",
                 "[pull check] {}: planned {} mobs cost {:.2f} -> came {}, deaths {}, duration {}s, via {}; plan {}, "
                 "drop {:.2f}, spot {}{}",
                 tank->GetName(), record.mobs, record.cost, record.came.size(), record.deaths, duration,
                 record.method.empty() ? "body" : record.method, record.kind, record.drop, record.spot,
                 record.startMs ? "" : " (no fight)");
        // [pull_estimate_vs_actual] one line per planned pull: the estimate against what engaged (the harness copies
        // it into its JSONL through PullEstimate::Take)
        std::string const kind = record.kind.substr(0, record.kind.find(" ("));
        int32 const diff = int32(record.came.size()) - int32(record.mobs);
        LOG_INFO("playerbots",
                 "[pull_estimate_vs_actual] {}: estimated {} mobs (cost {:.2f}) -> engaged {} ({:+d}), deaths {}, "
                 "{} s, plan {}",
                 tank->GetName(), record.mobs, record.cost, record.came.size(), diff, record.deaths, duration, kind);
        PullEstimate::Result& result = pullEstimates[tank->GetGUID().GetRawValue()];
        if (!++pullEstimateSeq)
            ++pullEstimateSeq;
        result.id = pullEstimateSeq;
        result.target = record.target;
        result.kind = kind;
        result.reason = record.kind;
        result.estimated = record.mobs;
        result.cost = record.cost;
        result.engaged = uint32(record.came.size());
        result.deaths = record.deaths;
        result.durationSec = duration;
        result.fought = record.startMs != 0;
        result.via = record.method.empty() ? "body" : record.method;
    }
}

namespace PullCheck
{
    void Planned(Player* tank, ObjectGuid target, uint32 mobs, float cost, float drop, std::string const& kind,
                 std::string const& spot)
    {
        PullRecord record;
        record.target = target;
        record.spot = spot;
        record.mobs = mobs;
        record.cost = cost;
        record.drop = drop;
        record.kind = kind;
        record.plannedMs = getMSTime();
        // A Sap or a trap the group placed before the pull opens the fight.
        PrePullCc::Plan plan;
        if (PrePullCc::Get(tank, plan) && plan.tank == tank->GetGUID())
        {
            for (PrePullCc::Claim const& claim : plan.claims)
            {
                if (claim.ready && !claim.target.IsEmpty())
                {
                    record.method = "cc";
                    break;
                }
            }
        }

        std::lock_guard<std::mutex> guard(pullCheckLock);
        record.dropScale = DropScaleLocked(tank);  // [lead259] the ratio this drop was planned with
        lastPlannedMs[tank->GetGUID().GetRawValue()] = record.plannedMs ? record.plannedMs : 1;
        pullChecks[tank->GetGUID().GetRawValue()] = std::move(record);
    }

    void NoteSpell(Player* tank, std::string const& spell)
    {
        std::lock_guard<std::mutex> guard(pullCheckLock);
        auto const found = pullChecks.find(tank->GetGUID().GetRawValue());
        if (found == pullChecks.end() || found->second.startMs || !found->second.method.empty())
            return;

        found->second.method = spell.empty() ? "body" : "spell " + spell;  // empty: a mob came before the cast
    }

    void Sample(PlayerbotAI* botAI)
    {
        Player* bot = botAI ? botAI->GetBot() : nullptr;
        if (!bot)
            return;

        uint64 const key = bot->GetGUID().GetRawValue();
        uint32 const now = getMSTime();
        bool const fighting = IsAnyoneFighting(bot);
        GuidVector attackers;
        if (fighting)
        {
            attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
            // the tank's last verdict stays the group's next fight through this fight and the rest after it
            CarefulPlanVerdict::NoteCombat(bot);
        }

        std::lock_guard<std::mutex> guard(pullCheckLock);
        if (fighting)
            lastFights[key] = now ? now : 1;

        TrackFight(botAI, bot, fighting, attackers, now);  // [adaptive_cap]
        auto const found = pullChecks.find(key);
        if (found == pullChecks.end())
            return;

        PullRecord& record = found->second;
        if (fighting)
        {
            if (!record.startMs)
            {
                record.startMs = now;
                record.takenAtStart = DamageTaken::Total(bot->GetGUID());
                record.spellAtStart = DamageTaken::Spell(bot->GetGUID());
                record.tankStartPct = bot->GetHealthPct();  // [lead259]
                if (Group* group = bot->GetGroup())  // [lead258] the healers' healing and mana at the start
                    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                        if (Player* member = ref->GetSource(); member && member->IsAlive() &&
                                                               member->GetMap() == bot->GetMap() &&
                                                               PlayerbotAI::IsHeal(member))
                            record.healers.push_back({member->GetGUID(), HealingDone::Effective(member->GetGUID()),
                                                      float(member->GetPower(POWER_MANA))});
                if (record.method.empty())
                {
                    // Nothing cast first: a pet opened, or a mob came by itself.
                    Unit* opener = botAI->GetUnit(record.target);
                    Unit* victim = opener ? opener->GetVictim() : nullptr;
                    record.method = victim && (victim->IsPet() || victim->IsGuardian()) ? "pet" : "body";
                }
            }

            for (ObjectGuid const& guid : attackers)
            {
                if (record.came.size() < PULL_CHECK_MAX_MOBS &&
                    std::find(record.came.begin(), record.came.end(), guid) == record.came.end())
                {
                    Unit* mob = botAI->GetUnit(guid);
                    record.came.push_back(guid);
                    record.cameHealth.push_back(mob ? mob->GetMaxHealth() : 0);
                }
            }

            record.deaths = std::max(record.deaths, CountDeadMembers(bot));
            record.tankLowPct = std::min(record.tankLowPct, bot->IsAlive() ? bot->GetHealthPct() : 0.0f);
            return;
        }

        if (!record.startMs && getMSTimeDiff(record.plannedMs, now) < PULL_CHECK_TIMEOUT_MS)
            return;

        LogPullCheck(bot, record, now);
        NoteMeasuredDps(botAI, bot, record, now);
        NoteMeasuredTaken(botAI, bot, record, now);
        NoteMeasuredHealing(bot, record, now);
        if (record.startMs)  // [lead264] the group's measured seconds per mob
            RestState::NoteFightLength(bot, uint32(record.came.size()),
                                       float(getMSTimeDiff(record.startMs, now)) / float(IN_MILLISECONDS));
        if (!record.deaths || record.tankLowPct <= 0.0f)  // [lead259] a wipe by others says nothing of the drop
            NoteMeasuredDrop(botAI, bot, record);
        pullChecks.erase(found);
    }

    uint32 LastFightMs(Player* tank)
    {
        std::lock_guard<std::mutex> guard(pullCheckLock);
        auto const found = lastFights.find(tank->GetGUID().GetRawValue());
        return found == lastFights.end() ? 0 : found->second;
    }
}

// [b33_aoepull] The harness's required-blocker marks and the planner's issued aoe_pull_plans (AggressiveTargetValue.h).
namespace PullPlanning  // [lead31a] was anonymous: shared with the tank's execution
{
    constexpr uint32 AOE_PULL_REQUIRED_TTL_MS = 10 * IN_MILLISECONDS;
    constexpr uint32 AOE_PULL_PLAN_TTL_MS = 10 * IN_MILLISECONDS;
    struct AoeRequiredEntry
    {
        ObjectGuid mob;
        uint32 ms = 0;
    };
    struct AoePlanEntry
    {
        std::vector<ObjectGuid> pack;
        uint32 firstMs = 0;
        uint32 ms = 0;
    };
    std::mutex aoePullLock;
    std::unordered_map<uint64, AoeRequiredEntry> aoeRequired;  // by tank
    std::unordered_map<uint64, AoePlanEntry> aoePlans;         // by tank

    // Any rank of the spells whose first ranks are listed (a higher rank may replace the first in the spell book).
    bool KnowsAnyRank(Player* player, std::initializer_list<uint32> firstRanks)
    {
        for (uint32 const first : firstRanks)
        {
            uint32 spell = first;
            for (uint32 guard = 0; spell && guard < 20; ++guard, spell = sSpellMgr->GetNextSpellInChain(spell))
                if (player->HasSpell(spell))
                    return true;
        }
        return false;
    }

    // A tank that holds a stack: Consecration, Thunder Clap, Swipe (Bear), Death and Decay / Blood Boil, Hammer of
    // the Righteous.
    bool HasAoeThreat(Player* tank)
    {
        return KnowsAnyRank(tank, {26573, 6343, 779, 43265, 48721, 53595});
    }

    // A damage dealer that hits the stack: Arcane Explosion, Blizzard, Flamestrike, Cone of Cold; Rain of Fire,
    // Hellfire, Seed of Corruption; Volley, Explosive Trap; Chain Lightning, Magma Totem, Fire Nova; Hurricane, Swipe
    // (Cat); Mind Sear, Holy Nova; Fan of Knives, Blade Flurry; Whirlwind, Cleave, Bladestorm; Consecration, Divine
    // Storm; Death and Decay, Blood Boil, Howling Blast.
    bool HasAoeDamage(Player* member)
    {
        return KnowsAnyRank(member, {1449,  10,    2120,  120,   5740,  1949,  27243, 1510,  13813,
                                     421,   8190,  1535,  16914, 62078, 48045, 15237, 51723, 13877,
                                     1680,  845,   46924, 26573, 53385, 43265, 48721, 49184});
    }

    // [lead261] What a member's area spells deal to each mob of a stack per second, from its spell data: the best of
    // its known area spells (school damage and periodic area damage with its spell power, weapon damage from its own
    // weapon), each at the fastest the spell lets it come again (cast or channel, global cooldown, cooldown, a ground
    // effect's duration), with the spell's own target cap (0: every mob in it). Cached per member while its spells and
    // spell power stay the same (a lookup, no clock).
    struct AreaDps
    {
        float perTarget = 0.0f;
        uint32 maxTargets = 0;
        std::string spell;
    };

    AreaDps MemberAreaDps(Player* member, Unit* victim)
    {
        struct Cached
        {
            size_t spells = 0;
            int32 power = -1;
            AreaDps dps;
        };
        thread_local std::unordered_map<uint64, Cached> cache;
        Cached& cached = cache[member->GetGUID().GetRawValue()];
        int32 const power = member->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_ALL);
        if (cached.spells == member->GetSpellMap().size() && cached.power == power)
            return cached.dps;

        AreaDps best;
        float const weapon =
            (member->GetFloatValue(UNIT_FIELD_MINDAMAGE) + member->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2.0f;
        for (auto const& [spellId, known] : member->GetSpellMap())
        {
            if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || info->IsPassive() || info->IsPositive() || !info->IsAffectingArea())
                continue;

            float damage = 0.0f;
            float groundMs = 0.0f;
            for (SpellEffectInfo const& effect : info->GetEffects())
            {
                bool const area = effect.IsTargetingArea() || effect.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA;
                if (!area)
                    continue;
                int32 const base = std::max(effect.CalcValue(member), 0);
                switch (effect.Effect)
                {
                    case SPELL_EFFECT_SCHOOL_DAMAGE:
                        damage += float(member->SpellDamageBonusDone(victim, info, uint32(base), SPELL_DIRECT_DAMAGE,
                                                                     effect.EffectIndex));
                        break;
                    case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                        damage += weapon * float(base) / 100.0f;
                        break;
                    case SPELL_EFFECT_WEAPON_DAMAGE:
                    case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                    case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                        damage += weapon + float(base);
                        break;
                    case SPELL_EFFECT_PERSISTENT_AREA_AURA:
                    case SPELL_EFFECT_APPLY_AURA:
                        if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE && effect.Amplitude)
                        {
                            groundMs = float(std::max(info->GetMaxDuration(), 0));
                            damage += groundMs / float(effect.Amplitude) *
                                      float(member->SpellDamageBonusDone(victim, info, uint32(base), DOT,
                                                                         effect.EffectIndex));
                        }
                        break;
                    default:
                        break;
                }
            }
            if (damage <= 0.0f)
                continue;

            float const castMs =
                info->IsChanneled() ? float(std::max(info->GetDuration(), 0)) : float(info->CalcCastTime(member));
            float const everyMs = std::max({castMs, float(info->StartRecoveryTime), float(info->RecoveryTime),
                                            float(info->CategoryRecoveryTime), groundMs, 1.0f});
            float const perTarget = damage * float(IN_MILLISECONDS) / everyMs;
            if (perTarget > best.perTarget)
                best = {perTarget, info->MaxAffectedTargets, info->SpellName[0] ? info->SpellName[0] : "?"};
        }
        cached = {member->GetSpellMap().size(), power, best};
        return best;
    }
}

namespace AoePull
{
    void MarkRequired(ObjectGuid tank, ObjectGuid mob)
    {
        std::lock_guard<std::mutex> guard(aoePullLock);
        AoeRequiredEntry& entry = aoeRequired[tank.GetRawValue()];
        entry.mob = mob;
        entry.ms = getMSTime();
    }

    bool IsRequired(ObjectGuid tank, std::vector<ObjectGuid> const& pack)
    {
        std::lock_guard<std::mutex> guard(aoePullLock);
        auto const found = aoeRequired.find(tank.GetRawValue());
        if (found == aoeRequired.end() || getMSTimeDiff(found->second.ms, getMSTime()) > AOE_PULL_REQUIRED_TTL_MS)
            return false;
        return std::find(pack.begin(), pack.end(), found->second.mob) != pack.end();
    }

    void NotePlan(ObjectGuid tank, std::vector<ObjectGuid> const& pack)
    {
        std::lock_guard<std::mutex> guard(aoePullLock);
        uint32 const now = getMSTime();
        AoePlanEntry& entry = aoePlans[tank.GetRawValue()];
        // the same plan kept on (a mob of it in the last one, still fresh): its first issue stays
        bool const same = entry.ms && getMSTimeDiff(entry.ms, now) <= AOE_PULL_PLAN_TTL_MS &&
                          std::any_of(pack.begin(), pack.end(), [&entry](ObjectGuid const& guid)
                                      { return std::find(entry.pack.begin(), entry.pack.end(), guid) !=
                                               entry.pack.end(); });
        if (!same)
            entry.firstMs = now ? now : 1;
        entry.pack = pack;
        entry.ms = now ? now : 1;
    }

    bool Planned(ObjectGuid tank, ObjectGuid mob, uint32* sinceMs)
    {
        std::lock_guard<std::mutex> guard(aoePullLock);
        auto const found = aoePlans.find(tank.GetRawValue());
        if (found == aoePlans.end() || getMSTimeDiff(found->second.ms, getMSTime()) > AOE_PULL_PLAN_TTL_MS ||
            std::find(found->second.pack.begin(), found->second.pack.end(), mob) == found->second.pack.end())
            return false;
        if (sinceMs)
            *sinceMs = found->second.firstMs;
        return true;
    }
}

namespace PullEstimate
{
    bool Take(ObjectGuid tank, uint32 afterId, Result& out)
    {
        std::lock_guard<std::mutex> guard(pullCheckLock);
        auto const found = pullEstimates.find(tank.GetRawValue());
        if (found == pullEstimates.end() || !found->second.id || found->second.id == afterId)
            return false;

        out = found->second;
        return true;
    }

    uint32 LastPlannedMs(ObjectGuid tank)
    {
        std::lock_guard<std::mutex> guard(pullCheckLock);
        auto const found = lastPlannedMs.find(tank.GetRawValue());
        return found == lastPlannedMs.end() ? 0 : found->second;
    }

    bool Proven(Player* tank, float& cost, float& mobs)
    {
        ProvenCap const cap = ProvenFor(tank);
        cost = cap.cost;  // [lead262] the cost the proof carries (no weighted count any more)
        mobs = cap.mobs;
        return cap.mobs > 0.0f;
    }
}

// Temporary diagnostics (wave 12): why a tank out of combat does not pull. One line per bot per 10 s.
// [lead1] The tank's commit of a pull, posted to the leader's order registry (Leader::Orders, a no-op for a tank no
// harness party leads): a commit that went through opens / refreshes the tank's order, one that did not is a note.
// [lead7] Set by Execute: the decision it runs was issued by the leader (LEADER_PLANNER_IN_LEADER), so the commit is a
// report of the leader's decision, not one of the tank's own.
namespace PullPlanning  // [lead31a] was anonymous: shared with the tank's execution
{
    thread_local bool executingLeaderDecision = false;
}

bool PostCommit(Player* bot, Unit* target, bool ok, Leader::PullMethod method, std::string const& why,
                       uint32 mobs, float cost, bool single)
{
    if (!target)
        return ok;
    if (ok)
        Leader::Orders::Report(bot, target, Leader::OrderState::Issued, method, why, mobs, cost, single,
                               executingLeaderDecision);
    else
        Leader::Orders::Note(bot, target, "commit_failed", std::string(Leader::MethodName(method)) + ": " + why,
                             executingLeaderDecision);
    return ok;
}

void LogPullHold(Player* bot, char const* reason, Unit* target)
{
    if (PullPlanner::IsShadow())
        return;  // [lead7] no slot of the tank's own lines

    static std::mutex mutex;
    static std::unordered_map<uint64, uint32> last;
    uint32 const now = getMSTime();
    {
        std::lock_guard<std::mutex> lock(mutex);
        uint32& at = last[bot->GetGUID().GetRawValue()];
        if (at && getMSTimeDiff(at, now) < 10 * IN_MILLISECONDS)
            return;
        at = now;
    }
    LOG_INFO("playerbots", "[pull hold] {}: {} ({} at {:.1f} yd)", bot->GetName(), reason,
             target ? target->GetName() : std::string("no target"), target ? bot->GetDistance(target) : 0.0f);
}

// "[pull blocked] <tank>: <reason> (<target> at <yd>)": the planner has nothing it may pull around the target, not
// even the hard minimum. One line per bot per 10 s.
void LogPullBlocked(Player* bot, std::string const& reason, Unit* target)
{
    if (PullPlanner::IsShadow())
        return;  // [lead7] no slot of the tank's own lines

    static std::mutex mutex;
    static std::unordered_map<uint64, uint32> last;
    uint32 const now = getMSTime();
    {
        std::lock_guard<std::mutex> lock(mutex);
        uint32& at = last[bot->GetGUID().GetRawValue()];
        if (at && getMSTimeDiff(at, now) < 10 * IN_MILLISECONDS)
            return;
        at = now;
    }
    LOG_INFO("playerbots", "[pull blocked] {}: {} ({} at {:.1f} yd)", bot->GetName(), reason,
             target ? target->GetName() : std::string("no target"), target ? bot->GetDistance(target) : 0.0f);
}

bool AggressiveTargetAction::DecideLook()
{
    _lookVerdict = PullPlanner::Verdict::None;  // [lead7] what this look's return means (FillDecision)
    _lookCareful = false;
    _lookTarget.Clear();
    _lookWhy.clear();
    _lookReadiness = false;
    _verdictRec = false;  // [lead5] this look's verdict, for the leader's memory
    _aoePlanNoted.clear();
    if (bot->IsInCombat())
    {
        _blockedSinceMs = 0;  // a fight ends the stand the hard cap's escalation counts
        return false;
    }
    // [lead3] a leader look (LeaderInputs bound): the leader picked the target and judges readiness
    PullPlanner::LeaderInputs const* const leaderInputs = _snap ? _snap->Leader() : nullptr;

    _planTarget.Clear();
    _approach.Clear();
    // Krik'thir's watcher room is pulled by its own tactic (AN strategy): a timed gauntlet the careful planner
    // must not split into single pulls.
    // [hleft-ak-jedoga-owns] ... and Jedoga's room while her initiates live (AK strategy, AKJedogaRoom).
    if (ANWatcherRoom::OwnsPull(botAI) || MagistersTerrace::SelinRoom::OwnsPull(botAI) ||
        AKJedogaRoom::OwnsPull(botAI))
        return false;

    if (PlayerbotAI::IsTank(bot))
    {
        Unit* target = PlanAggressiveTarget();  // [lead7] the look's snapshot
        if (!target)
        {
            // [lead5] LEADER_ONE_PICKER: the leader picked no mob, so there is none to plan: the leader's route walks
            // the group on, no approach walk of the planner's own (FindApproachTarget is the tank's own look only).
            if (leaderInputs && Leader::Leads(Leader::LEADER_ONE_PICKER))
            {
                _lookVerdict = PullPlanner::Verdict::Held;
                _lookWhy = "the leader picks no mob" + (leaderInputs->pickWhy.empty()
                                                            ? std::string()
                                                            : " (" + leaderInputs->pickWhy + ")");
                return false;
            }
            LogPullHold(bot, "no aggressive target", nullptr);
            // Nobody stands still while mobs are there: the tank walks toward the nearest one it cannot pick.
            _approach = FindApproachTarget();
            _lookVerdict = PullPlanner::Verdict::Held;
            _lookWhy = "no aggressive target";
            return !_approach.IsEmpty();
        }

        _lookTarget = target->GetGUID();
        _noTargetMs = 0;
        // [lead6] LEADER_ONE_READINESS: the leader's one readiness verdict (the harness's LeaderReadiness: the group
        // here and topped up, nobody buffing or sitting, PullReadiness::GroupPreparing) holds the look first; the
        // library's own boss / buff specifics below still apply to a ready group.
        if (leaderInputs && Leader::Leads(Leader::LEADER_ONE_READINESS) && !leaderInputs->ready &&
            !target->IsInCombat())
        {
            std::string const reason = "held for now: the leader: " + (leaderInputs->notReadyWhy.empty()
                                                                         ? std::string("the group is not ready")
                                                                         : leaderInputs->notReadyWhy);
            LogPullHold(bot, reason.c_str(), target);
            CarefulPlanVerdict::Fight fight;
            CarefulPlanVerdict::GetNextFight(bot->GetGUID(), fight);
            fight.guaranteeWaitMs = PULL_BUFF_WAIT_MS;
            fight.boss = fight.boss || IsBossMob(target);
            PublishVerdict(target->GetGUID(), false, 1, GuidVector{target->GetGUID()}, fight, false, reason);
            _lookVerdict = PullPlanner::Verdict::Held;
            _lookWhy = reason;
            _lookReadiness = true;
            return false;
        }
        // A pull of the tank's own waits for the group's buffs (user rule: everyone buffed).
        if (!target->IsInCombat() && bot->GetMap()->IsNonRaidDungeon() && WaitForBuffs())
        {
            // a verdict for the harness on this look too (a hold, re-checked soon)
            CarefulPlanVerdict::Fight fight;
            CarefulPlanVerdict::GetNextFight(bot->GetGUID(), fight);
            fight.guaranteeWaitMs = PULL_BUFF_WAIT_MS;
            PublishVerdict(target->GetGUID(), false, 1, GuidVector{target->GetGUID()}, fight, false,
                           "held for now: waits for " + _buffMissing);  // [lead5] [lead7]
            _lookVerdict = PullPlanner::Verdict::Held;
            _lookWhy = "waits for " + _buffMissing;
            _lookReadiness = true;
            return false;
        }

        // [pick_waits_ready] build 27 P0 (ac-n-5A-55, ub-n-5A-29 "aggressive target (4.00) -> Hungarfen",
        // strat-h-5A-10): the tank's own pick took an idle boss while the group (or the tank itself) was not ready,
        // then the walk to him was refused ([reach_idle_waits_ready]) and the tank stood in the combat engine. The
        // pick waits for the same readiness as the walk: nobody buffing or sitting below its line
        // (PullReadiness::GroupPreparing), the tank not resting or wanting to (its own drink / food line), and for a
        // boss the group ready for him (BossReadyGap). The tank's own rest line holds at most SELF_REST_HOLD_MS per
        // target (a tank with no water left would otherwise never pull); the harness's regen stall release covers
        // the others.
        if (!target->IsInCombat() && bot->GetMap()->IsNonRaidDungeon() && !GroupDamage::NoDamageRelease(bot))
        {
            constexpr uint32 SELF_REST_HOLD_MS = 60 * IN_MILLISECONDS;
            thread_local std::unordered_map<ObjectGuid, std::pair<ObjectGuid, uint32>> selfRestHolds;
            uint32 const now = getMSTime();
            std::string why;
            bool held = PullReadiness::GroupPreparing(bot, &why);
            // [lead221] b185 rfc-h 5E 1317-1324 s: "paladin tank at 73 % mana (needs 77 %)", then "the tank rests"
            // while it drank on to 100 % - its drink held the pull and the hold (lead50) its Rest. A led tank's rest
            // holds the pull while it is below its own lines (the tank mana line below holds the rest), or eats
            bool const restHolds = Leader::LedGroup(bot) ? RestState::BelowOwnLine(bot) || RestState::IsEating(bot)
                                                         : RestState::IsDrinking(bot) || RestState::IsEating(bot) ||
                                                               RestState::WantsDrink(bot) || RestState::WantsFood(bot);
            if (!held && restHolds)
            {
                std::pair<ObjectGuid, uint32>& hold = selfRestHolds[bot->GetGUID()];
                if (hold.first != target->GetGUID() || !hold.second)
                    hold = {target->GetGUID(), now ? now : 1};
                held = getMSTimeDiff(hold.second, now) < SELF_REST_HOLD_MS;
                // [lead39] b88 rfc-h 21.7 s "the tank rests (health 98 %, mana 100 %)": it ate its Well Fed food
                // (the harness's buff food, a 10 s eat) - named as what it is
                bool const buffFood = RestState::IsEating(bot) && !RestState::WantsFood(bot) &&
                                      !RestState::WantsDrink(bot);
                if (held)
                    why = Acore::StringFormat("the tank {} (health {:.0f} %, mana {:.0f} %)",
                                              buffFood ? "eats its buff food" : "rests", bot->GetHealthPct(),
                                              bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 100.0f);
            }
            else if (!held)
                selfRestHolds.erase(bot->GetGUID());

            if (!held && IsBossMob(target))
            {
                // the missing buff the tank has waited for long enough (WaitForBuffs' clock) holds no boss either
                bool const askBuffs = !_buffWaitMs || getMSTimeDiff(_buffWaitMs, now) < BOSS_BUFF_WAIT_MS;
                why = BossReadyGap(bot, askBuffs, IsEncounterBoss(target), target);
                held = !why.empty();
            }

            if (held)
            {
                std::string const reason = "held for now: " + why;
                LogPullHold(bot, reason.c_str(), target);
                // a verdict for the harness on this look too (a hold, re-checked soon), as WaitForBuffs gives it
                CarefulPlanVerdict::Fight fight;
                CarefulPlanVerdict::GetNextFight(bot->GetGUID(), fight);
                fight.guaranteeWaitMs = PULL_BUFF_WAIT_MS;
                fight.boss = fight.boss || IsBossMob(target);
                PublishVerdict(target->GetGUID(), false, 1, GuidVector{target->GetGUID()}, fight, false,
                               reason);  // [lead5] [lead7]
                _lookVerdict = PullPlanner::Verdict::Held;
                _lookWhy = reason;
                _lookReadiness = true;  // [lead108] b141 rfc-h 116.3 s: "the tank rests" refused the route target
                return false;
            }
        }

        // The nearest mob is held (or about to be held) by the group's crowd control: the tank pulls the mob it
        // planned instead, and the held one is the last of its pack (KillOrder: nobody fights a controlled mob).
        bool swapped = false;
        if (!target->IsInCombat() && (IsCrowdControlled(target) || PrePullCc::IsClaimed(bot, target->GetGUID())))
        {
            PrePullCc::Plan plan;
            Unit* planned = PrePullCc::Get(bot, plan) && plan.tank == bot->GetGUID() && !plan.pulledMs
                                ? botAI->GetUnit(plan.pullTarget)
                                : nullptr;
            if (planned && planned != target && planned->IsAlive() && !planned->IsInCombat() &&
                !IsCrowdControlled(planned))
            {
                target = planned;
                swapped = true;
                _lookTarget = target->GetGUID();
            }
        }

        PartyStrength const strength = PlanStrength();  // [lead7] the look's snapshot
        PullMode const mode = GetPullMode(bot, strength);
        if (mode != PullMode::Default)
        {
            _lookCareful = true;
            bool const planned = PlanCarefulPull(target, mode == PullMode::Measured, IsHeroicPace(bot, strength));
            // Published for the group's crowd control and the test harness, on every look (the cached plan too).
            // [lead7] not by a shadow look ([lead5] PublishVerdict records it for the leader either way)
            if (_planVerdict != PlanVerdictState::None)
            {
                CarefulPlanVerdict::Fight fight;
                fight.mobs = _planVerdictFightMobs;
                fight.cost = _planVerdictCost;
                fight.fightSec = _planVerdictSec;
                fight.boss = _planVerdictBoss;
                fight.realBoss = _planVerdictBoss && PackHasEncounterBoss(botAI, _planVerdictPack);
                // [lead78] a pull that brings a boss: the published fight is his fight's length (the members'
                // rest asks what the boss gate asks)
                if (_planVerdictBoss)
                    for (ObjectGuid const& guid : _planVerdictPack)
                        if (Unit* packBoss = botAI->GetUnit(guid); packBoss && IsBossMob(packBoss))
                            fight.fightSec = std::max(fight.fightSec, BossFightSec(bot, packBoss));
                fight.guaranteeWaitMs = _planVerdictGuaranteeWaitMs;
                PublishVerdict(planned ? _planTarget : target->GetGUID(), _planVerdict == PlanVerdictState::Accepted,
                               _planVerdictMobs, _planVerdictPack, fight, _planVerdictHopeless, _planVerdictWhy);
            }
            else if (!planned)
            {
                // Every look gets a verdict (strat-h build 19: a Ravaged Cadaver formation held "not topped up" and
                // "not ready" had none, the harness read it as unknown for 110 s and skipped 34 targets): a hold is a
                // refusal for now with its reason and a re-check, the next fight's estimate kept for the rest logic.
                constexpr uint32 HOLD_RECHECK_MS = 30 * IN_MILLISECONDS;
                CarefulPlanVerdict::Fight fight;
                CarefulPlanVerdict::GetNextFight(bot->GetGUID(), fight);
                // [rest_predict_real_pull] a plan was chosen and a readiness gate holds it: that pack is the next fight
                if (_planChosenValid)
                {
                    fight.mobs = _planChosenMobs;
                    fight.cost = _planChosenCost;
                    fight.fightSec = _planChosenSec;
                    fight.boss = _planChosenBoss;
                    fight.realBoss = _planChosenRealBoss;
                }
                fight.guaranteeWaitMs = HOLD_RECHECK_MS;
                // A boss's readiness holds it (BossReadyGap): the next fight is his, so the group tops up for a boss
                // (slabs-n build 20: Cabal Ritualists that bring Hellmaw held for "Rlpalam at 54 % mana", the rest
                // logic read the old trash fight and nobody drank, 261 s).
                constexpr uint32 BOSS_HOLD_FRESH_MS = 2 * IN_MILLISECONDS;
                if (!_bossHoldFor.IsEmpty() && getMSTimeDiff(_bossHoldSeenMs, getMSTime()) < BOSS_HOLD_FRESH_MS)
                {
                    fight.boss = true;
                    fight.realBoss = IsEncounterBoss(botAI->GetUnit(_bossHoldFor));  // [rest_topup_real_boss]
                    // [lead78] ... and the boss's fight length
                    fight.fightSec = std::max(fight.fightSec, BossFightSec(bot, botAI->GetUnit(_bossHoldFor)));
                }
                std::string const why = "held for now: " + (_planNotReadyWhy.empty()
                                                                 ? std::string("the group rests or gathers")
                                                                 : _planNotReadyWhy);
                PublishVerdict(target->GetGUID(), false, uint32(_planLookPack.size()), _planLookPack, fight, false,
                               why);  // [lead5]
            }

            if (!planned)
            {
                // [lead7] the look's verdict for the leader / the diff
                bool const refused = _planRefused || _planVerdict == PlanVerdictState::Refused;
                _lookVerdict = refused ? PullPlanner::Verdict::Refused : PullPlanner::Verdict::Held;
                _lookWhy = refused ? _planVerdictWhy
                                   : (_planNotReadyWhy.empty() ? std::string("the group rests or gathers")
                                                               : _planNotReadyWhy);
                // The stand the hard cap's escalation counts: every look the group stands topped up without a pull,
                // whichever hold or refusal it was (a refusal, an overlap or patrol wait, a lone mob over the cap).
                uint32 const standNow = getMSTime();
                if (IsGroupToppedUp(bot))
                {
                    if (!_blockedSinceMs || getMSTimeDiff(_blockedSeenMs, standNow) > CAREFUL_BLOCK_GAP_MS)
                        _blockedSinceMs = standNow;

                    _blockedSeenMs = standNow;
                    if (_planRefused)
                    {
                        NotePackBlocked(target->GetGUID(), standNow);
                        for (ObjectGuid const& guid : _planVerdictPack)
                            NotePackBlocked(guid, standNow);
                    }
                }

                std::string const hold =
                    _planRefused ? std::string("careful plan refused")
                                 : "careful plan not ready" +
                                       (_planNotReadyWhy.empty() ? std::string() : " (" + _planNotReadyWhy + ")");
                LogPullHold(bot, hold.c_str(), target);
                // A pack refused as it stands may be taken once the group's crowd control holds part of it.
                if (_planRefused)
                    HoldForPrePullCc(target, true, true);

                return false;
            }

            Unit* pull = botAI->GetUnit(_planTarget);
            // A lone-mob pull needs no crowd control, and holding for it re-planned the same pull for 5 minutes
            // (Blood Furnace wave 19: [pull one] x10, never cast).
            bool const held = (!_planSingle || _planCountsSap) && !_planPatrol &&
                              HoldForPrePullCc(pull ? pull : target, _planFallback, false,
                                               uint32(std::max<size_t>(_planVerdictPack.size(), 1)));
            if (held)
            {
                LogPullHold(bot, "held for pre-pull cc", pull ? pull : target);
                _lookVerdict = PullPlanner::Verdict::Held;  // [lead7]
                _lookWhy = "held for pre-pull cc";
            }
            return !held;
        }

        _planSide = false;
        _planMobs = 0;
        float danger = 0.0f;
        // A raid boss encounter (the boss itself, or a pack it stands with: Magtheridon's channelers, sweep batch2
        // 21:37 "pack not manageable (Hellfire Channeler at 0.0 yd)" while the boss strategy waited for the fight)
        // is the boss strategy's business, never a pack the danger estimate may refuse.
        bool raidEncounter = false;
        if (bot->GetMap()->IsRaid())
        {
            raidEncounter = IsBossMob(target);
            if (!raidEncounter)
            {
                constexpr float RAID_ENCOUNTER_BOSS_RANGE = 40.0f;
                GuidVector const encounterCandidates = PlanPossible(true);  // [lead7]
                for (ObjectGuid const& guid : encounterCandidates)
                {
                    Unit* unit = botAI->GetUnit(guid);
                    if (unit && unit->IsAlive() && IsBossMob(unit) &&
                        unit->GetExactDist(target) <= RAID_ENCOUNTER_BOSS_RANGE)
                    {
                        raidEncounter = true;
                        break;
                    }
                }
            }
        }

        if (!raidEncounter && !IsPackManageable(botAI, bot, target, strength, danger))
        {
            if (!target->IsInCombat() && IsGroupReadyToPull(bot, danger, strength))
                HoldForPrePullCc(target, true, true);

            LogPullHold(bot, "pack not manageable", target);
            _lookVerdict = PullPlanner::Verdict::Refused;  // [lead7]
            _lookWhy = "pack not manageable";
            return false;
        }

        if (!IsGroupReadyToPull(bot, danger, strength))
        {
            LogPullHold(bot, "group not ready", target);
            _lookVerdict = PullPlanner::Verdict::Held;  // [lead7]
            _lookWhy = "group not ready";
            _lookReadiness = true;
            return false;
        }
        if (HoldForPrePullCc(target, false))
        {
            LogPullHold(bot, "held for pre-pull cc", target);
            _lookVerdict = PullPlanner::Verdict::Held;  // [lead7]
            _lookWhy = "held for pre-pull cc";
            return false;
        }

        if (swapped)
        {
            _planTarget = target->GetGUID();
            _planPullBack = false;
        }

        // Tempo mode charges in, unless the pack has casters enough to pull round a corner.
        uint32 const now = getMSTime();
        if (_losTempoFor != target->GetGUID() || getMSTimeDiff(_losTempoMs, now) >= LOS_PULL_TEMPO_CACHE_MS)
        {
            _losTempoFor = target->GetGUID();
            _losTempoMs = now;
            _losTempoResult = false;
            if (!target->IsInCombat() && !IsGroupFighting(bot))
            {
                GuidVector const possible = PlanPossible(false);  // [lead7]
                _losTempoResult = PlanLosPull(target, possible, [](std::vector<Unit*> const&) { return true; });
            }
        }

        _planLos = _losTempoResult;
        if (_planLos)
            _planTarget = target->GetGUID();
    }

    return true;
}

void AggressiveTargetAction::NotePackBlocked(ObjectGuid mob, uint32 now)
{
    if (_packBlocked.size() > PACK_BLOCKED_MAX)
    {
        for (auto itr = _packBlocked.begin(); itr != _packBlocked.end();)
            itr = getMSTimeDiff(itr->second.second, now) > PACK_BLOCKED_GAP_MS ? _packBlocked.erase(itr)
                                                                                 : std::next(itr);
    }

    auto& [since, seen] = _packBlocked[mob];
    if (!since || getMSTimeDiff(seen, now) > PACK_BLOCKED_GAP_MS)
        since = now;

    seen = now;
}

uint32 AggressiveTargetAction::PackBlockedMs(std::vector<ObjectGuid> const& mobs, uint32 now) const
{
    uint32 longest = 0;
    for (ObjectGuid const& mob : mobs)
    {
        auto const found = _packBlocked.find(mob);
        if (found != _packBlocked.end() && getMSTimeDiff(found->second.second, now) <= PACK_BLOCKED_GAP_MS)
            longest = std::max(longest, getMSTimeDiff(found->second.first, now));
    }

    return longest;
}

bool AggressiveTargetAction::WaitForBuffs()
{
    // [lead17b] the buff pass is the leader's (party.needBuff in its readiness)
    if (bool ready = false; PullReadiness::LeaderVerdict(bot, ready, nullptr))
        return false;
    uint32 const now = getMSTime();
    if (!_buffCheckMs || getMSTimeDiff(_buffCheckMs, now) >= PULL_BUFF_CHECK_MS)
    {
        _buffCheckMs = now;
        _buffMissing = MissingGroupBuff(bot);
        if (_buffMissing.empty())
            _buffWaitMs = 0;
        else if (!_buffWaitMs)
            _buffWaitMs = now;
    }

    if (_buffMissing.empty() || getMSTimeDiff(_buffWaitMs, now) >= PULL_BUFF_WAIT_MS ||
        GroupDamage::NoDamageRelease(bot))
        return false;

    LogPullGate(bot, "missing " + _buffMissing);
    return true;
}

ObjectGuid AggressiveTargetAction::FindApproachTarget()
{
    Map* map = bot->GetMap();
    uint32 const now = getMSTime();

    // The stall is counted while this is asked without a break: isUseful returns before it in combat, and the time
    // kept from before a fight made the walk start at once after it (no 20 s standing).
    {
        static std::mutex callMutex;
        static std::unordered_map<uint64, uint32> lastCall;
        std::lock_guard<std::mutex> lock(callMutex);
        uint32& last = lastCall[bot->GetGUID().GetRawValue()];
        if (last && getMSTimeDiff(last, now) > APPROACH_CALL_GAP_MS)
            _noTargetMs = 0;
        last = now;
    }

    // A tank on the move (the group's travel, its own walk) is not standing still: no stall. Stratholme 2026-10-24
    // 09:48: 20 s after the last pull, on the way to the next target, the walk toward the nearest mob replaced the
    // travel that stops at the aggro edge and took the group into a street pack the careful planner had refused.
    // [reach_idle_waits_ready] nor while a member still buffs or sits below its line
    if (!map || !map->IsNonRaidDungeon() || bot->IsInCombat() || bot->isMoving() || IsGroupFighting(bot) ||
        !IsGroupToppedUp(bot) || PullReadiness::GroupPreparing(bot))
    {
        _noTargetMs = 0;
        return ObjectGuid::Empty;
    }

    if (!_noTargetMs)
    {
        _noTargetMs = now;
        return ObjectGuid::Empty;
    }

    if (getMSTimeDiff(_noTargetMs, now) < APPROACH_STALL_MS)
        return ObjectGuid::Empty;

    std::vector<std::pair<float, Unit*>> candidates;
    for (ObjectGuid const& guid : PlanPossible(false))  // [lead7] the look's snapshot
    {
        Unit* mob = botAI->GetUnit(guid);
        if (!IsIdleEnemy(bot, mob) || IsCrowdControlled(mob) || EncounterOrder::IsLocked(mob) ||
            std::fabs(mob->GetPositionZ() - bot->GetPositionZ()) > APPROACH_MAX_HEIGHT)
            continue;

        float const distance = bot->GetExactDist(mob);
        if (distance <= APPROACH_DISTANCE + 1.0f)  // close already: refused for another reason than range
            continue;

        candidates.emplace_back(distance, mob);
    }

    if (candidates.empty())
        return ObjectGuid::Empty;

    std::sort(candidates.begin(), candidates.end(),
              [](auto const& left, auto const& right) { return left.first < right.first; });

    // A real tank walks up to the next pack only where no other pack sees it come: the walk (MoveTo toward the mob,
    // to APPROACH_DISTANCE of it, at most SpellDistance) stays outside every idle mob's aggro reach - aggro range or
    // attack distance, combat distance and a margin, the mob sees the point; the mobs out of the tank's sight too
    // (the pack behind a corner). The harness approach and the side pull use the same test.
    std::vector<std::pair<Creature*, float>> reaches;
    GuidVector const approachNoLos = PlanPossible(true);  // [lead7] the look's snapshot
    for (ObjectGuid const& guid : approachNoLos)
    {
        Creature* mob = botAI->GetCreature(guid);
        if (!mob || !IsIdleEnemy(bot, mob) || !mob->HasReactState(REACT_AGGRESSIVE))
            continue;

        // A mob whose margin the tank stands in already blocks only a walk that comes nearer to it.
        float const reach = MovementAction::IdleAggroReach(mob, bot, APPROACH_AGGRO_MARGIN);  // the core's
        reaches.emplace_back(mob, std::min(reach, bot->GetExactDist(mob) - 1.0f));
    }

    float const eye = bot->GetCollisionHeight();
    Player* const walker = bot;
    auto const enters = [&reaches, eye, walker](G3D::Vector3 const& point) -> Creature*
    {
        for (auto const& [mob, reach] : reaches)
            if (mob->GetExactDist(point.x, point.y, point.z) < reach &&
                MovementAction::PassesAggroZGate(mob, walker, point.z) &&
                mob->IsWithinLOS(point.x, point.y, point.z + eye))
                return mob;

        return nullptr;
    };

    uint32 paths = APPROACH_MAX_PATHS;
    std::vector<PatrolGroup> patrols;  // looked up for the first walk that passes the aggro test
    bool patrolsFound = false;
    for (auto const& [distance, mob] : candidates)
    {
        if (!paths)
            break;

        // Where MoveTo(mob, APPROACH_DISTANCE) walks to.
        float needToGo = bot->GetDistance(mob) - APPROACH_DISTANCE;
        if (needToGo <= 0.0f)
            continue;

        needToGo = std::min(needToGo, float(sPlayerbotAIConfig.SpellDistance));
        float const angle = bot->GetAngle(mob);
        float x = bot->GetPositionX() + std::cos(angle) * needToGo;
        float y = bot->GetPositionY() + std::sin(angle) * needToGo;
        float z = bot->GetPositionZ() + (mob->GetPositionZ() - bot->GetPositionZ()) * (needToGo / distance);
        bot->UpdateAllowedPositionZ(x, y, z);

        --paths;
        LeaderPath path(bot);  // [lead99]
        path.CalculatePath(x, y, z, false);
        if (path.GetPathType() & PATHFIND_NOPATH)
            continue;

        Creature* blocker = nullptr;
        Movement::PointsArray const& points = path.GetPath();
        for (size_t i = 1; i < points.size() && !blocker; ++i)
        {
            G3D::Vector3 const segment = points[i] - points[i - 1];
            float const length = segment.length();
            for (float along = 0.0f; along <= length && !blocker; along += APPROACH_SAMPLE_STEP)
                blocker = enters(points[i - 1] + (length > 0.0f ? segment * (along / length) : segment));
        }

        if (!blocker && !points.empty())
            blocker = enters(points.back());

        if (blocker)
        {
            LogPullHold(bot, "approach walk would enter an idle mob's aggro", blocker);
            continue;
        }

        // Nor where a patrol walks by while the group stands there (forecast over the walk and PATROL_GROUP_HORIZON).
        if (!points.empty() && map)
        {
            if (!patrolsFound)
            {
                patrols = FindPatrols(botAI, bot, approachNoLos);
                patrolsFound = true;
            }

            Position const end(points.back().x, points.back().y, points.back().z);
            float const walk = bot->GetExactDist(end) / std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
            size_t const samples = size_t((walk + PATROL_GROUP_HORIZON) / PATROL_SAMPLE_SEC) + 1;
            Unit* patrolBlocker = nullptr;
            for (PatrolGroup const& patrol : patrols)
            {
                std::vector<bool> const flags =
                    PatrolInReach(map, bot->GetPhaseMask(), patrol.track, patrol.reach, {end}, samples);
                if (FirstInReach(flags, samples) >= 0)
                {
                    patrolBlocker = patrol.nearest;
                    break;
                }
            }

            if (patrolBlocker)
            {
                LogPullHold(bot, "approach walk would meet a patrol", patrolBlocker);
                continue;
            }
        }

        LOG_INFO("playerbots", "[pull approach] {}: no pick in range for {} s, walks toward {} ({:.1f} yd)",
                 bot->GetName(), getMSTimeDiff(_noTargetMs, now) / IN_MILLISECONDS, mob->GetName(), distance);
        return mob->GetGUID();
    }

    return ObjectGuid::Empty;
}

namespace PullPlanning  // [lead31a] was anonymous: shared with the tank's execution
{
    bool LosPullBanned(Player* bot, Unit* target);  // [b35_lbrs2] the split round a corner (defined below)
}

bool AggressiveTargetAction::PlanCarefulPull(Unit* target, bool measured, bool rescaled)
{
    uint32 const now = getMSTime();
    if (_planFor == target->GetGUID() && getMSTimeDiff(_planMs, now) < CAREFUL_PLAN_CACHE_MS)
    {
        if (_planResult)
            _planTarget = _planCached;

        return _planResult;
    }

    // On the way to a side spot: the plan holds until the tank is there (it plans again from there).
    if (_planSide && _sideFor == target->GetGUID() && !target->IsInCombat() &&
        getMSTimeDiff(_sideMs, now) < SIDE_PULL_WALK_MS && bot->GetExactDist(_sideSpot) > SIDE_PULL_ARRIVED)
    {
        _planTarget = _sideFor;
        return true;
    }

    _planFor = target->GetGUID();
    _planMs = now;
    _planResult = false;
    _planCached.Clear();
    _planPullBack = false;
    _planLos = false;
    _planRefused = false;
    _planFallback = false;
    _planSingle = false;
    _planSide = false;
    _planPatrol = false;
    _planKillFor.Clear();
    _planFightFor.Clear();
    _planFightNote.clear();
    _planMobs = 0;
    _planReason.clear();
    _planNotReadyWhy.clear();
    _planVerdict = PlanVerdictState::None;
    _planVerdictMobs = 0;
    _planVerdictPack.clear();
    _planVerdictFightMobs = 0;
    _planVerdictCost = 0.0f;
    _planVerdictSec = 0.0f;
    _planVerdictBoss = false;
    _planCountsSap = false;
    _planGuaranteed = false;
    _planVerdictHopeless = false;
    _planVerdictGuaranteeWaitMs = 0;
    _planVerdictWhy.clear();
    _planLookPack.assign(1, target->GetGUID());
    _planChosenValid = false;

    // The dungeon bosses' own fights (BossDrawnIn), sampled on every look (at most once per CAREFUL_PLAN_CACHE_MS).
    Map* const planMap = bot->GetMap();
    GuidVector const bossWatch = planMap && planMap->IsDungeon() ? PlanPossible(true) : GuidVector();  // [lead7]
    SampleBossCycles(botAI, bot, bossWatch);

    // A mob already fighting the group is an add: take it at once.
    if (target->IsInCombat())
    {
        _planCached = target->GetGUID();
        _planTarget = _planCached;
        _planResult = true;
        return true;
    }

    // [b32_smpull] A pull the tank gave up for range again and again from where it stands (PullAction: the walk for
    // range never came) is no plan for a while: held with that reason, the harness takes another target.
    if (CarefulPlanVerdict::PullOutOfReach(bot->GetGUID(), target->GetGUID()))
    {
        _planNotReadyWhy = Acore::StringFormat("the pull of {} was given up for range three times from here ({:.1f} "
                                               "yd): out of reach for now", target->GetName(), bot->GetExactDist(target));
        return false;
    }

    // [b32_smpull] The boss's linked trash first (what players do): Commander Mograine's engage sends every idle mob
    // of his chapel at his victim (ClassicShared.h), so while one stands he is no pull of his own - the nearest of
    // them is, planned by the whole planner (its plan, its split, the lone mob), and this look's verdict covers him.
    // sm-h 20261025-221216-sm-h-5E-14 (3306-3748 s): "plan Scarlet Commander Mograine 6 mobs = 1 start + 4 scripted
    // call" -> "pulls one mob, Scarlet Commander Mograine", 48 "out of pull range (33.0 yd)" against his own keep-away
    // hold, skipped as "route end", Whitemane's door never opened. One that the tank's straight walk reaches without
    // entering his keep-away ring first; else the nearest (the harness walks round him: keep_away_detour).
    if (!_planBlockerDepth && target->GetEntry() == ScarletMonastery::NPC_COMMANDER_MOGRAINE &&
        ScarletMonastery::IdleMograineWithChapel(bot) == target)
    {
        std::vector<Unit*> chapel;
        ScarletMonastery::CollectMograineCall(target, chapel);
        Position const from = bot->GetPosition();
        Position const ring = target->GetPosition();
        // the closest the straight walk from the tank to `mob` comes to Mograine (2d)
        auto const passes = [&from, &ring](Unit* mob)
        {
            float const sx = mob->GetPositionX() - from.GetPositionX();
            float const sy = mob->GetPositionY() - from.GetPositionY();
            float const lengthSq = sx * sx + sy * sy;
            float t = lengthSq > 0.0f ? ((ring.GetPositionX() - from.GetPositionX()) * sx +
                                         (ring.GetPositionY() - from.GetPositionY()) * sy) /
                                            lengthSq
                                      : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            float const dx = from.GetPositionX() + sx * t - ring.GetPositionX();
            float const dy = from.GetPositionY() + sy * t - ring.GetPositionY();
            return std::sqrt(dx * dx + dy * dy);
        };
        Unit* first = nullptr;
        bool firstClear = false;
        for (Unit* mob : chapel)
        {
            if (!mob || IsCrowdControlled(mob) || PrePullCc::IsClaimed(bot, mob->GetGUID()) ||
                CarefulPlanVerdict::PullOutOfReach(bot->GetGUID(), mob->GetGUID()))
                continue;
            bool const clear = passes(mob) >= ScarletMonastery::MOGRAINE_KEEP_AWAY;
            if (!first || (clear && !firstClear) ||
                (clear == firstClear && bot->GetExactDist(mob) < bot->GetExactDist(first)))
            {
                first = mob;
                firstClear = clear;
            }
        }

        ObjectGuid const mograineGuid = target->GetGUID();
        static thread_local uint32 linkedLogMs = 0;  // per map thread
        bool const logNow = !linkedLogMs || getMSTimeDiff(linkedLogMs, now) > 5 * IN_MILLISECONDS;
        if (!first)
        {
            _planNotReadyWhy = Acore::StringFormat("{}'s chapel ({} idle mobs) is cleared before him, and none of "
                                                   "them can be pulled now", target->GetName(), chapel.size());
            if (logNow)
            {
                linkedLogMs = now ? now : 1;
                LOG_INFO("playerbots", "[b32_smpull] linked_trash_first: {}: {}", bot->GetName(), _planNotReadyWhy);
            }
            return false;
        }

        std::string const firstName = first->GetName();
        float const firstDistance = bot->GetExactDist(first);
        ++_planBlockerDepth;
        bool const planned = PlanCarefulPull(first, measured, rescaled);
        --_planBlockerDepth;
        // the look stays Mograine's: cached for him, its verdict covers him (GetRefusal by his guid)
        _planFor = mograineGuid;
        _planMs = now;
        if (std::find(_planVerdictPack.begin(), _planVerdictPack.end(), mograineGuid) == _planVerdictPack.end())
            _planVerdictPack.push_back(mograineGuid);
        if (std::find(_planLookPack.begin(), _planLookPack.end(), mograineGuid) == _planLookPack.end())
            _planLookPack.push_back(mograineGuid);
        if (planned)
            _planReason = Acore::StringFormat("Mograine's linked trash first: {}; ", firstName) + _planReason;
        else if (!_planVerdictWhy.empty())
            _planVerdictWhy = Acore::StringFormat("Mograine's linked trash first ({}): ", firstName) + _planVerdictWhy;
        if (logNow)
        {
            linkedLogMs = now ? now : 1;
            LOG_INFO("playerbots", "[b32_smpull] linked_trash_first: {}: {} waits for his chapel ({} idle): {} at "
                     "{:.1f} yd ({}) - {}", bot->GetName(), target->GetName(), chapel.size(), firstName,
                     firstDistance, firstClear ? "a walk clear of his ring" : "the walk nears his ring",
                     planned ? "planned (" + _planReason + ")"
                             : (_planVerdict == PlanVerdictState::Refused ? "refused for now (" + _planVerdictWhy + ")"
                                                                           : "held (" + _planNotReadyWhy + ")"));
        }
        return planned;
    }

    // One pack at a time: nothing new while someone still fights, nor with a paladin tank that cannot taunt.
    if (IsGroupFighting(bot) && !GroupDamage::NoDamageRelease(bot))
    {
        _planNotReadyWhy = "a group member still fights";
        return false;
    }

    // [lead74] user: no hard mana constant - the tank's computed line for its next fight (RestState::TankManaLine:
    // its measured net use x the fight's seconds + the reserve + the pick-ups' taunt cost), the same line its rest
    // uses; CAREFUL_TANK_MANA only without a known next fight
    {
        RestState::NextFight next;
        std::string terms;
        // no published plan: the pack this look is about (its mobs, the fight seconds from them), no fixed %
        if (!RestState::PredictNextFight(bot, next))
        {
            next.mobs = std::max<uint32>(1, uint32(_planLookPack.size()));
            // [lead182b] the members' own rest lines read the same pack (PredictNextFight) - no fixed rest line
            CarefulPlanVerdict::NoteLook(bot, next.mobs);
        }
        // [lead474] b207 rfc-h 5E 1050.9 s: the published plan was the pull that had just ended (1 mob pulled back,
        // cost 0), its line let the paladin tank at low mana commit "walks in; 5 mobs" - then it drank 25 s under the
        // open order (harness_hold "no walk", 15 s). The line is for the pull this look plans: at least its pack
        else if (next.mobs < _planLookPack.size())
        {
            next.mobs = uint32(_planLookPack.size());
            next.fightSec = 0.0f;  // read from the mobs (FightSecOf), not the ended pull's seconds
        }
        float const tankLine = bot->GetMaxPower(POWER_MANA) ? RestState::TankManaLine(bot, next, &terms) : 0.0f;
        if (bot->getClass() == CLASS_PALADIN && bot->GetPowerPct(POWER_MANA) < tankLine)
        {
            _planNotReadyWhy = Acore::StringFormat("paladin tank at {:.0f} % mana (needs {})",
                                                   bot->GetPowerPct(POWER_MANA), terms);
            return false;
        }
    }

    // Nor with a healer still drinking up from the last fight (wave 10 Stratholme: the next pack 24 s after a
    // 133 s chain, the healer healed nobody and the group died in 21 s).
    // [lead166] b179 rfc-h 5H 459-512 s: "healer mana 15 %, needs 50 %" (the fixed CarefulPullMinHealerMana) held
    // Oggleflint after the leader's Rest had stood the healer up at its computed line (15 %), and the hold named no
    // member, so no Rest came: 53 s of spirit regen. A led group: each healer's own computed line for this pull (a
    // boss: the boss gate's FightManaLine, else its rest line OwnManaLine), named "<healer> at N % mana" so the
    // leader's Rest (lead50) drinks it up to exactly that line
    if (Leader::LedGroup(bot))
    {
        Creature const* targetCreature = target->ToCreature();
        bool const bossPull = targetCreature && (targetCreature->IsDungeonBoss() || targetCreature->isWorldBoss());
        std::vector<Player*> healers{bot};
        if (Group* group = bot->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource(); member && member != bot &&
                                                       member->GetMapId() == bot->GetMapId() &&
                                                       !ToppedUpWatch::Stranded(bot, member))
                    healers.push_back(member);
        for (Player* healer : healers)
        {
            if (!healer->IsAlive() || !PlayerbotAI::IsHeal(healer) || healer->getPowerType() != POWER_MANA)
                continue;
            std::string terms;
            // [lead182] b180 rfc-h 5E: "Rtpriac at 58 % mana (needs 60 % (no next fight known))" - this gate runs
            // before the look publishes its fight, so OwnManaLine fell back to the fixed rest line. As the tank's
            // line above: no published fight, the pack this look is about (its mobs, the fight seconds from them)
            RestState::NextFight next;
            if (!bossPull && !RestState::PredictNextFight(healer, next))
                next.mobs = std::max<uint32>(1, uint32(_planLookPack.size()));
            float const nextSec = RestState::FightSecOf(healer, next);  // [lead264]
            float const line = bossPull
                                   ? RestState::FightManaLine(healer, NextPullSec(healer, target), 0.0f, &terms, true)
                                   : RestState::FightManaLine(healer, nextSec, next.cost, &terms);
            if (healer->GetPowerPct(POWER_MANA) < line)
            {
                _planNotReadyWhy = Acore::StringFormat("{} at {:.0f} % mana (needs {})", healer->GetName(),
                                                       healer->GetPowerPct(POWER_MANA), terms);
                return false;
            }
        }
    }
    else
    {
        float const minHealerMana = sPlayerbotAIConfig.CarefulPullMinHealerMana;
        float const healerMana = LowestHealerMana(bot);
        if (minHealerMana > 0.0f && healerMana >= 0.0f && healerMana < minHealerMana)
        {
            _planNotReadyWhy = Acore::StringFormat("healer mana {:.0f} %, needs {:.0f} %", healerMana, minHealerMana);
            return false;
        }
    }

    PartyStrength const strength = PlanStrength();  // [lead7] the look's snapshot
    GroupSupport const support = CountSupport(bot);
    TankKit const kit = PartyStrengthShared::GetTankKit(bot);
    // Measured: what comes is not limited to what the tank sees from where it stands (a room behind a door frame).
    GuidVector const possible = PlanPossible(measured);  // [lead7]
    // User rule (packs stand apart at spawn): every possible target is sampled where it stands (once a second at
    // most), so a mob that walks is told from one that stands, and a group that crosses a pack now is noted with
    // the plan instead of merged into it (split), unless a wait for one ran out a moment ago.
    if (_overlapGiveUpMs && getMSTimeDiff(_overlapGiveUpMs, now) >= OVERLAP_GIVE_UP_MS)
        _overlapGiveUpMs = 0;

    bool const split = !_overlapGiveUpMs;
    if (split)
    {
        for (ObjectGuid const& guid : possible)
        {
            if (Creature* creature = botAI->GetCreature(guid))
                SampleMotion(creature);
        }
    }

    PackWhyScope const packWhyScope;  // "[pack why]": records every CollectPack of this plan (log at info)
    SapClaimScope const sapClaimScope(botAI, bot);  // a claimed pre-pull Sap counts as applied (CollectPack)
    std::vector<PackOverlap> overlaps;  // what the last CollectPack found crossing its pack
    std::vector<PackOverlap>* overlapsOut = split ? &overlaps : nullptr;
    std::vector<PackOverlap> chosenOverlaps;
    // The fight's length and cost, in every careful mode: the tank's health carries part of the fight.
    // [lead265] the dungeon's measured spread once a fight is measured (MeasuredGroupDps), else the model
    float const fightsDps = MeasuredGroupDps(bot);
    float const groupDps =
        fightsDps > 0.0f ? fightsDps : std::max(PlanGroupDps(), GroupWeaponDps(bot));  // [lead7] [lead259]
    EstimateMode estimateMode;
    estimateMode.measured = measured;
    estimateMode.healHps = support.healHps;  // [lead259]
    estimateMode.healGapSec = support.healGapSec;
    estimateMode.dropScale = DropCalibration(bot);
    LandedRatios const landedRatios = LandedCalibration(bot);  // [lead209]
    estimateMode.landedScale = landedRatios.trash;
    estimateMode.landedByEntry = &landedRatios.byEntry;
    // [b44_packs] the group's AoE on a stack: a tank with AoE threat holds it, [lead261] and every member's area
    // spells (MemberAreaDps, the tank's own too; healers heal) hit each stacked mob
    uint32 weakAoeDealers = 0;
    bool const weakAoeTank = HasAoeThreat(bot);
    if (Group* group = bot->GetGroup(); group && weakAoeTank)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || (member != bot && !IsPresent(bot, member)) || PlayerbotAI::IsHeal(member))
                continue;
            AreaDps const area = MemberAreaDps(member, bot);
            if (area.perTarget <= 0.0f)
                continue;
            ++weakAoeDealers;
            estimateMode.aoeTargetDps += area.perTarget;
            if (area.maxTargets && (!estimateMode.aoeMaxTargets || area.maxTargets < estimateMode.aoeMaxTargets))
                estimateMode.aoeMaxTargets = area.maxTargets;
        }
    }
    std::vector<Unit*> pack;
    PackEstimate chosen;
    std::vector<Unit*> chosenPack;  // what the chosen plan expects to come (empty: not known, a boss pull)

    // Measured: never more mobs at once than this, the topped-up fallback included (a pack that cannot be split
    // below it is refused, and the route comes back to it).
    uint32 const maxMobs =
        measured && sPlayerbotAIConfig.CarefulPullMaxMobs
            ? std::max(CAREFUL_MIN_MAX_MOBS,
                       uint32(std::lround(float(sPlayerbotAIConfig.CarefulPullMaxMobs) *
                                          std::clamp(strength.PullScale(), CAREFUL_MAX_MOBS_SCALE_MIN,
                                                     CAREFUL_MAX_MOBS_SCALE_MAX))))
            : 0;
    // [adaptive_cap] what this group proved it takes in this run (ProvenFor): a pack within it fits whatever the
    // configured cap and the cost model say; the last resort and the guarantee scale with it too (lastResortMobs).
    ProvenCap const proven = ProvenFor(bot);
    // [b44_packs] A weak pack: it dies to the group's AoE faster than one by one ([lead261]), the healers keep
    // up with what the whole stack lands meanwhile (the measured peak term with every mob alive: the healers' best
    // rate, HealerThroughput), the tank's drop in 9 pulls of 10 (DropP90 of the stack) is within its most
    // (PackEstimate::maxDrop) and the healers have the mana (HealerPool). The AoE cost carries the same spell part.
    // Not with crowd control planned on the pack (the AoE breaks it) and never a boss.
    auto const weakAoeFits = [&](PackEstimate const& estimate) -> bool
    {
        // [lead261] an AoE pack: the AoE hits more than the focus and kills the stack faster than one by one
        if (estimate.boss || estimate.aoeSec <= 0.0f ||
            estimate.mobs - std::min(estimate.mobs, estimate.rangedMobs) < 2)
            return false;

        float const tankHealth = PlanTankHealth(bot);
        PackEstimate aoe = estimate;
        aoe.cost = estimate.aoeCost;
        aoe.spellCost = estimate.aoeSpellCost;
        aoe.fightSec = estimate.aoeSec;
        aoe.drop = estimate.aoeDrop;
        // [lead258] what the stack lands per second on average against the healers' best rate (HealerThroughput)
        float const peak = support.healHps > 0.0f ? AverageDtps(aoe, tankHealth) / support.healHps : 0.0f;
        float const drop = DropP90(aoe);
        float const pool = HealerPool(support);
        float const mana = pool > 0.0f ? TakenCost(aoe) / pool : 0.0f;  // [lead258] no healer: no healer mana
        char const* why = nullptr;
        if (estimate.aoeSec >= estimate.fightSec)
            why = "no faster than one by one";
        else if (peak > 1.0f)
            why = "the healers do not keep up";
        else if (drop > estimate.maxDrop)
            why = "the tank's drop";
        else if (mana > 1.0f)
            why = "healer mana";
        else if (PrePullCc::HasPlan(bot, target->GetGUID()))
            why = "crowd control planned";

        uint32 const now = getMSTime();
        static thread_local uint32 weakLogMs[2] = {0, 0};  // per map thread: weak, not weak
        uint32& logMs = weakLogMs[why ? 1 : 0];
        if (!logMs || getMSTimeDiff(logMs, now) > 10 * IN_MILLISECONDS)
        {
            logMs = now ? now : 1;
            LOG_INFO("playerbots",
                     "[b44_packs] {}: {} pack {} mobs ttk {:.1f}s vs {:.1f}s: {} (aoe {:.0f}/s per mob "
                     "over {} members, cap {}, landed {:.0f}/s, peak {:.2f}, drop {:.2f}, mana {:.2f}, cost {:.2f} vs "
                     "one by one {:.2f}, {} casters)",
                     bot->GetName(), why ? "not weak:" : "weak", estimate.mobs, estimate.aoeSec, estimate.fightSec,
                     why ? why : "aoe", estimateMode.aoeTargetDps, weakAoeDealers, estimateMode.aoeMaxTargets,
                     estimate.landedDps, peak, drop, mana, estimate.aoeCost, estimate.cost, estimate.rangedMobs);
        }

        return !why;
    };
    auto const provenFits = [&proven, &weakAoeFits, this](PackEstimate const& estimate)
    {
        // [b45_weakoff] off: strat-h 092306 wiped on a 10-mob "weak" pack (aoe estimate far above measured)
        if (false && weakAoeFits(estimate))  // [b44_packs]
            return true;
        if (!ProvenFits(estimate, proven))
            return false;
        uint32 const now = getMSTime();
        static thread_local uint32 provenLogMs = 0;  // per map thread
        if (!provenLogMs || getMSTimeDiff(provenLogMs, now) > 10 * IN_MILLISECONDS)
        {
            provenLogMs = now ? now : 1;
            LOG_INFO("playerbots",
                     "[adaptive cap] {}: {} mobs ({:.1f} counted, cost {:.2f}, {} ranged) within the "
                     "proven {:.1f} mobs, cost {:.2f}, {} ranged: taken",
                     bot->GetName(), estimate.mobs, estimate.capMobs, estimate.cost, estimate.rangedMobs, proven.mobs,
                     proven.cost, proven.ranged);
        }
        return true;
    };
    // Against the weighted count (CAREFUL_SWARM_CAP_WEIGHT); "[pull swarm]" when the weighting alone lets it in.
    auto const withinCap = [maxMobs, &provenFits, this](PackEstimate const& estimate)
    {
        if (!maxMobs || estimate.mobs <= maxMobs)
            return true;
        if (provenFits(estimate))
            return true;
        if (estimate.capMobs > float(maxMobs) + 0.01f)
            return false;
        uint32 const now = getMSTime();
        static thread_local uint32 swarmCapLogMs = 0;  // per map thread
        if (!swarmCapLogMs || getMSTimeDiff(swarmCapLogMs, now) > 30 * IN_MILLISECONDS)
        {
            swarmCapLogMs = now ? now : 1;
            LOG_INFO("playerbots", "[pull swarm] {}: swarm_cap_weight: {} mobs count {:.1f} against the cap {}",
                     bot->GetName(), estimate.mobs, estimate.capMobs, maxMobs);
        }
        return true;
    };
    // Over what the group takes (the levelling topped-up fallback), a plan is still no more than the tank's health
    // can carry; measured plans are held to the tank's and the healers' limits themselves (CarefulOverload).
    // `fallbackCost`: a fallback plan is being looked for, held to what the tank carries whatever the mode
    // ([lead263] TankCarries).
    bool fallbackCost = false;
    auto const affordable = [measured, &fallbackCost, &support](PackEstimate const& estimate, float limit)
    {
        if (fallbackCost && !estimate.boss && !TankCarries(estimate, support))
            return false;

        return limit <= 1.0f || measured || TankCarries(estimate, support);
    };

    // The smallest plan of a pack nothing fits: the last resort of a group that has waited long enough.
    struct BestPlan
    {
        ObjectGuid mob;
        bool pullBack = false;
        float overload = 0.0f;
        PackEstimate estimate;
        OverloadTerms terms;
        std::vector<Unit*> pack;
        std::vector<PackOverlap> overlaps;
        Position kill;  // a pull back: where it is fought
    } best;
    auto const consider = [&best, &pack, &overlaps](Unit* candidate, bool pullBack, PackEstimate const& estimate,
                                                    OverloadTerms const& terms, Position const& kill)
    {
        if (estimate.boss || (!best.mob.IsEmpty() && terms.Max() >= best.overload))
            return;

        best.mob = candidate->GetGUID();
        best.pullBack = pullBack;
        best.kill = kill;
        best.overload = terms.Max();
        best.estimate = estimate;
        best.terms = terms;
        best.pack = pack;
        best.overlaps = overlaps;
    };

    // The tank's walk from `from` to `to` (its navmesh path) wakes an idle mob that the last CollectPack neither
    // counted nor noted as a crossing group (the overlap hold waits for those).
    Creature* lastWoken = nullptr;  // the mob the last walkPulls found woken (the plan's reason)
    Creature* targetBlocker = nullptr;  // [plan_route_blocker] the first mob a walk for the target itself woke
    auto const walkPulls = [&](Unit* candidate, Position const& from, Position const& to) -> bool
    {
        std::vector<Unit*> known(pack);
        if (overlapsOut)
        {
            for (PackOverlap const& overlap : overlaps)
                known.push_back(overlap.outside);
        }

        bool walkable = false;
        Creature* const woken = WalkEntersAggro(botAI, bot, possible, from, to, known, CAREFUL_MAX_WALK_CHECK,
                                                CAREFUL_AGGRO_MARGIN, walkable);
        lastWoken = woken;
        if (woken && candidate == target && !targetBlocker)
            targetBlocker = woken;
        if (woken)
            LOG_DEBUG("playerbots", "[pull walk] {}: the walk to ({:.1f} {:.1f} {:.1f}) for {} wakes {} ({:.1f} yd "
                      "from the tank): not this way", bot->GetName(), to.GetPositionX(), to.GetPositionY(),
                      to.GetPositionZ(), candidate->GetName(), woken->GetName(), bot->GetExactDist(woken));

        return woken != nullptr;
    };

    // A pull back of `candidate` cast at `castSpot` is fought where the tank stands now (the pull position), or, for
    // a mob that calls or flees for help at low health, at a spot up to KILL_SPOT_RETREATS steps further back on the
    // line from it through the tank where fewer come (the fewest, nearest first; the walk back there wakes nobody
    // else). Its pack in `pack` (and `overlaps`), the spot in `kill`.
    uint32 killSearches = KILL_SPOT_SEARCHES;
    uint32 fightSearches = KILL_SPOT_SEARCHES;  // searches for a fight spot back from the movers (FindFightSpotBack)
    auto const fightSearch = [&fightSearches]() -> bool { return fightSearches && fightSearches-- > 0; };
    // [b32_pull] A caster or shooter of a pulled-back pack does not come to the kill spot: it stands where it was and
    // casts (KeepsRange), so the tank fights it there and whatever sits round its stand spot joins. scholo-n
    // 20261025-221216-scholo-n-5C-4 pull 8: "one (every plan too dangerous, one mob at range; packs: one 6 where it is
    // fought)" Risen Guard pulled back, the Neophytes / Acolytes cast from the hall, 3 -> 7 -> 12 -> 13 came and all
    // five died. Each such mob's own pack at its stand spot is added to the estimate.
    auto const addCasterStands = [&]()
    {
        if (pack.size() <= 1)
            return;

        std::vector<Unit*> const base(pack);
        std::vector<PackOverlap> const baseOverlaps(overlaps);
        // [b33_brs] brs-h 20261026-000434-brs-h-5D-9 / lbrs-n 20261026-000434-lbrs-n-5H-10 (the Firebrand hall by
        // Voone, -40,-426,-18.8, 20 Firebrands in 2-4 mob groups 10-15 yd apart): every way in counted 20-23 mobs,
        // every one at full weight, and the route stalled; the same hall went in 2-9 mob pulls (estimate = engaged)
        // before this merge (brs-h 20261025-163739-5A-7 pulls 43-57, lbrs-n 20261025-184229-5A-14 pulls 50-62). Two
        // faults: the stand looks ran for casters of any hop (CollectPack's own drawsFight takes only a mob that came
        // by itself, hop 0: "not on from caster to caster across a hall"), and each stand look's CollectPack
        // overwrote lastPackHops, so EstimatePack weighed every chain link of the pack in full (CHAIN_STEP_CAP_WEIGHT
        // lost). Now only a hop-0 caster's stand is looked at, what its stand adds is one chain link further (hop 1),
        // and the merged pack's hops are left for EstimatePack.
        bool const hopsKnown =
            !lastPackHops.empty() && base.front() && lastPackHops.front().first == base.front()->GetGUID();
        std::vector<std::pair<ObjectGuid, uint8>> mergedHops;
        mergedHops.reserve(base.size());
        for (Unit* mob : base)
        {
            uint8 hop = 0;
            if (hopsKnown && mob)
            {
                for (auto const& [guid, known] : lastPackHops)
                {
                    if (guid == mob->GetGUID())
                    {
                        hop = known;
                        break;
                    }
                }
            }
            mergedHops.emplace_back(mob ? mob->GetGUID() : ObjectGuid::Empty, hop);
        }
        std::vector<Unit*> merged(base);
        std::vector<Unit*> around;
        for (size_t index = 0; index < base.size(); ++index)
        {
            Unit* mob = base[index];
            Creature* creature = mob ? mob->ToCreature() : nullptr;
            if (!creature || !KeepsRange(creature) || mergedHops[index].second)
                continue;

            CollectPack(botAI, bot, mob, mob->GetPosition(), possible, around, FightHere(mob->GetPosition()), measured,
                        false, nullptr);
            for (Unit* joined : around)
                if (joined && std::find(merged.begin(), merged.end(), joined) == merged.end())
                {
                    merged.push_back(joined);
                    mergedHops.emplace_back(joined->GetGUID(), uint8(1));
                }
        }
        pack = merged;
        lastPackHops = mergedHops;  // [b33_brs] for EstimatePack on this pack
        if (overlapsOut)
            overlaps = baseOverlaps;
    };

    auto const collectPullBack = [&](Unit* candidate, Position const& castSpot, Position& kill)
    {
        kill = bot->GetPosition();
        CollectPack(botAI, bot, candidate, castSpot, possible, pack, FightAt(kill), measured, false, overlapsOut);
        Creature* creature = candidate->ToCreature();
        // [b47_plan] a kill spot that brings a neighbour pack (KILL_SPOT_DRIFT: the fight's spread reaches it) is
        // moved back too, as the scripted calls' and flights' spots are: a player pulls to a clear spot behind
        bool const killSpotBrings = [&]() -> bool
        {
            if (pack.size() <= 1 || !creature || !killSearches || kill.GetExactDist(castSpot) <= CAREFUL_AGGRO_MARGIN)
                return false;

            std::vector<Unit*> const atKill(pack);
            auto const atKillHops = lastPackHops;
            std::vector<PackOverlap> const atKillOverlaps(overlaps);
            std::vector<Unit*> atCast;
            CollectPack(botAI, bot, candidate, castSpot, possible, atCast, FightHere(castSpot), measured, false,
                        nullptr);
            pack = atKill;
            lastPackHops = atKillHops;
            if (overlapsOut)
                overlaps = atKillOverlaps;
            return atKill.size() > atCast.size();
        }();
        if (pack.size() <= 1 || !creature || !killSearches ||
            (GetScriptedHelp(creature).callRadius <= 0.0f && !FleesForHelp(creature) && !killSpotBrings))
        {
            addCasterStands();  // [b32_pull]
            return;
        }

        float const dx = bot->GetPositionX() - candidate->GetPositionX();
        float const dy = bot->GetPositionY() - candidate->GetPositionY();
        float const length = std::sqrt(dx * dx + dy * dy);
        if (length < 1.0f)
            return;

        --killSearches;
        std::vector<Unit*> fewest(pack);
        std::vector<PackOverlap> fewestOverlaps(overlaps);
        auto fewestHops = lastPackHops;  // [b33_brs] the hops of the pack kept, not of the last look
        Position fewestKill = kill;
        for (uint32 i = 1; i <= KILL_SPOT_RETREATS && fewest.size() > 1; ++i)
        {
            float const back = KILL_SPOT_RETREAT_STEP * float(i);
            float const x = bot->GetPositionX() + dx / length * back;
            float const y = bot->GetPositionY() + dy / length * back;
            float z = 0.0f;
            if (!GroundNear(bot, x, y, bot->GetPositionZ(), PULL_PATH_HEIGHT, z))
                break;  // the way back ends here

            Position const option(x, y, z);
            // the healer stays with the group: a kill spot it cannot see from there is no option
            if (!HealerSees(bot, option))
                continue;

            CollectPack(botAI, bot, candidate, castSpot, possible, pack, FightAt(option), measured, false, overlapsOut);
            if (pack.size() >= fewest.size())
                continue;

            bool walkable = false;
            float const straight = castSpot.GetExactDist(option);
            if (WalkEntersAggro(botAI, bot, possible, castSpot, option, pack, straight * 2.0f + KILL_SPOT_RETREAT_STEP,
                                CAREFUL_AGGRO_MARGIN, walkable) ||
                !walkable)
                continue;

            fewest = pack;
            fewestOverlaps = overlaps;
            fewestHops = lastPackHops;  // [b33_brs]
            fewestKill = option;
        }

        // [lead8] LEADER_FIGHT_SPOT_COUNT, a leader look: the spots the group already fought and cleared this run
        // (the leader's record, LeaderInputs::cleared) are fight spot candidates too, nearest to the tank first,
        // counted from there as every other option (the healer's sight, the walk back out of every idle reach).
        if (PullPlanner::LeaderInputs const* inputs =
                Leader::Leads(Leader::LEADER_FIGHT_SPOT_COUNT) && _snap ? _snap->Leader() : nullptr;
            inputs && fewest.size() > 1 && !inputs->cleared.empty())
        {
            constexpr float CLEARED_SPOT_MAX = KILL_SPOT_RETREAT_STEP * float(KILL_SPOT_RETREATS) + 5.0f;
            constexpr size_t CLEARED_SPOT_TRIES = 3;
            std::vector<Position> cleared;
            for (Position const& spot : inputs->cleared)
                if (bot->GetExactDist(spot) <= CLEARED_SPOT_MAX &&
                    std::fabs(spot.GetPositionZ() - bot->GetPositionZ()) <= PULL_PATH_HEIGHT &&
                    spot.GetExactDist(fewestKill) > CAREFUL_AGGRO_MARGIN)
                    cleared.push_back(spot);
            std::sort(cleared.begin(), cleared.end(), [this](Position const& a, Position const& b)
                      { return bot->GetExactDist(a) < bot->GetExactDist(b); });
            if (cleared.size() > CLEARED_SPOT_TRIES)
                cleared.resize(CLEARED_SPOT_TRIES);
            for (Position const& option : cleared)
            {
                if (fewest.size() <= 1 || !HealerSees(bot, option))
                    continue;

                CollectPack(botAI, bot, candidate, castSpot, possible, pack, FightAt(option), measured, false,
                            overlapsOut);
                if (pack.size() >= fewest.size())
                    continue;

                bool walkable = false;
                float const straight = castSpot.GetExactDist(option);
                if (WalkEntersAggro(botAI, bot, possible, castSpot, option, pack,
                                    straight * 2.0f + KILL_SPOT_RETREAT_STEP, CAREFUL_AGGRO_MARGIN, walkable) ||
                    !walkable)
                    continue;

                LOG_DEBUG("playerbots", "[lead8] cleared_fight_spot: {}: {} pulled back to the cleared spot ({:.1f} "
                          "{:.1f} {:.1f}): {} mobs, was {}", bot->GetName(), candidate->GetName(),
                          option.GetPositionX(), option.GetPositionY(), option.GetPositionZ(), pack.size(),
                          fewest.size());
                fewest = pack;
                fewestOverlaps = overlaps;
                fewestHops = lastPackHops;
                fewestKill = option;
            }
        }

        pack = fewest;
        lastPackHops = fewestHops;  // [b33_brs]
        if (overlapsOut)
            overlaps = fewestOverlaps;

        kill = fewestKill;
        addCasterStands();  // [b32_pull]
    };

    // User, RFC 2026-10-24 20:04: «танк пошёл влево, потом на дальний пак, хотя справа стоял большой пак - в
    // результате заагрил два пака». An engage at a spot the healer does not see was planned as the 2 mobs there,
    // "the healer steps after"; the healer could not step (its walk woke the big pack), the tank came back into its
    // sight and the fight there drew the pack beside it: 9 came. Such an engage is fought where the healer sees it
    // from where it stands now: back along the line from the mob to the tank, FIGHT_SPOT_STEP at a time, up to
    // FIGHT_SPOT_STEPS, or the tank's own spot, on its floor, that no mover walks into over `seconds` and the walk
    // back to it wakes nobody outside the pack. Its pack counted with that spot as the fight's (CollectPack kill
    // spot: what reaches it, the walk back to it, the calls and flights from it, what its neighbours draw); the
    // fewest, nearest the mob first, in `pack` (and `overlaps`), the spot in `fight`. False (the pack at the mob's
    // spot kept): no such spot, or no search left for this plan.
    uint32 sightSearches = KILL_SPOT_SEARCHES;
    auto const collectSightFight = [&](Unit* candidate, float seconds, Position& fight) -> bool
    {
        if (!sightSearches)
            return false;

        --sightSearches;
        Position const from = candidate->GetPosition();
        std::vector<Unit*> const atSpot(pack);
        std::vector<PackOverlap> const atSpotOverlaps(overlaps);
        std::vector<Position> options;
        float const dx = bot->GetPositionX() - from.GetPositionX();
        float const dy = bot->GetPositionY() - from.GetPositionY();
        float const length = std::sqrt(dx * dx + dy * dy);
        for (uint32 i = 1; i <= FIGHT_SPOT_STEPS && FIGHT_SPOT_STEP * float(i) < length; ++i)
        {
            float const back = FIGHT_SPOT_STEP * float(i);
            float z = 0.0f;
            if (!GroundNear(bot, from.GetPositionX() + dx / length * back, from.GetPositionY() + dy / length * back,
                            from.GetPositionZ(), PULL_PATH_HEIGHT, z))
                break;  // the way back ends here

            options.emplace_back(from.GetPositionX() + dx / length * back,
                                 from.GetPositionY() + dy / length * back, z);
        }

        options.push_back(bot->GetPosition());
        bool found = false;
        std::vector<Unit*> fewest;
        std::vector<PackOverlap> fewestOverlaps;
        for (Position const& option : options)
        {
            FightIntruder intruder;
            if (!HealerSees(bot, option) || WalksIntoFight(botAI, bot, possible, candidate, option, seconds, intruder))
                continue;

            bool walkable = false;
            float const straight = from.GetExactDist(option);
            if (WalkEntersAggro(botAI, bot, possible, from, option, atSpot, straight * 2.0f + FIGHT_SPOT_STEP,
                                CAREFUL_AGGRO_MARGIN, walkable) ||
                !walkable)
                continue;

            CollectPack(botAI, bot, candidate, from, possible, pack, FightAt(option), measured, false, overlapsOut);
            if (found && pack.size() >= fewest.size())
                continue;

            found = true;
            fewest = pack;
            fewestOverlaps = overlaps;
            fight = option;
            if (pack.size() <= atSpot.size())
                break;  // no more than at the mob's spot: no spot further back brings fewer
        }

        pack = found ? fewest : atSpot;
        if (overlapsOut)
            overlaps = found ? fewestOverlaps : atSpotOverlaps;

        return found;
    };

    // Fought where it stands, or pulled at range back to where the tank stands now (or further back, for a mob that
    // calls for help), if the pack that comes is at most `limit` times what the group takes; `comfort`: and within
    // the size the group prefers. `losOnly`: round a corner or not at all. Neither plan's walk (to the mob, to the
    // cast spot) may wake a mob the pack does not count.
    // [lead206] the plan without a corner: fought where it stands, or pulled back (tryPlan below asks for it).
    auto const tryPlanStand = [&](Unit* candidate, bool allowBossPack, float limit, bool comfort) -> bool
    {
        OverloadTerms terms;
        CollectPack(botAI, bot, candidate, candidate->GetPosition(), possible, pack,
                    FightHere(candidate->GetPosition()), measured, false, overlapsOut);
        PackEstimate estimate = EstimatePack(pack, bot, groupDps, estimateMode);
        CarefulOverload(bot, estimate, strength, support, kit, measured, &terms);
        float overload = terms.Max(comfort);
        // every plan kind is held to the same gate, with the pack counted where its fight is
        auto const planFits = [&](PackEstimate const& planned, float plannedOverload)
        {
            return (allowBossPack || !planned.boss) &&
                   (((planned.boss || (withinCap(planned) && affordable(planned, limit))) &&
                     plannedOverload <= limit) ||
                    provenFits(planned));
        };
        bool fits = planFits(estimate, overload);
        bool better = !estimate.boss && (best.mob.IsEmpty() || terms.Max() < best.overload);
        // "[pull plan]": the pack of each plan kind looked at for this mob, counted where its fight is
        std::string packSizes = Acore::StringFormat("engage at its spot {}", estimate.mobs);
        // a boss is pulled as before; the walk is judged only for a plan that may be taken
        // The walk to a boss is judged too (mech-h build 19: the engage on Mechano-Lord Capacitus walked 32 yd past a
        // Bloodwarder pack, 12 mobs and the boss, wipe): the woken pack is taken first (the boss plan below).
        bool const walkInWakes = (estimate.boss ? IsBossMob(candidate) : (fits || better)) &&
                                 walkPulls(candidate, bot->GetPosition(), candidate->GetPosition());
        if (walkInWakes)
            packSizes += Acore::StringFormat(" + {} the walk in wakes",
                                             lastWoken ? lastWoken->GetName() : std::string("a mob"));

        // The mob's spot out of the healer's sight: the engage is fought where the healer sees it (collectSightFight),
        // and its pack is the one there; with no such spot it is no engage.
        Player* blind = nullptr;
        // [lead27] a led tank's healer sight is the leader's (it re-spots the healer after the tank holds the mobs):
        // never a reason to move the fight
        bool const leaderHealerSight = Leader::LEADER_HEALER_RESPOTS && Leader::Orders::IsTracked(bot->GetGUID());
        bool const engageSeen = leaderHealerSight || estimate.boss || walkInWakes || !(fits || better) ||
                                HealerSees(bot, candidate->GetPosition(), &blind);
        Position sightFight;
        float sightBack = 0.0f;
        bool const sightFound = !engageSeen && collectSightFight(candidate, FightWindowSec(estimate), sightFight);
        bool const sightNone = !engageSeen && !sightFound;
        std::string const blindName = blind ? blind->GetName() : std::string("the healer");
        if (sightFound)
        {
            sightBack = candidate->GetExactDist(sightFight);
            estimate = EstimatePack(pack, bot, groupDps, estimateMode);
            CarefulOverload(bot, estimate, strength, support, kit, measured, &terms);
            overload = terms.Max(comfort);
            fits = planFits(estimate, overload);
            better = !estimate.boss && (best.mob.IsEmpty() || terms.Max() < best.overload);
            packSizes += Acore::StringFormat(", engage in {}'s sight {:.0f} yd back {}", blindName, sightBack,
                                             estimate.mobs);
        }
        else if (sightNone)
            packSizes += Acore::StringFormat(", no engage spot in {}'s sight", blindName);

        // where the engage's pack was counted
        std::string const fightWhere =
            sightFound ? Acore::StringFormat("in {}'s sight {:.0f} yd back from its spot", blindName, sightBack)
                       : std::string("at its spot");

        if (!walkInWakes && !sightNone)
            consider(candidate, false, estimate, terms, candidate->GetPosition());

        // User, Stockade 2026-10-24: «зачем стрелять луком, если никого нет, он мог просто подойти и бить». The tank
        // engages (Charge, or walks in) unless there is a reason to pull back: the walk in wakes another mob, the
        // pack at the mob's spot is more than the group takes, or the healer cannot see that spot (a corner).
        bool const engageFits = fits && !walkInWakes && !sightNone;
        auto const takeEngage = [&](std::string const& note) -> bool
        {
            _planCached = candidate->GetGUID();
            _planPullBack = false;
            _planLos = false;
            _planKillFor.Clear();
            _planFightFor.Clear();
            _planFightNote.clear();
            chosen = estimate;
            chosenPack = pack;
            chosenOverlaps = overlaps;
            uint32 const chargeId = bot->getClass() == CLASS_WARRIOR ? AI_VALUE2(uint32, "spell id", "charge") : 0;
            float const distance = bot->GetExactDist(candidate);
            // out of Battle Stance the tank dances into it for the Charge (CastChargeAction::isPossible)
            bool const charge = chargeId && !bot->IsInCombat() && !bot->HasSpellCooldown(chargeId) &&
                                distance >= CHARGE_MIN_RANGE && distance <= CHARGE_MAX_RANGE &&
                                (botAI->HasAura("battle stance", bot) ||
                                 AI_VALUE2(uint32, "spell id", "battle stance"));
            _planReason = std::string(charge ? "charge" : "walks in") + "; " + note + "; packs: " + packSizes;
            return true;
        };

        // User, RFC 2026-10-24: a mob that walks (a wanderer, a patrol) and can come to the mob's spot while the
        // fight lasts moves the fight back (WalksIntoFight).
        float const engageSec =
            FightWindowSec(estimate) + bot->GetExactDist(candidate) / std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
        FightIntruder intruder;
        bool const engageCrossed = engageFits && engageSeen && !estimate.boss &&
                                   WalksIntoFight(botAI, bot, possible, candidate, candidate->GetPosition(),
                                                  engageSec, intruder);
        // Wave 44 (RFC pull 33): the tank charged an Enforcer on the lava edge and the whole group fought there. A mob
        // whose spot is within TERRAIN_HAZARD_FIGHT_RADIUS of lava, slime or a drop-off is pulled back to the tank
        // (a kill spot off the hazard), or engaged and dragged off it by the tank when no pull back is possible.
        Map* const hazardMap = bot->GetMap();
        TerrainHazardProbe hazard;
        if (engageFits && engageSeen && !estimate.boss && hazardMap)
            hazard = ProbeHazard(hazardMap, bot->GetPhaseMask(), candidate->GetPositionX(), candidate->GetPositionY(),
                                 candidate->GetPositionZ(), TERRAIN_HAZARD_FIGHT_RADIUS);

        bool const engageHazard = hazard.Found();

        // [lead26] the fight spot from the dps placement (user: «если лишние мобы сагрятся, то танк неправильно
        // выбирает позицию»): melee behind the mob (away from the tank), ranged at range on the group's side - an
        // unpulled mob outside the pack reaching one of them makes the engage spot wrong: pulled back instead
        Unit* crowd = nullptr;
        std::vector<Unit*> crowds;  // [lead51] every unpulled mob that reaches the placement
        if (Leader::LEADER_PLACEMENT_FIGHT_SPOT && engageFits && engageSeen && !engageHazard && !estimate.boss)
        {
            float const ex = bot->GetPositionX() - candidate->GetPositionX();
            float const ey = bot->GetPositionY() - candidate->GetPositionY();
            float const elen = std::max(0.1f, std::sqrt(ex * ex + ey * ey));
            std::vector<Position> places;
            places.emplace_back(candidate->GetPositionX() - ex / elen * 3.0f,
                                candidate->GetPositionY() - ey / elen * 3.0f, candidate->GetPositionZ());
            for (float turn : {0.0f, 0.7f, -0.7f})
            {
                float const angle = std::atan2(ey, ex) + turn;
                places.emplace_back(candidate->GetPositionX() + std::cos(angle) * PLACEMENT_RANGED_YD,
                                    candidate->GetPositionY() + std::sin(angle) * PLACEMENT_RANGED_YD,
                                    candidate->GetPositionZ());
            }
            for (ObjectGuid const& guid : possible)
            {
                Unit* mob = botAI->GetUnit(guid);
                if (!mob || mob == candidate || std::find(pack.begin(), pack.end(), mob) != pack.end() ||
                    !IsIdleEnemy(bot, mob))
                    continue;
                // [lead29] a walker (patrol / wanderer) is the walker rules' (taken along or pulled first)
                if (Creature* walker = mob->ToCreature(); walker && (walker->isMoving() ||
                                                                     walker->GetDefaultMovementType() !=
                                                                         IDLE_MOTION_TYPE))
                    continue;
                for (Position const& place : places)
                    if (ReachesSpot(bot, mob, place))
                    {
                        if (!crowd)
                            crowd = mob;
                        crowds.push_back(mob);
                        break;
                    }
            }
        }

        // [lead26] pull together (user: «они могут трёх этих мобов сразу начать АОЕшить»): a lone mob / wanderer
        // that would walk into the engage, with its own pack, taken in the same pull when the whole fits the group's
        // gate and clears faster than one by one (the planner's fight-time model plus a pull's overhead)
        if (Leader::LEADER_PULL_TOGETHER && engageFits && engageSeen && engageCrossed && !engageHazard && !crowd &&
            intruder.mob && !intruder.mob->IsInCombat() && !IsBossMob(intruder.mob) &&
            intruder.motion != MobMotion::Patrol)
        {
            std::vector<Unit*> alonePack;
            CollectPack(botAI, bot, intruder.mob, intruder.mob->GetPosition(), possible, alonePack,
                        FightHere(intruder.mob->GetPosition()), measured, false, nullptr);
            std::vector<Unit*> together = pack;
            for (Unit* mob : alonePack)
                if (std::find(together.begin(), together.end(), mob) == together.end())
                    together.push_back(mob);
            if (std::find(together.begin(), together.end(), intruder.mob) == together.end())
                together.push_back(intruder.mob);
            PackEstimate const togetherEstimate = EstimatePack(together, bot, groupDps, estimateMode);
            PackEstimate const aloneEstimate = EstimatePack(alonePack, bot, groupDps, estimateMode);
            OverloadTerms togetherTerms;
            CarefulOverload(bot, togetherEstimate, strength, support, kit, measured, &togetherTerms);
            float const togetherOverload = togetherTerms.Max(comfort);
            float const togetherSec = FightWindowSec(togetherEstimate);
            float const separateSec =
                FightWindowSec(estimate) + FightWindowSec(aloneEstimate) + PULL_TOGETHER_OVERHEAD_SEC;
            bool const safe = planFits(togetherEstimate, togetherOverload);
            std::string const compare = Acore::StringFormat(
                "plan_compare: combined {} mobs {:.0f} s (overload {:.2f}, {}) vs one by one {} + {} mobs {:.0f} s",
                togetherEstimate.mobs, togetherSec, togetherOverload, safe ? "fits" : "over the gate", estimate.mobs,
                aloneEstimate.mobs, separateSec);
            LOG_INFO("playerbots", "[lead26] {}: {} ({})", bot->GetName(), compare, FightIntruderNote(intruder));
            // [lead27] user: one lone mob joining is no danger - within the gate it is engaged in place (combined),
            // whichever is faster; pull back stays for real packs that would overload
            if (safe)
            {
                pack = together;
                estimate = togetherEstimate;
                terms = togetherTerms;
                return takeEngage(Acore::StringFormat("pull together: {} mobs at one meeting point ({}); {}",
                                                      togetherEstimate.mobs, FightIntruderNote(intruder), compare));
            }
        }

        // [lead60] user: «можно пулить несколько мобов - наш гир это позволяет»: a neighbouring idle pack (a mob
        // within PLACEMENT_RANGED_YD of this one, the ranged placement's own distance) is taken in the same pull when
        // the whole fits the capacity model (EstimatePack / CarefulOverload / planFits: the proven strength) and
        // clears faster than one by one (plan_compare)
        if (Leader::LEADER_COMBINE_PACKS && engageFits && engageSeen && !engageCrossed && !engageHazard && !crowd)
        {
            std::vector<Unit*> together = pack;
            std::string merged;
            for (ObjectGuid const& guid : possible)
            {
                Unit* other = botAI->GetUnit(guid);
                if (!other || other == candidate ||
                    std::find(together.begin(), together.end(), other) != together.end() ||
                    !IsIdleEnemy(bot, other) || IsBossMob(other) ||
                    other->GetExactDist(candidate) > PLACEMENT_RANGED_YD)
                    continue;
                std::vector<Unit*> otherPack;
                CollectPack(botAI, bot, other, other->GetPosition(), possible, otherPack,
                            FightHere(other->GetPosition()), measured, false, nullptr);
                otherPack.push_back(other);
                std::vector<Unit*> trial = together;
                for (Unit* mob : otherPack)
                    if (std::find(trial.begin(), trial.end(), mob) == trial.end())
                        trial.push_back(mob);
                PackEstimate const trialEstimate = EstimatePack(trial, bot, groupDps, estimateMode);
                OverloadTerms trialTerms;
                CarefulOverload(bot, trialEstimate, strength, support, kit, measured, &trialTerms);
                PackEstimate const otherEstimate = EstimatePack(otherPack, bot, groupDps, estimateMode);
                float const combinedSec = FightWindowSec(trialEstimate);
                float const separateSec = FightWindowSec(estimate) + FightWindowSec(otherEstimate) +
                                          PULL_TOGETHER_OVERHEAD_SEC;
                if (!planFits(trialEstimate, trialTerms.Max(comfort)) || combinedSec >= separateSec)
                    continue;
                together = trial;
                estimate = trialEstimate;
                terms = trialTerms;
                merged += (merged.empty() ? "" : ", ") + other->GetName();
                LOG_INFO("playerbots",
                         "[lead60] pull_combine {}: + {} ({} mobs, overload {:.2f}, {:.0f} s vs {:.0f} s)",
                         bot->GetName(), other->GetName(), trialEstimate.mobs, trialTerms.Max(comfort), combinedSec,
                         separateSec);
            }
            if (!merged.empty())
            {
                pack = together;
                return takeEngage(Acore::StringFormat("pull_combine: {} mobs with {} (fits, faster together)",
                                                      estimate.mobs, merged));
            }
        }

        if (engageFits && engageSeen && !engageCrossed && !engageHazard && !crowd)
            return takeEngage(Acore::StringFormat("{} mobs at its spot, nothing else near", estimate.mobs));

        std::string const pullBackWhy =
            !engageFits
                ? (walkInWakes && lastWoken
                       ? Acore::StringFormat("the walk in wakes {} ({:.0f} yd)", lastWoken->GetName(),
                                             bot->GetExactDist(lastWoken))
                       : (sightNone ? Acore::StringFormat("its spot is out of {}'s sight, no spot in its sight to "
                                                          "fight it", blindName)
                                    : Acore::StringFormat("{} mobs {}, over what the group takes ({:.2f})",
                                                          estimate.mobs, fightWhere, overload)))
                : (engageCrossed ? FightIntruderNote(intruder)
                   : crowd ? Acore::StringFormat("the dps placement at its spot is in the reach of {} (pulled back to "
                                                 "a clear spot)", crowd->GetName())
                                 : (engageHazard ? Acore::StringFormat("its spot is {:.0f} yd from {}", hazard.distance,
                                                                       HazardKindName(hazard.kind))
                                                 : Acore::StringFormat("its spot is out of {}'s sight", blindName)));
        // A fight round the corner: engaged after all when no pull back is possible, fought where the healer sees it
        // (collectSightFight: the tank walks it back there, FightSpot), its pack counted there. One a mover walks into
        // is engaged and walked back by the tank, to a spot behind it none reaches (FightSpot), if the pack there is
        // one the group takes too.
        auto const engageAfterAll = [&]() -> bool
        {
            if (!engageFits)
                return false;

            // by a hazard, no pull back: engaged where it stands, the tank drags the fight off it (FightNearHazard)
            if (engageHazard && !engageCrossed)
            {
                // [lead54] b103 rfc-h pull 8: "the tank drags the fight off it" for a led tank, whose own drag is off
                // (LEADER_TANK_OWN_PICKS_OFF): no executor - no such plan
                if (Leader::LEADER_TANK_OWN_PICKS_OFF && Leader::Orders::IsTracked(bot->GetGUID()))
                    return false;
                return takeEngage(pullBackWhy + ", the tank drags the fight off it");
            }

            // [lead51] user: «чем больше берёшь одновременно и не умираешь - тем лучше ... динамически» - b99 rfc-h
            // pull 8: "fought in its sight" of the Troggs reaching the dps placement, estimated 2, engaged 12. Engaged
            // here, those mobs and their packs come: they are counted and the whole is judged by the cost model -
            // taken deliberately when it fits, else no engage (another method / the next pick)
            if (!engageCrossed && crowd)
            {
                std::vector<Unit*> together = pack;
                for (Unit* seen : crowds)
                {
                    std::vector<Unit*> seenPack;
                    CollectPack(botAI, bot, seen, seen->GetPosition(), possible, seenPack,
                                FightHere(seen->GetPosition()), measured, false, nullptr);
                    seenPack.push_back(seen);
                    for (Unit* mob : seenPack)
                        if (std::find(together.begin(), together.end(), mob) == together.end())
                            together.push_back(mob);
                }
                PackEstimate const togetherEstimate = EstimatePack(together, bot, groupDps, estimateMode);
                OverloadTerms togetherTerms;
                CarefulOverload(bot, togetherEstimate, strength, support, kit, measured, &togetherTerms);
                if (!planFits(togetherEstimate, togetherTerms.Max(comfort)))
                    return false;
                pack = together;
                estimate = togetherEstimate;
                terms = togetherTerms;
            }
            if (!engageCrossed)
            {
                takeEngage(pullBackWhy +
                           (sightBack > CAREFUL_AGGRO_MARGIN
                                ? Acore::StringFormat(", no pull back possible: the tank walks the fight {:.0f} yd "
                                                      "back into its sight", sightBack)
                                : std::string(", no pull back possible: fought in its sight")));
                if (sightBack > CAREFUL_AGGRO_MARGIN)
                {
                    _planFightFor = candidate->GetGUID();
                    _planFight = sightFight;
                    _planFightBack = sightBack;
                    _planFightNote = pullBackWhy;
                }

                return true;
            }

            float const dx = bot->GetPositionX() - candidate->GetPositionX();
            float const dy = bot->GetPositionY() - candidate->GetPositionY();
            float const length = std::sqrt(dx * dx + dy * dy);
            Position back;
            bool walksBack = length >= 1.0f && fightSearch() &&
                             FindFightSpotBack(botAI, bot, possible, candidate, bot->GetPosition(), dx / length,
                                               dy / length, candidate->GetPosition(), pack, engageSec, 0, back);
            if (walksBack && back.GetExactDist(candidate->GetPosition()) > CAREFUL_AGGRO_MARGIN)
            {
                // the pack where the fight is walked back to, against the same gate
                std::vector<Unit*> const atSpot(pack);
                std::vector<PackOverlap> const atSpotOverlaps(overlaps);
                CollectPack(botAI, bot, candidate, candidate->GetPosition(), possible, pack, FightAt(back), measured,
                            false, overlapsOut);
                PackEstimate const backEstimate = EstimatePack(pack, bot, groupDps, estimateMode);
                OverloadTerms backTerms;
                CarefulOverload(bot, backEstimate, strength, support, kit, measured, &backTerms);
                packSizes += Acore::StringFormat(", engage walked {:.0f} yd back {}",
                                                 candidate->GetExactDist(back), backEstimate.mobs);
                if (planFits(backEstimate, backTerms.Max(comfort)))
                    estimate = backEstimate;
                else
                {
                    packSizes += " (over what the group takes)";
                    pack = atSpot;
                    overlaps = atSpotOverlaps;
                    walksBack = false;
                }
            }

            takeEngage(pullBackWhy + (walksBack ? ", no pull back possible: the tank walks the fight back"
                                                : ", no pull back possible, no spot back: fought where it stands"));
            if (walksBack)
            {
                _planFightFor = candidate->GetGUID();
                _planFight = back;
                _planFightBack = candidate->GetExactDist(back);
                _planFightNote = pullBackWhy;
            }

            return true;
        };

        // [b37_sv] or a ranged pull given up out of its sight three times (NotePullAbort): engaged, never pulled back
        if ((candidate->GetGUID() == _lastPullBack && getMSTimeDiff(_lastPullBackMs, now) < CAREFUL_PULL_RETRY_MS) ||
            CarefulPlanVerdict::EngageOnly(bot->GetGUID(), candidate->GetGUID()))
            return engageAfterAll();

        Position castSpot;
        if (!PullBackSpot(candidate, castSpot))
            return engageAfterAll();

        PackEstimate const engageEstimate = estimate;
        std::vector<Unit*> const engagePack = pack;
        std::vector<PackOverlap> const engageOverlaps = overlaps;
        Position kill;
        collectPullBack(candidate, castSpot, kill);
        estimate = EstimatePack(pack, bot, groupDps, estimateMode);
        // The kill spot against the movers too: one that walks into the fight there moves it further back, along the
        // line from the mob through the tank (the way the group came), to where none reaches.
        std::string fightNote;
        float fightBack = 0.0f;
        if (!estimate.boss)
        {
            FightIntruder atKill;
            float const killSec = FightWindowSec(estimate);
            if (WalksIntoFight(botAI, bot, possible, candidate, kill, killSec, atKill))
            {
                float const dx = kill.GetPositionX() - candidate->GetPositionX();
                float const dy = kill.GetPositionY() - candidate->GetPositionY();
                float const length = std::sqrt(dx * dx + dy * dy);
                Position back;
                if (length >= 1.0f && fightSearch() &&
                    FindFightSpotBack(botAI, bot, possible, candidate, kill, dx / length, dy / length, castSpot, pack,
                                      killSec, 1, back))
                {
                    fightNote = FightIntruderNote(atKill);
                    fightBack = kill.GetExactDist(back);
                    kill = back;
                    CollectPack(botAI, bot, candidate, castSpot, possible, pack, FightAt(kill), measured, false,
                                overlapsOut);
                    estimate = EstimatePack(pack, bot, groupDps, estimateMode);
                }
                else if (engageCrossed)
                {
                    // nowhere back is out of its reach: fought where the mob is, as before
                    estimate = engageEstimate;
                    pack = engagePack;
                    overlaps = engageOverlaps;
                    return takeEngage(pullBackWhy + ", no spot back out of its reach: fought where it stands");
                }
            }
        }

        CarefulOverload(bot, estimate, strength, support, kit, measured, &terms);
        overload = terms.Max(comfort);
        fits = planFits(estimate, overload);
        better = !estimate.boss && (best.mob.IsEmpty() || terms.Max() < best.overload);
        packSizes += Acore::StringFormat(", pull back {}", estimate.mobs);
        auto const restoreEngage = [&]() -> bool
        {
            estimate = engageEstimate;
            pack = engagePack;
            overlaps = engageOverlaps;
            return engageAfterAll();
        };

        // the walk to the cast spot (the tank walks its path in until it sees the mob in range)
        if (!estimate.boss && (fits || better) && walkPulls(candidate, bot->GetPosition(), castSpot))
            return restoreEngage();

        consider(candidate, true, estimate, terms, kill);
        // the fight stays where the healer sees it, off lava, slime and drop-offs
        if (!fits || (!estimate.boss && !HealerSees(bot, kill)))
            return restoreEngage();

        if (!estimate.boss && hazardMap &&
            NearHazard(hazardMap, bot->GetPhaseMask(), kill.GetPositionX(), kill.GetPositionY(), kill.GetPositionZ(),
                       TERRAIN_HAZARD_FIGHT_RADIUS))
        {
            packSizes += ", its kill spot by a hazard";
            return restoreEngage();
        }

        _planCached = candidate->GetGUID();
        _planPullBack = true;
        _planLos = false;
        _planKillFor = candidate->GetGUID();
        _planKill = kill;
        _planFightFor.Clear();
        _planFightNote.clear();
        if (engageCrossed || !fightNote.empty())
        {
            // "[fight spot]" at the pull: from the mob's spot (an engage a mover walks into), else from the kill spot
            _planFightFor = candidate->GetGUID();
            _planFightBack = engageCrossed ? candidate->GetExactDist(kill) : fightBack;
            _planFightNote = engageCrossed ? pullBackWhy : fightNote;
        }

        chosen = estimate;
        chosenPack = pack;
        chosenOverlaps = overlaps;
        _planReason = Acore::StringFormat("{}; {}{} mobs come back to the tank; packs: {}", pullBackWhy,
                                          fightNote.empty() || engageCrossed
                                              ? std::string()
                                              : Acore::StringFormat("{}, the fight {:.0f} yd further back; ",
                                                                    fightNote, fightBack),
                                          estimate.mobs, packSizes);
        return true;
    };

    // [lead206] the corner pull (PlanLosPull) when it is cheaper than the fight where the pack stands (CornerVsStand);
    // losOnly: round a corner or not at all.
    auto const tryPlan = [&](Unit* candidate, bool allowBossPack, float limit, bool comfort, bool losOnly) -> bool
    {
        // A mob the group's crowd control holds or has claimed is not opened on while the rest of its pack lives.
        if (candidate != target && (IsCrowdControlled(candidate) || PrePullCc::IsClaimed(bot, candidate->GetGUID())))
            return false;

        // Nor one whose encounter's predecessor has not started (EncounterOrder: it evades at once when engaged).
        if (EncounterOrder::IsLocked(candidate))
            return false;

        // [lead182] b180 rfc-h 5E 1305-1475 s: the los pull / lone-mob plans alternated between Voidwalker Minion 142
        // and 133 of the look's pack every 2 s, each aborted by the tank "out of sight on another floor" and capped
        // out of reach (b34_wc) - which only the look target was checked against. A candidate the tank gave up as out
        // of its reach from here is no pull mob either
        if (candidate != target && CarefulPlanVerdict::PullOutOfReach(bot->GetGUID(), candidate->GetGUID()))
            return false;

        // Several casters in the pack: round a corner first, if the pack that comes is one the group takes.
        PackEstimate losEstimate;
        std::vector<Unit*> losSeen;
        auto const acceptLos = [&](std::vector<Unit*> const& losPack) -> bool
        {
            losSeen = losPack;
            losEstimate = EstimatePack(losPack, bot, groupDps, estimateMode, true);
            OverloadTerms losTerms;
            CarefulOverload(bot, losEstimate, strength, support, kit, measured, &losTerms);
            return (withinCap(losEstimate) && affordable(losEstimate, limit) && losTerms.Max(comfort) <= limit) ||
                   provenFits(losEstimate);
        };

        if (!PlanLosPull(candidate, possible, acceptLos, measured))
            return !losOnly && tryPlanStand(candidate, allowBossPack, limit, comfort);

        // [lead206] b181 rfc-h 5E: 8 corner pulls, 351 yd in fights (5B: 5, 153 yd). The corner's cost against the
        // plan the planner takes without the corner: [lead215] an engage or a pull back (b184 rfc-h 5F: 9 of 13
        // corners were taken against a pull back without a comparison)
        Position const corner = _losSpot;
        Position const gather = _losGather;
        std::vector<Unit*> const packBefore = pack;
        std::vector<PackOverlap> const overlapsBefore = overlaps;
        auto const hopsBefore = lastPackHops;
        std::string cornerNote;
        if (!losOnly && tryPlanStand(candidate, allowBossPack, limit, comfort) && !_planLos &&
            _planCached == candidate->GetGUID())
        {
            Position castSpot = bot->GetPosition();
            PullBackSpot(candidate, castSpot);
            // [lead215] the healers' healing rate turns a loose caster's damage into seconds (CarefulOverload's)
            float const healRate = support.healHps;  // [lead258] the healers' best rate (HealerThroughput)
            Position const pullBackKill = _planKill;
            CornerCost const cost =
                CornerVsStand(botAI, bot, candidate, castSpot, corner, gather, losSeen, losEstimate.fightSec,
                              chosenPack, chosen.fightSec, groupDps, healRate, _planPullBack ? &pullBackKill : nullptr);
            // [lead212] b184 rfc-h 5F 784 s: "corner 23 s vs where it stands 23 s (threat loss 0 s)": the corner. At
            // the same cost the group stays where the route takes it
            bool const stand = cost.standSec <= cost.cornerSec;
            cornerNote = Acore::StringFormat(
                "corner {:.0f} s vs where it stands {:.0f} s{} (threat loss {:.0f} s, "
                "ranged tools hold {} of {}; the corner's threat loss {:.0f} s): {}",
                cost.cornerSec, cost.standSec, _planPullBack ? " pulled back" : "", cost.lossSec, cost.held,
                cost.ranged, cost.cornerLossSec, stand ? "no corner" : "the corner");
            LOG_DEBUG("playerbots", "[lead206] {}: {}: {}", bot->GetName(), candidate->GetName(), cornerNote);
            if (stand)
            {
                _planReason += "; " + cornerNote;
                return true;
            }
        }

        pack = packBefore;
        overlaps = overlapsBefore;
        lastPackHops = hopsBefore;
        _planReason = Acore::StringFormat(
            "{} casters / shooters pulled round the corner; packs: los pull {} at "
            "the tank's spot round the corner{}",
            CountRanged(losSeen), losSeen.size(), cornerNote.empty() ? std::string() : "; " + cornerNote);
        _planCached = candidate->GetGUID();
        _planPullBack = false;
        _planLos = true;
        _planKillFor.Clear();
        _planFightFor.Clear();
        _planFightNote.clear();
        chosen = losEstimate;
        chosenPack = losSeen;
        chosenOverlaps.clear();
        return true;
    };

    bool const boss = IsBossMob(target);
    // [b40_occ] A flier this far above or below a walking tank is no pull of its own (no plan, no lone mob): its cast
    // spot hangs over the drop and the tank walks the path to the mob instead, under it and into what stands there.
    // occ-h 20261026-054314-occ-h-5A-16 (737 s, [b39_occ] lone mob) and 20261026-060218-occ-h-5H-16 (805 s, a "pull
    // back" plan): an Azure Ring Guardian 15-16 yd over the north island, picked as the plan of its Construct pack;
    // the walk woke the Constructs and the Sorceress, both groups died. It stays in the pack's count.
    constexpr float CAREFUL_PLAN_FLIER_MAX_DZ = 8.0f;
    auto const highFlier = [this](Unit* mob)
    {
        return !bot->IsFlying() && !bot->GetVehicleBase() && mob->IsFlying() &&
               std::fabs(mob->GetPositionZ() - bot->GetPositionZ()) > CAREFUL_PLAN_FLIER_MAX_DZ;
    };
    std::vector<Unit*> adds;
    {
        CollectPack(botAI, bot, target, target->GetPosition(), possible, pack, FightHere(target->GetPosition()),
                    measured, false, overlapsOut);
        for (Unit* mob : pack)
        {
            if (mob != target && !IsBossMob(mob) && !highFlier(mob))  // [b40_occ]
                adds.push_back(mob);
        }
    }

    // the pack where it stands brings a boss (its own, or one on the way in): never the stand's guarantee
    // [banished_boss_no_pack_boss] a boss immune to players now (banished: Shadow Labyrinth's Ambassador Hellmaw,
    // SetImmuneToAll until his Cabal Ritualists die) is no fight their pull brings - they are his gate.
    bool const targetPackBoss = std::any_of(pack.begin(), pack.end(),
                                            [](Unit* mob) { return IsBossMob(mob) && !mob->IsImmuneToPC(); });
    for (Unit* mob : adds)
        _planLookPack.push_back(mob->GetGUID());

    // "[pack why]": the pack a refusal counts ("no manageable pull around <target> (N mobs").
    std::vector<Unit*> const targetPack = packWhyRecorder.on ? pack : std::vector<Unit*>();

    // The pack is too big where it stands: its edge, nearest to the tank first, each mob with what it brings.
    std::sort(adds.begin(), adds.end(),
              [this](Unit* a, Unit* b) { return bot->GetExactDist(a) < bot->GetExactDist(b); });

    auto const planAround = [&](float limit, bool comfort, bool losOnly) -> bool
    {
        // A boss room with adds that can be pulled on their own: the adds first.
        if ((!boss || adds.empty()) && tryPlan(target, boss, limit, comfort, losOnly))
            return true;

        size_t tried = 0;
        for (Unit* add : adds)
        {
            if (tried++ >= CAREFUL_MAX_ALTERNATIVES)
                break;

            if (tryPlan(add, false, limit, comfort, losOnly))
                return true;
        }

        return false;
    };

    bool found = false;
    bool fallback = false;
    bool patrolFirst = false;  // the plan takes a patrol before it comes by itself
    // ... and that patrol plan passed the ordinary gate (limit 1.0), not the topped-up fallback's: it is no fallback
    // for the fallback cost cap. Steam Vault wave 6 (Rlpalaf): "patrol Steam Surger comes to the group: pulls it
    // first, 9 mobs, cost 1.91" was dropped every time by "fallback plan costs 1.91, over 0.80" (the flag only skips
    // the rest check: the patrol comes anyway), and the group held for the same patrol for minutes.
    bool patrolFits = false;
    Map* map = bot->GetMap();

    // A patrol held for this long already is planned as usual (it walks in anyway).
    // [lead4] LEADER_ONE_PATROL_OWNER, a leader look: no hold clock. The patrol the leader decided to pull
    // (LeaderInputs::intercept) is never waited for; any other is waited for only while its forecast walk leaves the
    // group's spot (PatrolLeavesReach: a window), else it is planned as usual now (it walks in anyway).
    PullPlanner::LeaderInputs const* const patrolInputs =
        _snap && Leader::Leads(Leader::LEADER_ONE_PATROL_OWNER) ? _snap->Leader() : nullptr;
    auto const mayHoldFor = [&](Creature* walker, float reach) -> bool
    {
        if (patrolInputs)
        {
            if (walker->GetGUID() == patrolInputs->intercept)
                return false;
            bool known = false;
            bool const window = PullPlanner::PatrolLeavesReach(walker, bot->GetPosition(), reach, &known);
            if (known)
                return window;
        }
        if (_patrolHoldFor != walker->GetGUID() || getMSTimeDiff(_patrolHoldSeenMs, now) > PATROL_HOLD_GAP_MS)
        {
            _patrolHoldFor = walker->GetGUID();
            _patrolHoldMs = now;
        }

        _patrolHoldSeenMs = now;
        return getMSTimeDiff(_patrolHoldMs, now) < PATROL_MAX_HOLD_MS;
    };

    // Expert rule: no pull starts while a patrol walks into the group. One within its aggro reach of where the group
    // stands now, or about to be (PATROL_GROUP_HORIZON), is pulled first if the group takes it alone (it comes anyway,
    // and the tank meets it before it reaches the healer), else the tank holds until it is alone or has passed.
    std::vector<PatrolGroup> const patrols = FindPatrols(botAI, bot, possible);
    if (!patrols.empty() && map)
    {
        std::vector<Position> const groupPoints = GroupPoints(bot);
        size_t const groupSamples = size_t(PATROL_GROUP_HORIZON / PATROL_SAMPLE_SEC) + 1;
        PatrolGroup const* incoming = nullptr;
        int32 incomingAt = -1;
        for (PatrolGroup const& patrol : patrols)
        {
            std::vector<bool> const flags =
                PatrolInReach(map, bot->GetPhaseMask(), patrol.track, patrol.reach, groupPoints, groupSamples);
            int32 const first = FirstInReach(flags, groupSamples);
            if (first >= 0 && (!incoming || first < incomingAt))
            {
                incoming = &patrol;
                incomingAt = first;
            }
        }

        if (incoming)
        {
            Unit* member = incoming->nearest;
            float const eta = float(incomingAt) * PATROL_SAMPLE_SEC;
            float const distance = bot->GetExactDist(member);
            // it comes anyway: planned with what it passes on its way, as before (not split)
            std::vector<PackOverlap>* const splitOut = overlapsOut;
            overlapsOut = nullptr;
            // [b43_plan] The patrol's plan counts the idle pack it stands or walks beside (CollectPatrolSurroundings):
            // it is pulled now and engages within PATROL_PULL_LEAD_SEC, its casters stand and cast by the group. A
            // plan that no longer fits with that pack is not taken (the patrol is held for or met below, where the
            // [b39_ohb] hold and the [b39_strat] pull back read the same count).
            auto const withSurroundings = [&](float limit) -> bool
            {
                PatrolSurroundings const around =
                    CollectPatrolSurroundings(botAI, bot, possible, *incoming, PATROL_PULL_LEAD_SEC,
                                              bot->GetPosition(), chosenPack, measured);
                if (around.mobs.empty())
                    return true;

                // the plan's hops kept (EstimatePack's chain weight), what stands beside the patrol one link further
                auto const planHops = lastPackHops;
                bool const hopsKnown = !planHops.empty() && !chosenPack.empty() && chosenPack.front() &&
                                       planHops.front().first == chosenPack.front()->GetGUID();
                std::vector<Unit*> merged(chosenPack);
                std::vector<std::pair<ObjectGuid, uint8>> mergedHops;
                for (Unit* mob : chosenPack)
                {
                    uint8 hop = 0;
                    for (size_t i = 0; hopsKnown && mob && i < planHops.size(); ++i)
                    {
                        if (planHops[i].first == mob->GetGUID())
                        {
                            hop = planHops[i].second;
                            break;
                        }
                    }
                    mergedHops.emplace_back(mob ? mob->GetGUID() : ObjectGuid::Empty, hop);
                }
                for (Unit* mob : around.mobs)
                {
                    merged.push_back(mob);
                    mergedHops.emplace_back(mob->GetGUID(), uint8(1));
                }
                lastPackHops = mergedHops;
                PackEstimate const estimate = EstimatePack(merged, bot, groupDps, estimateMode, _planLos);
                OverloadTerms terms;
                CarefulOverload(bot, estimate, strength, support, kit, measured, &terms);
                bool const fits = (!estimate.boss && withinCap(estimate) && affordable(estimate, limit) &&
                                   terms.Max(false) <= limit) ||
                                  provenFits(estimate);
                Unit* const beside = around.nowAt ? around.nowAt : around.aheadAt;
                LogPatrolSurroundings(
                    bot, Acore::StringFormat("[b43_plan] [pull patrol] {}: patrol {} brings {} idle mobs from beside "
                                             "it ({}{}): {} -> {} mobs, cost {:.2f}: {}",
                                             bot->GetName(), member->GetName(), around.mobs.size(),
                                             beside ? beside->GetName() : std::string("?"),
                                             around.nowAt ? ", where it stands" : ", on its next walk",
                                             chosenPack.size(), estimate.mobs, estimate.cost,
                                             fits ? "taken with them" : "over, not taken"));
                if (!fits)
                {
                    _planCached.Clear();
                    _planLos = false;
                    _planPullBack = false;
                    _planKillFor.Clear();
                    _planFightFor.Clear();
                    _planFightNote.clear();
                    return false;
                }

                chosen = estimate;
                chosenPack = merged;
                _planReason += Acore::StringFormat("; +{} idle mobs beside the patrol", around.mobs.size());
                return true;
            };
            bool taken = tryPlan(member, false, 1.0f, false, false) && withSurroundings(1.0f);
            patrolFits = taken;
            if (!taken && !measured && IsGroupToppedUp(bot))
            {
                fallbackCost = true;
                taken = tryPlan(member, false, CAREFUL_FALLBACK_OVERLOAD, false, false) &&
                        withSurroundings(CAREFUL_FALLBACK_OVERLOAD);
                fallbackCost = false;
            }
            overlapsOut = splitOut;
            if (taken)
            {
                found = fallback = patrolFirst = true;
                _planReason = "a patrol comes to the group, taken first: " + _planReason;
                LogPatrol(bot, Acore::StringFormat(
                                   "[pull patrol] {}: patrol {} comes to the group ({:.1f} yd, eta {:.0f} s): pulls it "
                                   "first, {} mobs, cost {:.2f}",
                                   bot->GetName(), member->GetName(), distance, eta, chosen.mobs, chosen.cost));
            }
            else if (eta <= PATROL_IMMINENT_SEC && !IsBossMob(member) &&
                     std::none_of(incoming->members.begin(), incoming->members.end(),
                                  [](Unit* mob) { return IsBossMob(mob); }))
            {
                // [patrol_meet_imminent] The patrol reaches the group in a moment whatever the tank does: holding
                // lets it land on whoever it meets first. brd-n 2026-10-25 (20261025-115805-brd-n-5A-2, pull 36):
                // "patrol Anvilrage Officer approaches (eta 6 s): too many to pull now", the tank stood, the patrol
                // aggroed the healer first ("unplanned: Rldrubr", cause "patrol proximity aggro"), wipe; scholo-h
                // (115805-scholo-h-5A-34, pull 11) the same with a Risen Protector on the hunter, wipe. The tank
                // opens on it where it meets the group, so it holds the first threat.
                _planCached = member->GetGUID();
                // [b39_strat] A patrol that stands within the reach of an idle pack (it walks along a refused chain)
                // is not walked to: the tank opens on it from where it stands (pull back: a ranged opener, Death Grip,
                // the patrol comes to the group). strat-n 20261026-054314-strat-n-5C-2 pull 4 (451 s): the lone
                // Plague Ghoul "reaches the group (23.1 yd, eta 1 s)" while it stood 4 yd from the hopeless Ravaged
                // Cadaver chain (18 mobs); the engage walked the tank and the warrior 10 yd to it, the chain woke
                // (8 mobs in the kill order 10 s later, then more), three deaths 488-495 s.
                bool atIdlePack = false;
                float const imminentAssistRadius =
                    sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);  // [b39_ohb]
                for (ObjectGuid const& guid : possible)
                {
                    Unit* unit = botAI->GetUnit(guid);
                    if (!unit || !unit->IsAlive() || unit->IsInCombat() ||
                        std::find(incoming->members.begin(), incoming->members.end(), unit) !=
                            incoming->members.end())
                        continue;
                    if (InCoreAggroReach(unit, bot, member->GetPosition(), CAREFUL_AGGRO_MARGIN) ||
                        unit->GetExactDist(member) <= imminentAssistRadius)  // [b39_ohb] its call for help
                    {
                        atIdlePack = true;
                        break;
                    }
                }
                // [b43_plan] The same count as the patrol's plan above (CollectPatrolSurroundings), over the eta plus
                // PATROL_IMMINENT_SEC of its walk: a mob within the assistance radius of any member where it is now,
                // or of where its casters stand and cast by the group, is "at an idle pack" ([b39_ohb] hold); one
                // only along its next seconds of walk is "ahead" ([b39_strat] pull back); both are in the estimate.
                PatrolSurroundings const imminentAround =
                    CollectPatrolSurroundings(botAI, bot, possible, *incoming, eta + PATROL_IMMINENT_SEC,
                                              bot->GetPosition(), incoming->members, measured);
                if (imminentAround.nowAt)
                    atIdlePack = true;
                if (!imminentAround.mobs.empty())
                {
                    Unit* const beside = imminentAround.nowAt ? imminentAround.nowAt : imminentAround.aheadAt;
                    LogPatrolSurroundings(
                        bot, Acore::StringFormat("[b43_plan] [patrol_meet_imminent] {}: patrol {} brings {} idle mobs "
                                                 "from beside it ({}{})",
                                                 bot->GetName(), member->GetName(), imminentAround.mobs.size(),
                                                 beside ? beside->GetName() : std::string("?"),
                                                 imminentAround.nowAt ? ", where it stands" : ", on its next walk"));
                }
                // [b39_ohb] The patrol AT an idle pack is not opened on at all: any opener there (the walk in, or a
                // ranged one: the patrol's call for help reaches the pack) brings the pack, and the estimate counted
                // the patrol alone. ohb-h 20261026-054314-ohb-h-5D-15 pull 17 (667 s): "patrol Durnholde Tracking
                // Hound reaches the group (29.5 yd, eta 3 s), 2 mobs" while it walked past the Durnholde Rifleman /
                // Sentry / Warden pack below the ramp (its tryPlan with what it passes had failed); the bear's body
                // engage chased it 14 yd south (2120.5,78.7 -> 2119.8,64.5), a Rifleman was on the tank at 0 s, 6
                // attackers at 669 s, about 10 by 680 s: four of five dead 702-712 s. The tank waits while it walks
                // clear (mayHoldFor, at most PATROL_MAX_HOLD_MS, then the [b39_strat] pull back). A patrol clear of
                // the pack now whose next seconds of walk (the eta plus PATROL_IMMINENT_SEC) or whose walk-in from the
                // tank pass an idle pack is pulled back from here (no walk to it).
                if (atIdlePack && mayHoldFor(incoming->walker, incoming->reach))
                {
                    _planCached.Clear();
                    LogPatrol(bot, Acore::StringFormat("[b39_ohb] [pull hold] {}: patrol {} reaches the group ({:.1f} "
                                                       "yd, eta {:.0f} s) but stands at an idle pack: waits for it "
                                                       "to walk clear", bot->GetName(), member->GetName(), distance,
                                                       eta));
                    _planNotReadyWhy = Acore::StringFormat("patrol wait: {} at an idle pack (eta {:.0f} s)",
                                                           member->GetName(), eta);
                    return false;
                }
                bool aheadIdlePack = imminentAround.aheadAt != nullptr;  // [b43_plan]
                if (!atIdlePack && !aheadIdlePack)
                {
                    size_t const lookSamples =
                        std::min(incoming->track.size(), size_t((eta + PATROL_IMMINENT_SEC) / PATROL_SAMPLE_SEC) + 1);
                    for (ObjectGuid const& guid : possible)
                    {
                        Unit* unit = botAI->GetUnit(guid);
                        if (aheadIdlePack)
                            break;
                        if (!unit || !unit->IsAlive() || unit->IsInCombat() ||
                            std::find(incoming->members.begin(), incoming->members.end(), unit) !=
                                incoming->members.end())
                            continue;
                        for (size_t sample = 1; sample < lookSamples && !aheadIdlePack; ++sample)
                        {
                            G3D::Vector3 const& at = incoming->track[sample];
                            aheadIdlePack = InCoreAggroReach(unit, bot, at.x, at.y, at.z, CAREFUL_AGGRO_MARGIN);
                        }
                    }
                    bool walkable = false;
                    if (!aheadIdlePack &&
                        WalkEntersAggro(botAI, bot, possible, bot->GetPosition(), member->GetPosition(),
                                        incoming->members, CAREFUL_MAX_WALK_CHECK, CAREFUL_AGGRO_MARGIN, walkable))
                        aheadIdlePack = true;
                }
                _planPullBack = atIdlePack || aheadIdlePack;
                _planLos = false;
                _planKillFor.Clear();
                // [b43_plan] the patrol and what stands beside it (the hops: the patrol 0, the pack one link on)
                chosenPack = incoming->members;
                lastPackHops.clear();
                for (Unit* mob : incoming->members)
                    lastPackHops.emplace_back(mob->GetGUID(), uint8(0));
                for (Unit* mob : imminentAround.mobs)
                {
                    chosenPack.push_back(mob);
                    lastPackHops.emplace_back(mob->GetGUID(), uint8(1));
                }
                chosen = EstimatePack(chosenPack, bot, groupDps, estimateMode);
                chosenOverlaps.clear();
                found = fallback = patrolFirst = true;
                _planReason = Acore::StringFormat("a patrol reaches the group in {:.0f} s, met by the tank: ", eta);
                if (!imminentAround.mobs.empty())
                    _planReason += Acore::StringFormat("+{} idle mobs beside the patrol; ", imminentAround.mobs.size());
                LogPatrol(bot, Acore::StringFormat("[patrol_meet_imminent] {}: patrol {} reaches the group ({:.1f} yd, "
                                                   "eta {:.0f} s), {} mobs, over every plan: the tank opens on it",
                                                   bot->GetName(), member->GetName(), distance, eta,
                                                   chosenPack.size()));
            }
            else if (mayHoldFor(incoming->walker, incoming->reach))
            {
                LogPatrol(bot, Acore::StringFormat(
                                   "[pull hold] {}: patrol {} approaches ({:.1f} yd, eta {:.0f} s): comes to the "
                                   "group, too many with it to pull now",
                                   bot->GetName(), member->GetName(), distance, eta));
                _planNotReadyWhy = Acore::StringFormat("patrol wait: {} comes to the group (eta {:.0f} s)",
                                                       member->GetName(), eta);
                return false;
            }
        }
    }

    // Measured: the size the group prefers first (about three mobs at the content's gear, more for a stronger tank),
    // split off the pack where it can be.
    if (!found)
        found = planAround(1.0f, measured, false);

    // [plan_route_blocker] No plan reaches the target and a walk in for it wakes another idle pack ("not this way"):
    // that pack is the pull, planned by the whole planner as a target of its own (its plan, the topped-up fallback,
    // the lone mob, the stand's guarantee); this look's verdict covers the route target too, so the harness's edge
    // hold reads the blocker's verdict instead of an endless "best 0 mobs" refusal. One level deep (the blocker's own
    // blocker is its planner's business on the next look). A blocker that is or brings a boss stays the boss logic's.
    if (!found && !boss && PLAN_ROUTE_BLOCKER && !_planBlockerDepth)
    {
        if (!targetBlocker)
        {
            CollectPack(botAI, bot, target, target->GetPosition(), possible, pack, FightHere(target->GetPosition()),
                        measured, false, overlapsOut);
            if (walkPulls(target, bot->GetPosition(), target->GetPosition()))
                targetBlocker = lastWoken;
        }

        Creature* const blocker = targetBlocker;
        bool takeBlocker = blocker && blocker->IsAlive() && !blocker->IsInCombat() && !IsBossMob(blocker) &&
                           !EncounterOrder::IsLocked(blocker) &&
                           std::find(_planLookPack.begin(), _planLookPack.end(), blocker->GetGUID()) ==
                               _planLookPack.end();
        if (takeBlocker)
        {
            std::vector<Unit*> blockerPack;
            CollectPack(botAI, bot, blocker, blocker->GetPosition(), possible, blockerPack,
                        FightHere(blocker->GetPosition()), measured, false, nullptr);
            takeBlocker =
                std::none_of(blockerPack.begin(), blockerPack.end(), [](Unit* mob) { return IsBossMob(mob); });
            if (takeBlocker)
            {
                ObjectGuid const routeTarget = target->GetGUID();
                std::string const targetName = target->GetName();
                std::string const blockerName = blocker->GetName();
                float const blockerDistance = bot->GetExactDist(blocker);
                size_t const blockerMobs = blockerPack.size();
                ++_planBlockerDepth;
                bool const planned = PlanCarefulPull(blocker, measured, rescaled);
                --_planBlockerDepth;
                // the look stays the route target's: cached for it, its verdict covers it (GetRefusal by its guid)
                _planFor = routeTarget;
                _planMs = now;
                if (std::find(_planVerdictPack.begin(), _planVerdictPack.end(), routeTarget) == _planVerdictPack.end())
                    _planVerdictPack.push_back(routeTarget);
                if (std::find(_planLookPack.begin(), _planLookPack.end(), routeTarget) == _planLookPack.end())
                    _planLookPack.push_back(routeTarget);
                if (planned)
                    _planReason = Acore::StringFormat("the way to {} wakes {}: {} is the pull; ", targetName,
                                                      blockerName, blockerName) +
                                  _planReason;
                else if (!_planVerdictWhy.empty())
                    _planVerdictWhy = Acore::StringFormat("the way to {} wakes {} ({} mobs): ", targetName,
                                                          blockerName, blockerMobs) +
                                      _planVerdictWhy;

                static thread_local uint32 blockerLogMs = 0;  // per map thread
                uint32 const logNow = getMSTime();
                if (!blockerLogMs || getMSTimeDiff(blockerLogMs, logNow) > 5 * IN_MILLISECONDS)
                {
                    blockerLogMs = logNow ? logNow : 1;
                    LOG_INFO("playerbots", "[plan_route_blocker] {}: the way to {} wakes {} ({} mobs, {:.1f} yd): "
                             "planned as the pull - {}", bot->GetName(), targetName, blockerName, blockerMobs,
                             blockerDistance,
                             planned ? "planned (" + _planReason + ")"
                                     : (_planVerdict == PlanVerdictState::Refused
                                            ? "refused for now (" + _planVerdictWhy + ")"
                                            : "held (" + _planNotReadyWhy + ")"));
                }
                return planned;
            }
        }
    }

    // A pack that cannot be split that small is not skipped for good. Topped up, the group takes the first plan the
    // tank and the healers carry (measured), or one at most CAREFUL_FALLBACK_OVERLOAD over its size (levelling), and
    // rests for nothing more (it is full already).
    std::string notToppedUp = boss ? std::string("a boss pull") : std::string();
    // [guarantee_ready_gate] the fallback plans and the stand's guarantee wait for the rebuff round and the sitters
    // below their lines too (PullReadiness::GroupPreparing), as the harness's BlockerPullGate holds on GroupFullyReady
    bool const toppedUp = !boss && IsGroupToppedUp(bot, &notToppedUp) &&
                          !PullReadiness::GroupPreparing(bot, &notToppedUp);

    // User rule: from one side a pack brings fewer mobs than from the other (an edge takes one pack, the middle all
    // of them). Before taking the whole pack, the tank looks for a cast spot round it that brings fewer.
    if (!found && measured && toppedUp && !boss &&
        (_sideFailFor != target->GetGUID() || getMSTimeDiff(_sideFailMs, now) >= SIDE_PULL_RETRY_MS))
    {
        float range = 0.0f;
        Position defaultSpot;
        if (!IsCrowdControlled(target) && PullBackSpot(target, defaultSpot, false, &range))
        {
            CollectPack(botAI, bot, target, defaultSpot, possible, pack, FightHere(defaultSpot), measured, false,
                        overlapsOut);
            size_t const defaultMobs = pack.size();
            size_t bestMobs = defaultMobs;
            Position bestSpot;
            PackEstimate bestEstimate;
            std::vector<Unit*> bestPack;
            std::vector<PackOverlap> bestOverlaps;
            uint32 paths = SIDE_PULL_MAX_PATHS;
            float const radius = std::max(range - SIDE_PULL_RANGE_MARGIN, CAREFUL_MIN_PULL_RANGE);
            // The walk there enters no idle mob's aggro reach but the pack's own.
            auto const safeWalk = [&](Position const& spot, std::vector<Unit*> const& sidePack) -> bool
            {
                LeaderPath path(bot);  // [lead99]
                path.CalculatePath(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), false);
                if (path.GetPathType() != PATHFIND_NORMAL || path.getPathLength() > SIDE_PULL_MAX_WALK)
                    return false;

                std::vector<std::pair<Creature*, float>> reaches;
                for (ObjectGuid const& guid : possible)
                {
                    Creature* mob = botAI->GetCreature(guid);
                    if (!mob || !IsIdleEnemy(bot, mob) || !mob->HasReactState(REACT_AGGRESSIVE) ||
                        std::find(sidePack.begin(), sidePack.end(), mob) != sidePack.end())
                        continue;

                    reaches.emplace_back(mob, MovementAction::IdleAggroReach(mob, bot, SIDE_PULL_AGGRO_MARGIN));
                }

                Movement::PointsArray const& points = path.GetPath();
                float const eye = bot->GetCollisionHeight();
                for (size_t i = 1; i < points.size(); ++i)
                {
                    G3D::Vector3 const segment = points[i] - points[i - 1];
                    float const length = segment.length();
                    for (float along = 0.0f; along <= length; along += SIDE_PULL_SAMPLE_STEP)
                    {
                        G3D::Vector3 const point =
                            points[i - 1] + (length > 0.0f ? segment * (along / length) : segment);
                        for (auto const& [mob, reach] : reaches)
                        {
                            if (mob->GetExactDist(point.x, point.y, point.z) < reach &&
                                MovementAction::PassesAggroZGate(mob, bot, point.z) &&
                                mob->IsWithinLOS(point.x, point.y, point.z + eye))
                                return false;
                        }
                    }
                }

                return true;
            };

            for (uint32 i = 0; i < SIDE_PULL_ANGLES && paths; ++i)
            {
                float const angle = 2.0f * float(M_PI) * float(i) / float(SIDE_PULL_ANGLES);
                float const x = target->GetPositionX() + radius * std::cos(angle);
                float const y = target->GetPositionY() + radius * std::sin(angle);
                float z = 0.0f;
                if (!GroundNear(bot, x, y, target->GetPositionZ(), PULL_PATH_HEIGHT, z) ||
                    !target->IsWithinLOS(x, y, z + CAREFUL_EYE_HEIGHT))
                    continue;

                Position const spot(x, y, z);
                CollectPack(botAI, bot, target, spot, possible, pack, FightHere(spot), measured, false, overlapsOut);
                if (pack.size() >= bestMobs)
                    continue;

                PackEstimate const estimate = EstimatePack(pack, bot, groupDps, estimateMode);
                OverloadTerms terms;
                CarefulOverload(bot, estimate, strength, support, kit, measured, &terms);
                if (estimate.boss || !withinCap(estimate) || terms.Max() > 1.0f || !TankCarries(estimate, support))
                    continue;

                // the cast spot stays in the healer's sight
                if (!HealerSees(bot, spot))
                    continue;

                --paths;
                if (!safeWalk(spot, pack))
                    continue;

                bestMobs = pack.size();
                bestSpot = spot;
                bestEstimate = estimate;
                bestPack = pack;
                bestOverlaps = overlaps;
            }

            if (bestMobs < defaultMobs)
            {
                _planCached = target->GetGUID();
                _planPullBack = true;
                _planSide = true;
                _sideSpot = bestSpot;
                _sideFor = target->GetGUID();
                _sideMs = now;
                _sideNote = Acore::StringFormat("side {} mobs vs default {}", bestMobs, defaultMobs);
                _planReason = "a side of the pack brings fewer: " + _sideNote;
                chosen = bestEstimate;
                chosenPack = bestPack;
                chosenOverlaps = bestOverlaps;
                found = fallback = true;
                LOG_INFO("playerbots", "[pull side] {}: {} from ({:.1f} {:.1f} {:.1f}), {:.1f} yd away: {}",
                         bot->GetName(), target->GetName(), bestSpot.GetPositionX(), bestSpot.GetPositionY(),
                         bestSpot.GetPositionZ(), bot->GetExactDist(bestSpot), _sideNote);
            }
            else
            {
                _sideFailFor = target->GetGUID();
                _sideFailMs = now;
            }
        }
    }

    if (!found && measured && toppedUp)
    {
        fallbackCost = true;
        fallback = found = planAround(1.0f, false, false);
        fallbackCost = false;
    }

    // A boss whose adds stand too close to be split off is pulled as before, unless the walk to him wakes another
    // pack: that pack is planned first (an ordinary plan, the topped-up fallback when the group is full), and with
    // none the boss waits ("the walk to the boss wakes").
    Creature* bossWalkWakes = nullptr;
    if (!found && boss)
    {
        CollectPack(botAI, bot, target, target->GetPosition(), possible, pack, FightHere(target->GetPosition()),
                    measured, false, overlapsOut);
        if (walkPulls(target, bot->GetPosition(), target->GetPosition()))
            bossWalkWakes = lastWoken;
    }

    if (!found && boss && bossWalkWakes)
    {
        found = tryPlan(bossWalkWakes, false, 1.0f, measured, false) ||
                tryPlan(bossWalkWakes, false, 1.0f, false, false);
        if (!found && IsGroupToppedUp(bot))
        {
            fallbackCost = true;
            fallback = found = tryPlan(bossWalkWakes, false, CAREFUL_FALLBACK_OVERLOAD, false, false);
            fallbackCost = false;
        }

        // [lead90] b131 rfc-h: 25 s of "the pack does not fit (a boss pull: no fallback plan)" - the walk to the boss
        // woke a mob no plan took, and the boss waited for the group topped up. The woken mob is its own pull at the
        // fallback's measured overload whatever the top-up (the leader's readiness lines hold the pull)
        if (!found)
        {
            fallbackCost = true;
            fallback = found = tryPlan(bossWalkWakes, false, CAREFUL_FALLBACK_OVERLOAD, false, false);
            fallbackCost = false;
        }
        if (found)
            _planReason = Acore::StringFormat("the walk to {} wakes {}, taken first; ", target->GetName(),
                                              bossWalkWakes->GetName()) +
                          _planReason;
    }

    // [lead90] ... and with no plan at all for it the boss is pulled: the woken mob joins his fight
    if (!found && boss)
    {
        float danger = 0.0f;
        bool const manageable = IsPackManageable(botAI, bot, target, strength, danger);
        // [lead86] b129 rfc-h 683.2-727 s: "the pack does not fit (a boss pull: no fallback plan)" on Taragaman, who
        // died in 50-54 s with 0 deaths in b100 / b118 / b128 - the boss is the route's goal, never "does not fit ->
        // nothing": over the capacity estimate he is pulled where he stands all the same, behind the boss gate's full
        // readiness and with the leader's burn (a boss fight always burns)
        _planCached = target->GetGUID();
        _planPullBack = false;
        _planLos = false;
        chosen = PackEstimate();
        chosenPack.clear();
        chosenOverlaps.clear();
        chosen.danger = danger;
        chosen.boss = true;
        found = true;
        _planReason = manageable ? std::string("boss pulled where it stands")
                                 : Acore::StringFormat("boss pulled where it stands: over the capacity estimate "
                                                       "(danger {:.2f}), the boss is the route's goal", danger);
        if (bossWalkWakes)
            _planReason += Acore::StringFormat("; the walk wakes {}: it joins his fight", bossWalkWakes->GetName());
    }

    // The mob of the pack that brings the fewest when pulled at range to where the tank stands with a pull spell
    // that hits it alone: none (no formation, no neighbour, no assistance) at best, nearest first among equals, with
    // what it brings in `onePack`. Scholomance hall (wave 27): its nearest mob was a standing caster whose assistance
    // chain brought 8-20 to a pull planned as one, while a Risen Guard of the same hall comes with 3.
    // A pack that brings a boss (its call for help reaching the boss or its helpers) ranks after every pack that
    // does not, whatever its size.
    std::vector<Unit*> onePack;
    std::vector<PackOverlap> oneOverlaps;
    Position oneKill;
    bool oneBoss = false;
    // [lead253] b192 rfc-h 5G 313 s: the pick was the on-path Ragefire Shaman 12; "every plan too dangerous, one
    // mob walked in" took Ragefire Trogg 13 of its adds, which the leader had judged off the route (62 / 114 s) - the
    // tank walked ~127 yd out to it, took Trogg 21 there and ~122 yd back (~60 s). The one mob is one of the leader's
    // path: an add it judged off the path is never it (the target itself is the leader's pick)
    PullPlanner::LeaderInputs const* const pathInputs = PullPlanner::CurrentLeaderInputs();
    auto const offPath = [&](Unit* mob)
    {
        return pathInputs && mob != target &&
               std::find(pathInputs->offPath.begin(), pathInputs->offPath.end(), mob->GetGUID()) !=
                   pathInputs->offPath.end();
    };
    auto const loneMob = [&]() -> Unit*
    {
        std::vector<Unit*> candidates(adds);
        if (!boss)
            candidates.push_back(target);

        std::sort(candidates.begin(), candidates.end(),
                  [this](Unit* a, Unit* b) { return bot->GetExactDist(a) < bot->GetExactDist(b); });
        Unit* smallest = nullptr;
        size_t tried = 0;
        for (Unit* candidate : candidates)
        {
            if (tried >= CAREFUL_LONE_MOB_CANDIDATES)
                break;

            if (IsBossMob(candidate) || IsCrowdControlled(candidate) || offPath(candidate) ||
                PrePullCc::IsClaimed(bot, candidate->GetGUID()) ||
                (candidate->GetGUID() == _lastPullBack &&
                 getMSTimeDiff(_lastPullBackMs, now) < CAREFUL_PULL_RETRY_MS) ||
                CarefulPlanVerdict::EngageOnly(bot->GetGUID(), candidate->GetGUID()) ||    // [b37_sv]
                CarefulPlanVerdict::PullOutOfReach(bot->GetGUID(), candidate->GetGUID()))  // [lead182]
                continue;

            // [b39_occ] No lone-mob pull back of a flier high over a walking tank: occ-h 20261026-054314-occ-h-5A-16
            // (737 s) picked an Azure Ring Guardian 15 yd over the north island as "the one mob" of a Construct pack
            // (a wanderer of that pack). Its cast spot hung over the drop ("cast_spot_off_floor"), the tank walked
            // the path to it, under it and into the Constructs, stayed in "out of pull range (40.5 yd)" for 9 s
            // while three Constructs killed the healer, then the group (death burst 745-761 s).
            if (highFlier(candidate))  // [b40_occ] the same rule as the plan alternatives
                continue;

            ++tried;
            Position castSpot;
            if (!PullBackSpot(candidate, castSpot, true))
                continue;

            Position kill;
            collectPullBack(candidate, castSpot, kill);
            // A caster that keeps its range does not come back to the tank: the fight goes to where it stands, and a
            // lone mob that brings others there is no lone mob (scholo-n wave 7: a Scholomance Adept 3 yd from idle
            // Necromancers, "expects 4", 26 came and the group died). Its pack is pulled round a corner instead. A
            // small caster pack of its own (CASTER_LONE_MAX_MOBS, nothing from another pack) is taken: ok-n wave 10,
            // a Frostbringer with two Eyes of Taldaram (cost 1.06) had no lone mob at all and stood for good.
            Creature* const lone = candidate->ToCreature();
            if (lone && pack.size() > 1 && KeepsRange(lone))
            {
                bool const ownPack = pack.size() <= CASTER_LONE_MAX_MOBS &&
                                     std::all_of(pack.begin(), pack.end(), [&](Unit* mob)
                                                 {
                                                     return mob == target || std::find(adds.begin(), adds.end(),
                                                                                       mob) != adds.end();
                                                 });
                if (!ownPack)
                    continue;
            }

            bool const bringsBoss = std::any_of(pack.begin(), pack.end(), [](Unit* mob) { return IsBossMob(mob); });
            if (!smallest || (oneBoss && !bringsBoss) || (oneBoss == bringsBoss && pack.size() < onePack.size()))
            {
                smallest = candidate;
                onePack = pack;
                oneOverlaps = overlaps;
                oneKill = kill;
                oneBoss = bringsBoss;
            }

            if (pack.size() == 1)
                break;
        }

        return smallest;
    };

    // Nothing fits and the group has waited topped up: a real tank does not stand before the pack for good. Measured:
    // a caster pack round a corner (the casters come into melee) if that is at most CAREFUL_FALLBACK_OVERLOAD over
    // what the group takes; levelling: the smallest plan it has if that is within the cap and the tank's health
    // carries it. Else a lone mob of the pack (the hard minimum).
    bool lastResort = false;
    // [b36_sv] the plan is a [b35_lbrs2] split peel round a corner (_planSingle false, unlike the straight peel): the
    // fallback cost gate below holds it to the split's own cost cap (singleCap), the one its search used. sv-h-5D-36
    // 20261026-034959: every corner peel "4 come, cost 1.51" (search cap 3.0) was dropped by "fallback plan costs
    // 1.51 tank hp, over 0.80" whenever the crossing patrol was away, so the overlap wait restarted forever.
    bool splitPlan = false;
    // [adaptive_cap] at least what the group proved in this run ([lead262] the proof is its own bound: its cost cap)
    uint32 const baseLastResortMobs = maxMobs ? maxMobs : CAREFUL_LAST_RESORT_DEFAULT_CAP;
    uint32 const lastResortMobs = std::max(baseLastResortMobs, uint32(proven.mobs));
    // the over-cap scale stays on the configured cap: a proven size is taken as it is, not x2 again
    size_t const overCapMobs = std::max<size_t>(
        size_t(std::ceil(float(baseLastResortMobs) * GUARANTEE_OVER_CAP_SCALE)), lastResortMobs);
    if (!found && !toppedUp)
    {
        std::string const why = Acore::StringFormat("refused and not topped up: {} (tank {:.0f} % hp, lowest healer "
                                                    "{:.0f} % mana)", notToppedUp, bot->GetHealthPct(),
                                                    LowestHealerMana(bot));
        LogPullHold(bot, why.c_str(), target);
        // [b30_topped_up] the real reason (dmw-n: "not topped up" hid a healer stranded 300 yd off; a boss's pack
        // has no fallback plan at all, toppedUp is never true for it)
        _planNotReadyWhy = boss ? std::string("the pack does not fit (a boss pull: no fallback plan)")
                                : "the pack does not fit and the group is not topped up yet: " + notToppedUp;
    }

    if (!found && toppedUp)
    {
        // The wait runs across a nearest target that flips between mobs of the same packs (Steam Vault wave 16:
        // Coilfang Siren / Warrior alternated, the per-target wait restarted for 4 minutes). A wait older than a
        // few stall periods is from an earlier stand (a fight came between) and starts again.
        // [lead111] user: no elapsed-time rule changes a decision - the last resort runs whenever there is no plan,
        // by fact (the group topped up); the stall clock, the quick lone-mob mode and its period are gone
        if (_stallFor.IsEmpty())
        {
            _stallFor = target->GetGUID();
            _stallMs = now;
        }
        bool const noPlanBuilt = best.mob.IsEmpty() || best.estimate.cost <= 0.0f;
        {
            bool const quickOne = false;
            uint32 const stalled = getMSTimeDiff(_stallMs, now);  // the reason's text only

            fallbackCost = true;
            bool const losFallback = !quickOne && measured && planAround(CAREFUL_FALLBACK_OVERLOAD, false, true);
            fallbackCost = false;
            if (losFallback)
                fallback = lastResort = found = true;
            // [lead111] the smallest plan when the leader's estimate carries it (JudgeMobs), no size / cost cap
            else if (float smallestOverload = 0.0f; !quickOne && !measured && !noPlanBuilt &&
                     [&]() { std::string judged; return JudgeMobs(best.pack, smallestOverload, judged); }())
            {
                _planCached = best.mob;
                _planPullBack = best.pullBack;
                _planLos = false;
                if (best.pullBack)
                {
                    _planKillFor = best.mob;
                    _planKill = best.kill;
                }
                else
                    _planKillFor.Clear();

                chosen = best.estimate;
                chosenPack = best.pack;
                chosenOverlaps = best.overlaps;
                fallback = lastResort = found = true;
                _planReason = Acore::StringFormat("last resort: the smallest plan, {} mobs", best.estimate.mobs);
            }
            else
            {
                // User rule (hard minimum): nobody stands still while mobs are there. When every plan looks too
                // dangerous, the tank still pulls ONE mob with a single-target ranged pull and backs it off; the
                // real outcome then shows whether its friends come. The mob the model expects to bring the fewest
                // (loneMob), else the nearest mob of the pack.
                Unit* one = loneMob();
                if (!one)
                {
                    float nearest = 0.0f;
                    std::vector<Unit*> candidates(adds);
                    if (!boss)
                        candidates.push_back(target);
                    for (Unit* candidate : candidates)
                    {
                        // nor a caster that stays where it is (loneMob above)
                        Creature* const lone = candidate->ToCreature();
                        if (IsBossMob(candidate) || IsCrowdControlled(candidate) || offPath(candidate) ||
                            PrePullCc::IsClaimed(bot, candidate->GetGUID()) || (lone && KeepsRange(lone)))
                            continue;
                        float const distance = bot->GetExactDist(candidate);
                        if (!one || distance < nearest)
                        {
                            one = candidate;
                            nearest = distance;
                        }
                    }
                }
                if (!one)
                {
                    // no lone mob at all (casters that stay in packs, bosses, crowd control): said, not silent; the
                    // stand's guarantee below judges the whole pack, so no dead end is declared here
                    _planVerdictWhy = Acore::StringFormat("refused for now: no lone mob around {} (casters that stay "
                                                          "in a pack, a boss, or crowd control)", target->GetName());
                    LogPullBlocked(bot, _planVerdictWhy, target);
                }

                Position castSpot;
                // The lone mob is pulled at range with a single-target spell; while none is ready (Hand of
                // Reckoning on cooldown) the tank waits instead of walking in (Stratholme wave 24: 16 mobs came).
                bool walkIn = false;
                // [lead111] no ranged pull ready (a cooldown, a stance): the walk in when the leader's estimate
                // carries what it brings (JudgeMobs), else the wait for the ranged pull - a game mechanic, no clock
                if (one && !PullBackSpot(one, castSpot, true))
                {
                    std::vector<Unit*> walkPack;
                    CollectPack(botAI, bot, one, one->GetPosition(), possible, walkPack, FightHere(one->GetPosition()),
                                measured, false, nullptr);
                    if (std::find(walkPack.begin(), walkPack.end(), one) == walkPack.end())
                        walkPack.insert(walkPack.begin(), one);
                    float walkOverload = 0.0f;
                    std::string walkJudged;
                    if (JudgeMobs(walkPack, walkOverload, walkJudged))
                    {
                        LogPullHold(bot, ("lone-mob pull: no single-target ranged pull ready, walks in (" +
                                          walkJudged + ")").c_str(), one);
                        walkIn = true;
                    }
                    else
                    {
                        LogPullHold(bot, ("lone-mob pull waits for a single-target ranged pull (the walk in: " +
                                          walkJudged + ")").c_str(), one);
                        one = nullptr;
                    }
                }
                // The lone mob's pack where it is fought, as every plan's: walked in, at its spot; else its pull
                // back (the cast spot, the kill spot), unless loneMob counted it already.
                if (one && (walkIn || onePack.empty() || onePack.front() != one))
                {
                    if (walkIn)
                    {
                        CollectPack(botAI, bot, one, one->GetPosition(), possible, pack, FightHere(one->GetPosition()),
                                    measured, false, overlapsOut);
                        oneKill = one->GetPosition();
                    }
                    else
                        collectPullBack(one, castSpot, oneKill);

                    onePack = pack;
                    oneOverlaps = overlaps;
                }

                // The quick lone mob: only a single that truly comes alone (its pack where it is fought is itself,
                // the walk to it or to its cast spot wakes nobody), that the tank's health carries, fought where the
                // healer sees it. RFC 2026-10-24 20:04: a "single" that drew the pack beside it.
                if (one && quickOne)
                {
                    PackEstimate const quick = EstimatePack(onePack, bot, groupDps, estimateMode);
                    Position const fightAt = walkIn ? one->GetPosition() : oneKill;
                    pack = onePack;
                    overlaps = oneOverlaps;
                    Creature* const woken =
                        walkPulls(one, bot->GetPosition(), walkIn ? one->GetPosition() : castSpot) ? lastWoken
                                                                                                    : nullptr;
                    std::string problem;
                    if (onePack.size() > 1)
                        problem = Acore::StringFormat("brings {} mobs where it is fought", onePack.size());
                    else if (woken)
                        problem = Acore::StringFormat("is reached by a walk that wakes {}", woken->GetName());
                    else if (std::string carries; !TankCarries(quick, support, &carries))
                        problem = "is more than the tank carries: " + carries;
                    else if (!HealerSees(bot, fightAt))
                        problem = "is fought where the healer does not see";

                    if (!problem.empty())
                    {
                        std::string const why = Acore::StringFormat(
                            "group ready, no safe pack plan, the nearest single mob {} {}", one->GetName(), problem);
                        LogPullHold(bot, why.c_str(), one);
                        one = nullptr;
                    }
                }

                // Planned as what it brings, so the pull check measures the chain model too.
                std::vector<Unit*> const single = !one ? std::vector<Unit*>()
                                                  : (!onePack.empty() && onePack.front() == one
                                                         ? onePack
                                                         : std::vector<Unit*>{one});
                PackEstimate const oneEstimate =
                    one ? EstimatePack(single, bot, groupDps, estimateMode) : PackEstimate();
                // A hard minimum the leader's judge refuses (JudgeMobs) is no pull: round a corner (CAREFUL_FALLBACK_
                // OVERLOAD, the cost cap of a fallback), else wait for a mover of what it brings to walk off, else
                // blocked; the refused pack is left to the group's pre-pull crowd control (HoldForPrePullCc).
                // Sweep batch4 (LBRS worgs 6.94, Shattered Halls entrance 3.59, Gundrak golems 3.35): a blocked pack
                // the route needs stalled the run for good. The cap escalates while the tank stands at the hard
                // minimum (the group ready all along): x2 after CAREFUL_BLOCK_ESCALATE_MS, x3 after twice that - the
                // hard pack is taken rested rather than never; a 15-cost pull (wave 44 Gilnid) stays blocked.
                // The clock is the tank's stand, not the lone mob's: within one stand the nearest target and its lone
                // mob flip between mobs of the same packs (sh-n Heathen / Legionnaire, dmw-n Mana Remnant: "blocked
                // 0 s" on every look, never escalated). isUseful runs it (_blockedSinceMs): from the first look the
                // group stood topped up without a pull, anew after a fight or a gap (CAREFUL_BLOCK_GAP_MS).
                uint32 const blockNow = getMSTime();
                if (!quickOne)
                    _blockedTarget = one ? one->GetGUID() : ObjectGuid::Empty;

                // [lead110] user: no fixed pack cap and no stand clock. The smallest pull is judged by the leader's
                // own estimate - the overload of what it brings (JudgeMobs: the plans' danger model, "overload x of
                // 1.00"): it fits, it is pulled now (walked in too when no ranged pull: what it wakes is part of the
                // pull); it does not, round a corner, else no safe pull here (the split, the route round it). b143
                // rfc-h 853-913 s: the cap / stand refusals cycled 60 s in the Searing Blade room.
                (void)blockNow;
                float singleOverload = 0.0f;
                std::string singleJudged;
                bool const singleFits = one && !oneEstimate.boss && JudgeMobs(single, singleOverload, singleJudged);
                if (one && !oneEstimate.boss && !singleFits)
                {
                    std::string const capWhy = Acore::StringFormat("the lone mob {} brings {} mobs: {}",
                                                                   one->GetName(), single.size(), singleJudged);
                    bool corner = false;
                    if (!measured || quickOne)  // a measured group tried the corner above already
                    {
                        fallbackCost = true;
                        corner = planAround(CAREFUL_FALLBACK_OVERLOAD, false, true);
                        fallbackCost = false;
                    }

                    if (corner)
                    {
                        fallback = lastResort = found = true;
                        _planReason = capWhy + ", pulled round a corner instead: " + _planReason;
                    }
                    else
                    {
                        _planVerdictHopeless = true;
                        _planVerdictGuaranteeWaitMs = 0;
                        _planVerdictWhy = Acore::StringFormat("no safe pull: the smallest pull ({} {}) brings {} mobs: "
                                                              "{}", one->GetName(), walkIn ? "walked in" : "at range",
                                                              single.size(), singleJudged);
                        LogPullBlocked(bot, capWhy + "; no pull round a corner", one);
                    }

                    one = nullptr;
                }
                else if (one && !oneEstimate.boss)
                    _planGuaranteed = true;  // [lead110] the estimate carries it: no fixed cost cap refuses it later

                if (one)
                {
                    _planCached = one->GetGUID();
                    _planPullBack = !walkIn;
                    _planSingle = true;
                    _planLos = false;
                    if (_planPullBack && !onePack.empty() && onePack.front() == one)
                    {
                        _planKillFor = one->GetGUID();
                        _planKill = oneKill;
                    }
                    else
                        _planKillFor.Clear();

                    chosen = oneEstimate;
                    chosenPack = single;
                    chosenOverlaps =
                        !onePack.empty() && onePack.front() == one ? oneOverlaps : std::vector<PackOverlap>();
                    fallback = lastResort = found = true;
                    _planReason =
                        (quickOne ? Acore::StringFormat("group ready, no safe pack plan for {:.0f} s → nearest single "
                                                        "mob{}", float(stalled) / float(IN_MILLISECONDS),
                                                        walkIn ? ", walked in (no ranged pull)" : ", at range")
                                  : Acore::StringFormat("every plan too dangerous, one mob{}",
                                                        walkIn ? " walked in (no ranged pull)" : " at range")) +
                        Acore::StringFormat("; packs: one {} where it is fought", single.size());
                    LOG_INFO("playerbots", "[pull one] {}: every plan around {} too dangerous ({} mobs, cost {:.2f} "
                             "tank hp); pulls one mob, {}{}, expects {}", bot->GetName(), target->GetName(),
                             adds.size() + 1, best.estimate.cost, one->GetName(), _planPullBack ? " (pull back)" : "",
                             single.size());
                }
            }

            if (!found && !quickOne)
                _stallFor.Clear();
        }
    }

    // the smallest real pull of the pack (the split below): its best plan's count when one was looked at, else the pack
    size_t const smallestPull =
        best.mob.IsEmpty() ? adds.size() + 1 : std::min<size_t>(adds.size() + 1, best.estimate.mobs);
    // [lead110] user: no fixed pack cap, no stand clock - the pack as it comes (the pack, what the way in wakes, the
    // chain of the core's call for assistance) is judged by the leader's estimate (JudgeMobs, the plans' danger
    // model): it fits, it is pulled now as one planned pull, at range when the tank has a pull, else walked in (the
    // woken packs are part of the pull); it does not, no safe pull here (the split below, the route round it)
    // [lead208] the pull as it comes of a pack the leader must clear (its whole-pack request) that is over the
    // budget: kept as the cheapest option left, taken when no split below carries the pack (never a dead end)
    struct CheapestOption
    {
        std::vector<Unit*> pack;
        bool back = false;
        float overload = 0.0f;
        std::string judged;
        std::string woken;
    } cheapest;
    if (!found && toppedUp && !boss && !targetPackBoss)
    {
        std::vector<ObjectGuid> packGuids = {target->GetGUID()};
        for (Unit* mob : adds)
            packGuids.push_back(mob->GetGUID());

        // an encounter tactic's request (GuaranteeRequest) is walked in when asked
        ObjectGuid requestedMob;
        bool requestedWalkIn = false;
        bool requested = GuaranteeRequest::Get(bot->GetGUID(), requestedMob, requestedWalkIn) &&
                         std::find(packGuids.begin(), packGuids.end(), requestedMob) != packGuids.end();
        // [lead3] LEADER_NO_BODY_FALLBACK: the leader's whole-pack request comes in the look's inputs
        if (PullPlanner::LeaderInputs const* inputs = _snap ? _snap->Leader() : nullptr;
            inputs && Leader::Leads(Leader::LEADER_NO_BODY_FALLBACK) && !inputs->wholePack.IsEmpty() &&
            std::find(packGuids.begin(), packGuids.end(), inputs->wholePack) != packGuids.end())
        {
            requested = true;
            requestedMob = inputs->wholePack;
            requestedWalkIn = requestedWalkIn || inputs->walkIn;
        }
        if (!IsCrowdControlled(target) && !EncounterOrder::IsLocked(target))
        {
            Position castSpot;
            bool const back = !(requested && requestedWalkIn) && PullBackSpot(target, castSpot);
            Position const walkTo = back ? castSpot : target->GetPosition();
            // the pack of the pull it makes: pulled back to the tank, or where it stands when walked in
            Position kill;
            if (back)
                collectPullBack(target, castSpot, kill);
            else
                CollectPack(botAI, bot, target, target->GetPosition(), possible, pack, FightHere(target->GetPosition()),
                            measured, false, nullptr);
            std::vector<Unit*> guaranteed(pack);
            if (std::find(guaranteed.begin(), guaranteed.end(), target) == guaranteed.end())
                guaranteed.insert(guaranteed.begin(), target);
            Unit* wokenBoss = nullptr;
            Creature* firstWoken = nullptr;
            for (uint32 round = 0; round < GUARANTEE_WALK_ROUNDS && !wokenBoss; ++round)
            {
                bool walkable = false;
                Creature* const woken = WalkEntersAggro(botAI, bot, possible, bot->GetPosition(), walkTo, guaranteed,
                                                        CAREFUL_MAX_WALK_CHECK, CAREFUL_AGGRO_MARGIN, walkable);
                if (!woken)
                    break;

                if (!firstWoken)
                    firstWoken = woken;

                std::vector<Unit*> wokenPack;
                CollectPack(botAI, bot, woken, woken->GetPosition(), possible, wokenPack,
                            FightHere(woken->GetPosition()), measured, false, nullptr);
                if (std::find(wokenPack.begin(), wokenPack.end(), woken) == wokenPack.end())
                    wokenPack.push_back(woken);

                for (Unit* mob : wokenPack)
                {
                    if (IsBossMob(mob))
                        wokenBoss = mob;
                    if (std::find(guaranteed.begin(), guaranteed.end(), mob) == guaranteed.end())
                        guaranteed.push_back(mob);
                }
            }

            // [guarantee_chain_count] the pack as the core's call for assistance grows it (Creature::CallAssistance),
            // one step deep from the mobs that engage by themselves (the helpers call next from the tank's fight)
            size_t const engagedByThemselves = guaranteed.size();
            float const assistRadius = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
            if (!wokenBoss && assistRadius > 0.0f)
            {
                for (size_t index = 0; index < engagedByThemselves && !wokenBoss; ++index)
                {
                    Creature* caller = guaranteed[index]->ToCreature();
                    if (!caller || !CallsOnEngage(caller))
                        continue;

                    for (ObjectGuid const& guid : possible)
                    {
                        Creature* helper = botAI->GetCreature(guid);
                        if (!helper || !helper->IsAlive() || helper->IsInCombat() ||
                            std::find(guaranteed.begin(), guaranteed.end(), helper) != guaranteed.end() ||
                            !helper->IsWithinDistInMap(caller, assistRadius) || !helper->CanAssistTo(caller, bot) ||
                            !helper->IsWithinLOSInMap(caller))
                            continue;

                        guaranteed.push_back(helper);
                        if (IsBossMob(helper))
                            wokenBoss = helper;
                        if (!firstWoken)
                            firstWoken = helper;
                    }
                }
            }

            float overload = 0.0f;
            std::string judged;
            bool const fits = !wokenBoss && JudgeMobs(guaranteed, overload, judged);
            if (!fits && !wokenBoss && requested)
            {
                cheapest.pack = guaranteed;
                cheapest.back = back;
                cheapest.overload = overload;
                cheapest.judged = judged;
                cheapest.woken = firstWoken ? firstWoken->GetName() : std::string();
            }
            if (!fits)
            {
                _planVerdictHopeless = true;
                _planVerdictGuaranteeWaitMs = 0;
                _planVerdictWhy =
                    firstWoken || wokenBoss
                        ? Acore::StringFormat("no safe pull: the way in to {} wakes {}{}; with it {}",
                                              target->GetName(),
                                              firstWoken ? firstWoken->GetName() : std::string("another pack"),
                                              wokenBoss ? " and brings " + wokenBoss->GetName() : std::string(),
                                              wokenBoss ? Acore::StringFormat("{} mobs, a boss", guaranteed.size())
                                                        : judged)
                        : Acore::StringFormat("no safe pull: {} as it comes: {}", target->GetName(), judged);
                LogPullBlocked(bot, "the pull as it comes refused: " + _planVerdictWhy, target);
            }
            else
            {
                chosenPack = guaranteed;
                chosen = EstimatePack(chosenPack, bot, groupDps, estimateMode);
                chosenOverlaps.clear();
                _planCached = target->GetGUID();
                _planPullBack = back;
                _planLos = false;
                _planSingle = false;
                _planKillFor.Clear();
                _planFightFor.Clear();
                _planFightNote.clear();
                _planGuaranteed = true;
                fallback = lastResort = found = true;
                _planReason = Acore::StringFormat(
                    "the pull as it comes{}: {}",
                    firstWoken ? " with what the way in wakes (" + firstWoken->GetName() + ")" : std::string(), judged);
                LOG_INFO("playerbots", "[pull as it comes] {}: {} ({} mobs with the way in): {}; {} it",
                         bot->GetName(), target->GetName(), chosenPack.size(), judged,
                         back ? "pulls back" : "walks in on");
            }
        }
    }

    // [b32_smpull] A big pack is split, not walked round (user 2026-10-25: «мы дали задачу всех боссов убивать...
    // почему обойти, а не бить?»). A good group peels the chain from its edge: the nearest mob, pulled at range with a
    // single-target spell and fought back on the line from it through the tank (KILL_SPOT_RETREAT_STEP at a time,
    // SPLIT_RETREATS at most: the step back round a corner takes the rest out of sight), where the healer sees,
    // reached by walks that wake nobody else; [lead111] what comes is taken when the leader's estimate carries it
    // (JudgeMobs: no peel size, no cost cap). Searched on every look with no plan; a failed search of the pack makes
    // the refusal hopeless (the
    // harness's detour round it, hopeless_detour, is the last resort). lbrs-n 20261025-221216-lbrs-n-5D-10: the
    // Firebrand Grunt chain "brings 13 mobs over the cap 9 at cost 14.45" was hopeless at the first look and the
    // harness walked round it; strat-h King's Square ("22 mobs over 18"); sm-h Mograine's chapel (above, linked trash).
    constexpr uint32 SPLIT_CANDIDATES = 6;
    constexpr uint32 SPLIT_RETREATS = 3;
    constexpr uint32 SPLIT_MEMO_GAP_MS = 2 * MINUTE * IN_MILLISECONDS;
    struct SplitMemo
    {
        std::vector<ObjectGuid> pack;
        uint32 fails = 0;
        uint32 firstFailMs = 0;
        uint32 lastTryMs = 0;
    };
    static thread_local std::unordered_map<uint64, SplitMemo> splitMemos;  // per map thread, by tank
    // [lead111] a split is looked for whenever no plan stands (no size trigger), every look (no retry clock)
    bool const splitWanted = !found && toppedUp && !boss && !targetPackBoss;
    (void)smallestPull;
    SplitMemo* splitMemo = nullptr;
    if (splitWanted)
    {
        SplitMemo& memo = splitMemos[bot->GetGUID().GetRawValue()];
        bool const samePack = memo.lastTryMs && getMSTimeDiff(memo.lastTryMs, now) <= SPLIT_MEMO_GAP_MS &&
                              std::find(memo.pack.begin(), memo.pack.end(), target->GetGUID()) != memo.pack.end();
        if (!samePack)
        {
            memo = SplitMemo();
            memo.pack.push_back(target->GetGUID());
            for (Unit* mob : adds)
                memo.pack.push_back(mob->GetGUID());
        }
        splitMemo = &memo;
    }
    if (splitMemo)
    {
        splitMemo->lastTryMs = now ? now : 1;
        // [lead111] a peel is taken when the leader's estimate carries it (JudgeMobs), no cost cap
        auto const peelFits = [this](std::vector<Unit*> const& peel)
        {
            float overload = 0.0f;
            std::string judged;
            return JudgeMobs(peel, overload, judged);
        };
        std::vector<Unit*> candidates(adds);
        candidates.push_back(target);
        std::sort(candidates.begin(), candidates.end(),
                  [this](Unit* a, Unit* b) { return bot->GetExactDist(a) < bot->GetExactDist(b); });
        // [b44_packs] The edge first (user 2026-10-26: pull the outermost mob, from the side where it touches the
        // fewest others). A candidate comes with its linked formation (FormationBrings: creature_formations groupAI,
        // the core's CreatureGroup::MemberEngagingTarget): that unit is what is peeled. Its touches: the other pack
        // mobs within the assistance radius (CreatureFamilyAssistanceRadius, Creature::CallAssistance) plus the
        // fight's drift of any of its members. The fewest touches are tried first (then the nearest); what really
        // comes is counted by CollectPack with the core's own rules, as before. A linked unit bigger than
        // SPLIT_MAX_MOBS (strat-h King's Square: 7-mob Cadaver / Skeleton formations, groupAI 3, "round a corner
        // fewest 7" over 6, hopeless for 150 s) is peeled whole when it is within the run's size cap
        // (lastResortMobs: the configured cap, or what the group proved) and touches nothing else (the pack that
        // comes is the unit); the split's cost cap still applies.
        float const edgeRadius =
            std::max(sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS), 0.0f) + PACK_SOCIAL_DRIFT;
        struct EdgeInfo
        {
            uint32 touches = 0;
            uint32 unit = 1;
        };
        std::unordered_map<Unit*, EdgeInfo> edgeOf;
        for (Unit* candidate : candidates)
        {
            if (!candidate)
                continue;
            std::vector<Unit*> unit = {candidate};
            Creature* const unitCreature = candidate->ToCreature();
            CreatureGroup const* formation = unitCreature ? unitCreature->GetFormation() : nullptr;
            if (formation && FormationBrings(unitCreature, formation))
            {
                for (auto const& linked : formation->GetMembers())
                {
                    Unit* const linkedUnit = linked.first;
                    if (linkedUnit != candidate &&
                        std::find(candidates.begin(), candidates.end(), linkedUnit) != candidates.end())
                        unit.push_back(linkedUnit);
                }
            }
            EdgeInfo info;
            info.unit = uint32(unit.size());
            for (Unit* other : candidates)
            {
                if (!other || std::find(unit.begin(), unit.end(), other) != unit.end())
                    continue;
                if (std::any_of(unit.begin(), unit.end(),
                                [other, edgeRadius](Unit* member) { return member->GetExactDist(other) <= edgeRadius; }))
                    ++info.touches;
            }
            edgeOf[candidate] = info;
        }
        auto const edgeInfo = [&edgeOf](Unit* candidate) -> EdgeInfo
        {
            auto const found = edgeOf.find(candidate);
            return found != edgeOf.end() ? found->second : EdgeInfo{~uint32(0), 1};
        };
        std::stable_sort(candidates.begin(), candidates.end(), [&edgeInfo](Unit* a, Unit* b)
                         { return edgeInfo(a).touches < edgeInfo(b).touches; });
        uint32 tried = 0;
        size_t fewest = 0;  // the fewest a candidate brought (the failure's reason)
        // [b35_lbrs2] The peel round a corner (user 2026-10-26, brs-h 20261026-024524-brs-h-5D-8, the Firebrand hall by
        // Voone: 15 mobs at cost 30.16, "no split in 24 searches", every peel "fewest 14"). Real players pull one
        // pair at a time at range from a corner: the pair runs to the tank out of the next pair's sight, and the
        // periodic assistance call (10 yd, every 3 s) comes only from where the fighting mob is - on its walk and at
        // the kill spot. The straight peel above counts the pair's casters where they stand and cast (KeepsRange:
        // "draws fight", the hall's chain from there); round a corner they walk to the tank. So each candidate is
        // also tried as a line-of-sight pull: its tank spot hidden from the pack (FindLosPullSpot, as PlanLosPull),
        // the pack counted there in CollectPack's losPull mode (the walks' calls: [b34_est]), at most SPLIT_MAX_MOBS,
        // its walks waking nobody else and under the split's cost cap. A caster candidate is tried this way only.
        constexpr uint32 SPLIT_LOS_CANDIDATES = 4;  // line-of-sight spot searches per split search
        bool const losSplit = botAI->HasStrategy("pull back", BOT_STATE_COMBAT) && bot->GetGroup() &&
                              bot->GetMap() && bot->GetMap()->IsNonRaidDungeon();
        uint32 losTried = 0;
        size_t losFewest = 0;  // the fewest a line-of-sight peel brought
        for (Unit* candidate : candidates)
        {
            if (found || tried >= SPLIT_CANDIDATES)
                break;
            Creature* const creature = candidate ? candidate->ToCreature() : nullptr;
            // a caster that keeps its range does not come back to the kill spot: crowd control's, not a peel
            // ([b35_lbrs2] but round a corner it walks to the tank: tried there only)
            if (!creature || IsBossMob(candidate) || IsCrowdControlled(candidate) ||
                PrePullCc::IsClaimed(bot, candidate->GetGUID()) ||
                CarefulPlanVerdict::PullOutOfReach(bot->GetGUID(), candidate->GetGUID()))
                continue;
            bool const losOnly = KeepsRange(creature);  // [b35_lbrs2]
            if (losOnly && (!losSplit || losTried >= SPLIT_LOS_CANDIDATES || LosPullBanned(bot, candidate)))
                continue;
            if (!losOnly)
                ++tried;
            Position castSpot;
            if (!losOnly && !PullBackSpot(candidate, castSpot, true))
                continue;
            float const dx = bot->GetPositionX() - candidate->GetPositionX();
            float const dy = bot->GetPositionY() - candidate->GetPositionY();
            float const length = std::sqrt(dx * dx + dy * dy);
            if (length < 1.0f)
                continue;
            for (uint32 step = 0; step <= SPLIT_RETREATS && !found && !losOnly; ++step)
            {
                Position option = bot->GetPosition();
                if (step)
                {
                    float const back = KILL_SPOT_RETREAT_STEP * float(step);
                    float const x = bot->GetPositionX() + dx / length * back;
                    float const y = bot->GetPositionY() + dy / length * back;
                    float z = 0.0f;
                    if (!GroundNear(bot, x, y, bot->GetPositionZ(), PULL_PATH_HEIGHT, z))
                        break;  // the way back ends here
                    option = Position(x, y, z);
                    if (!HealerSees(bot, option))
                        continue;
                }
                CollectPack(botAI, bot, candidate, castSpot, possible, pack, FightAt(option), measured, false,
                            overlapsOut);
                addCasterStands();
                if (std::find(pack.begin(), pack.end(), candidate) == pack.end())
                    pack.insert(pack.begin(), candidate);
                if (!fewest || pack.size() < fewest)
                    fewest = pack.size();
                if (std::any_of(pack.begin(), pack.end(), [](Unit* mob) { return IsBossMob(mob); }))
                    continue;  // [lead111] no size bound: the estimate below judges it
                // the walk to the cast spot, and from it back to the kill spot, wakes nobody outside what comes
                if (walkPulls(candidate, bot->GetPosition(), castSpot))
                    continue;
                if (step)
                {
                    bool walkable = false;
                    float const straight = castSpot.GetExactDist(option);
                    if (WalkEntersAggro(botAI, bot, possible, castSpot, option, pack,
                                        straight * 2.0f + KILL_SPOT_RETREAT_STEP, CAREFUL_AGGRO_MARGIN, walkable) ||
                        !walkable)
                        continue;
                }
                PackEstimate const estimate = EstimatePack(pack, bot, groupDps, estimateMode);
                if (estimate.boss || !peelFits(pack))
                    continue;

                _planCached = candidate->GetGUID();
                _planPullBack = true;
                _planSingle = true;
                _planLos = false;
                _planKillFor = candidate->GetGUID();
                _planKill = option;
                chosen = estimate;
                chosenPack = pack;
                chosenOverlaps = overlapsOut ? overlaps : std::vector<PackOverlap>();
                fallback = lastResort = found = true;
                _planVerdictHopeless = false;
                _planVerdictWhy.clear();
                _planReason = Acore::StringFormat("split: {} peeled off the {}-mob chain of {} ({} come, cost "
                                                  "{:.2f}, fought {:.0f} yd back)",
                                                  candidate->GetName(), adds.size() + 1, target->GetName(),
                                                  pack.size(), estimate.cost, KILL_SPOT_RETREAT_STEP * float(step));
                LOG_INFO("playerbots", "[b32_smpull] split_pull: {}: {} (failed searches before: {})", bot->GetName(),
                         _planReason, splitMemo->fails);
                LOG_INFO("playerbots", "[b44_packs] edge pull {} from pack of {}: touches {} (linked unit {}, {} come, "
                         "cost {:.2f}, pulled at range from ({:.1f} {:.1f} {:.1f}), fought {:.0f} yd back)",
                         candidate->GetName(), adds.size() + 1, edgeInfo(candidate).touches, edgeInfo(candidate).unit,
                         pack.size(), estimate.cost, castSpot.GetPositionX(), castSpot.GetPositionY(),
                         castSpot.GetPositionZ(), KILL_SPOT_RETREAT_STEP * float(step));
                splitMemo->fails = 0;
                splitMemo->firstFailMs = 0;
            }

            // [b35_lbrs2] the same candidate peeled round a corner (see SPLIT_LOS_CANDIDATES)
            if (found || !losSplit || losTried >= SPLIT_LOS_CANDIDATES || LosPullBanned(bot, candidate))
                continue;
            ++losTried;
            Position losCast;
            if (!PullBackSpot(candidate, losCast))  // the cast spot of a line-of-sight pull (StartLosPull)
                continue;
            // the pack at the cast spot: what the tank's spot must be hidden from and out of the aggro of
            CollectPack(botAI, bot, candidate, losCast, possible, pack, FightHere(losCast), measured, true,
                        overlapsOut);
            if (std::find(pack.begin(), pack.end(), candidate) == pack.end())
                pack.insert(pack.begin(), candidate);
            std::vector<Unit*> others;
            for (ObjectGuid const& guid : possible)
            {
                Unit* mob = botAI->GetUnit(guid);
                if (mob && IsIdleEnemy(bot, mob) && std::find(pack.begin(), pack.end(), mob) == pack.end())
                    others.push_back(mob);
            }
            Position losSpot;
            Position losGather;
            // [b40_mgt] ... else the corner this group already pulled round (FindLosPullCornerSpot)
            // [b51_strat] first the tank's own cleared trail, out of every idle mob's reach + assist radius
            float const clearMargin = std::max(sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS), 0.0f);
            if (!(clearMargin > 0.0f &&
                  FindLosPullTrailSpot(bot, pack, others, losCast, losSpot, losGather, clearMargin)) &&
                !FindLosPullSpot(bot, pack, others, losCast, losSpot, losGather) &&
                !FindLosPullCornerSpot(bot, pack, others, losCast, losSpot, losGather) &&
                !FindLosPullTrailSpot(bot, pack, others, losCast, losSpot, losGather))  // [b47_plan]
            {
                LOG_DEBUG("playerbots", "[b35_lbrs2] split_los: {}: no hidden spot near the cast spot for {} ({} mobs)",
                          bot->GetName(), candidate->GetName(), pack.size());
                continue;
            }
            // the fight is at the tank's spot round the corner: the pack counted there (the walks' calls included)
            CollectPack(botAI, bot, candidate, losCast, possible, pack, FightAt(losSpot), measured, true, overlapsOut);
            if (std::find(pack.begin(), pack.end(), candidate) == pack.end())
                pack.insert(pack.begin(), candidate);
            if (!losFewest || pack.size() < losFewest)
                losFewest = pack.size();
            // [b38_scholo] Round a corner the pack comes to the tank's hidden spot one by one and is fought there,
            // out of the next pack's sight: what a player takes that way is the run's own size cap (lastResortMobs,
            // the adaptive cap), up to SPLIT_LOS_MAX_MOBS, not the straight peel's 4. scholo-h
            // 20261026-050341-scholo-h-5B-3: "Diseased Ghoul / Reanimated Corpse brings 6 mobs round the corner at
            // (247.8 186.2 97.1), over 4" (the group proven 9.9 mobs over 13 clean fights, cap 9); the planner fell
            // to a lone-mob last resort expecting 7 and 14 came. The cost cap, the walks' checks below still apply.
            if (std::any_of(pack.begin(), pack.end(), [](Unit* mob) { return IsBossMob(mob); }))  // [lead111]
            {
                LOG_DEBUG("playerbots", "[b35_lbrs2] split_los: {}: {} brings {} mobs round the corner at ({:.1f} "
                          "{:.1f} {:.1f}), over {}", bot->GetName(), candidate->GetName(), pack.size(),
                          losSpot.GetPositionX(), losSpot.GetPositionY(), losSpot.GetPositionZ(), "a boss");
                continue;
            }
            // the walk to the cast spot, and from it to the tank's spot, wakes nobody outside what comes
            if (walkPulls(candidate, bot->GetPosition(), losCast))
                continue;
            bool losWalkable = false;
            float const losStraight = losCast.GetExactDist(losSpot);
            if (losStraight > CAREFUL_AGGRO_MARGIN &&
                (WalkEntersAggro(botAI, bot, possible, losCast, losSpot, pack,
                                 losStraight * 2.0f + KILL_SPOT_RETREAT_STEP, CAREFUL_AGGRO_MARGIN, losWalkable) ||
                 !losWalkable))
                continue;
            PackEstimate const losEstimate = EstimatePack(pack, bot, groupDps, estimateMode, true);
            if (losEstimate.boss || !peelFits(pack))
                continue;

            _planCached = candidate->GetGUID();
            _planPullBack = false;
            _planSingle = false;
            _planLos = true;
            _planKillFor.Clear();
            _losSpot = losSpot;
            _losGather = losGather;
            _losPack.clear();
            for (Unit* mob : pack)
                _losPack.push_back(mob->GetGUID());
            chosen = losEstimate;
            chosenPack = pack;
            chosenOverlaps = overlapsOut ? overlaps : std::vector<PackOverlap>();
            fallback = lastResort = found = true;
            splitPlan = true;  // [b36_sv] the fallback cost gate takes it under splitCostCap, as its search did
            _planVerdictHopeless = false;
            _planVerdictWhy.clear();
            _planReason = Acore::StringFormat("split round the corner: {} peeled off the {}-mob chain of {} ({} come, "
                                              "cost {:.2f}, tank spot {:.0f} yd from the cast spot)",
                                              candidate->GetName(), adds.size() + 1, target->GetName(), pack.size(),
                                              losEstimate.cost, losStraight);
            LOG_INFO("playerbots", "[b35_lbrs2] split_los_pull: {}: {}, tank spot ({:.1f} {:.1f} {:.1f}), group spot "
                     "({:.1f} {:.1f} {:.1f}) (failed searches before: {})", bot->GetName(), _planReason,
                     losSpot.GetPositionX(), losSpot.GetPositionY(), losSpot.GetPositionZ(),
                     losGather.GetPositionX(), losGather.GetPositionY(), losGather.GetPositionZ(), splitMemo->fails);
            LOG_INFO("playerbots", "[b44_packs] edge pull {} from pack of {}: touches {} (linked unit {}, {} come, cost "
                     "{:.2f}, round the corner to ({:.1f} {:.1f} {:.1f}))", candidate->GetName(), adds.size() + 1,
                     edgeInfo(candidate).touches, edgeInfo(candidate).unit, pack.size(), losEstimate.cost,
                     losSpot.GetPositionX(), losSpot.GetPositionY(), losSpot.GetPositionZ());
            splitMemo->fails = 0;
            splitMemo->firstFailMs = 0;
        }
        if (!found)
        {
            ++splitMemo->fails;
            if (!splitMemo->firstFailMs)
                splitMemo->firstFailMs = now ? now : 1;
            LOG_INFO("playerbots", "[b32_smpull] split_failed: {}: {} ({} mobs): no peel the estimate carries from "
                     "{} candidates (fewest {}; [b35_lbrs2] round a corner {} tried, fewest {}), search {}",
                     bot->GetName(), target->GetName(), adds.size() + 1, tried, fewest, losTried, losFewest,
                     splitMemo->fails);
        }
    }

    // [b33_aoepull] The AoE pull of a REQUIRED route blocker (user 2026-10-26: «почему обойти, а не бить?»; tanks
    // gather the adds, everyone AoEs the stack, in every dungeon). strat-n 20261026-000434-strat-n-5A-2 (and strat-h)
    // King's Square: the Ravaged Cadaver chain wakes 19-36 mobs over the cap 18, every split search failed ("no peel of
    // at most 4 mobs from 6 candidates (fewest 14-22)"), the harness rightly refused its walk-in ([b33_bodypull]) and
    // the run stalled on the only way on. A pack the split did not peel, that the harness marks required (AoePull::
    // MarkRequired: on the only way to a boss / event / required target), is pulled whole when all of these hold:
    //  (1) the group is fully ready: everyone alive and within AOE_PULL_GATHER_RANGE of the tank, topped up and nobody
    //      sitting below its line or buffing (toppedUp, IsAnyoneSittingBelowLine), every healer within
    //      AOE_PULL_HEALER_RANGE of the tank and in its sight, the healer seeing the cast spot and the stack spot;
    //  (2) [lead263] the group proved that many mobs in this run (ProvenFor: the biggest pull it survived cleanly,
    //      with what it left), or its AoE carries the stack: a tank with AoE threat, the members' area spells
    //      (MemberAreaDps) and the stack's fight played out (EstimatePack) within what the tank carries
    //      (TankCarries) - instead of the 0.7 proven share, the 1.5 cap scale, the 2 AoE dealers, the 4x proven
    //      and the 18-tank-health ceiling (mgt-h 20261026-000434-mgt-h-5G-16: 20-21 Sunblade mobs over the cap 18);
    //  (3) a stack spot: KILL_SPOT_RETREAT_STEP at a time (AOE_PULL_RETREATS at most) back on the line from the pulled
    //      mob through the tank, the healer sees it, the walks there wake nobody outside what comes; one round a corner
    //      (out of the pulled mob's sight: the chain turns the corner and stacks) is preferred to a plain back pull.
    // The plan pulls the nearest mob at range with a single-target spell (the chain follows) and fights at the spot,
    // logged as aoe_pull_plan (predicted, proven, spot) and published for the harness (AoePull::NotePlan). With (2)
    // and (3) holding but (1) not, the refusal is a readiness hold ("group not ready", RefusalIsReadiness), never a
    // dead end; with (2) or (3) failing, the hold / hopeless verdict below stands as before (aoe_pull_rejected).
    constexpr uint32 AOE_PULL_CANDIDATES = 4;
    constexpr uint32 AOE_PULL_RETREATS = 4;
    constexpr float AOE_PULL_GATHER_RANGE = 40.0f;
    constexpr float AOE_PULL_HEALER_RANGE = 30.0f;
    constexpr uint32 AOE_PULL_SEARCH_MS = 2 * IN_MILLISECONDS;
    constexpr uint32 AOE_PULL_LOG_MS = 10 * IN_MILLISECONDS;
    // [b34_lbrs] The stack spot on the tank's own trail (MovementAction::TankTrailPoints, every 2 yd): back the way the
    // group came, a corridor it walked already. lbrs-n 20261026-014521-lbrs-n-5F-10 (build 34, Prot warrior Rtwarbb):
    // the Firebrand hall chain (13-14 mobs, cost 12.29, over the guarantee's 9.00 ceiling the paladin run 20261025-
    // 184229-lbrs-n-5A-14 passed at 8.8 on the same chain) met (2) at once ("proven 0.9, cap 18, tank aoe yes, aoe dps
    // 2"), but every spot on the straight line from the mob through the tank was "the healer sees no stack spot": the
    // line ran into the hall's walls, while the corridor it came by (-2,-443 -> 21,-472) bends out of the hall's
    // sight. A trail spot the healer does not see from where it stands is taken with a group spot further back on the
    // trail that sees it (the group waits there, as a line-of-sight pull's group spot, and the tank stacks the chain at
    // the trail spot): the healer stands where it sees the fight before the pull, not after a walk round a corner.
    constexpr float AOE_PULL_TRAIL_MIN = 10.0f;          // the nearest trail spot
    constexpr float AOE_PULL_TRAIL_MAX = 45.0f;          // and the farthest (straight from the tank, both)
    constexpr float AOE_PULL_TRAIL_WALK_MAX = 90.0f;     // the trail walked back at most
    constexpr float AOE_PULL_TRAIL_STEP = 6.0f;          // trail spots this far apart along the trail
    constexpr float AOE_PULL_TRAIL_FLOOR = 5.0f;         // a trail point this far above/below the tank: another floor
    constexpr size_t AOE_PULL_TRAIL_SPOTS = 6;
    constexpr float AOE_PULL_TRAIL_GATHER_MIN = 4.0f;    // the group spot, this far further back on the trail
    constexpr float AOE_PULL_TRAIL_GATHER_MAX = 16.0f;
    struct AoeMemo
    {
        std::vector<ObjectGuid> pack;
        uint32 searchMs = 0;  // the last full spot search; in between only its spot is checked again
        ObjectGuid mob;       // the spot it found (empty: none)
        Position spot;
        bool corner = false;
        bool viaGather = false;  // [b34_lbrs] a trail spot with its group spot behind it
        Position gather;
        std::string failWhy;
        uint32 logMs = 0;
    };
    static thread_local std::unordered_map<uint64, AoeMemo> aoeMemos;  // per map thread, by tank
    std::string aoeReadinessWhy;  // (2) and (3) hold, (1) does not: the refusal below is a readiness hold
    if (!found && splitWanted)
    {
        std::vector<ObjectGuid> packGuids = {target->GetGUID()};
        for (Unit* mob : adds)
            packGuids.push_back(mob->GetGUID());
        // [lead5] LEADER_ONE_PICKER: on a leader look the leader's own required marks (LeaderInputs::required), not
        // the AoePull registry the harness wrote for the tank's thread
        PullPlanner::LeaderInputs const* const aoeInputs =
            _snap && Leader::Leads(Leader::LEADER_ONE_PICKER) ? _snap->Leader() : nullptr;
        bool const aoeRequired =
            aoeInputs ? std::any_of(packGuids.begin(), packGuids.end(),
                                    [aoeInputs](ObjectGuid const& guid)
                                    {
                                        return std::find(aoeInputs->required.begin(), aoeInputs->required.end(),
                                                         guid) != aoeInputs->required.end();
                                    })
                      : AoePull::IsRequired(bot->GetGUID(), packGuids);
        if (aoeRequired)
        {
            AoeMemo& memo = aoeMemos[bot->GetGUID().GetRawValue()];
            if (std::find(memo.pack.begin(), memo.pack.end(), target->GetGUID()) == memo.pack.end())
            {
                memo = AoeMemo();
                memo.pack = packGuids;
            }

            // the comp and who is here
            bool const tankAoe = HasAoeThreat(bot);
            uint32 aoeDps = 0;
            std::string missing;
            if (Group* group = bot->GetGroup())
            {
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    if (!member || member == bot)
                        continue;
                    if (missing.empty() &&
                        (!IsPresent(bot, member) || bot->GetExactDist(member) > AOE_PULL_GATHER_RANGE))
                        missing = Acore::StringFormat("{} is not with the group ({:.0f} yd{})", member->GetName(),
                                                      member->GetMap() == bot->GetMap() ? bot->GetExactDist(member)
                                                                                        : 0.0f,
                                                      member->IsAlive() ? "" : ", dead");
                    else if (missing.empty() && PlayerbotAI::IsHeal(member) && !PlayerbotAI::IsTank(member) &&
                             (bot->GetExactDist(member) > AOE_PULL_HEALER_RANGE || !bot->IsWithinLOSInMap(member)))
                        missing = Acore::StringFormat("healer {} {:.1f} yd from the tank{}", member->GetName(),
                                                      bot->GetExactDist(member),
                                                      bot->IsWithinLOSInMap(member) ? "" : ", out of sight");
                    if (IsPresent(bot, member) && !PlayerbotAI::IsTank(member) && !PlayerbotAI::IsHeal(member) &&
                        HasAoeDamage(member))
                        ++aoeDps;
                }
            }

            struct AoeChoice
            {
                Unit* mob = nullptr;
                Position cast;
                Position spot;
                bool corner = false;
                uint32 step = 0;
                size_t predicted = 0;
                std::vector<Unit*> pack;
                std::vector<PackOverlap> overlaps;
                bool viaGather = false;  // [b34_lbrs] a trail spot: the group waits at `gather`, behind it
                Position gather;
            };
            AoeChoice choice;
            std::string failWhy;
            // (2) and the walks of (3) for one cast spot and stack spot ([b34_lbrs] `gather`: a trail spot's group
            // spot)
            auto const tryOption = [&](Unit* candidate, Position const& castSpot, Position const& option, bool corner,
                                       uint32 step, Position const* gather = nullptr) -> bool
            {
                CollectPack(botAI, bot, candidate, castSpot, possible, pack, FightAt(option), measured, false,
                            overlapsOut);
                addCasterStands();
                if (std::find(pack.begin(), pack.end(), candidate) == pack.end())
                    pack.insert(pack.begin(), candidate);
                if (std::any_of(pack.begin(), pack.end(), [](Unit* mob) { return IsBossMob(mob); }))
                {
                    failWhy = "the pull brings a boss";
                    return false;
                }
                if (walkPulls(candidate, bot->GetPosition(), castSpot))
                {
                    failWhy = Acore::StringFormat("the walk to the cast spot wakes {}",
                                                  lastWoken ? lastWoken->GetName() : std::string("another pack"));
                    return false;
                }
                bool walkable = false;
                float const straight = castSpot.GetExactDist(option);
                if (WalkEntersAggro(botAI, bot, possible, castSpot, option, pack,
                                    straight * 2.0f + KILL_SPOT_RETREAT_STEP, CAREFUL_AGGRO_MARGIN, walkable) ||
                    !walkable)
                {
                    failWhy = "the walk back to the stack spot wakes another pack";
                    return false;
                }
                size_t const predicted = std::max(pack.size(), adds.size() + 1);
                // [lead263] proven, or the AoE fight of the stack carried (b34_strat: strat-h
                // 20261026-014521-strat-h-5E-1 took a 26-mob chain at cost 45.50 on the comp alone and wiped; the AoE
                // fight is judged here)
                bool const provenCarries = proven.mobs >= float(predicted);
                std::string carries = "no AoE estimate";
                bool compCarries = false;
                if (!provenCarries && tankAoe)
                {
                    PackEstimate stack = EstimatePack(pack, bot, groupDps, estimateMode);
                    if (stack.aoeSec > 0.0f)
                    {
                        stack.cost = stack.aoeCost;
                        stack.spellCost = stack.aoeSpellCost;
                        stack.fightSec = stack.aoeSec;
                        stack.drop = stack.aoeDrop;
                        compCarries = TankCarries(stack, support, &carries);
                    }
                }
                if (!provenCarries && !compCarries)
                {
                    failWhy = Acore::StringFormat("{} mobs predicted: proven {:.1f}, tank aoe {}, aoe dps {}: {}",
                                                  predicted, proven.mobs, tankAoe ? "yes" : "no", aoeDps, carries);
                    return false;
                }
                if (choice.mob && (choice.corner || !corner))
                    return true;  // a spot as good is kept
                choice.mob = candidate;
                choice.cast = castSpot;
                choice.spot = option;
                choice.corner = corner;
                choice.step = step;
                choice.predicted = predicted;
                choice.pack = pack;
                choice.overlaps = overlapsOut ? overlaps : std::vector<PackOverlap>();
                choice.viaGather = gather != nullptr;  // [b34_lbrs]
                choice.gather = gather ? *gather : Position();
                return true;
            };
            // the stack spot `step` steps back on the line from `candidate` through the tank (false: no ground)
            auto const backSpot = [this](Unit* candidate, uint32 step, Position& option) -> bool
            {
                float const dx = bot->GetPositionX() - candidate->GetPositionX();
                float const dy = bot->GetPositionY() - candidate->GetPositionY();
                float const length = std::sqrt(dx * dx + dy * dy);
                if (length < 1.0f)
                    return false;
                float const back = KILL_SPOT_RETREAT_STEP * float(step);
                float const x = bot->GetPositionX() + dx / length * back;
                float const y = bot->GetPositionY() + dy / length * back;
                float z = 0.0f;
                if (!GroundNear(bot, x, y, bot->GetPositionZ(), PULL_PATH_HEIGHT, z))
                    return false;
                option = Position(x, y, z);
                return true;
            };
            auto const isCorner = [](Unit* candidate, Position const& option)
            {
                return !candidate->IsWithinLOS(option.GetPositionX(), option.GetPositionY(),
                                               option.GetPositionZ() + 2.0f);
            };
            // [b34_lbrs] the stack spots on the tank's trail, AOE_PULL_TRAIL_STEP apart along it, AOE_PULL_TRAIL_MIN to
            // AOE_PULL_TRAIL_MAX from the tank, on its floor: one the healer sees from where it stands as it is,
            // else with a group spot AOE_PULL_TRAIL_GATHER_MIN..MAX further back on the trail that sees it (the line
            // of sight of a line-of-sight pull's group spot). The same for every candidate: looked up once.
            struct TrailSpot
            {
                Position spot;
                bool viaGather = false;
                Position gather;
            };
            std::vector<TrailSpot> trailSpots;
            size_t trailPoints = 0;
            bool trailLooked = false;
            bool const gatherPossible = botAI->HasStrategy("pull back", BOT_STATE_COMBAT) && bot->GetMap() &&
                                        bot->GetMap()->IsNonRaidDungeon();
            auto const collectTrailSpots = [&]()
            {
                if (trailLooked)
                    return;
                trailLooked = true;
                std::vector<G3D::Vector3> const trail = TankTrailPoints(bot);
                trailPoints = trail.size();
                Map* const map = bot->GetMap();
                if (trail.size() < 2 || !map)
                    return;
                G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
                std::vector<float> walkedBack(trail.size(), 0.0f);
                float walked = 0.0f;
                G3D::Vector3 previous = here;
                for (size_t i = trail.size(); i-- > 0;)
                {
                    walked += (trail[i] - previous).length();
                    previous = trail[i];
                    walkedBack[i] = walked;
                }
                float nextAt = AOE_PULL_TRAIL_MIN;
                for (size_t i = trail.size(); i-- > 0 && trailSpots.size() < AOE_PULL_TRAIL_SPOTS;)
                {
                    // the trail zigzags where earlier pulls of the hall walked out and back: walked up to
                    // AOE_PULL_TRAIL_WALK_MAX, a spot is AOE_PULL_TRAIL_MIN..MAX from the tank in a straight line
                    if (walkedBack[i] > AOE_PULL_TRAIL_WALK_MAX)
                        break;
                    float const straight = (trail[i] - here).length();
                    if (walkedBack[i] < nextAt || std::fabs(trail[i].z - here.z) > AOE_PULL_TRAIL_FLOOR ||
                        straight < AOE_PULL_TRAIL_MIN || straight > AOE_PULL_TRAIL_MAX)
                        continue;  // too near (the trail looped back to the tank), too far, or another floor
                    TrailSpot entry;
                    entry.spot = Position(trail[i].x, trail[i].y, trail[i].z);
                    if (!HealerSees(bot, entry.spot))
                    {
                        if (!gatherPossible)
                            continue;
                        for (size_t j = i; j-- > 0 && !entry.viaGather;)
                        {
                            float const behind = walkedBack[j] - walkedBack[i];
                            if (behind > AOE_PULL_TRAIL_GATHER_MAX)
                                break;
                            if (behind < AOE_PULL_TRAIL_GATHER_MIN ||
                                std::fabs(trail[j].z - trail[i].z) > LOS_PULL_GATHER_FLOOR ||
                                !map->isInLineOfSight(trail[i].x, trail[i].y, trail[i].z + LOS_PULL_EYE_HEIGHT,
                                                      trail[j].x, trail[j].y, trail[j].z + LOS_PULL_EYE_HEIGHT,
                                                      bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                                      VMAP::ModelIgnoreFlags::Nothing))
                                continue;
                            entry.viaGather = true;
                            entry.gather = Position(trail[j].x, trail[j].y, trail[j].z);
                        }
                        if (!entry.viaGather)
                            continue;
                    }
                    nextAt = walkedBack[i] + AOE_PULL_TRAIL_STEP;
                    trailSpots.push_back(entry);
                }
            };

            bool const search = !memo.searchMs || getMSTimeDiff(memo.searchMs, now) >= AOE_PULL_SEARCH_MS;
            if (search)
            {
                memo.searchMs = now ? now : 1;
                std::vector<Unit*> candidates(adds);
                candidates.push_back(target);
                std::sort(candidates.begin(), candidates.end(),
                          [this](Unit* a, Unit* b) { return bot->GetExactDist(a) < bot->GetExactDist(b); });
                uint32 tried = 0;
                for (Unit* candidate : candidates)
                {
                    if (tried >= AOE_PULL_CANDIDATES || (choice.mob && choice.corner))
                        break;
                    Creature* const creature = candidate ? candidate->ToCreature() : nullptr;
                    if (!creature || IsBossMob(candidate) || IsCrowdControlled(candidate) ||
                        PrePullCc::IsClaimed(bot, candidate->GetGUID()) || KeepsRange(creature) ||
                        CarefulPlanVerdict::PullOutOfReach(bot->GetGUID(), candidate->GetGUID()))
                        continue;
                    ++tried;
                    Position castSpot;
                    if (!PullBackSpot(candidate, castSpot, true))
                    {
                        failWhy = Acore::StringFormat("no ranged pull of {}", candidate->GetName());
                        continue;
                    }
                    for (uint32 step = 1; step <= AOE_PULL_RETREATS && !(choice.mob && choice.corner); ++step)
                    {
                        Position option;
                        if (!backSpot(candidate, step, option))
                            break;  // the way back ends here
                        if (!HealerSees(bot, option))
                        {
                            failWhy = "the healer sees no stack spot";
                            continue;
                        }
                        tryOption(candidate, castSpot, option, isCorner(candidate, option), step);
                    }
                    // [b34_lbrs] then back along the way the group came; a corner there replaces a plain back pull
                    if (!(choice.mob && choice.corner))
                    {
                        collectTrailSpots();
                        uint32 trailStep = AOE_PULL_RETREATS;
                        for (TrailSpot const& trailSpot : trailSpots)
                        {
                            if (choice.mob && choice.corner)
                                break;
                            tryOption(candidate, castSpot, trailSpot.spot, isCorner(candidate, trailSpot.spot),
                                      ++trailStep, trailSpot.viaGather ? &trailSpot.gather : nullptr);
                        }
                    }
                }
                if (!tried && failWhy.empty())
                    failWhy = "no mob of the pack to pull at range";
                if (!choice.mob && trailLooked && trailSpots.empty() && failWhy == "the healer sees no stack spot")
                    failWhy += Acore::StringFormat(" (nor on the tank's trail: {} points, none {:.0f}-{:.0f} yd back "
                                                   "seen by the healer or from a group spot behind it)",
                                                   trailPoints, AOE_PULL_TRAIL_MIN, AOE_PULL_TRAIL_MAX);
                memo.mob = choice.mob ? choice.mob->GetGUID() : ObjectGuid::Empty;
                memo.spot = choice.spot;
                memo.corner = choice.corner;
                memo.viaGather = choice.mob && choice.viaGather;  // [b34_lbrs]
                memo.gather = choice.gather;
                memo.failWhy = choice.mob ? std::string() : failWhy;
            }
            else if (Unit* const kept = memo.mob.IsEmpty() ? nullptr : botAI->GetUnit(memo.mob);
                     kept && kept->IsAlive() && !kept->IsInCombat())
            {
                // between searches: the last search's spot, checked again
                Position castSpot;
                if (!PullBackSpot(kept, castSpot, true))
                    failWhy = Acore::StringFormat("no ranged pull of {}", kept->GetName());
                else if (memo.viaGather)  // [b34_lbrs] a trail spot: its group spot sees it (fixed ground), not the
                    tryOption(kept, castSpot, memo.spot, memo.corner, 0, &memo.gather);  // healer where it stands
                else if (!HealerSees(bot, memo.spot))
                    failWhy = "the healer sees no stack spot";
                else
                    tryOption(kept, castSpot, memo.spot, memo.corner, 0);
            }
            else
                failWhy = memo.failWhy.empty() ? std::string("no stack spot") : memo.failWhy;

            bool const logDue = !memo.logMs || getMSTimeDiff(memo.logMs, now) >= AOE_PULL_LOG_MS;
            if (choice.mob)
            {
                // (1) readiness: the spot found, the group must be all there and ready
                std::string notReady = missing;
                if (notReady.empty() && IsAnyoneSittingBelowLine(bot))
                    notReady = "a member rests below its line";
                if (notReady.empty() && !HealerSees(bot, choice.cast))
                    notReady = "the healer does not see the cast spot";
                // [b34_lbrs] a trail spot's group spot sees it: the healer waits there before the pull
                if (notReady.empty() && !choice.viaGather && !HealerSees(bot, choice.spot))
                    notReady = "the healer does not see the stack spot";

                if (notReady.empty())
                {
                    _planCached = choice.mob->GetGUID();
                    _planPullBack = true;
                    _planSingle = true;
                    _planLos = false;
                    _planKillFor = choice.mob->GetGUID();
                    _planKill = choice.spot;
                    if (choice.viaGather)
                    {
                        // [b34_lbrs] pulled as a line-of-sight pull (StartLosPull, LosPull::Register): the group
                        // takes the trail's group spot first, the tank pulls from the cast spot and stacks the
                        // chain at the trail spot, the group holds behind it until the chain came round
                        _planPullBack = false;
                        _planSingle = false;
                        _planLos = true;
                        _planKillFor.Clear();
                        _losSpot = choice.spot;
                        _losGather = choice.gather;
                        _losPack.clear();
                        for (Unit* mob : choice.pack)
                            _losPack.push_back(mob->GetGUID());
                        if (logDue)
                            LOG_INFO("playerbots", "[b34_lbrs] aoe_pull_trail: {}: {} stacked at the trail spot "
                                     "({:.1f} {:.1f} {:.1f}){}, the group waits at ({:.1f} {:.1f} {:.1f}), {:.0f} yd "
                                     "behind it", bot->GetName(), target->GetName(), choice.spot.GetPositionX(),
                                     choice.spot.GetPositionY(), choice.spot.GetPositionZ(),
                                     choice.corner ? " round a corner" : "", choice.gather.GetPositionX(),
                                     choice.gather.GetPositionY(), choice.gather.GetPositionZ(),
                                     choice.spot.GetExactDist(choice.gather));
                    }
                    chosen = EstimatePack(choice.pack, bot, groupDps, estimateMode);
                    chosenPack = choice.pack;
                    chosenOverlaps = choice.overlaps;
                    fallback = lastResort = found = true;
                    _planGuaranteed = true;  // no fallback cost cap: the proven capacity carries it
                    _planVerdictHopeless = false;
                    _planVerdictWhy.clear();
                    float const back = choice.spot.GetExactDist2d(bot->GetPositionX(), bot->GetPositionY());
                    _planReason = Acore::StringFormat("aoe_pull_plan: {} pulled at range, the {}-mob chain of {} "
                                                      "stacked {} {:.0f} yd back ({} predicted, proven {:.1f})",
                                                      choice.mob->GetName(), adds.size() + 1, target->GetName(),
                                                      choice.corner ? "round a corner" : "on a back pull", back,
                                                      choice.predicted, proven.mobs);
                    std::vector<ObjectGuid> planGuids(packGuids);
                    for (Unit* mob : choice.pack)
                        if (std::find(planGuids.begin(), planGuids.end(), mob->GetGUID()) == planGuids.end())
                            planGuids.push_back(mob->GetGUID());
                    if (!_shadow)  // [lead7] a shadow look publishes nothing
                        AoePull::NotePlan(bot->GetGUID(), planGuids);
                    _aoePlanNoted = planGuids;  // [lead5] the leader's own memory (Decision::aoePlan)
                    if (logDue)
                    {
                        memo.logMs = now ? now : 1;
                        LOG_INFO("playerbots",
                                 "[b33_aoepull] aoe_pull_plan: {}: {} ({} mobs, required route blocker): "
                                 "predicted {}, proven {:.1f} (cost {:.2f}), cap {}, spot {} "
                                 "({:.1f} {:.1f} {:.1f}) {:.0f} yd back, pulled {} at range, tank aoe {}, aoe dps {}, "
                                 "cost {:.2f}",
                                 bot->GetName(), target->GetName(), adds.size() + 1, choice.predicted, proven.mobs,
                                 proven.cost, overCapMobs, choice.corner ? "corner" : "back pull",
                                 choice.spot.GetPositionX(), choice.spot.GetPositionY(), choice.spot.GetPositionZ(),
                                 back, choice.mob->GetName(), tankAoe ? "yes" : "no", aoeDps, chosen.cost);
                    }
                }
                else
                {
                    aoeReadinessWhy = Acore::StringFormat("group not ready: aoe_pull_plan of {} ({} predicted, proven "
                                                          "{:.1f}) waits: {}", target->GetName(), choice.predicted,
                                                          proven.mobs, notReady);
                    _planVerdictHopeless = false;
                    _planVerdictWhy = aoeReadinessWhy;
                    if (logDue)
                    {
                        memo.logMs = now ? now : 1;
                        LOG_INFO("playerbots", "[b33_aoepull] aoe_pull_hold: {}: {}", bot->GetName(), aoeReadinessWhy);
                    }
                }
            }
            else if (logDue)
            {
                memo.logMs = now ? now : 1;
                LOG_INFO("playerbots", "[b33_aoepull] aoe_pull_rejected: {}: {} ({} mobs, required route blocker): {} "
                         "(proven {:.1f}, cap {}, tank aoe {}, aoe dps {}): the hold / hopeless verdict stands",
                         bot->GetName(), target->GetName(), adds.size() + 1, failWhy, proven.mobs, overCapMobs,
                         tankAoe ? "yes" : "no", aoeDps);
            }
        }
    }

    // [lead208] b182 rfc-h 5G 500-717 s: the Searing Blade room's pack was refused (overload 1.16, no split), every
    // pull on the path woke it, the run stalled. A pack the leader must clear with every option over the budget is
    // pulled the cheapest way left - the pull as it comes, the group topped up - not held for good
    if (!found && !cheapest.pack.empty())
    {
        chosenPack = cheapest.pack;
        chosen = EstimatePack(chosenPack, bot, groupDps, estimateMode);
        chosenOverlaps.clear();
        _planCached = target->GetGUID();
        _planPullBack = cheapest.back;
        _planLos = false;
        _planSingle = false;
        _planKillFor.Clear();
        _planFightFor.Clear();
        _planFightNote.clear();
        _planGuaranteed = true;
        _planVerdictHopeless = false;
        _planVerdictWhy.clear();
        fallback = lastResort = found = true;
        _planReason = Acore::StringFormat(
            "every option over the budget, the cheapest: the pull as it comes{} (overload {:.2f}): {}",
            cheapest.woken.empty() ? std::string() : " with what the way in wakes (" + cheapest.woken + ")",
            cheapest.overload, cheapest.judged);
        LOG_INFO("playerbots", "[lead208] {}: {} ({} mobs): {}", bot->GetName(), target->GetName(), chosenPack.size(),
                 _planReason);
    }

    if (!found)
    {
        // The inputs of the smallest plan against what the group takes (temporary diagnostics, wave 12 calibration).
        float const maxHealth = PlanTankHealth(bot);  // [b32_smpull]
        TankMitigation const mitigation = GetTankMitigation(bot);
        LOG_DEBUG("playerbots",
                  "{} careful pull: no manageable pull around {} ({} mobs; best {} mobs{}: landed {:.0f}/s of {:.0f} "
                  "peak, cost {:.2f} tank hp over {:.0f} s at {:.0f} group dps, heals {:.0f}/s; tank {:.0f} hp, "
                  "armor {}, avoid {:.2f}, block {:.2f}/{:.0f}; hold {:.0f}, cap {}; size {:.2f} hold {:.2f} "
                  "peak {:.2f} drain {:.2f}; drop {:.2f} comfort {:.2f} spell {:.2f}; measured dps {:.0f}, landed "
                  "scale {:.2f} melee / {:.2f} spell)",
                  bot->GetName(), target->GetName(), adds.size() + 1, best.estimate.mobs,
                  best.pullBack ? " pull back" : "", best.estimate.landedDps, AverageDtps(best.estimate, maxHealth),
                  best.estimate.cost, best.estimate.fightSec, groupDps, support.healHps, maxHealth, PlanTankArmor(bot),
                  mitigation.avoidance, mitigation.block, mitigation.blockValue, best.terms.holdMobs, maxMobs,
                  best.terms.size, best.terms.hold, best.terms.peak, best.terms.drain, best.terms.drop,
                  best.terms.comfort, best.estimate.spellCost, MeasuredGroupDps(bot), estimateMode.landedScale.melee,
                  estimateMode.landedScale.spell);
        if (packWhyRecorder.on && !targetPack.empty() && PackWhyDue(_packWhyMs, target->GetGUID(), now))
            LOG_INFO("playerbots", "{}",
                     DescribePackWhy(bot, target, targetPack, FindPackWhy(targetPack), adds.size() + 1, "refused"));

        // Crowd control may make it one (HoldForPrePullCc), once the group is ready for a pack this size.
        _planRefused = toppedUp;
        if (toppedUp)
        {
            _planVerdict = PlanVerdictState::Refused;
            // the next fight, for the rest logic: the smallest plan the pack has (what the group takes on it)
            _planVerdictFightMobs = best.mob.IsEmpty() ? uint32(adds.size() + 1) : best.estimate.mobs;
            _planVerdictCost = best.estimate.cost;
            _planVerdictSec = best.estimate.fightSec;
            _planVerdictBoss = boss;
            _planVerdictPack.push_back(target->GetGUID());
            for (Unit* mob : adds)
                _planVerdictPack.push_back(mob->GetGUID());

            // The refusal for the harness on every look, not only the stall-period one that ran the hard minimum
            // (wave 11: guarantee holds with waitSec 0 and an empty why, skipped long before the 90 s clock).
            if (!_planVerdictWhy.empty())
            {
                _refusalFor = target->GetGUID();
                _refusalWhy = _planVerdictWhy;
                _refusalHopeless = _planVerdictHopeless;
                _refusalMs = now;
            }
            else if (_refusalMs && getMSTimeDiff(_refusalMs, now) <= REFUSAL_MEMO_MS &&
                     std::find(_planVerdictPack.begin(), _planVerdictPack.end(), _refusalFor) !=
                         _planVerdictPack.end())
            {
                _planVerdictWhy = _refusalWhy;
                _planVerdictHopeless = _refusalHopeless;
            }

            if (_planVerdictWhy.empty())
                _planVerdictWhy = Acore::StringFormat("refused for now: {} ({} mobs, the smallest plan {} mobs at cost "
                                                      "{:.2f})", target->GetName(), adds.size() + 1,
                                                      _planVerdictFightMobs, _planVerdictCost);

            // The wait mirrors the guarantee's tiers exactly (ok-n build 18: "15 mobs", waitSec 192, a pack the
            // guarantee could never take): within the cap 90 s, within the over-cap scale 180 s, past it hopeless.
            // A pack that brings a boss is no guarantee's: it is pulled as his fight once the group is ready for him.
            // [lead111] no size cap makes a refusal hopeless: the estimates above (the pull as it comes, the
            // split) did
            if (boss || targetPackBoss)
                _planVerdictWhy = Acore::StringFormat("refused for now: the pack of {} brings a boss, pulled as his "
                                                      "fight once the group is ready for him", target->GetName());

            // [b32_smpull] hopeless only once the split search of this pack failed - [lead111] its last search (a
            // fact: no peel the estimate carries), no count of searches over a time (a group not topped up yet has
            // not searched: it holds, no detour)
            if (_planVerdictHopeless && !boss && !targetPackBoss)
            {
                auto const memo = splitMemos.find(bot->GetGUID().GetRawValue());
                bool const searched =
                    memo != splitMemos.end() &&
                    std::find(memo->second.pack.begin(), memo->second.pack.end(), target->GetGUID()) !=
                        memo->second.pack.end();
                uint32 const fails = searched ? memo->second.fails : 0;
                bool const exhausted = fails >= 1;
                if (!exhausted)
                {
                    _planVerdictHopeless = false;
                    _planVerdictWhy = "refused for now (no split searched yet): " + _planVerdictWhy;
                }
                else
                    _planVerdictWhy += Acore::StringFormat(" (no split in {} searches)", fails);
            }

            // [b33_aoepull] the aoe pull of a required blocker waits for the group's readiness: a hold, no dead end
            if (!aoeReadinessWhy.empty())
            {
                _planVerdictHopeless = false;
                _planVerdictWhy = aoeReadinessWhy;
            }

            _planVerdictGuaranteeWaitMs = 0;  // [lead111] no stand to wait out

            // [rest_predict_real_pull] STN92_EFFICIENCY 2026-10-25: the rest logic's next fight is the pull this
            // refusal ends in - the last resort's single or the guarantee's pack, at most lastResortMobs - not the
            // whole refused pack (st-n-92: "need 90 % for next fight (8 mobs, ~48 s)" where 3-5 mobs came for 12-13 s).
            if (!boss && !targetPackBoss && _planVerdictFightMobs > lastResortMobs)
            {
                float const scale = float(lastResortMobs) / float(_planVerdictFightMobs);
                _planVerdictSec *= scale;
                _planVerdictCost *= scale;
                _planVerdictFightMobs = lastResortMobs;
            }
        }

        return false;
    }

    _planFallback = fallback;
    // [rest_predict_real_pull] the pack about to be pulled: the next fight for the rest logic while a readiness gate
    // below holds it (isUseful publishes it with the hold, not the previous fight)
    _planChosenValid = true;
    _planChosenMobs = chosen.mobs;
    _planChosenCost = chosen.cost;
    _planChosenSec = chosen.fightSec;
    _planChosenBoss = chosen.boss;
    _planChosenRealBoss = chosen.boss && std::any_of(chosenPack.begin(), chosenPack.end(),
                                                     [](Unit* mob) { return IsEncounterBoss(mob); });
    // kept for a wait for a crossing group, which holds this plan (it is planned again a second later)
    ObjectGuid const stallFor = _stallFor;
    uint32 const stallMs = _stallMs;
    uint32 const loneWaitMs = _loneWaitMs;
    _stallFor.Clear();
    _loneWaitMs = 0;

    // A boss leaves its own damage out of the estimate (the encounter is built for the group): the group rests by
    // the danger rules for it, as before.
    float const healerManaNeed = measured && !chosen.boss ? HealerManaNeed(chosen, support) : -1.0f;
    // [chain_pull] right after a fight a small cheap pull close by goes at once, the group above its own rest lines
    bool chain = false;
    if (CHAIN_PULL && !fallback && !chosen.boss && chosen.mobs && chosen.mobs <= lastResortMobs &&
        TankCarries(chosen, support) && bot->GetHealthPct() >= CHAIN_PULL_TANK_HEALTH && IsGroupPresent(bot))
    {
        uint32 const lastFight = PullCheck::LastFightMs(bot);
        Unit* const chainMob = botAI->GetUnit(_planCached);
        if (lastFight && getMSTimeDiff(lastFight, now) <= CHAIN_PULL_WINDOW_MS && chainMob &&
            bot->GetExactDist(chainMob) <= CHAIN_PULL_RANGE)
        {
            std::string below;
            auto const aboveLines = [&below](Player* member)
            {
                if (member->IsSitState() || RestState::WantsDrink(member) || RestState::WantsFood(member))
                    below = member->GetName();
                return below.empty();
            };
            bool lines = aboveLines(bot);
            if (Group* group = bot->GetGroup())
            {
                for (GroupReference* ref = group->GetFirstMember(); ref && lines; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    if (member && member != bot && IsPresent(bot, member))
                        lines = aboveLines(member);
                }
            }

            chain = lines;
            if (chain)
                LOG_INFO("playerbots", "[chain_pull] {}: chains {} ({} mobs, cost {:.2f}, {:.1f} yd) {:.1f} s after "
                         "the last fight, the group above its rest lines", bot->GetName(), chainMob->GetName(),
                         chosen.mobs, chosen.cost, bot->GetExactDist(chainMob),
                         float(getMSTimeDiff(lastFight, now)) / float(IN_MILLISECONDS));
        }
    }

    // [b32_engage] A chain or fallback pull skips the rest lines, never a sitter below its line nor a healer out of
    // reach: ohb-h-5H-21 20261025-221216 #18 (the hunter drinking 63 yd off), bf-h-5H-84 20261025-223902 #18.
    std::string notReady;
    if ((fallback || chain) && !GroupDamage::NoDamageRelease(bot) &&
        (PullReadiness::GroupPreparing(bot, &notReady) || HealerOutOfReach(bot, &notReady)))
    {
        LogPullGate(bot, notReady);
        _planNotReadyWhy = "group not ready to pull: " + notReady;
        return false;
    }

    if (!fallback && !chain && !IsGroupReadyToPull(bot, chosen.danger, strength, healerManaNeed))
    {
        _planNotReadyWhy = Acore::StringFormat("group not ready to pull {} mobs (danger {:.2f}: rest, mana, health)",
                                               chosen.mobs, chosen.danger);
        return false;
    }

    // User rule: a weak pack may go with a damage dealer down, a dangerous one waits for all of them. A patrol that
    // walks into the group is met by the tank either way.
    OverloadTerms chosenTerms;
    CarefulOverload(bot, chosen, strength, support, kit, measured, &chosenTerms);
    if (!patrolFirst && !chosen.boss && chosenTerms.Max() > CAREFUL_WEAK_PACK_OVERLOAD && !IsEveryDpsPresent(bot))
    {
        _planNotReadyWhy = Acore::StringFormat("a damage dealer is missing for a pack this heavy (overload {:.2f})",
                                               chosenTerms.Max());
        return false;
    }

    // User rule: packs stand apart at spawn, and one that crosses another walks on. A group that crosses the planned
    // pack now (CollectPack split mode), or crossed it at the last look and is not yet OVERLAP_MARGIN past the radius
    // that would bring it, holds the pull while it is there: closing in, steady, or walking off (then until the gap
    // is over that radius and the margin). At most OVERLAP_MAX_HOLD_MS, then planned as before, merged ("[pull
    // overlap] ... plans it merged"). A crossing group that stands where it is does not merge (two static mobs are
    // never split: the assistance and flee chains count them as before).
    if (split && !chosen.boss && !patrolFirst)
    {
        auto const inPlan = [&chosenPack](Unit* mob)
        { return std::find(chosenPack.begin(), chosenPack.end(), mob) != chosenPack.end(); };
        struct Crossing
        {
            Unit* outside = nullptr;
            Creature* mover = nullptr;
            Position anchor;
            float reach = 0.0f;
            float distance = 0.0f;
        };
        std::vector<Crossing> crossings;
        auto const keep = [&](Unit* outside, Creature* mover, Position const& anchor, float reach)
        {
            if (!outside || !mover || !IsIdleEnemy(bot, outside) || inPlan(outside) ||
                std::any_of(crossings.begin(), crossings.end(),
                            [outside](Crossing const& seen) { return seen.outside == outside; }))
                return;

            float const distance = mover->GetExactDist2d(anchor.GetPositionX(), anchor.GetPositionY());
            if (distance <= reach + OVERLAP_MARGIN)
                crossings.push_back({outside, mover, anchor, reach, distance});
        };

        for (PackOverlap const& overlap : chosenOverlaps)
            keep(overlap.outside, overlap.mover, overlap.anchor, overlap.reach);

        if (_overlapHoldMs && getMSTimeDiff(_overlapSeenMs, now) <= OVERLAP_HOLD_GAP_MS)
        {
            for (OverlapMemo const& memo : _overlapMemo)
                keep(botAI->GetUnit(memo.outside), botAI->GetCreature(memo.mover), memo.anchor, memo.reach);
        }

        if (crossings.empty())
        {
            if (_overlapHoldMs && !_overlapMemo.empty())
            {
                LogPatrol(bot,
                          Acore::StringFormat("[pull overlap] {}: pack {} ({} mobs): the crossing group has walked "
                                              "off after {:.0f} s, pulls",
                                              bot->GetName(), target->GetName(), chosen.mobs,
                                              float(getMSTimeDiff(_overlapHoldMs, now)) / float(IN_MILLISECONDS)),
                          1);
            }

            _overlapHoldMs = 0;
            _overlapMemo.clear();
        }
        else
        {
            // the one that stays longest: the least far past its radius
            Crossing const& binding = *std::min_element(crossings.begin(), crossings.end(),
                                                        [](Crossing const& a, Crossing const& b)
                                                        { return a.distance - a.reach < b.distance - b.reach; });
            // Sweep batch5 sh-n: the target flipped between a refused pack and this one, so the hold was looked at
            // only every 10-20 s and its clock restarted on each look ("held 0 s", forever). A wait for the same
            // crossing group keeps its clock over a longer gap (OVERLAP_GIVE_UP_MS); OVERLAP_MAX_HOLD_MS bounds it.
            uint32 const sinceSeen = _overlapHoldMs ? getMSTimeDiff(_overlapSeenMs, now) : 0;
            // [overlap_clock_per_pack] two crossing groups taking turns as the binding one (scholo-h wave 23: two
            // Risen Protectors, reach 10 / 12, "held 0-7 s" for 4 minutes) restarted the clock on each switch: the
            // clock now runs for the pack whichever group binds, OVERLAP_MAX_HOLD_MS bounds the whole wait.
            if (!_overlapHoldMs || sinceSeen > OVERLAP_GIVE_UP_MS)
            {
                _overlapHoldMs = now;
                // [b36_sv] one more hold with no fight since the last (see OVERLAP_MAX_CYCLES)
                uint32 const lastFightMs = PullCheck::LastFightMs(bot);
                if (!_overlapCycles || lastFightMs != _overlapCycleFightMs ||
                    getMSTimeDiff(_overlapCycleMs, now) > OVERLAP_CYCLE_WINDOW_MS)
                    _overlapCycles = 0;
                ++_overlapCycles;
                _overlapCycleMs = now;
                _overlapCycleFightMs = lastFightMs;
            }

            _overlapSeenMs = now;
            _overlapBinding = binding.outside->GetGUID();
            uint32 const held = getMSTimeDiff(_overlapHoldMs, now);
            // every crossing mover gets its sample, so whichever binds next has a trend
            for (Crossing const& crossing : crossings)
                SampleMotion(crossing.mover);

            float seconds = 0.0f;
            float const change = DistanceTrend(binding.mover, binding.anchor, seconds);
            MobMotion const motion = MotionOf(binding.mover);
            // A crossing group that does not walk off is no group crossing (OVERLAP_TREND_MIN_SEC): counted in the
            // pack now, planned merged for a while (OVERLAP_GIVE_UP_MS), no hold.
            bool const staysPut =
                motion != MobMotion::Patrol && change <= OVERLAP_TREND_MIN && seconds >= OVERLAP_TREND_MIN_SEC;
            // the other group moves, or (static) the pack's own mob walks past it
            std::string const kind =
                binding.mover != binding.outside
                    ? Acore::StringFormat("static, the pack's {} is a {}", binding.mover->GetName(),
                                          MobMotionName(motion))
                    : std::string(MobMotionName(motion));
            char const* trend =
                change < -OVERLAP_TREND_MIN ? "closing in" : (change > OVERLAP_TREND_MIN ? "walking off" : "steady");
            // [b36_sv] the hold came back too often with no fight between (OVERLAP_MAX_CYCLES)
            // [lead4] LEADER_ONE_PATROL_OWNER, a leader look: no cycle count and no wait clock for a waypoint patrol
            // that crosses the pack itself. The one forecast decides: its walk leaves the pack's reach (a window):
            // wait for it; it never does, or the leader decided to pull it (intercept): pulled first now.
            PullPlanner::LeaderInputs const* const overlapInputs =
                _snap && Leader::Leads(Leader::LEADER_ONE_PATROL_OWNER) ? _snap->Leader() : nullptr;
            bool overlapWindow = false;  // a forecast window for a waypoint patrol: waited for without a clock
            bool overlapNoWindow = false;
            if (overlapInputs && motion == MobMotion::Patrol && binding.mover == binding.outside)
            {
                bool known = false;
                bool const window = PullPlanner::PatrolLeavesReach(binding.mover, binding.anchor,
                                                                   binding.reach + OVERLAP_MARGIN, &known);
                overlapWindow = known && window && binding.mover->GetGUID() != overlapInputs->intercept;
                overlapNoWindow = (known && !window) || binding.mover->GetGUID() == overlapInputs->intercept;
            }
            bool const forecastDecides = overlapInputs && (overlapWindow || overlapNoWindow);
            // [lead31b] a led tank's planner holds no overlap wait: decided at once (the crossing patrol first when
            // it fits, else the pack planned merged)
            bool const ledNow = Leader::LEADER_NO_FORCING_TIMERS && Leader::Orders::IsTracked(bot->GetGUID());
            bool const cycledOut =
                !staysPut && (ledNow || (forecastDecides ? overlapNoWindow : _overlapCycles > OVERLAP_MAX_CYCLES));
            std::string decision;
            if (staysPut)
                decision = "does not walk off, counted in the pack (plans it merged)";
            // [lead122] a led tank's planner never counts holds (ledNow): its answer is decided at once - the crossing
            // patrol first when it fits (tryPlan's estimate), else the pack planned merged; the old text named the
            // hold count, which decides nothing for it
            else if (cycledOut && ledNow)
                decision = "the leader decides at once (no overlap wait): the crossing patrol first when it fits, "
                           "else planned merged";
            else if (cycledOut)
                decision = Acore::StringFormat("held {} times with no fight between, [b36_sv] gives up the wait",
                                               _overlapCycles);
            else if (held < OVERLAP_MAX_HOLD_MS || overlapWindow)
                decision = change > OVERLAP_TREND_MIN
                               ? Acore::StringFormat("holds until the gap is over {:.0f} yd",
                                                     binding.reach + OVERLAP_MARGIN)
                               : std::string("holds");
            else
                decision = "waited long enough, plans it merged";

            Unit* const pullMob = botAI->GetUnit(_planCached);
            LogPatrol(bot,
                      Acore::StringFormat("[pull overlap] {}: pack {} ({} mobs), other group {} ({}): {:.1f} yd of "
                                          "{:.0f} reach, {} ({:+.1f} yd in {:.0f} s), held {:.0f} s: {}",
                                          bot->GetName(), pullMob ? pullMob->GetName() : target->GetName(),
                                          chosen.mobs, binding.outside->GetName(), kind, binding.distance,
                                          binding.reach, trend, change, seconds, float(held) / float(IN_MILLISECONDS),
                                          decision),
                      1);
            _stallFor = stallFor;
            _stallMs = stallMs;
            _loneWaitMs = loneWaitMs;
            // [b36_sv] past the cycle cap a waypoint patrol that crosses the pack itself is pulled first, planned
            // with what it passes (as an incoming patrol, not split: a pull that wakes the pack does not fit)
            if (cycledOut && motion == MobMotion::Patrol && binding.mover == binding.outside)
            {
                std::vector<PackOverlap>* const splitOut = overlapsOut;
                overlapsOut = nullptr;
                bool const taken = tryPlan(binding.mover, false, 1.0f, false, false);
                overlapsOut = splitOut;
                if (taken)
                {
                    patrolFirst = patrolFits = true;
                    splitPlan = false;
                    _planSingle = false;
                    _planReason = "[b36_sv] the patrol that kept crossing the pack, taken first: " + _planReason;
                    LogPatrol(bot,
                              Acore::StringFormat("[b36_sv] overlap_cap: {}: {} crossed pack {} in {} holds with no "
                                                  "fight: pulls the patrol first, {} mobs, cost {:.2f}",
                                                  bot->GetName(), binding.mover->GetName(),
                                                  pullMob ? pullMob->GetName() : target->GetName(), _overlapCycles,
                                                  chosen.mobs, chosen.cost),
                              1);
                    float const need = measured && !chosen.boss ? HealerManaNeed(chosen, support) : -1.0f;
                    if (!IsGroupReadyToPull(bot, chosen.danger, strength, need))
                    {
                        // the count stays: the next look tries the patrol again
                        _planNotReadyWhy = Acore::StringFormat(
                            "group not ready for the crossing patrol {} ({} mobs, danger {:.2f})",
                            binding.mover->GetName(), chosen.mobs, chosen.danger);
                        return false;
                    }

                    _overlapHoldMs = 0;
                    _overlapMemo.clear();
                    _overlapCycles = 0;
                }
            }

            // [b36_sv] the patrol taken first above: planned on below with it (the overlap hold is over)
            if (!patrolFirst && (staysPut || cycledOut || (held >= OVERLAP_MAX_HOLD_MS && !overlapWindow)))
            {
                _overlapGiveUpMs = now;
                _overlapHoldMs = 0;
                _overlapMemo.clear();
                if (cycledOut)
                    _overlapCycles = 0;  // [b36_sv] the merged plan's turn; a later hold counts anew
                _planNotReadyWhy = Acore::StringFormat("overlap wait: {} {}, planned merged on the next look",
                                                       binding.outside->GetName(), decision);
                return false;
            }
            else if (!patrolFirst)
            {
                _overlapMemo.clear();
                for (Crossing const& crossing : crossings)
                    _overlapMemo.push_back({crossing.outside->GetGUID(), crossing.mover->GetGUID(), crossing.anchor,
                                            crossing.reach});

                _planNotReadyWhy = Acore::StringFormat("overlap wait: {} crosses the pack, {} ({:.0f} s, trend {})",
                                                       binding.outside->GetName(), decision,
                                                       float(held) / float(IN_MILLISECONDS), trend);
                return false;
            }
        }
    }

    // A patrol taken first below is planned with what it passes, as before (not split).
    overlapsOut = nullptr;

    // Expert rule: a patrol that walks into the fight is waited out, else pulled first, else counted. Each patrol the
    // pull does not take is followed over the pull's walk and the fight (its estimate, longer by PATROL_FIGHT_MARGIN)
    // at the fight's spot and where the tank stands. One that comes, and that a wait of at most PATROL_MAX_WAIT_SEC
    // keeps out of the whole fight, is waited for ("[pull hold] ... patrol <name> approaches"), the one the pack
    // counts already too (the pull after it is smaller); one that cannot be waited out is pulled first if the group
    // takes it alone, else counted in the plan, which must still fit.
    Unit* planned = botAI->GetUnit(_planCached);
    if (!patrolFirst && planned && map && !patrols.empty())
    {
        Position const fightSpot =
            _planLos ? _losSpot
                     : (_planPullBack ? (_planKillFor == planned->GetGUID() ? _planKill : bot->GetPosition())
                                      : (_planFightFor == planned->GetGUID() ? _planFight : planned->GetPosition()));
        float const walkIn = _planLos || _planPullBack
                                 ? 0.0f
                                 : bot->GetExactDist(planned) / std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
        float const fight = (chosen.boss ? PATROL_BOSS_FIGHT_SEC
                                         : std::clamp(chosen.fightSec, PATROL_MIN_FIGHT_SEC, PATROL_MAX_FIGHT_SEC)) *
                            PATROL_FIGHT_MARGIN;
        size_t const window = size_t((PATROL_PULL_LEAD_SEC + walkIn + fight) / PATROL_SAMPLE_SEC) + 1;
        size_t const maxWait = size_t(PATROL_MAX_WAIT_SEC / PATROL_SAMPLE_SEC);
        std::vector<Position> const points = {fightSpot, bot->GetPosition()};
        float const limit = fallback && !measured ? CAREFUL_FALLBACK_OVERLOAD : 1.0f;
        auto const inPlan = [&chosenPack](Unit* mob)
        { return std::find(chosenPack.begin(), chosenPack.end(), mob) != chosenPack.end(); };
        bool changed = false;
        for (PatrolGroup const& patrol : patrols)
        {
            if (patrol.walker == planned ||
                std::find(patrol.members.begin(), patrol.members.end(), planned) != patrol.members.end())
                continue;  // the pull takes this patrol

            std::vector<bool> const flags =
                PatrolInReach(map, bot->GetPhaseMask(), patrol.track, patrol.reach, points, maxWait + window);
            int32 const first = FirstInReach(flags, window);
            if (first < 0)
                continue;

            Unit* member = patrol.nearest;
            bool const counted = std::any_of(patrol.members.begin(), patrol.members.end(), inPlan);
            float const eta = float(first) * PATROL_SAMPLE_SEC;
            float const distance = bot->GetExactDist(member);
            int32 const wait = PatrolWait(flags, window, maxWait);
            // [lead29] user: no wait on a time forecast for a led party - the walker is decided at once: taken along
            // when the whole still fits (pull together), else pulled first, else held as no fit (below)
            bool const ledNoWait = Leader::LEADER_NO_FORCING_TIMERS && Leader::Orders::IsTracked(bot->GetGUID());
            if (ledNoWait && !counted && !lastResort && !chosenPack.empty())
            {
                std::vector<Unit*> together = chosenPack;
                for (Unit* mob : patrol.members)
                    if (!inPlan(mob))
                        together.push_back(mob);
                PackEstimate const togetherEstimate = EstimatePack(together, bot, groupDps, estimateMode, _planLos);
                OverloadTerms togetherTerms;
                CarefulOverload(bot, togetherEstimate, strength, support, kit, measured, &togetherTerms);
                if (togetherEstimate.boss || (withinCap(togetherEstimate) && togetherTerms.Max() <= limit))
                {
                    chosenPack = together;
                    chosen = togetherEstimate;
                    changed = true;
                    LogPatrol(bot, Acore::StringFormat(
                                       "[lead29] [pull patrol] {}: walker {} ({:.1f} yd) joins the fight at {}: taken "
                                       "along, {} mobs, cost {:.2f} (no wait)",
                                       bot->GetName(), member->GetName(), distance, planned->GetName(), chosen.mobs,
                                       chosen.cost));
                    continue;
                }
            }
            if (wait > 0 && !ledNoWait && mayHoldFor(patrol.walker, patrol.reach))
            {
                LogPatrol(bot, Acore::StringFormat(
                                   "[pull hold] {}: patrol {} approaches ({:.1f} yd, eta {:.0f} s): walks into the "
                                   "fight at {} ({} mobs{}, about {:.0f} s), waits about {:.0f} s for it to pass",
                                   bot->GetName(), member->GetName(), distance, eta, planned->GetName(), chosen.mobs,
                                   counted ? " with it" : "", fight, float(wait) * PATROL_SAMPLE_SEC));
                _planNotReadyWhy = Acore::StringFormat("patrol wait: {} walks into the fight, about {:.0f} s",
                                                       member->GetName(), float(wait) * PATROL_SAMPLE_SEC);
                return false;
            }

            if (counted || lastResort || chosenPack.empty())
                continue;

            // It cannot be waited out: alone first, if the group takes it.
            if (tryPlan(member, false, limit, false, false))
            {
                patrolFirst = changed = true;
                _planSingle = false;
                LogPatrol(bot, Acore::StringFormat(
                                   "[pull patrol] {}: patrol {} ({:.1f} yd, eta {:.0f} s) walks into the fight at {} "
                                   "and cannot be waited out: pulls it first, {} mobs, cost {:.2f}",
                                   bot->GetName(), member->GetName(), distance, eta, planned->GetName(), chosen.mobs,
                                   chosen.cost));
                break;
            }

            for (Unit* mob : patrol.members)
            {
                if (!inPlan(mob))
                    chosenPack.push_back(mob);
            }

            chosen = EstimatePack(chosenPack, bot, groupDps, estimateMode, _planLos);
            changed = true;
            OverloadTerms withPatrol;
            CarefulOverload(bot, chosen, strength, support, kit, measured, &withPatrol);
            bool const fits = chosen.boss || (withinCap(chosen) && withPatrol.Max() <= limit);
            if (!fits && mayHoldFor(patrol.walker, patrol.reach))
            {
                LogPatrol(bot, Acore::StringFormat(
                                   "[pull hold] {}: patrol {} approaches ({:.1f} yd, eta {:.0f} s): cannot be waited "
                                   "out nor pulled alone, the fight at {} with it is {} mobs, cost {:.2f}",
                                   bot->GetName(), member->GetName(), distance, eta, planned->GetName(), chosen.mobs,
                                   chosen.cost));
                _planNotReadyWhy = Acore::StringFormat("patrol wait: {} can be neither waited out nor pulled alone",
                                                       member->GetName());
                return false;
            }

            LogPatrol(bot, Acore::StringFormat(
                               "[pull patrol] {}: patrol {} ({:.1f} yd, eta {:.0f} s) counted in the fight at {}: {} "
                               "mobs, cost {:.2f}{}",
                               bot->GetName(), member->GetName(), distance, eta, planned->GetName(), chosen.mobs,
                               chosen.cost, fits ? "" : " (held long enough)"));
        }

        if (changed)
        {
            splitPlan = false;  // [b36_sv] a patrol counted in / pulled first: no longer the peel its search costed
            float const need = measured && !chosen.boss ? HealerManaNeed(chosen, support) : -1.0f;
            if (!fallback && !IsGroupReadyToPull(bot, chosen.danger, strength, need))
            {
                _planNotReadyWhy = Acore::StringFormat("group not ready for {} mobs with the patrol (danger {:.2f})",
                                                       chosen.mobs, chosen.danger);
                return false;
            }

            CarefulOverload(bot, chosen, strength, support, kit, measured, &chosenTerms);
        }
    }

    // A fallback that grew past what the tank carries (a patrol counted in) is not pulled ([lead263] TankCarries,
    // the same test its search used: [careful_fallback_cap_consistent], wave 23 held 2-3 mob fallbacks its search
    // took under another cap, a hold no rest can change).
    if (std::string carries;
        fallback && !patrolFits && !chosen.boss && !_planGuaranteed && !TankCarries(chosen, support, &carries))
    {
        LogPullHold(bot, ("fallback plan is more than the tank carries: " + carries).c_str(), target);
        _planNotReadyWhy = "fallback plan is more than the tank carries: " + carries;
        return false;
    }

    // Sweep batch4 DTK (Trollgore): a trash fight that brings a dungeon boss is no trash pull. The fight's spot is
    // judged against every boss near it (BossDrawnIn): one whose own encounter runs (he fights by himself, or his
    // cycle's next fight starts before this one ends) holds the pull until it fits between two of his fights - a
    // pull back's kill spot out of his room passes, so a pull back far enough is taken as before; held
    // BOSS_WINDOW_MAX_HOLD_MS, or drawn in another way (aggro, formation, assistance), it is his fight: pulled only
    // with the group ready for him (BossReadyGap). So is a plan that counts a boss already for a mob that is none (a
    // lone mob or a patrol that brings him), which the hard cap and the fallback cost exempt.
    Unit* const plannedMob = botAI->GetUnit(_planCached);
    if (plannedMob && map && map->IsDungeon() && !IsBossMob(plannedMob))
    {
        Position const engageAt =
            _planFightFor == plannedMob->GetGUID() ? _planFight : plannedMob->GetPosition();
        Position const fightSpot =
            _planLos ? _losSpot
                     : (_planPullBack ? (_planKillFor == plannedMob->GetGUID() ? _planKill : bot->GetPosition())
                                      : engageAt);
        float const walkIn = _planLos || _planPullBack
                                 ? 0.0f
                                 : bot->GetExactDist(plannedMob) / std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
        float const fightSec = PATROL_PULL_LEAD_SEC + walkIn +
                               std::clamp(chosen.fightSec, PATROL_MIN_FIGHT_SEC, PATROL_MAX_FIGHT_SEC) *
                                   PATROL_FIGHT_MARGIN;
        Unit* drawn = nullptr;
        char const* drawnWhy = nullptr;
        float waitSec = 0.0f;
        if (chosen.boss)
        {
            for (Unit* mob : chosenPack)
            {
                if (IsBossMob(mob))
                {
                    drawn = mob;
                    drawnWhy = "the plan counts him";
                    break;
                }
            }
        }

        for (ObjectGuid const& guid : bossWatch)
        {
            if (drawn)
                break;

            Unit* bossUnit = botAI->GetUnit(guid);
            if (!bossUnit || !bossUnit->IsAlive() || !IsBossMob(bossUnit))
                continue;

            if (char const* why = BossDrawnIn(bot, bossUnit, fightSpot, fightSec, chosenPack, waitSec))
            {
                drawn = bossUnit;
                drawnWhy = why;
            }
        }

        // [lead220] b184 rfc-h 5H 862.9-869 s: the engage on the patrol Searing Blade Enforcer was judged where it
        // walked then; the tank met it 8 yd from Taragaman, who came 7 s after the last trash fight, no boss
        // readiness (the hunter at 29 % mana). A walker's fight is where the tank meets it: its forecast walk until
        // the tank's walk in is over is judged as the fight spot
        if (Creature* walker = plannedMob->ToCreature(); !drawn && walker && !_planLos && !_planPullBack)
        {
            std::vector<G3D::Vector3> track;
            float sampleSec = 0.0f;
            if (PatrolForecast(walker, track, sampleSec) && sampleSec > 0.0f)
            {
                size_t const steps = std::min(track.size(), size_t((PATROL_PULL_LEAD_SEC + walkIn) / sampleSec) + 1);
                for (size_t step = 0; step < steps && !drawn; ++step)
                {
                    Position const meet(track[step].x, track[step].y, track[step].z);
                    for (ObjectGuid const& guid : bossWatch)
                    {
                        Unit* bossUnit = botAI->GetUnit(guid);
                        float meetWait = 0.0f;
                        if (bossUnit && bossUnit->IsAlive() && IsBossMob(bossUnit) &&
                            BossDrawnIn(bot, bossUnit, meet, fightSec, chosenPack, meetWait))
                        {
                            drawn = bossUnit;
                            drawnWhy = "its walk takes the fight to him";
                            break;
                        }
                    }
                }
            }
        }

        if (drawn)
        {
            if (_bossHoldFor != drawn->GetGUID() || getMSTimeDiff(_bossHoldSeenMs, now) > BOSS_HOLD_GAP_MS)
            {
                _bossHoldFor = drawn->GetGUID();
                _bossHoldMs = now;
            }

            _bossHoldSeenMs = now;
            uint32 const held = getMSTimeDiff(_bossHoldMs, now);
            std::string const what = Acore::StringFormat("{} ({} mobs) at ({:.1f} {:.1f} {:.1f}) brings {} {:.0f} yd "
                                                         "away: {}", plannedMob->GetName(), chosen.mobs,
                                                         fightSpot.GetPositionX(), fightSpot.GetPositionY(),
                                                         fightSpot.GetPositionZ(), drawn->GetName(),
                                                         drawn->GetExactDist(fightSpot), drawnWhy);
            // the missing buff the tank has waited for long enough (WaitForBuffs' clock) holds no boss either
            bool const askBuffs = !_buffWaitMs || getMSTimeDiff(_buffWaitMs, now) < BOSS_BUFF_WAIT_MS;
            std::string const readyGap = waitSec > 0.0f && held < BOSS_WINDOW_MAX_HOLD_MS
                                             ? std::string()
                                             : BossReadyGap(bot, askBuffs, IsEncounterBoss(drawn), drawn);
            bool hold = true;
            if (waitSec > 0.0f && held < BOSS_WINDOW_MAX_HOLD_MS)
            {
                LogPatrol(bot, Acore::StringFormat("[pull boss] {}: {}; waits about {:.0f} s for a fight that fits "
                                                   "(held {:.0f} s)", bot->GetName(), what, waitSec,
                                                   float(held) / float(IN_MILLISECONDS)));
                _planNotReadyWhy = Acore::StringFormat("boss window: {} {}", drawn->GetName(), drawnWhy);
            }
            else if (!readyGap.empty())
            {
                LogPatrol(bot, Acore::StringFormat("[pull boss] {}: {}; his fight, the group is not ready for him "
                                                   "({})", bot->GetName(), what, readyGap));
                _planNotReadyWhy = Acore::StringFormat("boss pull: {} comes with it, {}", drawn->GetName(), readyGap);
            }
            else
                hold = false;

            if (hold)
            {
                _stallFor = stallFor;
                _stallMs = stallMs;
                _loneWaitMs = loneWaitMs;
                return false;
            }

            LOG_INFO("playerbots", "[pull boss] {}: {}; pulled as his fight, the group ready for him", bot->GetName(),
                     what);
            _planReason = Acore::StringFormat("brings {} ({}): his fight, the group ready; ", drawn->GetName(),
                                              drawnWhy) +
                          _planReason;
            if (std::find(chosenPack.begin(), chosenPack.end(), drawn) == chosenPack.end())
            {
                chosenPack.push_back(drawn);
                ++chosen.mobs;
            }

            chosen.boss = true;
        }
        else
            _bossHoldFor.Clear();
    }
    else if (plannedMob && map && map->IsNonRaidDungeon() && IsBossMob(plannedMob) && !plannedMob->IsInCombat())
    {
        // [boss_pull_ready_gate] The boss himself as the plan's mob had no readiness gate of a boss: the branch above
        // judges only a trash plan that draws a boss in, and the trash rest gate (IsGroupReadyToPull) let the tank
        // engage him at the trash lines. sh-h w24 (20261025-115805-sh-h-5A-37, pull 46): "tank's reach melee" on
        // Warchief Kargath Bladefist 9 s after "buffing: before Warchief Kargath Bladefist", the group's mana at 69 %,
        // the healer dead to Blade Dance 31 s in. A boss is pulled with the group as ready as for a boss
        // (BossReadyGap: health, mana, nobody sitting, nobody still buffing, no group buff missing).
        bool const askBuffs = !_buffWaitMs || getMSTimeDiff(_buffWaitMs, now) < BOSS_BUFF_WAIT_MS;
        std::string const readyGap = BossReadyGap(bot, askBuffs, IsEncounterBoss(plannedMob), plannedMob);
        if (!readyGap.empty())
        {
            LogPatrol(bot, Acore::StringFormat("[pull boss] {}: {} himself, the group is not ready for him ({})",
                                               bot->GetName(), plannedMob->GetName(), readyGap));
            _planNotReadyWhy = Acore::StringFormat("boss pull: {}, {}", plannedMob->GetName(), readyGap);
            _stallFor = stallFor;
            _stallMs = stallMs;
            _loneWaitMs = loneWaitMs;
            return false;
        }
    }

    // [b44_gerst] A boss fight (the boss himself, or a pack that draws him in) is fought on cleared ground with room
    // round it (FindBossFightSpot): a pull back's kill spot, or the spot an engage walks the fight back to once it is
    // on (FightSpot). Not for a line-of-sight pull (its corner is the spot) nor a boss that does not walk to the tank.
    if (plannedMob && map && map->IsNonRaidDungeon() && !_planLos && (chosen.boss || IsBossMob(plannedMob)))
    {
        Unit* boss = IsBossMob(plannedMob) ? plannedMob : nullptr;
        for (Unit* mob : chosenPack)
            if (!boss && IsBossMob(mob))
                boss = mob;

        Creature* const bossCreature = boss ? boss->ToCreature() : nullptr;
        if (bossCreature && bossCreature->IsAlive() && !bossCreature->IsInCombat() &&
            bossCreature->IsCombatMovementAllowed() && !bossCreature->IsRooted())
        {
            std::vector<Unit*> fight = chosenPack;
            for (Unit* unit : {plannedMob, boss})
                if (std::find(fight.begin(), fight.end(), unit) == fight.end())
                    fight.push_back(unit);

            Position const asked =
                _planPullBack ? (_planKillFor == plannedMob->GetGUID() ? _planKill : bot->GetPosition())
                              : (_planFightFor == plannedMob->GetGUID() ? _planFight : plannedMob->GetPosition());
            Position spot;
            std::string note;
            if (!FindBossFightSpot(botAI, bot, boss, fight, asked, spot, note))
                LOG_INFO("playerbots", "[b44_gerst] boss_fight_spot: {}: {} ({} mobs) fought at ({:.1f},{:.1f},{:.1f}) "
                         "as planned: {}; no cleared spot with room on the way back", bot->GetName(), boss->GetName(),
                         fight.size(), asked.GetPositionX(), asked.GetPositionY(), asked.GetPositionZ(),
                         note.empty() ? std::string("-") : note);
            else if (spot.GetExactDist(asked) > FIGHT_SPOT_ARRIVED)
            {
                LOG_INFO("playerbots", "[b44_gerst] boss_fight_spot: {}: {} ({} mobs) dragged to "
                         "({:.1f},{:.1f},{:.1f}), {:.1f} yd from the tank, instead of ({:.1f},{:.1f},{:.1f}): {}",
                         bot->GetName(),
                         boss->GetName(), fight.size(), spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(),
                         bot->GetExactDist(spot), asked.GetPositionX(), asked.GetPositionY(), asked.GetPositionZ(),
                         note.empty() ? std::string("-") : note);
                if (_planPullBack)
                {
                    _planKillFor = plannedMob->GetGUID();
                    _planKill = spot;
                }
                else
                {
                    _planFightFor = plannedMob->GetGUID();
                    _planFight = spot;
                    _planFightBack = plannedMob->GetExactDist(spot);
                    _planFightNote = Acore::StringFormat("boss fight spot for {}: {}", boss->GetName(), note);
                }
            }
        }
    }

    _planPatrol = patrolFirst;
    _planTarget = _planCached;
    _planResult = true;
    _planMobs = chosen.mobs;
    _planVerdict = PlanVerdictState::Accepted;
    _planVerdictMobs = chosen.mobs;
    _planVerdictFightMobs = chosen.mobs;
    _planVerdictCost = chosen.cost;
    _planVerdictSec = chosen.fightSec;
    _planVerdictBoss = chosen.boss;
    _planVerdictPack.push_back(_planCached);
    for (Unit* mob : chosenPack)
    {
        if (mob->GetGUID() != _planCached)
            _planVerdictPack.push_back(mob->GetGUID());
    }

    // a Sap the plan counts as applied is part of the accepted pull: the rogue casts it now (TankPullsPack), and
    // the tank holds for it even on a lone-mob pull (no Cancel of the crowd control plan)
    _planCountsSap = !sapClaimed.empty();
    for (ObjectGuid const& guid : sapClaimed)
    {
        if (std::find(_planVerdictPack.begin(), _planVerdictPack.end(), guid) == _planVerdictPack.end())
            _planVerdictPack.push_back(guid);
    }

    _planCost = chosen.cost;
    _planDrop = chosenTerms.drop;
    LOG_DEBUG("playerbots", "{} careful pull{}: {} {} (asked {}; {} mobs, landed {:.0f}/s, cost {:.2f} tank hp)",
              bot->GetName(),
              patrolFirst ? " (patrol first)"
                          : (lastResort ? (_planSingle ? " (last resort, lone mob)" : " (last resort)")
                                        : (fallback ? " (topped-up fallback)" : "")),
              _planLos ? "los pull" : (_planPullBack ? "pull back" : "engage"), _planTarget.ToString(),
              target->GetName(), chosen.mobs, chosen.landedDps, chosen.cost);
    // [lead257] the inputs of the estimate: each mob kind's melee and spells as the game reads them
    if (sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
    {
        // [lead258] and the healers' facts the mana and healing terms read
        LOG_DEBUG("playerbots",
                  "[lead258] {} estimate healers: {:.0f}/s together, pool {:.2f} tank hp, mana need "
                  "{:.0f} %; {}",
                  bot->GetName(), support.healHps, HealerPool(support), HealerManaNeed(chosen, support),
                  support.healerFacts.empty() ? std::string("none") : support.healerFacts);
        LOG_DEBUG("playerbots",
                  "[lead259] {} estimate drop {:.2f} = {:.2f} (landed {:.0f}/s over {:.0f} s, heals {:.0f}/s after "
                  "{:.1f} s) x measured {:.2f}; most {:.2f}; group dps {:.0f} (measured {:.0f}, weapons {:.0f})",
                  bot->GetName(), chosen.drop, chosen.dropRaw, chosen.landedDps, chosen.fightSec, support.healHps,
                  support.healGapSec, estimateMode.dropScale, chosen.maxDrop, groupDps, MeasuredGroupDps(bot),
                  GroupWeaponDps(bot));
        std::vector<uint32> logged;
        for (Unit* mob : chosenPack)
        {
            if (!mob || std::find(logged.begin(), logged.end(), mob->GetEntry()) != logged.end())
                continue;
            logged.push_back(mob->GetEntry());
            std::string inputs;
            LandedMeleeDps(mob, bot, _planLos, nullptr, &inputs);
            LOG_DEBUG("playerbots", "[lead257] {} estimate input {} (level {}): {}", bot->GetName(), mob->GetName(),
                      mob->GetLevel(), inputs);
        }
    }
    if (packWhyRecorder.on && chosen.mobs >= PACK_WHY_MIN_PLAN_MOBS && !chosenPack.empty() &&
        PackWhyDue(_packWhyMs, chosenPack.front()->GetGUID(), now))
        LOG_INFO("playerbots", "{}",
                 DescribePackWhy(bot, chosenPack.front(), chosenPack, FindPackWhy(chosenPack), chosen.mobs, "plan"));

    return true;
}

// Where the tank casts its ranged pull from: where it stands when in range, else on the way in at the spell's range.
// False when its pull strategy has no ranged spell it can use on this mob now.
bool AggressiveTargetAction::PullBackSpot(Unit* candidate, Position& spot, bool single, float* rangeOut)
{
    // A ranged pull of this mob was just given up before the cast (the walk for range would leave the healer, the
    // mob on another floor): no ranged pull of it for a while, the planner engages it or takes another (scholo-h
    // build 21: the same los pull of a Neophyte planned and aborted again and again, the harness skipped the hall).
    constexpr uint32 PULL_ABORT_MEMO_MS = 30 * IN_MILLISECONDS;
    ObjectGuid aborted;
    if (CarefulPlanVerdict::RecentlyAborted(bot->GetGUID(), PULL_ABORT_MEMO_MS, &aborted) &&
        aborted == candidate->GetGUID())
        return false;

    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!strategy || strategy->HasTarget() || strategy->IsPullPendingToStart() || strategy->HasPullStarted())
        return false;

    // The class strategies pick the spell for the mob in "pull strategy target"; set only while asking.
    Value<ObjectGuid>* pullTarget = context->GetValue<ObjectGuid>("pull strategy target");
    if (!pullTarget)
        return false;

    pullTarget->Set(candidate->GetGUID());
    bool const wasSingle = strategy->IsSingleMob();
    strategy->SetSingleMob(single);
    bool const usable = strategy->CanDoPullAction(candidate);
    std::string const spellName = strategy->GetSpellName();
    float const range = strategy->GetRange();
    strategy->SetSingleMob(wasSingle);
    pullTarget->Set(ObjectGuid::Empty);

    uint32 const spellId = usable ? AI_VALUE2(uint32, "spell id", spellName) : 0;
    if (!spellId || bot->HasSpellCooldown(spellId) || range < CAREFUL_MIN_PULL_RANGE)
        return false;

    // A lone mob is pulled with a spell that hits it alone (Avenger's Shield jumps to the pack).
    if (single && PullStrategy::HitsMoreThanOne(sSpellMgr->GetSpellInfo(spellId)))
        return false;

    if (rangeOut)
        *rangeOut = range;

    // [lead223b] b185 rfc-h 5B 608.9 s: the cast spot was the straight line's point at the spell's range,
    // (-209.6,123.7,-24.9) - in the air 13 yd over Ragefire's magma, no ground path to it; the tank's walk there was a
    // drop into the lava. A spot out of range is a point of the tank's own walk to the mob (below), never the line's
    float const distance = bot->GetExactDist(candidate);
    if (distance <= range)
    {
        spot.Relocate(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());

        // RFC 2026-10-24 18:28: the tank walked round a corner alone for Shoot range, out of the healer's sight, and
        // died under 7 mobs. A cast spot the healer does not see is no ranged pull: the cast spot moves on along the
        // approach (below) to one in range that sees the mob and the healer sees (RFC 2026-10-24 20:04: no such spot
        // at the spell's range, the tank walked in on a mob round the corner and the fight drew the pack beside it).
        if (candidate->IsWithinLOS(spot.GetPositionX(), spot.GetPositionY(),
                                   spot.GetPositionZ() + CAREFUL_EYE_HEIGHT) &&
            HealerSees(bot, spot))
        {
            _castSpots[candidate->GetGUID()] = {spot, getMSTime()};
            return true;
        }
    }

    // The pull is cast in sight (IsWithinPullRange): the tank walks its path in until it sees the mob, and the cast
    // spot is there, or further in (still in range) where the healer sees it too. Stockade wave 2: a Defias Convict
    // in its cell, out of sight of the corridor spot at 30 yd; the tank walked in to 9.5 yd, by Hamhock's room, and
    // 11 came.
    LeaderPath path(bot);  // [lead99]
    path.CalculatePath(candidate->GetPositionX(), candidate->GetPositionY(), candidate->GetPositionZ(), false);
    if (path.GetPathType() != PATHFIND_NORMAL || path.getPathLength() > CAREFUL_MAX_WALK_CHECK)
        return false;

    Movement::PointsArray const& points = path.GetPath();
    for (size_t i = 1; i < points.size(); ++i)
    {
        G3D::Vector3 const segment = points[i] - points[i - 1];
        float const length = segment.length();
        for (float along = 0.0f; along <= length; along += WALK_SAMPLE_STEP)
        {
            G3D::Vector3 const point = points[i - 1] + (length > 0.0f ? segment * (along / length) : segment);
            float const left = candidate->GetExactDist(point.x, point.y, point.z);
            if (left > range)
                continue;

            if (left < CAREFUL_MIN_PULL_RANGE)
                return false;  // in sight only once in melee: no ranged pull from here

            Position const option(point.x, point.y, point.z);
            if (candidate->IsWithinLOS(point.x, point.y, point.z + CAREFUL_EYE_HEIGHT) && HealerSees(bot, option))
            {
                spot.Relocate(point.x, point.y, point.z);
                _castSpots[candidate->GetGUID()] = {spot, getMSTime()};
                return true;
            }
        }
    }

    return false;
}

// [los_pull_plan_loop] A line-of-sight pull planned again and again on one mob that never comes is no pull: brs-h
// w24 (20261025-115805-brs-h-5A-32, 303-527 s) registered 83 plans on the Spellbinder / Warlock packs in 220 s
// ("tank spot 6.0 yd from the cast spot"), not one was cast, the harness withheld the route for the running pull. After
// LOS_LOOP_MAX_STARTS starts on the same idle mob within LOS_LOOP_WINDOW_MS the mob gets no line-of-sight plan for
// LOS_LOOP_BAN_MS: the planner pulls it another way (pull back, engage) or refuses it, and the harness routes on.
namespace PullPlanning  // [lead31a] was anonymous: shared with the tank's execution
{
    constexpr uint32 LOS_LOOP_MAX_STARTS = 6;
    constexpr uint32 LOS_LOOP_WINDOW_MS = 60 * IN_MILLISECONDS;
    constexpr uint32 LOS_LOOP_BAN_MS = 60 * IN_MILLISECONDS;

    struct LosLoopEntry
    {
        ObjectGuid target;
        uint32 starts = 0;
        uint32 firstMs = 0;
        ObjectGuid banned;
        uint32 bannedMs = 0;
    };

    std::unordered_map<ObjectGuid, LosLoopEntry>& LosLoops()
    {
        thread_local std::unordered_map<ObjectGuid, LosLoopEntry> loops;  // per tank, its own map thread
        return loops;
    }

    bool LosPullBanned(Player* bot, Unit* target)
    {
        auto const found = LosLoops().find(bot->GetGUID());
        return found != LosLoops().end() && found->second.banned == target->GetGUID() &&
               getMSTimeDiff(found->second.bannedMs, getMSTime()) < LOS_LOOP_BAN_MS;
    }

    // Counts this start; false when it is one too many (the mob is banned from now on).
    bool NoteLosPullStart(Player* bot, Unit* target)
    {
        uint32 const now = getMSTime();
        LosLoopEntry& entry = LosLoops()[bot->GetGUID()];
        if (entry.target != target->GetGUID() || !entry.firstMs ||
            getMSTimeDiff(entry.firstMs, now) > LOS_LOOP_WINDOW_MS)
        {
            entry.target = target->GetGUID();
            entry.starts = 0;
            entry.firstMs = now ? now : 1;
        }

        if (++entry.starts <= LOS_LOOP_MAX_STARTS)
            return true;

        LOG_INFO("playerbots", "[los_pull_plan_loop] {}: {} line-of-sight pull plans on {} in {} s, none cast: no "
                 "line-of-sight plan for it for {} s", bot->GetName(), entry.starts - 1, target->GetName(),
                 getMSTimeDiff(entry.firstMs, now) / IN_MILLISECONDS, LOS_LOOP_BAN_MS / IN_MILLISECONDS);
        entry.banned = target->GetGUID();
        entry.bannedMs = now ? now : 1;
        entry.starts = 0;
        entry.firstMs = 0;
        return false;
    }
}

bool AggressiveTargetAction::PlanLosPull(Unit* candidate, GuidVector const& seen,
                                         std::function<bool(std::vector<Unit*> const&)> const& accept,
                                         bool assistChain)
{
    // [los_pull_unseen] A pull round a corner is about what the tank does not see from where it stands: the pack at
    // the cast spot and at the tank's spot, the walk there and the other idle mobs the spot must be out of reach of
    // are taken from every unit in sight range, line of sight or not ("possible targets no los"). With the seen list
    // only ("possible targets", the non-measured modes) the spot was checked against nothing behind the hall's
    // pillars: scholo-h 20261025-132016 pull 6, "los pull 3 at the tank's spot round the corner" on a Spectral
    // Researcher at (212.7, 9.7) -> came 23, 34 kills in 152 s, wipe at 372 s; the tank's spot (~201, 17) stood
    // 5 yd from a Spectral Researcher and an Acolyte and 10 yd from a Risen Guard and a Neophyte the list lacked.
    GuidVector const everyUnit = PlanPossible(true);  // [lead7] the look's snapshot
    GuidVector const& possible = everyUnit.empty() ? seen : everyUnit;
    uint32 const minCasters = sPlayerbotAIConfig.LosPullMinCasters;
    Map* map = bot->GetMap();
    if (!minCasters || !map || !map->IsNonRaidDungeon() || !bot->GetGroup() || !candidate ||
        candidate->IsInCombat() || IsBossMob(candidate) || LosPullBanned(bot, candidate))
        return false;

    // The tank has to step back to its spot after the pull ("return to pull position").
    if (!botAI->HasStrategy("pull back", BOT_STATE_COMBAT))
        return false;

    uint32 const now = getMSTime();
    if ((candidate->GetGUID() == _lastPullBack && getMSTimeDiff(_lastPullBackMs, now) < CAREFUL_PULL_RETRY_MS) ||
        CarefulPlanVerdict::EngageOnly(bot->GetGUID(), candidate->GetGUID()))  // [b37_sv]
        return false;

    // Counted as a pull round a corner: the casters walk out of their room instead of standing in it and calling
    // the rest of it (the Scholomance hall: 27 mobs where they stand, 4-9 round the corner).
    std::vector<Unit*> pack;
    CollectPack(botAI, bot, candidate, candidate->GetPosition(), possible, pack, FightHere(candidate->GetPosition()),
                assistChain, true);
    uint32 const casters = CountRanged(pack);
    if (casters < minCasters)
        return false;

    Position castSpot;
    if (!PullBackSpot(candidate, castSpot))
        return false;

    CollectPack(botAI, bot, candidate, castSpot, possible, pack, FightHere(castSpot), assistChain, true);
    for (Unit* mob : pack)
    {
        if (IsBossMob(mob))
            return false;
    }

    // RFC 5B-1 pull 41 (wave 44): "los pull (reason: 0 casters pulled round the corner) ... 1 mobs": the whole
    // group ran back behind the corner for one melee mob. The pack that comes from the cast spot must have the
    // casters or shooters the corner is for; a melee pack is engaged or pulled back (only the tank steps back).
    if (!CountRanged(pack))
    {
        LOG_DEBUG("playerbots", "[los pull] {}: {} brings {} mobs from the cast spot, none that keeps its range: no "
                  "los pull", bot->GetName(), candidate->GetName(), pack.size());
        return false;
    }

    // The tank's walk to the cast spot (PullBackSpot keeps it in the healer's sight) enters no other mob's reach.
    bool walkable = false;
    if (bot->GetExactDist(castSpot) > CAREFUL_AGGRO_MARGIN)
    {
        if (Creature* woken = WalkEntersAggro(botAI, bot, possible, bot->GetPosition(), castSpot, pack,
                                              CAREFUL_MAX_WALK_CHECK, CAREFUL_AGGRO_MARGIN, walkable))
        {
            LOG_DEBUG("playerbots", "[los pull] {}: the walk to the cast spot for {} wakes {}: no los pull",
                      bot->GetName(), candidate->GetName(), woken->GetName());
            return false;
        }

        if (!walkable)
            return false;
    }

    if (!accept(pack))
        return false;

    // A recent search from about here is reused: a group that rests re-plans every second.
    _losMemo.erase(std::remove_if(_losMemo.begin(), _losMemo.end(),
                                  [now](LosPullMemo const& memo)
                                  {
                                      return getMSTimeDiff(memo.ms, now) >=
                                             (memo.found ? LOS_PULL_FOUND_MEMO_MS : LOS_PULL_FAILED_MEMO_MS);
                                  }),
                   _losMemo.end());

    LosPullMemo const* memo = nullptr;
    for (LosPullMemo const& entry : _losMemo)
    {
        if (entry.mob == candidate->GetGUID() &&
            bot->GetExactDist2d(entry.botX, entry.botY) <= LOS_PULL_MEMO_MOVE)
        {
            memo = &entry;
            break;
        }
    }

    Position spot;
    Position gather;
    bool found = false;
    if (memo)
    {
        found = memo->found;
        spot = memo->spot;
        gather = memo->gather;
    }
    else
    {
        std::vector<Unit*> others;
        for (ObjectGuid const& guid : possible)
        {
            Unit* mob = botAI->GetUnit(guid);
            if (mob && IsIdleEnemy(bot, mob) && std::find(pack.begin(), pack.end(), mob) == pack.end())
                others.push_back(mob);
        }

        // [b51_strat] the tank's own cleared trail first (ClearOfIdle by the assistance radius)
        float const clearMargin = std::max(sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS), 0.0f);
        found = (clearMargin > 0.0f && FindLosPullTrailSpot(bot, pack, others, castSpot, spot, gather, clearMargin)) ||
                FindLosPullSpot(bot, pack, others, castSpot, spot, gather) ||
                FindLosPullTrailSpot(bot, pack, others, castSpot, spot, gather);  // [b47_plan]

        if (_losMemo.size() >= LOS_PULL_MEMO_SIZE)
            _losMemo.erase(_losMemo.begin());

        LosPullMemo entry;
        entry.mob = candidate->GetGUID();
        entry.ms = now;
        entry.found = found;
        entry.botX = bot->GetPositionX();
        entry.botY = bot->GetPositionY();
        entry.spot = spot;
        entry.gather = gather;
        _losMemo.push_back(entry);

        if (found)
        {
            LOG_DEBUG("playerbots",
                      "[los pull] {} plans on {}: {} mobs, {} casters; tank spot {:.1f} yd from the cast spot "
                      "({:.1f} {:.1f} {:.1f}), group spot {:.1f} yd behind it",
                      bot->GetName(), candidate->GetName(), pack.size(), casters,
                      castSpot.GetExactDist(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ()),
                      spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(),
                      spot.GetExactDist(gather.GetPositionX(), gather.GetPositionY(), gather.GetPositionZ()));
        }
        else
        {
            LOG_DEBUG("playerbots", "[los pull] {}: no hidden spot near the cast spot for {} ({} mobs, {} casters)",
                      bot->GetName(), candidate->GetName(), pack.size(), casters);
        }
    }

    if (!found)
        return false;

    // The fight is at the tank's spot round the corner: the pack counted there too (what reaches it, the calls and
    // flights from it), against the same gate.
    // [lead8] LEADER_FIGHT_SPOT_COUNT, a leader look: counted from the corner whatever its distance from the cast spot
    // (one rule for every method: the fight spot), not only past CAREFUL_AGGRO_MARGIN
    if (castSpot.GetExactDist(spot) > CAREFUL_AGGRO_MARGIN ||
        (Leader::Leads(Leader::LEADER_FIGHT_SPOT_COUNT) && PullPlanner::CurrentLeaderInputs()))
    {
        CollectPack(botAI, bot, candidate, castSpot, possible, pack, FightAt(spot), assistChain, true, nullptr);
        if (std::any_of(pack.begin(), pack.end(), [](Unit* mob) { return IsBossMob(mob); }) || !accept(pack))
        {
            LOG_DEBUG("playerbots", "[los pull] {}: {} mobs at the tank's spot round the corner for {}: no los pull",
                      bot->GetName(), pack.size(), candidate->GetName());
            return false;
        }
    }

    _losSpot = spot;
    _losGather = gather;
    _losPack.clear();
    for (Unit* mob : pack)
        _losPack.push_back(mob->GetGUID());

    return true;
}

// The pull sequence of the "pull" and "pull back" strategies, with the tank's spot round the corner as the place it
// comes back to, and the group's spot shared with every member.
void AggressiveTargetAction::PublishCastSpot(Unit* target)
{
    constexpr uint32 CAST_SPOT_FRESH_MS = 5 * IN_MILLISECONDS;  // found by this plan, not by an earlier one
    auto const found = _castSpots.find(target->GetGUID());
    if (found != _castSpots.end() && getMSTimeDiff(found->second.second, getMSTime()) <= CAST_SPOT_FRESH_MS)
        PullCastSpot::Set(bot, target->GetGUID(), found->second.first);
    else
        PullCastSpot::Clear(bot);

    if (_castSpots.size() > 64)
        _castSpots.clear();
}

namespace PullPlanning  // [lead31a] was anonymous: shared with the tank's execution
{
    constexpr uint32 PULL_CAST_SPOT_TTL_MS = 20 * IN_MILLISECONDS;  // longer than a pull's walk for range
    struct PullCastSpotEntry
    {
        ObjectGuid target;
        Position spot;
        Position kill;
        bool hasKill = false;
        uint32 ms = 0;
    };
    std::mutex pullCastSpotLock;
    std::unordered_map<uint64, PullCastSpotEntry> pullCastSpots;  // by tank
}

namespace PullCastSpot
{
    void Set(Player* tank, ObjectGuid target, Position const& spot)
    {
        std::lock_guard<std::mutex> guard(pullCastSpotLock);
        PullCastSpotEntry& entry = pullCastSpots[tank->GetGUID().GetRawValue()];
        entry.target = target;
        entry.spot = spot;
        entry.hasKill = false;
        entry.ms = getMSTime();
    }

    void SetKill(Player* tank, ObjectGuid target, Position const& kill)
    {
        std::lock_guard<std::mutex> guard(pullCastSpotLock);
        auto const found = pullCastSpots.find(tank->GetGUID().GetRawValue());
        if (found == pullCastSpots.end() || found->second.target != target)
            return;

        found->second.kill = kill;
        found->second.hasKill = true;
    }

    bool GetLatest(Player* tank, ObjectGuid& target, Position& spot, Position& kill, bool& hasKill)
    {
        std::lock_guard<std::mutex> guard(pullCastSpotLock);
        auto const found = pullCastSpots.find(tank->GetGUID().GetRawValue());
        if (found == pullCastSpots.end() || getMSTimeDiff(found->second.ms, getMSTime()) > PULL_CAST_SPOT_TTL_MS)
            return false;

        target = found->second.target;
        spot = found->second.spot;
        kill = found->second.kill;
        hasKill = found->second.hasKill;
        return true;
    }

    void Clear(Player* tank)
    {
        std::lock_guard<std::mutex> guard(pullCastSpotLock);
        pullCastSpots.erase(tank->GetGUID().GetRawValue());
    }

    bool Get(Player* tank, ObjectGuid target, Position& spot)
    {
        std::lock_guard<std::mutex> guard(pullCastSpotLock);
        auto const found = pullCastSpots.find(tank->GetGUID().GetRawValue());
        if (found == pullCastSpots.end() || found->second.target != target ||
            getMSTimeDiff(found->second.ms, getMSTime()) > PULL_CAST_SPOT_TTL_MS)
            return false;

        spot = found->second.spot;
        return true;
    }
}

bool AttackAnythingAction::isPossible() { return GetTarget() && AttackAction::isPossible(); }

// [lead7] C:/WoW/LEADER_REFACTOR_PLAN.md step 7: the careful pull planner as the leader's library (PullPlanner in
// ChooseTargetActions.h). The look (DecideLook) is the tank's own isUseful before step 7, unchanged in its gates,
// costs and thresholds; what changed is who runs it (a planner instance of its own per tank), what it reads (the
// look's Snapshot) and what it returns (a Decision the tank's Execute runs).

// [lead5] Every verdict of a look goes through here: published for the group's crowd control and rest logic (not by a
// shadow look), and kept for the Decision, so the leader reads its own planner's verdict (Steer's PartyRefusal /
// PartyVerdict) instead of the cross-thread registry.
void AggressiveTargetAction::PublishVerdict(ObjectGuid target, bool accepted, uint32 mobs, GuidVector const& pack,
                                            CarefulPlanVerdict::Fight const& fight, bool hopeless,
                                            std::string const& why)
{
    _verdictRec = true;
    _verdictTarget = target;
    _verdictAccepted = accepted;
    _verdictMobs = mobs;
    _verdictPack = pack;
    _verdictHopeless = hopeless;
    _verdictWhy = why;
    _verdictWaitMs = fight.guaranteeWaitMs;
    if (!_shadow)
        CarefulPlanVerdict::Set(bot, target, accepted, mobs, pack, fight, hopeless, why);
}

Unit* AggressiveTargetAction::PlanAggressiveTarget()
{
    return _snap ? _snap->Target() : AI_VALUE(Unit*, "aggressive target");
}

PartyStrength AggressiveTargetAction::PlanStrength()
{
    return _snap ? _snap->Strength() : AI_VALUE(PartyStrength, "party strength");
}

GuidVector AggressiveTargetAction::PlanPossible(bool noLos)
{
    if (_snap)
        return noLos ? _snap->PossibleNoLos() : _snap->Possible();

    return AI_VALUE(GuidVector, noLos ? "possible targets no los" : "possible targets");
}

float AggressiveTargetAction::PlanGroupDps()
{
    return _snap ? _snap->GroupDps() : AI_VALUE(float, "estimated group dps");
}

// [lead33] the planner's danger model on a set of mobs: the same estimate (EstimatePack, the measured mode) and
// overload terms (CarefulOverload) as a plan, against the plan's limit (1.0)
bool AggressiveTargetAction::JudgeMobs(std::vector<Unit*> const& mobs, float& overload, std::string& why)
{
    PartyStrength const strength = PlanStrength();
    GroupSupport const support = CountSupport(bot);
    TankKit const kit = PartyStrengthShared::GetTankKit(bot);
    float const fightsDps = MeasuredGroupDps(bot);  // [lead265] as PlanCarefulPull
    float const groupDps = fightsDps > 0.0f ? fightsDps : std::max(PlanGroupDps(), GroupWeaponDps(bot));
    EstimateMode mode;
    mode.measured = true;
    mode.healHps = support.healHps;  // [lead259]
    mode.healGapSec = support.healGapSec;
    mode.dropScale = DropCalibration(bot);
    LandedRatios const landedRatios = LandedCalibration(bot);  // [lead209]
    mode.landedScale = landedRatios.trash;
    mode.landedByEntry = &landedRatios.byEntry;
    PackEstimate estimate = EstimatePack(mobs, bot, groupDps, mode);
    // [lead201] a boss is judged on his own facts: his health over the group's measured damage, what he (and the adds
    // his script brings) lands on the tank meanwhile against the healers' mana and their healing per second
    if (estimate.boss)
    {
        mode.withBoss = true;
        estimate = EstimatePack(mobs, bot, groupDps, mode);
    }
    OverloadTerms terms;
    CarefulOverload(bot, estimate, strength, support, kit, true, &terms);
    // [lead207] b182 rfc-h 5G 507 s: Taragaman with two Cultists judged "overload 1.18" by the drain term - the tank's
    // 90th percentile health drop modelled on trash pulls (DropP90 of the cost: 0.7 of its health). The b181 facts of
    // the same fights (32 boss fights, 8 comps): the tank's lowest health 73-100 % (one 18 %), 1-65k damage taken over
    // 39-103 s - the healers heal a boss fight as it goes. A boss is judged by the healers' mana and healing rate and
    // by the mobs the tank holds, not by the trash pulls' drop model
    if (estimate.boss)
    {
        terms.drain = 0.0f;
        terms.comfort = 0.0f;
    }
    if (estimate.boss && estimate.mobs == 1)  // a lone boss: CarefulOverload's peak term is a pack's (mobs > 1)
    {
        // [lead258] his average damage per second against the healers' best rate (HealerThroughput)
        if (support.healHps > 0.0f)
            terms.peak = AverageDtps(estimate, PlanTankHealth(bot)) / support.healHps;
    }
    overload = terms.Max();
    bool const fits = !estimate.boss && overload <= 1.0f;
    why = Acore::StringFormat("{} mobs, cost {:.2f} tank hp, fight {:.0f} s, overload {:.2f} of 1.00{}", estimate.mobs,
                              estimate.cost, estimate.fightSec, overload, estimate.boss ? ", a boss" : "");
    if (estimate.boss)
        why += Acore::StringFormat(
            " (healer mana {:.2f}, healing {:.2f}, group dps {:.0f}, landed {:.0f}/s; "
            "healers: {})",
            terms.size, terms.peak, groupDps, estimate.landedDps, support.healerFacts);
    return fits;
}

bool AggressiveTargetAction::LibraryLook(PullPlanner::Snapshot& snap, bool shadow, PullPlanner::Decision& out)
{
    _snap = &snap;
    _shadow = shadow;
    _fromLeader = false;
    bool const wasShadow = PullPlanner::shadowLook;
    PullPlanner::shadowLook = shadow;
    PullPlanner::LeaderInputs const* const wasInputs = PullPlanner::currentInputs;  // [lead3]
    PullPlanner::currentInputs = snap.Leader();
    bool const go = DecideLook();
    FillDecision(go, out);
    // The tank executes a Go decision (Execute): this instance's caches and memos as after it, so the next look
    // plans afresh as the tank's own did.
    if (go)
        MirrorExecute();

    PullPlanner::currentInputs = wasInputs;
    PullPlanner::shadowLook = wasShadow;
    _shadow = false;
    _snap = nullptr;
    return go;
}

void AggressiveTargetAction::FillDecision(bool go, PullPlanner::Decision& out)
{
    out = PullPlanner::Decision();
    uint32 const now = getMSTime();
    out.ms = now ? now : 1;
    out.tank = bot->GetGUID();
    out.careful = _lookCareful;
    out.lookTarget = _lookTarget;
    // [lead5] the verdict this look published, and its aoe_pull_plan, for the leader's own memory
    out.hasVerdict = _verdictRec;
    if (_verdictRec)
    {
        out.verdictTarget = _verdictTarget;
        out.verdictAccepted = _verdictAccepted;
        out.verdictMobs = _verdictMobs;
        out.verdictPack = _verdictPack;
        out.verdictHopeless = _verdictHopeless;
        out.verdictWhy = _verdictWhy;
        out.verdictWaitMs = _verdictWaitMs;
    }
    out.aoePlan = _aoePlanNoted;
    if (!go)
    {
        out.verdict = _lookVerdict;
        out.why = _lookWhy;
        out.readinessHold = _lookVerdict == PullPlanner::Verdict::Held && _lookReadiness;  // [lead108]
        // [lead3] a refusal the leader may take as its whole-pack request's answer only with the group topped up
        if (_lookVerdict == PullPlanner::Verdict::Refused)
            out.toppedUp = IsGroupToppedUp(bot);
        if (_lookVerdict == PullPlanner::Verdict::Refused && _lookCareful)
        {
            out.hopeless = _planVerdictHopeless;
            out.pack = _planVerdictPack;
            out.mobs = _planVerdictFightMobs;  // the refused pack's smallest plan
            out.cost = _planVerdictCost;
        }
        return;
    }

    // [lead58] user: the method is the leader's - an Arcatraz corpse trap never comes back to a pull back / LoS shot
    // (b34_arc): the leader's method for it is the walk-in (it bursts at 8 yd), the tank no longer changes it
    if ((_planPullBack || _planLos) && !_planTarget.IsEmpty())
        if (Unit* trap = botAI->GetUnit(_planTarget); trap && Arcatraz::IsCorpseTrap(trap) && !trap->IsInCombat())
        {
            _planPullBack = false;
            _planLos = false;
            _planReason += "; a corpse trap: walked to (it bursts at 8 yd)";
        }

    out.verdict = PullPlanner::Verdict::Go;
    out.why = _planReason;
    out.approach = _approach;
    out.planTarget = _planTarget;
    out.pullBack = _planPullBack;
    out.los = _planLos;
    out.single = _planSingle;
    out.countsSap = _planCountsSap;
    out.patrolFirst = _planPatrol;
    out.mobs = _planMobs;
    out.cost = _planCost;
    out.drop = _planDrop;
    out.reason = _planReason;
    out.side = _planSide;
    out.sideSpot = _sideSpot;
    out.sideFor = _sideFor;
    out.sideMs = _sideMs;
    out.sideNote = _sideNote;
    out.killFor = _planKillFor;
    out.kill = _planKill;
    out.fightFor = _planFightFor;
    out.fight = _planFight;
    out.fightBack = _planFightBack;
    out.fightNote = _planFightNote;
    out.losSpot = _losSpot;
    out.losGather = _losGather;
    out.losPack = _losPack;
    out.pack = _planVerdictPack;
    if (!_planTarget.IsEmpty())
    {
        constexpr uint32 CAST_SPOT_FRESH_MS = 5 * IN_MILLISECONDS;  // as PublishCastSpot
        auto const found = _castSpots.find(_planTarget);
        if (found != _castSpots.end() && getMSTimeDiff(found->second.second, now) <= CAST_SPOT_FRESH_MS)
        {
            out.hasCast = true;
            out.castSpot = found->second.first;
        }
    }

    // What Execute does with it, in its order.
    if (!_approach.IsEmpty())
        out.kind = "approach";
    else if (_planSide && !_planTarget.IsEmpty() && bot->GetExactDist(_sideSpot) > SIDE_PULL_ARRIVED)
        out.kind = "side walk";
    else if (_planTarget.IsEmpty())
    {
        out.kind = "attack";
        out.method = Leader::PullMethod::Engage;
    }
    else if (!_planPullBack && !_planLos)
    {
        out.kind = "engage";
        out.method = Leader::PullMethod::Engage;
    }
    else
    {
        out.method = _planLos ? Leader::PullMethod::LosPull
                              : (_planSingle ? Leader::PullMethod::One : Leader::PullMethod::PullBack);
        out.kind = Leader::MethodName(out.method);
    }
}

// Execute's resets of the planner's own state, on a library instance that does not execute (the tank does).
void AggressiveTargetAction::MirrorExecute()
{
    if (!_approach.IsEmpty())
    {
        _approach.Clear();
        return;
    }

    if (_planSide && !_planTarget.IsEmpty() && bot->GetExactDist(_sideSpot) > SIDE_PULL_ARRIVED)
    {
        _planFor.Clear();
        return;
    }

    if (_planTarget.IsEmpty())
        return;

    Unit* target = botAI->GetUnit(_planTarget);
    _planFor.Clear();
    _losTempoFor.Clear();
    if (!target || !target->IsInWorld() || !target->IsAlive())
        return;

    bool pullBack = _planPullBack;
    bool losPull = _planLos;
    if ((pullBack || losPull) && Arcatraz::IsCorpseTrap(target) && !target->IsInCombat())
    {
        pullBack = false;
        losPull = false;
    }

    if (_planMobs)
    {
        if (_sideFor == target->GetGUID())
            _sideFor.Clear();

        _planMobs = 0;
    }

    if (!pullBack && !losPull)
    {
        if (bot->IsWithinLOSInMap(target))
            _planFightFor.Clear();

        return;
    }

    _lastPullBack = target->GetGUID();
    _lastPullBackMs = getMSTime();
    if (!losPull && PullStrategy::Get(botAI))
    {
        _planKillFor.Clear();  // PullBack's
        _planFightFor.Clear();
    }
}

namespace PullPlanner
{
    namespace
    {
        struct PlannerEntry
        {
            PlayerbotAI* ai = nullptr;
            Player* tank = nullptr;
            bool shadow = false;
            std::unique_ptr<AggressiveTargetAction> planner;
        };

        struct LeaderSlot
        {
            uint32 askMs = 0;
            bool askTaken = true;
            bool issued = false;
            Decision decision;
        };

        struct DiffState
        {
            DiffCounts counts;
            std::string lastSig;
            uint32 lastLogMs = 0;
            uint32 repeats = 0;
            std::deque<DiffLine> queue;
        };

        constexpr size_t MAX_DIFF_LINES = 64;
        constexpr uint32 DIFF_MIN_GAP_MS = 1000;  // a new disagreement is queued at most once a second

        // The registry maps only: an instance is used by one thread at a time (diff mode: the tank's map thread;
        // leader mode: the world thread while no map thread runs).
        std::mutex plannerLock;
        std::unordered_map<ObjectGuid, PlannerEntry> planners;
        std::mutex leaderLock;
        std::unordered_map<ObjectGuid, LeaderSlot> leaderSlots;
        std::mutex diffLock;
        std::unordered_map<ObjectGuid, DiffState> diffs;

        uint32 NowMs()
        {
            uint32 const now = getMSTime();
            return now ? now : 1;
        }

        std::string GuidShort(ObjectGuid guid)
        {
            return guid.IsEmpty() ? std::string("-") : Acore::StringFormat("{}:{}", guid.GetEntry(), guid.GetCounter());
        }
    }

    Snapshot::Snapshot(PlayerbotAI* tankAI) : _ai(tankAI), _tank(tankAI ? tankAI->GetBot() : nullptr)
    {
        _ms = NowMs();
        if (_tank)
            _tankPos = _tank->GetPosition();
    }

    Unit* Snapshot::Target()
    {
        if (!_hasTarget && _ai)
        {
            // [lead5] LEADER_ONE_PICKER: the leader's own pick (it ran the same choice, AggressiveTargetValue::PickFor,
            // with its own route target), not the tank's "aggressive target"
            if (_leader.bound && Leader::Leads(Leader::LEADER_ONE_PICKER))
            {
                Unit* picked = _leader.pick.IsEmpty() ? nullptr : _ai->GetUnit(_leader.pick);
                _target = picked && picked->IsAlive() && picked->IsInWorld() ? picked : nullptr;
            }
            else
                _target = _ai->GetAiObjectContext()->GetValue<Unit*>("aggressive target")->Get();
            _hasTarget = true;
        }
        return _target;
    }

    PartyStrength Snapshot::Strength()
    {
        if (!_hasStrength && _ai)
        {
            _strength = _ai->GetAiObjectContext()->GetValue<PartyStrength>("party strength")->Get();
            _hasStrength = true;
        }
        return _strength;
    }

    GuidVector Snapshot::Possible()
    {
        if (!_hasPossible && _ai)
        {
            _possible = _ai->GetAiObjectContext()->GetValue<GuidVector>("possible targets")->Get();
            _hasPossible = true;
        }
        return _possible;
    }

    GuidVector Snapshot::PossibleNoLos()
    {
        if (!_hasPossibleNoLos && _ai)
        {
            _possibleNoLos = _ai->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();
            _hasPossibleNoLos = true;
        }
        return _possibleNoLos;
    }

    float Snapshot::GroupDps()
    {
        if (!_hasGroupDps && _ai)
        {
            _groupDps = _ai->GetAiObjectContext()->GetValue<float>("estimated group dps")->Get();
            _hasGroupDps = true;
        }
        return _groupDps;
    }

    char const* VerdictName(Verdict verdict)
    {
        switch (verdict)
        {
            case Verdict::Go:
                return "go";
            case Verdict::Held:
                return "held";
            case Verdict::Refused:
                return "refused";
            default:
                return "none";
        }
    }

    std::string Decision::Describe() const
    {
        std::string line = VerdictName(verdict);
        if (verdict == Verdict::Go)
            line += Acore::StringFormat(" {} {} mobs {} cost {:.2f}{}", kind,
                                        GuidShort(planTarget.IsEmpty() ? approach : planTarget), mobs, cost,
                                        side ? " (side)" : "");
        else if (verdict != Verdict::None)
            line += Acore::StringFormat("{} {}: {}", hopeless ? " hopeless" : "", GuidShort(lookTarget), why);

        return line;
    }

    bool Decide(Player* tank, Snapshot& snap, bool shadow, Decision& out)
    {
        PlayerbotAI* ai = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        if (!ai || snap.Tank() != tank)
            return false;

        AggressiveTargetAction* planner = nullptr;
        {
            std::lock_guard<std::mutex> lock(plannerLock);
            PlannerEntry& entry = planners[tank->GetGUID()];
            if (!entry.planner || entry.ai != ai || entry.tank != tank || entry.shadow != shadow)
            {
                entry.planner = std::make_unique<AggressiveTargetAction>(ai);
                entry.ai = ai;
                entry.tank = tank;
                entry.shadow = shadow;
            }
            planner = entry.planner.get();
        }

        planner->LibraryLook(snap, shadow, out);
        return true;
    }

    bool JudgePack(Player* tank, std::vector<ObjectGuid> const& mobs, float& overload, std::string& why)
    {
        PlayerbotAI* ai = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        if (!ai)
            return false;
        AggressiveTargetAction* planner = nullptr;
        {
            std::lock_guard<std::mutex> lock(plannerLock);
            PlannerEntry& entry = planners[tank->GetGUID()];
            if (!entry.planner || entry.ai != ai || entry.tank != tank)
            {
                entry.planner = std::make_unique<AggressiveTargetAction>(ai);
                entry.ai = ai;
                entry.tank = tank;
                entry.shadow = false;
            }
            planner = entry.planner.get();
        }
        std::vector<Unit*> units;
        for (ObjectGuid const& guid : mobs)
            if (Unit* unit = ObjectAccessor::GetUnit(*tank, guid);
                unit && unit->IsAlive() && std::find(units.begin(), units.end(), unit) == units.end())
                units.push_back(unit);
        return planner->JudgeMobs(units, overload, why);
    }

    bool Plan(Player* tank, Unit* target, Snapshot& snap, Decision& out)
    {
        if (!tank || !target)
            return false;
        if (LeaderInputs const* inputs = snap.Leader())
        {
            LeaderInputs asked = *inputs;
            asked.pick = target->GetGUID();
            snap.BindLeader(asked);
        }
        return Decide(tank, snap, false, out);
    }

    void Release(ObjectGuid tank)
    {
        {
            std::lock_guard<std::mutex> lock(plannerLock);
            planners.erase(tank);
        }
        {
            std::lock_guard<std::mutex> lock(leaderLock);
            leaderSlots.erase(tank);
        }
        std::lock_guard<std::mutex> lock(diffLock);
        diffs.erase(tank);
    }

    void Ask(Player* tank)
    {
        if (!tank)
            return;

        std::lock_guard<std::mutex> lock(leaderLock);
        LeaderSlot& slot = leaderSlots[tank->GetGUID()];
        slot.askMs = NowMs();
        slot.askTaken = false;
    }

    bool TakeAsk(ObjectGuid tank, uint32 maxAgeMs)
    {
        std::lock_guard<std::mutex> lock(leaderLock);
        auto const found = leaderSlots.find(tank);
        if (found == leaderSlots.end() || found->second.askTaken ||
            getMSTimeDiff(found->second.askMs, getMSTime()) > maxAgeMs)
            return false;

        found->second.askTaken = true;
        return true;
    }

    bool AskedWithin(ObjectGuid tank, uint32 ms)
    {
        std::lock_guard<std::mutex> lock(leaderLock);
        auto const found = leaderSlots.find(tank);
        return found != leaderSlots.end() && found->second.askMs &&
               getMSTimeDiff(found->second.askMs, getMSTime()) <= ms;
    }

    void Issue(ObjectGuid tank, Decision const& decision)
    {
        std::lock_guard<std::mutex> lock(leaderLock);
        LeaderSlot& slot = leaderSlots[tank];
        slot.decision = decision;
        // [lead48] b97 rfc-h 98.8-183 s: the committed pull back was re-issued with the decision's old timestamp
        // (decided at the commit), TakeIssued dropped it as stale at once and the tank never got it - the commitment
        // stood "issued" with no order for 85 s. An issue is fresh when it is issued
        uint32 const now = getMSTime();
        slot.decision.ms = now ? now : 1;
        slot.issued = true;
    }

    // [lead49] user: no elapsed-time rule changes a decision - an issued order has no age limit: it stays until the
    // tank takes it or the leader replaces / cancels it (CancelIssued); `maxAgeMs` is no longer read
    bool TakeIssued(ObjectGuid tank, uint32 /*maxAgeMs*/, Decision& out, bool consume)
    {
        std::lock_guard<std::mutex> lock(leaderLock);
        auto const found = leaderSlots.find(tank);
        if (found == leaderSlots.end() || !found->second.issued)
            return false;

        if (consume)
            found->second.issued = false;  // taken once
        out = found->second.decision;
        return true;
    }

    void CancelIssued(ObjectGuid tank)
    {
        std::lock_guard<std::mutex> lock(leaderLock);
        if (auto const found = leaderSlots.find(tank); found != leaderSlots.end())
            found->second.issued = false;
    }

    void ConsumeIssued(ObjectGuid tank, uint32 decisionMs)
    {
        std::lock_guard<std::mutex> lock(leaderLock);
        auto const found = leaderSlots.find(tank);
        if (found != leaderSlots.end() && found->second.issued && found->second.decision.ms == decisionMs)
            found->second.issued = false;
    }

    std::string DiffKinds(Decision const& own, Decision const& library, bool& whyOnly)
    {
        whyOnly = false;
        std::string kinds;
        auto const add = [&kinds](char const* kind)
        {
            if (!kinds.empty())
                kinds += ",";
            kinds += kind;
        };

        if (own.verdict != library.verdict)
            add("verdict");
        else if (own.verdict == Verdict::Go)
        {
            if (own.kind != library.kind)
                add("method");
            if (own.planTarget != library.planTarget || own.approach != library.approach)
                add("target");
            if (own.mobs != library.mobs)
                add("mobs");
            else if (std::fabs(own.cost - library.cost) > 0.01f)
                add("cost");
            if (own.side != library.side)
                add("side");
        }
        else if (own.verdict != Verdict::None)
        {
            if (own.lookTarget != library.lookTarget)
                add("target");
            if (own.hopeless != library.hopeless)
                add("hopeless");
        }

        if (kinds.empty() && own.why != library.why)
            whyOnly = true;

        return kinds;
    }

    void NoteDiff(Player* tank, Decision const& own, Decision const& library)
    {
        if (!tank)
            return;

        bool whyOnly = false;
        std::string const kinds = DiffKinds(own, library, whyOnly);
        uint32 const now = NowMs();
        std::lock_guard<std::mutex> lock(diffLock);
        DiffState& state = diffs[tank->GetGUID()];
        ++state.counts.compared;
        if (kinds.empty())
        {
            ++state.counts.same;
            if (whyOnly)
                ++state.counts.whyOnly;
            return;
        }

        size_t start = 0;
        while (start <= kinds.size())
        {
            size_t const comma = kinds.find(',', start);
            size_t const length = comma == std::string::npos ? std::string::npos : comma - start;
            std::string const kind = kinds.substr(start, length);
            if (!kind.empty())
                ++state.counts.byKind[kind];
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }

        // the same disagreement (kinds, verdicts, what and whom) again: counted, not queued
        std::string const sig = kinds + "|" + VerdictName(own.verdict) + own.kind + GuidShort(own.lookTarget) +
                                GuidShort(own.planTarget) + "|" + VerdictName(library.verdict) + library.kind +
                                GuidShort(library.lookTarget) + GuidShort(library.planTarget);
        uint32 const since = state.lastLogMs ? getMSTimeDiff(state.lastLogMs, now) : Leader::PLAN_DIFF_LOG_MS;
        if ((sig == state.lastSig && since < Leader::PLAN_DIFF_LOG_MS) || since < DIFF_MIN_GAP_MS)
        {
            ++state.repeats;
            return;
        }

        DiffLine line;
        line.ms = now;
        line.mob = own.lookTarget.IsEmpty() ? library.lookTarget : own.lookTarget;
        line.kinds = kinds;
        line.own = own.Describe();
        line.library = library.Describe();
        line.repeats = state.repeats;
        state.repeats = 0;
        state.lastSig = sig;
        state.lastLogMs = now;
        if (state.queue.size() >= MAX_DIFF_LINES)
            state.queue.pop_front();
        state.queue.push_back(std::move(line));
    }

    bool TakeDiff(ObjectGuid tank, DiffLine& out)
    {
        std::lock_guard<std::mutex> lock(diffLock);
        auto const found = diffs.find(tank);
        if (found == diffs.end() || found->second.queue.empty())
            return false;

        out = std::move(found->second.queue.front());
        found->second.queue.pop_front();
        return true;
    }

    bool GetDiffCounts(ObjectGuid tank, DiffCounts& out)
    {
        std::lock_guard<std::mutex> lock(diffLock);
        auto const found = diffs.find(tank);
        if (found == diffs.end() || !found->second.counts.compared)
            return false;

        out = found->second.counts;
        return true;
    }

    // [lead3] LEADER_NO_BODY_FALLBACK: the boss pull as a leader option (the planner never picks a boss; the harness's
    // ROUTE_BOSS_PULL_IMMEDIATE body pull is gone in leader mode). An engage of the boss where he stands: his own
    // encounter pack counted there (the assistance chain in the measured mode, as the careful plan counts it), so
    // the order's expected count is priced. The tank's Execute walks in for sight first, then attacks him.
    bool BossPullDecision(Player* tank, Unit* boss, std::string const& why, Decision& out)
    {
        out = Decision();
        PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        if (!tankAI || !boss || !boss->IsInWorld() || !boss->IsAlive() || boss->IsInCombat() ||
            EncounterOrder::IsLocked(boss))
            return false;

        out.ms = NowMs();
        out.tank = tank->GetGUID();
        out.verdict = Verdict::Go;
        out.careful = true;
        out.lookTarget = boss->GetGUID();
        out.attack = boss->GetGUID();
        out.kind = "boss pull";
        out.method = Leader::PullMethod::BossPull;
        out.why = why;
        out.reason = why;
        PartyStrength const strength =
            tankAI->GetAiObjectContext()->GetValue<PartyStrength>("party strength")->Get();
        bool const measured = GetPullMode(tank, strength) == PullMode::Measured;
        GuidVector const possible =
            tankAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();
        std::vector<Unit*> pack;
        CollectPack(tankAI, tank, boss, boss->GetPosition(), possible, pack, FightHere(boss->GetPosition()),
                    measured, false, nullptr);
        if (std::find(pack.begin(), pack.end(), boss) == pack.end())
            pack.insert(pack.begin(), boss);
        out.mobs = uint32(pack.size());
        for (Unit* mob : pack)
            out.pack.push_back(mob->GetGUID());
        return true;
    }

    // [lead10] The stand's guarantee's whole-pack cap (PlanCarefulPull's overCapMobs: the configured cap scaled by
    // the party strength, the proven size, GUARANTEE_OVER_CAP_SCALE).
    uint32 WholePackCap(Player* tank)
    {
        PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        if (!tankAI)
            return 0;

        PartyStrength const strength =
            tankAI->GetAiObjectContext()->GetValue<PartyStrength>("party strength")->Get();
        PullMode const mode = GetPullMode(tank, strength);
        if (mode == PullMode::Default)
            return 0;

        bool const measured = mode == PullMode::Measured;
        uint32 const maxMobs =
            measured && sPlayerbotAIConfig.CarefulPullMaxMobs
                ? std::max(CAREFUL_MIN_MAX_MOBS,
                           uint32(std::lround(float(sPlayerbotAIConfig.CarefulPullMaxMobs) *
                                              std::clamp(strength.PullScale(), CAREFUL_MAX_MOBS_SCALE_MIN,
                                                         CAREFUL_MAX_MOBS_SCALE_MAX))))
                : 0;
        ProvenCap const proven = ProvenFor(tank);
        uint32 const baseLastResortMobs = maxMobs ? maxMobs : CAREFUL_LAST_RESORT_DEFAULT_CAP;
        uint32 const lastResortMobs = std::max(baseLastResortMobs, uint32(proven.mobs));  // [lead262]
        return std::max<uint32>(uint32(std::ceil(float(baseLastResortMobs) * GUARANTEE_OVER_CAP_SCALE)),
                                lastResortMobs);
    }

    // [lead4] The one patrol forecast (see the header): its waypoint walk against the spot.
    bool PatrolLeavesReach(Creature* patrol, Position const& spot, float reach, bool* known)
    {
        if (known)
            *known = false;
        WaypointPath const* path = patrol ? PatrolPath(patrol) : nullptr;
        if (!path || path->Nodes.empty())
            return true;

        if (known)
            *known = true;
        for (WaypointNode const& node : path->Nodes)
            if (std::fabs(node.Z - spot.GetPositionZ()) > PULL_PATH_HEIGHT ||
                spot.GetExactDist2d(node.X, node.Y) > reach)
                return true;

        return false;
    }
}
