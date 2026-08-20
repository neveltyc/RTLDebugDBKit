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
# `*_exact` flags, `dropped_operand_count`, the resolved_* columns, and the
# meta counts -- so it costs an export nothing and can be run against a
# database produced weeks ago.
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


TABLES = ("module", "tree_node", "inst", "inst_param", "prim", "net", "term",
          "term_map",
          "net_conn", "proc", "stmt", "assign_target", "assign_operand",
          "expr_ref", "proc_event", "net_dep", "hier_ref", "data_type", "file",
          "src_file")

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
# fans out to more of the object than the RTL really reaches.
inexact = {}
for tbl, col in (("net_dep", "src_exact"), ("net_dep", "tgt_exact"),
                 ("net_conn", "outer_exact"), ("net_conn", "term_exact"),
                 ("term_map", "term_exact"), ("term_map", "inner_exact"),
                 ("assign_target", "is_exact"), ("assign_operand", "is_exact"),
                 ("expr_ref", "is_exact"), ("hier_ref", "is_exact")):
    total = scalar(f'SELECT count(*) FROM "{tbl}" WHERE "{col}" IS NOT NULL')
    n = scalar(f'SELECT count(*) FROM "{tbl}" WHERE "{col}" = 0')
    inexact[f"{tbl}.{col}"] = {"inexact": n, "of": total, "pct": share(n, total)}
report["inexact_ranges"] = inexact

# Dependencies whose two ends do not correspond bit for bit. Not a defect
# count -- a design full of arithmetic legitimately reports a high share --
# but it is the fraction of the graph a bit-level trace cannot follow
# precisely, which is worth knowing before trusting one.
deps_total = scalar("SELECT count(*) FROM net_dep WHERE map_exact IS NOT NULL")
coarse = scalar("SELECT count(*) FROM net_dep WHERE map_exact = 0")
report["coarse_bit_mapping"] = {
    "n": coarse, "of": deps_total, "pct": share(coarse, deps_total)}
conns_total = scalar("SELECT count(*) FROM net_conn WHERE map_exact IS NOT NULL")
ccoarse = scalar("SELECT count(*) FROM net_conn WHERE map_exact = 0")
report["coarse_conn_mapping"] = {
    "n": ccoarse, "of": conns_total, "pct": share(ccoarse, conns_total)}

# Operands the exporter removed: compile-time constants, and references it
# could not store as a path. A statement with one operand and three dropped
# does not read the way one that genuinely reads one signal does.
stmts = report["rows"]["stmt"]
with_dropped = scalar("SELECT count(*) FROM stmt WHERE dropped_operand_count > 0")
report["dropped_operands"] = {
    "total": scalar("SELECT sum(dropped_operand_count) FROM stmt"),
    "statements_affected": with_dropped,
    "of": stmts,
    "pct": share(with_dropped, stmts),
}

# Dependencies the database records but cannot attribute to a source net.
# `q <= 8'h0` is one on purpose; a large share is the shape a systematic miss
# takes, because a target whose sources were all lost still gets its row.
deps = report["rows"]["net_dep"]
null_src = scalar("SELECT count(*) FROM net_dep WHERE src_net_id IS NULL")
report["deps_without_source"] = {
    "n": null_src, "of": deps, "pct": share(null_src, deps)}

# References that leave their instance, split by whether the export resolved
# them to a stamped object. Unresolved is not wrong -- an upward reference
# from a shared body has no one answer, and a path into a black box has no
# object to land on -- but the share is worth watching: it is the fraction of
# cross-hierarchy structure a consumer must resolve by hand.
hrefs = report["rows"]["hier_ref"]
resolved = scalar("SELECT count(*) FROM hier_ref WHERE resolved_net_id IS NOT NULL")
inst_only = scalar("""SELECT count(*) FROM hier_ref
                      WHERE resolved_inst_id IS NOT NULL AND resolved_net_id IS NULL""")
report["hier_ref_resolution"] = {
    "to_net": resolved, "to_instance_only": inst_only, "of": hrefs,
    "pct_to_net": share(resolved, hrefs)}

# Nets with no driver of any kind -- no dependency targets them, no
# crossing feeds them, and they are not a design input. The boundary nets
# are held apart: the world outside drives those by definition, while
# internal nets nothing drives are what a systematic extraction miss looks
# like.
#
# Phrased as set membership, not as a correlated NOT EXISTS. v_driver is a
# seven-branch union, and a correlated subquery re-runs it once per net --
# 90 seconds on a design where scanning it once takes 20 milliseconds.
root_inputs = scalar("""
    SELECT count(DISTINCT m.inner_net_id) FROM term_map m
    JOIN term t ON t.id = m.term_id
    JOIN tree_node n ON n.id = t.inst_id
    WHERE n.node_kind = 'root' AND t.direction IN ('input', 'inout')""")
undriven = scalar("""
    SELECT count(*) FROM net WHERE id NOT IN
        (SELECT signal_net_id FROM v_driver WHERE signal_net_id IS NOT NULL)""")
undriven_internal = scalar("""
    SELECT count(*) FROM net WHERE id NOT IN
        (SELECT signal_net_id FROM v_driver WHERE signal_net_id IS NOT NULL)
      AND id NOT IN
        (SELECT m.inner_net_id FROM term_map m JOIN term t ON t.id = m.term_id
         JOIN tree_node n ON n.id = t.inst_id
         WHERE n.node_kind = 'root' AND t.direction IN ('input', 'inout'))""")
nets = report["rows"]["net"]
report["undriven_nets"] = {
    "internal": undriven_internal,
    "root_inputs": root_inputs,
    "all_undriven": undriven,
    "of": nets,
    "pct_internal": share(undriven_internal, nets),
}

# The expansion ratio: how many occurrences each shared body was stamped out
# to. The instance-level model pays row count for object identity, and this
# is the multiplier it paid -- a ratio near 1.0 means the design has almost
# no replication and the expansion cost nothing.
variants = scalar("""SELECT count(*) FROM (SELECT DISTINCT module_id,
                     COALESCE(param_signature, '') FROM inst
                     WHERE module_id IS NOT NULL)""")
instances = scalar("SELECT count(*) FROM inst WHERE module_id IS NOT NULL")
report["expansion_ratio"] = round(instances / variants, 2) if variants else 0.0
report["variants"] = variants

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

print(f"\nexpansion   {report['expansion_ratio']}x  "
      f"({instances:,} occurrences over {variants:,} parameterised bodies)")

print("\ninexact bit ranges")
for k, v in report["inexact_ranges"].items():
    print(f"  {k:<26} {v['inexact']:>10,} / {v['of']:<12,} {v['pct']:>6}%")

d = report["dropped_operands"]
print(f"\ndropped operands   {d['total']:,} across {d['statements_affected']:,} "
      f"of {d['of']:,} statements ({d['pct']}%)")
e = report["deps_without_source"]
print(f"deps w/o source    {e['n']:,} of {e['of']:,} ({e['pct']}%)")
c = report["coarse_bit_mapping"]
print(f"no per-bit map     {c['n']:,} of {c['of']:,} mapped deps ({c['pct']}%)")
pc = report["coarse_conn_mapping"]
print(f"  at boundaries    {pc['n']:,} of {pc['of']:,} net ties ({pc['pct']}%)")
h = report["hier_ref_resolution"]
print(f"references out     {h['to_net']:,} of {h['of']:,} resolved to a net "
      f"({h['pct_to_net']}%), {h['to_instance_only']:,} to an instance only")
u = report["undriven_nets"]
print(f"undriven internal  {u['internal']:,} of {u['of']:,} nets "
      f"({u['pct_internal']}%)")
print(f"  (root input nets held apart: {u['root_inputs']:,}, "
      f"driven by the world outside)")
