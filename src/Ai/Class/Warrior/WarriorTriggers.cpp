/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WarriorTriggers.h"
#include "WarriorActions.h"  // [rot19] WarriorLeavesRageForStrike
#include "Playerbots.h"
#include "RaidUtilityAssignment.h"
#include "SpellAuraEffects.h"  // [b33_class]
#include "SpellAuras.h"        // [b33_class]
#include "ThreatManager.h"
#include <algorithm>         // [b33_class]
#include <initializer_list>  // [b33_class]

namespace
{
constexpr uint32 SPELL_VIGILANCE = 50720;
constexpr uint32 SPELL_SHATTERING_THROW = 64382;
constexpr uint32 SPELL_DIVINE_SHIELD = 642;
constexpr uint32 SPELL_ICE_BLOCK = 45438;
// [b31_deep] 41450 is an npc spell; the player's Hand of Protection is 1022 / 5599 / 10278
constexpr uint32 SPELL_BLESSING_OF_PROTECTION[] = { 41450, 1022, 5599, 10278 };
constexpr uint32 SHATTERING_THROW_MIN_COMBAT_S = 6;      // wowsims: Shattering Throw after 6 s of the fight
constexpr float SHATTERING_THROW_MIN_LIFETIME_S = 15.0f; // the 10 s armor debuff must pay its 1.5 s cast
constexpr uint32 SPELL_COMMANDING_PRESENCE_RANKS[] = { 12318, 12857, 12858, 12860, 12861 };
constexpr time_t VIGILANCE_REVIEW_SECONDS = 30;       // how often the tank asks who should carry its Vigilance
constexpr float VIGILANCE_KEEP_THREAT_SHARE = 0.8f;   // the holder keeps it while this close to the top threat
constexpr uint32 VIGILANCE_KEEP_GEAR_MARGIN = 5;      // ... or, out of a fight, this close to the best item level
}

namespace WarriorVigilance
{
    Player* CurrentHolder(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return nullptr;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsInWorld() && member->GetMap() == bot->GetMap() &&
                member->HasAura(SPELL_VIGILANCE, bot->GetGUID()))
                return member;
        }

        return nullptr;
    }

    Player* FindTarget(PlayerbotAI* botAI)
    {
        Player* bot = botAI->GetBot();
        Group* group = bot->GetGroup();
        if (!group)
            return nullptr;

        GuidVector attackers;
        if (bot->IsInCombat())
            attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();

        Player* holder = nullptr;
        float holderThreat = 0.0f;
        uint32 holderGear = 0;
        Player* topThreat = nullptr;
        float bestThreat = 0.0f;
        Player* topGear = nullptr;
        uint32 bestGear = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            // members on another map belong to another thread, and Vigilance does nothing there anyway
            if (!member || member == bot || !member->IsAlive() || !member->IsInWorld() ||
                member->GetMap() != bot->GetMap() || PlayerbotAI::IsTank(member))
                continue;

            bool const ours = member->HasAura(SPELL_VIGILANCE, bot->GetGUID());
            // another warrior's Vigilance is not ours to move; a new target must also be in range
            if (!ours &&
                (member->HasAura(SPELL_VIGILANCE) || bot->GetDistance(member) > sPlayerbotAIConfig.SpellDistance))
                continue;

            float threat = 0.0f;
            for (ObjectGuid const& guid : attackers)
            {
                Unit* attacker = botAI->GetUnit(guid);
                if (attacker && attacker->IsAlive() && !attacker->IsPlayer())
                    threat += attacker->GetThreatMgr().GetThreat(member);
            }

            uint32 const gear = PlayerbotAI::IsDps(member) ? botAI->GetEquipGearScore(member) : 0;
            if (ours)
            {
                holder = member;
                holderThreat = threat;
                holderGear = gear;
            }

            if (threat > bestThreat)
            {
                bestThreat = threat;
                topThreat = member;
            }

            if (gear > bestGear)
            {
                bestGear = gear;
                topGear = member;
            }
        }

        if (topThreat)
            return holder && holderThreat >= bestThreat * VIGILANCE_KEEP_THREAT_SHARE ? holder : topThreat;

        if (holder && (holderGear + VIGILANCE_KEEP_GEAR_MARGIN >= bestGear || !topGear))
            return holder;

        return topGear;
    }
}

bool BloodrageBuffTrigger::IsActive()
{
    return AI_VALUE2(uint8, "health", "self target") >= sPlayerbotAIConfig.MediumHealth &&
           AI_VALUE2(uint8, "rage", "self target") < 20;
}

// [rot20]
bool BloodrageTankTrigger::IsActive()
{
    return AI_VALUE2(uint8, "health", "self target") >= sPlayerbotAIConfig.MediumHealth &&
           AI_VALUE2(uint8, "rage", "self target") < TANK_BLOODRAGE_BELOW_RAGE;
}

bool BloodrageLowRageTrigger::IsActive()
{
    return bot->IsInCombat() && AI_VALUE2(uint8, "rage", "self target") < LOW_RAGE &&
           AI_VALUE2(uint8, "health", "self target") >= sPlayerbotAIConfig.MediumHealth;
}

bool VigilanceTrigger::IsActive()
{
    if (!bot->HasSpell(SPELL_VIGILANCE))
        return false;

    if (!bot->GetGroup())
        return false;

    Player* holder = WarriorVigilance::CurrentHolder(bot);
    if (!holder)
        return true;

    // Vigilance is up: every so often see whether someone else now draws more threat (a dps that outgears
    // the rest, or one that just popped its cooldowns) and move it there.
    time_t const now = time(nullptr);
    if (now < _nextReview)
        return false;

    _nextReview = now + VIGILANCE_REVIEW_SECONDS;
    Player* best = WarriorVigilance::FindTarget(botAI);
    return best && best != holder;
}

bool ShatteringThrowTrigger::IsActive()
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

// [b31_deep]
bool WarriorHasShatterableImmunity(Unit* unit)
{
    if (!unit)
        return false;

    if (unit->HasAura(SPELL_DIVINE_SHIELD) || unit->HasAura(SPELL_ICE_BLOCK))
        return true;

    for (uint32 const spellId : SPELL_BLESSING_OF_PROTECTION)
    {
        if (unit->HasAura(spellId))
            return true;
    }

    return false;
}

// [b31_deep]
int32 WarriorRage::Cost(Player* bot, uint32 spellId)
{
    SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    return info ? info->CalcPowerCost(bot, info->GetSchoolMask()) : 0;
}

// [b31_deep]
bool WarriorRage::HasRageFor(Player* bot, uint32 spellId)
{
    return int32(bot->GetPower(POWER_RAGE)) >= Cost(bot, spellId);
}

// [b31_deep]
bool ThunderClapTrigger::IsActive()
{
    if (!DebuffTrigger::IsActive())
        return false;

    Unit* target = GetTarget();
    return !botAI->HasAura("frost fever", target, false, false) &&
           !botAI->HasAura("judgements of the just", target, false, false) &&
           !botAI->HasAura("infected wounds", target, false, false);
}

// [b31_deep]
bool DemoralizingShoutDebuffTrigger::IsActive()
{
    if (!DebuffTrigger::IsActive())
        return false;

    return !WarriorDemoSlotCovered(botAI, GetTarget());  // [rot43] Demoralizing Screech too
}

// [b31_deep]
bool DeathWishTrigger::IsActive() { return AI_VALUE2(uint32, "spell id", spell) && BoostTrigger::IsActive(); }

// [b31_deep]
bool ShatteringThrowBossTrigger::IsActive()
{
    if (!bot->HasSpell(SPELL_SHATTERING_THROW) || bot->HasSpellCooldown(SPELL_SHATTERING_THROW) ||
        bot->GetShapeshiftForm() != FORM_BATTLESTANCE || !WarriorRage::HasRageFor(bot, SPELL_SHATTERING_THROW))
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !ai::raidutil::IsBossEngaged(botAI, target) || ai::raidutil::ShouldHoldBurstForLust(botAI) ||
        ai::raidutil::GetCombatSeconds(botAI) < SHATTERING_THROW_MIN_COMBAT_S)
        return false;

    return !botAI->HasAura("shattering throw", target, false, false) &&
           AI_VALUE2(float, "estimated lifetime", "current target") >= SHATTERING_THROW_MIN_LIFETIME_S;
}

// [b33_class] Battle Shout / Blessing of Might and Commanding Shout / Blood Pact are exclusive: the server keeps the
// stronger aura and the weaker cast is lost. The old check compared Battle Shout + Commanding Presence (~685 AP) with
// Blessing of Might's raw BasePoints (550), blind to Improved Blessing of Might (+25 %, ~687): the shout lost to the
// blessing on every cast and was recast 24 times in one boss pull (run 20261026-000434). Now: our talent-boosted
// shout vs the competitor's LIVE aura amount, plus a backstop when a fresh cast did not stick.
namespace
{
constexpr time_t SHOUT_OVERRIDDEN_RETRY_S = 60;  // [b33_class] no recast this long after a shout that did not stick
constexpr float COMMANDING_PRESENCE_BONUS[] = { 0.05f, 0.10f, 0.15f, 0.20f, 0.25f };

// [b33_class] Our shout's amount on the given aura type, with Commanding Presence; 0 when unknown.
int32 OwnShoutAmount(Player* bot, uint32 spellId, AuraType auraType)
{
    SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!info)
        return 0;

    int32 base = 0;
    for (uint8 eff = 0; eff < MAX_SPELL_EFFECTS; ++eff)
    {
        if (info->Effects[eff].ApplyAuraName == auraType)
        {
            base = info->Effects[eff].BasePoints + 1;
            break;
        }
    }

    float bonus = 0.0f;
    for (int rank = 4; rank >= 0; --rank)
    {
        if (bot->HasAura(SPELL_COMMANDING_PRESENCE_RANKS[rank]))
        {
            bonus = COMMANDING_PRESENCE_BONUS[rank];
            break;
        }
    }

    return int32(base * (1.0f + bonus));
}

// [b33_class] Strongest live amount (talents of its caster included) of the named auras on the bot.
int32 StrongestCompetitorAmount(PlayerbotAI* botAI, Player* bot, AuraType auraType,
                                std::initializer_list<char const*> names)
{
    int32 best = 0;
    for (char const* name : names)
    {
        Aura* aura = botAI->GetAura(name, bot);
        if (!aura)
            continue;

        for (uint8 eff = 0; eff < MAX_SPELL_EFFECTS; ++eff)
        {
            AuraEffect* effect = aura->GetEffect(eff);
            if (effect && effect->GetAuraType() == auraType)
                best = std::max(best, effect->GetAmount());
        }
    }

    return best;
}

// [b33_class] Cast lately (CastSpell stamps "last spell cast time") yet not on us: another aura overrode it.
bool ShoutRecentlyOverridden(PlayerbotAI* botAI, Player* bot, std::string const& shout)
{
    time_t const lastCast = botAI->GetAiObjectContext()->GetValue<time_t>("last spell cast time", shout)->Get();
    return lastCast && time(nullptr) - lastCast < SHOUT_OVERRIDDEN_RETRY_S && !botAI->HasAura(shout, bot);
}

// [b33_class] Shared by both shouts: false while a stronger exclusive buff holds the slot or a fresh cast was lost.
bool ShoutWorthCasting(PlayerbotAI* botAI, Player* bot, std::string const& shout, AuraType auraType,
                       std::initializer_list<char const*> competitors)
{
    if (ShoutRecentlyOverridden(botAI, bot, shout))
        return false;

    int32 const ownAmount =
        OwnShoutAmount(bot, botAI->GetAiObjectContext()->GetValue<uint32>("spell id", shout)->Get(), auraType);
    if (!ownAmount)
        return false;

    return StrongestCompetitorAmount(botAI, bot, auraType, competitors) < ownAmount;
}
}

bool BattleShoutTrigger::IsActive()
{
    if (!BuffTrigger::IsActive())
        return false;

    if (!AI_VALUE2(uint32, "spell id", "battle shout"))
        return false;

    // [b33_class] live Blessing of Might amount (Improved Blessing of Might included) vs our boosted shout
    return ShoutWorthCasting(botAI, bot, "battle shout", SPELL_AURA_MOD_ATTACK_POWER,
                             { "blessing of might", "greater blessing of might" });
}

bool RecklessnessReadyTrigger::IsActive()
{
    uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
    if (!spellId || bot->HasSpellCooldown(spellId))
        return false;

    return BoostTrigger::IsActive();
}

bool CriticalHealthSoloTrigger::IsActive() { return !bot->GetGroup() && LowHealthTrigger::IsActive(); }

// [b31_wr]
bool CommandingShoutTrigger::IsActive()
{
    // [b33_class] same exclusive-slot check as Battle Shout: live Blood Pact (Improved Imp included) vs our shout
    return AI_VALUE2(uint32, "spell id", spell) && BuffTrigger::IsActive() &&
           ShoutWorthCasting(botAI, bot, spell, SPELL_AURA_MOD_INCREASE_HEALTH_2, { "blood pact" });
}

// [b31_wr]
bool BattleShoutNoCommandingShoutTrigger::IsActive()
{
    if (AI_VALUE2(uint32, "spell id", "commanding shout"))
        return false;

    return BattleShoutTrigger::IsActive();
}

// [b31_wr]
bool WarriorSpellReadyTrigger::IsActive()
{
    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    return spellId && !bot->HasSpellCooldown(spellId) && (!checkRage || WarriorRage::HasRageFor(bot, spellId)) &&
           GetTarget();  // [b31_deep] rage
}

// [b31_wr]
bool WarriorStanceAoeTrigger::IsActive()
{
    uint32 const form = bot->GetShapeshiftForm();
    if (!form || form > 32 || !(stanceMask & (1u << (form - 1))))
        return false;

    if (!spellName.empty() && !AI_VALUE2(uint32, "spell id", spellName))  // [b31_deep] not known
        return false;

    return AoeTrigger::IsActive();
}

// [rot19]
bool FuryHeroicStrikeRageTrigger::IsActive()
{
    return RageAvailable::IsActive() && WarriorLeavesRageForStrike(botAI, bot, "heroic strike");
}

// [rot63]
bool ArmsHeroicStrikeTrigger::IsActive()
{
    uint32 const swingMs = bot->getAttackTimer(BASE_ATTACK);
    if (swingMs > std::max(botAI->GetGlobalCooldownRemaining(), botAI->GetReactDelay()))
        return false;

    return FuryHeroicStrikeRageTrigger::IsActive();
}
