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

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ActorFacts.h"
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

    // [jrn30] ABSENT IS NOT ZERO, and these three are what keep them apart on the way out of the JSON. A drop of
    // 0.0 means the judge computed no tank-health cost; an absent drop means it never weighed one, and a column
    // that wrote 0 for both would answer a query and be wrong. JNumber above cannot tell them apart by design (it
    // is for the places where the caller has already established the field is there), so the promoted columns use
    // these.
    Optional<float> JFloat(std::string const& json, char const* key)
    {
        std::string const text = JField(json, key);
        if (text.empty())
            return Optional<float>();
        try
        {
            return Optional<float>(std::stof(text));
        }
        catch (...)
        {
            return Optional<float>();
        }
    }

    Optional<uint32> JUInt(std::string const& json, char const* key)
    {
        std::string const text = JField(json, key);
        if (text.empty())
            return Optional<uint32>();
        try
        {
            return Optional<uint32>(uint32(std::stol(text)));
        }
        catch (...)
        {
            return Optional<uint32>();
        }
    }

    Optional<bool> JBool(std::string const& json, char const* key)
    {
        std::string const text = JField(json, key);
        if (text.empty())
            return Optional<bool>();
        return Optional<bool>(text == "true");
    }

    // [jrn30] The keys that are now COLUMNS and therefore leave `fields`. The direction matters: this is a list of
    // what to DROP and not a list of what to keep, so a field the emitter adds tomorrow still lands in the JSON
    // where somebody can see it. A whitelist would silently throw away what nobody foresaw, which is the one thing
    // the insurance field is for. What stays is what is read with the eyes: the full sentence, the verdict's word,
    // the tag, the printed routeTarget, the pick.
    std::string WithoutPromoted(std::string const& json)
    {
        static char const* const PROMOTED[] = {"fightSecLeft", "runInSec",    "openSec",   "opener",
                                               "distance",     "overload",    "drop",      "tankHealth",
                                               "unionSec",     "groupDps",    "walkSec",   "walkYd",
                                               "healer",       "healerWalks", "healerSec", "due",
                                               "stop",         "packMobs",    "fightMobs", "unionMobs",
                                               // [test1] the world gates are columns now, so they leave `fields`
                                               "gateBoss",     "gateWalker",  "gateBuff",  "gateDead",
                                               "gateHolds",    "gateWakes"};
        if (json.size() < 2 || json.front() != '{' || json.back() != '}')
            return json;
        std::string out = "{";
        size_t at = 1;
        while (at < json.size() - 1)
        {
            // one "key":value pair, respecting quotes, brackets and braces inside the value
            size_t const keyFrom = json.find('"', at);
            if (keyFrom == std::string::npos)
                break;
            size_t const keyTo = json.find('"', keyFrom + 1);
            if (keyTo == std::string::npos)
                break;
            std::string const key = json.substr(keyFrom + 1, keyTo - keyFrom - 1);
            size_t i = json.find(':', keyTo);
            if (i == std::string::npos)
                break;
            ++i;
            size_t const valueFrom = i;
            int depth = 0;
            bool inString = false;
            for (; i < json.size(); ++i)
            {
                char const c = json[i];
                if (inString)
                {
                    if (c == '\\')
                        ++i;
                    else if (c == '"')
                        inString = false;
                    continue;
                }
                if (c == '"')
                    inString = true;
                else if (c == '{' || c == '[')
                    ++depth;
                else if (c == '}' || c == ']')
                {
                    if (depth == 0)
                        break;  // the object's own closing brace
                    --depth;
                }
                else if (c == ',' && depth == 0)
                    break;
            }
            bool promoted = false;
            for (char const* name : PROMOTED)
                if (key == name)
                    promoted = true;
            if (!promoted)
            {
                if (out.size() > 1)
                    out += ',';
                out += json.substr(keyFrom, i - keyFrom);
            }
            at = i + 1;
        }
        return out + "}";
    }

    // [jrn50] THE PARTICIPANTS of a decision, out of the judge's own `fields` and into rows that can be filtered.
    //
    // The translation itself lives in ActorFacts.{h,cpp} - a seam with no world, no clock and no database - and is
    // covered by tests/unit/ActorFactsTest.cpp. It was moved out of this file for a measured reason: the bots of the
    // five were not written AT ALL and every mob's level, health and mana was NULL, which is exactly why the two
    // DeathReplay tests skip ("put the same conditions back and run the judge again" had nothing to put back), and a
    // reader that cannot be tested is how that stayed unnoticed. Now a test feeds a recorded line and demands the
    // value reach its column.
    //
    // What the emitters put in the line (LeaderPlan):
    //   five        [{name, low, level, role, spec, hp, hpMax, mana, manaMax, x, y, z, tankYd, targetYd, combat,
    //                openerReady, drinks, foods, drinksNeed, doing}]   - the bots of the five, side = Bot
    //   lookAt       {name, low, entry, level, hp, hpMax, x, y, z, tankYd, combat, wanders}  - the mob answered about
    //   packAt      [{low, entry, x, y, z, tankYd, wanders, level, hp, hpMax, combat}]       - the pack aimed at
    //   wanderArea  [{spawn, checks}]                                  - every wandering spawn the plan judged
    //
    // NOTHING HERE ASKS A UNIT ANYTHING. Every number comes from the judge's own pass under the judge's own lock; a
    // lookup here would be a second reading of the world at a second moment - a second judge of one quantity. A key
    // the emitter left out stays NULL, and NULL is "not supplied", never 0.
    void DecisionActors(std::vector<Journal::Actor>& out, std::string const& json)
    {
        for (Journal::Facts::ActorFact const& f : Journal::Facts::ActorsFromFields(json))
        {
            Journal::Actor a;
            a.side = f.mob ? Journal::Actor::Side::Mob : Journal::Actor::Side::Bot;
            a.name = f.name;
            a.low = f.low;
            a.entry = f.entry;
            a.spawnId = f.spawnId;
            a.level = f.level;
            a.spec = f.spec;
            a.role = f.role;
            a.health = f.health;
            a.healthMax = f.healthMax;
            a.healthPct = f.healthPct;
            a.mana = f.mana;
            a.manaMax = f.manaMax;
            a.manaPct = f.manaPct;
            a.x = f.x;
            a.y = f.y;
            a.z = f.z;
            a.tankYd = f.tankYd;
            a.targetYd = f.targetYd;
            a.inCombat = f.inCombat;
            a.wanders = f.wanders;
            a.openerReady = f.openerReady;
            a.doing = f.doing;
            a.drinks = f.drinks;
            a.foods = f.foods;
            a.drinksNeed = f.drinksNeed;
            a.checks = f.checks;
            a.chkReachesSpot = f.chkReachesSpot;
            a.chkPullPath = f.chkPullPath;
            a.chkWalk = f.chkWalk;
            a.chkWalksIntoFight = f.chkWalksIntoFight;
            a.note = f.note;
            // the same candidate is re-judged every tick and the same pack is printed by several lines of one
            // answer: one row per distinct participant of the decision, not one per line that mentioned it. The
            // identity is the field (guid counter / spawn id) plus why it is in the record, never the name.
            bool already = false;
            for (Journal::Actor const& seen : out)
                if (seen.side == a.side && seen.low == a.low && seen.spawnId == a.spawnId && seen.note == a.note)
                    already = true;
            if (!already)
                out.push_back(a);
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
                    // [jrn50] the participants go with the options they were read from: the candidates judged
                    // before this answer belong to THIS answer, and so do the mobs they named
                    carried.actors.swap(pending.decision.actors);
                    Flush(pending);
                    pending = Pending();
                    pending.lastMs = stamp;
                    // the candidate verdicts seen before this answer belong to it, not to the one already written
                    pending.decision.options.swap(carried.options);
                    pending.decision.actors.swap(carried.actors);
                }
                Fill(pending.decision, run, who, what, mob, mobName, stamp, json);
                pending.decision.topic = "pull";
                pending.decision.verdict = VerdictOf(JField(json, "answer"));
                pending.decision.choice = JField(json, "pick");
                pending.decision.why = JField(json, "why");
                pending.decision.readField = "answer";
                pending.decision.kind = JField(json, "kind");
                // "routeTarget":"GUID Full: 0x... Type: Creature Entry: 3637  Low: 157" -> two columns. The printed
                // string is for the eyes; the identity is what gets filtered, and a name is not an identity.
                std::string const routeTarget = JField(json, "routeTarget");
                if (size_t const e = routeTarget.find("Entry: "); e != std::string::npos)
                    pending.decision.routeTargetEntry = uint32(std::strtoul(routeTarget.c_str() + e + 7, nullptr, 10));
                if (size_t const l = routeTarget.find("Low: "); l != std::string::npos)
                    pending.decision.routeTargetLow = uint32(std::strtoul(routeTarget.c_str() + l + 5, nullptr, 10));
                // [jrn50] the five and the mob answered about become actor rows of THIS decision: the state the
                // judge saw, which is what a replay needs and what pb_journal_actor had none of
                DecisionActors(pending.decision.actors, json);
                pending.hasAnswer = true;
                return 0;
            }

            Journal::Option option;
            option.kind = what;
            option.name = mobName.empty() ? JField(json, "pick") : mobName;
            // [jrn30] `fields` keeps only what is read with the eyes; every number below is a column now
            option.fields = WithoutPromoted(json);
            // [jrn30] WHERE THE OPTION'S NUMBERS COME FROM, which was empty in all 2 503 option rows. A chain-pull
            // candidate is a live creature the judge weighed in the world ("mob"); a committed pull's numbers are
            // the leader's own plan ("plan"). It is a column because "was a drop source among the options at all"
            // is a question that gets asked, and an empty source_kind cannot answer it either way.
            option.sourceKind = what == "chain_pull" ? "mob" : "plan";
            // [jrn30] the judge's numbers out of the JSON and into columns, each absent-aware: a 0.0 drop is a
            // computed zero, an absent drop is a gap, and NULL is what keeps them apart
            option.fightSecLeft = JFloat(json, "fightSecLeft");
            option.runInSec = JFloat(json, "runInSec");
            option.openSec = JFloat(json, "openSec");
            option.opener = JField(json, "opener");
            option.distance = JFloat(json, "distance");
            option.overload = JFloat(json, "overload");
            option.drop = JFloat(json, "drop");
            option.tankHealth = JFloat(json, "tankHealth");
            option.unionSec = JFloat(json, "unionSec");
            option.groupDps = JFloat(json, "groupDps");
            option.walkSec = JFloat(json, "walkSec");
            option.walkYd = JFloat(json, "walkYd");
            option.healer = JField(json, "healer");
            option.healerWalks = JBool(json, "healerWalks");
            option.healerSec = JFloat(json, "healerSec");
            option.due = JBool(json, "due");
            option.stop = JUInt(json, "stop");
            option.packMobs = JUInt(json, "packMobs");
            option.fightMobs = JUInt(json, "fightMobs");
            option.unionMobs = JUInt(json, "unionMobs");
            if (what == "chain_pull")
            {
                // [test1] the judge's world gates. JBool gives an EMPTY Optional for a key the event does not
                // carry, and the emitter leaves out exactly the gates the judge never reached (it returns at the
                // first one that trips), so "never reached" arrives as NULL and is never confused with false.
                option.gateBoss = JBool(json, "gateBoss");
                option.gateWalker = JBool(json, "gateWalker");
                option.gateBuff = JBool(json, "gateBuff");
                option.gateDead = JBool(json, "gateDead");
                option.gateHolds = JBool(json, "gateHolds");
                option.gateWakes = JBool(json, "gateWakes");
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
            // the mobs this option weighed become actor rows of the decision, with their checks as fields
            DecisionActors(pending.decision.actors, json);
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
            // [jrn30] WHO decided and WHERE. `who` is the word "leader" and carries no name and no GUID, which is
            // why judge_name and judge_low were empty in all 9 457 rows: the sink was never handed an identity.
            // The emitter now puts the judge and its place in the line (LeaderPlan), and they become columns here.
            // The GUID counter is the identity and the name is for the eyes - a name is not a unit, and no
            // name-to-unit lookup happens anywhere in this file.
            if (std::string const judge = JField(json, "judge"); !judge.empty())
                decision.judgeName = judge;
            if (Optional<uint32> const low = JUInt(json, "judgeLow"); low)
                decision.judgeLow = *low;
            if (Optional<uint32> const map = JUInt(json, "map"); map)
                decision.mapId = *map;
            if (Optional<uint32> const zone = JUInt(json, "zone"); zone)
                decision.zoneId = *zone;
            // the judge's own coordinates, out of the "tank":[x,y,z] the line carries. Taken HERE and not only at
            // the flush, because a held answer may have no taken option to read it from.
            if (size_t const at = json.find("\"tank\":["); at != std::string::npos)
            {
                float x = 0.f, y = 0.f, z = 0.f;
                if (std::sscanf(json.c_str() + at + 8, "%f,%f,%f", &x, &y, &z) == 3)
                {
                    decision.judgeX = x;
                    decision.judgeY = y;
                    decision.judgeZ = z;
                }
            }
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
            // [jrn20] the promoted columns are filled HERE and not when the answer arrived, because they come from
            // the order that follows it in the same group (method / expected / pack / careful / the tank's position),
            // and the group is only complete at the flush. They are columns rather than JSON because they are what
            // questions are asked by - "every pull bigger than one", "every careful one", "what happened at this
            // spot" - and `fields` keeps whatever nobody filters on.
            for (Journal::Option const& o : pending.decision.options)
            {
                if (!o.taken || o.fields.empty())
                    continue;
                std::string const& f = o.fields;
                if (pending.decision.method.empty())
                    pending.decision.method = JField(f, "method");
                if (!pending.decision.packSize)
                {
                    std::string const pack = JField(f, "pack");
                    if (!pack.empty())
                        pending.decision.packSize = uint32(JNumber(f, "pack"));
                }
                if (!pending.decision.careful)
                {
                    std::string const careful = JField(f, "careful");
                    if (!careful.empty())
                        pending.decision.careful = careful == "true";
                }
                if (!pending.decision.judgeX)
                {
                    // "tank":[x,y,z] - the judge's own position, so a spot on the map becomes a WHERE clause
                    size_t const at = f.find("\"tank\":[");
                    if (at != std::string::npos)
                    {
                        float x = 0.f, y = 0.f, z = 0.f;
                        if (std::sscanf(f.c_str() + at + 8, "%f,%f,%f", &x, &y, &z) == 3)
                        {
                            pending.decision.judgeX = x;
                            pending.decision.judgeY = y;
                            pending.decision.judgeZ = z;
                        }
                    }
                }
            }
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
