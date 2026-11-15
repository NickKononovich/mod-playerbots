/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Fixture.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

// the standalone rig (tests/CMakeLists.txt) defines this on the command line; the core's unit_tests target cannot
// take a definition from the module's cmake hook (it is configured later), so there it comes from a generated
// header instead. Either way the path is baked in at configure time and PB_FIXTURE_DIR overrides it at run time.
#ifndef PB_FIXTURE_DIR_DEFAULT
    #include "PbFixtureDir.h"
#endif

namespace PbFixture
{
    namespace
    {
        void SkipSpace(std::string const& text, size_t& at)
        {
            while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n'))
                ++at;
        }

        // a JSON string starting at text[at] == '"'; false: not a string there
        bool ReadString(std::string const& text, size_t& at, std::string& out)
        {
            if (at >= text.size() || text[at] != '"')
                return false;
            ++at;
            out.clear();
            while (at < text.size() && text[at] != '"')
            {
                if (text[at] == '\\' && at + 1 < text.size())
                {
                    ++at;
                    switch (text[at])
                    {
                        case 'n':
                            out += '\n';
                            break;
                        case 't':
                            out += '\t';
                            break;
                        default:
                            out += text[at];
                            break;
                    }
                }
                else
                    out += text[at];
                ++at;
            }
            if (at >= text.size())
                return false;
            ++at;
            return true;
        }
    }

    bool Flat::Load(std::string const& path, std::string& error)
    {
        _path = path;
        _values.clear();
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            error = "cannot open " + path;
            return false;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        std::string const text = buffer.str();

        size_t at = 0;
        SkipSpace(text, at);
        if (at >= text.size() || text[at] != '{')
        {
            error = path + ": not a JSON object";
            return false;
        }
        ++at;
        while (true)
        {
            SkipSpace(text, at);
            if (at < text.size() && text[at] == '}')
                return true;
            std::string key;
            if (!ReadString(text, at, key))
            {
                error = path + ": a key is not a string";
                return false;
            }
            SkipSpace(text, at);
            if (at >= text.size() || text[at] != ':')
            {
                error = path + ": no ':' after key " + key;
                return false;
            }
            ++at;
            SkipSpace(text, at);
            std::string value;
            if (at < text.size() && text[at] == '"')
            {
                if (!ReadString(text, at, value))
                {
                    error = path + ": unterminated string for key " + key;
                    return false;
                }
            }
            else if (at < text.size() && (text[at] == '[' || text[at] == '{'))
            {
                error = path + ": key " + key + " is nested; a fixture is a flat object";
                return false;
            }
            else
            {
                size_t const start = at;
                while (at < text.size() && text[at] != ',' && text[at] != '}')
                    ++at;
                value = text.substr(start, at - start);
                while (!value.empty() && (value.back() == ' ' || value.back() == '\r' || value.back() == '\n' ||
                                          value.back() == '\t'))
                    value.pop_back();
            }
            _values[key] = value;
            SkipSpace(text, at);
            if (at < text.size() && text[at] == ',')
            {
                ++at;
                continue;
            }
            if (at < text.size() && text[at] == '}')
                return true;
            error = path + ": no ',' or '}' after key " + key;
            return false;
        }
    }

    std::string Flat::Raw(std::string const& key) const
    {
        auto const found = _values.find(key);
        return found == _values.end() ? std::string() : found->second;
    }

    std::string Flat::Str(std::string const& key, std::string const& fallback) const
    {
        auto const found = _values.find(key);
        return found == _values.end() ? fallback : found->second;
    }

    double Flat::Num(std::string const& key, double fallback) const
    {
        auto const found = _values.find(key);
        if (found == _values.end() || found->second.empty() || found->second == "null")
            return fallback;
        try
        {
            return std::stod(found->second);
        }
        catch (...)
        {
            return fallback;
        }
    }

    int64_t Flat::Int(std::string const& key, int64_t fallback) const
    {
        auto const found = _values.find(key);
        if (found == _values.end() || found->second.empty() || found->second == "null")
            return fallback;
        try
        {
            return std::stoll(found->second);
        }
        catch (...)
        {
            return fallback;
        }
    }

    bool Flat::Bool(std::string const& key, bool fallback) const
    {
        auto const found = _values.find(key);
        if (found == _values.end())
            return fallback;
        if (found->second == "true" || found->second == "1")
            return true;
        if (found->second == "false" || found->second == "0")
            return false;
        return fallback;
    }

    std::vector<std::string> ListJson(std::string const& dir)
    {
        std::vector<std::string> out;
        std::error_code code;
        if (!std::filesystem::is_directory(dir, code))
            return out;
        for (auto const& entry : std::filesystem::directory_iterator(dir, code))
            if (entry.is_regular_file() && entry.path().extension() == ".json")
                out.push_back(entry.path().string());
        std::sort(out.begin(), out.end());
        return out;
    }

    std::string Root()
    {
        if (char const* fromEnv = std::getenv("PB_FIXTURE_DIR"); fromEnv && *fromEnv)
            return fromEnv;
        return PB_FIXTURE_DIR_DEFAULT;
    }
}
