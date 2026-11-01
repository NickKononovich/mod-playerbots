/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "PullFacts.h"

#include "AttackerCountValues.h"
#include "Creature.h"
#include "Group.h"
#include "CreatureGroups.h"
#include "GenericSpellActions.h"
#include "LeaderActions.h"
#include "LeaderKnowledge.h"
#include "Player.h"
#include "Playerbots.h"
#include "LeaderOrders.h"  // [lead108] Leader::Route
#include <algorithm>

namespace Leader::Fight
{
    Unit* WalkInWakes(Player* tank, Unit* target, std::vector<ObjectGuid> const& planMobs)
    {
        PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        Creature* creature = target ? target->ToCreature() : nullptr;
        if (!tankAI || !creature)
            return nullptr;
        auto* mover =
            dynamic_cast<LeaderMoveOrderAction*>(tankAI->GetAiObjectContext()->GetAction("leader move order"));
        if (!mover)
            return nullptr;
        auto const counts = [&](Unit* woken)
        {
            return woken && woken->ToCreature() &&
                   std::find(planMobs.begin(), planMobs.end(), woken->GetGUID()) == planMobs.end() &&
                   !(creature->GetFormation() && woken->ToCreature()->GetFormation() == creature->GetFormation());
        };
        if (Unit* woken = mover->WalkWakes(target->GetPosition(), target->GetGUID()); counts(woken))
            return woken;
        // [lead113] b147 rfc-h 59.9 s: the healer following the tank 12 yd behind woke Earthborer 4 (45 yd from the
        // fight spot) - the followers walk in too: each member's own walk to the target (its route search against
        // every idle pack's reach, its own aggro check) is judged the same way
        if (Group* group = tank->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                PlayerbotAI* memberAI = member && member != tank ? GET_PLAYERBOT_AI(member) : nullptr;
                if (!memberAI || !member->IsAlive() || member->GetMap() != tank->GetMap())
                    continue;
                if (auto* follower = dynamic_cast<LeaderMoveOrderAction*>(
                        memberAI->GetAiObjectContext()->GetAction("leader move order")))
                    if (Unit* woken = follower->WalkWakes(target->GetPosition(), target->GetGUID()); counts(woken))
                        return woken;
            }
        return nullptr;
    }

    bool OrderMobStaysPick(Player* tank, Creature* own)
    {
        return own && own->IsAlive() && !own->IsInCombat() && TankHasWalk(tank, own);
    }

    bool TankHasWalk(Player* tank, Unit* mob)
    {
        if (!tank || !mob)
            return false;
        Leader::RouteResult const path = Leader::Route(tank, mob->GetPosition());
        if (!path.found)
            return false;
        if (path.complete || path.points.empty())
            return path.complete;
        float const straight = tank->GetExactDist(mob);
        float const left =
            (path.points.back() - G3D::Vector3(mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ())).length();
        return left + 5.0f < straight;
    }

    bool NeedsFleeStop(Unit* mob)
    {
        if (!mob || !mob->IsAlive() || !mob->ToCreature() || mob->HasAuraType(SPELL_AURA_MOD_STUN) ||
            mob->HasAuraType(SPELL_AURA_MOD_ROOT) || mob->HasBreakableByDamageCrowdControlAura() ||
            FledMobs::StoppedRecently(mob->GetGUID()))
            return false;
        float const fleeAt = Leader::Knowledge::FleeForAssistHealthPct(mob->GetEntry());
        return (fleeAt > 0.0f && mob->GetHealthPct() <= fleeAt) || FledMobs::IsRunning(mob);
    }

    Player* FleeStopper(std::vector<Player*> const& members, Unit* mob)
    {
        for (Player* member : members)
        {
            PlayerbotAI* memberAI = member ? GET_PLAYERBOT_AI(member) : nullptr;
            if (!memberAI || !member->IsAlive() || PlayerbotAI::IsHeal(member))
                continue;
            if (auto* stop = dynamic_cast<FleeStopAction*>(memberAI->GetAiObjectContext()->GetAction("flee stop"));
                stop && stop->CanStop(mob))
                return member;
        }
        return nullptr;
    }

    bool MobComesToTank(Player* tank, Unit* mob)
    {
        if (!tank || !mob || !mob->IsAlive() || !mob->IsInCombat() || mob->GetVictim() != tank || !mob->isMoving() ||
            mob->HasUnitState(UNIT_STATE_ROOT) || tank->IsWithinMeleeRange(mob))
            return false;
        Creature* creature = mob->ToCreature();
        return !creature || !creature->CanNotReachTarget();
    }
}
