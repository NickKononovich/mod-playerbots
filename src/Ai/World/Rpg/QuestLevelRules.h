/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_QUESTLEVELRULES_H
#define PLAYERBOTS_QUESTLEVELRULES_H

#include "Formulas.h"
#include "Player.h"
#include "QuestDef.h"

#include <algorithm>

// How many levels above the bot a quest may be before the bot refuses it. Shared by the NewRPG
// capable check and the quest-giver teleport index so both agree.
constexpr int32 QUEST_MAX_LEVELS_ABOVE_BOT = 3;

// [pull9] a quest of orange or red con for the bot (the core's level rule): not taken at a giver, as the quest pick
// holds one in the log until the bot is closer (b345: 28 "quest of orange level", 5 taken at a giver in the run). A
// quest already in the log is kept (QUEST_MAX_LEVELS_ABOVE_BOT drops only what is farther above).
inline bool QuestTooHighToTake(Player* bot, Quest const* quest)
{
    int32 const questLevel = std::clamp<int32>(bot->GetQuestLevel(quest), 1, 255);
    return Acore::XP::GetColorCode(bot->GetLevel(), uint8(questLevel)) <= XP_ORANGE;
}

#endif
