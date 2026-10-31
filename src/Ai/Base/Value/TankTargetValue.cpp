/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TankTargetValue.h"
#include "AiObjectContext.h"
#include "AttackersValue.h"
#include "Group.h"
#include "KillOrder.h"  // [b33_hadronox] TankHoldBoss
#include "MotionMaster.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RaidLeaderState.h"
#include "Strategy.h"
#include "StringFormat.h"
#include "Timer.h"

#include <mutex>
#include <unordered_map>

namespace
{
    // A patrol this much beyond its own aggro range of the tank is coming into the fight (one or two seconds of walk).
    constexpr float PATROL_COMING_MARGIN = 8.0f;
    constexpr uint32 SKIP_LOG_MS = 5 * IN_MILLISECONDS;

    // The member a unit fights for: itself, or the owner of a pet, totem or guardian.
    Player* GroupPlayerOf(Player* bot, Unit* unit)
    {
        if (!unit)
            return nullptr;

        Player* player = unit->GetCharmerOrOwnerPlayerOrPlayerItself();
        return player && (player == bot || bot->IsInSameRaidWith(player)) ? player : nullptr;
    }
}

namespace TankPullRules
{
    bool IsEngagedWithGroup(Player* bot, Unit* unit)
    {
        if (!unit || !unit->IsAlive() || !unit->IsInCombat())
            return false;

        if (GroupPlayerOf(bot, unit->GetVictim()))
            return true;

        if (unit->IsInCombatWith(bot))
            return true;

        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->GetMap() == bot->GetMap() && unit->IsInCombatWith(member))
                return true;
        }

        return false;
    }

    bool IsPatrolComing(Player* bot, Creature* creature)
    {
        return creature && creature->isMoving() &&
               creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE &&
               bot->GetExactDist(creature) <= creature->GetAggroRange(bot) + PATROL_COMING_MARGIN;
    }

    uint32 FightMobsLeft(PlayerbotAI* botAI, Unit* except)
    {
        Player* bot = botAI->GetBot();
        uint32 left = 0;
        for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
        {
            Unit* unit = botAI->GetUnit(guid);
            if (!unit || unit == except || !unit->IsAlive() || IsCrowdControlled(unit) ||
                !IsEngagedWithGroup(bot, unit))
                continue;

            ++left;
        }

        return left;
    }

    std::string GroupNotReady(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return std::string();

        bool hasHealer = false;
        bool healerUp = false;
        bool hasDps = false;
        bool dpsUp = false;
        std::string down;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot)
                continue;

            bool const here = member->GetMap() == bot->GetMap() && member->IsAlive();
            if (PlayerbotAI::IsTank(member))
                continue;

            if (PlayerbotAI::IsHeal(member))
            {
                hasHealer = true;
                healerUp |= here;
                if (!here && down.empty())
                    down = "healer " + member->GetName() + " dead";
            }
            else
            {
                hasDps = true;
                dpsUp |= here;
            }

            if (!here)
                continue;

            bool drinking = false;
            if (RaidLeader::IsEatingOrDrinking(member, drinking))
            {
                if (drinking && member->GetMaxPower(POWER_MANA) > 0)
                    return Acore::StringFormat("{} drinking {:.0f} %", member->GetName(),
                                               member->GetPowerPct(POWER_MANA));

                return Acore::StringFormat("{} eating {:.0f} %", member->GetName(), member->GetHealthPct());
            }
        }

        if (!bot->IsAlive())
            return "tank dead";

        if (hasHealer && !healerUp)
            return down;

        if (hasDps && !dpsUp)
            return "no damage dealer alive";

        return std::string();
    }

    std::string RefusesIdle(PlayerbotAI* botAI, Unit* unit, bool checkReady)
    {
        Player* bot = botAI->GetBot();
        if (!unit || unit->IsPlayer() || IsEngagedWithGroup(bot, unit) || IsPatrolComing(bot, unit->ToCreature()))
            return std::string();

        if (uint32 const left = FightMobsLeft(botAI, unit))
            return Acore::StringFormat("fight not over ({} left)", left);

        if (!checkReady)
            return std::string();

        std::string const notReady = GroupNotReady(bot);
        return notReady.empty() ? std::string() : "group not ready (" + notReady + ")";
    }

    void LogSkip(Player* bot, Unit* unit, std::string const& reason)
    {
        static std::mutex mutex;
        static std::unordered_map<uint64, std::pair<uint64, uint32>> last;  // tank -> mob, when
        uint32 const now = getMSTime();
        {
            std::lock_guard<std::mutex> lock(mutex);
            std::pair<uint64, uint32>& entry = last[bot->GetGUID().GetRawValue()];
            if (entry.first == unit->GetGUID().GetRawValue() && getMSTimeDiff(entry.second, now) < SKIP_LOG_MS)
                return;

            entry = {unit->GetGUID().GetRawValue(), now};
        }

        LOG_INFO("playerbots", "[tank target] {} skips idle {} ({:.1f} yd): {}", bot->GetName(), unit->GetName(),
                 bot->GetExactDist(unit), reason);
    }
}

namespace
{
    // Held by crowd control, or marked for it in a 5-man and not (or no longer) hitting anyone of the group: the
    // tank leaves it be, its own threat and area damage would break it.
    bool IsKeptForCrowdControl(Player* bot, Unit* unit)
    {
        if (IsCrowdControlled(unit))
            return true;

        Group* group = bot->GetGroup();
        Map* map = bot->GetMap();
        if (!group || !map || !map->IsNonRaidDungeon())
            return false;

        Unit* victim = unit->GetVictim();
        Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
        if (victimPlayer && bot->IsInSameRaidWith(victimPlayer))
            return false;

        ObjectGuid const guid = unit->GetGUID();
        for (uint8 icon = 0; icon < TARGETICONCOUNT; ++icon)
        {
            if (icon != RtiTargetValue::skullIndex && icon != RtiTargetValue::crossIndex &&
                group->GetTargetIcon(icon) == guid)
                return true;
        }

        return false;
    }

    // The mob hits the bot or one of its group.
    bool IsOnGroup(Player* bot, Unit* unit)
    {
        Unit* victim = unit->GetVictim();
        Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
        return victimPlayer && bot->IsInSameRaidWith(victimPlayer);
    }

    // Fled at low health to fetch help (SMART_ACTION_FLEE_FOR_ASSIST): the tank stays with the pack instead of
    // running after it into the next one (wave 11 Scarlet Monastery: three chases took the fight 75 yd down the
    // corridor, 2 planned mobs became 13).
    bool IsRunningForHelp(Unit* unit)
    {
        return unit->HasUnitState(UNIT_STATE_FLEEING) ||
               unit->GetMotionMaster()->GetCurrentMovementGeneratorType() == ASSISTANCE_MOTION_TYPE;
    }
}

class FindTargetForTankStrategy : public FindNonCcTargetStrategy
{
public:
    FindTargetForTankStrategy(PlayerbotAI* botAI) : FindNonCcTargetStrategy(botAI), minThreat(0) {}

    void CheckAttacker(Unit* creature, ThreatManager* threatMgr) override
    {
        if (!creature || !creature->IsAlive())
            return;

        Player* bot = botAI->GetBot();
        float threat = threatMgr->GetThreat(bot);
        if (!result)
        {
            minThreat = threat;
            result = creature;
        }
        // neglect if victim is main tank, or no victim (for untauntable target)
        if (Unit* victim = threatMgr->CanHaveThreatList() ? threatMgr->GetCurrentVictim() : creature->GetVictim())
        {
            if (victim->ToPlayer() && PlayerbotAI::IsMainTank(victim->ToPlayer()))
                return;
        }
        if (minThreat >= threat)
        {
            minThreat = threat;
            result = creature;
        }
    }

protected:
    float minThreat;
};

class FindTankTargetSmartStrategy : public FindTargetStrategy
{
public:
    // takeFreeMarks: a bit per raid icon whose crowd control mob hits the group unheld long enough
    // (TankTargetValue::FREE_MARK_TAKE_MS); the last pass takes any mob, held, marked or running.
    FindTankTargetSmartStrategy(PlayerbotAI* botAI, bool skipCrowdControl, uint8 takeFreeMarks)
        : FindTargetStrategy(botAI), _skipCrowdControl(skipCrowdControl), _takeFreeMarks(takeFreeMarks)
    {
    }

    void CheckAttacker(Unit* attacker, ThreatManager* /*threatMgr*/) override
    {
        if (!attacker->IsAlive())
            return;

        // "attackers" also lists the prioritized and the skull mob, fighting or not: an idle one is a new pull, and
        // that is the pull planner's (its gate), never the tank's target choice.
        Player* bot = botAI->GetBot();
        if (bot->GetMap()->IsNonRaidDungeon() && !attacker->IsPlayer() &&
            !TankPullRules::IsEngagedWithGroup(bot, attacker) &&
            !TankPullRules::IsPatrolComing(bot, attacker->ToCreature()))
        {
            std::string const refusal = TankPullRules::RefusesIdle(botAI, attacker, true);
            TankPullRules::LogSkip(bot, attacker, refusal.empty() ? "a new pull, left to the pull planner" : refusal);
            return;
        }

        if (_skipCrowdControl)
        {
            // [rot_b31_cc] every crowd control icon, not only the moon: the Hex (star), trap (square), Shackle and
            // Repentance (circle) mobs came at the tank and were its target before their spell landed, so the cast
            // was refused (build 30: 300+ "refuses crowd control ... the group kills it now (main tank target)")
            Group* group = botAI->GetBot()->GetGroup();
            for (uint8 icon = 0; group && icon < TARGETICONCOUNT; ++icon)
            {
                if (icon != RtiTargetValue::skullIndex && icon != RtiTargetValue::crossIndex &&
                    group->GetTargetIcon(icon) == attacker->GetGUID() && !(_takeFreeMarks & (1u << icon)))
                    return;
            }

            if (IsKeptForCrowdControl(botAI->GetBot(), attacker) || IsRunningForHelp(attacker))
                return;
        }

        if (!result || IsBetter(attacker, result))
            result = attacker;
    }
    bool IsBetter(Unit* new_unit, Unit* old_unit)
    {
        Player* bot = botAI->GetBot();
        // if group has multiple tanks, explicit main tank just focus on the current target
        Unit* currentTarget = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
        if (currentTarget && PlayerbotAI::IsExplicitMainTank(bot) && PlayerbotAI::GetGroupTankNum(bot) > 1)
        {
            if (old_unit == currentTarget)
                return false;

            if (new_unit == currentTarget)
                return true;
        }
        float new_threat = new_unit->GetThreatMgr().GetThreat(bot);
        float old_threat = old_unit->GetThreatMgr().GetThreat(bot);
        float new_dis = bot->GetDistance(new_unit);
        float old_dis = bot->GetDistance(old_unit);
        // hasAggro? -> withinMelee? -> threat
        if (GetIntervalLevel(new_unit) != GetIntervalLevel(old_unit))
            return GetIntervalLevel(new_unit) > GetIntervalLevel(old_unit);

        int32_t interval = GetIntervalLevel(new_unit);
        if (interval >= 2)
            return new_dis < old_dis;

        return new_threat < old_threat;
    }
    int32_t GetIntervalLevel(Unit* unit)
    {
        if (!botAI->HasAggro(unit))
        {
            // A mob on the healer or a damage dealer comes before one on a totem or pet.
            Player* bot = botAI->GetBot();
            Unit* victim = unit->GetVictim();
            Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
            if (victimPlayer && victimPlayer != bot && bot->IsInSameRaidWith(victimPlayer) &&
                !PlayerbotAI::IsTank(victimPlayer))
                return PlayerbotAI::IsHeal(victimPlayer) ? 4 : 3;

            return 2;
        }

        if (botAI->GetBot()->IsWithinMeleeRange(unit))
            return 1;

        return 0;
    }

private:
    bool _skipCrowdControl;
    uint8 _takeFreeMarks;
};

static_assert(TankTargetValue::ICON_COUNT == TARGETICONCOUNT, "one free-mark clock per raid icon");

Unit* TankTargetValue::Calculate()
{
    GuidSet const exclusions = GatherStrategyTargetExclusions(botAI, TargetValueExclusionType::TankTarget);
    // Out of combat the mob on a member first, read from the member, not from the cached "attackers".
    if (!bot->IsInCombat())
    {
        Unit* onMember = AI_VALUE(Unit*, "defend member target");
        if (onMember && exclusions.find(onMember->GetGUID()) == exclusions.end())
            return onMember;
    }

    std::string const rti = botAI->GetAiObjectContext()->GetValue<std::string>("rti")->Get();
    Unit* rtiTarget = RtiTargetValue::Calculate();
    if (rtiTarget && exclusions.find(rtiTarget->GetGUID()) == exclusions.end())
    {
        Unit* victim = rtiTarget->GetVictim();

        if (victim && victim != bot)
        {
            if (Player* victimPlayer = victim->ToPlayer())
            {
                // rti target is attacking a non-tank player
                if (!PlayerbotAI::IsTank(victimPlayer))
                    return rtiTarget;
                // rti target is attacking a tank player, check if the tank is a bot and has the same rti setting
                PlayerbotAI* victimBotAI = GET_PLAYERBOT_AI(victimPlayer);
                if (!victimBotAI || victimBotAI->GetAiObjectContext()->GetValue<std::string>("rti")->Get() != rti)
                    return rtiTarget;
            }
        }
    }

    // A crowd control mob (any icon but Skull and Cross) is left to its crowd control while that holds it or it fights
    // nobody of the group; one that hits the group unheld for FREE_MARK_TAKE_MS is taken like any other.
    // [rot_b31_cc] for every crowd control icon, each with its own clock (was the moon only)
    uint8 takeFreeMarks = 0;
    Group* group = bot->GetGroup();
    uint32 const now = getMSTime();
    for (uint8 icon = 0; icon < TARGETICONCOUNT; ++icon)
    {
        if (icon == RtiTargetValue::skullIndex || icon == RtiTargetValue::crossIndex)
            continue;

        Unit* mark = group ? botAI->GetUnit(group->GetTargetIcon(icon)) : nullptr;
        if (!mark || !mark->IsAlive() || IsCrowdControlled(mark) || !IsOnGroup(bot, mark))
        {
            _freeMarks[icon].Clear();
            continue;
        }

        if (_freeMarks[icon] != mark->GetGUID())
        {
            _freeMarks[icon] = mark->GetGUID();
            _freeMarkSinceMs[icon] = now;
        }

        if (getMSTimeDiff(_freeMarkSinceMs[icon], now) >= FREE_MARK_TAKE_MS)
            takeFreeMarks |= uint8(1u << icon);
    }

    // [b33_hadronox] A 5-man boss fight with adds: the tank stays on the boss and the adds come to it (taunt
    // reaction, Death Grip, its area threat), so they stand on the boss for the damage dealers' area spells. an-n
    // 20261026-000434 Hadronox: the tank walked from add to add (70 taunt reactions, its target an add in ~70 % of
    // the samples) and left her on the warrior 35 s and the mage 16 s. A mob on a group healer still comes first.
    if (bot->IsInCombat())
        if (Unit* boss = KillOrder::TankHoldBoss(botAI, exclusions))
            return boss;

    FindTankTargetSmartStrategy strategy(botAI, true, takeFreeMarks);
    if (Unit* target = FindTarget(&strategy, exclusions))
        return target;

    // nothing but crowd controlled, marked or fleeing mobs left: the last of the pack is the tank's after all
    FindTankTargetSmartStrategy remaining(botAI, false, 0xFF);
    return FindTarget(&remaining, exclusions);
}
