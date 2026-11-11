/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PriestActions.h"

#include <algorithm>
#include <map>

#include "AiFactory.h"             // [dps240] the spec tab
#include "DpsEstimate.h"           // [dps240] FightsAlone
#include "Event.h"
#include "HealAssignmentValues.h"  // [dps240] the group health loss
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

namespace
{
// [dps240] spell_pri_power_word_shield (spell_priest.cpp): the absorb's healing power coefficient
constexpr float SHIELD_HEALING_COEFF = 0.8068f;
constexpr float HEAL_LOSS_WINDOW_SEC = 2.0f;  // GroupHealthLossValue: the share of max health lost in 2 s
constexpr uint8 PRIEST_TAB_DISCIPLINE = 0;
constexpr size_t SHIELD_LOG_MAX = 4096;

// a heal spell's mean amount on `target` with the caster's bonuses (Unit::SpellHealingBonusDone)
float MeanHeal(Player* bot, Unit* target, SpellInfo const* info)
{
    for (SpellEffectInfo const& effect : info->GetEffects())
        if (effect.Effect == SPELL_EFFECT_HEAL)
        {
            float const base = float(effect.BasePoints + 1) + float(std::max(effect.DieSides - 1, 0)) / 2.0f;
            return float(bot->SpellHealingBonusDone(target, info, uint32(std::max(base, 0.0f)), HEAL,
                                                    effect.EffectIndex));
        }
    return 0.0f;
}
}  // namespace

// [dps240] Power Word: Shield pays where the damage it absorbs before it expires heals more per mana than the best
// direct heal the priest knows does on the target's missing health (Rawr.HealPriest's healing per mana: a direct heal
// lands on what is missing, a shield on what comes), or where the target dies before the fastest of those heals
// lands. Discipline keeps its shield rotation (Rawr: talents raise the absorb, Rapture returns its mana); a priest
// fighting alone keeps the shield as its survival tool (SoloSafety judges its fights). The absorb is the spell's
// amount with the core's coefficient, the damage the target's net health loss over the last 2 s (the healers'
// "group health loss"); Weakened Soul (15 s) stays each shield action's own check. b424 rfc-n five5 pull 36:
// Orenem L17 (holy) cast 8 shields against 1 Heal and went dry: rank 2 absorbs 88 for 23 % of base mana (3.8 per
// %), Heal rank 1 restores 318 for 32 % (9.9 per %)
bool PowerWordShieldPays(PlayerbotAI* botAI, Unit* target)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || !target || !target->IsAlive())
        return false;
    if (AiFactory::GetPlayerSpecTab(bot) == PRIEST_TAB_DISCIPLINE || DpsEstimate::FightsAlone(bot))
        return true;
    AiObjectContext* context = botAI->GetAiObjectContext();
    SpellInfo const* shield = sSpellMgr->GetSpellInfo(AI_VALUE2(uint32, "spell id", "power word: shield"));
    if (!shield)
        return false;
    float absorb = 0.0f;
    for (SpellEffectInfo const& effect : shield->GetEffects())
        if (effect.ApplyAuraName == SPELL_AURA_SCHOOL_ABSORB)
            absorb = float(effect.CalcValue(bot));
    absorb += SHIELD_HEALING_COEFF * float(bot->SpellBaseHealingBonusDone(shield->GetSchoolMask()));
    float const shieldCost = float(std::max(1, shield->CalcPowerCost(bot, shield->GetSchoolMask())));
    float const durationSec = float(std::max(shield->GetMaxDuration(), 0)) / float(IN_MILLISECONDS);

    HealthLossMap const& losses = context->GetValue<HealthLossMap>("group health loss")->RefGet();
    float const lossPerSec =
        RecentHealthLossPct(target, losses) / 100.0f * float(target->GetMaxHealth()) / HEAL_LOSS_WINDOW_SEC;
    float const absorbed = std::min(absorb, lossPerSec * durationSec);

    float bestPerMana = 0.0f;
    float fastestSec = 0.0f;
    for (char const* name : {"lesser heal", "heal", "greater heal", "flash heal"})
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(AI_VALUE2(uint32, "spell id", name));
        if (!info)
            continue;
        float const castSec = float(info->CalcCastTime(bot)) / float(IN_MILLISECONDS);
        float const missing = float(target->GetMaxHealth() - target->GetHealth()) + lossPerSec * castSec;
        float const heal = std::min(MeanHeal(bot, target, info), missing);
        float const cost = float(std::max(1, info->CalcPowerCost(bot, info->GetSchoolMask())));
        bestPerMana = std::max(bestPerMana, heal / cost);
        fastestSec = fastestSec > 0.0f ? std::min(fastestSec, castSec) : castSec;
    }
    bool const dying = lossPerSec > 0.0f && float(target->GetHealth()) <= lossPerSec * fastestSec;
    bool const pays = dying || bestPerMana <= 0.0f || absorbed / shieldCost >= bestPerMana;

    // one line per verdict change for this priest and target
    thread_local std::map<std::pair<uint64, uint64>, bool> logged;
    auto const key = std::make_pair(bot->GetGUID().GetRawValue(), target->GetGUID().GetRawValue());
    auto const found = logged.find(key);
    if (found == logged.end() || found->second != pays)
    {
        if (logged.size() > SHIELD_LOG_MAX)
            logged.clear();
        logged[key] = pays;
        LOG_INFO("playerbots",
                 "[dps240] {} L{} shield on {} ({:.0f} %): {} - absorbs {:.0f} of {:.0f} at {:.1f} hp/s for {:.0f} "
                 "mana ({:.2f} a mana), best heal {:.2f} a mana{}",
                 bot->GetName(), bot->GetLevel(), target->GetName(), target->GetHealthPct(), pays ? "pays" : "held",
                 absorbed, absorb, lossPerSec, shieldCost, absorbed / shieldCost, bestPerMana,
                 dying ? ", dies before a heal lands" : "");
    }
    return pays;
}

bool CastPowerWordShieldAction::isUseful()
{
    return CastBuffSpellAction::isUseful() && PowerWordShieldPays(botAI, bot);
}

bool CastPowerWordShieldOnPartyAction::isUseful()
{
    return HealPartyMemberAction::isUseful() && PowerWordShieldPays(botAI, GetTarget());
}

bool CastRemoveShadowformAction::Execute(Event /*event*/)
{
    botAI->RemoveAura("shadowform");
    return true;
}

// [b31_deep]
bool CastShadowWordDeathAction::isUseful()
{
    return AI_VALUE2(uint8, "health", "self target") > sPlayerbotAIConfig.LowHealth && CastSpellAction::isUseful();
}

// [b31_deep]
Unit* CastPowerWordShieldOnPartyAction::GetTarget()
{
    Unit* triaged = HealPartyMemberAction::GetTarget();
    if (!triaged || !botAI->HasAnyAuraOf(triaged, "weakened soul", "power word: shield", nullptr))
        return triaged;

    // a member in the low / critical / emergency bands keeps the GCD: the node's next heal goes to him
    Group* group = bot->GetGroup();
    if (!group || triaged->GetHealthPct() < sPlayerbotAIConfig.LowHealth)
        return triaged;

    Player* best = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == triaged || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
            member->GetHealthPct() >= sPlayerbotAIConfig.MediumHealth ||
            member->GetDistance2d(bot) > sPlayerbotAIConfig.SpellDistance ||
            botAI->HasAnyAuraOf(member, "weakened soul", "power word: shield", nullptr))
            continue;

        if (!best || member->GetHealthPct() < best->GetHealthPct())
            best = member;
    }

    return best ? best : triaged;
}

bool CastShadowWordPainAction::isUseful()
{
    if (!ai::priest::ShadowWeavingReady(botAI, bot, GetTarget()))
        return false;

    beforeDuration = ai::priest::ShadowWordPainRefreshMs(bot);
    return CastPriestDotAction::isUseful();
}

bool CastVampiricTouchAction::isUseful()
{
    beforeDuration = ai::priest::VampiricTouchRefreshMs(bot, AI_VALUE2(uint32, "spell id", spell));
    return CastPriestDotAction::isUseful();
}

bool CastDevouringPlagueAction::isUseful()
{
    if (ai::priest::DevouringPlagueFinalCast(botAI, GetTarget()))
        return CastSpellAction::isUseful();

    return CastPriestDotAction::isUseful();
}

bool CastFadeAction::isUseful()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    return bot->GetGroup() && !(target && target->IsPlayer());  // players have no threat
}

bool CastPowerWordShieldOnMainTankAction::isUseful()
{
    return BuffOnMainTankAction::isUseful() && !botAI->HasAura("weakened soul", GetTarget()) &&
           PowerWordShieldPays(botAI, GetTarget());  // [dps240]
}

bool CastRemoveShadowformAction::isUseful() { return botAI->HasAura("shadowform", AI_VALUE(Unit*, "self target")); }

Unit* CastPowerWordShieldOnAlmostFullHealthBelowAction::GetTarget()
{
    Group* group = bot->GetGroup();
    if (!group)  // [b31_deep]
        return nullptr;

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (!player)
            continue;
        if (player->isDead())
        {
            continue;
        }
        if (player->GetHealthPct() > sPlayerbotAIConfig.AlmostFullHealth)
        {
            continue;
        }
        if (player->GetDistance2d(bot) > sPlayerbotAIConfig.SpellDistance)
        {
            continue;
        }
        if (botAI->HasAnyAuraOf(player, "weakened soul", "power word: shield", nullptr))
        {
            continue;
        }
        return player;
    }
    return nullptr;
}

bool CastPowerWordShieldOnAlmostFullHealthBelowAction::isUseful()
{
    Group* group = bot->GetGroup();
    if (!group)  // [b31_deep]
        return false;

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (!player)
            continue;
        if (player->isDead())
        {
            continue;
        }
        if (player->GetHealthPct() > sPlayerbotAIConfig.AlmostFullHealth)
        {
            continue;
        }
        if (player->GetDistance2d(bot) > sPlayerbotAIConfig.SpellDistance)
        {
            continue;
        }
        if (botAI->HasAnyAuraOf(player, "weakened soul", "power word: shield", nullptr))
        {
            continue;
        }
        return true;
    }
    return false;
}

Unit* CastPowerWordShieldOnNotFullAction::GetTarget()
{
    Group* group = bot->GetGroup();
    if (!group)  // [b31_deep]
        return nullptr;

    MinValueCalculator calc(100);
    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (!player)
            continue;
        if (player->isDead() || player->IsFullHealth())
        {
            continue;
        }
        if (player->GetDistance2d(bot) > sPlayerbotAIConfig.SpellDistance)
        {
            continue;
        }
        if (botAI->HasAnyAuraOf(player, "weakened soul", "power word: shield", nullptr))
        {
            continue;
        }
        calc.probe(player->GetHealthPct(), player);
    }
    return (Unit*)calc.param;
}

bool CastPowerWordShieldOnNotFullAction::isUseful()
{
    return GetTarget();
}

bool CastPowerWordShieldOnValueTargetAction::isUseful()
{
    Unit* target = GetTarget();
    return target && CastHealOnValueTargetAction::isUseful() && !botAI->HasAura("weakened soul", target) &&
           PowerWordShieldPays(botAI, target);  // [dps240]
}
