/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LeaderLog.h"

#include "Journal.h"  // [jrn20] Journal::Attach, the reference that puts the journal into the link

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <vector>

namespace Leader
{
    namespace
    {
        // nullptr: no primary sink is set, so only the observers (the MySQL journal) see the lines
        DecisionSink* g_decisionSink = nullptr;

        // [jrn20] the primary sink plus its observers, behind the one reference every call site already uses
        class FanOutSink : public DecisionSink
        {
        public:
            uint32 Write(DecisionRun const& run, std::string const& who, std::string const& what, ObjectGuid mob,
                         std::string const& mobName, bool decides, JsonObject fields, uint32 ms) override
            {
                uint32 const id =
                    g_decisionSink ? g_decisionSink->Write(run, who, what, mob, mobName, decides, fields, ms) : 0;
                for (DecisionSink* observer : _observers)
                    observer->Write(run, who, what, mob, mobName, decides, fields, ms);
                return id;
            }

            void NoteConflict(DecisionRun const& run, std::string const& kind, ObjectGuid mob, std::string const& about,
                              JsonObject fields) override
            {
                if (g_decisionSink)
                    g_decisionSink->NoteConflict(run, kind, mob, about, fields);
                for (DecisionSink* observer : _observers)
                    observer->NoteConflict(run, kind, mob, about, fields);
            }

            void CountConflict(DecisionRun const& run, std::string const& kind) override
            {
                if (g_decisionSink)
                    g_decisionSink->CountConflict(run, kind);
                for (DecisionSink* observer : _observers)
                    observer->CountConflict(run, kind);
            }

            void Add(DecisionSink* observer)
            {
                if (observer && std::find(_observers.begin(), _observers.end(), observer) == _observers.end())
                    _observers.push_back(observer);
            }

        private:
            std::vector<DecisionSink*> _observers;
        };

        FanOutSink& FanOut()
        {
            static FanOutSink sink;
            return sink;
        }
    }

    void SetDecisionSink(DecisionSink* sink)
    {
        g_decisionSink = sink;
        // [jrn20] the earliest point the journal can be attached without a startup hook of its own: the harness
        // sets its sink while the server starts, so the boot row lands then and not at the first dungeon. The guard
        // inside Attach() makes the second caller a no-op.
        Journal::Attach();
    }

    void AddDecisionObserver(DecisionSink* observer) { FanOut().Add(observer); }

    DecisionSink& Decisions()
    {
        FanOutSink& sink = FanOut();
        // [jrn20] The MySQL journal's one reference from outside its own directory, and the reason it is here.
        // The module is a static library: the linker takes an object file out of it only when a symbol in it is
        // asked for. With nothing referring to src/Ai/Journal, its object files were compiled, archived and then
        // left behind - b495 wrote not one row, not even the boot row due on every start, and no file-scope
        // registration inside those files could help, because a discarded object file's constructor never runs.
        // Attaching from here is also where it belongs: this file owns where the leader's decision lines go.
        //
        // The call is made AFTER FanOut() has returned, never from inside the sink's own construction: Attach()
        // reaches AddDecisionObserver(), which calls FanOut() again, and that would be a recursive initialisation
        // of the same static.
        static bool const journalAttached = (Journal::Attach(), true);
        (void)journalAttached;
        return sink;
    }

    std::string JsonEscape(std::string const& text)
    {
        std::string out;
        out.reserve(text.size() + 2);
        out += '"';
        for (char c : text)
        {
            switch (c)
            {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    break;
                case '\t':
                    out += ' ';
                    break;
                default:
                    if (static_cast<unsigned char>(c) >= 0x20)
                        out += c;
                    break;
            }
        }
        out += '"';
        return out;
    }

    void JsonObject::Key(char const* key)
    {
        if (!_body.empty())
            _body += ',';
        _body += JsonEscape(key);
        _body += ':';
    }

    JsonObject& JsonObject::Str(char const* key, std::string const& value)
    {
        Key(key);
        _body += JsonEscape(value);
        return *this;
    }

    JsonObject& JsonObject::Num(char const* key, int64 value)
    {
        Key(key);
        _body += std::to_string(value);
        return *this;
    }

    JsonObject& JsonObject::Real(char const* key, double value)
    {
        Key(key);
        if (!std::isfinite(value))
        {
            _body += "0";
            return *this;
        }
        std::ostringstream out;
        out << std::fixed << std::setprecision(1) << value;
        _body += out.str();
        return *this;
    }

    JsonObject& JsonObject::Bool(char const* key, bool value)
    {
        Key(key);
        _body += value ? "true" : "false";
        return *this;
    }

    JsonObject& JsonObject::Raw(char const* key, std::string const& json)
    {
        Key(key);
        _body += json.empty() ? "null" : json;
        return *this;
    }

    JsonObject& JsonObject::Merge(JsonObject const& other)
    {
        if (other._body.empty())
            return *this;
        if (!_body.empty())
            _body += ',';
        _body += other._body;
        return *this;
    }

    JsonArray& JsonArray::Raw(std::string const& json)
    {
        if (!_body.empty())
            _body += ',';
        _body += json;
        return *this;
    }

    JsonArray& JsonArray::Str(std::string const& value) { return Raw(JsonEscape(value)); }

    JsonArray& JsonArray::Num(int64 value) { return Raw(std::to_string(value)); }
}
