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

#include "Define.h"  // [guild15] first: _USE_MATH_DEFINES before G3D pulls in <cmath> (Position.h needs M_PI)
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
    // [deaths12] ... as many casts as the fight's seconds hold (cast time / global cooldown, a heal over time once per
    // its duration); `onTheMove`: only the instant ones (a run past, a chase: a cast time breaks on the move)
    // [death411] `incomingDps`: the damage the fight deals it meanwhile - a heal cast is that much less (its cast time
    // the bot deals no damage, the fight runs that much longer: SurvivalKit::Gain's own rule)
    float HealPoints(Player* bot, float fightSec, float spareMana, bool items, bool onTheMove = false,
                     float incomingDps = 0.0f);
    // [death442] a solo fight's heals and its rotation's own shield with the mana split the way its play spends it:
    // the rotation spends `manaPerSec` from the start, the kit casts a heal only when HealDue's rule holds (its pick:
    // a spell before an item, the most net of its cast's hits) on the mana left then, each heal up to max health,
    // the solo priest's Power Word: Shield whenever Weakened Soul is off; net of the fight's longer run under the
    // attackers while it casts. b462 Septimon L17 judged "ehp 1245 at hp 256" on four Heals into a 312 hp body paid
    // from the mana the rotation spent first
    // [death444] an attacker's interrupt of the bot's casts (its SmartAI's Shield Bash / Kick on a casting victim):
    // its first check and repeat (the means of the script's rolls; 0: once a fight)
    struct Interrupter
    {
        float firstSec = 0.0f;
        float repeatSec = 0.0f;
    };
    // [death444] ... each interrupter's check catches a heal with a cast time by the share of the fight the bot casts
    // them: that heal is lost (b462 Rollothan L15, holy: two Defias Pathstalkers' Shield Bash on its Holy Lights)
    float FightHeals(Player* bot, float fightSec, float health, float mana, float manaPerSec, float incomingDps,
                     bool items, std::vector<Interrupter> const* interrupters = nullptr);
    // [death440] its heal items alone (one per shared cooldown): what a grouped bot's combat casts on itself without
    // the kit (the "potions" strategy at critical health)
    float ItemHealPoints(Player* bot);
    // [lag360] the speed piece a run past casts as its chase starts: instant (cast on the move), castable now (its
    // cooldown, mana, form), its aura not on the bot yet, lasting `seconds`, and no daze of its own on being struck
    // (Aspect of the Cheetah); 0: none. `pct`: its speed share (0.7 for Sprint)
    uint32 RunPastSpeed(Player* bot, float seconds, float& pct);
    // [death385] the run-past executor's heal: the biggest on-the-move heal HealPoints counts, cast when the chases
    // to come (`damageToCome`) reach its health
    bool RunPastHeal(Player* bot, float damageToCome);
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

// [death389] a solo bot standing in its judged fight out of the judged area (no chase took it there: a ranged or a
// healer bot never runs ReachCombatTo's [fight50] look): judged again where it stands (SoloSafety::FightPastJudged),
// it steps back to the judged spot by a complete route clear of refused reaches
class SoloFightDriftTrigger : public Trigger
{
public:
    SoloFightDriftTrigger(PlayerbotAI* botAI) : Trigger(botAI, "solo fight drift", 2) {}

    bool IsActive() override;
};

class SoloFightDriftAction : public MovementAction
{
public:
    SoloFightDriftAction(PlayerbotAI* botAI) : MovementAction(botAI, "solo fight drift") {}

    bool Execute(Event event) override;
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
