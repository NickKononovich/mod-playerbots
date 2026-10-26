/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LeaderValues.h"

#include "Creature.h"
#include "EstimatedLifetimeValue.h"
#include "GameObject.h"
#include "Group.h"
#include "HealthTriggers.h"
#include "InstanceScript.h"  // [b44_hos]
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "PartyStrengthValue.h"
#include "Playerbots.h"
#include "RaidConsumables.h"
#include "RaidLeaderState.h"
#include "SpellAuras.h"
#include "StringFormat.h"
#include "TestHarnessData.h"
#include "ThreatValues.h"
#include "Timer.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace RaidLeader;

namespace
{
    constexpr uint8 HP_MARK_HALF_BIT = 0x01;
    constexpr uint8 HP_MARK_LAST_BIT = 0x02;
    constexpr int32 LUST_FRESH_MS = 15 * IN_MILLISECONDS;  // Sated / Exhaustion applied in the last seconds
    constexpr float HEALER_OOM_PCT = 5.0f;

    // Fight outlook (LeaderFightOutlookValue): absolute numbers only, never a boss health percent. The members die
    // one after another, so over the time the group lasts it deals about half its full damage.
    constexpr float ATTRITION_SHARE = 0.5f;
    constexpr float OUTLOOK_EMA_ALPHA = 0.2f;  // per one-second sample: about five seconds of memory
    constexpr uint32 OUTLOOK_MIN_SAMPLE_MS = 700;
    constexpr uint32 OUTLOOK_WARM_SAMPLES = 3;
    constexpr float EFFICIENCY_FLOOR = 0.35f;  // dealt / expected damage, clamped against single bad seconds
    constexpr float EFFICIENCY_CEILING = 3.0f;
    constexpr float ENEMY_ABILITY_FACTOR = 1.3f;  // spells and abilities on top of the enemies' melee swings
    constexpr float HEALED_DRAIN_SHARE = 0.5f;    // before the first samples: healers cover half the damage
    constexpr float MANA_BURN_EPSILON = 0.5f;
    constexpr float MAX_SURVIVE_SEC = 600.0f;
    constexpr float LOW_MANA_PCT = 10.0f;
    constexpr float LOW_MANA_DPS_FACTOR = 0.3f;  // a caster without mana is down to its wand
    constexpr uint8 LOST_STREAK = 2;
    // [b33_harn] hos-h 20261026-000434-hos-h-5B-37 (1909 s): the holy paladin died; 3 s later the outlook said
    // "ttk 22 s vs survive 6 s" and the rogue Vanished (sat 16 s in "wipe escape") while the tank lived 48 s more and
    // the group killed 10 mobs. The survive time came from the enemies' theoretical swing damage (no armor, block,
    // Shield Wall, self-heals) and from the dead healer's whole health counted as one second of drain. Without a
    // healer the fight is judged on the group's OWN observed unhealed drain, and only after this many samples of it.
    constexpr uint32 UNHEALED_WARM_SAMPLES = 8;
    constexpr uint32 JUDGE_LOG_MS = 5 * IN_MILLISECONDS;

    // Wipe escape (WipeEscapeSpellValue, WipeEscapeHoldValue).
    constexpr uint32 ESCAPE_GRACE_MS = 4 * IN_MILLISECONDS;    // Invisibility fades in over 3 seconds
    constexpr uint32 ESCAPE_SETTLE_MS = 10 * IN_MILLISECONDS;  // the mobs walk home before anyone stands up
    constexpr uint32 ESCAPE_HOLD_MAX_MS = 150 * IN_MILLISECONDS;
    // Hidden but losing its life (a ground effect under it, hos-h 2026-10-25: Feign Death in a Searing Gaze): this
    // many health points below the start of the hold, it ends so the bot steps out, as a player stands up out of
    // the fire.
    constexpr float ESCAPE_HURT_DROP_PCT = 25.0f;

    bool IsCoordinator(AiObjectContext* context, Player* bot)
    {
        return context->GetValue<ObjectGuid>("raid coordinator")->Get() == bot->GetGUID();
    }

    bool HasCoordinator(AiObjectContext* context)
    {
        return !context->GetValue<ObjectGuid>("raid coordinator")->Get().IsEmpty();
    }

    bool FreshLust(Player* member)
    {
        for (uint32 spellId : {SPELL_SATED, SPELL_EXHAUSTION})
            if (Aura* aura = member->GetAura(spellId))
                if (aura->GetMaxDuration() - aura->GetDuration() < LUST_FRESH_MS)
                    return true;

        return false;
    }

    struct EncounterSnapshot
    {
        ObjectGuid engaged;
        bool pullAnnounced = false;
        uint8 hpMarks = 0;
        bool lust = false;
        bool wipeCalled = false;
        bool escapeOrdered = false;
        std::unordered_set<ObjectGuid> battleRes;
    };

    struct GroupCensus
    {
        uint32 total = 0;
        uint32 dead = 0;
        uint32 tanks = 0;
        uint32 tanksAlive = 0;
        uint32 healers = 0;
        uint32 healersDead = 0;
        uint32 healersOom = 0;  // alive, below HEALER_OOM_PCT mana
        bool deadTankCanSelfRes = false;
        bool battleRes = false;
        bool freshLust = false;
        ObjectGuid battleResTarget;
    };

    GroupCensus TakeCensus(Player* bot, EncounterSnapshot const& snapshot)
    {
        GroupCensus census;
        Group* group = bot->GetGroup();
        Map* map = bot->GetMap();
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsInWorld() || member->GetMap() != map)
                continue;

            bool const alive = member->IsAlive();
            ++census.total;
            census.dead += alive ? 0 : 1;

            if (PlayerbotAI::IsTank(member))
            {
                ++census.tanks;
                census.tanksAlive += alive ? 1 : 0;
                if (!alive && member->GetUInt32Value(PLAYER_SELF_RES_SPELL))
                    census.deadTankCanSelfRes = true;
            }
            else if (PlayerbotAI::IsHeal(member))
            {
                ++census.healers;
                census.healersDead += alive ? 0 : 1;
                if (alive && member->GetMaxPower(POWER_MANA) && member->GetPowerPct(POWER_MANA) < HEALER_OOM_PCT)
                    ++census.healersOom;
            }

            if (alive)
            {
                census.battleRes = census.battleRes || HasBattleResurrection(member);
                census.freshLust = census.freshLust || FreshLust(member);
            }
            else if (!census.battleResTarget && member->isResurrectRequested() &&
                     !snapshot.battleRes.count(member->GetGUID()))
                census.battleResTarget = member->GetGUID();
        }

        return census;
    }

    // Why the fight may be lost: only the question. The answer is the fight outlook (absolute enemy health against
    // the group's damage and how long it lasts); a tank dying is no wipe while the enemies die first.
    std::string WipeReason(GroupCensus const& census)
    {
        if (census.tanks && !census.tanksAlive && !census.battleRes && !census.deadTankCanSelfRes)
            return "tanks dead, no battle res";

        if (census.healers && census.healersDead * 2 > census.healers)
            return "healers dead";

        if (census.healers && census.healersOom && census.healersOom + census.healersDead == census.healers)
            return "healers out of mana";

        return "";
    }

    // [b44_hos] An instance encounter runs while no boss fights the group: a scripted event (the Tribunal of Ages'
    // endless waves on a 300 s timer, Culling of Stratholme's waves, an escort). The fight outlook weighs the enemies
    // on the field against the group's damage, but such an event is not won by killing them (more keep coming, the
    // timer or the script ends it), so a kill time beyond the survive time means nothing there. hos-h Tribunal
    // (DPS_GAP_TRIBUNAL_COS.md 2.5): "fight lost" sent the hunter into Feign Death and the mage into Invisibility for
    // up to 150 s while the rest of the group fought on. Such an event is lost only once most of the group is dead.
    bool ScriptedEventHeld(Player* bot, GroupCensus const& census)
    {
        InstanceScript* instance = bot->GetInstanceScript();
        return instance && instance->IsEncounterInProgress() && census.dead * 2 < census.total;
    }

    float SwingDps(Unit* unit)
    {
        uint32 const attackMs = unit->GetAttackTime(BASE_ATTACK);
        if (!attackMs)
            return 0.0f;

        float const swing =
            (unit->GetFloatValue(UNIT_FIELD_MINDAMAGE) + unit->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2.0f;
        return swing * float(IN_MILLISECONDS) / float(attackMs);
    }

    // The damage the member should deal for its role, level and gear (the test harness' expectation table).
    float ExpectedDps(Player* member, uint8 groupSize)
    {
        TestHarness::Role const role = PlayerbotAI::IsTank(member)   ? TestHarness::Role::Tank
                                       : PlayerbotAI::IsHeal(member) ? TestHarness::Role::Healer
                                                                     : TestHarness::Role::Melee;
        float dps =
            TestHarness::ExpectedMemberDps(role, member->GetLevel(), member->GetAverageItemLevel(), groupSize);
        if (role != TestHarness::Role::Healer && member->getPowerType() == POWER_MANA &&
            member->GetMaxPower(POWER_MANA) && member->GetPowerPct(POWER_MANA) < LOW_MANA_PCT)
            dps *= LOW_MANA_DPS_FACTOR;

        return dps;
    }

    bool HasSoulstone(PlayerbotAI* botAI, Player* member)
    {
        return botAI->HasAura("soulstone resurrection", member);
    }

    // Reincarnation (shaman, level 30): known, off cooldown and an Ankh in the bags.
    bool CanReincarnate(Player* bot)
    {
        constexpr uint32 SPELL_REINCARNATION = 20608;
        constexpr uint32 ITEM_ANKH = 17030;
        return bot->HasSpell(SPELL_REINCARNATION) && !bot->HasSpellCooldown(SPELL_REINCARNATION) &&
               bot->HasItemCount(ITEM_ANKH, 1);
    }

    bool IsHiddenFromFight(Player* bot)
    {
        return bot->HasStealthAura() || bot->HasInvisibilityAura() || bot->HasUnitFlag2(UNIT_FLAG2_FEIGN_DEATH) ||
               bot->HasAura(SPELL_DIVINE_INTERVENTION_AURA);
    }

    bool OtherMemberFighting(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->GetMap() == bot->GetMap() && member->IsAlive() &&
                member->IsInCombat())
                return true;
        }

        return false;
    }
}

std::string FormatOutlook(FightOutlook const& outlook)
{
    return Acore::StringFormat("(enemies {}, eHP {:.0f}, groupDps {:.0f}, ttk {:.0f} s vs survive {:.0f} s)",
                               outlook.enemies, outlook.enemyHp, outlook.groupDps, outlook.killSec,
                               outlook.surviveSec);
}

// Divine Intervention takes a living resurrecter of the group out of the fight: a healer first.
Player* FindDivineInterventionTarget(PlayerbotAI* botAI, Player* bot)
{
    constexpr float DIVINE_INTERVENTION_RANGE = 30.0f;
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    Player* found = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || member->GetMap() != bot->GetMap() || !member->IsAlive() ||
            bot->GetDistance(member) > DIVINE_INTERVENTION_RANGE || !CanResurrect(member) ||
            member->HasAura(SPELL_DIVINE_INTERVENTION_AURA) || HasSoulstone(botAI, member))
            continue;

        if (!found || (PlayerbotAI::IsHeal(member) && !PlayerbotAI::IsHeal(found)))
            found = member;
    }

    return found;
}

void LeaderFightOutlookValue::Clear()
{
    _lastMs = 0;
    _samples = 0;
    _lostStreak = 0;
    _unhealedSamples = 0;  // [b33_harn]
    _damageEma = 0.0f;
    _capacityEma = 0.0f;
    _drainEma = 0.0f;
    _manaBurnEma = 0.0f;
    _enemyHp.clear();
    _members.clear();
}

FightOutlook LeaderFightOutlookValue::Calculate()
{
    FightOutlook outlook;
    Group* group = bot->GetGroup();
    if (!group || !bot->IsInCombat() || !IsCoordinator(context, bot))
    {
        Clear();
        return outlook;
    }

    uint32 const now = getMSTime();
    uint32 const elapsedMs = _lastMs ? getMSTimeDiff(_lastMs, now) : 0;
    if (_lastMs && elapsedMs < OUTLOOK_MIN_SAMPLE_MS)
        return value;  // asked again within the second (a reader's Reset): the last sample stands

    // The enemies: their health, the health they lost since the last sample, and what they deal.
    std::unordered_map<ObjectGuid, uint32> enemyHp;
    float damage = 0.0f;
    float enemyDps = 0.0f;
    for (ObjectGuid const guid : AI_VALUE(GuidVector, "attackers"))
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive())
            continue;

        uint32 const health = unit->GetHealth();
        enemyHp[guid] = health;
        ++outlook.enemies;
        outlook.enemyHp += float(health);

        auto const previous = _enemyHp.find(guid);
        if (previous != _enemyHp.end() && previous->second > health)
            damage += float(previous->second - health);

        if (unit->GetVictim())
            enemyDps += SwingDps(unit);
    }

    for (auto const& [guid, health] : _enemyHp)
        if (!enemyHp.count(guid))
            if (Unit* unit = botAI->GetUnit(guid))
                if (!unit->IsAlive())
                    damage += float(health);  // died since: the rest of its health was dealt too

    // The group: expected damage of the living, its health, what it lost net of heals, the healers' mana.
    uint8 const groupSize = uint8(std::min<uint32>(group->GetMembersCount(), MAXRAIDSIZE));
    std::unordered_map<ObjectGuid, MemberSample> members;
    float capacity = 0.0f;
    float healthPool = 0.0f;
    float drain = 0.0f;
    float manaSpent = 0.0f;
    uint32 healerMana = 0;
    bool healerWithMana = false;
    Map* map = bot->GetMap();
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || member->GetMap() != map)
            continue;

        auto const previous = _members.find(member->GetGUID());
        if (!member->IsAlive())
            continue;  // [b33_harn] its last health is out of the pool, not a second's drain on the living

        MemberSample sample;
        sample.health = member->GetHealth();
        healthPool += float(sample.health);
        capacity += ExpectedDps(member, groupSize);
        if (previous != _members.end())
            drain += float(previous->second.health) - float(sample.health);

        if (PlayerbotAI::IsHeal(member) && member->GetMaxPower(POWER_MANA))
        {
            sample.mana = member->GetPower(POWER_MANA);
            healerMana += sample.mana;
            healerWithMana = healerWithMana || member->GetPowerPct(POWER_MANA) >= HEALER_OOM_PCT;
            if (previous != _members.end())
                manaSpent += float(previous->second.mana) - float(sample.mana);
        }

        members[member->GetGUID()] = sample;
    }

    float const rawDps = enemyDps * ENEMY_ABILITY_FACTOR;
    if (!_lastMs)
    {
        _damageEma = capacity;
        _capacityEma = capacity;
        _drainEma = healerWithMana ? rawDps * HEALED_DRAIN_SHARE : rawDps;
        _manaBurnEma = 0.0f;
    }
    else
    {
        float const seconds = float(elapsedMs) / float(IN_MILLISECONDS);
        auto blend = [](float& ema, float sample) { ema += OUTLOOK_EMA_ALPHA * (sample - ema); };
        blend(_damageEma, damage / seconds);
        blend(_capacityEma, capacity);
        blend(_drainEma, drain / seconds);
        blend(_manaBurnEma, manaSpent / seconds);
        ++_samples;
    }

    _lastMs = now;
    _enemyHp = std::move(enemyHp);
    _members = std::move(members);

    float efficiency = 1.0f;
    if (_samples >= OUTLOOK_WARM_SAMPLES && _capacityEma > 0.0f)
        efficiency = std::clamp(_damageEma / _capacityEma, EFFICIENCY_FLOOR, EFFICIENCY_CEILING);

    outlook.groupDps = capacity * efficiency;
    outlook.killSec = outlook.enemyHp / std::max(outlook.groupDps, 1.0f);

    // Healed while the healers have mana (the observed net drain), then the enemies' full damage.
    // [b33_harn] With no healer left, the observed drain IS the unhealed drain: it is used once warm.
    _unhealedSamples = healerWithMana ? 0 : _unhealedSamples + 1;
    bool const unhealedWarm = !healerWithMana && _unhealedSamples >= UNHEALED_WARM_SAMPLES && _drainEma > 0.0f;
    float const unhealedDrain = unhealedWarm ? std::max(_drainEma, 1.0f) : std::max({rawDps, _drainEma, 1.0f});
    float manaSec = 0.0f;
    if (healerWithMana)
        manaSec = _manaBurnEma > MANA_BURN_EPSILON ? float(healerMana) / _manaBurnEma : MAX_SURVIVE_SEC;

    float surviveSec = 0.0f;
    if (manaSec <= 0.0f)
        surviveSec = healthPool / unhealedDrain;
    else if (_drainEma <= 0.0f)
        surviveSec = manaSec + healthPool / unhealedDrain;
    else if (healthPool / _drainEma <= manaSec)
        surviveSec = healthPool / _drainEma;
    else
        surviveSec = manaSec + (healthPool - _drainEma * manaSec) / unhealedDrain;

    outlook.surviveSec = std::min(surviveSec, MAX_SURVIVE_SEC);

    // [b33_harn] no healer: not lost before the unhealed drain was observed (UNHEALED_WARM_SAMPLES)
    bool const lostNow = outlook.enemies && (healerWithMana || unhealedWarm) &&
                         outlook.killSec > outlook.surviveSec * ATTRITION_SHARE;
    _lostStreak = lostNow ? uint8(std::min<uint32>(_lostStreak + 1, LOST_STREAK)) : 0;
    outlook.lost = _lostStreak >= LOST_STREAK;
    return outlook;
}

ObjectGuid RaidCoordinatorValue::Calculate()
{
    Player* coordinator = FindCoordinator(bot);
    return coordinator ? coordinator->GetGUID() : ObjectGuid::Empty;
}

ObjectGuid LeaderNearbyBossValue::Calculate()
{
    if (bot->IsInCombat() || !IsCoordinator(context, bot))
        return ObjectGuid::Empty;

    Player* mainTank = nullptr;
    if (Group* group = bot->GetGroup())
        mainTank = ObjectAccessor::FindPlayer(PlayerbotAI::GetMainTankGuid(group));
    if (mainTank && mainTank->GetMap() != bot->GetMap())
        mainTank = nullptr;

    for (ObjectGuid const guid : AI_VALUE(GuidVector, "possible targets"))
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || unit->IsInCombat() || !IsBossCreature(unit))
            continue;

        if (bot->GetDistance(unit) <= BOSS_NEAR_DISTANCE ||
            (mainTank && mainTank->GetDistance(unit) <= BOSS_NEAR_DISTANCE))
            return guid;
    }

    return ObjectGuid::Empty;
}

Unit* LeaderEncounterEventValue::FindEngagedBoss()
{
    for (ObjectGuid const guid : AI_VALUE(GuidVector, "possible targets no los"))
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || !unit->IsInCombat() || !IsBossCreature(unit))
            continue;

        Unit* victim = unit->GetVictim();
        Player* victimPlayer = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (victimPlayer && victimPlayer->IsInSameRaidWith(bot))
            return unit;
    }

    return nullptr;
}

LeaderEvent LeaderEncounterEventValue::Calculate()
{
    LeaderEvent event;
    Group* group = bot->GetGroup();
    if (!group || !IsCoordinator(context, bot))
        return event;

    uint32 const nowMs = getMSTime();
    EncounterSnapshot const snapshot = Registry::Instance().With(
        group->GetGUID(),
        [nowMs](GroupState& state)
        {
            EncounterSnapshot result;
            result.engaged = state.engagedBoss;
            result.pullAnnounced = state.pullAnnounced;
            result.hpMarks = state.hpMarks;
            result.lust = state.lustAnnounced;
            result.wipeCalled = state.wipeCalled;
            result.battleRes = state.battleResAnnounced;
            result.escapeOrdered =
                state.escapeOrderMs && getMSTimeDiff(state.escapeOrderMs, nowMs) < WIPE_ESCAPE_ORDER_MS;
            return result;
        });

    if (!snapshot.engaged)
    {
        if (!bot->IsInCombat())
            return event;

        if (Unit* boss = FindEngagedBoss())
        {
            event.type = LeaderEventType::Engage;
            event.boss = boss->GetGUID();
            return event;
        }

        // A trash fight is judged the same way.
        if (!snapshot.escapeOrdered)
        {
            GroupCensus const census = TakeCensus(bot, snapshot);
            if (!ScriptedEventHeld(bot, census))  // [b44_hos]
                JudgeFight(WipeReason(census), event);
            else if (uint32 const now = getMSTime();
                     !_lastJudgeLogMs || getMSTimeDiff(_lastJudgeLogMs, now) >= JUDGE_LOG_MS)
            {
                _lastJudgeLogMs = now;
                LOG_INFO("playerbots", "[wipe] {}: keep fighting, an encounter script runs with no boss on the group "
                         "({} of {} members alive)", bot->GetName(), census.total - census.dead, census.total);
            }
        }

        return event;
    }

    event.boss = snapshot.engaged;
    Unit* boss = botAI->GetUnit(snapshot.engaged);
    if (!boss)
    {
        event.type = LeaderEventType::Reset;
        return event;
    }

    if (!boss->IsAlive())
    {
        event.type = LeaderEventType::BossDown;
        return event;
    }

    GroupCensus const census = TakeCensus(bot, snapshot);
    Creature* creature = boss->ToCreature();
    if (!boss->IsInCombat() || (creature && creature->IsInEvadeMode()))
    {
        bool const lost = !snapshot.wipeCalled && census.dead * 2 >= census.total;
        event.type = lost ? LeaderEventType::Wipe : LeaderEventType::Reset;
        event.reason = lost ? "boss reset" : "";
        return event;
    }

    if (!snapshot.pullAnnounced)
    {
        event.type = LeaderEventType::Engage;
        return event;
    }

    float const hpPct = boss->GetHealthPct();
    if (!snapshot.wipeCalled && !snapshot.escapeOrdered && JudgeFight(WipeReason(census), event))
        return event;

    if (!(snapshot.hpMarks & HP_MARK_LAST_BIT) && hpPct <= LAST_PHASE_HP_PCT)
    {
        event.type = LeaderEventType::HpMark;
        event.mark = HP_MARK_LAST_BIT;
        return event;
    }

    if (!(snapshot.hpMarks & (HP_MARK_HALF_BIT | HP_MARK_LAST_BIT)) && hpPct <= HP_MARK_HALF)
    {
        event.type = LeaderEventType::HpMark;
        event.mark = HP_MARK_HALF_BIT;
        return event;
    }

    if (!snapshot.lust && census.freshLust)
    {
        event.type = LeaderEventType::Bloodlust;
        return event;
    }

    if (census.battleResTarget)
    {
        event.type = LeaderEventType::BattleRes;
        event.target = census.battleResTarget;
    }

    return event;
}

bool LeaderEncounterEventValue::JudgeFight(std::string const& reason, LeaderEvent& event)
{
    // Sampled every second of the fight, so its rates are warm when a reason comes up.
    FightOutlook const outlook = AI_VALUE(FightOutlook, "leader fight outlook");
    if (reason.empty())
        return false;

    if (outlook.lost)
    {
        event.type = LeaderEventType::LostFight;
        event.reason = reason;
        return true;
    }

    uint32 const now = getMSTime();
    if (!_lastJudgeLogMs || getMSTimeDiff(_lastJudgeLogMs, now) >= JUDGE_LOG_MS)
    {
        _lastJudgeLogMs = now;
        LOG_INFO("playerbots", "[wipe] {}: keep fighting, {} {}", bot->GetName(), reason, FormatOutlook(outlook));
    }

    return false;
}

TankAssignment LeaderTankAssignmentValue::Calculate()
{
    TankAssignment result;
    Group* group = bot->GetGroup();
    if (!group || !group->isRaidGroup() || !IsCoordinator(context, bot))
        return result;

    Map* map = bot->GetMap();
    bool const inCombat = bot->IsInCombat();

    ObjectGuid currentTank;
    ObjectGuid currentAssist;
    for (Group::MemberSlot const& slot : group->GetMemberSlots())
    {
        if (slot.flags & MEMBER_FLAG_MAINTANK)
            currentTank = slot.guid;
        if (slot.flags & MEMBER_FLAG_MAINASSIST)
            currentAssist = slot.guid;
    }

    // A flagged tank keeps the flag (it may have been set by hand) until it leaves, or dies during a fight.
    auto keep = [&](ObjectGuid guid) -> Player*
    {
        Player* player = guid ? ObjectAccessor::FindPlayer(guid) : nullptr;
        if (!player || player->GetMap() != map || !group->IsMember(guid) || !PlayerbotAI::IsTank(player))
            return nullptr;
        return (inCombat && !player->IsAlive()) ? nullptr : player;
    };

    auto best = [&](Player* exclude) -> Player*
    {
        Player* found = nullptr;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == exclude || member->GetMap() != map || !member->IsAlive() ||
                !PlayerbotAI::IsTank(member))
                continue;

            if (!found || member->GetMaxHealth() > found->GetMaxHealth())
                found = member;
        }
        return found;
    };

    Player* mainTank = keep(currentTank);
    if (!mainTank)
        mainTank = best(nullptr);
    if (!mainTank)
        return result;

    Player* mainAssist = keep(currentAssist);
    if (mainAssist == mainTank)
        mainAssist = nullptr;
    if (!mainAssist)
        mainAssist = best(mainTank);

    result.mainTank = mainTank->GetGUID();
    result.mainAssist = mainAssist ? mainAssist->GetGUID() : ObjectGuid::Empty;
    result.changed = result.mainTank != currentTank || (mainAssist && result.mainAssist != currentAssist);
    return result;
}

bool GroupPausedValue::Calculate() { return RaidLeader::IsPaused(bot); }

bool PullHoldValue::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group || bot->IsInCombat())
        return false;

    // A group pause holds every pull, in the open world too and with nobody coordinating.
    if (AI_VALUE(bool, "group paused"))
        return true;

    if (!HasCoordinator(context))
        return false;

    // A person leads: the boss is held until the fight starts (the person pulls, or orders the pull).
    bool const ledByPerson = RaidLeader::IsLedByPerson(bot);
    uint32 const now = getMSTime();
    // [ready_hold_30s] every member alive, here and above its own rest lines (RestState::WantsDrink / WantsFood)
    bool groupAtLines = true;
    for (GroupReference* ref = group->GetFirstMember(); groupAtLines && ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || member->GetMap() != bot->GetMap())
            continue;
        if (!member->IsAlive() || bot->GetDistance(member) > READY_MAX_LEADER_DISTANCE ||
            (GET_PLAYERBOT_AI(member) && (RestState::WantsDrink(member) || RestState::WantsFood(member))))
            groupAtLines = false;
    }
    return Registry::Instance().With(
        group->GetGUID(),
        [now, ledByPerson, groupAtLines](GroupState& state)
        {
            if (state.recovering && getMSTimeDiff(state.wipeMs, now) < WIPE_RECOVERY_MAX_MS + READY_CHECK_ANSWER_MS)
                return true;

            if (!state.holdBoss)
                return false;

            if (ledByPerson)
                return getMSTimeDiff(state.holdSinceMs, now) <= PULL_HOLD_PERSON_MAX_MS;

            if (getMSTimeDiff(state.holdSinceMs, now) > PULL_HOLD_MAX_MS)
                return false;

            // [ready_hold_30s] held PULL_HOLD_READY_MAX_MS with the group ready by its own lines: the pull goes on
            if (groupAtLines && getMSTimeDiff(state.holdSinceMs, now) > PULL_HOLD_READY_MAX_MS)
                return false;

            return state.readyBoss != state.holdBoss || !state.readyMs ||
                   getMSTimeDiff(state.readyMs, now) < PULL_DELAY_MS;
        });
}

bool PullCountdownValue::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group || !bot->IsInCombat() || !HasCoordinator(context))
        return false;

    // A tank in combat is never held: held, its tank assist
    // (the only way its non-combat engine joins a fight), rotation and taunts stopped and the boss killed the rest
    // of the group (Bronjahm, two wipes with the tank at full health).
    if (PlayerbotAI::IsTank(bot))
        return false;

    uint32 const holdMs = sPlayerbotAIConfig.RaidLeaderPullHoldSecs * IN_MILLISECONDS;
    uint32 const now = getMSTime();
    ObjectGuid engaged;
    uint32 engageMs = 0;
    Registry::Instance().With(group->GetGUID(),
                              [&](GroupState& state)
                              {
                                  engaged = state.engagedBoss;
                                  engageMs = state.engageMs;
                              });

    // A wipe call never holds attacks: a held member only dies standing. Everyone fights to the end, and only the
    // members who can leave the fight do (wipe escape).
    if (!holdMs || botAI->HasStrategy("wait for attack", BOT_STATE_COMBAT))
        return false;

    // [combat_no_damage_release] the group in combat and nobody hitting anything for a while: nobody is held
    if (GroupDamage::NoDamageRelease(bot))
        return false;

    // A tank soft for the content, or outgeared by its dps (party strength), gets longer to build its lead.
    constexpr uint32 CAREFUL_EXTRA_HOLD_MS = 2 * IN_MILLISECONDS;
    bool const careful = AI_VALUE(PartyStrength, "party strength").CarefulThreat();
    uint32 const careHoldMs = careful ? CAREFUL_EXTRA_HOLD_MS : 0;
    if (engaged)
    {
        if (getMSTimeDiff(engageMs, now) >= holdMs + careHoldMs)
            return false;

        // [threat-open] The hold is the longest wait, not a fixed one: a dps opens on the boss as soon as the tank
        // holds a safe threat lead on it (its threat after the next cast under ThreatLead's open share, the boss'
        // own threat table), the way a player opens when Omen shows the tank ahead.
        Unit* boss = AI_VALUE(Unit*, "current target");
        if (boss && boss->GetGUID() == engaged && ThreatLead::SafeToOpen(botAI, boss, careful))
        {
            if (_openedOn != engaged)
            {
                _openedOn = engaged;
                LOG_INFO("playerbots", "[threat-open] {} opens on {} {} ms after the engage: safe threat lead",
                         bot->GetName(), boss->GetName(), getMSTimeDiff(engageMs, now));
            }

            return false;
        }

        return true;
    }

    // The coordinator records the pull within a second; until then a boss target that is already fighting
    // the group counts as just pulled.
    Unit* target = AI_VALUE(Unit*, "current target");
    return target && IsBossCreature(target) && target->IsInCombat();
}

bool ReadyCheckInProgressValue::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group || !HasCoordinator(context))
        return false;

    return Registry::Instance().With(group->GetGUID(), [](GroupState& state)
                                     { return state.phase != ReadyCheckPhase::Idle || state.recovering; });
}

uint32 PrePullWindowValue::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group || bot->IsInCombat() || !RaidConsumables::Allowed(bot) || !HasCoordinator(context))
        return 0;

    uint32 const now = getMSTime();
    ObjectGuid readyBoss;
    uint32 readyMs = 0;
    Registry::Instance().With(group->GetGUID(),
                              [&](GroupState& state)
                              {
                                  readyBoss = state.readyBoss;
                                  readyMs = state.readyMs;
                              });

    if (!readyBoss || !readyMs || getMSTimeDiff(readyMs, now) > PRE_POT_WINDOW_MS)
        return 0;

    Unit* boss = botAI->GetUnit(readyBoss);
    if (!boss || !boss->IsAlive() || boss->IsInCombat() || bot->GetDistance(boss) > PRE_POT_BOSS_DISTANCE)
        return 0;

    return readyMs;
}

ObjectGuid RaidFlaskValue::Calculate()
{
    if (bot->IsInCombat() || !bot->IsAlive() || !RaidConsumables::Allowed(bot))
        return ObjectGuid::Empty;

    return RaidConsumables::FindFlask(bot);
}

ObjectGuid RaidFoodBuffValue::Calculate()
{
    if (bot->IsInCombat() || !bot->IsAlive() || !RaidConsumables::Allowed(bot))
        return ObjectGuid::Empty;

    return RaidConsumables::FindFoodBuff(bot);
}

ObjectGuid PrePullPotionValue::Calculate()
{
    if (!bot->IsAlive() || !RaidConsumables::Allowed(bot))
        return ObjectGuid::Empty;

    return RaidConsumables::FindPrePullPotion(bot);
}

ObjectGuid NearbySoulwellValue::Calculate()
{
    Map* map = bot->GetMap();
    if (!sPlayerbotAIConfig.RaidConsumables || !map || !map->IsDungeon() || bot->IsInCombat() || !bot->IsAlive())
        return ObjectGuid::Empty;

    // A Soulwell when the bot has no healthstone; a Refreshment Table when it has no Mana Strudel.
    bool const wantsHealthstone = !RaidConsumables::FindHealthstone(bot);
    // [queue-soulwell-loop] a Mana Strudel only for a mana user (slabs-n 20261025-153805-slabs-n-5A-50: all five
    // members on "use soulwell (13.00)" at a Refreshment Table that gave nothing, above "test runner move" (11)).
    bool const wantsStrudel = bot->getPowerType() == POWER_MANA &&
                              !bot->HasItemCount(RaidConsumables::ITEM_MANA_STRUDEL_RANK_1, 1) &&
                              !bot->HasItemCount(RaidConsumables::ITEM_MANA_STRUDEL_RANK_2, 1);
    if (!wantsHealthstone && !wantsStrudel)
        return ObjectGuid::Empty;

    for (ObjectGuid const guid : AI_VALUE(GuidVector, "nearest game objects"))
    {
        GameObject* go = botAI->GetGameObject(guid);
        if (!go || !go->isSpawned() || bot->GetDistance(go) > RaidConsumables::SOULWELL_SEARCH_DISTANCE)
            continue;

        if ((wantsHealthstone && RaidConsumables::IsSoulwell(go)) ||
            (wantsStrudel && RaidConsumables::IsRefreshmentTable(go)))
        {
            // [soulwell_give_up] A well or table the bot still "wants" after SOULWELL_GIVE_UP_MS of tries gives it
            // nothing (no charge left, an item it cannot take): it is left alone. mgt-n 2026-10-25
            // (20261025-115805-mgt-n-5A-15, 575-640 s): four members ran "use soulwell" at the same Refreshment
            // Table every few seconds for 65 s at x 206-208, outside Selin's door, so her room entry never filled.
            constexpr uint32 SOULWELL_GIVE_UP_MS = 20 * IN_MILLISECONDS;
            constexpr uint32 SOULWELL_FORGET_MS = 10 * IN_MILLISECONDS;
            struct Tries
            {
                ObjectGuid go;
                uint32 sinceMs = 0;
                uint32 seenMs = 0;
                bool logged = false;
            };
            thread_local std::unordered_map<ObjectGuid, Tries> tries;
            uint32 const now = getMSTime();
            Tries& entry = tries[bot->GetGUID()];
            if (entry.go != guid || getMSTimeDiff(entry.seenMs, now) > SOULWELL_FORGET_MS)
            {
                // a new well, or one not given up after a gap; a well that gave nothing stays given up
                // ([queue-soulwell-loop]: the old 200 s re-try put the group on it again - slabs-n 153805, 882 s)
                if (entry.go != guid || (!entry.logged && getMSTimeDiff(entry.seenMs, now) > SOULWELL_FORGET_MS))
                {
                    entry.go = guid;
                    entry.sinceMs = now ? now : 1;
                    entry.logged = false;
                }
            }
            entry.seenMs = now ? now : 1;
            if (getMSTimeDiff(entry.sinceMs, now) >= SOULWELL_GIVE_UP_MS)
            {
                if (!entry.logged)
                {
                    entry.logged = true;
                    LOG_INFO("playerbots", "[soulwell_give_up] {}: {} gave nothing in {} s of tries, left alone",
                             bot->GetName(), go->GetName(), SOULWELL_GIVE_UP_MS / IN_MILLISECONDS);
                }
                continue;
            }
            return guid;
        }
    }

    return ObjectGuid::Empty;
}

ObjectGuid NearbySoulPortalValue::Calculate()
{
    Map* map = bot->GetMap();
    if (!sPlayerbotAIConfig.RaidConsumables || !map || !map->IsDungeon() || bot->IsInCombat() || !bot->IsAlive())
        return ObjectGuid::Empty;

    for (ObjectGuid const guid : AI_VALUE(GuidVector, "nearest game objects"))
    {
        GameObject* go = botAI->GetGameObject(guid);
        if (RaidConsumables::IsSoulPortal(go) && go->isSpawned() && go->GetOwnerGUID() != bot->GetGUID() &&
            bot->GetDistance(go) <= RaidConsumables::SOULWELL_SEARCH_DISTANCE)
        {
            // [b43_class] no click (a channel of its own) at "pull imminent" or next to a pack: brd-h 060218-5F-5,
            // the tank clicked at "pull start (106)" and lost ~8 s of its pull
            std::string const blocked = RaidConsumables::RitualBlockedReason(botAI);
            if (!blocked.empty())
            {
                LOG_DEBUG("playerbots", "[b43_class] {}: no click on {}: {}", bot->GetName(), go->GetName(), blocked);
                return ObjectGuid::Empty;
            }

            return guid;
        }
    }

    return ObjectGuid::Empty;
}

std::string WipeEscapeSpellValue::Calculate()
{
    if (!bot->IsAlive() || !bot->IsInCombat() || AI_VALUE(uint32, "wipe escape at") || !HasCoordinator(context) ||
        !IsWipeEscapeOrdered(bot))
        return "";

    // A Soulstone or Reincarnation brings the bot back by itself once the mobs are gone: it fights to the end.
    if (HasSoulstone(botAI, bot) || CanReincarnate(bot))
        return "";

    // Spells that take the bot off every enemy's threat list in an instance. Divine Shield, Ice Block and running
    // do not: there the mobs never leash (Creature::CanCreatureAttack) and an immune target stays their victim.
    static constexpr char const* escapes[] = {"vanish", "feign death", "invisibility", "shadowmeld"};
    for (char const* spell : escapes)
        if (botAI->CanCastSpell(spell, bot))
            return spell;

    if (bot->getClass() == CLASS_PALADIN)
        if (Player* target = FindDivineInterventionTarget(botAI, bot))
            if (botAI->CanCastSpell("divine intervention", target))
                return "divine intervention";

    return "";
}

bool WipeEscapeHoldValue::Calculate()
{
    uint32 const escapeAt = AI_VALUE(uint32, "wipe escape at");
    bool const intervened = bot->HasAura(SPELL_DIVINE_INTERVENTION_AURA);
    if ((!escapeAt && !intervened) || !bot->IsAlive())
    {
        _quietSinceMs = 0;
        _holdHealthPct = 0.0f;
        return false;
    }

    uint32 const now = getMSTime();
    uint32 const sinceMs = escapeAt ? getMSTimeDiff(escapeAt, now) : 0;
    if (sinceMs > ESCAPE_HOLD_MAX_MS)
        return false;

    // Found again (stealth detected, Feign Death resisted): it fights on.
    bool const hidden = IsHiddenFromFight(bot);
    if (bot->IsInCombat() && !hidden && (!escapeAt || sinceMs > ESCAPE_GRACE_MS))
        return false;

    if (_holdHealthPct <= 0.0f)
        _holdHealthPct = bot->GetHealthPct();
    if (bot->GetHealthPct() + ESCAPE_HURT_DROP_PCT < _holdHealthPct)
        return false;

    if (bot->IsInCombat() || OtherMemberFighting(bot))
    {
        _quietSinceMs = 0;
        return true;
    }

    if (!_quietSinceMs)
        _quietSinceMs = now;

    return getMSTimeDiff(_quietSinceMs, now) < ESCAPE_SETTLE_MS;
}
