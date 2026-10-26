/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UBStrategy.h"
#include "Playerbots.h"
#include "UBMultipliers.h"
#include "UBShared.h"

void TbcDungeonUnderbogStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("ub foul spores", {
        NextAction("ub retreat from foul spores", ACTION_EMERGENCY + 10) }));

    triggers.push_back(new TriggerNode("ub spore cloud danger", {
        NextAction("ub vacate spore cloud", ACTION_EMERGENCY + 2) }));

    triggers.push_back(new TriggerNode("ub underbat lash", {
        NextAction("ub clear underbat back", ACTION_EMERGENCY + 1) }));

    // Ghaz'an: fight on land, never in the Frenzy lake
    triggers.push_back(new TriggerNode("ub ghazan lake", {
        NextAction("ub leave lake", ACTION_EMERGENCY + 6) }));
}

void TbcDungeonUnderbogStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new HungarfenFoulSporesMultiplier(botAI));
    multipliers.push_back(new HungarfenMushroomIgnoreMultiplier(botAI));
    multipliers.push_back(new UnderbatFacingMultiplier(botAI));
    multipliers.push_back(new UBGhazanDryLandMultiplier(botAI));
}

void TbcDungeonUnderbogStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType /*type*/)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    auto const& mushrooms = AI_VALUE_REF(GuidVector, "ub mushrooms");
    exclusions.insert(mushrooms.begin(), mushrooms.end());

    // A swimming Frenzy that is not right next to the bot is never worth chasing: it cannot leave the water and
    // drops the fight once nobody is in reach.
    Player* bot = botAI->GetBot();
    if (!UnderbogGhazan::LakeFight(bot))
        return;

    for (ObjectGuid const& guid : AI_VALUE_REF(GuidVector, "attackers"))
    {
        if (guid.GetEntry() != UnderbogGhazan::NPC_UNDERBOG_FRENZY)
            continue;

        Unit* frenzy = botAI->GetUnit(guid);
        if (frenzy && UnderbogGhazan::IsWet(frenzy) && !bot->IsWithinMeleeRange(frenzy))
            exclusions.insert(guid);
    }
}
