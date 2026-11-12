/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LeaderHazardVerdict.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "Creature.h"
#include "EstimatedLifetimeValue.h"  // GroupDamage
#include "HealthTriggers.h"          // RestState::HealerThroughput
#include "LeaderFightChecks.h"       // LEADER_HAZARD_EDGE, LeaderMoveManaLine
#include "LeaderGroup.h"
#include "LeaderSupport.h"  // Support::MemberTakenPerSec
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "ScriptMgr.h"
#include "SmartScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "UnitScript.h"

namespace Leader::HazardVerdict
{
namespace
{
// The instance's damage factor is a UnitScript hook of mod-raid-rescale (ModifySpellDamageTaken /
// ModifyPeriodicDamageAurasTick: RFC heroic spells x30). The other scripts on those hooks record what landed (the
// death recorder, the harness's spell stats): an estimate must not reach them, so only the rescale's own script is
// asked, by its registered name (no rescale loaded: the amount stays as it is)
constexpr char const* RESCALE_SCRIPT = "RaidRescaleUnitScript";

uint32 InstanceFactorApplied(Unit* victim, Unit* caster, uint32 amount, SpellInfo const* info, bool periodic)
{
    if (!caster)
        return amount;
    auto const& hooks = ScriptRegistry<UnitScript>::EnabledHooks;
    uint16 const hook = periodic ? UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK : UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN;
    if (hook >= hooks.size())
        return amount;
    for (UnitScript* script : hooks[hook])
    {
        if (!script || script->GetName() != RESCALE_SCRIPT)
            continue;
        if (periodic)
            script->ModifyPeriodicDamageAurasTick(victim, caster, amount, info);
        else
        {
            int32 damage = int32(amount);
            script->ModifySpellDamageTaken(victim, caster, damage, info);
            amount = uint32(std::max(damage, 0));
        }
    }
    return amount;
}

// the mean of an effect's value at the caster's level (CalcValue rolls the die sides: their mean over the samples)
float MeanValue(SpellEffectInfo const& effect, Unit* caster)
{
    if (std::abs(effect.DieSides) <= 1)
        return float(effect.CalcValue(caster));
    constexpr int32 SAMPLES = 8;
    float sum = 0.0f;
    for (int32 i = 0; i < SAMPLES; ++i)
        sum += float(effect.CalcValue(caster));
    return sum / float(SAMPLES);
}

// one amount of `info` on `victim` the way the core lands it: the caster's and the victim's spell bonuses
// (Spell::EffectSchoolDMG, AuraEffect::HandlePeriodicDamageAurasTick), the instance's factor and the armor
// (Unit::CalculateSpellDamageTaken), else the average resistance (Unit::CalcAbsorbResist)
float Landed(Unit* caster, SpellInfo const* info, Player* victim, float value, uint8 effIndex, bool periodic,
             bool weapon)
{
    uint32 amount = uint32(std::max(value, 0.0f));
    DamageEffectType const type = periodic ? DOT : SPELL_DIRECT_DAMAGE;
    if (caster && !weapon)
    {
        amount = caster->SpellDamageBonusDone(victim, info, amount, type, effIndex);
        amount = victim->SpellDamageBonusTaken(caster, info, amount, type);
    }
    amount = InstanceFactorApplied(victim, caster, amount, info, periodic);
    SpellSchoolMask const school = info->GetSchoolMask();
    if (Unit::IsDamageReducedByArmor(school, info, effIndex))
        return float(Unit::CalcArmorReducedDamage(caster, victim, amount, info, caster ? 0 : victim->GetLevel()));
    return float(amount) *
           (1.0f - Unit::GetEffectiveResistChance(caster, school, victim, info, caster ? 0 : victim->GetLevel()));
}

struct SpellHit
{
    float direct = 0.0f;  // its hit (school damage, weapon damage)
    float tick = 0.0f;    // one tick of its periodic damage
    uint32 ticks = 0;     // the ticks of that periodic damage over its duration
};

SpellHit DamageOf(Unit* caster, SpellInfo const* info, Player* victim)
{
    SpellHit hit;
    float weapon = 0.0f;
    float weaponPct = 1.0f;
    int8 weaponIndex = -1;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effect = info->Effects[i];
        switch (effect.Effect)
        {
            case SPELL_EFFECT_SCHOOL_DAMAGE:
                hit.direct += Landed(caster, info, victim, MeanValue(effect, caster), i, false, false);
                break;
            case SPELL_EFFECT_WEAPON_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
            case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                weapon += MeanValue(effect, caster);
                weaponIndex = int8(i);
                break;
            case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                weaponPct *= MeanValue(effect, caster) / 100.0f;
                weaponIndex = int8(i);
                break;
            case SPELL_EFFECT_APPLY_AURA:
            case SPELL_EFFECT_PERSISTENT_AREA_AURA:
            case SPELL_EFFECT_APPLY_AREA_AURA_ENEMY:
                if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE && effect.Amplitude > 0)
                {
                    hit.tick += Landed(caster, info, victim, MeanValue(effect, caster), i, true, false);
                    int32 const duration = info->GetMaxDuration();
                    hit.ticks = duration > 0 ? uint32(duration / effect.Amplitude) : 0;
                }
                break;
            default:
                break;
        }
    }
    // the caster's weapon (its rescaled damage range) with the effects' bonus and percent
    if (weaponIndex >= 0 && caster)
    {
        float const swing = (caster->GetWeaponDamageRange(BASE_ATTACK, MINDAMAGE) +
                             caster->GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE)) /
                            2.0f;
        hit.direct += Landed(caster, info, victim, (swing + weapon) * weaponPct, uint8(weaponIndex), false, true);
    }
    return hit;
}

std::string Amount(float value)
{
    return value >= 1000.0f || value <= -1000.0f ? Acore::StringFormat("{:.1f}k", value / 1000.0f)
                                                 : Acore::StringFormat("{:.0f}", value);
}

bool IsMelee(Player* member) { return Leader::Fight::LeaderFightsMelee(member); }  // [lead1329]

// The rotation's own loss while the member is away `awaySec`: its damage over time on `target` that keeps ticking
// meanwhile is no loss (the measured dps has it in), only past its end; its own damage buffs that run out
// meanwhile are off until it recasts them on its return - the buff's share of its damage over the recast's global
// cooldown. Returns the net change to dps x awaySec (negative: the ticks that still land)
float RotationTerm(Player* member, Unit* target, float awaySec, float dps, std::string& parts)
{
    float term = 0.0f;
    if (target)
        for (AuraEffect const* effect : target->GetAuraEffectsByType(SPELL_AURA_PERIODIC_DAMAGE))
        {
            if (!effect || effect->GetCasterGUID() != member->GetGUID() || !effect->GetBase() ||
                effect->GetAmplitude() <= 0)
                continue;
            float const leftSec = std::max(effect->GetBase()->GetDuration(), 0) / 1000.0f;
            uint8 const stacks = effect->GetBase()->GetStackAmount();
            uint32 tick = member->SpellDamageBonusDone(target, effect->GetSpellInfo(),
                                                       uint32(std::max(effect->GetAmount(), 0)), DOT,
                                                       effect->GetEffIndex(), 0.0f, stacks);
            tick = target->SpellDamageBonusTaken(member, effect->GetSpellInfo(), tick, DOT,
                                                 effect->GetBase()->GetStackAmount());
            float const still = float(tick) * 1000.0f / float(effect->GetAmplitude()) * std::min(leftSec, awaySec);
            if (still <= 0.0f)
                continue;
            term -= still;
            parts += Acore::StringFormat(", {} ticks on {:.1f} s -{}", effect->GetSpellInfo()->SpellName[LOCALE_enUS],
                                         std::min(leftSec, awaySec), Amount(still));
        }
    for (auto const& [id, application] : member->GetAppliedAuras())
    {
        Aura const* aura = application ? application->GetBase() : nullptr;
        if (!aura || aura->GetCasterGUID() != member->GetGUID())
            continue;
        float pct = 0.0f;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (AuraEffect const* effect = aura->GetEffect(i);
                effect && effect->GetAuraType() == SPELL_AURA_MOD_DAMAGE_PERCENT_DONE)
                pct += float(effect->GetAmount());
        if (pct <= 0.0f)
            continue;
        // a linked buff (Savage Roar's 62071 under 52610) runs as long as the timed aura of its name
        Aura const* timed = aura->GetMaxDuration() > 0 ? aura : nullptr;
        if (!timed)
            for (auto const& [otherId, other] : member->GetAppliedAuras())
                if (Aura const* base = other ? other->GetBase() : nullptr;
                    base && base != aura && base->GetCasterGUID() == member->GetGUID() && base->GetMaxDuration() > 0 &&
                    std::string(base->GetSpellInfo()->SpellName[LOCALE_enUS]) ==
                        aura->GetSpellInfo()->SpellName[LOCALE_enUS])
                    timed = base;
        if (!timed || timed->GetDuration() / 1000.0f >= awaySec)
            continue;
        float const gcdSec = timed->GetSpellInfo()->StartRecoveryTime / 1000.0f;
        float const lost = dps * pct / (100.0f + pct) * gcdSec;
        term += lost;
        parts += Acore::StringFormat(", {} runs out in {:.1f} s +{}", timed->GetSpellInfo()->SpellName[LOCALE_enUS],
                                     timed->GetDuration() / 1000.0f, Amount(lost));
    }
    return term;
}

// The verdict from the two costs: `burst` the first hit, `perSec` the hazard's ticks while it stays in, `taken`
// all it deals over the stay; `awaySec` the member's seconds without damage when it leaves. [lead820] `toCome` the
// damage of the hazard's hits still to come in the fight (`toComeWhy` their count and cadence): the healer's mana
// heals them all, with the other stays' heals (`key`: the hazard, Facts::stayHeals)
Verdict Decide(Facts const& facts, Player* member, Unit* target, std::string const& key, std::string const& what,
               float burst, float perSec, float taken, float awaySec, float toCome, std::string const& toComeWhy)
{
    Verdict verdict;
    verdict.heal = taken + toCome;
    float const dps = facts.fighting ? GroupDamage::MemberFormulaDps(member) : 0.0f;  // [lead1303] the formula's
    std::string rotation;
    float const lostDps = dps * awaySec;
    float const lost = std::max(lostDps + RotationTerm(member, target, awaySec, dps, rotation), 0.0f);

    // the healers: their gap between two heals, their rate, the main healer's mana
    std::vector<Player*> everyone(facts.seats ? *facts.seats : std::vector<Player*>());
    if (facts.tank)
        everyone.push_back(facts.tank);
    float gapSec = std::numeric_limits<float>::max();
    float hps = 0.0f;
    float incoming = 0.0f;
    Player* mainHealer = nullptr;
    RestState::HealerFacts mainFacts;
    for (Player* bot : everyone)
    {
        if (!bot || !bot->IsAlive())
            continue;
        incoming += Leader::Support::MemberTakenPerSec(*facts.group, bot);
        if (!PlayerbotAI::IsHeal(bot))
            continue;
        RestState::HealerFacts const healer = RestState::HealerThroughput(bot, member);
        if (healer.hps <= 0.0f)
            continue;
        hps += healer.hps;
        gapSec = std::min(gapSec, healer.gapSec);
        if (!mainHealer || healer.hps > mainFacts.hps)
        {
            mainHealer = bot;
            mainFacts = healer;
        }
    }
    float const takenPerSec = Leader::Support::MemberTakenPerSec(*facts.group, member);
    float const health = float(member->GetHealth());
    float const afterHit = health - burst;
    float const afterGap = mainHealer ? afterHit - (takenPerSec + perSec) * gapSec : afterHit;
    bool const survives = mainHealer && afterGap > 0.0f;
    float const spare = hps - incoming - perSec;
    bool const healRate = mainHealer && spare > 0.0f;
    float const coverSec = healRate ? taken / spare : 0.0f;
    // the main healer's mana after healing it back, against its line for the fight's seconds left
    // [lead820] every hit still to come in the fight, not the next one only, and the heal the other stays ask (the
    // other members' and its own other hazards'): one mana pool heals them all
    bool manaHolds = true;
    std::string mana;
    if (mainHealer && mainFacts.maxMana > 0.0f)
    {
        float others = 0.0f;
        if (facts.stayHeals)
            for (auto const& [guid, heals] : *facts.stayHeals)
                for (auto const& [hazard, heal] : heals)
                    if (guid != member->GetGUID() || hazard != key)
                        others += heal;
        float const nowPct = mainHealer->GetPowerPct(POWER_MANA);
        float const afterPct =
            mainFacts.healPerMana > 0.0f
                ? nowPct - (verdict.heal + others) / mainFacts.healPerMana / mainFacts.maxMana * 100.0f
                : 0.0f;
        float const line =
            Leader::Fight::LeaderMoveManaLine(mainHealer, true, facts.fightSecLeft, facts.bossFight, nullptr);
        manaHolds = mainFacts.healPerMana > 0.0f && (line < 0.0f || afterPct >= line);
        mana = Acore::StringFormat(", [lead820] {} mana {:.0f} % -> {:.0f} % (line {:.0f} %) healing {} ({} now + "
                                   "{} {}{})",
                                   mainHealer->GetName(), nowPct, afterPct, line, Amount(verdict.heal + others),
                                   Amount(taken), Amount(toCome), toComeWhy,
                                   others > 0.0f ? " + " + Amount(others) + " the other stays" : "");
    }
    bool const cheaper = lost > taken;
    verdict.stays = dps > 0.0f && cheaper && survives && healRate && manaHolds;
    std::string const because = !facts.fighting ? " (no fight: the move costs no damage)"
                                : dps <= 0.0f  ? " (no measured dps)"
                                : !cheaper     ? " (the move costs less)"
                                : !mainHealer  ? " (no healer heals it back)"
                                : !survives    ? " (it would not outlive the heal gap)"
                                : !healRate    ? " (the healers' rate is spent)"
                                : !manaHolds   ? " (the healer's mana line)"
                                               : "";
    verdict.why = Acore::StringFormat(
        "{}{}: {} {} vs {} dps lost ({:.1f} s away x {} dps{}), health {} -> {} ({} after the {:.1f} s heal gap at {} "
        "taken/s{}), heal covers in {:.1f} s (heal {}/s vs taken {}/s{}){}",
        verdict.stays ? "stays" : "leaves", because, Amount(taken), what, Amount(lost), awaySec, Amount(dps), rotation,
        Amount(health), Amount(afterHit), Amount(afterGap), mainHealer ? gapSec : 0.0f, Amount(takenPerSec),
        perSec > 0.0f ? " + " + Amount(perSec) + " its ticks" : "", coverSec, Amount(hps), Amount(incoming),
        perSec > 0.0f ? " + " + Amount(perSec) : "", mana);
    return verdict;
}

// [lead780] a cleaver's cone spell from its data: the damage spell of its SmartAI in-combat timers or its template
// spells with a cone target (TARGET_SELECT_CATEGORY_CONE), and the mean seconds between two casts
SpellInfo const* ConeSpell(Creature* creature, float& everySec)
{
    auto const cone = [](SpellInfo const* info)
    {
        if (!info || info->IsPositive())
            return false;
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.TargetA.GetSelectionCategory() == TARGET_SELECT_CATEGORY_CONE ||
                effect.TargetB.GetSelectionCategory() == TARGET_SELECT_CATEGORY_CONE)
                return true;
        return false;
    };
    if (creature->GetAIName() == "SmartAI")
        for (SmartScriptHolder const& holder :
             sSmartScriptMgr->GetScript(int32(creature->GetEntry()), SMART_SCRIPT_TYPE_CREATURE))
            if (holder.GetActionType() == SMART_ACTION_CAST && holder.GetEventType() == SMART_EVENT_UPDATE_IC &&
                holder.event.minMaxRepeat.repeatMax)
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(holder.action.cast.spell); cone(info))
                {
                    everySec = float(holder.event.minMaxRepeat.repeatMin + holder.event.minMaxRepeat.repeatMax) /
                               2.0f / 1000.0f;
                    return info;
                }
    if (CreatureTemplate const* proto = creature->GetCreatureTemplate())
        for (uint32 spellId : proto->spells)
            if (SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr; cone(info))
            {
                everySec = std::max({info->RecoveryTime, info->CategoryRecoveryTime, info->CalcCastTime(creature)}) /
                           1000.0f;
                return info;
            }
    return nullptr;
}

// [lead820] the seconds between two of `caster`'s casts of `info`: its encounter timer (the SmartAI in-combat event
// that casts it: the mean of its repeat), else the spell's own interval (its cooldown, category cooldown, cast time
// and global cooldown: the longest); 0: not known (`source` says which). [lead1307] coordinator 2026-11-13: from the
// creature and spell data only - the interval of its casts seen in this fight is this fight's timing, no rate: it goes
// to `source` (the log) only
float CadenceSec(Facts const& facts, Unit* caster, SpellInfo const* info, std::string& source)
{
    float everySec = 0.0f;
    if (Creature* creature = caster ? caster->ToCreature() : nullptr; creature && creature->GetAIName() == "SmartAI")
        for (int32 const id : {-int32(creature->GetSpawnId()), int32(creature->GetEntry())})
        {
            if (!id || everySec > 0.0f)
                continue;
            for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(id, SMART_SCRIPT_TYPE_CREATURE))
                if (holder.GetActionType() == SMART_ACTION_CAST && holder.action.cast.spell == info->Id &&
                    holder.GetEventType() == SMART_EVENT_UPDATE_IC && holder.event.minMaxRepeat.repeatMax)
                {
                    source = Acore::StringFormat("its encounter timer {:.0f}-{:.0f} s",
                                                 holder.event.minMaxRepeat.repeatMin / 1000.0f,
                                                 holder.event.minMaxRepeat.repeatMax / 1000.0f);
                    everySec = float(holder.event.minMaxRepeat.repeatMin + holder.event.minMaxRepeat.repeatMax) /
                               2.0f / 1000.0f;
                    break;
                }
        }
    if (everySec <= 0.0f)
    {
        // a spell with a cooldown of its own: the longest of its cooldowns, its cast and its global cooldown (a
        // spell with no cooldown has no repeat its data knows)
        uint32 const cooldownMs = std::max(info->RecoveryTime, info->CategoryRecoveryTime);
        if (cooldownMs)
        {
            uint32 const castMs = caster ? uint32(info->CalcCastTime(caster)) : 0;
            everySec = float(std::max({cooldownMs, castMs, uint32(info->StartRecoveryTime)})) / 1000.0f;
            source = "its cooldown";
        }
    }
    if (facts.casts && caster)
        if (auto const seen = facts.casts->find({caster->GetGUID(), info->Id});
            seen != facts.casts->end() && seen->second.count >= 2)
            source += Acore::StringFormat("{}seen every {:.1f} s over {} casts (log only)", source.empty() ? "" : "; ",
                                          getMSTimeDiff(seen->second.firstMs, seen->second.lastMs) / 1000.0f /
                                              float(seen->second.count - 1),
                                          seen->second.count);
    return everySec;
}

// [lead820] the damage of the zone's hits still to come in the fight after the one in front of the member: a cast's
// next casts from its landing on at its cadence, an area's next areas (each its ticks over its duration, the fight's
// end cutting the last); a trap or an area for good (counted whole already) has none
float HitsToCome(Facts const& facts, Unit* caster, SpellInfo const* info, HazardZone const& zone, float burst,
                 float perSec, float leftSec, std::string& why)
{
    if (!caster || zone.leftMs < 0)
    {
        why = !caster ? "(no caster: this hit only)" : "(it lasts the fight: counted whole)";
        return 0.0f;
    }
    float const fightSec = std::max(facts.fightSecLeft, 0.0f);
    std::string source;
    float const everySec = CadenceSec(facts, caster, info, source);
    // a cadence below the hazard scan's step is none the leader can count casts by
    if (everySec * IN_MILLISECONDS < float(Leader::Fight::LEADER_HAZARD_SCAN_MS))
    {
        why = "(no cadence known: this hit only)";
        return 0.0f;
    }
    uint32 hits = 0;
    float damage = 0.0f;
    if (!zone.periodMs)
    {
        hits = uint32(std::max(fightSec - leftSec, 0.0f) / everySec);
        damage = float(hits) * burst;
    }
    else
    {
        float const durationSec = std::max(info->GetMaxDuration(), 0) / 1000.0f;
        float const goneSec = std::max(durationSec - zone.leftMs / 1000.0f, 0.0f);
        for (float startSec = std::max(everySec - goneSec, 0.0f); startSec < fightSec; startSec += everySec)
        {
            ++hits;
            damage += perSec * std::min(durationSec, fightSec - startSec);
        }
    }
    why = Acore::StringFormat("for {} more every {:.1f} s ({}) in the fight's {:.0f} s left", hits, everySec, source,
                              fightSec);
    return damage;
}
}  // namespace

void NoteCasts(CastLog& log, Player* viewer, std::vector<HazardZone> const& zones, uint32 nowMs)
{
    for (HazardZone const& zone : zones)
    {
        if (!zone.spellId || zone.caster.IsEmpty() || zone.leftMs < 0)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(zone.spellId);
        if (!info)
            continue;
        // an area's / a channel's length; a cast bar's own cast time
        int32 fullMs = info->GetMaxDuration();
        int32 leftMs = zone.leftMs;
        if (!zone.source.IsEmpty())
            if (Unit* caster = ObjectAccessor::GetUnit(*viewer, zone.caster))
                if (Spell* spell = caster->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                    spell && spell->GetSpellInfo() && spell->GetSpellInfo()->Id == zone.spellId)
                {
                    fullMs = spell->GetCastTime();
                    leftMs = spell->GetCastTimeRemaining();
                }
        if (fullMs <= 0)
            continue;
        uint32 const startMs = nowMs - uint32(std::clamp(fullMs - leftMs, 0, fullMs));
        CastSeen& seen = log[{zone.caster, zone.spellId}];
        // the same cast (another member's view of it, a cast pushed back): within its own length of the last start
        if (seen.count && int32(startMs - seen.lastMs) <= fullMs)
            continue;
        if (!seen.count)
            seen.firstMs = startMs;
        seen.lastMs = startMs;
        ++seen.count;
    }
}

Verdict JudgeZone(Facts const& facts, Player* member, HazardZone const& zone)
{
    Verdict verdict;
    SpellInfo const* info = zone.spellId ? sSpellMgr->GetSpellInfo(zone.spellId) : nullptr;
    if (!info || !facts.group)
    {
        verdict.why = "leaves: no spell data for " + zone.what;
        return verdict;
    }
    Unit* caster = zone.caster.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*member, zone.caster);
    SpellHit const hit = DamageOf(caster, info, member);
    bool const cast = !zone.source.IsEmpty() && !zone.periodMs;
    float const fightSec = std::max(facts.fightSecLeft, 0.0f);
    // the seconds it still runs: the cast's remaining time (read now), the area's duration within the fight
    float leftSec = zone.leftMs < 0 ? fightSec : std::min(zone.leftMs / 1000.0f, std::max(fightSec, 0.0f));
    if (cast)
    {
        leftSec = std::max(zone.leftMs, 0) / 1000.0f;
        if (caster)
            if (Spell* spell = caster->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                spell && spell->GetSpellInfo() && spell->GetSpellInfo()->Id == zone.spellId)
                leftSec = spell->GetCastTimeRemaining() / 1000.0f;
    }
    // staying: a cast's / trap's one hit (and the damage over time it leaves), an area's ticks while it runs
    float burst = 0.0f;
    float perSec = 0.0f;
    float taken = 0.0f;
    if (zone.periodMs)
    {
        float const perTick = hit.tick > 0.0f ? hit.tick : hit.direct;
        perSec = perTick * 1000.0f / float(zone.periodMs);
        burst = perTick;
        taken = std::max(burst, perSec * leftSec);
    }
    else
    {
        burst = hit.direct + hit.tick * float(hit.ticks);
        taken = burst;
    }
    // leaving: the walk out of the circle; a cast it cannot walk out of before it lands saves nothing
    float const speed = std::max(member->GetSpeed(MOVE_RUN), 0.1f);
    float const outYd =
        std::max(zone.radius + Leader::Fight::LEADER_HAZARD_EDGE -
                     zone.centre.GetExactDist2d(member->GetPositionX(), member->GetPositionY()),
                 0.0f);
    float const outSec = outYd / speed;
    if (cast && outSec > leftSec)
    {
        verdict.stays = true;
        verdict.heal = taken;
        verdict.why = Acore::StringFormat("stays: {} {} lands in {:.1f} s, the walk out takes {:.1f} s ({:.1f} yd)",
                                          Amount(taken), zone.what, leftSec, outSec, outYd);
        return verdict;
    }
    // melee whose target stands in it: out until the cast lands / the area ends, and the walk back
    Unit* target = member->GetVictim() ? member->GetVictim() : facts.fightTarget;
    bool const targetIn =
        target && zone.centre.GetExactDist2d(target->GetPositionX(), target->GetPositionY()) < zone.radius;
    float const awaySec = IsMelee(member) && targetIn ? std::max(outSec, leftSec) + outSec : outSec;
    // [lead820] the hazard's hits still to come in the fight: the healer heals them all
    std::string toComeWhy;
    float const toCome = HitsToCome(facts, caster, info, zone, burst, perSec, leftSec, toComeWhy);
    return Decide(facts, member, target, zone.what, zone.what, burst, perSec, taken, awaySec, toCome, toComeWhy);
}

Verdict JudgeCleave(Facts const& facts, Player* member, Unit* cleaver, float exitYd)
{
    Verdict verdict;
    Creature* creature = cleaver ? cleaver->ToCreature() : nullptr;
    float everySec = 0.0f;
    SpellInfo const* info = creature ? ConeSpell(creature, everySec) : nullptr;
    if (!info || everySec <= 0.0f || !facts.group)
    {
        verdict.why = "leaves: no cone spell data for " + std::string(cleaver ? cleaver->GetName() : "the cleaver");
        return verdict;
    }
    SpellHit const hit = DamageOf(cleaver, info, member);
    float const burst = hit.direct + hit.tick * float(hit.ticks);
    float const perSec = burst / everySec;
    // staying in front for the fight's rest: its casts over those seconds; leaving: the side step
    float const taken = std::max(burst, perSec * std::max(facts.fightSecLeft, 0.0f));
    float const awaySec = exitYd / std::max(member->GetSpeed(MOVE_RUN), 0.1f);
    Unit* target = member->GetVictim() ? member->GetVictim() : facts.fightTarget;
    // its casts over the fight's rest are in `taken` already (lead780): none more to come
    return Decide(facts, member, target, "cleave: " + cleaver->GetName(),
                  std::string(info->SpellName[LOCALE_enUS]) + " of " + cleaver->GetName() + " every " +
                      Acore::StringFormat("{:.1f} s", everySec),
                  burst, perSec, taken, awaySec, 0.0f, "(its casts in the fight's rest counted)");
}

float InstanceDamageFactor(Unit* caster, SpellInfo const* info, bool periodic)
{
    // a probe large enough that the hook's integer rounding does not count, small enough that no factor of the
    // rescale's tables overflows its int32 hit
    constexpr uint32 PROBE_DAMAGE = 1000000;
    if (!caster || !info)
        return 1.0f;
    // the rescale's hooks do not read their target (`Unit* /*target*/`): the caster stands in for it
    return float(InstanceFactorApplied(caster, caster, PROBE_DAMAGE, info, periodic)) / float(PROBE_DAMAGE);
}

float LandedHit(Unit* caster, SpellInfo const* info, Player* victim)
{
    if (!info || !victim)
        return 0.0f;
    return DamageOf(caster, info, victim).direct;
}
}  // namespace Leader::HazardVerdict
