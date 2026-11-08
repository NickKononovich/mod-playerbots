/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PALADINHELPER_H
#define PLAYERBOTS_PALADINHELPER_H

#include "Player.h"
#include "Unit.h"
#include <initializer_list>

namespace ai::paladin
{
static constexpr uint32 SPELL_HAND_OF_PROTECTION = 1022;
static constexpr uint32 SPELL_HAND_OF_SALVATION = 1038;
static constexpr uint32 SPELL_HAND_OF_FREEDOM = 1044;
static constexpr uint32 SPELL_HAND_OF_SACRIFICE = 6940;
static constexpr uint32 SPELL_GUARDED_BY_THE_LIGHT_R1 = 53583;
static constexpr uint32 SPELL_GUARDED_BY_THE_LIGHT_R2 = 53585;

// [rot150] Guarded by the Light (Spell.dbc 53583 / 53585: effect 2 PROC_TRIGGER_SPELL 63521 on a melee swing or melee
// spell hit, 50 / 100 %; spell_pal_guarded_by_the_light refreshes Divine Plea, whose 5 % of max mana ticks every 3 s go
// on). Rawr ClassTalents.cs:2037-2041. A tank with it keeps one Divine Plea up for the whole fight by hitting.
// [lead1071] b230-b232: never true - a passive talent is in the core's talent map only
// (Player::_addTalentAurasAndSpells casts its aura, Player::HasSpell reads m_spells), so the Plea gates [rot150]
// [lead1040] [lead1063] never ran (0 [lead1040] lines in three batches) and the tank cast Plea whenever its mana was
// not full. Player::HasTalent on the active spec reads the talent
inline bool KeepsDivinePleaUpByHits(Player* bot)
{
    return bot && (bot->HasTalent(SPELL_GUARDED_BY_THE_LIGHT_R1, bot->GetActiveSpec()) ||
                   bot->HasTalent(SPELL_GUARDED_BY_THE_LIGHT_R2, bot->GetActiveSpec()));
}

// [lead1040] a Guarded by the Light tank's Divine Plea now: in a boss fight; on trash only when its recharge is back
// before the next boss pull (the leader's estimate, the cooldowns-for-bosses rule); [rot500] never out of a fight (no
// melee hit refreshes it there: b257 5E every pre-pull cast ran out before the first swing on the boss)
bool TankPleaNow(Player* bot);

// [kzk1] the judgement debuff this paladin keeps on the boss of its fight (dungeon, raid or world boss): one paladin of
// the group keeps Judgement of Wisdom on him (the raid's mana), a second one Judgement of Light. Computed from the
// group each call: the paladins alive in the boss's fight within Judgement's range of him, ranked damage dealer, then
// healer, then tank (a tank judges its adds for threat), then the lower guid. "judgement of wisdom" / "judgement of
// light", nullptr: none (no boss, not ranked); boss is set to him. Logs "[judge] ..." on an owner change
char const* BossJudgementDuty(Player* bot, Unit*& boss);

inline bool HasHandFromCaster(Unit* target, Unit* caster, std::initializer_list<uint32> spellIds)
{
    if (!target || !caster)
        return false;

    for (uint32 spellId : spellIds)
    {
        if (target->HasAura(spellId, caster->GetGUID()))
            return true;
    }

    return false;
}

inline bool HasAnyPaladinHandFromCaster(Unit* target, Unit* caster)
{
    return HasHandFromCaster(target, caster,
        { SPELL_HAND_OF_PROTECTION, SPELL_HAND_OF_SALVATION, SPELL_HAND_OF_FREEDOM, SPELL_HAND_OF_SACRIFICE });
}

// [b33_class] What Hand of Freedom can actually clear: a root or a snare (movement-impairing). PlayerbotAI::
// IsMovementImpaired also reports stuns and knockdowns (IsRooted() / zero run speed), which Hand of Freedom cannot
// remove: the paladin burned the GCD on them. A stunned target is skipped even when also snared.
inline bool HasFreedomRemovableImpairment(Unit* unit)
{
    if (!unit || unit->HasUnitState(UNIT_STATE_STUNNED) || unit->HasAuraType(SPELL_AURA_MOD_STUN))
        return false;

    return unit->HasAuraType(SPELL_AURA_MOD_ROOT) || unit->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ||
           unit->HasAuraWithMechanic((1ULL << MECHANIC_ROOT) | (1ULL << MECHANIC_SNARE));
}
}

#endif
