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
    float pushHitsPerSec = 0.0f;  // [death457] the landed hits on it a second its casts' pushback is priced at
};

// `target`: the unit it fights (its level, armor, avoidance and auras are read); null: a mob of its level
// (creature_classlevelstats armor, the core's creature avoidance). fightSec 0: the burst. [death457] pushHitsPerSec >= 0:
// the landed hits on the bot a second (every mob on it, MeleeHitsPerSec summed) its casts' pushback is priced at; < 0:
// its target's alone
Result Estimate(Player* bot, Unit* target, BuffContext ctx, float fightSec = 0.0f, float pushHitsPerSec = -1.0f);
// [death457] a mob's landed melee hits on the bot a second when it attacks it (its swing time, its miss / dodge / parry
// against the bot): the pushback rate it adds to the bot's casts
float MeleeHitsPerSec(Player* bot, Unit* mob);
float EstimateDps(Player* bot, Unit* target, BuffContext ctx, float fightSec = 0.0f);
float EstimateTps(Player* bot, Unit* target, BuffContext ctx);
// [lead1309] the threat it builds on `target` within the next `horizonSec`: its rotation's tps, a rage user's spells
// held to the rage it has now plus what its swings landing within the horizon bring (a warrior / bear opens at 0
// rage: b191 rfc-h warrior tanks ~740 tps in a pull's first 2 s against ~1690 later). Mana and rune users: the tps
// [lead1310] `takenDamage`: the damage it takes within the horizon, whose rage (Unit::RewardRage) a rage user spends
float ThreatWithin(Player* bot, Unit* target, BuffContext ctx, float horizonSec, float takenDamage = 0.0f);
// [lead1310] the threat its area spells build within `horizonSec` on `mob`, one it does not target (`target`: the one
// it does, its profile): `areaDps` (its area spells' damage a target a second) held to its rotation's area parts that
// reach the mob from where it stands, at its threat per damage (tps / dps), never above its spells' threat rate, and
// a rage user's to the same rage within the horizon as ThreatWithin
float AreaThreatWithin(Player* bot, Unit* target, Unit* mob, BuffContext ctx, float horizonSec, float areaDps,
                       float takenDamage = 0.0f);

// [rot603] A cooldown nuke cast only where it pays (Fire Blast, Mind Blast while levelling): one cast's damage on
// `target` (SpellPayoff::Read) at least the filler's in the cast's execute time (SpellPayoff::FillerDps). The bot's
// "fire blast pays" / "mind blast pays" triggers and the formula read this one rule. True when no filler is known.
bool CastPays(Player* bot, Unit* target, uint32 spellId, uint32 fillerSpellId);

// [rot609] One filler's damage per second on `target` from the bot's state now, as the formula reads it (the core's
// numbers: its land chance, crit, armor, its own DoT refreshed cast after cast, the pushback of the target's hits on
// it). The bot picks the stronger of two fillers by it (Fireball over Frostbolt) and the formula by the same rule.
// b341 frost mages: SpellPayoff::FillerDps (direct hits only, a measured mean per hit) picked the filler the formula
// read 10-25 % weaker (ideal / formula 1.12)
float FillerDps(Player* bot, Unit* target, uint32 spellId);

// [rot610] The target's remaining life by the formula: its health over the bot's formula dps times the members on it
// (its own included). No fight history (user 2026-11-08); max when the bot deals nothing.
float RemainingLifeSec(Player* bot, Unit* target);

// [rot610] A DoT or debuff `spellId` on `target` pays (replaces the fixed 8 s needLifeTime gate of DebuffTrigger and
// CastDebuffSpellAction). A DoT (its periodic part above its direct one): its direct part and the ticks that land
// within RemainingLifeSec beat what the same resource buys in its rotation - the best damage per rage / energy of its
// other rage / energy spells (Heroic Strike, Mortal Strike, a builder-finisher cycle), or its filler in the DoT's
// execute time for a mana spell. Any other debuff (its worth is the debuff, not its hit): the target outlives its cast.
// b341: arms warriors never cast Rend (0 casts in every log b330-b341): Rend's rage came after the mob fell under
// the 8 s gate.
bool DebuffPays(Player* bot, Unit* target, uint32 spellId);

char const* ContextName(BuffContext ctx);
// a solo bot outside a group: Solo; a party: Group; a raid group: Raid. [deaths9] A grouped bot with no living
// member within the core's group reward distance (Player::IsAtGroupRewardDistance) fights alone: Solo (b345
// Guinudrond L11, a user pack's leader questing hundreds of yards from its members, was judged with their buffs)
BuffContext ContextOf(Player* bot);

// [deaths9] alone out in the world (SoloSafety::SoloPlayerBot's rule): not in an instance, no group or no living
// member within the group reward distance
bool FightsAlone(Player* bot);

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
    // [dps41] why a melee bot's white swings fall short of the formula's: [dps238] measured melee white at 0.66-0.85
    // of it at b426-b429 and b479 still reads 0.75 for the guild's prot warriors (the dominant term of their 81 % of
    // the ideal, their rotation being 92 % of it), with nothing in the log saying where the swings went. The formula
    // has a melee bot in reach of its target for the whole fight (`meleeShare` is 1 for every melee rotation), so
    // every one of these is time it counted swings the bot could not take. Sampled where meleeMs and actMs already
    // are - at the fight's damage events, the interval attributed to the state seen at the one before - and together
    // they tile `windowSec` below, which is what the allowance divides.
    float ableSec = 0.0f;     // in reach, facing, swinging, free to act: the time the swing timer could run
    float rangeSec = 0.0f;    // out of melee reach of the mob it fought
    float facingSec = 0.0f;   // in reach, not facing it (Unit::isInFront: a swing is refused)
    float noSwingSec = 0.0f;  // in reach and facing, no melee attack under way (no victim / attack stopped)
    float controlSec = 0.0f;  // stunned, feared, confused, rooted or fleeing: not its own to spend
    float switchSec = 0.0f;   // swinging at a unit that is no attacker of this fight
    // [dps50] the window the six buckets above tile, and the lead swing inside it. The buckets are sampled at the
    // fight's damage events, so they span the first event to the last one; the fight the allowance is compared
    // against runs one swing longer, because the swing landing at the first event was swung for over the swing timer
    // before it (CloseFight's `seconds`). That lead was in no bucket, and it is the whole of the undercount measured
    // on b480: the unaccounted part of every [meleeup] line was the bot's own swing time to the centisecond, ratio
    // 1.00 for all nine specs, 18.6 % of the log's fight time. It is charged to the reason the first sample reads,
    // and `windowSec` is span + `leadSec` - the swing speed at the open, which a druid's shift changes, so the
    // window is carried and not recomputed from the speed at the close.
    float windowSec = 0.0f;
    float leadSec = 0.0f;
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
