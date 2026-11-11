/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

/*
 * Ported from the CMaNGOS playerbots project (https://github.com/cmangos/playerbots), GPL v2,
 * with modifications for AzerothCore.
 * Original authors:
 *   ike3 <ike@email.org> - original author
 *   Sebastiaan Keek (mostlikely4r) <sebastiaan.keek@gmail.com>
 *   David Parra Ausina (Flekz) <davidparraausina@gmail.com>
 */

#ifndef PLAYERBOTS_PULLSTRATEGY_H
#define PLAYERBOTS_PULLSTRATEGY_H

#include "ObjectGuid.h"
#include "Position.h"
#include "Strategy.h"

#include <string>
#include <vector>

class Action;
class Multiplier;
class Player;
class SpellInfo;
class Unit;

class PlayerbotAI;

// Line-of-sight pull of a caster pack (planned by AggressiveTargetAction): the tank casts its ranged pull, then
// steps to a spot none of the pack can see, so the casters have to walk round the corner into melee instead of
// casting from where they stand. The group waits a few yards behind that spot, out of the pack's sight too, and
// holds there until the pack has come round. One plan per group, shared by every member (one registry, like the
// interrupt claims in InterruptValues).
namespace LosPull
{
    constexpr uint32 GATHER_WAIT_MS = 8000;   // the tank waits this long at most for the group before it pulls
    constexpr float GATHER_RADIUS = 4.0f;     // a member this close to the group spot has arrived
    constexpr float GATHER_STAND = 2.5f;      // a member farther than this walks back to the group spot

    struct Plan
    {
        uint32 id = 0;
        uint32 mapId = 0;
        uint32 instanceId = 0;
        ObjectGuid tank;
        ObjectGuid target;
        GuidVector pack;
        Position spot;            // where the tank fights, out of the pack's sight
        Position gather;          // where the rest of the group waits, behind the spot
        uint32 plannedMs = 0;
        uint32 waitMs = 0;        // the tank is in range of its pull and waits for the group
        uint32 castMs = 0;        // the pack was pulled
        // [b32_engage] the first pull of this pack in this fight: the hold is capped from here, however often the
        // tank plans the pull again
        uint32 holdStartMs = 0;
        uint32 evalMs = 0;
        bool gathered = false;    // every member stands at the group spot
        bool arrived = false;     // a mob of the pack reached the tank in melee
        bool released = false;    // the pack is at the spot or sees it: the fight goes on as usual
    };

    void Register(Player* tank, Unit* target, GuidVector const& pack, Position const& spot, Position const& gather);

    // The live plan of the bot's group, brought up to date (at most every few hundred ms per group); false when
    // the group has none, or it is over.
    bool Get(Player* bot, Plan& plan);

    // The group holds its spots: before the pull, and after it until the pack has come round the corner.
    bool IsHolding(Player* bot);

    // [b32_engage] The plan holds this bot: a melee dps is let go once a mob of the pack is in the tank's melee.
    bool Holds(Player* bot, Plan const& plan);

    // The tank is in range of its pull and waits; returns how long it has waited.
    uint32 BeginWait(Player* tank);

    void MarkCast(Player* tank);

    // The pull ended before the tank cast it: the group goes back to following.
    void Cancel(Player* tank);
}

// Crowd control before a hard trash pull (Playerbots.AutoCcPrePull): the tank, about to pull a pack worth it, names
// the pack in its kill order (AggressiveTargetAction). The group's crowd control bots each claim one mob from the
// tail of that order, never the first two (the group kills those now), and put their icon on it; a Sap or a Freezing
// Trap goes out before the pull, a Polymorph or a Hex lands as the pack comes. The tank holds its pull a moment for
// the claims and until the Sap or the trap is placed. One plan per group, shared by every member (like LosPull).
namespace PrePullCc
{
    constexpr uint32 CLAIM_WINDOW_MS = 1500;  // the tank waits this long for every crowd control bot to answer
    constexpr uint32 MAX_WAIT_MS = 12000;     // and this long at most for a Sap or a trap to be placed
    // [prepull_cc_claim_drop] STN92_EFFICIENCY 2026-10-25 (st-n-92: "pulls after 17668 ms: 0 of 3 claims placed",
    // 77 s of crowd control waits for 3 of 21 claims): a claim not placed this long after it was made is dropped
    // (failed) and holds the pull no longer - the pull goes on without it.
    constexpr uint32 CLAIM_PLACE_MS = 3000;
    constexpr uint8 KILL_FIRST = 2;           // the head of the kill order (Skull, Cross) is never claimed

    struct Claim
    {
        ObjectGuid bot;
        ObjectGuid target;   // empty: the bot has nothing to hold in this pack
        uint32 claimedMs = 0;
        bool ready = false;  // nothing left to do before the pull (a Sap landed, a trap is down, a cast waits)
        bool failed = false;
        // [split250] the leader's order (RegisterAssigned): its planner priced the pack with this mob held, so the
        // pull waits for it (no CLAIM_PLACE_MS drop) and its failure re-plans the pack (HasFailedAssigned)
        bool assigned = false;
        std::string spell;      // the assigned hold
        bool breakable = true;  // damage breaks the assigned hold (all but Banish)
    };

    struct Plan
    {
        uint32 id = 0;
        uint32 mapId = 0;
        uint32 instanceId = 0;
        ObjectGuid tank;
        ObjectGuid pullTarget;
        GuidVector pack;         // kill order, the pull target first
        GuidVector controllers;  // members that may claim
        std::vector<Claim> claims;
        uint32 plannedMs = 0;
        uint32 pulledMs = 0;
        bool heavy = false;      // the careful planner refused the pack as it stands
    };

    // The tank's plan for this pull target, if any (no copy of the pack).
    bool HasPlan(Player* tank, ObjectGuid pullTarget);

    // Registers the tank's plan (or moves it onto `pullTarget` when that mob is of the same pack). `reopen`: the
    // tank's verdict on the pack turned from refused to accepted - the plan takes the new `heavy`, the members that
    // declined or failed under the refusal answer again (their claims are dropped), the live claims stand, and the
    // claim window starts over.
    void Register(Player* tank, Unit* pullTarget, GuidVector const& pack, GuidVector const& controllers, bool heavy,
                  bool reopen = false);

    // [split250] The leader's crowd control plan: its planner chose who holds which mob of the pack (the pack priced
    // without them) and the pull target among the rest. Replaces the group's plan; each assignment is a claim of its
    // member from now (the mark and the placing are the member's, PrePullCcAction).
    struct Assignment
    {
        ObjectGuid bot;
        ObjectGuid target;
        std::string spell;
        bool breakable = true;
    };
    void RegisterAssigned(Player* tank, Unit* pullTarget, GuidVector const& pack,
                          std::vector<Assignment> const& assignments, std::string const& why);

    // [split250] An assignment of the tank's live plan failed (its member could not place it): the pack is priced
    // again without that hold. `member` / `target`: the first failed one.
    bool HasFailedAssigned(Player* tank, ObjectGuid* member = nullptr, ObjectGuid* target = nullptr);

    // The tank holds its pull now: the claim window is open, or a claimed Sap or trap is still on its way.
    bool IsHolding(Player* tank);

    // The tank pulls: no more holding; the plan stays a while for the members' marks.
    void MarkPulled(Player* tank);

    // The tank pulls a single mob instead (the lone-mob minimum): the plan is dropped, nobody controls the mobs that
    // stay, since a Polymorph / Trap on one of them brings its pack.
    void Cancel(Player* tank);

    // The plan of the bot's group on its map; false when there is none or it expired.
    bool Get(Player* bot, Plan& plan);

    // The mob is claimed by a member of the bot's group.
    bool IsClaimed(Player* bot, ObjectGuid target);

    // Claims `target` for `bot` (empty: declines). False when the plan changed, the mob is taken, or the plan has
    // `maxClaims` already.
    bool TryClaim(Player* bot, uint32 planId, ObjectGuid target, uint32 maxClaims, bool ready);

    void SetClaimState(Player* bot, uint32 planId, bool ready, bool failed);

    // [split250] The bot's claim waits for the tank's verdict (a Sap on a pack refused as it stands): its placing
    // time starts again (MAX_WAIT_MS counts the placing, not the wait).
    void TouchClaim(Player* bot, uint32 planId);
}

class PullStrategy : public Strategy
{
public:
    PullStrategy(PlayerbotAI* botAI, std::string const action, std::string const preAction = "");

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "pull"; }
    std::vector<NextAction> getDefaultActions() override;
    uint32 GetType() const override { return STRATEGY_TYPE_COMBAT | STRATEGY_TYPE_NONCOMBAT; }

    static PullStrategy* Get(PlayerbotAI* botAI);
    static uint8 GetMaxPullTime() { return 15; }

    time_t GetPullStartTime() const { return pullStartTime; }
    bool IsPullPendingToStart() const { return pendingToStart; }
    bool HasPullStarted() const { return pullStartTime > 0; }

    bool CanDoPullAction(Unit* target);
    Unit* GetTarget() const;
    bool HasTarget() const;

    virtual std::string GetPullActionName() const;
    // The pull action cast now: the class's choice, or the plain one when a lone mob must come and the choice
    // jumps to or hits others (Avenger's Shield).
    std::string GetCastActionName() const;
    std::string GetSpellName() const;
    float GetRange() const;
    virtual std::string GetPreActionName() const;
    // [b31_holyshield_pull] The tank's pre-pull buff: an action cast at the pull commit (PullAction::Execute, every
    // wait passed, in range, standing), one GCD before the pull spell, at most once per pull. Empty: none. The class
    // checks its own conditions (O(1)): the action's isUseful() decides whether it is cast at all.
    virtual std::string GetPrePullBuffActionName() const { return ""; }
    // [b32_tank_aoe] The tank's form for the walk to the pull (ReachPullAction, out of combat, before the first step):
    // a bear tank walked into body pulls in caster form after its out-of-combat buffs. Empty: none, or already in it.
    virtual std::string GetApproachFormActionName() const { return ""; }
    bool IsPrePullBuffDone() const { return prePullBuffDone; }
    void SetPrePullBuffDone() { prePullBuffDone = true; }

    // `single` (with `resetTime`): the pull must bring the mob alone; a re-request keeps the setting.
    void RequestPull(Unit* target, bool resetTime = true, bool single = false);
    bool IsSingleMob() const { return singleMob; }
    void SetSingleMob(bool single) { singleMob = single; }
    // The spell hits more than its target: it jumps (chain targets) or hits an area.
    static bool HitsMoreThanOne(SpellInfo const* spellInfo);
    void RestartPullTimer() { pullStartTime = time(nullptr); }
    void OnPullStarted();
    void OnPullEnded();

    ReactStates GetPetReactState() const { return petReactState; }
    void SetPetReactState(ReactStates reactState) { petReactState = reactState; }

private:
    void SetTarget(Unit* target);

private:
    std::string const action;
    std::string const preAction;
    bool pendingToStart = false;
    bool singleMob = false;
    bool prePullBuffDone = false;  // [b31_holyshield_pull] cleared by a new pull request and by the pull's end
    time_t pullStartTime = 0;
    ReactStates petReactState = REACT_DEFENSIVE;
};

class PullMultiplier : public Multiplier
{
public:
    PullMultiplier(PlayerbotAI* botAI);

    float GetValue(Action* action) override;

private:
    uint32 checkedMs = 0;  // the pull check (PullCheck::Sample) looks at the fight about once a second
};

class PossibleAddsStrategy : public Strategy
{
public:
    PossibleAddsStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "adds"; }
};

class PullBackStrategy : public Strategy
{
public:
    PullBackStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "pull back"; }
};

// Every group member's part in a line-of-sight pull (LosPull): walk to the group spot and hold there.
class LosPullStrategy : public Strategy
{
public:
    LosPullStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "los pull"; }
};

// [b32_engage] Out of combat a dungeon tank does not walk on (nor mounts to) while a member near it rests below its
// line or buffs (PullReadiness::GroupPreparing): no pull starts over a sitter. Capped per rest.
class TankWaitsForSittersMultiplier : public Multiplier
{
public:
    TankWaitsForSittersMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "tank waits for sitters") {}

    float GetValue(Action* action) override;

private:
    uint32 checkedMs = 0;
    uint32 holdStartMs = 0;
    bool holding = false;
};

// While the group holds a line-of-sight pull, nobody walks after the pack: no reach, follow, ranged position,
// formation, flank or peel moves. The tank's own pull moves, healing reach and AoE dodging stay.
class LosPullHoldMultiplier : public Multiplier
{
public:
    LosPullHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "los pull hold") {}

    float GetValue(Action* action) override;

private:
    uint32 checkedMs = 0;
    bool holding = false;
};

#endif
