/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TargetValue.h"
#include "BotPath.h"  // [lead109]
#include "TerrainHazard.h"
#include "CombatManager.h"
#include "LastMovementValue.h"
#include "ObjectGuid.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "RtiTargetValue.h"
#include "ScriptedCreature.h"
#include "Strategy.h"
#include "ThreatManager.h"

#include <algorithm>

GuidSet GatherStrategyTargetExclusions(PlayerbotAI* botAI, TargetValueExclusionType type)
{
    GuidSet exclusions;
    if (!botAI || !botAI->HasTargetExclusions())
        return exclusions;

    for (auto const& strategyName : botAI->GetStrategies(BOT_STATE_COMBAT))
    {
        Strategy* strategy = botAI->GetStrategy(strategyName, BOT_STATE_COMBAT);
        if (!strategy)
            continue;

        strategy->AppendTargetExclusions(exclusions, type);
    }

    return exclusions;
}

namespace
{
    constexpr uint64 BREAKABLE_CROWD_CONTROL_MECHANICS =
        (1ULL << MECHANIC_POLYMORPH) | (1ULL << MECHANIC_SAPPED) | (1ULL << MECHANIC_FREEZE) |
        (1ULL << MECHANIC_SHACKLE) | (1ULL << MECHANIC_SLEEP) | (1ULL << MECHANIC_BANISH) |
        (1ULL << MECHANIC_KNOCKOUT) | (1ULL << MECHANIC_CHARM);
    // Charm is Seduction. Not disorient: Dragon's Breath and Scatter Shot put it on mobs the group still kills.
    // A spell this small is placed on or around its target, so a crowd-controlled unit inside means it breaks.
    // Larger ranges are target counts around the caster (channel cancel checks), not the spell area.
    constexpr float AOE_CROWD_CONTROL_VETO_RANGE = 20.0f;
}

bool IsCrowdControlled(Unit const* unit)
{
    if (!unit || !unit->IsAlive())
        return false;

    // cheap pre-check: every mechanic above applies a stun, confuse or transform aura
    if (!unit->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED) && !unit->HasAuraType(SPELL_AURA_TRANSFORM))
        return false;

    return unit->HasAuraWithMechanic(BREAKABLE_CROWD_CONTROL_MECHANICS);
}

bool HasEnoughAoeTargets(PlayerbotAI* botAI, Position const& center, float range, uint32 minCount)
{
    GuidVector attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    auto const isInRange = [&center, range](Unit* unit)
    { return unit && unit->IsAlive() && unit->GetDistance(center) <= range; };
    bool const vetoOnCrowdControl = range <= AOE_CROWD_CONTROL_VETO_RANGE;

    uint32 count = 0;
    for (ObjectGuid const guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!isInRange(unit))
            continue;

        if (IsCrowdControlled(unit))
        {
            if (vetoOnCrowdControl)
                return false;

            continue;
        }

        ++count;
    }

    if (count < minCount || !botAI->HasTargetExclusions())
        return count >= minCount;

    GuidSet const exclusions = GatherStrategyTargetExclusions(botAI, TargetValueExclusionType::Aoe);
    for (ObjectGuid const guid : attackers)
    {
        if (exclusions.find(guid) == exclusions.end())
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (isInRange(unit) && !IsCrowdControlled(unit))
            --count;
    }

    return count >= minCount;
}

Unit* FindTargetStrategy::GetResult() { return result; }

Unit* TargetValue::FindTarget(FindTargetStrategy* strategy, GuidSet const& exclusions)
{
    GuidVector attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || exclusions.find(guid) != exclusions.end())
            continue;

        ThreatManager& threatMgr = unit->GetThreatMgr();
        strategy->CheckAttacker(unit, &threatMgr);
    }

    return strategy->GetResult();
}

bool FindNonCcTargetStrategy::IsCcTarget(Unit* attacker)
{
    if (Group* group = botAI->GetBot()->GetGroup())
    {
        Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
        for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
        {
            Player* member = ObjectAccessor::FindPlayer(itr->guid);
            if (!member || !member->IsAlive())
                continue;

            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(member))
            {
                if (botAI->GetAiObjectContext()->GetValue<Unit*>("rti cc target")->Get() == attacker)
                    return true;

                std::string const rti = botAI->GetAiObjectContext()->GetValue<std::string>("rti cc")->Get();
                int32 index = RtiTargetValue::GetRtiIndex(rti);
                if (index != -1)
                {
                    if (ObjectGuid guid = group->GetTargetIcon(index))
                        if (attacker->GetGUID() == guid)
                            return true;
                }
            }
        }

        if (ObjectGuid guid = group->GetTargetIcon(4))
            if (attacker->GetGUID() == guid)
                return true;
    }

    return false;
}

void FindTargetStrategy::GetPlayerCount(Unit* creature, uint32* tankCount, uint32* dpsCount)
{
    Player* bot = botAI->GetBot();
    if (tankCountCache.find(creature) != tankCountCache.end())
    {
        *tankCount = tankCountCache[creature];
        *dpsCount = dpsCountCache[creature];
        return;
    }

    *tankCount = 0;
    *dpsCount = 0;

    Unit::AttackerSet attackers(creature->getAttackers());
    for (Unit* attacker : attackers)
    {
        if (!attacker || !attacker->IsAlive() || attacker == bot)
            continue;

        Player* player = attacker->ToPlayer();
        if (!player)
            continue;

        if (PlayerbotAI::IsTank(player))
            ++(*tankCount);
        else
            ++(*dpsCount);
    }

    tankCountCache[creature] = *tankCount;
    dpsCountCache[creature] = *dpsCount;
}

bool FindTargetStrategy::IsHighPriority(Unit* attacker)
{
    if (Group* group = botAI->GetBot()->GetGroup())
    {
        ObjectGuid guid = group->GetTargetIcon(7);
        if (guid && attacker->GetGUID() == guid)
        {
            return true;
        }
    }
    GuidVector prioritizedTargets = botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Get();
    for (ObjectGuid targetGuid : prioritizedTargets)
    {
        if (targetGuid && attacker->GetGUID() == targetGuid)
        {
            return true;
        }
    }
    return false;
}

WorldPosition LastLongMoveValue::Calculate()
{
    LastMovement& lastMove = *context->GetValue<LastMovement&>("last movement");
    if (lastMove.lastPath.empty())
        return WorldPosition();

    return lastMove.lastPath.getBack();
}

WorldPosition HomeBindValue::Calculate()
{
    return WorldPosition(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ, 0.f);
}

Unit* FindTargetValue::Calculate()
{
    if (qualifier == "")
    {
        return nullptr;
    }
    Group* group = bot->GetGroup();
    if (!group)
    {
        return nullptr;
    }
    for (auto const& [guid, ref] : bot->GetThreatMgr().GetThreatenedByMeList())
    {
        Unit* unit = ref->GetOwner();
        if (!unit)
            continue;

        std::wstring wnamepart;
        Utf8toWStr(unit->GetName(), wnamepart);
        wstrToLower(wnamepart);
        if (!qualifier.empty() && qualifier.length() == wnamepart.length() && Utf8FitTo(qualifier, wnamepart))
            return unit;
    }

    return nullptr;
}

void FindBossTargetStrategy::CheckAttacker(Unit* attacker, ThreatManager* /*threatManager*/)
{
    UnitAI* unitAI = attacker->GetAI();
    BossAI* bossAI = dynamic_cast<BossAI*>(unitAI);
    if (bossAI)
    {
        result = attacker;
    }
}

Unit* BossTargetValue::Calculate()
{
    FindBossTargetStrategy strategy(botAI);
    return FindTarget(&strategy);
}

namespace
{
    constexpr uint32 REACHABILITY_CACHE_MS = 10 * IN_MILLISECONDS;
    constexpr size_t REACHABILITY_CACHE_MAX = 64;
    constexpr float REACHABILITY_END_SLACK = 5.0f;    // the path may stop this far short of the mob
    constexpr float DETOUR_FACTOR = 3.0f;             // a path this many times the straight line goes round a floor
    constexpr float DETOUR_SLACK = 40.0f;
}

bool TargetReachability::IsReachable(Player* bot, Unit* unit)
{
    if (!bot || !unit)
        return false;

    uint32 const now = getMSTime();
    auto const it = _cache.find(unit->GetGUID());
    if (it != _cache.end() && getMSTimeDiff(it->second.checkedAt, now) < REACHABILITY_CACHE_MS)
        return it->second.reachable;

    if (_cache.size() >= REACHABILITY_CACHE_MAX)
        _cache.clear();

    bool reachable = true;
    // Flying or swimming movers do not use the navmesh; the height filter of the caller covers them.
    if (!unit->IsFlying() && !bot->IsFlying() && !bot->IsInWater() && !unit->IsInWater())
    {
        BotPath const path =  // [lead109]
            FindBotPath(bot, unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ());
        PathType const type = path.type;
        if (type & PATHFIND_NOT_USING_PATH)
            reachable = true;
        else if (type & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORT))
            reachable = false;
        else
        {
            G3D::Vector3 const& end = path.actualEnd;
            float const straight = bot->GetExactDist(unit);
            float const reach = bot->GetCombatReach() + unit->GetCombatReach() + REACHABILITY_END_SLACK;
            reachable = unit->GetExactDist(end.x, end.y, end.z) <= reach &&
                        path.length <= std::max(straight * DETOUR_FACTOR, straight + DETOUR_SLACK);
        }

        // [ledge_drop] a mob below the bot is reachable only by a ground path or a justified drop (JudgeLedgeDrop:
        // the landing joined to the rest, a fall the group takes): dtk-n 20261025-163758-62, the tank walked off
        // the upper level after a Death Knight on a navmesh island 18-34 yd below.
        if (reachable && bot->GetPositionZ() - unit->GetPositionZ() > LEDGE_DROP_DZ &&
            !JudgeLedgeDrop(bot, unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(), "pull target")
                 .allowed)
            reachable = false;
    }

    _cache[unit->GetGUID()] = {reachable, now};
    return reachable;
}
