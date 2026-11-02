/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HealthTriggers.h"
#include "AggressiveTargetValue.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "AKTriggers.h"   // [b47_patrol] AKJedogaRoom::OwnsPull
#include "Bag.h"          // [lead177] the drinks in the bags
#include "BuyAction.h"    // [lead177] RESTOCK_SPELL_CATEGORY_DRINK
#include "ANTriggers.h"   // [b47_patrol] ANWatcherRoom::OwnsPull / HoldsRally
#include "CoSTriggers.h"  // [b33_cos] CoSHelpers::IsGuardianTimerRunning (timed run rest lines)
#include "CreatureGroups.h"   // [b46_patrol]
#include "Group.h"
#include "GenericSpellActions.h"
#include "HealAssignmentValues.h"
#include "LeaderOrders.h"  // [lead15] LEADER_ONE_REST_RULE
#include "MgTShared.h"        // [b47_patrol] MagistersTerrace::SelinRoom
#include "MovementActions.h"  // [b46_patrol] MovementAction::IdleAggroReach
#include "ObjectMgr.h"        // [lead264] CreatureBaseStats
#include "PartyMemberToHeal.h"
#include "Playerbots.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "StringFormat.h"

bool HealthInRangeTrigger::IsActive()
{
    return ValueInRangeTrigger::IsActive() && !AI_VALUE2(bool, "dead", GetTargetName());
}

float HealthInRangeTrigger::GetValue() { return AI_VALUE2(uint8, "health", GetTargetName()); }

bool PartyMemberLosingHealthFastTrigger::IsActive()
{
    float const threshold = sPlayerbotAIConfig.HealerFastHealthLossPct;
    if (threshold <= 0.0f)
        return false;

    // The cached loss map first: it rules out the usual tick without recomputing the heal target.
    HealthLossMap const& losses = context->GetValue<HealthLossMap>("group health loss")->RefGet();
    auto const losingFast = [threshold](auto const& entry) { return entry.second >= threshold; };
    if (std::none_of(losses.begin(), losses.end(), losingFast))
        return false;

    Unit* target = GetTarget();
    if (!target || !target->IsAlive() || target->GetHealthPct() >= sPlayerbotAIConfig.AlmostFullHealth)
        return false;

    return RecentHealthLossPct(target, losses) >= threshold;
}

bool PartyMemberDeadTrigger::IsActive() { return GetTarget(); }

bool CombatPartyMemberDeadTrigger::IsActive() { return GetTarget(); }

bool DeadTrigger::IsActive() { return AI_VALUE2(bool, "dead", GetTargetName()); }

bool AoeHealTrigger::IsActive() { return AI_VALUE2(uint8, "aoe heal", type) >= count; }

bool HealerLowManaTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    return target->GetPowerPct(POWER_MANA) < sPlayerbotAIConfig.LowMana;
}

PartyMemberEmergencyHealthTrigger::PartyMemberEmergencyHealthTrigger(PlayerbotAI* botAI)
    : PartyMemberLowHealthTrigger(botAI, "party member emergency health", HealTriage::EMERGENCY_HEALTH_PCT, 0)
{
}

float HealerEmergencyMultiplier::GetValue(Action* action)
{
    if (!action || !bot->IsInCombat() || !bot->GetGroup())
        return 1.0f;

    bool const facing = action->getName() == "set facing";
    CurePartyMemberAction* cure = facing ? nullptr : dynamic_cast<CurePartyMemberAction*>(action);
    CastSpellAction* spellAction = nullptr;
    if (!facing && !cure)
    {
        spellAction = dynamic_cast<CastSpellAction*>(action);
        if (!spellAction || dynamic_cast<CastHealingSpellAction*>(action))
            return 1.0f;
    }

    // Cached this tick by the heal triggers; no recalculation per action.
    Unit* toHeal = context->GetValue<Unit*>("party member to heal")->LazyGet();

    // [healer_audit_b30] Below low mana a cure on another member waits while somebody needs a heal: an-h-5D-36
    // Hadronox 18:53:32-18:54:30, the resto shaman cleansed the reapplied poison 13 times and was at 0-3 % mana
    // when the tank died ("riptide ... not enough power").
    if (cure && toHeal && toHeal->IsAlive() && toHeal->GetHealthPct() < sPlayerbotAIConfig.AlmostFullHealth &&
        cure->GetTarget() != toHeal && bot->getPowerType() == POWER_MANA &&
        bot->GetPowerPct(POWER_MANA) < sPlayerbotAIConfig.LowMana)
        return 0.0f;

    if (!toHeal || !toHeal->IsAlive() || toHeal->GetHealthPct() >= HealTriage::EMERGENCY_HEALTH_PCT)
        return 1.0f;

    if (facing)
        return 0.0f;

    if (cure)
        return cure->GetTarget() == toHeal ? 1.0f : 0.0f;

    // A damage spell on an enemy (crowd control, interrupts without damage and the healer's own buffs stay).
    Unit* target = spellAction->GetTarget();
    if (!target || !bot->IsHostileTo(target))
        return 1.0f;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", spellAction->getSpell());
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!spellInfo || (!spellInfo->HasEffect(SPELL_EFFECT_SCHOOL_DAMAGE) &&
                       !spellInfo->HasAura(SPELL_AURA_PERIODIC_DAMAGE)))
        return 1.0f;

    // Entangling Roots, Hammer of Justice, ...: crowd control that also deals damage stays.
    if (spellInfo->HasAura(SPELL_AURA_MOD_ROOT) || spellInfo->HasAura(SPELL_AURA_MOD_STUN) ||
        spellInfo->HasAura(SPELL_AURA_MOD_CONFUSE) || spellInfo->HasAura(SPELL_AURA_MOD_FEAR))
        return 1.0f;

    return 0.0f;
}

bool AoeInGroupTrigger::IsActive()
{
    int32 member = botAI->GetNearGroupMemberCount();
    // [heal_triage] A party with a member away (or 3-4 players) still has group-wide damage: 3 hurt members are
    // the group heal setting from 3 near members up (was: nothing below 5 near members).
    if (member < 3)
        return false;
    int threshold = member * 0.5;
    if (member <= 5)
        threshold = 3;
    else if (member <= 10)
        threshold = std::min(threshold, 5);
    else if (member <= 25)
        threshold = std::min(threshold, 10);
    else
        threshold = std::min(threshold, 15);

    return AI_VALUE2(uint8, "aoe heal", type) >= threshold;
}

namespace RestState
{
namespace
{
// A food / drink aura by its spell specific: a passive MOD_POWER_REGEN (Blessing of Wisdom, talents) is not drinking.
bool HasRestAura(Player* player, bool wantFood, bool wantDrink)
{
    for (auto const& [spellId, aurApp] : player->GetAppliedAuras())
    {
        switch (aurApp->GetBase()->GetSpellInfo()->GetSpellSpecific())
        {
            case SPELL_SPECIFIC_FOOD:
                if (wantFood)
                    return true;
                break;
            case SPELL_SPECIFIC_DRINK:
                if (wantDrink)
                    return true;
                break;
            case SPELL_SPECIFIC_FOOD_AND_DRINK:
                return true;
            default:
                break;
        }
    }
    return false;
}

// The group members alive on the bot's map within GROUP_RANGE, the bot itself excluded.
template <typename Check>
bool AnyNearMember(Player* bot, Check check)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() ||
            member->GetMap() != bot->GetMap() || bot->GetDistance(member) > GROUP_RANGE)
            continue;
        if (check(member))
            return true;
    }
    return false;
}
}  // namespace

bool IsEating(Player* player) { return HasRestAura(player, true, false); }

// [b46_patrol] See the header.
Creature* PatrolComes(Player* bot, float* gap)
{
    PlayerbotAI* botAI = PATROL_REST_GATE ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI || !bot->GetGroup() || !botAI->IsInNonRaidDungeon())
        return nullptr;

    // [b47_patrol] Not where the boss logic or a room tactic owns the group's spot (mgt-n 20261026-104908-mgt-n-5H-13:
    // Selin's Wretched Skulker walks past the rally the room tactic holds the group at; the tactic times the pulls):
    // an encounter in progress, Selin's / the Krik'thir watcher / Jedoga's room tactic, a live boss within
    // PATROL_REST_BOSS_RANGE (its room).
    constexpr float PATROL_REST_BOSS_RANGE = 60.0f;
    AiObjectContext* context = botAI->GetAiObjectContext();
    if (context->GetValue<bool>("encounter in progress")->Get() ||
        (bot->GetMapId() == MagistersTerrace::MAP_MAGISTERS_TERRACE &&
         (MagistersTerrace::SelinRoom::HoldsGroup(botAI) || MagistersTerrace::SelinRoom::OwnsPull(botAI))) ||
        (bot->GetMapId() == ANWatcherRoom::MAP_AZJOL_NERUB &&
         (ANWatcherRoom::OwnsPull(botAI) || ANWatcherRoom::HoldsRally(botAI))) ||
        (bot->GetMapId() == AKJedogaRoom::MAP_AHNKAHET && AKJedogaRoom::OwnsPull(botAI)))
        return nullptr;

    GuidVector const& nearMobs = context->GetValue<GuidVector>("possible targets no los")->Get();
    for (ObjectGuid const guid : nearMobs)
    {
        Creature* mob = botAI->GetCreature(guid);
        if (mob && mob->IsAlive() && mob->IsDungeonBoss() && bot->GetExactDist(mob) <= PATROL_REST_BOSS_RANGE)
            return nullptr;
    }

    auto const walksPatrol = [](Creature* mob)
    {
        if (mob->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE)
            return true;
        CreatureGroup* formation = mob->GetFormation();
        Creature* leader = formation ? formation->GetLeader() : nullptr;
        return leader && leader != mob && leader->IsAlive() && !leader->IsInCombat() &&
               leader->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE;
    };

    for (ObjectGuid const guid : nearMobs)
    {
        Creature* mob = botAI->GetCreature(guid);
        if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->HasReactState(REACT_AGGRESSIVE) ||
            !bot->IsHostileTo(mob) || mob->IsDungeonBoss() || !walksPatrol(mob))
            continue;

        float const distance = bot->GetExactDist(mob);
        if (distance <= MovementAction::IdleAggroReach(mob, bot, PATROL_REST_MARGIN))
        {
            if (gap)
                *gap = distance;
            return mob;
        }
    }
    return nullptr;
}

bool IsDrinking(Player* player) { return HasRestAura(player, false, true); }

bool GroupFighting(Player* bot)
{
    return bot->IsInCombat() || AnyNearMember(bot, [](Player* member) { return member->IsInCombat(); });
}

bool GroupResting(Player* bot)
{
    return AnyNearMember(bot, [](Player* member) { return HasRestAura(member, true, true); });
}

bool CanRest(Player* bot)
{
    // On dry ground only (a swimmer cannot sit; the harness walks its resting members out of the water).
    return bot->IsAlive() && !bot->IsMounted() && !bot->IsInFlight() && !bot->Unit::IsInWater() &&
           !GroupFighting(bot);
}

bool HealerHasWork(Player* bot)
{
    if (!PlayerbotAI::IsHeal(bot))
        return false;

    return AnyNearMember(bot,
                         [](Player* member)
                         { return member->GetHealthPct() < sPlayerbotAIConfig.MediumHealth && !IsEating(member); });
}

bool PredictNextFight(Player* bot, NextFight& fight)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    ObjectGuid const tank = PlayerbotAI::GetMainTankGuid(group);
    if (tank.IsEmpty())
        return false;

    // the tank's latest planned or refused fight (CarefulPlanVerdict, behind its lock)
    return CarefulPlanVerdict::GetNextFight(tank, fight);
}

namespace
{
// The mana a fight takes from this bot (% of max), or a negative value when the role keeps the fixed lines
// (PREDICT_DRINK_ALL_ROLES off: tank, melee and hunter mana users).
float PredictedManaNeed(Player* bot, NextFight const& fight, float& fightSec)
{
    fightSec = FightSecOf(bot, fight);  // [lead264]
    if (PlayerbotAI::IsHeal(bot))
        return std::max(SpellManaRate(bot) * fightSec, HealerPctPerTankHealth(bot) * fight.cost);
    if (!PlayerbotAI::IsCaster(bot) && !PREDICT_DRINK_ALL_ROLES)
        return -1.0f;
    return SpellManaRate(bot) * fightSec;  // [lead264] the spell data's rate, not a role's measured median
}

// [b33_cos] Rest lines of a timed run (CoSHelpers::IsGuardianTimerRunning): before a real boss the healer tops up to
// this, the others to that; on trash the fixed lines (HEALER_REST_MANA_PCT / REST_MANA_PCT).
constexpr float TIMED_RUN_BOSS_HEALER_MANA_PCT = 75.0f;
constexpr float TIMED_RUN_BOSS_MANA_PCT = 60.0f;

// [b33_cos] A timed run: a sitter stands up at its own line even while others still sit (no top-up to 100 %).
bool TimedRunStandsAtLine(Player* bot) { return CoSHelpers::IsGuardianTimerRunning(bot); }
}  // namespace

namespace
{
    std::mutex manaRateLock;
    std::unordered_map<uint64, float> measuredManaRate;  // [lead51] % of max mana per second of fight

}

void NoteManaRate(ObjectGuid member, float pctPerSec)
{
    std::lock_guard<std::mutex> lock(manaRateLock);
    measuredManaRate[member.GetRawValue()] = pctPerSec;
}

namespace
{
    std::unordered_map<uint64, float> bossManaRate;  // [lead232] under manaRateLock
    std::unordered_map<uint64, float> peakManaRate;  // [lead232] under manaRateLock
}

void NoteBossManaRate(ObjectGuid member, float pctPerSec)
{
    std::lock_guard<std::mutex> lock(manaRateLock);
    bossManaRate[member.GetRawValue()] = pctPerSec;
}

void NotePeakManaRate(ObjectGuid member, float pctPerSec)
{
    std::lock_guard<std::mutex> lock(manaRateLock);
    peakManaRate[member.GetRawValue()] = pctPerSec;
}

namespace
{
    std::unordered_map<uint64, float> healerPctPerCost;  // [lead393] under manaRateLock
}

void NoteHealerPctPerCost(ObjectGuid member, float pctPerCost)
{
    std::lock_guard<std::mutex> lock(manaRateLock);
    healerPctPerCost[member.GetRawValue()] = pctPerCost;
}

namespace
{
    std::unordered_map<uint64, bool> pleaMeasured;  // [lead367] by member, under manaRateLock
}

void NoteDivinePleaInFight(ObjectGuid member)
{
    std::lock_guard<std::mutex> lock(manaRateLock);
    pleaMeasured[member.GetRawValue()] = true;
}

namespace
{
    struct FightLengthError
    {
        float sum = 0.0f;
        uint32 fights = 0;
        float estimatedSq = 0.0f;  // [lead363] the sum of estimate x estimate over these fights
        float crossSum = 0.0f;     // [lead363] the sum of estimate x real length
        uint32 fitted = 0;         // [lead366] the fights in the fit (an estimate to scale)
        float fittedMax = 0.0f;    // [lead366] the longest estimate in the fit
    };
    std::unordered_map<uint64, FightLengthError> fightLengthErrors;  // [lead88] by group, under manaRateLock

    // [lead362] the group's real / estimated fight seconds so far (1 before a fight is measured); under manaRateLock
    // [lead363] b197: real / estimated sums let one near-zero estimate decide it - the first fight estimated 0.02 s
    // and lasted 13.2 s, the scale went to ~770, the next fight's error 4334 s, "x 2173.6 s uncertainty", every member
    // held to 100 %. The least-squares scale (real ~ scale x estimate): a fight without an estimate weighs nothing
    // [lead366] b198 rfc-h 5A: after the first fight alone (estimated 0.9 s, 24.3 s real) the fit was that one fight
    // - scale 27, "x 217 s" for the next 8 s estimate - and its unfitted error "x 23.5 s uncertainty", the tank held
    // to 84-100 % twice. One fight fits the one parameter exactly and leaves nothing to judge it by: the scale needs
    // two fights in the fit, and a fight's overrun counts as the uncertainty only when it was judged by a fitted scale.
    // b198 5B / 5C / 5D: fits of first fights estimated 0.0-0.1 s (29.8 s real) scaled the next 8-15 s estimates to
    // "x 2041 s", "x 4274 s", "x 12448 s" - a slope measured on estimates near zero says nothing of a longer one: the
    // scale holds over the estimates it was measured on (no longer than the fit's longest), beyond them the estimate
    // stays the plan's own
    bool ScaleFitted(FightLengthError const& error, float estimatedSec)
    {
        return error.fitted >= 2 && estimatedSec <= error.fittedMax;
    }
    float EstimateScale(FightLengthError const& error, float estimatedSec)
    {
        return ScaleFitted(error, estimatedSec) && error.estimatedSq > 0.0f && error.crossSum > 0.0f
                   ? error.crossSum / error.estimatedSq
                   : 1.0f;
    }
}

// [lead362] b196 rfc-h 5E: the plan's fight estimates ran 1227 s against 879 s real (47 fights; 5A 984 / 761, 5G
// 702 / 647): "pull together: 7 mobs ... 65 s" fought 1 mob in 8 s. The mana lines took the raw estimate ("x 53 s",
// "x 48 s") and its whole bias again as the reserve ("x 15.1 s uncertainty"), and the group drank for fights that
// never came. The estimate is scaled by what this group's fights measured; the uncertainty is what is left
void NoteFightLengthError(ObjectGuid group, float estimatedSec, float realSec)
{
    std::lock_guard<std::mutex> lock(manaRateLock);
    FightLengthError& error = fightLengthErrors[group.GetRawValue()];
    // [lead363] the reserve covers a fight that runs longer than its scaled estimate - a shorter one needs no mana
    // beyond the estimate's: the mean overrun, never more than the seconds the fights really ran past it
    if (ScaleFitted(error, estimatedSec))  // [lead366] judged by the fit; before it the reserve: one more mob's fight
    {
        error.sum += std::max(0.0f, realSec - estimatedSec * EstimateScale(error, estimatedSec));
        ++error.fights;
    }
    if (estimatedSec > 0.0f)
    {
        ++error.fitted;
        error.fittedMax = std::max(error.fittedMax, estimatedSec);
        error.estimatedSq += estimatedSec * estimatedSec;
        error.crossSum += estimatedSec * realSec;
    }
}

namespace
{
struct MeasuredHealing
{
    uint32 instanceId = 0;
    float healed = 0.0f;
    float mana = 0.0f;
    float bestHps = 0.0f;  // the most a fight of this dungeon measured per second
};
std::unordered_map<uint64, MeasuredHealing> measuredHealing;  // [lead258] by healer, under manaRateLock

bool IsHealSpell(SpellInfo const* info)
{
    for (SpellEffectInfo const& effect : info->GetEffects())
        if (effect.Effect == SPELL_EFFECT_HEAL ||
            (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL))
            return true;
    return false;
}
}  // namespace

void NoteHealing(Player* healer, float healed, float manaSpent, float fightSec)
{
    if (!healer || healed <= 0.0f)
        return;

    std::lock_guard<std::mutex> lock(manaRateLock);
    MeasuredHealing& entry = measuredHealing[healer->GetGUID().GetRawValue()];
    if (entry.instanceId != healer->GetInstanceId())
        entry = MeasuredHealing();  // this dungeon's fights only
    entry.instanceId = healer->GetInstanceId();
    if (manaSpent > 0.0f)
    {
        entry.healed += healed;
        entry.mana += manaSpent;
    }
    if (fightSec > 0.0f)
        entry.bestHps = std::max(entry.bestHps, healed / fightSec);
}

HealerFacts HealerThroughput(Player* healer, Unit* target)
{
    HealerFacts facts;
    if (!healer || !target)
        return facts;

    facts.maxMana = float(healer->GetMaxPower(POWER_MANA));
    // the spell scan is cached per healer while its spells, spell power and mana stay the same (a lookup, no clock)
    struct Cached
    {
        size_t spells = 0;
        int32 power = 0;
        uint32 mana = 0;
        HealerFacts facts;
    };
    thread_local std::unordered_map<uint64, Cached> cache;
    Cached& cached = cache[healer->GetGUID().GetRawValue()];
    int32 const power = healer->SpellBaseHealingBonusDone(SPELL_SCHOOL_MASK_ALL);
    if (cached.spells != healer->GetSpellMap().size() || cached.power != power ||
        cached.mana != healer->GetMaxPower(POWER_MANA))
    {
        HealerFacts game;
        game.maxMana = facts.maxMana;
        for (auto const& [spellId, known] : healer->GetSpellMap())
        {
            if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || info->IsPassive() || !info->IsPositive() || info->PowerType != POWER_MANA ||
                !IsHealSpell(info))
                continue;

            float amount = 0.0f;
            float durationMs = 0.0f;
            for (SpellEffectInfo const& effect : info->GetEffects())
            {
                int32 const base = effect.CalcValue(healer);
                if (base <= 0)
                    continue;
                if (effect.Effect == SPELL_EFFECT_HEAL)
                    amount += float(target->SpellHealingBonusTaken(
                        healer, info,
                        healer->SpellHealingBonusDone(target, info, uint32(base), HEAL, effect.EffectIndex), HEAL));
                else if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL &&
                         effect.Amplitude)
                {
                    durationMs = float(std::max(info->GetMaxDuration(), 0));
                    float const ticks = durationMs / float(effect.Amplitude);
                    amount +=
                        ticks *
                        float(target->SpellHealingBonusTaken(
                            healer, info,
                            healer->SpellHealingBonusDone(target, info, uint32(base), DOT, effect.EffectIndex), DOT));
                }
            }
            if (amount <= 0.0f)
                continue;

            float const castMs = float(info->CalcCastTime(healer));
            float const gapMs = std::max(castMs, float(info->StartRecoveryTime));
            float const everyMs =
                std::max({gapMs, float(info->RecoveryTime), float(info->CategoryRecoveryTime), durationMs, 1.0f});
            float const hps = amount * float(IN_MILLISECONDS) / everyMs;
            int32 const cost = info->CalcPowerCost(healer, info->GetSchoolMask());
            if (cost > 0)
                game.healPerMana = std::max(game.healPerMana, amount / float(cost));
            if (hps > game.hps)
            {
                game.hps = hps;
                game.manaPctPerSec = facts.maxMana > 0.0f && cost > 0
                                         ? 100.0f * float(cost) * float(IN_MILLISECONDS) / everyMs / facts.maxMana
                                         : 0.0f;
                game.gapSec = std::max(gapMs, 1.0f) / float(IN_MILLISECONDS);
                game.spell = info->SpellName[0] ? info->SpellName[0] : "?";
            }
        }
        cached = {healer->GetSpellMap().size(), power, healer->GetMaxPower(POWER_MANA), game};
    }
    facts = cached.facts;

    std::lock_guard<std::mutex> lock(manaRateLock);
    if (auto const found = measuredHealing.find(healer->GetGUID().GetRawValue());
        found != measuredHealing.end() && found->second.instanceId == healer->GetInstanceId())
    {
        if (found->second.mana > 0.0f)
        {
            facts.healPerMana = found->second.healed / found->second.mana;
            facts.measured = true;
        }
        facts.hps = std::max(facts.hps, found->second.bestHps);
    }
    return facts;
}

float HealerPctPerTankHealth(Player* healer)
{
    Group* group = healer ? healer->GetGroup() : nullptr;
    Player* tank = group ? ObjectAccessor::FindPlayer(PlayerbotAI::GetMainTankGuid(group)) : nullptr;
    if (!tank || tank->GetMap() != healer->GetMap())
        tank = healer;  // no tank here: a tank health is read as the healer's own
    HealerFacts const facts = HealerThroughput(healer, tank);
    if (facts.healPerMana <= 0.0f || facts.maxMana <= 0.0f)
        return 0.0f;
    return 100.0f * float(tank->GetMaxHealth()) / (facts.healPerMana * facts.maxMana);
}

float SpellManaRate(Player* bot)
{
    float const maxMana = float(bot->GetMaxPower(POWER_MANA));
    if (maxMana <= 0.0f)
        return 0.0f;
    if (PlayerbotAI::IsHeal(bot))
        return HealerThroughput(bot, bot).manaPctPerSec;

    // the spell scan is cached per bot while its spells and spell power stay the same (a lookup, no clock)
    struct Cached
    {
        size_t spells = 0;
        int32 power = -1;
        uint32 mana = 0;
        float rate = 0.0f;
    };
    thread_local std::unordered_map<uint64, Cached> cache;
    Cached& cached = cache[bot->GetGUID().GetRawValue()];
    int32 const power = bot->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_ALL);
    if (cached.spells == bot->GetSpellMap().size() && cached.power == power &&
        cached.mana == bot->GetMaxPower(POWER_MANA))
        return cached.rate;

    float const weapon = (bot->GetFloatValue(UNIT_FIELD_MINDAMAGE) + bot->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2.0f;
    float bestDps = 0.0f;
    float rate = 0.0f;
    for (auto const& [spellId, known] : bot->GetSpellMap())
    {
        if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || info->IsPassive() || info->IsPositive() || info->PowerType != POWER_MANA)
            continue;
        int32 const cost = info->CalcPowerCost(bot, info->GetSchoolMask());
        if (cost <= 0)
            continue;

        float damage = 0.0f;
        float durationMs = 0.0f;
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            int32 const base = std::max(effect.CalcValue(bot), 0);
            switch (effect.Effect)
            {
                case SPELL_EFFECT_SCHOOL_DAMAGE:
                    damage += float(base);
                    break;
                case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                    damage += weapon * float(base) / 100.0f;
                    break;
                case SPELL_EFFECT_WEAPON_DAMAGE:
                case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                    damage += weapon + float(base);
                    break;
                case SPELL_EFFECT_APPLY_AURA:
                case SPELL_EFFECT_PERSISTENT_AREA_AURA:
                    if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE && effect.Amplitude)
                    {
                        durationMs = float(std::max(info->GetMaxDuration(), 0));
                        damage += float(base) * durationMs / float(effect.Amplitude);
                    }
                    break;
                default:
                    break;
            }
        }
        if (damage <= 0.0f)
            continue;

        float const castMs =
            info->IsChanneled() ? float(std::max(info->GetDuration(), 0)) : float(info->CalcCastTime(bot));
        float const everyMs = std::max({castMs, float(info->StartRecoveryTime), float(info->RecoveryTime),
                                        float(info->CategoryRecoveryTime), durationMs, 1.0f});
        float const dps = damage / everyMs;
        if (dps > bestDps)
        {
            bestDps = dps;
            rate = 100.0f * float(cost) * float(IN_MILLISECONDS) / everyMs / maxMana;
        }
    }
    cached = {bot->GetSpellMap().size(), power, bot->GetMaxPower(POWER_MANA), rate};
    return rate;
}

float CheapestCastPct(Player* bot)
{
    float const maxMana = float(bot->GetMaxPower(POWER_MANA));
    if (maxMana <= 0.0f)
        return 0.0f;

    // the spell scan is cached per bot while its spells and mana pool stay the same (a lookup, no clock)
    struct Cached
    {
        size_t spells = 0;
        uint32 mana = 0;
        float pct = 0.0f;
    };
    thread_local std::unordered_map<uint64, Cached> cache;
    Cached& cached = cache[bot->GetGUID().GetRawValue()];
    if (cached.spells == bot->GetSpellMap().size() && cached.mana == bot->GetMaxPower(POWER_MANA))
        return cached.pct;

    bool const healer = PlayerbotAI::IsHeal(bot);
    int32 cheapest = 0;
    for (auto const& [spellId, known] : bot->GetSpellMap())
    {
        if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || info->IsPassive() || info->PowerType != POWER_MANA ||
            (healer ? !(info->IsPositive() && IsHealSpell(info)) : info->IsPositive()))
            continue;
        int32 const cost = info->CalcPowerCost(bot, info->GetSchoolMask());
        if (cost > 0 && (!cheapest || cost < cheapest))
            cheapest = cost;
    }
    cached = {bot->GetSpellMap().size(), bot->GetMaxPower(POWER_MANA), 100.0f * float(cheapest) / maxMana};
    return cached.pct;
}

namespace
{
struct FightLengths
{
    uint32 instanceId = 0;
    float sec = 0.0f;
    float mobs = 0.0f;
};
std::unordered_map<uint64, FightLengths> fightLengths;  // [lead264] by group, under manaRateLock
}  // namespace

void NoteFightLength(Player* tank, uint32 mobs, float sec)
{
    Group* group = tank ? tank->GetGroup() : nullptr;
    if (!group || !mobs || sec <= 0.0f)
        return;
    std::lock_guard<std::mutex> lock(manaRateLock);
    FightLengths& entry = fightLengths[group->GetGUID().GetRawValue()];
    if (entry.instanceId != tank->GetInstanceId())
        entry = FightLengths();  // this dungeon's fights only
    entry.instanceId = tank->GetInstanceId();
    entry.sec += sec;
    entry.mobs += float(mobs);
}

float FightSecOf(Player* bot, NextFight const& fight)
{
    if (fight.fightSec > 0.0f)
    {
        float scale = 1.0f;  // [lead362] the plan's estimate as this group's fights measured it
        if (Group* group = bot->GetGroup())
        {
            std::lock_guard<std::mutex> lock(manaRateLock);
            if (auto const found = fightLengthErrors.find(group->GetGUID().GetRawValue());
                found != fightLengthErrors.end())
                scale = EstimateScale(found->second, fight.fightSec);
        }
        return fight.fightSec * scale;
    }
    float const mobs = float(std::max<uint32>(fight.mobs, 1));
    if (Group* group = bot->GetGroup())
    {
        std::lock_guard<std::mutex> lock(manaRateLock);
        if (auto const found = fightLengths.find(group->GetGUID().GetRawValue());
            found != fightLengths.end() && found->second.instanceId == bot->GetInstanceId() &&
            found->second.mobs > 0.0f)
            return found->second.sec / found->second.mobs * mobs;
    }
    // the game's: a creature of the bot's level (base health of its expansion's table) per mob over the group's damage
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    float const groupDps = botAI ? botAI->GetAiObjectContext()->GetValue<float>("estimated group dps")->Get() : 0.0f;
    CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(bot->GetLevel(), CLASS_WARRIOR);
    uint8 const expansion = bot->GetLevel() > 70 ? 2 : bot->GetLevel() > 60 ? 1 : 0;
    if (!stats || groupDps <= 0.0f)
        return 0.0f;
    return float(stats->BaseHealth[expansion]) * mobs / groupDps;
}

namespace
{
    // [lead177] the mana per second (% of max mana) the best drink in the bags gives (its SPELL_AURA_MOD_POWER_REGEN
    // amount is mana per 5 s); 0 without one. Cached per bot for a few seconds: the bags do not change in a fight
    float DrinkPctPerSec(Player* bot)
    {
        uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
        if (!maxMana)
            return 0.0f;
        constexpr uint32 DRINK_CACHE_MS = 5 * IN_MILLISECONDS;  // a lookup cache only, no decision clock
        struct Cached
        {
            uint32 ms = 0;
            float pct = 0.0f;
        };
        thread_local std::unordered_map<uint64, Cached> cache;
        uint32 const now = getMSTime();
        Cached& cached = cache[bot->GetGUID().GetRawValue()];
        if (cached.ms && getMSTimeDiff(cached.ms, now) < DRINK_CACHE_MS)
            return cached.pct;
        float best = 0.0f;
        auto const consider = [&](Item* item)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE ||
                proto->Spells[0].SpellCategory != RESTOCK_SPELL_CATEGORY_DRINK ||
                bot->CanUseItem(proto) != EQUIP_ERR_OK)
                return;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(proto->Spells[0].SpellId);
            if (!info)
                return;
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.ApplyAuraName == SPELL_AURA_MOD_POWER_REGEN && effect.MiscValue == POWER_MANA)
                    best = std::max(best, 100.0f * float(effect.CalcValue(bot)) / 5.0f / float(maxMana));
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = bot->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    consider(bag->GetItemByPos(static_cast<uint8>(slot)));
        cached = {now ? now : 1, best};
        return best;
    }

    // [lead177] the group's damage dealers present (the tank too): one of them casting no damage costs the fight
    // 1 / this share of its damage
    uint32 DamageDealers(Player* bot)
    {
        uint32 count = 0;
        if (Group* group = bot->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource();
                    member && member->IsAlive() && member->GetMap() == bot->GetMap() && !PlayerbotAI::IsHeal(member))
                    ++count;
        return std::max<uint32>(count, 1);
    }
}

float DrinkPctPerSecOf(Player* bot) { return DrinkPctPerSec(bot); }

namespace
{
    constexpr uint32 SPELL_MANA_LEECH = 34650;  // the Shadowfiend's hit: mana to its priest (spell_pri_mana_leech)

    float ToolPct(Player* caster, Player* receiver, SpellInfo const* info, uint8 depth, int32 lifeMs)
    {
        float const maxMana = float(receiver->GetMaxPower(POWER_MANA));
        if (!info || maxMana <= 0.0f)
            return 0.0f;
        int32 const durationMs = info->GetMaxDuration() > 0 ? info->GetMaxDuration() : lifeMs;
        float pct = 0.0f;
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (!effect.IsEffect())
                continue;
            float const ticks =
                effect.Amplitude > 0 && durationMs > 0 ? float(durationMs) / float(effect.Amplitude) : 0.0f;
            if (effect.Effect == SPELL_EFFECT_ENERGIZE_PCT && effect.MiscValue == POWER_MANA)
                pct += float(effect.CalcValue(caster));
            else if (effect.Effect == SPELL_EFFECT_ENERGIZE && effect.MiscValue == POWER_MANA)
                pct += float(effect.CalcValue(caster)) / maxMana * 100.0f;
            else if (effect.ApplyAuraName == SPELL_AURA_OBS_MOD_POWER && effect.MiscValue == POWER_MANA)
                pct += float(effect.CalcValue(caster)) * ticks;
            else if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_ENERGIZE && effect.MiscValue == POWER_MANA)
                pct += info->Id == SPELL_INNERVATE
                           ? CalculatePct(float(caster->GetCreatePowers(POWER_MANA)), float(effect.CalcValue(caster))) /
                                 maxMana * 100.0f
                           : float(effect.CalcValue(caster)) * ticks / maxMana * 100.0f;
            else if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL && depth && effect.TriggerSpell)
                pct += ticks * ToolPct(caster, receiver, sSpellMgr->GetSpellInfo(effect.TriggerSpell), depth - 1, 0);
            else if ((effect.Effect == SPELL_EFFECT_SUMMON) && depth && effect.MiscValue > 0)
                if (CreatureTemplate const* summoned = sObjectMgr->GetCreatureTemplate(uint32(effect.MiscValue)))
                    for (uint32 spell : summoned->spells)  // a totem's aura lives as long as the totem
                        if (spell)
                            pct += ToolPct(caster, receiver, sSpellMgr->GetSpellInfo(spell), depth - 1, durationMs);
        }
        return pct;
    }
}

float ManaToolPct(Player* caster, Player* receiver, uint32 spell)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
    if (!info || !caster || !receiver || !receiver->GetMaxPower(POWER_MANA))
        return 0.0f;
    if (spell == SPELL_SHADOWFIEND)
    {
        // its hits over its life (the summon's duration over the fiend's attack time), each its Mana Leech
        SpellInfo const* leech = sSpellMgr->GetSpellInfo(SPELL_MANA_LEECH);
        float perHit = leech ? ToolPct(caster, receiver, leech, 0, 0) : 0.0f;
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == SPELL_EFFECT_SUMMON && effect.MiscValue > 0)
                if (CreatureTemplate const* fiend = sObjectMgr->GetCreatureTemplate(uint32(effect.MiscValue));
                    fiend && fiend->BaseAttackTime && info->GetMaxDuration() > 0)
                    return perHit * float(info->GetMaxDuration()) / float(fiend->BaseAttackTime);
        return 0.0f;
    }
    return ToolPct(caster, receiver, info, 2, 0);
}

float FightManaLine(Player* bot, float fightSec, float cost, std::string* why, bool boss, bool credits)
{
    float rate = SpellManaRate(bot);  // [lead264] nothing measured: the bot's spell data
    bool measured = false;
    char const* source = "measured";
    {
        std::lock_guard<std::mutex> lock(manaRateLock);
        uint64 const key = bot->GetGUID().GetRawValue();
        if (auto const found = measuredManaRate.find(key); found != measuredManaRate.end())
        {
            rate = found->second;
            measured = true;
        }
        // [lead232] b187 rfc-h: the arcane mage's boss gate "mana 26 % < 26 % = 0.23 %/s (measured) x 105 s" - the
        // trash rate (few casts per pull) while on a boss it runs its whole rotation (Arcane Blast 14/min vs the sim's
        // 27). A boss fight reads the rate measured on boss fights, else its highest full-length fight's rate
        if (boss)
        {
            if (auto const found = bossManaRate.find(key); found != bossManaRate.end())
            {
                rate = found->second;
                measured = true;
                source = "measured on bosses";
            }
            else if (auto const peak = peakManaRate.find(key); peak != peakManaRate.end() && peak->second > rate)
            {
                rate = peak->second;
                measured = true;
                source = "its full fights";
            }
        }
    }
    float need = rate * fightSec;
    // [lead393] b201 rfc-h 5C: the disc priest drank to 100 % three times (768.9 s 62 -> 100 % in 19 s, 779-819 s
    // "needs 100 % = 0.13 %/s (measured) x 88 s + 4 reserve; heals 51.3 % per tank health x 2.15 cost") - its
    // measured use was 11 % for such a fight, the heal term 110 %: the throughput model's mana for every tank health
    // the plan costs, while the blood DK healed itself and the absorbs took the rest. Once a healer's net use per
    // costed tank health is measured in this run's fights, the term reads it (the same unit, the plan's cost)
    float perTankHealth = PlayerbotAI::IsHeal(bot) ? HealerPctPerTankHealth(bot) : 0.0f;  // [lead258]
    bool perCostMeasured = false;
    if (PlayerbotAI::IsHeal(bot))
    {
        std::lock_guard<std::mutex> lock(manaRateLock);
        if (auto const found = healerPctPerCost.find(bot->GetGUID().GetRawValue()); found != healerPctPerCost.end())
        {
            perTankHealth = found->second;
            perCostMeasured = true;
        }
    }
    if (PlayerbotAI::IsHeal(bot))
        need = std::max(need, perTankHealth * cost);
    // [lead88] user: no flat reserve - the reserve is the spend over the measured fight-length uncertainty (the mean
    // |real - estimated| seconds of this run's fights); a healer adds its share of the predicted tank damage over
    // those seconds. [lead264] While no fight is measured: the spend of one more mob's fight (FightSecOf one mob)
    NextFight oneMob;
    oneMob.mobs = 1;
    float const oneMobSec = FightSecOf(bot, oneMob);
    float reserve = rate * oneMobSec;
    if (PlayerbotAI::IsHeal(bot) && fightSec > 0.0f)
        reserve += perTankHealth * cost * oneMobSec / fightSec;
    float uncertainSec = -1.0f;
    if (Group* group = bot->GetGroup())
    {
        std::lock_guard<std::mutex> lock(manaRateLock);
        if (auto const found = fightLengthErrors.find(group->GetGUID().GetRawValue());
            found != fightLengthErrors.end() && found->second.fights)
            uncertainSec = found->second.sum / float(found->second.fights);
    }
    if (uncertainSec >= 0.0f && measured)
    {
        reserve = rate * uncertainSec;
        if (PlayerbotAI::IsHeal(bot) && fightSec > 0.0f)
            reserve += perTankHealth * cost * uncertainSec / fightSec;
    }
    // the mana its own ready cooldowns return in the fight ([lead423] spell data, ManaToolPct: Evocation, Divine
    // Plea, a priest's Shadowfiend)
    // [lead177] a cooldown's mana counts only when using it in the fight costs less time than drinking that mana before
    // the pull: its cast (a channel's duration, else the global cooldown) takes this member's share of the group's
    // damage out of the fight (cast x 1 / damage dealers, a healer's the whole cast), the drink costs the group the
    // mana over the best drink's rate (no drink in the bags: the cooldown it is)
    float returned = 0.0f;
    std::string pick;
    float const drinkRate = DrinkPctPerSec(bot);
    auto const credit = [&](uint32 spell, float pct, char const* name)
    {
        if (!credits || !bot->HasSpell(spell) || bot->HasSpellCooldown(spell))
            return;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        if (!info)
            return;
        float const castSec = info->IsChanneled() ? std::max(info->GetDuration(), 0) / 1000.0f
                                                  : std::max<uint32>(info->StartRecoveryTime, info->CalcCastTime(bot)) /
                                                        1000.0f;
        float const fightCost = PlayerbotAI::IsHeal(bot) ? castSec : castSec / float(DamageDealers(bot));
        float const drinkSec = drinkRate > 0.0f ? pct / drinkRate : -1.0f;
        bool const inFight = drinkSec < 0.0f || fightCost <= drinkSec;
        if (inFight)
            returned += pct;
        pick += Acore::StringFormat("{}{} {}: {:.1f} s in the fight vs {:.1f} s drinking", pick.empty() ? "" : "; ",
                                    name, inFight ? "in the fight" : "drunk before", fightCost, drinkSec);
    };
    credit(SPELL_EVOCATION, ManaToolPct(bot, bot, SPELL_EVOCATION), "Evocation");
    credit(SPELL_SHADOWFIEND, ManaToolPct(bot, bot, SPELL_SHADOWFIEND), "Shadowfiend");  // [lead423]
    // [lead224] a measured rate is the net use of fights the paladin keeps Divine Plea up in (rot48, Guarded by the
    // Light): its mana is in that rate already, a credit on top counted it twice
    // [lead367] b198 rfc-h 5A: the tank's rate came from its opening fight alone, no Divine Plea up yet (first cast
    // in fight 2), and the credit was skipped as soon as any rate was measured. The credit is skipped only when the
    // measured fights had Plea up
    bool pleaInRate = false;
    if (measured)
    {
        std::lock_guard<std::mutex> lock(manaRateLock);
        auto const found = pleaMeasured.find(bot->GetGUID().GetRawValue());
        pleaInRate = found != pleaMeasured.end() && found->second;
    }
    if (!pleaInRate)
        credit(SPELL_DIVINE_PLEA, ManaToolPct(bot, bot, SPELL_DIVINE_PLEA), "Divine Plea");
    // [lead361] b196 rfc-h 5E: the BM hunter sat to drink before trash ("Rthunab rests below its line (mana 23 %,
    // need 46 %)") - a WotLK hunter regenerates in the fight with Aspect of the Viper (wowsims: Viper below 10 %,
    // Dragonhawk again above 30 %). Viper's mana counts in the fight when a Viper second costs the group less than
    // drinking its mana before the pull, as a cooldown's does above: its damage cut (the aura's
    // SPELL_AURA_MOD_DAMAGE_PERCENT_DONE) of this member's share of the group's damage, per % of mana it returns -
    // the aura's own power tick (SPELL_AURA_OBS_MOD_POWER) plus the hit energize spell_hun_ascpect_of_the_viper casts:
    // the ranged attack time in seconds as % of max mana per ranged hit, 1 % per second of Auto Shot
    constexpr uint32 SPELL_ASPECT_OF_THE_VIPER = 34074;
    if (bot->HasSpell(SPELL_ASPECT_OF_THE_VIPER) && fightSec > 0.0f)
        if (SpellInfo const* viper = sSpellMgr->GetSpellInfo(SPELL_ASPECT_OF_THE_VIPER))
        {
            float viperPerSec = 1.0f;  // the hit energize over Auto Shot
            float damageCut = 0.0f;
            for (SpellEffectInfo const& effect : viper->GetEffects())
            {
                if (effect.ApplyAuraName == SPELL_AURA_OBS_MOD_POWER && effect.MiscValue == POWER_MANA &&
                    effect.Amplitude > 0)
                    viperPerSec += float(effect.CalcValue(bot)) * float(IN_MILLISECONDS) / float(effect.Amplitude);
                else if (effect.ApplyAuraName == SPELL_AURA_MOD_DAMAGE_PERCENT_DONE && effect.CalcValue(bot) < 0)
                    damageCut = std::max(damageCut, -float(effect.CalcValue(bot)) / 100.0f);
            }
            float const viperCostPerPct = damageCut / float(DamageDealers(bot)) / viperPerSec;
            float const drinkCostPerPct = drinkRate > 0.0f ? 1.0f / drinkRate : -1.0f;
            bool const inFight = damageCut > 0.0f && (drinkCostPerPct < 0.0f || viperCostPerPct <= drinkCostPerPct);
            float const viperPct = std::min(viperPerSec * fightSec, std::max(need + reserve - returned, 0.0f));
            if (inFight)
                returned += viperPct;
            pick += Acore::StringFormat("{}Aspect of the Viper {}: {:.2f} s per % in the fight vs {:.2f} s drinking",
                                        pick.empty() ? "" : "; ", inFight ? "in the fight" : "drunk before",
                                        viperCostPerPct, drinkCostPerPct);
        }
    // [lead251] b191 rfc-h 5E: the prot paladin tank rested 15 times "mana 6-14 % < need 15 %" with its computed
    // need 2-12 % - the config's LowMana as the floor (80 s of its drinks); b192's floor at its mean deepest drop in a
    // fight (lead248) went over the computed need in 104 of 152 lines (5E tank 24 % against 4-13 %, the frost mage 47 %
    // against -34 % with Evocation), drinking 5C 75 -> 160 s, 5B 3 -> 87 s. A measured rate is the fight's own need:
    // no floor over it; the low line guards only the role's estimate (nothing measured yet)
    float const floorPct = measured ? 0.0f : float(sPlayerbotAIConfig.LowMana);
    float const line = std::clamp(need + reserve - returned, floorPct, 100.0f);
    if (why)
        *why = Acore::StringFormat("{:.0f} % = {:.2f} %/s ({}) x {:.0f} s + {:.0f} reserve ({}) - {:.0f} cooldowns{}",
                                   line, rate, measured ? source : "spell data", fightSec, reserve,
                                   uncertainSec >= 0.0f && measured
                                       ? Acore::StringFormat("x {:.1f} s uncertainty", uncertainSec)
                                       : Acore::StringFormat("one more mob, {:.1f} s", oneMobSec),
                                   returned, pick.empty() ? std::string() : " (" + pick + ")");
    if (why && perTankHealth > 0.0f)  // [lead258]
        *why += Acore::StringFormat("; heals {:.1f} % per tank health ({}) x {:.2f} cost", perTankHealth,
                                    perCostMeasured ? "measured" : "throughput", cost);
    return line;
}

namespace
{
    thread_local bool ignoreLeaderRest = false;  // [lead59] BelowOwnLine reads the bot's own lines
}

float TankManaLine(Player* tank, NextFight const& fight, std::string* why)
{
    float const fightSec = FightSecOf(tank, fight);  // [lead264]
    std::string terms;
    float line = FightManaLine(tank, fightSec, fight.cost, &terms);
    float tauntPct = 0.0f;
    uint32 const maxMana = tank->GetMaxPower(POWER_MANA);
    // Hand of Reckoning, Righteous Defense (the paladin's taunts); the others' taunts cost no mana
    for (uint32 spell : {62124u, 31789u})
        if (tank->HasSpell(spell) && maxMana)
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell))
            {
                float const pct = 100.0f * float(info->CalcPowerCost(tank, info->GetSchoolMask())) / float(maxMana);
                tauntPct = tauntPct > 0.0f ? std::min(tauntPct, pct) : pct;
            }
    float const pickUps = fight.mobs > 1 ? float(fight.mobs - 1) : 0.0f;
    line = std::min(100.0f, line + pickUps * tauntPct);
    if (why)
        *why = Acore::StringFormat("{} + {:.0f} pick-ups x {:.1f} %", terms, pickUps, tauntPct);
    return line;
}

// [lead47] the leader's Rest command is on this led bot: it rests now, by the leader's line (no line of its own)
bool LeaderRests(Player* bot)
{
    Leader::MemberEvents events;
    return Leader::LedBot(bot) && Leader::Events::Get(bot->GetGUID(), events) &&
           events.ready == Leader::ReadyEvent::Rest;
}

bool WantsDrink(Player* bot, std::string* why, std::string* skip)
{
    // A warrior, rogue, DK (or a druid's rage / energy on top of its mana pool) has no mana to drink for.
    uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
    if (!maxMana)
        return false;

    // [lead47] b96 rfc-h 332-372 s: the leader's Rest ("the boss pull waits: mana 78 % < 90 %") never reached the
    // drink - "rest mana" fires only below the member's own line (70 % before a mini-boss). Under the leader's Rest
    // the member drinks now; the leader's command ends when its line is reached
    if (!ignoreLeaderRest && LeaderRests(bot) && bot->GetPowerPct(POWER_MANA) < 100.0f)
    {
        if (why)
            *why = Acore::StringFormat("the leader's Rest (mana {:.0f} %)", bot->GetPowerPct(POWER_MANA));
        return true;
    }

    float const mana = bot->GetPowerPct(POWER_MANA);
    bool const healer = PlayerbotAI::IsHeal(bot);
    float const fixedLine = healer ? HEALER_REST_MANA_PCT : REST_MANA_PCT;
    float line = fixedLine;
    std::string fightNote;
    // [lead194] b181 rfc-h 5E: the holy priest's Rests "mana 33 % < 60 %" - no next fight known, the fixed line. In
    // a led group the line is the next fight's only (below); with none known the leader's planner judges it when it
    // looks (its healer gate holds and Rests at the computed line): here only the low line
    if (Leader::LEADER_ONE_REST_RULE && Leader::LedGroup(bot))
    {
        line = float(sPlayerbotAIConfig.LowMana);
        fightNote = Acore::StringFormat("the low line {:.0f} % (no next fight known)", line);
    }

    // The next fight decides the line when it is known: what it costs this bot plus a reserve, never below the low
    // line (DrinkAction drinks there anyway), and a full top-up before a boss.
    NextFight fight;
    if (PREDICT_DRINK && PredictNextFight(bot, fight) && (fight.boss || fight.mobs))
    {
        float fightSec = 0.0f;
        // [rest_topup_real_boss] the full top-up before a real boss only; a mini-boss is a trash fight with a floor
        bool const realBoss = fight.boss && fight.realBoss;
        // [lead51] a led group: the line is computed for the next fight (boss or trash alike, FightManaLine) - no
        // fixed boss top-up, no mini-boss floor, no trash cap
        if (Leader::LEADER_ONE_REST_RULE && Leader::LedGroup(bot))
        {
            fightSec = FightSecOf(bot, fight);  // [lead264]
            std::string terms;
            // [lead74] the tank: its line with the pick-ups (the same as the planner's tank hold)
            line = PlayerbotAI::IsTank(bot) ? TankManaLine(bot, fight, &terms)
                                            : FightManaLine(bot, fightSec, fight.cost, &terms, fight.boss);
            fightNote = Acore::StringFormat("need {} for the next fight ({}{} mobs)", terms,
                                            fight.boss ? "a boss, " : "", fight.mobs);
        }
        else
        {
        float const need = realBoss ? -1.0f : PredictedManaNeed(bot, fight, fightSec);
        if (realBoss)
        {
            line = BOSS_TOPUP_PCT;
            fightNote = Acore::StringFormat("top-up {:.0f} % before a boss ({} mobs)", line, fight.mobs);
        }
        else if (need >= 0.0f)
        {
            // [lead264] the reserve: one more mob's fight at the bot's rate; no cap over the need (75 % was one)
            NextFight oneMob;
            oneMob.mobs = 1;
            float const reserve = need / std::max(fightSec, 1.0f) * FightSecOf(bot, oneMob);
            line = std::clamp(need + reserve, float(sPlayerbotAIConfig.LowMana), 100.0f);
            fightNote = Acore::StringFormat("need {:.0f} % for next fight ({} mobs, ~{:.0f} s)", line, fight.mobs,
                                            fightSec);
        }

        if (fight.boss && !realBoss && line < MINI_BOSS_TOPUP_PCT)
        {
            line = MINI_BOSS_TOPUP_PCT;
            fightNote = Acore::StringFormat("[rest_topup_real_boss] {:.0f} % before a mini-boss ({} mobs, no full "
                                            "top-up)", line, fight.mobs);
        }
        }

        // [lead149] b170 rfc-h 20261101-142639 63.2-70.5 s: "mana 48 % < top-up 82 %: the tank waits for the group
        // topped up" - the fixed line of the old planner's 80 % wait. A led group's readiness is the leader's verdict
        // on the computed line (FightManaLine above, PullReadiness::LeaderVerdict): no fixed top-up there
        if (healer && fight.topUp && line < REFUSED_TOPUP_HEALER_PCT &&
            !(Leader::LEADER_ONE_REST_RULE && Leader::LedGroup(bot)))
        {
            line = REFUSED_TOPUP_HEALER_PCT;
            fightNote = Acore::StringFormat("top-up {:.0f} %: the tank waits for the group topped up", line);
        }
    }

    // [rest_prot_paladin_line] the protection paladin's own trash line (its mana comes back in the fight)
    if (!(fight.boss && fight.realBoss) && !healer && bot->getClass() == CLASS_PALADIN && PlayerbotAI::IsTank(bot) &&
        line > PROT_PALADIN_REST_MANA_PCT)
    {
        line = std::max(PROT_PALADIN_REST_MANA_PCT, float(sPlayerbotAIConfig.LowMana));
        fightNote = Acore::StringFormat("protection paladin line {:.0f} % (mana back in the fight)", line);
    }

    // [b33_cos] A timed run (heroic Culling, Guardian of Time running): no predicted 75 % line and no 95 % top-up, the
    // fixed line on trash and a short top-up before a real boss. The tank's refused-pack top-up stays (the planner
    // waits for it). cos-h 20261026-000434-cos-h-5H-22: the bots sat at 52-75 % ("need 75 % for next fight") and drank
    // to 100 % while the group walked on; 25 "sits above its line", ~175 s of far waits in the 25 minutes (mostly for
    // those sitters), 6 pulls without the healer, one wipe.
    if (!fight.topUp && CoSHelpers::IsGuardianTimerRunning(bot))
    {
        float const cap = (fight.boss && fight.realBoss) ? (healer ? TIMED_RUN_BOSS_HEALER_MANA_PCT
                                                                   : TIMED_RUN_BOSS_MANA_PCT)
                                                         : fixedLine;
        if (line > cap)
        {
            line = std::max(cap, float(sPlayerbotAIConfig.LowMana));
            fightNote = Acore::StringFormat("timed run line {:.0f} % (Guardian of Time running)", line);
        }
    }

    // [lead15] LEADER_ONE_REST_RULE: no sit-down for a deficit under LEADER_REST_MIN_DEFICIT_PCT to the line (one
    // drink = 15-25 s of the group waiting); the boss line is a gate and takes any deficit; a sitter drinks on
    if (mana < line && Leader::LEADER_ONE_REST_RULE && !(fight.boss && fight.realBoss) && !IsDrinking(bot) &&
        line - mana < Leader::LEADER_REST_MIN_DEFICIT_PCT && mana >= sPlayerbotAIConfig.LowMana &&
        Leader::LedGroup(bot))
    {
        if (skip)
            *skip = Acore::StringFormat("mana {:.0f} %: {:.0f} % under its line {:.0f} % (min deficit {:.0f} %)", mana,
                                        line - mana, line, Leader::LEADER_REST_MIN_DEFICIT_PCT);
        return false;
    }

    if (mana < line)
    {
        // Above the low line a healer heals first and drinks after.
        if (healer && mana >= sPlayerbotAIConfig.LowMana && HealerHasWork(bot))
            return false;
        if (why)
            *why = fightNote.empty() ? Acore::StringFormat("mana {:.0f} % < {:.0f} %", mana, line)
                                     : Acore::StringFormat("mana {:.0f} % < {}", mana, fightNote);
        return true;
    }

    // The fixed line would have it drink, the next fight does not need it.
    if (skip && mana < fixedLine && !fightNote.empty())
        *skip = Acore::StringFormat("mana {:.0f} % >= {}", mana, fightNote);

    // The group sits anyway: a bot below the company line drinks with it ("for company"); [rest_no_company] off.
    if (REST_JOIN_COMPANY && mana < COMPANY_PCT && GroupResting(bot))
    {
        if (why)
            *why = Acore::StringFormat("joins the group's rest, mana {:.0f} % < {:.0f} %", mana, COMPANY_PCT);
        return true;
    }
    return false;
}

float OwnManaLine(Player* bot, std::string* why)
{
    NextFight fight;
    if (Leader::LEADER_ONE_REST_RULE && Leader::LedGroup(bot) && PredictNextFight(bot, fight) &&
        (fight.boss || fight.mobs))
    {
        float const fightSec = FightSecOf(bot, fight);  // [lead264]
        return PlayerbotAI::IsTank(bot) ? TankManaLine(bot, fight, why)
                                        : FightManaLine(bot, fightSec, fight.cost, why, fight.boss);
    }
    float const line = PlayerbotAI::IsHeal(bot) ? HEALER_REST_MANA_PCT : REST_MANA_PCT;
    if (why)
        *why = Acore::StringFormat("{:.0f} % (no next fight known)", line);
    return line;
}

bool BelowOwnLine(Player* bot, std::string* why)
{
    ignoreLeaderRest = true;
    bool const below = WantsDrink(bot, why) || WantsFood(bot, why);
    ignoreLeaderRest = false;
    return below;
}

float HealthLine(Player* bot)
{
    NextFight fight;
    if (Leader::LEADER_ONE_REST_RULE && Leader::LedGroup(bot) && PlayerbotAI::IsTank(bot) &&
        PredictNextFight(bot, fight) && fight.cost > 0.0f)
        return std::min(100.0f, fight.cost * 100.0f);
    return REST_HEALTH_PCT;
}

bool WantsFood(Player* bot, std::string* why)
{
    float const health = bot->GetHealthPct();
    // [lead116] b151 rfc-h 16-56 s: the tank ate at 95 % (its next pull cost 0.5 tank hp), stood up, ate again - a
    // led group rests health to its computed line, never to full above it, and not with a healer free to heal it
    bool const ledLine = Leader::LEADER_ONE_REST_RULE && Leader::LedGroup(bot);
    float const line = HealthLine(bot);
    // A healer close by with mana heals it up faster, and for free. The bot itself counts when it heals.
    Player* healer = nullptr;
    float healerMana = 0.0f;
    auto const healerFree = [&]()
    {
        auto const consider = [&](Player* member)
        {
            if (!PlayerbotAI::IsHeal(member) || !member->GetMaxPower(POWER_MANA))
                return;
            float const pct = member->GetPowerPct(POWER_MANA);
            if (!healer || pct > healerMana)
            {
                healer = member;
                healerMana = pct;
            }
        };
        consider(bot);
        AnyNearMember(bot,
                      [&](Player* member)
                      {
                          consider(member);
                          return false;
                      });
        return healer && healerMana >= HEALER_BUSY_MANA_PCT;
    };
    if (!ignoreLeaderRest && LeaderRests(bot) && health < (ledLine ? line : 100.0f) &&
        !(ledLine && healerFree()))  // [lead47] the leader's Rest: it eats now
    {
        if (why)
            *why = Acore::StringFormat("the leader's Rest (health {:.0f} %)", health);
        return true;
    }
    if (health < line)
    {
        if (healerFree())
            return false;
        if (why)
            *why = healer ? Acore::StringFormat("hp {:.0f} % < {:.0f} %, healer mana {:.0f} %", health, line,
                                                healerMana)
                          : Acore::StringFormat("hp {:.0f} % < {:.0f} %, no healer close by", health, line);
        return true;
    }

    if (REST_JOIN_COMPANY && !ledLine && health < COMPANY_PCT && GroupResting(bot))
    {
        if (why)
            *why = Acore::StringFormat("joins the group's rest, hp {:.0f} % < {:.0f} %", health, COMPANY_PCT);
        return true;
    }
    return false;
}

bool RestDone(Player* bot)
{
    // Cheap first: a bot standing has no food / drink aura (it breaks on standing up).
    if (!bot->IsSitState())
        return false;

    bool const eating = IsEating(bot);
    bool const drinking = IsDrinking(bot);
    if (!eating && !drinking)
        return false;
    if (bot->IsInCombat())
        return true;
    if (PatrolComes(bot))
        return true;  // [b46_patrol] up before the patrol walks in

    // Food and water at once (or a combined item): up only when both are full - or ([rest_stand_at_line]) both are
    // above the bot's own lines while nobody close by sits any more (the group is ready, nobody tops up to 100 %).
    // [lead116] a led group's sitter is done with its health at its computed line (no eat to full above it)
    bool const healthDone = !eating || bot->GetHealthPct() >= 100.0f ||
                            (Leader::LEADER_ONE_REST_RULE && Leader::LedGroup(bot) &&
                             bot->GetHealthPct() >= HealthLine(bot));
    bool manaDone = !drinking || !bot->GetMaxPower(POWER_MANA) || bot->GetPowerPct(POWER_MANA) >= 100.0f;
    // [lead221] b185 rfc-h 5H 753-776 s: the planner's answer named 15 % for the disc priest, the leader's Rest ended
    // there and the priest drank on to 100 % (22 s) - a led sitter drank to full whatever the leader's line. It drinks
    // while the leader's Rest holds or its own computed line is above its mana, and stands when neither does
    if (!manaDone && Leader::LEADER_ONE_REST_RULE && Leader::LedBot(bot) && !TimedRunStandsAtLine(bot) &&
        !LeaderRests(bot))
    {
        ignoreLeaderRest = true;
        manaDone = !WantsDrink(bot);
        ignoreLeaderRest = false;
    }
    if (healthDone && manaDone)
        return true;

    // [b32_dmw] Only a sitter still below its own line keeps the others seated: that is the sitter the harness waits
    // for (TestHarnessMgr [regen_wait_below_line]). Two sitters both above their lines kept each other down (dmw-n
    // 20261025-221216-5E-12: the hunter 19x "sits above its line, not waited for" at 75 % - its line is capped at
    // PREDICT_MAX_LINE_PCT - and drank on to 78-99 % next to the priest's top-up, the group walked on, 42 "far"
    // waits for the hunter, 54 for the priest).
    bool const othersSit =
        REST_STAND_BELOW_LINE_COMPANY
            ? AnyNearMember(bot,
                            [](Player* member)
                            {
                                return (IsDrinking(member) && WantsDrink(member)) ||
                                       (IsEating(member) && WantsFood(member));
                            })
            : GroupResting(bot);
    if (!REST_STAND_AT_LINE || (othersSit && !TimedRunStandsAtLine(bot)))  // [b33_cos]
        return false;
    // [lead15] LEADER_ONE_REST_RULE: the leader's Rest is eat / drink to full (the group waits for every sitter)
    if (Leader::LEADER_ONE_REST_RULE && Leader::LedGroup(bot) && !TimedRunStandsAtLine(bot))
        return false;

    bool const healthAtLine = healthDone || !WantsFood(bot);
    bool const manaAtLine = manaDone || !WantsDrink(bot);
    if (healthAtLine && manaAtLine)
    {
        LOG_INFO("playerbots", "[rest_stand_at_line] {}: stands up at hp {:.0f} %, mana {:.0f} % (above its lines, "
                 "nobody else sits)", bot->GetName(), bot->GetHealthPct(),
                 bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 100.0f);
        return true;
    }
    return false;
}
}  // namespace RestState

namespace
{
    // [lead150] one own_decision line per member and below-line stretch, cleared when the member stands at its line
    // again or the leader rests it. Noted on the trigger's second look in a row (it asks every two seconds): the
    // leader's own tick between them had the chance to rest it - the first look alone is no disagreement
    std::mutex ownRestLock;
    std::unordered_map<uint64, uint32> ownRestLooks;

    uint64 OwnRestKey(Player* bot, char const* trigger)
    {
        return bot->GetGUID().GetRawValue() * 2 + (std::strcmp(trigger, "rest health") == 0 ? 1 : 0);
    }

    // the trigger's verdict: `own` - it would fire without the leader's Rest (noted once per stretch)
    void OwnRestVerdict(Player* bot, char const* trigger, bool own)
    {
        {
            std::lock_guard<std::mutex> guard(ownRestLock);
            if (!own)
            {
                ownRestLooks.erase(OwnRestKey(bot, trigger));
                return;
            }
            if (++ownRestLooks[OwnRestKey(bot, trigger)] != 2)
                return;
        }
        std::string const why = Acore::StringFormat("its own rest line (mana {:.0f} %, health {:.0f} %) without the "
                                                    "leader's Rest",
                                                    bot->GetPowerPct(POWER_MANA), bot->GetHealthPct());
        Leader::Events::NoteOwn(bot->GetGUID(), Leader::OwnDecision{trigger, "rest", why, 0});
    }
}

bool RestManaTrigger::IsActive()
{
    if (RestState::IsDrinking(bot) || !RestState::CanRest(bot))
    {
        // A fight (or a sit-down) since the last skip: the next one is logged again.
        if (bot->IsInCombat() || RestState::IsDrinking(bot))
            _skipLogged.clear();
        return false;
    }

    // [lead150] user: rest is the leader's decision. A led executor member's own rest trigger is off; one that would
    // fire without the leader's Rest (below its own line, able to rest) is an own decision (the run's detector)
    if (Leader::ExecutorMember(bot))
    {
        bool const leaderRest = RestState::LeaderRests(bot);
        bool const wants = RestState::WantsDrink(bot);
        OwnRestVerdict(bot, "rest mana", !leaderRest && wants);
        return leaderRest && wants;
    }

    std::string skip;
    if (RestState::WantsDrink(bot, nullptr, &skip))
        return true;

    // Once per next fight, not per mana percent regenerated: the trigger asks every two seconds.
    std::string::size_type const fightAt = skip.find(">=");
    std::string const key = fightAt == std::string::npos ? skip : skip.substr(fightAt);
    if (!skip.empty() && key != _skipLogged)
    {
        LOG_INFO("playerbots", "[rest] {} skips: {}", bot->GetName(), skip);
        _skipLogged = key;
    }
    return false;
}

bool RestHealthTrigger::IsActive()
{
    if (RestState::IsEating(bot) || !RestState::CanRest(bot))
        return false;
    if (Leader::ExecutorMember(bot))  // [lead150] as "rest mana"
    {
        bool const leaderRest = RestState::LeaderRests(bot);
        bool const wants = RestState::WantsFood(bot);
        OwnRestVerdict(bot, "rest health", !leaderRest && wants);
        return leaderRest && wants;
    }
    return RestState::WantsFood(bot);
}

bool RestedTrigger::IsActive() { return RestState::RestDone(bot); }
