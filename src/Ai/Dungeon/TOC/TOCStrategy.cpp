/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TOCStrategy.h"

#include "TOCMultipliers.h"
#include "Playerbots.h"
#include "TOCShared.h"

void WotlkDungeonToCStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("toc lance",
        { NextAction("toc lance", ACTION_RAID + 5) }));
    triggers.push_back(new TriggerNode("toc ue lance",
        { NextAction("toc ue lance", ACTION_RAID + 2) }));
    triggers.push_back(new TriggerNode("toc mount near",
        { NextAction("toc mount", ACTION_RAID + 4) }));
    triggers.push_back(new TriggerNode("toc mounted",
        { NextAction("toc mounted", ACTION_RAID + 6) }));
    triggers.push_back(new TriggerNode("toc eadric",
        { NextAction("toc eadric", ACTION_RAID + 3) }));
    triggers.push_back(new TriggerNode("toc start event",
        { NextAction("toc start event", ACTION_RAID + 7) }));
    triggers.push_back(new TriggerNode("toc argent soldiers",
        { NextAction("toc pull argent soldiers", ACTION_RAID + 1) }));
    triggers.push_back(new TriggerNode("toc reflective shield",
        { NextAction("toc hold fire", ACTION_RAID + 8) }));
    triggers.push_back(new TriggerNode("toc interrupt",
        { NextAction("toc interrupt", ACTION_INTERRUPT + 5) }));
    triggers.push_back(new TriggerNode("toc ghoul explode",
        { NextAction("toc avoid ghoul explode", ACTION_EMERGENCY + 5) }));
    triggers.push_back(new TriggerNode("toc paletress renew",
        { NextAction("toc dispel renew", ACTION_DISPEL + 2) }));
    triggers.push_back(new TriggerNode("toc marked for death",
        { NextAction("toc marked defensive", ACTION_EMERGENCY + 2) }));
    // above the low / medium health heals, below the critical ones
    triggers.push_back(new TriggerNode("toc marked for death heal",
        { NextAction("toc heal marked", ACTION_MEDIUM_HEAL + 6) }));
}

void WotlkDungeonToCStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new ToCJoustMultiplier(botAI));
    multipliers.push_back(new ToCHoldFireMultiplier(botAI));
}

void WotlkDungeonToCStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType /*type*/)
{
    if (Creature* paletress = ToCShared::GetShieldedPaletress(botAI->GetBot()))
        exclusions.insert(paletress->GetGUID());
}
