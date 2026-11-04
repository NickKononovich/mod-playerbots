/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PaladinActions.h"
#include "AiFactory.h"
#include "EstimatedLifetimeValue.h"  // [rot281] ExpectedLifetime
#include "Event.h"
#include "GenericBuffUtils.h"
#include "ObjectAccessor.h"
#include "PaladinHelper.h"
#include "Pet.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "SpellAuras.h"
#include <algorithm>

bool CastSealSpellAction::isUseful()
{
    return AI_VALUE2(bool, "combat", "self target");
}

Value<Unit*>* CastTurnUndeadAction::GetTargetValue()
{
    return context->GetValue<Unit*>("cc target", getName());
}

Unit* CastHandOfFreedomOnPartyAction::GetTarget()
{
    bool const selfImpaired = ai::paladin::HasFreedomRemovableImpairment(bot);  // [b33_class] root/snare only
    bool const hasSelfHand =
        selfImpaired && ai::paladin::HasAnyPaladinHandFromCaster(bot, bot);

    if (!bot->GetGroup())
    {
        if (selfImpaired && !hasSelfHand)
            return bot;

        return nullptr;
    }

    if (selfImpaired && !hasSelfHand)
        return bot;

    return CastBuffSpellAction::GetTarget();
}

Value<Unit*>* CastHandOfFreedomOnPartyAction::GetTargetValue()
{
    return context->GetValue<Unit*>("party member snared target");
}

bool CastHandOfFreedomOnPartyAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    return CastBuffSpellAction::isUseful() &&
           !ai::paladin::HasAnyPaladinHandFromCaster(target, bot);
}

Unit* CastRighteousDefenseAction::GetTarget()
{
    Unit* current_target = AI_VALUE(Unit*, "current target");
    if (!current_target)
        return nullptr;

    return current_target->GetVictim();
}

bool CastDivineSacrificeAction::isUseful()
{
    return GetTarget() && (GetTarget() != nullptr) && CastSpellAction::isUseful() &&
           !botAI->HasAura("divine guardian", GetTarget(), false, false, -1, true);
}

bool CastCancelDivineSacrificeAction::Execute(Event /*event*/)
{
    botAI->RemoveAura("divine sacrifice");
    return true;
}

bool CastCancelDivineSacrificeAction::isUseful()
{
    return botAI->HasAura("divine sacrifice", GetTarget(), false, true, -1, true);
}

bool CastLayOnHandsOnPartyAction::isUseful()
{
    Unit* target = GetTarget();
    return target && target != bot && HealPartyMemberAction::isUseful();
}

bool CastDivineShieldAction::isUseful() { return !PlayerbotAI::IsTank(bot) && CastBuffSpellAction::isUseful(); }

bool CastHandOfSacrificeOnSpikingTankAction::isUseful()
{
    Unit* target = GetTarget();
    return target && target != bot && !PlayerbotAI::IsTank(bot) &&
           bot->GetHealthPct() > sPlayerbotAIConfig.MediumHealth && CastSpikeProtectSpellAction::isUseful();
}

bool DropStaleSealAction::Execute(Event /*event*/)
{
    // [rot_b31_pdd] PlayerbotAI::RemoveAura removes a seal only when the bot has it
    for (char const* seal : {"seal of righteousness", "seal of justice", "seal of light", "seal of wisdom",
                             "seal of command", "seal of vengeance", "seal of corruption"})
        botAI->RemoveAura(seal);

    LOG_INFO("playerbots", "[rot_b31_pdd] {} dropped a seal without AURA_STATE_JUDGEMENT", bot->GetName());
    return true;
}

// [b31_deep]
bool ai::paladin::HasHolyWrathTargetWithin(PlayerbotAI* botAI, Player* bot, float range)
{
    auto const isHolyWrathType = [](Unit* unit)
    {
        if (!unit || !unit->IsAlive())
            return false;

        uint32 const creatureType = unit->GetCreatureType();
        return creatureType == CREATURE_TYPE_UNDEAD || creatureType == CREATURE_TYPE_DEMON;
    };

    AiObjectContext* context = botAI->GetAiObjectContext();
    Unit* target = context->GetValue<Unit*>("current target")->Get();
    if (isHolyWrathType(target) && bot->GetDistance(target) <= range)
        return true;

    for (ObjectGuid const& guid : context->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit != target && isHolyWrathType(unit) && bot->GetDistance(unit) <= range)
            return true;
    }

    return false;
}

// [b31_deep]
bool CastHolyWrathAction::isUseful()
{
    return CastMeleeSpellAction::isUseful() && ai::paladin::HasHolyWrathTargetWithin(botAI, bot, HOLY_WRATH_RADIUS);
}

// [b31_deep]
bool CastHolyWrathOnGroupMemberAttackersAction::isUseful()
{
    return CastAreaSpellOnGroupMemberAttackersAction::isUseful() &&
           ai::paladin::HasHolyWrathTargetWithin(botAI, bot, CastHolyWrathAction::HOLY_WRATH_RADIUS);
}

// [b34_pal]
static bool IsHolyWrathCreatureType(Unit* unit)
{
    if (!unit || !unit->IsAlive())
        return false;

    uint32 const creatureType = unit->GetCreatureType();
    return creatureType == CREATURE_TYPE_UNDEAD || creatureType == CREATURE_TYPE_DEMON;
}

// [b34_pal]
uint32 ai::paladin::CountHolyWrathTargetsWithin(PlayerbotAI* botAI, Player* bot, float range)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    Unit* target = context->GetValue<Unit*>("current target")->Get();
    uint32 count = IsHolyWrathCreatureType(target) && bot->GetDistance(target) <= range ? 1 : 0;

    for (ObjectGuid const& guid : context->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit != target && IsHolyWrathCreatureType(unit) && bot->GetDistance(unit) <= range)
            ++count;
    }

    return count;
}

// [b34_pal] Holy Wrath damages undead and demons only: only those can lose a crowd control to it or be pulled by it.
bool ai::paladin::HolyWrathWouldBreakCcOrPull(PlayerbotAI* botAI, Player* bot)
{
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!IsHolyWrathCreatureType(unit) || bot->GetDistance(unit) > CastHolyWrathAction::HOLY_WRATH_RADIUS)
            continue;

        if (unit->HasBreakableByDamageCrowdControlAura() || (unit->IsCreature() && !unit->IsInCombat()))
            return true;
    }

    return false;
}

// [b34_pal]
bool CastHolyWrathUndeadPackAction::isUseful()
{
    return CastSpellAction::isUseful() &&
           ai::paladin::CountHolyWrathTargetsWithin(botAI, bot, ai::paladin::HOLY_WRATH_PACK_RANGE) >=
               ai::paladin::HOLY_WRATH_PACK_MIN &&
           !ai::paladin::HolyWrathWouldBreakCcOrPull(botAI, bot);
}

// [b31_deep]
bool CastRetConsecrationAction::isUseful()
{
    if (!CastConsecrationAction::isUseful())
        return false;

    Unit* target = GetTarget();
    if (!target || !target->IsAlive())
        return false;

    // [rot281] the APL's remainingTime is the fight's, not the current target's: the longest ExpectedLifetime of the
    // target and the group's other attackers - a dying add is no end of the fight, its pack fights on (b218 rfc-h 5G
    // Rtpalae: Consecration 2 / 3 / 2 / 2 vs wowsims 3 / 4 / 3 / 3 in the boss fights)
    float remaining = ExpectedLifetime(botAI, target);
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "attackers"))
        if (Unit* mob = botAI->GetUnit(guid); mob && mob != target && mob->IsAlive())
            remaining = std::max(remaining, ExpectedLifetime(botAI, mob));
    return remaining > MIN_TARGET_LIFE_SECS;
}

// [b31_pal]
bool CastAvengingWrathAction::isUseful()
{
    constexpr uint32 FORBEARANCE = 25771;
    constexpr uint32 AVENGING_WRATH_MARKER = 61987;
    return !bot->HasAura(FORBEARANCE) && !bot->HasAura(AVENGING_WRATH_MARKER) && CastBuffSpellAction::isUseful();
}

bool CastDivinePleaAction::isUseful()
{
    // [b31_deep] trained at 71 (Spell.dbc spellLevel 71): the tank's "high mana" queued it at 60-70 and it failed on
    // "spell not known" (build 30 L60s: 2504 refusals); the cached spell id is 0 then
    if (!AI_VALUE2(uint32, "spell id", "divine plea"))
        return false;

    if (!CastBuffSpellAction::isUseful())
        return false;

    // [rot150] a tank with Guarded by the Light keeps Divine Plea for the fight, where its hits keep it up (the
    // non-combat "medium mana" K4 line spent it between pulls: TankDivinePleaTrigger)
    // [lead1040] ... and gives it on trash only back before the boss, out of a fight only just before a boss pull
    if (PlayerbotAI::IsTank(bot) && ai::paladin::KeepsDivinePleaUpByHits(bot) && !ai::paladin::TankPleaNow(bot))
        return false;

    if (!PlayerbotAI::IsHeal(bot))
        return true;

    // Divine Plea halves healing done, so a healer holds it while someone needs big heals or the group takes
    // area damage; between pulls nothing holds it back.
    if (!bot->IsInCombat())
        return true;

    Unit* target = AI_VALUE(Unit*, "party member to heal");
    if (target && target->GetHealthPct() < sPlayerbotAIConfig.MediumHealth)
        return false;

    return AI_VALUE2(uint8, "aoe heal", "medium") < 3;
}

// [b31_holyshield_pull] A paladin tank out of combat, at 30 % mana or more, that knows Holy Shield and has none up.
bool CastHolyShieldPrePullAction::isUseful()
{
    if (bot->IsInCombat() || !PlayerbotAI::IsTank(bot) || bot->GetPowerPct(POWER_MANA) < MIN_MANA_PCT)
        return false;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    return spellId && bot->HasSpell(spellId) && CastBuffSpellAction::isUseful();
}

bool CastHolyShieldPrePullAction::Execute(Event event)
{
    if (!CastBuffSpellAction::Execute(event))
        return false;

    LOG_INFO("playerbots", "[holy shield pre-pull] {}: Holy Shield before the pull ({:.0f} % mana)", bot->GetName(),
             bot->GetPowerPct(POWER_MANA));
    return true;
}
