/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SURVIVALKIT_H
#define PLAYERBOTS_SURVIVALKIT_H

// [guild80] user 2026-11-08: a levelling bot fighting alone whose fight the SoloSafety projection turns lost uses its
// survival kit like a player, every piece read from spell and item data (no tuned numbers):
// - a reset that ends the fight (a spell with SPELL_EFFECT_SANCTUARY - Vanish, Shadowmeld - or SPELL_AURA_FEIGN_DEATH):
//   the mobs drop it and go home, it rests to full after;
// - else the pieces that turn the projection won, held for the time they matter: its heals (direct, periodic, Lay on
//   Hands) and heal items (potions, healthstones) cast once the missing health takes the whole heal or the hits before
//   the next chance would kill it; absorbs (Power Word: Shield), damage-taken / dodge / immunity buffs (Divine
//   Protection, Barkskin, Evasion, Divine Shield - not one that stuns its caster, Ice Block), stuns on the attackers
//   that hold through damage (Hammer of Justice, War Stomp), silences on casters (Arcane Torrent), and controls that
//   break on damage (fears, incapacitates, roots) only as a window for its heal or bandage (First Aid) - the shortest
//   cooldowns first, so the long ones stay for the fights that need them;
// - else a run, only when the core's leash says it lives: a world mob drops it once it is more than
//   CONFIG_CREATURE_LEASH_RADIUS (2D) from its reset spot and its last leash extension (Creature::GetLeashTimer, the
//   extension refreshed by direct damage, Unit::Attack, every hostile spell hit and the mob's loss of control) is
//   older than the timer; it runs only when its health outlasts the hits until then on a way clear of other mobs, with
//   all damage off (no attack, auto shot off, its pet passive and at heel), a snare / root / speed buff first when it
//   has one;
// - else everything it has, the biggest piece first.
// Logged "[solo] survival <spell> <reason>" / "[solo] survival flee leash=Ns dist=... ".

#include "G3D/Vector3.h"
#include "MovementActions.h"
#include "Trigger.h"

#include <string>
#include <vector>

class Player;
class PlayerbotAI;

namespace SurvivalKit
{
    // the trigger's check: the projection is lost (or a kit step is under way); true while the kit has a step
    bool Evaluate(Player* bot);
    // [deaths3] the healing it can give itself over a fight of `fightSec`, from its spell and item data now (no fight
    // history): each heal item once per shared cooldown (with `items`), a heal without mana as often as its cooldown
    // allows, its best heal per mana as far as `spareMana` (the mana its rotation leaves) pays; bandages need a window
    // and are not counted
    float HealPoints(Player* bot, float fightSec, float spareMana, bool items);
}

class SoloSurvivalTrigger : public Trigger
{
public:
    SoloSurvivalTrigger(PlayerbotAI* botAI) : Trigger(botAI, "solo survival") {}

    bool IsActive() override;
};

class SoloSurvivalAction : public MovementAction
{
public:
    SoloSurvivalAction(PlayerbotAI* botAI) : MovementAction(botAI, "solo survival") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// [runpast11] the run-past executor: a solo bot walking a leg the route search priced as a run past, chased only by
// mobs of that priced set, keeps walking the leg at full speed with every damage off (no attack, auto shot off, its
// pet at heel); the mobs leash by the core's rule. Not while the kit runs, when the chase no longer leaves it alive,
// or when the judge wins the one attacker for less damage than its chase (SoloSafety::RunPastStep)
class SoloRunPastTrigger : public Trigger
{
public:
    SoloRunPastTrigger(PlayerbotAI* botAI) : Trigger(botAI, "solo run past") {}

    bool IsActive() override;
};

class SoloRunPastAction : public MovementAction
{
public:
    SoloRunPastAction(PlayerbotAI* botAI) : MovementAction(botAI, "solo run past") {}

    bool Execute(Event event) override;
    bool isUseful() override;  // the full RunPastStep (attackers, chases, the judge); Execute walks its answer

private:
    std::vector<G3D::Vector3> rest;
    std::string line;
};

#endif
