/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HoRMultipliers.h"
#include "GenericSpellActions.h"
#include "HoRTriggers.h"
#include "InstanceScript.h"
#include "AttackAction.h"
#include "PaladinActions.h"
#include "PullActions.h"
#include "Playerbots.h"
#include "ReachTargetActions.h"
#include "ShamanActions.h"

float HoRSharedSufferingMultiplier::GetValue(Action* action)
{
    if (!dynamic_cast<CurePartyMemberAction*>(action) && !dynamic_cast<CastCureSpellAction*>(action) &&
        action->getName() != "mass dispel")
        return 1.0f;

    InstanceScript* instance = HoRHelpers::GetInstance(bot);
    if (!instance || instance->GetBossState(HoRIds::DATA_MARWYN) != IN_PROGRESS)
        return 1.0f;

    return 0.0f;
}

float HoRShadowAuraMultiplier::GetValue(Action* action)
{
    if (bot->getClass() != CLASS_PALADIN)
        return 1.0f;

    if (!dynamic_cast<CastDevotionAuraAction*>(action) && !dynamic_cast<CastConcentrationAuraAction*>(action) &&
        !dynamic_cast<CastRetributionAuraAction*>(action) && !dynamic_cast<CastFrostResistanceAuraAction*>(action) &&
        !dynamic_cast<CastFireResistanceAuraAction*>(action) && !dynamic_cast<CastCrusaderAuraAction*>(action) &&
        !dynamic_cast<CastSanctityAuraAction*>(action))
        return 1.0f;

    return HoRHelpers::IsShadowAuraPaladin(botAI) && HoRHelpers::IsShadowBossFight(bot) ? 0.0f : 1.0f;
}

float HoRFalricHeroismMultiplier::GetValue(Action* action)
{
    if (!dynamic_cast<CastHeroismAction*>(action) && !dynamic_cast<CastBloodlustAction*>(action))
        return 1.0f;

    InstanceScript* instance = HoRHelpers::GetInstance(bot);
    if (!instance || instance->GetBossState(HoRIds::DATA_FALRIC) != IN_PROGRESS)
        return 1.0f;

    Creature* falric = HoRHelpers::GetInstanceCreature(bot, HoRIds::DATA_FALRIC);
    return falric && falric->IsAlive() && falric->GetHealthPct() > HoRIds::FALRIC_HEROISM_HEALTH_PCT ? 0.0f : 1.0f;
}

float HoRFrostswornGeneralMultiplier::GetValue(Action* action)
{
    Creature* general = HoRHelpers::GetFrostswornGeneral(bot);
    if (!general)
        return 1.0f;

    if (general->IsInCombat())
        return dynamic_cast<MoveFightSpotBackAction*>(action) ? 0.0f : 1.0f;

    // Before the pull: nobody walks into him or attacks him while a living member is more than 30 yd away.
    if (bot->IsInCombat() || action->GetTarget() != general ||
        (!dynamic_cast<ReachTargetAction*>(action) && !dynamic_cast<AttackAction*>(action)))
        return 1.0f;

    return HoRHelpers::IsGroupGathered(bot, HoRIds::GENERAL_GATHER_RANGE) ? 1.0f : 0.0f;
}

float HoREscapeMeetAtLeaderMultiplier::GetValue(Action* action)
{
    if (PlayerbotAI::IsTank(bot) || !bot->IsInCombat() || !dynamic_cast<ReachTargetAction*>(action))
        return 1.0f;

    Unit* target = action->GetTarget();
    if (!target || target->ToPlayer() || !HoRHelpers::IsEscapeSummon(target->GetEntry()) ||
        !HoRHelpers::IsLichKingFightActive(bot))
        return 1.0f;

    Creature* leader = HoRHelpers::GetInstanceCreature(bot, HoRIds::NPC_SYLVANAS_PART2);
    if (!leader || !leader->IsAlive())
        return 1.0f;

    return target->GetExactDist2d(leader) > HoRIds::ESCAPE_MEET_RANGE ? 0.0f : 1.0f;
}

float HoRWaveHoldMultiplier::GetValue(Action* action)
{
    if (!bot->IsInCombat() || !dynamic_cast<ReachTargetAction*>(action))
        return 1.0f;

    return HoRHelpers::IsOutsideWaveHold(bot, action->GetTarget()) ? 0.0f : 1.0f;
}
