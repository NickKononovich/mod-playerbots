/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SLOWUPDATE_H
#define PLAYERBOTS_SLOWUPDATE_H

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

#include "Log.h"
#include "StringFormat.h"

// [perf212] one bot update's stages timed: the engine's stage marks (the freeze trace's: pre-checks, each trigger
// check, each action's checks and run) each close the stage before them. An update slower than SLOW_UPDATE_MS logs
// its bot and its longest stages - the AI spikes of b436 / b437 (one update of 0.8-1.5 s, "AI max" = the tick)
// named no SlowScope. One clock read per stage, a copy of the name only for a stage of STAGE_MS or more; a log,
// no decision reads it
namespace SlowUpdate
{
    constexpr long long SLOW_UPDATE_MS = 100;  // the core's MinRecordUpdateTimeDiff: an update this slow is a lag
    constexpr long long STAGE_US = 5000;       // a stage this long is named on the line
    constexpr size_t NAMED = 4;                // the longest stages named (a line's length)

    struct Piece
    {
        long long us = 0;
        char name[72] = {};
    };

    struct State
    {
        bool active = false;
        std::chrono::steady_clock::time_point start;
        std::chrono::steady_clock::time_point mark;
        char const* prefix = "";
        void* object = nullptr;                  // the stage's trigger / action, named only when the stage is long
        std::string (*namer)(void*) = nullptr;  // its getName (a copy: AiObject::getName returns by value)
        Piece top[NAMED];
        size_t count = 0;
    };

    inline thread_local State state;

    inline void Close(std::chrono::steady_clock::time_point now)
    {
        long long const us = std::chrono::duration_cast<std::chrono::microseconds>(now - state.mark).count();
        if (us < STAGE_US)
            return;
        size_t at = state.count;
        while (at > 0 && state.top[at - 1].us < us)
            --at;
        if (at >= NAMED)
            return;
        size_t const last = state.count < NAMED ? state.count : NAMED - 1;
        for (size_t i = last; i > at; --i)
            state.top[i] = state.top[i - 1];
        if (state.count < NAMED)
            ++state.count;
        Piece& piece = state.top[at];
        piece.us = us;
        std::string const label = std::string(state.prefix) + (state.object ? state.namer(state.object) : "");
        std::size_t const length = std::min(label.size(), sizeof(piece.name) - 1);
        std::memcpy(piece.name, label.data(), length);
        piece.name[length] = 0;
    }

    // the bot's update starts (OnPlayerAfterUpdate, round PlayerbotAI::UpdateAI)
    inline void Begin()
    {
        state.active = true;
        state.start = state.mark = std::chrono::steady_clock::now();
        state.prefix = "start";
        state.object = nullptr;
        state.count = 0;
    }

    template <typename T>
    std::string NameOf(void* object)
    {
        return static_cast<T*>(object)->getName();
    }

    // a new stage: `prefix` a string literal, `object` (a Trigger / Action, or none) outlives the update
    inline void Stage(char const* prefix)
    {
        if (!state.active)
            return;
        auto const now = std::chrono::steady_clock::now();
        Close(now);
        state.mark = now;
        state.prefix = prefix;
        state.object = nullptr;
    }

    template <typename T>
    void Stage(char const* prefix, T* object)
    {
        if (!state.active)
            return;
        auto const now = std::chrono::steady_clock::now();
        Close(now);
        state.mark = now;
        state.prefix = prefix;
        state.object = object;
        state.namer = &NameOf<T>;
    }

    inline void End(std::string const& who)
    {
        if (!state.active)
            return;
        auto const now = std::chrono::steady_clock::now();
        Close(now);
        state.active = false;
        long long const ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.start).count();
        if (ms < SLOW_UPDATE_MS)
            return;
        std::string stages;
        long long named = 0;
        for (size_t i = 0; i < state.count; ++i)
        {
            stages += Acore::StringFormat("{}{} {} ms", i ? ", " : "", state.top[i].name, state.top[i].us / 1000);
            named += state.top[i].us;
        }
        LOG_INFO("playerbots", "[perf212] slow bot update {}: {} ms; longest stages: {}; the rest {} ms", who, ms,
                 stages.empty() ? std::string("none of 5+ ms") : stages, ms - named / 1000);
    }
}

#endif
