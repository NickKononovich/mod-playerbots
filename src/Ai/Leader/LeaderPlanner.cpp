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
#include "TestRunnerStrategy.h"  // [b32_strat_gate] TestRunner::IsDoorCut
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
            if (why)
                *why = member->GetName() + " buffing";
            return true;
        }

        if ((RestState::IsDrinking(member) && RestState::WantsDrink(member)) ||
            (RestState::IsEating(member) && RestState::WantsFood(member)))
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
    constexpr float TEMPO_MANA_RESERVE = 20.0f;        // healer mana kept back for a pull that goes wrong
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
    constexpr uint32 CAREFUL_SUMMON_FIGHT_MS = 30 * IN_MILLISECONDS;  // the fight a pack mob's summons are counted over
    constexpr uint32 CAREFUL_MAX_SUMMONS = 8;
    // [b36_scholo] a summon's stats against its summoner's are read down to this share; one with less than
    // SUMMON_QUICK_HEALTH_SHARE of its summoner's health dies in the group's area damage at once: it lands its melee
    // over its own kill time, not in the queue of the pack's kills (EstimatePack)
    constexpr float SUMMON_MIN_SHARE = 0.05f;
    constexpr float SUMMON_QUICK_HEALTH_SHARE = 0.3f;
    constexpr float CAREFUL_MIN_HOLD = 2.0f;
    constexpr float CAREFUL_MAX_HOLD = 8.0f;
    // Share of the tank's health a levelling group heals per second: wave 4 healers did 4% (level 15) to 9% (58).
    constexpr float CAREFUL_SUSTAIN_PER_SECOND = 0.045f;
    // The tank's own health carries part of the fight: the healers keep it above half, they need not out-heal the
    // pack. Waves 10-12 5-man normals (levels 15-80, about 5,500 survived pulls): the tank lost 0.6-1.3 of its health
    // over a pull (p90) while its healers healed 3-4% of it per second (p90).
    constexpr float CAREFUL_TANK_POOL = 0.5f;
    // Melee on the tank before the first mob dies, in tank health per second: the median over the level bands of the
    // highest average a survived pull put on the tank (5-10%, waves 10-12).
    constexpr float CAREFUL_PEAK_PER_SECOND = 0.07f;
    // Mobs over what the tank's spells hold that the damage dealers still kill off it (the kill order takes them
    // first). Waves 10-12 levelling normals, tank kits of 2-3: 1-7% of pulls of 1-5 mobs lost someone, 8% at 6,
    // 15% at 7, 33% at 8.
    constexpr float CAREFUL_LOOSE_MOBS = 2.0f;
    // A pack refused this long with the group topped up, after its crowd control had its chance (PrePullCc holds a
    // pull up to 1.5 s for claims and 12 s for a Sap or a trap): the smallest plan found is pulled if it is within
    // the pack-size cap and CAREFUL_LAST_RESORT_MAX_COST, else a lone mob at range, else the pack is left for the
    // next one (wave 12: tanks stood for minutes before 2-mob packs). Waves 10-12: 7-8 mobs lost someone in 8-15%
    // of pulls, 12 and more in 17-21%, and the wipes came from 12 mobs and more.
    constexpr uint32 CAREFUL_STALL_MS = 20 * IN_MILLISECONDS;
    constexpr uint32 CAREFUL_LAST_RESORT_DEFAULT_CAP = 6;  // with Playerbots.CarefulPullMaxMobs 0
    // The most a fallback or last-resort plan may cost, in tank healths: wave 13 Razorfen Kraul pulled a plan of
    // 1.08 and the tank died; above it the tank takes a single mob at range, or leaves the pack.
    constexpr float CAREFUL_LAST_RESORT_MAX_COST = 0.9f;
    // [careful_ready_last_resort] User rule 2026-10-25 (combat time as high as possible): a ready group (topped up,
    // nobody sitting below its own rest line) takes the last resort / the hard minimum on the first refused look when
    // its smallest plan is within the pack-size cap - the CAREFUL_STALL_MS wait stays for packs over the cap only. A
    // look that built no plan at all (cost 0, "best 0 mobs": every way in woke a mob, no danger estimate) goes to the
    // hard minimum at once too. st-n 2026-10-25 08:49: Nightmare Wanderer, 3 mobs at cost 0.95, stood 20 s + 11 s and
    // the fight left the tank at 92 %; 132 of the wave's 432 refusals were "at cost 0.00".
    constexpr bool CAREFUL_READY_LAST_RESORT = true;
    // [careful_small_pack_cost] The cost cap of the last resort and of the fallback for a small plan. The calibrated
    // drop law below (waves 13/15) puts the tank's p90 lowest health over a pull of cost 1.0 at a drop of 0.32 (melee)
    // / 0.36 (casters), well inside CAREFUL_MAX_DROP 0.6. The deaths behind the 0.9 / 0.8 caps were 7 mobs at 1.82
    // (RFC) and RFK at 1.08; the caps refused 3 mobs at 0.95 (the fight: tank >= 92 %) while the guarantee took 6 at
    // 0.90 after the stand. A plan of at most CAREFUL_SMALL_PACK_MOBS mobs is held to CAREFUL_SMALL_PACK_MAX_COST.
    constexpr uint32 CAREFUL_SMALL_PACK_MOBS = 3;
    constexpr float CAREFUL_SMALL_PACK_MAX_COST = 1.0f;
    float SmallPackCostCap(uint32 mobs, float cap)
    {
        return mobs <= CAREFUL_SMALL_PACK_MOBS ? std::max(cap, CAREFUL_SMALL_PACK_MAX_COST) : cap;
    }
    constexpr float CAREFUL_EXTRA_HEALER_SHARE = 0.5f;   // a second healer adds half again
    constexpr float CAREFUL_NO_HEALER_SHARE = 0.5f;
    constexpr float CAREFUL_LANDED_SHARE = 0.7f;         // melee that is not dodged, parried, blocked or missed
    constexpr float CAREFUL_ARMOR_CAP = 0.75f;
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
    // single costs under CAREFUL_FALLBACK_MAX_COST and the healer sees its fight; else the CAREFUL_STALL_MS path.
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
    // mobs in Stratholme and Scholomance and the group wiped, while packs of 3-5 went through.
    constexpr float MEASURED_MAX_AVOIDANCE = 0.75f;      // miss + dodge + parry the estimate trusts at most
    constexpr float MEASURED_MIN_LANDED = 0.25f;
    constexpr float LOS_CASTER_SPELL_SHARE = 0.5f;       // a caster round the corner still casts this share
    // The rescale multiplied mob weapon damage by about 10-12 but their spells only by 4.0-4.5 (Classic) or 2.4-2.8
    // (TBC) (raid_rescale PLAN.md section 12): a caster's weapon damage overstates its spells about twice there.
    constexpr float MEASURED_CASTER_SPELL_SCALE = 0.5f;
    // Outside the rescale a standing caster's weapon damage also overstates what its spells land on the tank (it
    // casts on others too, is interrupted and controlled): zf-n wave 8 (level 45) Witch Doctor / Shadowcaster packs
    // took the tank 0.1-0.5 of its health against 1.3 planned (TakenCost), ramps-n wave 8 (level 62) a Bleeding
    // Hollow Scryer pack 0.2 against 0.66. Both about 3 times; read at the same share as the rescale's.
    constexpr float CASTER_SPELL_SCALE = 0.5f;
    // Tank healths one healer heals from the mana above its reserve (a level 80 healer: about 25k mana at 2.5-3
    // health per mana against a 40k tank).
    constexpr float CAREFUL_HEALER_TANK_HEALTHS = 2.0f;
    constexpr float CAREFUL_MIN_GROUP_DPS = 100.0f;
    // Measured careful pulls, calibrated on wave 13 (380 executed plans with their outcome) and checked on wave 15:
    // the planned cost overstates large packs (the group kills them faster than one by one, stuns and area damage
    // help), so the tank's lowest health over a pull follows the plan's cost by a power law. Median drop (share of
    // the tank's health): 0.146 cost^0.41 for melee packs, 0.164 cost^0.66 for packs with casters (spells ignore
    // armor and avoidance); the 90th percentile is 2.2 times the median in both waves.
    constexpr float DROP_MELEE_SCALE = 0.146f;
    constexpr float DROP_MELEE_EXPONENT = 0.41f;
    constexpr float DROP_SPELL_SCALE = 0.164f;
    constexpr float DROP_SPELL_EXPONENT = 0.66f;
    constexpr float DROP_P90 = 2.2f;
    // (a) The tank survives: in 9 pulls of 10 it keeps 40% of its health. Waves 13 and 15: no plan within it lost
    // someone except where more mobs came than were counted.
    constexpr float CAREFUL_MAX_DROP = 0.6f;
    // The size the group prefers when the pack can be split: the 90th percentile drop of the median 3-mob plan at
    // the content's target gear (wave 13: cost 0.70). A tank with more health or mitigation, or a pack of softer
    // mobs, fits more mobs in it; a soft tank fewer.
    constexpr float CAREFUL_COMFORT_DROP = 0.28f;
    // (b) The healers keep up: the pack's average melee on the tank (0.71 of the planned landed melee over the
    // fight, median of wave 13) within 8% of the tank's health per second per healer, the most a survived pull
    // put on its tank (waves 13-15; the Steamvault tank that took 10%/s died).
    constexpr float CAREFUL_DTPS_SHARE = 0.71f;
    constexpr float CAREFUL_MAX_DTPS = 0.08f;
    // Healer mana the pull takes: the damage the tank really took against the plan (wave 13 median 0.33 cost^0.73).
    constexpr float CAREFUL_TAKEN_SCALE = 0.33f;
    constexpr float CAREFUL_TAKEN_EXPONENT = 0.73f;
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
    constexpr uint32 MEASURED_DPS_MIN_FIGHT_MS = 6 * IN_MILLISECONDS;
    constexpr uint32 MEASURED_DPS_TTL_MS = 15 * MINUTE * IN_MILLISECONDS;
    constexpr float MEASURED_DPS_TRUST = 0.8f;   // the share of the measured damage the planner counts on
    constexpr float MEASURED_DPS_WEIGHT = 0.5f;  // the newest fight's weight in the running value
    // The same for the damage the tank really takes against what the plan expected of it (TakenCost of the planned
    // cost): the pack's landed damage is scaled down to it, never up. zf-n wave 8: landed 129-247/s planned, 40-50/s
    // taken. Only fights whose mob count came as planned (+1), after LANDED_CALIBRATION_MIN_FIGHTS of them; the
    // measured ratio times LANDED_CALIBRATION_SAFETY, at least LANDED_CALIBRATION_FLOOR. The hard cap of the lone
    // mob (CAREFUL_LAST_RESORT_HARD_CAP) judges a cost scaled by at least LANDED_HARD_CAP_FLOOR.
    constexpr float LANDED_CALIBRATION_MIN_COST = 0.05f;
    constexpr uint32 LANDED_CALIBRATION_MIN_FIGHTS = 2;
    constexpr float LANDED_CALIBRATION_SAFETY = 1.5f;
    constexpr float LANDED_CALIBRATION_FLOOR = 0.4f;
    constexpr float LANDED_HARD_CAP_FLOOR = 0.6f;
    // A tank's calibration from its last instance (the model's error, not the place's) seeds the next one halfway
    // to 1 and counts as one fight there less, if it is at most this old.
    constexpr uint32 LANDED_SEED_TTL_MS = 60 * MINUTE * IN_MILLISECONDS;
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
    // A fallback plan (topped up, or the last resort) never costs this much of the tank's health or more: only the
    // hard-minimum lone mob is left then. RFC 2026-10-24 18:28: a topped-up fallback los pull of cost 1.82 (182 % of
    // the tank's health), 7 mobs came, the tank died.
    constexpr float CAREFUL_FALLBACK_MAX_COST = 0.8f;
    // The hard minimum (the lone mob of a pack nothing fits, the quick single) is pulled only while what it brings
    // costs at most this. Wave 44 Deadmines (Gilnid's foundry): "one (every plan too dangerous) Goblin Craftsman,
    // 12 mobs, cost 15.41" was pulled, 20 came, the group died. Over it: a pull round a corner, a wait for a mover of
    // its pack to walk off, the group's pre-pull crowd control (the refused pack), else "[pull blocked]".
    // Sweep batch2 (21:27): at 1.2 the cap blocked ordinary 3-4 mob packs (cost 1.37-2.76: RFK Quilguards, ZF,
    // Uldaman, Magister's Terrace) and stalled the runs; wave 44 took such packs (Searing Blade 4 mobs, 1.37)
    // without a death. 3.0 still blocks the 8+ mob pulls (4.36-15.41).
    constexpr float CAREFUL_LAST_RESORT_HARD_CAP = 3.0f;
    // [b47_plan] The hard minimum's lone mob that costs more than this while a mover it brings (a patrol walking in,
    // a wanderer) can walk off waits for it first, as long as the stand's mover wait runs (2 x CAREFUL_BLOCK_
    // ESCALATE_MS): brd-h 20261026-093620-brd-h-5B-6 took "one ... Anvilrage Soldier, 6 mobs, cost 1.75" with the
    // Blazing Fireguard patrol 28.7 yd off counted in it; 11 came, the wipe.
    constexpr float CAREFUL_LAST_RESORT_SOFT_COST = 1.25f;
    // The escalation of that cap (x2, then x3 after twice this) while the tank stands topped up without a pull; the
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
    // [b30_overpull] ... and what such a pack costs stays within this (the hard cap's top escalation, x3): over it the
    // over-cap pack is no guarantee but "no safe pull" (a corner, a mover's walk-off or crowd control still may).
    constexpr float GUARANTEE_OVER_CAP_MAX_COST = 3.0f * CAREFUL_LAST_RESORT_HARD_CAP;
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
        return std::max(0.0f, healerMana - TEMPO_MANA_RESERVE) / (TEMPO_MANA_PER_DANGER * std::max(danger, 1.0f));
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
    };

    GroupSupport CountSupport(Player* bot)
    {
        GroupSupport support;
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || !member->IsAlive() || member->GetMapId() != bot->GetMapId() ||
                    ToppedUpWatch::Stranded(bot, member))  // [b30_topped_up] no support from a stranded healer
                    continue;

                ++support.helpers;
                if (PlayerbotAI::IsHeal(member))
                    ++support.healers;
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
        // [b32_strat_gate] a member the harness found cut off behind a closed door (TestRunner::SetDoorCut) holds every
        // pull, however near it stands: strat-n 20261025-221216-strat-n-5H-2, the healer 8 yd behind the shut Service
        // Entrance Gate counted "with the group" (GATHER_HEALER_RANGE) while it could neither heal nor come.
        // [lead10] LEADER_DOOR_INPUTS: on a leader look the leader's own door cut (LeaderInputs::doorCut), not the
        // TestRunner registry the harness wrote for the tank's thread.
        PullPlanner::LeaderInputs const* const doorInputs =
            Leader::Leads(Leader::LEADER_DOOR_INPUTS) ? PullPlanner::CurrentLeaderInputs() : nullptr;
        auto const doorCut = [doorInputs](Player* member)
        {
            if (doorInputs)
                return std::find(doorInputs->doorCut.begin(), doorInputs->doorCut.end(), member->GetGUID()) !=
                       doorInputs->doorCut.end();
            return TestRunner::IsDoorCut(member->GetGUID());
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
        uint64 const key = creature->GetGUID().GetRawValue();
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
        uint64 const key = walker->GetGUID().GetRawValue();
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
        PathGenerator path(bot);
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
    // in (SMART_ACTION_CALL_FOR_HELP). A summon on a repeating timer counts once per repeat in
    // CAREFUL_SUMMON_FIGHT_MS; out-of-combat summons (pets) are already standing there. Read once per entry and map
    // thread.
    struct ScriptedHelp
    {
        uint32 summons = 0;
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

            uint32 times = 1;
            if (event == SMART_EVENT_UPDATE_IC && trigger->event.minMaxRepeat.repeatMin)
                times += CAREFUL_SUMMON_FIGHT_MS / std::max(trigger->event.minMaxRepeat.repeatMin, uint32(1000));

            help.summons += times;
            // [b36_scholo] the summon's own stats against the summoner's (see ScriptedHelp)
            float healthShare = 1.0f;
            float damageShare = 1.0f;
            CreatureTemplate const* own = creature->GetCreatureTemplate();
            CreatureTemplate const* summon = summonEntry ? sObjectMgr->GetCreatureTemplate(summonEntry) : nullptr;
            if (own && summon && own->ModHealth > 0.0f && own->DamageModifier > 0.0f)
            {
                healthShare = std::clamp(summon->ModHealth / own->ModHealth, SUMMON_MIN_SHARE, 1.0f);
                damageShare = std::clamp(summon->DamageModifier / own->DamageModifier, SUMMON_MIN_SHARE, 1.0f);
            }
            summonHealthSum += healthShare * float(times);
            summonDamageSum += damageShare * float(times);
        }

        if (help.summons)
        {
            help.summonHealth = summonHealthSum / float(help.summons);
            help.summonDamage = summonDamageSum / float(help.summons);
        }
        help.summons = std::min(help.summons, CAREFUL_MAX_SUMMONS);
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
                    PathGenerator walk(caller);
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
                                   CAREFUL_MIN_GROUP_DPS);

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

    // What the tank's own stats take off a melee hit that armor already reduced: the miss, dodge and parry of its
    // character sheet, and the block value of its shield on a blocked hit (measured careful pulls only).
    struct TankMitigation
    {
        float avoidance = 0.0f;  // share of swings that miss, are dodged or parried
        float block = 0.0f;      // share of swings that are blocked
        float blockValue = 0.0f;
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
        float avoidance = 0.0f;  // miss + dodge + parry, percent
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
            stats.avoidance =
                tank->GetUnitMissChance(BASE_ATTACK) + tank->GetUnitDodgeChance() + tank->GetUnitParryChance();
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
        float const avoidance =
            bear ? bear->avoidance
                 : tank->GetUnitMissChance(BASE_ATTACK) + tank->GetUnitDodgeChance() + tank->GetUnitParryChance();
        mitigation.avoidance = std::clamp(avoidance / 100.0f, 0.0f, MEASURED_MAX_AVOIDANCE);
        mitigation.block = std::clamp(tank->GetUnitBlockChance() / 100.0f, 0.0f, 1.0f - mitigation.avoidance);
        mitigation.blockValue = mitigation.block > 0.0f ? float(tank->GetShieldBlockValue()) : 0.0f;
        return mitigation;
    }

    // Melee the tank takes per second from this mob after armor and avoidance. Casters (MobRoles: they stand and cast)
    // hit with spells, which armor does not reduce; their weapon damage is the same order as their spells. A paladin
    // class mob with mana is not a caster for that alone: wave 12 read Arcatraz Warders, Sunblade Mage Guards and
    // Bloodwarder Protectors (melee guards) as casters, 2.7 times their measured damage on the tank. With `mitigation`
    // (measured careful pulls) the avoidance is the tank's own instead of the levelling estimate, `rescaledSpells`
    // reads a caster's spells at the rescale's lower spell multiplier, and a caster pulled round a corner
    // (`casterInMelee`) spends part of the fight in melee, where armor and avoidance count.
    // `spellPart` (optional): the share of the result that is spells, which armor and avoidance do not reduce.
    float LandedMeleeDps(Unit* mob, Player* tank, TankMitigation const* mitigation = nullptr,
                         bool casterInMelee = false, bool rescaledSpells = false, float* spellPart = nullptr)
    {
        if (spellPart)
            *spellPart = 0.0f;

        uint32 const attackTime = mob->GetAttackTime(BASE_ATTACK);
        if (!attackTime)
            return 0.0f;

        float const hit = (mob->GetFloatValue(UNIT_FIELD_MINDAMAGE) + mob->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2.0f;
        float const dps = hit * float(IN_MILLISECONDS) / float(attackTime);
        bool const caster = MobRoles::IsCaster(mob);
        float const spell =
            dps * CAREFUL_LANDED_SHARE * (rescaledSpells ? MEASURED_CASTER_SPELL_SCALE : CASTER_SPELL_SCALE);
        if (caster && !casterInMelee)
        {
            if (spellPart)
                *spellPart = spell;

            return spell;
        }

        // armor against an attacker of this level (3.3.5 formula, two bands)
        float const level = float(mob->GetLevel());
        float const constant = level < 60.0f ? 400.0f + 85.0f * level : 467.5f * level - 22167.5f;
        float const armor = PlanTankArmor(tank);  // [b32_smpull] a bear out of form: its bear's armor
        float const reduction = std::clamp(armor / std::max(armor + constant, 1.0f), 0.0f, CAREFUL_ARMOR_CAP);
        float share = CAREFUL_LANDED_SHARE;
        if (mitigation)
        {
            float const armoredHit = hit * (1.0f - reduction);
            float const blocked = armoredHit > 0.0f ? std::min(mitigation->blockValue / armoredHit, 1.0f) : 0.0f;
            share = std::max(1.0f - mitigation->avoidance - mitigation->block * blocked, MEASURED_MIN_LANDED);
        }

        float const melee = dps * (1.0f - reduction) * share;
        if (caster)
        {
            if (spellPart)
                *spellPart = LOS_CASTER_SPELL_SHARE * spell;

            return LOS_CASTER_SPELL_SHARE * spell + (1.0f - LOS_CASTER_SPELL_SHARE) * melee;
        }

        return melee;
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
        // [adaptive_cap] the count held against the group's proven strength in this run (ProvenWeight): a caster or
        // a rare elite counts more than a melee elite, a weak or non-elite mob less
        float provenMobs = 0.0f;
        uint32 rangedMobs = 0;  // [adaptive_cap_b29] mobs that keep their range (KeepsRange)
        bool boss = false;
        // [b44_packs] the pack fought as one stack under the group's AoE (EstimateMode::aoeTargetDps > 0): its kill
        // time and the tank healths it lands meanwhile (every mob alive most of that time); 0: not estimated
        float aoeSec = 0.0f;
        float aoeCost = 0.0f;
        float aoeSpellCost = 0.0f;
    };

    // [b44_packs] User 2026-10-26: «некоторые паки из 20 мобов - это мобы с одного удара, их надо просто АОЕшить».
    // A pack that dies to the group's AoE in a few seconds is pulled whole, as a real group does, if the tank and the
    // healers carry what it lands meanwhile. PACK_AUDIT 2026-10-26 (pull_end of the 20261026-04..07 runs, 24 contents,
    // 2600 pulls of 3+ kills): the AoE spells' damage per second per mob alive (alive: (kills + 1) / 2) is 4-14 % of
    // the group's damage per second on heroic, median 8.4 % (strat-h 4.2, scholo-h 4.3, mgt-h 4.9, dme-h 8.1, sv-h
    // 8.6, sm-h 8.9, mara-h 10.7, brd-h 14.3), with a tank with AoE threat and 1-3 AoE damage dealers;
    // WEAK_PACK_AOE_TARGET_SHARE is that median for the usual comp (an AoE tank and two AoE dealers), scaled by how
    // many the group has here.
    constexpr float WEAK_PACK_AOE_TARGET_SHARE = 0.08f;
    constexpr uint32 WEAK_PACK_AOE_MAX_TARGETS = 10;    // a melee stack the AoE covers at once (the rest waits its turn)
    constexpr float WEAK_PACK_ALIVE_SHARE = 0.75f;      // AoE kills the stack together: most of it lives most of it
    constexpr float WEAK_PACK_TTK_SEC = 12.0f;          // "dies in 10-15 s to AoE" (user)
    constexpr uint32 WEAK_PACK_MIN_MOBS = 3;            // fewer is no AoE pack (the normal gates take it)
    constexpr float WEAK_PACK_MAX_RANGED_SHARE = 0.25f; // casters / shooters stand apart: no stack for the AoE

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

    // [adaptive_cap] User 2026-10-25 (scholo-h 153805: «тупят, излишне ждут» - 26 and 16 mobs per pull with no death,
    // yet a lone Adept refused at the cap 9): the weight of one mob in the group's proven strength, the same for
    // what it fought (ProvenStrength) and what a plan brings (PackEstimate::provenMobs). An elite melee mob is 1; a
    // caster or shooter (KeepsRange: it stands and casts, the healer cannot outrange it) PROVEN_CASTER_WEIGHT; a rare
    // elite PROVEN_RARE_WEIGHT; a non-elite mob PROVEN_NORMAL_WEIGHT and a weak swarm add CAREFUL_SWARM_CAP_WEIGHT.
    constexpr float PROVEN_CASTER_WEIGHT = 1.3f;
    constexpr float PROVEN_RARE_WEIGHT = 1.5f;
    constexpr float PROVEN_NORMAL_WEIGHT = 0.75f;
    constexpr float PROVEN_SUMMON_WEIGHT = 0.5f;  // a summoned add read as an average mob (EstimatePack)

    float ProvenWeight(Unit* mob, Player* tank)
    {
        Creature* creature = mob->ToCreature();
        if (!creature)
            return 1.0f;

        float weight = 1.0f;
        if (!creature->isElite() && !creature->IsDungeonBoss())
            weight = CapWeight(mob, tank) < 1.0f ? CAREFUL_SWARM_CAP_WEIGHT : PROVEN_NORMAL_WEIGHT;
        else if (creature->GetCreatureTemplate()->rank == CREATURE_ELITE_RAREELITE)
            weight = PROVEN_RARE_WEIGHT;

        if (KeepsRange(creature))
            weight *= PROVEN_CASTER_WEIGHT;

        return weight;
    }

    // How the pack's melee is read: `measured` with the tank's own avoidance and block (a measured careful pull),
    // `rescaledSpells` with the rescale's lower spell multiplier for casters (heroic or max-level content).
    struct EstimateMode
    {
        bool measured = false;
        bool rescaledSpells = false;
        float landedScale = 1.0f;  // the tank's measured calibration (LandedCalibration), at most 1
        float aoeTargetDps = 0.0f;  // [b44_packs] the group's AoE damage per second on each stacked mob (0: none)
    };

    // `groupDps` > 0: the fight's cost and length are read from the mobs' health too. The group kills one mob after
    // another in no known order: a mob lives its own kill time plus, on average, half of the others', and lands its
    // melee on the tank all that time.
    PackEstimate EstimatePack(std::vector<Unit*> const& pack, Player* tank, float groupDps, EstimateMode const& mode,
                              bool losPull = false)
    {
        PackEstimate estimate;
        TankMitigation const mitigation = mode.measured ? GetTankMitigation(tank) : TankMitigation();
        float totalHealth = 0.0f;
        float landedSpell = 0.0f;
        std::vector<std::pair<float, float>> lives;  // landed melee, health
        std::vector<float> spells;                   // the spell part of each life's landed damage
        uint32 summoned = 0;
        float summonHealthSum = 0.0f;  // [b36_scholo] the summons' shares of an average mob (ScriptedHelp)
        float summonDamageSum = 0.0f;
        std::vector<bool> quick;  // [b36_scholo] a weak summon: lives its own kill time only
        for (Unit* mob : pack)
        {
            estimate.danger += MobDanger(mob, tank);
            ++estimate.mobs;
            float const chainWeight = ChainStepWeight(pack, mob);  // [chain_step_weight]
            estimate.capMobs += CapWeight(mob, tank) * chainWeight;
            estimate.provenMobs += ProvenWeight(mob, tank) * chainWeight;
            if (Creature* ranged = mob->ToCreature(); ranged && KeepsRange(ranged))
                ++estimate.rangedMobs;
            if (IsBossMob(mob))
            {
                estimate.boss = true;
                continue;
            }

            if (Creature* creature = mob->ToCreature())
            {
                ScriptedHelp const& help = GetScriptedHelp(creature);
                summoned += help.summons;
                summonHealthSum += help.summonHealth * float(help.summons);  // [b36_scholo]
                summonDamageSum += help.summonDamage * float(help.summons);
            }

            float spell = 0.0f;
            float const landed = LandedMeleeDps(mob, tank, mode.measured ? &mitigation : nullptr,
                                                mode.measured && losPull, mode.rescaledSpells, &spell) *
                                 mode.landedScale;
            spell *= mode.landedScale;
            if (spell > 0.0f)
                ++estimate.casters;

            estimate.landedDps += landed;
            landedSpell += spell;
            float const health = float(mob->GetHealth());
            totalHealth += health;
            lives.emplace_back(landed, health);
            spells.push_back(spell);
            quick.push_back(false);
        }

        // What the pack summons while it fights, each read as an average mob of the pack, [b36_scholo] scaled by the
        // summon's own health and damage against its summoner's (ScriptedHelp).
        if (summoned && !lives.empty())
        {
            float const healthShare = summonHealthSum / float(summoned);
            float const damageShare = summonDamageSum / float(summoned);
            float const share = 1.0f / float(lives.size());
            float const landed = estimate.landedDps * share * damageShare;
            float const spell = landedSpell * share * damageShare;
            float const health = totalHealth * share * healthShare;
            float const danger = estimate.danger / float(estimate.mobs);
            for (uint32 i = 0; i < summoned; ++i)
            {
                lives.emplace_back(landed, health);
                spells.push_back(spell);
                quick.push_back(healthShare < SUMMON_QUICK_HEALTH_SHARE);
                estimate.landedDps += landed;
                landedSpell += spell;
                totalHealth += health;
                estimate.danger += danger;
                ++estimate.mobs;
                estimate.capMobs += 1.0f;
                estimate.provenMobs += PROVEN_SUMMON_WEIGHT;
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
        }

        // [b44_packs] The same pack under the group's AoE: the group's damage on its first target plus the AoE on the
        // other stacked mobs (the melee ones; casters keep their range out of it), at most WEAK_PACK_AOE_MAX_TARGETS
        // at once; the whole pack lands its melee and spells for WEAK_PACK_ALIVE_SHARE of that time.
        if (groupDps > 0.0f && mode.aoeTargetDps > 0.0f && !estimate.boss && tank->GetMaxHealth() &&
            totalHealth > 0.0f)
        {
            uint32 const stacked = estimate.mobs - std::min(estimate.mobs, estimate.rangedMobs);
            float const hit = float(std::min(stacked, WEAK_PACK_AOE_MAX_TARGETS));
            float const killDps = groupDps + mode.aoeTargetDps * std::max(hit - 1.0f, 0.0f);
            float const tankHealth = PlanTankHealth(tank);
            estimate.aoeSec = totalHealth / killDps;
            if (tankHealth > 0.0f)
            {
                estimate.aoeCost = estimate.landedDps * estimate.aoeSec * WEAK_PACK_ALIVE_SHARE / tankHealth;
                estimate.aoeSpellCost = landedSpell * estimate.aoeSec * WEAK_PACK_ALIVE_SHARE / tankHealth;
            }
        }

        return estimate;
    }

    // Tank healths the pull really costs the healers (see CAREFUL_TAKEN_SCALE).
    float TakenCost(PackEstimate const& estimate)
    {
        return estimate.cost > 0.0f ? CAREFUL_TAKEN_SCALE * std::pow(estimate.cost, CAREFUL_TAKEN_EXPONENT) : 0.0f;
    }

    float HealerShare(GroupSupport const& support)
    {
        return support.healers ? 1.0f + CAREFUL_EXTRA_HEALER_SHARE * float(support.healers - 1)
                               : CAREFUL_NO_HEALER_SHARE;
    }

    // Tank healths the group's healers restore from full mana, above the reserve kept for a pull that goes wrong.
    float HealerPool(GroupSupport const& support)
    {
        return CAREFUL_HEALER_TANK_HEALTHS * HealerShare(support) * (100.0f - TEMPO_MANA_RESERVE) / 100.0f;
    }

    // Healer mana (percent) a measured careful pull of this cost needs before it starts.
    float HealerManaNeed(PackEstimate const& estimate, GroupSupport const& support)
    {
        float const perCent = CAREFUL_HEALER_TANK_HEALTHS * HealerShare(support) / 100.0f;
        return std::min(TEMPO_MANA_RESERVE + TakenCost(estimate) / perCent, 100.0f);
    }

    // Each limit a pack is held against, as a share of it (1.0: just fits).
    struct OverloadTerms
    {
        float size = 0.0f;     // danger against the group's capacity (levelling) or the healers' mana (measured)
        float hold = 0.0f;     // mobs against what the tank's spells hold, plus the loose mobs the dps kill off it
        float peak = 0.0f;     // melee on the tank before a mob dies (levelling) or its average against what the
                               // healers heal (measured)
        float drain = 0.0f;    // the fight's melee against what the healers heal over it plus half the tank's health
                               // (levelling) or the tank's 90th percentile health drop against CAREFUL_MAX_DROP
        float comfort = 0.0f;  // measured: the drop against the size the group prefers (CAREFUL_COMFORT_DROP)
        float drop = 0.0f;     // measured: the tank's 90th percentile health drop
        float holdMobs = 0.0f;

        // What the group can take at all; `withComfort`: and prefers when it has a choice.
        float Max(bool withComfort = false) const
        {
            return std::max({size, hold, peak, drain, withComfort ? comfort : 0.0f});
        }
    };

    // Measured careful pulls: the tank's health drop over the fight in 9 pulls of 10, from the planned cost (see
    // DROP_MELEE_SCALE), weighted by how much of it is spells.
    float DropP90(PackEstimate const& estimate)
    {
        if (estimate.cost <= 0.0f)
            return 0.0f;

        float const spellShare = std::clamp(estimate.spellCost / estimate.cost, 0.0f, 1.0f);
        float const melee = DROP_MELEE_SCALE * std::pow(estimate.cost, DROP_MELEE_EXPONENT);
        float const spell = DROP_SPELL_SCALE * std::pow(estimate.cost, DROP_SPELL_EXPONENT);
        return DROP_P90 * ((1.0f - spellShare) * melee + spellShare * spell);
    }

    // How far the pack is over what the group takes (1.0: just fits): the group's danger capacity (levelling) or the
    // healers' mana for the fight (measured), the tank's spells hold the pack but for a couple of loose mobs, and the
    // tank's health carries the melee it takes, at once and over the fight. About 4-5 mobs for a tank without area
    // threat, more with Consecration or Thunder Clap, fewer for a soft group. Measured: (a) the tank keeps
    // CAREFUL_MAX_DROP of its health in 9 pulls of 10 and (b) the healers keep up with the pack's average damage
    // and have the mana for it; the tank's own health, armor and avoidance are in the cost, the healers' gear scales
    // what they heal.
    float CarefulOverload(Player* bot, PackEstimate const& estimate, PartyStrength const& strength,
                          GroupSupport const& support, TankKit const& kit, bool measured,
                          OverloadTerms* out = nullptr)
    {
        OverloadTerms terms;
        terms.size = measured ? TakenCost(estimate) / HealerPool(support)
                              : SwarmDanger(estimate.danger, estimate.mobs, strength) /
                                    PackCapacity(bot, strength, support);
        if (estimate.mobs > 1)
        {
            terms.holdMobs =
                std::clamp(std::round(kit.holdMobs * strength.PullScale()), CAREFUL_MIN_HOLD, CAREFUL_MAX_HOLD) +
                CAREFUL_LOOSE_MOBS;
            terms.hold = float(estimate.mobs) / terms.holdMobs;

            float const maxHealth = PlanTankHealth(bot);  // [b32_smpull]
            if (measured)
            {
                terms.drop = DropP90(estimate);
                terms.drain = terms.drop / CAREFUL_MAX_DROP;
                terms.comfort = terms.drop / CAREFUL_COMFORT_DROP;
                float const healing = maxHealth * CAREFUL_MAX_DTPS * HealerShare(support) * strength.healer;
                float const mobs = float(estimate.mobs);
                if (healing > 0.0f)
                    terms.peak = CAREFUL_DTPS_SHARE * estimate.landedDps * (mobs + 1.0f) / (2.0f * mobs) / healing;
            }
            else
            {
                float const share = strength.PullScale() * HealerShare(support);
                if (maxHealth > 0.0f && share > 0.0f)
                {
                    terms.peak = estimate.landedDps / (maxHealth * CAREFUL_PEAK_PER_SECOND * share);
                    terms.drain =
                        estimate.cost / (CAREFUL_SUSTAIN_PER_SECOND * share * estimate.fightSec + CAREFUL_TANK_POOL);
                }
            }
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

        uint64 const key = pet->GetGUID().GetRawValue();
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
    std::string BossReadyGap(Player* bot, bool askBuffs, bool realBoss = true)
    {
        float const readyHealth = realBoss ? BOSS_READY_HEALTH : MINI_BOSS_READY_HEALTH;
        float const readyMana = realBoss ? BOSS_READY_MANA : MINI_BOSS_READY_MANA;
        auto const gap = [readyHealth, readyMana](Player* member) -> std::string
        {
            if (!member->IsAlive())
                return member->GetName() + " dead";

            if (member->GetHealthPct() < readyHealth)
                return Acore::StringFormat("{} at {:.0f} % health", member->GetName(), member->GetHealthPct());

            if (member->getPowerType() == POWER_MANA && member->GetPowerPct(POWER_MANA) < readyMana)
                return Acore::StringFormat("{} at {:.0f} % mana", member->GetName(), member->GetPowerPct(POWER_MANA));

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

        if (missing.empty() && askBuffs)
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
        PathGenerator path(bot);
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
    return BossReadyGap(bot, true, IsEncounterBoss(boss));
}

namespace PullPlanning  // [lead31a] was anonymous: shared with the tank's execution
{
    // [drift_caster_corner] scholo-n 20261025-150737-7 pull 12: Adepts and Necromancers cast from range and never
    // came to the tank; the hunter died 44-48 yd off. Casters that keep their range in a fight already on are broken
    // by line of sight: the tank steps round the nearest corner (FindLosPullSpot), the group gathers behind it
    // (LosPull: its spot and hold), the casters walk round into melee. Once per fight, through FightSpot::Due.
    namespace CasterCorner
    {
        constexpr uint32 MIN_CASTERS = 2;      // casters / shooters standing off the tank
        constexpr float OFF_TANK = 12.0f;      // yd from the tank: one that stays out there casts from range
        constexpr uint32 FIGHT_MIN_MS = 4000;  // the fight is this old (the casters had time to come)
        constexpr float MAX_WALK = 25.0f;      // the corner is at most this far from the tank
    }

    // [drift_caster_corner] The casters and shooters of the fight on that stand off the tank and cast at it from
    // there (in its sight, not walking): at least CasterCorner::MIN_CASTERS, no boss among the attackers. True with
    // the tank's corner `spot` and the group's `gather` spot behind it (FindLosPullSpot, at most MAX_WALK away),
    // and the casters in `casters`.
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

            casters.push_back(mob);
        }

        if (casters.size() < CasterCorner::MIN_CASTERS)
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
    };

    std::mutex pullCheckLock;
    std::unordered_map<uint64, PullRecord> pullChecks;  // by tank
    std::unordered_map<uint64, uint32> lastFights;     // [chain_pull] by tank: its group's last fight (Sample)

    struct MeasuredDps
    {
        float dps = 0.0f;
        uint32 ms = 0;
        uint32 instanceId = 0;
    };
    std::unordered_map<uint64, MeasuredDps> measuredDps;  // by tank, under pullCheckLock

    struct MeasuredTaken
    {
        float ratio = 1.0f;  // damage taken against TakenCost of the plan
        uint32 fights = 0;
        uint32 ms = 0;
        uint32 instanceId = 0;
    };
    std::unordered_map<uint64, MeasuredTaken> measuredTaken;  // by tank, under pullCheckLock

    // The fight is over: what the tank took against what the plan expected it to take. Under pullCheckLock.
    void NoteMeasuredTaken(Player* tank, PullRecord const& record, uint32 now)
    {
        uint32 const fightMs = record.startMs ? getMSTimeDiff(record.startMs, now) : 0;
        if (fightMs < MEASURED_DPS_MIN_FIGHT_MS || record.deaths || record.cost < LANDED_CALIBRATION_MIN_COST ||
            record.came.empty() || record.came.size() + 1 < record.mobs || record.came.size() > record.mobs + 1 ||
            !tank->GetMaxHealth())
            return;

        PackEstimate planned;
        planned.cost = record.cost;
        float const expected = TakenCost(planned);
        uint64 const total = DamageTaken::Total(tank->GetGUID());
        if (expected <= 0.0f || total < record.takenAtStart)
            return;

        float const taken = float(total - record.takenAtStart) / float(tank->GetMaxHealth());
        float const ratio = taken / expected;
        MeasuredTaken& entry = measuredTaken[tank->GetGUID().GetRawValue()];
        bool const fresh = entry.ms && entry.instanceId == tank->GetInstanceId() &&
                           getMSTimeDiff(entry.ms, now) < MEASURED_DPS_TTL_MS;
        bool const seeded = !fresh && entry.ms && entry.instanceId != tank->GetInstanceId() &&
                            getMSTimeDiff(entry.ms, now) < LANDED_SEED_TTL_MS;
        if (fresh || seeded)
        {
            float const prior = fresh ? entry.ratio : (entry.ratio + 1.0f) / 2.0f;
            entry.ratio = prior * (1.0f - MEASURED_DPS_WEIGHT) + ratio * MEASURED_DPS_WEIGHT;
        }
        else
            entry.ratio = ratio;

        entry.fights = fresh ? entry.fights + 1 : (seeded ? LANDED_CALIBRATION_MIN_FIGHTS : 1);
        entry.ms = now;
        entry.instanceId = tank->GetInstanceId();
    }

    // The scale of the pack's landed damage on this tank in this instance: 1 until measured, never above 1.
    float LandedCalibration(Player* tank)
    {
        std::lock_guard<std::mutex> guard(pullCheckLock);
        auto const found = measuredTaken.find(tank->GetGUID().GetRawValue());
        if (found == measuredTaken.end() || found->second.instanceId != tank->GetInstanceId() ||
            found->second.fights < LANDED_CALIBRATION_MIN_FIGHTS ||
            getMSTimeDiff(found->second.ms, getMSTime()) >= MEASURED_DPS_TTL_MS)
            return 1.0f;

        return std::clamp(found->second.ratio * LANDED_CALIBRATION_SAFETY, LANDED_CALIBRATION_FLOOR, 1.0f);
    }

    // The fight is over: the health of what it killed over its length (corpses still in the world), kept as the
    // tank's measured group damage. Under pullCheckLock.
    void NoteMeasuredDps(PlayerbotAI* botAI, Player* tank, PullRecord const& record, uint32 now)
    {
        uint32 const fightMs = record.startMs ? getMSTimeDiff(record.startMs, now) : 0;
        if (fightMs < MEASURED_DPS_MIN_FIGHT_MS || record.deaths)
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

        float const dps = killed * float(IN_MILLISECONDS) / float(fightMs);
        MeasuredDps& entry = measuredDps[tank->GetGUID().GetRawValue()];
        bool const fresh = entry.ms && entry.instanceId == tank->GetInstanceId() &&
                           getMSTimeDiff(entry.ms, now) < MEASURED_DPS_TTL_MS;
        entry.dps = fresh ? entry.dps * (1.0f - MEASURED_DPS_WEIGHT) + dps * MEASURED_DPS_WEIGHT : dps;
        entry.ms = now;
        entry.instanceId = tank->GetInstanceId();
    }

    // The tank's measured group damage in this instance (MEASURED_DPS_TRUST of it), 0 when none is known.
    float MeasuredGroupDps(Player* tank)
    {
        std::lock_guard<std::mutex> guard(pullCheckLock);
        auto const found = measuredDps.find(tank->GetGUID().GetRawValue());
        if (found == measuredDps.end() || found->second.instanceId != tank->GetInstanceId() ||
            getMSTimeDiff(found->second.ms, getMSTime()) >= MEASURED_DPS_TTL_MS)
            return 0.0f;

        return found->second.dps * MEASURED_DPS_TRUST;
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
    // before a lone Adept. Every fight of the tank's group (planned or not) is recorded here: what came (weighted by
    // ProvenWeight), the deaths, the lowest tank health, the lowest healer mana at the end and the damage per second.
    // A clean fight proves the group takes that much here: its weighted size times a headroom read from what the
    // fight left (PROVEN_HEADROOM_*: a healer at 80 % mana and a tank that never fell under 50 % may take a little
    // more next time, a healer at 20 % less). A fight with a death caps the proof at PROVEN_DEATH_SCALE of it. The
    // proof is scaled down when the group's damage now is under that fight's (a damage dealer dead or gone). Per tank
    // and instance, under pullCheckLock.
    constexpr uint32 PROVEN_MIN_FIGHT_MS = 5 * IN_MILLISECONDS;
    constexpr float PROVEN_HEADROOM_BASE = 0.55f;
    constexpr float PROVEN_HEADROOM_MANA = 0.5f;     // times the lowest healer's mana share at the end
    constexpr float PROVEN_HEADROOM_HEALTH = 0.25f;  // times the tank's lowest health share in the fight
    constexpr float PROVEN_HEADROOM_MIN = 0.75f;
    constexpr float PROVEN_HEADROOM_MAX = 1.25f;
    constexpr float PROVEN_DEATH_SCALE = 0.8f;
    constexpr float PROVEN_DPS_FLOOR = 0.7f;
    constexpr float PROVEN_MAX_CAP_SCALE = 3.0f;  // the last resort's cap rises to at most this times the configured
    constexpr size_t PROVEN_MAX_TRACKED = 96;  // mobs one fight record keeps
    // [adaptive_cap_b29] Build-29 wipe (slabs-h 5A, pull 10 at 681 s, Rtpalba): "[adaptive cap] 14 mobs (14.9
    // weighted, cost 5.63) within the proven 21.7 weighted / 17.2 mobs: taken" - 9 attackers at +2 s, 15 at +10 s,
    // the healer died to Cabal Acolyte melee at +5 s, then all. The proof came from fights whose mobs arrived one by
    // one (a los pull, the Hellmaw event), counted by their total, and it overrode the cost model. Now:
    // - only an ordinary planned trash pull taken as one pack (plan "engage" or "pull back", no encounter in
    //   progress, no boss) proves anything, and it proves its PEAK: the most mobs (weighted) on the group at once;
    //   a fight whose peak stayed under PROVEN_ONE_PACK_SHARE of what came arrived staggered and proves nothing;
    // - the proof never overrides the cost: a pack within it is taken only at a cost of at most
    //   PROVEN_COST_SCALE x the highest planned cost of a proving fight;
    // - a heroic caster pack (PROVEN_CASTER_SHARE of it keeps its range, at least PROVEN_CASTER_MIN_MOBS) keeps the
    //   planner's own los / split plan whatever the proof (Cabal Shadow Priests / Acolytes / Deathsworn).
    constexpr float PROVEN_ONE_PACK_SHARE = 0.75f;
    constexpr float PROVEN_COST_SCALE = 1.25f;
    constexpr float PROVEN_CASTER_SHARE = 0.5f;
    constexpr uint32 PROVEN_CASTER_MIN_MOBS = 3;

    struct FightTrack
    {
        uint32 startMs = 0;
        uint32 lastMs = 0;
        GuidVector came;
        float weighted = 0.0f;
        float health = 0.0f;  // the maximum health of what came
        uint32 deadAtStart = 0;
        uint32 deaths = 0;  // most members dead at once, over those dead at the start
        float lowestTankHealth = 100.0f;
        bool boss = false;
        bool encounter = false;     // [adaptive_cap_b29] an encounter of the instance ran during it
        float peakWeighted = 0.0f;  // [adaptive_cap_b29] the most (weighted) mobs on the group at once
        uint32 peakMobs = 0;
    };
    std::unordered_map<uint64, FightTrack> fightTracks;  // by tank

    struct ProvenEntry
    {
        uint32 instanceId = 0;
        float weighted = 0.0f;  // the biggest clean fight, its headroom applied
        float mobs = 0.0f;      // the same in plain mobs
        float dps = 0.0f;       // that fight's health per second
        float deathCap = 0.0f;  // 0: no death yet
        uint32 cleanFights = 0;
        float cost = 0.0f;      // [adaptive_cap_b29] the highest planned cost of a proving fight
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
            }

            track.lastMs = now;
            if (InstanceScript* script = tank->GetInstanceScript(); script && script->IsEncounterInProgress())
                track.encounter = true;
            // [adaptive_cap_b29] the mobs on the group now (alive), weighted: the fight's peak
            float nowWeighted = 0.0f;
            uint32 nowMobs = 0;
            for (ObjectGuid const& guid : attackers)
            {
                Unit* mob = botAI->GetUnit(guid);
                if (!mob || !mob->IsAlive())
                    continue;
                nowWeighted += ProvenWeight(mob, tank);
                ++nowMobs;
            }
            if (nowWeighted > track.peakWeighted)
            {
                track.peakWeighted = nowWeighted;
                track.peakMobs = nowMobs;
            }
            for (ObjectGuid const& guid : attackers)
            {
                if (track.came.size() >= PROVEN_MAX_TRACKED ||
                    std::find(track.came.begin(), track.came.end(), guid) != track.came.end())
                    continue;

                Unit* mob = botAI->GetUnit(guid);
                if (!mob)
                    continue;

                track.came.push_back(guid);
                track.weighted += ProvenWeight(mob, tank);
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
        if (fightMs < PROVEN_MIN_FIGHT_MS || track.boss || track.came.empty())
            return;

        // [adaptive_cap_b29] what may prove: the tank's own planned pull of one pack, taken at once
        auto const planned = pullChecks.find(key);
        bool const plannedPull = planned != pullChecks.end() && planned->second.startMs &&
                                 (planned->second.kind.rfind("engage", 0) == 0 ||
                                  planned->second.kind.rfind("pull back", 0) == 0);
        bool const atOnce = track.peakWeighted >= PROVEN_ONE_PACK_SHARE * track.weighted;
        std::string notProving;
        if (track.encounter)
            notProving = "an encounter was in progress";
        else if (!plannedPull)
            notProving = planned == pullChecks.end() || !planned->second.startMs
                             ? "no planned pull"
                             : "plan " + planned->second.kind.substr(0, planned->second.kind.find(" ("));
        else if (!atOnce)
            notProving = Acore::StringFormat("staggered: peak {:.1f} of {:.1f} weighted", track.peakWeighted,
                                             track.weighted);

        float const mana = LowestHealerMana(tank);
        float const manaShare = mana < 0.0f ? 0.5f : mana / 100.0f;
        float const headroom =
            std::clamp(PROVEN_HEADROOM_BASE + PROVEN_HEADROOM_MANA * manaShare +
                           PROVEN_HEADROOM_HEALTH * track.lowestTankHealth / 100.0f,
                       PROVEN_HEADROOM_MIN, PROVEN_HEADROOM_MAX);
        ProvenEntry& entry = provenStrength[key];
        if (entry.instanceId != tank->GetInstanceId())
        {
            entry = ProvenEntry();
            entry.instanceId = tank->GetInstanceId();
        }

        if (!track.deaths && notProving.empty())
        {
            ++entry.cleanFights;
            entry.cost = std::max(entry.cost, planned->second.cost);
            if (track.peakWeighted * headroom > entry.weighted)
            {
                entry.weighted = track.peakWeighted * headroom;
                entry.mobs = float(track.peakMobs) * headroom;
                entry.dps = track.health * float(IN_MILLISECONDS) / float(fightMs);
            }
        }
        // [b38_mgt] only a fight with a death caps it: a clean fight that proves nothing (no planned pull, a los pull,
        // staggered) set it too - mgt-n 20261026-050341-mgt-n-5E-14 (Rlpalam, 0 deaths all run) ended at "death cap
        // 0.8" after a clean 1-mob fight with "no planned pull", crushing the proven 2.2 to 0.8.
        else if (track.deaths)
        {
            float const cap = track.weighted * PROVEN_DEATH_SCALE;
            entry.deathCap = entry.deathCap > 0.0f ? std::min(entry.deathCap, cap) : cap;
        }

        LOG_INFO("playerbots",
                 "[adaptive cap] {}: fight of {} mobs ({:.1f} weighted, peak {} / {:.1f}) in {} s, deaths {}, tank "
                 "low {:.0f} %, healer mana {:.0f} %: headroom {:.2f}{}; proven {:.1f} weighted / {:.1f} mobs, cost "
                 "{:.2f}, over {} clean fights{}",
                 tank->GetName(), track.came.size(), track.weighted, track.peakMobs, track.peakWeighted,
                 fightMs / IN_MILLISECONDS, track.deaths, track.lowestTankHealth, mana, headroom,
                 notProving.empty() ? std::string() : " (proves nothing: " + notProving + ")", entry.weighted,
                 entry.mobs, entry.cost, entry.cleanFights,
                 entry.deathCap > 0.0f ? Acore::StringFormat(", death cap {:.1f}", entry.deathCap) : std::string());
    }

    struct ProvenCap
    {
        float weighted = 0.0f;  // 0: nothing proven
        float mobs = 0.0f;
        float cost = 0.0f;      // [adaptive_cap_b29] the highest planned cost of a proving fight
        bool heroic = false;
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
        if (entry.weighted <= 0.0f || entry.cost <= 0.0f)
            return cap;

        cap.cost = entry.cost;
        cap.heroic = tank->GetMap() && tank->GetMap()->IsHeroic();

        float scale = 1.0f;
        float const current = MeasuredGroupDps(tank) / MEASURED_DPS_TRUST;
        if (current > 0.0f && entry.dps > 0.0f)
            scale = std::clamp(current / entry.dps, PROVEN_DPS_FLOOR, 1.0f);

        cap.weighted = entry.weighted * scale;
        cap.mobs = entry.mobs * scale;
        if (entry.deathCap > 0.0f && cap.weighted > entry.deathCap)
        {
            cap.mobs *= entry.deathCap / cap.weighted;
            cap.weighted = entry.deathCap;
        }

        return cap;
    }

    // A pack no bigger than what the group proved it takes in this run, at no more than PROVEN_COST_SCALE x the
    // proving fights' planned cost ([adaptive_cap_b29]: the proof lifts the size cap, never the cost model). Never a
    // boss, never a heroic caster pack (its los / split plan stays).
    bool ProvenFits(PackEstimate const& estimate, ProvenCap const& proven)
    {
        if (estimate.boss || proven.weighted <= 0.0f || proven.cost <= 0.0f)
            return false;
        if (proven.heroic && estimate.mobs >= PROVEN_CASTER_MIN_MOBS &&
            float(estimate.rangedMobs) >= PROVEN_CASTER_SHARE * float(estimate.mobs))
            return false;
        return estimate.provenMobs <= proven.weighted + 0.01f && estimate.capMobs <= proven.mobs + 0.5f &&
               estimate.cost <= proven.cost * PROVEN_COST_SCALE;
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
            return;
        }

        if (!record.startMs && getMSTimeDiff(record.plannedMs, now) < PULL_CHECK_TIMEOUT_MS)
            return;

        LogPullCheck(bot, record, now);
        NoteMeasuredDps(botAI, bot, record, now);
        NoteMeasuredTaken(bot, record, now);
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

    bool Proven(Player* tank, float& weighted, float& mobs)
    {
        ProvenCap const cap = ProvenFor(tank);
        weighted = cap.weighted;
        mobs = cap.mobs;
        return cap.weighted > 0.0f;
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
            if (!held && (RestState::IsDrinking(bot) || RestState::IsEating(bot) || RestState::WantsDrink(bot) ||
                          RestState::WantsFood(bot)))
            {
                std::pair<ObjectGuid, uint32>& hold = selfRestHolds[bot->GetGUID()];
                if (hold.first != target->GetGUID() || !hold.second)
                    hold = {target->GetGUID(), now ? now : 1};
                held = getMSTimeDiff(hold.second, now) < SELF_REST_HOLD_MS;
                if (held)
                    why = Acore::StringFormat("the tank rests (health {:.0f} %, mana {:.0f} %)", bot->GetHealthPct(),
                                              bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 100.0f);
            }
            else if (!held)
                selfRestHolds.erase(bot->GetGUID());

            if (!held && IsBossMob(target))
            {
                // the missing buff the tank has waited for long enough (WaitForBuffs' clock) holds no boss either
                bool const askBuffs = !_buffWaitMs || getMSTimeDiff(_buffWaitMs, now) < BOSS_BUFF_WAIT_MS;
                why = BossReadyGap(bot, askBuffs, IsEncounterBoss(target));
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
        PathGenerator path(bot);
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

    if (bot->getClass() == CLASS_PALADIN && bot->GetPowerPct(POWER_MANA) < CAREFUL_TANK_MANA)
    {
        _planNotReadyWhy = Acore::StringFormat("paladin tank at {:.0f} % mana", bot->GetPowerPct(POWER_MANA));
        return false;
    }

    // Nor with a healer still drinking up from the last fight (wave 10 Stratholme: the next pack 24 s after a
    // 133 s chain, the healer healed nobody and the group died in 21 s).
    float const minHealerMana = sPlayerbotAIConfig.CarefulPullMinHealerMana;
    float const healerMana = LowestHealerMana(bot);
    if (minHealerMana > 0.0f && healerMana >= 0.0f && healerMana < minHealerMana)
    {
        _planNotReadyWhy =
            Acore::StringFormat("healer mana {:.0f} %, needs {:.0f} %", healerMana, minHealerMana);
        return false;
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
    float const groupDps =
        std::max({PlanGroupDps(), MeasuredGroupDps(bot), CAREFUL_MIN_GROUP_DPS});  // [lead7]
    EstimateMode estimateMode;
    estimateMode.measured = measured;
    estimateMode.rescaledSpells = measured && rescaled;
    estimateMode.landedScale = LandedCalibration(bot);
    // [b44_packs] the group's AoE on a stack (WEAK_PACK_AOE_TARGET_SHARE): a tank with AoE threat and at least one
    // AoE damage dealer here, else no AoE estimate at all
    uint32 weakAoeDealers = 0;
    bool const weakAoeTank = HasAoeThreat(bot);
    if (Group* group = bot->GetGroup(); group && weakAoeTank)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && IsPresent(bot, member) && !PlayerbotAI::IsTank(member) &&
                !PlayerbotAI::IsHeal(member) && HasAoeDamage(member))
                ++weakAoeDealers;
        }
    }
    if (weakAoeTank && weakAoeDealers)
        estimateMode.aoeTargetDps =
            groupDps * WEAK_PACK_AOE_TARGET_SHARE * float(std::min<uint32>(weakAoeDealers + 1, 4)) / 3.0f;
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
    // [b44_packs] A weak pack (WEAK_PACK_*): it dies to the group's AoE within WEAK_PACK_TTK_SEC, the healers keep
    // up with what the whole stack lands meanwhile (the measured peak term with every mob alive: CAREFUL_MAX_DTPS
    // of the tank's health per second per healer), the tank keeps CAREFUL_MAX_DROP of its health in 9 pulls of 10
    // (DropP90 of that cost) and the healers have the mana (HealerPool). The planner's caps on spell damage stay in
    // the estimate (MEASURED_CASTER_SPELL_SCALE / the rescale's spell caps): the AoE cost carries the same spell part.
    // Not with crowd control planned on the pack (the AoE breaks it) and never a boss.
    auto const weakAoeFits = [&](PackEstimate const& estimate) -> bool
    {
        if (estimate.boss || estimate.aoeSec <= 0.0f || estimate.mobs < WEAK_PACK_MIN_MOBS ||
            estimate.aoeSec > 2.0f * WEAK_PACK_TTK_SEC)
            return false;

        float const tankHealth = PlanTankHealth(bot);
        float const healing = tankHealth * CAREFUL_MAX_DTPS * HealerShare(support) * strength.healer;
        PackEstimate aoe = estimate;
        aoe.cost = estimate.aoeCost;
        aoe.spellCost = estimate.aoeSpellCost;
        float const peak =
            healing > 0.0f ? CAREFUL_DTPS_SHARE * estimate.landedDps * WEAK_PACK_ALIVE_SHARE / healing : 99.0f;
        float const drop = DropP90(aoe);
        float const pool = HealerPool(support);
        float const mana = pool > 0.0f ? TakenCost(aoe) / pool : 99.0f;
        char const* why = nullptr;
        if (estimate.aoeSec > WEAK_PACK_TTK_SEC)
            why = "too slow";
        else if (float(estimate.rangedMobs) > WEAK_PACK_MAX_RANGED_SHARE * float(estimate.mobs))
            why = "casters stand out of the stack";
        else if (peak > 1.0f)
            why = "the healers do not keep up";
        else if (drop > CAREFUL_MAX_DROP)
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
            LOG_INFO("playerbots", "[b44_packs] {}: {} pack {} mobs ttk {:.1f}s: {} (aoe {:.0f}/s per mob over {} "
                     "dealers + tank, landed {:.0f}/s, peak {:.2f}, drop {:.2f}, mana {:.2f}, cost {:.2f} vs one by "
                     "one {:.2f}, {} casters)", bot->GetName(), why ? "not weak:" : "weak", estimate.mobs,
                     estimate.aoeSec, why ? why : "aoe", estimateMode.aoeTargetDps, weakAoeDealers, estimate.landedDps,
                     peak, drop, mana, estimate.aoeCost, estimate.cost, estimate.rangedMobs);
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
            LOG_INFO("playerbots", "[adaptive cap] {}: {} mobs ({:.1f} weighted, cost {:.2f}) within the proven {:.1f} "
                     "weighted / {:.1f} mobs: taken", bot->GetName(), estimate.mobs, estimate.provenMobs,
                     estimate.cost, proven.weighted, proven.mobs);
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
    // `fallbackCost`: a fallback plan is being looked for, held under CAREFUL_FALLBACK_MAX_COST whatever the mode.
    bool fallbackCost = false;
    auto const affordable = [measured, &fallbackCost](PackEstimate const& estimate, float limit)
    {
        if (fallbackCost && !estimate.boss &&
            estimate.cost >= SmallPackCostCap(estimate.mobs, CAREFUL_FALLBACK_MAX_COST))
            return false;

        return limit <= 1.0f || measured ||
               estimate.cost <= SmallPackCostCap(estimate.mobs, CAREFUL_LAST_RESORT_MAX_COST);
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
    auto const tryPlan = [&](Unit* candidate, bool allowBossPack, float limit, bool comfort, bool losOnly) -> bool
    {
        // A mob the group's crowd control holds or has claimed is not opened on while the rest of its pack lives.
        if (candidate != target &&
            (IsCrowdControlled(candidate) || PrePullCc::IsClaimed(bot, candidate->GetGUID())))
            return false;

        // Nor one whose encounter's predecessor has not started (EncounterOrder: it evades at once when engaged).
        if (EncounterOrder::IsLocked(candidate))
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

        if (PlanLosPull(candidate, possible, acceptLos, measured))
        {
            _planReason = Acore::StringFormat("{} casters / shooters pulled round the corner; packs: los pull {} at "
                                              "the tank's spot round the corner", CountRanged(losSeen),
                                              losSeen.size());
            _planCached = candidate->GetGUID();
            _planPullBack = false;
            _planLos = true;
            _planKillFor.Clear();
            chosen = losEstimate;
            chosenPack = losSeen;
            chosenOverlaps.clear();
            return true;
        }

        if (losOnly)
            return false;

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
                        crowd = mob;
                        break;
                    }
                if (crowd)
                    break;
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
                return takeEngage(pullBackWhy + ", the tank drags the fight off it");

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
                PathGenerator path(bot);
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
                if (estimate.boss || !withinCap(estimate) || terms.Max() > 1.0f ||
                    estimate.cost >= CAREFUL_FALLBACK_MAX_COST)
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

        if (found)
            _planReason = Acore::StringFormat("the walk to {} wakes {}, taken first; ", target->GetName(),
                                              bossWalkWakes->GetName()) +
                          _planReason;
        else
            _planNotReadyWhy = Acore::StringFormat("the walk to {} wakes {} ({:.0f} yd), and no plan takes it",
                                                   target->GetName(), bossWalkWakes->GetName(),
                                                   bot->GetExactDist(bossWalkWakes));
    }

    if (!found && boss && !bossWalkWakes)
    {
        float danger = 0.0f;
        if (IsPackManageable(botAI, bot, target, strength, danger))
        {
            _planCached = target->GetGUID();
            _planPullBack = false;
            _planLos = false;
            chosen = PackEstimate();
            chosenPack.clear();
            chosenOverlaps.clear();
            chosen.danger = danger;
            chosen.boss = true;
            found = true;
            _planReason = "boss pulled where it stands";
        }
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

            if (IsBossMob(candidate) || IsCrowdControlled(candidate) ||
                PrePullCc::IsClaimed(bot, candidate->GetGUID()) ||
                (candidate->GetGUID() == _lastPullBack &&
                 getMSTimeDiff(_lastPullBackMs, now) < CAREFUL_PULL_RETRY_MS) ||
                CarefulPlanVerdict::EngageOnly(bot->GetGUID(), candidate->GetGUID()))  // [b37_sv]
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
    // [adaptive_cap] at least what the group proved in this run, at most PROVEN_MAX_CAP_SCALE x the cap
    uint32 const baseLastResortMobs = maxMobs ? maxMobs : CAREFUL_LAST_RESORT_DEFAULT_CAP;
    uint32 const lastResortMobs = std::max(
        baseLastResortMobs, std::min(uint32(proven.mobs), uint32(float(baseLastResortMobs) * PROVEN_MAX_CAP_SCALE)));
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
        bool const freshStall = _stallFor.IsEmpty() || getMSTimeDiff(_stallMs, now) >= 6 * CAREFUL_STALL_MS;
        if (freshStall)
        {
            _stallFor = target->GetGUID();
            _stallMs = now;
        }
        // [careful_ready_last_resort] ready and within the cap (or no plan built at all): the slow path at once
        bool const sittingBelowLine = IsAnyoneSittingBelowLine(bot);
        bool const noPlanBuilt = best.mob.IsEmpty() || best.estimate.cost <= 0.0f;
        bool const readyNow = CAREFUL_READY_LAST_RESORT && !sittingBelowLine &&
                              (noPlanBuilt || best.estimate.mobs <= lastResortMobs);
        if (uint32 const stalled = getMSTimeDiff(_stallMs, now);
            (!freshStall || readyNow) &&
            (stalled >= CAREFUL_STALL_MS || readyNow ||
             (stalled >= CAREFUL_READY_ONE_MS && getMSTimeDiff(_quickOneMs, now) >= CAREFUL_READY_ONE_MS &&
              !sittingBelowLine)))
        {
            // The quick lone mob (CAREFUL_READY_ONE_MS): the group is ready, only the single is tried, once per
            // CAREFUL_READY_ONE_MS, and a failed try keeps the stall running to the CAREFUL_STALL_MS path.
            bool const quickOne = stalled < CAREFUL_STALL_MS && !readyNow;
            if (readyNow && stalled < CAREFUL_STALL_MS)
            {
                static thread_local uint32 readyLogMs = 0;  // per map thread
                if (!readyLogMs || getMSTimeDiff(readyLogMs, now) > 10 * IN_MILLISECONDS)
                {
                    readyLogMs = now ? now : 1;
                    LOG_INFO("playerbots", "[careful_ready_last_resort] {}: {} refused after {} s, group ready, {}: "
                             "last resort now (cap {})", bot->GetName(), target->GetName(), stalled / IN_MILLISECONDS,
                             noPlanBuilt ? std::string("no plan built (cost 0)")
                                         : Acore::StringFormat("smallest plan {} mobs at cost {:.2f}",
                                                               best.estimate.mobs, best.estimate.cost),
                             lastResortMobs);
                }
            }
            if (quickOne)
                _quickOneMs = now;

            fallbackCost = true;
            bool const losFallback = !quickOne && measured && planAround(CAREFUL_FALLBACK_OVERLOAD, false, true);
            fallbackCost = false;
            if (losFallback)
                fallback = lastResort = found = true;
            else if (!quickOne && !measured && !noPlanBuilt && best.estimate.mobs <= lastResortMobs &&
                     best.estimate.cost <= SmallPackCostCap(best.estimate.mobs, CAREFUL_LAST_RESORT_MAX_COST) &&
                     best.estimate.cost < SmallPackCostCap(best.estimate.mobs, CAREFUL_FALLBACK_MAX_COST))
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
                        if (IsBossMob(candidate) || IsCrowdControlled(candidate) ||
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
                // The wait ends (user rule: the tank always takes one mob): with no ranged pull it can use for
                // CAREFUL_LONE_MOB_RANGED_WAIT_MS (no ranged weapon, a Taunt outside Defensive Stance), the tank walks
                // in on the lone mob. Wave 37: the wait had no end, 87 refusals of one Ragefire Trogg.
                bool walkIn = false;
                if (one && !PullBackSpot(one, castSpot, true))
                {
                    if (!_loneWaitMs || getMSTimeDiff(_loneSeenMs, now) >= CAREFUL_LONE_MOB_WAIT_GAP_MS)
                        _loneWaitMs = now;

                    _loneSeenMs = now;
                    if (getMSTimeDiff(_loneWaitMs, now) <
                        (quickOne ? CAREFUL_READY_ONE_MS : CAREFUL_LONE_MOB_RANGED_WAIT_MS))
                    {
                        LogPullHold(bot, "lone-mob pull waits for a single-target ranged pull", one);
                        one = nullptr;
                    }
                    else
                    {
                        LogPullHold(bot, "lone-mob pull found no single-target ranged pull, walks in", one);
                        walkIn = true;
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
                    else if (quick.cost >= SmallPackCostCap(quick.mobs, CAREFUL_FALLBACK_MAX_COST))
                        problem = Acore::StringFormat("costs {:.2f} tank hp (cap {:.2f})", quick.cost,
                                                      SmallPackCostCap(quick.mobs, CAREFUL_FALLBACK_MAX_COST));
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
                // Over CAREFUL_LAST_RESORT_HARD_CAP the hard minimum is no pull: round a corner (CAREFUL_FALLBACK_
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

                uint32 blockedMs = _blockedSinceMs ? getMSTimeDiff(_blockedSinceMs, blockNow) : 0;
                // the pack's own stand, over the harness's retargets and skips (_packBlocked)
                {
                    std::vector<ObjectGuid> packGuids = {target->GetGUID()};
                    for (Unit* mob : adds)
                        packGuids.push_back(mob->GetGUID());
                    if (one)
                        packGuids.push_back(one->GetGUID());
                    blockedMs = std::max(blockedMs, PackBlockedMs(packGuids, blockNow));
                }
                float const hardCap = CAREFUL_LAST_RESORT_HARD_CAP *
                                      (blockedMs >= 2 * CAREFUL_BLOCK_ESCALATE_MS ? 3.0f
                                       : blockedMs >= CAREFUL_BLOCK_ESCALATE_MS   ? 2.0f
                                                                                  : 1.0f);
                _lastResortCap = hardCap;
                // the hard cap trusts the measured calibration only down to LANDED_HARD_CAP_FLOOR
                float const landedScale = estimateMode.landedScale;
                float const capScale = std::max(landedScale, LANDED_HARD_CAP_FLOOR);
                // [b34_scholo] ...for the melee part only: the calibration is the tank's taken damage over its cheap
                // fights, and a caster pack's spells (Shadow Bolt: no armor, no avoidance) are not what it measured.
                // scholo-h 20261026-014521-scholo-h-5H-3: landed scale 0.40 from three Risen Guard pulls (the tank
                // took 0-156 damage), so "Spectral Researcher ... 9 mobs, cost 7.00" (4.67 scaled, 11.7 unscaled) was
                // taken; in the hall fight the tank took 30k of Shadow Bolts in 15 s (7.7 tank healths in all).
                float const spellShare =
                    oneEstimate.cost > 0.0f ? std::clamp(oneEstimate.spellCost / oneEstimate.cost, 0.0f, 1.0f) : 0.0f;
                float const capShare = capScale * (1.0f - spellShare) + spellShare;
                float const capCost = landedScale > 0.0f ? oneEstimate.cost / landedScale * capShare : oneEstimate.cost;
                if (one && !oneEstimate.boss && capCost <= hardCap && capCost > CAREFUL_LAST_RESORT_HARD_CAP)
                    LOG_INFO("playerbots", "[pull escalate] {}: {} blocked {} s, cost {:.2f} within the escalated cap "
                             "{:.2f}: taken as a hard pack", bot->GetName(), one->GetName(), blockedMs / 1000,
                             capCost, hardCap);
                // the stand's guarantee (CAREFUL_BLOCK_GUARANTEE_MS): a pack within the size cap is no stall for good
                size_t const overCap = overCapMobs;
                // The size against the weighted count (CAREFUL_SWARM_CAP_WEIGHT): weak swarm adds count less.
                float singleWeighted = 0.0f;
                for (Unit* mob : single)
                    singleWeighted += CapWeight(mob, bot);
                bool const withinCap = singleWeighted <= float(lastResortMobs) + 0.01f;
                if (withinCap && single.size() > lastResortMobs)
                    LOG_INFO("playerbots", "[pull swarm] {}: swarm_cap_weight: guarantee pack of {} counts {:.1f} "
                             "against the cap {}", bot->GetName(), single.size(), singleWeighted, lastResortMobs);
                uint32 const guaranteeMs = withinCap ? CAREFUL_BLOCK_GUARANTEE_MS : CAREFUL_BLOCK_GUARANTEE_OVER_MS;
                // [b30_overpull] an over-cap guarantee pack is taken only while its cost stays within
                // GUARANTEE_OVER_CAP_MAX_COST: strat-h 20261025-184229-strat-h-5C-5, "Ravaged Cadaver blocked 47 s,
                // brings 16 mobs (cap 9), cost 16.21 over the hard cap 6.00: taken" (landed scale 1.00) - 20 came,
                // the wipe at 227 s. The within-cap guarantee keeps taking any cost (the levelling overstatement).
                // [b32_pull] The within-cap guarantee takes any cost because the levelling model overstates 3-7x. A
                // tank whose measured calibration says the model does not overstate (landed scale ~1) gets the
                // escalated hard cap's x2 as its ceiling: dtk-h 20261025-223307-dtk-h-5G-74, "Flesheating Ghoul
                // blocked 15 s, brings 7 mobs (cap 9), cost 8.86 over the hard cap 3.00: taken" (landed scale 1.00),
                // 7 came as estimated and all five died. The hard cap's own escalation (x3 after 60 s) still applies.
                constexpr float MEASURED_COST_TRUST = 0.9f;
                // [b34_scholo] a heroic / max-level pack whose cost is mostly spells has no measured calibration
                // either (above): the same ceiling, waiting for the hard cap's escalation, not a dead end
                constexpr float SPELL_PACK_SHARE = 0.5f;
                bool const spellPack = estimateMode.rescaledSpells && spellShare >= SPELL_PACK_SHARE;
                bool const measuredCeiling = withinCap && (landedScale >= MEASURED_COST_TRUST || spellPack) &&
                                             capCost > 2.0f * CAREFUL_LAST_RESORT_HARD_CAP && capCost > hardCap;
                bool const overCostCeiling =
                    (!withinCap && capCost > GUARANTEE_OVER_CAP_MAX_COST) || measuredCeiling;
                // [b32_pull] The hard minimum is a lone mob only while what it brings fits the pack-size cap: a cheap
                // "one" that brings more than the cap went at once on cost alone (scholo-n 20261025-221216-scholo-n-
                // 5C-4 pull 8, dtk-h 20261025-223307-dtk-h-5G-74 pull 7). Over the cap it is refused like a costly
                // one: round a corner, a mover's wait, or the over-cap guarantee after its longer stand.
                bool const overSize = one && !oneEstimate.boss && !withinCap;
                bool const refusedSingle = capCost > hardCap || overSize;
                // [b34_scholo] The stand's guarantee walks in on no pack, as the pack guarantee below
                // ([guarantee_body_pull_route_only]): with no single-target ranged pull the "one" is walked in on, and
                // a walk into a pack that brings more than that one mob is the harness's route body pull to decide.
                // scholo-h 5H-3: "one mob walked in (no ranged pull) Spectral Researcher at 41.7 yd, 9 mobs", on the
                // Chamber of Summoning floor (one linked area of 15-24 mobs, DUNGEON_TACTICS 2).
                bool const walkInPack = walkIn && single.size() > 1;
                bool const guarantee = one && !oneEstimate.boss && refusedSingle && blockedMs >= guaranteeMs &&
                                       singleWeighted <= float(overCap) + 0.01f && !overCostCeiling && !walkInPack &&
                                       std::none_of(single.begin(), single.end(),
                                                    [](Unit* mob) { return IsBossMob(mob); });
                if (guarantee)
                {
                    _planGuaranteed = true;
                    LOG_INFO("playerbots", "[pull guarantee] {}: {} blocked {} s, brings {} mobs (cap {}), cost {:.2f} "
                             "over the hard cap {:.2f}: taken, the stand ends", bot->GetName(), one->GetName(),
                             blockedMs / 1000, single.size(), lastResortMobs, capCost, hardCap);
                }
                else if (one && !oneEstimate.boss && refusedSingle)  // [b32_pull] was capCost > hardCap
                {
                    // the refusal for the harness: hopeless only past the over-cap scale (a dead end, route round)
                    // [b32_pull] the measured ceiling is a wait for the hard cap's escalation, not a dead end
                    _planVerdictHopeless =
                        singleWeighted > float(overCap) + 0.01f || (overCostCeiling && !measuredCeiling);
                    _planVerdictGuaranteeWaitMs = blockedMs < guaranteeMs ? guaranteeMs - blockedMs : 0;
                    _planVerdictWhy = overCostCeiling && !measuredCeiling && singleWeighted <= float(overCap) + 0.01f
                                          ? Acore::StringFormat("no safe pull: the smallest pull ({} at range) "
                                                                "brings {} mobs over the cap {} at cost {:.2f}, over "
                                                                "the guarantee's ceiling {:.2f}",
                                                                one->GetName(), single.size(), lastResortMobs, capCost,
                                                                GUARANTEE_OVER_CAP_MAX_COST)
                                      : _planVerdictHopeless
                                          ? Acore::StringFormat("no safe pull: the smallest pull ({} at range) "
                                                                "brings {} mobs, over {} (the cap {} x{:.1f})",
                                                                one->GetName(), single.size(), overCap,
                                                                baseLastResortMobs, GUARANTEE_OVER_CAP_SCALE)
                                          : Acore::StringFormat("refused for now{}: {} brings {} mobs at cost {:.2f}, "
                                                                "taken after a {} s stand (blocked {} s)",
                                                                withinCap ? "" : " (unsafe, over the cap)",
                                                                one->GetName(), single.size(), capCost,
                                                                guaranteeMs / IN_MILLISECONDS, blockedMs / 1000);
                    // [b34_scholo] said, and no guarantee wait the harness holds for. Worded as the over-cap class
                    // (RefusalIsOverCap: "does not fit"), so the harness does not walk the whole group into it either.
                    if (walkInPack && !_planVerdictHopeless)
                    {
                        _planVerdictGuaranteeWaitMs = 0;
                        _planVerdictWhy = Acore::StringFormat(
                            "refused for now: no ranged pull of {}; walked in, its pack does not fit the hard minimum "
                            "({} mobs at cost {:.2f}, hard cap {:.2f}), and the stand's guarantee walks in on no pack",
                            one->GetName(), single.size(), capCost, hardCap);
                    }
                }
                // [b47_plan] within the hard cap but well over a tank health: a mover it brings walks off first
                if (one && !oneEstimate.boss && !refusedSingle && !quickOne &&
                    capCost > CAREFUL_LAST_RESORT_SOFT_COST && blockedMs < 2 * CAREFUL_BLOCK_ESCALATE_MS)
                {
                    Unit* mover = nullptr;
                    for (Unit* mob : single)
                    {
                        Creature* creature = mob ? mob->ToCreature() : nullptr;
                        if (mob != one && creature && !IsBossMob(creature) &&
                            MotionOf(creature) != MobMotion::Static)
                        {
                            mover = mob;
                            break;
                        }
                    }

                    // its own bound too: the stand's clock restarts when the nearest target flips between packs
                    constexpr uint32 SOFT_WAIT_MAX_MS = 45 * IN_MILLISECONDS;
                    constexpr uint32 SOFT_WAIT_GAP_MS = 10 * IN_MILLISECONDS;
                    thread_local std::unordered_map<uint64, std::pair<uint32, uint32>> softWaits;  // since, seen
                    std::pair<uint32, uint32>& softWait = softWaits[bot->GetGUID().GetRawValue()];
                    if (mover)
                    {
                        if (!softWait.first || getMSTimeDiff(softWait.second, now) > SOFT_WAIT_GAP_MS)
                            softWait.first = now ? now : 1;
                        softWait.second = now ? now : 1;
                        if (getMSTimeDiff(softWait.first, now) >= SOFT_WAIT_MAX_MS)
                            mover = nullptr;  // waited long enough: taken as before
                    }

                    if (mover)
                    {
                        std::string const why = Acore::StringFormat(
                            "[b47_plan] last_resort_soft: the lone mob {} brings {} mobs at cost {:.2f} (over {:.2f}, "
                            "blocked {} s); waits for {} ({}) to walk off", one->GetName(), single.size(), capCost,
                            CAREFUL_LAST_RESORT_SOFT_COST, blockedMs / 1000, mover->GetName(),
                            MobMotionName(MotionOf(mover->ToCreature())));
                        LogPullHold(bot, why.c_str(), one);
                        _planNotReadyWhy = "spacing wait: " + why;
                        one = nullptr;
                    }
                }

                if (one && !oneEstimate.boss && refusedSingle && !guarantee)  // [b32_pull] was capCost > hardCap
                {
                    std::string const capWhy =
                        capCost > hardCap
                            ? Acore::StringFormat("the lone mob {} brings {} mobs, cost {:.2f} tank hp, over the hard "
                                                  "cap {:.2f} (blocked {} s)",
                                                  one->GetName(), single.size(), capCost, hardCap, blockedMs / 1000)
                            : Acore::StringFormat("the lone mob {} brings {} mobs ({:.1f} weighted), over the size "
                                                  "cap {} (blocked {} s)",
                                                  one->GetName(), single.size(), singleWeighted, lastResortMobs,
                                                  blockedMs / 1000);
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
                        // The wait for a mover is bounded by the stand's clock too: a patrol that paces beside its
                        // pack (sh-n Legionnaire by the Darkcasters) never walks off.
                        Unit* mover = nullptr;
                        bool const moverWait = blockedMs < 2 * CAREFUL_BLOCK_ESCALATE_MS;
                        for (Unit* mob : single)
                        {
                            Creature* creature = mob->ToCreature();
                            if (moverWait && creature && !IsBossMob(creature) &&
                                MotionOf(creature) != MobMotion::Static)
                            {
                                mover = mob;
                                break;
                            }
                        }

                        if (mover)
                        {
                            std::string const why = capWhy + Acore::StringFormat(
                                "; waits for {} ({}) to walk off", mover->GetName(),
                                MobMotionName(MotionOf(mover->ToCreature())));
                            LogPullHold(bot, why.c_str(), one);
                            _planNotReadyWhy = "spacing wait: " + why;
                        }
                        else
                            LogPullBlocked(bot, capWhy + "; no pull round a corner, nothing walks off: left to the "
                                                         "group's pre-pull crowd control", one);
                    }

                    one = nullptr;
                }

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

    // The stand's guarantee for any refusal (wave 12: 3-4 mob packs refused with no plan at all, "smallest plan 4 mobs
    // at cost 0.00": every way in woke a mob, no lone mob, a crowd control claim; the hard-minimum guarantee never
    // ran for them). Topped up and blocked CAREFUL_BLOCK_GUARANTEE_MS on this pack, no boss in it, within the
    // last resort's mob cap with what the way in wakes (each woken mob with its pack): the pack is pulled as it
    // stands, at range when the tank has a pull, else walked in. Over the cap, or a boss woken: hopeless, said why.
    size_t const guaranteeOverCap = overCapMobs;
    // the smallest real pull of the pack: its best plan's count when one was looked at, else the pack at its spot
    // [b33_fix1] reverted [b33_brs]'s weighted count (capMobs) here: the guarantee is the stand's hard minimum, it
    // skips the cost gates for a pack within the cap, so the cap holds real mobs. scholo-h 20261026-012428-5G-3
    // (241-369 s): the weighted count took "31 mobs, cost 48.93 tank hp" as within 18 (and within the 9 that skips
    // GUARANTEE_OVER_CAP_MAX_COST), 42 enemies came, wipe.
    size_t const smallestPull =
        best.mob.IsEmpty() ? adds.size() + 1 : std::min<size_t>(adds.size() + 1, best.estimate.mobs);
    if (!found && toppedUp && !boss && !targetPackBoss && smallestPull <= guaranteeOverCap)
    {
        std::vector<ObjectGuid> packGuids = {target->GetGUID()};
        for (Unit* mob : adds)
            packGuids.push_back(mob->GetGUID());

        // an encounter tactic's request (GuaranteeRequest) takes the pack at once, walked in when asked
        ObjectGuid requestedMob;
        bool requestedWalkIn = false;
        bool requested = GuaranteeRequest::Get(bot->GetGUID(), requestedMob, requestedWalkIn) &&
                         std::find(packGuids.begin(), packGuids.end(), requestedMob) != packGuids.end();
        // [lead3] LEADER_NO_BODY_FALLBACK: the leader's whole-pack request (a pack it must clear: the old harness body
        // pull's domain) comes in the look's inputs, not the GuaranteeRequest registry, and is offered whole at once
        // (allowWholePack: the over-cap scale, no stand clock); the walk-in is priced by the rounds below.
        bool leaderWhole = false;
        if (PullPlanner::LeaderInputs const* inputs = _snap ? _snap->Leader() : nullptr;
            inputs && Leader::Leads(Leader::LEADER_NO_BODY_FALLBACK) && !inputs->wholePack.IsEmpty() &&
            std::find(packGuids.begin(), packGuids.end(), inputs->wholePack) != packGuids.end())
        {
            leaderWhole = true;
            requested = true;
            requestedMob = inputs->wholePack;
            requestedWalkIn = requestedWalkIn || inputs->walkIn;
        }
        uint32 const stood = PackBlockedMs(packGuids, now);
        uint32 const packBlocked =
            leaderWhole ? std::max(stood, CAREFUL_BLOCK_GUARANTEE_OVER_MS)
                        : (requested ? std::max(stood, CAREFUL_BLOCK_GUARANTEE_MS) : stood);
        // within the cap after the stand, up to the over-cap scale after the longer one
        size_t const allowed = packBlocked >= CAREFUL_BLOCK_GUARANTEE_OVER_MS ? guaranteeOverCap : lastResortMobs;
        if (packBlocked >= CAREFUL_BLOCK_GUARANTEE_MS && smallestPull <= allowed && !IsCrowdControlled(target) &&
            !EncounterOrder::IsLocked(target))
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
            // [b33_fix1] reverted [b33_brs]'s base-pack chain weighting (ChainStepWeight on the pack's own hops): every
            // mob of the pack and of what the way in wakes counts in full again, as in build 32; only the helpers the
            // assistance calls bring after it count CHAIN_STEP_CAP_WEIGHT ([guarantee_chain_count] below)
            size_t engagedByThemselves = 0;  // 0: not fixed yet (the walk rounds), every mob engages by itself
            auto const guaranteedWeight = [&guaranteed, &engagedByThemselves]()
            {
                size_t const engaged =
                    engagedByThemselves ? std::min(guaranteed.size(), engagedByThemselves) : guaranteed.size();
                return float(engaged) + CHAIN_STEP_CAP_WEIGHT * float(guaranteed.size() - engaged);
            };
            Unit* wokenBoss = nullptr;
            Creature* firstWoken = nullptr;
            for (uint32 round = 0; round < GUARANTEE_WALK_ROUNDS && !wokenBoss && guaranteed.size() <= allowed; ++round)
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

            // [guarantee_chain_count] The pack as the core's call for assistance grows it (Creature::CallAssistance:
            // an idle mob of a helping faction within CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS of a fighting one,
            // in its sight), repeated over every mob it brings: the formation count alone took "a pack within 7"
            // in the Scholomance Necromancer room (scholo-n w23, 20261025-112011-scholo-n-5A-7) and 25 came.
            // [guarantee_chain_fight_position] plan29 follow-up 5 (scholo-h 20261025-163739-5, 141 s idle: "the way to
            // Risen Guard wakes Scholomance Necrolyte ... with it 19 mobs, over 14", the chain run helper to helper
            // from their spawns): only a mob that engages by itself (the pack, what the way in wakes) calls from
            // where it stands; a helper engages with its call off (AssistDelayEvent) and calls next from where it
            // fights - by the tank, counted by the pack there - so the chain is one step deep here, and each helper
            // counts CHAIN_STEP_CAP_WEIGHT against the size (as in CollectPack, [chain_step_weight]).
            engagedByThemselves = guaranteed.size();  // [b33_brs] the weight lambda moved above the walk rounds
            float const assistRadius = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
            if (!wokenBoss && assistRadius > 0.0f)
            {
                for (size_t index = 0; index < engagedByThemselves && guaranteedWeight() <= float(allowed) &&
                                       !wokenBoss;
                     ++index)
                {
                    Creature* caller = guaranteed[index]->ToCreature();
                    if (!caller || !CallsOnEngage(caller))  // [b33_brs] no engage call: its periodic one is the pack's
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

            // [guarantee_body_pull_route_only] The guarantee walks in on a pack only when an encounter tactic asks
            // it (GuaranteeRequest walk-in); a pack it cannot pull from range is left to the harness, whose
            // BlockerPullGate body-pulls only the pack on the only way to a boss or an event.
            bool const walkInRefused = !back && !(requested && requestedWalkIn);

            float const guaranteedCount = guaranteedWeight();
            if (walkInRefused && !wokenBoss && guaranteedCount <= float(allowed) + 0.01f)
            {
                _planVerdictHopeless = false;
                _planVerdictWhy = Acore::StringFormat("refused for now: no pull of {} from range, and the stand's "
                                                      "guarantee walks in on no pack (the route's body pull decides)",
                                                      target->GetName());
                LogPullBlocked(bot, "the stand's guarantee refused: " + _planVerdictWhy, target);
            }
            else if (wokenBoss || guaranteedCount > float(allowed) + 0.01f)
            {
                // hopeless only for a boss woken or past the over-cap scale; else the longer stand may take it
                _planVerdictHopeless = wokenBoss || guaranteedCount > float(guaranteeOverCap) + 0.01f;
                // [b33_brs] nothing woken on the way in: the pack's own chain is the count (was "wakes another pack")
                _planVerdictWhy =
                    firstWoken || wokenBoss
                        ? Acore::StringFormat(
                              "{}: the way in to {} wakes {}{}; with it {} mobs (counted {:.1f}), over {}",
                              _planVerdictHopeless ? "no safe pull" : "refused for now (unsafe, over the cap)",
                              target->GetName(), firstWoken ? firstWoken->GetName() : std::string("another pack"),
                              wokenBoss ? " and brings " + wokenBoss->GetName() : std::string(), guaranteed.size(),
                              guaranteedCount, allowed)
                        : Acore::StringFormat("{}: the chain of {} itself brings {} mobs (counted {:.1f}), over {}",
                                              _planVerdictHopeless ? "no safe pull"
                                                                   : "refused for now (unsafe, over the cap)",
                                              target->GetName(), guaranteed.size(), guaranteedCount, allowed);
                LogPullBlocked(bot, "the stand's guarantee refused: " + _planVerdictWhy, target);
            }
            // [b30_overpull] the over-cap pack's cost ceiling, as the lone mob's guarantee (GUARANTEE_OVER_CAP_MAX_COST)
            else if (PackEstimate const overEstimate = EstimatePack(guaranteed, bot, groupDps, estimateMode);
                     guaranteedCount > float(lastResortMobs) + 0.01f && overEstimate.cost > GUARANTEE_OVER_CAP_MAX_COST)
            {
                _planVerdictHopeless = true;
                _planVerdictWhy = Acore::StringFormat("no safe pull: {} brings {} mobs over the cap {} at cost {:.2f}, "
                                                      "over the guarantee's ceiling {:.2f}", target->GetName(),
                                                      guaranteed.size(), lastResortMobs, overEstimate.cost,
                                                      GUARANTEE_OVER_CAP_MAX_COST);
                LogPullBlocked(bot, "the stand's guarantee refused: " + _planVerdictWhy, target);
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
                _planReason = Acore::StringFormat("the stand's guarantee: blocked {} s, {} mobs within {}",
                                                  packBlocked / 1000, chosenPack.size(), allowed);
                LOG_INFO("playerbots", "[pull guarantee] {}: {} blocked {} s (no plan of its own), {} mobs with the "
                         "way in (allowed {}), cost {:.2f}: {} it, the stand ends", bot->GetName(), target->GetName(),
                         packBlocked / 1000, chosenPack.size(), allowed, chosen.cost,
                         back ? "pulls back" : "walks in on");
            }
        }
    }

    // [b32_smpull] A big pack is split, not walked round (user 2026-10-25: «мы дали задачу всех боссов убивать...
    // почему обойти, а не бить?»). A good group peels the chain from its edge: the nearest mob that comes with at most
    // SPLIT_MAX_MOBS, pulled at range with a single-target spell and fought back on the line from it through the tank
    // (KILL_SPOT_RETREAT_STEP at a time, SPLIT_RETREATS at most: the step back round a corner takes the rest out of
    // sight), where the healer sees, reached by walks that wake nobody else, and cheap enough for the hard minimum's
    // cap. Then the next one. Searched once per SPLIT_RETRY_MS for a pack the other rules refused as over the cap
    // or hopeless; only SPLIT_FAILS_HOPELESS failed searches over SPLIT_HOPELESS_MS make the refusal hopeless (the
    // harness's detour round it, hopeless_detour, is the last resort). lbrs-n 20261025-221216-lbrs-n-5D-10: the
    // Firebrand Grunt chain "brings 13 mobs over the cap 9 at cost 14.45" was hopeless at the first look and the
    // harness walked round it; strat-h King's Square ("22 mobs over 18"); sm-h Mograine's chapel (above, linked trash).
    constexpr size_t SPLIT_MAX_MOBS = 4;
    constexpr uint32 SPLIT_CANDIDATES = 6;
    constexpr uint32 SPLIT_RETREATS = 3;
    constexpr uint32 SPLIT_RETRY_MS = 3 * IN_MILLISECONDS;
    constexpr uint32 SPLIT_FAILS_HOPELESS = 3;
    constexpr uint32 SPLIT_HOPELESS_MS = 20 * IN_MILLISECONDS;
    constexpr uint32 SPLIT_MEMO_GAP_MS = 2 * MINUTE * IN_MILLISECONDS;
    struct SplitMemo
    {
        std::vector<ObjectGuid> pack;
        uint32 fails = 0;
        uint32 firstFailMs = 0;
        uint32 lastTryMs = 0;
    };
    static thread_local std::unordered_map<uint64, SplitMemo> splitMemos;  // per map thread, by tank
    bool const splitWanted = !found && toppedUp && !boss && !targetPackBoss &&
                             (_planVerdictHopeless || smallestPull > lastResortMobs ||
                              adds.size() + 1 > size_t(lastResortMobs));
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
    if (splitMemo && (!splitMemo->lastTryMs || getMSTimeDiff(splitMemo->lastTryMs, now) >= SPLIT_RETRY_MS))
    {
        splitMemo->lastTryMs = now ? now : 1;
        float const splitCostCap = std::max(CAREFUL_LAST_RESORT_HARD_CAP, _lastResortCap);
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
        auto const edgePeelMax = [&edgeInfo, lastResortMobs](Unit* candidate) -> size_t
        {
            size_t const least = SPLIT_MAX_MOBS;
            uint32 const unit = edgeInfo(candidate).unit;
            return std::max(least, size_t(unit <= lastResortMobs ? unit : 0));
        };
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
                if (pack.size() > edgePeelMax(candidate) ||  // [b44_packs] was SPLIT_MAX_MOBS
                    std::any_of(pack.begin(), pack.end(), [](Unit* mob) { return IsBossMob(mob); }))
                    continue;
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
                if (estimate.boss || estimate.cost >= splitCostCap)
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
            constexpr size_t SPLIT_LOS_MAX_MOBS = 6;
            size_t const losMaxMobs =
                std::max({SPLIT_MAX_MOBS, std::min(SPLIT_LOS_MAX_MOBS, size_t(lastResortMobs)),
                          edgePeelMax(candidate)});  // [b44_packs] a linked unit within the run's cap
            if (pack.size() > losMaxMobs ||
                std::any_of(pack.begin(), pack.end(), [](Unit* mob) { return IsBossMob(mob); }))
            {
                LOG_DEBUG("playerbots", "[b35_lbrs2] split_los: {}: {} brings {} mobs round the corner at ({:.1f} "
                          "{:.1f} {:.1f}), over {}", bot->GetName(), candidate->GetName(), pack.size(),
                          losSpot.GetPositionX(), losSpot.GetPositionY(), losSpot.GetPositionZ(), losMaxMobs);
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
            if (losEstimate.boss || losEstimate.cost >= splitCostCap)
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
            LOG_INFO("playerbots", "[b32_smpull] split_failed: {}: {} ({} mobs): no peel of at most {} mobs from {} "
                     "candidates (fewest {}; [b35_lbrs2] round a corner {} tried, fewest {}), search {} of {}",
                     bot->GetName(), target->GetName(), adds.size() + 1, SPLIT_MAX_MOBS, tried, fewest, losTried,
                     losFewest, splitMemo->fails, SPLIT_FAILS_HOPELESS);
        }
    }

    // [b33_aoepull] The AoE pull of a REQUIRED route blocker (user 2026-10-26: «почему обойти, а не бить?»; tanks gather
    // the adds, everyone AoEs the stack, in every dungeon). strat-n 20261026-000434-strat-n-5A-2 (and strat-h) King's
    // Square: the Ravaged Cadaver chain wakes 19-36 mobs over the cap 18, every split search failed ("no peel of at
    // most 4 mobs from 6 candidates (fewest 14-22)"), the harness rightly refused its walk-in ([b33_bodypull]) and the
    // run stalled on the only way on. A pack the split did not peel, that the harness marks required (AoePull::
    // MarkRequired: on the only way to a boss / event / required target), is pulled whole when all of these hold:
    //  (1) the group is fully ready: everyone alive and within AOE_PULL_GATHER_RANGE of the tank, topped up and nobody
    //      sitting below its line or buffing (toppedUp, IsAnyoneSittingBelowLine), every healer within
    //      AOE_PULL_HEALER_RANGE of the tank and in its sight, the healer seeing the cast spot and the stack spot;
    //  (2) the group's proven capacity in this run (ProvenFor: the biggest pull it survived cleanly) is at least
    //      AOE_PULL_PROVEN_SHARE of the predicted size, or the predicted size is within AOE_PULL_CAP_SCALE x the cap
    //      (overCapMobs: the cap the hopeless verdict names - "over 18" in King's Square and in mgt-h 20261026-000434-
    //      mgt-h-5G-16's Sunblade Mage Guard, 20-21 mobs, split fewest 17, proven 7.3, Blood DK tank) and the comp has
    //      a tank with AoE threat (HasAoeThreat) and AOE_PULL_MIN_AOE_DPS AoE damage dealers;
    //  (3) a stack spot: KILL_SPOT_RETREAT_STEP at a time (AOE_PULL_RETREATS at most) back on the line from the pulled
    //      mob through the tank, the healer sees it, the walks there wake nobody outside what comes; one round a corner
    //      (out of the pulled mob's sight: the chain turns the corner and stacks) is preferred to a plain back pull.
    // The plan pulls the nearest mob at range with a single-target spell (the chain follows) and fights at the spot,
    // logged as aoe_pull_plan (predicted, proven, spot) and published for the harness (AoePull::NotePlan). With (2)
    // and (3) holding but (1) not, the refusal is a readiness hold ("group not ready", RefusalIsReadiness), never a
    // dead end; with (2) or (3) failing, the hold / hopeless verdict below stands as before (aoe_pull_rejected).
    constexpr float AOE_PULL_PROVEN_SHARE = 0.7f;
    constexpr float AOE_PULL_CAP_SCALE = 1.5f;
    // [b34_strat] the most tank healths an aoe pull the comp carries (not the proven capacity) may cost
    constexpr float AOE_PULL_COMP_MAX_COST = 2.0f * GUARANTEE_OVER_CAP_MAX_COST;
    // [b38_mgt] ... and the comp carries at most this many times what the group proved in this run: mgt-n
    // 20261026-050341-mgt-n-5E-14 (1224-1255 s, lvl 70 Prot Pala, 14.9k hp, group dps ~2000) took the 16-mob Sunblade
    // Magister chain on "predicted 18, proven 0.8, tank aoe yes, aoe dps 2, cost 14.92": 15 attackers, tank, lock and
    // priest dead in 4 s. The same wave's strat Ravaged Cadaver aoe pull (Rtwaraa, cost 14.25) lived on proven 6.8
    // for 23 predicted (x3.4).
    constexpr float AOE_PULL_COMP_PROVEN_SCALE = 4.0f;
    constexpr uint32 AOE_PULL_MIN_AOE_DPS = 2;
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
                bool const provenCarries = proven.mobs >= AOE_PULL_PROVEN_SHARE * float(predicted);
                bool const compCarries = float(predicted) <= AOE_PULL_CAP_SCALE * float(overCapMobs) && tankAoe &&
                                         aoeDps >= AOE_PULL_MIN_AOE_DPS &&
                                         float(predicted) <= AOE_PULL_COMP_PROVEN_SCALE * proven.mobs;  // [b38_mgt]
                if (!provenCarries && !compCarries)
                {
                    failWhy = Acore::StringFormat("{} mobs predicted: proven {:.1f} (needs {:.1f}), cap {} x{:.1f}, "
                                                  "tank aoe {}, aoe dps {} (needs {}), comp at most x{:.0f} proven",
                                                  predicted, proven.mobs, AOE_PULL_PROVEN_SHARE * float(predicted),
                                                  overCapMobs, AOE_PULL_CAP_SCALE, tankAoe ? "yes" : "no", aoeDps,
                                                  AOE_PULL_MIN_AOE_DPS, AOE_PULL_COMP_PROVEN_SCALE);
                    return false;
                }
                // [b34_strat] the comp alone carries no pull the planner's own estimate calls unsurvivable: strat-h
                // 20261026-014521-strat-h-5E-1 took King's Square's Ravaged Cadaver chain on "26 predicted, proven
                // 0.0, tank aoe yes, aoe dps 3" at cost 45.50 tank healths (35 mobs with the patrol forecast) and
                // wiped (31 attackers, tank dead in 28 s). The four aoe pulls of build 34 that lived cost 3.0-17.9.
                if (!provenCarries)
                    if (PackEstimate const aoeEstimate = EstimatePack(pack, bot, groupDps, estimateMode);
                        aoeEstimate.cost > AOE_PULL_COMP_MAX_COST)
                    {
                        failWhy = Acore::StringFormat("{} mobs predicted at cost {:.2f}, over the aoe ceiling {:.2f} "
                                                      "(proven {:.1f}, needs {:.1f})", predicted, aoeEstimate.cost,
                                                      AOE_PULL_COMP_MAX_COST, proven.mobs,
                                                      AOE_PULL_PROVEN_SHARE * float(predicted));
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
                        LOG_INFO("playerbots", "[b33_aoepull] aoe_pull_plan: {}: {} ({} mobs, required route blocker): "
                                 "predicted {}, proven {:.1f} ({:.1f} weighted), cap {}, spot {} "
                                 "({:.1f} {:.1f} {:.1f}) {:.0f} yd back, pulled {} at range, tank aoe {}, aoe dps {}, cost {:.2f}",
                                 bot->GetName(), target->GetName(), adds.size() + 1, choice.predicted, proven.mobs,
                                 proven.weighted, overCapMobs, choice.corner ? "corner" : "back pull",
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

    if (!found)
    {
        // The inputs of the smallest plan against what the group takes (temporary diagnostics, wave 12 calibration).
        float const maxHealth = PlanTankHealth(bot);  // [b32_smpull]
        float const share = strength.PullScale() * HealerShare(support);
        TankMitigation const mitigation = GetTankMitigation(bot);
        LOG_DEBUG("playerbots",
                  "{} careful pull: no manageable pull around {} ({} mobs; best {} mobs{}: landed {:.0f}/s of {:.0f} "
                  "peak, cost {:.2f} tank hp over {:.0f} s at {:.0f} group dps, heals {:.0f}/s; tank {:.0f} hp, "
                  "armor {}, avoid {:.2f}, block {:.2f}/{:.0f}; hold {:.0f}, cap {}; size {:.2f} hold {:.2f} "
                  "peak {:.2f} drain {:.2f}; drop {:.2f} comfort {:.2f} spell {:.2f}; measured dps {:.0f}, landed "
                  "scale {:.2f})",
                  bot->GetName(), target->GetName(), adds.size() + 1, best.estimate.mobs,
                  best.pullBack ? " pull back" : "", best.estimate.landedDps,
                  maxHealth * CAREFUL_PEAK_PER_SECOND * share, best.estimate.cost, best.estimate.fightSec, groupDps,
                  maxHealth * CAREFUL_SUSTAIN_PER_SECOND * share, maxHealth, PlanTankArmor(bot), mitigation.avoidance,
                  mitigation.block, mitigation.blockValue, best.terms.holdMobs, maxMobs, best.terms.size,
                  best.terms.hold, best.terms.peak, best.terms.drain, best.terms.drop, best.terms.comfort,
                  best.estimate.spellCost, MeasuredGroupDps(bot), estimateMode.landedScale);
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
                                                      "{:.2f}); the stand's guarantee takes a pack within {} mobs",
                                                      target->GetName(), adds.size() + 1, _planVerdictFightMobs,
                                                      _planVerdictCost, lastResortMobs);

            // The wait mirrors the guarantee's tiers exactly (ok-n build 18: "15 mobs", waitSec 192, a pack the
            // guarantee could never take): within the cap 90 s, within the over-cap scale 180 s, past it hopeless.
            // A pack that brings a boss is no guarantee's: it is pulled as his fight once the group is ready for him.
            size_t const packSize = smallestPull;  // [b33_brs] the weighted smallest pull, as the guarantee's
            size_t const packOverCap = overCapMobs;
            if (!_planVerdictHopeless && !boss && !targetPackBoss && packSize > packOverCap)
            {
                _planVerdictHopeless = true;
                _planVerdictWhy = Acore::StringFormat("no safe pull: {} brings {} mobs, over {} (the cap {} x{:.1f})",
                                                      target->GetName(), packSize, packOverCap, baseLastResortMobs,
                                                      GUARANTEE_OVER_CAP_SCALE);
            }
            else if (boss || targetPackBoss)
                _planVerdictWhy = Acore::StringFormat("refused for now: the pack of {} brings a boss, pulled as his "
                                                      "fight once the group is ready for him", target->GetName());

            // [b32_smpull] hopeless only once the split search failed SPLIT_FAILS_HOPELESS times over
            // SPLIT_HOPELESS_MS on this pack (a group not topped up yet has not searched: it holds, no detour)
            if (_planVerdictHopeless && !boss && !targetPackBoss)
            {
                auto const memo = splitMemos.find(bot->GetGUID().GetRawValue());
                bool const searched =
                    memo != splitMemos.end() &&
                    std::find(memo->second.pack.begin(), memo->second.pack.end(), target->GetGUID()) !=
                        memo->second.pack.end();
                uint32 const fails = searched ? memo->second.fails : 0;
                bool const exhausted = fails >= SPLIT_FAILS_HOPELESS && memo->second.firstFailMs &&
                                       getMSTimeDiff(memo->second.firstFailMs, now) >= SPLIT_HOPELESS_MS;
                if (!exhausted)
                {
                    _planVerdictHopeless = false;
                    _planVerdictWhy = Acore::StringFormat("refused for now (split search {} of {}): ", fails,
                                                          SPLIT_FAILS_HOPELESS) +
                                      _planVerdictWhy;
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

            uint32 const packBlocked = PackBlockedMs(_planVerdictPack, now);
            uint32 const dueMs = packSize <= lastResortMobs ? CAREFUL_BLOCK_GUARANTEE_MS
                                                             : CAREFUL_BLOCK_GUARANTEE_OVER_MS;
            _planVerdictGuaranteeWaitMs = _planVerdictHopeless || boss || targetPackBoss || packBlocked >= dueMs
                                              ? 0
                                              : dueMs - packBlocked + CAREFUL_STALL_MS;  // the next slow-path look

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
        chosen.cost < SmallPackCostCap(chosen.mobs, CAREFUL_FALLBACK_MAX_COST) &&
        bot->GetHealthPct() >= CHAIN_PULL_TANK_HEALTH && IsGroupPresent(bot))
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
            bool const cycledOut =
                !staysPut && (forecastDecides ? overlapNoWindow : _overlapCycles > OVERLAP_MAX_CYCLES);
            std::string decision;
            if (staysPut)
                decision = "does not walk off, counted in the pack (plans it merged)";
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

    // A fallback that grew past CAREFUL_FALLBACK_MAX_COST (a patrol counted in) is not pulled, a lone mob past
    // CAREFUL_LAST_RESORT_HARD_CAP neither.
    // [careful_fallback_cap_consistent] The fallback's cap is the one its search used (SmallPackCostCap: 1.0 for a
    // plan of at most CAREFUL_SMALL_PACK_MOBS). Wave 23 (build 24): the search took 2-3 mob fallbacks at 0.86-0.98
    // under the small-pack cap, this gate held them under the plain 0.80 as "held for now: fallback plan costs 0.98
    // tank hp" - a hold no rest can change, the harness waits a "held for now" without a clock: gd-h 0/5, nex-h 1/5,
    // scholo-h stalled at a 2-mob pack for 4 minutes with the group full.
    float const singleCap = std::max(CAREFUL_LAST_RESORT_HARD_CAP, _lastResortCap);
    float const fallbackCap = SmallPackCostCap(chosen.mobs, CAREFUL_FALLBACK_MAX_COST);
    bool const splitCapped = _planSingle || splitPlan;  // [b36_sv] a corner peel: its search's cap (see splitPlan)
    if (fallback && !patrolFits && !chosen.boss && !_planGuaranteed &&
        chosen.cost >= (splitCapped ? singleCap : fallbackCap))
    {
        LogPullHold(bot, Acore::StringFormat("fallback plan costs {:.2f} tank hp, over {:.2f}", chosen.cost,
                                             splitCapped ? singleCap : fallbackCap)
                             .c_str(),
                    target);
        _planNotReadyWhy = Acore::StringFormat("fallback plan costs {:.2f} tank hp", chosen.cost);
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
                                             : BossReadyGap(bot, askBuffs, IsEncounterBoss(drawn));
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
        std::string const readyGap = BossReadyGap(bot, askBuffs, IsEncounterBoss(plannedMob));
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

    float const distance = bot->GetExactDist(candidate);
    if (distance <= range)
    {
        spot.Relocate(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    }
    else
    {
        float const share = range / distance;
        spot.Relocate(candidate->GetPositionX() + (bot->GetPositionX() - candidate->GetPositionX()) * share,
                      candidate->GetPositionY() + (bot->GetPositionY() - candidate->GetPositionY()) * share,
                      candidate->GetPositionZ() + (bot->GetPositionZ() - candidate->GetPositionZ()) * share);
    }

    // RFC 2026-10-24 18:28: the tank walked round a corner alone for Shoot range, out of the healer's sight, and
    // died under 7 mobs. A cast spot the healer does not see is no ranged pull: the cast spot moves on along the
    // approach (below) to one in range that sees the mob and the healer sees (RFC 2026-10-24 20:04: no such spot at
    // the spell's range, the tank walked in on a mob round the corner and the fight drew the pack beside it).
    if (candidate->IsWithinLOS(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ() + CAREFUL_EYE_HEIGHT) &&
        HealerSees(bot, spot))
    {
        _castSpots[candidate->GetGUID()] = {spot, getMSTime()};
        return true;
    }

    // The pull is cast in sight (IsWithinPullRange): the tank walks its path in until it sees the mob, and the cast
    // spot is there, or further in (still in range) where the healer sees it too. Stockade wave 2: a Defias Convict
    // in its cell, out of sight of the corridor spot at 30 yd; the tank walked in to 9.5 yd, by Hamhock's room, and
    // 11 came.
    PathGenerator path(bot);
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
        slot.issued = true;
    }

    bool TakeIssued(ObjectGuid tank, uint32 maxAgeMs, Decision& out, bool consume)
    {
        std::lock_guard<std::mutex> lock(leaderLock);
        auto const found = leaderSlots.find(tank);
        if (found == leaderSlots.end() || !found->second.issued)
            return false;

        if (getMSTimeDiff(found->second.decision.ms, getMSTime()) > maxAgeMs)
        {
            found->second.issued = false;  // stale: dropped
            return false;
        }

        if (consume)
            found->second.issued = false;  // taken once
        out = found->second.decision;
        return true;
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
        uint32 const lastResortMobs =
            std::max(baseLastResortMobs,
                     std::min(uint32(proven.mobs), uint32(float(baseLastResortMobs) * PROVEN_MAX_CAP_SCALE)));
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
