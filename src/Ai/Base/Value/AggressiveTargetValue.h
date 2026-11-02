/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AGGRESSIVETARGETVALUE_H
#define PLAYERBOTS_AGGRESSIVETARGETVALUE_H

#include "ObjectGuid.h"
#include "TargetValue.h"

#include <string>
#include <vector>

class PlayerbotAI;
class Player;
class Unit;

namespace RestState
{
    struct NextFight;
}

// Mobs of a pack the careful pull planner left after waiting topped up (nothing smaller came off it): the tank takes
// another pack for a while (AggressiveTargetAction::PlanCarefulPull).
namespace CarefulPullSkip
{
    void Mark(Player* tank, ObjectGuid mob);
    std::vector<ObjectGuid> Get(Player* tank);
}

// Mobs the tank's target choice rejected on its last look (another floor, no walkable path): the test harness does
// not hold the group's walk at their aggro edge, nobody would pull them (Stratholme / Gundrak wave 31-32). Written by
// the tank's map thread, read by the harness: behind a lock, entries age out.
namespace TankRejects
{
    void Set(Player* tank, std::vector<ObjectGuid> const& mobs);
    // A mob the harness found the tank never takes ("tank does not see it"): rejected for a few minutes.
    void MarkUnseen(ObjectGuid tank, ObjectGuid mob);
    bool IsRejected(ObjectGuid tank, ObjectGuid mob);
}

// The mob the test harness walks this tank to (its route target, TestHarnessMgr Party::target). Written by the
// harness on the world thread on every steer tick, read by the tank's map thread (AggressiveTargetValue): behind a
// lock, an entry ages out after a few seconds without a refresh. The tank's pick takes it up to
// ROUTE_TARGET_PICK_RANGE instead of the dungeon pull range, so the careful planner plans the harness's own pull from
// the approach stop instead of idling until the walk brings it within 40 yd (BRS heroic 22:41, Utgarde Keep 22:25).
namespace RouteTarget
{
    void Set(ObjectGuid tank, ObjectGuid mob);
    void Clear(ObjectGuid tank);
    // Empty when none, or stale.
    ObjectGuid Get(ObjectGuid tank);
}

// An encounter tactic that gives the pull back to the planner (no pull spot of its own: MgT's Selin room) asks for
// the stand's guarantee on a pack at once: the planner takes the pack of `mob` whole (within the mob cap, no boss,
// the group topped up) without the 90 s stand, walking in when `walkIn`. A request lives a few seconds; the tactic
// renews it while it holds.
namespace GuaranteeRequest
{
    void Set(ObjectGuid tank, ObjectGuid mob, bool walkIn);
    bool Get(ObjectGuid tank, ObjectGuid& mob, bool& walkIn);
}

// [b33_aoepull] The AoE pull of a REQUIRED route blocker (user 2026-10-26: «почему обойти, а не бить?», tanks gather
// the adds and everyone AoEs the stack). The test harness marks a pack that stands on the only way to a boss / event /
// required target (MarkRequired, refreshed while it holds or pulls there); the careful planner, for such a pack only,
// may issue an `aoe_pull_plan` (the whole chain pulled at range to a stack spot) once the group is fully ready and
// its proven capacity carries it, and publishes it (NotePlan) for the harness, which then holds its own walk-in.
// Written by the harness (world thread) and the tank's map thread: behind a lock, entries age out.
namespace AoePull
{
    void MarkRequired(ObjectGuid tank, ObjectGuid mob);
    // Any mob of `pack` is marked required for this tank (fresh).
    bool IsRequired(ObjectGuid tank, std::vector<ObjectGuid> const& pack);
    // The planner issued an aoe_pull_plan on `pack` (refreshed on every look while the plan stands).
    void NotePlan(ObjectGuid tank, std::vector<ObjectGuid> const& pack);
    // A fresh aoe_pull_plan on the pack `mob` belongs to; `sinceMs`: when that plan was first issued (getMSTime).
    bool Planned(ObjectGuid tank, ObjectGuid mob, uint32* sinceMs = nullptr);
}

// The tank's last careful-plan verdict on a pack (AggressiveTargetAction::PlanCarefulPull): accepted (the plan is
// chosen and the group is ready, the tank is about to pull) or refused as it stands. Written by the tank's map
// thread, read by its group's crowd control (PrePullCcAction) and the test harness without a cross-thread GetValue:
// behind a lock, an entry ages out after a few seconds (the tank refreshes it on every look).
//
// The fight the verdict is about (Set's `fight`, from the plan's PackEstimate) is read by the group's rest logic
// (RestState::PredictNextFight: drink for the next pull) through GetNextFight: the latest verdict, accepted or
// refused, while fresh (CAREFUL_VERDICT_TTL_MS), and for CAREFUL_VERDICT_AFTER_COMBAT_MS after the tank's last fight
// (the tank does not plan in combat, NoteCombat keeps the last plan alive through the fight and the rest after it).
namespace CarefulPlanVerdict
{
    struct Fight
    {
        uint32 mobs = 0;
        float fightSec = 0.0f;  // the estimate's fight length (0: not estimated)
        float cost = 0.0f;      // tank healths the pack lands over the fight
        bool boss = false;
        bool realBoss = false;       // the boss is a DungeonEncounter.dbc encounter of the map, not a mini-boss
        uint32 guaranteeWaitMs = 0;  // a refusal: how long until the stand's guarantee takes it (GetRefusal)
        bool refused = false;        // GetNextFight: the verdict refused the pack (the planner waits topped up)
    };

    // `target`: the mob the plan pulls (or refused); `packMobs`: every mob the verdict is about; `fight`: its
    // estimate (cost, length, boss), `mobs` stays the count the verdict names.
    void Set(Player* tank, ObjectGuid target, bool accepted, uint32 mobs, std::vector<ObjectGuid> const& packMobs,
             Fight const& fight = Fight(), bool hopeless = false, std::string const& why = std::string());
    // A fresh refusal of the pack `mob` belongs to: `hopeless` when even its smallest pull is over the planner's mob
    // cap (no pull will come, route around it), else the planner takes it once the stand's guarantee is due
    // (`waitMs` from now, 0: due); `why` names it. False when there is no fresh refusal.
    // False means unknown (no verdict on that pack, or a stale one): the harness holds as for an unknown wait.
    bool GetRefusal(ObjectGuid tank, ObjectGuid mob, bool& hopeless, uint32& waitMs, std::string& why);
    // The tank gave up a pull it had started before the cast (the walk for range would leave the healer, the mob on
    // another floor): the group holds where it stands while the tank plans again. NoteAborted by the pull actions;
    // RecentlyAborted: such a give-up within `withinMs`.
    void NoteAborted(Player* tank, ObjectGuid mob);
    bool RecentlyAborted(ObjectGuid tank, uint32 withinMs, ObjectGuid* mob = nullptr);
    // [b32_smpull] The pull of `mob` was given up again and again for range from where the tank stands (the walk for
    // range never came: a hazard hold, a refused path): the planner takes another mob, or holds, for `forMs`.
    void NotePullOutOfReach(Player* tank, ObjectGuid mob, uint32 forMs);
    bool PullOutOfReach(ObjectGuid tank, ObjectGuid mob);
    // [b37_sv] The ranged pull (pull back / los pull) of `mob` was given up again and again in its range but out of
    // its sight, because the step in for sight leaves the healer: no range problem, the mob is walked to. The planner
    // plans only the engage (walk in / charge, the fight walked back into the healer's sight) for `forMs`.
    void NoteEngageOnly(Player* tank, ObjectGuid mob, uint32 forMs);
    bool EngageOnly(ObjectGuid tank, ObjectGuid mob);
    // The tank's fresh verdict on the pack `mob` belongs to (its target or any mob of its pack); false when none.
    bool Get(ObjectGuid tank, ObjectGuid mob, bool& accepted, uint32& mobs);
    // The tank is fighting (called by the tank's own map thread, at most once a second): its last verdict is kept for
    // the rest after the fight.
    void NoteCombat(Player* tank);
    // The tank's latest verdict (accepted or refused) as the next fight: mob count, fight seconds, cost, boss. False
    // when there is none, or it is stale and the tank has not fought for CAREFUL_VERDICT_AFTER_COMBAT_MS.
    // [rest_after_boss_fight] an accepted verdict the tank has fought since (NoteCombat) names no boss any more: that
    // boss fight is over, the rest after it is no top-up for him (st-n-92: 95 % "before a boss" after Morphaz).
    bool GetNextFight(ObjectGuid tank, Fight& out);
    // [lead182b] the planner's look held at a readiness gate before any verdict: the pack it looks at (its mob count)
    // is the next fight until a verdict comes (GetNextFight's fallback, fresh as a verdict)
    void NoteLook(Player* tank, uint32 mobs);
    // As above, filling the rest logic's struct (RestState::NextFight, HealthTriggers.h).
    bool GetNextFight(ObjectGuid tank, RestState::NextFight& out);
}

// [pull_estimate_vs_actual] Every planned careful pull, once its fight is over (PullCheck in ChooseTargetActions.cpp):
// the planner's estimate against the mobs that actually engaged the group. The test harness takes each new one into
// its JSONL ("pull_estimate_vs_actual"). [adaptive_cap] Proven: what the tank's group proved it takes in this instance
// (the biggest clean fight, weighted by mob danger, with the headroom its end left), false when nothing is proven.
// Thread-safe (the tank's map thread writes, the harness reads).
namespace PullEstimate
{
    struct Result
    {
        uint32 id = 0;  // 0: none; grows with every finished pull of any tank
        ObjectGuid target;
        std::string kind;    // engage, pull back, los pull, one
        std::string reason;  // the plan's kind with its reason
        std::string via;     // how it opened: spell <name>, body, cc, pet
        uint32 estimated = 0;
        float cost = 0.0f;
        uint32 engaged = 0;
        uint32 deaths = 0;
        uint32 durationSec = 0;
        bool fought = false;
    };

    // The tank's last finished planned pull, when its id is not `afterId`.
    bool Take(ObjectGuid tank, uint32 afterId, Result& out);
    bool Proven(Player* tank, float& cost, float& mobs);  // [lead262] the proof: its cost cap and its mobs
    // [pull_label] when the tank's planner last carried out a plan (PullCheck::Planned: its pull plan, spot and
    // readiness were judged), getMSTime; 0: never
    uint32 LastPlannedMs(ObjectGuid tank);
}

// Encounter order: a mob whose script refuses an engage until an earlier event of its instance has started evades at
// once when pulled (Utgarde Keep heroic wave 6, 00:07:37: the tank pulled Forge Master 3 first, 1 s, 0 kills, then
// stuck). The tank's own pick (AggressiveTargetValue), its approach walk and the careful planner pass such a mob and
// its formation over; the test harness keeps its own gate (UKIds::IsForgeMasterLocked). One rule per dungeon in
// IsLocked; it reads the bot's own map's instance data (map thread).
namespace EncounterOrder
{
    // `mob`, or a member of its formation, cannot be engaged yet: its event's predecessor has not started.
    bool IsLocked(Unit* mob);
}

// [b32_sh_ooze] Respawn-ignored entries per instance: hostile non-boss DB spawns whose creature.spawntimesecs is at
// most RESPAWN_IGNORE_MAX_S, marked by the test harness at the run's start (sh-h 20261025-223237-sh-h-5B-71: the four
// Creeping Oozeling 17357 patrols respawn every 5 s, the group re-cleared them 10 minutes). A spawn of such an
// entry with a respawn delay of at most RESPAWN_IGNORE_MAX_S is no route target, stops no walk and is no tank pick
// while idle; it is fought when it attacks. Written by the harness (world thread), read by the tanks' map threads:
// behind a lock.
namespace RespawnIgnored
{
    constexpr uint32 RESPAWN_IGNORE_MAX_S = 120;
    // True when `entry` is new for this instance.
    bool Mark(uint32 instanceId, uint32 entry);
    // `unit` is a DB spawn of a marked entry in its instance with a respawn delay of 1..RESPAWN_IGNORE_MAX_S s.
    bool IsIgnored(Unit const* unit);
    // Any entry marked in this instance (a respawn zone exists).
    bool Any(uint32 instanceId);
}

class AggressiveTargetValue : public TargetValue
{
public:
    AggressiveTargetValue(PlayerbotAI* botAI, std::string const name = "aggressive target") : TargetValue(botAI, name) {}

    Unit* Calculate() override;
    // [lead5] The choice Calculate makes, with the route target given (Calculate: the RouteTarget registry's; the
    // dungeon leader with LEADER_ONE_PICKER: its own, on the world thread) and the caller's reachability cache.
    static Unit* PickFor(PlayerbotAI* botAI, ObjectGuid routeTarget, TargetReachability& reachability);
    // Diagnostics (test harness): why this bot's possible / aggressive target rejects the unit, "accepted" when not.
    static std::string Explain(Player* bot, Unit* unit);
    // [lead5] ... with the route target the pick sees given (the leader's own)
    static std::string Explain(Player* bot, Unit* unit, ObjectGuid routeTarget);

private:
    TargetReachability _reachability;
};

#endif
