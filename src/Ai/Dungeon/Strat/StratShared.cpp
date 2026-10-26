/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "StratShared.h"

#include <algorithm>

#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "Playerbots.h"

namespace Stratholme
{
bool InStratholme(Player* bot) { return bot && bot->GetMapId() == MAP_STRATHOLME; }

Player* GroupTank(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    Player* tank = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || member->GetMap() != bot->GetMap() || !PlayerbotAI::IsTank(member))
            continue;

        if (!tank || PlayerbotAI::IsMainTank(member))
            tank = member;
    }

    return tank == bot ? nullptr : tank;
}

namespace
{
// Creature::MoveInLineOfSight aggroes within GetAggroRange (+ its combat distance); a wanderer adds its leash.
float IdleReach(Creature* mob, Player* bot)
{
    float wander = 0.0f;
    if (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
        wander = std::min(mob->GetWanderDistance(), IDLE_WANDER_MAX);

    return mob->GetAggroRange(bot) + mob->m_CombatDistance + wander + IDLE_AGGRO_MARGIN;
}

bool CanAggro(Creature* mob, Player* bot)
{
    return mob->IsAlive() && !mob->IsInCombat() && !mob->IsInEvadeMode() && mob->IsHostileTo(bot) &&
           !mob->IsCivilian() && mob->GetReactState() == REACT_AGGRESSIVE &&
           !mob->HasUnitFlag(UNIT_FLAG_IMMUNE_TO_PC) && !mob->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) &&
           !mob->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) && !mob->IsCritter();
}
}  // namespace

std::vector<IdleMob> CollectIdleMobs(PlayerbotAI* botAI)
{
    std::vector<IdleMob> mobs;
    Player* bot = botAI->GetBot();
    GuidVector const units = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();
    for (ObjectGuid const& guid : units)
    {
        if (!guid.IsCreature())
            continue;

        Creature* mob = bot->GetMap()->GetCreature(guid);
        if (!mob || !CanAggro(mob, bot))
            continue;

        float const reach = IdleReach(mob, bot);
        // only what a step of the group can reach matters
        if (bot->GetExactDist2d(mob) > reach + 20.0f)
            continue;

        mobs.push_back({mob, reach});
    }
    return mobs;
}

bool SpotDrawsAggro(Player* bot, Player* tank, std::vector<IdleMob> const& mobs, float x, float y, float z)
{
    float const eyeZ = z + bot->GetCollisionHeight();
    for (IdleMob const& idle : mobs)
    {
        float const dist = idle.mob->GetExactDist2d(x, y);
        if (dist > idle.reach)
            continue;

        // the tank, deeper in this mob's reach, meets it first
        if (tank && tank->GetExactDist2d(idle.mob) + TANK_LEAD <= dist)
            continue;

        if (idle.mob->IsWithinLOS(x, y, eyeZ))
            return true;
    }
    return false;
}

Unit* FindSwarmer(Player* tank, Player*& victim)
{
    victim = nullptr;
    Group* group = tank->GetGroup();
    if (!group)
        return nullptr;

    Unit* best = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == tank || !member->IsAlive() || member->GetMap() != tank->GetMap() ||
            PlayerbotAI::IsTank(member) || tank->GetExactDist(member) > DEFEND_RANGE)
            continue;

        for (Unit* attacker : member->getAttackers())
        {
            if (!attacker || !attacker->IsAlive() || attacker->GetVictim() != member ||
                !tank->IsValidAttackTarget(attacker) || tank->GetExactDist(attacker) > DEFEND_RANGE)
                continue;

            if (!best || tank->GetExactDist(attacker) < tank->GetExactDist(best))
            {
                best = attacker;
                victim = member;
            }
        }
    }
    return best;
}
}  // namespace Stratholme
