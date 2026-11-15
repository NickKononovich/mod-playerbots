/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "IssueKey.h"

#include <algorithm>  // [jrn47] the reason set is sorted and deduplicated

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
        // [supply131] THE SECOND CAUSE FIELD, AND IT WAS PICKED BY MEASURING HOW OFTEN IT TURNS OVER INSIDE ONE
        // EPISODE - not by how causal it looks. That measurement is the whole point, because a graph that flips
        // mid-episode does not narrow the key, it SPLITS ONE EPISODE INTO TWO KEYS, which is the very staircase
        // [jrn41] took out of this function. On b504's unheld standing samples (episode identity
        // `at_ms - stood_sec*1000`, a TIME join and marked as one), over 686 episodes with more than one sample:
        //   * "is the bot in a group" flipped inside 5 of 686 episodes (0.7 %)  -> it may be in the key;
        //   * "did its engine execute nothing" flipped inside 181 of 686 (26.4 %) -> a COLUMN and never the key,
        //     although it splits the largest key almost in half (1068 against 888) and reads like a cause.
        // And it is a cause: of that key's 1956 unheld samples over 43 bots, 1284 stood out of any group and 672
        // in one, and the 2601 s of #42 Cassand and #44 Blahadrin that blocked a whole certification batch were
        // the out-of-group nature ([supply131], a five member with no group, held at the gate by a supply wait).
        //
        // ONLY THE OUT-OF-GROUP CASE SPELLS ITS GRAPH. Unstated (std::nullopt, an emitter that does not hold the
        // fact) and `true` both leave the key byte for byte what it was, so every count already measured on
        // b501-b504 goes on meaning what it meant - the same rule by which [jrn45] appended the place suffix to
        // the new case only, and [jrn54] wrote the walking part only when it was non-zero.
        (void)actionTrace;
        return "standing|" + role + "|" + std::string(inCombat ? "combat" : "peace") + "|state " +
               std::to_string(engineState) + (heldBy.empty() ? std::string() : "|held: " + heldBy) +
               (inGroup.has_value() && !*inGroup ? "|alone" : "");
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
        // [jrn52] AND THE ENGINE'S OWN WORDS FOR "I NAMED NOTHING" ARE NOT "IT NAMED ACTIONS". Engine::LogAction
        // never clears lastAction (src/Bot/Engine/Engine.cpp:932-939), so what arrives here is a cross-tick trail
        // of up to 512 characters, empty only when the engine has logged nothing since the bot logged in - a fact
        // about that login, not about this stuck target: on b503 bot 1814 wrote 3 empty-trail rows beside 24 of
        // its own with a full trail. The fact the label is about is written into the trail in words by the engine
        // itself - "no actions executed", Engine.cpp:367 - and 10 of b503's 199 rows (5 dps, 5 healer) carry that
        // sentence ALONE and were filed as "named actions and none ran", the opposite of what they say. A trail
        // that does name an action ("|A:reach spell - USELESS|no actions executed") named one and keeps its label:
        // only the bare sentence is stripped, so every key measured before this still means what it meant.
        std::string named = actionRunning;
        for (char const* const word : {"|no actions executed", "no actions executed"})
            for (size_t at = named.find(word); at != std::string::npos; at = named.find(word))
                named.erase(at, std::string(word).size());
        return "stuck_target|" + role + "|" +
               (named.empty() ? "its engine named no action" : "its engine named actions and none ran");
    }

    std::string Idle(std::string const& role, IdleBuckets const& buckets, bool inDungeon)
    {
        std::string shape;
        auto const bucket = [&shape](char const* name, uint32_t value)
        {
            if (value)
                shape += (shape.empty() ? "" : "+") + std::string(name);
        };
        // [jrn54] WALKING AND STANDING WERE ONE NAME, and that was the last merged cause this table had left.
        // `noTarget` was the largest of b503's ten unexplained dungeon causes (548 of 1 435 cases) and it was not
        // one cause: the bucket chain asked `!target` and then "out of reach or sight" BEFORE `isMoving()`, so a
        // bot WALKING to the next pack was counted as "it had no target" and a bot RUNNING AT its mob as "out of
        // range", while `moving` could only catch the residue where the target was already in reach and in sight -
        // 6 002 ticks of the build's 96 070 dungeon combat ticks (6.2 %) against 46 119 and 14 851. Measured from
        // the positions the table already carried (LAG over the same bot on the same map): of those 548 windows
        // 373 (68.1 %) belong to a bot that covered 20 yd or more during the window, average displacement 32.4 yd,
        // and 48 (8.8 %) to one that stood inside 5 yd with no named hold. Under one name a fix for either would
        // turn a test green while the other lived on beneath it - the wide direction this seam exists to assert.
        //
        // THE PART IS NAMED AFTER THE WHOLE AND NEVER IN PLACE OF IT, and a window with NO walking part keeps its
        // key CHARACTER FOR CHARACTER: `noTargetMoving` is written only when the walking part is non-zero, which
        // on every build before [jrn52] it could not be (the emitter did not count it), so the keys measured on
        // b501, b502 and b503 go on meaning what they meant. The same rule [jrn45] followed for the place suffix
        // and [jrn42] for the empty hold name.
        //
        // THE PART CANNOT EXCEED THE WHOLE, and if an emitter ever says it does, the standing part is NOTHING -
        // not an unsigned wrap-around that would read as four billion ticks of standing. Nonsense is not a count.
        uint32_t const stoodWithNoTarget = buckets.noTarget > buckets.noTargetMoving
                                               ? buckets.noTarget - buckets.noTargetMoving
                                               : 0;
        uint32_t const stoodOutOfReach =
            buckets.rangeLos > buckets.rangeLosMoving ? buckets.rangeLos - buckets.rangeLosMoving : 0;
        bucket("noTarget", stoodWithNoTarget);
        bucket("noTargetMoving", buckets.noTargetMoving);
        bucket("rangeLos", stoodOutOfReach);
        bucket("rangeLosMoving", buckets.rangeLosMoving);
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
    // [jrn47] the role plus the set of checks that refused, and nothing of the counts or the trace
    std::string CombatSpotNone(std::string const& role, std::vector<std::string> const& reasons,
                               std::string const& /*trace*/)
    {
        // The SET: sorted, so the order the stages were tried in is none of the cause, and without repeats, so
        // one check refusing at three stages is one check. `trace` is deliberately not read - it is the thing
        // that made 219 keys out of 387 events, and the test asserts no part of it reaches the key.
        std::vector<std::string> set = reasons;
        std::sort(set.begin(), set.end());
        set.erase(std::unique(set.begin(), set.end()), set.end());
        std::string shape;
        for (std::string const& reason : set)
            shape += (shape.empty() ? "" : "+") + reason;
        // An event with no reason at all is its own cause ("no candidate searched"): the search produced nothing
        // to refuse, which is a different defect from every candidate being refused, and must not read as one of
        // them nor as an empty key that swallows both.
        return "combat_spot_none|" + role + "|" + (shape.empty() ? std::string("no candidate searched") : shape);
    }

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

    // THE FIRST VERSION OF THIS KEY, KEPT BECAUSE A TEST ASSERTS THE TRAP AGAINST IT: the key as the emitter's
    // own printed sentence, which is exactly how both sibling kinds were written the first time. The printed
    // sentence carries the episode's counts, and the counts climb with every report, so MountEpisodeTest's
    // OneEpisodeIsOneProblemWhateverTheCounts shows seven reports of ONE episode of Emdyl's becoming seven
    // "problems" here against one under MountFlap(). It is called from the test and from nowhere else - on
    // purpose: a measured trap with no witness turns back into an opinion.
    std::string MountFlapNaive(std::string const& printedWhy) { return "mount_flap|" + printedWhy; }

    std::string MountFlap(std::string const& role, bool hasWalk, bool hasLeader, std::string const& justification)
    {
        // The justification is already a closed set of fact names (Journal::MountState::Justification), so it
        // carries no number to take out and no trace to drop: it is the cause, character for character. What is
        // added beside it is the role (as both sibling kinds carry it) and the two facts that decide WHICH judge
        // is flapping - whether a walk was in play, and whether the bot copies a mount leader at all.
        return "mount_flap|" + role + "|" + (hasWalk ? "walk" : "no walk") + "|" +
               (hasLeader ? "led" : "solo") + "|" + justification;
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
