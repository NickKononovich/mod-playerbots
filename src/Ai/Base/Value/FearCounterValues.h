/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FEARCOUNTERVALUES_H
#define PLAYERBOTS_FEARCOUNTERVALUES_H

#include "ObjectGuid.h"
#include "Value.h"

class Creature;
class Player;
class PlayerbotAI;
class Unit;
struct FearCounterState;

// [fear_counter] Generic fear / charm / sleep counters (user 2026-10-25, mara-h 20261025-163739-17: Princess
// Theradras' Repulsive Gaze 21869 feared the tank while the shaman kept Strength of Earth up the whole fight).
// One per-bot state value answers three questions every 500 ms:
// - who of the party is under a fear, charm or sleep effect right now within Tremor Totem reach of the bot;
// - does this fight have a fear source: an attacker whose creature spells / SmartAI cast one, an attacker casting
//   one now, a C++-scripted boss from a short table, or a party member already feared in this fight;
// - is such a cast in progress near the bot (for a pre-emptive Berserker Rage).
// The shaman, priest, warrior and PvP-trinket counters of FearCounterStrategy read only this cached value.
namespace FearCounter
{
enum Threat : uint8
{
    THREAT_NONE = 0,
    THREAT_TREMOR = 1 << 0,    // fear, charm or sleep: what Tremor Totem removes
    THREAT_FEAR = 1 << 1,      // a real fear: what Fear Ward absorbs and Berserker Rage prevents
    THREAT_INCOMING = 1 << 2,  // a fear / charm / sleep cast is in progress within reach of the bot
};

constexpr uint32 SPELL_TREMOR_TOTEM = 8143;
constexpr uint32 SPELL_FEAR_WARD = 6346;
constexpr uint32 SPELL_BERSERKER_RAGE = 18499;
constexpr float TREMOR_RADIUS = 30.0f;      // Tremor Totem pulse radius (8146)
constexpr float TREMOR_DROP_REACH = 28.0f;  // the anchor may move while the totem lands at the shaman's feet
constexpr float FEAR_WARD_RANGE = 30.0f;
constexpr float INCOMING_RADIUS = 30.0f;    // most boss fears are 10-30 yd roars or targeted 30 yd casts

// Threat flags of one spell (and of the spell it triggers, one level deep).
uint8 SpellThreat(uint32 spellId);
// Threat flags of a creature entry: template spells, SmartAI casts, known C++ boss scripts (cached per entry).
uint8 CreatureThreat(Creature* creature);
// Remember that this entry was seen casting a fear-type spell (C++ scripts have no spell list to read).
void LearnCreatureThreat(uint32 entry, uint8 threat);
// The spell id of a fear / charm / sleep aura on the unit, 0 when none. fearOnly: only SPELL_AURA_MOD_FEAR;
// caster (optional) receives the aura caster.
uint32 TremorableAuraSpell(Unit* unit, bool fearOnly = false, ObjectGuid* caster = nullptr);
// True when the bot's own Tremor Totem stands and reaches the anchor (nullptr: it just stands).
bool TremorCovers(Player* bot, Unit* anchor);
// The unit a Tremor Totem dropped now must reach: the victim (reactive), else the tank when the bot stands close
// enough to cover him, else the bot itself. Tremor lands at the shaman's feet: nothing moves him for it.
Unit* TremorAnchor(PlayerbotAI* botAI, FearCounterState const& state, bool reactive);
// One "[fear_counter]" LOG_INFO line plus a "fear_counter" test-harness JSONL event.
void Log(Player* bot, std::string const& action, Unit* victim, uint32 auraSpell);
}  // namespace FearCounter

struct FearCounterState
{
    ObjectGuid victim;        // a party member under fear / charm / sleep within Tremor reach of the bot
    uint32 victimAura = 0;    // its aura's spell id
    ObjectGuid tank;          // the main tank (else the first tank) of the group on this map
    ObjectGuid source;        // the unit the threat was read from (logged)
    uint32 sourceSpell = 0;   // the spell seen in progress or in its list, when known
    uint8 threat = FearCounter::THREAT_NONE;
};

class FearCounterStateValue : public CalculatedValue<FearCounterState>
{
public:
    FearCounterStateValue(PlayerbotAI* botAI) : CalculatedValue<FearCounterState>(botAI, "fear counter state", 500) {}

    FearCounterState Calculate() override;

private:
    uint8 _seenThisFight = FearCounter::THREAT_NONE;  // a party member was already feared / charmed in this fight
    uint32 _lastInCombatMs = 0;                       // the last in-combat calculation (the value is only read in
                                                      // combat by the shaman / warrior: a gap is a new fight)
};

#endif
