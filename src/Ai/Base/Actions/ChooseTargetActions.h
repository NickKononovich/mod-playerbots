/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CHOOSETARGETACTIONS_H
#define PLAYERBOTS_CHOOSETARGETACTIONS_H

#include "AttackAction.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class Player;
class PlayerbotAI;
class Unit;

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
    bool GroupPreparing(Player* bot, std::string* why = nullptr);
    // [boss_pull_ready_gate] What keeps the group from being ready for `boss` (the planner's BossReadyGap: health,
    // mana, sitting, buffing, a missing group buff); empty when ready.
    std::string BossNotReady(Player* bot, Unit* boss);
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

    std::string const GetTargetName() override { return "aggressive target"; }
    bool Execute(Event event) override;
    bool isUseful() override;

private:
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
