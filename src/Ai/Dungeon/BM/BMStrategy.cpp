/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BMStrategy.h"
#include "AiObjectContext.h"
#include "BMMultipliers.h"
#include "BMShared.h"
#include "Playerbots.h"
#include "StringFormat.h"

void TbcDungeonBlackMorassStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Before the start: every member asks Sa'at for its own Chrono-beacon (one per player).
    triggers.push_back(new TriggerNode("bm chrono beacon from saat",
        { NextAction("bm chrono beacon from saat", ACTION_MOVE + 4) }));

    // The portals are one fight: the tank's trash fight spot (FightHold) is Medivh's hold spot, not the last pull's.
    triggers.push_back(new TriggerNode("bm fight spot at medivh",
        { NextAction("bm fight spot at medivh", ACTION_RAID + 6) }));

    // The tank brings each rift keeper to Medivh instead of the group going 95-138 yd out to the rift.
    triggers.push_back(new TriggerNode("bm drag rift guardian",
        { NextAction("bm drag rift guardian", ACTION_RAID + 4) }));
    triggers.push_back(new TriggerNode("bm fetch rift guardian",
        { NextAction("bm fetch rift guardian", ACTION_RAID + 2) }));

    // Adds walking to Medivh / draining his shield come first for every damage dealer.
    triggers.push_back(new TriggerNode("bm medivh threatened",
        { NextAction("bm defend medivh", ACTION_RAID + 3) }));

    // No add channels: the keeper or rift boss fighting at Medivh dies first (its rift closes, the next one opens).
    triggers.push_back(new TriggerNode("bm kill rift guardian",
        { NextAction("bm kill rift guardian", ACTION_RAID + 2) }));

    // Damage dealers and the healer stay at Medivh during the portals.
    triggers.push_back(new TriggerNode("bm hold near medivh",
        { NextAction("bm hold near medivh", ACTION_RAID + 1) }));

    // Hard rifts (13-17): the Time Keeper drake helps against the adds (never at a boss: they banish it).
    triggers.push_back(new TriggerNode("bm use chrono beacon",
        { NextAction("bm use chrono beacon", ACTION_RAID) }));

    // Temporus' Hasten, Aeonus' Frenzy.
    triggers.push_back(new TriggerNode("bm boss buff",
        { NextAction("bm remove boss buff", ACTION_DISPEL + 2) }));
}

void TbcDungeonBlackMorassStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new BlackMorassDefendFocusMultiplier(botAI));
    multipliers.push_back(new BlackMorassStayAtMedivhMultiplier(botAI));
    multipliers.push_back(new BlackMorassDragRiftGuardianMultiplier(botAI));
    multipliers.push_back(new BlackMorassTemporusReflectMultiplier(botAI));
    multipliers.push_back(new BlackMorassNoRestForRiftBossMultiplier(botAI));  // [b32_bm]
}

// A damage dealer's "dps target" is never a keeper while an add channels Corrupt Medivh, nor one far from him while a
// tank lives to bring it (bm-n 2026-10-25 rift 4: all five walked 77 yd to a Rift Lord, the shield went 89 -> 0 in
// 50 s). Adds that only fight do not shield a keeper: with them as the bar a dragged keeper never died (build 25).
void TbcDungeonBlackMorassStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    Player* bot = botAI->GetBot();
    if (type != TargetValueExclusionType::DpsTarget || PlayerbotAI::IsTank(bot) || !BMShared::IsEventInProgress(bot))
        return;

    AiObjectContext* context = botAI->GetAiObjectContext();
    GuidVector const guardians = context->GetValue<GuidVector>("bm rift guardians")->Get();
    if (guardians.empty())
        return;

    bool const threatened = BMShared::AnyCorrupting(botAI);
    bool const tankBrings = BMShared::HasLivingTank(bot);
    for (ObjectGuid const& guid : guardians)
    {
        Unit* guardian = botAI->GetUnit(guid);
        if (!guardian)
            continue;

        bool const farGuardian = BMShared::IsFarGuardian(bot, guardian);
        if (!threatened && !(farGuardian && tankBrings))
            continue;

        exclusions.insert(guid);
        BMShared::Log(bot, "bm guardian", guid.ToString() + ":excluded",
                      Acore::StringFormat("leaves {} alone: {}", guardian->GetName(),
                                          threatened ? "adds drain Medivh" : "far from Medivh, the tank brings it"),
                      30 * IN_MILLISECONDS);
    }
}
