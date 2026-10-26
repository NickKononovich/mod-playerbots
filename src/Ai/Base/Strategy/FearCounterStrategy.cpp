/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FearCounterStrategy.h"

#include "FearCounterActions.h"
#include "FearCounterValues.h"
#include "Playerbots.h"

// [fear_counter]
void FearCounterStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Player* bot = botAI->GetBot();
    switch (bot->getClass())
    {
        case CLASS_SHAMAN:
            // A feared tank is an emergency; the pre-drop only has to win over the earth totem upkeep (55-60).
            triggers.push_back(new TriggerNode("party member feared",
                                               { NextAction("fear counter tremor totem", ACTION_EMERGENCY + 2) }));
            triggers.push_back(
                new TriggerNode("fear threat", { NextAction("fear counter tremor totem", ACTION_RAID + 1) }));
            break;
        case CLASS_PRIEST:
            // Above the every-fight "fear ward on main tank" (ACTION_HIGH + 3), below the critical heals.
            triggers.push_back(
                new TriggerNode("fear ward needed", { NextAction("fear counter fear ward", ACTION_HIGH + 6) }));
            break;
        case CLASS_WARRIOR:
            triggers.push_back(new TriggerNode("fear incoming",
                                               { NextAction("fear counter berserker rage", ACTION_EMERGENCY + 1) }));
            break;
        default:
            break;
    }

    // "fear charm sleep" reads the bot's auras every tick: only for a bot wearing a PvP trinket when its engine is
    // built (a later gear change is picked up at the next strategy reset). Racials go first (ACTION_EMERGENCY + 1);
    // Every Man for Himself shares the trinket's cooldown.
    uint32 trinketSpell = 0;
    if (FearCounterPvpTrinketAction::FindPvpTrinket(bot, trinketSpell))
        triggers.push_back(
            new TriggerNode("fear charm sleep", { NextAction("fear counter pvp trinket", ACTION_EMERGENCY) }));
}

void FearCounterStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    if (botAI->GetBot()->getClass() == CLASS_SHAMAN)
        multipliers.push_back(new FearCounterTotemMultiplier(botAI));
}

float FearCounterTotemMultiplier::GetValue(Action* action)
{
    if (!action || !bot->IsInCombat())
        return 1.0f;

    std::string const name = action->getName();
    bool const competing = name == "strength of earth totem" || name == "stoneskin totem" ||
                           name == "earthbind totem" || name == "stoneclaw totem" || name == "call of the elements";
    if (!competing || !bot->HasSpell(FearCounter::SPELL_TREMOR_TOTEM))
        return 1.0f;

    FearCounterState const state = AI_VALUE(FearCounterState, "fear counter state");
    if (state.victim.IsEmpty() && !(state.threat & FearCounter::THREAT_TREMOR))
        return 1.0f;

    return 0.0f;
}
