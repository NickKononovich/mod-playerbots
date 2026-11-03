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

#include "Common.h"
#include <string>
#include <vector>

#include "HazardZone.h"

class Player;
class Unit;

namespace Leader
{
class LeaderGroup;
}

namespace Leader::HazardVerdict
{
struct Facts
{
    LeaderGroup* group = nullptr;
    Player* tank = nullptr;
    std::vector<Player*> const* seats = nullptr;  // the members without the tank
    Unit* fightTarget = nullptr;                  // the fight's target (a member's melee target when it has none)
    float fightSecLeft = 0.0f;
    bool bossFight = false;
    bool fighting = false;  // out of a fight a move costs no damage dealt: the member leaves
};

struct Verdict
{
    bool stays = false;
    std::string why;  // the numbers: "stays: 3.2k hit vs 9.1k dps lost (...), health 21k -> 17k, heal covers in 1.8 s"
};

// `member` stands in `zone`: stay or leave
Verdict JudgeZone(Facts const& facts, Player* member, HazardZone const& zone);

// `member` stands in `cleaver`'s frontal cone, `exitYd` from the nearest spot out of it: stay or leave
Verdict JudgeCleave(Facts const& facts, Player* member, Unit* cleaver, float exitYd);
}  // namespace Leader::HazardVerdict

#endif
