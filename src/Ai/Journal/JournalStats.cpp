/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [jrn31] SERVER STATISTICS INTO pb_journal_metric, collected continuously instead of snapshotted on command.
//
// Why this exists. The user wants to judge optimisation FROM THE DATABASE: "if there is something we can optimise,
// so that we can see it - collect the server's statistics and write those too". What existed was PerfMonitor: it
// measures named regions of code, but it aggregates FROM THE MOMENT IT WAS RESET and prints to a file on command.
// That makes two builds comparable only when their reset points were the same, which is this project's own trap
// written out - an instrument that changed between two measurements is not a baseline - and it is why every row
// here states what makes it comparable: the build, the number of bots and sessions, and the window's own start.
//
// THE MEAN HIDES THE SPIKE, and that is the design. A mean of 50 ms over a window that contains one 900 ms tick is
// 50 ms, and the 900 ms is what the bots stand on. A row that carried only the mean would draw a reassuring flat
// line over exactly the defect we are looking for. So every row carries the window's smallest sample, its largest,
// and its 95th percentile, each as its own column with an index (we do not filter on JSON), beside the mean.
//
// EACH MAP SEPARATELY, NEVER THE SUM. One stalled map thread with nineteen healthy ones is a sum that looks fine,
// and "the total looked fine" is a class of defect here, not an inconvenience. So the scope of a map row names the
// map and the instance, and there is no row that adds them together.
//
// WHAT map_tick_ms ACTUALLY IS, stated because the name invites a wrong reading: it is the measured wall interval
// between two consecutive updates of the SAME map - that map's whole cycle, its update plus the wait before the
// next one, which is what a bot on it experiences. It is NOT the duration of Map::Update's body: the core announces
// the map's update near its end only (Map::Update -> OnMapUpdate), so separating the body from the gap would need a
// hook at the start of Map::Update, which is a core change and not this file's business. `map_diff_ms` beside it is
// the diff the core itself handed that map, which is the same quantity as the core measures it.
//
// WHAT THE COLLECTION COSTS, because a sampler that disturbs what it measures makes us fix the instrument thinking
// we are fixing the server: per world tick and per map update it is one mutex lock and one push_back of a float -
// tens of nanoseconds. The window's own work (sorting a few hundred floats per key and pushing one row per key into
// the journal's queue) happens once every StatsSeconds on the world thread.
//
// Nothing here reads a bot, a unit or the world. It cannot change a decision, and it has no clock that any bot
// consults: the period is the sampler's own window, not a rule any behaviour is timed against.

#include "Journal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Config.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#ifdef _MSC_VER
// psapi.lib is not in the core's link line anywhere (nothing else in the tree asks the process for its memory), so
// the dependency is declared here rather than in CMake: the pragma keeps it with the one function that needs it,
// and adding a library to the module's cmake for one call would make every build carry it.
#pragma comment(lib, "psapi.lib")
#endif
#else
#include <cstdio>
#endif

namespace
{
    // one key's samples over the open window. The samples are kept rather than folded into a running mean, because
    // a percentile cannot be recovered from a mean and a count.
    struct Bucket
    {
        std::vector<double> samples;
        std::string unit;
        std::string scope;
    };

    struct StatsState
    {
        std::mutex lock;
        std::map<std::string, Bucket> buckets;  // "name|scope" -> its samples
        uint64 windowFromMs = 0;
        uint64 lastWorldMs = 0;
        uint32 bots = 0;
        uint32 sessions = 0;
        std::map<uint64, uint64> lastMapMs;  // (mapId << 32 | instanceId) -> when that map last reported
    };

    StatsState& State()
    {
        static StatsState state;
        return state;
    }

    struct StatsSettings
    {
        bool enabled = true;
        uint32 windowMs = 10000;
    };

    // Read where every other journal setting is read - at use time, never during static initialisation (asking the
    // config from a file-scope initialiser reads unconstructed containers in Config.cpp: an access violation, not
    // an exception, so there is nothing to be tolerant with).
    StatsSettings const& Settings()
    {
        static StatsSettings const settings = []
        {
            StatsSettings s;
            s.enabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Journal.Stats", true, false);
            // 10 s is the sampler's own window and no bot consults it. It is a setting rather than a constant
            // because the only honest lever on the volume of a sampled table is its window, exactly as the
            // retention is the only lever on the journal's size.
            int32 const seconds = sConfigMgr->GetOption<int32>("AiPlayerbot.Journal.StatsSeconds", 10, false);
            s.windowMs = uint32(std::max(1, seconds)) * 1000u;
            return s;
        }();
        return settings;
    }

    uint64 NowMs()
    {
        return uint64(std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count());
    }

    // The process's resident memory in megabytes; 0 when the platform did not answer, which is "not measured" and
    // is why it is written as a sample only when it is above zero.
    double ResidentMb()
    {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
            return double(counters.WorkingSetSize) / (1024.0 * 1024.0);
        return 0.0;
#else
        if (FILE* statm = std::fopen("/proc/self/statm", "r"))
        {
            unsigned long long pages = 0, resident = 0;
            int const read = std::fscanf(statm, "%llu %llu", &pages, &resident);
            std::fclose(statm);
            if (read == 2)
                return double(resident) * 4096.0 / (1024.0 * 1024.0);
        }
        return 0.0;
#endif
    }

    // Caps a bucket so a window can never grow without bound (a map updating every few ms for ten seconds is a few
    // thousand samples). The cap is stated in the row through `samples`: a max over two samples is not a max.
    constexpr size_t SAMPLES_MAX = 8192;

    // The caller holds StatsState::lock. Kept that way on purpose: one lock for one push_back, and never a syscall
    // or a query inside it (the memory reading is taken before the lock for exactly that reason).
    void Add(std::string const& name, std::string const& scope, char const* unit, double value)
    {
        StatsState& state = State();
        Bucket& bucket = state.buckets[name + "|" + scope];
        if (bucket.unit.empty())
        {
            bucket.unit = unit;
            bucket.scope = scope;
        }
        if (bucket.samples.size() < SAMPLES_MAX)
            bucket.samples.push_back(value);
    }

    // The 95th percentile: the element at index floor(0.95 * n) of the sorted order, clamped to the last one. The
    // rule is written out rather than left to be inferred from the number, and so is its consequence: for a window
    // of fewer than 20 samples that index IS the largest sample, so p95 and max are the same number there. That is
    // why `samples` is a column of the row - a percentile over 12 samples is not the same statement as one over
    // 200, and a reader who cannot tell them apart will read the first as if it were the second.
    double Percentile95(std::vector<double>& values)
    {
        if (values.empty())
            return 0.0;
        size_t rank = size_t(double(values.size()) * 0.95);
        if (rank >= values.size())
            rank = values.size() - 1;
        std::nth_element(values.begin(), values.begin() + rank, values.end());
        return values[rank];
    }

    // Writes the open window out, one metric row per (name, scope), and starts a fresh one. World thread only.
    void CloseWindow(uint64 nowMs)
    {
        StatsState& state = State();
        std::map<std::string, Bucket> taken;
        uint64 fromMs = 0;
        uint32 bots = 0;
        uint32 sessions = 0;
        {
            std::lock_guard<std::mutex> lock(state.lock);
            taken.swap(state.buckets);
            fromMs = state.windowFromMs;
            bots = state.bots;
            sessions = state.sessions;
            state.windowFromMs = nowMs;
            // Instances are created and destroyed all day, each with a new id, so the per-map stamps would grow
            // without a bound over a long uptime. A map that reported nothing for a whole window is gone or idle;
            // its stamp goes, and the next report simply starts its interval again (one sample fewer, which
            // `samples` says). This is the sampler's own window, not a rule any behaviour is timed against.
            for (auto it = state.lastMapMs.begin(); it != state.lastMapMs.end();)
                it = nowMs - it->second > Settings().windowMs ? state.lastMapMs.erase(it) : std::next(it);
        }
        for (auto& [key, bucket] : taken)
        {
            if (bucket.samples.empty())
                continue;
            std::string const name = key.substr(0, key.find('|'));
            double total = 0.0;
            double smallest = bucket.samples.front();
            double largest = bucket.samples.front();
            for (double sample : bucket.samples)
            {
                total += sample;
                smallest = std::min(smallest, sample);
                largest = std::max(largest, sample);
            }

            Journal::Metric metric;
            metric.name = name;
            metric.scope = bucket.scope;
            metric.unit = bucket.unit;
            metric.value = total / double(bucket.samples.size());
            metric.fromMs = fromMs;
            metric.toMs = nowMs;
            metric.valueMin = smallest;
            metric.valueMax = largest;
            metric.valueP95 = Percentile95(bucket.samples);
            metric.samples = uint32(bucket.samples.size());
            metric.bots = bots;
            metric.sessions = sessions;
            Journal::RecordMetric(metric);
        }
    }
}  // namespace

namespace Journal
{
    void NoteMapTick(uint32 mapId, uint32 instanceId, uint32 diff)
    {
        if (!Settings().enabled)
            return;
        uint64 const nowMs = NowMs();
        uint64 const key = (uint64(mapId) << 32) | uint64(instanceId);
        std::string const scope = "map:" + std::to_string(mapId) + "/" + std::to_string(instanceId);

        StatsState& state = State();
        std::lock_guard<std::mutex> lock(state.lock);
        if (!state.windowFromMs)
            state.windowFromMs = nowMs;
        // the diff the core itself handed this map: the same quantity the core measures, not a second reading of it
        Add("map_diff_ms", scope, "ms", double(diff));
        uint64& last = state.lastMapMs[key];
        // the first report of a map has no interval behind it, and an interval invented from the window's start
        // would be a number about nothing. It is skipped, and `samples` in the row says one fewer.
        if (last && nowMs >= last)
            Add("map_tick_ms", scope, "ms", double(nowMs - last));
        last = nowMs;
    }

    void NoteWorldTick(uint32 diff, uint32 bots, uint32 sessions)
    {
        if (!Settings().enabled)
            return;
        uint64 const nowMs = NowMs();
        // taken BEFORE the lock: a syscall inside a mutex every map thread also waits on is the sampler disturbing
        // what it measures, which is the one thing it must not do
        double const residentMb = ResidentMb();
        uint64 const queued = QueuedRows();
        uint64 const dropped = DroppedRows();
        StatsState& state = State();
        uint64 fromMs = 0;
        {
            std::lock_guard<std::mutex> lock(state.lock);
            if (!state.windowFromMs)
                state.windowFromMs = nowMs;
            fromMs = state.windowFromMs;
            state.bots = bots;
            state.sessions = sessions;
            Add("world_diff_ms", "server", "ms", double(diff));
            if (state.lastWorldMs && nowMs >= state.lastWorldMs)
                Add("world_tick_ms", "server", "ms", double(nowMs - state.lastWorldMs));
            state.lastWorldMs = nowMs;
            Add("bots_online", "server", "bots", double(bots));
            Add("sessions_online", "server", "sessions", double(sessions));
            // [jrn31] the journal's own two numbers. The queue depth is the honest sign of whether the one push
            // mutex shared by every map thread is being fought over: while it stays near zero and no row is ever
            // dropped there is no contention, and changing the writer would be work without a reason.
            Add("journal_queue_rows", "server", "rows", double(queued));
            Add("journal_dropped_rows", "server", "rows", double(dropped));
            if (residentMb > 0.0)
                Add("memory_resident_mb", "server", "mb", residentMb);
        }
        if (nowMs - fromMs >= Settings().windowMs)
            CloseWindow(nowMs);
    }
}  // namespace Journal
