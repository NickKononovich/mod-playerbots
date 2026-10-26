/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ACMultipliers.h"
#include "ACActions.h"
#include "ACTriggers.h"
#include "GenericSpellActions.h"
#include "HunterActions.h"
#include "MageActions.h"
#include "MovementActions.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ReachTargetActions.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

// Shirrak the Dead Watcher

// Flee from Focus Fire and dont run back in
float ShirrakFleeFocusFireMultiplier::GetValue(Action* action)
{
    if (dynamic_cast<AttackAction*>(action))
        return 1.0f;

    bool const isMovementSpell = dynamic_cast<CastReachTargetSpellAction*>(action) ||
        dynamic_cast<CastBlinkBackAction*>(action) || dynamic_cast<CastDisengageAction*>(action);

    if (!isMovementSpell && !dynamic_cast<MovementAction*>(action))
        return 1.0f;

    if (!AI_VALUE2(Unit*, "find target", "shirrak the dead watcher"))
        return 1.0f;

    if (dynamic_cast<ShirrakFleeFocusFireAction*>(action))
        return 1.0f;

    Creature* flare = bot->FindNearestCreature(NPC_FOCUS_FIRE, FLARE_SEARCH_RADIUS);
    if (!flare)
        return 1.0f;

    if (isMovementSpell)
        return 0.0f;

    float currentDistance = bot->GetExactDist2d(flare);
    constexpr float safeDistance = 12.0f;
    constexpr float buffer = 2.0f;
    return currentDistance < safeDistance + buffer ? 0.0f : 1.0f;
}

// [b35_lock]
float ShirrakHoldLongRangeMultiplier::GetValue(Action* action)
{
    if (!dynamic_cast<ReachSpellAction*>(action))
        return 1.0f;

    if (!PlayerbotAI::IsRanged(bot) || PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))
        return 1.0f;

    Unit* shirrak = AI_VALUE2(Unit*, "find target", "shirrak the dead watcher");
    if (!shirrak || AI_VALUE(Unit*, "current target") != shirrak)
        return 1.0f;

    float const ring = ShirrakRangedDpsDistance(botAI);
    if (ring <= sPlayerbotAIConfig.SpellDistance)
        return 1.0f;

    return bot->GetDistance(shirrak) <= ring + SHIRRAK_RANGE_MARGIN && bot->IsWithinLOSInMap(shirrak) ? 0.0f : 1.0f;
}

// [b35_lock]
float ShirrakInhibitMagicInstantsMultiplier::GetValue(Action* action)
{
    CastSpellAction* spellAction = dynamic_cast<CastSpellAction*>(action);
    if (!spellAction || dynamic_cast<CastShootAction*>(action) || dynamic_cast<CastHealingSpellAction*>(action))
        return 1.0f;

    Aura* inhibit = bot->GetAura(SPELL_INHIBIT_MAGIC);
    if (!inhibit || inhibit->GetStackAmount() < 2)
        return 1.0f;

    Unit* target = action->GetTarget();
    if (!target || target == bot || !bot->IsHostileTo(target))
        return 1.0f;

    SpellInfo const* info = sSpellMgr->GetSpellInfo(AI_VALUE2(uint32, "spell id", spellAction->getSpell()));
    if (!info || info->IsPositive() || info->IsChanneled() || info->CalcCastTime(bot) > 0)
        return 1.0f;

    return SHIRRAK_INSTANT_PREFERENCE;
}
