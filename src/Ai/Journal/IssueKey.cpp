/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "IssueKey.h"

namespace Journal::Key
{
    std::string Standing(std::string const& role, bool inCombat, uint32_t engineState, std::string const& heldBy,
                         std::string const& actionTrace)
    {
        // THE TRACE COMES IN AND DOES NOT GO INTO THE KEY, and that is this function's whole reason to exist:
        // Engine::GetLastAction() is the trace of an instant, it changes on every sample of one episode, and the
        // five rows it made of Kylantino's one episode are in the test beside this file. What is left is what this
        // detector can honestly call a cause: standing through a fight is a different problem from standing in
        // peace, and so is each state of the bot's AI. The trace stays in `why` and in pb_journal_idle.doing.
        //
        // THE ROLE IS A CAUSE and it is in the key, as it is in both sibling kinds (`idle|<role>|<buckets>` and
        // `stuck_target|<role>|...`): a healer standing still is not the same problem as a tank standing still,
        // and it is constant through an episode, so it narrows the key without splitting one episode. Measured on
        // b501's rows: 42 dps, 18 tank and 3 healer episodes out of combat, 3 tank and 2 dps in combat.
        //
        // [jrn42] THE FIRST CAUSE FIELD THIS KEY EVER HAD, and the request for it stood in this very comment:
        // "narrowing it further needs CAUSE FIELDS from the emitter ... and not a string that happens to differ".
        // `heldBy` is such a field - a closed set of names read as facts at the sample's instant, not a text that
        // varies - so it narrows without splitting one episode, and it is what separates a bot standing with no
        // order from a bot standing exactly where its leader told it to. The empty name is kept out of the key
        // instead of being spelled, so the keys already measured on b501 and b502 go on meaning what they meant.
        (void)actionTrace;
        return "standing|" + role + "|" + std::string(inCombat ? "combat" : "peace") + "|state " +
               std::to_string(engineState) + (heldBy.empty() ? std::string() : "|held: " + heldBy);
    }

    std::string StuckTarget(std::string const& role, std::string const& actionRunning)
    {
        // Whether the engine had anything at all to run is the cause ("no action of its combat engine ran on it");
        // WHICH actions it listed is the trace of that instant and belongs in pb_journal_stuck.action_running.
        // [jrn43] AND NEITHER LABEL MAY SAY WHAT CANNOT BE TRUE. "an action ran" was the label of a non-empty
        // trace in 121 of the 123 live occurrences, while the caller is
        // ReleaseIdleTargetNobodyReaches(acted) - it returns at once when `acted`, so NOTHING ran in any row this
        // kind ever writes, and the row's own sentence says so ("with no action that runs"). What the trace being
        // empty or not really tells apart is whether the engine NAMED anything at all, which is the fact this
        // problem is about, so the two labels now say that and nothing more.
        return "stuck_target|" + role + "|" +
               (actionRunning.empty() ? "its engine named no action" : "its engine named actions and none ran");
    }

    std::string Idle(std::string const& role, IdleBuckets const& buckets, bool inDungeon)
    {
        std::string shape;
        auto const bucket = [&shape](char const* name, uint32_t value)
        {
            if (value)
                shape += (shape.empty() ? "" : "+") + std::string(name);
        };
        bucket("noTarget", buckets.noTarget);
        bucket("rangeLos", buckets.rangeLos);
        bucket("moving", buckets.moving);
        bucket("gcd", buckets.gcd);
        bucket("threat", buckets.threat);
        bucket("leaderHold", buckets.leaderHold);
        bucket("other", buckets.other);
        // [jrn43] AND THE REFUSAL BEHIND `other`, WHICH IS WHERE THE CAUSE IS. `other` is the bucket for a tick
        // the engine produced nothing in and the list of causes has no name for; the engine's own answer is
        // recorded beside it (ref_*, and it sums exactly to `other`: 30 842 ticks against 30 842 over b502's
        // hour), so the name goes in the key. Measured: without it, 109 keys stood over 336 refusal combinations,
        // and the largest key alone held 14 of them.
        //
        // IN PARENTHESES AFTER `other`, never in place of it, and NOT WRITTEN AT ALL when `other` is zero: a
        // window with no `other` has no refusal to name, so its key stays character for character the key it had,
        // and the keys already measured on b501 and b502 go on meaning what they meant. The same rule the empty
        // hold name of Standing() follows.
        if (buckets.other)
        {
            std::string refusals;
            auto const refusal = [&refusals](char const* name, uint32_t value)
            {
                if (value)
                    refusals += (refusals.empty() ? "" : "+") + std::string(name);
            };
            refusal("unsaid", buckets.refUnsaid);
            refusal("useless", buckets.refUseless);
            refusal("impossible", buckets.refImpossible);
            refusal("prereq", buckets.refPrereq);
            refusal("failed", buckets.refFailed);
            refusal("blocked", buckets.refBlocked);
            refusal("noCandidate", buckets.refNoCandidate);
            // An absent breakdown is not a breakdown of zeros: a build before the columns existed cannot say, and
            // such a window keeps the bare `other` it used to have instead of claiming a refusal of "none".
            if (!refusals.empty())
                shape += "(" + refusals + ")";
        }
        // [jrn45] AND WHERE IT IDLED, now that the window is measured outside instances too. Until this build the
        // emitter returned at once unless the bot was in a group inside an instance, so every row this key ever
        // wrote was a five in a dungeon and the field could not differ; with a bot levelling alone in the world
        // measured as well, a dungeon five's `gcd+other` and a solo bot's `gcd+other` are two different defects and
        // one key would hide a fix for either behind the other's green test.
        //
        // THE SUFFIX IS ADDED ONLY FOR THE NEW CASE, exactly as [jrn42] added the hold name: the keys already
        // measured on b501 and b502 keep their text to the byte, so the counts in this project's notes go on
        // meaning what they meant.
        //
        // THE ENGINE STATE IS DELIBERATELY NOT IN THIS KEY, although it is in the row as a column and in the
        // sibling Standing() key. A window is 15 s of ticks and the engine may switch inside it, so the state is
        // not a property of the episode the way the role and the place are - the same reason the action trace is
        // kept out. `engine_state` beside `in_combat` in pb_journal_idle is where that slice is taken.
        return "idle|" + role + "|" + shape + (inDungeon ? "" : "|world");
    }

    std::string WaitBehind(bool sameZone)
    {
        // The zone, because the map CANNOT differ: the emitter walks to meet a member only when
        // `member->GetMapId() == bot->GetMapId()`, so the field this key used to carry read "same map" in all 43
        // live rows and in every row it could ever write. The zone is the same fact one step finer and it does
        // come out both ways - a member a few yards behind in the bot's own zone is a follow that has not caught
        // up, a member in another zone is a travel leg behind - and the member's zone is written to the row
        // (pb_journal_stuck.target_zone_id) so that the split can be checked by a query and not by this comment.
        return std::string("wait_behind|") + (sameZone ? "same zone" : "another zone");
    }

    // [jrn46] `run_failed`: the run's own record. The shape, the key and the ranged value over one struct of facts.
    bool RunClean(RunFacts const& facts)
    {
        return facts.result == "done" && facts.bossesKilled >= facts.bossesTotal && !facts.deaths && !facts.wipes;
    }

    std::string RunFailedShape(RunFacts const& facts)
    {
        std::string shape;
        auto const note = [&shape](char const* what, bool on)
        {
            if (on)
                shape += (shape.empty() ? "" : "+") + std::string(what);
        };
        note("not done", facts.result != "done");
        note("boss left", facts.bossesKilled < facts.bossesTotal);
        note("deaths", facts.deaths > 0);
        note("wipes", facts.wipes > 0);
        return shape;
    }

    std::string RunFailed(RunFacts const& facts)
    {
        return "run_failed|" + facts.contentKey + "|" + facts.result + "|" + RunFailedShape(facts);
    }

    RangedValue RunFailedValue(RunFacts const& facts)
    {
        // The order is the order of the shape itself, so the quantity is always one the key left FREE: the part
        // that is IN the key fixes its own number, and a number the key has fixed cannot be a range. `not done`
        // is not in this list - it is the result, which the key carries in full and which is no number at all.
        if (facts.bossesKilled < facts.bossesTotal)
            return {double(facts.bossesTotal - facts.bossesKilled), "boss"};
        if (facts.deaths)
            return {double(facts.deaths), "death"};
        if (facts.wipes)
            return {double(facts.wipes), "wipe"};
        // Every boss down, nobody dead, no wipe, and still not `done`: the seconds are the only fact left open.
        return {double(facts.durationSec), "s"};
    }
}

namespace Journal::Real
{
    bool StaleVictim(std::string const& why)
    {
        // The three reasons that are the normal end of a fight, spelled exactly as TickStaleVictim writes them.
        // Anything else - `evading`, `an idle mob no pull order is for`, or a reason a later build adds - stays a
        // problem: the user's rule for this table is to over-report rather than to miss, so the default is `yes`
        // and only a named routine reason answers `no`.
        return why != "dead" && why != "gone" && why != "out of the map";
    }

    bool AlertKindDuplicatesDomainRow(std::string const& alertKind)
    {
        // The death is recorded by the core's own "killed by" hook, with the killer, the damage and what the judge
        // expected, in pb_journal_death and in an issue row of kind `death`. The alert is the coordinator's notice
        // of that same event 1-2 ms later: 9 against 9 on b502, each alert behind its own death.
        return alertKind == "DEATH" || alertKind == "DEATHS";
    }
}
