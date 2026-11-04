/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SPELLPAYOFF_H
#define PLAYERBOTS_SPELLPAYOFF_H

#include "Define.h"

class Player;
class Unit;

// [rot400] [rot401] What one cast of a damage spell is worth to its caster, from the game's own numbers (Spell.dbc
// through the core: effect values, the caster's spell power and done modifiers, crit chance and bonus, hasted cast
// / channel / global cooldown, spell mods on duration). Two decisions read it:
// - a DoT is cast only when the ticks that land before the target dies beat the filler cast in the same time
//   (DotPays): b228 rfc-h, single-mob trash pulls last 6-8 s, Shadow left 476k and Affliction 246k DoT damage per
//   run on dying mobs;
// - an area spell is cast only on at least the targets where its damage per target times the count beats the filler
//   (AoeBreakEvenTargets), the count from the spells, not a fixed 3.
// Cheap: a few effect reads and the core's bonus calls, no scan; read by triggers after their own cheap checks.
namespace SpellPayoff
{
    struct Cast
    {
        float direct = 0.0f;       // per target: its direct hits (crit included as expected value)
        float tick = 0.0f;         // per target: one periodic tick (its own or a periodically triggered spell's)
        uint32 amplitudeMs = 0;    // between ticks; 0: no periodic part
        uint32 ticks = 0;          // ticks of its full duration
        float castMs = 0.0f;       // till it lands: the hasted cast time (0 for an instant and a channel)
        float executeMs = 0.0f;    // the caster's time for one cast: cast or channel, at least the global cooldown
        bool channel = false;      // its ticks land while it is channelled

        float Total() const { return direct + tick * static_cast<float>(ticks); }
    };

    // false when `spellId` is no damage spell of `bot`
    bool Read(Player* bot, Unit* target, uint32 spellId, Cast& out);

    // the filler's damage per second on one target (all of a channel's ticks over its channel); 0 when unknown
    float FillerDps(Player* bot, Unit* target, uint32 fillerSpellId);

    struct DotVerdict
    {
        bool pays = true;
        float dot = 0.0f;      // the DoT's damage that lands within the lifetime
        float filler = 0.0f;   // the filler's damage in the DoT's execute time
        uint32 ticks = 0;      // the DoT's ticks that land
    };

    // [rot400] The DoT `dotSpellId` on `target` living `lifetimeSec` more against `fillerSpellId` cast instead:
    // its direct hit plus every tick landing after its cast and before the death (`upfrontShare` x its full periodic
    // damage lands at once on top: Improved Devouring Plague) vs the filler's damage per second x the DoT's execute
    // time. Pays when no filler is known.
    DotVerdict DotPays(Player* bot, Unit* target, uint32 dotSpellId, uint32 fillerSpellId, float lifetimeSec,
                       float upfrontShare = 0.0f);

    // [rot401] The fewest targets on which one cast of `aoeSpellId` deals at least the filler's damage in the same
    // execute time: ceil(filler dps / area dps per target). 0 when either is unknown (no verdict).
    // [rot420] `fillerScale`: the filler's damage at the rotation's steady state over its damage now (an Arcane Blast
    // at its full stacks, read while the stacks are down)
    uint32 AoeBreakEvenTargets(Player* bot, Unit* target, uint32 aoeSpellId, uint32 fillerSpellId,
                               float fillerScale = 1.0f);

    // [rot401] The area's radius from its damage effects (or its periodically triggered spell's); 0 when none
    float AreaRadius(Player* bot, uint32 aoeSpellId);
}

#endif
