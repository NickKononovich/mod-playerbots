#!/usr/bin/env python3
"""Export a recorded situation from the decision journal into a fixture FILE next to the tests.

[test1] Why a file and not a query from the test: the journal's retention is 24 h on the stream and 48 h on the
narrow tables, and the sweep drops whole partitions. A test that asked the live journal would stop finding its rows
inside two days, and until then it would quietly change meaning as rows aged out. So the fixture is taken once,
written to tests/fixtures/, and committed. The test reads files only.

The same reason makes the ISSUE path urgent: an issue row is meant to outlive the journal (it lives until it is
fixed), so its inputs must be snapshotted AT THE MOMENT THE ISSUE IS RAISED - by then-plus-two-days they are gone.
See `issue` below for the contract that closes this.

Usage (from the module root):

  py -3 tests/tools/export_fixture.py chain-pull --all
  py -3 tests/tools/export_fixture.py chain-pull --decision 7708256751925167445 --idx 0
  py -3 tests/tools/export_fixture.py issue --id 42
  py -3 tests/tools/export_fixture.py death --id 7 --before-ms 60000
  py -3 tests/tools/export_fixture.py route-detour --limit 50

Nothing here is called from the game. The only database access is this script.
"""

import argparse
import json
import os
import subprocess
import sys

MYSQL = r"C:/WoW/deps/mysql/bin/mysql.exe"
DB = "acore_playerbots"
HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.normpath(os.path.join(HERE, "..", "fixtures"))

# the journal prints these numbers with one decimal, so a fixture cannot settle a case that falls inside the
# rounding band. The tests are told the band instead of guessing it.
NUMBER_PRECISION = 0.1


def query(sql):
    """Rows of a query, as lists of strings. '\\N' (MySQL's NULL) becomes None - absence is not zero."""
    out = subprocess.run([MYSQL, "-uacore", "-pacore", DB, "--batch", "--raw", "-N", "-e", sql],
                         capture_output=True, text=True, encoding="utf-8", errors="replace")
    if out.returncode != 0:
        # never judge success by a pipeline's exit code: read the error text
        sys.stderr.write(out.stderr)
        raise SystemExit("query failed: " + sql[:120])
    rows = []
    for line in out.stdout.splitlines():
        if not line.strip():
            continue
        rows.append([None if cell == "\\N" else cell for cell in line.split("\t")])
    return rows


def table_exists(name):
    return bool(query("SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA='%s' AND TABLE_NAME='%s'"
                      % (DB, name)))


def write(subdir, name, payload):
    directory = os.path.join(FIXTURES, subdir)
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, name)
    # a flat object, one key per line: the test's reader takes nothing nested, and a flat file reviews as a diff
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("{\n")
        items = list(payload.items())
        for index, (key, value) in enumerate(items):
            comma = "," if index + 1 < len(items) else ""
            handle.write("    %s: %s%s\n" % (json.dumps(key), json.dumps(value), comma))
        handle.write("}\n")
    return path


# --------------------------------------------------------------------------------------------------------------
# chain pull: the twenty numbers of the chain pull judge, one fixture per recorded judgement
# --------------------------------------------------------------------------------------------------------------

CHAIN_KEYS = [
    ("fight_sec_left", "fightSecLeft", float), ("walk_sec", "walkSec", float),
    ("run_in_sec", "runInSec", float), ("open_sec", "openSec", float),
    ("tank_health", "tankHealth", float), ("drop", "drop", float),
    ("overload", "overload", float), ("union_sec", "unionSec", float),
    ("group_dps", "groupDps", float), ("distance", "distance", float),
    ("walk_yd", "walkYd", float), ("healer_sec", "healerSec", float),
    ("pack_mobs", "packMobs", int), ("fight_mobs", "fightMobs", int),
    ("union_mobs", "unionMobs", int), ("stop", "stop", int),
    ("opener", "opener", str), ("healer", "healer", str),
    ("healer_walks", "healerWalks", bool), ("recorded_due", "due", bool),
]


def chain_rows(decision=None, idx=None, limit=None):
    where = "o.fields LIKE '%fightSecLeft%'"
    if decision is not None:
        where += " AND o.decision_id = %d" % int(decision)
    if idx is not None:
        where += " AND o.idx = %d" % int(idx)
    sql = ("SELECT o.decision_id, o.idx, o.at_ms, o.build, o.taken, REPLACE(o.fields, '\\t', ' '), "
           "       COALESCE(d.run_id, ''), COALESCE(d.run_ms, 0), COALESCE(d.judge_name, ''), COALESCE(d.topic, '') "
           "FROM pb_journal_option o LEFT JOIN pb_journal_decision d "
           "  ON d.id = o.decision_id AND d.hour_bucket = o.hour_bucket "
           "WHERE " + where + " ORDER BY o.decision_id, o.idx")
    if limit:
        sql += " LIMIT %d" % int(limit)
    return query(sql)


def export_chain(args):
    rows = chain_rows(args.decision, args.idx, args.limit)
    if not rows:
        print("no chain pull judgements matched; nothing written")
        return 1
    written = 0
    for decision_id, option_idx, at_ms, build, taken, fields, run_id, run_ms, judge, topic in rows:
        raw = json.loads(fields)
        payload = {
            "fixture": "chain_pull",
            "source": "pb_journal_option + pb_journal_decision",
            "decision_id": int(decision_id),
            "idx": int(option_idx),
            "at_ms": int(at_ms),
            # the clock the judge read, as a fact of the fixture. The judge's seam never asks the machine.
            "now_ms": int(run_ms or 0),
            "run_id": run_id or "",
            "judge_name": judge or "",
            "topic": topic or "",
            "build": build or "",
            "recorded_kind": raw.get("kind", ""),
            "recorded_taken": bool(int(taken)),
            "recorded_why": raw.get("why", ""),
            "walk": raw.get("kind") == "walk",
            "number_precision": NUMBER_PRECISION,
        }
        for key, source, kind in CHAIN_KEYS:
            if source not in raw:
                continue  # absent stays absent; the test's fallback says so
            value = raw[source]
            payload[key] = bool(value) if kind is bool else kind(value)
        name = "%s_%s.json" % (decision_id, option_idx)
        write("chain_pull", name, payload)
        written += 1
    print("wrote %d chain pull fixture(s) into %s" % (written, os.path.join(FIXTURES, "chain_pull")))
    return 0


# --------------------------------------------------------------------------------------------------------------
# issue: one fixture per row of the problem table, taken at the moment the issue is raised
# --------------------------------------------------------------------------------------------------------------
#
# THE CONTRACT, to be agreed with the session that owns src/Ai/Journal/**:
#
#   An issue row outlives the journal (it lives until the problem is fixed, weeks), while the inputs that reproduce
#   it are dropped with their partition after 24-48 h. So the row itself must carry its inputs:
#
#     pb_journal_issue.fixture  TEXT   -- the flat JSON of the inputs, written when the issue is raised
#     pb_journal_issue.klass    VARCHAR-- which of the three classes below the row belongs to
#     pb_journal_issue.decision_id     -- the decision it came out of, when there is one
#     pb_journal_issue.run_id          -- the run, for the window before the event
#
#   The emitter calls nothing of mine on the game thread (no file is written from a map thread): it fills `fixture`
#   from the facts it already holds, exactly as it fills every other column. This script then turns rows into files.
#
#   The three classes, because the test that can be written differs per class:
#     "decision" - inputs recorded and the judge deterministic (a death, a pull taken with a non-positive margin,
#                  an underpriced pack, a healer farther than the judge's own rule allows). A real test: the
#                  verdict turns over on the recorded inputs.
#     "detector" - the defect is that we did not NOTICE (an idle diagnosis that excludes tanks and healers is a
#                  defect of the instrument). The test feeds the recorded state and demands the detector fire.
#                  Cheap and badly needed: our instruments have broken silently five times.
#     "sequence" - a property of the world over time (standing still for a minute, an order and a walk blocking
#                  each other, a looping activity). One set of inputs cannot hold it; it needs a series of states.
#                  These close on a run, not on a test.
def export_issue(args):
    if not table_exists("pb_journal_issue"):
        print("pb_journal_issue does not exist yet (it is being created by the journal session). "
              "Nothing to export; the contract for its `fixture` and `klass` columns is in the comment above.")
        return 1
    columns = [row[0] for row in query("SELECT COLUMN_NAME FROM information_schema.COLUMNS "
                                       "WHERE TABLE_SCHEMA='%s' AND TABLE_NAME='pb_journal_issue'" % DB)]
    if "fixture" not in columns:
        print("pb_journal_issue has no `fixture` column: the inputs of an issue are not snapshotted when it is "
              "raised, so a row older than the retention cannot be reproduced at all. That column is the fix.")
        return 1
    where = "1=1" if args.id is None else "id = %d" % int(args.id)
    rows = query("SELECT id, %s, fixture FROM pb_journal_issue WHERE %s ORDER BY id"
                 % ("COALESCE(klass,'')" if "klass" in columns else "''", where))
    for issue_id, klass, fixture in rows:
        if not fixture:
            print("issue %s carries no fixture: raised without its inputs, cannot be reproduced" % issue_id)
            continue
        payload = json.loads(fixture)
        payload.setdefault("fixture", "issue")
        payload["issue_id"] = int(issue_id)
        payload["klass"] = klass or ""
        write(os.path.join("issue", klass or "unclassified"), "issue_%s.json" % issue_id, payload)
    print("wrote %d issue fixture(s)" % len(rows))
    return 0


# --------------------------------------------------------------------------------------------------------------
# death, and the window of decisions before it (a delayed effect is sought backwards in the record)
# --------------------------------------------------------------------------------------------------------------
def export_death(args):
    if not table_exists("pb_journal_death"):
        print("pb_journal_death does not exist yet (the journal session is creating it). Nothing to export.")
        return 1
    rows = query("SELECT id, at_ms, COALESCE(run_id,''), COALESCE(decision_id,0) FROM pb_journal_death "
                 + ("WHERE id = %d" % int(args.id) if args.id is not None else "")
                 + " ORDER BY id DESC LIMIT %d" % (1 if args.id is not None else int(args.limit or 20)))
    if not rows:
        print("no death rows yet")
        return 1
    for death_id, at_ms, run_id, decision_id in rows:
        write("death", "death_%s.json" % death_id,
              {"fixture": "death", "death_id": int(death_id), "at_ms": int(at_ms), "run_id": run_id,
               "decision_id": int(decision_id or 0), "number_precision": NUMBER_PRECISION})
        # the window BEFORE the event: a death may be the effect of a decision taken earlier, so the stand must be
        # able to load a series. No criterion for "this decision caused that death" is invented here - the journal
        # holds none; the window is loaded and shown, and the link is a finding a human makes.
        before = int(args.before_ms or 60000)
        window = query("SELECT id, idx_or_zero FROM (SELECT id, 0 AS idx_or_zero FROM pb_journal_decision "
                       "WHERE at_ms BETWEEN %d AND %d %s ORDER BY at_ms) w"
                       % (int(at_ms) - before, int(at_ms),
                          ("AND run_id = '%s'" % run_id.replace("'", "")) if run_id else ""))
        for seq, (decision, _) in enumerate(window):
            export_chain(argparse.Namespace(decision=decision, idx=None, limit=None))
            seq += 1
        print("death %s: %d decision(s) in the %d ms before it" % (death_id, len(window), before))
    return 0


# --------------------------------------------------------------------------------------------------------------
# route detour: the one route question that needs no navmesh in the tests
# --------------------------------------------------------------------------------------------------------------
def route_detour(args):
    """A detour shows ONLY as the ratio of path_yd to straight_yd - neither number alone shows it. No mesh is
    needed to read the ratio off the recorded legs, which is why the tests carry no navmesh: this answers whether
    the geometry of a path is ever our problem at all. Never fires: work saved. Fires on a real route: there is the
    fixture that would justify linking Detour, and the user decides again then."""
    if not table_exists("pb_journal_route"):
        print("pb_journal_route does not exist yet")
        return 1
    rows = query("SELECT COUNT(*) FROM pb_journal_route WHERE path_yd > 0 AND straight_yd > 0")
    if not rows or rows[0][0] == "0":
        print("pb_journal_route holds no leg with both path_yd and straight_yd yet (the walking domain's calls are "
              "being added by another session). The detour sign is ready and has nothing to read.")
        return 0
    rows = query("SELECT event, COALESCE(purpose,''), path_yd, straight_yd, path_yd/straight_yd AS ratio, "
                 "COALESCE(why,'') FROM pb_journal_route WHERE path_yd > 0 AND straight_yd > 0 "
                 "ORDER BY ratio DESC LIMIT %d" % int(args.limit or 50))
    print("%-10s %-14s %9s %9s %7s  why" % ("event", "purpose", "path_yd", "straight", "ratio"))
    for event, purpose, path_yd, straight, ratio, why in rows:
        print("%-10s %-14s %9.1f %9.1f %7.2f  %s" % (event, purpose, float(path_yd), float(straight), float(ratio),
                                                     why))
    return 0


# --------------------------------------------------------------------------------------------------------------
# slice: one problem's whole situation, across the tables that hold it
# --------------------------------------------------------------------------------------------------------------
#
# A fixture for a problem is not one row: it is what the five was doing - the open decisions, the participants, the
# route's stops, the fights and the aggro in the window. Three rules this follows:
#
# 1. THE WINDOW IS SET BY AN EVENT, NOT BY EYE. Choosing "how many seconds back" per stall by hand would bake the
#    author's guess into the fixture and the test would then guard the guess. The window runs from the last real
#    progress (a route stop changed, a boss killed, a pull begun, damage dealt) to the moment of the problem. That
#    is what `last_progress_ms` on the issue row is for; with it the slice is a query, not a human decision - the
#    same thought as recording a duration instead of a threshold.
# 2. JOINED BY IDENTITY, AND A JOIN BY TIME IS MARKED. run_id joins decisions, fights, aggro, deaths, routes and
#    loot honestly. It does NOT join pb_journal_line (no run_id at all, only bot_low) or playerbots_fight_history
#    (the column exists and is indexed but is empty in all 75 074 rows). Whatever has to be gathered by nearness in
#    time carries joined_by="time", so a test can see that its evidence may belong to ANOTHER fight. Four times in
#    one day two different objects here wore the same name; this is that family of error.
# 3. THE FIXTURE IS SELF-CONTAINED. It is a file in the repository and it outlives the journal's retention, so it
#    carries everything the test needs and nothing that needs the database again - the build, the window and its
#    source included, or in a month nobody can tell what it describes.

# table -> (how it joins to a run, the time column)
SLICE_TABLES = [
    ("pb_journal_decision", "run_id", "at_ms", "identity"),
    ("pb_journal_actor", None, None, "identity"),          # child of a decision, taken through its decision_id
    ("pb_journal_option", None, None, "identity"),
    ("pb_journal_route", "run_id", "at_ms", "identity"),
    ("pb_journal_route_stop", None, None, "identity"),
    ("pb_journal_fight", "run_id", "at_ms", "identity"),
    ("pb_journal_aggro", "run_id", "at_ms", "identity"),
    ("pb_journal_death", "run_id", "at_ms", "identity"),
    ("pb_journal_loot", "run_id", "at_ms", "identity"),
    ("pb_journal_line", None, "at_ms", "time"),            # no run_id exists on this table
]


def columns_of(table):
    return [row[0] for row in query("SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='%s' "
                                    "AND TABLE_NAME='%s' ORDER BY ORDINAL_POSITION" % (DB, table))]


def export_slice(args):
    run_id = args.run_id
    to_ms = args.to_ms
    from_ms = args.from_ms
    window_source = "given"
    if args.issue is not None:
        if not table_exists("pb_journal_issue"):
            print("pb_journal_issue does not exist yet; pass --run/--to-ms/--from-ms by hand meanwhile")
            return 1
        have = columns_of("pb_journal_issue")
        need = [c for c in ("run_id", "at_ms", "last_progress_ms") if c not in have]
        if need:
            print("pb_journal_issue is missing %s: without last_progress_ms the window would be a human guess, "
                  "which is exactly what must not go into a fixture" % ", ".join(need))
            return 1
        rows = query("SELECT run_id, at_ms, last_progress_ms FROM pb_journal_issue WHERE id=%d" % int(args.issue))
        if not rows:
            print("no issue %s" % args.issue)
            return 1
        run_id, to_ms, from_ms = rows[0][0], int(rows[0][1]), int(rows[0][2] or 0)
        window_source = "last_progress"
    if not to_ms:
        print("--to-ms (the moment of the problem) is required")
        return 1
    if not from_ms:
        print("no window start: pass --from-ms, or an issue row carrying last_progress_ms. The window is set by an "
              "event, never by eye.")
        return 1

    name = args.name or ("slice_%s_%s" % (run_id or "norun", to_ms))
    subdir = os.path.join("slice", name)
    manifest = {
        "fixture": "slice",
        "run_id": run_id or "",
        "from_ms": int(from_ms),
        "to_ms": int(to_ms),
        "window_ms": int(to_ms) - int(from_ms),
        "window_source": window_source,
        "exported_from": DB,
    }
    builds = set()
    written = {}
    missing = []
    decision_ids = []
    for table, runCol, timeCol, how in SLICE_TABLES:
        if not table_exists(table):
            missing.append(table)
            continue
        have = columns_of(table)
        where = []
        if timeCol and timeCol in have:
            where.append("%s BETWEEN %d AND %d" % (timeCol, int(from_ms), int(to_ms)))
        if runCol and runCol in have and run_id:
            where.append("%s = '%s'" % (runCol, run_id.replace("'", "")))
        elif runCol and run_id and runCol not in have:
            how = "time"  # the column the join needed is not there
        if not where and decision_ids and "decision_id" in have:
            where.append("decision_id IN (%s)" % ",".join(decision_ids))
        if not where:
            missing.append(table + " (no key and no time column to cut by)")
            continue
        rows = query("SELECT %s FROM %s WHERE %s ORDER BY %s LIMIT %d"
                     % (", ".join("COALESCE(CAST(%s AS CHAR), '\\\\N')" % c for c in have), table,
                        " AND ".join(where), timeCol if timeCol and timeCol in have else have[0],
                        int(args.limit or 500)))
        for index, row in enumerate(rows):
            payload = {"fixture": table, "joined_by": how, "run_id": run_id or "", "from_ms": int(from_ms),
                       "to_ms": int(to_ms), "window_source": window_source}
            for key, value in zip(have, row):
                payload[key] = "" if value is None else value
            if table == "pb_journal_decision" and payload.get("id"):
                decision_ids.append(str(payload["id"]))
            if payload.get("build"):
                builds.add(payload["build"])
            write(subdir, "%s_%04d.json" % (table.replace("pb_journal_", ""), index), payload)
        written[table] = len(rows)
    manifest["builds"] = ",".join(sorted(builds))
    manifest["rows"] = ",".join("%s=%d" % (t, n) for t, n in sorted(written.items()))
    manifest["absent"] = ",".join(missing)
    # the two gaps that force part of a slice to be joined by time instead of identity, named in the fixture itself
    manifest["joins_by_time_because"] = ("pb_journal_line carries no run_id (only bot_low); "
                                         "playerbots_fight_history.run_id exists but is empty in every row")
    path = write(subdir, "slice.json", manifest)
    print("wrote the slice manifest %s" % path)
    print("rows: %s" % (manifest["rows"] or "none"))
    if missing:
        print("absent or unjoinable: %s" % manifest["absent"])
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="what", required=True)

    chain = sub.add_parser("chain-pull", help="the chain pull judge's recorded judgements")
    chain.add_argument("--decision", type=int, help="one decision id")
    chain.add_argument("--idx", type=int, help="one option index of that decision")
    chain.add_argument("--limit", type=int, help="at most this many")
    chain.add_argument("--all", action="store_true", help="every recorded judgement")
    chain.set_defaults(run=export_chain)

    issue = sub.add_parser("issue", help="one row of the problem table, with the inputs it carries")
    issue.add_argument("--id", type=int)
    issue.set_defaults(run=export_issue)

    death = sub.add_parser("death", help="a death and the decisions in the window before it")
    death.add_argument("--id", type=int)
    death.add_argument("--limit", type=int, default=20)
    death.add_argument("--before-ms", type=int, default=60000)
    death.set_defaults(run=export_death)

    slice_cmd = sub.add_parser("slice", help="one problem's situation across the tables that hold it")
    slice_cmd.add_argument("--issue", type=int, help="take the run, the moment and the window from an issue row")
    slice_cmd.add_argument("--run", dest="run_id", help="run_id")
    slice_cmd.add_argument("--to-ms", type=int, help="the moment of the problem")
    slice_cmd.add_argument("--from-ms", type=int, help="the last real progress before it")
    slice_cmd.add_argument("--name", help="the directory name under tests/fixtures/slice/")
    slice_cmd.add_argument("--limit", type=int, default=500, help="at most this many rows per table")
    slice_cmd.set_defaults(run=export_slice)

    route = sub.add_parser("route-detour", help="the detour sign: path_yd against straight_yd")
    route.add_argument("--limit", type=int, default=50)
    route.set_defaults(run=route_detour)

    args = parser.parse_args()
    return args.run(args)


if __name__ == "__main__":
    raise SystemExit(main())
