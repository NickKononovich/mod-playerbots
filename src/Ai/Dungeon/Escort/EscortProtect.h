/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ESCORTPROTECT_H
#define PLAYERBOTS_ESCORTPROTECT_H

#include "Action.h"
#include "AttackAction.h"
#include "MovementActions.h"
#include "NamedObjectContext.h"
#include "SelfTravelActions.h"  // [b38_ohb] SelfTravel::RouteWalk
#include "Strategy.h"
#include "Trigger.h"
#include "Value.h"

class Creature;
class PlayerbotAI;
class Unit;

// Escort protection shared by the dungeons whose event is an escorted NPC the group must keep alive (table in
// EscortProtect.cpp, keyed by map and creature entry). Nothing is done to the NPC itself (no buffs, no scaling): the
// group plays the escort as players do.
// - the tank takes whatever hits the NPC: a taunt on the mob (Righteous Defense on the NPC for a paladin), then
//   attacks;
// - the damage dealers kill the NPC's attackers first;
// - healers heal the NPC once it is hurt (it is no group member, so the group heal logic never looks at it);
// - for a row with a leash (Old Hillsbrad's Thrall) the group walks with the NPC and comes back to it while it waits
//   at a checkpoint ("escort npc far" / "escort stay close").
// Gnomeregan: Blastmaster Emi Shortfuse 7998 (level 26, SmartAI 7998: trogg waves from the two tunnels while she
// blasts them shut; 2026-10-25 gnomer-h: she died in 24 of 24 runs 43-72 s after the start, nobody on her attackers).
namespace EscortProtect
{
// The escort table has a row for this map.
bool MapHasEscort(uint32 mapId);
// The escorted NPC of the bot's map near the bot (cached "escort npc" value), alive, or nullptr.
Creature* Escort(PlayerbotAI* botAI);
// The attacker of the escort the group takes first (the nearest to the NPC the bot may attack), or nullptr.
Unit* EscortAttacker(PlayerbotAI* botAI, Creature* escort);
// The escort the bot is too far from (its table row has a leash), or nullptr; stand gets the limit in force.
Creature* EscortLeash(PlayerbotAI* botAI, float& stand);
// [escort_anchor] The leashed escort (Thrall) under way near the bot, alive, or nullptr: while it exists the escort
// leads the group's movement, not the harness route nor the group leader. waiting: it stands at a checkpoint with
// its gossip up; waitReturn: its row's return distance for that wait. O(1) on maps without an escort row.
Creature* EscortAnchor(PlayerbotAI* botAI, bool& waiting, float& waitReturn);

// [b36_ohb] Escort pacing. ohb-h 20261026-034959-ohb-h-5H-17 1476-1483 s: Thrall (walking, no gossip to hold him)
// walked from waypoint 13 into the idle armory ambush (Durnholde Veteran / Mage / Sentry / Warden, summoned at his
// dressing and standing at ~2190,140) while the tank's planned "pull one ... Durnholde Sentry" sat at the pull gate
// ("healer Rtpriaf 10.0 yd from the tank, out of sight": the healer followed Thrall, the tank stood at its sight
// spot); "[escort heal] Rtpriaf: renew on Thrall" then put the four on the priest (dead in 4 s), the run wiped.
// Players pull what stands ahead of the NPC before he gets there (Thrall fights along), and do not heal him into a
// pack the tank has not picked up.
// The idle hostile creature nearest along the escort's way ahead (its script waypoints from the leg it walks or
// stands on, at most `lookahead` yd) whose proximity aggro reaches the NPC somewhere on that way
// (MovementAction::IdleAggroBlocker: z gate, range, sight), or nullptr; wayDist: how far along the way. Cached 2 s
// per escort. Uses no AI value: safe on the world thread (test harness).
Creature* EscortWayBlocker(Player* bot, Creature* escort, float lookahead, float* wayDist = nullptr);
// A leashed escort under way near the bot WALKS (no gossip up) into a fight: it fights already, or an idle pack
// stands within ESCORT_WALK_LOOKAHEAD of its way. The tank's pull gate does not wait for a healer out of sight then
// (the NPC does not wait), and its harness waypoint ahead of the NPC stands.
bool EscortWalksIntoPack(PlayerbotAI* botAI, std::string* why = nullptr);
// The gossip that sends a waiting leashed escort on (Thrall) waits while an idle pack stands on the next
// ESCORT_GOSSIP_LOOKAHEAD yd of its way (the planner / harness clear it meanwhile), at most ESCORT_GOSSIP_HOLD_MAX_MS
// per stop.
bool EscortGossipHeld(Player* bot, Creature* escort, std::string* why = nullptr);
// [b42_ohb] The escort a member's "follow" anchors on (EscortAnchor), or nullptr for the usual formation follow:
// while the NPC waits and the group leader, alive here, is not with it yet (farther than its return distance), a
// member follows the leader, who leads the walk back ("escort stay close" holds the member for the leader).
Creature* EscortFollowAnchor(PlayerbotAI* botAI);
}  // namespace EscortProtect

class EscortNpcValue : public ObjectGuidCalculatedValue
{
public:
    EscortNpcValue(PlayerbotAI* botAI) : ObjectGuidCalculatedValue(botAI, "escort npc", 1000) {}

protected:
    ObjectGuid Calculate() override;
};

// Tank or damage dealer: a mob hits the escorted NPC and is not the bot's target yet.
class EscortNpcAttackedTrigger : public Trigger
{
public:
    EscortNpcAttackedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "escort npc attacked") {}
    bool IsActive() override;
};

// Healer: the escorted NPC is hurt and in reach.
class EscortNpcLowHealthTrigger : public Trigger
{
public:
    EscortNpcLowHealthTrigger(PlayerbotAI* botAI) : Trigger(botAI, "escort npc low health") {}
    bool IsActive() override;
};

// [b36_ohb] Healer, out of combat: a leashed escort walks into a pack (EscortProtect::EscortWalksIntoPack) and the
// group's tank, near the NPC, is out of the healer's sight or reach.
class EscortHealerSightTrigger : public Trigger
{
public:
    EscortHealerSightTrigger(PlayerbotAI* botAI) : Trigger(botAI, "escort healer sight") {}
    bool IsActive() override;
};

class EscortHealerSightAction : public MovementAction
{
public:
    EscortHealerSightAction(PlayerbotAI* botAI) : MovementAction(botAI, "escort healer sight") {}
    bool Execute(Event event) override;

private:
    uint32 _logMs = 0;
};

// A leashed escort (Thrall) walks and the bot is too far from it, or it waits and the bot is out of combat far away.
class EscortNpcFarTrigger : public Trigger
{
public:
    EscortNpcFarTrigger(PlayerbotAI* botAI) : Trigger(botAI, "escort npc far") {}
    bool IsActive() override;
};

class EscortStayCloseAction : public MovementAction
{
public:
    EscortStayCloseAction(PlayerbotAI* botAI) : MovementAction(botAI, "escort stay close") {}
    bool Execute(Event event) override;

protected:
    // [b42_ohb] The tank's walk back to a waiting escort is judged by the idle-mob path guard (it stops at a pack's
    // aggro edge, the planner pulls the pack), as a member's walk always is.
    bool GuardsTankWalk() override { return WalksBackToWaitingEscort(); }
    bool GuardsIdleTankWalk() override { return WalksBackToWaitingEscort(); }

private:
    uint32 _logMs = 0;
    uint32 _groupWaitSinceMs = 0;  // [b32_pull] the return to a waiting escort waits for the group
    bool _walking = false;         // [b32_pull] the bot's move is this action's
    // [b38_ohb] The return walk to a waiting escort far off: by the complete navmesh route, leg by leg, when
    // PathGenerator's path is cut short (SelfTravel::NextRouteLeg, as DungeonEventStepAction), and a straight step
    // back onto the navmesh for a bot standing off it.
    SelfTravel::RouteWalk _route;
    bool _routeWalk = false;
    uint32 _pathCheckedMs = 0;
    uint32 _offMeshTriedMs = 0;
    uint32 _routeLogMs = 0;

    bool WalksBackToWaitingEscort();  // [b42_ohb]
};

class EscortProtectAction : public AttackAction
{
public:
    EscortProtectAction(PlayerbotAI* botAI) : AttackAction(botAI, "escort protect") {}
    bool Execute(Event event) override;
};

class EscortHealAction : public Action
{
public:
    EscortHealAction(PlayerbotAI* botAI) : Action(botAI, "escort heal") {}
    bool Execute(Event event) override;
};

// Active on the maps of the escort table (PlayerbotAI::ApplyInstanceStrategies), next to the map's own strategy.
class DungeonEscortStrategy : public Strategy
{
public:
    DungeonEscortStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "dungeon-escort"; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;

    // While the escort is hit, a damage dealer's "dps target" is one of its attackers: without the exclusion
    // "dps assist" switches back to the generic choice the tick after "escort protect" attacked.
    bool HasTargetExclusions() const override { return true; }
    void AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type) override;
};

// [w24b-escort-no-sibling] Its own strategy context without siblings: in DungeonStrategyContext (siblings on) the map
// strategy added after it (tbc-ohb, classic-gnomer) removed it again (Engine::addStrategy drops every sibling). ohb-h
// 2026-10-25 115805: not one [escort protect] / [escort heal] / [escort leash] line all session, Thrall died at the
// first ambush 9 times.
class DungeonEscortStrategyContext : public NamedObjectContext<Strategy>
{
public:
    DungeonEscortStrategyContext() : NamedObjectContext<Strategy>(false, false)
    {
        creators["dungeon-escort"] = &DungeonEscortStrategyContext::dungeon_escort;
    }

private:
    static Strategy* dungeon_escort(PlayerbotAI* botAI) { return new DungeonEscortStrategy(botAI); }
};

class DungeonEscortValueContext : public NamedObjectContext<UntypedValue>
{
public:
    DungeonEscortValueContext() { creators["escort npc"] = &DungeonEscortValueContext::escort_npc; }

private:
    static UntypedValue* escort_npc(PlayerbotAI* botAI) { return new EscortNpcValue(botAI); }
};

class DungeonEscortTriggerContext : public NamedObjectContext<Trigger>
{
public:
    DungeonEscortTriggerContext()
    {
        creators["escort npc attacked"] = &DungeonEscortTriggerContext::escort_npc_attacked;
        creators["escort npc low health"] = &DungeonEscortTriggerContext::escort_npc_low_health;
        creators["escort npc far"] = &DungeonEscortTriggerContext::escort_npc_far;
        creators["escort healer sight"] = &DungeonEscortTriggerContext::escort_healer_sight;  // [b36_ohb]
    }

private:
    static Trigger* escort_healer_sight(PlayerbotAI* botAI) { return new EscortHealerSightTrigger(botAI); }
    static Trigger* escort_npc_attacked(PlayerbotAI* botAI) { return new EscortNpcAttackedTrigger(botAI); }
    static Trigger* escort_npc_low_health(PlayerbotAI* botAI) { return new EscortNpcLowHealthTrigger(botAI); }
    static Trigger* escort_npc_far(PlayerbotAI* botAI) { return new EscortNpcFarTrigger(botAI); }
};

class DungeonEscortActionContext : public NamedObjectContext<Action>
{
public:
    DungeonEscortActionContext()
    {
        creators["escort protect"] = &DungeonEscortActionContext::escort_protect;
        creators["escort heal"] = &DungeonEscortActionContext::escort_heal;
        creators["escort stay close"] = &DungeonEscortActionContext::escort_stay_close;
        creators["escort healer sight"] = &DungeonEscortActionContext::escort_healer_sight;  // [b36_ohb]
    }

private:
    static Action* escort_healer_sight(PlayerbotAI* botAI) { return new EscortHealerSightAction(botAI); }
    static Action* escort_protect(PlayerbotAI* botAI) { return new EscortProtectAction(botAI); }
    static Action* escort_heal(PlayerbotAI* botAI) { return new EscortHealAction(botAI); }
    static Action* escort_stay_close(PlayerbotAI* botAI) { return new EscortStayCloseAction(botAI); }
};

#endif
