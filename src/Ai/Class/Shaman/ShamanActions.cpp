/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ShamanActions.h"
#include "Action.h"
#include "Group.h"
#include "LeaderOrders.h"  // [lead149] AtCommandedSpot
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RaidUtilityAssignment.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TargetValue.h"
#include "TotemsShamanStrategy.h"

namespace
{
// [shside_enh_mana] Magma Totem pulses within 8 yd of itself (it lands at the shaman's feet); Fire Nova bursts from
// the fire totem and hits within 10 yd of it. Both are 3+ mob tools: on one or two mobs they cost more than they add.
constexpr float MAGMA_TOTEM_RADIUS = 8.0f;
constexpr float FIRE_NOVA_RADIUS = 10.0f;
constexpr uint32 FIRE_AOE_MIN_TARGETS = 3;
// The fight moved on when the standing totem is this far from the current target.
constexpr float AURA_TOTEM_REACH = 20.0f;

Creature* FindOwnTotemBySpell(Player* bot, uint32 spellId)
{
    if (!spellId)
        return nullptr;

    for (uint8 slot = SUMMON_SLOT_TOTEM_FIRE; slot < MAX_TOTEM_SLOT; ++slot)
    {
        if (bot->m_SummonSlot[slot].IsEmpty())
            continue;

        Creature* totem = bot->GetMap()->GetCreature(bot->m_SummonSlot[slot]);
        if (totem && totem->IsAlive() && totem->GetUInt32Value(UNIT_CREATED_BY_SPELL) == spellId)
            return totem;
    }

    return nullptr;
}

// [b31_deep] PlayerbotAI::CanCastSpell does not read the power cost: the mana for the known rank is checked here
// before a mana-heavy totem / nova is queued (else it is picked every tick and refused by the core).
bool HasManaForSpell(Player* bot, uint32 spellId)
{
    SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    return info && static_cast<int32>(bot->GetPower(POWER_MANA)) >= info->CalcPowerCost(bot, info->GetSchoolMask());
}
}  // namespace

namespace ai::shaman
{
// [b31_deep] See ShamanTriggers.h. A strategy lookup, the level, and on the low-level path a boss test and the mana.
bool AllowsSingleTargetFireAoe(PlayerbotAI* botAI, Unit* target)
{
    if (!target || !botAI->HasStrategy("enh", BOT_STATE_COMBAT))
        return false;

    Player* bot = botAI->GetBot();
    if (bot->GetLevel() >= FIRE_AOE_SINGLE_TARGET_LEVEL)
        return true;

    return ai::raidutil::IsBossUnit(target) && bot->GetPowerPct(POWER_MANA) >= FIRE_AOE_SINGLE_TARGET_BOSS_MANA_PCT;
}

bool TankHoldsTarget(Unit* target)
{
    Unit* victim = target ? target->GetVictim() : nullptr;
    Player* tank = victim ? victim->ToPlayer() : nullptr;
    return tank && PlayerbotAI::IsTank(tank) && target->GetThreatMgr().GetThreat(tank) > 0.0f;
}

bool MagmaTotemPays(PlayerbotAI* botAI, Unit* target)
{
    return AllowsSingleTargetFireAoe(botAI, target) ||
           HasEnoughAoeTargets(botAI, botAI->GetBot()->GetPosition(), MAGMA_TOTEM_RADIUS, FIRE_AOE_MIN_TARGETS);
}
}  // namespace ai::shaman

bool CastTotemAction::isUseful()
{
    if (botAI->HasAura(buff, bot))
        return false;

    // [lead149] b170 rfc-h 20261101-142639, user: «шаман поставил тотемы где-то в центре комнаты, а потом обратно
    // убежал» - the totems went down where the fight start found it (following the tank), then it walked to its fight
    // spot out of their range. A led member drops its totems at the spot the leader puts it, its walk done
    if (bot->IsInCombat() && !Leader::Events::AtCommandedSpot(bot))
        return false;

    if (!AI_VALUE2(bool, "has totem", name))
        return CastBuffSpellAction::isUseful();

    // User 2026-10-24: «танк прошёл, видит, что не получает тотемов — значит переставить на новом месте». The totem
    // still stands, but its aura ("strength of earth", "wrath of air totem", ...) no longer reaches the shaman: the
    // group moved on. In a fight, drop it again where the group is now.
    if (!auraTotem || !bot->IsInCombat())
        return false;

    // [shside_enh_mana] ...but only when the fight itself moved: the shaman stepping out of the aura for a moment
    // (a runner, an interrupt at range, a hazard) while the mobs still stand at the totem is no reason to pay for it
    // again (scholo-h 20261025-153805-4: Windfury Totem 4x and Strength of Earth 2x in one 148 s pull).
    if (Creature* standing = FindOwnTotemBySpell(bot, AI_VALUE2(uint32, "spell id", name)))
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        if (target && standing->GetDistance(target) <= AURA_TOTEM_REACH)
            return false;
    }

    return CastBuffSpellAction::isUseful();
}

bool CastMagmaTotemAction::isUseful() {
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !bot->IsWithinMeleeRange(target))
        return false;

    // [shside_enh_mana] 27 % base mana every 20 s: only for a pack around the shaman, never a lone target.
    // [b31_deep] ...except the single-target case of ai::shaman::AllowsSingleTargetFireAoe (wowsims: Magma on one
    // target too, from level 70 on or on a boss with mana to spare).
    if (!ai::shaman::MagmaTotemPays(botAI, target) || !HasManaForSpell(bot, AI_VALUE2(uint32, "spell id", name)))
        return false;
    if (!ai::shaman::TankHoldsTarget(target))  // [lead149] the damaging totems only on a mob the tank holds
        return false;

    return CastTotemAction::isUseful() && !AI_VALUE2(bool, "has totem", name);
}

// User 2026-10-24, RFC live: «шаман ставит тотемы, а тотем вообще не в радиусе, не достаёт до моба». The totem lands at
// the shaman's feet and shoots only what is within its range: drop it only when the current target is in that range,
// and drop a new one when the standing fire totem can no longer reach the target.
bool CastSearingTotemAction::isUseful()
{
    constexpr float SEARING_TOTEM_RANGE = 20.0f;  // Searing Totem's Attack range
    constexpr float DROP_MARGIN = 2.0f;           // the target moves a little while the totem lands

    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsAlive() || !bot->IsInCombat())
        return false;

    // [lead149] at the leader's spot, on a mob the tank holds
    if (!Leader::Events::AtCommandedSpot(bot) || !ai::shaman::TankHoldsTarget(target))
        return false;

    if (bot->GetDistance(target) > SEARING_TOTEM_RANGE - DROP_MARGIN || !bot->IsWithinLOSInMap(target))
        return false;

    if (Creature* fireTotem = bot->GetMap()->GetCreature(bot->m_SummonSlot[1]))
        if (fireTotem->IsAlive() && fireTotem->GetDistance(target) <= SEARING_TOTEM_RANGE &&
            fireTotem->IsWithinLOSInMap(target))
            return false;  // the fire totem standing there still reaches the target

    return CastBuffSpellAction::isUseful() && !botAI->HasAura(buff, bot);
}

// [b31_sd] The range that matters is totem-to-target (8 yd) and the pack around the totem (3+ within its 10 yd
// burst), not shaman-to-target: the action is cast on self, CastSpellAction::isUseful() below checks the spell only.
bool CastFireNovaAction::isUseful() {
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsAlive() || !bot->IsInCombat())
        return false;

    Creature* fireTotem = bot->GetMap()->GetCreature(bot->m_SummonSlot[1]);
    if (!fireTotem)
        return false;

    if (!fireTotem->IsAlive() || target->GetDistance(fireTotem) > 8.0f)
        return false;

    // [shside_enh_mana] 22 % base mana: only when the nova hits 3+ mobs around the fire totem.
    // [b31_deep] ...or in the Enhancement single-target case (wowsims: Fire Nova on cooldown, one target too).
    if (!ai::shaman::AllowsSingleTargetFireAoe(botAI, target) &&
        !HasEnoughAoeTargets(botAI, fireTotem->GetPosition(), FIRE_NOVA_RADIUS, FIRE_AOE_MIN_TARGETS))
        return false;

    if (!HasManaForSpell(bot, AI_VALUE2(uint32, "spell id", spell)))
        return false;

    return CastSpellAction::isUseful();
}

bool CastCleansingTotemAction::isUseful()
{
    return CastTotemAction::isUseful() && !AI_VALUE2(bool, "has totem", "mana tide totem");
}

// Will only cast Stoneclaw Totem if low on health and not in a group
bool CastStoneclawTotemAction::isUseful()
{
    return !bot->GetGroup();
}

// Chain Heal only pays off when its jumps land on injured players: the target plus two more nearby in a raid, plus
// one more in a party. [healer_audit_b30] In a five-man the first jump alone (tank + melee) already beats Lesser
// Healing Wave per mana; with 3 required the build 30 resto shamans at 80 cast it 5 times in 1455 s of combat.
bool CastChainHealAction::isUseful()
{
    constexpr float CHAIN_HEAL_JUMP_RANGE = 12.5f;

    if (!HealPartyMemberAction::isUseful())
        return false;

    Unit* target = GetTarget();
    Group* group = bot->GetGroup();
    if (!target || !group)
        return false;

    uint8 const MIN_INJURED = group->isRaidGroup() ? 3 : 2;

    uint8 injured = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || !member->IsInMap(target))
            continue;

        if (member->GetHealthPct() >= sPlayerbotAIConfig.AlmostFullHealth)
            continue;

        if (member != target && !member->IsWithinDist(target, CHAIN_HEAL_JUMP_RANGE))
            continue;

        if (++injured >= MIN_INJURED)
            return true;
    }

    return false;
}

namespace
{
    constexpr uint32 FLAME_SHOCK_SPELL_IDS[] = { 8050, 8052, 8053, 10447, 10448, 29228, 25457, 49232, 49233 };
    // Server update and travel-estimate slack on top of the Lava Burst cast and flight time.
    constexpr int32 LAVA_BURST_LANDING_SLACK_MS = 200;
    // Earth Shock and Flame Shock share this cooldown (6 s, 5 s with Reverberation; the longer one is assumed).
    constexpr int32 SHOCK_COOLDOWN_MS = 6 * IN_MILLISECONDS;

    Aura* GetOwnFlameShock(Unit* target, ObjectGuid casterGuid)
    {
        for (uint32 spellId : FLAME_SHOCK_SPELL_IDS)
        {
            if (Aura* aura = target->GetAura(spellId, casterGuid))
                return aura;
        }

        return nullptr;
    }
}

// Lava Burst crits for sure only on a target that still has this shaman's Flame Shock when the bolt lands: a cast
// started on a Flame Shock about to drop (it is refreshed 1 s before it ends) landed without it, and Lava Burst hit
// for 1.07x a Lightning Bolt instead of ~1.7x (DPS audit 2026-10-24).
bool CastLavaBurstAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    Aura* flameShock = GetOwnFlameShock(target, bot->GetGUID());
    if (!flameShock)
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(AI_VALUE2(uint32, "spell id", spell));
    if (!spellInfo)
        return true;

    int32 landingMs = static_cast<int32>(spellInfo->CalcCastTime(bot)) + LAVA_BURST_LANDING_SLACK_MS;
    if (spellInfo->Speed > 0.0f)
        landingMs += static_cast<int32>(bot->GetDistance(target) / spellInfo->Speed * IN_MILLISECONDS);

    return flameShock->GetDuration() >= landingMs;
}

// Moving, a caster has only its instants: Earth Shock while this shaman's Flame Shock outlasts the shared shock
// cooldown, so the Flame Shock refresh is never held back by it.
bool CastEarthShockOnTheMoveAction::isUseful()
{
    if (!bot->isMoving() || !CastSpellAction::isUseful())
        return false;

    Unit* target = GetTarget();
    if (!target)
        return false;

    Aura* flameShock = GetOwnFlameShock(target, bot->GetGUID());
    return flameShock && flameShock->GetDuration() > SHOCK_COOLDOWN_MS;
}

// [b31_sd] 3.3.5 Enhancement: Flame Shock upkeep, Earth Shock in between. Earth Shock now leaves the shared cooldown
// free when the Flame Shock refresh (last FLAME_SHOCK_REFRESH_WINDOW_MS) comes due: it goes out only while the
// own Flame Shock has more than the shock cooldown plus a second left. Without a Flame Shock on the target it goes
// out only when none is coming: the spell is not known, or the target is below Flame Shock's lifetime gate (the
// same health / "estimated group dps" test as FlameShockTrigger). Two aura lookups and a cached value.
bool CastEarthShockBetweenFlameShocksAction::isUseful()
{
    constexpr int32 EARTH_SHOCK_FLAME_SHOCK_MARGIN_MS = 1 * IN_MILLISECONDS;

    if (!CastSpellAction::isUseful())
        return false;

    Unit* target = GetTarget();
    if (!target || !target->IsAlive())
        return false;

    if (Aura* flameShock = GetOwnFlameShock(target, bot->GetGUID()))
        return flameShock->GetDuration() > SHOCK_COOLDOWN_MS + EARTH_SHOCK_FLAME_SHOCK_MARGIN_MS;

    if (!AI_VALUE2(uint32, "spell id", "flame shock"))
        return true;

    return (target->GetHealth() / AI_VALUE(float, "estimated group dps")) < FLAME_SHOCK_MIN_LIFETIME_S;
}

// [b31_deep] See the header: the current target's own Flame Shock must outlast the shared shock cooldown plus a
// second; with none on it the "flame shock" trigger (5.3, above this) owns the cooldown. Two aura lookups.
bool CastFlameShockOnAttackerAction::isUseful()
{
    constexpr int32 FLAME_SHOCK_MULTIDOT_MARGIN_MS = 1 * IN_MILLISECONDS;

    if (!CastDebuffSpellOnAttackerAction::isUseful())
        return false;

    Unit* current = AI_VALUE(Unit*, "current target");
    Unit* target = GetTarget();
    if (!current || !target || current == target)
        return false;

    Aura* mainFlameShock = GetOwnFlameShock(current, bot->GetGUID());
    return mainFlameShock && mainFlameShock->GetDuration() > SHOCK_COOLDOWN_MS + FLAME_SHOCK_MULTIDOT_MARGIN_MS;
}

// Logic for making a guardian (spirit wolf) use a spell (spirit walk)
// There is no existing code for guardians casting spells in the AC/Playerbots repo.
bool CastSpiritWalkAction::Execute(Event /*event*/)
{
    constexpr uint32 SPIRIT_WOLF = 29264;
    constexpr uint32 SPIRIT_WALK_SPELL = 58875;

    for (Unit* unit : bot->m_Controlled)
    {
        if (unit->GetEntry() == SPIRIT_WOLF && unit->HasSpell(SPIRIT_WALK_SPELL))
        {
            unit->CastSpell(unit, SPIRIT_WALK_SPELL, false);
            return true;
        }
    }

    return false;
}

// Set Strategy Assigned Totems (Actions) - First, it checks
// the highest-rank spell the bot knows for each totem type,
// then adds it to the Call of the Elements bar.
bool SetTotemAction::Execute(Event /*event*/)
{
    uint32 totemSpell = 0;
    for (size_t i = 0; i < totemSpellIdsCount; ++i)
    {
        if (bot->HasSpell(totemSpellIds[i]))
        {
            totemSpell = totemSpellIds[i];
            break;
        }
    }

    if (!totemSpell)
        return false;

    if (ActionButton const* button = bot->GetActionButton(actionButtonId);
        button && button->GetType() == ACTION_BUTTON_SPELL &&
        button->GetAction() == totemSpell)
    {
        return false;
    }

    bot->addActionButton(actionButtonId, totemSpell, ACTION_BUTTON_SPELL);
    return true;
}

bool ApplyTotemAssignmentAction::isUseful() { return !bot->IsInCombat() && bot->GetGroup(); }

bool ApplyTotemAssignmentAction::Execute(Event /*event*/)
{
    std::string changes;
    for (uint8 element = 0; element < ai::raidutil::TOTEM_ELEMENT_COUNT; ++element)
    {
        std::string const strategy = ai::shaman::GetTotemAssignmentChange(botAI, element);
        if (strategy.empty())
            continue;

        if (!changes.empty())
            changes += ",";
        changes += "+" + strategy;
    }

    if (changes.empty())
        return false;

    // Totem strategies of one element are siblings, so adding one replaces the previous one.
    botAI->ChangeStrategy(changes, BOT_STATE_COMBAT);
    return true;
}

bool CastEarthShieldOnBattleResurrectedTankAction::isUseful()
{
    Unit* target = GetTarget();
    Player* player = target ? target->ToPlayer() : nullptr;
    return player && PlayerbotAI::IsTank(player) && CastBuffOnValueTargetAction::isUseful();
}

// [b31_sd] The base isUseful() (CastBuffSpellAction) refuses while any Earth Shield is on the tank; the trigger also
// fires on this shaman's own shield down to EARTH_SHIELD_RECAST_CHARGES, and the recast then goes out.
bool CastEarthShieldOnMainTankAction::isUseful()
{
    if (BuffOnMainTankAction::isUseful())
        return true;

    Unit* target = GetTarget();
    if (!target || !target->IsAlive() || !CastSpellAction::isUseful())
        return false;

    Aura* own = botAI->GetAura(spell, target, true);
    return own && own->GetCharges() <= EARTH_SHIELD_RECAST_CHARGES;
}

bool CastCallOfTheElementsAction::isUseful()
{
    // [lead149] the four totems at the spot the leader puts the member, its walk done
    if (bot->IsInCombat() && !Leader::Events::AtCommandedSpot(bot))
        return false;
    return CastSpellAction::isUseful();
}
