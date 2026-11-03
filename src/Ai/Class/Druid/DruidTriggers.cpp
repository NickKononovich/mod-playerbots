/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DruidTriggers.h"
#include "DruidActions.h"
#include "DruidCatActions.h"  // [rot64] CatIsBehind
#include "EstimatedLifetimeValue.h"  // [rot71] ExpectedLifetime
#include "Creature.h"
#include "DynamicObject.h"
#include "InstanceScript.h"
#include "LeaderCommandState.h"  // [rot2] [rot3]
#include "LeaderOrders.h"        // [rot10] Leader::ExecutorMember
#include "Log.h"  // [rot1]
#include "Player.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "StringFormat.h"  // [rot51]
#include "TargetValue.h"
#include "HealthTriggers.h"  // [b32_tank_aoe] RestState
#include "Timer.h"           // [b32_tank_aoe] getMSTime
#include "FeralDruidStrategy.h"  // [b32_brez] DruidHasThickHide
#include "Group.h"               // [b32_brez]
#include "SpellMgr.h"            // [b32_brez]
#include "TankAoeThreat.h"       // [b32_brez] TankPeel, TankAoePackTrigger

namespace
{
    // [rot1] The bot or a member of its group on the bot's map is in combat (the non-combat engine also runs between
    // the mobs of a pull).
    bool DruidGroupInCombat(Player* bot)
    {
        if (bot->IsInCombat())
            return true;

        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsAlive() && member->GetMap() == bot->GetMap() &&
                member->IsInCombat())
                return true;
        }

        return false;
    }

    // [rot1] Mark of the Wild only when it is missing (no Mark, no Gift): a refresh before it runs out is a GCD and
    // mana for an hour-long buff.
    bool MissingMark(PlayerbotAI* botAI, Unit* target)
    {
        return target && !botAI->HasAura("mark of the wild", target) && !botAI->HasAura("gift of the wild", target);
    }

    // [rot2] a led druid buffs only under the leader's Prepare / Ready? / Rest (the leader times the buffs)
    bool LeaderAllowsBuffs(Player* bot)
    {
        Leader::MemberEvents command;
        LeaderCommandState::State const state = LeaderCommandState::Read(bot, command);
        if (state == LeaderCommandState::State::NotLed)
            return true;

        // [rot10] the led tank is no executor: the leader sends it no Prepare / Ready? / Rest (only its pull order),
        // so it buffs on its own rule (run 20261101-171156-rfc-h-5D-4: the bear never marked the DK, the pull held
        // for that Mark)
        if (!Leader::ExecutorMember(bot))
            return true;

        // [rot3] a stale command: no buffs of its own
        return state == LeaderCommandState::State::Fresh &&
               (command.ready == Leader::ReadyEvent::Prepare || command.ready == Leader::ReadyEvent::ReadyCheck ||
                command.ready == Leader::ReadyEvent::Rest);
    }
}

// [rot30]
bool InsectSwarmMissingTrigger::IsActive()
{
    if (DruidHasEclipseTalent(bot) && DruidLunarProcReady(context, bot))
        return false;

    return DruidDotMissingTrigger::IsActive();
}

// [rot1] Out of combat only (and the group out of combat) and only when missing.
bool MarkOfTheWildTrigger::IsActive()
{
    Unit* target = GetTarget();
    return MissingMark(botAI, target) && !DruidGroupInCombat(bot) && LeaderAllowsBuffs(bot) && BuffTrigger::IsActive();
}

bool MarkOfTheWildOnPartyTrigger::IsActive()
{
    Unit* target = GetTarget();
    return MissingMark(botAI, target) && !target->IsInCombat() && !DruidGroupInCombat(bot) && LeaderAllowsBuffs(bot) &&
           BuffOnPartyTrigger::IsActive();
}

bool DruidGroupNeedsHeal(Player* bot, bool* healerAlive)
{
    bool low = false;
    bool healer = false;
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsAlive() || !member->IsInWorld() || member->GetMap() != bot->GetMap())
                continue;

            if (member != bot && PlayerbotAI::IsHeal(member) && bot->IsWithinDistInMap(member, 60.0f))
                healer = true;
            if (member->GetHealthPct() < BALANCE_HEAL_PCT)
                low = true;
        }
    }
    else
        low = bot->GetHealthPct() < BALANCE_HEAL_PCT;

    if (healerAlive)
        *healerAlive = healer;
    return low;
}

bool MoonkinFormUpkeepTrigger::IsActive()
{
    // flags and one aura read first, the group pass last
    if (!bot->IsAlive() || !bot->HasSpell(SPELL_MOONKIN_FORM))
        return false;

    uint32 const now = getMSTime();
    if (bot->HasAura(SPELL_MOONKIN_FORM))
    {
        _inFormMs = now ? now : 1;
        return false;
    }

    if (_inFormMs && getMSTimeDiff(_inFormMs, now) < LEFT_FORM_GRACE_MS)
        return false;

    if (bot->IsMounted() || bot->IsInFlight() || bot->IsNonMeleeSpellCast(false, true, true))
        return false;

    if (botAI->HasAnyAuraOf(bot, "travel form", "aquatic form", "flight form", "swift flight form", nullptr))
        return false;

    if (bot->IsSitState() || RestState::IsDrinking(bot) || RestState::IsEating(bot))
        return false;

    if (!bot->IsInCombat() && (RestState::WantsDrink(bot) || RestState::WantsFood(bot)))
        return false;

    if (!bot->IsInCombat() && DruidGroupNeedsHeal(bot))
        return false;

    if (!_loggedMs || getMSTimeDiff(_loggedMs, now) >= LOG_MS)
    {
        _loggedMs = now ? now : 1;
        LOG_INFO("playerbots", "[rot1] [moonkin upkeep] {} back to Moonkin Form ({}, out of form {} ms)",
                 bot->GetName(), bot->IsInCombat() ? "combat" : "no combat",
                 _inFormMs ? getMSTimeDiff(_inFormMs, now) : 0u);
    }

    return true;
}

bool ThornsOnPartyTrigger::IsActive()
{
    return BuffOnPartyTrigger::IsActive() && !botAI->HasAura("thorns", GetTarget());
}

bool EntanglingRootsKiteTrigger::IsActive()
{
    return DebuffTrigger::IsActive() && AI_VALUE(uint8, "attacker count") < 3 && !GetTarget()->GetPower(POWER_MANA);
}

bool ThornsTrigger::IsActive() { return BuffTrigger::IsActive() && !botAI->HasAura("thorns", GetTarget()); }

bool BearFormTrigger::IsActive() { return !botAI->HasAnyAuraOf(bot, "bear form", "dire bear form", nullptr); }

bool BearTankFormTrigger::IsActive()
{
    // [b32_tank_aoe] cheap flags first
    if (bot->IsInCombat() || !bot->IsAlive() || bot->IsMounted() || bot->IsInWater() || bot->IsInFlight())
        return false;

    uint32 const now = getMSTime();
    if (botAI->HasAnyAuraOf(bot, "dire bear form", "bear form", nullptr))
    {
        _inFormMs = now;
        return false;
    }

    // left the form for a buff / heal a moment ago: let that sequence finish
    if (_inFormMs && getMSTimeDiff(_inFormMs, now) < LEFT_FORM_GRACE_MS)
        return false;

    if (!PlayerbotAI::IsTank(bot) || !botAI->HasStrategy("bear", BOT_STATE_COMBAT))
        return false;

    // sitting with food / drink, or about to: the rest flow keeps caster form (drinking needs it)
    if (bot->IsSitState() || RestState::IsDrinking(bot) || RestState::IsEating(bot))
        return false;

    return !RestState::WantsDrink(bot) && !RestState::WantsFood(bot);
}

bool TreeFormTrigger::IsActive()
{
    return !bot->HasAura(SPELL_TREE_OF_LIFE);
}

bool CatFormTrigger::IsActive() { return !botAI->HasAura("cat form", bot); }

bool HealerShouldAttackAndNotBlanketingTrigger::IsActive()
{
    if (bot->GetGroup() && bot->HasSpell(SPELL_TREE_OF_LIFE) && botAI->HasStrategy("blanketing", BOT_STATE_COMBAT))
        return false;

    return HealerShouldAttackTrigger::IsActive();
}

Unit* BlanketHotTrigger::GetTarget() { return botAI->GetUnit(AI_VALUE2(ObjectGuid, "blanket hot target", _spell)); }

bool BlanketHotTrigger::IsActive()
{
    if (!bot->GetGroup() || !botAI->HasStrategy("resto", BOT_STATE_COMBAT))
        return false;

    return GetTarget();
}

bool AquaticFormTrigger::IsActive()
{
    return !bot->IsInCombat() && !botAI->HasAura("aquatic form", bot) &&
           bot->GetLiquidData().Status == LIQUID_MAP_UNDER_WATER;
}

bool RebirthTargetLowHealthTrigger::IsActive()
{
    Unit* target = RebirthFollowUp::GetTarget(botAI);
    return target && target->GetHealthPct() < RebirthFollowUp::SAFE_HEALTH_PCT;
}

bool RebirthTargetNoMarkTrigger::IsActive()
{
    Unit* target = RebirthFollowUp::GetTarget(botAI);
    return target && !botAI->HasAura("mark of the wild", target) && !botAI->HasAura("gift of the wild", target);
}

bool ProwlTrigger::IsActive()
{
    if (botAI->HasAura("prowl", bot) || bot->IsInCombat())
        return false;

    if (!botAI->HasSpell("prowl"))
        return false;

    uint32 const prowlId = AI_VALUE2(uint32, "spell id", "prowl");
    if (bot->HasSpellCooldown(prowlId))
        return false;

    float distance = 30.f;

    Unit* target = AI_VALUE(Unit*, "enemy player target");
    if (target && !target->IsInWorld())
        return false;
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

const std::set<uint32> HurricaneChannelCheckTrigger::HURRICANE_SPELL_IDS = {
    16914,  // Hurricane Rank 1
    17401,  // Hurricane Rank 2
    17402,  // Hurricane Rank 3
    27012,  // Hurricane Rank 4
    48467   // Hurricane Rank 5
};

bool HurricaneChannelCheckTrigger::IsActive()
{
    if (Spell* spell = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
    {
        if (!HURRICANE_SPELL_IDS.count(spell->m_spellInfo->Id))
            return false;

        // Find this bot's own Hurricane DynamicObject
        DynamicObject* dynObj = nullptr;
        for (uint32 spellId : HURRICANE_SPELL_IDS)
        {
            dynObj = bot->GetDynObject(spellId);
            if (dynObj)
                break;
        }

        if (!dynObj)
            return false;

        // Count attackers actually inside the Hurricane AoE
        return !HasEnoughAoeTargets(botAI, dynObj->GetPosition(), dynObj->GetRadius(), minEnemies);
    }

    return false;
}

bool LifebloomOnMainTankTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target || !target->IsAlive() || !target->IsInCombat())
        return false;

    Aura* lifebloom = botAI->GetAura("lifebloom", target, true, true);
    if (LifebloomLetBloom(bot, target, lifebloom))  // [b31_sd]
        return false;

    return !lifebloom || lifebloom->GetStackAmount() < 3 || lifebloom->GetDuration() < LIFEBLOOM_REFRESH_WINDOW_MS;
}

bool LifebloomLetBloom(Player* bot, Unit* tank, Aura* lifebloom)
{
    if (!lifebloom || !tank || tank->GetHealthPct() < LIFEBLOOM_BLOOM_TANK_HEALTH_PCT)
        return false;

    if (InstanceScript* instance = bot->GetInstanceScript())
        if (instance->IsEncounterInProgress())
            return false;

    Creature* victim = tank->GetVictim() ? tank->GetVictim()->ToCreature() : nullptr;
    return !victim || !(victim->IsDungeonBoss() || victim->isWorldBoss());
}

bool RejuvenationOnMainTankTrigger::IsActive()
{
    Unit* target = GetTarget();
    return target && target->IsAlive() && target->IsInCombat() && BuffOnMainTankTrigger::IsActive();
}

// ---- [b32_brez] bear tank battle res ---------------------------------------------------------------------------

namespace
{
    BearBattleResTargetValue* BearBattleResValue(PlayerbotAI* botAI)
    {
        return dynamic_cast<BearBattleResTargetValue*>(
            botAI->GetAiObjectContext()->GetValue<Unit*>("bear battle res target"));
    }

    bool InBearForm(PlayerbotAI* botAI, Player* bot)
    {
        return botAI->HasAnyAuraOf(bot, "dire bear form", "bear form", nullptr);
    }

    // Another druid's Rebirth, read from its spell book (not its AI context): its highest rank known and ready.
    bool KnownRebirthReady(Player* druid)
    {
        constexpr uint32 SPELL_REBIRTH_RANK_1 = 20484;
        uint32 known = 0;
        for (uint32 spellId = SPELL_REBIRTH_RANK_1; spellId; spellId = sSpellMgr->GetNextSpellInChain(spellId))
        {
            if (druid->HasSpell(spellId))
                known = spellId;
        }

        return known && !druid->HasSpellCooldown(known);
    }
}  // namespace

void BearBattleRes::RecordAttempt(PlayerbotAI* botAI, Unit* target)
{
    if (BearBattleResTargetValue* value = BearBattleResValue(botAI))
        value->RecordAttempt(target ? target->GetGUID() : ObjectGuid::Empty, getMSTime());
}

uint32 BearBattleRes::LastAttemptMs(PlayerbotAI* botAI)
{
    BearBattleResTargetValue const* value = BearBattleResValue(botAI);
    return value ? value->LastAttemptMs() : 0;
}

void BearBattleResTargetValue::RecordAttempt(ObjectGuid guid, uint32 now)
{
    _lastAttemptMs = now;
    if (!guid.IsEmpty())
        _attempts[guid] = now;

    // the next scan sees the attempt at once (no second cast on the same corpse from the cached answer)
    Reset();
}

bool BearBattleResTargetValue::AttemptedRecently(ObjectGuid guid, uint32 now) const
{
    auto const attempt = _attempts.find(guid);
    return attempt != _attempts.end() && getMSTimeDiff(attempt->second, now) < BearBattleRes::RETRY_MS;
}

bool BearBattleResTargetValue::CanAffordCast(uint32 rebirthId) const
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(rebirthId);
    if (!spellInfo)
        return false;

    // CanCastSpell ignores power and reagents (TRIGGERED_IGNORE_POWER_AND_REAGENT_COST): leaving the form for a cast
    // that then fails on them only costs the tank its form. The mana covers the shift back as well: a bear that
    // cannot pay for Dire Bear Form after Rebirth stays in caster form under its pack.
    int32 cost = spellInfo->CalcPowerCost(bot, spellInfo->GetSchoolMask());
    uint32 formId = AI_VALUE2(uint32, "spell id", "dire bear form");
    if (!formId)
        formId = AI_VALUE2(uint32, "spell id", "bear form");
    if (SpellInfo const* formInfo = formId ? sSpellMgr->GetSpellInfo(formId) : nullptr)
        cost += formInfo->CalcPowerCost(bot, formInfo->GetSchoolMask());
    if (static_cast<int32>(bot->GetPower(POWER_MANA)) < cost)
        return false;

    if (bot->CanNoReagentCast(spellInfo))
        return true;

    for (uint32 index = 0; index < MAX_SPELL_REAGENTS; ++index)
    {
        if (spellInfo->Reagent[index] > 0 &&
            !bot->HasItemCount(static_cast<uint32>(spellInfo->Reagent[index]), spellInfo->ReagentCount[index]))
            return false;
    }

    return true;
}

bool BearBattleResTargetValue::PackHeld() const
{
    uint32 const onBear = static_cast<uint32>(bot->getAttackers().size());
    float const floor = bot->GetLevel() * TankAoePackTrigger::SAFE_FLOOR_PER_LEVEL;
    bool held = true;
    GuidVector const attackers = AI_VALUE(GuidVector, "attackers");
    for (ObjectGuid const& guid : attackers)
    {
        Unit* mob = botAI->GetUnit(guid);
        if (!mob || !mob->IsAlive() || mob->IsPlayer() || !mob->IsInWorld() || mob->GetMap() != bot->GetMap())
            continue;

        // a boss mid-cast: its hit lands while the bear is out of form (any range: a boss cast reaches the tank)
        if (TankPeel::IsBoss(mob) &&
            (mob->GetCurrentSpell(CURRENT_GENERIC_SPELL) || mob->GetCurrentSpell(CURRENT_CHANNELED_SPELL)))
            return false;

        if (!held || !bot->IsWithinDistInMap(mob, TankPeel::ANCHOR_RANGE) || IsCrowdControlled(mob))
            continue;

        Unit* victim = mob->GetVictim();
        if (victim && victim != bot)
        {
            held = false;
            continue;
        }

        if (!mob->CanHaveThreatList())
            continue;

        float mine = 0.0f;
        float others = 0.0f;
        for (ThreatReference const* ref : mob->GetThreatMgr().GetUnsortedThreatList())
        {
            if (ref->GetVictim() == bot)
                mine = ref->GetThreat();
            else
                others = std::max(others, ref->GetThreat());
        }

        if (mine < floor || mine < others * TankAoePackTrigger::SAFE_LEAD)
            held = false;
    }

    return onBear <= BearBattleRes::MAX_ATTACKERS || held;
}

Unit* BearBattleResTargetValue::Calculate()
{
    // cheap flags first
    if (!bot->IsInCombat() || !bot->IsAlive() || bot->getClass() != CLASS_DRUID || !PlayerbotAI::IsTank(bot))
        return nullptr;

    if (bot->GetHealthPct() <= BearBattleRes::MIN_HEALTH_PCT ||
        AI_VALUE(float, "recent health loss") >= sPlayerbotAIConfig.TankCooldownLossPct)
        return nullptr;

    Group* group = bot->GetGroup();
    uint32 const rebirthId = AI_VALUE2(uint32, "spell id", "rebirth");
    if (!group || !rebirthId || !bot->HasSpell(rebirthId) || bot->HasSpellCooldown(rebirthId) ||
        !CanAffordCast(rebirthId))
        return nullptr;

    uint32 const now = getMSTime();
    Player* healer = nullptr;
    Player* valuable = nullptr;
    bool healerDead = false;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || member->GetMap() != bot->GetMap())
            continue;

        if (member->IsAlive())
        {
            // Another druid rezzes first ("rebirth" nodes of a resto / balance / cat druid, GenericDruidStrategy;
            // the "battle res target" queue puts the tanks last): the bear keeps its pack.
            if (member->getClass() == CLASS_DRUID && GET_PLAYERBOT_AI(member) && !PlayerbotAI::IsTank(member) &&
                !DruidHasThickHide(member))
            {
                if (KnownRebirthReady(member))
                    return nullptr;
            }

            continue;
        }

        // as the "battle res target" queue: Soulstone / Reincarnation holders and pending requests are handled
        if (member->getDeathState() != DeathState::Corpse || member->isResurrectRequested() ||
            member->GetUInt32Value(PLAYER_SELF_RES_SPELL))
            continue;

        bool const isHealer = PlayerbotAI::IsHeal(member);
        healerDead = healerDead || isHealer;
        if (!isHealer && !PlayerbotAI::IsTank(member) && !IsRealPlayer(member))
            continue;

        if (AttemptedRecently(member->GetGUID(), now) || !bot->IsWithinDistInMap(member, BearBattleRes::RANGE) ||
            !bot->IsWithinLOSInMap(member))
            continue;

        Player*& pick = isHealer ? healer : valuable;
        if (!pick || member->GetGUID() < pick->GetGUID())
            pick = member;
    }

    // another member only while no healer is dead
    Player* target = healer ? healer : (healerDead ? nullptr : valuable);
    if (!target || !PackHeld())
        return nullptr;

    return target;
}

bool BearBattleResTrigger::IsActive()
{
    // cheap flags, then the cached scan
    return bot->IsInCombat() && bot->GetHealthPct() > BearBattleRes::MIN_HEALTH_PCT && GetTarget();
}

bool BearBattleResReshiftTrigger::IsActive()
{
    if (!bot->IsInCombat() || !bot->IsAlive() || bot->IsNonMeleeSpellCast(false) || InBearForm(botAI, bot))
        return false;

    uint32 const attempt = BearBattleRes::LastAttemptMs(botAI);
    return attempt && getMSTimeDiff(attempt, getMSTime()) < BearBattleRes::RESHIFT_WINDOW_MS;
}

// [rot51] b187 5H cat: Rip 1.7 a minute against the sim's 2.2, Shred 12.8 against 17.4. The Rip / Shred inputs on a
// boss, one line per change (seconds and energy in steps of 10, Shred's own position requirement included).
void FerociousBiteTimeTrigger::TraceCat()
{
    constexpr uint8 ENERGY_STEP = 10;
    constexpr int32 SECONDS_STEP = 5;
    Unit* target = AI_VALUE(Unit*, "current target");
    Creature* boss = target ? target->ToCreature() : nullptr;
    if (!boss || !boss->IsDungeonBoss() || !bot->IsInCombat())
        return;

    Aura const* rip = botAI->GetAura("rip", target, true);
    Aura const* rake = botAI->GetAura("rake", target, true);
    Aura const* roar = botAI->GetAura("savage roar", bot);
    auto seconds = [](Aura const* aura)
    { return aura ? aura->GetDuration() / IN_MILLISECONDS / SECONDS_STEP * SECONDS_STEP : 0; };
    std::string const trace = Acore::StringFormat(
        "cp {} energy {}+ clearcasting {} cat form {} melee {} behind {} melee attacking {}, own rip {}+ s rake {}+ "
        "s savage roar {}+ s, mangle on target {}",
        AI_VALUE2(uint8, "combo", "current target"),
        AI_VALUE2(uint8, "energy", "self target") / ENERGY_STEP * ENERGY_STEP,
        botAI->HasAura("clearcasting", bot), botAI->HasAura("cat form", bot), bot->IsWithinMeleeRange(target),
        !target->HasInArc(static_cast<float>(M_PI), bot), bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING), seconds(rip),
        seconds(rake), seconds(roar),
        botAI->HasAura("mangle (cat)", target, false, false, -1, true) ||
            botAI->HasAura("mangle (bear)", target, false, false, -1, true) ||
            botAI->HasAura("trauma", target, false, false, -1, true));
    if (trace == lastTrace)
        return;

    lastTrace = trace;
    LOG_INFO("playerbots", "[rot51] {} cat check on {}: {}", bot->GetName(), boss->GetName(), trace);
}

// [rot64]
bool CatFrontBuilderTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    return target && !CatIsBehind(bot, target) && ComboPointsNotFullTrigger::IsActive();
}

bool FerociousBiteExecuteTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsAlive() || !botAI->HasSpell("ferocious bite"))
        return false;
    if (!AI_VALUE2(uint8, "combo", "current target"))
        return false;
    // [rot71] the Bite's own energy (spell data), or Clearcasting's free one
    SpellInfo const* bite = sSpellMgr->GetSpellInfo(AI_VALUE2(uint32, "spell id", "ferocious bite"));
    if (!bite)
        return false;
    if (bot->GetPower(POWER_ENERGY) < int32(bite->CalcPowerCost(bot, bite->GetSchoolMask())) &&
        !botAI->HasAura("clearcasting", bot))
        return false;
    float const horizon =
        ComboFinishHorizon(botAI, bot, CatIsBehind(bot, target) ? "shred" : "mangle (cat)", "ferocious bite");
    return horizon > 0.0f && ExpectedLifetime(botAI, target) < horizon;
}

bool CatRipTicksOut(PlayerbotAI* botAI, Unit* target)
{
    if (!target)
        return false;

    float const fight = ExpectedFightLifetime(botAI, target);  // [rot240] [rot341]
    bool const ticksOut = fight >= RIP_MIN_TARGET_LIFETIME;
    if (ticksOut != (ExpectedLifetime(botAI, target) >= RIP_MIN_TARGET_LIFETIME))
        LOG_DEBUG("playerbots", "[rot341] {} on {}: Rip {} by its fight lifetime {:.1f} s (4 s rate {:.1f} s)",
                  botAI->GetBot()->GetName(), target->GetName(), ticksOut ? "ticks out" : "would not tick out",
                  fight, ExpectedLifetime(botAI, target));
    return ticksOut;
}

// [rot330] Mangle (Bear) on its cooldown with less than `withinMs` of it left (wowsims spellTimeToReady < x and not
// spellIsReady). Under Berserk Mangle has no cooldown, so nothing waits.
bool BearMangleDueWithin(PlayerbotAI* botAI, Player* bot, uint32 withinMs)
{
    uint32 const mangle = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "mangle (bear)")->Get();
    if (!mangle || !bot->HasSpellCooldown(mangle) || bot->GetSpellCooldownDelay(mangle) >= withinMs)
        return false;

    LOG_DEBUG("playerbots", "[rot330] {} holds the GCD for Mangle (Bear): back in {} ms (< {} ms)", bot->GetName(),
              bot->GetSpellCooldownDelay(mangle), withinMs);
    return true;
}
