/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LeaderLog.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace Leader
{
    namespace
    {
        class NullSink : public DecisionSink
        {
        public:
            uint32 Write(DecisionRun const&, std::string const&, std::string const&, ObjectGuid, std::string const&,
                         bool, JsonObject, uint32) override
            {
                return 0;
            }
            void NoteConflict(DecisionRun const&, std::string const&, ObjectGuid, std::string const&,
                              JsonObject) override
            {
            }
            void CountConflict(DecisionRun const&, std::string const&) override {}
        };

        DecisionSink* g_decisionSink = nullptr;
    }

    void SetDecisionSink(DecisionSink* sink) { g_decisionSink = sink; }

    DecisionSink& Decisions()
    {
        static NullSink none;
        return g_decisionSink ? *g_decisionSink : none;
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
