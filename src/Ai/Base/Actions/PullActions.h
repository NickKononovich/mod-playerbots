/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

/*
 * Ported from the CMaNGOS playerbots project (https://github.com/cmangos/playerbots), GPL v2,
 * with modifications for AzerothCore.
 * Original authors:
 *   David Parra Ausina (davidonete/Flekz) <davidparraausina@gmail.com> - original author
 *   Sebastiaan Keek (mostlikely4r) <sebastiaan.keek@gmail.com>
 *   Cyberium <cyberium@users.noreply.github.com>
 */

#ifndef PLAYERBOTS_PULLACTIONS_H
#define PLAYERBOTS_PULLACTIONS_H

#include "GenericSpellActions.h"
#include "ReachTargetActions.h"
#include "Trigger.h"

class PullRequestAction : public Action
{
public:
    PullRequestAction(PlayerbotAI* botAI, std::string const name) : Action(botAI, name) {}

    bool Execute(Event event) override;

protected:
    virtual Unit* GetPullTarget(Event event) = 0;
};

class PullMyTargetAction : public PullRequestAction
{
public:
    PullMyTargetAction(PlayerbotAI* botAI) : PullRequestAction(botAI, "pull my target") {}

private:
    Unit* GetPullTarget(Event event) override;
};

class PullRtiTargetAction : public PullRequestAction
{
public:
    PullRtiTargetAction(PlayerbotAI* botAI) : PullRequestAction(botAI, "pull rti target") {}

private:
    Unit* GetPullTarget(Event event) override;
};

class PullStartAction : public Action
{
public:
    PullStartAction(PlayerbotAI* botAI, std::string const name = "pull start") : Action(botAI, name) {}

    bool Execute(Event event) override;
};

class PullAction : public CastSpellAction
{
public:
    PullAction(PlayerbotAI* botAI, std::string const name = "pull action");

    bool Execute(Event event) override;
    bool isPossible() override;
    bool isUseful() override;
    std::vector<NextAction> getPrerequisites() override;
    Unit* GetTarget() override;

private:
    void InitPullAction();

    // [pull_out_of_range_loop] the target this pull has found out of range since `_outOfRangeSinceMs`, and the
    // closest the tank came to it meanwhile (a walk that makes no progress ends the pull)
    ObjectGuid _outOfRangeFor;
    uint32 _outOfRangeSinceMs = 0;
    float _outOfRangeBest = 0.0f;
    // [b32_smpull] consecutive out-of-range give-ups of `_giveUpFor` from about (`_giveUpX`, `_giveUpY`), the last at
    // `_giveUpMs`: the third makes the pull "out of reach" for the planner (CarefulPlanVerdict::NotePullOutOfReach)
    ObjectGuid _giveUpFor;
    uint32 _giveUpCount = 0;
    uint32 _giveUpMs = 0;
    float _giveUpX = 0.0f;
    float _giveUpY = 0.0f;
};

class PullEndAction : public Action
{
public:
    PullEndAction(PlayerbotAI* botAI, std::string const name = "pull end") : Action(botAI, name) {}

    bool Execute(Event event) override;
};

class ReachPullAction : public ReachTargetAction
{
public:
    ReachPullAction(PlayerbotAI* botAI) : ReachTargetAction(botAI, "reach pull", botAI->GetRange("spell")) {}

    bool Execute(Event event) override;
    bool isUseful() override;
    Unit* GetTarget() override;

private:
    // [w24b-pull-slope] the last walk check of an out-of-sight target below or above, and the log throttle
    ObjectGuid _slopeTarget;
    uint32 _slopeCheckMs = 0;
    float _slopeWalk = 0.0f;
    bool _slopeOk = false;
    uint32 _floorLogMs = 0;
    // [b34_wc] the last check of the careful plan's cast spot (is it on the tank's floor, on the way in?)
    ObjectGuid _castCheckTarget;
    Position _castCheckSpot;
    uint32 _castCheckMs = 0;
    bool _castCheckBad = false;
};

class ReturnToPullPositionAction : public MovementAction
{
public:
    ReturnToPullPositionAction(PlayerbotAI* botAI) : MovementAction(botAI, "return to pull position") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// [chain_pull_in_combat] While the group finishes the last mob of a fight ("chain pull" trigger), the tank casts its
// ranged pull at the next idle single or small pack within CHAIN_PULL_RANGE without leaving the mob it fights: no
// boss, at most CHAIN_PULL_MAX_MOBS, every member present and above its rest lines (RestState), one per cooldown.
class ChainPullAction : public Action
{
public:
    ChainPullAction(PlayerbotAI* botAI) : Action(botAI, "chain pull") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    // The next mob to chain (nullptr: none), the pull spell for it and how many mobs come.
    Unit* FindNext(Unit* fighting, std::string& spellName, uint32& packMobs);
    bool GroupAboveRestLines(std::string& below);

    uint32 _lastChainMs = 0;
};

// A group member walks to the group spot of its tank's line-of-sight pull (LosPull in PullStrategy.h).
class MoveToLosPullSpotAction : public MovementAction
{
public:
    MoveToLosPullSpotAction(PlayerbotAI* botAI) : MovementAction(botAI, "move to los pull spot") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// A tank in a fight that a mob walking about (a wanderer, a patrol) can walk into: it walks the fight back to a spot
// none reaches, once per fight (FightSpot in ChooseTargetActions.h; "[fight spot] <tank> moves the fight back").
class FightSpotBackTrigger : public Trigger
{
public:
    FightSpotBackTrigger(PlayerbotAI* botAI) : Trigger(botAI, "fight spot back") {}

    bool IsActive() override;
};

class MoveFightSpotBackAction : public MovementAction
{
public:
    MoveFightSpotBackAction(PlayerbotAI* botAI) : MovementAction(botAI, "move fight spot back") {}

    bool Execute(Event event) override;
    bool isUseful() override;
    // [fight_spot_back_guard] plan29 follow-up 4: the walk back to the fight spot never wakes another idle pack
    // (build 29: 67-78 pulls "unplanned (tank's move fight spot back)"); the idle-mob path guard judges it as it
    // judges the tank's other chase walks, and a walk it refuses leaves the fight where the tank stands.
    bool GuardsTankWalk() override { return true; }
    bool GuardsIdleTankWalk() override { return true; }
};

// [drift_fight_hold] scholo-h 20261025-150737-4 pull 6: the tank chased loose mobs 44-61 yd from the pull spot
// ("move to group member attackers", "tank assist" -> "reach melee" 27 yd, "reach healer attacker"), attackers went
// 7 -> 46, the healer left behind died. In a 5-man trash fight the tank holds a fight spot: the pull spot (or the
// spot the fight was walked back / round a corner to) and RADIUS round it. Its chase walks end at that edge
// (MovementAction::HoldFightSpot); a loose mob is taken with a taunt or a ranged threat spell and comes to him.
namespace FightHold
{
    constexpr float RADIUS = 15.0f;
    constexpr float MELEE_SLACK = 8.0f;  // a melee damage dealer walks no further than RADIUS + this from the spot
    // The tank's combat tick (FightSpotBackTrigger): starts the anchor of a new fight, keeps it alive.
    void Tick(PlayerbotAI* botAI, Player* tank);
    // The fight spot of `tank` now; false when no hold applies (no fight on, a boss fight, not a 5-man dungeon).
    bool Get(Player* tank, Position& anchor);
    // The fight moves to a spot chosen for it (walked back from movers, round a corner): the hold moves with it.
    void Move(Player* tank, Position const& spot);
}

#endif
