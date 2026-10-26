/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgStrategy.h"

NewRpgStrategy::NewRpgStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

std::vector<NextAction> NewRpgStrategy::getDefaultActions()
{
    // the releavance should be greater than grind
    return {
        NextAction("new rpg status update", 11.0f)
    };
}

void NewRpgStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // a stalled taxi spline is restarted before anything else looks at the bot
    triggers.push_back(
        new TriggerNode(
            "new rpg taxi stalled",
            {
                NextAction("new rpg taxi unstall", 14.0f)
            }
        )
    );
    // the selfbot leader is away: rest in an inn (above waiting for the group)
    triggers.push_back(
        new TriggerNode(
            "new rpg leader afk",
            {
                NextAction("new rpg rest at inn", 13.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "new rpg unknown flight master",
            {
                NextAction("new rpg discover flight master", 12.5f)
            }
        )
    );
    // above "new rpg status update" (11) so nothing moves the leader while the group catches up
    triggers.push_back(
        new TriggerNode(
            "new rpg group not ready",
            {
                NextAction("new rpg wait for group", 12.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "go grind status",
            {
                NextAction("new rpg go grind", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "go camp status",
            {
                NextAction("new rpg go camp", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "wander random status",
            {
                NextAction("new rpg wander random", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "wander npc status",
            {
                NextAction("new rpg wander npc", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "do quest status",
            {
                NextAction("new rpg do quest", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "travel flight status",
            {
                NextAction("new rpg travel flight", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "outdoor pvp status",
            {
                NextAction("new rpg outdoor pvp", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "do gather status",
            {
                NextAction("new rpg do gather", 3.0f)
            }
        )
    );
}

void NewRpgStrategy::InitMultipliers(std::vector<Multiplier*>&)
{
}
