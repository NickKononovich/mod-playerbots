/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_LEADERHAZARDVERDICT_H
#define PLAYERBOTS_LEADER_LEADERHAZARDVERDICT_H

// [lead780] User rule (hazard-stay-or-leave, 2026-11-03): for every avoidable damage source the leader decides
// stay vs leave by computation, for every member (tank, healer, damage dealers) and every hazard (area casts such as
// Fire Nova, ground areas, traps, trigger NPCs' pulses, frontal cleaves). Leaving costs the damage the member does
// not deal meanwhile: the seconds out (the walk out of the circle, the wait for a cast to land / an area its melee
// target stands in to end, the walk back for melee) times its measured dps (GroupDamage::MemberSustainedDps), plus
// its rotation's loss - its own damage buffs (SPELL_AURA_MOD_DAMAGE_PERCENT_DONE: Savage Roar) and its damage over
// time on its target that run out while it is away and stay off until it is back. Staying costs the damage the
// hazard deals to it: the spell's data (Spell.dbc effects at the caster's level, SpellEffectInfo::CalcValue) with
// the caster's and the member's spell bonuses (SpellDamageBonusDone / Taken), the instance's spell factor (the
// rescale's ModifySpellDamageTaken / ModifyPeriodicDamageAurasTick hooks, mod-raid-rescale: RFC heroic x30) and
// the member's armor (Unit::CalcArmorReducedDamage) or average resistance (Unit::GetEffectiveResistChance). The
// member stays only when staying costs less AND it survives (health now - the hit - its measured damage taken
// (Support::MemberTakenPerSec) and the hazard's own ticks over the healer's gap between two heals > 0) AND the
// healers cover it (their heal rate, RestState::HealerThroughput, above the group's measured damage taken plus the
// hazard's ticks; the healer's mana after healing it back still at its fight line, LeaderMoveManaLine). A cast the
// member cannot walk out of before it lands is stayed in (the walk saves nothing). Without spell data (a
// DungeonHazard circle, a cleaver without a cone spell) or without a measured dps the member leaves.
// [lead820] User decision 2026-11-03 (hazard-stay-or-leave, healer mana): "the healer can heal it, but the leader must
// also count that his mana lasts - on long bosses it means leaving more". The healer's mana is judged on every hit of
// the hazard still to come in the fight, not on the one in front of the member: the hazard's cadence (the caster's
// encounter timer - its SmartAI in-combat repeat -, else the measured interval of its previous casts in this fight,
// else the spell's cooldown) over the fight's seconds left (the boss / pack estimate, lead80), plus the heal the other
// members that stay in the same hazard already take from that healer, on top of the healer's mana line for the
// fight. A healer that cannot afford the stay to the fight's end: the member leaves. The verdict is judged anew on
// every hazard scan, so a late hit (fewer of them left) can be stayed in where an early one was not.

#include "Common.h"
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "HazardZone.h"
#include "ObjectGuid.h"

class Player;
class Unit;

namespace Leader
{
class LeaderGroup;
}

namespace Leader::HazardVerdict
{
// [lead820] the casts of a hazard spell the leader saw in this fight, per caster and spell: the first and the last
// start (getMSTime) and how many - the measured interval is (last - first) / (count - 1)
struct CastSeen
{
    uint32 firstMs = 0;
    uint32 lastMs = 0;
    uint32 count = 0;
};
using CastLog = std::map<std::pair<ObjectGuid, uint32>, CastSeen>;

// [lead820] the heal a member's stay asks of the main healer per hazard it stays in (all its hits to come)
using StayHeals = std::map<ObjectGuid, std::map<std::string, float>>;

// [lead820] notes the starts of the casts / areas in `zones` (the hazard scan, `viewer` the member they were collected
// for): a cast's start is now less its cast time gone (Spell::GetCastTime - GetCastTimeRemaining), an area's now less
// its duration gone; a start within the cast's / area's own length of the last one is the same cast
void NoteCasts(CastLog& log, Player* viewer, std::vector<HazardZone> const& zones, uint32 nowMs);

struct Facts
{
    LeaderGroup* group = nullptr;
    Player* tank = nullptr;
    std::vector<Player*> const* seats = nullptr;  // the members without the tank
    Unit* fightTarget = nullptr;                  // the fight's target (a member's melee target when it has none)
    float fightSecLeft = 0.0f;
    bool bossFight = false;
    bool fighting = false;  // out of a fight a move costs no damage dealt: the member leaves
    CastLog const* casts = nullptr;        // [lead820] the hazard casts seen in this fight
    StayHeals const* stayHeals = nullptr;  // [lead820] the other members' stays: the heal each asks
    uint32 nowMs = 0;
};

struct Verdict
{
    bool stays = false;
    std::string why;  // the numbers: "stays: 3.2k hit vs 9.1k dps lost (...), health 21k -> 17k, heal covers in 1.8 s"
    float heal = 0.0f;  // [lead820] the heal its stay asks of the main healer: this hit and every one to come
};

// `member` stands in `zone`: stay or leave
Verdict JudgeZone(Facts const& facts, Player* member, HazardZone const& zone);

// `member` stands in `cleaver`'s frontal cone, `exitYd` from the nearest spot out of it: stay or leave
Verdict JudgeCleave(Facts const& facts, Player* member, Unit* cleaver, float exitYd);
}  // namespace Leader::HazardVerdict

#endif
