/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_JOURNAL_JOURNAL_H
#define PLAYERBOTS_JOURNAL_JOURNAL_H

// [jrn20] The MySQL journal: the server's own history, in `acore_playerbots`, with a millisecond stamp on every row.
//
// Why a table and not the log file. Playerbots.log is truncated at every server start, so the baseline a comparison
// needs disappears with the thing it compares; the window a measurement covers is whatever the run happened to last,
// so raw counts are not comparable and have to be normalised by hand; and the count itself is a grep pattern, so two
// readers measuring "the same" thing can disagree because their patterns match different lines - one of them scoring
// zero by construction, looking for a cause in a line that never carries it. In a table none of that happens: the
// history outlives the restart (`boot_id`), the window is a WHERE clause on `at_ms`, and the count is one query
// everybody runs.
//
// What the journal must make queryable, and why it is built this way: four diagnoses in one night were wrong the same
// way - a log line was taken for the verdict without asking who reads it and what it actually counts. So a journal row
// carries, besides the choice: the judge (`judge`), the code site that printed it (`tag`, `src`), the field the judge
// read (`read_field`), and who reads its verdict (`read_by` - the empty string means "nobody known", which has twice
// turned out to be the truth, the line standing on the losing side of an argument between two judges). The options the
// judge did NOT take are rows of their own in `pb_journal_option`, each with its own reason, cost and source kind - so
// "why did the bot stand" and "where did that rate come from, and was a mob drop counted as a source at all" are
// queries, not investigations.
//
// Cost. Nothing here runs a query on the world thread: Record() formats the row's values and pushes it onto a vector
// under a short lock; a writer thread of its own swaps that vector, builds one multi-row INSERT per batch and hands it
// to PlayerbotsDatabase.Execute(), which is itself asynchronous. Retention drops whole hour partitions
// (ALTER TABLE ... DROP PARTITION), never DELETE, so cleaning a day out of a five-million-row table is a metadata
// operation and holds no lock on the hot partition.
//
// Settings (playerbots.conf, read once at the first row - the retention is a setting, not a constant in the code):
//   AiPlayerbot.Journal.Enable          (bool, default true)   write at all
//   AiPlayerbot.Journal.RetentionHours  (int,  default 48)     how far back the journal keeps rows
//   AiPlayerbot.Journal.BatchRows       (int,  default 500)    rows per INSERT
//   AiPlayerbot.Journal.FlushMs         (int,  default 1000)   how long a partial batch waits
//   AiPlayerbot.Journal.Stream          (bool, default false)  also mirror raw log lines into pb_journal_line
//
// Tables: pb_journal_boot (one row per server start), pb_journal_decision (a choice, parsed into columns),
// pb_journal_option (one row per option the judge weighed, taken or not), pb_journal_read (a reader that actually
// consumed a verdict), pb_journal_line (the raw stream, when enabled or filled by tools/journal_load.py).
// The DDL is also kept as a file for review: data/sql/playerbots/updates/2026_11_15_01_pb_journal_jrn20.sql.

#include <string>
#include <vector>

#include "Define.h"
#include "ObjectGuid.h"
#include "Optional.h"

class Creature;
class Player;
class Unit;

namespace Journal
{
    // Who the judge is. The same decision taken by a different judge is a different decision, and "one judge per
    // decision" is only checkable when the row says which one spoke.
    enum class Judge : uint8
    {
        Bot = 0,          // the bot deciding about itself
        FiveLeader = 1,   // the leader of a five, inside a run
        GuildLeader = 2,  // the global leader of the 25
        Harness = 3,      // the test harness / a tool
        Unknown = 4
    };

    char const* JudgeName(Judge judge);

    // One option the judge weighed. `taken` marks the one it chose; every other row is what it rejected and why.
    // `sourceKind` exists for the priced options (where a number came from): "node", "drop", "vendor", "craft",
    // "quest", "bank", "mail", "" - a plan refused on a rate of 0.01/min is only answerable when the row says whether
    // a drop source was among the options at all, or was never considered.
    struct Option
    {
        std::string name;        // the option ("Mageweave Cloth x4784", "Deviate Ravager (low 167)", "pull now")
        std::string kind;        // the option's class ("mob", "pack", "item", "spot", "plan", "member", ...)
        std::string reason;      // why it was taken or rejected, in the judge's own words
        std::string sourceKind;  // where its number comes from, when it has one
        double cost = 0.0;       // the price the judge put on it (its own unit, named by `costUnit` on the decision)
        double value = 0.0;      // the gain the judge put on it
        bool taken = false;

        // [jrn30] THE CHAIN-PULL JUDGE'S OWN NUMBERS, promoted out of `fields` into columns because they are what
        // the questions are about. "Every refusal where the healer had more than N yards to walk" is a query we
        // want to ask, and while these lived inside one JSON field it could only be asked as a LIKE over text -
        // measured at 384 ms on 411 819 rows, growing linearly, so about 8 s on a day of a thousand bots. The user's
        // rule is the short form of that measurement: we do not filter on JSON; if something is needed it becomes a
        // column. `fields` keeps what is read with the eyes - the full sentence, the tag, the printed strings.
        //
        // EVERY ONE IS OPTIONAL AND NULL IS NOT ZERO. The judge prints these only for the candidate kinds that have
        // them: a 0.0 drop means the judge computed no tank-health cost, an absent drop means it never weighed one,
        // and a query that merges the two will answer and be wrong.
        Optional<float> fightSecLeft;  // seconds the current fight still has to run
        Optional<float> runInSec;      // seconds the tank needs to run in
        Optional<float> openSec;       // seconds its opener takes
        std::string opener;            // the opener the judge meant to use ("" when it named none)
        Optional<float> distance;      // yards to the candidate
        Optional<float> overload;      // the overload gate's number
        Optional<float> drop;          // tank health the candidate would cost (0..1)
        Optional<float> tankHealth;    // the tank's health when it judged (0..1)
        Optional<float> unionSec;      // seconds the joined fight would take
        Optional<float> groupDps;      // the group damage the judge priced it with
        Optional<float> walkSec;       // seconds of walk the plan needs
        Optional<float> walkYd;        // and its length in yards: the two are not interchangeable
        std::string healer;            // the healer the plan leans on ("" when it named none)
        Optional<bool> healerWalks;    // that healer has to walk for it
        Optional<float> healerSec;     // and how long it would take
        Optional<bool> due;            // the pull was due now
        Optional<uint32> stop;         // the route stop this candidate belongs to
        Optional<uint32> packMobs;     // mobs in the candidate's own pack
        Optional<uint32> fightMobs;    // mobs already in the fight
        Optional<uint32> unionMobs;    // mobs in the two together, which is what the gate judges

        // [test1] THE JUDGE'S WORLD GATES, one column each. Until these existed the row carried only `kind`, the
        // name of the gate that rejected, which says nothing about the gates BEFORE it - so a recorded chain-pull
        // verdict could not be replayed at all (ReplayChainPull answered Undecided on the first gate it had no
        // fact for, over all 159 exported fixtures). `kind` is the outcome; these are the inputs behind it.
        // NULL MEANS THE JUDGE NEVER REACHED THAT GATE, which is not false. JudgeChainPull returns at the first
        // gate that trips, so a refusal at the walker gate computed no buff, no dead, no holds and no wakes
        // answer; a reader that wrote those as false would replay a verdict nobody ever made. The emitter leaves
        // the key out of the event entirely and JBool turns an absent key into an empty Optional, so absence
        // travels the whole way instead of being flattened on the first hop.
        Optional<bool> gateBoss;    // no boss in the pack (false: one is there - his own pull, buff pass first)
        Optional<bool> gateWalker;  // the stop is nobody's walking stop
        Optional<bool> gateBuff;    // the group has its buffs
        Optional<bool> gateDead;    // every member alive
        Optional<bool> gateHolds;   // the tank holds everything already in the fight
        Optional<bool> gateWakes;   // the way in (and, for a walk, its end and its path) wakes nothing idle

        std::string fields;  // what is left for the eyes, as one JSON object ("" for none)
    };

    // [jrn20] One participant of one decision: a bot of the five, or a mob the judge weighed. The point is replay -
    // enough of the state the judge saw to put the same conditions back, run the judge again and see whether the
    // verdict turns over, and to make a hundred real rows into unit-test fixtures instead of invented ones.
    //
    // ABSENT IS NOT ZERO, and here it is carried by NULL. Every number the emitter did not supply stays unset and
    // reaches the table as NULL, never as 0: a mob at 0 % health is a corpse, a mob whose health was not recorded is
    // a gap, and a query that cannot tell them apart will answer anyway. `Optional` is what keeps them apart.
    //
    // A share is never stored without its denominator: health and mana go in absolute AND as a percentage, because
    // "11 %" with no pool behind it cannot be checked, and the tank drinking at 11 % mana was exactly that argument.
    struct Actor
    {
        enum class Side : uint8
        {
            Bot = 0,   // a member of the five
            Mob = 1,   // a creature the judge weighed
        };

        Side side = Side::Bot;
        std::string name;
        uint32 low = 0;             // GUID counter: the identity. Four times in one day two objects shared a name.
        uint32 entry = 0;           // creature entry (mobs); 0 for a bot
        uint32 spawnId = 0;         // creature spawn id, when the judge named the spawn rather than a live unit

        Optional<uint32> level;
        Optional<std::string> spec;      // bots: the spec the judge saw
        Optional<std::string> role;      // bots: "tank" / "healer" / "dps" in THIS five
        Optional<uint32> health;         // absolute
        Optional<uint32> healthMax;      // the denominator, always beside it
        Optional<float> healthPct;
        Optional<uint32> mana;
        Optional<uint32> manaMax;
        Optional<float> manaPct;
        Optional<float> x;
        Optional<float> y;
        Optional<float> z;
        Optional<float> tankYd;          // distance to the tank
        Optional<float> targetYd;        // distance to the decision's subject
        Optional<bool> inCombat;
        Optional<bool> wanders;          // mobs: it has a wander area
        Optional<bool> openerReady;      // bots: its opener is off cooldown - 121 refusals rode on exactly this
        Optional<std::string> doing;     // what it was doing at that instant

        // [jrn50] THE SUPPLY FACTS OF A MEMBER, and they are columns because a run was lost to them: a five failed
        // wc-n giving 58 % of the run to resting after its paladin tank ran out of water, and the only way to
        // establish that afterwards was a LIKE over the text. `drinks` / `foods` are what it CARRIES at this
        // instant (the bags, counted by the emitter in its own pass), `drinksNeed` what the five's gate priced its
        // run at ([dun194]). Both sides are needed: "0 drinks" is only a finding beside "the run needs 19".
        Optional<uint32> drinks;
        Optional<uint32> foods;
        Optional<uint32> drinksNeed;

        // The checks a mob passed in the judge's own words, kept as the text AND as fields, because the text was
        // unfilterable: "reaches spot,pull path,walk,walks into the fight".
        Optional<std::string> checks;
        Optional<bool> chkReachesSpot;
        Optional<bool> chkPullPath;
        Optional<bool> chkWalk;
        Optional<bool> chkWalksIntoFight;

        std::string note;  // why this participant is in the record at all ("pack member", "wander area", ...)
    };

    // One decision. Everything here is a column, not text to grep.
    struct Decision
    {
        std::string topic;    // what was being decided ("pull", "rest", "town", "profession_plan", "five_roster")
        std::string choice;   // the option chosen ("engage Deviate Ravager", "no pull", "Tailoring 20->580")
        std::string verdict;  // the choice's class: "taken" | "held" | "refused" | "none" - one word, queryable
        std::string why;      // the judge's own sentence
        Judge judge = Judge::Unknown;
        std::string judgeName;  // the deciding bot / leader by name ("" when the judge has no character)
        uint32 judgeLow = 0;    // its GUID counter (0: none)

        std::string subject;      // what the decision is about (a mob, an item, a member)
        uint32 subjectEntry = 0;  // its entry (creature / item), 0 when it has none
        uint32 subjectLow = 0;    // its GUID counter, 0 when it has none
        std::string subjectKind;  // "creature" | "item" | "member" | "spot" | "" ...

        double cost = 0.0;     // the price of the choice
        std::string costUnit;  // what `cost` is measured in ("tank hp", "sec", "copper", "min", ...)

        // [jrn20] PROMOTED OUT OF `fields` INTO COLUMNS, because these are what questions are asked by: "every pull
        // bigger than one", "every careful one", "what happened at this spot on the map". The rule, and it is the
        // whole reason these exist: IF A FIELD IS FILTERED ON, IT IS A COLUMN WITH AN INDEX - `fields` is for what
        // is read with the eyes, not for what is searched. Not everything moves: only what gets filtered, and the
        // rest stays in `fields` where it costs nothing to keep.
        std::string kind;            // the judge's own class for the choice ("engage", "wakes", "walk", ...)
        std::string method;          // "engage" / "body" / "shoot" / ... how the thing was to be done
        // the route target the judge was working toward, BY IDENTITY and not as the printed GUID string: "every
        // decision whose route target was this mob" is a question that gets asked, so it is two columns
        uint32 routeTargetEntry = 0;
        uint32 routeTargetLow = 0;
        Optional<uint32> packSize;   // how many mobs the choice took on (NULL: the judge did not say)
        Optional<bool> careful;      // the judge was being careful about it
        Optional<float> judgeX;      // where the judge stood when it decided - "what happened at this spot"
        Optional<float> judgeY;
        Optional<float> judgeZ;
        // [jrn30] WHERE the judge stood, as the map says it and not only as three floats. The user asked for this in
        // so many words ("log the positions too, just so we know where he actually is, where he pulled whom"), and
        // "what happens in this zone" is a question that gets asked, so the map and the zone are columns beside the
        // coordinates. 0 means the emitter did not state it - a map 0 row with no coordinates is the gap, not Azeroth.
        uint32 mapId = 0;
        uint32 zoneId = 0;

        std::string tag;        // the code tag that owns the line ("[pull81]"), so the row names its own author
        std::string src;        // file:line of the judge, so "who prints this" needs no grep
        std::string readField;  // the field the judge actually read to decide ("fightSecLeft", "rate_per_min")
        std::string readBy;     // who consumes this verdict; "" means nobody known - the point of the column

        std::string runId;   // the run a decision inside a run belongs to (guild_project_runs / harness id)
        uint32 runMs = 0;    // ms since that run started
        std::string fields;  // the judge's remaining inputs as one JSON object ("" for none)

        // [jrn20] ONE count, not two. The line the server printed about this decision is kept in the row itself, so
        // the table is not a second, independently produced tally of what the file says: there is one event, written
        // once, and the printed line carries the row's id ("[jrn20 #<id>]"). A grep of the file and a COUNT over the
        // table therefore cannot disagree - if they did, the ids would show which rows are missing, and that is a
        // defect with an address rather than an argument between two patterns. Nothing recomputes a narrow table
        // from the stream table; the narrow row is the original.
        std::string text;

        std::vector<Option> options;  // every option weighed, the taken one included
        std::vector<Actor> actors;    // the state of EVERY participant the judge saw: the five, and the mobs
    };

    // Writes one decision (and its options). World thread safe, no query runs here. Returns the row's journal id
    // (0 when the journal is off), for NoteRead() and for referring to it from another row.
    uint64 Record(Decision const& decision);

    // One loot item and the decision behind it. The run is referred to by `runId` only: `guild_project_runs` stays
    // the one table of runs, and the journal never keeps a second copy of a run (its `at` is the run's END - the
    // row is written when the finished run is inserted, and at+duration lands in the future, which settles it,
    // so a window over runs is ORDER BY id DESC LIMIT n, not a range over `at`).
    struct Loot
    {
        uint32 itemEntry = 0;
        std::string itemName;
        uint32 count = 1;
        uint32 quality = 0;
        std::string fromKind;  // "mob" | "boss" | "chest" | "quest" | "craft" | "vendor"
        std::string fromName;  // the creature / object it came from
        uint32 fromEntry = 0;
        std::string toName;  // the member it went to ("" when nobody took it)
        uint32 toLow = 0;
        std::string verdict;    // "equipped" | "bank" | "vendor" | "disenchant" | "nobody" | "held"
        std::string why;        // the leader's own sentence
        double worth = 0.0;     // copper, or the weight the leader priced it in
        std::string worthUnit;  // "copper" | "weight"
        std::string runId;
        uint64 decisionId = 0;  // the pb_journal_decision row that chose this, when there is one
    };

    void RecordLoot(Loot const& loot);

    // A reader states that it consumed decision `decisionId`. This is what makes "nobody reads this verdict" a
    // query (LEFT JOIN pb_journal_read ... WHERE reader IS NULL) instead of a reading of the code.
    void NoteRead(uint64 decisionId, std::string const& reader);

    // One raw stream line, when AiPlayerbot.Journal.Stream is on. The insurance: whatever a narrow table forgot to
    // foresee is still here, with where the bot was when it was printed.
    struct Line
    {
        std::string tag;
        std::string text;
        std::string botName;
        uint32 botLow = 0;
        uint32 mapId = 0;
        uint32 zoneId = 0;
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        // [jrn40] the run this line belongs to. EMPTY MEANS "outside a run", not "lost": a line out of the
        // appender has no run to name, and the appender is handed finished text and nothing else. Without this
        // column all 411 819 stream rows - every alert, idle report and stuck line - could not be joined to a run
        // by identity at all, only by nearness in time, which is the mistake a fixture must never bake in.
        std::string runId;
        // the wall clock in ms when the line was WRITTEN, 0 for "now". The log appender passes its own stamp rather
        // than letting the row take one later: the order of two things inside one second is the point.
        uint64 atMs = 0;
    };

    void RecordLine(Line const& line);

    // One measured value. `fromMs`/`toMs` are the window it was measured over and are never implied: numbers taken
    // over different windows were compared as the same number three times in one day. `scope` names what it is about
    // ("bot:Bierya", "five:3", "dungeon:wc-n", "server"). With the build in the row, "what changed between build N
    // and N+1" is a WHERE clause.
    struct Metric
    {
        std::string name;  // "dps_vs_ideal_pct", "levels_per_hour", "clean_run_share"
        double value = 0.0;
        std::string unit;  // "%", "per_hour", "sec", "copper"
        uint64 fromMs = 0;
        uint64 toMs = 0;
        std::string scope;

        // [jrn31] THE MEAN HIDES THE SPIKE, and that is the whole reason these are columns. A mean of 50 ms over a
        // window that contains one 900 ms tick is 50 ms, and 900 ms is what the bots stand on; a row carrying only
        // the mean draws a reassuring flat line over the defect. So a sampled metric states its own distribution:
        // the smallest sample, the largest, and the upper percentile - each its own column with an index, because
        // "every window whose worst tick went over 200 ms" is a WHERE clause and not a read of JSON.
        Optional<double> valueMin;
        Optional<double> valueMax;
        Optional<double> valueP95;  // the 95th percentile over the window's samples
        Optional<uint32> samples;   // how many samples the window holds: a max over 2 samples is not a max
        // [jrn31] WHAT MAKES TWO ROWS COMPARABLE, stated in the row instead of assumed. A tick time at 75 bots and
        // one at 300 bots are not the same number, and `build` alone does not say so; the window's own start is
        // `fromMs`, which is why it is never implied either.
        Optional<uint32> bots;
        Optional<uint32> sessions;
    };

    void RecordMetric(Metric const& metric);

    // [jrn30] THE START OF A FIGHT, as an event of its own.
    //
    // Measured on the live journal: in one 2.69 h window `playerbots_fight_history` held 9 467 finished fights and
    // the journal held NOT ONE beginning. A fight's start was visible only through a sampler, so "who pulled this,
    // where, and against how many" was a reconstruction from neighbouring lines rather than a row.
    //
    // It is written at the combat-state TRANSITION the core already announces (CombatManager::UpdateOwnerCombatState
    // -> OnPlayerEnterCombat fires only when the owner was not in combat and now is), so it needs no clock of its
    // own, cannot fire twice for one fight, and covers every bot rather than the half a sampler happened to catch.
    // The fight's END stays in `playerbots_fight_history`: one quantity, one owner.
    struct Fight
    {
        std::string event;  // "start"
        std::string botName;
        uint32 botLow = 0;
        Optional<uint32> botLevel;
        Optional<std::string> role;  // "tank" | "healer" | "dps" in THIS group
        uint32 mapId = 0;
        uint32 zoneId = 0;
        uint32 areaId = 0;
        float x = 0.f, y = 0.f, z = 0.f;
        bool inDungeon = false;

        // against whom, BY IDENTITY: four times in one day two objects shared a name, so the entry and the GUID
        // counter are the columns and the name is for the eyes
        std::string enemyName;
        uint32 enemyEntry = 0;
        uint32 enemyLow = 0;
        Optional<uint32> enemyLevel;
        Optional<uint32> mobs;  // creatures in combat with this bot at that instant (NULL: not counted)

        // WHO PULLED IT AND WITH WHAT. The open pull order of the group's tank is the authority on this, because it
        // is the thing that was actually issued; nothing is inferred from a name.
        std::string pullerName;
        uint32 pullerLow = 0;
        std::string pullKind;    // "order" (an open pull order names it) | "self" | "other" | "none"
        std::string pullMethod;  // the order's method ("" when no order names one)
        Optional<uint32> pullExpected;  // mobs the plan expected
        double pullCost = 0.0;
        uint32 pullOrderId = 0;

        std::string runId;
        uint64 decisionId = 0;
    };

    void RecordFight(Fight const& fight);

    // [jrn30] AGGRO GAINED, and the point of the row is the REASON CODE.
    //
    // The user asked for it in these words: "it caught some aggro, that too" - and then for the one case that
    // mattered: "an error of the estimate". That class was not merely unrecorded, it was INEXPRESSIBLE: nothing in
    // the journal said why a mob was on a bot, so the one question worth asking could not be asked. `reason` is
    // therefore a column with an index, never a phrase inside `why`, and the sentence in `why` is what is read with
    // the eyes afterwards.
    //
    //   "came"     nobody pulled it: the bot stood still and the mob engaged it (a wanderer, a respawn, a patrol)
    //   "path"     hooked on the way: the bot was walking and out of combat when the mob engaged it
    //   "estimate" AN ERROR OF THE ESTIMATE: a pull the leader judged was open, and the mob engaged somebody the
    //              plan did not put in front of it (not the order's tank), or a mob the plan did not count
    //   "add"      a mob joined a fight already running (the bot was in combat when this one engaged it)
    //   "off_tank" the mob was torn off the tank mid-fight. DECLARED AND NOT YET EMITTED - see the note below
    //   "unknown"  the emitter could not classify it, and says so rather than picking a neighbour
    //
    // WHY "off_tank" HAS NO EMITTER YET, stated here rather than left to be discovered: every reason above is a
    // fact at the instant a creature ENGAGES (Creature::AtEngage, announced once per creature per fight). A mid-fight
    // threat switch is not an engage - it is a change of `GetVictim()` between two ticks, so catching it needs a
    // per-tick watch over every mob of every fight, which is a cost and a placement decision in the leader's own
    // tick. The code is in the schema so that the day it gets an emitter no table changes; until then no row carries
    // it, and a query that finds none must read that as "not measured", not as "never happens".
    struct Aggro
    {
        std::string botName;
        uint32 botLow = 0;
        Optional<std::string> role;
        Optional<float> botHealthPct;
        // [jrn44] `bot_in_combat` IS THE CORE'S FLAG AND NOTHING MORE, and at this instant it CANNOT come out false:
        // this hook runs after the core has joined the bot and the mob, so it was 1 in all 4 661 live rows. It stays
        // as the raw reading of that flag; the fact the reason code is actually decided on is the next field.
        Optional<bool> botInCombat;
        // was the bot in combat with anybody OTHER than this mob - the fact "already fighting" has to mean, and the
        // repair of the reason code. NULL in every row written before [jrn44]: not measured, which is not a 0.
        Optional<bool> inCombatOther;
        Optional<bool> botMoving;    // it was walking: what separates "hooked on the way" from "it came"

        std::string mobName;
        uint32 mobEntry = 0;
        uint32 mobLow = 0;
        Optional<uint32> mobLevel;

        uint32 mapId = 0;
        uint32 zoneId = 0;
        float x = 0.f, y = 0.f, z = 0.f;

        std::string reason;  // the code above - a column, because this is what gets filtered
        std::string why;     // the same thing in words, for the eyes

        std::string tankName;  // the tank the plan meant to take it
        uint32 tankLow = 0;
        Optional<bool> expected;  // the open plan put THIS bot in front of it
        uint32 orderTargetEntry = 0;  // what the open pull order was aimed at (0: no order)
        uint32 orderTargetLow = 0;

        std::string runId;
        uint64 decisionId = 0;
    };

    void RecordAggro(Aggro const& aggro);

    // [jrn30] A DEATH, as an event that CANNOT be confused with a death being prevented.
    //
    // The defect this fixes, measured: 20 502 stream lines in one window carried a tag beginning with "[death", and
    // the real deaths in that window were 17. The 20 485 others are the death-PREVENTION domain printing under its
    // own numbered tags ("[death384] holds its hit at ...", "[deaths20] within the reach of ...: steps out to ...") -
    // they are about a death that did NOT happen. A count by tag family therefore overstated deaths by about 1 200
    // times, and `LIKE '[death%'` is exactly the shape of query that produces it.
    //
    // So a real death is a ROW IN A TABLE OF ITS OWN. Nothing about prevention is ever written here, and a count of
    // deaths is `SELECT COUNT(*) FROM pb_journal_death` over a window - no pattern, nothing to disagree about.
    //
    // [runpast88] THE DEATH'S NUMBERS, EACH ITS OWN VALUE, filled by the emitter that already holds them.
    //
    // Every one of these stood inside the death's `context` sentence as words. The user's first rule is that
    // nothing is filtered by text or by JSON: "the deaths where more mobs hit it than its route search had priced"
    // has to be a WHERE over columns, not a LIKE over a sentence (a LIKE over pb_journal_line's text was measured
    // at 384 ms across 411 819 rows and grows linearly). SoloSafety::DeathContext fills this in the SAME pass that
    // builds the sentence, under the same lock, at the same instant - so the words and the numbers can never
    // disagree, and nothing is read from the world a second time.
    //
    // AN UNSET VALUE MEANS THE EMITTER DID NOT HOLD IT (no fight of its own tracked, no chase committed, no route
    // search behind it) AND IS NOT A ZERO: `hitBy` 0 would say nobody hit it.
    struct DeathFacts
    {
        // the fight it died in, as SoloSafety measured it from its own damage events - open world and dungeon alike
        Optional<uint32> hitBy;             // DISTINCT creatures that dealt it damage this fight
        Optional<uint32> takenThisFight;    // what they dealt it, summed
        Optional<uint32> damageFromKiller;  // the killer's own share of that sum
        Optional<float> fightSec;           // its first damage event of the fight to its last

        // [runpast87] the committed run-past chase it was walking under when it died, if any
        Optional<float> runPastPriced;  // the chase's MEAN price - the number the death line prints
        // [death386] the HIGH end: the mean plus two standard deviations of the chasers' swings. THIS is the number
        // SurvivesRunPast was asked, and it was in no column and no line. b502 Lanbuzz: the sentence says 577, the
        // gate let the walk through on 677, 911 arrived. One quantity, one judge - so the judged number gets a
        // column of its own beside the printed one, and a query comparing a price with the damage uses this one.
        Optional<float> runPastPricedHigh;
        Optional<float> runPastSec;        // the seconds the chase was priced over
        Optional<float> runPastHp;         // the health (with its heal kit) it was priced against
        Optional<uint32> runPastTaken;     // what actually arrived since the chase was committed
        Optional<bool> runPastOfItsChase;  // [surv20] the killer or a hitter is one of that chase's own chasers

        // [deaths] how its last route search had seen the mobs that went on to hit it, as counts
        Optional<uint32> routeAgeMs;  // the search's age at the death
        Optional<bool> routeComplete;
        Optional<bool> routeThroughRefused;
        Optional<uint32> routePriced;      // seen and priced
        Optional<uint32> routeRefused;     // seen and walked round
        Optional<uint32> routeUnseen;      // out of the search's range altogether
        Optional<uint32> routeOtherFight;  // [surv21] in range, left out because its fight was not the bot's
    };

    // A DEATH IS A CALCULATION BUG in this project and not bad luck, so the row carries the INPUTS and not only the
    // outcome: who, where, by whom, at what damage, and what the judge expected when it let the fight happen.
    struct Death
    {
        std::string botName;
        uint32 botLow = 0;
        Optional<uint32> botLevel;
        Optional<uint32> botClass;
        Optional<std::string> role;
        Optional<std::string> spec;

        uint32 mapId = 0;
        uint32 zoneId = 0;
        uint32 areaId = 0;
        float x = 0.f, y = 0.f, z = 0.f;
        bool inDungeon = false;

        std::string killerName;
        uint32 killerEntry = 0;
        uint32 killerLow = 0;
        Optional<uint32> killerLevel;
        Optional<float> killerHealthPct;

        Optional<uint32> healthMax;    // the denominator: the health at 0 % is only readable against the pool
        // what the KILLER dealt to this bot inside this instance (DamageTaken::From).
        // [runpast88] UNSET OUTSIDE A DUNGEON, and that is the honest value: DeathRecorder::IsTracked only
        // accumulates for a player whose map is a dungeon, so in the open world there is nothing to read. All eight
        // b502 deaths were out in the world and every one of them carried a 0 here, which read as "the killer dealt
        // it nothing". What the killer dealt it out there is `facts.damageFromKiller`, measured by the other
        // instrument - SoloSafety's own damage events - and the two are kept apart on purpose.
        Optional<uint32> damageFromKiller;
        // how many were on it at the moment it died.
        // [runpast88] UNSET WHEN THE CORE'S LIST IS EMPTY AT THE HOOK, which is always: the kill clears the
        // attacker list before OnPlayerKilledByCreature runs, so a 0 here was "cannot be measured at this instant"
        // dressed as "nobody". The count that answers the question is `facts.hitBy`.
        Optional<uint32> attackers;

        // [runpast88] the fight, the chase and the route search behind this death, as values - see DeathFacts
        DeathFacts facts;

        // WHAT THE JUDGE EXPECTED. A death is the estimate being wrong, so the estimate goes in the row.
        Optional<bool> judgedPull;      // a pull the leader judged was open when it died
        std::string expected;           // the order's own reason, in the leader's words ("" when no order)
        Optional<uint32> pullExpected;  // mobs that plan counted on
        std::string pullMethod;

        std::string context;  // the existing death context sentence (its fight, the chase it belonged to)
        std::string threat;   // the killer's threat list at the kill: whom else it was fighting

        std::string runId;
        uint64 decisionId = 0;
    };

    // Returns the row's id so an issue row can point at it (0 when the journal is off).
    uint64 RecordDeath(Death const& death);

    // [jrn40] THE LIST OF PROBLEMS: one row per PROBLEM, driven to zero rows.
    //
    // The user's own words: "if it stands in one place for more than a minute, standing exactly, then for what
    // reason... so that we have ready-made tables of problems. Say, an issues table, and we will analyse every
    // problem there until there are 0 rows."
    //
    // WHAT THIS TABLE IS AND IS NOT. It is a THIN, UNIFORM POINTER, the same shape for every kind of problem, and
    // it is not a copy of anybody's inputs. A death already has pb_journal_death with its killer, its health and
    // the pool behind it; copying those here would put one quantity in two places, against "do not duplicate
    // another owner's table, link to it" and against "one quantity, one owner" - and the columns would become the
    // union of every kind's fields, NULL for all the others, which is the tagged union this project already
    // rejected once. So the inputs stay with their domain (pb_journal_death, pb_journal_idle, pb_journal_stuck,
    // pb_journal_alert, guild_project_runs) and this row carries `source_table` + `source_id` to reach them.
    //
    // THE FOUR THINGS THAT KEEP IT HONEST, each of which it would otherwise lie about:
    //
    //  1. IT IS NOT UNDER RETENTION. Every other journal table lives 24-48 h and is cleaned by dropping hour
    //     partitions. A list of problems that evaporates every two days is not a list of problems. This table is
    //     not partitioned and the sweep does not name it.
    //
    //  2. A ROW IS A PROBLEM, NOT AN OCCURRENCE. This is trap #1 of this project: in the log one cause printed 3-4
    //     times and a run's report line printed in five places, inflating counts by up to 7.3x. So the row carries
    //     `firstSeenMs`, `lastSeenMs`, `occurrences`, and the deduplication key IS a column with a UNIQUE index on
    //     its hash - a second occurrence updates the row instead of adding one. Without that the table could never
    //     reach zero and its count would mean nothing.
    //
    //  3. NUMBERS COME OUT OF THE REASON, or one cause fans out into a staircase. Trap #2: "177 at 15 %, 158 at
    //     14 %..." looked like fifteen causes and was ONE cause behind 3 216 holds. So `dedupKey` is the reason
    //     with its numbers replaced, and the range they spanned is kept beside it in `valueMin` / `valueMax`.
    //
    //  4. A ROW CLOSES CHECKABLY AND REOPENS BY ITSELF. `status` and `fixedInBuild`, and the rule is in the SQL:
    //     an occurrence arriving after the row was marked fixed, on a build not older than the one it was fixed in,
    //     puts the row back to "returned". Nothing has to be believed.
    //
    //     AND THE REAL DANGER OF A TABLE DRIVEN TO ZERO: it rewards closing a row by RECLASSIFYING it instead of
    //     fixing it. This project already has the example - 20 502 lines tagged as deaths turned out to be deaths
    //     PREVENTED, and the real ones were 17. Had those become issues, twenty thousand problems would have been
    //     "solved" by renaming. So every row carries `realWhen`: the emitter's own criterion for this being a real
    //     problem, written when the row is created and never rewritten by whoever closes it. The audit is a query:
    //     a row with status other than "open" and an empty `fixedInBuild` was closed by hand with no fix shipped.
    struct Issue
    {
        // [jrn40] HOW REPRODUCIBLE IT IS, as a code in its own indexed column, because it decides what can be
        // written as a test at all:
        //   "decision" - the inputs exist and the judge is deterministic: a death, a pull taken with a negative
        //                margin, an underpriced pack, a healer further off than the judge allowed. A test can put
        //                the same conditions back and watch the verdict.
        //   "instrument" - we did not notice: the idle diagnostics that skipped tanks and healers. The world was
        //                fine; the measurement was not.
        //   "not_offline" - a property of the world over time: standing for a minute, an order or a walk blocked,
        //                an activity looping. There is nothing to replay offline, so a test cannot be the proof.
        std::string kind;        // the problem's kind, taken from the live tags and never invented
        std::string severity;    // "low" | "mid" | "high" - the emitter's own call
        std::string reproClass;  // one of the three above
        std::string status;      // "open" | "fixed" | "returned" | "wontfix"

        // [jrn40] THE USER'S OWN COLUMNS: "say there is a `checked` column, you set checked true, meaning the
        // problem is checked, solved; a unit test is written there". They are in the schema and so are the three
        // things that keep the tick from becoming perpetual optimism - these are SCHEMA properties, not discipline:
        //
        //  1. THE TICK IS NOT TAKEN ON TRUST. `checked_state` is a GENERATED column, computed by the database from
        //     what is actually there, so it cannot be set by hand and cannot disagree with the evidence:
        //       "unchecked"   - checked is 0
        //       "by_test"     - a repro class of "decision" with `test_name` filled: a test holds it
        //       "by_run"      - a repro class of "not_offline" with `cleared_in_run` and `fixed_in_build` filled:
        //                       a run where the sign is gone holds it
        //       "unconfirmed" - checked is 1 and NEITHER of the above. Visible as a value, not as a comment.
        //     So "how many problems are really closed" and "how many are closed on someone's word" are two
        //     different queries over the same column, and neither needs anybody to be believed.
        //
        //  2. THE TICK CLEARS ITSELF. An occurrence arriving after the row was marked fixed, on a build not older
        //     than `fixed_in_build`, sets the row back to "returned" AND zeroes `checked` in the same statement.
        //
        //  3. MERGING DOES NOT LOSE THE COUNT. `merged_into` names the row this one was folded into when the cause
        //     turned out to be the same, and the merged row's `occurrences` must be added to the target's -
        //     otherwise merging becomes a way to lower the number of problems without lowering the problems.
        //     The audit for that is a query: SUM(occurrences) over the merged rows against the target's.
        bool checked = false;
        std::string checkedBy;      // who or what set it ("" while unchecked)
        std::string testName;       // the unit test that holds it, when one is written
        std::string clearedInRun;   // the run_id of a run in which the sign was gone
        std::string fixedInBuild;   // the build the fix shipped in
        uint64 mergedInto = 0;      // the issue row this one was folded into (0: none)

        // the key that makes the row a problem instead of a case: the reason with its numbers taken out. The UNIQUE
        // index is on its hash (computed in SQL from this very string), so the full key stays readable while two
        // different keys can never be merged by a truncated index.
        std::string dedupKey;

        std::string why;       // the LAST occurrence's own sentence, numbers and all: for the eyes
        std::string realWhen;  // the criterion the EMITTER named for this being real, in its own words

        // [jrn40] IS IT A REAL PROBLEM - A COLUMN, NOT A GATE. The user: "better to over-worry than under-worry.
        // If we add rubbish, we will decide it is rubbish. But decide it gradually, not by filtering first and then
        // finding we have no data and cannot tell what happened. Better to start wide and narrow later."
        //
        // The asymmetry is real and this project has lived it: positions were not written at all, so no question
        // about where a bot stood had an answer. A surplus row costs one query; a missing one costs the whole
        // investigation. So every detector writes, and whether it is real is recorded rather than decided at the
        // door.
        //
        // THREE VALUES, AND THE THIRD IS NOT A GAP BY NEGLIGENCE. This is the project's own '' against '?'
        // distinction - "checked, nobody reads it" against "not established yet" - which has paid for itself more
        // than once: a query that merges them answers anyway and lies.
        //   "yes"     - the emitter knows at runtime that this is real, and `realWhen` says by what criterion
        //   "no"      - judged rubbish, with `realWhy` saying why IN WORDS. The row STAYS: marking it is a case
        //               analysed, not a case that vanished
        //   "unknown" - the cause is not known at runtime. An honest value, and the one post-analysis fills in
        // The count driven to zero is `real IN ('yes','unknown')`.
        std::string real;        // "yes" | "no" | "unknown"
        std::string realWhy;     // why it is or is not real, in words
        std::string realSource;  // "emitter" (measured at runtime) | "post" (post-analysis) - without this, a
                                 // month later there is no telling the measured from the inferred

        // the number the reason carried, if it had one: its range over every occurrence
        Optional<double> value;
        std::string valueUnit;

        std::string botName;
        uint32 botLow = 0;
        uint32 mapId = 0;
        uint32 zoneId = 0;
        float x = 0.f, y = 0.f, z = 0.f;

        std::string runId;
        // WHERE THE INPUTS ARE. The row points at the domain's own record and keeps no copy of it.
        std::string sourceTable;  // "pb_journal_death" | "pb_journal_idle" | "pb_journal_stuck" | ...
        uint64 sourceId = 0;
        uint64 decisionId = 0;  // when the problem hangs off a decision

        // [jrn40] THE WINDOW OF THE SLICE, TAKEN FROM THE DATA AND NOT CHOSEN BY HAND. A fixture built for a test
        // has to cover some stretch of time, and deciding by eye how many seconds back to look would bake OUR
        // GUESS into the test. So the window is given by an event: from the last real PROGRESS to the moment of the
        // problem. `lastProgressMs` is that moment; with `run_id` and the problem's own time the slice is a query.
        // 0 means the emitter could not name a last progress, which is a gap and not "the run's beginning".
        uint64 lastProgressMs = 0;

        // [jrn40] THE FLAT JSON OF THE INPUTS, here for one reason only: the DOMAIN row this issue points at is
        // swept after 24-48 h while this row lives for ever, so without a copy taken when the problem was opened,
        // `source_id` reaches nothing a week later and the fixture cannot be rebuilt. It is NOT a filterable field -
        // it is read with the eyes and by the fixture exporter - and it is not a second home for anything that has
        // a column of its own. Empty means the inputs live only in the domain row and will go with it, which is a
        // gap a query can see rather than a silent loss.
        std::string fixture;
    };

    // Writes (or updates) one problem. Returns nothing: the row's identity is its dedup key, not an id the caller
    // could hold - a caller that kept an id would be tempted to count occurrences itself, and the count lives in
    // the row.
    void RecordIssue(Issue const& issue);

    // [jrn40] THE CONSEQUENCE THAT MUST NOT BE LOST, and how it is actually carried. The issue row lives for ever
    // while the domain record it points at is swept after 24-48 h, so a week later `source_id` reaches nothing and
    // the inputs are gone. The copy that survives is `Issue::fixture` - the flat JSON the EMITTER puts there,
    // because the emitter is holding those values at that instant anyway. The offline exporter turns rows into
    // files afterwards.
    //
    // THERE WAS A FUNCTION-POINTER SEAM HERE AND IT IS DELETED ON PURPOSE. It let an owner install a writer that
    // RecordIssue would call, and nothing could ever install one: writing a file from a map thread is not allowed,
    // and the exporter is offline by design. A seam nobody can fill looks like working machinery to the next
    // reader, which is worse than no seam at all - the same reason a comment about what the code no longer does
    // costs more than a wrong number. `fixture` empty means the emitter had no inputs to hand over, which is a gap
    // a query can see.

    // [jrn40] One idle-diagnostics record: the 15 s window of why a bot in a fight cast nothing, and the standing
    // watch. Both belong to PlayerbotAI's own tick, which is the only moment those numbers exist.
    //
    // THE EXCLUSION THAT WAS LIFTED, and why it mattered: the window used to return at once for tanks and healers
    // (`IsTank(bot) || IsHeal(bot)`), so two roles out of three were NEVER measured - and the healer is exactly
    // where a chain of deaths came from. A quantity that cannot be measured for two thirds of its subjects is not
    // objective, it is objective-looking. `role` is a column so the roles are read apart rather than mixed.
    //
    // `event` says which record this is, because they are two different quantities and nothing may average them:
    //   "window"   - one IDLE_DIAG window: the tick counts and the gap counts below
    //   "standing" - the bot has not left a small radius for longer than a minute (the user asked for this one by
    //                name, "standing exactly"): `stoodSec` and `stoodYd`, and the tick counts are NULL
    struct Idle
    {
        std::string event;  // "window" | "standing"
        std::string botName;
        uint32 botLow = 0;
        Optional<uint32> botLevel;
        Optional<std::string> role;  // tank / healer / dps - the whole point of lifting the exclusion
        uint32 mapId = 0;
        uint32 zoneId = 0;
        float x = 0.f, y = 0.f, z = 0.f;
        Optional<bool> inDungeon;
        Optional<bool> inCombat;
        // [jrn45] the two facts the diagnostics window used to be GATED on instead of carrying. A gate decides and
        // says nothing; a column lets the slice be a WHERE. `engineState` is BotState (0 = BOT_STATE_COMBAT), so
        // "the engine ran the non-combat set while the core held the bot in combat" - the shape Thelengyl died in -
        // is `engine_state = 1 AND in_combat = 1`, with no LIKE over `why`.
        Optional<bool> inGroup;
        Optional<uint32> engineState;

        Optional<float> windowSec;  // the window the counts below cover: a count without its window is not a count
        Optional<uint32> ticks;
        Optional<uint32> casts;
        Optional<uint32> castWait;
        Optional<uint32> nonCast;
        // why a tick produced nothing - each reason its own column, because each is filtered on
        Optional<uint32> noTarget;
        Optional<uint32> rangeLos;
        Optional<uint32> moving;
        Optional<uint32> gcd;
        Optional<uint32> threat;     // the bot's OWN threat on the target
        Optional<uint32> leaderHold;  // the leader's command zeroed its actions
        Optional<uint32> other;
        // and `other` split by the engine's own refusal, which is what made 37 % of idle ticks answerable
        Optional<uint32> refUnsaid;
        Optional<uint32> refUseless;
        Optional<uint32> refImpossible;
        Optional<uint32> refPrereq;
        Optional<uint32> refFailed;
        Optional<uint32> refBlocked;
        Optional<uint32> refNoCandidate;
        // the gaps between ticks. The two boundaries are the ones the domain already counts; nothing new.
        Optional<uint32> gapN;
        Optional<uint32> gapAvgMs;
        Optional<uint32> gapMaxMs;
        Optional<uint32> gapOver150;
        Optional<uint32> gapOver400;
        std::string topActions;  // the window's busiest actions, printed: read with the eyes
        // [jrn51] AND THE REFUSED ACTION THE WINDOW SPENT MOST OF ITS IDLE TICKS ON. `other` says a tick produced
        // nothing and `refUseless` says the engine's reason was "not useful"; WHAT was not useful had no home,
        // and that is the shape of b503's largest unexplained key (307 cases). Columns and not words inside `why`,
        // because the first question asked of them is "which action, how often, on which builds". Absent (NULL)
        // when no tick of the window fell into `other` at all - which is not a refusal of "none".
        Optional<std::string> refTopAction;
        Optional<std::string> refTopCause;  // the IDLE_REFUSAL_* name: useless / impossible / failed / blocked ...
        Optional<uint32> refTopTicks;

        // the standing watch
        Optional<float> stoodSec;  // how long it has not left the radius
        Optional<float> stoodYd;   // the radius it stayed inside - the pair is the fact, neither number alone is
        std::string doing;         // what the engine's last action was, as the engine already knows it
        // [jrn42] the legitimate stillness this episode stands under, by name (Journal::Standing::HoldName):
        // "its leader ordered it to stand", "waits for a transport to dock", "waits for its group", "sitting"...
        // EMPTY MEANS "checked at this instant, nothing held it still" - a finding, as it is in `read_by`, and
        // not a blank. It is a column and not a word inside `why` because every question worth asking of this
        // table filters on it: how much of the standing is the leader's own hold, and how much is nobody's.
        std::string heldBy;
        std::string why;           // the reason in words, from what the domain knows at this instant

        std::string runId;
    };

    // Returns the row's id so the issue row can point at it.
    uint64 RecordIdle(Idle const& idle);

    // [jrn40] Something is held or awaited and nothing progresses. Four emitters, one shape; each writes its own
    // row, and `kind` says which of them did:
    //   "stuck_target" - the combat engine holds an idle mob as its target and no action of it runs ([stuck258])
    //   "stale_victim" - the leader held a target outside a fight whose victim is gone ([queue-stale-idle-victim])
    //   "pet_reach"    - a pet has no spot out of an idle mob's reach, or trails its owner ([pet idle reach])
    //   "wait_behind"  - a member is beyond the group reward distance and is walked to ([wait14])
    struct Stuck
    {
        std::string kind;
        std::string botName;
        uint32 botLow = 0;
        Optional<uint32> botLevel;
        Optional<std::string> role;
        uint32 mapId = 0;
        uint32 zoneId = 0;
        float x = 0.f, y = 0.f, z = 0.f;

        std::string targetName;
        uint32 targetEntry = 0;
        uint32 targetLow = 0;
        Optional<float> targetYd;
        Optional<uint32> targetLevel;
        // [jrn43] the TARGET's zone, which for `wait_behind` is the member walked to. It is here because a cause
        // field nobody can check by a query is a comment: the key of `wait_behind` splits on whether the member is
        // in the bot's own zone, and until this column existed the row carried only the bot's zone, so the split
        // could not be verified or faulted. NULL = the emitter did not pass it, which is not zone 0.
        Optional<uint32> targetZoneId;
        Optional<bool> inCombat;
        std::string actionRunning;  // the engine's last action, or "" when it had none to run - which is the point
        std::string why;
        std::string runId;
    };

    uint64 RecordStuck(Stuck const& stuck);

    // [jrn47] ONE SEARCH FOR A COMBAT PLACE THAT FOUND NONE (`combat_spot_none`), with the search's own refusals
    // as rows and not as a printed line.
    //
    // By the project's rule a place for a ranged member ALWAYS exists, and falling back to the tank's place is
    // forbidden (its cleave), so this event is a bug of the search and never a normal state: the issue row it
    // writes is `is_real = 'yes'` without a judgement. It used to be a LOG_ERROR and nothing else - 387 lines in
    // one window, 52 KB of a file configured to hold Fatal and Error only, and the one thing a reader needs,
    // WHICH CHECK TURNED THE PLACES DOWN, lived inside that line's text as "near: heal reach x43; ring: out of
    // the healer's band x33; ...". Counting it meant a LIKE over the stream, which rule 1 forbids.
    //
    // THE REFUSALS ARE A CHILD TABLE, a row per (stage, reason), because they are homogeneous participants of one
    // event: the stage is where the candidates came from (`near` / `ring` / `walk` / `at target` / `front` /
    // `tank's spot`) and the reason is the check that turned them down. `candidates` is how many that pair turned
    // down - the number, never part of the key: measured over the 387 live events, keying on the counts gave 154
    // keys for 52 causes.
    struct SpotReject
    {
        std::string stage;
        std::string reason;
        uint32 candidates = 0;
    };

    struct Spot
    {
        std::string botName;
        uint32 botLow = 0;
        Optional<uint32> botLevel;
        Optional<std::string> role;  // tank / healer / dps IN THIS FIVE, NULL when the emitter knows none
        // the kind of place that was looked for, which decides which checks could speak at all
        Optional<bool> melee;
        Optional<bool> healer;
        uint32 mapId = 0;
        uint32 zoneId = 0;
        float x = 0.f, y = 0.f, z = 0.f;

        // the mob the place was for - BY ENTRY AND GUID, not by name (a name is not an identity: over the 387
        // live events 16 names carried one cause, and keying on them split it into 91)
        std::string targetName;
        uint32 targetEntry = 0;
        uint32 targetLow = 0;
        Optional<float> targetYd;

        // what the member is short of where it stands now ("first spot" when it has none at all), and whether it
        // reaches its target from there
        std::string breaks;
        Optional<bool> reaches;
        uint32 candidates = 0;  // candidates turned down in all, the sum of the children's

        // the idle mob whose aggro reach covered the places, when one did: the commonest single refusal
        std::string idleName;
        uint32 idleEntry = 0;
        uint32 idleLow = 0;
        // [spot90] THE TWO FACTS THE RULE TURNS ON (IdlePlace.h), because the question "why is there no place"
        // cannot be settled without them: whether the LEADER had taken that very mob into this fight
        // (joinedPack), and whether the TANK had still to open on it ([lead1140] joinedHeld, which keeps the
        // other members out of its reach until the tank has it). NULL = the emitter did not say, which is not a
        // 0: a row with no idle blocker at all has nothing to say about either.
        Optional<bool> idleTaken;
        Optional<bool> idleTankOpens;

        std::string runId;
        std::vector<SpotReject> rejects;
    };

    uint64 RecordSpot(Spot const& spot);

    // [jrn40] One alert. GuildCoordinator::Alert is the single funnel every alert already passes through, so it is
    // the one place this is written from: an alert recorded both there and at the site that raised it would be the
    // double count this journal exists to prevent.
    struct Alert
    {
        std::string kind;  // the alert's first word: LOOP | DEATH | MISSED_DUNGEON | DOUBLE_DECISION | HANDOUT | ...
        Optional<uint32> num;      // the "#N" the alert prints: the member's number in the plan
        std::string botName;
        uint32 botLow = 0;
        Optional<uint32> botLevel;
        uint32 mapId = 0;
        uint32 zoneId = 0;
        float x = 0.f, y = 0.f, z = 0.f;

        // the LOOP alert's own facts, filled by the site that has them
        Optional<uint32> loopN;
        std::string activity;  // the RPG status that re-entered without progress
        Optional<bool> noProgressXp;
        Optional<bool> noProgressQuest;
        Optional<bool> samePlace;
        std::string lastAction;

        std::string reason;  // the cause in words, as the alert prints it
        std::string text;    // the whole printed line: one event, written once
        std::string runId;
    };

    uint64 RecordAlert(Alert const& alert);

    // [jrn40] THE DETECTOR THE USER ASKED FOR BY NAME: "if it stands in one place for more than a minute, STANDING
    // exactly, then for what reason". Standing is judged BY THE FACT OF POSITION and never by a declared status -
    // the project's rule is that idleness is counted by fact, because TRAVEL and DUNGEON hide a hang.
    //
    // IT IS AN INSTRUMENT AND NOTHING ELSE. The minute is the DETECTOR's threshold; it may not reach a single bot
    // decision, and the user's rule that no elapsed-time rule changes a decision stands untouched. Nothing here
    // writes to the bot, the engine or any order.
    //
    // THE RADIUS IS NOT A NEW NUMBER. It is max(GetCombatReach(), 1.5f) - exactly the expression the loop detector
    // in GuildCoordinator already uses for "same place". Inventing a second radius for the same idea would be two
    // judges of one quantity.
    //
    // THE REASON COMES FROM WHAT IS ALREADY KNOWN AT THIS INSTANT - the engine's own last action and state, read
    // through public accessors. The world is NOT asked a second time: a second reading at another moment would be
    // a second judge, and the reason would then belong to a different instant than the standing it explains.
    //
    // Called from the unit-update hook, which already dispatches to this module for every unit, so the cost added
    // is one branch for a non-player plus, for a bot, two float compares and a stamp under a sharded lock.
    void NoteStanding(Player* bot);

    // Drops a bot's standing episode (its logout or removal): the state is per bot and must not outlive it.
    void ForgetStanding(uint32 botLow);

    // [jrn45] HOW LONG THIS BOT HAS NOT LEFT ITS OWN RADIUS - max(combat reach, 1.5 yd) - as the ONE OWNER of that
    // quantity. GuildCoordinator's STUCK watch kept a second anchor with the same radius, its own reset list and its
    // own 60 s count: on b502, 71 of the 106 `LOOP` alerts have a `standing` row of the same bot_low within +-60 s,
    // and rule 5 makes a second place recomputing one number a defect before it bites. The watch tracks the episode
    // whether or not the journal writes rows, so a reader always has an answer.
    //
    // 0 IS "NO OPEN EPISODE" - moving, or not standing at all (dead, sitting, casting, in flight, teleporting) - and
    // not "it has stood for zero seconds". A reader that confuses the two will answer and be wrong.
    float StoodSecOf(ObjectGuid const& bot);

    // [jrn30] The three callers that turn a core hook into one of the rows above (JournalFight.cpp). They are
    // separate from Record*() so that the script layer stays a one-line call and the facts of each event are
    // gathered in one place that can be read whole.
    //
    // EVERY VALUE IS READ HERE, SYNCHRONOUSLY, ON THE THREAD THE EVENT HAPPENED ON. Health, position and "who is in
    // combat" exist only on that thread and only at that instant; storing the GUID and reading them later would be
    // a reading of the world AT A DIFFERENT MOMENT - a second judge of one quantity, which is the defect the whole
    // journal exists to prevent. What is read is only ever put into the struct; nothing here decides anything.
    //
    // Declared in Journal.h and defined in JournalFight.cpp on purpose: the module is a STATIC library, and a
    // linker takes an object file out of one only when a symbol in it is asked for. These are called from
    // src/Script, which is certainly linked, so the object file cannot be discarded the way JournalPull.obj and
    // JournalStream.obj silently were in b495.
    void NoteFightStart(Player* bot, Unit* enemy);
    void NoteAggro(Creature* mob, Unit* victim);
    // [runpast88] `facts`: the death's numbers as values, filled by the same DeathContext pass that built `context`
    void NoteDeath(Player* killed, Creature* killer, std::string const& context, std::string const& threat,
                   DeathFacts const& facts);

    // [jrn31] The writer's own two numbers, exposed so the STATS sampler can put them in a metric row instead of a
    // log line. They are the honest sign of the one contention the journal could have: a single mutex guards the
    // push queue for every map thread at once. While `DroppedRows()` stays 0 and `QueuedRows()` stays small there is
    // nothing being fought over and nothing to change - the queue is the measurement, not the guess.
    //
    // ONE OWNER PER QUANTITY: `DroppedRows()` is monotonic since the process started, and the writer's own error
    // line reports the DELTA it has not yet reported. Two readers of one counter, each resetting it, is the defect
    // this journal exists to prevent.
    // [jrn40] the journal's ONE wall clock, exposed so an emitter outside this file stamps its rows from the
    // same source. Two clocks for one quantity is the defect this journal exists to prevent.
    // [jrn40] A REASON WITH ITS NUMBERS TAKEN OUT, which is what a deduplication key is made of. Trap #2 of this
    // project in one line: "177 at 15 %, 158 at 14 %..." looked like fifteen causes and was ONE cause behind 3 216
    // holds. Every run of digits becomes "N", so the staircase collapses and the range it spanned is kept beside
    // it in the row (value_min / value_max) instead of being spread across fifteen rows.
    std::string WithoutNumbers(std::string const& text);

    // [jrn40] A REASON WITH ITS NUMBERS TAKEN OUT, which is what a deduplication key is made of. Trap #2 of this
    // project in one line: "177 at 15 %, 158 at 14 %..." looked like fifteen causes and was ONE cause behind 3 216
    // holds. Every run of digits becomes "N", so the staircase collapses into one row and the range it spanned is
    // kept beside it (value_min / value_max) instead of being spread over fifteen rows that each mean nothing.
    std::string WithoutNumbers(std::string const& text);

    uint64 ClockMs();

    uint64 QueuedRows();
    uint64 DroppedRows();

    // [jrn20] A ROUTE, recorded as EVENTS and not as sampled positions. Writing a coordinate every tick is the same
    // stream over again, only dearer: the raw stream already costs about 1.5 GB a day at 75 bots (measured), and a
    // position taken on a timer still cannot say WHY the bot went there. Four events reproduce a route completely:
    // the plan being built, the plan changing (and what changed), a leg starting, and a leg ending. The position is
    // written at those moments, so the route replays and the volume stays event-shaped.
    //
    // `event` is which of the four this row is: "planned" | "changed" | "leg_start" | "leg_end".
    // On "changed", `changedStop` and `changedWhy` say WHICH stop appeared or went and why - one stop once tripled a
    // tour's length and the reason was buried in the text.
    // `verdict` on a leg end: "arrived" | "refused" | "abandoned", with `why` carrying the reason.
    // Lengths are kept BOTH along the path and straight-line, because a detour is only visible as the ratio of the
    // two, and a single number cannot show it.
    struct RouteStop
    {
        uint32 seq = 0;              // order along the route
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        std::string kind;            // "pack" | "boss" | "vendor" | "node" | "quest" | "mailbox" | "entrance" ...
        std::string what;            // the name of the thing at that stop
        uint32 entry = 0;
        uint32 low = 0;
        std::string verdict;         // this stop's own outcome, when it has one
        std::string why;             // and its reason
    };

    struct Route
    {
        std::string event;           // planned | changed | leg_start | leg_end
        std::string purpose;         // what the route is for ("dungeon entrance", "drink run", "gather", "quest")
        std::string target;          // where it ends up
        uint32 targetEntry = 0;
        uint32 targetLow = 0;
        std::string why;             // why THIS target, in the chooser's words
        std::string verdict;         // leg_end: arrived | refused | abandoned

        std::string botName;
        uint32 botLow = 0;
        uint32 mapId = 0;
        uint32 zoneId = 0;
        float fromX = 0.f, fromY = 0.f, fromZ = 0.f;
        // [jrn50] WHERE THE ROUTE WAS HEADED, and it is NULLABLE because a leg can end with the target gone from
        // the world (killed, despawned): there is then no place left to write, and 0 would not say that - it would
        // say "the point (0, 0, 0)". This is the journal's own trap (absence is not zero) in a new column, and
        // telling it apart by a negative straight_yd is indirect: that works only until somebody tidies that up
        // too. An empty one writes NULL.
        Optional<float> toX, toY, toZ;
        float pathYd = 0.f;          // the length along the path
        float straightYd = 0.f;      // and in a straight line: the detour is the ratio, not either number
        uint32 stopCount = 0;

        Optional<uint32> changedStop;  // "changed": which stop appeared or went
        std::string changedWhy;        // and why

        std::string runId;             // the run it belongs to; guild_project_runs stays the one table of runs
        uint64 decisionId = 0;         // the pb_journal_decision row that chose this target, when there is one

        std::vector<RouteStop> stops;  // written with "planned" and "changed"; a leg event needs none
    };

    // Returns the route row's id (0 when the journal is off), so a later leg can point at its plan.
    uint64 RecordRoute(Route const& route);

    // The server start this process is. Called once lazily; the boot row is what makes windows across restarts
    // comparable at all.
    uint32 BootId();

    // [jrn20] Attaches the journal's readers (the leader's decision lines, and the log stream when it is on). Called
    // once, from Leader::Decisions().
    //
    // WHY THIS FUNCTION EXISTS AT ALL, because it looks like something a constructor should do and it must not be.
    // b495 wrote nothing - not one row, not even the boot row that is due on every single start. The cause was not
    // the config and not the appender: the module is a STATIC library, and a linker pulls an object file out of one
    // only when some symbol in it is asked for. Nothing outside src/Ai/Journal referred to the journal
    // (`grep -rn "Journal::" src | grep -v "^src/Ai/Journal/"` = 0), so Journal.obj, JournalPull.obj and
    // JournalStream.obj were compiled, archived, and then left behind. Searching the binary showed it plainly:
    // `AppenderJournal` and an unrelated control string were in worldserver.exe, `pb_journal_boot`,
    // `pb_journal_line` and `AiPlayerbot.Journal.Stream` were not there at all.
    //
    // The readers used to attach themselves from file-scope objects, and the constructor of a discarded object file
    // never runs - so the registration could not fix the discarding that removed it. An explicit call from a
    // translation unit that is certainly linked is what makes the journal real: it writes because something asks it
    // to, not because the linker was told to keep it. Attaching from here, rather than from a startup hook, is also
    // what LeaderLog is for - it owns where the leader's decision lines go.
    //
    // Does nothing when AiPlayerbot.Journal.Enable is off, and is safe to call more than once.
    void Attach();

    // Defined in JournalPull.cpp and JournalStream.cpp; called only by Attach(), which is also what drags those two
    // object files into the link.
    void AttachPullObserver();
    void AttachLogStream();

    // [jrn31] SERVER STATISTICS, collected continuously instead of snapshotted on command.
    //
    // Why this exists next to PerfMonitor rather than instead of it. PerfMonitor measures regions of code and
    // aggregates FROM THE MOMENT IT WAS RESET: it is a reset -> run -> print cycle, written to a file, so two builds
    // are only comparable when their reset points were the same. That is the project's own trap - if the instrument
    // changed between two measurements, they cannot be compared - and it is why these rows state what makes them
    // comparable at all: the build, the number of bots, and the window's own start.
    //
    // What is sampled, and why each one: the world tick's length and the diff it was handed; the bots and the
    // sessions (the denominator of every other number here); the update time OF EACH MAP THREAD SEPARATELY - never
    // their sum, because a sum hides the one thread that stalled and "the sum looked fine" is its own class of
    // defect here; the journal's queue depth and its dropped-row counter; and the process's resident memory.
    //
    // Every sampled row carries the window's max and upper percentile beside the mean (see Metric): the mean is what
    // draws a flat line over a 900 ms tick.
    //
    // NoteMapTick is called from map threads and NoteWorldTick from the world thread; both only add a number to a
    // bucket under a short lock, and the window is written out by NoteWorldTick when it elapses.
    void NoteWorldTick(uint32 diff, uint32 bots, uint32 sessions);
    void NoteMapTick(uint32 mapId, uint32 instanceId, uint32 diff);

    // [jrn20] The counter a window line prints, taken from the SAME call that wrote the row - never by reading the
    // journal back. The journal is the one store of cases; a count made by a second pass over those cases would be
    // a second judge of one quantity, which is the defect this whole table exists to prevent. So: one Record() call
    // raises one counter and writes one row, and a window line and a query over the window can only agree.
    // `topic` is the decision's topic; the empty topic is the total over all of them.
    struct Count
    {
        uint64 decisions = 0;
        uint64 refused = 0;  // verdict other than "taken"
        uint64 optionsRejected = 0;
        uint64 sinceMs = 0;  // when this counter last started: a count NEVER carries an implied window
    };

    Count Counted(std::string const& topic);

    // Starts a fresh window for the counters (the printer of a window line calls this when it has printed).
    void ResetCounts();

    // Flushes what is queued (shutdown, and the `.journal flush` path). Blocks until the batch is handed to the pool.
    void Flush();
}  // namespace Journal

#endif
