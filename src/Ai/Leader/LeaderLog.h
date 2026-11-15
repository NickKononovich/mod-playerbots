/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_LEADERLOG_H
#define PLAYERBOTS_LEADER_LEADERLOG_H

// [lead107] the leader's log sinks: the JSON line builder, the run a decision line belongs to and the decision sink
// the leader writes to. The test harness's DecisionLog is the sink while a run is logged; with none set the lines
// go nowhere. The group's events go through the group view (LeaderGroup::LogEvent).

#include "Define.h"
#include "ObjectGuid.h"

#include <string>

namespace Leader
{
    std::string JsonEscape(std::string const& text);

    // One JSON object, built field by field; every line written is self-contained.
    class JsonObject
    {
    public:
        JsonObject& Str(char const* key, std::string const& value);
        JsonObject& Num(char const* key, int64 value);
        JsonObject& Real(char const* key, double value);
        JsonObject& Bool(char const* key, bool value);
        JsonObject& Raw(char const* key, std::string const& json);
        JsonObject& Merge(JsonObject const& other);
        std::string Build() const { return "{" + _body + "}"; }

    private:
        void Key(char const* key);
        std::string _body;
    };

    class JsonArray
    {
    public:
        JsonArray& Raw(std::string const& json);
        JsonArray& Str(std::string const& value);
        JsonArray& Num(int64 value);
        std::string Build() const { return "[" + _body + "]"; }
        bool Empty() const { return _body.empty(); }

    private:
        std::string _body;
    };

    // The run a decision line belongs to.
    struct DecisionRun
    {
        std::string runId;
        uint32 runSeq = 0;
        uint32 runStartMs = 0;
    };

    // Where the leader's decision lines go (the signatures of the harness's DecisionLog). World thread only.
    class DecisionSink
    {
    public:
        virtual ~DecisionSink() = default;

        // One decision / report line; `ms` is its getMSTime() stamp (0: now). Returns the line's id.
        virtual uint32 Write(DecisionRun const& run, std::string const& who, std::string const& what, ObjectGuid mob,
                             std::string const& mobName, bool decides, JsonObject fields, uint32 ms = 0) = 0;
        // A conflict the caller detected (`about`: the decision line ids or a short description).
        virtual void NoteConflict(DecisionRun const& run, std::string const& kind, ObjectGuid mob,
                                  std::string const& about, JsonObject fields) = 0;
        // Counts a conflict of `kind` without a line of its own.
        virtual void CountConflict(DecisionRun const& run, std::string const& kind) = 0;
    };

    // The sink set last (nullptr: none, the lines are dropped).
    void SetDecisionSink(DecisionSink* sink);

    // [jrn20] A second reader of the same lines. The harness's DecisionLog stays the primary sink (it owns the line
    // ids and the conflict counting); an observer is handed every line as well, which is how the MySQL journal reads
    // the leader's decisions without any call site changing and without two sinks fighting over SetDecisionSink.
    // An observer's return value is ignored, so only the primary decides a line's id. World thread only.
    void AddDecisionObserver(DecisionSink* observer);

    // The primary sink and its observers as one sink.
    DecisionSink& Decisions();
}

#endif
