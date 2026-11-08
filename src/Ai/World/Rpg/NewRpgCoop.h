/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGCOOP_H
#define PLAYERBOTS_NEWRPGCOOP_H

// [coop] user design 2026-11-08: co-op questing of the guild bots. Bots of the guild plan (GuildCoordinator::
// IsPlanned) that work the same quest objective (the same creature to kill, the same item to loot) in the same zone,
// or one whose objective lies in a camp it cannot solo, form a real core party when the computed time together beats
// the time alone, the partner's walk included. The core shares the kill credit (Player::RewardPlayerAndGroupAtKill),
// gives each member its own quest drops and splits the kill XP (Acore::XP::xp_in_group_rate).
// - The party's leader is computed: the tank, else the highest level (the lowest guid on a tie). It offers, runs its
// own
//   New RPG plan and judges every pull for the party (JudgePull); the members stand their own plan down and go with it.
// - A member whose counter closes leaves at once; the one that needs fewer kills never waits for the other.
// - Only a bot on the quest default of the global leader (New RPG DO_QUEST, no profession call) is offered; a later
//   order of the global leader that moves the bot off that quest (a five's call: its status changes) ends its co-op.
// - No timers: the offer is looked at when the board of its objective changes (a counter, a bot coming or going).
// Log: [coop] offer|accept|refuse|join|wait|shared-kill|exit|gain|seek <bot> k=v ... t=<unix ms> [reason=<text>]
// (C:/WoW/tools/quest_metrics.py "groups").

#include <functional>
#include <string>

#include "Define.h"
#include "Multiplier.h"
#include "SoloSafety.h"
#include "TravelMgr.h"

class Player;
class PlayerbotAI;
class Unit;

namespace NewRpgCoop
{
// what a party member does this tick instead of its own plan
struct Order
{
    enum class Kind : uint8
    {
        None,   // not a member: its own plan
        Stay,   // a member within reach of its leader: loot and fights run, its own plan waits
        Follow  // a member out of reach: it goes to its leader
    };
    Kind kind = Kind::None;
    Player* leader = nullptr;  // this tick only
    WorldPosition pos{};       // the leader's spot (Follow beyond sight: the far walk)
    bool farWalk = false;
};

// map thread, the New RPG status tick: the bot's board entry (O(1)), the offer when the board of its objective
// changed, the party's exits; for a member the leader's word
Order Tick(PlayerbotAI* botAI, Player* bot);
// the member found no walk to its leader: it leaves the party
void NoWalk(Player* bot);
// MoveFarTo refused the walk to the bot's quest objective (a camp it does not pull): the objective is put on the
// board as one it looks for a party for
void OnSpotRefused(PlayerbotAI* botAI, Player* bot, uint32 questId, int32 objectiveIdx, WorldPosition const& dest);

// a formed party's member that is not its leader (its own plan stands down)
bool IsMember(Player* bot);
// a formed party's leader: it judges its pulls for the party
bool JudgesPulls(Player* bot);
// the leader's pull of `target` for the party: the pack's damage on the leader over the time the party's damage
// needs, against the leader's health and the party's heals (as SoloSafety::Judge does for one bot)
SoloSafety::Verdict JudgePull(Player* leader, Unit* target);
// the judge wants the party rested before the pull: every member below full rests
void RestParty(Player* leader, std::string const& why);
// the bot is in a co-op party (its group is a co-op one; the global leader may take it out). Its own lock, never held
// while waiting on another: callable with GuildCoordinator::lock_ held
bool InParty(uint32 low);

// the global leader's side of co-op, set once by GuildCoordinator at its Load (before any bot ticks); an unset member
// falls back to what its current API shows and to [tasks] log lines.
struct CoordinatorHooks
{
    // map thread: the bot runs, or must start now, an URGENT task (why: its name). Called with the board's lock held
    // (lock order: board, then GuildCoordinator::lock_; under lock_ the coordinator may call only InParty)
    std::function<bool(Player* bot, std::string* why)> urgentTask;
    // world thread, without the board's lock: the co-op task enters the bot's task list (priority between URGENT and
    // CONVENIENT, place FIELD); `leads`: it is the party's leader
    std::function<void(uint32 low, uint32 partnerLow, uint32 questId, bool leads, std::string const& why)> addTask;
    // map / world thread, without the board's lock: the co-op task ends (its exit reason)
    std::function<void(uint32 low, uint32 questId, std::string const& why)> endTask;
};
void SetCoordinatorHooks(CoordinatorHooks const& coordinatorHooks);
}  // namespace NewRpgCoop

// drops the member's own New RPG status actions and its own grind pulls while it goes with its party's leader
class NewRpgCoopMultiplier : public Multiplier
{
public:
    NewRpgCoopMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "new rpg coop") {}

    float GetValue(Action* action) override;

private:
    uint32 _checkedAt{0};
    bool _checked{false};
    bool _member{false};
};

void AddNewRpgCoopScripts();

#endif
