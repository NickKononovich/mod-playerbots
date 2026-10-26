/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaidLeaderStrategy.h"

#include "Action.h"
#include "Group.h"
#include "AttackAction.h"         // [lead15] the own-decision check
#include "GenericSpellActions.h"  // [lead15] CastBuffSpellAction / CastCrowdControlSpellAction
#include "MovementActions.h"      // [lead15]
#include "TestRunnerStrategy.h"   // [lead17] the harness's waypoint for a member
#include "LeaderOrders.h"  // [lead15] the leader's member events
#include "Playerbots.h"
#include "RaidLeaderState.h"

#include <map>
#include <mutex>
#include <string_view>

namespace
{
    // Actions that start a fight out of combat.
    bool IsPullStarter(std::string const& name)
    {
        static constexpr std::string_view starters[] = {"aggressive target", "attack anything",  "pull my target",
                                                        "pull rti target",   "pull start",       "pull action",
                                                        "reach pull",        "attack rti target"};

        for (std::string_view starter : starters)
            if (name == starter)
                return true;

        return false;
    }

    // Orders a leader gives in chat; when a person leads, they start the boss fight he held.
    bool IsLeaderOrder(std::string const& name)
    {
        static constexpr std::string_view orders[] = {"attack my target", "pull my target", "pull rti target",
                                                      "reach pull",       "pull start",     "pull action"};

        for (std::string_view order : orders)
            if (name == order)
                return true;

        return false;
    }

    // Out-of-combat actions that take the group somewhere new (SelfBot leader questing, grinding, travelling).
    bool IsLeadingMove(std::string const& name)
    {
        static constexpr std::string_view moves[] = {
            "choose travel target", "move to travel target", "move random",     "rpg",
            "choose rpg target",    "move to rpg target",    "attack anything", "grind chat shortcut"};

        if (name.compare(0, 8, "new rpg ") == 0 && name != "new rpg status update")
            return true;

        for (std::string_view move : moves)
            if (name == move)
                return true;

        return false;
    }
}

void RaidLeaderStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Rare bookkeeping for the coordinator; placed above the rotation so a call is not delayed by it.
    triggers.push_back(
        new TriggerNode("leader ready check needed", {NextAction("leader ready check", ACTION_HIGH + 1)}));
    triggers.push_back(
        new TriggerNode("leader ready check done", {NextAction("leader ready check result", ACTION_HIGH + 1)}));
    triggers.push_back(
        new TriggerNode("leader encounter event", {NextAction("leader encounter event", ACTION_MOVE + 9)}));
    triggers.push_back(new TriggerNode("leader tank flags", {NextAction("leader assign tanks", ACTION_MOVE + 8)}));

    // Members: water from the bags while the leader waits for mana.
    triggers.push_back(new TriggerNode("ready check low mana", {NextAction("drink", ACTION_NORMAL)}));

    // A lost fight: who can leave it does, and stands up again once the fight is over (any engine).
    triggers.push_back(new TriggerNode("wipe escape", {NextAction("wipe escape", ACTION_EMERGENCY + 5)}));
    triggers.push_back(new TriggerNode("wipe escape over", {NextAction("wipe escape end", ACTION_EMERGENCY + 4)}));

    // An ordered "burst" closes on time.
    triggers.push_back(new TriggerNode("burst over", {NextAction("burst end", ACTION_HIGH + 1)}));

    // [lead15] The dungeon leader's events (LeaderOrders.h Events): its MoveTo / StandAt above every own move of the
    // member (a dodge out of a cleave comes from the leader, ACTION_EMERGENCY + 4 > "dungeon avoid frontal cleave"),
    // its Attack at the pull's opening. Inert without a fresh event (any group the leader does not lead).
    triggers.push_back(
        new TriggerNode("leader move order", {NextAction("leader move order", ACTION_EMERGENCY + 4)}));
    triggers.push_back(
        new TriggerNode("leader attack order", {NextAction("leader attack order", ACTION_HIGH + 3)}));
}

void RaidLeaderStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new RaidLeaderHoldMultiplier(botAI));
}

void RaidConsumablesStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("pre pull potion", {NextAction("use pre pull potion", ACTION_HIGH + 2)}));
    triggers.push_back(new TriggerNode("soul portal available", {NextAction("click soul portal", ACTION_NORMAL + 4)}));
    triggers.push_back(new TriggerNode("soulwell available", {NextAction("use soulwell", ACTION_NORMAL + 3)}));
    triggers.push_back(new TriggerNode("need raid flask", {NextAction("use raid flask", ACTION_NORMAL + 2)}));
    triggers.push_back(new TriggerNode("need food buff", {NextAction("use food buff", ACTION_NORMAL + 1)}));
}

namespace
{
    // [lead17b] a move / attack the executor whitelist blocked: a blocked_move / blocked_attack line (the step-6
    // work list: which tactics the leader takes over first), at most once per member and action per 10 s
    void NoteBlocked(Player* self, std::string const& name, char const* kind)
    {
        constexpr uint32 BLOCKED_LOG_MS = 10 * IN_MILLISECONDS;
        static std::mutex lock;
        static std::map<std::pair<ObjectGuid, std::string>, uint32> last;
        uint32 const now = getMSTime();
        {
            std::lock_guard<std::mutex> guard(lock);
            uint32& at = last[{self->GetGUID(), name}];
            if (at && getMSTimeDiff(at, now) < BLOCKED_LOG_MS)
                return;
            at = now ? now : 1;
        }
        std::string const where = self->GetMap() ? self->GetMap()->GetMapName() : "?";
        Leader::Events::NoteOwn(self->GetGUID(), Leader::OwnDecision{name, kind, where, 0});
    }

    // [lead15] the member's own decisions the executor model turns off (LeaderOrders.h LEADER_NO_OWN_*)
    // [lead18] the action casts on an enemy, by its target value's name (no target computed)
    bool EnemyTargetName(Action* action)
    {
        std::string const target = action->GetTargetName();
        return target == "current target" || target == "dps target" || target == "enemy player target" ||
               target.find("attacker") != std::string::npos || target.find("cc target") != std::string::npos ||
               target.find("enemy") != std::string::npos || target.find("aoe") != std::string::npos;
    }

    bool OwnDecisionOff(Player* self, Action* action, std::string const& name, Leader::MemberEvents const& events,
                        bool combat, bool gate = false)
    {
        if (name == "leader move order" || name == "leader attack order")
            return false;
        auto const in = [&name](std::initializer_list<std::string_view> names)
        {
            for (std::string_view n : names)
                if (name == n)
                    return true;
            return false;
        };
        if (in({"leader external", "leader threat", "leader cc"}))
            return false;
        bool const prepareCmd = events.ready == Leader::ReadyEvent::Prepare ||
                                events.ready == Leader::ReadyEvent::ReadyCheck ||
                                events.ready == Leader::ReadyEvent::Rest;
        // [lead17] LEADER_EXECUTOR_WHITELIST: no movement and no attack the leader did not order
        if (Leader::LEADER_EXECUTOR_WHITELIST)
        {
            TestRunner::Waypoint waypoint;
            if (name == "test runner move" && TestRunner::GetWaypoint(self->GetGUID(), waypoint))
                return false;  // the harness's (the leader's) walk for this member
            if (dynamic_cast<AttackAction*>(action))
            {
                // [lead18] gate: by name only - the basic attacks run on the leader's target (its Attack switched
                // the current target), every own target pick is off
                if (gate)
                    return !(events.attack == Leader::AttackEvent::Attack &&
                             in({"melee", "shoot", "auto shot", "attack"}));
                Unit* target = action->GetTarget();
                bool const blocked = !(events.attack == Leader::AttackEvent::Attack && target &&
                                       target->GetGUID() == events.target);
                if (blocked && name != "dps assist" && name != "attack anything")
                    NoteBlocked(self, name, "blocked_attack");
                return blocked;
            }
            if (dynamic_cast<MovementAction*>(action))
            {
                if (in({"stand up"}))
                    return false;
                if (events.loot && in({"move to loot", "loot", "open loot"}))
                    return false;
                if (prepareCmd && in({"reach party member to resurrect"}))
                    return false;
                if (!in({"follow"}))
                    NoteBlocked(self, name, "blocked_move");
                return true;
            }
        }
        if (Leader::LEADER_NO_OWN_FOLLOW && in({"follow"}))
            return true;
        // [lead18] LEADER_HEALER_HEALS_ONLY: a led healer casts nothing on an enemy
        if (Leader::LEADER_HEALER_HEALS_ONLY && PlayerbotAI::IsHeal(self) &&
            !dynamic_cast<CastHealingSpellAction*>(action) &&
            (dynamic_cast<AttackAction*>(action) || dynamic_cast<CastShootAction*>(action) ||
             (dynamic_cast<CastSpellAction*>(action) && EnemyTargetName(action))))
            return true;
        // [lead16] loot only under the leader's Loot
        if (Leader::LEADER_LOOT_EVENTS && !events.loot &&
            in({"loot", "move to loot", "open loot", "add all loot", "add loot", "store loot", "release loot"}))
            return true;
        if (Leader::LEADER_NO_OWN_ASSIST && in({"dps assist", "dps aoe", "attack anything", "attack rti target"}))
            return true;
        if (Leader::LEADER_NO_OWN_REST && in({"drink", "food"}) && events.ready != Leader::ReadyEvent::Rest)
            return true;
        if (Leader::LEADER_NO_OWN_POSITIONS && in({"move to ranged position", "combat formation move", "rear flank",
                                                   "set behind", "move out of enemy contact", "flee"}))
            return true;
        if (Leader::LEADER_NO_OWN_CLEAVE_DODGE && in({"dungeon avoid frontal cleave"}))
            return true;
        if (Leader::LEADER_NO_OWN_LOS_GATHER && in({"move to los pull spot"}))
            return true;
        if (Leader::LEADER_NO_OWN_AOE_DODGE && in({"avoid aoe", "avoid dungeon hazard"}))
            return true;
        // [lead16] a leader spot (hazard, combat spot, aggro loss, walk) holds: no own range move off it
        if (Leader::LEADER_COMBAT_SPOTS && in({"reach melee", "reach spell"}) &&
            (events.move == Leader::MoveEvent::MoveTo || events.move == Leader::MoveEvent::StandAt ||
             events.move == Leader::MoveEvent::Hold))
            return true;
        if (Leader::LEADER_NO_OWN_CC && in({"auto cc mark", "cast auto cc", "emergency cc"}))
            return true;
        if (Leader::LEADER_NO_OWN_CC && name != "pre pull cc" &&
            (dynamic_cast<CastCrowdControlSpellAction*>(action) ||
             (name.size() > 6 && name.compare(name.size() - 6, 6, " on cc") == 0)))
            return true;
        bool const prepare = events.ready == Leader::ReadyEvent::Prepare ||
                             events.ready == Leader::ReadyEvent::ReadyCheck ||
                             events.ready == Leader::ReadyEvent::Rest;
        // [lead16] rot2: only a buff on ANOTHER member waits for Prepare; a self buff (a shapeshift form, Inner Fire,
        // an armor, a seal, a presence) is the bot's own upkeep
        if (Leader::LEADER_NO_OWN_BUFF_TIMING && !combat && !prepare && dynamic_cast<CastBuffSpellAction*>(action))
        {
            if (gate)
            {
                if (action->GetTargetName() != "self target")
                    return true;
            }
            else if (Unit* buffTarget = action->GetTarget(); buffTarget && buffTarget != self)
                return true;
        }
        // out of combat a member told only to walk / stand does just that
        if (!combat && !prepare && !events.loot && events.attack == Leader::AttackEvent::None &&
            (events.move == Leader::MoveEvent::MoveTo || events.move == Leader::MoveEvent::StandAt ||
             events.move == Leader::MoveEvent::Hold) &&
            !in({"stand up", "reach party member to resurrect"}))
            return true;
        return false;
    }
}

bool RaidLeader::GateVetoes(PlayerbotAI* botAI, Leader::MemberEvents const& events,
                            std::vector<NextAction> const& handlers)
{
    if (handlers.empty())
        return false;
    Player* bot = botAI->GetBot();
    bool const combat = bot->IsInCombat();
    for (NextAction handler : handlers)  // getName is not const
    {
        Action* action = botAI->GetAiObjectContext()->GetAction(handler.getName());
        if (!action || !OwnDecisionOff(bot, action, handler.getName(), events, combat, true))
            return false;
    }
    return true;
}

bool RaidLeaderHoldMultiplier::KeepsVeto(Action* /*action*/)
{
    return Leader::ExecutorMember(bot) || (Leader::LEADER_TANK_EXECUTES && Leader::LedBot(bot) &&
                                           Leader::Orders::IsTracked(bot->GetGUID()));
}

void RaidLeader::CheckOwnDecision(PlayerbotAI* botAI, Action* action, Unit* victimBefore)
{
    if (!Leader::LEADER_OWN_DECISION_CHECK || !botAI || !action)
        return;
    Player* bot = botAI->GetBot();
    if (!Leader::ExecutorMember(bot))
        return;
    std::string const& name = action->getName();
    if (name == "leader move order" || name == "leader attack order")
        return;
    Leader::MemberEvents events;
    Leader::Events::Get(bot->GetGUID(), events);
    bool const combat = bot->IsInCombat();
    bool const fights = events.attack == Leader::AttackEvent::Attack || events.attack == Leader::AttackEvent::Heal;
    bool const prepare = events.ready == Leader::ReadyEvent::Prepare ||
                         events.ready == Leader::ReadyEvent::ReadyCheck || events.ready == Leader::ReadyEvent::Rest;
    auto const in = [&name](std::initializer_list<std::string_view> names)
    {
        for (std::string_view n : names)
            if (name == n)
                return true;
        return false;
    };
    auto const note = [&](char const* kind, std::string const& why)
    { Leader::Events::NoteOwn(bot->GetGUID(), Leader::OwnDecision{name, kind, why, 0}); };

    // the target: a new victim that is not the leader's target
    Unit* victim = bot->GetVictim();
    if (victim && victim != victimBefore &&
        !(events.attack == Leader::AttackEvent::Attack && victim->GetGUID() == events.target) &&
        victim->GetGUID() != events.threatTarget)
    {
        note("target", "attacks " + victim->GetName() + " without a leader Attack on it");
        return;
    }
    if (dynamic_cast<AttackAction*>(action))
        return;
    if (in({"drink", "food"}))
    {
        if (events.ready != Leader::ReadyEvent::Rest)
            note("rest", "sits without the leader's Rest");
        return;
    }
    if (dynamic_cast<MovementAction*>(action))
    {
        // the rotation's own range on the commanded target is its "how"
        bool const rotationRange = combat && fights && in({"reach melee", "reach spell"});
        bool const resurrect = prepare && in({"reach party member to resurrect"});
        bool const plannedCc = !events.cc.IsEmpty() && in({"pre pull cc"});
        bool const looting = events.loot && in({"move to loot", "loot", "open loot"});
        if (!rotationRange && !resurrect && !plannedCc && !looting && !in({"stand up"}))
            note("move", "moves without a leader MoveTo / StandAt");
        return;
    }
    if (!dynamic_cast<CastSpellAction*>(action))
        return;
    if (dynamic_cast<CastCrowdControlSpellAction*>(action) ||
        (name.size() > 6 && name.compare(name.size() - 6, 6, " on cc") == 0))
    {
        if (events.cc.IsEmpty())
            note("cc", "crowd control without a leader CC assignment");
        return;
    }
    if (in({"innervate", "power infusion", "pain suppression", "hand of sacrifice", "hand of protection",
            "misdirection", "misdirection on party", "tricks of the trade", "tricks of the trade on main tank",
            "hymn of hope", "divine hymn", "hand of salvation", "hand of freedom"}))
    {
        if (!events.externalSpell)
            note("external", "an external without a leader assignment");
        return;
    }
    if (in({"bloodlust", "heroism", "icy veins", "arcane power", "presence of mind", "avenging wrath", "death wish",
            "recklessness", "berserk", "army of the dead", "summon gargoyle", "rapid fire", "bestial wrath",
            "adrenaline rush", "blade flurry", "killing spree", "shadowfiend", "feral spirit", "elemental mastery",
            "metamorphosis", "mirror image", "starfall", "force of nature", "summon infernal", "dancing rune weapon",
            "unholy frenzy", "hysteria"}))
    {
        if (events.intensity != Leader::Intensity::Burn)
            note("burn", "a burn cooldown without the leader's Intensity burn");
        return;
    }
    if (dynamic_cast<CastBuffSpellAction*>(action))
        if (Unit* target = action->GetTarget(); target && target != bot && !prepare && !combat)
            note("buff on others", "buffs " + target->GetName() + " without Prepare / Ready?");
}

float RaidLeaderHoldMultiplier::GetValue(Action* action)
{
    if (!action || !RaidLeader::IsEnabled())
        return 1.0f;

    // [lead17] LEADER_TANK_OWN_PICKS_OFF: the led tank's own next pack and own hazard step are off in every state
    if (Leader::LEADER_TANK_OWN_PICKS_OFF && Leader::Orders::IsTracked(bot->GetGUID()) && Leader::LedBot(bot) &&
        (action->getName() == "chain pull" || action->getName() == "move fight off hazard"))
        return 0.0f;

    // [lead16] LEADER_TANK_EXECUTES: in a fight the tank's target is the leader's Attack
    if (Leader::LEADER_TANK_EXECUTES && bot->IsInCombat() && Leader::Orders::IsTracked(bot->GetGUID()) &&
        Leader::LedBot(bot))
    {
        Leader::MemberEvents events;
        static constexpr std::string_view tankPicks[] = {"tank assist", "attack anything", "dps assist",
                                                         "tank defend on attacked group member",
                                                         "reach healer attacker", "chain pull",
                                                         "move fight off hazard"};
        if (Leader::Events::Get(bot->GetGUID(), events) && events.attack == Leader::AttackEvent::Attack)
            for (std::string_view pick : tankPicks)
                if (action->getName() == pick)
                    return 0.0f;
    }

    // [lead15] LEADER_MEMBER_EXECUTOR: the member executes the leader's command, its own decisions are off (alive:
    // a dead member's release / corpse run are its own)
    if (bot->IsAlive() && Leader::ExecutorMember(bot))
    {
        Leader::MemberEvents events;
        Leader::Events::Get(bot->GetGUID(), events);  // none: every field None
        if (OwnDecisionOff(bot, action, action->getName(), events, bot->IsInCombat()))
        {
            // [lead18] a vetoed action reached the multiplier: its trigger / chain was evaluated (vetoed_eval)
            if (Leader::LEADER_EXECUTOR_TRIGGER_GATE)
                Leader::Events::NoteVetoedEval(bot->GetGUID(), action->getName());
            return 0.0f;
        }
    }

    // [lead15] the leader's member events: while its MoveTo / StandAt holds, the member's own moves (the dodges it
    // picked itself, its ranged spot, follow, the los pull gather, formation) yield to it - no two movement rules; a
    // Hold stops every move; StopDps stops damage, DontAttack every new attack out of combat.
    if (Leader::LEADER_MEMBER_EVENTS)
    {
        Leader::MemberEvents events;
        if (Leader::Events::Get(bot->GetGUID(), events))
        {
            std::string const& name = action->getName();
            static constexpr std::string_view ownMoves[] = {
                "dungeon avoid frontal cleave", "move to ranged position", "follow", "move to los pull spot",
                "combat formation move", "rear flank", "set behind", "move out of enemy contact", "flee"};
            bool const leaderMove =
                events.move == Leader::MoveEvent::MoveTo || events.move == Leader::MoveEvent::StandAt;
            if (leaderMove && Leader::LEADER_OWNS_DODGE && name != "leader move order")
                for (std::string_view own : ownMoves)
                    if (name == own)
                        return 0.0f;
            if (events.move == Leader::MoveEvent::Hold && name != "leader move order")
                for (std::string_view own : ownMoves)
                    if (name == own)
                        return 0.0f;
            if ((events.attack == Leader::AttackEvent::StopDps || events.intensity == Leader::Intensity::Stop) &&
                RaidLeader::IsOffensiveAction(action, bot))
                return 0.0f;
            if (events.attack == Leader::AttackEvent::DontAttack && !bot->IsInCombat() &&
                RaidLeader::IsOffensiveAction(action, bot))
                return 0.0f;
        }
    }

    // Out of a lost fight (stealth, Feign Death, Shadowmeld, Divine Intervention): any action or step gives the bot
    // away, so it does nothing until the fight is over ("wipe escape over" then ends the hold).
    if (AI_VALUE(bool, "wipe escape hold"))
        return 0.0f;

    if (bot->IsInCombat())
        return (AI_VALUE(bool, "pull countdown") && RaidLeader::IsOffensiveAction(action, bot)) ? 0.0f : 1.0f;

    if (!AI_VALUE(bool, "pull hold"))
        return 1.0f;

    std::string const& name = action->getName();
    if (AI_VALUE(bool, "group paused"))
    {
        // On a break the group stays where it is: no pulls, no leading on. Buffs, food and following go on.
        if (IsLeadingMove(name))
            return 0.0f;
    }
    else if (IsLeaderOrder(name) && RaidLeader::IsLedByPerson(bot))
        return 1.0f;

    // [lead17] slabs-h 20261026-184720: the tank sat 20 s on an Issued leader order while this hold (fed by the old
    // 40 yd readiness) blocked its pull: an open leader order was judged ready by the leader - one readiness owner
    if (Leader::LEADER_ONE_GATHER_OWNER && Leader::Orders::IsOpen(bot->GetGUID()))
        return 1.0f;

    if (IsPullStarter(name))
        return 0.0f;

    if (!RaidLeader::IsOffensiveAction(action, bot))
        return 1.0f;

    // The hold stops new pulls only: a tank whose group is already attacked still picks the attackers up.
    if (PlayerbotAI::IsTank(bot))
        return RaidLeader::IsTankDefendingGroup(botAI) ? 1.0f : 0.0f;

    // ... and the rest of the group joins a fight that is already on: mobs that came to the tank without a pull
    // (Deadmines 2026-10-24) killed it while the dps, out of combat themselves, had every attack held.
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->GetMap() == bot->GetMap() && member->IsAlive() &&
                member->IsInCombat())
                return 1.0f;
        }
    }

    return 0.0f;
}
