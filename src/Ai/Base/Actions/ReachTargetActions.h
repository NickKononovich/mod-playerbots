/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_REACHTARGETACTIONS_H
#define PLAYERBOTS_REACHTARGETACTIONS_H

#include "GenericSpellActions.h"
#include "MovementActions.h"

#include <G3D/Vector3.h>

class PlayerbotAI;

class ReachTargetAction : public MovementAction
{
public:
    ReachTargetAction(PlayerbotAI* botAI, std::string const name, float distance)
        : MovementAction(botAI, name), distance(distance)
    {
    }

    bool Execute(Event event) override;
    bool isUseful() override;
    std::string const GetTargetName() override;

protected:
    float distance;
};

class CastReachTargetSpellAction : public CastSpellAction
{
public:
    CastReachTargetSpellAction(PlayerbotAI* botAI, std::string const spell, float distance)
        : CastSpellAction(botAI, spell), distance(distance)
    {
    }

    bool isUseful() override;

protected:
    float distance;
};

// The first idle aggressive mob (not `target` itself) whose aggro reach covers `target`'s spot (the core's Z gate and
// 3D range, IdleAggroBlocker, plus a margin; no line of sight check, the fight drifts): a tank that walks there pulls
// the next pack. nullptr when none does.
Creature* IdleMobReachingSpot(PlayerbotAI* botAI, Player* bot, Unit* target);

// A dungeon tank's walk to `target`, a mob already fighting (not a boss) farther than TANK_FAR_MOB, would end in the
// aggro reach of an idle pack (IdleMobReachingSpot, returned): the taunts take it from where the tank stands.
Creature* TankWalkIntoIdlePack(PlayerbotAI* botAI, Player* bot, Unit* target);

// A dungeon tank does not chase a mob that already attacks it (or no one of the group) away from the healer, nor a
// far one into the room of an idle pack (TankWalkIntoIdlePack); a walk to a far mob is judged by the path guard.
class ReachMeleeAction : public ReachTargetAction
{
public:
    static constexpr float TANK_FAR_MOB = 10.0f;

    ReachMeleeAction(PlayerbotAI* botAI) : ReachTargetAction(botAI, "reach melee", sPlayerbotAIConfig.MeleeDistance) {}

    bool Execute(Event event) override;
    bool isUseful() override;

protected:
    bool GuardsTankWalk() override;

private:
    // [b32_arc] A refused far target that holds the tank in combat but fights nobody (a combat-only reference: no
    // threat, no victim, it never comes) is pulled from range instead: Execute casts the taunt / ranged threat spell
    // from here, or steps into its range on a spot no idle mob reaches. Empty: the ordinary walk.
    bool PullPhantomFromRange(Unit* target);
    ObjectGuid _phantomPull;
    uint32 _phantomLogMs = 0;
    ObjectGuid _phantomTaunted;    // [b33_arc] the phantom target the last taunt landed on, and when
    uint32 _phantomTauntedMs = 0;

    // [b46_ohb] A refused far target that shoots the tank from where it stands (a SmartAI caster / shooter with
    // SMARTCAST_COMBAT_MOVE holds its range while it sees its victim) and that no ranged member or healer sees: the
    // tank steps out of its sight to a member's spot (the healer's first) that it does not see, out of every idle
    // reach; the fight spot moves there. A shooter that loses its victim's sight walks to it, away from the idle pack.
    bool BreakShooterSight(Unit* target);
    ObjectGuid _sightBreak;          // set by isUseful for Execute
    ObjectGuid _shooter;             // the standing shooter refused, and since when
    uint32 _shooterSinceMs = 0;
    uint32 _sightBreakFailMs = 0;    // the last search that found no spot (retry after a pause)
    uint32 _sightBreakLogMs = 0;
};

// Tank: walk the path to a mob hitting the healer out of reach ("far healer attacker"). Also out of combat: an idle
// tank's attack needs line of sight, so behind a corner the healer died while the tank stood still. Not to a mob in
// the aggro reach of an idle pack: the taunts take it from where the tank stands.
class ReachHealerAttackerAction : public ReachTargetAction
{
public:
    ReachHealerAttackerAction(PlayerbotAI* botAI)
        : ReachTargetAction(botAI, "reach healer attacker", sPlayerbotAIConfig.MeleeDistance)
    {
    }

    std::string const GetTargetName() override { return "far healer attacker"; }
    bool isUseful() override;

protected:
    bool GuardsTankWalk() override { return true; }
    bool GuardsIdleTankWalk() override { return true; }

private:
    static constexpr uint32 TAUNT_LOG_MS = 5000;
    uint32 _tauntLogMs = 0;
};

// Tank: walk to the mob the peel would take ("group member attacker") when it stands within TankPeelWalkRange,
// out of melee: in melee it is held by Hammer of the Righteous, Thunder Clap or Swipe while the taunts cool down.
class ReachGroupMemberAttackerAction : public ReachTargetAction
{
public:
    ReachGroupMemberAttackerAction(PlayerbotAI* botAI)
        : ReachTargetAction(botAI, "reach group member attacker", sPlayerbotAIConfig.MeleeDistance)
    {
    }

    std::string const GetTargetName() override { return "group member attacker"; }
    bool isUseful() override;

protected:
    bool GuardsTankWalk() override { return true; }
};

// A healer or ranged member a mob is meleeing walks to the group's tank (the one "group ranged position" found),
// where its area threat and taunts take the mob ("chased away from tank" trigger), instead of standing alone.
class MoveToTankAction : public ReachTargetAction
{
public:
    static constexpr float REFUGE_DISTANCE = 3.0f;  // close enough for Consecration / Thunder Clap to reach the mob

    MoveToTankAction(PlayerbotAI* botAI) : ReachTargetAction(botAI, "move to tank", REFUGE_DISTANCE) {}

    Unit* GetTarget() override;
    bool Execute(Event event) override;
    bool isUseful() override;
};

// Both walk to range, and also, in range, round the corner that hides the target (MovementAction::MoveToSight):
// the triggers fire on a lost line of sight, the plain reach only measured the distance.
class ReachSpellAction : public ReachTargetAction
{
public:
    ReachSpellAction(PlayerbotAI* botAI);

    bool Execute(Event event) override;
    bool isUseful() override;
};

class ReachPartyMemberToHealAction : public ReachTargetAction
{
public:
    ReachPartyMemberToHealAction(PlayerbotAI* botAI);

    std::string const GetTargetName() override;
    bool Execute(Event event) override;
    bool isUseful() override;
};

// Out of combat the resurrecter of a corpse round a corner or among an idle pack walks to the farthest spot within the
// spell's range that sees the corpse and keeps out of every idle mob's aggro reach (+ a margin) on the whole walk: the
// first such point of the path toward the corpse, else a spot on the range ring on the group's side. The resurrected
// comes up where the caster stands (Player::ResurectUsingRequestData), out of the pack's reach too. No such spot: the
// search is noted (CorpseRes) and the dead releases at once instead of waiting for a resurrection that cannot come.
class ReachPartyMemberToResurrectAction : public ReachTargetAction
{
public:
    ReachPartyMemberToResurrectAction(PlayerbotAI* botAI);

    std::string const GetTargetName() override;
    bool Execute(Event event) override;
    bool isUseful() override;

private:
    enum class SpotSearch : uint8
    {
        FOUND,    // `_spot` sees the corpse within range, its walk wakes nothing
        BLOCKED,  // every spot that sees the corpse, or its walk, is in an idle mob's reach
        NONE      // no idle mob around and no spot found: the plain reach
    };

    SpotSearch FindSpot(Unit* corpse);

    uint32 _spotMs = 0;
    ObjectGuid _spotFor;
    SpotSearch _search = SpotSearch::NONE;
    G3D::Vector3 _spot;
    float _spotWalk = 0.0f;
    std::string _spotReason;
    uint32 _loggedMs = 0;
    G3D::Vector3 _loggedSpot;
};

// What the resurrecters' spot searches (ReachPartyMemberToResurrectAction) found for a corpse, for its dead owner's
// release (AutoReleaseSpiritAction): every recent search found no spot out of the pack's reach.
namespace CorpseRes
{
    void Note(Player* dead, bool blocked);
    // The searches for `dead` have found no spot for at least `forMs`, the last one just now.
    bool Blocked(Player* dead, uint32 forMs);
}

#endif
