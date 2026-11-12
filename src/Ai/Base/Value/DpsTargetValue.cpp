/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DpsTargetValue.h"
#include "AttackerCountValues.h"
#include "Creature.h"
#include "EstimatedLifetimeValue.h"
#include "Group.h"
#include "KillOrder.h"
#include "Map.h"
#include "MotionMaster.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RaidMarkOwnership.h"
#include "ReflectAuras.h"
#include "Strategy.h"

#include <algorithm>

namespace
{
    // A ranged bot shoots a mob running for help (SMART_ACTION_FLEE_FOR_ASSIST at low health) within this range
    // before it brings the next pack; melee and the tank stay with the fight (TankTargetValue).
    constexpr float HELP_RUNNER_RANGE = 30.0f;

    // [flee_stop] also the core's plain flee (no help in reach: UNIT_STATE_FLEEING without a fear aura); noted for the
    // group (FledMobs: a survivor of it is finished later)
    bool IsRunningForHelp(Unit const* unit)
    {
        if (!FledMobs::IsRunning(unit))
            return false;

        FledMobs::Note(unit);
        return true;
    }

    // The moon mob when nothing else is left to fight and it hits the group unheld: the last of the pack.
    Unit* FreeCrowdControlMark(PlayerbotAI* botAI, Player* bot, GuidSet const& exclusions)
    {
        Group* group = bot->GetGroup();
        ObjectGuid const guid = group ? group->GetTargetIcon(RtiTargetValue::moonIndex) : ObjectGuid::Empty;
        if (!guid || exclusions.find(guid) != exclusions.end())
            return nullptr;

        GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
        if (std::find(attackers.begin(), attackers.end(), guid) == attackers.end())
            return nullptr;

        Unit* mark = botAI->GetUnit(guid);
        Unit* victim = mark ? mark->GetVictim() : nullptr;
        Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
        if (!mark || !mark->IsAlive() || IsCrowdControlled(mark) || !victimPlayer ||
            !bot->IsInSameRaidWith(victimPlayer) || !KillOrder::IsInGroupFight(bot, mark))
            return nullptr;

        return mark;
    }

    // [b33_aoe_class] A mob this close to the fight tank stands in its pile (KillOrder PACK_RADIUS).
    constexpr float CLOSE_THREAT_TANK_PILE_REACH = 10.0f;

    // [b33_aoe_class] 5-man, no strategy exclusions: the mobs on this bot or in the tank's pile, when some other mob
    // is neither (`narrowed` skips the rest). hor-n-5G-27 escape (299 s, 5-15 adds, wipe): with no exclusions the
    // lifetime / caster choice sent the dps to a distant priority mob while the adds on them and the tank lived.
    bool NarrowToCloseThreats(PlayerbotAI* botAI, Player* bot, GuidSet const& exclusions, GuidSet& narrowed)
    {
        Map* map = bot->GetMap();
        if (!map || !map->IsNonRaidDungeon() || PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))
            return false;

        Player* tank = KillOrder::FindFightTank(bot);
        narrowed = exclusions;
        bool anyClose = false;
        bool anyFar = false;
        for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
        {
            if (exclusions.find(guid) != exclusions.end())
                continue;

            Unit* unit = botAI->GetUnit(guid);
            if (!unit || !unit->IsAlive())
                continue;

            if (unit->GetVictim() == bot || (tank && unit->GetExactDist(tank) <= CLOSE_THREAT_TANK_PILE_REACH))
            {
                anyClose = true;
                continue;
            }

            anyFar = true;
            narrowed.insert(guid);
        }
        return anyClose && anyFar;
    }

    // A melee dps leaves to the tank a loose mob (on nobody of the tank, the bot or a healer) standing further than
    // this from the tank: its caster victim walks it to the tank ("chased away from tank"), a melee walking to it
    // drags the fight out of the tank's reach. Stockade 2026-10-24: a rogue ran 11 yd out to a mob on the priest
    // 24 yd from the tank, took it and two more, and died there out of the tank's sight and the healer's range.
    constexpr float MELEE_LOOSE_MOB_TANK_REACH = 15.0f;

    void ExcludeLooseMobsAwayFromTank(PlayerbotAI* botAI, Player* bot, GuidSet& exclusions)
    {
        Map* map = bot->GetMap();
        if (!sPlayerbotAIConfig.RangedFleeToTank || !map || !map->IsNonRaidDungeon() || PlayerbotAI::IsTank(bot) ||
            PlayerbotAI::IsRanged(bot) || PlayerbotAI::IsHeal(bot) || !KillOrder::IsTrashFight(bot))
            return;

        Player* tank = KillOrder::FindFightTank(bot);
        if (!tank)
            return;

        for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
        {
            if (exclusions.find(guid) != exclusions.end())
                continue;

            Unit* unit = botAI->GetUnit(guid);
            Creature* creature = unit ? unit->ToCreature() : nullptr;
            if (!creature || !creature->IsAlive() || creature->IsDungeonBoss() || creature->isWorldBoss() ||
                creature->GetExactDist(tank) <= MELEE_LOOSE_MOB_TANK_REACH)
                continue;

            Unit* victim = creature->GetVictim();
            Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
            if (victim == bot || victim == tank || (victimPlayer && PlayerbotAI::IsHeal(victimPlayer)) ||
                IsRunningForHelp(creature))
                continue;

            exclusions.insert(guid);
        }
    }

    // Never before the tank (user, Stockade 2026-10-24: «Рога полетел вперед, хотя не должен был»; the rogue reached
    // a Defias Captive nobody fought and took its pack). In a 5-man with a tank a dps opens only on a mob that fights
    // the group already: on the tank (the tank has it) or on another member (a peel); never on an idle mob, nor on one
    // in combat with nobody of the group. A mob an encounter script names, a totem, and a mob running for help are
    // fought anyway.
    bool WaitsForTank(PlayerbotAI* botAI, Player* bot, Unit* unit)
    {
        Map* map = bot->GetMap();
        if (!unit || !map || !map->IsNonRaidDungeon() || PlayerbotAI::IsTank(bot) || !KillOrder::FindFightTank(bot))
            return false;

        if (unit->IsTotem() || IsRunningForHelp(unit))
            return false;

        GuidVector const& prioritized =
            botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->RefGet();
        if (std::find(prioritized.begin(), prioritized.end(), unit->GetGUID()) != prioritized.end())
            return false;

        if (!unit->IsInCombat())
            return true;

        Unit* victim = unit->GetVictim();
        Player* owner = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (owner && bot->IsInSameRaidWith(owner))
            return false;

        // [b32_engage] A mob the tank pulled that cannot get to it (in the water, on a ledge: no victim, the tank on
        // its threat list, not evading) is the ranged dps' to shoot: ub-h-5F-64 20261025-221741 pull #30, the
        // Underbog Frenzy the tank shot stayed in the water with no victim and the warlock / hunter 13-17 yd away
        // "followed" for 30 s. Melee leave it: walking in after it takes the fight away from the tank.
        Creature* creature = unit->ToCreature();
        Player* tank = KillOrder::FindFightTank(bot);
        return victim || !PlayerbotAI::IsRanged(bot) || PlayerbotAI::IsHeal(bot) || !creature ||
               creature->IsInEvadeMode() || !tank || unit->GetThreatMgr().GetThreat(tank) <= 0.0f;
    }

    void ExcludeNotYetTanked(PlayerbotAI* botAI, Player* bot, GuidSet& exclusions)
    {
        for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
        {
            if (exclusions.find(guid) == exclusions.end() && WaitsForTank(botAI, bot, botAI->GetUnit(guid)))
                exclusions.insert(guid);
        }
    }
}

class FindMaxThreatGapTargetStrategy : public FindTargetStrategy
{
public:
    FindMaxThreatGapTargetStrategy(PlayerbotAI* botAI) : FindTargetStrategy(botAI), minThreat(0) {}

    void CheckAttacker(Unit* attacker, ThreatManager* threatMgr) override
    {
        if (!attacker->IsAlive())
            return;

        if (foundHighPriority)
            return;

        if (IsHighPriority(attacker))
        {
            result = attacker;
            foundHighPriority = true;
            return;
        }
        if (!result || CalcThreatGap(attacker, threatMgr) > CalcThreatGap(result, &result->GetThreatMgr()))
            result = attacker;
    }
    float CalcThreatGap(Unit* attacker, ThreatManager* threatMgr)
    {
        Unit* victim = attacker->GetVictim();
        return threatMgr->GetThreat(victim) - threatMgr->GetThreat(attacker);
    }

protected:
    float minThreat;
};

// caster
class CasterFindTargetSmartStrategy : public FindTargetStrategy
{
public:
    CasterFindTargetSmartStrategy(PlayerbotAI* botAI, float dps)
        : FindTargetStrategy(botAI), dps_(dps), targetExpectedLifeTime(1000000)
    {
        result = nullptr;
    }

    void CheckAttacker(Unit* attacker, ThreatManager* /*threatMgr*/) override
    {
        if (Group* group = botAI->GetBot()->GetGroup())
        {
            ObjectGuid guid = group->GetTargetIcon(4);
            if (guid && attacker->GetGUID() == guid)
                return;
        }
        if (!attacker->IsAlive())
            return;

        if (foundHighPriority)
            return;

        if (IsHighPriority(attacker))
        {
            result = attacker;
            foundHighPriority = true;
            return;
        }
        // [lead1300] its health now over the group's formula dps (no observed health loss: a measured rate)
        float expectedLifeTime = LifeTime(attacker);
        // Unit* victim = attacker->GetVictim();
        if (!result || IsBetter(attacker, result))
        {
            targetExpectedLifeTime = expectedLifeTime;
            result = attacker;
        }
    }
    float LifeTime(Unit* unit) const
    {
        return static_cast<float>(unit->GetHealth()) / dps_;
    }
    bool IsBetter(Unit* new_unit, Unit* old_unit)
    {
        float new_time = LifeTime(new_unit);
        float old_time = LifeTime(old_unit);
        // [5-30] > (5-0] > (20-inf)
        int new_level = GetIntervalLevel(new_unit);
        int old_level = GetIntervalLevel(old_unit);
        if (new_level != old_level)
            return new_level > old_level;

        int32_t level = new_level;
        if (level % 10 == 2 || level % 10 == 0)
            return new_time < old_time;
        // dont switch targets when all of them with low health
        Unit* currentTarget = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
        if (currentTarget == new_unit)
            return true;

        if (currentTarget == old_unit)
            return false;

        return new_time > old_time;
    }
    int32_t GetIntervalLevel(Unit* unit)
    {
        float time = LifeTime(unit);
        float dis = unit->GetDistance(botAI->GetBot());
        float attackRange = PlayerbotAI::IsRanged(botAI->GetBot()) ? sPlayerbotAIConfig.SpellDistance
                                                                   : sPlayerbotAIConfig.MeleeDistance;
        attackRange += 5.0f;
        int level = dis < attackRange ? 10 : 0;
        if (time >= 5 && time <= 30)
            return level + 2;

        if (time > 30)
            return level;

        return level + 1;
    }

protected:
    float dps_;
    float targetExpectedLifeTime;
};

// General
class GeneralFindTargetSmartStrategy : public FindTargetStrategy
{
public:
    GeneralFindTargetSmartStrategy(PlayerbotAI* botAI, float dps)
        : FindTargetStrategy(botAI), dps_(dps), targetExpectedLifeTime(1000000)
    {
    }

    void CheckAttacker(Unit* attacker, ThreatManager*) override
    {
        if (Group* group = botAI->GetBot()->GetGroup())
        {
            ObjectGuid guid = group->GetTargetIcon(4);
            if (guid && attacker->GetGUID() == guid)
                return;
        }
        if (!attacker->IsAlive())
            return;

        if (foundHighPriority)
            return;

        if (IsHighPriority(attacker))
        {
            result = attacker;
            foundHighPriority = true;
            return;
        }
        float expectedLifeTime = attacker->GetHealth() / dps_;
        // Unit* victim = attacker->GetVictim();
        if (!result || IsBetter(attacker, result))
        {
            targetExpectedLifeTime = expectedLifeTime;
            result = attacker;
        }
    }
    bool IsBetter(Unit* new_unit, Unit* old_unit)
    {
        float new_time = new_unit->GetHealth() / dps_;
        float old_time = old_unit->GetHealth() / dps_;
        int new_level = GetIntervalLevel(new_unit);
        int old_level = GetIntervalLevel(old_unit);
        if (new_level != old_level)
        {
            return new_level > old_level;
        }
        // attack enemy in range and with lowest health
        int level = new_level;
        if (level == 10)
            return new_time < old_time;

        // all targets are far away, choose the closest one
        return botAI->GetBot()->GetDistance(new_unit) < botAI->GetBot()->GetDistance(old_unit);
    }
    int32_t GetIntervalLevel(Unit* unit)
    {
        float dis = unit->GetDistance(botAI->GetBot());
        float attackRange = PlayerbotAI::IsRanged(botAI->GetBot()) ? sPlayerbotAIConfig.SpellDistance
                                                                   : sPlayerbotAIConfig.MeleeDistance;
        attackRange += 5.0f;
        int level = dis < attackRange ? 10 : 0;
        return level;
    }

protected:
    float dps_;
    float targetExpectedLifeTime;
};

// combo
class ComboFindTargetSmartStrategy : public FindTargetStrategy
{
public:
    ComboFindTargetSmartStrategy(PlayerbotAI* botAI, float dps)
        : FindTargetStrategy(botAI), dps_(dps), targetExpectedLifeTime(1000000)
    {
    }

    void CheckAttacker(Unit* attacker, ThreatManager*) override
    {
        if (Group* group = botAI->GetBot()->GetGroup())
        {
            ObjectGuid guid = group->GetTargetIcon(4);
            if (guid && attacker->GetGUID() == guid)
                return;
        }
        if (!attacker->IsAlive())
            return;

        if (foundHighPriority)
            return;

        if (IsHighPriority(attacker))
        {
            result = attacker;
            foundHighPriority = true;
            return;
        }
        float expectedLifeTime = attacker->GetHealth() / dps_;
        // Unit* victim = attacker->GetVictim();
        if (!result || IsBetter(attacker, result))
        {
            targetExpectedLifeTime = expectedLifeTime;
            result = attacker;
        }
    }
    bool IsBetter(Unit* new_unit, Unit* old_unit)
    {
        float new_time = new_unit->GetHealth() / dps_;
        float old_time = old_unit->GetHealth() / dps_;
        // [5-20] > (5-0] > (20-inf)
        int new_level = GetIntervalLevel(new_unit);
        int old_level = GetIntervalLevel(old_unit);
        if (new_level != old_level)
            return new_level > old_level;

        // attack enemy in range and with lowest health
        int level = new_level;
        Player* bot = botAI->GetBot();
        if (level == 10)
        {
            Unit* combo_unit = bot->GetComboTarget();
            if (new_unit == combo_unit)
                return true;

            return new_time < old_time;
        }
        // all targets are far away, choose the closest one
        return bot->GetDistance(new_unit) < bot->GetDistance(old_unit);
    }
    int32_t GetIntervalLevel(Unit* unit)
    {
        float dis = unit->GetDistance(botAI->GetBot());
        float attackRange = PlayerbotAI::IsRanged(botAI->GetBot()) ? sPlayerbotAIConfig.SpellDistance
                                                                   : sPlayerbotAIConfig.MeleeDistance;
        attackRange += 5.0f;
        int level = dis < attackRange ? 10 : 0;
        return level;
    }

protected:
    float dps_;
    float targetExpectedLifeTime;
};

Unit* DpsTargetValue::Calculate()
{
    GuidSet const exclusions = GatherStrategyTargetExclusions(botAI, TargetValueExclusionType::DpsTarget);

    // A mob that would turn this bot's attacks back on it (a reflected spell, a counterattack on every melee swing)
    // is left while that lasts if anything else is there to hit; otherwise the bot holds its attacks on it.
    bool const avoidCounter = PlayerbotAI::IsMelee(bot) && !PlayerbotAI::IsTank(bot);
    bool const avoidReflect = PlayerbotAI::IsCaster(bot);
    GuidSet avoided = exclusions;
    if (avoidCounter || avoidReflect)
    {
        for (ObjectGuid const& guid : AI_VALUE(GuidVector, "attackers"))
        {
            Unit* unit = botAI->GetUnit(guid);
            if (unit && ((avoidCounter && ReflectAuras::CountersMelee(unit, bot)) ||
                         (avoidReflect && ReflectAuras::ReflectsSpells(unit))))
                avoided.insert(guid);
        }
    }

    if (avoided.size() != exclusions.size())
    {
        if (Unit* target = Pick(avoided))
            return target;
    }

    return Pick(exclusions);
}

Unit* DpsTargetValue::Pick(GuidSet const& given)
{
    // [b30_mech_pos] a runner low on health is finished before any mark: it reaches its helpers and gets healed
    // otherwise (zf-h 20261025-184229: Sandfury Blood Drinkers fled at 15 % 20 times, the Witch Doctors healed them,
    // 287 s, 0 kills). The skull kept the ranged on another mob while the runner went.
    constexpr float RUNNER_FINISH_PCT = 30.0f;
    if (PlayerbotAI::IsRanged(bot))
        for (ObjectGuid const guid : AI_VALUE(GuidVector, "attackers"))
        {
            Unit* unit = botAI->GetUnit(guid);
            if (unit && unit->IsAlive() && given.find(guid) == given.end() &&
                unit->GetHealthPct() <= RUNNER_FINISH_PCT && IsRunningForHelp(unit) &&
                bot->IsWithinDistInMap(unit, HELP_RUNNER_RANGE) && bot->IsWithinLOSInMap(unit))
                return unit;
        }

    Unit* rti = RtiTargetValue::Calculate();
    // [b33_hadronox] In a 5-man boss fight the tank's own automatic Skull / Cross on an add does not pull the damage
    // dealers off the boss (KillOrder::BossFocusTarget); a mark put out on purpose still counts.
    Creature* rtiCreature = rti ? rti->ToCreature() : nullptr;
    if (rtiCreature && !rtiCreature->IsDungeonBoss() && !rtiCreature->isWorldBoss() &&
        KillOrder::IsTankOwnKillMark(bot, rti->GetGUID()) && KillOrder::FindFightBoss(botAI, given))
        rti = nullptr;
    // A mark left on a mob that has not joined this fight is no reason to pull it; script-owned marks always count.
    // The tank's own kill mark on a mob out in the room waits for it to come (KillOrder::IsInGroupFight).
    if (rti && given.find(rti->GetGUID()) == given.end() &&
        (rti->IsInCombat() || !bot->IsInCombat() || RaidMarkOwnership::IsOwned(bot)) &&
        KillOrder::IsInGroupFight(bot, rti) && (RaidMarkOwnership::IsOwned(bot) || !WaitsForTank(botAI, bot, rti)))
        return rti;

    if (PlayerbotAI::IsRanged(bot))
    {
        for (ObjectGuid const guid : AI_VALUE(GuidVector, "attackers"))
        {
            Unit* unit = botAI->GetUnit(guid);
            if (unit && unit->IsAlive() && given.find(guid) == given.end() && IsRunningForHelp(unit) &&
                bot->IsWithinDistInMap(unit, HELP_RUNNER_RANGE) && bot->IsWithinLOSInMap(unit))
                return unit;
        }
    }

    // 5-man dungeons: what the tank's fight holds, never a mob deep in the room that is in combat but not at the
    // tank or on anyone of the group (Scholomance wave 29: a Researcher 50 yd from the tank drew two more packs).
    GuidSet exclusions = given;
    KillOrder::ExcludeOutsideGroupFight(botAI, exclusions);
    ExcludeLooseMobsAwayFromTank(botAI, bot, exclusions);
    ExcludeNotYetTanked(botAI, bot, exclusions);

    float dps = AI_VALUE(float, "estimated group dps");

    auto const findTarget = [this, dps](GuidSet const& skip) -> Unit*
    {
        if (botAI->GetNearGroupMemberCount() > 3)
        {
            if (PlayerbotAI::IsCaster(bot))
            {
                // Caster find target strategy avoids casting spells on enemies
                // with too low health to ensure the effectiveness of casting
                CasterFindTargetSmartStrategy strategy(botAI, dps);
                return TargetValue::FindTarget(&strategy, skip);
            }
            else if (PlayerbotAI::IsCombo(bot))
            {
                ComboFindTargetSmartStrategy strategy(botAI, dps);
                return TargetValue::FindTarget(&strategy, skip);
            }
        }
        GeneralFindTargetSmartStrategy strategy(botAI, dps);
        return TargetValue::FindTarget(&strategy, skip);
    };

    // Stay on the tank's pack, casters first, and leave a mob that is still loose at the pull to the tank.
    GuidSet narrowed;
    KillOrder::PackFocus const focus = KillOrder::NarrowToTankPack(botAI, exclusions, narrowed);
    // [b33_hadronox] A boss fight with adds: once the tank's opening lead allows it (Wait above), the single target
    // is the boss and the adds are the area spells' (KillOrder::BossFocusTarget). Without it the casters' lifetime
    // choice (CasterFindTargetSmartStrategy: a 5-30 s life first) prefers every fresh add over a long-lived boss.
    if (focus != KillOrder::PackFocus::Wait)
        if (Unit* boss = KillOrder::BossFocusTarget(botAI, exclusions))
            return boss;

    switch (focus)
    {
        case KillOrder::PackFocus::Wait:
            return nullptr;
        case KillOrder::PackFocus::Narrowed:
            if (Unit* target = findTarget(narrowed))
                return target;
            break;
        case KillOrder::PackFocus::None:
            break;
    }

    // [b33_aoe_class] No strategy exclusions (`given` empty): a mob on this bot or in the tank's pile before a
    // distant priority mob; the plain choice below when none is close.
    if (given.empty())
    {
        GuidSet close;
        if (NarrowToCloseThreats(botAI, bot, exclusions, close))
            if (Unit* target = findTarget(close))
                return target;
    }

    if (Unit* target = findTarget(exclusions))
        return target;

    // Every strategy above leaves the moon mob to its crowd control; with nothing else left and the mob free on the
    // group, the group kills it (wave 11 Shadow Labyrinth: nobody fought it for 82 s and the shaman died).
    return FreeCrowdControlMark(botAI, bot, exclusions);
}

class FindMaxHpTargetStrategy : public FindTargetStrategy
{
public:
    FindMaxHpTargetStrategy(PlayerbotAI* botAI) : FindTargetStrategy(botAI), maxHealth(0) {}

    void CheckAttacker(Unit* attacker, ThreatManager*) override
    {
        if (Group* group = botAI->GetBot()->GetGroup())
        {
            ObjectGuid guid = group->GetTargetIcon(4);
            if (guid && attacker->GetGUID() == guid)
                return;
        }

        if (!result || result->GetHealth() < attacker->GetHealth())
            result = attacker;
    }

protected:
    float maxHealth;
};

Unit* DpsAoeTargetValue::Calculate()
{
    GuidSet exclusions = GatherStrategyTargetExclusions(botAI, TargetValueExclusionType::DpsTarget);
    Unit* rti = RtiTargetValue::Calculate();
    if (rti && exclusions.find(rti->GetGUID()) == exclusions.end() && KillOrder::IsInGroupFight(bot, rti))
        return rti;

    KillOrder::ExcludeOutsideGroupFight(botAI, exclusions);
    FindMaxHpTargetStrategy strategy(botAI);
    return TargetValue::FindTarget(&strategy, exclusions);
}
