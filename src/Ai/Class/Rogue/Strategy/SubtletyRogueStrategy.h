/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SUBTLETYROGUESTRATEGY_H
#define PLAYERBOTS_SUBTLETYROGUESTRATEGY_H

#include "GenericRogueStrategy.h"

class PlayerbotAI;

// [b31_wr] Minimal 3.3.5 Subtlety (was commented out; Subtlety ran the Assassination strategy): Hemorrhage debuff
// upkeep, Backstab from behind else Hemorrhage, Slice and Dice upkeep, Rupture / Eviscerate at 5 combo points,
// Shadow Dance into Ambush, Premeditation in Stealth or Shadow Dance.
class SubtletyRogueStrategy : public GenericRogueStrategy
{
public:
    SubtletyRogueStrategy(PlayerbotAI* botAI);

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "subtlety"; }
    std::vector<NextAction> getDefaultActions() override;
};

#endif
