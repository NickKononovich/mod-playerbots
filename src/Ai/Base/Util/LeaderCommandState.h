/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_LEADERCOMMANDSTATE_H
#define _PLAYERBOT_LEADERCOMMANDSTATE_H

// [rot3] The rotation side's three states of a bot under the dungeon leader (user 2026-10-26): not led (a group
// without our leader: the bot's own rules), led with a fresh command (the command is the rule), led with a missing or
// stale command (older than LEADER_EVENT_TTL_MS): the SAFE MINIMUM only - the rotation on the current target while it
// is in the fight, the healer's heals, the emergency personal saves - and no decision of its own (no own interrupt,
// external, buff on another, crowd control, burn cooldown or target change). There is no fallback to the old own
// rules for a led bot.

#include "LeaderCommand.h"
#include "Log.h"
#include "Player.h"
#include "Timer.h"

#include <unordered_map>

namespace LeaderCommandState
{
    enum class State : uint8
    {
        NotLed,
        Fresh,
        Stale
    };

    constexpr uint32 STALE_LOG_MS = 5000;  // one [stale command] line per bot this often

    // the bot's last fresh command (ms), per map thread: the age the [stale command] line reports
    inline std::unordered_map<ObjectGuid, uint32>& LastFresh()
    {
        thread_local std::unordered_map<ObjectGuid, uint32> lastFresh;
        return lastFresh;
    }

    inline State Read(Player* bot, Leader::MemberEvents& out)
    {
        out = Leader::MemberEvents();
        if (!LeaderCommand::Led(bot))
            return State::NotLed;

        if (!LeaderCommand::CurrentCommand(bot, out))
            return State::Stale;

        uint32 const now = getMSTime();
        LastFresh()[bot->GetGUID()] = now ? now : 1;
        return State::Fresh;
    }

    inline State Read(Player* bot)
    {
        Leader::MemberEvents command;
        return Read(bot, command);
    }

    // [rot3] [stale command] <bot> <what>: <age>, rate-limited per bot
    inline void LogStale(Player* bot, char const* what)
    {
        thread_local std::unordered_map<ObjectGuid, uint32> logged;
        uint32 const now = getMSTime();
        uint32& last = logged[bot->GetGUID()];
        if (last && getMSTimeDiff(last, now) < STALE_LOG_MS)
            return;

        last = now ? now : 1;
        auto const fresh = LastFresh().find(bot->GetGUID());
        if (fresh == LastFresh().end())
            LOG_INFO("playerbots", "[rot3] [stale command] {} {}: no leader command yet", bot->GetName(), what);
        else
            LOG_INFO("playerbots", "[rot3] [stale command] {} {}: the last fresh command {} ms ago", bot->GetName(),
                     what, getMSTimeDiff(fresh->second, now));
    }

    // [rot3] The leader's pre-emptive personal save (PreSave: "a big boss hit is coming"). Wired to
    // LeaderCommand::AssignedPreSave(bot) once the leader side has the getter; until then no command, false.
    inline bool PreSaveAssigned(Player* /*bot*/) { return false; }
}

#endif
