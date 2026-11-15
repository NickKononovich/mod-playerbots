/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TESTS_FIXTURE_H
#define PLAYERBOTS_TESTS_FIXTURE_H

// [test1] A fixture is a FILE, never a query. The journal's retention is 24 h on the stream and 48 h on the narrow
// tables, and the sweep drops whole partitions: a test that asked the live journal for its rows would stop finding
// them inside two days, and until then it would quietly change meaning as rows aged out. So tools/export_fixture.py
// writes what it read into tests/fixtures/, those files are committed, and the tests read only files.
//
// The format is a flat JSON object - one level, numbers, booleans and strings. Nothing filters on a fixture file, so
// JSON costs nothing here (the journal's "no filtering on JSON" rule is about the database's columns, not about
// these files), and a flat object needs no library: the reader below is the whole dependency.

#include <map>
#include <string>
#include <vector>

namespace PbFixture
{
    class Flat
    {
    public:
        // false: the file could not be read or is not a flat JSON object (`error` says which)
        bool Load(std::string const& path, std::string& error);

        bool Has(std::string const& key) const { return _values.find(key) != _values.end(); }
        // the raw text of the value, "" when absent
        std::string Raw(std::string const& key) const;
        std::string Str(std::string const& key, std::string const& fallback = std::string()) const;
        // `fallback` is returned when the key is absent: absence is not zero, so every caller names its own
        double Num(std::string const& key, double fallback) const;
        int64_t Int(std::string const& key, int64_t fallback) const;
        bool Bool(std::string const& key, bool fallback) const;

        std::string const& Path() const { return _path; }

    private:
        std::string _path;
        std::map<std::string, std::string> _values;  // key -> raw value text (strings already unquoted)
    };

    // every *.json in `dir`, sorted by name; empty when the directory is missing
    std::vector<std::string> ListJson(std::string const& dir);

    // the fixture root the tests read: PB_FIXTURE_DIR from the environment, else the path baked in at configure
    // time (PB_FIXTURE_DIR_DEFAULT). A rig that found no fixtures must say so and fail, never pass quietly.
    std::string Root();
}

#endif
