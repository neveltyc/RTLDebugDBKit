#!/usr/bin/env python3
# Export one corpus with two rtl-designdb binaries and diff every table row
# by row. This is the migration gate for internal refactors: the contract is
# that a representation change produces byte-identical rows, so the gate is
# "the diff is empty, or every differing (case, table) pair was declared in
# an expected-changes file before the change was made".
#
# check-reproducible.py proves one binary agrees with itself; this proves two
# binaries agree with each other. It compares base tables only -- the views
# are derived, so a view can only differ if some table does.
#
#   diff-designdb.py --old BIN --new BIN [options]
#
#   --old BIN / --new BIN   the two exporters (--old may equal --new: the
#                           self-test that must always come back empty)
#   --repo DIR              repository root (default: this script's parent)
#   --out DIR               working dir for the exports (default: build/diffdb)
#   --expected FILE         allowed diffs, one "case table" pair per line,
#                           '#' comments; a pair that never fires is reported
#                           as stale so the list cannot outlive its change
#   --designs auto|off|DIR  the export-real-designs.sh corpus: auto (default)
#                           probes the same ../rwa layout that script uses,
#                           off skips, DIR names the checkout
#   --triage                for tables that differ, also diff with id-shaped
#                           columns removed -- separates "same content, ids
#                           renumbered" from a real content change
#
# The corpus and each case's command line mirror ci.yml's export loop and
# export-real-designs.sh exactly; the fixture list comes from
# verify-designdb.py --list-modes so it stays named in one place.
import argparse
import collections
import os
import sqlite3
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# The one seal column that legitimately differs between two builds of the
# tool. Nothing else is excluded: schema_version, slang_version and
# config_digest must match, so a phase that accidentally moves one fails here
# first. Keyed by (table, column) because the seal is a row now: a row filter
# would have to know which key it was dropping, and there are no keys.
EXCLUDED_COLUMNS = {("db_info", "producer_revision")}

# The timing note threshold. Informational, not a gate: the plan's +-10%
# budget is judged by a human against the report.
SLOWDOWN_NOTE = 1.10


def fail(msg):
    sys.exit(f"diff-designdb: {msg}")


def list_modes(repo):
    verify = os.path.join(repo, "scripts", "verify-designdb.py")
    out = subprocess.run([sys.executable, verify, "--list-modes"],
                         capture_output=True, text=True)
    if out.returncode != 0:
        fail(f"--list-modes failed: {out.stderr.strip()}")
    return out.stdout.split()


def corpus(repo, designs):
    # (name, cwd, argv-without-output). Paths absolute so cwd is free.
    ex = os.path.join(repo, "examples")
    cases = [("basic", repo, [os.path.join(ex, "basic", "top.sv"),
                              "--top", "top"])]
    for m in list_modes(repo):
        cases.append((m, repo, [os.path.join(ex, "constructs", m + ".sv")]))
    cases.append(("options", os.path.join(ex, "options"),
                  ["-f", "opts.f", "--single-unit", "+incdir+.",
                   "+define+EXTRA_WIDTH=4"]))
    cases.append(("reorder", os.path.join(ex, "reorder"), ["-f", "reorder.f"]))

    if designs != "off":
        rwa = None
        if designs == "auto":
            for cand in ("../rwa", "../../rwa", "../../../rwa",
                         "../../../../rwa"):
                p = os.path.abspath(os.path.join(repo, cand))
                if os.path.isdir(os.path.join(p, "picorv32")):
                    rwa = p
                    break
        elif os.path.isdir(designs):
            rwa = os.path.abspath(designs)
        else:
            fail(f"--designs {designs}: no such directory")
        if rwa:
            cases.append(("picorv32", os.path.join(rwa, "picorv32"),
                          [os.path.join(rwa, "picorv32", "picorv32.v"),
                           "--top", "picorv32"]))
            tiny = os.path.join(rwa, "tinyriscv")
            if os.path.isfile(os.path.join(tiny, "tiny.f")):
                cases.append(("tinyriscv", tiny,
                              ["-f", "tiny.f", "--top", "tinyriscv_soc_top"]))
            veer = os.path.join(rwa, "veerwolf_run", "build",
                                "veerwolf_0.7.5", "sim-verilator")
            if os.path.isfile(os.path.join(veer, "designdb_real.f")):
                cases.append(("veerwolf", veer,
                              ["-f", "designdb_real.f",
                               "--top", "veerwolf_core"]))
    return cases


def export(binary, name, cwd, args, outdir):
    db = os.path.join(outdir, name + ".db")
    if os.path.exists(db):
        os.remove(db)
    start = time.monotonic()
    # `-o` is the only option this adds, because the two binaries straddle a
    # change and need not agree on any other spelling: a quiet flag was here
    # once and made the gate unusable across the release that renamed it,
    # while buying nothing -- the output is captured either way, and only a
    # failing export ever prints it.
    r = subprocess.run([binary] + args + ["-o", db],
                       cwd=cwd, capture_output=True, text=True)
    elapsed = time.monotonic() - start
    # 0, 3 and 4 all wrote a database -- complete, partial and hierarchy
    # only. Two fixtures and some third-party trees are deliberately not
    # complete, and their rows have to be compared like any other's.
    if r.returncode not in (0, 3, 4):
        fail(f"export {name} with {binary} failed (exit {r.returncode}):"
             f"\n{r.stdout}{r.stderr}")
    return db, elapsed


def tables_of(cur):
    cur.execute("SELECT name FROM sqlite_master WHERE type='table'"
                " AND name NOT LIKE 'sqlite_%' ORDER BY name")
    return [r[0] for r in cur.fetchall()]


def columns_of(cur, table):
    cur.execute(f'PRAGMA table_info("{table}")')
    return [r[1] for r in cur.fetchall()]


def rows_of(cur, table, cols, drop_ids=False):
    kept = [c for c in cols
            if (table, c) not in EXCLUDED_COLUMNS
            and not (drop_ids and (c == "id" or c.endswith("_id")))]
    if not kept:
        return []
    sel = ", ".join(f'"{c}"' for c in kept)
    order = ", ".join(f'"{c}"' for c in kept)
    q = f'SELECT {sel} FROM "{table}"'
    cur.execute(q + f" ORDER BY {order}")
    return cur.fetchall()


def diff_rows(old_rows, new_rows, limit=5):
    # Multiset difference: comparing rows for ORDER would re-implement
    # SQLite's collation across NULLs and mixed types. Samples keep the
    # tables' own row order.
    co = collections.Counter(old_rows)
    cn = collections.Counter(new_rows)
    gone = co - cn
    born = cn - co
    samples = []
    for r in old_rows:
        if len(samples) >= limit:
            break
        if gone.get(r):
            samples.append(("-", r))
            gone[r] -= 1
    gone = co - cn
    for r in new_rows:
        if len(samples) >= 2 * limit:
            break
        if born.get(r):
            samples.append(("+", r))
            born[r] -= 1
    born = cn - co
    return sum(gone.values()), sum(born.values()), samples


def compare_case(name, old_db, new_db, triage):
    diffs = []          # (table, only_old, only_new, samples, triage_line)
    with sqlite3.connect(old_db) as co, sqlite3.connect(new_db) as cn:
        po, pn = co.cursor(), cn.cursor()
        to, tn = tables_of(po), tables_of(pn)
        if to != tn:
            diffs.append(("<schema>", len(set(to) - set(tn)),
                          len(set(tn) - set(to)),
                          [("-", t) for t in to if t not in tn] +
                          [("+", t) for t in tn if t not in to], ""))
            common = [t for t in to if t in tn]
        else:
            common = to
        for t in common:
            co_cols, cn_cols = columns_of(po, t), columns_of(pn, t)
            if co_cols != cn_cols:
                diffs.append((t, 0, 0,
                              [("-", tuple(co_cols)), ("+", tuple(cn_cols))],
                              "column sets differ"))
                continue
            old_rows = rows_of(po, t, co_cols)
            new_rows = rows_of(pn, t, cn_cols)
            if old_rows == new_rows:
                continue
            oo, on, samples = diff_rows(old_rows, new_rows)
            note = ""
            if triage:
                # Multiset equality; sorting would trip over NULLs, which
                # Python will not order against integers.
                bo = collections.Counter(rows_of(po, t, co_cols, drop_ids=True))
                bn = collections.Counter(rows_of(pn, t, cn_cols, drop_ids=True))
                note = ("ids renumbered only" if bo == bn
                        else "content differs beyond ids")
            diffs.append((t, oo, on, samples, note))
    return diffs


def load_expected(path):
    allowed = set()
    for ln in open(path):
        ln = ln.split("#", 1)[0].strip()
        if not ln:
            continue
        parts = ln.split()
        if len(parts) != 2:
            fail(f"{path}: expected 'case table', got: {ln}")
        allowed.add((parts[0], parts[1]))
    return allowed


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--repo", default=os.path.dirname(HERE))
    ap.add_argument("--out", default=None)
    ap.add_argument("--expected", default=None)
    ap.add_argument("--designs", default="auto")
    ap.add_argument("--triage", action="store_true")
    opt = ap.parse_args()

    repo = os.path.abspath(opt.repo)
    old_bin = os.path.abspath(opt.old)
    new_bin = os.path.abspath(opt.new)
    for b in (old_bin, new_bin):
        if not os.access(b, os.X_OK):
            fail(f"not an executable: {b}")
    outdir = os.path.abspath(opt.out or os.path.join(repo, "build", "diffdb"))
    old_out = os.path.join(outdir, "old")
    new_out = os.path.join(outdir, "new")
    os.makedirs(old_out, exist_ok=True)
    os.makedirs(new_out, exist_ok=True)
    allowed = load_expected(opt.expected) if opt.expected else set()

    cases = corpus(repo, opt.designs)
    fired = set()
    unexpected = 0
    slow_notes = []
    print(f"corpus: {len(cases)} cases   old: {old_bin}   new: {new_bin}")
    for name, cwd, args in cases:
        old_db, t_old = export(old_bin, name, cwd, args, old_out)
        new_db, t_new = export(new_bin, name, cwd, args, new_out)
        if t_old > 0.5 and t_new > t_old * SLOWDOWN_NOTE:
            slow_notes.append(f"{name}: {t_old:.2f}s -> {t_new:.2f}s")
        diffs = compare_case(name, old_db, new_db, opt.triage)
        if not diffs:
            print(f"  {name}: identical")
            continue
        for table, oo, on, samples, note in diffs:
            pair = (name, table)
            verdict = "expected" if pair in allowed else "UNEXPECTED"
            if pair in allowed:
                fired.add(pair)
            else:
                unexpected += 1
            print(f"  {name}: {table}: -{oo} +{on} rows [{verdict}]"
                  + (f" ({note})" if note else ""))
            for sign, row in samples:
                print(f"      {sign} {row}")

    stale = allowed - fired
    for pair in sorted(stale):
        print(f"  stale expectation never fired: {pair[0]} {pair[1]}")
    for n in slow_notes:
        print(f"  timing note (>{SLOWDOWN_NOTE:.0%} of old): {n}")
    if unexpected:
        sys.exit(f"diff-designdb: {unexpected} differing (case, table) pairs"
                 " were not declared expected")
    if stale:
        sys.exit("diff-designdb: expected-changes list has entries that"
                 " never fired; prune it")
    print("diff-designdb: gate passed")


if __name__ == "__main__":
    main()
