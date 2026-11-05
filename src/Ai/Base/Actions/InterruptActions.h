/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_INTERRUPTACTIONS_H
#define PLAYERBOTS_INTERRUPTACTIONS_H

#include "Action.h"
#include "MovementActions.h"  // [lead405] LeaderUseObjectAction
#include "UseItemAction.h"    // [lead402] LeaderConsumeAction
#include "ObjectGuid.h"       // [lead1390]
#include "SharedDefines.h"    // [lead1390] SpellCastResult

#include <string>

class GameObject;
class PlayerbotAI;
class SpellInfo;
class Unit;

namespace Leader
{
    struct MemberEvents;
}

// [lead1390] the game's refusal of a leader order now, for the member that executes it (PlayerbotAI::CheckCastNow):
// SPELL_CAST_OK when the game takes it, when only a passing state holds it (a walk, sitting, the global cooldown, a
// cast under way), or when the order's own execution removes the answer (the walk to a dead member, the form a
// resurrection or a portal click leaves, a movement form, the bounced buff's refresh). The orders' isPossible and
// the leader's invariant check (order_impossible) read the same answer
namespace LeaderOrderCheck
{
    SpellCastResult ExternalRefusal(PlayerbotAI* botAI, uint32 spell, ObjectGuid target);
    SpellCastResult DispelRefusal(PlayerbotAI* botAI, uint32 spell, ObjectGuid target);
    SpellCastResult ConsumeRefusal(PlayerbotAI* botAI, uint32 itemId, ObjectGuid target);
    // a game object's click (GameObject::Use casts its spell on the clicker, the game's flags)
    SpellCastResult UseObjectRefusal(PlayerbotAI* botAI, GameObject* go);
    // the first refused order of the member's current events ("external 48169 on Rtpriaa: SPELL_FAILED_...");
    // empty: none
    std::string OrderRefusal(PlayerbotAI* botAI, Leader::MemberEvents const& events);
}

// Casts the interrupt the group rotation assigned to this bot on the enemy whose cast it was assigned.
class InterruptAssignedCastAction : public Action
{
public:
    InterruptAssignedCastAction(PlayerbotAI* botAI) : Action(botAI, "interrupt assigned cast") {}

    bool isUseful() override;
    bool isPossible() override;
    bool Execute(Event event) override;

private:
    Unit* GetCaster();
    SpellInfo const* GetAssignedSpell();
};

// [rot2] The dungeon leader's assigned casts (LeaderCommand.h). Each does exactly what the command names and
// reports it to the leader (Leader::Events::Ack); with no fresh command for the bot each is useless.

// The external save / buff the leader assigns (spell id, on whom): Innervate, Hymn of Hope, Divine Hymn, Pain
// Suppression, Power Infusion, the Hand spells, Misdirection, Tricks, Bloodlust / Heroism.
// [lead401] ... and a resurrection on a dead member (a battle resurrection in a fight, out of one Resurrection /
// Redemption / Ancestral Spirit / Revive): out of a fight the walk into its range and sight comes first.
class LeaderExternalAction : public Action
{
public:
    LeaderExternalAction(PlayerbotAI* botAI) : Action(botAI, "leader external") {}

    bool isUseful() override;
    bool isPossible() override;
    bool Execute(Event event) override;
    std::vector<NextAction> getPrerequisites() override;
private:
    bool externalVoidLogged = false;  // [lead1320] the leader's External ended by its own condition, logged once
};

// [lead400] The leader's Dispel: the dispel spell it names on the member it names (a debuff of a type the spell
// removes is on it).
class LeaderDispelAction : public Action
{
public:
    LeaderDispelAction(PlayerbotAI* botAI) : Action(botAI, "leader dispel") {}

    bool isUseful() override;
    bool isPossible() override;
    bool Execute(Event event) override;
};

// [lead402] The leader's Consume of a bag item: a potion on the bot itself, a soulstone on the member it names.
class LeaderConsumeAction : public UseItemAction
{
public:
    LeaderConsumeAction(PlayerbotAI* botAI) : UseItemAction(botAI, "leader consume", true) {}

    bool isUseful() override;
    bool isPossible() override;
    bool Execute(Event event) override;
};

// [lead405] The leader's Consume of a game object (a Soulwell, a ritual's portal): the walk into its reach, the use.
class LeaderUseObjectAction : public MovementAction
{
public:
    LeaderUseObjectAction(PlayerbotAI* botAI) : MovementAction(botAI, "leader use object") {}

    bool isUseful() override;
    bool Execute(Event event) override;
};

// The tank takes back the mob the leader names: its taunt or AoE threat spell, else just attacks it.
class LeaderThreatAction : public Action
{
public:
    LeaderThreatAction(PlayerbotAI* botAI) : Action(botAI, "leader threat") {}

    bool isUseful() override;
    bool Execute(Event event) override;
};

// The crowd control the leader assigns: the bot's own hold spell for that mob (FindCrowdControl).
class LeaderCcAction : public Action
{
public:
    LeaderCcAction(PlayerbotAI* botAI) : Action(botAI, "leader cc") {}

    bool isUseful() override;
    bool isPossible() override;
    bool Execute(Event event) override;
};

// [rot3] The leader's PreSave (a big hit foreseen on this bot): the bot's best personal save it has ready.
class LeaderPreSaveAction : public Action
{
public:
    LeaderPreSaveAction(PlayerbotAI* botAI) : Action(botAI, "leader pre save") {}

    bool isUseful() override;
    bool Execute(Event event) override;

private:
    char const* PickSave();
};

#endif
