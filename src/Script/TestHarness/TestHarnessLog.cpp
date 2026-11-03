/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TestHarnessLog.h"

#include "LeaderOrders.h"  // [lead1] the decision log switches

#include "Creature.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Timer.h"
#include "Unit.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace TestHarness
{
    namespace
    {
        std::mutex fileLock;
        bool dirReady = false;

        // [rot200] a member's own cast in its group's fight: it or a group member near it is in combat. The boss
        // opener a ranged member casts before its first hit lands (Arcane Power, Icy Veins, Mirror Image at the
        // leader's Burn) runs before the game puts it in combat. b218 rfc-h 5D: the arcane mage's Icy Veins and
        // Mirror Image left the leader's ready list at each boss pull (on cooldown) with no counted cast.
        bool InGroupFight(Player* player)
        {
            if (player->IsInCombat())
                return true;
            Group* group = player->GetGroup();
            if (!group)
                return false;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    if (member != player && member->IsInMap(player) && member->IsInCombat())
                        return true;
            return false;
        }

        Player* OwnerPlayer(Unit* unit)
        {
            if (!unit)
                return nullptr;
            return unit->GetCharmerOrOwnerPlayerOrPlayerItself();
        }
    }

    std::string WallClock()
    {
        std::time_t const now = std::time(nullptr);
        std::tm localTime{};
#ifdef _WIN32
        localtime_s(&localTime, &now);
#else
        localtime_r(&now, &localTime);
#endif
        std::ostringstream out;
        out << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
        return out.str();
    }

    std::string TestsDir() { return sLog->GetLogsDir() + "tests/"; }

    void AppendLine(std::string const& fileName, std::string const& line)
    {
        std::lock_guard<std::mutex> lock(fileLock);
        if (!dirReady)
        {
            std::error_code error;
            std::filesystem::create_directories(TestsDir(), error);
            if (error)
                LOG_ERROR("playerbots", "Test harness: cannot create {}: {}", TestsDir(), error.message());
            dirReady = true;
        }

        std::ofstream file(TestsDir() + fileName, std::ios::app);
        if (file)
            file << line << '\n';
    }

    // [lead1] DecisionLog (TestHarnessLog.h, C:/WoW/LEADER_REFACTOR_PLAN.md 2.7)
    DecisionLog& DecisionLog::Instance()
    {
        static DecisionLog instance;
        return instance;
    }

    namespace
    {
        // [lead107] the leader's decision lines (Leader::Decisions) go to the harness's decision log
        bool const decisionSinkSet = (Leader::SetDecisionSink(&DecisionLog::Instance()), true);
    }

    uint32 DecisionLog::Write(DecisionRun const& run, std::string const& who, std::string const& what, ObjectGuid mob,
                              std::string const& mobName, bool decides, JsonObject fields, uint32 ms)
    {
        if (!Leader::LEADER_DECISION_LOG || run.runId.empty())
            return 0;

        uint32 const stamp = ms ? ms : getMSTime();
        uint32 const elapsedMs =
            run.runStartMs && int32(stamp - run.runStartMs) >= 0 ? getMSTimeDiff(run.runStartMs, stamp) : 0;
        std::string conflict;
        std::string text;
        uint32 id = 0;
        {
            std::lock_guard<std::mutex> lock(_lock);
            id = ++_nextId;
            RunState& state = _runs[run.runSeq];
            ++state.lines[what];
            JsonObject line;
            line.Real("t", double(elapsedMs) / 1000.0).Num("id", id).Str("time", WallClock())
                .Str("runId", run.runId).Str("who", who).Str("what", what).Bool("decides", decides);
            if (!mob.IsEmpty())
                line.Str("mob", mobName).Num("mobEntry", mob.GetEntry()).Num("mobLow", mob.GetCounter());
            line.Merge(fields);
            text = line.Build();
            Say(state, elapsedMs, stamp, who, what, mobName, decides, text);

            if (decides)
                ++state.owners[who];
            // [lead17] ONE decider: any decision whose owner is not the leader is a blocker (non_leader_decisions)
            if (decides && who.rfind("leader", 0) != 0)
            {
                ++state.nonLeader[who.substr(0, who.find(':')) + "|" + what];
                ++state.conflicts["non_leader_decision"];
            }
            if (decides && !mob.IsEmpty())
            {
                MobLast& last = state.lastByMob[mob];
                int32 const gap = last.id ? int32(stamp - last.ms) : 0;
                uint32 const gapMs = uint32(gap < 0 ? -gap : gap);
                if (last.id && last.owner != who && gapMs <= Leader::DECISION_CONFLICT_MS)
                {
                    ++state.conflicts["two_owners"];
                    conflict = JsonObject()
                                   .Real("t", double(elapsedMs) / 1000.0).Num("id", ++_nextId)
                                   .Str("time", WallClock()).Str("runId", run.runId).Str("who", "leader")
                                   .Str("what", "conflict").Bool("decides", false).Str("kind", "two_owners")
                                   .Str("mob", mobName).Num("mobEntry", mob.GetEntry())
                                   .Num("mobLow", mob.GetCounter()).Num("aId", last.id).Str("aWho", last.owner)
                                   .Str("aWhat", last.what).Num("bId", id).Str("bWho", who).Str("bWhat", what)
                                   .Num("gapMs", gapMs)
                                   .Build();
                }
                last.owner = who;
                last.what = what;
                last.id = id;
                last.ms = stamp;
            }
        }

        AppendLine(run.runId + ".decisions.jsonl", text);
        if (!conflict.empty())
            AppendLine(run.runId + ".decisions.jsonl", conflict);
        return id;
    }

    namespace
    {
        // [speech] the value of a string / number field in one JSON line ("" when absent)
        std::string JField(std::string const& json, char const* key)
        {
            std::string const pattern = std::string("\"") + key + "\":";
            size_t at = json.find(pattern);
            if (at == std::string::npos)
                return {};
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

        std::string Cut(std::string text, size_t max)
        {
            if (text.size() > max)
                text = text.substr(0, max - 3) + "...";
            return text;
        }
    }

    // [speech] one decision line -> at most one sentence, or nothing (noise / duplicates inside the topic's gap)
    void DecisionLog::Say(RunState& state, uint32 elapsedMs, uint32 stamp, std::string const& who,
                          std::string const& what, std::string const& mobName, bool /*decides*/,
                          std::string const& json)
    {
        if (!Leader::LEADER_SPEECH)
            return;
        // members only acknowledge; target picking and spot bookkeeping are noise
        std::string const why = JField(json, "why");
        std::string const member = JField(json, "member");
        std::string const stateText = JField(json, "state");
        std::string const mob = mobName.empty() ? JField(json, "mob") : mobName;
        if (what == "member_ack" || what == "order_report" || what == "pull_estimate_vs_actual")
            return;
        // a member decides on its own: that is exactly what the user wants to see, flagged
        if (who.rfind("member", 0) == 0)
        {
            if (what != "own_decision")
                return;
            std::string const action = JField(json, "action");
            std::string const kind = JField(json, "kind");
            std::string const topicOwn = what + who + action;
            uint32& lastOwn = state.speechAt[topicOwn];
            if (lastOwn && getMSTimeDiff(lastOwn, stamp) < 15000)
                return;
            lastOwn = stamp ? stamp : 1;
            uint32 const totalOwn = elapsedMs / 1000;
            char clockOwn[16];
            std::snprintf(clockOwn, sizeof(clockOwn), "%u:%02u", totalOwn / 60, totalOwn % 60);
            state.speech.push_back(std::string("|cffff6666[Own decision ") + clockOwn + "]|r " + who + ": " + action +
                                   " (" + kind + ") - " + JField(json, "why"));
            while (state.speech.size() > 60)
                state.speech.pop_front();
            return;
        }
        if (what == "target" || what == "skip_target" || what == "skip_lifted" || what == "target_commit")
        {
            // the leader's target choice: one short line per target, so the user sees what it aims at
            std::string const name = JField(json, "name");
            if (what != "target" || name.empty())
                return;
            std::string text2 = "Next target: " + name + " (score " + JField(json, "score") + ", " +
                                (JField(json, "boss") == "true" ? std::string("boss") : std::string("trash")) + ")";
            uint32& lastT = state.speechAt["target" + name];
            if (lastT && getMSTimeDiff(lastT, stamp) < 15000)
                return;
            lastT = stamp ? stamp : 1;
            uint32 const totalT = elapsedMs / 1000;
            char clockT[16];
            std::snprintf(clockT, sizeof(clockT), "%u:%02u", totalT / 60, totalT % 60);
            state.speech.push_back(std::string("|cff66ccff[Leader ") + clockT + "]|r " + text2);
            while (state.speech.size() > 60)
                state.speech.pop_front();
            return;
        }
        if (what == "leader_spot" || what == "approach_stop" || what == "approach_sight" || what == "pull_hold")
        {
            std::string const note = what == "leader_spot" ? member + " gets a spot (" + why + ", ground " + JField(json, "ground") + ")"
                                                           : what + " " + mobName + ": " + why;
            std::string const topicN = what + member + mobName;
            uint32& lastN = state.speechAt[topicN];
            if (lastN && getMSTimeDiff(lastN, stamp) < 12000)
                return;
            lastN = stamp ? stamp : 1;
            uint32 const totalN = elapsedMs / 1000;
            char clockN[16];
            std::snprintf(clockN, sizeof(clockN), "%u:%02u", totalN / 60, totalN % 60);
            state.speech.push_back(std::string("|cff66ccff[Leader ") + clockN + "]|r " + Cut(note, 220));
            while (state.speech.size() > 60)
                state.speech.pop_front();
            return;
        }

        std::string text;
        std::string topic = what;
        uint32 gapMs = 8000;

        if (what == "leader_event")
        {
            std::string const event = JField(json, "event");
            if (event == "Rest")
            {
                text = member + " rests (" + why + ")";
                topic += member;
                gapMs = 20000;
            }
            else if (event == "Intensity")
            {
                text = "Intensity " + JField(json, "intensity") + " for " + member + " (" + why + ")";
                topic += member + JField(json, "intensity");
                gapMs = 10000;
            }
            else if (event == "External")
            {
                text = member + " casts an external: " + why;
                topic += member + why;
                gapMs = 10000;
            }
            else if (event == "MoveTo" && why.find("aggro loss") != std::string::npos)
            {
                text = member + " runs to the tank (" + why + ")";
                topic += member;
                gapMs = 6000;
            }
            else if (event == "MoveTo")
            {
                bool const hazard = why.find("hazard") != std::string::npos || why.find("cleave") != std::string::npos ||
                                    why.find("aoe") != std::string::npos || why.find("fire") != std::string::npos;
                text = member + (hazard ? " leaves danger: " : " moves: ") + why;
                topic += member + why;
                gapMs = hazard ? 4000 : 10000;
            }
            else if (event == "StandAt")
            {
                text = member + " stands at (" + JField(json, "x") + ", " + JField(json, "y") + "): " + why;
                topic += member + why;
                gapMs = 12000;
            }
            else if (event == "Attack")
            {
                text = member + " attacks " + mob + (JField(json, "aoe").empty() ? std::string() : " (aoe " + JField(json, "aoe") + ")");
                topic += member + mob;
                gapMs = 8000;
            }
            else if (event == "Heal")
            {
                text = member + " heals the group";
                topic += member;
                gapMs = 30000;
            }
            else if (event == "DontAttack")
            {
                text = member + " must not attack " + mob;
                topic += member + mob;
                gapMs = 8000;
            }
            else if (event == "Hold")
            {
                text = member + " holds: " + why;
                topic += member + why;
                gapMs = 15000;
            }
            else if (event == "Ready?")
            {
                text = "Waiting for everyone to be ready: " + why;
                topic += why;
                gapMs = 15000;
            }
            else if (event == "Prepare")
            {
                text = member + " prepares (" + why + ")";
                topic += member;
                gapMs = 20000;
            }
            else if (event == "Loot")
            {
                text = member + " loots";
                topic += member;
                gapMs = 10000;
            }
            else
            {
                text = event + " for " + member + ": " + why;
                topic += event + member;
                gapMs = 10000;
            }
        }
        else if (what == "pull_order")
        {
            text = "Pull " + mob + " by " + JField(json, "method") + ", expecting " + JField(json, "expected") +
                   " mobs. " + JField(json, "reason");
            gapMs = 2000;
        }
        else if (what == "pull_commit")
        {
            text = "Committed to the pull of " + mob;
            gapMs = 2000;
        }
        else if (what == "commit_released")
        {
            text = "Plan for " + mob + " released: " + why;
            gapMs = 2000;
        }
        else if (what == "patrol_window")
        {
            // [lead24] no line for a window without a patrol
            std::string const patrolName = JField(json, "patrol");
            if (patrolName.empty() || patrolName == "-")
                return;
            text = "Patrol " + JField(json, "patrol") + ": window " + stateText + " (gap " + JField(json, "gap") +
                   " yd, reach " + JField(json, "reach") + ")";
            topic += stateText;
        }
        else if (what == "leader_patrol_wait")
        {
            text = "Waiting for patrol " + JField(json, "patrol") + ": " + why;
            gapMs = 15000;
        }
        else if (what == "patrol_step_back")
        {
            text = "Patrol " + JField(json, "patrol") + ": " + stateText + " (" + why + ")";
            gapMs = 15000;
        }
        else if (what == "harness_hold")
        {
            text = "Not moving: " + why + " - " + stateText;
            topic += stateText.substr(0, 24);
            gapMs = 12000;
        }
        else if (what == "guarantee_hold")
        {
            text = "Not pulling " + mob + ": " + why;
            gapMs = 12000;
        }
        else if (what == "leader_aggro_loss")
        {
            text = JField(json, "victim") + " has " + mob + " (" + JField(json, "tankDistance") +
                   " yd from the tank)";
            topic += JField(json, "victim");
            gapMs = 6000;
        }
        else if (what == "leader_aggro_recovered")
        {
            text = mob + " is back on the tank after " + JField(json, "sec") + " s";
            gapMs = 3000;
        }
        else if (what == "spot_rejected_aggro")
        {
            text = "Spot for " + member + " rejected: inside the reach of " + JField(json, "mob") + " (" +
                   JField(json, "distance") + " yd, reach " + JField(json, "aggroRange") + ")";
            topic += member;
            gapMs = 20000;
        }
        else if (what == "invalid_spot")
        {
            text = member + " refused the walk: " + why;
            topic += member;
            gapMs = 20000;
        }
        else if (what == "leader_unstuck")
        {
            text = "Stuck for " + JField(json, "sec") + " s: " + stateText;
            gapMs = 15000;
        }
        else if (what == "spot_gate_stuck")
        {
            text = "Spot gate stuck: " + why;
            gapMs = 15000;
        }
        else if (what == "order_orphan")
        {
            text = "Order orphaned: " + JField(json, "about");
            gapMs = 15000;
        }
        else if (what == "plan_door_step")
        {
            text = "Door on the way to " + mob + ": " + JField(json, "door");
            gapMs = 10000;
        }
        else if (what == "room_order")
        {
            text = "Room order for " + mob;
            gapMs = 20000;
        }
        else if (what == "heal_reach")
        {
            text = "Healer cannot cover " + member + ": " + why;
            topic += member;
            gapMs = 15000;
        }
        else if (what == "member_stuck")
        {
            text = member + " is stuck, no longer waited for";
            topic += member;
            gapMs = 20000;
        }
        else if (what == "leader_presave")
        {
            text = "Pre-save for " + member + ": " + why;
            gapMs = 10000;
        }
        else if (what == "conflict")
        {
            text = "CONFLICT " + JField(json, "kind") + ": " + JField(json, "aWho") + " vs " + JField(json, "bWho");
            topic += JField(json, "kind");
            gapMs = 10000;
        }
        else if (what.rfind("leader_", 0) == 0 || what == "pull_after_hold" || what == "route_blocker_body_pull" ||
                 what == "stall_short_step")
        {
            text = what + (why.empty() ? std::string() : ": " + why) +
                   (mob.empty() ? std::string() : " [" + mob + "]");
            gapMs = 15000;
        }
        else
            return;

        uint32& last = state.speechAt[topic];
        if (last && getMSTimeDiff(last, stamp) < gapMs)
            return;
        last = stamp ? stamp : 1;

        uint32 const total = elapsedMs / 1000;
        char clock[16];
        std::snprintf(clock, sizeof(clock), "%u:%02u", total / 60, total % 60);
        state.speech.push_back(std::string("|cff66ccff[Leader ") + clock + "]|r " + Cut(text, 230));
        while (state.speech.size() > 60)
            state.speech.pop_front();
    }

    std::vector<std::string> DecisionLog::DrainSpeech(uint32 runSeq)
    {
        std::vector<std::string> out;
        std::lock_guard<std::mutex> lock(_lock);
        auto const found = _runs.find(runSeq);
        if (found == _runs.end())
            return out;
        for (std::string const& line : found->second.speech)
            out.push_back(line);
        found->second.speech.clear();
        return out;
    }

    void DecisionLog::NoteConflict(DecisionRun const& run, std::string const& kind, ObjectGuid mob,
                                   std::string const& about, JsonObject fields)
    {
        if (!Leader::LEADER_DECISION_LOG || run.runId.empty())
            return;

        uint32 const now = getMSTime();
        uint32 const elapsedMs =
            run.runStartMs && int32(now - run.runStartMs) >= 0 ? getMSTimeDiff(run.runStartMs, now) : 0;
        std::string text;
        {
            std::lock_guard<std::mutex> lock(_lock);
            ++_runs[run.runSeq].conflicts[kind];
            JsonObject line;
            line.Real("t", double(elapsedMs) / 1000.0).Num("id", ++_nextId).Str("time", WallClock())
                .Str("runId", run.runId).Str("who", "leader").Str("what", "conflict").Bool("decides", false)
                .Str("kind", kind).Str("about", about);
            if (!mob.IsEmpty())
                line.Num("mobEntry", mob.GetEntry()).Num("mobLow", mob.GetCounter());
            line.Merge(fields);
            text = line.Build();
        }
        AppendLine(run.runId + ".decisions.jsonl", text);
    }

    void DecisionLog::CountConflict(DecisionRun const& run, std::string const& kind)
    {
        if (!Leader::LEADER_DECISION_LOG || run.runId.empty())
            return;
        std::lock_guard<std::mutex> lock(_lock);
        ++_runs[run.runSeq].conflicts[kind];
    }

    std::string DecisionLog::SummaryJson(uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        JsonObject lines;
        JsonObject conflicts;
        JsonObject owners;
        JsonObject nonLeader;
        uint32 nonLeaderTotal = 0;
        uint32 conflictTotal = 0;
        auto const it = _runs.find(runSeq);
        if (it != _runs.end())
        {
            for (auto const& [what, count] : it->second.lines)
                lines.Num(what.c_str(), count);
            for (auto const& [kind, count] : it->second.conflicts)
            {
                conflicts.Num(kind.c_str(), count);
                conflictTotal += count;
            }
            for (auto const& [who, count] : it->second.owners)
                owners.Num(who.c_str(), count);
            for (auto const& [key, count] : it->second.nonLeader)
            {
                nonLeader.Num(key.c_str(), count);
                nonLeaderTotal += count;
            }
        }
        return JsonObject().Bool("oneTankMover", Leader::LEADER_ONE_TANK_MOVER).Num("conflictTotal", conflictTotal)
            .Raw("conflicts", conflicts.Build()).Raw("decisionsByOwner", owners.Build()).Raw("lines", lines.Build())
            .Num("nonLeaderDecisionTotal", nonLeaderTotal).Raw("nonLeaderDecisions", nonLeader.Build())
            .Build();
    }

    void DecisionLog::EndRun(uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        _runs.erase(runSeq);
    }

    StatsRegistry& StatsRegistry::Instance()
    {
        static StatsRegistry instance;
        return instance;
    }

    void StatsRegistry::TrackMember(ObjectGuid guid, uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        _memberRun[guid] = runSeq;
        _counters[guid] = MemberCounters();
        _active.store(true, std::memory_order_relaxed);
    }

    void StatsRegistry::TrackInstance(uint32 instanceId, uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        _instanceRun[instanceId] = runSeq;
    }

    void StatsRegistry::UntrackRun(uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        for (auto itr = _memberRun.begin(); itr != _memberRun.end();)
        {
            if (itr->second == runSeq)
            {
                _counters.erase(itr->first);
                itr = _memberRun.erase(itr);
            }
            else
                ++itr;
        }

        for (auto itr = _instanceRun.begin(); itr != _instanceRun.end();)
        {
            if (itr->second == runSeq)
                itr = _instanceRun.erase(itr);
            else
                ++itr;
        }

        _kills.erase(runSeq);
        _deaths.erase(runSeq);
        _killedEntries.erase(runSeq);
        _openers.erase(runSeq);
        _active.store(!_memberRun.empty(), std::memory_order_relaxed);
    }

    uint32 StatsRegistry::RunOfMember(ObjectGuid guid) const
    {
        auto itr = _memberRun.find(guid);
        return itr == _memberRun.end() ? 0 : itr->second;
    }

    MemberCounters StatsRegistry::Snapshot(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(_lock);
        auto itr = _counters.find(guid);
        return itr == _counters.end() ? MemberCounters() : itr->second;
    }

    uint32 StatsRegistry::TakeKills(uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        auto itr = _kills.find(runSeq);
        if (itr == _kills.end())
            return 0;

        uint32 const kills = itr->second;
        itr->second = 0;
        return kills;
    }

    std::vector<uint32> StatsRegistry::TakeKilledEntries(uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        std::vector<uint32> result;
        auto itr = _killedEntries.find(runSeq);
        if (itr != _killedEntries.end())
            result.swap(itr->second);
        return result;
    }

    std::vector<DeathNote> StatsRegistry::TakeDeaths(uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        std::vector<DeathNote> result;
        auto itr = _deaths.find(runSeq);
        if (itr != _deaths.end())
            result.swap(itr->second);
        return result;
    }

    void StatsRegistry::OnDamage(Unit* attacker, Unit* victim, uint32 damage)
    {
        if (!Active() || !victim)
            return;

        // [opener_zero_hit] a member's hit on a mob that dealt nothing (evading, absorbed): noted, nothing counted
        if (!damage)
        {
            Player* source = victim->IsCreature() ? OwnerPlayer(attacker) : nullptr;
            if (!source)
                return;

            std::lock_guard<std::mutex> lock(_lock);
            uint32 const runSeq = RunOfMember(source->GetGUID());
            if (!runSeq)
                return;

            constexpr size_t MAX_OPENER_ZERO_HITS = 40;
            OpenerNote& note = _openers[runSeq];
            if (note.memberFirstZeroHits.size() < MAX_OPENER_ZERO_HITS &&
                std::none_of(note.memberFirstZeroHits.begin(), note.memberFirstZeroHits.end(),
                             [source](auto const& hit) { return hit.first == source->GetGUID(); }))
                note.memberFirstZeroHits.emplace_back(source->GetGUID(), getMSTime());
            return;
        }

        Player* source = OwnerPlayer(attacker);
        Player* target = victim->ToPlayer();
        if (!source && !target)
            return;

        std::lock_guard<std::mutex> lock(_lock);
        if (source && RunOfMember(source->GetGUID()) && !victim->IsPlayer())
            _counters[source->GetGUID()].damageDone += damage;

        if (target && RunOfMember(target->GetGUID()))
            _counters[target->GetGUID()].damageTaken += damage;

        // The first hit between a member and a mob since the last fight ("who aggroed first").
        uint32 const sourceRun = source && victim->IsCreature() ? RunOfMember(source->GetGUID()) : 0;
        uint32 const targetRun = target && attacker && attacker->IsCreature() ? RunOfMember(target->GetGUID()) : 0;
        if (uint32 const runSeq = sourceRun ? sourceRun : targetRun)
        {
            OpenerNote& note = _openers[runSeq];
            if (!note.hitMs)
            {
                note.hitMs = getMSTime();
                note.hitByMember = sourceRun != 0;
                note.hitBy = sourceRun ? source->GetName() : attacker->GetName();
                note.hitByGuid = sourceRun ? source->GetGUID() : attacker->GetGUID();
                note.hitVictim = victim->GetName();
            }

            constexpr size_t MAX_OPENER_HITS = 40;
            if (sourceRun && note.memberFirstHits.size() < MAX_OPENER_HITS &&
                std::none_of(note.memberFirstHits.begin(), note.memberFirstHits.end(),
                             [source](auto const& hit) { return hit.first == source->GetGUID(); }))
                note.memberFirstHits.emplace_back(source->GetGUID(), getMSTime());
        }
    }

    void StatsRegistry::OnPlayerEnterCombat(Player* player, Unit* enemy)
    {
        if (!Active() || !player)
            return;

        std::lock_guard<std::mutex> lock(_lock);
        uint32 const runSeq = RunOfMember(player->GetGUID());
        if (!runSeq)
            return;

        OpenerNote& note = _openers[runSeq];
        constexpr size_t MAX_OPENER_MEMBERS = 40;
        if (note.members.size() >= MAX_OPENER_MEMBERS)
            return;

        for (OpenerMember const& entry : note.members)
            if (entry.guid == player->GetGUID())
                return;

        OpenerMember entry;
        entry.guid = player->GetGUID();
        entry.name = player->GetName();
        entry.enemy = enemy ? enemy->GetName() : "";
        entry.enemyEntry = enemy && enemy->IsCreature() ? enemy->GetEntry() : 0;
        entry.ms = getMSTime();
        note.members.push_back(entry);
    }

    void StatsRegistry::OnCreatureEngage(Unit* creature, Unit* victim)
    {
        if (!Active() || !creature || !victim || !creature->IsCreature())
            return;

        Player* owner = OwnerPlayer(victim);
        if (!owner)
            return;

        std::lock_guard<std::mutex> lock(_lock);
        uint32 const runSeq = RunOfMember(owner->GetGUID());
        if (!runSeq)
            return;

        OpenerNote& note = _openers[runSeq];
        if (note.mobMs)
            return;

        note.mobMs = getMSTime();
        note.mob = creature->GetName();
        note.mobEntry = creature->GetEntry();
        note.mobSpawnId = creature->ToCreature()->GetSpawnId();
        note.mobVictim = victim->GetName();
        note.mobVictimGuid = owner->GetGUID();
    }

    OpenerNote StatsRegistry::Opener(uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        auto itr = _openers.find(runSeq);
        return itr == _openers.end() ? OpenerNote() : itr->second;
    }

    void StatsRegistry::ResetOpener(uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        _openers.erase(runSeq);
    }

    void StatsRegistry::OnHeal(Unit* healer, Unit* /*target*/, uint32 gain)
    {
        if (!gain || !Active())
            return;

        Player* source = OwnerPlayer(healer);
        if (!source)
            return;

        std::lock_guard<std::mutex> lock(_lock);
        if (RunOfMember(source->GetGUID()))
            _counters[source->GetGUID()].healingDone += gain;
    }

    // [harness20] b218 5H bear Rtdruae at Taragaman spent ~490 rage in 60 s (Maul 17, Mangle 12, Lacerate 9) where the
    // sim's income at the same 94 dtps is ~400: each source of the member's own power counted (the member, not its
    // pet - a pet's focus / energy is its own bar)
    void StatsRegistry::OnPowerGain(Unit* unit, uint8 power, int32 gain, uint8 source, uint32 spellId)
    {
        if (gain <= 0 || !Active() || !unit || !unit->IsPlayer())
            return;

        std::lock_guard<std::mutex> lock(_lock);
        if (RunOfMember(unit->GetGUID()))
            _counters[unit->GetGUID()].powerGains[PowerGainKey(power, source, spellId)] += uint64(gain);
    }

    void StatsRegistry::OnSpellCast(Player* player, Spell* spell, uint32 castItemEntry)
    {
        if (!Active() || !player || !spell || !spell->m_spellInfo)
            return;

        // [b31_harness] The cast mix counted at PLAYERHOOK_ON_SPELL_CAST, which runs at the START of Spell::_cast,
        // before the final CheckCast(false) and the target selection: a cast bar ending in a failure (target gone,
        // out of LOS, immune) and a next-swing spell refused at the swing (Heroic Strike / Cleave / Rune Strike
        // without rage / runic power) were counted as casts (Prot Warrior pulls at 70-80 casts/min, 20 HS+Cleave in a
        // 36 s pull). Now the caller is the end of _cast: the spell went off (SPELL_GO). Only the member's own casts:
        // not a triggered spell (procs, channel / periodic ticks, auto-repeat shots), not an item, not a cast whose
        // original caster is someone else.
        if (spell->GetCaster() != player || (spell->GetOriginalCaster() && spell->GetOriginalCaster() != player))
            return;

        SpellInfo const* info = spell->m_spellInfo;
        bool const triggered = spell->IsTriggered();
        bool const interrupt = info->HasEffect(SPELL_EFFECT_INTERRUPT_CAST);
        bool const dispel = info->HasEffect(SPELL_EFFECT_DISPEL);
        // Rotation review: what the member casts by its own choice in a fight (procs and triggered casts excluded).
        bool const rotation = InGroupFight(player) && !triggered && !castItemEntry;
        if (!interrupt && !dispel && !castItemEntry && !rotation)
            return;

        uint64 busy = 0;
        if (rotation)
        {
            // GCD: StartRecoveryTime (1.5 s, 1 s for energy users), hasted for spells, never below 1 s.
            float gcd = float(info->StartRecoveryTime);
            if (gcd > 0.0f && info->DmgClass != SPELL_DAMAGE_CLASS_MELEE && info->DmgClass != SPELL_DAMAGE_CLASS_RANGED)
                gcd = std::max(1000.0f, gcd * player->GetFloatValue(UNIT_MOD_CAST_SPEED));
            float const cast = float(std::max(spell->GetCastTime(), 0));
            float const channel = info->IsChanneled() ? float(std::max(info->GetDuration(), 0)) *
                                                            player->GetFloatValue(UNIT_MOD_CAST_SPEED)
                                                      : 0.0f;
            busy = uint64(std::max({gcd, cast, channel}));
        }

        uint32 const itemEntry = castItemEntry;
        std::lock_guard<std::mutex> lock(_lock);
        if (!RunOfMember(player->GetGUID()))
            return;

        MemberCounters& counters = _counters[player->GetGUID()];
        if (interrupt)
            ++counters.interrupts;
        if (dispel)
            ++counters.dispels;
        if (itemEntry)
            ++counters.itemUses[itemEntry];
        if (rotation)
        {
            ++counters.spells[info->Id].casts;
            counters.busyMs += busy;

            // [b31_harness] Long cooldowns (>= 60 s): a second counted cast well inside the cooldown is either a
            // real reset (Cold Snap, Preparation, Readiness, a harness re-prepare) or a counting error. The audits
            // saw Arcane Power 2.6/min, Presence of Mind 2.3/min, Metamorphosis 1.1/min; this line names the case.
            uint32 const recovery = std::max(info->RecoveryTime, info->CategoryRecoveryTime);
            if (recovery >= 60 * IN_MILLISECONDS)
            {
                static std::map<std::pair<ObjectGuid, uint32>, uint32> lastLongCooldownCast;  // under _lock
                uint32 const now = getMSTime();
                auto [itr, fresh] = lastLongCooldownCast.try_emplace({player->GetGUID(), info->Id}, now);
                if (!fresh)
                {
                    uint32 const since = getMSTimeDiff(itr->second, now);
                    if (since < recovery * 9 / 10)
                        LOG_INFO("playerbots",
                                 "[b31_harness] cast inside cooldown: {} {} ({}) again after {:.1f} s, cooldown "
                                 "{:.0f} s",
                                 player->GetName(), info->SpellName[LOCALE_enUS] ? info->SpellName[LOCALE_enUS] : "",
                                 info->Id, since / 1000.0f, recovery / 1000.0f);
                    itr->second = now;
                }
            }
        }
    }

    void StatsRegistry::OnSpellAmount(Unit* source, Unit* other, SpellInfo const* spell, uint32 amount, bool heal)
    {
        if (!amount || !spell || !Active())
            return;

        Player* first = OwnerPlayer(source);
        Player* second = OwnerPlayer(other);
        std::lock_guard<std::mutex> lock(_lock);
        // The heal hooks pass (target, healer) from aura ticks but (healer, target) from HealBySpell: credit the side
        // that cast the spell.
        Player* caster = nullptr;
        for (Player* candidate : {first, second})
        {
            if (caster || !candidate || !RunOfMember(candidate->GetGUID()))
                continue;
            auto counters = _counters.find(candidate->GetGUID());
            if (counters != _counters.end() && counters->second.spells.count(spell->Id))
                caster = candidate;
        }
        if (!caster && !heal && first && RunOfMember(first->GetGUID()))
            caster = first;
        if (!caster)
            return;

        SpellCounters& counters = _counters[caster->GetGUID()].spells[spell->Id];
        if (heal)
            counters.healing += amount;
        else
            counters.damage += amount;
    }

    // [b30_topped_up] user rule 2026-10-25: «если нет пути обратно, то можно перезайти в данж. Но это будет
    // считаться как смерть» - [b30_reenter] every re-entry (user 2026-10-25: «перезаход это считается как смерть»)
    // is one more death in the member's counters (run_end deaths, the clean-pass
    // verdict, the streaks) and a death note (the "death" event, killer "stranded_reenter: ...").
    void StatsRegistry::RecordReentry(Player* player, std::string const& killer)
    {
        if (!Active() || !player)
            return;

        std::lock_guard<std::mutex> lock(_lock);
        uint32 const runSeq = RunOfMember(player->GetGUID());
        if (!runSeq)
            return;

        ++_counters[player->GetGUID()].deaths;
        DeathNote note;
        note.guid = player->GetGUID();
        note.name = player->GetName();
        note.killer = killer;
        note.ms = getMSTime();
        _deaths[runSeq].push_back(note);
    }

    void StatsRegistry::OnUnitDeath(Unit* victim, Unit* killer)
    {
        if (!Active() || !victim || !victim->GetMap())
            return;

        uint32 const instanceId = victim->GetMap()->GetInstanceId();
        std::lock_guard<std::mutex> lock(_lock);
        if (Player* player = victim->ToPlayer())
        {
            uint32 const runSeq = RunOfMember(player->GetGUID());
            if (!runSeq)
                return;

            ++_counters[player->GetGUID()].deaths;
            DeathNote note;
            note.guid = player->GetGUID();
            note.name = player->GetName();
            note.killer = killer ? killer->GetName() : "";
            // Drowning, falling, lava: Player::EnvironmentalDamage kills with the player as its own killer.
            if (killer == victim)
            {
                LiquidData const& liquid = victim->GetLiquidData();
                if (liquid.Status == LIQUID_MAP_UNDER_WATER && !(liquid.Flags & MAP_LIQUID_TYPE_MAGMA))
                    note.killer = "environment: drowning";
                else if (liquid.Status != LIQUID_MAP_NO_WATER && (liquid.Flags & MAP_LIQUID_TYPE_MAGMA))
                    note.killer = "environment: lava";
                else
                    note.killer = "environment";
            }
            note.killerEntry = killer && killer->IsCreature() ? killer->GetEntry() : 0;
            note.ms = getMSTime();
            _deaths[runSeq].push_back(note);
            return;
        }

        if (!instanceId || !victim->IsCreature())
            return;

        // [rot66] a member's own summon (Army of the Dead ghouls, a Raise Dead ghoul, treants, Feral Spirits) is no
        // kill: b201 rfc-h 5D the Taragaman fight (the boss and one add) logged 9 kills with the 8 Army ghouls
        if (victim->GetCharmerOrOwnerPlayerOrPlayerItself())
            return;

        auto itr = _instanceRun.find(instanceId);
        if (itr != _instanceRun.end())
        {
            ++_kills[itr->second];
            std::vector<uint32>& entries = _killedEntries[itr->second];
            if (entries.size() < 256)  // drained every harness tick; the cap only guards a stopped run
                entries.push_back(victim->GetEntry());
        }
    }
}
