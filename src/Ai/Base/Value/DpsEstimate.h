/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DPSESTIMATE_H
#define PLAYERBOTS_DPSESTIMATE_H

#include <limits>
#include <string>

#include "Define.h"

class Player;
class SpellInfo;
class Unit;

// [dps1] What a bot can deal per second against a target, from a formula over its state now (user 2026-11-08: no
// fight history ever feeds a decision). The core's own numbers carry its gear, enchants, gems, talents and auras:
// weapon damage and speed, attack power, spell power, the attack table and spell hit against the target's level,
// armor and armor penetration, crit and its bonus, haste. The rotation is the spells it knows (highest rank) in a
// simplified priority per spec after the wowsims APLs (C:/WoW/tools/simpct_data/vendor/src/wotlk/ui/*/apls) and
// Rawr 2.3.23: DoTs kept up, cooldown spells on cooldown, the best filler in the rest of the time, limited by
// rage / energy / combo points / runes / runic power; mana limits it over the fight length passed in.
// Cached per bot by a power signature (its level, spells, form, pet and the core's stat outputs): recomputed only
// when one of them changes.
namespace DpsEstimate
{
enum class BuffContext : uint8
{
    Solo,   // the self-buffs it can cast and does not wear now
    Group,  // + one buff set per present group member's class and known spells
    Raid    // + the full raid set (every class, highest rank at its level)
};

struct Result
{
    float dps = 0.0f;         // over the fight length on its mana now (the burst when no length is given)
    float burstDps = 0.0f;    // the rotation unlimited by mana (rage, energy, runes always limit it)
    float whiteDps = 0.0f;    // what lasts with no mana: weapon swings, Auto Shot or wand, and the pet
    float petDps = 0.0f;      // the pet's part, in all three above
    float tps = 0.0f;         // threat per second of the burst rotation (the pet's not included)
    float manaPerSec = 0.0f;  // the rotation's mana use minus its in-combat regeneration (0: none or no mana)
    float oomSec = std::numeric_limits<float>::max();  // seconds of rotation on its mana now
    float cycleSec = 0.0f;  // one pass of the rotation: every spell it uses once, at least one global cooldown
    uint8 spells = 0;       // rotation spells it knows and uses
};

// `target`: the unit it fights (its level, armor, avoidance and auras are read); null: a mob of its level
// (creature_classlevelstats armor, the core's creature avoidance). fightSec 0: the burst
Result Estimate(Player* bot, Unit* target, BuffContext ctx, float fightSec = 0.0f);
float EstimateDps(Player* bot, Unit* target, BuffContext ctx, float fightSec = 0.0f);
float EstimateTps(Player* bot, Unit* target, BuffContext ctx);

// [rot603] A cooldown nuke cast only where it pays (Fire Blast, Mind Blast while levelling): one cast's damage on
// `target` (SpellPayoff::Read) at least the filler's in the cast's execute time (SpellPayoff::FillerDps). The bot's
// "fire blast pays" / "mind blast pays" triggers and the formula read this one rule. True when no filler is known.
bool CastPays(Player* bot, Unit* target, uint32 spellId, uint32 fillerSpellId);

char const* ContextName(BuffContext ctx);
// a solo bot outside a group: Solo; a party: Group; a raid group: Raid
BuffContext ContextOf(Player* bot);

// [dps1] one closed fight, formula vs what it dealt: logged as a [dps] line and written to
// playerbots_fight_history for the analysis scripts only (no decision reads either)
struct FightSample
{
    uint32 targetEntry = 0;
    uint8 targetLevel = 0;
    uint32 mobs = 0;
    float formulaDps = 0.0f;  // Estimate(...).dps at the fight's start
    float cycleSec = 0.0f;    // Estimate(...).cycleSec at the fight's start
    uint64 dealt = 0;         // its damage to its attackers in the fight
    float fightSec = 0.0f;
    float activeSec = 0.0f;  // the time it attacked (a victim, a cast or Auto Shot under way)
    uint64 taken = 0;
    char const* outcome = "won";  // won / fled / died
    std::string invalidWhy;       // empty: a valid sample
    BuffContext ctx = BuffContext::Solo;
};
void RecordFight(Player* bot, FightSample const& sample);

// [dpscal] what a solo bot (and its pet, its totems) does between two RecordFight calls, for that fight's [dps] /
// [dpsparts] lines only (nothing stored, no decision reads it): every hit with its overkill (the formula is the
// damage it deals, a kill's last hit is dealt in full), the hits it takes, and its casts by spell.
// NoteDealt from the UnitScript DealDamage hook (before the victim's health changes), NoteCast from
// PlayerScript::OnPlayerSpellCast (a cast that went off)
void NoteDealt(Unit* attacker, Unit* victim, uint32 damage, uint8 damageType);
void NoteCast(Player* player, SpellInfo const* info);
}  // namespace DpsEstimate

#endif
