/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PaladinTriggers.h"
#include "HealthTriggers.h"  // [lead87] RestState::OwnManaLine
#include "LeaderOrders.h"    // [lead1040] the leader's open pull order
#include <map>
#include <set>
#include "SpellMgr.h"
#include "GenericBuffUtils.h"
#include "PaladinActions.h"
#include "PaladinHelper.h"
#include "PartyMemberToHeal.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"

bool SealTrigger::IsActive()
{
    Unit* target = GetTarget();
    return !botAI->HasAura("seal of justice", target) && !botAI->HasAura("seal of command", target) &&
           !botAI->HasAura("seal of vengeance", target) && !botAI->HasAura("seal of corruption", target) &&
           !botAI->HasAura("seal of righteousness", target) && !botAI->HasAura("seal of light", target) &&
           (!botAI->HasAura("seal of wisdom", target) || AI_VALUE2(uint8, "mana", "self target") > 70);
}

// [rot_b31_pdd] the aura state flag first (one bit test); the seal names only while it is missing
bool SealStateLostTrigger::IsActive()
{
    if (bot->HasAuraState(AURA_STATE_JUDGEMENT))
        return false;

    return botAI->HasAnyAuraOf(bot, "seal of righteousness", "seal of justice", "seal of light", "seal of wisdom",
                               "seal of command", "seal of vengeance", "seal of corruption", nullptr);
}

bool HolyDivinePleaTrigger::IsActive()
{
    if (!bot->IsInCombat() || !AI_VALUE2(bool, "has mana", "self target"))
        return false;

    // [b31_pal] Divine Plea is trained at 71 (Spell.dbc spellLevel 71): a level 70 holy paladin has none, and the
    // trigger queued an action that failed on "spell not known" every tick
    if (!AI_VALUE2(uint32, "spell id", "divine plea"))
        return false;

    uint8 const mana = AI_VALUE2(uint8, "mana", "self target");
    if (mana < HOLY_DIVINE_PLEA_MANA_PCT)
        return true;

    if (mana >= HOLY_DIVINE_PLEA_BOSS_MANA_PCT)
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    Creature* creature = target ? target->ToCreature() : nullptr;
    return creature && creature->IsInCombat() && (creature->IsDungeonBoss() || creature->isWorldBoss());
}

// [b31_pal]
bool JudgementsOfThePureTrigger::IsActive()
{
    if (!bot->IsInCombat())
        return false;

    // the haste buff, ranks 1-5
    for (uint32 const buff : {53655u, 53656u, 53657u, 54152u, 54153u})
        if (bot->HasAura(buff))
            return false;

    // the talent, ranks 1-5
    for (uint32 const talent : {53671u, 53673u, 54151u, 54154u, 54155u})
        if (bot->HasAura(talent))
            return true;

    return false;
}

// [b31_pal]
bool HolyDivineIlluminationTrigger::IsActive()
{
    if (!bot->IsInCombat() || !AI_VALUE2(bool, "has mana", "self target"))
        return false;

    uint8 const mana = AI_VALUE2(uint8, "mana", "self target");
    if (mana < HOLY_DI_ANYWAY_MANA_PCT)
        return true;

    if (mana >= HOLY_DI_MANA_PCT)
        return false;

    Unit* target = AI_VALUE(Unit*, "party member to heal");
    return target && target->IsAlive() && target->GetHealthPct() < sPlayerbotAIConfig.AlmostFullHealth;
}

// [b31_pal]
bool PaladinSpellReadyTrigger::IsActive()
{
    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    if (!spellId || bot->HasSpellCooldown(spellId))
        return false;

    Unit* target = GetTarget();
    if (!target || !target->IsAlive())
        return false;

    return !minManaPct || AI_VALUE2(uint8, "mana", "self target") >= minManaPct;
}

// [b31_pal]
bool AvengersShieldBossFillerTrigger::IsActive()
{
    if (!PaladinSpellReadyTrigger::IsActive())
        return false;

    Creature* creature = GetTarget()->ToCreature();
    return creature && creature->IsInCombat() && (creature->IsDungeonBoss() || creature->isWorldBoss());
}

bool CrusaderAuraTrigger::IsActive()
{
    Unit* target = GetTarget();
    return AI_VALUE2(bool, "mounted", "self target") && !botAI->HasAura("crusader aura", target);
}

bool DivineShieldLowHealthTrigger::IsActive()
{
    return botAI->HasAura("divine shield", bot) && AI_VALUE2(uint8, "health", "self target") < 80;
}

Unit* HandOfFreedomOnPartyTrigger::GetTarget()
{
    bool const selfImpaired = ai::paladin::HasFreedomRemovableImpairment(bot);  // [b33_class] root/snare only
    bool const hasSelfHand = selfImpaired && ai::paladin::HasAnyPaladinHandFromCaster(bot, bot);

    if (!bot->GetGroup())
    {
        if (selfImpaired && !hasSelfHand)
            return bot;

        return nullptr;
    }

    if (selfImpaired && !hasSelfHand)
        return bot;

    return Trigger::GetTarget();
}

bool HandOfFreedomOnPartyTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    if (target != bot &&
        bot->GetExactDist2dSq(target->GetPositionX(), target->GetPositionY()) > 30.0f * 30.0f)
        return false;

    if (!botAI->CanCastSpell("hand of freedom", target))
        return false;

    // [b33_class] root/snare only: Hand of Freedom cannot clear a stun or knockdown
    return !ai::paladin::HasAnyPaladinHandFromCaster(target, bot) && ai::paladin::HasFreedomRemovableImpairment(target);
}

bool NotSensingUndeadTrigger::IsActive()
{
    return !botAI->HasAura("sense undead", bot);
}

namespace
{
    constexpr float HAND_SPELL_RANGE = 30.0f;
    constexpr uint8 AURA_MASTERY_AOE_HEAL_COUNT = 4;

    bool IsSpellReady(PlayerbotAI* botAI, Player* bot, std::string const& spell)
    {
        uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", spell)->Get();
        return spellId && !bot->HasSpellCooldown(spellId);
    }

    bool IsCastingAreaMagicDamage(Unit* caster)
    {
        if (!caster || caster->ToPlayer())
            return false;

        for (CurrentSpellTypes type : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
        {
            Spell* spell = caster->GetCurrentSpell(type);
            SpellInfo const* spellInfo = spell ? spell->GetSpellInfo() : nullptr;
            if (!spellInfo || !spellInfo->IsAffectingArea() || !(spellInfo->GetSchoolMask() & SPELL_SCHOOL_MASK_MAGIC))
                continue;

            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                SpellEffectInfo const& effect = spellInfo->Effects[i];
                if (effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE ||
                    (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE))
                {
                    return true;
                }
            }
        }

        return false;
    }
}

bool HandOfSalvationOnPartyTrigger::IsActive()
{
    if (!bot->GetGroup() || !IsSpellReady(botAI, bot, "hand of salvation"))
        return false;

    return GetTarget() != nullptr;
}

bool HandOfSacrificeOnMainTankTrigger::IsActive()
{
    if (PlayerbotAI::IsTank(bot) || !IsSpellReady(botAI, bot, "hand of sacrifice"))
        return false;

    Unit* tank = GetTarget();
    if (!tank || tank == bot || !tank->IsAlive() || bot->GetDistance(tank) > HAND_SPELL_RANGE)
        return false;

    return tank->GetHealthPct() < sPlayerbotAIConfig.LowHealth &&
           bot->GetHealthPct() > sPlayerbotAIConfig.MediumHealth && BuffTrigger::IsActive();
}

bool AuraMasteryTrigger::IsActive()
{
    if (!bot->GetGroup() || !IsSpellReady(botAI, bot, "aura mastery"))
        return false;

    if (IsCastingAreaMagicDamage(AI_VALUE(Unit*, "current target")) ||
        IsCastingAreaMagicDamage(AI_VALUE(Unit*, "boss target")))
    {
        return true;
    }

    return bot->GetGroup()->isRaidGroup() && AI_VALUE2(uint8, "aoe heal", "medium") >= AURA_MASTERY_AOE_HEAL_COUNT;
}

bool RetDivinePleaTrigger::IsActive()
{
    constexpr uint8 RET_DIVINE_PLEA_MANA_PCT = 90;
    constexpr uint8 RET_DIVINE_PLEA_IN_WRATH_MANA_PCT = 30;  // [b31_pal]
    constexpr uint32 AVENGING_WRATH = 31884;
    if (!AI_VALUE2(bool, "has mana", "self target") || !AI_VALUE2(uint32, "spell id", "divine plea"))
        return false;

    uint8 const mana = AI_VALUE2(uint8, "mana", "self target");
    if (mana >= RET_DIVINE_PLEA_MANA_PCT)
        return false;

    // [b31_pal] not a GCD of the Avenging Wrath window, unless mana is running out
    return mana < RET_DIVINE_PLEA_IN_WRATH_MANA_PCT || !bot->HasAura(AVENGING_WRATH);
}

bool HolyWrathTargetTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsAlive())
        return false;

    uint32 const creatureType = target->GetCreatureType();
    return creatureType == CREATURE_TYPE_UNDEAD || creatureType == CREATURE_TYPE_DEMON;
}

// [b34_pal]
bool HolyWrathUndeadPackTrigger::IsActive()
{
    return bot->IsInCombat() &&
           ai::paladin::CountHolyWrathTargetsWithin(botAI, bot, ai::paladin::HOLY_WRATH_PACK_RANGE) >=
               ai::paladin::HOLY_WRATH_PACK_MIN;
}

// [b31_deep]
bool PaladinTankInGroupValue::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || member->getClass() != CLASS_PALADIN || !member->IsAlive() ||
            member->GetMapId() != bot->GetMapId())
            continue;

        if (PlayerbotAI::IsTank(member))
            return true;
    }

    return false;
}

// [b31_deep]
bool PaladinTankJudgesWisdomTrigger::IsActive()
{
    if (!AI_VALUE(bool, "paladin tank in group"))
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    return target && target->IsAlive();
}

// [b31_deep]
bool TankHammerOfWrathTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsAlive() || target->GetHealthPct() >= 20.0f)  // the spell's own 20 % rule
        return false;
    // [lead105] b139 simpct: never cast on the bosses - on a boss the fight IS the next fight: its execute is cast
    if (Creature* boss = target->ToCreature(); boss && boss->IsDungeonBoss())
        return true;
    uint32 const spell = AI_VALUE2(uint32, "spell id", "hammer of wrath");
    uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
    SpellInfo const* info = spell ? sSpellMgr->GetSpellInfo(spell) : nullptr;
    if (!info || !maxMana || bot->HasSpellCooldown(spell))
        return false;
    float const costPct = 100.0f * float(info->CalcPowerCost(bot, info->GetSchoolMask())) / float(maxMana);
    return bot->GetPowerPct(POWER_MANA) - costPct >= RestState::OwnManaLine(bot);
}

bool TankDivinePleaTrigger::IsActive()
{
    constexpr uint32 SPELL_DIVINE_PLEA = 54428;
    // [lead105] b140 rfc-h: Divine Plea 8 times in 1355 s (60 s cooldown, ~20 possible), the tank rested 100 s
    if (!PlayerbotAI::IsTank(bot) || !bot->HasSpell(SPELL_DIVINE_PLEA) || bot->HasSpellCooldown(SPELL_DIVINE_PLEA) ||
        !bot->GetMaxPower(POWER_MANA) || botAI->HasAura("divine plea", bot))
        return false;
    // [rot48] Guarded by the Light refreshes Divine Plea on every melee hit (spell 63521): cast in a fight it lasts the
    // whole fight, whatever the mana (wowsims prot APL: cast before the pull at full mana, never again)
    // [rot150] b213 rfc-h 5E: Divine Plea 15 times in the 934 s of fights, the tank at 1-13 % mana at 1010 / 1033 /
    // 1243 s, 60 s stood as "the tank rests" and 56 s drinking. Cast between pulls it gives its 25 % once (15 s, no
    // hits to refresh it) and is on its cooldown when the next pull opens; cast in the fight the hits keep it up and
    // it gives 5 % every 3 s for the whole fight. A tank with Guarded by the Light keeps it for the fight.
    // [lead1040] b230 rfc-h 5E: Divine Plea was up 40 % of the boss fights (5A 100 %) - cast on the trash pull ~20 s
    // before Oggleflint, it ran out in the gap and was on its cooldown at his pull (2.67 %/s boss mana rate against
    // 0.81, 25 s drinking). TankPleaNow: the boss fights first, as every long cooldown (wowsims casts it before the
    // pull)
    if (ai::paladin::KeepsDivinePleaUpByHits(bot))
        return ai::paladin::TankPleaNow(bot);
    return bot->GetPower(POWER_MANA) < bot->GetMaxPower(POWER_MANA);
}

bool ai::paladin::TankPleaNow(Player* bot)
{
    constexpr uint32 SPELL_DIVINE_PLEA = 54428;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_DIVINE_PLEA);
    if (!bot || !info)
        return false;
    auto const isBoss = [](Unit* unit)
    {
        Creature const* creature = unit ? unit->ToCreature() : nullptr;
        return creature && (creature->IsDungeonBoss() || creature->isWorldBoss());
    };
    // [lead1063] one line per tank and verdict (a log throttle of the aura's own duration, no decision): b231 never
    // logged the trash verdicts, and 5A-1 cast Plea on pull 38 with Jergosh's pull ~10 s off (aura 810 s, pulled
    // 832 s, on its cooldown all his fight)
    float const durationSec = float(std::max(info->GetMaxDuration(), 0)) / float(IN_MILLISECONDS);
    auto const note = [bot, durationSec](char const* verdict, float toBoss, float rechargeSec)
    {
        thread_local std::map<std::pair<uint64, std::string>, uint32> logged;
        uint32 const now = getMSTime() ? getMSTime() : 1;
        uint32& at = logged[{bot->GetGUID().GetRawValue(), verdict}];
        if (at && getMSTimeDiff(at, now) < uint32(durationSec * IN_MILLISECONDS))
            return;
        at = now;
        LOG_INFO("playerbots",
                 "[lead1040] {}: Divine Plea {} (the next boss pull in {:.0f} s, recharge {:.0f} s, aura {:.0f} s)",
                 bot->GetName(), verdict, toBoss, rechargeSec, durationSec);
    };
    int32 rechargeMs = int32(std::max(info->RecoveryTime, info->CategoryRecoveryTime));
    bot->ApplySpellMod(SPELL_DIVINE_PLEA, SPELLMOD_COOLDOWN, rechargeMs);
    float const rechargeSec = float(std::max(rechargeMs, 0)) / float(IN_MILLISECONDS);
    float const toBoss = RestState::BossPullSecOf(bot);
    if (bot->IsInCombat())
    {
        if (isBoss(bot->GetVictim()))
            return true;
        for (Unit* attacker : bot->getAttackers())
            if (isBoss(attacker))
                return true;
        // [lead1083] user 2026-11-04 (Q59): no estimate never means "spend it on trash" - only the leader's "no boss
        // left" does (a bot no leader leads keeps its own rule: the trash has it)
        if (toBoss < 0.0f)
        {
            if (!Leader::LedBot(bot) || RestState::NoBossLeftOf(bot))
            {
                note("on trash: no boss left", toBoss, rechargeSec);
                return true;
            }
            note("held: no boss estimate yet", toBoss, rechargeSec);
            return false;
        }
        if (rechargeSec <= toBoss)
        {
            note("on trash: back before the boss", toBoss, rechargeSec);
            return true;
        }
        return false;
    }
    // [lead1063] b231: the pre-pull Plea never fired - the leader's boss pull order and the tank's pull are one tick
    // ("pull_trace execute: 1 ms after the issue"), and the pull action outranks the non-combat Plea, so an open boss
    // order out of a fight was never seen here. The leader's own estimate of the boss pull (BossPullSecOf: this
    // fight's rest, the trash, the rest and the walk to him) within the aura's duration is the moment: cast now it is
    // still up at his pull, and Guarded by the Light keeps it the whole fight (wowsims prot: before the pull)
    if (!Leader::LedBot(bot))
        return false;
    if (toBoss >= 0.0f && toBoss <= durationSec)
    {
        note("before the boss pull (its estimate within the aura)", toBoss, rechargeSec);
        return true;
    }
    Leader::PullOrder order;
    if (!Leader::Orders::GetOpen(bot->GetGUID(), order))
        return false;
    Creature* boss = bot->GetMap() ? bot->GetMap()->GetCreature(order.target) : nullptr;
    if (!boss || !boss->IsAlive() || !isBoss(boss))
        return false;
    float const walkSec = std::max(bot->GetExactDist(boss) - bot->GetMeleeRange(boss), 0.0f) /
                          std::max(bot->GetSpeed(MOVE_RUN), 0.1f);
    if (walkSec > durationSec)
        return false;
    thread_local std::set<std::pair<uint64, uint32>> logged;  // one line per tank and order
    if (logged.insert({bot->GetGUID().GetRawValue(), order.id}).second)
        LOG_INFO("playerbots", "[lead1040] {}: Divine Plea before the pull of {} ({:.1f} s walk <= its {:.0f} s)",
                 bot->GetName(), boss->GetName(), walkSec, durationSec);
    return true;
}

namespace
{
    // [b31_deep] the cached heal target, alive and below almost-full health
    Unit* HurtHealTarget(PlayerbotAI* botAI)
    {
        Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("party member to heal")->Get();
        if (!target || !target->IsAlive() || target->GetHealthPct() >= sPlayerbotAIConfig.AlmostFullHealth)
            return nullptr;

        return target;
    }

    // [b31_deep] the cached "boss target" is alive and fighting
    bool InBossFight(PlayerbotAI* botAI)
    {
        Unit* boss = botAI->GetAiObjectContext()->GetValue<Unit*>("boss target")->Get();
        return boss && boss->IsAlive() && boss->IsInCombat();
    }
}

// [b31_deep]
bool InfusionOfLightTrigger::IsActive()
{
    constexpr uint32 INFUSION_OF_LIGHT_RANK_1 = 53672;
    constexpr uint32 INFUSION_OF_LIGHT_RANK_2 = 54149;
    if (!bot->HasAura(INFUSION_OF_LIGHT_RANK_2) && !bot->HasAura(INFUSION_OF_LIGHT_RANK_1))
        return false;

    return HurtHealTarget(botAI) != nullptr;
}

// [b31_deep]
bool HolyLightBossTankTrigger::IsActive()
{
    if (!bot->IsInCombat())
        return false;

    Unit* target = HurtHealTarget(botAI);
    Player* player = target ? target->ToPlayer() : nullptr;
    if (!player || !PlayerbotAI::IsTank(player))
        return false;

    return InBossFight(botAI);
}

// [b31_deep]
bool HolyAvengingWrathTrigger::IsActive()
{
    if (!bot->IsInCombat())
        return false;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", "avenging wrath");
    if (!spellId || bot->HasSpellCooldown(spellId))
        return false;

    Unit* target = AI_VALUE(Unit*, "party member to heal");
    if (!target || !target->IsAlive() || target->GetHealthPct() >= HealTriage::EMERGENCY_HEALTH_PCT)
        return false;

    return InBossFight(botAI);
}
