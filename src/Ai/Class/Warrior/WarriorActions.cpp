/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WarriorActions.h"
#include "AiFactory.h"
#include "FearCounterValues.h"  // [fear_counter]
#include "Playerbots.h"
#include "RaidUtilityAssignment.h"
#include "TerrainHazard.h"
#include "WarriorTriggers.h"

namespace
{
constexpr uint32 SPELL_RETALIATION = 20230;
// [b31_deep] the immunity list (with the real Hand of Protection ranks) is WarriorHasShatterableImmunity
constexpr uint32 SPELL_VIGILANCE = 50720;
constexpr uint32 STRIKE_SOON_MS = 1500;                    // one global cooldown

// [b31_deep] Rage (x10) a filler must leave for the main strike ready within a global cooldown: Mortal Strike,
// Bloodthirst, or Whirlwind in Berserker Stance (Arms knows Whirlwind but cannot use it in Battle Stance). The
// largest cost of those; 0 when none is close. CanCastSpell ignores rage, so without this a 15 rage filler took the
// rage of a strike that came off cooldown a moment later (Fury build 30: Sunder 5.6 a minute, above Whirlwind).
int32 RageForStrikeSoon(PlayerbotAI* botAI, Player* bot)
{
    static char const* const strikes[] = { "mortal strike", "bloodthirst", "whirlwind" };
    AiObjectContext* context = botAI->GetAiObjectContext();
    int32 reserve = 0;
    for (char const* name : strikes)
    {
        if (name == strikes[2] && bot->GetShapeshiftForm() != FORM_BERSERKERSTANCE)
            continue;

        uint32 const spellId = context->GetValue<uint32>("spell id", name)->Get();
        if (spellId && bot->GetSpellCooldownDelay(spellId) < STRIKE_SOON_MS)
            reserve = std::max(reserve, WarriorRage::Cost(bot, spellId));
    }

    return reserve;
}

// Rage (x10) left for the strike above after a spell of this name.
bool LeavesRageForStrike(PlayerbotAI* botAI, Player* bot, char const* spell)
{
    int32 const reserve = RageForStrikeSoon(botAI, bot);
    if (!reserve)
        return true;

    uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", spell)->Get();
    return int32(bot->GetPower(POWER_RAGE)) >= WarriorRage::Cost(bot, spellId) + reserve;
}
constexpr uint32 SPELL_SHATTERING_THROW = 64382;
constexpr uint8 DEMORALIZING_SHOUT_SPARE_RAGE = 30;  // Thunder Clap (20) and the shout (10)
constexpr uint32 SPELL_WARBRINGER = 57499;
constexpr uint32 STANCE_DANCE_WINDOW_MS = 2 * IN_MILLISECONDS;   // a dance gets this long to reach the Charge
constexpr uint32 STANCE_DANCE_RETRY_MS = 10 * IN_MILLISECONDS;   // and is not begun again for this long
}

bool CastChargeAction::isPossible()
{
    if (CastWarriorGapCloserAction::isPossible())
        return true;

    // Only the stance is missing: out of combat (Charge is an opener), no Warbringer, not in Battle Stance yet.
    if (bot->IsInCombat() || bot->HasAura(SPELL_WARBRINGER) || botAI->HasAura("battle stance", bot) ||
        bot->HasUnitState(UNIT_STATE_ROOT))
        return false;

    Unit* target = GetTarget();
    uint32 const chargeId = AI_VALUE2(uint32, "spell id", "charge");
    SpellInfo const* info = chargeId ? sSpellMgr->GetSpellInfo(chargeId) : nullptr;
    if (!target || !info || bot->HasSpellCooldown(chargeId) ||
        !bot->IsWithinCombatRange(target, info->GetMaxRange(false, bot)) ||
        bot->IsWithinCombatRange(target, info->GetMinRange(false)) || !bot->IsWithinLOSInMap(target))
        return false;

    // A dance that did not end in a Charge (the stance flipped back, the cast failed) is not begun again at once:
    // the stance would flip on every tick of the walk in.
    uint32 const now = getMSTime();
    if (_stanceDanceMs)
    {
        uint32 const since = getMSTimeDiff(_stanceDanceMs, now);
        if (since >= STANCE_DANCE_WINDOW_MS && since < STANCE_DANCE_RETRY_MS)
            return false;
    }

    if (!botAI->CanCastSpell("battle stance", bot))
        return false;

    if (!_stanceDanceMs || getMSTimeDiff(_stanceDanceMs, now) >= STANCE_DANCE_RETRY_MS)
        _stanceDanceMs = now;

    return true;
}

bool CastWarriorGapCloserAction::isUseful()
{
    if (!CastReachTargetSpellAction::isUseful())
        return false;

    Unit* target = GetTarget();
    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!target || !spellInfo || !bot->IsWithinCombatRange(target, spellInfo->GetMaxRange(false, bot)))
        return false;

    // [b30_oculus] no charge onto a target in the air or down a ledge (GapCloserLeavesGround: occ-h 184229-26)
    if (GapCloserLeavesGround(bot, target, spell.c_str()))
        return false;

    // [b32_peel] a tank with mobs on it does not charge away from them onto a mob that is not at a group member
    if (!TankPeel::MayCloseGapOnTarget(bot, target))
        return false;

    // [bear_aoe_threat] as the bear's Feral Charge (DruidBearActions.cpp): a dungeon tank does not charge onto a spot
    // an idle pack reaches
    return !PlayerbotAI::IsTank(bot) || !botAI->IsInNonRaidDungeon() || !IdleMobReachingSpot(botAI, bot, target);
}

bool CastDemoralizingShoutTankAoeAction::isUseful()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (target && botAI->HasAura("demoralizing shout", target))
        return false;

    if (AI_VALUE2(uint8, "rage", "self target") < DEMORALIZING_SHOUT_SPARE_RAGE)
    {
        uint32 const thunderClap = AI_VALUE2(uint32, "spell id", "thunder clap");
        if (thunderClap && !bot->HasSpellCooldown(thunderClap))
            return false;
    }

    return CastTankAreaThreatAction::isUseful();
}

Unit* CastInterveneOnAttackedGroupMemberAction::GetTarget() { return AI_VALUE(Unit*, "attacked group member"); }

bool CastInterveneOnAttackedGroupMemberAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target || target == bot || !CastSpellAction::isUseful())
        return false;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    return spellInfo && bot->IsWithinCombatRange(target, spellInfo->GetMaxRange(true, bot)) &&
           !bot->IsWithinCombatRange(target, spellInfo->GetMinRange(true));
}

bool CastBerserkerRageAction::isPossible()
{
    if (botAI->IsInVehicle() && !botAI->IsInVehicle(false, false, true))
        return false;

    uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
    if (!spellId)
        return false;

    if (!bot->HasSpell(spellId))
        return false;

    if (bot->HasSpellCooldown(spellId))
        return false;

    return true;
}

bool CastBerserkerRageAction::isUseful()
{
    return (bot->HasAuraType(SPELL_AURA_MOD_FEAR) ||
           bot->HasAuraWithMechanic(1 << MECHANIC_SLEEP) ||
           bot->HasAuraWithMechanic(1 << MECHANIC_SAPPED))
        && !botAI->HasAura("berserker rage", bot)
        && CastSpellAction::isUseful();
}

// [fear_counter] Logs the break of a fear / sleep (the sap case is not a fear counter).
bool CastBerserkerRageAction::Execute(Event event)
{
    uint32 const aura = FearCounter::TremorableAuraSpell(bot);
    if (!CastSpellAction::Execute(event))
        return false;

    if (aura)
        FearCounter::Log(bot, "berserker rage", bot, aura);
    return true;
}

// [b31_deep]
bool CastSlamFillerAction::isUseful()
{
    return CastSlamAction::isUseful() && LeavesRageForStrike(botAI, bot, "slam");
}

// [b31_deep] Enraged Regeneration known, off cooldown, and no enrage effect on the warrior yet.
bool WarriorEnrage::EnragedRegenerationWaitsForEnrage(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    uint32 const spellId =
        botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "enraged regeneration")->Get();
    return spellId && !bot->HasSpellCooldown(spellId) && !bot->HasAuraState(AURA_STATE_ENRAGE);
}

// [b31_deep]
bool CastBerserkerRageForEnrageAction::isUseful()
{
    return WarriorEnrage::EnragedRegenerationWaitsForEnrage(botAI) && CastSpellAction::isUseful();
}

// [b31_deep] Berserker Rage first (no health cost): Bloodrage only when it is unknown or cooling down.
bool CastBloodrageForEnrageAction::isUseful()
{
    if (!WarriorEnrage::EnragedRegenerationWaitsForEnrage(botAI))
        return false;

    uint32 const berserkerRage = AI_VALUE2(uint32, "spell id", "berserker rage");
    if (berserkerRage && !bot->HasSpellCooldown(berserkerRage))
        return false;

    return CastBloodrageAction::isUseful();
}

// [b31_deep]
bool CastShatteringThrowBossAction::isUseful()
{
    Unit* target = GetTarget();
    return target && !botAI->HasAura("shattering throw", target, false, false) && CastSpellAction::isUseful();
}

bool CastSunderArmorAction::isUseful()
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    // Raid utility coordinator: one major armor debuff owner per group (tanks always sunder in rotation).
    ai::raidutil::RaidUtilityAssignment const& assignment = ai::raidutil::GetAssignment(botAI);
    if (assignment.valid && assignment.armorCoordinated && !assignment.majorArmorOwner &&
        !PlayerbotAI::IsTank(bot, false))
    {
        return false;
    }

    if (!PlayerbotAI::IsTank(bot, false))
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsAlive() || !member->IsInWorld() ||
                member->GetMapId() != bot->GetMapId())
            {
                continue;
            }

            if (member->getClass() == CLASS_WARRIOR &&
                PlayerbotAI::IsTank(member, false))
                return false;
        }

        // [melee_audit_b30] A dps warrior spends five GCDs to stack it: only on a target that lives long enough to
        // pay them back. Build 30 Fury cast Sunder Armor 4.1-6.6 times a minute on 20 s trash (as often as
        // Bloodthirst), the tank branch above is untouched.
        constexpr float DPS_SUNDER_MIN_LIFETIME_S = 20.0f;
        if (AI_VALUE2(float, "estimated lifetime", "current target") < DPS_SUNDER_MIN_LIFETIME_S)
            return false;

        // [b31_deep] not with the rage of a Bloodthirst / Whirlwind / Mortal Strike due within a global cooldown
        if (!LeavesRageForStrike(botAI, bot, "sunder armor"))
            return false;
    }

    Aura* aura = botAI->GetAura("sunder armor", GetTarget(), false, true);
    return !aura || aura->GetStackAmount() < 5 || aura->GetDuration() <= 6000;
}

Unit* CastVigilanceAction::GetTarget()
{
    // The member that should carry it (most threat in a fight, best geared dps out of one); nothing to do when
    // it already does. Casting it on someone else moves it there: Vigilance is on one target at a time.
    Player* target = WarriorVigilance::FindTarget(botAI);
    if (!target || target->HasAura(SPELL_VIGILANCE, bot->GetGUID()))
        return nullptr;

    return target;
}

bool CastRetaliationAction::isUseful()
{
    if (!bot->HasSpell(SPELL_RETALIATION) || bot->HasSpellCooldown(SPELL_RETALIATION) ||
        bot->HasAura(SPELL_RETALIATION))
    {
        return false;
    }

    uint8 meleeAttackers = 0;
    GuidVector attackers = AI_VALUE(GuidVector, "attackers");

    for (ObjectGuid const& guid : attackers)
    {
        Unit* attacker = botAI->GetUnit(guid);
        if (!attacker || !attacker->IsAlive() || attacker->GetVictim() != bot)
            continue;

        // Check if the attacker is melee-based using unit_class
        if (attacker->IsCreature())
        {
            Creature* creature = attacker->ToCreature();
            if (creature && (creature->IsClass(CLASS_WARRIOR)
                || creature->IsClass(CLASS_ROGUE)
                || creature->IsClass(CLASS_PALADIN)))
            {
                ++meleeAttackers;
            }
        }
        else if (attacker->IsPlayer())
        {
            Player* playerAttacker = attacker->ToPlayer();
            if (playerAttacker && PlayerbotAI::IsMelee(playerAttacker)) // Reuse existing Player melee check
            {
                ++meleeAttackers;
            }
        }

        // Early exit if we already have enough melee attackers
        if (meleeAttackers >= 2)
            break;
    }

    // Only cast Retaliation if there are at least 2 melee attackers
    return meleeAttackers >= 2;
}

Unit* CastShatteringThrowAction::GetTarget()
{
    GuidVector enemies = AI_VALUE(GuidVector, "possible targets");

    for (ObjectGuid const& guid : enemies)
    {
        Unit* enemy = botAI->GetUnit(guid);
        if (!enemy || !enemy->IsAlive() || enemy->IsFriendlyTo(bot))
            continue;

        if (bot->IsWithinDistInMap(enemy, 25.0f) && WarriorHasShatterableImmunity(enemy))  // [b31_deep]
            return enemy;
    }

    return nullptr;
}

bool CastShatteringThrowAction::Execute(Event /*event*/)
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    return botAI->CastSpell("shattering throw", target);
}

bool CastShatteringThrowAction::isUseful()
{
    if (!bot->HasSpell(SPELL_SHATTERING_THROW) || bot->HasSpellCooldown(SPELL_SHATTERING_THROW))
        return false;

    GuidVector enemies = AI_VALUE(GuidVector, "possible targets");

    for (ObjectGuid const& guid : enemies)
    {
        Unit* enemy = botAI->GetUnit(guid);
        if (!enemy || !enemy->IsAlive() || enemy->IsFriendlyTo(bot))
            continue;

        if (bot->IsWithinDistInMap(enemy, 25.0f) && WarriorHasShatterableImmunity(enemy))  // [b31_deep]
            return true;
    }

    return false;
}

bool CastShatteringThrowAction::isPossible()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    // Range check: Shattering Throw is 30 yards
    if (!bot->IsWithinDistInMap(target, 30.0f))
        return false;

    // Check line of sight
    if (!bot->IsWithinLOSInMap(target))
        return false;

    // If the minimal checks above pass, simply return true.
    return true;
}
