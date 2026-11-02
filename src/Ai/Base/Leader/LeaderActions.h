/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADERACTIONS_H
#define PLAYERBOTS_LEADERACTIONS_H

#include "Action.h"
#include "AttackAction.h"  // [lead15] the leader's move / attack events (MovementAction, AttackAction)
#include "ObjectGuid.h"
#include "UseItemAction.h"

class PlayerbotAI;
namespace Leader
{
struct MemberEvents;
}

class LeaderReadyCheckAction : public Action
{
public:
    LeaderReadyCheckAction(PlayerbotAI* botAI) : Action(botAI, "leader ready check") {}

    bool Execute(Event event) override;
};

// Counts the answers: ready -> "pull in 3"; not ready -> names and reasons, and a new check a little later.
class LeaderReadyCheckResultAction : public Action
{
public:
    LeaderReadyCheckResultAction(PlayerbotAI* botAI) : Action(botAI, "leader ready check result") {}

    bool Execute(Event event) override;
};

// The single voice of the fight: pull, burn phase, Bloodlust, battle res, boss down and the wipe call.
class LeaderEncounterEventAction : public Action
{
public:
    LeaderEncounterEventAction(PlayerbotAI* botAI) : Action(botAI, "leader encounter event") {}

    bool Execute(Event event) override;
};

// Member: the leader's math says the fight is lost - drop out of it (Vanish, Feign Death, Invisibility, Shadowmeld,
// or a paladin's Divine Intervention on a resurrecter) to resurrect the group on the spot after it.
class WipeEscapeAction : public Action
{
public:
    WipeEscapeAction(PlayerbotAI* botAI) : Action(botAI, "wipe escape") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// Member: the escape is over (the fight is gone, or the bot was found again); it stands up and acts again.
class WipeEscapeEndAction : public Action
{
public:
    WipeEscapeEndAction(PlayerbotAI* botAI) : Action(botAI, "wipe escape end") {}

    bool Execute(Event event) override;
};

class LeaderAssignTanksAction : public Action
{
public:
    LeaderAssignTanksAction(PlayerbotAI* botAI) : Action(botAI, "leader assign tanks") {}

    bool Execute(Event event) override;

private:
    ObjectGuid _announcedTank;
    ObjectGuid _announcedAssist;
};

// Orders a raid leader gives in group chat (the BotControl menu sends them); none of them is a cheat.

// "pause": the group finishes the fight in progress, then holds its position and pulls nothing (RaidLeader::IsPaused).
class GroupPauseAction : public Action
{
public:
    GroupPauseAction(PlayerbotAI* botAI) : Action(botAI, "group pause") {}

    bool Execute(Event event) override;
    bool isUsefulWithoutControl() override { return true; }
};

// "resume": the break is over.
class GroupResumeAction : public Action
{
public:
    GroupResumeAction(PlayerbotAI* botAI) : Action(botAI, "group resume") {}

    bool Execute(Event event) override;
    bool isUsefulWithoutControl() override { return true; }
};

// "rest": eat and drink from the bags (whatever is missing of health and mana); moving or a fight ends it.
class RestChatAction : public UseItemAction
{
public:
    RestChatAction(PlayerbotAI* botAI) : UseItemAction(botAI, "rest") {}

    bool Execute(Event event) override;
    bool isPossible() override { return true; }
};

// "interrupt": the one member with the best ready interrupt kicks the cast of the leader's target now.
class InterruptOrderAction : public Action
{
public:
    InterruptOrderAction(PlayerbotAI* botAI) : Action(botAI, "interrupt order") {}

    bool Execute(Event event) override;
};

// "res [me|<name>]": that member is resurrected first (PartyMemberToResurrect), then the usual order.
class ResurrectFirstAction : public Action
{
public:
    ResurrectFirstAction(PlayerbotAI* botAI) : Action(botAI, "resurrect first") {}

    bool Execute(Event event) override;
    bool isUsefulWithoutControl() override { return true; }
};

// "burst [seconds]": cooldowns now (the burst window of the boost triggers) and the group's Bloodlust/Heroism,
// for a limited time; nothing is written to the saved strategies. "burst end" closes the window.
class BurstOrderAction : public Action
{
public:
    BurstOrderAction(PlayerbotAI* botAI) : Action(botAI, "burst order") {}

    bool Execute(Event event) override;
};

class BurstEndAction : public Action
{
public:
    BurstEndAction(PlayerbotAI* botAI) : Action(botAI, "burst end") {}

    bool Execute(Event event) override;
};

// [lead15] Executes the dungeon leader's MoveTo / StandAt event (LeaderOrders.h Events): the member walks to the spot
// the leader chose (a dodge out of a cleave, its spot before a pull); a sitter stands up for it.
class LeaderMoveOrderAction : public MovementAction
{
public:
    LeaderMoveOrderAction(PlayerbotAI* botAI) : MovementAction(botAI, "leader move order") {}

    bool Execute(Event event) override;
    // [lead19] the leader's checkedMove is the one safety check of a led member's leader walk: the member's own idle
    // path guard and fight-zone clamp do not veto it (a closed door still does: no walk through a wall)
    bool SkipsIdleGuard() override;
    // [lead19] what the member-side guards would refuse on the walk to `to` ("" = nothing): an idle mob it would wake
    // (IdleWakeOnWalk), a closed door on the path, no path. The leader calls it before it issues (the stricter model)
    std::string WalkRefusal(Position const& to);
    // [lead101] the idle mob the walk to `to` (the leader's route search) wakes, `exempt` not judged (null: none)
    Unit* WalkWakes(Position const& to, ObjectGuid exempt);
    // [lead71] the member's own MoveTo guards run dry (MovementAction::s_dryRun) for a walk to `to`: would it go?
    // [lead373] judged as the move `why` the leader is about to issue (its guards, not the current command's)
    bool DryMoveTo(Position const& to, std::string const& why);
    // [lead374] the member's follow / chase place behind `target` at place `index` (the executor's own computation)
    bool FollowPlace(Unit* target, uint8 index, float& x, float& y, float& z);
    // [lead374] the leader's check of a Follow before it issues it: the member's walk to its place, its own guards
    // run dry - "" when it would walk there (or needs no walk), else the refusal the member would report, and the
    // idle mob its walk would wake in `mob`
    // [lead491] `chase`: the leader's melee chase of its fight target - the idle reach is the leader's melee test
    // (LeaderAggroOnWalk on the target: no deeper into an idle mob's reach than the fight there), not its guard
    std::string DryFollow(Unit* target, uint8 index, ObjectGuid& mob, bool chase = false);

private:
    // [lead372] the walk to `spot` cannot reach it safely: the member stays, the leader reads the report
    void ReportRefusal(Leader::MemberEvents const& events, Position const& spot, std::string const& reason,
                       ObjectGuid mob);

    uint32 _attackFirstSeq = 0;  // [lead281] the command (seq) whose unstarted Attack this walk let run first
    std::string const* _dryWhy = nullptr;  // [lead373] the move DryMoveTo judges
    bool _dryFollow = false;               // [lead374] DryFollow runs
    bool _dryChase = false;                // [lead491] ... for the leader's melee chase
    // [lead370] the walk DryFollow judged clear (the target, where the member stood, the place): the executor's walk
    // with these inputs is that walk - the leader's check is final, its guard is not run a second time
    struct JudgedFollow
    {
        bool valid = false;
        ObjectGuid target;
        Position from;
        Position place;
        bool chase = false;  // [lead491] the leader's melee chase (DryFollow, chase)
    } _judgedFollow;
    bool _judgedWalk = false;  // [lead370] the executor walks a judged follow now
};

// [lead76] Executes the leader's "top up X": the healer's heal on the named member now.
class LeaderHealTargetAction : public Action
{
public:
    LeaderHealTargetAction(PlayerbotAI* botAI) : Action(botAI, "leader heal target") {}

    bool Execute(Event event) override;
};

// [lead15] Executes the leader's Attack event: attacks the leader's target (the pull's first mob) at once.
class LeaderAttackOrderAction : public AttackAction
{
public:
    LeaderAttackOrderAction(PlayerbotAI* botAI) : AttackAction(botAI, "leader attack order") {}

    bool Execute(Event event) override;
};

#endif
