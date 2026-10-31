/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CHOOSETARGETACTIONS_H
#define PLAYERBOTS_CHOOSETARGETACTIONS_H

#include "AttackAction.h"
#include "LeaderOrders.h"  // [lead7] Leader::PullMethod in PullPlanner::Decision
#include "ObjectGuid.h"
#include "PartyStrengthValue.h"  // [lead7] PartyStrength in PullPlanner::Snapshot
#include "Position.h"

#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class Creature;  // [lead4]
class Player;
class PlayerbotAI;
class Unit;

namespace CarefulPlanVerdict
{
    struct Fight;  // [lead5] AggressiveTargetValue.h
}

// [lead7] C:/WoW/LEADER_REFACTOR_PLAN.md step 7: the careful pull planner as the dungeon leader's library.
//
// The planner (AggressiveTargetAction's look: the readiness gates, PlanCarefulPull, PlanLosPull, the split search,
// the guarantee, FindApproachTarget, HoldForPrePullCc) runs on a planner instance of its own per tank, not on the
// tank's action: its clocks and memos are the planner's, keyed by the tank. It reads the tank's value context once
// per look through a Snapshot, and returns a Decision instead of acting: the tank's Execute is the executor.
//
// Callers:
// - LEADER_PLANNER_IN_LEADER (Leader:: switches, LeaderOrders.h): the test harness, on the world thread after the map
//   updates (no map thread runs then), plans for its party's tank whenever the tank asks (Ask: its engine looked at
//   "aggressive target", as it looked at the tank's own planner before), publishes the planner's verdicts as before
//   and issues a Go decision (Issue); the tank only takes it (TakeIssued) and executes it.
// - LEADER_PLANNER_DIFF: the tank decides as before, and right after its own look the planner instance looks in
//   shadow (no side effect, no log line) at the same instant; disagreements are queued (TakeDiff, "plan_diff").
// - Every other group (a real player leads, LEADER_* off): the tank's own action plans as before.
namespace PullPlanner
{
    // [lead3] [lead4] [lead5] [lead6] [lead8] [lead10] What the leader (the test harness) hands its planner on a look,
    // on top of the tank's value context: its own pick and requests, its readiness verdict, its door state and its
    // cleared spots. Only a leader look binds it (Snapshot::BindLeader); the tank's own look and every group the
    // harness does not lead see none, so their planner runs as before.
    struct LeaderInputs
    {
        bool bound = false;
        // [lead5] the look target the leader picked (AggressiveTargetValue::PickFor with its route target, or a
        // request's mob); empty: the leader picks no mob (no look, no approach walk of the planner's own)
        ObjectGuid pick;
        std::string pickWhy;
        // [lead3] the pack the leader must clear (the old harness body pull's domain): the planner's stand's
        // guarantee offers it whole at once (allowWholePack, up to the over-cap scale), walked in when `walkIn` (the
        // walk's wakes counted by the guarantee's own rounds)
        ObjectGuid wholePack;
        bool walkIn = false;
        // [lead5] the packs the leader marked required (the old AoePull::MarkRequired): the aoe_pull_plan may run
        GuidVector required;
        // [lead4] the patrol the leader decided to pull (an intercept): planned as the target, no patrol hold for it
        ObjectGuid intercept;
        // [lead6] the leader's one readiness verdict (LeaderReadiness): not ready holds the look
        bool ready = true;
        std::string notReadyWhy;
        // [lead10] members the leader found cut off behind a closed door (the old TestRunner::IsDoorCut)
        GuidVector doorCut;
        // [lead8] spots the group fought and cleared this run (the leader's record): pull-back fight spot candidates
        std::vector<Position> cleared;
    };

    // [lead3] The leader inputs of the library look running on this thread (nullptr: none, the tank's own look).
    LeaderInputs const* CurrentLeaderInputs();

    // What a look reads from the tank's value context, read once per look and lazily: a field the look does not
    // need is never computed, so the tank's own value caches keep their timing.
    class Snapshot
    {
    public:
        explicit Snapshot(PlayerbotAI* tankAI);

        // [lead3] a leader look: its pick, requests, readiness, doors, cleared spots (LeaderInputs)
        void BindLeader(LeaderInputs const& inputs) { _leader = inputs; _leader.bound = true; }
        LeaderInputs const* Leader() const { return _leader.bound ? &_leader : nullptr; }

        Player* Tank() const { return _tank; }
        uint32 Ms() const { return _ms; }
        Position const& TankPos() const { return _tankPos; }
        Unit* Target();  // "aggressive target"
        PartyStrength Strength();  // "party strength"
        GuidVector Possible();  // "possible targets"
        GuidVector PossibleNoLos();  // "possible targets no los"
        float GroupDps();  // "estimated group dps"

    private:
        PlayerbotAI* _ai = nullptr;
        Player* _tank = nullptr;
        uint32 _ms = 0;
        Position _tankPos;
        bool _hasTarget = false;
        Unit* _target = nullptr;
        bool _hasStrength = false;
        PartyStrength _strength;
        bool _hasPossible = false;
        GuidVector _possible;
        bool _hasPossibleNoLos = false;
        GuidVector _possibleNoLos;
        bool _hasGroupDps = false;
        float _groupDps = 0.0f;
        LeaderInputs _leader;  // [lead3]
    };

    enum class Verdict : uint8
    {
        None,     // no look: the tank fights, is no tank, or a room tactic owns the pull
        Go,       // the tank executes the decision now
        Held,     // a readiness gate or a wait holds the pull (a re-check soon)
        Refused   // the planner found no pull it may make around the target
    };
    char const* VerdictName(Verdict verdict);

    // One look's decision. Go: what the tank executes (AggressiveTargetAction::Execute's inputs, nothing else).
    struct Decision
    {
        uint32 ms = 0;
        ObjectGuid tank;
        Verdict verdict = Verdict::None;
        bool hopeless = false;     // Refused: even the smallest pull is over the mob cap (no guarantee comes)
        bool careful = false;      // the careful planner looked (else the default pull mode)
        ObjectGuid lookTarget;     // the mob the look was about (the aggressive target, or the crowd control swap)
        std::string why;           // Held / Refused: the reason; Go: the plan's reason
        GuidVector pack;           // the verdict's pack
        // Go
        std::string kind;          // "approach", "side walk", "attack", "engage", "pull back", "los pull", "one"
        Leader::PullMethod method = Leader::PullMethod::Unknown;
        ObjectGuid approach;       // kind "approach": walk toward this mob (no pick in range for long)
        ObjectGuid planTarget;     // the mob to tag (empty: attack lookTarget, no careful plan)
        bool pullBack = false;
        bool los = false;
        bool single = false;
        bool countsSap = false;
        bool patrolFirst = false;
        uint32 mobs = 0;           // expected mobs (0: not a careful plan)
        float cost = 0.0f;
        float drop = 0.0f;
        std::string reason;
        bool side = false;         // walk to sideSpot first, the plan goes on from there
        Position sideSpot;
        ObjectGuid sideFor;
        uint32 sideMs = 0;
        std::string sideNote;
        bool hasCast = false;      // the stand spot of a ranged pull (PullCastSpot)
        Position castSpot;
        ObjectGuid killFor;        // a pull back is fought at kill (its call for help reaches fewer)
        Position kill;
        ObjectGuid fightFor;       // an engage whose fight the tank walks back to fight once it is on
        Position fight;
        float fightBack = 0.0f;
        std::string fightNote;
        Position losSpot;          // a line-of-sight pull: the tank's corner and the group's gather spot
        Position losGather;
        GuidVector losPack;
        // [lead3] a leader-issued attack / boss pull: the tank attacks this mob (not its own aggressive target)
        ObjectGuid attack;
        // [lead5] the verdict this look published (CarefulPlanVerdict's data, for the leader's own memory: Steer reads
        // it instead of the cross-thread registry)
        bool hasVerdict = false;
        ObjectGuid verdictTarget;
        bool verdictAccepted = false;
        uint32 verdictMobs = 0;
        GuidVector verdictPack;
        bool verdictHopeless = false;
        std::string verdictWhy;
        uint32 verdictWaitMs = 0;
        // [lead5] the look issued an aoe_pull_plan on this pack (the old AoePull::NotePlan)
        GuidVector aoePlan;
        // [lead3] a refusal: the group stood topped up at the look (IsGroupToppedUp), so the refusal is no readiness
        bool toppedUp = false;

        std::string Describe() const;  // one short line (diff log)
    };

    // [lead3] A boss pull is a leader option (the planner never picks a boss): the leader issues it as an engage of
    // `boss` with its own encounter pack counted where he stands. false: the boss is locked (EncounterOrder) or gone.
    bool BossPullDecision(Player* tank, Unit* boss, std::string const& why, Decision& out);
    // [lead10] The whole-pack cap of the tank's group (the stand's guarantee's over-cap scale, the proven size
    // included); 0: no cap (the default pull mode).
    uint32 WholePackCap(Player* tank);
    // [lead4] The one patrol forecast both sides read: true when the patrol's waypoint walk (its formation leader's)
    // takes it farther than `reach` from `spot` somewhere (a window to wait for); false: it never leaves (a loop round
    // the spot): a wait is no option, the patrol is pulled. `known` false: no waypoint walk to forecast (the answer is
    // true then, and the caller keeps its own bound).
    bool PatrolLeavesReach(Creature* patrol, Position const& spot, float reach, bool* known = nullptr);

    // The planner's thread-local shadow flag: no log line, no side effect while a shadow look runs.
    bool IsShadow();

    // Runs one look of the tank's planner instance (created on first use, rebuilt when the tank's AI changes) on
    // `snap`. false: no instance could run (no AI). `shadow`: the diff mode's look (no side effect, no log line).
    bool Decide(Player* tank, Snapshot& snap, bool shadow, Decision& out);
    // The run ended: the tank's planner instance, its asks, issued decisions and diff counts go.
    void Release(ObjectGuid tank);
    // [lead33] the planner's danger model on a set of mobs (the same estimate and overload gate as a plan): true when
    // the group takes them in one fight; `why` the cost against the budget
    bool JudgePack(Player* tank, std::vector<ObjectGuid> const& mobs, float& overload, std::string& why);
    // [lead33] the leader's synchronous question for its own chosen target: a plan, or a refusal / hold with its
    // reason (Decision::verdict / why)
    bool Plan(Player* tank, Unit* target, Snapshot& snap, Decision& out);

    // LEADER_PLANNER_IN_LEADER: the tank's engine asked for a decision (any thread); the leader takes the ask (true:
    // one newer than `maxAgeMs` and not taken yet) and issues a Go decision; the tank takes it once while fresh.
    void Ask(Player* tank);
    bool TakeAsk(ObjectGuid tank, uint32 maxAgeMs);
    // [lead13] the tank asked within `ms` (taken or not): it stands without a target, the leader keeps a decision
    // ready for its next look
    bool AskedWithin(ObjectGuid tank, uint32 ms);
    void Issue(ObjectGuid tank, Decision const& decision);
    // [lead4c] `consume` false: a peek (isUseful may run without its Execute following); ConsumeIssued(ms) takes the
    // issued decision of that stamp once it is executed
    bool TakeIssued(ObjectGuid tank, uint32 maxAgeMs, Decision& out, bool consume = true);
    void ConsumeIssued(ObjectGuid tank, uint32 decisionMs);
    // [lead49] the leader replaces / cancels its issued order (a change of facts): the slot is empty
    void CancelIssued(ObjectGuid tank);

    // LEADER_PLANNER_DIFF: the tank's own decision against the planner instance's shadow look at the same instant.
    // `kinds` empty: they agree. Counted always; queued as a line when the disagreement is new (another signature)
    // or PLAN_DIFF_LOG_MS after the last queued one.
    std::string DiffKinds(Decision const& own, Decision const& library, bool& whyOnly);
    void NoteDiff(Player* tank, Decision const& own, Decision const& library);
    struct DiffLine
    {
        uint32 ms = 0;
        ObjectGuid mob;
        std::string kinds;
        std::string own;
        std::string library;
        uint32 repeats = 0;  // the same disagreement since the last line
    };
    bool TakeDiff(ObjectGuid tank, DiffLine& out);
    struct DiffCounts
    {
        uint32 compared = 0;
        uint32 same = 0;
        uint32 whyOnly = 0;  // the same decision, its reason in other words (a clock's seconds)
        std::map<std::string, uint32> byKind;
    };
    bool GetDiffCounts(ObjectGuid tank, DiffCounts& out);
}

// An idle mob whose aggro range reaches the spot (and sees it), a wanderer whose wander radius brings it there, or a
// patrol that walks into it: the careful pull's test, for anyone who stands somewhere before a pull (a rogue's Sap).
bool IdleMobReachesSpot(Player* bot, Unit* mob, Position const& spot);

// The group's healers see `spot` from where they stand now, within their heal range (true without a healer here).
bool HealerSeesSpot(Player* bot, Position const& spot);

// User, Ragefire Chasm 2026-10-24: a tank that has aggroed a mob walks it back where it came from, so the mobs that
// walk about (wanderers, patrols) do not come into the fight. The careful plan moves a pull back's kill spot out of
// their reach; this keeps the fight's spot per tank (its own map thread) for the pull and the fight that follow.
namespace FightSpot
{
    // An engage a mover walks into: once the fight is on, the tank walks it back to `spot` (`back` yd), once.
    void Plan(Player* tank, Position const& spot, float back, std::string const& reason);
    // The next fight's spot was checked against the movers already (a pull back's kill spot): no walk back.
    void Chosen(Player* tank);
    // Right after the pull is cast: a mover that can reach the pull position `spot` moves it back (true, `spot` set).
    bool Review(PlayerbotAI* botAI, Player* tank, Unit* target, Position& spot);
    // In combat, no pull on: the spot the tank walks the fight back to now, if any (one walk per fight; the tank's
    // own spot is looked at once a second in the fight's first seconds).
    bool Due(PlayerbotAI* botAI, Player* tank, Position& spot);
    // The walk back under way, without a look.
    bool Walking(Player* tank, Position& spot);
    void Arrived(Player* tank);
}

// The cast spot of the planned ranged pull (checked for the healer's sight by the plan, PullBackSpot): the pull's
// walk for range goes there instead of a point on the line at the spell's range (scholo-h build 21: that point left
// the healer, the pull was aborted again and again). Per tank, for its pull target; a few seconds' life past the pull.
namespace PullCastSpot
{
    void Set(Player* tank, ObjectGuid target, Position const& spot);
    void Clear(Player* tank);
    bool Get(Player* tank, ObjectGuid target, Position& spot);
    // [prepull_sight_spot] the pull back's kill spot (where the mob is fought, PullBack), set after Set.
    void SetKill(Player* tank, ObjectGuid target, Position const& kill);
    // The tank's latest fresh cast spot whatever its target (for the group's ranged before the first hit); `hasKill`
    // when a kill spot was published with it.
    bool GetLatest(Player* tank, ObjectGuid& target, Position& spot, Position& kill, bool& hasKill);
}

// Every executed careful plan is checked against what came: "[pull check] <tank>: planned N mobs cost C -> came M,
// deaths D, duration S, via <how>" once the fight is over. How the pull happened: "spell <name>" (the pull cast
// landed first), "body" (a mob aggroed on proximity, or before the cast), "cc" (a pre-pull crowd control opener),
// "pet".
namespace PullCheck
{
    // `kind`: engage, pull back, los pull, or one (the hard minimum).
    // `spot`: where the pull is cast from ("default", or a side spot with its count against the default one's).
    void Planned(Player* tank, ObjectGuid target, uint32 mobs, float cost, float drop, std::string const& kind,
                 std::string const& spot);
    // The pull action cast `spell` on its target.
    void NoteSpell(Player* tank, std::string const& spell);
    // The tank's fight so far; the caller limits it to about once a second.
    void Sample(PlayerbotAI* botAI);
    // [chain_pull] when Sample last saw the tank's group fighting (getMSTime, 0: never).
    uint32 LastFightMs(Player* tank);
}

// [pull_waits_for_sitters] The group is still getting ready for its next fight: a member close by still casts its
// rebuff round (ForceRebuffState pending, what the leader's ready check answers "buffing" for), or sits drinking or
// eating below its own rest line (RestState::WantsDrink / WantsFood). `why` names the first such member. The tank's
// own pulls (planner, stand guarantee, the walk in to an idle mob) hold on it; a sitter above its line stands up by
// itself, so the hold lasts no longer than the rest the bot asked for.
namespace PullReadiness
{
    // [lead17b] the leader's one readiness verdict for a led tank's group (false: none - the planner's own rules)
    bool LeaderVerdict(Player* bot, bool& ready, std::string* why);
    bool GroupPreparing(Player* bot, std::string* why = nullptr);
    // [boss_pull_ready_gate] What keeps the group from being ready for `boss` (the planner's BossReadyGap: health,
    // mana, sitting, buffing, a missing group buff); empty when ready.
    std::string BossNotReady(Player* bot, Unit* boss);
    // [lead46] the same readiness for one member: its health / mana below the boss ready lines (BossReadyGap's,
    // a real boss or a mini-boss by IsEncounterBoss); empty when this member is ready for `boss`
    std::string MemberBossGap(Player* member, Unit* boss);
}

class DpsAoeAction : public AttackAction
{
public:
    DpsAoeAction(PlayerbotAI* botAI) : AttackAction(botAI, "dps aoe") {}

    std::string const GetTargetName() override { return "dps aoe target"; }
};

class DpsAssistAction : public AttackAction
{
public:
    DpsAssistAction(PlayerbotAI* botAI) : AttackAction(botAI, "dps assist") {}

    std::string const GetTargetName() override { return "dps target"; }
    bool isUseful() override;

    // [healer_heals_first] A healer in combat whose "party member to heal" is below "almost full health" has
    // healing to do: the target pickers ("dps assist" at 50, "drop target" at 99) must not take its tick over the
    // heal triggers (~20-23). Wipe analysis w24-25: with the tank under 50 % the healer cast a heal in only 32.5 %
    // of 431 samples. False out of combat, with the whole group topped up, or out of mana (then a wand is all).
    static bool HealerHasHealingWork(PlayerbotAI* botAI);

    // [healer_audit_b30] A living healer in a group, with itself or a group member within 2 x HealDistance under
    // attack (attackers, or in combat with a victim). The healer belongs on the combat engine then even before its
    // own combat flag: ok-h-5G-37 18:52:57-18:53:08 the holy paladin stayed on the non-combat engine ("follow",
    // "flash of light on party [nc]") while the Blood DK tank went 74 -> 0 %. "dps assist", its only way into the
    // combat engine, is off while the healer has healing work ([healer_heals_first]).
    static bool HealerGroupUnderAttack(PlayerbotAI* botAI);
};

class TankAssistAction : public AttackAction
{
public:
    TankAssistAction(PlayerbotAI* botAI) : AttackAction(botAI, "tank assist") {}

    std::string const GetTargetName() override { return "tank target"; }
};

class AggressiveTargetAction : public AttackAction
{
public:
    AggressiveTargetAction(PlayerbotAI* botAI) : AttackAction(botAI, "aggressive target") {}

    // [lead70] the tank's own walk-in for an engage out of sight, run dry (MovementAction::s_dryRun): would its walk
    // guards (path, ledge drop, closed door, idle packs on the path, fight zone) let it walk in from where it stands?
    bool DryWalkIn(Unit* target);

    std::string const GetTargetName() override { return "aggressive target"; }
    bool Execute(Event event) override;
    bool isUseful() override;

    // [lead7] One look of this planner instance on `snap` (PullPlanner::Decide): the decision, and this instance's
    // state as after the tank executed a Go decision (MirrorExecute). `shadow`: no side effect, no log line.
    bool LibraryLook(PullPlanner::Snapshot& snap, bool shadow, PullPlanner::Decision& out);
    // [lead33] the danger judge of a set of mobs (PullPlanner::JudgePack)
    bool JudgeMobs(std::vector<Unit*> const& mobs, float& overload, std::string& why);

private:
    // [lead7] The look itself (the tank's own isUseful before step 7): the readiness gates, the plan, the verdicts.
    bool DecideLook();
    // [lead7] LEADER_PLANNER_IN_LEADER, the tank's side: ask the leader, take its fresh Go decision (loaded into the
    // members Execute reads) or do nothing. The tank decides nothing here.
    bool TakeLeaderDecision();
    void FillDecision(bool go, PullPlanner::Decision& out);
    void LoadDecision(PullPlanner::Decision const& decision);
    void MirrorExecute();
    // [lead7] The planner's reads of the tank's value context: from the look's snapshot when one is bound (a library
    // look), else the tank's own (AI_VALUE, as before).
    Unit* PlanAggressiveTarget();
    PartyStrength PlanStrength();
    GuidVector PlanPossible(bool noLos);
    float PlanGroupDps();
    PullPlanner::Snapshot* _snap = nullptr;
    bool _shadow = false;      // a diff-mode look: no verdict, no crowd control registration, no aoe plan note
    bool _fromLeader = false;  // the decision Execute runs was issued by the leader (the tank only executes)
    uint32 _fromLeaderMs = 0;  // [lead4c] its stamp (PullPlanner::ConsumeIssued)
    // [lead31b] the leader's decision this tank executes now: while its order stays open (Issued) out of combat, the
    // executor carries it on (the sight walk, the approach, the tag) by itself - nobody issues it again
    PullPlanner::Decision _activeDecision;
    bool _hasActive = false;
    // [lead5] CarefulPlanVerdict::Set for this look (not by a shadow look), recorded for the leader's own memory
    // (Decision::verdict*): the leader's Steer reads its planner's verdicts there, not from the registry.
    void PublishVerdict(ObjectGuid target, bool accepted, uint32 mobs, GuidVector const& pack,
                        CarefulPlanVerdict::Fight const& fight, bool hopeless, std::string const& why);
    bool _verdictRec = false;
    ObjectGuid _verdictTarget;
    bool _verdictAccepted = false;
    uint32 _verdictMobs = 0;
    GuidVector _verdictPack;
    bool _verdictHopeless = false;
    std::string _verdictWhy;
    uint32 _verdictWaitMs = 0;
    GuidVector _aoePlanNoted;   // [lead5] this look's aoe_pull_plan pack (AoePull::NotePlan)
    ObjectGuid _leaderAttack;   // [lead3] a leader-issued attack / boss pull: the mob Execute attacks
    Leader::PullMethod _leaderMethod = Leader::PullMethod::Unknown;  // [lead3] its method in the reports
    PullPlanner::Verdict _lookVerdict = PullPlanner::Verdict::None;  // what DecideLook's last return meant
    bool _lookCareful = false;
    ObjectGuid _lookTarget;
    std::string _lookWhy;

    // A levelling 5-man tank plans its pull: the mob to open on (the target itself, or an add at the edge of a room
    // or before a boss) and whether it pulls it at range back to where it stands. `measured`: a heroic or max-level
    // 5-man (Playerbots.CarefulPullHeroic) or a levelling one (Playerbots.CarefulPullMeasuredLevelling), where the
    // pack is sized by its real damage and health; `rescaled`: heroic or max-level content (rescaled caster spells).
    // A pack refused for long with the group topped up is pulled by its smallest plan if the tank carries it, else
    // by a lone mob of it at range, else left for another pack. Patrols are forecast along their waypoints: one that
    // walks into the group is pulled first or waited for, one that walks into the fight is waited out, pulled first,
    // or counted in the plan.
    bool PlanCarefulPull(Unit* target, bool measured, bool rescaled);
    // `single`: the pull must bring the mob alone (no spell that jumps or hits an area).
    // `range` (optional): the pull spell's range.
    bool PullBackSpot(Unit* candidate, Position& spot, bool single = false, float* range = nullptr);
    // The cast spots PullBackSpot found this plan, by mob (published for the pull that is made: PullCastSpot).
    std::unordered_map<ObjectGuid, std::pair<Position, uint32>> _castSpots;
    void PublishCastSpot(Unit* target);
    bool PullBack(Unit* target, bool single = false);

    // A pack with several casters (MobRoles: standing or ranged) is pulled at range, and the tank then steps round
    // a corner none of the pack can see past, so the casters walk into melee (LosPull in PullStrategy.h). `accept`
    // judges the pack that comes; true when a spot was found, kept in _losSpot / _losGather / _losPack.
    bool PlanLosPull(Unit* candidate, GuidVector const& possible,
                     std::function<bool(std::vector<Unit*> const&)> const& accept, bool assistChain = false);
    bool StartLosPull(Unit* target);

    // A hard pack (Playerbots.AutoCcPrePull): the group's crowd control claims mobs of the tail of its kill order
    // first (PrePullCc in PullStrategy.h); true while the tank holds its pull for it. `heavy`: the careful planner
    // refused the pack as it stands; `refused`: called for a refused verdict (a plan registered then is reopened
    // when the verdict turns to accepted: PrePullCc::Register `reopen`).
    // `plannedMobs` (an accepted plan, 0: none): what the plan expects to come; a plan that splits the pack (fewer than
    // come when its pull target is engaged where it stands) gets no crowd control opener at the pack.
    bool HoldForPrePullCc(Unit* pullTarget, bool heavy, bool refused = false, uint32 plannedMobs = 0);

    // A group buff someone of the group can give is missing: the tank waits for it a while (cached, see
    // PULL_BUFF_CHECK_MS).
    bool WaitForBuffs();

    // No pick in range for long while the group stands topped up: the nearest possible target to walk toward.
    ObjectGuid FindApproachTarget();

    struct LosPullMemo
    {
        ObjectGuid mob;
        uint32 ms = 0;
        bool found = false;
        float botX = 0.0f;  // where the tank stood: the cast spot, and so the search, depend on it
        float botY = 0.0f;
        Position spot;
        Position gather;
    };

    ObjectGuid _planTarget;  // set by isUseful for the Execute that follows
    ObjectGuid _planCached;  // the plan for _planFor, reused for a second
    ObjectGuid _planFor;
    uint32 _planMs = 0;
    bool _planResult = false;
    bool _planPullBack = false;
    bool _planLos = false;
    bool _planRefused = false;   // the last plan found no manageable pull around the target
    bool _planFallback = false;  // the last plan is the topped-up fallback for a pack over the group's size
    bool _planSingle = false;    // the last plan pulls a lone mob of a pack too big for the tank, at range
    bool _planCountsSap = false;  // the last accepted plan counts a claimed pre-pull Sap as applied (held for it)
    std::string _planReason;     // why the last plan pulls back / engages ("[pull plan]", "[pull check]")
    std::string _planNotReadyWhy;  // which early return of the last plan held it ("careful plan not ready")
    ObjectGuid _blockedTarget;     // the last lone mob the hard cap blocked (any mob of the stand: escalation)
    uint32 _blockedSinceMs = 0;    // the tank's first hard-minimum look of this stand (0: none; reset by a fight)
    uint32 _blockedSeenMs = 0;     // its last one (a gap over CAREFUL_BLOCK_GAP_MS starts a new stand)
    // The stand per pack (any mob of a refused pack: since, last seen), so the harness's retargets and skips do not
    // restart the guarantee's clock (CAREFUL_BLOCK_GUARANTEE_MS).
    std::unordered_map<ObjectGuid, std::pair<uint32, uint32>> _packBlocked;
    bool _planGuaranteed = false;  // the last plan is the stand's guarantee (no fallback cost cap afterwards)
    bool _planVerdictHopeless = false;  // a refusal whose smallest pull is over the mob cap (no guarantee comes)
    std::string _planVerdictWhy;        // the refusal in words, for the harness's skip reason
    uint32 _planVerdictGuaranteeWaitMs = 0;  // a refusal: how long until the guarantee takes it
    GuidVector _planLookPack;  // the mobs the last look was about (the target, its pack once counted)
    // The last slow-path refusal in words (the hard minimum runs once per stall period; the looks between keep it).
    ObjectGuid _refusalFor;
    std::string _refusalWhy;
    bool _refusalHopeless = false;
    uint32 _refusalMs = 0;
    void NotePackBlocked(ObjectGuid mob, uint32 now);
    uint32 PackBlockedMs(std::vector<ObjectGuid> const& mobs, uint32 now) const;
    ObjectGuid _bossHoldFor;       // the boss whose own fight (BossDrawnIn) holds the trash pull, since _bossHoldMs
    uint32 _bossHoldMs = 0;
    uint32 _bossHoldSeenMs = 0;
    float _lastResortCap = 0.0f;   // the hard cap in force for the last plan (escalated or not)
    // The last plan's verdict for CarefulPlanVerdict (None: held for another reason), the mobs it is about and how
    // many the accepted plan expects.
    enum class PlanVerdictState : uint8
    {
        None,
        Accepted,
        Refused
    };
    PlanVerdictState _planVerdict = PlanVerdictState::None;
    uint32 _planVerdictMobs = 0;
    GuidVector _planVerdictPack;
    // The fight the verdict is about (CarefulPlanVerdict::Fight, for the rest logic): the accepted plan's estimate,
    // or the smallest plan of a refused pack.
    uint32 _planVerdictFightMobs = 0;
    float _planVerdictCost = 0.0f;
    float _planVerdictSec = 0.0f;
    bool _planVerdictBoss = false;
    // [rest_predict_real_pull] the plan this look chose before a readiness gate held it (the group not ready, a damage
    // dealer missing, a crossing group, a boss window): the pack about to be pulled, the rest logic's next fight.
    bool _planChosenValid = false;
    uint32 _planChosenMobs = 0;
    float _planChosenCost = 0.0f;
    float _planChosenSec = 0.0f;
    bool _planChosenBoss = false;
    bool _planChosenRealBoss = false;
    uint8 _planBlockerDepth = 0;  // [plan_route_blocker] PlanCarefulPull runs for a route target's blocker
    bool _planPatrol = false;    // the last plan takes a patrol first, before it walks into the group or the fight
    ObjectGuid _planKillFor;     // a pull back of this mob is fought at _planKill (its call for help reaches nobody)
    Position _planKill;
    ObjectGuid _planFightFor;    // the fight with this mob moves back (a mover walks into it): see FightSpot
    Position _planFight;         // an engage: where the tank walks the fight back to once it is on
    float _planFightBack = 0.0f;
    std::string _planFightNote;  // which mover ("[fight spot]")
    ObjectGuid _patrolHoldFor;   // the patrol (its walker) the tank holds its pull for, since _patrolHoldMs
    uint32 _patrolHoldMs = 0;
    uint32 _patrolHoldSeenMs = 0;
    uint32 _planMobs = 0;        // the careful plan's count and cost, for the pull check (0: not a careful plan)
    float _planCost = 0.0f;
    float _planDrop = 0.0f;
    uint32 _buffCheckMs = 0;
    uint32 _buffWaitMs = 0;      // since when a buff has been missing
    std::string _buffMissing;
    bool _planSide = false;      // the last plan walks to _sideSpot first, a side of the pack that brings fewer mobs
    Position _sideSpot;
    ObjectGuid _sideFor;         // the side walk's target and its note for the pull check
    uint32 _sideMs = 0;
    std::string _sideNote;
    ObjectGuid _sideFailFor;     // no better side around this mob a moment ago
    uint32 _sideFailMs = 0;
    ObjectGuid _approach;        // set by isUseful for the Execute that follows: walk toward it
    uint32 _noTargetMs = 0;      // since when the tank has had no pick, the group topped up
    ObjectGuid _stallFor;        // the pack refused with the group topped up, since _stallMs
    uint32 _stallMs = 0;
    uint32 _loneWaitMs = 0;      // since when the lone-mob minimum has waited for a single-target ranged pull
    uint32 _loneSeenMs = 0;      // its last look (a wait not looked at for a while is from an earlier stand)
    uint32 _quickOneMs = 0;      // the last try of the quick lone mob (CAREFUL_READY_ONE_MS)
    // A group that crosses the planned pack now (user rule: packs stand apart at spawn): the tank waits until it has
    // walked off (its mob, where the side that stays stands, and the radius that would bring it).
    struct OverlapMemo
    {
        ObjectGuid outside;
        ObjectGuid mover;
        Position anchor;
        float reach = 0.0f;
    };
    std::vector<OverlapMemo> _overlapMemo;  // the crossing groups of the last overlap hold
    uint32 _overlapHoldMs = 0;              // since when the tank holds for crossing groups (0: no hold)
    uint32 _overlapSeenMs = 0;
    uint32 _overlapGiveUpMs = 0;            // the wait ran out: planned as before (merged) for a while
    ObjectGuid _overlapBinding;             // the crossing group the hold clock runs for (kept across target flips)
    // [b36_sv] overlap holds started with no fight between them (a looping patrol re-crosses: the clock restarts on
    // every walk-off); past OVERLAP_MAX_CYCLES the patrol is pulled itself, else the wait gives up (planned merged)
    uint32 _overlapCycles = 0;
    uint32 _overlapCycleMs = 0;             // the last hold's start
    uint32 _overlapCycleFightMs = 0;        // PullCheck::LastFightMs then (a fight since: the count starts anew)
    ObjectGuid _ccSkipFor;       // HoldForPrePullCc found nothing to hold around this mob, a moment ago
    ObjectGuid _ccSplitFor;      // HoldForPrePullCc found the accepted plan splits this mob's pack, a moment ago
    uint32 _ccSplitMs = 0;
    uint32 _ccSkipMs = 0;
    uint32 _ccRefusedPlanId = 0;  // the pre-pull crowd control plan registered for a refused verdict (0: none)
    ObjectGuid _lastPullBack;  // a ranged pull that left its mob idle is not tried again on it for a while
    uint32 _lastPullBackMs = 0;
    std::unordered_map<ObjectGuid, uint32> _packWhyMs;  // "[pack why]": its last line per candidate
    Position _losSpot;
    Position _losGather;
    GuidVector _losPack;
    std::vector<LosPullMemo> _losMemo;  // recent searches, so a resting group does not search every second
    ObjectGuid _losTempoFor;            // tempo mode: the decision for this target, reused for a second
    uint32 _losTempoMs = 0;
    bool _losTempoResult = false;
};

class AttackAnythingAction : public AttackAction
{
public:
    AttackAnythingAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack anything") {}

    std::string const GetTargetName() override { return "grind target"; }
    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;
};

class AttackLeastHpTargetAction : public AttackAction
{
public:
    AttackLeastHpTargetAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack least hp target") {}

    std::string const GetTargetName() override { return "least hp target"; }
};

class AttackEnemyPlayerAction : public AttackAction
{
public:
    AttackEnemyPlayerAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack enemy player") {}

    std::string const GetTargetName() override { return "enemy player target"; }
    bool isUseful() override;
};

class AttackRtiTargetAction : public AttackAction
{
public:
    AttackRtiTargetAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack rti target") {}

    std::string const GetTargetName() override { return "rti target"; }
    bool Execute(Event event) override;
    bool isUseful() override;
};

class AttackEnemyFlagCarrierAction : public AttackAction
{
public:
    AttackEnemyFlagCarrierAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack enemy flag carrier") {}

    std::string const GetTargetName() override { return "enemy flag carrier"; }
    bool isUseful() override;
};

class DropTargetAction : public Action
{
public:
    DropTargetAction(PlayerbotAI* botAI) : Action(botAI, "drop target") {}

    bool Execute(Event event) override;
};

class TargetRequesterAction : public Action
{
public:
    TargetRequesterAction(PlayerbotAI* botAI) : Action(botAI, "target me") {}

    bool Execute(Event event) override;
};

#endif
