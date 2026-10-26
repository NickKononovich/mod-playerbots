/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaidLeaderStrategy.h"

#include "Action.h"
#include "Group.h"
#include "GenericSpellActions.h"  // [lead15] CastBuffSpellAction / CastCrowdControlSpellAction
#include "LeaderOrders.h"  // [lead15] the leader's member events
#include "Playerbots.h"
#include "RaidLeaderState.h"

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
    // [lead15] the member's own decisions the executor model turns off (LeaderOrders.h LEADER_NO_OWN_*)
    bool OwnDecisionOff(Action* action, std::string const& name, Leader::MemberEvents const& events, bool combat)
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
        if (Leader::LEADER_NO_OWN_FOLLOW && in({"follow"}))
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
        if (Leader::LEADER_NO_OWN_CC && name != "pre pull cc" &&
            (dynamic_cast<CastCrowdControlSpellAction*>(action) ||
             (name.size() > 6 && name.compare(name.size() - 6, 6, " on cc") == 0)))
            return true;
        bool const prepare = events.ready == Leader::ReadyEvent::Prepare ||
                             events.ready == Leader::ReadyEvent::ReadyCheck ||
                             events.ready == Leader::ReadyEvent::Rest;
        if (Leader::LEADER_NO_OWN_BUFF_TIMING && !combat && !prepare && dynamic_cast<CastBuffSpellAction*>(action))
            return true;
        // out of combat a member told only to walk / stand does just that
        if (!combat && !prepare && events.attack == Leader::AttackEvent::None &&
            (events.move == Leader::MoveEvent::MoveTo || events.move == Leader::MoveEvent::StandAt ||
             events.move == Leader::MoveEvent::Hold) &&
            !in({"stand up", "reach party member to resurrect"}))
            return true;
        return false;
    }
}

bool RaidLeaderHoldMultiplier::KeepsVeto(Action* /*action*/) { return Leader::ExecutorMember(bot); }

float RaidLeaderHoldMultiplier::GetValue(Action* action)
{
    if (!action || !RaidLeader::IsEnabled())
        return 1.0f;

    // [lead15] LEADER_MEMBER_EXECUTOR: the member executes the leader's command, its own decisions are off
    if (Leader::ExecutorMember(bot))
    {
        Leader::MemberEvents events;
        Leader::Events::Get(bot->GetGUID(), events);  // none: every field None
        if (OwnDecisionOff(action, action->getName(), events, bot->IsInCombat()))
            return 0.0f;
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
