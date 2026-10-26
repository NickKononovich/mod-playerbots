/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PriestTriggers.h"
#include "Group.h"
#include "HealerSupport.h"
#include "PartyStrengthValue.h"  // [b31_deep]
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RaidUtilityAssignment.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TargetValue.h"
#include "ThreatValues.h"  // [b31_deep]

namespace ai::priest
{
bool ShadowWeavingReady(Player* bot)
{
    if (!bot->HasAura(SPELL_SHADOW_WEAVING_RANK_1) && !bot->HasAura(SPELL_SHADOW_WEAVING_RANK_2) &&
        !bot->HasAura(SPELL_SHADOW_WEAVING_RANK_3))
        return true;

    Aura* weaving = bot->GetAura(SPELL_SHADOW_WEAVING_BUFF);
    return weaving && weaving->GetStackAmount() >= SHADOW_WEAVING_MAX_STACKS;
}

uint32 ShadowWordPainRefreshMs(Player* bot)
{
    return bot->HasAura(SPELL_PAIN_AND_SUFFERING_RANK_3) ? 0 : DOT_REFRESH_MS;
}

uint32 VampiricTouchRefreshMs(Player* bot, uint32 spellId)
{
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!spellInfo)
        return VAMPIRIC_TOUCH_REFRESH_MS;

    return spellInfo->CalcCastTime(bot) + VAMPIRIC_TOUCH_REFRESH_MARGIN_MS;
}
}

bool ShadowWordPainTrigger::IsActive()
{
    if (!ai::priest::ShadowWeavingReady(bot))
        return false;

    beforeDuration = ai::priest::ShadowWordPainRefreshMs(bot);
    return PriestDotTrigger::IsActive();
}

bool VampiricTouchTrigger::IsActive()
{
    beforeDuration = ai::priest::VampiricTouchRefreshMs(bot, AI_VALUE2(uint32, "spell id", spell));
    return PriestDotTrigger::IsActive();
}

bool ShadowProtectionTrigger::IsActive()
{
    return BuffTrigger::IsActive() && !botAI->HasAura("prayer of shadow protection", GetTarget());
}

bool PowerWordFortitudeTrigger::IsActive()
{
    return BuffTrigger::IsActive() && !botAI->HasAura("power word: fortitude", GetTarget()) &&
           !botAI->HasAura("prayer of fortitude", GetTarget());
}

bool DivineSpiritTrigger::IsActive()
{
    return BuffTrigger::IsActive() && !botAI->HasAura("divine spirit", GetTarget()) &&
           !botAI->HasAura("prayer of spirit", GetTarget());
}

bool InnerFireTrigger::IsActive()
{
    Unit* target = GetTarget();
    return SpellTrigger::IsActive() && !botAI->HasAura(spell, target);
}

bool FearWardOnMainTankTrigger::IsActive()
{
    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    if (!spellId || bot->HasSpellCooldown(spellId))
        return false;

    return BuffOnMainTankTrigger::IsActive();
}

bool PowerWordShieldOnMainTankTrigger::IsActive()
{
    return BuffOnMainTankTrigger::IsActive() && !botAI->HasAura("weakened soul", GetTarget());
}

bool PrayerOfMendingOnMainTankTrigger::IsActive()
{
    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    if (!spellId || bot->HasSpellCooldown(spellId))
        return false;

    return BuffOnMainTankTrigger::IsActive();
}

bool ShadowformTrigger::IsActive() { return !botAI->HasAura("shadowform", bot); }

// [b31_deep]
bool ShadowWordPainOnAttackerTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target || target == AI_VALUE(Unit*, "current target"))
        return false;

    return DebuffOnAttackerTrigger::IsActive();
}

// [b31_deep]
bool PowerInfusionTrigger::IsActive()
{
    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    if (!spellId || bot->HasSpellCooldown(spellId))
        return false;

    return BoostTrigger::IsActive();
}

// [b31_deep]
bool RenewOnMainTankTrigger::IsActive()
{
    constexpr float RENEW_TANK_MAX_HEALTH_PCT = 98.0f;
    if (!BuffOnMainTankTrigger::IsActive())
        return false;

    Unit* tank = GetTarget();
    return tank && tank->GetHealthPct() < RENEW_TANK_MAX_HEALTH_PCT;
}

// [guide_b31_dead]
bool ShadowfiendTrigger::IsActive()
{
    constexpr uint32 SPELL_SHADOWFIEND = 34433;
    if (!bot->IsInCombat() || !bot->HasSpell(SPELL_SHADOWFIEND) || bot->HasSpellCooldown(SPELL_SHADOWFIEND))
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsAlive())
        return false;

    uint8 const mana = AI_VALUE2(uint8, "mana", "self target");
    if (mana < 50)
        return true;

    Creature* creature = target->ToCreature();
    if (!creature || !(creature->IsDungeonBoss() || creature->isWorldBoss()))
        return false;

    return !PlayerbotAI::IsHeal(bot) || mana < 60;
}

// [guide_b31_dead]
bool ShadowformNonCombatTrigger::IsActive()
{
    constexpr uint32 SPELL_SHADOWFORM = 15473;
    return bot->HasSpell(SPELL_SHADOWFORM) && !botAI->HasAura("shadowform", bot) &&
           !AI_VALUE(Unit*, "party member to resurrect");
}

// [guide_b31_dead]
bool MindFlayClipTrigger::IsActive()
{
    Spell* channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    if (!channel || !channel->m_spellInfo)
        return false;

    switch (channel->m_spellInfo->Id)
    {
        case 15407: case 17311: case 17312: case 17313: case 17314:
        case 18807: case 25387: case 48155: case 48156:
            break;
        default:
            return false;
    }

    // [b31_deep] Mind Flay shares the priest GCD category with Mind Blast, Vampiric Touch and Devouring Plague
    if (bot->GetGlobalCooldownMgr().HasGlobalCooldown(channel->m_spellInfo) ||
        AI_VALUE2(uint8, "mana", "self target") < sPlayerbotAIConfig.LowMana)
        return false;

    Unit* target = channel->m_targets.GetUnitTarget();
    if (!target || !target->IsAlive())
        return false;

    Aura* flay = target->GetAura(channel->m_spellInfo->Id, bot->GetGUID());
    if (!flay)
        return false;

    bool justTicked = false;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        AuraEffect* effect = flay->GetEffect(i);
        if (!effect || !effect->IsPeriodic() || effect->GetAmplitude() <= 0)
            continue;

        uint32 const ticks = effect->GetTickNumber();
        if (ticks < 1 || int32(ticks) >= effect->GetTotalTicks())
            return false;

        justTicked = effect->GetAmplitude() - effect->GetPeriodicTimer() <= 400;
        break;
    }

    if (!justTicked)
        return false;

    // [b31_deep] the DoTs only while their strategy runs, else nothing would cast them after the clip
    bool const dots = botAI->HasStrategy("shadow debuff", BOT_STATE_COMBAT);
    bool const due = MindBlastReady() ||
                     (dots && DotDue(target, "vampiric touch",
                                     ai::priest::VampiricTouchRefreshMs(
                                         bot, AI_VALUE2(uint32, "spell id", "vampiric touch")))) ||
                     (dots && DotDue(target, "devouring plague", ai::priest::DOT_REFRESH_MS));

    return due && !HeldForThreat(target);
}

// [b31_deep]
bool MindFlayClipTrigger::MindBlastReady()
{
    uint32 const mindBlast = AI_VALUE2(uint32, "spell id", "mind blast");
    return mindBlast && !bot->HasSpellCooldown(mindBlast);
}

// [b31_deep] the same test as PriestDotTrigger: missing or inside the refresh window, on a target that lives long
// enough (DOT_MIN_LIFETIME at the group's dps)
bool MindFlayClipTrigger::DotDue(Unit* target, std::string const spell, uint32 refreshMs)
{
    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    if (!spellId)
        return false;

    float const groupDps = AI_VALUE(float, "estimated group dps");
    if (groupDps > 0.0f && target->GetHealth() / groupDps < ai::priest::DOT_MIN_LIFETIME)
        return false;

    Aura* aura = target->GetAura(spellId, bot->GetGUID());
    return !aura || aura->GetDuration() <= int32(refreshMs);
}

// [b31_deep] Mirrors the single-target part of ThreatMultiplier::GetValue (ThreatStrategy.cpp): the hold depends on
// the bot's threat after the next cast, not on which spell it is, so Mind Blast and the DoTs are held alike.
// "neglect threat" is not read here: reading that value resets it.
bool MindFlayClipTrigger::HeldForThreat(Unit* target)
{
    // SINGLE_THREAT_LIMIT / CAREFUL_SINGLE_THREAT_LIMIT of ThreatStrategy.cpp
    constexpr uint8 SINGLE_THREAT_LIMIT = 80;
    constexpr uint8 CAREFUL_SINGLE_THREAT_LIMIT = 70;

    if (!botAI->HasStrategy("threat", BOT_STATE_COMBAT) || !bot->GetGroup() || PlayerbotAI::IsTank(bot))
        return false;

    if (target->GetVictim() == bot || target->IsPlayer())
        return false;

    bool const careful = AI_VALUE(PartyStrength, "party strength").CarefulThreat();
    float const projected = ThreatLead::ProjectedShare(botAI, target);
    if (projected >= 0.0f)
        return projected >= ThreatLead::PullOverPoint(bot, target) *
                                (careful ? ThreatLead::CAREFUL_HOLD_SHARE : ThreatLead::HOLD_SHARE);

    uint8 const limit = careful ? CAREFUL_SINGLE_THREAT_LIMIT : SINGLE_THREAT_LIMIT;
    return AI_VALUE2(uint8, "threat", "current target") >= limit;
}

BindingHealTrigger::BindingHealTrigger(PlayerbotAI* botAI)
    : PartyMemberLowHealthTrigger(botAI, "binding heal", sPlayerbotAIConfig.LowHealth, 0)
{
}

bool BindingHealTrigger::IsActive()
{
    return PartyMemberLowHealthTrigger::IsActive() &&
           AI_VALUE2(uint8, "health", "self target") < sPlayerbotAIConfig.MediumHealth;
}

const std::set<uint32> MindSearChannelCheckTrigger::MIND_SEAR_SPELL_IDS = {
    48045,  // Mind Sear Rank 1
    53023   // Mind Sear Rank 2
};

bool MindSearChannelCheckTrigger::IsActive()
{
    Player* bot = botAI->GetBot();

    // Check if the bot is channeling a spell
    if (Spell* spell = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
    {
        // Only trigger if the spell being channeled is Mind Sear
        // [b31_deep] Mind Sear hits 10 yd around its target but never the target: cancel once fewer than
        // minEnemies others stand there (the count includes the target, hence + 1). It was counted around the bot
        // in SightDistance, so the channel almost never broke.
        if (spell->m_spellInfo && MIND_SEAR_SPELL_IDS.count(spell->m_spellInfo->Id))
        {
            constexpr float MIND_SEAR_RADIUS = 10.0f;
            Unit* searTarget = spell->m_targets.GetUnitTarget();
            if (!searTarget || !searTarget->IsAlive())
                return false;

            return !HasEnoughAoeTargets(botAI, searTarget->GetPosition(), MIND_SEAR_RADIUS, minEnemies + 1);
        }
    }

    // Not channeling Mind Sear
    return false;
}

bool HymnOfHopeNeededTrigger::IsActive()
{
    if (!bot->GetGroup())
        return false;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", "hymn of hope");
    if (!spellId || bot->HasSpellCooldown(spellId))
        return false;

    std::string const qualifier = ai::raidutil::LowManaHealerQualifier(false, sPlayerbotAIConfig.MediumMana);
    // [guide_b31_dead] a 5-man has one healer, so the >= 2 rule never fired there: also allow the priest alone
    // at < 30 % mana when it is a healer (Shadow has Dispersion and Shadowfiend instead).
    if (AI_VALUE2(uint8, "low mana healer count", qualifier) < 2 &&
        (!PlayerbotAI::IsHeal(bot) || AI_VALUE2(uint8, "mana", "self target") >= 30))
        return false;

    // The priest channels for 8 sec: hold it while someone is in danger.
    Unit* healTarget = AI_VALUE(Unit*, "party member to heal");
    return !healTarget || healTarget->GetHealthPct() >= sPlayerbotAIConfig.MediumHealth;
}

// [guide_b31_dead]
bool PenanceOnMainTankTrigger::IsActive()
{
    if (!bot->IsInCombat())
        return false;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", "penance");
    if (!spellId || bot->HasSpellCooldown(spellId))
        return false;

    Unit* tank = AI_VALUE(Unit*, "main tank");
    return tank && tank->IsAlive() && tank->GetHealthPct() < 95.0f;
}

// [guide_b31_dead]
bool CircleOfHealingTwoInjuredTrigger::IsActive()
{
    uint32 const spellId = AI_VALUE2(uint32, "spell id", "circle of healing");
    if (!spellId || bot->HasSpellCooldown(spellId))
        return false;

    return AI_VALUE2(uint8, "aoe heal", "90") >= 2;
}

// [guide_b31_dead]
bool BorrowedTimeTrigger::IsActive()
{
    if (!bot->HasAura(59887) && !bot->HasAura(59888) && !bot->HasAura(59889) && !bot->HasAura(59890) &&
        !bot->HasAura(59891))
        return false;

    Unit* target = AI_VALUE(Unit*, "party member to heal");
    return target && target->IsAlive() && target->GetHealthPct() <= maxHealthPct;
}

// [guide_b31_dead]
bool SerendipityGreaterHealTrigger::IsActive()
{
    if (bot->isMoving())
        return false;

    Aura* serendipity = bot->GetAura(63735);
    if (!serendipity)
        serendipity = bot->GetAura(63734);
    if (!serendipity)
        serendipity = bot->GetAura(63731);
    if (!serendipity || serendipity->GetStackAmount() < 3)
        return false;

    Unit* tank = AI_VALUE(Unit*, "main tank");
    return tank && tank->IsAlive() && tank->GetHealthPct() < 70.0f;
}

Unit* BattleResurrectedMemberValue::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group)
    {
        _deadSeenAt.clear();
        return nullptr;
    }

    uint32 const now = getMSTime();
    Player* best = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || member->GetMap() != bot->GetMap())
            continue;

        if (!member->IsAlive())
        {
            _deadSeenAt[member->GetGUID()] = now;
            continue;
        }

        auto const seen = _deadSeenAt.find(member->GetGUID());
        if (seen == _deadSeenAt.end())
            continue;

        if (getMSTimeDiff(seen->second, now) > ai::healer::BATTLE_REZ_WINDOW_MS)
        {
            _deadSeenAt.erase(seen);
            continue;
        }

        if (bot->GetDistance(member) > sPlayerbotAIConfig.SightDistance)
            continue;

        if (!best || member->GetHealthPct() < best->GetHealthPct())
            best = member;
    }

    return best;
}

namespace
{
    // The tank's target is a mob hitting a non-tank group member, or the tank has just taunted it.
    bool IsPeeling(Player* tank, Group* group)
    {
        Unit* victim = tank->GetVictim();
        if (!victim || !victim->IsAlive())
            return false;

        if (Player* hit = victim->GetVictim() ? victim->GetVictim()->ToPlayer() : nullptr)
        {
            if (hit != tank && group->IsMember(hit->GetGUID()) && !PlayerbotAI::IsTank(hit))
                return true;
        }

        for (AuraEffect const* taunt : victim->GetAuraEffectsByType(SPELL_AURA_MOD_TAUNT))
        {
            if (taunt->GetCasterGUID() == tank->GetGUID())
                return true;
        }

        return false;
    }
}

Unit* PeelingTankValue::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group || !bot->IsInCombat())
    {
        _peelUntil.clear();
        return nullptr;
    }

    uint32 const now = getMSTime();
    Player* best = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* tank = ref->GetSource();
        if (!tank || tank == bot || !tank->IsAlive() || !tank->IsInCombat() || tank->GetMap() != bot->GetMap() ||
            !PlayerbotAI::IsTank(tank))
            continue;

        if (IsPeeling(tank, group))
            _peelUntil[tank->GetGUID()] = now + ai::healer::PEEL_HOLD_MS;

        auto const until = _peelUntil.find(tank->GetGUID());
        if (until == _peelUntil.end() || int32(until->second - now) <= 0)
            continue;

        if (bot->GetDistance(tank) > sPlayerbotAIConfig.SightDistance)
            continue;

        if (!best || tank->GetHealthPct() < best->GetHealthPct())
            best = tank;
    }

    return best;
}
