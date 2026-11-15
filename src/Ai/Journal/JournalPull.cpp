/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [jrn20] The journal's first slice: the leader's PULL decision, out of the leader's own decision lines and into
// pb_journal_decision / pb_journal_option.
//
// Why the pull and not something else. Of the leader's decision kinds it is the one that refuses most often - in the
// 1.28 h window measured on 2026-11-14/15, `planner_answer` came out 5 876 times and 4 445 of those were "held",
// another 327 "refused" or "none": three out of four of the leader's pull answers are a refusal, and "why is the
// five standing" was the question four wrong diagnoses in one night were about. The refusals' reasons are the whole
// value, and they were only ever a line of text.
//
// How the lines become one decision, stated here because it is a rule a query has to know (it is written into the
// row as "optionLink"):
//   * `planner_answer` IS the decision. One row per answer: answer -> verdict (plan: taken, held/refused/none as
//     printed), `why` -> why, `pick` -> choice.
//   * `chain_pull` is a verdict on ONE candidate, so it becomes an OPTION of the decision, with its own reason and
//     its own numbers (fightSecLeft, overload, drop, walkSec, healerSec...). It is not a decision row of its own:
//     counting it both ways would be a second, independent tally of the same event - the mistake this journal
//     exists to make impossible.
//   * `pull_commit` and `pull_order` describe the option that WAS taken (its method, pack size and cost), so they
//     become the taken option of the same decision.
//   * `commit_released` is a later, separate choice (giving a commit up), so it is its own decision row.
// The lines of one answer arrive within a few ms of each other, so they are gathered per (run, judge) and the group
// is written out when the next answer for that judge starts or when 1 s passes with nothing added to it.
//
// Nothing here changes a bot's behaviour: the observer only reads lines the leader already prints. It also prints
// nothing of its own, so the stream it mirrors stays the size it was.

#include <map>
#include <string>
#include <vector>

#include "Journal.h"
#include "LeaderLog.h"
#include "Timer.h"

namespace
{
    constexpr uint32 GROUP_IDLE_MS = 1000;

    // The value of one field of a flat JSON object, "" when absent. The leader's `fields` reach the sink as a built
    // JSON object, so this is the only way in; it is deliberately the same shape of reader the harness uses.
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

    double JNumber(std::string const& json, char const* key)
    {
        std::string const text = JField(json, key);
        if (text.empty())
            return 0.0;
        try
        {
            return std::stod(text);
        }
        catch (...)
        {
            return 0.0;
        }
    }

    // "leader" / "tank" / "member:Name" -> which judge spoke, and its name when the line names one. The judge is a
    // column because the same decision taken by a different judge is a different decision, and because a verdict
    // with no owner was twice the real defect behind a wrong diagnosis.
    Journal::Judge JudgeOf(std::string const& who, std::string& name)
    {
        size_t const colon = who.find(':');
        if (colon != std::string::npos)
            name = who.substr(colon + 1);
        std::string const role = who.substr(0, colon);
        if (role == "leader")
            return Journal::Judge::FiveLeader;
        if (role == "guild")
            return Journal::Judge::GuildLeader;
        if (role == "harness")
            return Journal::Judge::Harness;
        if (role.empty())
            return Journal::Judge::Unknown;
        return Journal::Judge::Bot;
    }

    // `answer` as the planner prints it -> the one word a query groups by. Anything unknown is kept as printed
    // rather than folded into a neighbour: a verdict this file does not recognise must stay visible, not be guessed.
    std::string VerdictOf(std::string const& answer)
    {
        if (answer == "plan")
            return "taken";
        return answer.empty() ? "none" : answer;
    }

    struct Pending
    {
        Journal::Decision decision;
        bool hasAnswer = false;
        uint32 lastMs = 0;
    };

    class PullObserver : public Leader::DecisionSink
    {
    public:
        uint32 Write(Leader::DecisionRun const& run, std::string const& who, std::string const& what, ObjectGuid mob,
                     std::string const& mobName, bool /*decides*/, Leader::JsonObject fields, uint32 ms) override
        {
            uint32 const stamp = ms ? ms : getMSTime();
            Sweep(stamp);

            bool const family = what == "planner_answer" || what == "chain_pull" || what == "pull_commit" ||
                                what == "pull_order" || what == "commit_released";
            if (!family)
                return 0;

            std::string const json = fields.Build();
            std::string const key = run.runId + "|" + who;

            if (what == "commit_released")
            {
                // a choice of its own: the commit is given up, at its own moment and for its own reason
                Journal::Decision released;
                Fill(released, run, who, mob, mobName, stamp, json);
                released.topic = "pull_release";
                released.verdict = "released";
                released.choice = JField(json, "pick");
                released.why = JField(json, "why");
                released.readField = "commitMob";
                Journal::Record(released);
                return 0;
            }

            Pending& pending = _open[key];
            pending.lastMs = stamp;

            if (what == "planner_answer")
            {
                if (pending.hasAnswer)
                {
                    Journal::Decision carried;
                    carried.options.swap(pending.decision.options);
                    Flush(pending);
                    pending = Pending();
                    pending.lastMs = stamp;
                    // the candidate verdicts seen before this answer belong to it, not to the one already written
                    pending.decision.options.swap(carried.options);
                }
                Fill(pending.decision, run, who, mob, mobName, stamp, json);
                pending.decision.topic = "pull";
                pending.decision.verdict = VerdictOf(JField(json, "answer"));
                pending.decision.choice = JField(json, "pick");
                pending.decision.why = JField(json, "why");
                pending.decision.readField = "answer";
                pending.hasAnswer = true;
                return 0;
            }

            Journal::Option option;
            option.kind = what;
            option.name = mobName.empty() ? JField(json, "pick") : mobName;
            option.fields = json;
            if (what == "chain_pull")
            {
                std::string const verdict = JField(json, "verdict");
                option.taken = verdict == "taken" || verdict == "walk";
                option.reason = verdict + ": " + JField(json, "why");
                option.cost = JNumber(json, "drop");  // the tank health the candidate would cost
                option.value = JNumber(json, "groupDps");
            }
            else  // pull_commit / pull_order: the option that was taken, with the method and price it was taken at
            {
                option.taken = true;
                option.reason = JField(json, "reason");
                option.cost = JNumber(json, "cost");
                option.value = JNumber(json, "expected");
            }
            // the same candidate is re-judged every tick; one option row per distinct verdict, not per tick
            for (Journal::Option const& already : pending.decision.options)
                if (already.kind == option.kind && already.name == option.name && already.reason == option.reason)
                    return 0;
            pending.decision.options.push_back(option);
            return 0;
        }

        void NoteConflict(Leader::DecisionRun const&, std::string const&, ObjectGuid, std::string const&,
                          Leader::JsonObject) override
        {
        }
        void CountConflict(Leader::DecisionRun const&, std::string const&) override {}

    private:
        void Fill(Journal::Decision& decision, Leader::DecisionRun const& run, std::string const& who, ObjectGuid mob,
                  std::string const& mobName, uint32 stamp, std::string const& json)
        {
            decision.judge = JudgeOf(who, decision.judgeName);
            decision.subject = mobName;
            decision.subjectKind = mob.IsEmpty() ? "" : "creature";
            decision.subjectEntry = mob.IsEmpty() ? 0 : mob.GetEntry();
            decision.subjectLow = mob.IsEmpty() ? 0 : mob.GetCounter();
            decision.costUnit = "tank hp";
            decision.tag = JField(json, "tag");
            decision.src = "Ai/Leader/Dungeon/LeaderPlan.cpp";
            // "?" means "not established yet", and it is NOT the same as "" ("checked, nobody reads it"). Keeping
            // the two apart is the point: an empty reader is a finding, an unchecked one is only a gap.
            decision.readBy = "?";
            decision.runId = run.runId;
            decision.runMs =
                run.runStartMs && int32(stamp - run.runStartMs) >= 0 ? getMSTimeDiff(run.runStartMs, stamp) : 0;
            decision.fields = json;
        }

        void Flush(Pending& pending)
        {
            if (!pending.hasAnswer)
                return;
            // the rule by which the options were attached travels with the row: a reader must never have to guess
            // how a child row got there (`text` stays empty - the leader prints these lines as JSON, not as a
            // sentence, so there is no printed sentence to claim one)
            std::string& fields = pending.decision.fields;
            std::string const note = "\"optionLink\":\"same run and judge, the lines of one answer within 1000 ms\"";
            if (fields.size() >= 2 && fields.front() == '{' && fields.back() == '}')
                fields = fields.substr(0, fields.size() - 1) + (fields.size() > 2 ? "," : "") + note + "}";
            else
                fields = "{" + note + "}";
            Journal::Record(pending.decision);
        }

        void Sweep(uint32 stamp)
        {
            for (auto it = _open.begin(); it != _open.end();)
            {
                if (getMSTimeDiff(it->second.lastMs, stamp) < GROUP_IDLE_MS)
                {
                    ++it;
                    continue;
                }
                Flush(it->second);
                it = _open.erase(it);
            }
        }

        std::map<std::string, Pending> _open;
    };

    PullObserver g_pullObserver;

    // [jrn20] the journal reads the leader's decision lines as an observer: the harness's DecisionLog stays the
    // primary sink, so no call site and no existing behaviour changes
    [[maybe_unused]] bool const registered = (Leader::AddDecisionObserver(&g_pullObserver), true);
}  // namespace
