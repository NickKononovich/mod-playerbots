/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_QUESTSTATS_H
#define PLAYERBOTS_QUESTSTATS_H

#include "Define.h"

#include <string>

class Player;

// [guild90] questing quality counters per bot, for C:/WoW/tools/guild_stats.py: XP by source, kills that served a
// quest, yards walked, turn-ins; the progress of the quest it works on. Counters only (no decision reads them); one
// cumulative [qstat] line per bot on a level-up, a logout and every QSTAT_LOG_MS of its rpg ticks.
namespace QuestStats
{
    // the New RPG status tick (out of combat): yards walked since the last tick, the active quest's progress line
    // [budget12] `bucket`: what the bot does now (its New RPG status, a group wait, a skinning grind, a vendor run);
    // the time from the last tick to this one goes to the last tick's bucket, out of combat and alive, else to
    // "COMBAT" / "DEAD" (the [qstat] line's budget= field: seconds per bucket)
    void OnRpgTick(Player* bot, uint32 activeQuest, char const* bucket);
    // one timestamped event line: [quest] <event> <bot> L<level> q<id> t=<unix ms> at=(x,y) <extra>
    // events: accept, progress, complete, turnin, abandon, postpone, target-switch, sweep, wait-respawn, kill
    void LogEvent(Player* bot, char const* event, uint32 questId, std::string const& extra);
    // [budget12] a fight's (or a death's: died, from the death to the rise) milliseconds, for the time budget
    void NoteCombat(Player* bot, uint32 combatMs, bool died);
    uint64 NowMs();
    // [log28] the triage's event lines, one per event: a town errand's start / done ("[town] <bot> <errand> done
    // after N s"), a trip's boarding / landing ("[trip] <bot> boards <what>", "[trip] <bot> lands at map M after N s")
    void TownStart(Player* bot, char const* kind);
    void TownDone(uint32 low, std::string const& name, char const* errand);
    void TripBoard(Player* bot, std::string const& what);
    void TripLand(Player* bot);
}

void AddQuestStatsScripts();

#endif
