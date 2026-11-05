/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HealthTriggers.h"
#include "AiFactory.h"  // [lead1327] GetPlayerSpecTab
#include "AggressiveTargetValue.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>  // [lead850]
#include <tuple>  // [lead1182]
#include <unordered_map>

#include "AKTriggers.h"   // [b47_patrol] AKJedogaRoom::OwnsPull
#include "Bag.h"          // [lead177] the drinks in the bags
#include "DBCStores.h"    // [lead1038] GetTalentSpellPos
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
#include "RaidConsumables.h"  // [lead482] the mana potion it can drink in the fight
#include "EstimatedLifetimeValue.h"  // [lead850] GroupDamage: a channel's damage share
#include "Spell.h"                   // [lead850] the Evocation channel in progress
#include "Playerbots.h"
#include "SpellAuraEffects.h"  // [lead855] AuraEffect: a free-cast proc talent
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"  // [lead855] SpellProcEntry, SpellBonusEntry
#include "StringFormat.h"
#include "World.h"  // [lead855] RATE_POWER_MANA

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
    float EvocationUseAtPct(Player* bot)
    {
        return bot && bot->getClass() == CLASS_MAGE && AiFactory::GetPlayerSpecTab(bot) == MAGE_TAB_FIRE
                   ? WOWSIMS_EVOCATION_AUTO_MANA_PCT
                   : WOWSIMS_EVOCATION_MANA_PCT;
    }

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
        return std::max(SpellManaRate(bot) * fightSec,  // [lead1431] its heals' mana plus its upkeep
                        HealerPctPerTankHealth(bot) * fight.cost + HealerUpkeepPctPerSec(bot) * fightSec);
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
    std::unordered_map<uint64, float> actingManaRate;  // [lead520] under manaRateLock
}

void NoteActingManaRate(ObjectGuid member, float pctPerSec)
{
    std::lock_guard<std::mutex> lock(manaRateLock);
    actingManaRate[member.GetRawValue()] = pctPerSec;
}

namespace
{
    std::unordered_map<uint64, float> bossActingManaRate;  // [lead610] under manaRateLock
}

void NoteBossActingManaRate(ObjectGuid member, float pctPerSec)
{
    std::lock_guard<std::mutex> lock(manaRateLock);
    bossActingManaRate[member.GetRawValue()] = pctPerSec;
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
    struct BossPullNote
    {
        float sec = -1.0f;
        uint32 notedMs = 0;
        bool bossLeft = true;  // [lead1083]
    };
    std::unordered_map<uint64, BossPullNote> bossPullSec;  // [lead990] by group, under manaRateLock
}

void NoteBossPullSec(ObjectGuid group, float sec, bool bossLeft)
{
    std::lock_guard<std::mutex> lock(manaRateLock);
    bossPullSec[group.GetRawValue()] = {sec, getMSTime(), bossLeft};
}

namespace
{
    std::unordered_map<uint64, std::pair<float, uint32>> groupWaitSec;  // [lead1333] by group, under manaRateLock
}

void NoteGroupWaitSec(ObjectGuid group, float sec)
{
    std::lock_guard<std::mutex> lock(manaRateLock);
    groupWaitSec[group.GetRawValue()] = {sec, getMSTime()};
}

float GroupWaitSecOf(Player* bot)
{
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group)
        return 0.0f;
    std::lock_guard<std::mutex> lock(manaRateLock);
    auto const found = groupWaitSec.find(group->GetGUID().GetRawValue());
    if (found == groupWaitSec.end())
        return 0.0f;
    // the wait as it stands now: the seconds gone since the leader noted it are part of it
    return std::max(found->second.first - getMSTimeDiff(found->second.second, getMSTime()) / 1000.0f, 0.0f);
}

bool NoBossLeftOf(Player* bot)
{
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group)
        return false;
    std::lock_guard<std::mutex> lock(manaRateLock);
    auto const found = bossPullSec.find(group->GetGUID().GetRawValue());
    return found != bossPullSec.end() && !found->second.bossLeft;
}

namespace
{
    std::unordered_map<uint64, bool> bossPulledShort;  // [lead1182] the member's last own boss line: pulled short
}

bool BossLinePulledShort(Player* bot)
{
    if (!bot)
        return false;
    std::lock_guard<std::mutex> lock(manaRateLock);
    auto const found = bossPulledShort.find(bot->GetGUID().GetRawValue());
    return found != bossPulledShort.end() && found->second;
}

float BossPullSecOf(Player* bot)
{
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group)
        return -1.0f;
    std::lock_guard<std::mutex> lock(manaRateLock);
    auto const found = bossPullSec.find(group->GetGUID().GetRawValue());
    if (found == bossPullSec.end() || found->second.sec < 0.0f)
        return -1.0f;
    // the estimate as it stands now: the seconds gone since the leader made it are on the way to that pull
    return std::max(found->second.sec - getMSTimeDiff(found->second.notedMs, getMSTime()) / 1000.0f, 0.0f);
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
    float upkeep = 0.0f;     // [lead1431] mana of its casts that heal nothing themselves
    float upkeepSec = 0.0f;  // [lead1431] ... over these fight seconds
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

void NoteHealing(Player* healer, float healed, float manaSpent, float fightSec, float upkeep)
{
    if (!healer)
        return;

    std::lock_guard<std::mutex> lock(manaRateLock);
    MeasuredHealing& entry = measuredHealing[healer->GetGUID().GetRawValue()];
    if (entry.instanceId != healer->GetInstanceId())
        entry = MeasuredHealing();  // this dungeon's fights only
    entry.instanceId = healer->GetInstanceId();
    // [lead1431] the upkeep is per second of every fight, one that healed nothing too
    if (fightSec > 0.0f)
    {
        entry.upkeep += std::max(upkeep, 0.0f);
        entry.upkeepSec += fightSec;
    }
    if (healed <= 0.0f && manaSpent <= 0.0f)
        return;
    // [lead1434] the heals' mana is their casts' costs: a fight's healing and its casts are summed apart (a heal over
    // time or a shield cast in one fight heals in the next, an overhealed cast heals nothing)
    entry.healed += std::max(healed, 0.0f);
    entry.mana += std::max(manaSpent, 0.0f);
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
                game.mainHealPerMana = cost > 0 ? amount / float(cost) : 0.0f;  // [lead1434]
                game.manaPctPerSec = facts.maxMana > 0.0f && cost > 0
                                         ? 100.0f * float(cost) * float(IN_MILLISECONDS) / everyMs / facts.maxMana
                                         : 0.0f;
                game.gapSec = std::max(gapMs, 1.0f) / float(IN_MILLISECONDS);
                game.spell = info->SpellName[0] ? info->SpellName[0] : "?";
                game.castPct = facts.maxMana > 0.0f && cost > 0 ? 100.0f * float(cost) / facts.maxMana : 0.0f;
            }
        }
        cached = {healer->GetSpellMap().size(), power, healer->GetMaxPower(POWER_MANA), game};
    }
    facts = cached.facts;
    if (facts.mainHealPerMana > 0.0f)  // [lead1434] the prior: the main heal's, not the most efficient one's
        facts.healPerMana = facts.mainHealPerMana;

    std::lock_guard<std::mutex> lock(manaRateLock);
    if (auto const found = measuredHealing.find(healer->GetGUID().GetRawValue());
        found != measuredHealing.end() && found->second.instanceId == healer->GetInstanceId())
    {
        // [lead1434] coordinator 2026-11-05: until the measured effective healing covers one full health of the
        // member it heals (the tank: its max health, game data), the main heal's average over its cost stands (b263
        // 5B-10: one overhealed Holy Light read 0.24 hp per mana and lead1372 asked 100 % for 0.22 tank health)
        facts.measuredHealed = found->second.healed;
        if (found->second.mana > 0.0f && found->second.healed >= float(target->GetMaxHealth()))
        {
            facts.healPerMana = found->second.healed / found->second.mana;
            facts.measured = true;
        }
        facts.hps = std::max(facts.hps, found->second.bestHps);
        if (found->second.upkeepSec > 0.0f && facts.maxMana > 0.0f)  // [lead1431]
            facts.upkeepPctPerSec = 100.0f * found->second.upkeep / found->second.upkeepSec / facts.maxMana;
    }
    return facts;
}

float HealerUpkeepPctPerSec(Player* healer)
{
    return healer ? HealerThroughput(healer, healer).upkeepPctPerSec : 0.0f;
}

float DirectHealAverage(Player* healer, SpellInfo const* info, Unit* target)
{
    if (!healer || !info || !target)
        return 0.0f;
    float amount = 0.0f;
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        int32 const base = effect.CalcValue(healer);
        if (effect.Effect == SPELL_EFFECT_HEAL && base > 0)
            amount += float(target->SpellHealingBonusTaken(
                healer, info, healer->SpellHealingBonusDone(target, info, uint32(base), HEAL, effect.EffectIndex),
                HEAL));
    }
    return amount;
}

bool OverhealsSmallerHeal(Player* healer, Unit* target, SpellInfo const* info)
{
    if (!healer || !target || !info || !target->IsAlive())
        return false;
    // [lead1434] coordinator 2026-11-05: b263 5B-10's first fight - the leader's TopUp cast Holy Light (its list's
    // order) on a tank that missed 102 health: 1274 mana for 1299 healed. The healer's smallest direct heal (spell
    // data, the highest rank of each it knows, on itself: cached while its spells and spell power stay the same) is
    // the bound: a bigger heal on a member that misses less than it heals only overheals
    struct Cached
    {
        size_t spells = 0;
        int32 power = 0;
        float smallest = 0.0f;
    };
    thread_local std::unordered_map<uint64, Cached> cache;
    Cached& cached = cache[healer->GetGUID().GetRawValue()];
    int32 const power = healer->SpellBaseHealingBonusDone(SPELL_SCHOOL_MASK_ALL);
    if (cached.spells != healer->GetSpellMap().size() || cached.power != power)
    {
        float smallest = 0.0f;
        for (auto const& [spellId, known] : healer->GetSpellMap())
        {
            if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active)
                continue;
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
            if (!spell || spell->IsPassive() || !spell->IsPositive() || spell->PowerType != POWER_MANA)
                continue;
            if (SpellInfo const* next = spell->GetNextRankSpell(); next && healer->HasSpell(next->Id))
                continue;  // a lower rank
            float const average = DirectHealAverage(healer, spell, healer);
            if (average > 0.0f && (smallest <= 0.0f || average < smallest))
                smallest = average;
        }
        cached = {healer->GetSpellMap().size(), power, smallest};
    }
    if (cached.smallest <= 0.0f)
        return false;
    float const missing = float(target->GetMaxHealth() - target->GetHealth());
    return missing < cached.smallest && DirectHealAverage(healer, info, healer) > cached.smallest;
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

float MaxSpendPctPerSec(Player* bot)
{
    float const maxMana = float(bot->GetMaxPower(POWER_MANA));
    if (maxMana <= 0.0f)
        return 0.0f;
    bool const healer = PlayerbotAI::IsHeal(bot);
    float most = 0.0f;
    for (auto const& [spellId, known] : bot->GetSpellMap())
    {
        if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || info->IsPassive() || info->PowerType != POWER_MANA ||
            (healer ? !(info->IsPositive() && IsHealSpell(info)) : info->IsPositive()))
            continue;
        int32 const cost = info->CalcPowerCost(bot, info->GetSchoolMask());
        if (cost <= 0)
            continue;
        float const castSec = float(info->CalcCastTime(bot)) / 1000.0f;
        float const channelSec = info->IsChanneled() ? float(std::max(info->GetMaxDuration(), 0)) / 1000.0f : 0.0f;
        float const gcdSec = float(info->StartRecoveryTime) / 1000.0f;
        float const cooldownSec = float(std::max(info->RecoveryTime, info->CategoryRecoveryTime)) / 1000.0f;
        float const sec = std::max({castSec, channelSec, gcdSec, cooldownSec});
        if (sec <= 0.0f)
            continue;  // no cast, no global cooldown, no cooldown: no rate of its own
        most = std::max(most, 100.0f * float(cost) / maxMana / sec);
    }
    return most;
}

// [lead855] b223 rfc-h: before any boss was measured, the boss line read the trash's acting rate (5D arcane 1.44 %/s,
// 5C fire 1.02 %/s) while on a boss the caster casts its rotation nonstop (5D arcane on Oggleflint: 27 + 16 + 25 + ~51
// % in 45 s, ~2.6 %/s): the gate passed casters at 4-27 % and they went dry. Its rotation's nonstop spend from spell
// data and its own stats: the filler (Arcane Blast's 36032 stacks +175 % cost each, 4 at most: its cost averaged over
// the ramp 0..4 that Arcane Missiles resets), each damage-over-time spell kept up (its cost over its duration), the
// filler in the seconds their casts leave; the casts a free-cast proc saves (Arcane Concentration's 10 %, Elemental
// Focus's 2 x -40 % on a crit) and the in-combat regen (Player::UpdateManaRegen's casting value) netted out
namespace
{
    // [lead855] a boss burn of the member's that raises its rotation's spend while it lasts
    struct RotationBurn
    {
        uint32 spell = 0;
        float sec = 0.0f;
        float factor = 1.0f;
        char const* name = "";
    };
}

namespace
{
    // [lead1103] b235 rfc-h 5B: the elemental shaman's boss line read "Flame Shock 403 + Lightning Bolt 237 ... - 18 %
    // (Elemental Focus)" = 0.70 %/s (its real burn 1.33-2.13 %/s): the cost was read while Elemental Focus's
    // Clearcasting (-40 %) was up and cached, then the proc was netted out again; 5D's arcane mage read "Arcane Blast
    // 1425 x 4.50 stack ramp" (its stacks up, then the ramp again). A spell's cost in the rotation is its cost
    // without the mods of the auras that come and go (Player::ApplySpellMod's sum: the non-passive auras' pct and
    // flat cost mods taken out); talents (passive) stay
    float SteadyCost(Player* bot, SpellInfo const* info, int32 cost)
    {
        float all = 1.0f;
        float steady = 1.0f;
        int32 transientFlat = 0;
        for (SpellModifier* mod : bot->GetSpellModList(SPELLMOD_COST))
        {
            if (!mod || !info->IsAffectedBySpellMod(mod))
                continue;
            bool const transient = mod->ownerAura && !mod->ownerAura->IsPassive();
            if (mod->type == SPELLMOD_PCT)
            {
                all += float(mod->value) / 100.0f;
                if (!transient)
                    steady += float(mod->value) / 100.0f;
            }
            else if (mod->type == SPELLMOD_FLAT && transient)
                transientFlat += mod->value;
        }
        if (all <= 0.0f)
            return float(cost);
        return std::max((float(cost) / all - float(transientFlat)) * steady, 0.0f);
    }
}

float RotationSpendPctPerSec(Player* bot, float fightSec, std::string* why)
{
    float const maxMana = float(bot->GetMaxPower(POWER_MANA));
    if (maxMana <= 0.0f || PlayerbotAI::IsHeal(bot) || !PlayerbotAI::IsCaster(bot) || bot->getClass() == CLASS_WARLOCK)
        return -1.0f;

    // the spell scan is cached per bot while its spells, spell power, mana pool, haste and form stay the same
    struct Cached
    {
        size_t spells = 0;
        int32 power = -1;
        uint32 mana = 0;
        float haste = 0.0f;
        uint32 form = 0;
        float gross = -1.0f;  // mana per second before the regen and the burns
        std::string terms;
        std::vector<RotationBurn> burns;
    };
    thread_local std::unordered_map<uint64, Cached> cache;
    Cached& cached = cache[bot->GetGUID().GetRawValue()];
    int32 const power = bot->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_ALL);
    float const haste = bot->GetFloatValue(UNIT_MOD_CAST_SPEED);
    uint32 const form = bot->GetShapeshiftForm();
    if (cached.spells != bot->GetSpellMap().size() || cached.power != power ||
        cached.mana != bot->GetMaxPower(POWER_MANA) || cached.haste != haste || cached.form != form)
    {
        struct Pick
        {
            SpellInfo const* info = nullptr;
            float cost = 0.0f;
            float sec = 0.0f;       // its cast (a channel's duration) or global cooldown, hasted
            float value = 0.0f;     // its damage per cast second
            float interval = 0.0f;  // a damage-over-time spell: cast again after its duration (or its cooldown)
            bool direct = false;    // [lead920] a direct damage part: a cast that can crit
        };
        Pick filler;
        std::unordered_map<uint32, Pick> dots;  // by spell specific (one curse / sting), else by spell
        // [lead1103] b235 5B: Lava Burst (8 s) and Chain Lightning (6 s) were in no term of the elemental's line - a
        // spell with a cooldown was neither filler nor damage over time. A cast-time damage spell with a cooldown whose
        // damage per cast second beats the filler is cast on its cooldown (wowsims elemental: Lava Burst, Chain
        // Lightning; shadow: Mind Blast), its cost over that cooldown, its cast seconds out of the filler's share
        std::unordered_map<uint32, Pick> onCooldown;  // by first rank
        std::vector<SpellInfo const*> burnSpells;
        std::vector<RotationBurn> burns;
        for (auto const& [spellId, known] : bot->GetSpellMap())
        {
            if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || info->IsPassive() || info->CheckShapeshift(form) != SPELL_CAST_OK)
                continue;
            if (uint32 const next = sSpellMgr->GetNextSpellInChain(spellId); next && bot->HasSpell(next))
                continue;  // a lower rank
            // [lead1038] b230 rfc-h 5D: the arcane mage's line read "Living Bomb 718 / 12 s" (7.04 %/s nonstop
            // against 1.29 %/s measured) - Living Bomb is the fire tree's talent, its higher ranks stay in the spell
            // book of a member whose active spec has not that talent (the core keeps them). A spell whose first rank
            // is a talent the active spec does not have is no spell of its rotation (wowsims arcane never casts it)
            if (SpellInfo const* first = info->GetFirstRankSpell();
                first && GetTalentSpellPos(first->Id) && !bot->HasTalent(first->Id, bot->GetActiveSpec()))
                continue;
            if (info->IsPositive())
            {
                if (std::max(info->RecoveryTime, info->CategoryRecoveryTime) > 0)
                    burnSpells.push_back(info);  // a cooldown of its own: a boss burn it may raise its spend with
                continue;
            }
            if (info->PowerType != POWER_MANA || info->IsAffectingArea())
                continue;
            // [lead990] b228 rfc-h 5C: the balance druid's rotation kept "Entangling Roots 244 / 27 s" up as a damage
            // over time spell - a crowd control no damage rotation casts (wowsims): a spell whose aura roots, stuns,
            // fears, confuses or pacifies is no rotation spell
            if (info->HasAura(SPELL_AURA_MOD_ROOT) || info->HasAura(SPELL_AURA_MOD_STUN) ||
                info->HasAura(SPELL_AURA_MOD_FEAR) || info->HasAura(SPELL_AURA_MOD_CONFUSE) ||
                info->HasAura(SPELL_AURA_MOD_PACIFY))
                continue;
            int32 const rawCost = info->CalcPowerCost(bot, info->GetSchoolMask());
            if (rawCost <= 0)
                continue;
            float const cost = SteadyCost(bot, info, rawCost);  // [lead1103]
            if (cost <= 0.0f)
                continue;
            SpellSchoolMask const school = info->GetSchoolMask();
            float const sp = float(bot->SpellBaseDamageBonusDone(school));
            // [lead990] b228 rfc-h: the fire mages' filler came out Arcane Blast ("Arcane Blast 228 x 4.50 stack ramp
            // / 2.43 s", 2.90-6.74 %/s; their Fireball spends ~1.3 %/s) - the pick read the spells' base damage and
            // coefficient without the member's talents (Fire Power's +10 % damage, Empowered Fire's +15 % of spell
            // power on Fireball). The coefficient and the damage take the member's spell mods as the core's
            // Unit::SpellDamageBonusDone applies them (SPELLMOD_BONUS_MULTIPLIER, SPELLMOD_DAMAGE / SPELLMOD_DOT)
            auto const coefficient = [&](SpellInfo const* spell, DamageEffectType type)
            {
                SpellBonusEntry const* bonus = sSpellMgr->GetSpellBonusData(spell->Id);
                float const own = bonus ? (type == DOT ? bonus->dot_damage : bonus->direct_damage) : -1.0f;
                float coeff = (own >= 0.0f ? own : bot->CalculateDefaultCoefficient(spell, type)) * 100.0f;
                bot->ApplySpellMod(spell->Id, SPELLMOD_BONUS_MULTIPLIER, coeff, nullptr, true);
                return coeff / 100.0f;
            };
            auto const modded = [&](SpellInfo const* spell, float value, DamageEffectType type)
            {
                bot->ApplySpellMod(spell->Id, type == DOT ? SPELLMOD_DOT : SPELLMOD_DAMAGE, value, nullptr, true);
                return value;
            };
            bool areaTick = false;  // [lead990] a channel whose ticks hit an area (Mind Sear): an AoE spell
            float damage = 0.0f;
            bool periodic = false;
            bool direct = false;  // [lead920]
            float const durationMs = float(std::max(info->GetMaxDuration(), 0));
            for (SpellEffectInfo const& effect : info->GetEffects())
            {
                if (effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
                {
                    damage += modded(info, float(std::max(effect.CalcValue(bot), 0)) +
                                               sp * coefficient(info, SPELL_DIRECT_DAMAGE),
                                     SPELL_DIRECT_DAMAGE);
                    direct = true;
                }
                else if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.Amplitude && durationMs > 0.0f)
                {
                    float const ticks = durationMs / float(effect.Amplitude);
                    if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE ||
                        effect.ApplyAuraName == SPELL_AURA_PERIODIC_LEECH)
                    {
                        damage += ticks * modded(info, float(std::max(effect.CalcValue(bot), 0)) +
                                                           sp * coefficient(info, DOT),
                                                 DOT);
                        periodic = true;
                    }
                    // a channel's ticks cast a damage spell (Mind Flay 58381, Arcane Missiles 42845)
                    else if (info->IsChanneled() && (effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL ||
                                                     effect.ApplyAuraName ==
                                                         SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE))
                        if (SpellInfo const* tick = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                        {
                            areaTick |= tick->IsAffectingArea();
                            float perTick =
                                effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE
                                    ? float(std::max(effect.CalcValue(bot), 0))
                                    : 0.0f;
                            if (!perTick)
                                for (SpellEffectInfo const& hit : tick->GetEffects())
                                    if (hit.Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
                                        perTick += float(std::max(hit.CalcValue(bot), 0));
                            if (perTick > 0.0f)
                                damage += ticks * modded(info, perTick + sp * coefficient(tick, SPELL_DIRECT_DAMAGE),
                                                         SPELL_DIRECT_DAMAGE);
                        }
                }
            }
            if (damage <= 0.0f || areaTick)  // [lead990] b228 5B: "Mind Sear 853 / 4.46 s" was the shadow filler
                continue;
            damage *= bot->GetTotalAuraMultiplierByMiscMask(SPELL_AURA_MOD_DAMAGE_PERCENT_DONE, school);
            // [lead990] the cast time without the mods that spend a charge (Presence of Mind: b228 "Frostbolt 359 /
            // 1.23 s" - the global cooldown - was a fire mage's filler for the cache's life): its talents' and its
            // haste's, as WorldObject::ModSpellCastTime applies them to a magic spell
            int32 castMs = info->CastTimeEntry ? info->CastTimeEntry->CastTime : 0;
            bot->ApplySpellMod(info->Id, SPELLMOD_CASTING_TIME, castMs, nullptr, true);
            if (info->DmgClass == SPELL_DAMAGE_CLASS_MAGIC)
                castMs = int32(float(castMs) * haste);
            float const castSec = info->IsChanneled() ? durationMs / 1000.0f * haste
                                                      : float(std::max<int32>(castMs, 0)) / 1000.0f;
            float const gcdSec =
                info->StartRecoveryTime ? std::max(float(info->StartRecoveryTime) / 1000.0f * haste, 1.0f) : 0.0f;
            float const sec = std::max(castSec, gcdSec);
            if (sec <= 0.0f)
                continue;
            float const cooldownSec = float(std::max(info->RecoveryTime, info->CategoryRecoveryTime)) / 1000.0f;
            Pick const pick{info, cost, sec, damage / sec, std::max(durationMs / 1000.0f, cooldownSec),
                            direct || info->IsChanneled()};
            if (periodic && !info->IsChanneled() && castSec <= gcdSec + 0.01f)
            {
                // a damage-over-time spell kept up on the target (instant, or cast within its global cooldown)
                SpellSpecificType const specific = info->GetSpellSpecific();
                uint32 const slot = specific == SPELL_SPECIFIC_CURSE || specific == SPELL_SPECIFIC_STING
                                        ? uint32(specific) | 0x80000000u
                                        : info->GetFirstRankSpell()->Id;  // [lead990] one per spell, not per rank
                                                                           // (b228: "Living Bomb 718 / 12 s" x 3)
                if (auto const found = dots.find(slot); found == dots.end() || found->second.value < pick.value)
                    dots[slot] = pick;
            }
            else if (cooldownSec <= 0.0f && pick.value > filler.value)
                filler = pick;  // the spell it casts nonstop: no cooldown of its own
            else if (cooldownSec > 0.0f && castMs > 0 && !info->IsChanneled() && !periodic)
            {
                uint32 const slot = info->GetFirstRankSpell()->Id;
                if (auto const found = onCooldown.find(slot);
                    found == onCooldown.end() || found->second.value < pick.value)
                    onCooldown[slot] = Pick{info, float(cost), sec, damage / sec, cooldownSec, true};
            }
        }

        float gross = -1.0f;
        std::string terms;
        if (filler.info || !dots.empty())
        {
            gross = 0.0f;
            float dotTime = 0.0f;
            for (auto const& [slot, dot] : dots)
            {
                gross += dot.cost / dot.interval;
                dotTime += dot.sec / dot.interval;
                terms += Acore::StringFormat("{}{} {:.0f} / {:.0f} s", terms.empty() ? "" : " + ",
                                             dot.info->SpellName[0], dot.cost, dot.interval);
            }
            for (auto const& [slot, cast] : onCooldown)  // [lead1103]
            {
                if (filler.info && cast.value <= filler.value)
                    continue;
                gross += cast.cost / cast.interval;
                dotTime += cast.sec / cast.interval;
                dots.emplace(slot | 0x40000000u, cast);  // its direct part can crit (Moonkin-style returns)
                terms += Acore::StringFormat("{}{} {:.0f} / {:.0f} s cooldown", terms.empty() ? "" : " + ",
                                             cast.info->SpellName[0], cast.cost, cast.interval);
            }
            if (filler.info)
            {
                // the filler's own cost stacks (Arcane Blast's 36032): their average over the ramp 0..max
                float ramp = 1.0f;
                for (SpellEffectInfo const& effect : filler.info->GetEffects())
                    if (SpellInfo const* stacks = effect.TriggerSpell ? sSpellMgr->GetSpellInfo(effect.TriggerSpell)
                                                                      : nullptr)
                        for (SpellEffectInfo const& mod : stacks->GetEffects())
                            if (mod.Effect == SPELL_EFFECT_APPLY_AURA &&
                                mod.ApplyAuraName == SPELL_AURA_ADD_PCT_MODIFIER && mod.MiscValue == SPELLMOD_COST &&
                                mod.CalcValue(bot) > 0 && filler.info->IsAffected(stacks->SpellFamilyName,
                                                                                  mod.SpellClassMask))
                                ramp = std::max(ramp, 1.0f + float(std::max<uint32>(stacks->StackAmount, 1)) / 2.0f *
                                                                 float(mod.CalcValue(bot)) / 100.0f);
                float const share = std::max(1.0f - dotTime, 0.0f);
                gross += share * filler.cost * ramp / filler.sec;
                terms += Acore::StringFormat("{}{} {:.0f}{} / {:.2f} s over {:.0f} % of the time",
                                             terms.empty() ? "" : " + ", filler.info->SpellName[0], filler.cost,
                                             ramp > 1.0f ? Acore::StringFormat(" x {:.2f} stack ramp", ramp)
                                                         : std::string(),
                                             filler.sec, share * 100.0f);
                // a free-cast proc (Clearcasting): the share of the casts it saves
                float crit = 0.0f;
                for (uint8 s = SPELL_SCHOOL_HOLY; s < MAX_SPELL_SCHOOL; ++s)
                    if (filler.info->GetSchoolMask() & (1 << s))
                        crit = std::max(crit, bot->GetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1 + s) / 100.0f);
                float saved = 0.0f;
                for (AuraEffect const* proc : bot->GetAuraEffectsByType(SPELL_AURA_PROC_TRIGGER_SPELL))
                {
                    SpellInfo const* talent = proc->GetSpellInfo();
                    SpellInfo const* freeCast =
                        sSpellMgr->GetSpellInfo(talent->Effects[proc->GetEffIndex()].TriggerSpell);
                    if (!freeCast)
                        continue;
                    float cut = 0.0f;
                    for (SpellEffectInfo const& mod : freeCast->GetEffects())
                        if (mod.Effect == SPELL_EFFECT_APPLY_AURA && mod.ApplyAuraName == SPELL_AURA_ADD_PCT_MODIFIER &&
                            mod.MiscValue == SPELLMOD_COST && mod.CalcValue(bot) < 0 &&
                            filler.info->IsAffected(freeCast->SpellFamilyName, mod.SpellClassMask))
                            cut = std::max(cut, std::min(-float(mod.CalcValue(bot)) / 100.0f, 1.0f));
                    if (cut <= 0.0f)
                        continue;
                    SpellProcEntry const* entry = sSpellMgr->GetSpellProcEntry(talent->Id);
                    float chance = (entry && entry->Chance > 0.0f ? entry->Chance : float(talent->ProcChance)) / 100.0f;
                    if (entry && entry->ProcsPerMinute > 0.0f)
                        chance = entry->ProcsPerMinute * filler.sec / 60.0f;
                    // [lead920] b225 rfc-h 5C: the balance druid's line read "- 100 % (Omen of Clarity)" (its
                    // spell_proc Chance 100, the core script rolls 6 %), its rotation 0.00 %/s: the Oggleflint gate
                    // passed it at 3 % and its Innervate went to the mage. The talent's roll is its script's
                    bool const scriptRoll = talent->Id == SPELL_OMEN_OF_CLARITY;
                    if (scriptRoll)
                        chance = OMEN_OF_CLARITY_SPELL_PROC_PCT / 100.0f;
                    if (entry && (entry->HitMask & PROC_HIT_CRITICAL))
                        chance *= crit;
                    float const charges = float(std::max<uint32>(freeCast->ProcCharges, 1));
                    float const part = std::min(std::min(chance, 1.0f) * charges, 1.0f) * cut;
                    if (part > 0.0f)
                    {
                        saved += part;
                        terms += Acore::StringFormat(" - {:.0f} % ({}{})", part * 100.0f, talent->SpellName[0],
                                                     scriptRoll ? " [lead920]" : "");
                    }
                }
                gross *= 1.0f - std::min(saved, 1.0f);
                // [lead920] the mana its crits return (Moonkin Form's 24905 -> 53506: 2 % of its max mana on a spell
                // crit, Rawr.Moonkin SpellRotation.cs:186, 256): a proc on a crit that energizes its own mana, per
                // second over the casts that can crit (the filler's, the damage-over-time spells' direct part)
                float critCasts = share / filler.sec;
                for (auto const& [slot, dot] : dots)
                    if (dot.direct)
                        critCasts += 1.0f / dot.interval;
                for (AuraEffect const* proc : bot->GetAuraEffectsByType(SPELL_AURA_PROC_TRIGGER_SPELL))
                {
                    SpellInfo const* talent = proc->GetSpellInfo();
                    SpellProcEntry const* entry = sSpellMgr->GetSpellProcEntry(talent->Id);
                    SpellInfo const* gain = sSpellMgr->GetSpellInfo(talent->Effects[proc->GetEffIndex()].TriggerSpell);
                    if (!gain || !entry || !(entry->HitMask & PROC_HIT_CRITICAL) || entry->ProcsPerMinute > 0.0f)
                        continue;
                    float manaPerProc = 0.0f;
                    for (SpellEffectInfo const& effect : gain->GetEffects())
                        if (effect.MiscValue == POWER_MANA && effect.CalcValue(bot) > 0)
                        {
                            if (effect.Effect == SPELL_EFFECT_ENERGIZE_PCT)
                                manaPerProc += maxMana * float(effect.CalcValue(bot)) / 100.0f;
                            else if (effect.Effect == SPELL_EFFECT_ENERGIZE)
                                manaPerProc += float(effect.CalcValue(bot));
                        }
                    float const chance =
                        std::min((entry->Chance > 0.0f ? entry->Chance : float(talent->ProcChance)) / 100.0f, 1.0f);
                    if (float const back = critCasts * crit * chance * manaPerProc; back > 0.0f)
                    {
                        gross = std::max(gross - back, 0.0f);
                        terms += Acore::StringFormat(" - {:.0f} mana/s ({} on a crit [lead920])", back,
                                                     talent->SpellName[0]);
                    }
                }
                // its own boss burns (lead853: cast at the engage) that raise the filler's cost (Arcane Power +20 %)
                // or its cast speed (Icy Veins +20 %, Elemental Mastery's 64701 +15 %, Heroism +30 %): their factor
                // over their own duration
                for (SpellInfo const* buff : burnSpells)
                {
                    auto const read = [&](SpellInfo const* spell)
                    {
                        float const sec = float(std::max(spell->GetMaxDuration(), 0)) / 1000.0f;
                        if (sec <= 0.0f)
                            return;
                        float factor = 1.0f;
                        for (SpellEffectInfo const& mod : spell->GetEffects())
                        {
                            int32 const value = mod.Effect == SPELL_EFFECT_APPLY_AURA ? mod.CalcValue(bot) : 0;
                            if (value <= 0)
                                continue;
                            if ((mod.ApplyAuraName == SPELL_AURA_ADD_PCT_MODIFIER && mod.MiscValue == SPELLMOD_COST &&
                                 filler.info->IsAffected(spell->SpellFamilyName, mod.SpellClassMask)) ||
                                mod.ApplyAuraName == SPELL_AURA_MOD_CASTING_SPEED_NOT_STACK ||
                                mod.ApplyAuraName == SPELL_AURA_HASTE_SPELLS)
                                factor *= 1.0f + float(value) / 100.0f;
                        }
                        if (factor > 1.0f)
                            burns.push_back({buff->Id, sec, factor, buff->SpellName[0]});
                    };
                    read(buff);
                    for (SpellEffectInfo const& effect : buff->GetEffects())
                        if (SpellInfo const* next = effect.TriggerSpell ? sSpellMgr->GetSpellInfo(effect.TriggerSpell)
                                                                        : nullptr)
                            read(next);
                }
            }
        }
        cached = {bot->GetSpellMap().size(), power, bot->GetMaxPower(POWER_MANA), haste, form, gross, terms, burns};
    }
    if (cached.gross < 0.0f)
        return -1.0f;
    // the boss burns ready for this fight: their factor over the share of the fight they last
    float gross = cached.gross;
    std::string burnTerms;
    for (RotationBurn const& burn : cached.burns)
    {
        if (fightSec <= 0.0f || bot->HasSpellCooldown(burn.spell))
            continue;
        float const share = std::min(burn.sec / fightSec, 1.0f);
        gross *= 1.0f + share * (burn.factor - 1.0f);
        burnTerms += Acore::StringFormat(" x {:.2f} ({} over {:.0f} % of the fight)", burn.factor, burn.name,
                                         share * 100.0f);
    }
    // the in-combat (casting) regen per second: Player::UpdateManaRegen, Player::Regenerate's rate
    // [lead1020] b229 rfc-h 5H-8 362-480 s: the fire mage's boss line read "- 20 regen" standing and the drink's
    // regen once it sat: UpdateManaRegen puts the drink aura's SPELL_AURA_MOD_POWER_REGEN (mp5) into this field, the
    // drink paid more than the rotation spends, the line fell to 0 % on the drink's first tick, MemberBossGap came
    // back empty, the leader ended its Rest, it stood up (the drink gone, the line back at 95 %) and sat again - 66
    // times (and the hunter's / priest's lines, reading the mage's wait "for Rtmagad anyway", ended with it). The
    // fight's regen is the regen without the food / drink aura (it breaks when the bot stands up for the fight)
    float drinkMp5 = 0.0f;
    for (AuraEffect const* effect : bot->GetAuraEffectsByType(SPELL_AURA_MOD_POWER_REGEN))
        if (effect->GetMiscValue() == POWER_MANA)
            if (SpellSpecificType const specific = effect->GetSpellInfo()->GetSpellSpecific();
                specific == SPELL_SPECIFIC_DRINK || specific == SPELL_SPECIFIC_FOOD_AND_DRINK)
                drinkMp5 += float(effect->GetAmount());
    float const regen =
        std::max(bot->GetFloatValue(UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER + POWER_MANA) - drinkMp5 / 5.0f,
                 0.0f) *
        sWorld->getRate(RATE_POWER_MANA);
    float const pct = 100.0f * std::max(gross - regen, 0.0f) / maxMana;
    if (why)
        *why = Acore::StringFormat("{:.2f} %/s = ({}){} - {:.0f} regen, mana per s of {:.0f}", pct, cached.terms,
                                   burnTerms, regen, maxMana);
    return pct;
}

float BossRotationRate(Player* bot, float fightSec, std::string* why)
{
    {
        std::lock_guard<std::mutex> lock(manaRateLock);
        uint64 const key = bot->GetGUID().GetRawValue();
        if (bossManaRate.count(key) || bossActingManaRate.count(key))
            return -1.0f;  // [lead610] a boss fight measured: its own rate
    }
    return RotationSpendPctPerSec(bot, fightSec, why);
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

float FightSecNoted(Player* bot)
{
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group)
        return 0.0f;
    std::lock_guard<std::mutex> lock(manaRateLock);
    auto const found = fightLengths.find(group->GetGUID().GetRawValue());
    return found != fightLengths.end() && found->second.instanceId == bot->GetInstanceId() ? found->second.sec : 0.0f;
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
    thread_local bool nestedManaLine = false;  // [lead525] another member's line read inside one: no overlap there

    // [lead1390] the credits each member's last own mana line counted ready now (ManaLineCredits)
    std::mutex manaCreditLock;
    std::unordered_map<uint64, std::vector<ManaLineCredit>> manaLineCredits;
    // [lead1421] the mana potion each member's last boss line out of combat (the boss gate's) credited, % of max mana
    std::unordered_map<uint64, float> bossPotionCredits;
}

std::vector<ManaLineCredit> ManaLineCredits(ObjectGuid member)
{
    std::lock_guard<std::mutex> lock(manaCreditLock);
    auto const found = manaLineCredits.find(member.GetRawValue());
    return found != manaLineCredits.end() ? found->second : std::vector<ManaLineCredit>();
}

float BossPotionCredit(ObjectGuid member)
{
    std::lock_guard<std::mutex> lock(manaCreditLock);
    auto const found = bossPotionCredits.find(member.GetRawValue());
    return found != bossPotionCredits.end() ? found->second : 0.0f;
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
            // [lead520] b211 rfc-h 5H: "Evocation in the fight: 2.0 s in the fight vs -1.0 s drinking" - the fire
            // mage carried 78 drinks and this read 0: the WotLK drinks (43183 and the core's list in
            // AuraEffect::UpdatePeriodic) carry 0 in their SPELL_AURA_MOD_POWER_REGEN effect, the core moves the
            // SPELL_AURA_PERIODIC_DUMMY effect's amount into it. Without a drink every mana tool won the comparison
            float regen = 0.0f;
            float dummy = 0.0f;
            bool manaRegen = false;
            for (SpellEffectInfo const& effect : info->GetEffects())
            {
                if (effect.ApplyAuraName == SPELL_AURA_MOD_POWER_REGEN && effect.MiscValue == POWER_MANA)
                {
                    manaRegen = true;
                    regen = std::max(regen, float(effect.CalcValue(bot)));
                }
                else if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_DUMMY)
                    dummy = std::max(dummy, float(effect.CalcValue(bot)));
            }
            if (manaRegen)
                best = std::max(best, 100.0f * (regen > 0.0f ? regen : dummy) / 5.0f / float(maxMana));
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
            // [lead425] the Mana Tide Totem's tick is a script (spell_sha_mana_tide_totem): each restores the
            // periodic aura's value in % of the receiver's max mana
            else if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL &&
                     effect.TriggerSpell == SPELL_MANA_TIDE_TICK)
                pct += ticks * float(effect.CalcValue(caster));
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
    float pct = ToolPct(caster, receiver, info, 2, 0);
    // [lead425] Dispersion's mana is its linked aura (spell_linked_spell), not an effect of its own
    if (spell == SPELL_DISPERSION)
        pct += ToolPct(caster, receiver, sSpellMgr->GetSpellInfo(SPELL_DISPERSION_MANA), 0, 0);
    return pct;
}

float ManaToolRadius(Player* caster, uint32 spell)
{
    if (spell != SPELL_MANA_TIDE_TOTEM)
        return 0.0f;
    SpellInfo const* tick = sSpellMgr->GetSpellInfo(SPELL_MANA_TIDE_TICK);
    return tick ? tick->Effects[EFFECT_0].CalcRadius(caster) : 0.0f;
}

namespace
{
    // [lead850] A mana return that arrives at one moment of the fight: a mana potion / gem (an instant) or Evocation's
    // channel (its ticks over channelSec, the member casts nothing meanwhile and the fight lasts the damage it does not
    // deal longer: extendSec). b223 rfc-h 5D Oggleflint: the line summed Evocation 60 % + potion 25 % + gem 16 % =
    // 101 % against a 95 % need as if all came at once; Evocation comes at the wowsims line (<= 25 %) once ready, the
    // potion and the gem only where their whole return fits the pool (TickPotions' room test), each once
    struct BurstReturn
    {
        float readySec = 0.0f;
        float pct = 0.0f;
        float channelSec = 0.0f;
        float extendSec = 0.0f;
        float useAtPct = 100.0f;  // a channel: used at or below this mana
        bool fitsPool = false;    // an instant: used once its whole return fits the pool
    };

    // [lead850] the member's mana through a fight of `fightSec` from `start` %, spending `rate` %/s while it casts:
    // true when it never runs dry before the fight's end and ends with `reserve` % or more
    bool BurstFightLasts(float start, float rate, float fightSec, float reserve, std::vector<BurstReturn> returns)
    {
        constexpr float EPS = 0.001f;
        float t = 0.0f;
        float mana = start;
        float end = fightSec;
        for (size_t step = 0; step < 4 * returns.size() + 8; ++step)
        {
            bool used = false;
            for (BurstReturn& r : returns)
            {
                if (r.pct <= 0.0f || r.readySec > t + EPS)
                    continue;
                bool const dry = mana <= EPS;
                if (!dry && !(r.fitsPool ? mana <= 100.0f - r.pct + EPS : mana <= r.useAtPct + EPS))
                    continue;
                float part = 1.0f;  // a channel the fight's end cuts returns its ticks until then
                if (r.channelSec > 0.0f)
                {
                    end += r.extendSec;
                    part = std::clamp((end - t) / r.channelSec, 0.0f, 1.0f);
                    t += r.channelSec;
                }
                mana = std::min(100.0f, mana + r.pct * part);
                r.pct = 0.0f;
                used = true;
                break;
            }
            if (used)
                continue;
            if (t >= end - EPS)
                break;
            float next = end;
            if (rate > 0.0f)
                next = std::min(next, t + mana / rate);
            for (BurstReturn const& r : returns)
            {
                if (r.pct <= 0.0f)
                    continue;
                if (r.readySec > t + EPS)
                    next = std::min(next, r.readySec);
                else if (rate > 0.0f)
                {
                    float const at = r.fitsPool ? 100.0f - r.pct : r.useAtPct;
                    if (mana > at)
                        next = std::min(next, t + (mana - at) / rate);
                }
            }
            if (next <= t + EPS)
                return false;  // dry, nothing it can use now
            mana -= rate * (next - t);
            t = next;
        }
        return t >= end - EPS && mana + EPS >= reserve;
    }

    // [lead850] the lowest start mana whose fight lasts (BurstFightLasts); -1: not even a full pool
    float BurstLine(float rate, float fightSec, float reserve, std::vector<BurstReturn> const& returns)
    {
        if (!BurstFightLasts(100.0f, rate, fightSec, reserve, returns))
            return -1.0f;
        float low = 0.0f;
        float high = 100.0f;
        if (BurstFightLasts(0.0f, rate, fightSec, reserve, returns))
            return 0.0f;
        for (uint32 i = 0; i < 14; ++i)
        {
            float const mid = (low + high) / 2.0f;
            (BurstFightLasts(mid, rate, fightSec, reserve, returns) ? high : low) = mid;
        }
        return high;
    }

    // [lead850] the share of the group's damage this member deals (GroupDamage, measured), else one of its damage
    // dealers'; a healer's cast stops the healing the whole group's damage goes on under: 1
    float DamageShare(Player* bot, uint32 dealers)
    {
        if (PlayerbotAI::IsHeal(bot))
            return 1.0f;
        float const group = GroupDamage::SustainedDps(bot);
        float const own = GroupDamage::MemberSustainedDps(bot);
        if (group > 0.0f && own > 0.0f)
            return std::min(own / group, 1.0f);
        return 1.0f / float(std::max<uint32>(dealers, 1));
    }
}

Unit* ShadowfiendReach(Player* caster, Unit* preferred)
{
    SpellInfo const* info = caster ? sSpellMgr->GetSpellInfo(SPELL_SHADOWFIEND) : nullptr;
    if (!info)
        return nullptr;
    float const range = info->GetMaxRange(false, caster);
    auto const reaches = [caster, range](Unit* unit)
    {
        return unit && unit->IsAlive() && unit->IsInWorld() && unit->GetMap() == caster->GetMap() &&
               caster->IsValidAttackTarget(unit) && caster->IsWithinDistInMap(unit, range) &&
               caster->IsWithinLOSInMap(unit);
    };
    if (reaches(preferred))
        return preferred;
    Unit* nearest = nullptr;
    auto const consider = [&](Player* member)
    {
        for (Unit* attacker : member->getAttackers())
            if (reaches(attacker) && (!nearest || caster->GetDistance(attacker) < caster->GetDistance(nearest)))
                nearest = attacker;
    };
    consider(caster);
    if (Group* group = caster->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource();
                member && member != caster && member->IsInWorld() && member->GetMap() == caster->GetMap())
                consider(member);
    return nearest;
}

float FightManaLine(Player* bot, float fightSec, float cost, std::string* why, bool boss, bool credits, bool potion,
                    bool damageCost)
{
    float rate = SpellManaRate(bot);  // [lead264] nothing measured: the bot's spell data
    bool measured = false;
    char const* source = "measured";
    // [lead855] a boss line before any boss fight of the member is measured: its rotation cast nonstop (spell data),
    // not the trash's acting rate
    std::string rotationWhy;
    // [lead990] b228 rfc-h: the trash line read the rate measured over the trash fights - 5H's fire mage "mana 1 % <
    // need 5 % = 0.61 %/s (measured) x 13 s" (946 s) - a rate measured while it wanded dry (5H fire 168 s a run on
    // the wand "not enough power", 5C fire 142, 5D arcane 61, 5B shadow 49, 5A frost 31): its low rate let it start
    // the next pull short and the short pull measured the low rate again. A damage caster's trash line reads its
    // rotation cast nonstop over the pull's estimated seconds too (wowsims: it casts the whole fight), its measured
    // rate where that is higher (AoE)
    float const rotation =
        boss ? BossRotationRate(bot, fightSec, &rotationWhy) : RotationSpendPctPerSec(bot, fightSec, &rotationWhy);
    // [lead1162] b240 rfc-h 5D: the arcane mage came to the bosses at 2 / 34 / 52 % (5D-4) and 41 / 16 / 68 / 80 %
    // (5D-12): the gate read "1.36 %/s (its acting seconds)", "0.95 %/s (its acting seconds on bosses)" against its
    // rotation's 2.46-2.67 %/s nonstop (Arcane Blast at its stacks, lead1103's steady costs), so 62 s x 1.36 + 5 - 89
    // cooldowns left a 0 % line; 5C's balance druid the same (0.83-1.18 %/s on bosses vs 1.49-1.76 nonstop). A rate
    // measured on a boss the caster came to short of mana is the fight it could not cast (lead563's words): its
    // potion, gem and drinks went first and its rotation waited. On a boss a damage caster casts its rotation the
    // whole fight (wowsims): its line's rate is never below the rotation's nonstop spend
    std::string nonstopWhy;
    float const nonstop = boss && rotation < 0.0f && !PlayerbotAI::IsHeal(bot)
                              ? RotationSpendPctPerSec(bot, fightSec, &nonstopWhy)
                              : -1.0f;
    bool bossMeasured = false;  // [lead855] the boss line reads a boss fight's measured rate
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
            // [lead520] b211 rfc-h 5H: the fire mage came to Oggleflint (the first boss) on "0.45 %/s (its full
            // fights) x 79 s" and burnt ~1.5 %/s there, dry before the end - its trash fights' seconds were held by
            // the threat headroom, moved and waited for the first hit. On a boss it runs its rotation the whole fight
            // (wowsims): before a boss is measured its rate per second it acted, when higher. Not a healer's: it
            // casts as the fight's damage asks (its heal term below), not a rotation
            // [lead563] b214 rfc-h 5H: Oggleflint's line read the fire mage's acting rate (1.18 %/s), it ran dry there
            // (Frostbolt, the wand: 1102 of the sim's 1581 Fireball DPS) and that starved fight became its rate
            // "measured on bosses" (0.70 %/s): Taragaman's line was 17 % for 102 s, the trash Evocation verdict saw
            // no boss short of mana from a full pool and spent it at 514 s (back at 754 s, the fight ended at 753 s),
            // the mage came at 17 % and ran dry again (Taragaman 61 % of wowsims). A boss rate measured while the
            // member could not cast is below what its rotation spends: the acting rate holds when higher
            // [lead610] b217 rfc-h 5H: Taragaman's line read "0.78 %/s (its acting seconds) x 84 s" - every fight's
            // acting rate, the trash pulls in it - while the fire mage's boss rotation burnt ~1.3 %/s there (64 -> 1 %
            // in ~50 s, dry ~15 s before the kill). Once a boss fight is measured, its acting rate on bosses is the
            // boss rotation's (the more specific fact); before that every fight's acting rate
            if (!PlayerbotAI::IsHeal(bot))
            {
                if (auto const acting = actingManaRate.find(key);
                    acting != actingManaRate.end() && acting->second > rate)
                {
                    rate = acting->second;
                    measured = true;
                    source = "its acting seconds";
                }
                if (auto const bossActing = bossActingManaRate.find(key);
                    bossActing != bossActingManaRate.end() && bossActing->second > 0.0f)
                {
                    auto const bossRate = bossManaRate.find(key);
                    rate = std::max(bossActing->second, bossRate != bossManaRate.end() ? bossRate->second : 0.0f);
                    measured = true;
                    source = "its acting seconds on bosses";
                }
                bossMeasured = bossManaRate.count(key) || bossActingManaRate.count(key);
            }
            // [lead855] b223 rfc-h: before a boss was measured the line read the trash's acting rate (5D arcane 1.44
            // %/s, 5C fire 1.02 %/s; Oggleflint took ~2.6 %/s of the arcane mage): its rotation cast nonstop
            if (rotation >= 0.0f)
            {
                rate = rotation;
                measured = true;
                source = "its rotation nonstop, no boss measured";
            }
            else if (nonstop > rate)  // [lead1162] a boss rate below its rotation's: a starved fight
            {
                rate = nonstop;
                measured = true;
                source = "its rotation nonstop (above its rate measured on bosses) [lead1162]";
                rotationWhy = nonstopWhy;
            }
        }
        else if (rotation > rate)  // [lead990] a trash line: the rotation's nonstop rate when higher
        {
            rate = rotation;
            measured = true;
            source = "its rotation nonstop";
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
    // [lead1294] b251 rfc-h 5E-5 t=1098: the gate passed the disc priest into Bazzalan at 32 % on a 6 % line - "heals
    // 9.4 % per tank health (measured) x 1.29 cost". The 1.29 was the boss's real damage on the tank (it took 1.43,
    // the priest healed 1.28), the 9.4 % its use per tank health of the trash plans' cost (20.4 planned against 7.9
    // healed over the run): its measured mana per tank health it healed was 32 % (lead258, 204041 healed for 32888
    // mana), the fight took 27 % + 8 % returned and asked 42 %. A cost in tank damage reads the mana per healed tank
    // health
    if (PlayerbotAI::IsHeal(bot) && !damageCost)
    {
        std::lock_guard<std::mutex> lock(manaRateLock);
        if (auto const found = healerPctPerCost.find(bot->GetGUID().GetRawValue()); found != healerPctPerCost.end())
        {
            perTankHealth = found->second;
            perCostMeasured = true;
        }
    }
    // [lead1431] a healer's mana per tank health healed is its heals' alone (lead258 notes its upkeep apart): the
    // heal term adds its measured upkeep over the fight's seconds. The trash term measured per costed tank health
    // (lead393) already holds it
    float const upkeep = PlayerbotAI::IsHeal(bot) && !perCostMeasured ? HealerUpkeepPctPerSec(bot) : 0.0f;
    if (PlayerbotAI::IsHeal(bot))
        need = std::max(need, perTankHealth * cost + upkeep * fightSec);
    // [lead88] user: no flat reserve - the reserve is the spend over the measured fight-length uncertainty (the mean
    // |real - estimated| seconds of this run's fights); a healer adds its share of the predicted tank damage over
    // those seconds. [lead264] While no fight is measured: the spend of one more mob's fight (FightSecOf one mob)
    NextFight oneMob;
    oneMob.mobs = 1;
    float const oneMobSec = FightSecOf(bot, oneMob);
    float reserve = rate * oneMobSec;
    if (PlayerbotAI::IsHeal(bot) && fightSec > 0.0f)
        reserve += perTankHealth * cost * oneMobSec / fightSec + upkeep * oneMobSec;
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
            reserve += perTankHealth * cost * uncertainSec / fightSec + upkeep * uncertainSec;
    }
    // [lead1400] b261 rfc-h 5B: the gate passed the holy paladin into Taragaman at 2 % (5B-2, lowest 2 %) and Jergosh
    // at 3 % (5B-2 / 5B-10, lowest 3-4 %) on 0 % lines: the line's bottom was 0 mana, where the game refuses every
    // heal (Spell::CheckPower: a cast needs its whole cost) until a return comes. A healer's boss line ends with the
    // mana of one cast of its heal (HealerThroughput's best rate heal, spell data) left: its lowest mana in the fight
    // is above zero by the line's own math, and every credit below covers it too (the potion verdict's line without
    // the potion asks for it once the pool falls under it)
    float healFloor = 0.0f;
    std::string healFloorWhy;
    if (boss && PlayerbotAI::IsHeal(bot))
    {
        HealerFacts const facts = HealerThroughput(bot, bot);
        healFloor = facts.castPct;
        if (healFloor > 0.0f)
            healFloorWhy = Acore::StringFormat(
                " + {:.1f} heal floor ({}'s cost) [lead1400], no credit covers it [lead1430]", healFloor, facts.spell);
        reserve += healFloor;
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
    // [lead525] coordinator 2026-11-03 (Q1): both costs as they are. A tool's cost is the damage it loses in the
    // fight (its cast / channel seconds x the member's damage share; Viper its damage cut while it runs); drinking's
    // cost is the wait it adds to the group's wait - nothing while the group waits for someone else anyway (the
    // slowest other member's drink to its own line). The cheaper one counts. Out of a fight only (in a fight there
    // is no drinking now: the plain drink seconds, as before); the other members' lines are read without this
    // overlap (one level, no recursion)
    float otherWaitSec = 0.0f;
    std::string waitFor;
    bool const overlap = drinkRate > 0.0f && !bot->IsInCombat() && !nestedManaLine;
    if (overlap)
        if (Group* group = bot->GetGroup())
        {
            nestedManaLine = true;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
                    member->IsInCombat() || !member->GetMaxPower(POWER_MANA))
                    continue;
                float const memberRate = DrinkPctPerSec(member);
                if (memberRate <= 0.0f)
                    continue;
                // [lead562] b214 rfc-h 5H 288-297.7 s: before Oggleflint the group waited 9 s for the fire mage's
                // boss line ("Rtmagad at 28 % mana (needs 70 %)") while the MM hunter, at 17 %, counted the mage's
                // trash line (34 %: ~2 s) as the wait it could drink in - its drink looked dearer than Viper in the
                // fight, it rested in Viper to 30 % and spent its four boss fights 12 times in Viper (5H MM 76 % of
                // wowsims, its acting rate 81 %). Before a boss the group waits for each member's boss line: the
                // other members' wait is to the same kind of line this one is read for
                float const otherLine =
                    boss ? FightManaLine(member, fightSec, 0.0f, nullptr, true) : OwnManaLine(member);
                float const sec = std::max(otherLine - member->GetPowerPct(POWER_MANA), 0.0f) / memberRate;
                if (sec > otherWaitSec)
                {
                    otherWaitSec = sec;
                    waitFor = member->GetName();
                }
            }
            nestedManaLine = false;
        }
    // [lead1333] user 2026-11-05 (Q70, delegated): the boss gate prices a drink only by the wait it adds beyond what
    // the group waits anyway - the other members' rest lines (above), the buff pass's casts still to give and the
    // ready check (answered on receipt: none). lead1328 (b256): the drink was read as the group's whole wait while
    // the buff pass ran beside it. The longer of the two waits is the group's (the pass's givers cast while the
    // sitters drink: they overlap)
    float rested = otherWaitSec;
    float passSec = 0.0f;
    if (overlap && boss)
    {
        passSec = GroupWaitSecOf(bot);
        if (passSec > otherWaitSec)
        {
            otherWaitSec = passSec;
            waitFor = "the buff pass";
        }
    }
    // the group wait the drink of `pct` adds (-1: no drink in the bags, the tool it is)
    auto const drinkCost = [&](float pct) -> float
    {
        if (drinkRate <= 0.0f)
            return -1.0f;
        if (!overlap)
            return pct / drinkRate;
        float const mana = bot->GetPowerPct(POWER_MANA);
        float const shortfall = std::max(need + reserve - returned, 0.0f);
        float const withTool = std::max(shortfall - pct - mana, 0.0f) / drinkRate;
        float const withoutTool = std::max(shortfall - mana, 0.0f) / drinkRate;
        return std::max(otherWaitSec, withoutTool) - std::max(otherWaitSec, withTool);
    };
    if (overlap && otherWaitSec > 0.0f)
        pick += Acore::StringFormat("the group waits {:.1f} s for {} anyway", otherWaitSec, waitFor);
    if (overlap && boss && (rested > 0.0f || passSec > 0.0f))  // [lead1333] its parts, for the triage
        pick += Acore::StringFormat("{}[lead1333] extra wait only: rests {:.1f} s, buff pass {:.1f} s, ready check 0 s",
                                    pick.empty() ? "" : "; ", rested, passSec);
    // [lead634] b218 rfc-h 5D 232.3-247.5 s: Oggleflint's pull held for "Rtmagac at 1 % mana (needs 60 %)" - its
    // Evocation (cast 162.1 s, 120 s recharge) was back at 282 s, 33 s into his 45 s fight (pulled 249.3 s; the leader
    // told it there at 282.7 s), and the line counted none of its 60 %: the mage drank 15 s. A cooldown that is back
    // within the fight counts like a potion's (lead502): the line then holds at least the mana it spends until then
    float cooldownFloor = 0.0f;
    std::vector<BurstReturn> bursts;  // [lead850] the returns on the fight's timeline
    float burstCredit = 0.0f;         // [lead850] their part of `returned`
    std::vector<ManaLineCredit> readyCredits;  // [lead1390] the credits counted ready now
    auto const credit = [&](uint32 spell, float pct, char const* name)
    {
        if (!credits || !bot->HasSpell(spell))
            return;
        uint32 readyMs = 0;
        if (bot->HasSpellCooldown(spell))
        {
            readyMs = bot->GetSpellCooldownDelay(spell);
            if (!readyMs || fightSec <= 0.0f || float(readyMs) >= fightSec * float(IN_MILLISECONDS))
                return;
        }
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        if (!info)
            return;
        // [lead1402] b262 rfc-h 5D-4 / 5H-8: the bear tanks' lines credited their own Innervate as ready now (the
        // leader's invariant check: 35 / 22 credit_unguaranteed, SPELL_FAILED_NOT_SHAPESHIFT) - Bear Form refuses it
        // (SpellInfo::CheckShapeshift) and a feral druid keeps its form through the fight (its rotation is the
        // form's). A tool its form refuses is not counted while it keeps that form
        if (ShapeshiftForm const form = bot->GetShapeshiftForm();
            form != FORM_NONE && bot->getClass() == CLASS_DRUID &&
            AiFactory::GetPlayerSpecTab(bot) == DRUID_TAB_FERAL && info->CheckShapeshift(form) != SPELL_CAST_OK)
        {
            pick += Acore::StringFormat("{}{} not counted [lead1402]: its form refuses it", pick.empty() ? "" : "; ",
                                        name);
            return;
        }
        // [lead1380] b260 rfc-h 5E-13 Bazzalan 1147-1189 s: the gate passed the holy priest at 10-15 % on a 0 % line
        // (Shadowfiend's 50 % "in the fight" against a heal term of 44 %). The leader told Shadowfiend on Bazzalan, the
        // priest stood at his heal reach 30 yd from the tank, out of its range: "leader external (Execute returned
        // false)" all fight (CanCastSpell passes SPELL_FAILED_OUT_OF_RANGE, the cast fails). The potion verdict's line
        // still counted Shadowfiend, so no potion came; he ended at 2 % with his heals held ("save mana x0.00").
        // Shadowfiend's summon needs a hostile unit in its range and sight (Spell::CheckCast). In a fight it counts
        // only while an enemy of the group is in its reach (ShadowfiendReach). A healer's boss line before the pull
        // does not count it: the leader places a healer within its heal reach of the tank (LEADER_HEAL_REACH_YD),
        // farther than Shadowfiend's range (spell data), so its cast in that fight is not certain
        if (spell == SPELL_SHADOWFIEND)
        {
            bool const inFight = bot->IsInCombat();
            bool const unreached = inFight ? !ShadowfiendReach(bot)
                                           : boss && PlayerbotAI::IsHeal(bot) &&
                                                 info->GetMaxRange(false, bot) < Leader::LEADER_HEAL_REACH_YD;
            if (unreached)
            {
                pick += Acore::StringFormat("{}{} not counted [lead1380]: {}", pick.empty() ? "" : "; ", name,
                                            inFight ? "no enemy in its range and sight"
                                                    : "its range is short of the healer's heal reach");
                return;
            }
        }
        // [lead990] b228 rfc-h 5H-8 163.9-302.1 s: the fire mage fought the trash at 1-19 % mana, its ready
        // Evocation "held for Oggleflint (his pull in 171 s, recharge 240 s)" 215 times - the leader's cooldown
        // verdict (cooldowns-boss-judged-like-pulls) - while its trash line counted Evocation's 60 % in the next fight
        // and so never had it drink. A trash line counts the member's own long mana cooldown only where that verdict
        // gives it on trash: no boss left, or its recharge after a cast (from when it is ready) is back before the
        // next boss pull (the leader's estimate, NoteBossPullSec)
        if (!boss && (spell == SPELL_EVOCATION || spell == SPELL_SHADOWFIEND))
            if (float const toBoss = BossPullSecOf(bot); toBoss >= 0.0f)
            {
                int32 rechargeMs = int32(std::max(info->RecoveryTime, info->CategoryRecoveryTime));
                bot->ApplySpellMod(spell, SPELLMOD_COOLDOWN, rechargeMs);
                float const backSec = (float(readyMs) + float(std::max(rechargeMs, 0))) / float(IN_MILLISECONDS);
                if (backSec > toBoss)
                {
                    pick += Acore::StringFormat("{}{} kept for the boss (back in {:.0f} s > his pull in {:.0f} s)",
                                                pick.empty() ? "" : "; ", name, backSec, toBoss);
                    return;
                }
            }
        float const castSec = info->IsChanneled() ? std::max(info->GetDuration(), 0) / 1000.0f
                                                  : std::max<uint32>(info->StartRecoveryTime, info->CalcCastTime(bot)) /
                                                        1000.0f;
        // [lead850] the cast's real cost: its seconds times this member's measured share of the group's damage (the
        // fight lasts that much longer), not one damage dealer's even share
        float const share = DamageShare(bot, DamageDealers(bot));
        float const fightCost = castSec * share;
        float const drinkSec = drinkCost(pct);  // [lead525]
        bool const inFight = drinkSec < 0.0f || fightCost < drinkSec;
        if (inFight)
        {
            returned += pct;
            if (!readyMs && pct > 0.0f)  // [lead1390]
                readyCredits.push_back({spell, 0, name});
            // [lead850] Evocation's return is on the fight's timeline (BurstLine): at the wowsims line once ready
            if (spell == SPELL_EVOCATION && info->IsChanneled())
            {
                BurstReturn evocation;
                evocation.readySec = float(readyMs) / float(IN_MILLISECONDS);
                evocation.pct = pct;
                evocation.channelSec = castSec;
                evocation.extendSec = fightCost;
                evocation.useAtPct = EvocationUseAtPct(bot);  // [lead1327]
                bursts.push_back(evocation);
                burstCredit += pct;
            }
            else if (readyMs)
                cooldownFloor = std::max(cooldownFloor, rate * float(readyMs) / float(IN_MILLISECONDS));
        }
        pick += Acore::StringFormat("{}{} {}: {:.1f} s channel / cast x {:.0f} % of the group's damage = {:.1f} s in "
                                    "the fight vs {:.1f} s drinking",
                                    pick.empty() ? "" : "; ", name, inFight ? "in the fight" : "drunk before", castSec,
                                    share * 100.0f, fightCost, drinkSec);
        if (readyMs)
            pick += Acore::StringFormat(" (ready in {:.0f} s: {:.0f} % until then)", float(readyMs) / 1000.0f,
                                        rate * float(readyMs) / float(IN_MILLISECONDS));
    };
    // [lead850] an Evocation channel under way: the ticks still to come are the member's mana now (b223 5C Taragaman
    // 671.8 s: "- 0 cooldowns" mid-channel, the potion verdict put a potion on it)
    if (credits)
        if (Spell* channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            channel && channel->getState() == SPELL_STATE_CASTING && channel->GetSpellInfo() &&
            channel->GetSpellInfo()->GetFirstRankSpell()->Id == SPELL_EVOCATION)
            if (int32 const durationMs = channel->GetSpellInfo()->GetDuration(); durationMs > 0)
            {
                float const leftSec = std::max(channel->GetCastTimeRemaining(), 0) / float(IN_MILLISECONDS);
                BurstReturn under;
                under.pct = ManaToolPct(bot, bot, SPELL_EVOCATION) * std::min(leftSec * 1000.0f / durationMs, 1.0f);
                under.channelSec = leftSec;
                under.extendSec = leftSec * DamageShare(bot, DamageDealers(bot));
                under.useAtPct = 100.0f;
                if (under.pct > 0.0f)
                {
                    bursts.push_back(under);
                    returned += under.pct;
                    burstCredit += under.pct;
                    pick += Acore::StringFormat("{}Evocation channelling: {:.0f} % in its {:.1f} s left",
                                                pick.empty() ? "" : "; ", under.pct, leftSec);
                }
            }
    credit(SPELL_EVOCATION, ManaToolPct(bot, bot, SPELL_EVOCATION), "Evocation");
    credit(SPELL_SHADOWFIEND, ManaToolPct(bot, bot, SPELL_SHADOWFIEND), "Shadowfiend");  // [lead423]
    // [lead733] the druid's own Innervate on itself (wowsims / Rawr's self-Innervate: cast at the bottom of the pool)
    // counts in its boss line like Evocation: the mana the spell returns to it (ManaToolPct, 225 % of its base mana
    // over 10 s), ready now or back within the fight (b219 rfc-h 5C: the balance druid's boss lines counted none -
    // "- 0 cooldowns"). A trash line does not: on trash the leader keeps it for the next boss unless it is back
    // before his pull (the cooldown verdict), which this line cannot know
    if (boss)
        credit(SPELL_INNERVATE, ManaToolPct(bot, bot, SPELL_INNERVATE), "Innervate");
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
    if (credits && bot->HasSpell(SPELL_ASPECT_OF_THE_VIPER) && fightSec > 0.0f)  // [lead425] its own credit too
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
            // [lead1328] b255 rfc-h 5H: the MM hunter pulled the bosses at 8-16 % ("- 175 cooldowns") and fought
            // 40-47 % of boss time in Viper (sim 0-13 %). Viper's damage cut was priced at one damage dealer's even
            // share; Evocation's channel at the member's measured share of the group's damage (lead850). Viper is
            // priced like it: the cut times this member's own share (DamageShare)
            float const viperCostPerPct = damageCut * DamageShare(bot, DamageDealers(bot)) / viperPerSec;
            // [lead726] b219 rfc-h: no hunter drank a mana potion in any of the 13 boss fights (itemUses {}), the
            // in-fight potion verdicts came at "the fight's 0-7 s left" (5H Rthunad "mana potion: 6 % < the 6 %
            // ... Aspect of the Viper in the fight: 0.9 s"), and the hunters fought their bosses in Viper at 6-15 %.
            // The verdict's line (the line without the potion) credited Viper with the whole shortfall, so the
            // potion was never needed while Viper could pay. A potion is off the global cooldown and costs no damage,
            // Viper cuts it: in a fight Viper covers only what the mana it has and a mana potion it can drink now
            // leave uncovered
            float coveredFirst = 0.0f;
            if (bot->IsInCombat() && bot->GetMaxPower(POWER_MANA))
            {
                uint32 potionAmount = 0;
                coveredFirst = bot->GetPowerPct(POWER_MANA);
                if (RaidConsumables::FindRestorePotion(bot, true, potionAmount))
                    coveredFirst += 100.0f * float(potionAmount) / float(bot->GetMaxPower(POWER_MANA));
            }
            float const viperPct =
                std::min(viperPerSec * fightSec, std::max(need + reserve - returned - coveredFirst, 0.0f));
            // [lead525] the whole shortfall Viper would cover: its damage lost vs the wait its drink adds
            float const viperCost = viperCostPerPct * viperPct;
            float const drinkSec = drinkCost(viperPct);
            bool const inFight = damageCut > 0.0f && viperPct > 0.0f && (drinkSec < 0.0f || viperCost < drinkSec);
            if (inFight)
                returned += viperPct;
            pick += Acore::StringFormat("{}Aspect of the Viper {}: {:.1f} s in the fight vs {:.1f} s drinking",
                                        pick.empty() ? "" : "; ", inFight ? "in the fight" : "drunk before",
                                        viperCost, drinkSec);
        }
    // [lead482] b209 rfc-h 5E: the prot paladin tank stood 257 s and drank 129 s ("53-56 % mana (needs 68 % = 4.07 %/s
    // (measured) x 10 s + 2") - lead480 took its in-fight potions out of the measured rate (b208 0.6 %/s had them
    // netted in) and nothing credited them back, while the leader's potion verdict gives one in every trash fight
    // under its need. A mana potion the game lets it drink in this fight counts like a ready cooldown: none drunk this
    // combat and the potions' cooldown ready (RaidConsumables::FindRestorePotion), at most the shortfall; not before a
    // boss pull whose pre-pull potion (the leader's, RaidConsumables::FindBuffPotion) takes the potions' cooldown
    // [lead502] b210 rfc-h 5E: still "paladin tank at 26-50 % mana (needs 62 % = 1.86 %/s x 33 s ... - 0 cooldowns)"
    // at 109-127 s, 15 s of loss - the potion it drank at 61.6 s was on the potions' cooldown, which runs from that
    // combat's end and was over 18 s into the next 33 s fight. A potion whose cooldown ends within the fight counts:
    // the line then holds at least the mana it spends until the potion is ready
    float potionFloor = 0.0f;
    float potionCredit = 0.0f;  // [lead1421]
    if (credits && potion && bot->GetMaxPower(POWER_MANA))
    {
        uint32 amount = 0;
        uint32 buffSpell = 0;
        uint32 readyMs = 0;
        // [lead520] b211 rfc-h 5H Oggleflint 336.5 s: the fire mage (11 %) and the MM hunter (31 %) drank their
        // pre-pull potions (Wild Magic / Speed) and the potions' cooldown then kept the in-fight Runic Mana Potion
        // from the fight they needed it in (the line jumped to 27 / 75 % after the pull was given). The pre-pull
        // potion is taken only when the member's mana covers the boss fight without the mana potion; otherwise the
        // mana potion is its potion for the fight (LeaderSupport TickPotions asks NeedsManaPotion, the same test)
        float const noPotionFloor = measured ? 0.0f : float(sPlayerbotAIConfig.LowMana);
        bool const prepot = boss && !bot->IsInCombat() && RaidConsumables::FindBuffPotion(bot, buffSpell) &&
                            bot->GetPowerPct(POWER_MANA) >= std::max(need + reserve - returned, noPotionFloor);
        uint32 const withinMs = fightSec > 0.0f ? uint32(fightSec * float(IN_MILLISECONDS)) : 0;
        uint32 const potionItem =
            prepot ? 0 : RaidConsumables::FindRestorePotion(bot, true, amount, withinMs, &readyMs);
        if (potionItem)
        {
            float const potionPct = std::min(100.0f * float(amount) / float(bot->GetMaxPower(POWER_MANA)),
                                             std::max(need + reserve - returned, 0.0f));
            returned += potionPct;
            potionCredit = potionPct;
            if (!readyMs && potionPct > 0.0f)  // [lead1390]
                readyCredits.push_back({0, potionItem, "mana potion"});
            // [lead850] on the fight timeline: its whole return once ready, where the pool has room for it
            if (potionPct > 0.0f)
            {
                BurstReturn drink;
                drink.readySec = float(readyMs) / float(IN_MILLISECONDS);
                drink.pct = 100.0f * float(amount) / float(bot->GetMaxPower(POWER_MANA));
                drink.fitsPool = true;
                bursts.push_back(drink);
                burstCredit += potionPct;
            }
            // [lead531] b212 rfc-h 5F 621.6-691 s: the boss gate passed the MM hunter at 17 % (Aspect of the Viper
            // covers the fight, its pre-pull potion the leader's); the pre-pull Potion of Speed put the potions on
            // their cooldown and this floor made the line "mana potion 0 % (ready in 60 s: 89 % until then)" - the
            // tank's walk to Taragaman held 70 s, the hunter drank for a line the leader had judged met. The floor is
            // the mana spent until the potion counted here is ready: a potion that covers nothing holds nothing
            if (potionPct > 0.0f)
                potionFloor = rate * float(readyMs) / float(IN_MILLISECONDS);
            pick += readyMs ? Acore::StringFormat("{}mana potion {:.0f} % (ready in {:.0f} s: {:.0f} % until then)",
                                                  pick.empty() ? "" : "; ", potionPct,
                                                  float(readyMs) / float(IN_MILLISECONDS), potionFloor)
                            : Acore::StringFormat("{}mana potion {:.0f} %", pick.empty() ? "" : "; ", potionPct);
        }
    }
    // [lead520] a mana gem (RaidConsumables::FindManaGem: its own cooldown, no potion slot) in a boss fight: the leader
    // keeps it for the bosses (TickPotions, the cooldown rule) - it counts like the potion, at most the shortfall
    if (credits && potion && boss && bot->GetMaxPower(POWER_MANA))
    {
        uint32 amount = 0;
        if (uint32 const gemItem = RaidConsumables::FindManaGem(bot, amount))
        {
            float const gemPct = std::min(100.0f * float(amount) / float(bot->GetMaxPower(POWER_MANA)),
                                          std::max(need + reserve - returned, 0.0f));
            returned += gemPct;
            if (gemPct > 0.0f)  // [lead1390]
                readyCredits.push_back({0, gemItem, "mana gem"});
            if (gemPct > 0.0f)  // [lead850] on the fight timeline, like the potion (ready now: FindManaGem)
            {
                BurstReturn gem;
                gem.pct = 100.0f * float(amount) / float(bot->GetMaxPower(POWER_MANA));
                gem.fitsPool = true;
                bursts.push_back(gem);
                burstCredit += gemPct;
            }
            pick += Acore::StringFormat("{}mana gem {:.0f} %", pick.empty() ? "" : "; ", gemPct);
        }
    }
    // [lead1390] the member's own line with its credits: what it counted ready now, for the leader's invariant check
    if (credits && !nestedManaLine)
    {
        std::lock_guard<std::mutex> lock(manaCreditLock);
        manaLineCredits[bot->GetGUID().GetRawValue()] = readyCredits;
        // [lead1421] the boss gate's line (a boss line out of combat with the potion): the potion it counted on
        if (boss && potion && !bot->IsInCombat())
            bossPotionCredits[bot->GetGUID().GetRawValue()] = potionCredit;
    }
    // [lead251] b191 rfc-h 5E: the prot paladin tank rested 15 times "mana 6-14 % < need 15 %" with its computed
    // need 2-12 % - the config's LowMana as the floor (80 s of its drinks); b192's floor at its mean deepest drop in a
    // fight (lead248) went over the computed need in 104 of 152 lines (5E tank 24 % against 4-13 %, the frost mage 47 %
    // against -34 % with Evocation), drinking 5C 75 -> 160 s, 5B 3 -> 87 s. A measured rate is the fight's own need:
    // no floor over it; the low line guards only the role's estimate (nothing measured yet)
    float const floorPct = measured ? 0.0f : float(sPlayerbotAIConfig.LowMana);
    float lineRaw = std::max({need + reserve - returned, potionFloor, cooldownFloor});  // [lead502] [lead634]
    // [lead1430] b263 rfc-h 5B-2 t=1104: the gate passed the holy paladin into Bazzalan at 1 % on a 0 % line ("0.00
    // %/s x 44 s + 8 reserve (+ 7.1 heal floor) - 22 cooldowns (mana potion 22 %)": heals 10.5 % x 1.30 cost), his
    // Taragaman lines the same (Divine Plea's 25 % against 18 + 9). The credits are sized to the shortfall with the
    // floor in it, so they cancelled the floor lead1400 put into the reserve, and the fight began at 1 % - under one
    // cast of Holy Light (Spell::CheckPower) until a return came. The floor is the pool's bottom by the line's math:
    // no credit covers it. A healer's boss line is the floor plus the cost its credits do not cover, the mana spent
    // until a credited potion or cooldown is ready on top of the floor too
    if (healFloor > 0.0f)
        lineRaw = std::max({lineRaw, healFloor, potionFloor + healFloor, cooldownFloor + healFloor});
    // [lead850] with a return on the fight timeline (Evocation, the mana potion, the gem): the lowest start mana its
    // timeline lasts the fight with, the other tools' mana (spread over the fight) taken off the spend rate. Not even
    // a full pool lasting: the plain sum (the clamp makes it 100 %). The timeline only raises the sum's line
    // (safety-over-speed): it lowers it by the channel's seconds without a cast, and the rate it would lower it on is
    // the trash-measured one until a boss is (b223 5D: 1.44 %/s counted, ~2.6 %/s spent on Oggleflint)
    // [lead855] a damage caster's boss line on its rotation's nonstop rate (no boss measured) or on a boss fight's
    // measured rate reads the boss's own spend: the timeline is the honest line there (the channel's seconds spend
    // nothing, the potion and the gem come where the pool has room), not only a raise. The other lines (a trash
    // fight, a healer, a member whose boss line still reads a trash rate) keep the raise-only rule. A cooldown back
    // within the fight (lead634: Shadowfiend, Innervate - spread over the fight on the timeline) still floors it
    bool const honestRate = boss && !PlayerbotAI::IsHeal(bot) && (rotation >= 0.0f || bossMeasured);
    std::string timeline;
    if (!bursts.empty() && fightSec > 0.0f)
    {
        float const spread = std::max(need - (returned - burstCredit), 0.0f) / fightSec;
        float const burstLine = BurstLine(spread, fightSec, reserve, bursts);
        float const sumLine = lineRaw;
        float const timedLine = burstLine >= 0.0f ? burstLine : 100.0f;
        lineRaw = honestRate ? std::max(timedLine, cooldownFloor) : std::max(lineRaw, timedLine);
        // the timeline raised the line: logged once per member and fight length (a log, no decision)
        thread_local std::set<std::pair<uint64, int32>> raisedLogged;
        if (lineRaw > sumLine + 0.5f && raisedLogged.insert({bot->GetGUID().GetRawValue(), int32(fightSec)}).second)
            LOG_INFO("playerbots",
                     "[lead850] {}: the fight's timeline raises its mana line {:.0f} -> {:.0f} % ({:.0f} s at {:.2f} "
                     "%/s, {} returns)",
                     bot->GetName(), sumLine, lineRaw, fightSec, spread, bursts.size());
        thread_local std::set<std::pair<uint64, int32>> loweredLogged;
        if (lineRaw < sumLine - 0.5f && loweredLogged.insert({bot->GetGUID().GetRawValue(), int32(fightSec)}).second)
            LOG_INFO("playerbots",
                     "[lead855] {}: the boss fight's timeline is its mana line {:.0f} -> {:.0f} % ({:.0f} s at {:.2f} "
                     "%/s, {} returns, {})",
                     bot->GetName(), sumLine, lineRaw, fightSec, spread, bursts.size(), source);
        timeline = burstLine >= 0.0f
                       ? Acore::StringFormat("; on the fight's timeline {:.0f} % at {:.2f} %/s", burstLine, spread)
                       : Acore::StringFormat("; on the fight's timeline not even a full pool at {:.2f} %/s", spread);
    }
    // [lead1182] user 2026-11-04 (Q63): the group does not stand while a damage caster drinks when its kill does not
    // need that mana. The two costs lead525 weighs for a tool, for the shortfall itself: drinking it before the pull
    // costs the group the wait it adds (nothing while it waits for someone else anyway); pulling without it costs the
    // fight the rotation seconds that mana pays (shortfall over its spend rate), dealt by the others at the group's
    // damage without its share - longer by those seconds x share / (1 - share). The cheaper one counts: pulled short,
    // its line is the mana it has. Safety first: only a damage dealer (no healer, no tank: their mana is the fight's
    // whole length), not a hunter (Aspect of the Viper is its dry rotation, credited above) nor a warlock (Life Tap,
    // the leader's way), and only while every healer's and tank's mana covers the longer fight (their own lines for
    // its seconds). Its share is never below an even one of the damage dealers' (a share measured while it was dry
    // would make its mana look worth less)
    std::string dryWhy;
    if (credits && potion && drinkRate > 0.0f && rate > 0.0f && fightSec > 0.0f && !bot->IsInCombat() &&
        bot->GetMaxPower(POWER_MANA) && !PlayerbotAI::IsHeal(bot) && !PlayerbotAI::IsTank(bot) &&
        bot->getClass() != CLASS_HUNTER && bot->getClass() != CLASS_WARLOCK)
    {
        float const mana = bot->GetPowerPct(POWER_MANA);
        float const shortPct = std::min(lineRaw, 100.0f) - mana;
        uint32 const dealers = DamageDealers(bot);
        float const share = std::max(DamageShare(bot, dealers), 1.0f / float(dealers));
        if (shortPct > 0.0f && share < 1.0f)
        {
            float drySec = std::min(shortPct / rate, fightSec);
            float const extendSec = drySec * share / (1.0f - share);
            // [lead1200] b243 rfc-h 5B elemental shaman 75 % of the sim (b242 86-95 %): 19 boss lines pulled short
            // (100 -> 1-15 %, "36-43 s of its rotation x 25-28 % share = the fight 12-17 s longer vs 16-25 s of the
            // group's wait drinking"); 5B-10 Jergosh at 8 %: 7 Lightning Bolts in 39 s, busy 34 %, "not enough
            // power (64 of 702)". A boss fight is judged by the member's own boss damage against the sim's (wowsims
            // starts every boss at full mana): on a boss its dry seconds cost that damage whole, over the boss
            // fight's full length (its seconds plus the longer seconds it stays dry in), not diluted by its share
            // of the group's dps. The boss line is pulled short only when those seconds are fewer than the group's
            // wait its drink adds; trash keeps the group's cost (lead1182)
            if (boss)
                drySec = std::min(shortPct / rate, fightSec + extendSec);
            float const costSec = boss ? drySec : extendSec;
            float const ownDrinkSec = shortPct / drinkRate;
            float const drinkWaitSec =
                overlap ? std::max(otherWaitSec, ownDrinkSec) - otherWaitSec : ownDrinkSec;
            if (boss && extendSec < drinkWaitSec && costSec >= drinkWaitSec)
            {
                thread_local std::set<std::pair<uint64, int32>> bossDryLogged;
                if (bossDryLogged.size() > 4096)
                    bossDryLogged.clear();
                if (!nestedManaLine && bossDryLogged.insert({bot->GetGUID().GetRawValue(), int32(fightSec)}).second)
                    LOG_INFO("playerbots",
                             "[lead1200] {}: its boss line is not pulled short ({:.0f} -> {:.0f} %): {:.1f} s of its "
                             "boss rotation dry >= {:.1f} s of the group's wait drinking",
                             bot->GetName(), lineRaw, mana, drySec, drinkWaitSec);
            }
            if (costSec < drinkWaitSec)
            {
                std::string uncovered;
                if (Group* group = bot->GetGroup())
                {
                    bool const wasNested = nestedManaLine;
                    nestedManaLine = true;
                    float const longerSec = fightSec + extendSec;
                    for (GroupReference* ref = group->GetFirstMember(); ref && uncovered.empty(); ref = ref->next())
                    {
                        Player* member = ref->GetSource();
                        if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
                            !member->GetMaxPower(POWER_MANA) || member->getPowerType() != POWER_MANA ||
                            (!PlayerbotAI::IsHeal(member) && !PlayerbotAI::IsTank(member)))
                            continue;
                        float const needLonger =
                            FightManaLine(member, longerSec, cost * longerSec / fightSec, nullptr, boss);
                        if (member->GetPowerPct(POWER_MANA) < needLonger)
                            uncovered = Acore::StringFormat("{} at {:.0f} % < {:.0f} % for {:.0f} s", member->GetName(),
                                                            member->GetPowerPct(POWER_MANA), needLonger, longerSec);
                    }
                    nestedManaLine = wasNested;
                }
                dryWhy = Acore::StringFormat(
                    "{:.0f} % short: {:.1f} s of its rotation x {:.0f} % share = the fight {:.1f} s longer vs {:.1f} s "
                    "of the group's wait drinking",
                    shortPct, drySec, share * 100.0f, extendSec, drinkWaitSec);
                if (uncovered.empty())
                {
                    thread_local std::set<std::tuple<uint64, int32, bool>> dryLogged;
                    if (dryLogged.size() > 4096)
                        dryLogged.clear();
                    if (!nestedManaLine &&
                        dryLogged.insert({bot->GetGUID().GetRawValue(), int32(fightSec), boss}).second)
                        LOG_INFO("playerbots", "[lead1182] {}: pulled short of its {} line {:.0f} -> {:.0f} % ({})",
                                 bot->GetName(), boss ? "boss" : "trash", lineRaw, mana, dryWhy);
                    lineRaw = mana;
                    dryWhy = "pulled short [lead1182]: " + dryWhy;
                }
                else
                    dryWhy = "drinks [lead1182]: " + dryWhy + "; not covered: " + uncovered;
            }
        }
    }
    if (boss && credits && potion && !nestedManaLine && !bot->IsInCombat())
    {
        std::lock_guard<std::mutex> lock(manaRateLock);
        bossPulledShort[bot->GetGUID().GetRawValue()] = dryWhy.rfind("pulled short", 0) == 0;
    }
    float const line = std::clamp(lineRaw, floorPct, 100.0f);
    if (why && !dryWhy.empty())
        timeline += "; " + dryWhy;
    if (why)
        *why = Acore::StringFormat("{:.0f} % = {:.2f} %/s ({}) x {:.0f} s + {:.0f} reserve ({}) - {:.0f} cooldowns{}",
                                   line, rate, measured ? source : "spell data", fightSec, reserve,
                                   (uncertainSec >= 0.0f && measured
                                        ? Acore::StringFormat("x {:.1f} s uncertainty", uncertainSec)
                                        : Acore::StringFormat("one more mob, {:.1f} s", oneMobSec)) +
                                       healFloorWhy,
                                   returned, pick.empty() ? std::string() : " (" + pick + ")");
    if (why)
        *why += timeline;
    if (why && (rotation >= 0.0f || (nonstop >= 0.0f && rate == nonstop)))  // [lead855] [lead1162]
        *why += "; its rotation " + rotationWhy;
    if (why && perTankHealth > 0.0f)  // [lead258]
        *why += Acore::StringFormat("; heals {:.1f} % per tank health ({}) x {:.2f} cost{}", perTankHealth,
                                    damageCost        ? "per tank health healed [lead1294]"
                                    : perCostMeasured ? "measured"
                                                      : "throughput",
                                    cost,
                                    upkeep > 0.0f ? Acore::StringFormat(" + upkeep {:.3f} %/s [lead1431]", upkeep)
                                                  : std::string());
    return line;
}

bool NeedsManaPotion(Player* bot, float fightSec, float cost)
{
    return bot && bot->GetMaxPower(POWER_MANA) &&
           bot->GetPowerPct(POWER_MANA) <
               FightManaLine(bot, fightSec, cost, nullptr, true, true, false, cost > 0.0f);  // [lead1362]
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
    // [lead467] b207 rfc-h: the tank sat on to full health / mana under the leader's issued pull order - its pull
    // started 12.6 s (5B warrior, eating to 100 % health), 19.1 s and 25.8 s (5E paladin, drinking to 100 % mana)
    // after the issue (pull_trace ask): 5E 662-682 s "pull_order: walks in ... [Taragaman]" 20 s of standing. The
    // leader decided the group is ready when it issued the order: the tank whose order is open stands up
    if (Leader::LedBot(bot) && Leader::Orders::IsOpen(bot->GetGUID()))
        return true;

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
