#!/usr/bin/env python3
# Copyright (c) 2026 neveltyc
# released under the BSD 3-Clause License (see LICENSE)
#
# Export one design twice with one binary and fail if any row differs.
#
# Why this exists: `src_file` ids were handed out in the order slang's source
# loader finished reading files, which is a thread pool's completion order.
# Two exports of an unchanged tinyriscv disagreed on 44 of 15,773 rows -- 23
# of 28 `src_file` rows and the 21 `file.src_file_id` values pointing at them
# -- while being, row for row, the same database. Nothing was wrong with
# either one; they simply could not be compared, and comparing them is what
# `config_digest` and the per-file SHA-256 digests exist to make possible.
#
# The exporter already pays for this property elsewhere -- TemplateBuilder's
# group key is a source location rather than a pointer precisely so that
# module ids do not follow an address -- and it is asserted here rather than
# assumed, because the assumption held everywhere except one loop and no test
# would have noticed.
#
#   check-reproducible.py [-n N] <exporter> [exporter-arg...]
#
# The `-o` argument is supplied by this script; everything else is passed to
# the exporter untouched, so the invocation is the one being made for real.
# `-n` exports N times instead of 2 and diffs each against the first, which
# is worth having because the disagreement is a race and a small design can
# win it: examples/options has three source files and came out in the same
# (wrong) order every time. That fixture is caught by the id-order invariant
# in verify-designdb.py, not by this script -- the two are complementary.
#
# Compares every table, in rowid order, plus the schema itself. Views are not
# compared: they are derived from the tables, so a table-level match implies
# them, and they carry no order of their own.
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile

argv = sys.argv[1:]
repeats = 2
if len(argv) >= 2 and argv[0] == "-n":
    repeats = int(argv[1])
    argv = argv[2:]
if len(argv) < 1 or repeats < 2:
    sys.exit(f"usage: {sys.argv[0]} [-n N] <exporter> [exporter-arg...]")
exporter, exporter_args = argv[0], argv[1:]


def tables(con):
    return [r[0] for r in con.execute(
        "SELECT name FROM sqlite_master WHERE type = 'table' "
        "AND name NOT LIKE 'sqlite_%' ORDER BY name")]


def schema(con):
    return con.execute(
        "SELECT type, name, sql FROM sqlite_master ORDER BY type, name"
    ).fetchall()


def compare(ref_db, new_db, run):
    """Report every way `new_db` differs from `ref_db`, as a list of strings."""
    a, b = sqlite3.connect(ref_db), sqlite3.connect(new_db)
    problems = []

    if schema(a) != schema(b):
        problems.append(f"run {run}: schema differs from run 1")
        return problems, 0

    ta, tb = tables(a), tables(b)
    if ta != tb:
        problems.append(f"run {run}: table set differs: {ta} vs {tb}")
        return problems, 0

    compared = 0
    for t in ta:
        # rowid order, not a canonical sort: insertion order is the thing
        # under test. Two databases holding one set of rows in two orders is
        # exactly the failure this script was written for, and ordering the
        # rows by their own contents would hide it.
        ca = a.execute(f'SELECT rowid, * FROM "{t}" ORDER BY rowid')
        cb = b.execute(f'SELECT rowid, * FROM "{t}" ORDER BY rowid')
        differing = 0
        for ra, rb in zip(ca, cb):
            compared += 1
            if ra != rb:
                differing += 1
                if differing <= 3:
                    problems.append(f"run {run}: {t} rowid {ra[0]}: "
                                    f"{ra[1:]} != {rb[1:]}")
        # zip() stops at the shorter side, so a length difference has to be
        # asked about separately or a truncated table reads as a clean match.
        na = a.execute(f'SELECT count(*) FROM "{t}"').fetchone()[0]
        nb = b.execute(f'SELECT count(*) FROM "{t}"').fetchone()[0]
        if na != nb:
            problems.append(f"run {run}: {t} has {na} rows, then {nb}")
        elif differing > 3:
            problems.append(f"run {run}: {t}: {differing} of {na} rows differ "
                            f"(first 3 shown)")
    a.close()
    b.close()
    return problems, compared


tmp = tempfile.mkdtemp(prefix="designdb-repro-")
try:
    dbs = []
    for run in range(1, repeats + 1):
        db = os.path.join(tmp, f"run{run}.db")
        # The exporter's own output is captured rather than let through: it
        # is the same banner N times over on success, and on failure it is
        # the only thing that says why, so it is printed then and not before.
        proc = subprocess.run([exporter, *exporter_args, "-o", db],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            sys.stderr.write(proc.stdout + proc.stderr)
            sys.exit(f"export run {run} failed with status {proc.returncode}")
        if not os.path.exists(db):
            sys.exit(f"export run {run} wrote no database")
        dbs.append(db)

    problems, compared = [], 0
    for run, db in enumerate(dbs[1:], start=2):
        p, n = compare(dbs[0], db, run)
        problems += p
        compared = max(compared, n)

    if problems:
        for line in problems:
            print(f"FAIL: {line}", file=sys.stderr)
        sys.exit(f"export is not reproducible: {len(problems)} difference(s) "
                 f"across {repeats} runs of {' '.join(exporter_args)}")

    ntables = len(tables(sqlite3.connect(dbs[0])))
    print(f"ok: {repeats} exports agree on all {compared} rows "
          f"across {ntables} tables")
finally:
    shutil.rmtree(tmp, ignore_errors=True)
