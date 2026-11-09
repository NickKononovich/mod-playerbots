/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SLOWSCOPE_H
#define PLAYERBOTS_SLOWSCOPE_H

#include <chrono>
#include <string>

#include "Log.h"
#include "PerfMonitor.h"

// [perf26] a scope that names itself in the log when one call of it took longer than SLOW_SCOPE_MS, with the PerfMon
// sample of it when PerfMon is on. Always on and cheap (one clock read at each end): the AI max of 0.5-1 s per tick
// (b375) comes from single calls, which the PerfMon totals (off by config) do not name. A log line, no decision
class SlowScope
{
public:
    static constexpr long long SLOW_SCOPE_MS = 20;  // a log threshold: a call this slow is worth a line

    SlowScope(char const* what, std::string const* who = nullptr)
        : what_(what), who_(who), start_(std::chrono::steady_clock::now()),
          pmo_(PerfMonitor::IsEnabled() ? sPerfMonitor.start(PERF_MON_VALUE, what) : nullptr)
    {
    }

    ~SlowScope()
    {
        if (pmo_)
            pmo_->finish();
        long long const ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
        if (ms >= SLOW_SCOPE_MS)
            LOG_INFO("playerbots", "[perf26] slow {}{}{}: {} ms", what_, who_ ? " " : "", who_ ? *who_ : "", ms);
    }

    SlowScope(SlowScope const&) = delete;
    SlowScope& operator=(SlowScope const&) = delete;

private:
    char const* what_;
    std::string const* who_;
    std::chrono::steady_clock::time_point start_;
    PerfMonitorOperation* pmo_;
};

#endif
