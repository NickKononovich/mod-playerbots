/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActorFacts.h"

#include <cstdlib>
#include <functional>

namespace Journal
{
    namespace Facts
    {
        namespace
        {
            // the raw text of one key of a flat JSON object, empty when the key is not there at all
            std::optional<std::string> Raw(std::string const& json, char const* key)
            {
                std::string const pattern = std::string("\"") + key + "\":";
                size_t at = json.find(pattern);
                if (at == std::string::npos)
                    return std::nullopt;
                at += pattern.size();
                if (at < json.size() && json[at] == '"')
                {
                    std::string out;
                    for (size_t i = at + 1; i < json.size() && json[i] != '"'; ++i)
                    {
                        if (json[i] == '\\' && i + 1 < json.size())
                            ++i;
                        out += json[i];
                    }
                    return out;
                }
                size_t const end = json.find_first_of(",}", at);
                return json.substr(at, end == std::string::npos ? std::string::npos : end - at);
            }

            // every {...} object inside the named array, in order
            void EachInArray(std::string const& json, char const* key,
                             std::function<void(std::string const&)> const& fn)
            {
                size_t at = json.find(std::string("\"") + key + "\":[");
                if (at == std::string::npos)
                    return;
                at = json.find('[', at);
                int depth = 0;
                size_t start = std::string::npos;
                for (size_t i = at; i < json.size(); ++i)
                {
                    if (json[i] == '{')
                    {
                        if (depth++ == 0)
                            start = i;
                    }
                    else if (json[i] == '}')
                    {
                        if (--depth == 0 && start != std::string::npos)
                            fn(json.substr(start, i - start + 1));
                    }
                    else if (json[i] == ']' && depth == 0)
                        return;
                }
            }

            // the single {...} object of a named key ("lookAt"), empty when absent
            std::optional<std::string> ObjectAt(std::string const& json, char const* key)
            {
                size_t at = json.find(std::string("\"") + key + "\":{");
                if (at == std::string::npos)
                    return std::nullopt;
                at = json.find('{', at);
                int depth = 0;
                for (size_t i = at; i < json.size(); ++i)
                {
                    if (json[i] == '{')
                        ++depth;
                    else if (json[i] == '}' && --depth == 0)
                        return json.substr(at, i - at + 1);
                }
                return std::nullopt;
            }

            // [jrn50] WHY THE PARTICIPANT IS IN THE RECORD, and why the emitter may name it.
            //
            // The note is what tells one participant row from another, and the journal keeps ONE row per distinct
            // participant of a decision (identity plus note) rather than one per line that mentioned it. One
            // decision is written from several lines - the planner's answer, the issued order, and a chain pull's
            // verdict - and those are DIFFERENT INSTANTS: the planner answers out of combat, the chain pull is
            // judged during a fight. Merging them into one row would splice two moments into one state, and the
            // facts that exist at only one of them (the opener gate's reading) would be lost to the row written
            // first. So an emitter that is looking at its own instant says so, and gets its own row.
            std::string Note(std::string const& o, char const* fallback)
            {
                if (std::optional<std::string> const own = Text(o, "note"); own)
                    return *own;
                return fallback;
            }

            // [jrn50] the mob fields shared by packAt and lookAt
            void ReadMob(ActorFact& a, std::string const& o)
            {
                a.mob = true;
                a.low = UInt(o, "low").value_or(0);
                a.entry = UInt(o, "entry").value_or(0);
                a.name = Text(o, "name").value_or(std::string());
                a.x = Float(o, "x");
                a.y = Float(o, "y");
                a.z = Float(o, "z");
                a.tankYd = Float(o, "tankYd");
                a.wanders = Bool(o, "wanders");
                a.level = UInt(o, "level");
                a.health = UInt(o, "hp");
                a.healthMax = UInt(o, "hpMax");
                a.healthPct = Share(a.health, a.healthMax);
                a.inCombat = Bool(o, "combat");
            }
        }  // namespace

        std::optional<std::string> Text(std::string const& json, char const* key)
        {
            std::optional<std::string> const raw = Raw(json, key);
            if (!raw || raw->empty())
                return std::nullopt;
            return raw;
        }

        std::optional<uint32_t> UInt(std::string const& json, char const* key)
        {
            std::optional<std::string> const raw = Raw(json, key);
            if (!raw || raw->empty())
                return std::nullopt;
            char* end = nullptr;
            double const value = std::strtod(raw->c_str(), &end);
            if (end == raw->c_str() || value < 0.0)
                return std::nullopt;
            return uint32_t(value);
        }

        std::optional<float> Float(std::string const& json, char const* key)
        {
            std::optional<std::string> const raw = Raw(json, key);
            if (!raw || raw->empty())
                return std::nullopt;
            char* end = nullptr;
            double const value = std::strtod(raw->c_str(), &end);
            if (end == raw->c_str())
                return std::nullopt;
            return float(value);
        }

        std::optional<bool> Bool(std::string const& json, char const* key)
        {
            std::optional<std::string> const raw = Raw(json, key);
            if (!raw)
                return std::nullopt;
            if (*raw == "true")
                return true;
            if (*raw == "false")
                return false;
            return std::nullopt;
        }

        std::optional<float> Share(std::optional<uint32_t> const& part, std::optional<uint32_t> const& whole)
        {
            if (!part || !whole || !*whole)
                return std::nullopt;
            return float(100.0 * double(*part) / double(*whole));
        }

        std::vector<ActorFact> ActorsFromFields(std::string const& json)
        {
            std::vector<ActorFact> out;

            EachInArray(json, "packAt",
                        [&out](std::string const& o)
                        {
                            ActorFact a;
                            ReadMob(a, o);
                            a.note = Note(o, "pack member");
                            out.push_back(a);
                        });

            EachInArray(json, "wanderArea",
                        [&out](std::string const& o)
                        {
                            ActorFact a;
                            a.mob = true;
                            a.note = Note(o, "wander area");
                            a.spawnId = UInt(o, "spawn").value_or(0);
                            std::optional<std::string> const checks = Text(o, "checks");
                            a.checks = checks;
                            a.wanders = true;
                            if (checks)
                            {
                                std::string const& c = *checks;
                                a.chkReachesSpot = c.find("reaches spot") != std::string::npos;
                                a.chkPullPath = c.find("pull path") != std::string::npos;
                                a.chkWalksIntoFight = c.find("walks into the fight") != std::string::npos;
                                // "walk" on its own, not the "walk" inside "walks into the fight"
                                bool walk = false;
                                for (size_t i = 0, n = 0; (n = c.find("walk", i)) != std::string::npos; i = n + 4)
                                    if (c.compare(n, 5, "walks") != 0)
                                        walk = true;
                                a.chkWalk = walk;
                            }
                            out.push_back(a);
                        });

            // [jrn50] the single mob the answer is about. It is a row of its own and not a pack member, because the
            // held pull's subject is the one mob a replay has to put back first, and "pack member" is a different
            // note - a query that cannot tell the subject from its neighbours counts the pack as subjects.
            if (std::optional<std::string> const look = ObjectAt(json, "lookAt"); look)
            {
                ActorFact a;
                ReadMob(a, *look);
                a.note = Note(*look, "look target");
                out.push_back(a);
            }

            // [jrn50] THE FIVE. These rows did not exist at all, and they are the ones a death replay needs: the
            // bots' health, mana, levels and supplies at the instant the judge answered. They come out of the
            // judge's own line, written in the judge's own pass - nothing here asks a unit anything.
            EachInArray(json, "five",
                        [&out](std::string const& o)
                        {
                            ActorFact a;
                            a.mob = false;
                            a.note = Note(o, "five");
                            a.name = Text(o, "name").value_or(std::string());
                            a.low = UInt(o, "low").value_or(0);
                            a.level = UInt(o, "level");
                            a.role = Text(o, "role");
                            a.spec = Text(o, "spec");
                            a.health = UInt(o, "hp");
                            a.healthMax = UInt(o, "hpMax");
                            a.healthPct = Share(a.health, a.healthMax);
                            a.mana = UInt(o, "mana");
                            a.manaMax = UInt(o, "manaMax");
                            a.manaPct = Share(a.mana, a.manaMax);
                            a.x = Float(o, "x");
                            a.y = Float(o, "y");
                            a.z = Float(o, "z");
                            a.tankYd = Float(o, "tankYd");
                            a.targetYd = Float(o, "targetYd");
                            a.inCombat = Bool(o, "combat");
                            a.openerReady = Bool(o, "openerReady");
                            a.drinks = UInt(o, "drinks");
                            a.foods = UInt(o, "foods");
                            a.drinksNeed = UInt(o, "drinksNeed");
                            a.doing = Text(o, "doing");
                            out.push_back(a);
                        });

            return out;
        }
    }  // namespace Facts
}  // namespace Journal
