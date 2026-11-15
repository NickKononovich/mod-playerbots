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

    // [jrn20] WHO READS THIS VERDICT. The column the whole journal was asked for, and the only honest way to fill
    // it is to read the consumer, which is what was done for each kind below. Two of the night's wrong diagnoses
    // came from verdicts nobody read at all - one was thrown away by the caller a line later, another was a bare
    // print - and that class is invisible while this column is empty.
    //
    // THE TWO VALUES ARE NOT THE SAME THING, and nothing may blur them:
    //   "?"  not established yet. A GAP. Says nothing about whether anyone reads it.
    //   ""   established: NOBODY reads it. A FINDING, and a defect to go and fix.
    // So a kind is only given a reader here once its consumer has actually been read in the code; anything else
    // keeps "?". Names are files and symbols, never line numbers: a line number in a comment goes stale silently,
    // and a comment about what the code no longer does costs more than a wrong number.
    //
    // What was traced, and where the verdict is actually consumed:
    //   planner_answer   LeaderPlan tests decision.verdict itself (Go clears refusedPicks, Held drives the rest and
    //                    line holds), stores it in lead.verdict* for Steer, and RouteEvents / RoutePatrol / RouteMeet
    //                    read that through PartyRefusal / PartyVerdict; the tank's own engine also tests it
    //                    (ChooseTargetActions).
    //   chain_pull       consumed in the same block that writes it: joinPack when taken, lead.fightMoveTo and
    //                    lead.planAhead when the fight walks. A "rejected" is read by that same branch - not taking
    //                    the pack IS the consumption.
    //   pull_commit      lead.commitMob / commitWhy are set and runCommit() issues the order from them.
    //   pull_order       goes out through Leader::Orders and is picked up by Leader::Orders::GetOpen in
    //                    ChooseTargetActions (the pull_trace ask / pickup / execute steps).
    //   commit_released  clears lead.keptPick and lead.pickJudgedFor, so the pick is judged again from scratch.
    //
    // WHICH OF THESE ACTUALLY LAND IN THE COLUMN TODAY: only planner_answer and commit_released, because they are
    // the two kinds that become a decision row; chain_pull, pull_commit and pull_order become OPTIONS of a decision
    // and option rows have no read_by of their own. Their three entries are kept because the tracing was done and
    // is worth not losing, and they go live the moment one of those kinds gets a row of its own - but five entries
    // here are not five filled columns, and this note is here so the next reader does not count them as coverage.
    char const* ReadBy(std::string const& what)
    {
        if (what == "planner_answer")
            return "LeaderPlan verdict+lead.verdict*; RouteEvents/RoutePatrol PartyRefusal; ChooseTargetActions";
        if (what == "chain_pull")
            return "LeaderEvents: joinPack when taken, lead.fightMoveTo/planAhead when it walks";
        if (what == "pull_commit")
            return "LeaderPlan: lead.commitMob then runCommit issues the order";
        if (what == "pull_order")
            return "Leader::Orders -> ChooseTargetActions Orders::GetOpen (pull_trace)";
        if (what == "commit_released")
            return "LeaderPlan: clears lead.keptPick and lead.pickJudgedFor, the pick is judged again";
        return "?";  // not traced yet - a gap, and deliberately not the empty string
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
                Fill(released, run, who, what, mob, mobName, stamp, json);
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
                Fill(pending.decision, run, who, what, mob, mobName, stamp, json);
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
        void Fill(Journal::Decision& decision, Leader::DecisionRun const& run, std::string const& who,
                  std::string const& what, ObjectGuid mob, std::string const& mobName, uint32 stamp,
                  std::string const& json)
        {
            decision.judge = JudgeOf(who, decision.judgeName);
            decision.subject = mobName;
            decision.subjectKind = mob.IsEmpty() ? "" : "creature";
            decision.subjectEntry = mob.IsEmpty() ? 0 : mob.GetEntry();
            decision.subjectLow = mob.IsEmpty() ? 0 : mob.GetCounter();
            decision.costUnit = "tank hp";
            decision.tag = JField(json, "tag");
            decision.src = "Ai/Leader/Dungeon/LeaderPlan.cpp";
            // who consumes this verdict, traced in the code (see ReadBy). "?" means "not established yet" and is
            // NOT the same as "" ("checked, nobody reads it"): an empty reader is a finding, an unchecked one is
            // only a gap, and the night's two wrong diagnoses were the first kind.
            decision.readBy = ReadBy(what);
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
}

namespace Journal
{
    // [jrn20] The journal reads the leader's decision lines as an observer: the harness's DecisionLog stays the
    // primary sink, so no call site and no existing behaviour changes.
    //
    // This used to be a file-scope object that registered itself. It could not work, and the reason is worth keeping:
    // the module is a static library, nothing outside src/Ai/Journal referred to this file, so the linker never took
    // the object file - and the constructor of an object file that was left behind does not run. The registration
    // therefore could not survive the very discarding it was meant to undo. Journal::Attach() calls this instead,
    // from a translation unit that is certainly linked.
    void AttachPullObserver() { Leader::AddDecisionObserver(&g_pullObserver); }
}
