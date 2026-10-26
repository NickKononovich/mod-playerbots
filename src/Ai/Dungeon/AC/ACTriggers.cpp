/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ACTriggers.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>

// Shirrak the Dead Watcher

bool ShirrakTankPositionBossTrigger::IsActive()
{
    return PlayerbotAI::IsTank(bot) &&
        AI_VALUE2(Unit*, "find target", "shirrak the dead watcher");
}

bool ShirrakFleeFocusFireTrigger::IsActive()
{
    if (!AI_VALUE2(Unit*, "find target", "shirrak the dead watcher"))
        return false;

    return bot->FindNearestCreature(NPC_FOCUS_FIRE, FLARE_SEARCH_RADIUS);
}

bool ShirrakRangedKeepDistanceTrigger::IsActive()
{
    if (!PlayerbotAI::IsRanged(bot) || PlayerbotAI::IsTank(bot))
        return false;

    Unit* shirrak = AI_VALUE2(Unit*, "find target", "shirrak the dead watcher");
    if (!shirrak)
        return false;

    if (PlayerbotAI::IsHeal(bot))
        return bot->GetDistance(shirrak) < SHIRRAK_HEALER_DISTANCE - SHIRRAK_DISTANCE_TOLERANCE;

    // [b35_lock] The long-range band has no 2 yd slack: below 35 yd is the second stack.
    float const desired = ShirrakRangedDpsDistance(botAI);
    if (desired > sPlayerbotAIConfig.SpellDistance)
        return bot->GetDistance(shirrak) < SHIRRAK_ONE_STACK_DISTANCE;

    return bot->GetDistance(shirrak) < desired - SHIRRAK_DISTANCE_TOLERANCE;
}

// [b35_lock]
float ShirrakRangedDpsDistance(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    float reach = 0.0f;
    if (bot->getClass() == CLASS_HUNTER)
        reach = botAI->GetRange("spell");  // the weapon's range with Hawk Eye, less its own margin
    else
    {
        // the class's damage fillers; the longest one the bot knows, with its range talents (spell mods)
        char const* nukes[2] = {nullptr, nullptr};
        switch (bot->getClass())
        {
            case CLASS_WARLOCK:
                nukes[0] = "incinerate";
                nukes[1] = "shadow bolt";
                break;
            case CLASS_MAGE:
                nukes[0] = "frostbolt";
                nukes[1] = "fireball";
                break;
            case CLASS_PRIEST:
                nukes[0] = "mind flay";
                nukes[1] = "mind blast";
                break;
            case CLASS_DRUID:
                nukes[0] = "wrath";
                nukes[1] = "starfire";
                break;
            case CLASS_SHAMAN:
                nukes[0] = "lightning bolt";
                break;
            default:
                break;
        }

        for (char const* nuke : nukes)
        {
            if (!nuke)
                continue;

            uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", nuke)->Get();
            if (SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr)
                reach = std::max(reach, info->GetMaxRange(false, bot));
        }
    }

    if (reach - SHIRRAK_RANGE_MARGIN < SHIRRAK_ONE_STACK_DISTANCE)
        return sPlayerbotAIConfig.SpellDistance;

    return SHIRRAK_ONE_STACK_DISTANCE + SHIRRAK_RANGE_MARGIN;
}
