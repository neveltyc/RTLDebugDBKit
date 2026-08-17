#!/usr/bin/env python3
# Copyright (c) 2026 neveltyc
# released under the BSD 3-Clause License (see LICENSE)
#
# What an export had to approximate, counted.
#
# verify-designdb.py answers "is this database well formed"; it is a pass/fail
# gate and says nothing about how much of the design it actually resolved. That
# question only becomes urgent at scale: the examples are small enough to read,
# so an operand silently dropped there is caught by eye, while on a real SoC the
# same bug is a rounding error in a number nobody printed.
#
# Everything here is derived from what the exporter already records -- the
# `*_exact` flags, `dropped_operands`, and the meta counts -- so it costs an
# export nothing and can be run against a database produced weeks ago.
#
# There is no pass/fail. A design legitimately full of dynamic indexing will
# report a high inexact share and be perfectly well extracted. The number worth
# watching is the one that moves when the exporter changes and the RTL does not.
#
#   designdb-coverage.py <design.db>            human-readable report
#   designdb-coverage.py <design.db> --json     the same numbers as JSON
import json
import os
import sqlite3
import sys

args = [a for a in sys.argv[1:] if not a.startswith("-")]
as_json = "--json" in sys.argv[1:]
if len(args) != 1:
    sys.exit(f"usage: {sys.argv[0]} <design.db> [--json]")

path = args[0]
con = sqlite3.connect(path)


def scalar(sql, default=0):
    row = con.execute(sql).fetchone()
    return row[0] if row and row[0] is not None else default


def meta(key):
    row = con.execute("SELECT value FROM meta WHERE key=?", (key,)).fetchone()
    return row[0] if row else None


def share(part, whole):
    """Percent, with the "no denominator" case spelled rather than divided."""
    return round(100.0 * part / whole, 2) if whole else 0.0


TABLES = ("module", "instance", "symbol", "edge", "assignment", "assign_operand",
          "child", "port", "proc_event", "hier_ref", "stmt_read", "name", "type",
          "file", "source_file")

report = {
    "database": os.path.basename(path),
    "bytes": os.path.getsize(path),
    "meta": {k: meta(k) for k in (
        "schema_version", "analysis_status", "tool_version", "slang_version",
        "producer_revision", "config_digest", "top", "error_count",
        "unresolved_count", "empty_procedure_count",
        "duplicate_path_count")},
    "rows": {t: scalar(f'SELECT count(*) FROM "{t}"') for t in TABLES},
}

# A reference the exporter could not narrow to specific bits. `exact=0` is an
# upper bound rather than the bits actually touched, so a trace crossing it
# fans out to more of the object than the RTL really reaches. Counted per
# table because the causes differ: a dynamic index in an assignment target, an
# unresolved property argument in an assertion, an interface path whose bits
# depend on a parameter this module cannot see.
inexact = {}
for tbl, col in (("edge", "src_exact"), ("edge", "dst_exact"),
                 ("port", "outer_exact"), ("port", "port_exact"),
                 ("assign_operand", "src_exact"),
                 ("stmt_read", "src_exact"), ("hier_ref", "path_exact")):
    total = scalar(f'SELECT count(*) FROM "{tbl}" WHERE "{col}" IS NOT NULL')
    n = scalar(f'SELECT count(*) FROM "{tbl}" WHERE "{col}" = 0')
    inexact[f"{tbl}.{col}"] = {"inexact": n, "of": total, "pct": share(n, total)}
report["inexact_ranges"] = inexact

# Edges whose two ends do not correspond bit for bit. Not a defect count -- a
# design full of arithmetic legitimately reports a high share -- but it is the
# fraction of the graph a bit-level trace cannot follow precisely, which is worth
# knowing before trusting one.
edges_total = scalar("SELECT count(*) FROM edge WHERE src IS NOT NULL")
coarse = scalar("SELECT count(*) FROM edge WHERE src IS NOT NULL AND map_exact = 0")
report["coarse_bit_mapping"] = {
    "n": coarse, "of": edges_total, "pct": share(coarse, edges_total)}
# The same fraction at the instance boundary: net ties whose two ends do not
# correspond bit for bit. NULL map_exact rows (no outer end) are no one's
# mapping and stay out of the denominator.
ports_total = scalar("SELECT count(*) FROM port WHERE map_exact IS NOT NULL")
pcoarse = scalar("SELECT count(*) FROM port WHERE map_exact = 0")
report["coarse_port_mapping"] = {
    "n": pcoarse, "of": ports_total, "pct": share(pcoarse, ports_total)}

# Operands the exporter removed: compile-time constants, and references that
# leave the module. A row with one operand and three dropped does not read the
# way a row that genuinely reads one signal does, and only this column tells
# them apart.
assignments = report["rows"]["assignment"]
with_dropped = scalar("SELECT count(*) FROM assignment WHERE dropped_operands > 0")
report["dropped_operands"] = {
    "total": scalar("SELECT sum(dropped_operands) FROM assignment"),
    "assignments_affected": with_dropped,
    "of": assignments,
    "pct": share(with_dropped, assignments),
}

# Drivers the database names but cannot attribute to a source signal. `q <= 8'h0`
# is one on purpose. A large share of them is not: it is the shape a systematic
# miss takes, because a target whose sources were all lost still gets its row.
edges = report["rows"]["edge"]
null_src = scalar("SELECT count(*) FROM edge WHERE src IS NULL")
report["edges_without_source"] = {"n": null_src, "of": edges, "pct": share(null_src, edges)}

# Declared signals that no edge drives, with input ports held apart from the
# rest. An input is driven by the parent and has no in-module driver by
# definition, so counting it as undriven buries the interesting number: a
# design's inputs are a fixed cost, while internal nets nothing drives are
# what a systematic extraction miss looks like. On examples/constructs the
# split is 16 inputs to 8 internal, and only the second number is worth
# watching.
NO_DRIVER = """FROM symbol sy WHERE sy.kind IN ('net', 'variable')
      AND NOT EXISTS (SELECT 1 FROM edge e
                      WHERE e.module = sy.module AND e.dst = sy.name)"""
undriven_in = scalar(f"SELECT count(*) {NO_DRIVER} AND sy.direction = 0")
undriven_other = scalar(f"SELECT count(*) {NO_DRIVER} AND (sy.direction IS NULL OR sy.direction != 0)")
signals = scalar("SELECT count(*) FROM symbol WHERE kind IN ('net','variable')")
non_input = scalar("""SELECT count(*) FROM symbol WHERE kind IN ('net','variable')
                      AND (direction IS NULL OR direction != 0)""")
report["undriven_signals"] = {
    "input_ports": undriven_in,
    "internal": undriven_other,
    "of_non_input": non_input,
    "pct_of_non_input": share(undriven_other, non_input),
    "of_all": signals,
}

# The folding ratio: how much the instance tree was compressed by keying rows
# on the module variant instead. It is the reason the schema stays small on a
# design that instantiates one core thirty-two times, and it is worth printing
# because a ratio near 1.0 means the folding is buying nothing -- a design of
# all-unique modules, or a parameterisation that split every instance into its
# own variant.
modules, instances = report["rows"]["module"], report["rows"]["instance"]
report["folding_ratio"] = round(instances / modules, 2) if modules else 0.0

if as_json:
    print(json.dumps(report, indent=2))
    sys.exit(0)

m = report["meta"]
print(f"{report['database']}  ({report['bytes'] / 1e6:.1f} MB, schema {m['schema_version']})")
print(f"  status     {m['analysis_status']}   "
      f"errors={m['error_count']} empty_proc={m['empty_procedure_count']} "
      f"unresolved={m['unresolved_count']} dup_paths={m['duplicate_path_count']}")
print(f"  producer   {m['tool_version']} / slang {m['slang_version']} / {m['producer_revision']}")

print("\nrows")
for t in TABLES:
    print(f"  {t:<16} {report['rows'][t]:>12,}")

print(f"\nfolding     {report['folding_ratio']}x  "
      f"({instances:,} instances over {modules:,} module variants)")

print("\ninexact bit ranges")
for k, v in report["inexact_ranges"].items():
    print(f"  {k:<26} {v['inexact']:>10,} / {v['of']:<12,} {v['pct']:>6}%")

d = report["dropped_operands"]
print(f"\ndropped operands   {d['total']:,} across {d['assignments_affected']:,} "
      f"of {d['of']:,} assignments ({d['pct']}%)")
e = report["edges_without_source"]
print(f"edges w/o source   {e['n']:,} of {e['of']:,} ({e['pct']}%)")
c = report["coarse_bit_mapping"]
print(f"no per-bit map     {c['n']:,} of {c['of']:,} sourced edges ({c['pct']}%)")
pc = report["coarse_port_mapping"]
print(f"  at boundaries    {pc['n']:,} of {pc['of']:,} net ties ({pc['pct']}%)")
u = report["undriven_signals"]
print(f"undriven internal  {u['internal']:,} of {u['of_non_input']:,} non-input signals "
      f"({u['pct_of_non_input']}%)")
print(f"  (plus {u['input_ports']:,} input port(s), driven by the parent by definition)")
