/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RogueTriggers.h"
#include "GenericTriggers.h"
#include "Playerbots.h"
#include "RaidUtilityAssignment.h"
#include "RoguePoisons.h"
#include "GenericBuffUtils.h"
#include "ServerFacade.h"
#include "TargetValue.h"

namespace
{
constexpr uint32 SPELL_STEALTH = 1784;
constexpr uint32 SPELL_SPRINT_RANK_1 = 2983;
constexpr uint8 EXPOSE_ARMOR_MIN_COMBO_POINTS = 5;
constexpr uint8 RUPTURE_MIN_COMBO_POINTS = 4;
constexpr uint32 SPELL_SHADOW_DANCE = 51713;  // [b31_wr]
constexpr uint32 SPELL_PREMEDITATION = 14183;  // [b31_wr]
constexpr uint8 PREMEDITATION_MAX_COMBO_POINTS = 3;  // [b31_wr] its 2 points do not overflow 5
// [b31_wr]
constexpr uint32 SPELL_ADRENALINE_RUSH = 13750;
constexpr uint32 SPELL_KILLING_SPREE = 51690;
constexpr uint32 SPELL_BLADE_FLURRY = 13877;
constexpr uint32 SPELL_COLD_BLOOD = 14177;
constexpr uint32 SPELL_HUNGER_FOR_BLOOD = 51662;
constexpr uint32 ADRENALINE_RUSH_MIN_KILLING_SPREE_CD_MS = 15 * IN_MILLISECONDS;
constexpr float BLADE_FLURRY_CC_CHECK_RANGE = 8.0f;
constexpr uint8 SND_MIN_COMBO_POINTS = 2;
constexpr uint8 SND_EARLY_REFRESH_MIN_COMBO_POINTS = 4;  // a running SnD is only refreshed with 4+ points
constexpr int32 SND_PRE_REFRESH_MS = 6000;
constexpr float SND_MIN_LIFETIME_S = 6.0f;
constexpr int32 COLD_BLOOD_MIN_SND_MS = 2000;
constexpr uint32 FINISHER_ENERGY = 35;

// Whether the current target lives minSeconds more at the group's damage (true when it cannot be told)
bool TargetLivesFor(PlayerbotAI* botAI, float minSeconds)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    Unit* target = context->GetValue<Unit*>("current target")->Get();
    if (!target || !target->IsAlive())
        return true;

    float const groupDps = context->GetValue<float>("estimated group dps")->Get();
    return groupDps <= 0.0f || target->GetHealth() / groupDps >= minSeconds;
}
}

// bool AdrenalineRushTrigger::isPossible()
// {
//     return !botAI->HasAura("stealth", bot);
// }

bool UnstealthTrigger::IsActive()
{
    if (!botAI->HasAura("stealth", bot))
        return false;

    if (!AI_VALUE2(bool, "moving", "self target"))
        return false;

    if (!AI_VALUE(uint8, "attacker count"))
        return true;

    return botAI->GetMaster() &&
           ServerFacade::instance().IsDistanceGreaterThan(AI_VALUE2(float, "distance", "group leader"), 10.0f) &&
           AI_VALUE2(bool, "moving", "group leader");
}

bool StealthTrigger::IsActive()
{
    if (bot->HasAura(SPELL_STEALTH) || bot->IsInCombat() || bot->HasSpellCooldown(SPELL_STEALTH))
        return false;

    float distance = 30.f;

    Unit* target = AI_VALUE(Unit*, "enemy player target");
    if (target && !target->IsInWorld())
    {
        return false;
    }
    if (!target)
        target = AI_VALUE(Unit*, "grind target");

    if (!target)
        target = AI_VALUE(Unit*, "dps target");

    if (!target)
        return false;

    if (target && target->GetVictim())
        distance -= 10;

    if (target->isMoving() && target->GetVictim())
        distance -= 10;

    if (bot->InBattleground())
        distance += 15;

    if (bot->InArena())
        distance += 15;

    return target && ServerFacade::instance().GetDistance2d(bot, target) < distance;
}

bool SapTrigger::IsPossible() { return bot->GetLevel() > 10 && botAI->HasSpell("sap") && !bot->IsInCombat(); }

bool SprintTrigger::IsPossible() { return bot->HasSpell(SPELL_SPRINT_RANK_1); }

bool SprintTrigger::IsActive()
{
    if (bot->HasSpellCooldown(SPELL_SPRINT_RANK_1))
        return false;

    float distance = botAI->GetMaster() ? 45.0f : 35.0f;
    if (botAI->HasAura("stealth", bot))
        distance -= 10;

    bool targeted = false;

    Unit* dps = AI_VALUE(Unit*, "dps target");
    Unit* enemyPlayer = AI_VALUE(Unit*, "enemy player target");

    if (enemyPlayer && !enemyPlayer->IsInWorld())
    {
        return false;
    }
    if (dps)
        targeted = (dps == AI_VALUE(Unit*, "current target"));

    if (enemyPlayer && !targeted)
        targeted = (enemyPlayer == AI_VALUE(Unit*, "current target"));

    if (!targeted)
        return false;

    if ((dps && dps->IsInCombat()) || enemyPlayer)
        distance -= 10;

    return AI_VALUE2(bool, "moving", "self target") &&
           (AI_VALUE2(bool, "moving", "dps target") || AI_VALUE2(bool, "moving", "enemy player target")) && targeted &&
           (ServerFacade::instance().IsDistanceGreaterThan(AI_VALUE2(float, "distance", "dps target"), distance) ||
            ServerFacade::instance().IsDistanceGreaterThan(AI_VALUE2(float, "distance", "enemy player target"), distance));
}

bool ExposeArmorTrigger::IsActive()
{
    // Raid utility coordinator: Sunder and Expose do not stack, only the major armor owner applies it.
    ai::raidutil::RaidUtilityAssignment const& assignment = ai::raidutil::GetAssignment(botAI);
    if (assignment.valid && assignment.armorCoordinated && !assignment.majorArmorOwner)
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    return DebuffTrigger::IsActive() && !botAI->HasAura("sunder armor", target, false, false, -1, true) &&
           AI_VALUE2(uint8, "combo", "current target") >= EXPOSE_ARMOR_MIN_COMBO_POINTS;
}

// [b31_wr]
bool WeaponPoisonTrigger::IsActive()
{
    Item* const weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!weapon)
        return false;

    if (!weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
        return minTargetLifetime <= 0.0f ||
               AI_VALUE2(float, "estimated lifetime", "current target") >= minTargetLifetime;

    // [rot4] the Deadly hand still carries another poison (the old Wound rule): replaced when Deadly is in the bags
    if (RoguePoisons::KindFor(botAI, slot) == RoguePoisons::Kind::DEADLY && !RoguePoisons::HasDeadly(weapon) &&
        RoguePoisons::IsDeadlyItem(RoguePoisons::ItemFor(botAI, slot)))
        return true;

    return minRemainingMs && weapon->GetEnchantmentDuration(TEMP_ENCHANTMENT_SLOT) < minRemainingMs;
}

// [b31_wr]
bool AdrenalineRushTrigger::IsActive()
{
    if (bot->GetPower(POWER_ENERGY) > ROGUE_BURST_MAX_ENERGY || bot->HasAura(SPELL_KILLING_SPREE))
        return false;

    if (bot->HasSpell(SPELL_KILLING_SPREE) &&
        bot->GetSpellCooldownDelay(SPELL_KILLING_SPREE) < ADRENALINE_RUSH_MIN_KILLING_SPREE_CD_MS)
        return false;

    return BoostTrigger::IsActive();
}

// [b31_wr]
bool KillingSpreeTrigger::IsActive()
{
    if (bot->GetPower(POWER_ENERGY) > ROGUE_BURST_MAX_ENERGY || bot->HasAura(SPELL_ADRENALINE_RUSH))
        return false;

    return BoostTrigger::IsActive();
}

// [b31_wr]
bool BladeFlurryReadyTrigger::IsActive()
{
    if (!bot->HasSpell(SPELL_BLADE_FLURRY) || bot->HasSpellCooldown(SPELL_BLADE_FLURRY) ||
        bot->HasAura(SPELL_BLADE_FLURRY))
        return false;

    if (!AI_VALUE(Unit*, "current target") || !botAI->GetAura("slice and dice", bot))
        return false;

    // false as well when a crowd-controlled attacker stands within the range (TargetValue.cpp veto)
    return HasEnoughAoeTargets(botAI, bot->GetPosition(), BLADE_FLURRY_CC_CHECK_RANGE, 1);
}

// [b31_wr]
bool SliceAndDicePreRefreshTrigger::IsActive()
{
    if (AI_VALUE2(uint8, "combo", "current target") < SND_EARLY_REFRESH_MIN_COMBO_POINTS)
        return false;

    Aura* aura = botAI->GetAura("slice and dice", bot);
    if (!aura || aura->GetDuration() >= SND_PRE_REFRESH_MS)
        return false;

    return TargetLivesFor(botAI, SND_MIN_LIFETIME_S);
}

// [b31_wr]
bool AssassinationRuptureTrigger::IsActive()
{
    if (!bot->HasSpell(SPELL_HUNGER_FOR_BLOOD) || !AI_VALUE2(uint8, "combo", "current target"))
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsAlive() || target->HasAuraState(AURA_STATE_BLEEDING))
        return false;

    Aura* hungerForBlood = botAI->GetAura("hunger for blood", bot);
    return !hungerForBlood || hungerForBlood->GetDuration() < static_cast<int32>(HUNGER_FOR_BLOOD_REFRESH_MS);
}

// [b31_wr]
bool ColdBloodFinisherTrigger::IsActive()
{
    if (!bot->HasSpell(SPELL_COLD_BLOOD) || bot->HasSpellCooldown(SPELL_COLD_BLOOD) || bot->HasAura(SPELL_COLD_BLOOD))
        return false;

    if (AI_VALUE2(uint8, "combo", "current target") < 5 || bot->GetPower(POWER_ENERGY) < FINISHER_ENERGY)
        return false;

    Aura* sliceAndDice = botAI->GetAura("slice and dice", bot);
    if (!sliceAndDice || sliceAndDice->GetDuration() < COLD_BLOOD_MIN_SND_MS)
        return false;

    // the Hunger for Blood enabler Rupture would take the points next
    Unit* target = AI_VALUE(Unit*, "current target");
    return target && (!bot->HasSpell(SPELL_HUNGER_FOR_BLOOD) || target->HasAuraState(AURA_STATE_BLEEDING));
}

bool HungerForBloodTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    return target && target->HasAuraState(AURA_STATE_BLEEDING) && BuffTrigger::IsActive();
}

// [b31_wr]
bool HemorrhageDebuffTrigger::IsActive()
{
    return AI_VALUE2(uint8, "combo", "current target") < 5 && DebuffTrigger::IsActive();
}

// [b31_wr]
bool PremeditationTrigger::IsActive()
{
    if (!bot->HasSpell(SPELL_PREMEDITATION) || bot->HasSpellCooldown(SPELL_PREMEDITATION))
        return false;

    if (!bot->HasAuraType(SPELL_AURA_MOD_STEALTH) && !bot->HasAura(SPELL_SHADOW_DANCE))
        return false;

    return AI_VALUE(Unit*, "current target") &&
           AI_VALUE2(uint8, "combo", "current target") <= PREMEDITATION_MAX_COMBO_POINTS;
}

// [rot27]
bool MutilateComboPointsTrigger::IsActive()
{
    return AI_VALUE(Unit*, "current target") &&
           AI_VALUE2(uint8, "combo", "current target") <= MUTILATE_MAX_COMBO_POINTS;
}

// [rot27]
bool VanishForOverkillTrigger::IsActive()
{
    constexpr uint32 SPELL_OVERKILL_TALENT = 58426;
    constexpr uint32 SPELL_OVERKILL_BUFF = 58427;
    if (!bot->IsInCombat() || !bot->HasSpell(SPELL_OVERKILL_TALENT) || bot->HasAura(SPELL_OVERKILL_BUFF) ||
        bot->HasAuraType(SPELL_AURA_MOD_STEALTH))
        return false;

    uint32 const vanish = AI_VALUE2(uint32, "spell id", "vanish");
    return vanish && !bot->HasSpellCooldown(vanish) && AI_VALUE(Unit*, "current target");
}

bool ShadowDanceTrigger::IsActive()
{
    // [rot26] wowsims shadow_dance.go: the dance at combo points <= 2 and energy >= 60 (was 70, any combo points)
    constexpr uint32 SHADOW_DANCE_MIN_ENERGY = 60;
    constexpr uint8 SHADOW_DANCE_MAX_COMBO_POINTS = 2;
    return bot->GetPower(POWER_ENERGY) >= SHADOW_DANCE_MIN_ENERGY &&
           AI_VALUE2(uint8, "combo", "current target") <= SHADOW_DANCE_MAX_COMBO_POINTS && BoostTrigger::IsActive();
}

// [melee_audit_b30] Slice and Dice fired at any combo point and outranked every finisher (HIGH+5): it was
// refreshed at 1-2 points whenever it fell off, so the points never reached Eviscerate / Envenom. Build 30:
// Combat 26-45 cast Slice and Dice 82 times against 2 Eviscerates, Combat 80 82 against 11, Assassination 80 132
// against 27 Envenoms. It now waits for 2 points (12 s+ of buff), and a target that dies within SND_MIN_LIFETIME_S
// gets the damage finisher instead (Eviscerate / Envenom on "target with combo points almost dead").
// [b31_wr] A 2-point cast only when the buff is gone; a running one (inside the refresh window) waits for 4 points,
// build 30 Combat refreshed it at 2 points 3.3 times a minute.
bool SliceAndDiceTrigger::IsActive()
{
    uint8 const comboPoints = AI_VALUE2(uint8, "combo", "current target");
    if (comboPoints < SND_MIN_COMBO_POINTS)
        return false;

    if (!TargetLivesFor(botAI, SND_MIN_LIFETIME_S))
        return false;

    Aura* aura = botAI->GetAura(spell, bot, checkIsOwner, checkDuration);
    if (!ai::buff::BuffBelowRefreshTarget(botAI, aura, beforeDuration))
        return false;

    return !aura || comboPoints >= SND_EARLY_REFRESH_MIN_COMBO_POINTS;
}

bool RuptureTrigger::IsActive()
{
    return AI_VALUE2(uint8, "combo", "current target") >= RUPTURE_MIN_COMBO_POINTS && DebuffTrigger::IsActive();
}

bool TricksOfTheTradeOnMainTankTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || target->IsPlayer())
        return false;  // players have no threat table

    Unit* mainTank = GetTarget();
    if (!mainTank || mainTank == bot)
        return false;

    return BuffOnMainTankTrigger::IsActive();
}
