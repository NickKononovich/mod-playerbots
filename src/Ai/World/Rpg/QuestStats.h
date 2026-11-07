/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_QUESTSTATS_H
#define PLAYERBOTS_QUESTSTATS_H

#include "Define.h"

class Player;

// [guild90] questing quality counters per bot, for C:/WoW/tools/guild_stats.py: XP by source, kills that served a
// quest, yards walked, turn-ins; the progress of the quest it works on. Counters only (no decision reads them); one
// cumulative [qstat] line per bot on a level-up, a logout and every QSTAT_LOG_MS of its rpg ticks.
namespace QuestStats
{
    // the New RPG status tick (out of combat): yards walked since the last tick, the active quest's progress line
    void OnRpgTick(Player* bot, uint32 activeQuest);
}

void AddQuestStatsScripts();

#endif
