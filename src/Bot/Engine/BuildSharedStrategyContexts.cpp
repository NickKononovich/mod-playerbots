/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AiObjectContext.h"
#include "DungeonStrategyContext.h"
#include "EscortProtect.h"
#include "LeaderContext.h"
#include "RaidStrategyContext.h"
#include "StrategyContext.h"
#include "SelfTravelContext.h"
#include "TestRunnerContext.h"

void AiObjectContext::BuildSharedStrategyContexts(SharedNamedObjectContextList<Strategy>& strategyContexts)
{
    strategyContexts.Add(new StrategyContext());
    strategyContexts.Add(new MovementStrategyContext());
    strategyContexts.Add(new AssistStrategyContext());
    strategyContexts.Add(new QuestStrategyContext());
    strategyContexts.Add(new DungeonStrategyContext());
    strategyContexts.Add(new DungeonEscortStrategyContext());  // [w24b-escort-no-sibling]
    strategyContexts.Add(new RaidStrategyContext());
    strategyContexts.Add(new TestRunnerStrategyContext());
    strategyContexts.Add(new SelfTravelStrategyContext());
    strategyContexts.Add(new LeaderStrategyContext());
}
