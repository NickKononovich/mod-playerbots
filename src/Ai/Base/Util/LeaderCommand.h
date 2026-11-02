/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_LEADERCOMMAND_H
#define _PLAYERBOT_LEADERCOMMAND_H

// [lead15] The dungeon leader's current command for a bot, for the rotation side (Ai/Class/**, ThreatStrategy).
// The responsibility split (user 2026-10-26): whatever needs the whole group's view is the leader's (positions, the
// target and kill order, the AoE rule, CC / interrupt / external-save assignments, intensity, rest / ready / pull);
// the bot keeps only per-GCD self-state (its rotation and which AoE spell, its own threat control, its PERSONAL saves,
// the healer's whom / how inside Heal). Every getter answers "no leader command" for a bot the leader does not lead
// (Leader::ExecutorMember false): the rotation keeps its own rule there. Leader side: TestHarnessMgr::TickLeaderEvents.

#include "LeaderOrders.h"
#include "Player.h"

namespace LeaderCommand
{
    // The bot's fresh leader command (stale fields come back None); false: not led, or no command at all.
    inline bool CurrentCommand(Player* bot, Leader::MemberEvents& out)
    {
        out = Leader::MemberEvents();
        return bot && Leader::LedBot(bot) && Leader::Events::Get(bot->GetGUID(), out);
    }

    // The bot is led by the leader (its rotation should read the getters below as the rule); the tank included.
    inline bool Led(Player* bot) { return bot && Leader::LedBot(bot); }

    // [lead410] the Unholy talent: Raise Dead summons the permanent pet
    constexpr uint32 SPELL_MASTER_OF_GHOULS = 52143;

    // [lead410] a burn-list action used as no burn, which the leader's cooldown verdict does not gate: Raise Dead
    // with Master of Ghouls (the permanent pet's summon), or the critical-health Raise Dead + Death Pact heal (its
    // personal save; `critical`: the bot is at its critical health); [lead411] a healer's Inner Focus (its heals)
    inline bool BurnToolUse(Player* bot, std::string const& name, bool critical, bool healer)
    {
        return (name == "raise dead" && (bot->HasAura(SPELL_MASTER_OF_GHOULS) || critical)) ||
               (name == "inner focus" && healer) ||
               // [lead524] Vanish as the threat drop: a mob already on the rogue, or its critical health
               (name == "vanish" && (!bot->getAttackers().empty() || critical));
    }

    // May the bot's AoE hit around `pos`? Not led or no rule from the leader: true (the rotation's own choice).
    // Forbidden: false. AllowedOnPack: `pos` within the leader's radius of the pack's center.
    inline bool AoeAllowed(Player* bot, Position const& pos)
    {
        Leader::MemberEvents command;
        if (!CurrentCommand(bot, command) || command.aoe == Leader::AoeRule::Unset)
            return true;
        if (command.aoe == Leader::AoeRule::Forbidden)
            return false;
        return command.aoeCenter.GetExactDist(pos) <= command.aoeRadius;
    }

    // Normal / Burn (press cooldowns, Bloodlust / Heroism) / Stop (no damage; also AttackEvent::StopDps).
    inline Leader::Intensity GetIntensity(Player* bot)
    {
        Leader::MemberEvents command;
        if (!CurrentCommand(bot, command))
            return Leader::Intensity::Normal;
        if (command.attack == Leader::AttackEvent::StopDps)
            return Leader::Intensity::Stop;
        return command.intensity;
    }

    // The leader's Attack target (Empty: none); the kill order after it in `killOrder` when given.
    inline ObjectGuid AttackTarget(Player* bot, std::vector<ObjectGuid>* killOrder = nullptr)
    {
        Leader::MemberEvents command;
        if (!CurrentCommand(bot, command) || command.attack != Leader::AttackEvent::Attack)
            return ObjectGuid::Empty;
        if (killOrder)
            *killOrder = command.killOrder;
        return command.target;
    }

    // The caster this bot is assigned to interrupt (Empty: none - a led bot does not pick one itself).
    inline ObjectGuid AssignedInterrupt(Player* bot)
    {
        Leader::MemberEvents command;
        return CurrentCommand(bot, command) ? command.interrupt : ObjectGuid::Empty;
    }

    // The mob this bot is assigned to crowd-control (Empty: none).
    inline ObjectGuid AssignedCc(Player* bot)
    {
        Leader::MemberEvents command;
        return CurrentCommand(bot, command) ? command.cc : ObjectGuid::Empty;
    }

    // The external save / buff the leader tells this bot to cast now (spell id, a rank of it the bot knows) and on
    // whom; false: none (a led bot casts no external of its own choice, its personal saves stay its own).
    inline bool AssignedExternal(Player* bot, uint32& spell, ObjectGuid& target)
    {
        Leader::MemberEvents command;
        if (!CurrentCommand(bot, command) || !command.externalSpell)
            return false;
        spell = command.externalSpell;
        target = command.externalTarget;
        return true;
    }
}

// The tank: the mob to take back from a member and the spell to do it with (a taunt when ready, else an AoE threat
// spell: Death and Decay, Blood Boil, Consecration, Thunder Clap, Swipe; 0: just attack it); false: none.
namespace LeaderCommand
{
    inline bool AssignedThreat(Player* bot, uint32& spell, ObjectGuid& mob)
    {
        Leader::MemberEvents command;
        if (!CurrentCommand(bot, command) || command.threatTarget.IsEmpty())
            return false;
        spell = command.threatSpell;
        mob = command.threatTarget;
        return true;
    }

    // [lead16] PreSave: the leader foresees a big hit on this bot - use a personal save now (Barkskin, Ice Block,
    // Divine Shield, Icebound Fortitude, Shield Wall...); `why` the leader's reason. False: none (the bot's own saves
    // are for critical health only).
    inline bool AssignedPreSave(Player* bot, std::string* why = nullptr)
    {
        Leader::MemberEvents command;
        if (!CurrentCommand(bot, command) || !command.preSaveUntilMs)
            return false;
        if (why)
            *why = command.preSaveWhy;
        return true;
    }

    // A mob loose on a member the bot must not damage until the tank has it back (Empty: none).
    inline ObjectGuid DontAttack(Player* bot)
    {
        Leader::MemberEvents command;
        return CurrentCommand(bot, command) ? command.dontAttack : ObjectGuid::Empty;
    }

    // [lead400] the leader's Dispel: the dispel spell (a rank the bot knows) and the member it removes a debuff from;
    // false: none (a led bot dispels nobody of its own choice)
    inline bool AssignedDispel(Player* bot, uint32& spell, ObjectGuid& target)
    {
        Leader::MemberEvents command;
        if (!CurrentCommand(bot, command) || !command.dispelSpell)
            return false;
        spell = command.dispelSpell;
        target = command.dispelTarget;
        return true;
    }

    // [lead402] [lead405] the leader's Consume: the bag item to use and on whom (Empty: itself), or the game object
    // to use; false: none
    inline bool AssignedConsume(Player* bot, uint32& item, ObjectGuid& target, ObjectGuid& object, std::string* why)
    {
        Leader::MemberEvents command;
        if (!CurrentCommand(bot, command) || (!command.consumeItem && command.consumeObject.IsEmpty()))
            return false;
        item = command.consumeItem;
        target = command.consumeTarget;
        object = command.consumeObject;
        if (why)
            *why = command.consumeWhy;
        return true;
    }
}

#endif
