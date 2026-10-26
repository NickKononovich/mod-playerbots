/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ThreatValues.h"
#include "EstimatedLifetimeValue.h"
#include "Group.h"
#include "Playerbots.h"
#include "ThreatManager.h"

#include <algorithm>

uint8 ThreatValue::Calculate()
{
    if (qualifier == "aoe")
    {
        // Only mobs an area spell of this bot can hit (around its target or around itself): a loose mob on the
        // healer 30 yd away held every AoE on a pack the tank already holds.
        constexpr float AOE_THREAT_RADIUS = 12.0f;
        Unit* aoeCenter = AI_VALUE(Unit*, "current target");
        uint8 maxThreat = 0;
        GuidVector attackers = context->GetValue<GuidVector>("attackers")->Get();
        for (ObjectGuid const guid : attackers)
        {
            Unit* unit = botAI->GetUnit(guid);
            if (!unit || !unit->IsAlive())
                continue;

            if (!bot->IsWithinDist(unit, AOE_THREAT_RADIUS) &&
                (!aoeCenter || !aoeCenter->IsWithinDist(unit, AOE_THREAT_RADIUS)))
                continue;

            uint8 threat = Calculate(unit);
            if (!maxThreat || threat > maxThreat)
                maxThreat = threat;
        }

        return maxThreat;
    }

    Unit* target = AI_VALUE(Unit*, qualifier);
    return Calculate(target);
}

uint8 ThreatValue::Calculate(Unit* target)
{
    if (!target)
        return 0;

    if (target->GetGUID().IsPlayer())
        return 0;

    Group* group = bot->GetGroup();
    if (!group)
        return 0;

    float botThreat = target->GetThreatMgr().GetThreat(bot);

    // [b44_hos] A mob on a friendly NPC (Brann at the Tribunal of Ages, an escort): its threat lead is the NPC's, not
    // the bot tank's. hos-h Tribunal (DPS_GAP_TRIBUNAL_COS.md 2.5): the adds under Brann's taunt read the tank's 0
    // threat as 100 % and zeroed every Single and Aoe spell of the group (Frost Mage on its wand, 19 % of sim).
    if (Unit* npc = ThreatLead::NpcHolder(bot, target))
    {
        // it stays on the NPC whatever the bot deals: no lead to keep
        if (ThreatLead::NpcHoldsRegardless(bot, target))
            return 0;

        float const npcThreat = target->GetThreatMgr().GetThreat(npc);
        if (npcThreat > 0.0f)
            return static_cast<uint8>(std::min(botThreat * 100.0f / npcThreat, 255.0f));

        // At no threat the NPC loses the mob to the first hit of anyone (ThreatManager::ReselectVictim, 110 % of 0).
        // A tank with threat on it is about to take it: its lead decides below. Else nobody holds the mob by threat
        // and nobody is coming for it (the fresh-add hold gave the tank its window): there is no lead to wait for.
        bool tankOnIt = false;
        for (GroupReference* gref = group->GetFirstMember(); gref && !tankOnIt; gref = gref->next())
        {
            Player* player = gref->GetSource();
            tankOnIt = player && player != bot && player->IsAlive() && PlayerbotAI::IsTank(player) &&
                       target->GetThreatMgr().GetThreat(player) > 0.0f;
        }
        if (!tankOnIt)
            return 0;
    }

    float maxThreat = -1.0f;
    bool hasTank = false;

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (!player || !player->IsAlive() || player == bot)
            continue;

        if (PlayerbotAI::IsTank(player))
        {
            hasTank = true;
            float threat = target->GetThreatMgr().GetThreat(player);
            if (maxThreat < threat)
                maxThreat = threat;
        }
    }

    if (maxThreat <= 0 && !hasTank)
        return 0;

    // calculate normal threat for fleeing targets
    bool fleeing = target->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLEEING_MOTION_TYPE ||
                   target->GetMotionMaster()->GetCurrentMovementGeneratorType() == TIMED_FLEEING_MOTION_TYPE;

    // return high threat if tank has no threat
    if (target->IsInCombat() && maxThreat <= 0 && botThreat <= 0 && hasTank && !fleeing)
        return 100;

    // return low threat if mob if fleeing
    if (hasTank && fleeing)
        return 0;

    // no tank threat to compare with (a mob not in combat yet): the old division gave inf or NaN
    if (maxThreat <= 0.0f)
        return botThreat > 0.0f ? 100 : 0;

    // a tank reading an off-tank's lower threat goes far over 100: saturate instead of wrapping the uint8
    return static_cast<uint8>(std::min(botThreat * 100.0f / maxThreat, 255.0f));
}

float NextCastThreatValue::Calculate()
{
    // EstimatedGroupDpsValue's per-player estimate: the level's basic dps scaled by the gear score.
    constexpr float GEAR_MIN = 0.75f;
    constexpr float GEAR_MAX = 4.0f;
    constexpr uint32 GEAR_TOP_ITEMS = 12;
    float const basicGs = EstimatedGroupDpsValue::GetBasicGs(bot->GetLevel());
    float gear = 1.0f;
    if (basicGs > 0.0f)
    {
        float const gearScore = static_cast<float>(PlayerbotAI::GetMixedGearScore(bot, true, false, GEAR_TOP_ITEMS));
        gear = std::clamp(gearScore / basicGs, GEAR_MIN, GEAR_MAX);
    }

    return EstimatedGroupDpsValue::GetBasicDps(bot->GetLevel()) * gear * NEXT_CAST_SECONDS;
}

namespace ThreatLead
{
    float HolderThreat(Player* bot, Unit* target)
    {
        Group* group = bot->GetGroup();
        if (!target || !group)
            return 0.0f;

        ThreatManager& threat = target->GetThreatMgr();
        Unit* victim = target->GetVictim();
        Player* holder = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (victim && victim != bot && holder && holder != bot && holder->GetGroup() == group)
            return threat.GetThreat(victim);

        // [b44_hos] A friendly NPC holds it: its real threat, or no bound at all when no threat takes the mob off it.
        // At no threat (any hit takes it) the tanks' threat below stands, as before.
        if (Unit* npc = NpcHolder(bot, target))
        {
            if (NpcHoldsRegardless(bot, target))
                return HOLD_UNBOUNDED;

            if (float const npcThreat = threat.GetThreat(npc); npcThreat > 0.0f)
                return npcThreat;
        }

        float best = 0.0f;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsAlive() && PlayerbotAI::IsTank(member))
                best = std::max(best, threat.GetThreat(member));
        }

        return best;
    }

    // [b44_hos]
    Unit* NpcHolder(Player* bot, Unit* target)
    {
        if (!target || target->GetGUID().IsPlayer())
            return nullptr;

        Unit* victim = target->GetVictim();
        if (!victim || victim->GetGUID().IsPlayer() || victim->IsControlledByPlayer() ||
            victim->GetCharmerOrOwnerPlayerOrPlayerItself() || bot->IsHostileTo(victim))
            return nullptr;

        return victim;
    }

    // [b44_hos]
    bool NpcHoldsRegardless(Player* bot, Unit* target)
    {
        Unit* npc = NpcHolder(bot, target);
        if (!npc)
            return false;

        ThreatManager const& threat = target->GetThreatMgr();
        if (threat.GetFixateTarget() == npc)
            return true;

        for (ThreatReference const* ref : threat.GetUnsortedThreatList())
            if (ref->GetVictim() == npc)
                return ref->IsTaunting() && ref->IsAvailable();

        return false;
    }

    float ProjectedShare(PlayerbotAI* botAI, Unit* target)
    {
        Player* bot = botAI->GetBot();
        float const holder = HolderThreat(bot, target);
        if (holder <= 0.0f)
            return -1.0f;

        float const next = botAI->GetAiObjectContext()->GetValue<float>("next cast threat")->Get();
        return (target->GetThreatMgr().GetThreat(bot) + next) / holder;
    }

    float PullOverPoint(Player* bot, Unit* target)
    {
        return target && target->IsWithinMeleeRange(bot) ? MELEE_PULL_OVER : RANGED_PULL_OVER;
    }

    bool SafeToOpen(PlayerbotAI* botAI, Unit* target, bool careful)
    {
        float const share = ProjectedShare(botAI, target);
        return share >= 0.0f && share <= (careful ? CAREFUL_OPEN_SHARE : OPEN_SHARE);
    }
}
