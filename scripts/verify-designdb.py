#!/usr/bin/env python3
# Copyright (c) 2026 neveltyc
# released under the BSD 3-Clause License (see LICENSE)
#
# Read an exported database back and fail if it is hollow or malformed.
#
# Two layers. The universal checks run against any database and assert what the
# schema contracts for every export: the subtype bijections, the ownership
# rules, the provenance matrix behind net_dep, the range discipline, and the
# stable views with their exact columns and row formulas. A mode adds
# the facts one fixture in examples/constructs/ must produce -- every fixture
# has one, so a file that carries no assertion of its own is a gap rather than
# a category.
#
# The fixtures are small enough to read, so a mode check pins an exact count
# wherever the count is a property of the RTL; `>=` is for the shapes the
# exporter may legitimately produce more of.
#
#   verify-designdb.py <design.db>          the universal checks
#   verify-designdb.py <design.db> <mode>   and that fixture's own
#   verify-designdb.py --list-modes         one mode per line, for CI to loop
#   verify-designdb.py --domain-coverage <db>...
#                                           fail unless the corpus together
#                                           produces every published value
import os
import pathlib
import re
import sqlite3
import sys

# mode -> the meta.top that fixture must elect. CI takes its loop from this
# table (--list-modes), so the fixture set is named in one place.
MODES = {
    "constructs": "constructs",
    "procedural": "procedural",
    "structural": "structural",
    "params": "params",
    "portshape": "portshape",
    "refport": "refport",
    "udp": "udps",
    "interfaces": "interfaces",
    "assertions": "assertions",
    "alias": "alias_top",
    "aliascat": "aliascat",
    "concatcursor": "concatcursor",
    "patterncase": "patterncase",
    "callsite": "callsite_top",
    "package": "package_top",
    "xmr": "xmr_top",
    "external": "tb_top",
    "outward": "outward_tb",
    "rootref": "rootref",
    "naming": "naming",
    "incomplete": "incomplete",
    "recursion": "recursion",
    "modport": "modport_top",
}

# Every closed value domain the schema publishes, as (table, column, values,
# nullable). A value here is part of the interface: it is in the DDL's CHECK
# clause and in doc/designdb-schema.md, so a consumer may write a branch for
# it -- which is why --domain-coverage requires the fixture corpus to produce
# every one of them, and why a value no input can reach does not belong.
DOMAINS = (
    ("module", "def_kind", ("module", "interface", "program", "package"), False),
    ("tree_node", "node_kind",
     ("root", "instance", "generate", "primitive", "unresolved", "package"), False),
    ("prim", "prim_kind", ("gate", "switch", "udp"), False),
    ("term", "term_kind", ("signal", "interface"), False),
    ("term", "direction", ("input", "output", "inout", "ref"), True),
    ("net_conn", "conn_kind",
     ("signal", "constant", "unconnected", "expression_operand", "interface",
      "external_reference"), False),
    ("proc", "proc_kind",
     ("always", "always_ff", "always_comb", "always_latch", "initial",
      "final"), False),
    ("stmt", "stmt_kind",
     ("assignment", "assertion", "wait", "call", "system_task", "event_control",
      "alias", "release", "trigger", "disable"), False),
    ("stmt", "assign_kind", ("continuous", "blocking", "nonblocking"), True),
    ("expr_ref", "role",
     ("control", "assertion", "wait", "event", "call_argument", "system_task"), False),
    ("branch", "branch_kind",
     ("if", "case", "case_item", "case_default", "loop"), False),
    ("branch", "sense", ("then", "else"), True),
    ("branch", "case_kind",
     ("case", "casez", "casex", "inside", "matches"), True),
    ("branch", "check_kind", ("unique", "unique0", "priority"), True),
    ("proc_event", "event_kind", ("sensitivity", "wait"), False),
    ("proc_event", "edge_kind", ("posedge", "negedge", "both"), True),
    ("net_dep", "dep_kind",
     ("data", "control", "primitive", "procedure", "alias"), False),
    ("hier_ref", "access", ("read", "write", "connect"), False),
)

# The view vocabularies, which are derived rather than stored and so carry no
# CHECK clause of their own. They are published the same way and covered the
# same way.
VIEW_DOMAINS = (
    ("v_driver", "driver_kind",
     ("data", "control", "primitive", "procedure", "connection",
      "connection_expression", "constant", "terminal", "system_task", "alias",
      "external", "trigger")),
    ("v_load", "load_kind",
     ("dataflow", "connection", "sensitivity", "wait", "statement", "terminal",
      "alias")),
    ("v_net_attachment", "attachment_kind",
     ("terminal_inside", "actual_outside", "written_by", "release_target",
      "alias_binding", "read_by", "condition", "statement_read", "event",
      "dep_in", "dep_out", "named_from_outside")),
    ("v_stmt_target", "target_kind",
     ("written_by", "release_target", "alias_binding")),
)


def domain_coverage(paths):
    """Fail unless the databases together produce every published value.

    A value domain is a promise to a consumer, and a fixture corpus that
    never reaches one of its values is a promise nothing tests. Run over the
    whole of examples/, this is what keeps the corpus driven by the schema
    rather than by whichever constructs happened to break once.
    """
    seen = {}
    for path in paths:
        c = sqlite3.connect(path)
        for tbl, col, _vals, _n in DOMAINS:
            for (v,) in c.execute(f'SELECT DISTINCT "{col}" FROM "{tbl}"'):
                seen.setdefault((tbl, col), set()).add(v)
        for view, col, _vals in VIEW_DOMAINS:
            for (v,) in c.execute(f'SELECT DISTINCT "{col}" FROM "{view}"'):
                seen.setdefault((view, col), set()).add(v)
        c.close()
    missing, total = [], 0
    for tbl, col, vals, _n in DOMAINS:
        total += len(vals)
        for v in sorted(set(vals) - seen.get((tbl, col), set())):
            missing.append(f"{tbl}.{col} = {v!r}")
    for view, col, vals in VIEW_DOMAINS:
        total += len(vals)
        for v in sorted(set(vals) - seen.get((view, col), set())):
            missing.append(f"{view}.{col} = {v!r}")
    if missing:
        for m in missing:
            print(f"FAIL: no fixture produces {m}", file=sys.stderr)
        sys.exit(f"{len(missing)} of {total} published values have no fixture "
                 f"({len(paths)} database(s) read)")
    print(f"ok: all {total} published values are produced by "
          f"{len(paths)} database(s)")


if sys.argv[1:2] == ["--domain-coverage"]:
    if len(sys.argv) < 3:
        sys.exit(f"usage: {sys.argv[0]} --domain-coverage <design.db>...")
    domain_coverage(sys.argv[2:])
    sys.exit(0)

if sys.argv[1:2] == ["--list-modes"]:
    # Written as bytes, not print()d: this is a machine-readable list, and text
    # mode on Windows would translate each newline to CRLF -- which a shell's
    # $(...) keeps, so the caller builds "<mode>\r.sv" and opens nothing.
    sys.stdout.buffer.write(("\n".join(MODES) + "\n").encode())
    sys.exit(0)
if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] not in MODES):
    sys.exit(f"usage: {sys.argv[0]} <design.db> [{'|'.join(MODES)}]\n"
             f"       {sys.argv[0]} --list-modes")

# Read-only, and only a file that is already there. A plain connect() CREATES
# the path it is handed, so verifying a mistyped name left a 0-byte database
# behind and then failed on `no such table: module` -- a traceback where the
# answer is "that file does not exist", and a file where a read-only tool
# should leave none.
db_path = sys.argv[1]
if not os.path.isfile(db_path):
    sys.exit(f"error: no such database: {db_path}")
try:
    con = sqlite3.connect(
        f"{pathlib.Path(db_path).resolve().as_uri()}?mode=ro", uri=True)
except sqlite3.Error as e:
    sys.exit(f"error: cannot read {db_path}: {e}")
mode = sys.argv[2] if len(sys.argv) == 3 else None

SCHEMA_VERSION = "20"

# Failures are collected rather than raised, so one run reports every broken
# contract instead of the first one. Only a precondition the rest of the file
# cannot run without -- a missing view, a meta key later code indexes -- stops
# it early, through fatal().
failures = []
checked = 0


def one(sql, *args):
    return con.execute(sql, args).fetchone()[0]


def check(ok, msg, detail=""):
    global checked
    checked += 1
    if not ok:
        failures.append(msg + (f" ({detail})" if detail else ""))


def fatal(msg):
    global checked
    checked += 1
    failures.append(msg)
    finish()


def finish():
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.exit(f"{len(failures)} of {checked} checks failed"
                 + (f" [{mode}]" if mode else ""))
    print(f"ok: {checked} checks passed" + (f" [{mode}]" if mode else ""))
    sys.exit(0)


# ----------------------------------------------------------------- hollow
counts = {
    t: one(f'SELECT count(*) FROM "{t}"')
    for t in ("module", "tree_node", "inst", "net", "term", "term_map")
}
empty = [t for t, n in counts.items() if n == 0]
check(not empty, "every core table has rows",
      f"empty: {', '.join(empty)}")

baddig = one("SELECT count(*) FROM src_file "
             "WHERE digest IS NULL OR length(digest) != 64")
total_sf = one("SELECT count(*) FROM src_file")
check(total_sf and not baddig, "every src_file row carries a SHA-256 digest",
      f"{baddig} of {total_sf} lack one")

# src_file ids ascend with path, which is the single-database form of "the
# export is reproducible". Interning them in the order slang's source loader
# finishes reading files -- a thread pool's completion order -- gives one
# unchanged design a different id column on every export, and nothing
# downstream reads an id's value, so the two databases are equivalent and
# still diff. Comparing them is what a digest-stamped export exists to make
# possible.
#
# Checked here rather than only by diffing two exports, because that diff is
# probabilistic on a small design and this is not: three files are enough to
# put them out of order.
#
# Compared as adjacent pairs in id order rather than by joining on id + 1,
# so a gap in the ids -- which nothing produces today, but which an ignored
# insert would -- does not silently skip the pair that straddles it.
#
# Ordered by SQLite rather than in Python, for two reasons. The exporter sorts
# with std::string::operator<, which is a byte comparison, and BINARY is the
# collation that means the same thing; Python's str comparison is by code
# point, which agrees for valid UTF-8 and is not the same rule. And a path is
# a byte string on Linux, not necessarily valid UTF-8 -- fetching one into
# Python raises rather than compares, so this was the one check that could
# make a well-formed database die on a directory name.
unsorted = con.execute("""
    SELECT a.id, a.path, b.id, b.path FROM src_file a JOIN src_file b
      ON b.id > a.id AND b.path < a.path COLLATE BINARY
    ORDER BY a.id, b.id LIMIT 1""").fetchone()
check(unsorted is None, f"src_file ids assigned in path order ({total_sf} rows)",
      "" if unsorted is None
      else f"id {unsorted[0]} is {unsorted[1]!r} but the later id "
           f"{unsorted[2]} is {unsorted[3]!r}")

# Structural integrity -- catches corruption and generator bugs. The writer
# leaves foreign_keys off for speed, so this is where the REFERENCES clauses
# are actually enforced.
ic = one("PRAGMA integrity_check")
check(ic == "ok", "integrity_check passes", ic)
con.execute("PRAGMA foreign_keys = ON")
fk_errs = con.execute("PRAGMA foreign_key_check").fetchall()
check(not fk_errs, "foreign_key_check passes",
      "" if not fk_errs else f"{len(fk_errs)} violation(s), first: "
                             f"table={fk_errs[0][0]} rowid={fk_errs[0][1]}")

# ---------------------------------------------------------- value domains
# The DDL carries CHECK constraints for the closed enums; re-checking here
# catches a database written by a producer that dropped them, and covers the
# NULL-required combinations CHECK cannot express. DOMAINS is also what
# --domain-coverage reads, so the set a fixture must reach and the set a row
# may hold are one table.
for tbl, col, values, nullable in DOMAINS:
    qs = ",".join("?" for _ in values)
    null = f'OR "{col}" IS NULL' if not nullable else ""
    bad = one(f'SELECT count(*) FROM "{tbl}" WHERE "{col}" NOT IN ({qs}) {null}',
              *values)
    check(not bad, f"{tbl}.{col} stays in its value domain",
          f"{bad} row(s) outside it")

for tbl, cols in (
    ("net", ("is_implicit",)),
    ("term_map", ("term_exact", "inner_exact", "map_exact")),
    ("net_conn", ("outer_exact", "term_exact", "map_exact")),
    ("stmt_target", ("is_exact",)),
    ("assign_operand", ("is_exact",)),
    ("expr_ref", ("is_exact",)),
    ("net_dep", ("src_exact", "tgt_exact", "map_exact")),
    ("hier_ref", ("is_exact",)),
):
    for col in cols:
        bad = one(f'SELECT count(*) FROM "{tbl}" WHERE "{col}" NOT IN (0,1) '
                  f'AND "{col}" IS NOT NULL')
        check(not bad, f"{tbl}.{col} is 0/1/NULL",
              f"{bad} value(s) outside it")

# ------------------------------------------------------- range discipline
# A range's lo and hi are both present or both absent, lo <= hi, and a range
# with real endpoints always says whether it is exact. NULL bits with
# exact=1 is the whole object; NULL bits with exact=0 is somewhere inside
# it; both spellings need the exact bit to be readable at all.
for tbl, lo, hi, exact in (
    ("term_map", "term_lo", "term_hi", "term_exact"),
    ("term_map", "inner_lo", "inner_hi", "inner_exact"),
    ("net_conn", "outer_lo", "outer_hi", "outer_exact"),
    ("net_conn", "term_lo", "term_hi", "term_exact"),
    ("stmt_target", "lo", "hi", "is_exact"),
    ("assign_operand", "lo", "hi", "is_exact"),
    ("expr_ref", "lo", "hi", "is_exact"),
    ("net_dep", "src_lo", "src_hi", "src_exact"),
    ("net_dep", "tgt_lo", "tgt_hi", "tgt_exact"),
    ("hier_ref", "lo", "hi", "is_exact"),
):
    bad = one(f'SELECT count(*) FROM "{tbl}" '
              f'WHERE ("{lo}" IS NULL) != ("{hi}" IS NULL)')
    check(not bad, f"{tbl}.{lo}/{hi} are both present or both absent",
          f"{bad} range(s) with one endpoint")
    bad = one(f'SELECT count(*) FROM "{tbl}" WHERE "{lo}" > "{hi}"')
    check(not bad, f"{tbl}.{lo} <= {hi}", f"{bad} range(s) inverted")
    bad = one(f'SELECT count(*) FROM "{tbl}" '
              f'WHERE "{lo}" IS NOT NULL AND "{exact}" IS NULL')
    check(not bad, f"{tbl} endpoints imply a readable {exact}",
          f"{bad} range(s) with endpoints and NULL {exact}")

# -------------------------------------------------- tree and the subtypes
check(one("SELECT count(*) FROM tree_node WHERE (parent_node_id IS NULL) != "
          "(node_kind IN ('root','package'))") == 0,
      "parentless nodes are exactly the roots and packages")
check(one("""
    SELECT count(*) FROM tree_node t
    WHERE (t.node_kind IN ('root','instance','unresolved','package'))
          != EXISTS (SELECT 1 FROM inst i WHERE i.id = t.id)""") == 0,
      "instance-like nodes have inst rows, others do not")
check(one("""
    SELECT count(*) FROM tree_node t
    WHERE (t.node_kind = 'primitive')
          != EXISTS (SELECT 1 FROM prim p WHERE p.id = t.id)""") == 0,
      "primitive nodes have primitive rows, others do not")
check(one("""
    SELECT count(*) FROM tree_node t JOIN inst i ON i.id = t.id
    WHERE (t.node_kind = 'unresolved') != (i.module_id IS NULL)""") == 0,
      "unresolved is exactly module_id NULL")
check(one("""
    SELECT count(*) FROM tree_node t JOIN inst i ON i.id = t.id
    WHERE t.node_kind = 'unresolved' AND (i.unresolved_def IS NULL
          OR i.param_signature IS NOT NULL)""") == 0,
      "an unresolved inst names its definition and no parameters")
check(one("SELECT count(*) FROM inst WHERE (parent_inst_id IS NULL) != "
          "(id IN (SELECT id FROM tree_node WHERE node_kind IN ('root','package')))") == 0,
      "the parentless inst rows are exactly the roots and packages")

# The hierarchy is encoded twice -- tree_node.parent_node_id and
# inst.parent_inst_id -- and the two must tell one story: parent_inst is the
# nearest inst on the tree_node ancestor chain. `nearest` walks up from each
# node, stopping at the first ancestor that IS an inst.
NEAREST = """
    WITH RECURSIVE up(node, cur) AS (
        SELECT id, parent_node_id FROM tree_node
        UNION ALL
        SELECT up.node, t.parent_node_id FROM up
        JOIN tree_node t ON t.id = up.cur
        WHERE NOT EXISTS (SELECT 1 FROM inst WHERE inst.id = up.cur)
    ),
    nearest(node, anc) AS (
        SELECT node, cur FROM up
        WHERE cur IS NULL OR EXISTS (SELECT 1 FROM inst WHERE inst.id = up.cur)
    )
"""
check(one(NEAREST + """
    SELECT count(*) FROM inst i JOIN nearest n ON n.node = i.id
    WHERE COALESCE(i.parent_inst_id, 0) != COALESCE(n.anc, 0)""") == 0,
      "inst.parent_inst_id is the nearest inst ancestor")
check(one(NEAREST + """
    SELECT count(*) FROM prim p JOIN nearest n ON n.node = p.id
    WHERE COALESCE(p.inst_id, 0) != COALESCE(n.anc, 0)""") == 0,
      "primitive.inst_id is the owning instance")

# A scope node is the instance itself or a generate level inside it; the
# owner of a scope is the scope when it is an inst, else its nearest inst.
SCOPE_OWNER = NEAREST + """
    , owner(node, inst) AS (
        SELECT t.id,
               CASE WHEN EXISTS (SELECT 1 FROM inst WHERE inst.id = t.id)
                    THEN t.id
                    ELSE (SELECT anc FROM nearest WHERE nearest.node = t.id) END
        FROM tree_node t
    )
"""
for tbl in ("net", "proc", "stmt"):
    check(one(SCOPE_OWNER + f"""
        SELECT count(*) FROM "{tbl}" x JOIN owner o ON o.node = x.scope_node_id
        WHERE o.inst != x.inst_id""") == 0,
          f"{tbl}.scope_node_id lies inside its own instance")
# A dot means a second segment -- unless the name is an ESCAPED identifier,
# which slang writes verbatim as `\name ` with no quoting, so `\u.1 ` is one
# segment containing a dot. Splitting a path on its last dot names that node
# `1`; the leaf has to come from the symbol, and this check must admit the
# escaped spelling or it forbids the correct answer.
check(one("""
    SELECT count(*) FROM tree_node
    WHERE instr(name, '.') > 0
      AND NOT (substr(name, 1, 1) = char(92) AND substr(name, -1, 1) = ' ')""") == 0,
      "every tree node name is a single path segment")
# A location is a file, a line and a column, and they come from one place or
# they are not a location. Lines and columns are 1-based, so 0 in either is not
# a position -- it is a call that failed and was stored anyway. A macro
# location is how that arrives: slang's getFileName and getLineNumber expand
# internally, getColumnNumber does not and returns 0 off a file location it
# was never given, leaving the row at the expansion site's file and line,
# column nothing.
for tbl in ("module", "inst", "prim", "net", "term", "proc", "stmt",
            "net_conn", "proc_event", "hier_ref"):
    check(one(f"""SELECT count(*) FROM "{tbl}"
                  WHERE file_id IS NOT NULL AND (line < 1 OR col < 1)""") == 0,
          f"{tbl} positions are 1-based where a file is named")
# Siblings sharing (parent, name) are exactly what duplicate_path_count
# admits to: the exporter counts every node after the first in a group,
# so the tree must show sum(n - 1) collisions -- no more, no fewer. A
# mismatch means the count and the tree disagree about how ambiguous a
# path lookup is.
check(one("""
    SELECT COALESCE(SUM(n - 1), 0) FROM (
        SELECT count(*) AS n FROM tree_node
        GROUP BY parent_node_id, name HAVING count(*) > 1)""") ==
      int(one("SELECT value FROM meta WHERE key='duplicate_path_count'")),
      "sibling name collisions match duplicate_path_count")

# ------------------------------------------------- parameter round-trip
# inst_param is param_signature made queryable, and must stay the SAME
# normalisation: reassembling each occurrence's pairs in ordinal order
# must reproduce the signature byte for byte -- and be absent exactly
# when the signature is. Reassembled in Python, where the order is
# guaranteed rather than an aggregate's accident.
sigs = dict(con.execute("SELECT id, COALESCE(param_signature, '') FROM inst"))
recon = {}
for iid, pname, pvalue in con.execute(
        "SELECT inst_id, name, value FROM inst_param ORDER BY inst_id, ordinal"):
    prev = recon.get(iid, "")
    recon[iid] = (prev + "," if prev else "") + f"{pname}={pvalue}"
bad_sigs = [iid for iid, sig in sigs.items() if recon.get(iid, "") != sig]
check(not bad_sigs, "inst_param reassembles every param_signature",
      f"{len(bad_sigs)} instance(s) disagree, first id "
      f"{bad_sigs[0] if bad_sigs else 0}")
check(one("""
    SELECT count(*) FROM inst_param p
    WHERE NOT EXISTS (SELECT 1 FROM inst i WHERE i.id = p.inst_id)""") == 0,
      "no parameter row floats free of an instance")

# ------------------------------------------------------------- ownership
check(one("""
    SELECT count(*) FROM term_map m
    JOIN term t ON t.id = m.term_id JOIN net n ON n.id = m.inner_net_id
    WHERE t.inst_id != n.inst_id""") == 0,
      "term_map stays inside one instance")
check(one("""
    SELECT count(*) FROM net_conn c
    JOIN term t ON t.id = c.term_id
    JOIN net n ON n.id = c.outer_net_id
    JOIN inst child ON child.id = t.inst_id
    WHERE n.inst_id != child.parent_inst_id""") == 0,
      "a connection's net belongs to the terminal's parent instance")
for tbl in ("stmt_target", "assign_operand", "expr_ref"):
    check(one(f"""
        SELECT count(*) FROM "{tbl}" x
        JOIN stmt s ON s.id = x.stmt_id JOIN net n ON n.id = x.net_id
        WHERE s.inst_id != n.inst_id""") == 0,
          f"{tbl} references nets of its statement's instance")
check(one("""
    SELECT count(*) FROM proc_event e
    JOIN proc p ON p.id = e.proc_id
    LEFT JOIN net n ON n.id = e.net_id
    LEFT JOIN stmt s ON s.id = e.stmt_id
    WHERE (n.id IS NOT NULL AND n.inst_id != p.inst_id)
       OR (s.id IS NOT NULL AND COALESCE(s.proc_id, 0) != e.proc_id)""") == 0,
      "proc_event stays inside its procedure")
# The ids are computed as base + index per instance, so the one error the
# REFERENCES clauses cannot see is a wrong base: an id that lands on a
# perfectly valid row of ANOTHER instance. These two joins are where that
# error becomes visible for the two per-instance references the block
# above does not already cover.
check(one("""
    SELECT count(*) FROM stmt s JOIN proc p ON p.id = s.proc_id
    WHERE p.inst_id != s.inst_id""") == 0,
      "a statement's procedure belongs to its own instance")
check(one("""
    SELECT count(*) FROM hier_ref h JOIN stmt s ON s.id = h.stmt_id
    WHERE s.inst_id != h.inst_id""") == 0,
      "a hier_ref's statement belongs to its own instance")

# ------------------------------------------------------------- ordinals
# Dense 0..n-1 per parent. The UNIQUE constraints stop duplicates on the
# statement children; nothing stops a list that starts past 0 or skips --
# which is what a producer indexing the wrong list looks like -- and the
# tables without a UNIQUE get their duplicate check here too.
ORDINALS = (("tree_node", "parent_node_id"), ("inst_param", "inst_id"),
            ("term", "inst_id"), ("term_map", "term_id"),
            ("net_conn", "term_id"), ("proc", "inst_id"),
            ("stmt", "inst_id"), ("stmt_target", "stmt_id"),
            ("assign_operand", "stmt_id"), ("expr_ref", "stmt_id"))
for tbl, parent in ORDINALS:
    check(one(f"""
        SELECT count(*) FROM (
          SELECT 1 FROM "{tbl}" GROUP BY "{parent}"
          HAVING min(ordinal) != 0
              OR max(ordinal) != count(*) - 1
              OR count(DISTINCT ordinal) != count(*))""") == 0,
          f"{tbl}.ordinal is dense per {parent}")
check(one("""
    SELECT count(*) FROM proc_event
    WHERE (event_kind = 'sensitivity') != (stmt_id IS NULL)""") == 0,
      "sensitivity events belong to the header, waits to a statement")

# ------------------------------------------------------- statement rules
check(one("""
    SELECT count(*) FROM stmt
    WHERE (stmt_kind = 'assignment') != (assign_kind IS NOT NULL)""") == 0,
      "assign_kind is set exactly on assignments")
# A release names what it lets go of and touches nothing else: at least
# one lvalue (a target row, or a hier_ref for a name outside the
# instance), no operands, and no dependency anywhere near it -- releasing
# is not driving, and a multiple-driver query must never see one.
check(one("""
    SELECT count(*) FROM stmt s WHERE s.stmt_kind='release'
      AND s.construct NOT IN ('release','deassign')""") == 0,
      "a release row says which spelling it was")
check(one("""
    SELECT count(*) FROM stmt s WHERE s.stmt_kind='release'
      AND NOT EXISTS (SELECT 1 FROM stmt_target t WHERE t.stmt_id = s.id)
      AND NOT EXISTS (SELECT 1 FROM hier_ref h WHERE h.stmt_id = s.id)""") == 0,
      "a release names what it lets go of")
check(one("""
    SELECT count(*) FROM stmt s WHERE s.stmt_kind='release'
      AND (EXISTS (SELECT 1 FROM assign_operand o WHERE o.stmt_id = s.id)
        OR EXISTS (SELECT 1 FROM net_dep d WHERE d.stmt_id = s.id))""") == 0,
      "and drives and reads nothing")
check(one("""
    SELECT count(*) FROM net_dep d
    JOIN stmt_target t ON t.id = d.stmt_target_id
    JOIN stmt s ON s.id = t.stmt_id
    WHERE s.stmt_kind='release'""") == 0,
      "no dependency borrows a release's target")

# ------------------------------------------------------------- branches
# The gating tree: what each kind carries, what must be NULL beside it, and
# that the chain is a proper nesting.
check(one("""
    SELECT count(*) FROM branch
    WHERE (branch_kind = 'if') != (sense IS NOT NULL)""") == 0,
      "sense is set exactly on if levels")
check(one("""
    SELECT count(*) FROM branch
    WHERE (branch_kind = 'case') != (case_kind IS NOT NULL)""") == 0,
      "case_kind is set exactly on case points")
check(one("""
    SELECT count(*) FROM branch
    WHERE check_kind IS NOT NULL AND branch_kind NOT IN ('if', 'case')""") == 0,
      "a unique/priority qualifier belongs to an if or a case point")
check(one("""
    SELECT count(*) FROM branch
    WHERE branch_kind != 'loop'
      AND (iter_net_id IS NOT NULL OR iter_first IS NOT NULL
        OR iter_step IS NOT NULL OR iter_count IS NOT NULL)""") == 0,
      "an iteration space belongs to a loop level")
# first and step describe the values the index takes and are published
# together: one without the other is nothing a consumer can substitute.
check(one("""
    SELECT count(*) FROM branch
    WHERE (iter_first IS NULL) != (iter_step IS NULL)""") == 0,
      "iter_first and iter_step are published together")
check(one("""
    SELECT count(*) FROM branch
    WHERE iter_first IS NOT NULL AND iter_count IS NULL""") == 0,
      "a published progression comes with its iteration count")
check(one("""
    SELECT count(*) FROM branch WHERE iter_count < 0""") == 0,
      "an iteration count is never negative")
# Labels hang on the arms that have them: a `default` matches no value, and
# a pattern-case item's patterns are not label values.
check(one("""
    SELECT count(*) FROM branch_label l JOIN branch b ON b.id = l.branch_id
    WHERE b.branch_kind != 'case_item'""") == 0,
      "only a case item carries labels")
check(one("""
    SELECT count(*) FROM branch_label l
    JOIN branch b ON b.id = l.branch_id
    JOIN branch p ON p.id = b.parent_branch_id
    WHERE p.case_kind = 'matches'""") == 0,
      "a pattern-case item carries no label values")
# The chain is a proper nesting: depth 1 at a root, one deeper per level,
# never leaving the instance. Strictly increasing depth makes it acyclic.
check(one("""
    SELECT count(*) FROM branch
    WHERE (parent_branch_id IS NULL) != (depth = 1)""") == 0,
      "an outermost branch level is exactly depth 1")
check(one("""
    SELECT count(*) FROM branch b JOIN branch p ON p.id = b.parent_branch_id
    WHERE b.depth != p.depth + 1 OR b.inst_id != p.inst_id""") == 0,
      "a nested branch level is one below its parent, same instance")
# A case arm is an arm OF something, and only of a case point.
check(one("""
    SELECT count(*) FROM branch b
    LEFT JOIN branch p ON p.id = b.parent_branch_id
    WHERE b.branch_kind IN ('case_item', 'case_default')
      AND (p.branch_kind IS NULL OR p.branch_kind != 'case')""") == 0,
      "a case arm hangs on a case point")
check(one("""
    SELECT count(*) FROM branch b
    WHERE b.branch_kind = 'case'
      AND NOT EXISTS (SELECT 1 FROM branch c
                      WHERE c.parent_branch_id = b.id)""") == 0,
      "a case point has at least one arm")
check(one("""
    SELECT count(*) FROM branch b JOIN net n ON n.id = b.iter_net_id
    WHERE n.inst_id != b.inst_id""") == 0,
      "a loop index is a net of the level's own instance")
# The whole point of the table: a statement and the levels gating it are in
# one instance, and every control read names the level that contributed it,
# which is on that statement's own chain.
check(one("""
    SELECT count(*) FROM stmt s JOIN branch b ON b.id = s.branch_id
    WHERE s.inst_id != b.inst_id""") == 0,
      "a statement's gating level is in its own instance")
check(one("""
    SELECT count(*) FROM expr_ref
    WHERE (role = 'control') != (branch_id IS NOT NULL)""") == 0,
      "branch_id is set on exactly the control reads")
check(one("""
    SELECT count(*) FROM expr_ref e JOIN stmt s ON s.id = e.stmt_id
    WHERE e.role = 'control' AND s.branch_id IS NULL""") == 0,
      "a statement with a control read names the level it sits in")
check(one("""
    WITH RECURSIVE chain(stmt_id, branch_id) AS (
        SELECT id, branch_id FROM stmt WHERE branch_id IS NOT NULL
      UNION ALL
        SELECT c.stmt_id, b.parent_branch_id
        FROM chain c JOIN branch b ON b.id = c.branch_id
        WHERE b.parent_branch_id IS NOT NULL)
    SELECT count(*) FROM expr_ref e
    WHERE e.role = 'control'
      AND NOT EXISTS (SELECT 1 FROM chain c
                      WHERE c.stmt_id = e.stmt_id
                        AND c.branch_id = e.branch_id)""") == 0,
      "a control read's level is on its statement's own chain")
check(one("""
    SELECT count(*) FROM hier_ref h
    WHERE h.branch_id IS NOT NULL AND h.access != 'read'""") == 0,
      "only a read leaves the instance as a branch condition")
check(one("""
    SELECT count(*) FROM hier_ref h JOIN branch b ON b.id = h.branch_id
    WHERE h.inst_id != b.inst_id""") == 0,
      "an outward condition's level is in its own instance")
# A loop index sources no dataflow: that is what the iteration space
# replaced. A `procedure` arc survives -- `t(i)` really does feed the formal,
# and dropping it would leave the formal with no driver at all.
check(one("""
    SELECT count(*) FROM net_dep d
    JOIN branch b ON b.iter_net_id = d.src_net_id
    JOIN net n ON n.id = d.src_net_id AND n.inst_id = b.inst_id
    WHERE d.dep_kind IN ('data', 'control')""") == 0,
      "a loop index sources no data or control dependency")
check(one("""
    SELECT count(*) FROM assign_operand o
    JOIN branch b ON b.iter_net_id = o.net_id
    JOIN net n ON n.id = o.net_id AND n.inst_id = b.inst_id""") == 0,
      "and is never an assignment operand")
# `ordinal` is the arm order under one case point, and nothing else has one.
check(one("""
    SELECT count(*) FROM branch
    WHERE (branch_kind IN ('case_item', 'case_default'))
       != (ordinal IS NOT NULL)""") == 0,
      "ordinal is set on exactly the case arms")
# Ordinal is the arm's WRITTEN position, so it is unique and increasing
# under one point but not contiguous: an arm whose body gates nothing has no
# row, and closing the gap would misstate which arms precede which.
check(one("""SELECT count(*) FROM branch WHERE ordinal < 0""") == 0,
      "an arm ordinal is never negative")
check(one("""
    SELECT count(*) FROM branch a JOIN branch b
      ON b.parent_branch_id = a.parent_branch_id
    WHERE a.ordinal < b.ordinal
      AND (a.line > b.line OR (a.line = b.line AND a.col > b.col))""") == 0,
      "arm ordinals run in source order under their point")
# A loop that provably never runs is dead code, and says so in the one
# column a dead-code filter reads.
check(one("""
    SELECT count(*) FROM branch
    WHERE iter_count = 0 AND static_taken IS NOT 0""") == 0,
      "a zero-trip loop level is marked unreachable")

# ------------------------------------------------------------ call sites
# A call site is a subroutine-body expansion, and the stmt/net_dep rows that
# name it must sit in the same instance -- a body walked at a call site is
# stamped into that occurrence, so its rows and the site share an inst.
check(one("""
    SELECT count(*) FROM stmt s JOIN call_site cs ON cs.id = s.call_site_id
    WHERE s.inst_id != cs.inst_id""") == 0,
      "a statement's call site is in its own instance")
check(one("""
    SELECT count(*) FROM net_dep d JOIN call_site cs ON cs.id = d.call_site_id
    JOIN stmt s ON s.id = d.stmt_id
    WHERE s.inst_id != cs.inst_id""") == 0,
      "a dependency's call site is in its statement's instance")
# A dependency that names a call site was made walking a subroutine body, so
# it names the statement it came from -- there is no call site without one.
check(one("""
    SELECT count(*) FROM net_dep
    WHERE call_site_id IS NOT NULL AND stmt_id IS NULL""") == 0,
      "a dependency in a call carries its statement")
# The parent chain is a proper call string: a nested call sits one level
# deeper than its parent and in the same instance, and an outermost call is
# depth 1. Depth strictly decreasing toward the parent makes it acyclic.
check(one("""
    SELECT count(*) FROM call_site cs
    WHERE (cs.parent_call_site_id IS NULL) != (cs.depth = 1)""") == 0,
      "an outermost call site is exactly depth 1")
check(one("""
    SELECT count(*) FROM call_site cs JOIN call_site p
      ON p.id = cs.parent_call_site_id
    WHERE cs.depth != p.depth + 1 OR cs.inst_id != p.inst_id""") == 0,
      "a nested call site is one level below its parent, same instance")
# The caller statement, when named, is a real statement of that instance.
check(one("""
    SELECT count(*) FROM call_site cs JOIN stmt s ON s.id = cs.caller_stmt_id
    WHERE s.inst_id != cs.inst_id""") == 0,
      "the caller statement belongs to the call site's instance")
# One direction only: a continuous assignment is never inside a procedure,
# but a procedure-less blocking/nonblocking row is legal -- a function body
# reached from an `assign` keeps its own `=`, and executes in no procedure.
check(one("""
    SELECT count(*) FROM stmt
    WHERE assign_kind = 'continuous' AND proc_id IS NOT NULL""") == 0,
      "a continuous assignment is never inside a procedure")
check(one("""
    SELECT count(*) FROM stmt
    WHERE proc_id IS NULL AND sequence IS NOT NULL""") == 0,
      "sequence never exists outside a procedure")
check(one("""
    SELECT count(*) FROM stmt
    WHERE sequence IS NULL AND proc_id IS NOT NULL
      AND stmt_kind != 'event_control'""") == 0,
      "inside a procedure only the header's event_control lacks a sequence")
check(one("""
    SELECT count(*) FROM expr_ref e JOIN stmt s ON s.id = e.stmt_id
    WHERE CASE e.role
        -- A condition gates whatever statement it encloses, including one
        -- that writes nothing this instance names.
        WHEN 'control'     THEN 0
        WHEN 'assertion'   THEN s.stmt_kind != 'assertion'
        WHEN 'wait'        THEN s.stmt_kind NOT IN ('wait', 'event_control')
        WHEN 'event'       THEN s.stmt_kind != 'event_control'
        WHEN 'system_task' THEN s.stmt_kind != 'system_task'
        WHEN 'call_argument' THEN s.stmt_kind NOT IN
            ('call', 'assignment', 'system_task')
        ELSE 1 END""") == 0,
      "expr_ref roles match their statement kinds")

# --------------------------------------------------- net_dep provenance
# Every dependency names where it came from, per kind, and the copies it
# carries of the operand/target facts agree with the rows it names. This is
# the declared redundancy that makes net_dep usable as the driver/load index.
# Per kind, and per END: a data/control end is either the local reference
# row or the resolved hierarchical one, exactly one of the two.
check(one("""
    SELECT count(*) FROM net_dep d
    WHERE CASE d.dep_kind
        WHEN 'data' THEN d.stmt_id IS NULL
             OR d.expr_ref_id IS NOT NULL OR d.prim_id IS NOT NULL
             OR (d.stmt_target_id IS NULL) = (d.tgt_hier_ref_id IS NULL)
             -- A NULL source net beside an operand row is a contradiction;
             -- beside a source reference it is an 'external' driver, and
             -- beside neither it is a constant. All three are legal shapes.
             OR (d.src_net_id IS NULL AND d.assign_operand_id IS NOT NULL)
             OR (d.src_net_id IS NOT NULL AND
                 (d.assign_operand_id IS NULL) = (d.src_hier_ref_id IS NULL))
        WHEN 'control' THEN d.stmt_id IS NULL
             -- A condition always has a source. Without this, a control row
             -- with src_net_id NULL is well formed in every other column and
             -- surfaces in v_driver as a CONSTANT tie-off on a gated signal.
             -- The source may be a reference that resolved to no net
             -- ('external'), but it must exist as a row of one of the two.
             OR (d.src_net_id IS NULL AND d.src_hier_ref_id IS NULL)
             OR d.assign_operand_id IS NOT NULL OR d.prim_id IS NOT NULL
             OR (d.expr_ref_id IS NULL) = (d.src_hier_ref_id IS NULL)
             OR (d.stmt_target_id IS NULL) = (d.tgt_hier_ref_id IS NULL)
             -- NULL-safe: `NULL != 0` is NULL, so the plain comparison read
             -- as "0 or NULL" and let an unset mapping through.
             OR d.map_exact IS NOT 0
        WHEN 'primitive' THEN d.prim_id IS NULL OR d.stmt_id IS NOT NULL
             OR d.stmt_target_id IS NOT NULL OR d.assign_operand_id IS NOT NULL
             OR d.expr_ref_id IS NOT NULL OR d.src_hier_ref_id IS NOT NULL
             OR d.tgt_hier_ref_id IS NOT NULL
        WHEN 'alias' THEN d.stmt_id IS NULL OR d.src_net_id IS NULL
             OR d.stmt_target_id IS NULL OR d.assign_operand_id IS NULL
             OR d.expr_ref_id IS NOT NULL OR d.prim_id IS NOT NULL
             OR d.src_hier_ref_id IS NOT NULL
             OR d.tgt_hier_ref_id IS NOT NULL
             OR d.map_exact IS NULL
        WHEN 'procedure' THEN d.prim_id IS NOT NULL
             OR d.assign_operand_id IS NOT NULL
             OR CASE WHEN d.src_net_id IS NULL AND d.src_hier_ref_id IS NULL
                  -- The write-back of a call whose formal is no net of this
                  -- instance -- a subroutine declared in a package, an
                  -- interface or $unit. There is no source to name, which is
                  -- the shape v_driver documents:
                  -- `driver_kind='procedure'` with a NULL driver net. The
                  -- stmt_target row beside it is what says the statement
                  -- writes the actual, and the two are one fact: neither may
                  -- stand without the other.
                  -- The target row is a position within a statement, so a
                  -- call in a CONDITION -- which belongs to no statement
                  -- this schema records -- has the dependency and no target
                  -- row. The two travel together wherever there is a
                  -- statement at all.
                  THEN (d.stmt_id IS NULL) IS NOT (d.stmt_target_id IS NULL)
                       OR d.expr_ref_id IS NOT NULL
                       OR d.tgt_hier_ref_id IS NOT NULL
                       OR d.map_exact IS NOT NULL
                  -- Every other procedure row names a source, and its
                  -- direction decides what may hang off it: the reading side
                  -- (actual -> formal) names where the actual came from and
                  -- writes nothing, the write-back side (formal -> actual)
                  -- carries the statement's target row. One row is one
                  -- direction, so it is never both -- and the reading side
                  -- names an argument reference or a resolved outward one,
                  -- never both of those either.
                  ELSE (d.expr_ref_id IS NOT NULL
                        AND d.stmt_target_id IS NOT NULL)
                       OR (d.expr_ref_id IS NOT NULL
                           AND d.src_hier_ref_id IS NOT NULL)
                END
        ELSE 1 END""") == 0,
      "net_dep provenance columns match dep_kind")

# The general form of what the write-back row above exists for: the two views
# that answer "what writes this net" must answer alike. v_stmt_target and
# v_net_attachment read stmt_target, v_driver reads net_dep -- and a call
# through an unstamped formal produces the first without the second unless
# something holds them together, leaving a package task that plainly writes
# its output actual reading as undriven.
check(one("""
    SELECT count(*) FROM v_stmt_target t
    WHERE t.target_kind = 'written_by'
      AND NOT EXISTS (SELECT 1 FROM v_driver d
                      WHERE d.signal_net_id = t.net_id
                        AND d.stmt_id = t.stmt_id)""") == 0,
      "every written_by target is a driver of its net from its own statement")
check(one("""
    SELECT count(*) FROM net_dep d JOIN hier_ref h ON h.id = d.src_hier_ref_id
    WHERE h.resolved_net_id IS NOT d.src_net_id""") == 0,
      "a hierarchical source copies its reference's resolution, NULL included")
# The reference a dependency crossed through is the one its own statement
# made. Sharing rows across statements -- a task body walked once per call
# site, a condition gating several statements -- left the second statement
# pointing at the first's reference, so "what does this statement read
# outside the instance" answered nothing.
check(one("""
    SELECT count(*) FROM net_dep d
    JOIN hier_ref h ON h.id IN (d.src_hier_ref_id, d.tgt_hier_ref_id)
    WHERE d.stmt_id IS NOT NULL AND h.stmt_id IS NOT NULL
      AND h.stmt_id != d.stmt_id""") == 0,
      "a dependency's reference belongs to its own statement")
check(one("""
    SELECT count(*) FROM net_dep d JOIN hier_ref h ON h.id = d.tgt_hier_ref_id
    WHERE h.resolved_net_id IS NULL OR h.resolved_net_id != d.tgt_net_id""") == 0,
      "a hierarchical target copies its reference's resolution")
check(one("""
    SELECT count(*) FROM net_dep d
    WHERE d.src_net_id IS NULL AND d.src_hier_ref_id IS NULL
      AND (d.src_lo IS NOT NULL
       OR d.src_exact IS NOT NULL OR d.map_exact IS NOT NULL)""") == 0,
      "a source-less dependency describes no source end")
check(one("""
    SELECT count(*) FROM net_dep d JOIN assign_operand o ON o.id = d.assign_operand_id
    WHERE o.net_id != d.src_net_id OR o.stmt_id != d.stmt_id""") == 0,
      "net_dep's operand copy agrees with the operand row")
check(one("""
    SELECT count(*) FROM net_dep d JOIN stmt_target t ON t.id = d.stmt_target_id
    WHERE t.net_id != d.tgt_net_id OR t.stmt_id != d.stmt_id""") == 0,
      "net_dep's target copy agrees with the target row")
check(one("""
    SELECT count(*) FROM net_dep d JOIN expr_ref e ON e.id = d.expr_ref_id
    WHERE e.net_id != d.src_net_id
       OR (d.stmt_id IS NOT NULL AND e.stmt_id != d.stmt_id)
       OR (d.dep_kind = 'control' AND e.role != 'control')
       OR (d.dep_kind = 'procedure' AND e.role != 'call_argument')""") == 0,
      "net_dep's expression reference agrees with the expr_ref row")
# Locality holds exactly where no end went through a hierarchical
# reference; a resolved cross-instance dependency is the point of the
# occurrence model, not a violation of it.
check(one("""
    SELECT count(*) FROM net_dep d
    JOIN net s ON s.id = d.src_net_id JOIN net t ON t.id = d.tgt_net_id
    JOIN stmt st ON st.id = d.stmt_id
    WHERE d.dep_kind IN ('data','control')
      AND d.src_hier_ref_id IS NULL AND d.tgt_hier_ref_id IS NULL
      AND (s.inst_id != st.inst_id OR t.inst_id != st.inst_id)""") == 0,
      "purely local dependencies stay inside one instance")
# An alias binds nets mutually: every pair it names appears in both
# directions, so each is the other's driver and the other's load. A single
# direction would answer one of those two questions and not the other.
check(one("""
    SELECT count(*) FROM net_dep d
    WHERE d.dep_kind = 'alias'
      AND NOT EXISTS (SELECT 1 FROM net_dep r
                      WHERE r.dep_kind = 'alias'
                        AND r.stmt_id = d.stmt_id
                        AND r.src_net_id = d.tgt_net_id
                        AND r.tgt_net_id = d.src_net_id)""") == 0,
      "every alias dependency has its opposite")
check(one("""
    SELECT count(*) FROM net_dep
    WHERE dep_kind = 'alias'
      AND src_net_id = tgt_net_id""") == 0,
      "an alias never binds a net to itself")
check(one("""
    SELECT count(*) FROM stmt
    WHERE stmt_kind = 'alias'
      AND (proc_id IS NOT NULL OR construct != 'alias')""") == 0,
      "an alias statement is module-level and names itself")
check(one("""
    SELECT count(*) FROM stmt_target a
    JOIN stmt s ON s.id = a.stmt_id
    WHERE s.stmt_kind != 'release'
      AND NOT EXISTS (SELECT 1 FROM net_dep d WHERE d.stmt_target_id = a.id)
      AND NOT EXISTS (SELECT 1 FROM hier_ref h
                      WHERE h.stmt_id = a.stmt_id AND h.access = 'read')""") == 0,
      "every assignment target has a dependency or an unresolved outward read")

# ------------------------------------------------------ windows and widths
# When a mapping claims to be one-to-one, the two sides must be the same
# width -- a bit-level trace follows it bit by bit, and unequal widths would
# take it off the end. Width of a side: its range when present, the object's
# width when the range means "whole".
check(one("""
    SELECT count(*) FROM term_map m
    JOIN term t ON t.id = m.term_id JOIN net n ON n.id = m.inner_net_id
    WHERE m.map_exact = 1
      AND COALESCE(m.term_hi - m.term_lo + 1, t.width) IS NOT NULL
      AND COALESCE(m.inner_hi - m.inner_lo + 1, n.width) IS NOT NULL
      AND COALESCE(m.term_hi - m.term_lo + 1, t.width)
          != COALESCE(m.inner_hi - m.inner_lo + 1, n.width)""") == 0,
      "an exact term_map maps equal widths")
# The same rule on dependencies, and it is the one that matters most: a
# one-to-one claim between ends of different widths is not coarse, it is
# impossible. `assign swap = {c[3:0], c[7:4]}` is the shape that produces it:
# two rows each claiming all eight bits of swap from a four-bit source --
# provably false, and every column in them well formed.
check(one("""
    SELECT count(*) FROM net_dep d
    JOIN net s ON s.id = d.src_net_id JOIN net t ON t.id = d.tgt_net_id
    WHERE d.map_exact = 1 AND d.src_exact = 1 AND d.tgt_exact = 1
      AND COALESCE(d.src_hi - d.src_lo + 1, s.width) IS NOT NULL
      AND COALESCE(d.tgt_hi - d.tgt_lo + 1, t.width) IS NOT NULL
      AND COALESCE(d.src_hi - d.src_lo + 1, s.width)
          != COALESCE(d.tgt_hi - d.tgt_lo + 1, t.width)""") == 0,
      "a one-to-one dependency maps equal widths")
check(one("""
    SELECT count(*) FROM net_conn c
    JOIN term t ON t.id = c.term_id LEFT JOIN net n ON n.id = c.outer_net_id
    WHERE c.map_exact = 1 AND c.term_exact = 1 AND c.outer_exact = 1
      AND COALESCE(c.term_hi - c.term_lo + 1, t.width) IS NOT NULL
      AND COALESCE(c.outer_hi - c.outer_lo + 1, n.width) IS NOT NULL
      AND COALESCE(c.term_hi - c.term_lo + 1, t.width)
          != COALESCE(c.outer_hi - c.outer_lo + 1, n.width)""") == 0,
      "an exact connection maps equal widths")
# A one-to-one window is exclusive: two segments both claiming a per-bit
# mapping cannot share formal bits. Expression operands are exempt by
# construction -- several reads legitimately feed one element's window, and
# their map_exact is 0.
for tbl in ("term_map", "net_conn"):
    check(one(f"""
        SELECT count(*) FROM "{tbl}" a JOIN "{tbl}" b
          ON a.term_id = b.term_id AND a.ordinal < b.ordinal
        WHERE a.map_exact = 1 AND b.map_exact = 1
          AND a.term_exact = 1 AND b.term_exact = 1
          AND a.term_lo IS NOT NULL AND b.term_lo IS NOT NULL
          AND a.term_lo <= b.term_hi AND b.term_lo <= a.term_hi""") == 0,
          f"one-to-one {tbl} windows on one terminal do not overlap")

# -------------------------------------------------------------- hier_ref
check(one("""
    SELECT count(*) FROM net_conn c JOIN hier_ref h ON h.id = c.outer_hier_ref_id
    WHERE h.access != 'connect'""") == 0,
      "a connection's outward tie is access='connect'")
# A signal connection to a resolved terminal always states its mapping; to
# an unresolved instance's terminal there is no formal end to correspond
# with, and NULL is the honest value.
check(one("""
    SELECT count(*) FROM net_conn c
    WHERE CASE c.conn_kind
        WHEN 'signal' THEN c.outer_net_id IS NULL OR (c.map_exact IS NULL
             AND NOT EXISTS (SELECT 1 FROM term t JOIN tree_node n
                             ON n.id = t.inst_id
                             WHERE t.id = c.term_id
                               AND n.node_kind = 'unresolved'))
        WHEN 'expression_operand' THEN
             (c.outer_net_id IS NULL) = (c.outer_hier_ref_id IS NULL)
             OR (c.outer_net_id IS NOT NULL AND c.map_exact != 0)
        WHEN 'constant' THEN c.outer_net_id IS NOT NULL OR c.map_exact IS NOT NULL
        WHEN 'unconnected' THEN c.outer_net_id IS NOT NULL OR c.map_exact IS NOT NULL
        WHEN 'interface' THEN c.outer_net_id IS NOT NULL
        WHEN 'external_reference' THEN c.outer_net_id IS NOT NULL
             OR c.outer_hier_ref_id IS NULL
             -- Like 'signal': a tie against a resolved formal states its
             -- mapping; only an unresolved instance's terminal has no
             -- formal end to correspond with.
             OR (c.map_exact IS NULL
                 AND NOT EXISTS (SELECT 1 FROM term t JOIN tree_node n
                                 ON n.id = t.inst_id
                                 WHERE t.id = c.term_id
                                   AND n.node_kind = 'unresolved'))
        ELSE 1 END""") == 0,
      "connection columns match conn_kind")

# ------------------------------------------------------------------ meta
# The doc states the required set as a rule -- the v_db_info columns minus
# `top` -- so it is derived here rather than hand-copied: a column added to
# the view then demands its meta key without this list needing to know.
required = [r[1] for r in con.execute("PRAGMA table_info(v_db_info)")
            if r[1] != "top"]
if not required:
    fatal("v_db_info is missing")
meta = dict(con.execute("SELECT key, value FROM meta"))
# Fatal rather than collected: every check below indexes these keys.
missing = [k for k in required if k not in meta or meta[k] is None]
if missing:
    fatal(f"meta lacks required key(s): {', '.join(missing)}")
check(meta["schema_version"] == SCHEMA_VERSION,
      f"schema_version is {SCHEMA_VERSION}", f"got {meta['schema_version']}")

# The version is stated in four independent places -- this constant, the
# exporter's SchemaVersion, the field reference's opening line and the
# README's measurements table -- and a bump that misses one leaves a consumer
# reading the wrong contract from a document that looks authoritative. Run
# only when the repository is beside this script, so verifying a database on
# its own is unaffected.
_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for _rel, _pattern in (("src/DesignDb.h", r"SchemaVersion\s*=\s*(\d+)"),
                       ("doc/designdb-schema.md", r"^Schema version (\d+)\."),
                       ("README.md", r"schema v(\d+)")):
    try:
        _text = open(os.path.join(_REPO, _rel), encoding="utf-8").read()
    except OSError:
        continue
    _m = re.search(_pattern, _text, re.M)
    check(_m is not None and _m.group(1) == SCHEMA_VERSION,
          f"{_rel} states schema version {SCHEMA_VERSION}",
          f"got {_m.group(1) if _m else 'no version line'}")
COUNTS = ("error_count", "unresolved_count", "empty_procedure_count",
          "duplicate_path_count", "recursion_count", "truncated_call_count",
          "checker_inst_count", "unanalysed_inst_count")
nonnumeric = [k for k in COUNTS if not meta[k].isdigit()]
if nonnumeric:
    fatal(f"meta count(s) not a number: {', '.join(nonnumeric)}")
status = meta["analysis_status"]
check(status in ("complete", "partial", "hierarchy_only"),
      "analysis_status is one of complete/partial/hierarchy_only",
      f"got {status!r}")
# Every cause of a non-`complete` status is a published count, so the
# implication runs both ways: `complete` beside a non-zero count is a malformed
# file, and a count that chose `partial` can be looked at rather than merely
# inferred. `unresolved_count` is not among them by design -- a black box is
# not an incompleteness of the export.
PARTIAL_CAUSES = ("error_count", "empty_procedure_count",
                  "duplicate_path_count", "truncated_call_count",
                  "unanalysed_inst_count")
explained = any(int(meta[k]) for k in PARTIAL_CAUSES)
check(not (status == "complete" and explained),
      "a complete status carries no count that would contradict it")
check(not (status == "partial" and not explained),
      "a partial status names the count that caused it")

info = con.execute("SELECT * FROM v_db_info").fetchone()
info_cols = [d[0] for d in con.execute("SELECT * FROM v_db_info LIMIT 0").description]
by = dict(zip(info_cols, info))
check(str(by["schema_version"]) == meta["schema_version"]
      and isinstance(by["schema_version"], int),
      "v_db_info.schema_version agrees with meta and is INTEGER",
      f"got {by['schema_version']!r}")
for k in COUNTS:
    check(by[k] == int(meta[k]) and isinstance(by[k], int),
          f"v_db_info.{k} agrees with meta and is INTEGER", f"got {by[k]!r}")

# --------------------------------------------------------- view contract
# The fifteen stable views: existence, exact columns in exact order, and row
# formulas. v_conn_arc is scaffolding, not contract, and is deliberately
# absent from this list.
VIEW_COLUMNS = {
    "v_db_info": [
        "schema_version", "tool", "tool_version", "slang_version",
        "producer_revision",
        "top", "analysis_status", "error_count", "unresolved_count",
        "empty_procedure_count", "duplicate_path_count", "recursion_count",
        "truncated_call_count", "checker_inst_count", "unanalysed_inst_count",
        "config_digest"],
    "v_tree_node": [
        "node_id", "parent_node_id", "node_name", "node_kind", "ordinal",
        "inst_id", "parent_inst_id", "module_id", "module_name", "def_kind",
        "param_signature", "def_name", "file_path", "src_path",
        "src_line", "src_col"],
    "v_net": [
        "net_id", "inst_id", "module_id", "module_name",
        "param_signature", "scope_node_id", "net_name", "decl_kind",
        "data_type", "width", "is_implicit", "file_path", "src_path",
        "src_line", "src_col"],
    "v_term": [
        "term_id", "inst_id", "module_id", "module_name",
        "term_name", "term_kind", "direction", "data_type", "width",
        "ordinal", "modport", "file_path", "src_path",
        "src_line", "src_col"],
    "v_term_map": [
        "term_map_id", "term_id", "term_inst_id", "term_name",
        "map_ordinal", "inner_net_id", "inner_net_name",
        "term_lo", "term_hi", "term_exact", "inner_lo", "inner_hi",
        "inner_exact", "map_exact"],
    "v_net_conn": [
        "conn_id", "outer_net_id", "outer_inst_id", "outer_net_name",
        "term_id", "term_inst_id", "term_name", "direction",
        "conn_kind", "ordinal", "outer_lo", "outer_hi", "outer_exact",
        "term_lo", "term_hi", "term_exact", "map_exact",
        "outer_intf_inst_id", "outer_hier_ref_id", "file_path", "src_path",
        "src_line", "src_col"],
    "v_net_dep": [
        "dep_id", "src_net_id", "src_inst_id", "src_name",
        "src_lo", "src_hi", "src_exact", "tgt_net_id",
        "tgt_inst_id", "tgt_name", "tgt_lo", "tgt_hi",
        "tgt_exact", "stmt_id", "assign_operand_id",
        "stmt_target_id", "expr_ref_id", "prim_id",
        "src_hier_ref_id", "tgt_hier_ref_id",
        "dep_kind", "map_exact", "call_site_id", "file_path", "src_path",
        "src_line", "src_col"],
    "v_driver": [
        "signal_net_id", "signal_inst_id", "signal_name", "signal_ref",
        "signal_lo", "signal_hi", "signal_exact",
        "driver_net_id", "driver_inst_id",
        "driver_name", "driver_ref", "driver_lo", "driver_hi", "driver_exact",
        "driver_kind", "dep_id", "conn_id", "stmt_id",
        "prim_id", "term_id", "map_exact", "call_site_id", "file_path",
        "src_path", "src_line", "src_col"],
    "v_load": [
        "signal_net_id", "signal_inst_id", "signal_name", "signal_ref",
        "signal_lo", "signal_hi", "signal_exact",
        "load_net_id", "load_inst_id",
        "load_name", "load_ref", "load_lo", "load_hi", "load_exact",
        "load_kind", "dep_id", "conn_id", "stmt_id", "proc_id",
        "term_id", "map_exact", "call_site_id", "file_path", "src_path",
        "src_line", "src_col"],
    "v_stmt": [
        "stmt_id", "inst_id", "module_id", "module_name",
        "scope_node_id", "proc_id", "ordinal", "sequence",
        "stmt_kind", "construct", "assign_kind", "delay",
        "dropped_operand_count", "call_site_id", "branch_id",
        "file_path", "src_path", "src_line", "src_col"],
    "v_stmt_target": [
        "target_id", "stmt_id", "ordinal", "net_id", "net_name",
        "target_kind", "lo", "hi", "is_exact", "call_site_id"],
    "v_stmt_operand": [
        "operand_id", "stmt_id", "ordinal", "net_id", "net_name",
        "lo", "hi", "is_exact", "call_site_id"],
    "v_net_attachment": [
        "net_id", "inst_id", "net_name", "attachment_kind",
        "lo", "hi", "is_exact", "stmt_id",
        "term_map_id", "conn_id", "stmt_target_id", "assign_operand_id",
        "expr_ref_id", "proc_event_id", "dep_id", "hier_ref_id"],
    "v_node_path": ["node_id", "node_path"],
    "v_proc_event": [
        "proc_event_id", "proc_id", "inst_id", "proc_kind", "stmt_id",
        "net_id", "net_name", "event_kind", "edge_kind",
        "file_path", "src_path", "src_line", "src_col"],
    "v_branch": [
        "branch_id", "inst_id", "module_id", "module_name",
        "parent_branch_id", "depth", "ordinal", "branch_kind", "sense",
        "case_kind",
        "check_kind", "static_taken", "iter_net_id", "iter_name",
        "iter_first", "iter_step", "iter_count", "labels",
        "file_path", "src_path", "src_line", "src_col"],
    "v_call_site": [
        "call_site_id", "inst_id", "module_id", "module_name",
        "caller_stmt_id", "parent_call_site_id", "subroutine_name", "depth"],
    "v_hier_ref": [
        "hier_ref_id", "inst_id", "module_id", "module_name", "stmt_id",
        "ref_path", "access", "resolved_inst_id", "resolved_net_id",
        "resolved_net_name", "lo", "hi", "is_exact",
        "file_path", "src_path", "src_line", "src_col"],
}
for view, want in VIEW_COLUMNS.items():
    row = con.execute(
        "SELECT count(*) FROM sqlite_master WHERE type='view' AND name=?",
        (view,)).fetchone()
    # Fatal: the reconciliations below query these views by name.
    if not row[0]:
        fatal(f"stable view missing: {view}")
    got = [d[0] for d in con.execute(f'SELECT * FROM "{view}" LIMIT 0').description]
    check(got == want, f"{view} keeps its contracted columns",
          "" if got == want else f"want {want}, got {got}")

# Fact views: one view row is one base row.
for view, base in (
    ("v_tree_node", "tree_node"), ("v_net", "net"), ("v_term", "term"),
    ("v_term_map", "term_map"), ("v_net_conn", "net_conn"),
    ("v_net_dep", "net_dep"), ("v_stmt", "stmt"),
    ("v_stmt_target", "stmt_target"),
    ("v_stmt_operand", "assign_operand"),
    ("v_branch", "branch"),
    ("v_call_site", "call_site"),
    ("v_hier_ref", "hier_ref"),
    ("v_proc_event", "proc_event"),
    ("v_node_path", "tree_node"),
):
    nv = one(f'SELECT count(*) FROM "{view}"')
    nb = one(f'SELECT count(*) FROM "{base}"')
    check(nv == nb, f"{view} has one row per {base} row",
          f"{nv} view rows, {nb} base rows")

# Composite views: the row count is the sum of the branches, each branch
# re-derived here from the base tables.
#
# The crossing branches (arcs_*) reconcile against the (net_conn, term_map)
# overlap COMPUTED HERE FROM THE BASE TABLES, not against v_conn_arc. Those
# two are the composition itself, so deriving the expected arc count from
# them would let a fault inside v_conn_arc inflate the view and the formula
# together and pass -- the seam the doc names. The predicate below is
# v_conn_arc's seg membership stated once (its two branches share it,
# differing only on outer_net_id NULL-ness), and it references only base
# tables. A separate check then pins v_conn_arc's own row count to it, so a
# join or filter regression inside the view fails even though v_driver and
# v_load would still self-reconcile.
SEG = """
    FROM net_conn c
    JOIN term t      ON t.id = c.term_id
    JOIN term_map mp ON mp.term_id = c.term_id
    LEFT JOIN hier_ref hr ON hr.id = c.outer_hier_ref_id
    WHERE c.conn_kind IN ('signal','expression_operand','constant',
                          'external_reference')
      AND (c.conn_kind != 'external_reference' OR hr.resolved_net_id IS NOT NULL)
      AND (c.conn_kind != 'expression_operand'
           OR c.outer_net_id IS NOT NULL OR hr.resolved_net_id IS NOT NULL)
      AND (c.term_lo IS NULL OR mp.term_hi IS NULL OR c.term_lo <= mp.term_hi)
      AND (mp.term_lo IS NULL OR c.term_hi IS NULL OR mp.term_lo <= c.term_hi)
"""
OUTER_PRESENT = "(c.outer_net_id IS NOT NULL OR hr.resolved_net_id IS NOT NULL)"
# The seam-closing check: the view emits exactly the base-table overlap, no
# row more or fewer.
check(one("SELECT count(*) FROM v_conn_arc") == one(f"SELECT count(*) {SEG}"),
      "v_conn_arc is exactly the net_conn/term_map overlap")
arcs_in = one(f"SELECT count(*) {SEG} AND t.direction IN ('input','inout','ref')")
arcs_out = one(f"""SELECT count(*) {SEG}
    AND t.direction IN ('output','inout','ref')
    AND c.conn_kind IN ('signal','external_reference') AND {OUTER_PRESENT}""")
term_in = one("""
    SELECT count(*) FROM term_map m JOIN term t ON t.id = m.term_id
    JOIN tree_node r ON r.id = t.inst_id AND r.node_kind = 'root'
    WHERE t.direction IN ('input','inout','ref')""")
term_out = one("""
    SELECT count(*) FROM term_map m JOIN term t ON t.id = m.term_id
    JOIN tree_node r ON r.id = t.inst_id AND r.node_kind = 'root'
    WHERE t.direction IN ('output','inout','ref')""")
n_driver = one("SELECT count(*) FROM v_driver")
want = one("SELECT count(*) FROM net_dep") + arcs_in + arcs_out + term_in
check(n_driver == want, "v_driver reconciles with its branch formula",
      f"{n_driver} rows, branch sum says {want}")

arcs_in_load = one(f"""SELECT count(*) {SEG}
    AND t.direction IN ('input','inout','ref') AND {OUTER_PRESENT}""")
n_load = one("SELECT count(*) FROM v_load")
want = (one("SELECT count(*) FROM net_dep WHERE src_net_id IS NOT NULL")
        + arcs_in_load + arcs_out + term_out
        + one("SELECT count(*) FROM proc_event WHERE net_id IS NOT NULL")
        + one("""SELECT count(*) FROM expr_ref e
                 WHERE e.role IN ('assertion','wait','event','system_task')
                    OR NOT EXISTS (SELECT 1 FROM net_dep d
                                   WHERE d.expr_ref_id = e.id)""")
        + one("""SELECT count(*) FROM assign_operand o
                 WHERE NOT EXISTS (SELECT 1 FROM net_dep d
                                   WHERE d.assign_operand_id = o.id)"""))
check(n_load == want, "v_load reconciles with its branch formula",
      f"{n_load} rows, branch sum says {want}")

n_att = one("SELECT count(*) FROM v_net_attachment")
want = (one("SELECT count(*) FROM term_map")
        + one("SELECT count(*) FROM net_conn WHERE outer_net_id IS NOT NULL")
        + one("""SELECT count(*) FROM net_conn c JOIN hier_ref h
                 ON h.id = c.outer_hier_ref_id
                 WHERE h.resolved_net_id IS NOT NULL""")
        + one("SELECT count(*) FROM stmt_target")
        + one("SELECT count(*) FROM assign_operand")
        + one("SELECT count(*) FROM expr_ref")
        + one("SELECT count(*) FROM proc_event WHERE net_id IS NOT NULL")
        + one("SELECT count(*) FROM net_dep")
        + one("SELECT count(*) FROM net_dep WHERE src_net_id IS NOT NULL")
        + one("SELECT count(*) FROM hier_ref WHERE resolved_net_id IS NOT NULL"))
check(n_att == want, "v_net_attachment reconciles with its branch formula",
      f"{n_att} rows, branch sum says {want}")
check(one("""
    SELECT count(*) FROM v_net_attachment
    WHERE attachment_kind NOT IN ('terminal_inside','actual_outside',
        'written_by','release_target','alias_binding','read_by','condition',
        'statement_read',
        'event','dep_in','dep_out','named_from_outside')""") == 0,
      "attachment_kind stays in its vocabulary")
# A release stores its lvalue as an stmt_target, and a written_by is derived
# from stmt_target -- so the two derivations meet on a statement that drives
# NOTHING of the net it names, which is the whole point of giving release its
# own statement kind. A written_by must never trace back to a release.
check(one("""
    SELECT count(*) FROM v_net_attachment a JOIN stmt s ON s.id = a.stmt_id
    WHERE a.attachment_kind = 'written_by' AND s.stmt_kind = 'release'""") == 0,
      "no release is mislabelled as a writer")
check(one("""
    SELECT count(*) FROM v_net_attachment a JOIN stmt s ON s.id = a.stmt_id
    WHERE a.attachment_kind = 'release_target'
      AND s.stmt_kind != 'release'""") == 0,
      "and release_target is exactly the releases")
# The same discipline for alias, and for the same reason: the storage is a
# stmt_target row but the statement writes nothing. v_driver excludes it by
# kind, so without this the two sides disagree about "who writes this net" --
# on nets no assignment in the design touches.
check(one("""
    SELECT count(*) FROM v_net_attachment a JOIN stmt s ON s.id = a.stmt_id
    WHERE a.attachment_kind = 'written_by' AND s.stmt_kind = 'alias'""") == 0,
      "no alias is mislabelled as a writer")
check(one("""
    SELECT count(*) FROM v_net_attachment a JOIN stmt s ON s.id = a.stmt_id
    WHERE a.attachment_kind = 'alias_binding'
      AND s.stmt_kind != 'alias'""") == 0,
      "and alias_binding is exactly the aliases")
# v_stmt_target draws the same three-way distinction from the statement
# side, with the same CASE. Two copies of one derivation drift unless
# something holds them together; this is that something.
check(one("""
    SELECT count(*) FROM v_stmt_target t
    JOIN v_net_attachment a ON a.stmt_target_id = t.target_id
    WHERE a.attachment_kind IS NOT t.target_kind""") == 0,
      "v_stmt_target.target_kind agrees with v_net_attachment row for row")
check(one("""
    SELECT count(*) FROM v_stmt_target
    WHERE target_kind NOT IN
        ('written_by', 'release_target', 'alias_binding')""") == 0,
      "and target_kind stays in that vocabulary")
# The statement layer's call_site_id is the statement's, on all three views.
# It is one column read three ways, so the only thing that can go wrong is
# the join that carries it -- which is what this holds.
for view in ("v_stmt", "v_stmt_target", "v_stmt_operand"):
    check(one(f"""
        SELECT count(*) FROM {view} v JOIN stmt s ON s.id = v.stmt_id
        WHERE v.call_site_id IS NOT s.call_site_id""") == 0,
          f"{view}.call_site_id is its statement's")
# The directional views take the same tag wherever the row has a statement to
# take it from. v_load's sensitivity, wait and statement arms read a base
# table directly, so a literal NULL there beside a tagged statement admits one
# call's read into every call's cone, for a consumer following the documented
# `= ? OR IS NULL` filter.
#
# A dependency is allowed one other answer: the summary arc of a CALL names
# the site that call opens, and the calling statement itself belongs to no
# site. Crossing and terminal rows have no statement and keep NULL.
for view in ("v_driver", "v_load", "v_net_dep"):
    check(one(f"""
        SELECT count(*) FROM {view} v JOIN stmt s ON s.id = v.stmt_id
        WHERE v.call_site_id IS NOT s.call_site_id
          AND NOT EXISTS (SELECT 1 FROM call_site c
                          WHERE c.id = v.call_site_id
                            AND c.caller_stmt_id = v.stmt_id)""") == 0,
          f"{view}.call_site_id is its statement's, or the call it opens")
# Exclusive arc, like net_dep: exactly one of the seven typed id columns is
# non-null per row, and it is the one attachment_kind names -- so a consumer
# joins the right base table without decoding the kind, and no row smuggles
# an id into a slot its kind does not own.
check(one("""
    SELECT count(*) FROM v_net_attachment
    WHERE (term_map_id IS NOT NULL) + (conn_id IS NOT NULL)
        + (stmt_target_id IS NOT NULL)
        + (assign_operand_id IS NOT NULL) + (expr_ref_id IS NOT NULL)
        + (proc_event_id IS NOT NULL) + (dep_id IS NOT NULL)
        + (hier_ref_id IS NOT NULL) != 1""") == 0,
      "every attachment names exactly one typed id")
check(one("""
    SELECT count(*) FROM v_net_attachment WHERE CASE attachment_kind
        WHEN 'terminal_inside'    THEN term_map_id IS NULL
        WHEN 'actual_outside'     THEN conn_id IS NULL
        WHEN 'written_by'         THEN stmt_target_id IS NULL
        WHEN 'release_target'     THEN stmt_target_id IS NULL
        WHEN 'alias_binding'      THEN stmt_target_id IS NULL
        WHEN 'read_by'            THEN assign_operand_id IS NULL
        WHEN 'condition'          THEN expr_ref_id IS NULL
        WHEN 'statement_read'     THEN expr_ref_id IS NULL
        WHEN 'event'              THEN proc_event_id IS NULL
        WHEN 'dep_in'             THEN dep_id IS NULL
        WHEN 'dep_out'            THEN dep_id IS NULL
        WHEN 'named_from_outside' THEN hier_ref_id IS NULL
        ELSE 1 END""") == 0,
      "and it is the typed id its attachment_kind implies")
# Each typed id resolves in its own table -- the join a consumer would make.
for col, tbl in (("term_map_id", "term_map"), ("conn_id", "net_conn"),
                 ("stmt_target_id", "stmt_target"),
                 ("assign_operand_id", "assign_operand"),
                 ("expr_ref_id", "expr_ref"),
                 ("proc_event_id", "proc_event"),
                 ("dep_id", "net_dep"), ("hier_ref_id", "hier_ref")):
    check(one(f"""SELECT count(*) FROM v_net_attachment a
        WHERE a.{col} IS NOT NULL
          AND NOT EXISTS (SELECT 1 FROM "{tbl}" b WHERE b.id = a.{col})""") == 0,
          f"v_net_attachment.{col} resolves in {tbl}")

# Which kinds may name no driver net, in two directions because they are two
# different statements. `constant`, `terminal`, `system_task` and `external`
# name no net BY DEFINITION -- there is no object on the far end. `primitive`
# and `procedure` merely MAY not: a `pullup` has no input terminal and a call
# into a subroutine declared outside this instance has no formal that is a net
# here, while the ordinary gate and the ordinary call both point at something.
# A kind that falls through to `constant` is a tie-off claim that inflates
# every multiple-driver count with a conflict that is not one.
check(one("""
    SELECT count(*) FROM v_driver
    WHERE driver_net_id IS NOT NULL
      AND driver_kind IN ('constant','terminal','system_task','external',
                          'trigger')""") == 0,
      "constant/terminal/system_task/external/trigger never name a driver net")
check(one("""
    SELECT count(*) FROM v_driver
    WHERE driver_net_id IS NULL
      AND driver_kind NOT IN ('constant','terminal','system_task','external',
                              'primitive','procedure','trigger')""") == 0,
      "only kinds that can lack a driver net do")
# An external driver is real but nameless HERE: no net row, so no name --
# yet unlike a constant it keeps its window, because the referenced
# object's bits exist. Its reference must have stayed unresolved (a
# resolved one would have carried the net id and the plain kind), and
# every unresolved-source dependency must surface as exactly one of them.
check(one("""
    SELECT count(*) FROM v_driver v
    JOIN net_dep d ON d.id = v.dep_id
    JOIN hier_ref h ON h.id = d.src_hier_ref_id
    WHERE v.driver_kind = 'external'
      AND (v.driver_name IS NOT NULL OR h.resolved_net_id IS NOT NULL)""") == 0,
      "an external driver names no net and its reference stayed unresolved")
check(one("SELECT count(*) FROM v_driver WHERE driver_kind='external'") ==
      one("""SELECT count(*) FROM net_dep
             WHERE src_net_id IS NULL AND src_hier_ref_id IS NOT NULL"""),
      "external drivers are exactly the unresolved-source dependencies")
# driver_ref/load_ref: the far end's spelling when it was named
# hierarchically. On a dependency row that is exactly the dependency's own
# reference on that end -- not "sometimes", or the column would be a hint
# rather than an answer, and a consumer would have to keep the join it was
# meant to retire.
check(one("""
    SELECT count(*) FROM v_driver v
    JOIN net_dep d ON d.id = v.dep_id
    WHERE (v.driver_ref IS NOT NULL) != (d.src_hier_ref_id IS NOT NULL)""") == 0,
      "a dependency's driver_ref is exactly its source reference")
check(one("""
    SELECT count(*) FROM v_load v
    JOIN net_dep d ON d.id = v.dep_id
    WHERE (v.load_ref IS NOT NULL) != (d.tgt_hier_ref_id IS NOT NULL)""") == 0,
      "and a dependency's load_ref is exactly its target reference")
check(one("""
    SELECT count(*) FROM v_driver v
    JOIN net_dep d ON d.id = v.dep_id
    JOIN v_hier_ref h ON h.hier_ref_id = d.src_hier_ref_id
    WHERE v.driver_ref IS NOT h.ref_path""") == 0,
      "and it carries that reference's path verbatim")
# The promise the column exists for: an external driver has no net row, so
# without this it names nothing at all.
check(one("""
    SELECT count(*) FROM v_driver
    WHERE driver_kind = 'external' AND driver_ref IS NULL""") == 0,
      "every external driver names the reference it reads")
check(one("""
    SELECT count(*) FROM v_load v
    JOIN net_dep d ON d.id = v.dep_id
    JOIN v_hier_ref h ON h.hier_ref_id = d.tgt_hier_ref_id
    WHERE v.load_ref IS NOT h.ref_path""") == 0,
      "and the load side carries its own verbatim")
# A crossing arcs both ways, and only ONE of the two rows has the outward
# tie as its far end -- so "is driver_ref set" must agree with "is the
# driver the net that reference resolved to", not merely with "does this
# connection have a reference". Stated the weak way, both columns could be
# placed on the wrong branch of the UNION and nothing outside a fixture
# would notice.
check(one("""
    SELECT count(*) FROM v_driver v
    JOIN net_conn c ON c.id = v.conn_id
    JOIN hier_ref h ON h.id = c.outer_hier_ref_id
    WHERE (v.driver_ref IS NOT NULL)
          IS NOT (v.driver_net_id IS NOT NULL
                  AND v.driver_net_id = h.resolved_net_id)""") == 0,
      "a crossing names a reference exactly when the reference IS its driver")
check(one("""
    SELECT count(*) FROM v_load v
    JOIN net_conn c ON c.id = v.conn_id
    JOIN hier_ref h ON h.id = c.outer_hier_ref_id
    WHERE (v.load_ref IS NOT NULL)
          IS NOT (v.load_net_id IS NOT NULL
                  AND v.load_net_id = h.resolved_net_id)""") == 0,
      "and on the load side exactly when it IS its load")
check(one("""
    SELECT count(*) FROM v_driver v
    JOIN net_conn c ON c.id = v.conn_id
    JOIN hier_ref h ON h.id = c.outer_hier_ref_id
    WHERE v.driver_ref IS NOT NULL AND v.driver_ref IS NOT h.path""") == 0,
      "and the spelling it carries is that tie's own")
# v_hier_ref resolves the four pointers the contract publishes at hier_ref.
# Its one derived column must not invent a name where the reference resolved
# to nothing, nor lose one where it did.
check(one("""
    SELECT count(*) FROM v_hier_ref
    WHERE (resolved_net_name IS NOT NULL) != (resolved_net_id IS NOT NULL)""") == 0,
      "v_hier_ref names a resolved net exactly when there is one")
# The rest is projection, and projection is where a pair of columns quietly
# swaps. Nothing else in this file reads the view's own range columns, so
# without this they are pinned by name and position and by nothing about
# their value -- and `resolved_inst_id` is no longer a column at all, so the
# view's derivation of it is pinned here too.
check(one("""
    SELECT count(*) FROM v_hier_ref v JOIN hier_ref h ON h.id = v.hier_ref_id
    WHERE v.inst_id IS NOT h.inst_id OR v.stmt_id IS NOT h.stmt_id
       OR v.ref_path IS NOT h.path OR v.access IS NOT h.access
       OR v.resolved_net_id IS NOT h.resolved_net_id
       OR v.resolved_inst_id IS NOT (SELECT n.inst_id FROM net n
                                     WHERE n.id = h.resolved_net_id)
       OR v.lo IS NOT h.lo OR v.hi IS NOT h.hi
       OR v.is_exact IS NOT h.is_exact
       OR v.src_line IS NOT h.line OR v.src_col IS NOT h.col""") == 0,
      "and every other v_hier_ref column is its base row's, unswapped")
check(one("""
    SELECT count(*) FROM v_driver
    WHERE driver_kind IN ('constant', 'terminal', 'system_task')
      AND (driver_name IS NOT NULL OR driver_ref IS NOT NULL
       OR driver_lo IS NOT NULL OR driver_hi IS NOT NULL
       OR driver_exact IS NOT NULL OR map_exact IS NOT NULL)""") == 0,
      "a driver-less row describes no driver end")
# The same discipline on the load side: without it the terminal branch is
# free to carry ranges for an end that does not exist, exactly the shape the
# null-source rule forbids.
check(one("""
    SELECT count(*) FROM v_load
    WHERE load_net_id IS NULL AND (load_name IS NOT NULL
       OR load_ref IS NOT NULL
       OR load_lo IS NOT NULL OR load_hi IS NOT NULL
       OR load_exact IS NOT NULL OR map_exact IS NOT NULL)""") == 0,
      "a target-less load describes no load end")
check(one("""
    SELECT count(*) FROM v_driver
    WHERE (driver_kind = 'terminal') != (term_id IS NOT NULL)""") == 0,
      "terminal drivers are exactly the rows naming a terminal")
check(one("""
    SELECT count(*) FROM v_load
    WHERE (load_kind IN ('sensitivity','wait','statement','terminal'))
          != (load_net_id IS NULL)""") == 0,
      "target-less loads are exactly sensitivity/wait/statement/terminal")
check(one("""
    SELECT count(*) FROM v_load
    WHERE (load_kind = 'terminal') != (term_id IS NOT NULL)""") == 0,
      "terminal loads are exactly the rows naming a terminal")
check(one("""
    SELECT count(*) FROM v_driver
    WHERE driver_kind NOT IN ('data','control','primitive','procedure',
                              'connection','connection_expression','constant',
                              'terminal','system_task','alias',
                              'external','trigger')""") == 0,
      "driver_kind stays in its vocabulary")
check(one("""
    SELECT count(*) FROM v_load
    WHERE load_kind NOT IN ('dataflow','connection','sensitivity','wait',
                            'statement','terminal','alias')""") == 0,
      "load_kind stays in its vocabulary")

# ------------------------------------------------- query plan discipline
# A point query on the driver/load views must seek, not scan. These are the
# two views a consumer walks a net at a time -- a fan-in cone is its own
# recursive query, by design -- so a plan that scans a base table turns one
# traced signal into one full scan per hop. Deriving the outer end of a
# crossing with COALESCE over two tables is how that happens: the value is
# attributable to neither, so neither table's index can be used, and tracing
# a clock takes minutes.
#
# The call-site columns are here for the same reason and are not about a net:
# the contract offers them as a LOOKUP -- "which rows belong to this call" --
# and a walk that does it per hop pays a scan per hop without an index behind
# each one.
for view, col in (("v_driver", "signal_net_id"), ("v_load", "signal_net_id"),
                  ("v_net_dep", "tgt_net_id"),
                  ("v_net_conn", "outer_net_id"),
                  ("v_net_attachment", "net_id"),
                  ("v_hier_ref", "resolved_net_id"),
                  ("v_proc_event", "net_id"),
                  ("v_net_dep", "call_site_id"),
                  ("v_stmt", "call_site_id"),
                  ("v_stmt_target", "call_site_id"),
                  ("v_stmt_operand", "call_site_id"),
                  # The branch chain is walked a level at a time, upward from
                  # a statement and downward to the statements a level gates,
                  # so both directions have to seek: a dead-code filter over
                  # a real design does one of each per level.
                  ("v_branch", "branch_id"),
                  ("v_branch", "parent_branch_id"),
                  ("v_stmt", "branch_id")):
    plan = con.execute(
        f"EXPLAIN QUERY PLAN SELECT * FROM {view} WHERE {col} = 1").fetchall()
    # A base table scanned is the fault. `SCAN (subquery-N)` is not one: it
    # reads back the rows a correlated co-routine produced, and whether THAT
    # seeks shows as its own line -- so a scan hidden inside one is still
    # caught by the table's own row.
    scanned = [r[3] for r in plan
               if r[3].startswith("SCAN ")
               and not r[3].startswith(f"SCAN {view}")
               and not r[3].startswith("SCAN (subquery")]
    check(not scanned, f"{view} seeks rather than scans for one {col}",
          "; ".join(scanned))

# Every `file` spelling resolves to the `src_file` it was read from. This is
# universal rather than mode-gated: CI passes no mode for examples/options,
# which is its only fixture whose `file` table has more than one row and so
# the only one where the join could come apart.
check(one("""
    SELECT count(*) FROM file
    WHERE src_file_id IS NULL""") == 0,
      "every file row joined to src_file")

# ------------------------------------------------------ mode-gated checks
if mode:
    check(meta.get("top") == MODES[mode], f"meta.top is {MODES[mode]}",
          f"got {meta.get('top')!r}")


if mode == "constructs":
    # One procedure takes several events on one net -- two waits on clk
    # here -- so the attachment names the event, not the procedure.
    check(one("""
        SELECT count(DISTINCT a.proc_event_id) FROM v_net_attachment a
        JOIN v_net n ON n.net_id = a.net_id
        WHERE a.attachment_kind = 'event' AND n.net_name = 'clk'""")
          == one("""
        SELECT count(*) FROM proc_event e JOIN net n ON n.id = e.net_id
        WHERE n.name = 'clk'"""),
          "each event on one net is its own attachment row")
    # The clock question in one row: which edge, in what kind of procedure.
    check(one("""
        SELECT count(*) FROM v_proc_event
        WHERE net_name = 'clk' AND edge_kind = 'posedge'
          AND proc_kind = 'always_ff' AND event_kind = 'sensitivity'""") >= 1,
          "v_proc_event answers edge and procedure kind together")

    # One pin takes several connection segments -- `.q({2{rep_r}})` tiles it
    # twice -- so the wiring attachment names the segment. A terminal id
    # cannot tell the two copies apart; the two rows must differ.
    check(one("""
        SELECT count(DISTINCT a.conn_id) FROM v_net_attachment a
        JOIN net_conn c ON c.id = a.conn_id
        JOIN v_net n ON n.net_id = a.net_id
        WHERE a.attachment_kind = 'actual_outside' AND n.net_name = 'rep_r'""") == 2,
          "each copy of a replicated actual is its own attachment row")
    # A system task's written argument is not a read of it, wherever the
    # call sits. From inside a condition the gating used to pick it up, so
    # the signal a plusarg fills gated whatever the branch wrote.
    check(one("""SELECT count(*) FROM v_driver
                 WHERE driver_name='seed' AND driver_kind='control'""") == 0,
          "a plusarg's destination does not gate the branch")
    check(one("""SELECT count(*) FROM v_load WHERE signal_name='seed'""") == 0,
          "and is not read by it")

    # force/release: the force is a blocking assignment whose construct
    # word marks the hijack, the release is its own statement kind naming
    # the signal it lets go of -- and neither pollutes the driver count:
    # the force drives (as the constant it assigns), the release never.
    check(one("""
        SELECT count(*) FROM v_stmt s
        JOIN v_stmt_target t ON t.stmt_id = s.stmt_id
        WHERE s.stmt_kind='assignment' AND s.construct='force'
          AND s.assign_kind='blocking' AND t.net_name='stim'""") == 1,
          "the force is marked as one, on the signal it hijacks")
    check(one("""
        SELECT count(*) FROM v_stmt s
        JOIN v_stmt_target t ON t.stmt_id = s.stmt_id
        WHERE s.stmt_kind='release' AND s.construct='release'
          AND t.net_name='stim'""") == 1,
          "and the release says where the hijack ends")
    # v_net_attachment must show the release as its own kind, never as a
    # writer: stim's attachments include a release_target and no written_by
    # from the release statement.
    check(one("""
        SELECT count(*) FROM v_net_attachment a JOIN v_net n ON n.net_id = a.net_id
        WHERE n.net_name='stim' AND a.attachment_kind='release_target'""") == 1,
          "the release hangs off stim as a release_target, not a writer")
    # Self-feedback survives, and arithmetic is range-level: cnt <= cnt + 1.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='cnt' AND tgt_name='cnt'
          AND src_net_id = tgt_net_id AND map_exact = 0""") >= 1,
          "the self-feedback dependency (cnt -> cnt), range-level")
    # Gates: primitive nodes with their rows, and the sr chain bit by bit.
    check(one("SELECT count(*) FROM prim WHERE prim_kind='gate'") >= 6,
          "gate primitives recorded as primitive nodes")
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE dep_kind='primitive'""") >= 8, "primitive dependencies")
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE dep_kind='primitive' AND src_name='sr' AND
              tgt_name='sr' AND src_lo=0 AND src_hi=0 AND
              tgt_lo=1 AND tgt_hi=1 AND map_exact=1""") == 1,
          "a gate driving one bit of a net from another, bit-exact")
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE dep_kind='primitive' AND src_net_id IS NULL""") >= 1,
          "the pullup's null-source dependency")
    # Net initialisers are continuous assignments.
    check(one("""
        SELECT count(*) FROM v_stmt
        WHERE assign_kind='continuous' AND proc_id IS NULL""") >= 2,
          "net initialisers are procedure-less continuous assignments")
    check(one("""
        SELECT count(*) FROM stmt_target a JOIN net n ON n.id=a.net_id
        WHERE n.name='w'""") >= 1, "the net initialiser's target (w)")
    # The call chain: d -> bump.v at the call, bump.v -> q in the body.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE dep_kind='procedure' AND src_name='d'
          AND tgt_name='bump.v'""") == 1,
          "the call's actual bound to the formal (d -> bump.v)")
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='bump.v' AND tgt_name='q'
          AND dep_kind='data'""") == 1,
          "the task body's write, the other half of that chain")
    check(one("""
        SELECT count(*) FROM net WHERE name='bump.v'""") >= 1,
          "the subroutine formal is a net row")
    # System tasks and waits read; their reads are expr_refs by role.
    check(one("""
        SELECT count(*) FROM expr_ref e JOIN stmt s ON s.id=e.stmt_id
        WHERE e.role='system_task' AND s.construct='$display'""") >= 1,
          "the system task argument's read")
    check(one("""
        SELECT count(*) FROM expr_ref WHERE role='wait'""") >= 1,
          "the wait condition's read")
    check(one("""
        SELECT count(*) FROM stmt WHERE stmt_kind='event_control'""") >= 2,
          "statement-level event controls are statements")
    # The downward XMR resolves to the child's real net -- the fact the
    # instance-level model exists to state.
    check(one("""
        SELECT count(*) FROM hier_ref h
        JOIN net n ON n.id = h.resolved_net_id
        JOIN tree_node t ON t.id = n.inst_id
        WHERE h.path='u_cnt.cnt' AND n.name='cnt' AND t.name='u_cnt'""") >= 1,
          "the downward XMR resolves to the child's net")
    check(one("""
        SELECT count(*) FROM hier_ref h JOIN stmt s ON s.id = h.stmt_id
        JOIN file f ON f.id = s.file_id
        WHERE f.path LIKE '%seq.svh'""") >= 1,
          "the included XMR, attributed to seq.svh")
    # Port windows: the part-select connection and the replication.
    check(one("""
        SELECT count(*) FROM v_net_conn
        WHERE outer_net_name='stim' AND outer_lo=0 AND outer_hi=3
          AND outer_exact=1""") >= 1,
          "the part-select port connection (.idx(stim[3:0]))")
    check(one("""
        SELECT count(*) FROM v_net_conn c1
        JOIN v_net_conn c2 ON c1.term_id = c2.term_id
          AND c1.ordinal < c2.ordinal
        WHERE c1.outer_net_name='rep_r' AND c2.outer_net_name='rep_r'
          AND c1.term_lo IS NOT NULL AND c2.term_lo IS NOT NULL
          AND c1.map_exact=1 AND c2.map_exact=1""") >= 1,
          "a replicated connection keeps one exact segment per copy")
    check(one("""
        SELECT count(*) FROM v_net_conn
        WHERE conn_kind='expression_operand'""") >= 1,
          "an expression-operand connection")
    check(one("""
        SELECT count(*) FROM v_net_conn
        WHERE conn_kind='constant'""") >= 1, "a constant tie-off")
    # Generate levels are their own nodes; instances inside them resolve.
    check(one("""
        SELECT count(*) FROM tree_node WHERE node_kind='generate'""") >= 2,
          "generate levels as their own nodes")
    check(one("""
        SELECT count(*) FROM tree_node g JOIN tree_node c
          ON c.parent_node_id = g.id
        WHERE g.node_kind='generate' AND c.node_kind='instance'""") >= 2,
          "module instances under generate levels")
    # Two parameterisations of one module stay two signatures.
    check(one("""
        SELECT count(DISTINCT i.param_signature) FROM inst i
        JOIN module m ON m.id = i.module_id WHERE m.name='scaled'""") == 2,
          "a module's two parameterisations keep distinct signatures")
    check(one("SELECT count(*) FROM module WHERE name='scaled'") == 1,
          "one definition row however many parameterisations")
    # Concatenated assignment: one statement, two targets, no crossing.
    pair = con.execute("""
        SELECT s.id FROM stmt s
        WHERE (SELECT count(*) FROM stmt_target a WHERE a.stmt_id=s.id) = 2
        LIMIT 1""").fetchone()
    check(pair is not None, "a concatenated write is one statement, two targets")
    sid = pair[0]
    check(one("""
        SELECT count(*) FROM net_dep d
        WHERE d.stmt_id=? AND d.dep_kind='data'""", sid) ==
          one("""SELECT count(*) FROM assign_operand o WHERE o.stmt_id=?""", sid),
          "the concatenation pairs halves, it does not cross them")
    # Dynamic select: an upper bound, not a guess (`assign q = bus[i]`).
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='bus' AND src_exact=0""") >= 1,
          "a dynamic select's read is an upper bound")
    # Dropped operands are counted.
    check(one("""
        SELECT count(*) FROM stmt WHERE dropped_operand_count > 0""") >= 1,
          "dropped constant operands are counted")
    # The crossing: the child counter's clk is driven by the parent's.
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE driver_kind='connection' AND signal_name='clk'
          AND signal_inst_id != driver_inst_id""") >= 1,
          "v_driver crosses the boundary for the child's clk")
    check(one("""
        SELECT count(*) FROM v_driver WHERE driver_kind='constant'""") >= 1,
          "v_driver keeps the null-driver row")
    check(one("""
        SELECT count(*) FROM v_driver WHERE driver_kind='control'""") >= 1,
          "v_driver keeps the control dependency, marked by kind")
    check(one("""
        SELECT count(*) FROM v_load WHERE load_kind='sensitivity'""") >= 2,
          "sensitivity reads are loads")
    check(one("""
        SELECT count(*) FROM v_load WHERE load_kind='wait'""") >= 1,
          "a wait is a load, distinct from sensitivity")
    check(one("""
        SELECT count(*) FROM v_load WHERE load_kind='connection'""") >= 2,
          "connections are loads of the nets they sample")
    check(one("""
        SELECT count(*) FROM v_tree_node WHERE node_kind='root'""") == 1,
          "v_tree_node has exactly one root")
    # Delays are normalised statement text, never a number.
    check(one("""
        SELECT count(*) FROM stmt WHERE delay='#3'
          AND assign_kind='continuous'""") == 1,
          "a delayed continuous assign keeps its delay text")
    check(one("""
        SELECT count(*) FROM stmt WHERE delay='#2'
          AND assign_kind='blocking'""") == 1,
          "an intra-assignment delay belongs to its own statement")
    # The undeclared left-hand side is a real net, marked implicit.
    check(one("""
        SELECT count(*) FROM net WHERE name='dly_w' AND is_implicit=1
          AND decl_kind='wire'""") == 1,
          "an implicit net is a row with is_implicit set")
    # One pair, two statements, two dependencies -- never folded.
    check(one("""
        SELECT count(DISTINCT d.stmt_id) FROM net_dep d
        JOIN net s ON s.id=d.src_net_id JOIN net t ON t.id=d.tgt_net_id
        WHERE s.name='a' AND t.name='r2' AND d.dep_kind='data'""") == 2,
          "the same pair from two statements stays two dependencies")

if mode == "modport":
    # An explicit modport port (`output .d(data)`) resolves past the rename
    # to the net behind it, exactly as a named port does -- and the renamed
    # net is driven through it.
    check(one("""
        SELECT count(*) FROM hier_ref h JOIN net n ON n.id = h.resolved_net_id
        WHERE h.path = 'p.d' AND h.access = 'write' AND n.name = 'data'""") == 1,
          "an explicit modport port resolves to the net it renames")
    check(one("""
        SELECT count(*) FROM v_driver v JOIN net n ON n.id = v.signal_net_id
        WHERE n.name = 'data' AND v.driver_kind = 'constant'
          AND v.signal_inst_id = (SELECT n.inst_id FROM hier_ref h
                                  JOIN net n ON n.id = h.resolved_net_id
                                  WHERE h.path = 'p.d')""") == 1,
          "and the renamed net is driven through it")
    # The select form keeps the port's own geometry, which is not the
    # net's: it stays unresolved rather than claiming bits of `data` the
    # reference does not touch.
    check(one("""
        SELECT count(*) FROM hier_ref
        WHERE path = 'p.n' AND resolved_net_id IS NULL""") == 1,
          "a selected connection stays honestly unresolved")

if mode == "interfaces":
    check(one("""
        SELECT count(*) FROM term WHERE term_kind='interface'""") >= 3,
          "interface terminals")
    # An interface array as the module's own port binds an element per
    # segment of one terminal, and the member references through it resolve
    # per element -- both were a single row naming no instance.
    for ordinal, inst in ((0, "barr[0]"), (1, "barr[1]")):
        check(one("""
            SELECT count(*) FROM net_conn c JOIN term tm ON tm.id = c.term_id
            JOIN tree_node t ON t.id = c.outer_intf_inst_id
            WHERE tm.name = 'bus_arr' AND c.ordinal = ? AND t.name = ?""",
                  ordinal, inst) == 1,
              f"the array port's segment {ordinal} binds {inst}")
    check(one("""
        SELECT count(*) FROM hier_ref h JOIN net n ON n.id = h.resolved_net_id
        JOIN tree_node t ON t.id = n.inst_id
        WHERE h.path = 'bus_arr[0].vld' AND t.name = 'barr[0]'
          AND n.name = 'vld'""") == 1,
          "and a member reference through it lands on that element")
    check(one("""
        SELECT count(*) FROM v_driver v JOIN tree_node t ON t.id = v.signal_inst_id
        WHERE t.name = 'barr[1]' AND v.signal_name = 'data'""") == 1,
          "so the interface net behind it is driven, not silent")
    # Forwarded through an intermediate module's own array port, and fed by
    # a slice of it: segment 0 of the child reads segment 2 of the parent, so
    # a connection that assumed the two ordinals agree bound the wrong
    # element -- and before that, nothing at all.
    for ordinal, inst in ((0, "bwide[2]"), (1, "bwide[3]")):
        check(one("""
            SELECT count(*) FROM net_conn c JOIN term tm ON tm.id = c.term_id
            JOIN tree_node t ON t.id = c.outer_intf_inst_id
            JOIN v_node_path p ON p.node_id = tm.inst_id
            WHERE tm.name = 'bus_arr' AND c.ordinal = ? AND t.name = ?
              AND p.node_path = 'interfaces.u_relay_arr.u_far'""",
                  ordinal, inst) == 1,
              f"a forwarded array's segment {ordinal} reads {inst}")
    check(one("""
        SELECT count(*) FROM v_driver v JOIN tree_node t ON t.id = v.signal_inst_id
        WHERE t.name = 'bwide[2]' AND v.signal_name = 'vld'""") == 1,
          "so the interface net behind the forwarded port is driven")
    # The same template, two occurrences, two arrays: the route is the
    # terminal and its segment, replayed per occurrence, never a stored id.
    check(one("""
        SELECT count(DISTINCT t.name) FROM net_conn c
        JOIN term tm ON tm.id = c.term_id
        JOIN tree_node t ON t.id = c.outer_intf_inst_id
        WHERE tm.name = 'bus_arr'""") == 4,
          "and the shared template resolves to four distinct elements")

    # Two dimensions: the segments are the LEAVES in declaration order. An
    # element of the outer array is an array, not an instance, so a walk one
    # level deep binds none of them.
    for ordinal, inst in ((0, "bgrid[0][0]"), (1, "bgrid[0][1]"),
                          (2, "bgrid[1][0]"), (3, "bgrid[1][1]")):
        check(one("""
            SELECT count(*) FROM net_conn c JOIN term tm ON tm.id = c.term_id
            JOIN tree_node t ON t.id = c.outer_intf_inst_id
            WHERE tm.name = 'grid' AND c.ordinal = ? AND t.name = ?""",
                  ordinal, inst) == 1,
              f"the two-dimensional port's segment {ordinal} binds {inst}")
    check(one("""
        SELECT count(*) FROM hier_ref h JOIN net n ON n.id = h.resolved_net_id
        JOIN tree_node t ON t.id = n.inst_id
        WHERE h.path = 'grid[1][0].data' AND t.name = 'bgrid[1][0]'
          AND n.name = 'data'""") == 1,
          "and a reference through it agrees with the connection side")

    # A task declared in the interface, called through a port: its body is
    # walked in the CALLER's template, so its bare names belong to a body
    # the caller cannot place. They resolve through the bound terminal, and
    # the interface's own nets carry the dataflow -- the write cross-instance
    # from the statement that made the call.
    for path, access, net in (("data", "write", "data"), ("vld", "read", "vld")):
        check(one("""
            SELECT count(*) FROM hier_ref h
            JOIN net n ON n.id = h.resolved_net_id
            JOIN tree_node t ON t.id = n.inst_id
            WHERE h.path = ? AND h.access = ? AND n.name = ?
              AND t.name = 'bus3'""", path, access, net) == 1,
              f"the interface task's {path} resolves to the bound instance")
    check(one("""
        SELECT count(*) FROM v_driver v
        JOIN tree_node t ON t.id = v.signal_inst_id
        WHERE t.name = 'bus3' AND v.signal_name = 'data'
          AND v.driver_name = 'vld' AND v.driver_kind = 'data'""") == 1,
          "and the interface's own net carries the task's dataflow")
    # The formal is no net of the interface -- nothing walks that body's
    # subroutines -- so it stays wholly unresolved rather than naming an
    # instance it cannot name a net in.
    check(one("""
        SELECT count(*) FROM hier_ref
        WHERE path = 'x' AND resolved_net_id IS NULL""") >= 1,
          "while its formal does not resolve at all")
    # Two terminals of one module reaching one interface: the call does not
    # say which port it went through, and the occurrence that binds them
    # apart would take the write to the wrong instance. Unresolved from
    # BOTH occurrences -- a missing answer, never a wrong one.
    check(one("""
        SELECT count(*) FROM hier_ref h JOIN inst i ON i.id = h.inst_id
        JOIN module m ON m.id = i.module_id
        WHERE m.name = 'stamp_pair' AND h.path = 'data'
          AND h.resolved_net_id IS NOT NULL""") == 0,
          "an ambiguous interface binding resolves to nothing at all")
    check(one("""
        SELECT count(*) FROM hier_ref h JOIN inst i ON i.id = h.inst_id
        JOIN module m ON m.id = i.module_id
        WHERE m.name = 'stamp_pair' AND h.path = 'data'""") == 2,
          "and both occurrences still record the reference")
    check(one("""
        SELECT count(*) FROM term
        WHERE term_kind='interface' AND modport IS NOT NULL""") >= 2,
          "the binding's declared modport")
    check(one("""
        SELECT count(*) FROM net_conn
        WHERE conn_kind='interface' AND outer_intf_inst_id IS NOT NULL""") >= 3,
          "interface bindings name their interface instance")
    check(one("""
        SELECT count(*) FROM net_conn c
        JOIN inst i ON i.id = c.outer_intf_inst_id
        JOIN module m ON m.id = i.module_id
        WHERE c.conn_kind='interface' AND m.def_kind='interface'""")
          == one("""SELECT count(*) FROM net_conn
                    WHERE conn_kind='interface'
                      AND outer_intf_inst_id IS NOT NULL"""),
          "every named interface binding points at an interface instance")
    # The pass-through: a grandchild's binding resolves to the same
    # top-level interface instance the parent was handed.
    check(one("""
        SELECT count(DISTINCT c.outer_intf_inst_id) FROM net_conn c
        JOIN term t ON t.id = c.term_id
        JOIN inst child ON child.id = t.inst_id
        JOIN inst parent ON parent.id = child.parent_inst_id
        WHERE c.conn_kind='interface'
          AND parent.parent_inst_id IS NOT NULL""") >= 1,
          "a pass-through binding resolves to the real instance")
    # Members referenced through the interface resolve to its nets.
    check(one("""
        SELECT count(*) FROM hier_ref h JOIN net n ON n.id=h.resolved_net_id
        WHERE h.access='write' AND n.name='vld'""") >= 1,
          "an interface member write resolves to the interface's net")
    check(one("""
        SELECT count(*) FROM hier_ref h JOIN net n ON n.id=h.resolved_net_id
        WHERE h.access='read' AND n.name IN ('vld','data')""") >= 2,
          "interface member reads resolve to the interface's nets")
    # An outward write and the reads that fed it share a statement -- and
    # driver_pair's two same-line statements stay two statements, each
    # pairing exactly its own operand rather than both.
    check(one("""
        SELECT count(DISTINCT w.stmt_id) FROM hier_ref w
        JOIN assign_operand o ON o.stmt_id = w.stmt_id
        WHERE w.access='write'""") >= 2,
          "each outward write pairs with the read that fed it, per statement")
    # A target fed only from outside is driven by what actually feeds it,
    # across the boundary -- not by a fabricated constant. `assign seen =
    # bus.vld && bus.data[0]` reaches the interface instance's own nets.
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name='seen' AND driver_kind='data'
          AND driver_inst_id != signal_inst_id
          AND driver_name IN ('vld','data')""") == 2,
          "a target fed from outside is driven across the boundary")
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name='seen' AND driver_kind='constant'""") == 0,
          "and is not reported as constant-driven")
    # The interface's own nets have real drivers and loads, which is what
    # the resolved references buy: a modport write reaches the net.
    check(one("""
        SELECT count(*) FROM v_driver d
        JOIN tree_node n ON n.id = d.signal_inst_id
        WHERE d.signal_name='vld' AND d.driver_kind='data'
          AND d.driver_inst_id != d.signal_inst_id""") >= 1,
          "an interface member is driven from the module that writes it")
    check(one("""
        SELECT count(*) FROM v_load
        WHERE signal_name='data' AND load_kind='dataflow'
          AND load_inst_id != signal_inst_id""") >= 1,
          "an interface member is read by the module that samples it")
    check(one("""
        SELECT count(*) FROM net_conn
        WHERE conn_kind='external_reference'
          AND outer_hier_ref_id IS NOT NULL""") >= 1,
          "a port tied outside the module keeps its row")
    check(one("""
        SELECT count(*) FROM hier_ref
        WHERE instr(path, ' ') > 0 OR instr(path, '/*') > 0""") == 0,
          "no reference path carries a space or a comment")

if mode == "assertions":
    # A checker produces no rows at all -- not an instance, not a node, not
    # its assertion. The count is what keeps that readable, and it does not
    # make the export `partial`: a construct this tool declines is not a
    # walk that fell short.
    check(one("SELECT checker_inst_count FROM v_db_info") == 1,
          "a checker instance is counted though it is not modelled")
    check(one("""
        SELECT count(*) FROM v_tree_node WHERE node_name = 'u_chk'""") == 0,
          "and contributes no node")
    check(one("SELECT analysis_status FROM v_db_info") == "complete",
          "while the status stays complete")
    check(one("""
        SELECT count(*) FROM stmt WHERE stmt_kind='assertion'""") >= 3,
          "assertion statements")
    for word in ("assert", "assume", "cover"):
        check(one("""
            SELECT count(*) FROM stmt
            WHERE stmt_kind='assertion' AND construct=?""", word) >= 1,
              f"{word} keeps its own word")
    check(one("""
        SELECT count(*) FROM expr_ref WHERE role='assertion'""") >= 3,
          "the concurrent assertion's reads")
    check(one("""
        SELECT count(*) FROM expr_ref WHERE role='assertion' AND is_exact=1""") == 0,
          "unresolved assertion reads are marked inexact")
    check(one("""
        SELECT count(*) FROM v_load WHERE load_kind='statement'""") >= 3,
          "an assertion's reads are statement-kind loads")

if mode == "params":
    # ---- a procedural if on a parameter, one variant per value
    # The elaboration settles the branch and the rows stay: a simulator has
    # the statement, a source view shows the line, and the level says which
    # arm nothing can reach. v18 filtered the parameter out of the gating as
    # a constant operand, so the dead arm carried no condition at all and q
    # read as unconditionally driven from two places.
    verdicts = sorted(con.execute("""
        SELECT i.param_signature, b.sense, b.static_taken
        FROM v_branch b JOIN inst i ON i.id = b.inst_id
        WHERE b.module_name='deadarm'""").fetchall())
    check(verdicts == [("EN=1'b0", "else", 1), ("EN=1'b0", "then", 0),
                       ("EN=1'b1", "else", 0), ("EN=1'b1", "then", 1)],
          "each variant marks the arm its own parameter value cannot reach",
          f"got {verdicts}")
    check(one("""
        SELECT count(*) FROM v_driver d
        JOIN v_stmt s ON s.stmt_id = d.stmt_id
        WHERE s.module_name='deadarm' AND d.signal_name='q'""") == 4,
          "and both arms keep their driver rows in both variants")
    check(one("""
        SELECT count(*) FROM v_driver d
        JOIN v_stmt s ON s.stmt_id = d.stmt_id
        JOIN v_branch b ON b.branch_id = s.branch_id
        WHERE s.module_name='deadarm' AND d.signal_name='q'
          AND b.static_taken = 1""") == 2,
          "so a live-driver query is one predicate on the level")

    # LRM 23.10 -- three ways a parameterisation key goes wrong.
    # ---- two values that print alike
    # A template is keyed by (definition, parameter values), so the parameter
    # TEXT is the identity: fold two values onto one string and the second
    # instance is replayed from the first one's analysis, reporting a body it
    # never elaborated.
    #
    # `SVInt::toString` prints a value that has unknown bits, is wider than
    # 64, and is neither all-x nor all-z as the single letter `X` unless
    # asked for exact unknowns. These two differ in P[1], which selects
    # opposite generate branches.
    check(one("""
        SELECT count(DISTINCT i.param_signature) FROM inst i
        JOIN module m ON m.id = i.module_id
        WHERE m.name='paramfold_sub'""") == 2,
          "two parameter values that print alike keep distinct signatures")
    # The payoff, read off the tree: each instance holds the branch its own
    # P[1] selects, and holds it exactly once.
    for inst, branch, net in (("u1", "lo", "lo.only_when_clear"),
                              ("u2", "hi", "hi.only_when_set")):
        check(one("""
            SELECT count(*) FROM tree_node g
            JOIN tree_node p ON p.id = g.parent_node_id
            WHERE g.node_kind='generate' AND g.name=? AND p.name=?""",
                  branch, inst) == 1,
              f"{inst} elaborates the {branch} branch")
        check(one("""
            SELECT count(*) FROM net n
            JOIN tree_node g ON g.id = n.scope_node_id
            JOIN tree_node p ON p.id = g.parent_node_id
            WHERE n.name=? AND p.name=?""", net, inst) == 1,
              f"and holds {net}, once")
    # ---- two spellings of one type
    # slang folds the two spellings of one type onto a single body, so pass 1
    # ends up with a group it never gets an analysed body for. The claim
    # under test is that this costs nothing: all four flops are stamped, each
    # with its procedure, and the status stays complete.
    check(one("""
        SELECT count(*) FROM inst i JOIN module m ON m.id = i.module_id
        WHERE m.name = 'reg1'""") == 4,
          "both pairs stamp both flops")
    check(one("""
        SELECT count(*) FROM inst i JOIN module m ON m.id = i.module_id
        WHERE m.name = 'reg1'
          AND NOT EXISTS (SELECT 1 FROM proc p WHERE p.inst_id = i.id)""") == 0,
          "and every one of them carries its procedure")
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name = 'q' AND driver_name = 'd'
          AND driver_kind = 'data'""") == 4,
          "so all four flop outputs have the flop as their driver")
    check(one("SELECT analysis_status FROM v_db_info") == "complete",
          "a deduplicated parameterisation does not make the export partial")
    # ---- a legal parameterised self-instantiation
    # The control for the recursion guard: a module instantiating itself,
    # legally, because the parameter shrinks each level. The guard keys on
    # (module, parameters) and this file repeats the module at every level
    # and the pair at none, so nothing is cut and the tree is whole.
    check(meta["analysis_status"] == "complete",
          "a terminating parameterised recursion compiles clean",
          f"got {meta['analysis_status']!r}")
    check(int(meta["recursion_count"]) == 0,
          "and nothing is cut", f"got {meta['recursion_count']}")
    # 1 + 2 + 4 + 8. A guard keyed on the module alone would stop at the
    # first level and leave one.
    check(one("""
        SELECT count(*) FROM inst i JOIN module m ON m.id = i.module_id
        WHERE m.name = 'redtree'""") == 15,
          "every level of the tree is stamped")
    # And the levels really are one module at four parameterisations, which
    # is what makes this a control rather than four different modules.
    check(one("""
        SELECT count(DISTINCT i.param_signature) FROM inst i
        JOIN module m ON m.id = i.module_id
        WHERE m.name = 'redtree'""") == 4,
          "as one module under four parameterisations")
    # Every level's output is driven, and the eight leaves are driven from
    # their own input bit -- so the dataflow survived the recursion and did
    # not merely get a tree of empty instances.
    check(one("""
        SELECT count(*) FROM net n JOIN inst i ON i.id = n.inst_id
        JOIN module m ON m.id = i.module_id
        WHERE m.name = 'redtree' AND n.name = 'y'
          AND NOT EXISTS (SELECT 1 FROM v_driver d
                          WHERE d.signal_net_id = n.id)""") == 0,
          "every level's output has a driver")
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name = 'y' AND driver_name = 'a'
          AND driver_kind = 'data'""") == 8,
          "and each of the eight leaves reduces its own bit")


if mode == "procedural":
    # `-> fired` is what makes an event happen. Without the row the event
    # had waiters and no cause; `constant` would have been worse, saying it
    # is tied off.
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name = 'fired' AND driver_kind = 'trigger'
          AND driver_net_id IS NULL""") == 1,
          "a triggered event is driven by its trigger, not by a constant")
    check(one("""
        SELECT count(*) FROM v_load
        WHERE signal_name = 'fired' AND load_kind = 'sensitivity'""") == 1,
          "and the procedure waiting on it is still its load")
    # Both statements exist so their gating has somewhere to land: the
    # condition reaching them is a read, and it had nowhere else to go.
    for kind in ("trigger", "disable"):
        check(one("""
            SELECT count(*) FROM expr_ref e JOIN stmt s ON s.id = e.stmt_id
            JOIN net n ON n.id = e.net_id
            WHERE s.stmt_kind = ? AND e.role = 'control' AND n.name = 'en'""",
                  kind) == 1,
              f"a gated {kind} records the condition it was reached under")
    check(one("""
        SELECT count(*) FROM stmt WHERE stmt_kind = 'disable'
          AND NOT EXISTS (SELECT 1 FROM stmt_target t WHERE t.stmt_id = stmt.id)
        """) == 1,
          "and a disable names no net, having written none")
    # ---- LRM 11.4.1: an assignment operator reads its target
    # LRM 11.4.1 -- an assignment operator reads its target. slang builds
    # `a += b` as BinaryExpression(LValueReference, b), and an
    # LValueReference is a bare placeholder with no link back to the lvalue,
    # so walking the right side finds b and never finds a.
    #
    # `explicit_self` is the control: it spells the read out. The compound
    # forms must export the same source set.
    for net in ("explicit_self", "compound_self", "masked"):
        check(one("""
            SELECT count(*) FROM v_driver
            WHERE signal_name=? AND driver_kind='data'
              AND driver_name IN (?, 'x')""", net, net) == 2,
              f"{net} is driven by itself and by x")
    # The one whose whole right side is the placeholder and a constant. With
    # no source recovered it had none at all, and surfaced as a tie-off.
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name='shift_self' AND driver_kind='data'
          AND driver_name='shift_self'""") == 1,
          "a shift-assign reads itself")
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name='shift_self' AND driver_kind='constant'""") == 0,
          "and is not reported as constant-driven")
    # ---- LRM 12 statements and LRM 13.4 return
    # LRM 13.4 -- `return e;` writes the subroutine's implicit result
    # variable, which slang does not synthesise as an assignment. The two
    # spellings compute the same thing and must export the same shape.
    for net in ("assign_style", "ret_style"):
        check(one("""
            SELECT count(*) FROM v_net_dep
            WHERE tgt_name=? AND dep_kind='data'
              AND src_name IN ('x', 'k')""", net) == 2,
              f"{net} reads both x and k")
    # LRM 12.5 / 12.7.3 -- a case selector and a do-while condition gate what
    # they enclose. Neither had a handler, so the condition signal had no
    # load row anywhere in the database.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='sel' AND tgt_name='matched'
          AND dep_kind='control'""") == 2,
          "a case selector gates each of its branches")
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='b' AND tgt_name='looped'
          AND dep_kind='control'""") == 1,
          "a do-while condition gates its body")
    # LRM 12.7.1 -- a for loop's initialiser and step move the INDEX, which
    # is not a design signal: it takes every value of the space on the way
    # through, so `i -> sum` said sum depended on something no debugging
    # question reaches. The loop level carries the space instead, and the
    # body sits in it.
    check(one("""
        SELECT count(*) FROM v_stmt_target t JOIN v_stmt s
          ON s.stmt_id = t.stmt_id
        WHERE t.net_name='i' AND s.module_name='stmtgaps'""") == 0,
          "a for-loop index is written by no statement row")
    check(one("""
        SELECT count(*) FROM v_net_dep d JOIN v_stmt s ON s.stmt_id = d.stmt_id
        WHERE d.src_name='i' AND s.module_name='stmtgaps'""") == 0,
          "and sources no dependency")
    check(one("""
        SELECT count(*) FROM v_branch
        WHERE module_name='stmtgaps' AND branch_kind='loop' AND iter_name='i'
          AND iter_first=0 AND iter_step=1 AND iter_count=4""") == 1,
          "the loop level carries the space it runs -- i = 0,1,2,3")
    # A level is a fact about the source, not about what landed under it.
    # `if (en) ;` has an arm and an empty loop body has a level, exactly as
    # an empty case arm has always had a row -- otherwise a reader counting
    # senses gets a level the design does not have and misses one it does.
    check(sorted(r[0] for r in con.execute("""
        SELECT sense FROM v_branch
        WHERE module_name='emptylevel' AND branch_kind='if'""")) ==
          ["else", "then"],
          "an if arm that gates nothing is still an arm, on both senses")
    check(one("""
        SELECT count(*) FROM v_branch
        WHERE module_name='emptylevel' AND branch_kind='loop'
          AND iter_count=2""") == 1,
          "and a loop with an empty body still carries its iteration space")
    check(one("""
        SELECT count(*) FROM v_stmt s
        JOIN v_branch b ON b.branch_id = s.branch_id
        JOIN v_stmt_operand o ON o.stmt_id = s.stmt_id
        WHERE s.module_name='stmtgaps' AND b.branch_kind='loop'
          AND b.iter_name='i' AND o.net_name='x'""") == 1,
          "and the body it guards sits in it")

    # LRM 12.7 -- the four ways taking a header variable for an index goes
    # wrong, each with the row that proves it did not.
    check(one("""
        SELECT count(*) FROM v_net_dep d JOIN v_stmt s ON s.stmt_id = d.stmt_id
        WHERE s.module_name='loopspace' AND d.src_name='i'""") == 1
          and one("""
        SELECT dep_kind FROM v_net_dep d JOIN v_stmt s ON s.stmt_id = d.stmt_id
        WHERE s.module_name='loopspace' AND d.src_name='i'""") == "procedure",
          "an index read as a value still feeds the formal it is bound to")
    check(one("""
        SELECT count(*) FROM v_driver d JOIN v_stmt s ON s.stmt_id = d.stmt_id
        WHERE s.module_name='loopspace' AND d.signal_name='acc'
          AND d.driver_name='d'""") == 1,
          "a header variable the header does not step keeps its rows")
    space = con.execute("""
        SELECT iter_name, iter_first, iter_step, iter_count, static_taken
        FROM v_branch WHERE module_name='loopspace' AND branch_kind='loop'
        ORDER BY src_line""").fetchall()
    check(space == [("i", 0, 1, 4, None), ("kk", None, None, None, None),
                    ("j", 7, -1, 8, None), ("z", None, None, 0, 0)],
          "each loop publishes exactly the space it can stand behind",
          f"got {space}")

    # LRM 12.4 -- the red line the branch table exists for. `gating` writes q
    # from all four arms of one if/else-if chain, and in v18 the two arms of
    # each `if` produced rows that were equal column for column: same
    # conditions, same expr_ref set, same everything but the operand. Each
    # must now name a level of its own, and the levels must nest by the
    # desugaring rather than flatten.
    arms = con.execute("""
        SELECT s.src_line, b.depth, b.sense
        FROM v_stmt s
        JOIN v_stmt_target t ON t.stmt_id = s.stmt_id
        JOIN v_branch b ON b.branch_id = s.branch_id
        WHERE s.module_name='gating' AND t.net_name='q'
        ORDER BY s.src_line""").fetchall()
    check([(d, sn) for _, d, sn in arms]
          == [(1, "then"), (2, "then"), (3, "then"), (3, "else")],
          "an if/else-if chain nests one level per else, not one flat set",
          f"got {arms}")
    check(one("""
        SELECT count(DISTINCT s.branch_id) FROM v_stmt s
        JOIN v_stmt_target t ON t.stmt_id = s.stmt_id
        WHERE s.module_name='gating' AND t.net_name='q'""") == 4,
          "so the four arms writing one target sit in four levels")
    # The reads stay identical -- that was never the problem -- which is why
    # the level is the only thing that can tell the arms apart.
    check(one("""
        SELECT count(DISTINCT reads) FROM (
            SELECT s.stmt_id AS sid,
                   group_concat(e.net_id) AS reads
            FROM v_stmt s
            JOIN v_stmt_target t ON t.stmt_id = s.stmt_id
            JOIN expr_ref e ON e.stmt_id = s.stmt_id AND e.role='control'
            WHERE s.module_name='gating' AND t.net_name='q'
              AND s.src_line IN (189, 190)
            GROUP BY s.stmt_id)""") == 1,
          "while the two innermost arms still read exactly the same nets")
    # LRM 12.5 -- the qualifier and the matching semantics belong to the case
    # POINT; the labels belong to the item, one row each, and no item sees
    # another's. v18 pushed the selector and every label onto one gate.
    check(sorted(con.execute("""
        SELECT case_kind, check_kind FROM v_branch
        WHERE module_name='gating' AND branch_kind='case'""").fetchall(),
        key=lambda r: (r[0], r[1] or "")) ==
        [("case", None), ("case", "priority"), ("casex", "unique0"),
         ("casez", "unique")],
          "each case point carries its own matching semantics and qualifier")
    check(one("""
        SELECT count(*) FROM branch_label l
        JOIN branch b ON b.id = l.branch_id
        JOIN branch p ON p.id = b.parent_branch_id
        JOIN inst i ON i.id = b.inst_id
        JOIN module m ON m.id = i.module_id
        WHERE m.name='gating' AND p.case_kind='casez'""") == 2,
          "a casez arm carries only its own label")
    check(one("""
        SELECT l.value FROM branch_label l
        JOIN branch b ON b.id = l.branch_id
        JOIN branch p ON p.id = b.parent_branch_id
        JOIN inst i ON i.id = b.inst_id
        JOIN module m ON m.id = i.module_id
        WHERE m.name='gating' AND p.case_kind='casez' AND b.line=208""")
          == "4'b1zzz",
          "and publishes it as the elaborated value, not the spelling")
    check(one("""
        SELECT count(*) FROM v_branch
        WHERE module_name='gating' AND branch_kind='case_default'""") == 4,
          "every case here has its default arm as a level of its own")
    # LRM 10.6.2 -- a release drives nothing, so it carries no dependency to
    # hang its gating on. What decides when the hijack ends is a control
    # reference on the release statement itself.
    check(one("""
        SELECT count(*) FROM v_stmt s
        JOIN expr_ref e ON e.stmt_id = s.stmt_id
        JOIN net n ON n.id = e.net_id
        WHERE s.stmt_kind='release' AND e.role='control'
          AND n.name='g'""") == 1,
          "a release records the condition that ends the hijack")
    check(one("""
        SELECT count(*) FROM v_load
        WHERE signal_name='g' AND load_kind='statement'""") == 1,
          "and that condition reads g as a statement-kind load")
    # ---- LRM 9.2.2.3 / 9.4.2: the procedure kind and edge kind that have
    # ---- no posedge/negedge spelling to be mistaken for
    check(one("""
        SELECT count(*) FROM proc WHERE proc_kind='always_latch'""") == 1,
          "a level-sensitive procedure keeps its own kind")
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='en' AND tgt_name='latched'
          AND dep_kind='control'""") == 1,
          "and the condition inside it still gates")
    check(one("""
        SELECT count(*) FROM proc_event
        WHERE event_kind='sensitivity' AND edge_kind='both'""") == 1,
          "an edge-agnostic event records edge_kind='both', not a direction")
    check(one("""
        SELECT count(*) FROM v_load l JOIN net n ON n.id = l.signal_net_id
        WHERE n.name='ev' AND l.load_kind='sensitivity'""") == 1,
          "and reads its signal as a sensitivity load")

    # ---- a macro location, and an output argument
    # A location inside a macro expansion. getFileName and getLineNumber
    # expand internally; getColumnNumber does not -- its precondition is a
    # FILE location, and a macro location's buffer holds an ExpansionInfo, so
    # it returned 0. File and line named the expansion site while the column
    # said 0, which is not a column in any 1-based numbering.
    #
    # That a column is >= 1 is universal; what is local here is that the row
    # lands on the macro's USE site, in the same file as the control beside
    # it, rather than on the `define.
    macro, direct = (con.execute("""
        SELECT s.src_path, s.src_line FROM v_stmt_target t
        JOIN v_stmt s ON s.stmt_id = t.stmt_id
        WHERE t.net_name = ?""", (n,)).fetchone() for n in
        ("via_macro", "direct"))
    check(macro[0] == direct[0],
          "a macro-expanded row names the file it expanded in",
          f"{macro[0]!r} vs {direct[0]!r}")
    check(macro[1] == direct[1] - 1,
          "and the line it was written on, not the one it was defined on",
          f"macro at {macro[1]}, control at {direct[1]}")
    # An output argument: Expression::bindLValue wraps the actual in an
    # AssignmentExpression, so a plain-reference test on the raw argument
    # sees the wrapper and every output binding claimed map_exact=0 --
    # including one exactly as wide as its formal.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE dep_kind='procedure' AND tgt_name='scratch'
          AND map_exact=1""") == 1,
          "a whole-to-whole output argument maps one-to-one")
    # And an output argument whose actual is NOT the formal's type, which is
    # the one that arrives wrapped in a Conversion. Both actuals are driven
    # by the same task through the same formal, so both must show exactly
    # one driver and it must be the procedure binding. `nib` showed two: the
    # binding, plus a source-less row that v_driver renders as a constant
    # tie-off, because the copy-back was recognised by expression kind and
    # only the bare form has that kind.
    # The narrower actual takes only the formal's low nibble, and those four
    # bits do correspond one for one -- the truncation is the source range,
    # not a lost correspondence.
    for actual, src_hi in (("scratch", None), ("nib", 3)):
        kinds = sorted(r[0] for r in con.execute("""
            SELECT driver_kind FROM v_driver d JOIN net n ON n.id = d.signal_net_id
            WHERE n.name = ?""", (actual,)))
        check(kinds == ["procedure"],
              f"the task is {actual}'s only driver, and it is the binding",
              f"got {kinds}")
        check(one("""
            SELECT count(*) FROM v_net_dep
            WHERE dep_kind='procedure' AND tgt_name=? AND src_name='pass.o'
              AND src_hi IS ? AND map_exact=1""", actual, src_hi) == 1,
              f"{actual} binds the formal's own bits, one for one")


if mode == "structural":
    # A terminal whose type has no packed width is still placed: the two
    # sides of one whole-to-whole binding must not disagree about it.
    check(one("""SELECT count(*) FROM v_term_map
                 WHERE term_name='arr' AND term_exact=1 AND map_exact=1""") >= 1,
          "an unpacked-array terminal maps whole and exact inside")
    check(one("""SELECT count(*) FROM v_net_conn
                 WHERE term_name='arr' AND term_exact=0""") == 0,
          "and its connection agrees from the outside")

    # Two occurrences of one parameterisation: one signature, two row sets.
    check(one("""
        SELECT count(*) FROM inst i JOIN module m ON m.id=i.module_id
        WHERE m.name='leaf' AND i.param_signature='W=4'""") == 2,
          "the same parameterisation twice is two occurrences")
    check(one("""
        SELECT count(DISTINCT n.id) FROM net n JOIN inst i ON i.id=n.inst_id
        JOIN module m ON m.id=i.module_id
        WHERE m.name='leaf' AND n.name='q' AND i.param_signature='W=4'""") == 2,
          "each occurrence owns its own nets")
    # The generate array: one level per element, one instance under each.
    check(one("""
        SELECT count(*) FROM tree_node WHERE node_kind='generate'
          AND name LIKE 'lane%'""") == 4,
          "a generate array is one level per element")
    check(one("""
        SELECT count(*) FROM tree_node c JOIN tree_node g
          ON g.id = c.parent_node_id
        WHERE g.node_kind='generate' AND c.node_kind='instance'""") == 4,
          "each element holds its own instance")
    check(one("""
        SELECT count(DISTINCT i.id) FROM inst i JOIN module m ON m.id=i.module_id
        WHERE m.name='leaf' AND i.param_signature='W=1'""") == 4,
          "the array's parameterisation is per element")
    # Non-ANSI directions survive; the inout terminal arcs both ways.
    for tname, tdir in (("a", "input"), ("y", "output"), ("t", "inout")):
        check(one("""
            SELECT count(*) FROM term t JOIN inst i ON i.id=t.inst_id
            JOIN module m ON m.id=i.module_id
            WHERE m.name='oldstyle' AND t.name=? AND t.direction=?""",
                  tname, tdir) == 1,
              f"the non-ANSI {tdir} {tname} keeps its direction")
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE driver_kind='connection' AND signal_name='pad'""") >= 1,
          "the inout arcs outward")
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE driver_kind='connection' AND driver_name='pad'""") >= 1,
          "the inout arcs inward")

    # LRM 24 -- a program is its own definition kind. slang's DefinitionKind
    # has three values and this is the one no synthesizable fixture reaches.
    check(one("""
        SELECT count(*) FROM module
        WHERE name='watcher' AND def_kind='program'""") == 1,
          "a program definition keeps def_kind='program'")
    check(one("""
        SELECT count(*) FROM v_tree_node t JOIN module m ON m.id = t.module_id
        WHERE m.def_kind='program' AND t.node_kind='instance'""") == 1,
          "and its instantiation is an ordinary instance node")


if mode == "portshape":
    # LRM 23.2.2.3 -- ports whose terminal cannot be found by name.
    #
    # A MultiPort connection arrives as one PortConnection per member, so
    # `.p({hi, lo})` is ONE terminal carrying two mappings, each an exact
    # window of the formal. Looking the terminal up by the member name finds
    # none and drops the connection.
    check(one("""
        SELECT count(*) FROM term t JOIN tree_node n ON n.id = t.inst_id
        WHERE n.name='u_mp' AND t.name='p'""") == 1,
          "a MultiPort is one terminal, not one per member")
    for member, lo, hi in (("hi", 4, 7), ("lo", 0, 3)):
        check(one("""
            SELECT count(*) FROM v_term_map m
            JOIN v_tree_node n ON n.node_id = m.term_inst_id
            WHERE n.node_name='u_mp' AND m.term_name='p'
              AND m.inner_net_name=? AND m.term_lo=? AND m.term_hi=?
              AND m.term_exact=1 AND m.map_exact=1""", member, lo, hi) == 1,
              f"and {member} takes bits {hi}:{lo} of it, one-to-one")
    # The outer side crosses whole: `bus` reaches both halves of the formal.
    check(one("""
        SELECT count(*) FROM v_net_conn
        WHERE outer_net_name='bus' AND term_name='p' AND map_exact=1""") == 2,
          "and the actual reaches both halves as two exact segments")
    # Two ports with no external name collapse onto one synthesized
    # `<unnamed>`, which is a name two terminals share -- so they are told
    # apart by (term_id, ordinal) and not by it.
    check(one("""
        SELECT count(*) FROM term t JOIN tree_node n ON n.id = t.inst_id
        WHERE n.name='u_anon' AND t.name='<unnamed>'""") == 2,
          "two unnamed ports stay two terminals of one name")
    check(one("""
        SELECT count(DISTINCT t.ordinal) FROM term t
        JOIN tree_node n ON n.id = t.inst_id
        WHERE n.name='u_anon' AND t.name='<unnamed>'""") == 2,
          "and their ordinals separate them")
    # A port whose reference carries a select sets both internalSymbol and
    # internalExpr: the mapping is the SELECT's window, not the whole net.
    for member in ("a", "b"):
        check(one("""
            SELECT count(*) FROM v_term_map m
            JOIN v_tree_node n ON n.node_id = m.term_inst_id
            WHERE n.node_name='u_anon' AND m.inner_net_name=?
              AND m.inner_lo=0 AND m.inner_hi=1 AND m.map_exact=1""",
                  member) == 1,
              f"a selected port maps {member}[1:0], not the whole net")


if mode == "udp":
    check(one("""
        SELECT count(*) FROM prim WHERE prim_kind='udp'
          AND def_name='latch_p'""") == 1,
          "the UDP is a primitive of its own kind")
    check(one("""
        SELECT count(*) FROM prim WHERE prim_kind='switch'
          AND def_name='tranif1'""") == 1,
          "the switch is a primitive of its own kind")
    # LRM 28.4 -- the whole switch family, not just what slang labels
    # BiDiSwitch. It registers the resistive variants and the MOS switches as
    # plain Fixed gates, so keying on its label alone reads them as gates.
    check(one("""
        SELECT count(*) FROM prim WHERE prim_kind='switch'
          AND def_name IN ('rtran','nmos')""") == 2,
          "rtran and nmos are switches too")
    check(one("""
        SELECT count(*) FROM v_net_dep d
        JOIN prim p ON p.id = d.prim_id
        WHERE p.def_name='rtran'
          AND ((d.src_name='ra' AND d.tgt_name='rb')
            OR (d.src_name='rb' AND d.tgt_name='ra'))""") == 2,
          "and the resistive switch still conducts both ways")
    check(one("""
        SELECT count(*) FROM prim WHERE prim_kind='gate'""") >= 1,
          "the buffer stays a gate")
    check(one("""
        SELECT count(*) FROM v_net_dep d
        JOIN prim p ON p.id = d.prim_id
        WHERE p.prim_kind='udp' AND d.tgt_name='q'
          AND d.src_name IN ('d','en')""") == 2,
          "the UDP couples its inputs to its output")
    # A tran conducts both ways: each end drives the other.
    check(one("""
        SELECT count(*) FROM v_net_dep d
        JOIN prim p ON p.id = d.prim_id
        WHERE p.prim_kind='switch'
          AND ((d.src_name='a' AND d.tgt_name='b')
            OR (d.src_name='b' AND d.tgt_name='a'))""") == 2,
          "the switch couples both directions")

    # Anonymous gates get a segment of their own, so a parent's children
    # stay distinguishable and no gate borrows the instance's name.
    check(one("""
        SELECT count(*) FROM tree_node t JOIN tree_node p
          ON p.id = t.parent_node_id
        WHERE p.name='u_anon' AND t.node_kind='primitive'
          AND t.name = p.name""") == 0,
          "an anonymous gate never takes its parent's name")
    check(one("""
        SELECT count(*) FROM tree_node t JOIN tree_node p
          ON p.id = t.parent_node_id
        WHERE p.name='u_anon' AND t.node_kind='primitive'""") == 4,
          "each anonymous gate is its own node")
    check(one("""
        SELECT count(*) FROM (SELECT parent_node_id, name, count(*) c
                              FROM tree_node GROUP BY parent_node_id, name
                              HAVING c > 1)""") == 0,
          "and no two siblings share a name")

if mode == "refport":
    # A `const ref` actual is read and never written: the direction alone
    # would make it a driver, and no call can produce that write.
    check(one("""SELECT count(*) FROM v_driver
                 WHERE signal_name='table_ro'""") == 0,
          "a const ref actual has no driver")
    check(one("""SELECT count(*) FROM v_load
                 WHERE signal_name='table_ro' AND load_kind='dataflow'""") >= 1,
          "and is read by the call")

    # LRM 23.2.2.4 -- the fourth port direction. A `ref` binds the actual
    # VARIABLE rather than a net both sides drive, and like `inout` it arcs
    # both ways: the terminal is a driver of the outer net and a load of it.
    # The arc formulas name `ref` beside `inout` on both sides, so a design
    # with only `inout` leaves half of each formula unwalked.
    check(one("""
        SELECT count(*) FROM v_term
        WHERE term_name='shared' AND direction='ref'""") == 1,
          "a ref port keeps direction='ref'")
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name='shared' AND driver_kind='connection'""") >= 1,
          "and the crossing drives the outer variable")
    check(one("""
        SELECT count(*) FROM v_load
        WHERE signal_name='shared' AND load_kind='connection'""") >= 1,
          "and reads it, the same terminal on both sides of the arc")
    check(one("""
        SELECT count(*) FROM v_term_map
        WHERE term_name='shared' AND map_exact=1""") == 1,
          "the binding is whole-to-whole, so it maps one-to-one")
    # What the child writes reaches the parent's variable: the read-modify-
    # write inside refsink is a driver of `shared` seen from outside it.
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name='o' AND driver_name='shared'
          AND driver_kind='data'""") == 1,
          "and the parent reads the variable the child assigned")


if mode == "patterncase":
    # LRM 12.6 -- `case ... matches` had no handler and fell to visitDefault,
    # which visits the condition; the walker has no handler for a bare value
    # expression either, so `sel` had zero load rows despite selecting the
    # branch.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='sel' AND tgt_name='q'
          AND dep_kind='control'""") == 2,
          "a pattern-case selector gates each of its branches")
    check(one("""
        SELECT count(*) FROM v_load
        WHERE signal_name='sel' AND load_name='q'""") == 2,
          "so the selector reads as a load")
    # LRM 12.6 -- a pattern-case item matches a pattern, not a value, so it
    # has no label to publish; the case_kind is what a consumer branches on.
    check(one("""
        SELECT count(*) FROM v_branch
        WHERE branch_kind='case' AND case_kind='matches'""") == 1,
          "a pattern case is a case point spelled 'matches'")
    check(one("""
        SELECT count(*) FROM branch_label l
        JOIN branch b ON b.id = l.branch_id
        JOIN branch p ON p.id = b.parent_branch_id
        WHERE p.case_kind='matches'""") == 0,
          "and its items publish no label values")
    # LRM 12.5.4 -- a range label reaches no single constant, so its value is
    # NULL while the row itself stays: "this item has a label this export
    # could not evaluate" is a different fact from "this item has no label".
    check(one("""
        SELECT count(*) FROM branch_label l
        JOIN branch b ON b.id = l.branch_id
        JOIN branch p ON p.id = b.parent_branch_id
        WHERE p.case_kind='inside' AND l.value IS NULL""") == 1,
          "an inside range is a label row with no value")
    # Two labels on one arm, each its own row, in written order.
    check(one("""
        SELECT group_concat(v, ' ') FROM (
            SELECT l.value AS v FROM branch_label l
            JOIN branch b ON b.id = l.branch_id
            JOIN branch p ON p.id = b.parent_branch_id
            WHERE p.case_kind='inside' AND l.value IS NOT NULL
            ORDER BY l.ordinal)""") == "4'b111 4'b1001",
          "and a multi-label arm keeps each label, evaluated, in order")
    check(one("""
        SELECT labels FROM v_branch
        WHERE case_kind IS NULL AND labels LIKE '%,%'""")
          == "4'b111,4'b1001",
          "which v_branch.labels joins in the same order")
    # LRM 12.4.2 -- the qualifier is on the `if` that carries it, not on the
    # `else if` nested inside its else arm.
    check(one("""
        SELECT count(*) FROM v_branch
        WHERE branch_kind='if' AND check_kind='priority'""") == 2,
          "a priority if qualifies both its own arms")
    check(one("""
        SELECT count(*) FROM v_branch b
        JOIN v_branch p ON p.branch_id = b.parent_branch_id
        WHERE p.check_kind='priority' AND b.check_kind IS NOT NULL""") == 0,
          "and does not reach the else-if nested below it")
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='x' AND tgt_name='q' AND dep_kind='data'""") == 1,
          "and the branch it selects keeps its own dataflow")


if mode == "outward":
    # A package function's `return` writes the subroutine's own result
    # variable, and the row names that variable. It used to be filed under
    # the RETURNED expression's text, which put `|a` in the database as the
    # name of a written object -- and a consumer reading a written name off
    # this row got an expression where a signal belongs.
    check(one("""
        SELECT count(*) FROM hier_ref
        WHERE access='write' AND path='outward_pkg::chk.chk'""") == 1,
          "a package function's return writes its own result variable")
    check(one("""
        SELECT count(*) FROM hier_ref
        WHERE access='write' AND path LIKE '%|%'""") == 0,
          "and no written name is an expression")
    # A built-in method registers as a system call in slang, but nothing
    # leaves the language: the row says `call`, with the method's own word.
    check(one("""
        SELECT count(*) FROM stmt
        WHERE stmt_kind = 'call' AND construct = 'push_back'""") == 1,
          "a built-in method is a call, not a system task")
    # And a system subroutine with no `$` that DOES write: the prefix alone
    # sent it down the branch with no writeRefs, so its argument came back
    # with no driver at all.
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name = 'drawn' AND driver_kind = 'system_task'
          AND driver_net_id IS NULL""") == 1,
          "a written argument makes a prefixless system call a system task")
    # A call whose formal is no net of this instance still drives its output
    # actual, and says so in both places: the target row that names the
    # statement, and a dependency with no source -- there is no formal here to
    # name as one -- which v_driver reports as `procedure` with a NULL driver.
    for net in ("taken", "setb"):
        check(one("""
            SELECT count(*) FROM v_driver
            WHERE signal_name = ? AND driver_kind = 'procedure'
              AND driver_net_id IS NULL AND stmt_id IS NOT NULL""", net) == 1,
              f"the package task drives {net} without naming a driver net")
        check(one("""
            SELECT count(*) FROM v_stmt_target t JOIN v_driver d
              ON d.signal_net_id = t.net_id AND d.stmt_id = t.stmt_id
            WHERE t.net_name = ? AND t.target_kind = 'written_by'""", net) == 1,
              f"and the two views agree that that statement writes {net}")
    # And the same write from a condition, which has no statement to be a
    # position within -- so the dependency stands alone, and the fact that
    # `condb` is written survives even though nothing can say where.
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name = 'condb' AND driver_kind = 'procedure'
          AND driver_net_id IS NULL AND stmt_id IS NULL""") == 1,
          "a call in a condition drives its actual with no statement to name")
    check(one("""
        SELECT count(*) FROM v_stmt_target WHERE net_name = 'condb'""") == 0,
          "and takes no target row, a target being a place in a statement")
    # The write is all it is. `condb` fills an `output` formal, so the
    # condition never reads it, and nothing it gates depends on its value.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE dep_kind = 'control' AND src_name = 'condb'""") == 0,
          "and gates nothing, an output actual being written and not read")
    # The reference goes with the edge: a statement gated by that condition
    # does not read condb either, in the calling procedure or in the body the
    # call walks. The connection to the parent is the only load it has.
    check(one("""
        SELECT count(*) FROM v_load
        WHERE signal_name = 'condb' AND load_kind <> 'connection'""") == 0,
          "and no statement reads it, the reference going with the edge")
    # What the condition does read still gates: the drop takes the written
    # actual, not the condition. `unit_cfg` is a $unit name, so it arrives as
    # a control dependency with no source net and its reference beside it.
    check(one("""
        SELECT count(*) FROM v_net_dep d JOIN hier_ref h
          ON h.id = d.src_hier_ref_id
        WHERE d.dep_kind = 'control' AND d.tgt_name = 'hit'
          AND d.src_net_id IS NULL AND h.path = 'unit_cfg'""") == 1,
          "while the operand the condition does read gates what it decides")

    # `setit` is the one with nothing to read: its statement has no reference
    # of any kind, so the write-back is the only thing holding the target up.
    check(one("""
        SELECT count(*) FROM v_stmt s
        WHERE s.stmt_kind = 'call'
          AND EXISTS (SELECT 1 FROM v_stmt_target t
                      WHERE t.stmt_id = s.stmt_id AND t.net_name = 'setb')
          AND NOT EXISTS (SELECT 1 FROM hier_ref h WHERE h.stmt_id = s.stmt_id)
          AND NOT EXISTS (SELECT 1 FROM expr_ref e WHERE e.stmt_id = s.stmt_id)
        """) == 1,
          "and it reads nothing at all, so nothing else stands in for it")

if mode == "naming":
    # The assembled path keeps every segment as the tree spells it: an
    # escaped identifier with its backslash, its terminator and the `.`
    # inside it, and a generate level exactly once.
    for path in ("naming.\\u.1 ", "naming.\\gn.1 ", "naming.g[0]"):
        check(one("SELECT count(*) FROM v_node_path WHERE node_path = ?",
                  path) == 1,
              f"v_node_path spells {path!r} as the tree does")
    # A net's full path is its INSTANCE's path plus the name -- the scope
    # node's path would repeat the generate segment the name already has.
    check(one("""
        SELECT count(*) FROM v_net n JOIN v_node_path p ON p.node_id = n.inst_id
        WHERE p.node_path || '.' || n.net_name = 'naming.g[0].w'""") == 1,
          "and a net path assembles from its instance, not its scope")
    # Every leaf comes through leafSegment, so an escaped name keeps slang's
    # own `\name ` spelling and an array element carries its SOURCE index --
    # for a gate exactly as for a module instantiation, since the two are one
    # symbol base. A gate's raw symbol name carries neither: an array
    # element's is the bare array name, and an escaped one arrives unescaped.
    for kind, names in (("instance", ("\\u.1 ", "\\u[2] ", "u[0]", "u[1]")),
                        ("primitive", ("\\g.1 ", "\\g[2] ", "p[0]", "p[1]"))):
        for name in names:
            check(one("""
                SELECT count(*) FROM tree_node t JOIN tree_node par
                  ON par.id = t.parent_node_id
                WHERE par.name = 'naming' AND t.node_kind = ?
                  AND t.name = ?""", kind, name) == 1,
                  f"the {kind} leaf {name!r} is spelled as written")
    # The point of the index suffix: two elements of one array are two
    # siblings, and the bare array name is not a leaf.
    check(one("""
        SELECT count(*) FROM tree_node
        WHERE name IN ('u', 'p')""") == 0,
          "and no node answers to the bare name of an array")
    # A generate label is a segment like any other: escaped, and spelled as
    # the net path beside it spells the same level.
    check(one("""
        SELECT count(*) FROM tree_node t JOIN tree_node par
          ON par.id = t.parent_node_id
        WHERE par.name = 'naming' AND t.node_kind = 'generate'
          AND t.name = '\\gn.1 '""") == 1,
          "an escaped generate label keeps slang's spelling")
    check(one("""
        SELECT count(*) FROM net n JOIN tree_node t ON t.id = n.scope_node_id
        WHERE t.name = '\\gn.1 ' AND n.name = '\\gn.1 .gw'""") == 1,
          "and the net under it spells the same segment")
    # A reference path keeps the escape and its terminating space: without
    # them `\u.1 .v` respells as `u.1.v`, a different identifier.
    check(one("""
        SELECT count(*) FROM hier_ref WHERE path = '\\u.1 .v'""") == 1,
          "a reference through an escaped name keeps its terminator")
    check(one("""
        SELECT count(*) FROM hier_ref WHERE path IN ('u.1.v', '\\u.1.v')""") == 0,
          "and never a respelling of it")

if mode == "concatcursor":
    # A pattern whose element order is not most significant first takes the
    # whole target at range granularity. Walking it anyway reported the
    # OPPOSITE window and called it exact, which is the one failure mode a
    # position must not have.
    for tgt in ("up_arr", "keyed", "up_keyed"):
        check(one("SELECT count(*) FROM v_net_dep WHERE tgt_name = ? "
                  "AND tgt_lo IS NULL AND map_exact = 0", tgt) == 2,
              f"the inverted-order pattern {tgt} claims no per-element window")
        check(one("SELECT count(*) FROM v_net_dep WHERE tgt_name = ? "
                  "AND tgt_lo IS NOT NULL", tgt) == 0,
              f"and never a window for {tgt}")

    # The two flattening directions, which a consumer cannot guess from one
    # example: a packed member declared first takes the HIGH offsets, an
    # unpacked element declared first takes the LOW ones.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name = 'a' AND tgt_name = 'packed_o'
          AND tgt_lo = 4 AND tgt_hi = 7""") == 1,
          "a packed pattern's first member takes the high offsets")
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name = 'a' AND tgt_name = 'arr' AND tgt_lo = 0
          AND tgt_hi = 7""") == 1,
          "while an unpacked array's first element takes the low ones")

    # An unpacked object's width is the flattened space its offsets index,
    # not its packed width -- without it a range like [8:15] on a byte array
    # names a window into a size the row does not state.
    for name, w in (("arr", 32), ("slice_src", 32), ("slice_dst", 16)):
        check(one("SELECT width FROM v_net WHERE net_name = ?", name) == w,
              f"the unpacked net {name} reports its flattened width")
    # An unpacked-array range select: slang's bounds cover one element
    # however many the select names, so no range is claimed at all rather
    # than one that says the rest is untouched.
    check(one("""SELECT count(*) FROM v_net_dep
                 WHERE src_name='slice_src'
                   AND src_lo IS NULL AND src_exact=0""") >= 1,
          "an unpacked-array slice claims no bits it did not verify")
    check(one("""SELECT count(*) FROM v_net_dep
                 WHERE src_name='slice_src' AND src_exact=1""") == 0,
          "and never calls the narrow span exact")

    # The cursor walk, in both directions. Every operand of the exact split
    # takes its own eighth of the source, MSB first; a wrapped cursor would
    # put one of them at an offset near 2^64 instead, so pinning all four is
    # what makes the guard's arrival visible if it ever fires wrongly.
    for name, lo, hi in (("a", 24, 31), ("b", 16, 23), ("c", 8, 15),
                         ("f", 0, 7)):
        check(one("""
            SELECT count(*) FROM v_net_dep
            WHERE src_name='d' AND tgt_name=? AND src_lo=? AND src_hi=?
              AND src_exact=1 AND map_exact=1""", name, lo, hi) == 1,
              f"the exact split gives {name} bits {hi}:{lo} of d")
    # And the same concatenation read back, where the positions land on the
    # target instead.
    for name, lo, hi in (("a", 24, 31), ("b", 16, 23), ("c", 8, 15),
                         ("f", 0, 7)):
        check(one("""
            SELECT count(*) FROM v_net_dep
            WHERE src_name=? AND tgt_name='whole' AND tgt_lo=? AND tgt_hi=?
              AND tgt_exact=1""", name, lo, hi) == 1,
              f"and reading it back puts {name} at bits {hi}:{lo} of whole")
    # A streaming operand's window survives as an UPPER BOUND, never exact:
    # the stream permutes bits, so which of a[5:0] reach either half is
    # unknown, and exact would claim bits that never arrive.
    for tgt in ("sh", "sl"):
        check(one("""
            SELECT count(*) FROM v_net_dep
            WHERE src_name='a' AND tgt_name=? AND src_lo=0 AND src_hi=5
              AND src_exact=0""", tgt) == 1,
              f"a streamed read reaches {tgt} as an upper bound")
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name IN ('a','b') AND tgt_name IN ('sh','sl')
          AND src_exact=1""") == 0,
          "and no streamed pairing claims exact source bits")
    # The mirror, a streaming TARGET: the written windows are the unknown.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='a' AND tgt_name='sm0' AND tgt_exact=0""") == 1,
          "a streamed write reaches its target as an upper bound")
    # A named pattern positions per member exactly as the simple one does.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='a' AND tgt_name='packed_o' AND tgt_lo=4 AND tgt_hi=7
          AND map_exact=1""") == 1,
          "a named pattern member lands on its own window")

    # A zero-width operand takes no position and does not stop the walk:
    # the two operands beside it land where they would with the pad absent.
    for name, lo, hi in (("a", 8, 15), ("b", 0, 7)):
        check(one("""
            SELECT count(*) FROM v_net_dep
            WHERE src_name=? AND tgt_name='padded' AND tgt_lo=? AND tgt_hi=?
              AND tgt_exact=1 AND map_exact=1""", name, lo, hi) == 1,
              f"a zero-count pad leaves {name} at bits {hi}:{lo} of padded")
    # And what the pad replicates is not read: it occupies no bits of the
    # result, so an edge from it would be a dependency the RTL does not have.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='clk' AND tgt_name='padded'""") == 0,
          "and what it replicates reaches nothing")

    # No slot anywhere in this file carries a range only an unsigned wrap
    # could produce. The bound is not a big positive number: a wrapped cursor
    # lands just below 2^64 and SQLite stores the offsets as signed INTEGER,
    # so it arrives NEGATIVE -- and the slot it belongs to keeps its own
    # honest `hi`, leaving lo above hi. Both are what to look for.
    check(one("""
        SELECT count(*) FROM net_dep
        WHERE src_lo < 0 OR src_hi < 0 OR tgt_lo < 0 OR tgt_hi < 0
           OR src_lo > src_hi OR tgt_lo > tgt_hi""") == 0,
          "no dependency carries a wrapped bit range")


if mode == "rootref":
    # A $root path is absolute, and the two occurrences of one body that
    # spell it must land on the SAME object. A downward replay would have
    # answered r0's own subtree for r0 and r1's for r1; there is nothing
    # below either, so the give-away is that both rows resolve at all and
    # resolve alike.
    check(one("""
        SELECT count(DISTINCT h.resolved_net_id) FROM hier_ref h
        WHERE h.path='$root.rootref.u_leaf.q' AND h.access='read'""") == 1,
          "one absolute path resolves to one net from every occurrence")
    check(one("""
        SELECT count(*) FROM hier_ref h
        JOIN net n ON n.id = h.resolved_net_id
        JOIN tree_node t ON t.id = n.inst_id
        WHERE h.path='$root.rootref.u_leaf.q'
          AND t.name='u_leaf' AND n.name='q'""") == 3,
          "and it names u_leaf.q -- the write and both reads")
    # The shortest absolute path there is: one tree segment, straight to a
    # net of the root instance.
    check(one("""
        SELECT count(*) FROM hier_ref h
        JOIN net n ON n.id = h.resolved_net_id
        JOIN tree_node t ON t.id = n.inst_id
        WHERE h.path='$root.rootref.own' AND t.name='rootref'
          AND n.name='own'""") == 2,
          "a one-segment absolute path resolves to the root's own net")
    # The upward spelling of the same net resolves per occurrence: the
    # search runs against the tree the occurrence actually sits in, so both
    # readers answer with the one net the source names.
    check(one("""
        SELECT count(*) FROM hier_ref h
        JOIN net n ON n.id = h.resolved_net_id
        JOIN tree_node t ON t.id = n.inst_id
        WHERE h.path='rootref.u_leaf.q' AND t.name='u_leaf' AND n.name='q'
        """) == 2,
          "an upward path resolves from both occurrences")
    check(one("""
        SELECT count(DISTINCT resolved_net_id) FROM hier_ref
        WHERE path IN ('rootref.u_leaf.q', '$root.rootref.u_leaf.q')""") == 1,
          "and lands on the net its absolute spelling names")
    # What the resolution is for: the write becomes a driver instead of a
    # dependency that could not be materialised, and the read becomes a load.
    check(one("""
        SELECT count(*) FROM v_driver v
        JOIN tree_node t ON t.id = v.signal_inst_id
        WHERE t.name='u_leaf' AND v.signal_name='q'
          AND v.driver_name='d' AND v.driver_kind='data'""") == 1,
          "the absolute write drives the far net")
    check(one("""
        SELECT count(*) FROM v_load
        WHERE signal_name='own' AND load_name='shallow_o'
          AND load_kind='dataflow'""") == 2,
          "and the absolute read loads the root's net, once per occurrence")
    # The case that separates absolute from downward. rootref_below sits
    # BELOW the path it spells, so the path does split below the one analysed
    # body -- and a downward replay would answer each occurrence's own leaf.
    # Both rows must name u_below_a's; the local `deep.q` beside them, which
    # does follow the occurrence, is the control.
    check(one("""
        SELECT count(*) FROM hier_ref h
        JOIN net n ON n.id = h.resolved_net_id
        JOIN tree_node d ON d.id = n.inst_id
        JOIN tree_node p ON p.id = d.parent_node_id
        WHERE h.path='$root.rootref.u_below_a.deep.q'
          AND d.name='deep' AND p.name='u_below_a'""") == 2,
          "a $root path below the analysed body still names one leaf, "
          "from both occurrences")
    check(one("""
        SELECT count(DISTINCT p.name) FROM hier_ref h
        JOIN net n ON n.id = h.resolved_net_id
        JOIN tree_node d ON d.id = n.inst_id
        JOIN tree_node p ON p.id = d.parent_node_id
        WHERE h.path='deep.q'""") == 2,
          "while the local path beside it follows the occurrence")

if mode == "xmr":
    # A system task called in a CONDITION still writes its argument. The
    # call belongs to no statement of its own, and v_driver tells a system
    # write from a tie-off by the statement it came from -- so the call
    # gets a row, as the procedure header does for reads with no statement.
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name = 'seeded' AND driver_kind = 'system_task'
          AND driver_net_id IS NULL""") == 1,
          "a system write inside a condition is attributed, not lost")
    check(one("""
        SELECT count(*) FROM v_stmt
        WHERE stmt_kind = 'system_task' AND construct = '$value$plusargs'""") == 1,
          "and the call it came from is the statement that carries it")
    # Nested calls in one condition: the outer collection reaches through
    # the inner one, so a row per call recorded the inner write twice. One
    # row per outermost call carries both writes, once each.
    for net in ("outer_v", "inner_v"):
        check(one("SELECT count(*) FROM v_driver WHERE signal_name = ? "
                  "AND driver_kind = 'system_task'", net) == 1,
              f"a nested system call records {net} exactly once")
    check(one("""
        SELECT count(*) FROM v_stmt
        WHERE stmt_kind = 'system_task' AND construct = '$cast'""") == 1,
          "and the nesting is one statement, not one per call")

    # A hierarchical WRITE names its target, and the driver view says so on
    # the row itself: `always_comb u.x = a` is one lookup from the far net,
    # not a walk out to net_dep and back through hier_ref.
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name='x' AND driver_name='a' AND signal_ref='u.x'""") == 1,
          "a hierarchically written signal carries its spelling on the arc")
    check(one("""
        SELECT count(*) FROM v_load
        WHERE signal_name='a' AND load_name='x' AND load_ref='u.x'""") == 1,
          "and the load view mirrors it")
    # A path that climbs out of a twice-instantiated body and back into it
    # names ONE object from both occurrences -- never the occurrence's own
    # net, which is the one thing the path does not say.
    check(one("""SELECT count(DISTINCT d.driver_net_id) FROM v_driver d
                 JOIN net n ON n.id=d.signal_net_id
                 WHERE n.name='named'""") == 1,
          "a path back into the analysed body names one net from both")
    check(one("""SELECT count(*) FROM v_driver d JOIN net n ON n.id=d.signal_net_id
                 JOIN net s ON s.id=d.driver_net_id
                 JOIN tree_node t ON t.id = s.inst_id
                 WHERE n.name='named' AND s.name='mine'
                   AND t.name='u_tw1'""") == 2,
          "and it is u_tw1's, read from u_tw2 as well as from u_tw1")

    # `return e` writes the subroutine's implicit result variable and
    # nothing else. Filing that write under the RETURNED expression made the
    # far net a driver of itself -- a driver the design does not have, on an
    # object the function only reads.
    check(one("""SELECT count(*) FROM v_driver WHERE signal_name='fetch_src'""") == 0,
          "a return of a downward read leaves what it reads undriven")
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name='fetched' AND driver_kind='data'
          AND driver_name='fetch_src' AND driver_ref='u.fetch_src'""") == 1,
          "while the value it returns still reaches the caller")

    # A downward read is a real dependency naming the reference it went
    # through -- not a hier_ref row beside a fabricated constant driver.
    check(one("""
        SELECT count(*) FROM v_net_dep d
        JOIN hier_ref h ON h.id = d.src_hier_ref_id
        WHERE d.src_name='x' AND d.tgt_name='q'
          AND d.src_inst_id != d.tgt_inst_id
          AND h.path='u.x' AND h.access='read'""") == 1,
          "a downward read crosses as a dependency naming its reference")
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name='q' AND driver_kind='constant'""") == 0,
          "and the target is not reported as constant-driven")
    # A downward write likewise, from the writing instance's operand.
    check(one("""
        SELECT count(*) FROM v_net_dep d
        JOIN hier_ref h ON h.id = d.tgt_hier_ref_id
        WHERE d.src_name='a' AND d.tgt_name='x'
          AND h.path='u.x' AND h.access='write'""") == 1,
          "a downward write crosses as a dependency naming its reference")
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name='x' AND driver_name='a' AND driver_kind='data'""") == 1,
          "the far instance's net has the real driver")
    check(one("""
        SELECT count(*) FROM v_load
        WHERE signal_name='x' AND load_name='q' AND load_kind='dataflow'""") == 1,
          "and the far instance's net has the real load")
    # Bits survive the crossing.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='wide' AND tgt_name='slice_o'
          AND src_lo=0 AND src_hi=3""") == 1,
          "a part-select of a downward reference keeps its bits")
    # A select spelled through the genvar: the stored path carries each
    # iteration's constant, so the two iterations are two keys.
    for path in ("u_arr[1].x", "u_arr[2].x"):
        check(one("""
            SELECT count(*) FROM hier_ref
            WHERE path = ? AND access = 'read'""", path) == 1,
              f"the genvar select resolves to {path}")
    check(one("SELECT count(*) FROM hier_ref WHERE path LIKE '%k+1%'") == 0,
          "and no path keeps the unexpanded spelling")
    # A control dependency whose target is outward.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='g1' AND tgt_name='wide'
          AND dep_kind='control'""") == 1,
          "a condition gating an outward write is a control dependency")
    # Two outward-gated statements in ONE procedure: each target takes its
    # own condition. The per-statement condition vectors are indexed in
    # lockstep, so a stale entry shows up exactly here -- as gate1's signal
    # on gate2's target, or as a missing edge.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='en' AND tgt_name='gated'
          AND dep_kind='control'""") == 1,
          "the first outward condition gates its own statement")
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='rst' AND tgt_name='gated'
          AND dep_kind='control'""") == 1,
          "and the second gates its own, not the first's")
    check(one("""
        SELECT count(DISTINCT stmt_id) FROM v_net_dep
        WHERE src_name IN ('en','rst') AND tgt_name='gated'
          AND dep_kind='control'""") == 2,
          "each outward condition lands on a distinct statement")
    # Two call sites, two conditions: each caller's gating reaches the
    # task body's write.
    for g in ("g1", "g2"):
        check(one("""
            SELECT count(*) FROM v_net_dep
            WHERE src_name=? AND tgt_name='hits'
              AND dep_kind='control'""", g) >= 1,
              f"the task body's write inherits {g} from its own call site")
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE tgt_name='put.v' AND dep_kind='procedure'""") == 2,
          "each call site binds its own actual to the formal")
    # Both call sites here are gated. A binding that loses its statement
    # under a branch is the shape that made per-call-site walking useless.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE tgt_name='put.v' AND dep_kind='procedure'
          AND stmt_id IS NOT NULL
          AND expr_ref_id IS NOT NULL""") == 2,
          "a gated call keeps its statement and its argument reference")
    check(one("""
        SELECT count(DISTINCT stmt_id) FROM v_net_dep
        WHERE src_name='put.v' AND tgt_name='hits'""") == 2,
          "the body's write is an occurrence per call site")
    # A signal that only appears in a sensitivity list is still a load.
    check(one("""
        SELECT count(*) FROM proc_event pe JOIN net n ON n.id = pe.net_id
        WHERE n.name='sens_only' AND pe.edge_kind IS NULL
          AND pe.event_kind='sensitivity'""") == 1,
          "a level-sensitive event is recorded with no edge")
    check(one("""
        SELECT count(*) FROM v_load
        WHERE signal_name='sens_only' AND load_kind='sensitivity'""") == 1,
          "and it reads as a sensitivity load")
    # A port tied to a name this instance does not have, resolved: it
    # crosses as a real arc instead of stopping at the hier_ref row, and
    # the constant beside it still tiles the rest of the formal.
    check(one("""
        SELECT count(*) FROM net_conn
        WHERE conn_kind='external_reference'
          AND outer_hier_ref_id IS NOT NULL""") == 2,
          "a port tied outward keeps its connection row, in both directions")
    check(one("""
        SELECT count(*) FROM v_driver d
        JOIN v_tree_node t ON t.node_id = d.signal_inst_id
        WHERE t.node_name='u_sink' AND d.signal_name='p'
          AND d.driver_kind='connection' AND d.driver_name='g'""") == 1,
          "and the resolved tie drives the formal across the boundary")
    # The tie is a positional element with a per-bit correspondence, so it
    # states its mapping: without one the arc reports 0 even though both
    # windows are exact, and nothing below bit granularity can follow it.
    check(one("""
        SELECT count(*) FROM v_driver d
        JOIN v_tree_node t ON t.node_id = d.signal_inst_id
        WHERE t.node_name='u_sink' AND d.signal_name='p'
          AND d.driver_kind='connection' AND d.driver_name='g'
          AND d.driver_lo=4 AND d.driver_hi=7 AND d.driver_exact=1
          AND d.map_exact=1""") == 1,
          "bit for bit: the external tie is traceable at bit granularity")
    # The tie names its net AND how the instantiation spelled it. The two
    # are different answers: `g` is where the bits live, `u.g` is what the
    # parent wrote, and only the second tells a reader why this net and not
    # one of the parent's own.
    check(one("""
        SELECT count(*) FROM v_driver d
        JOIN v_tree_node t ON t.node_id = d.signal_inst_id
        WHERE t.node_name='u_sink' AND d.signal_name='p'
          AND d.driver_kind='connection' AND d.driver_name='g'
          AND d.driver_ref='u.g'""") == 1,
          "and the crossing says how the tie was written, not only where it lands")
    # The mirror tie, on an output formal: u_sink2's `seen` drives u.split,
    # so the hierarchically written end is the LOAD, and the spelling has to
    # ride that side of the arc. The two branches place the same
    # v_conn_arc column, and only a design with ties in both directions can
    # tell a misplaced one from a correct one.
    check(one("""
        SELECT count(*) FROM v_load l
        JOIN v_tree_node t ON t.node_id = l.signal_inst_id
        WHERE t.node_name='u_sink2' AND l.signal_name='seen'
          AND l.load_kind='connection' AND l.load_name='split'
          AND l.load_ref='u.split'""") == 1,
          "an output tied outward names its spelling on the load side")
    check(one("""
        SELECT count(*) FROM v_driver d
        JOIN v_tree_node t ON t.node_id = d.driver_inst_id
        WHERE t.node_name='u_sink2' AND d.signal_name='split'
          AND d.driver_kind='connection' AND d.driver_name='seen'
          AND d.driver_ref IS NULL""") == 1,
          "and the driver side of that same arc claims no spelling")
    # v_conn_arc is scaffolding, so the connection's own reference is
    # reachable in contract through v_net_conn's pointer.
    check(one("""
        SELECT count(*) FROM v_net_conn c
        JOIN v_hier_ref h ON h.hier_ref_id = c.outer_hier_ref_id
        WHERE c.conn_kind='external_reference' AND h.access='connect'
          AND h.ref_path='u.g' AND h.resolved_net_name='g'""") == 1,
          "and v_hier_ref resolves the connection's outward tie")
    check(one("""
        SELECT count(*) FROM v_driver d
        JOIN v_tree_node t ON t.node_id = d.signal_inst_id
        WHERE t.node_name='u_sink' AND d.signal_name='p'
          AND d.driver_kind='constant'""") == 1,
          "the constant tiling the rest of that formal is still recorded")
    # The far side of the tie is a flat attachment too: g feeds u_sink's p
    # terminal through the resolved reference, so v_net_attachment answers
    # "which pin does g feed" the same way it would for a plain connection.
    check(one("""
        SELECT count(*) FROM v_net_attachment a
        JOIN v_net n ON n.net_id = a.net_id
        JOIN v_net_conn c ON c.conn_id = a.conn_id
        JOIN v_term t ON t.term_id = c.term_id
        JOIN v_tree_node tn ON tn.node_id = t.inst_id
        WHERE n.net_name='g' AND a.attachment_kind='actual_outside'
          AND t.term_name='p' AND tn.node_name='u_sink'""") == 1,
          "the resolved external tie shows g feeding u_sink.p as an attachment")
    # A condition gating a statement that writes nothing this instance
    # names is still a read of that signal.
    check(one("""
        SELECT count(*) FROM v_load
        WHERE signal_name='quiet_gate' AND load_kind='statement'""") == 1,
          "a condition gating a targetless statement is still a load")
    check(one("""
        SELECT count(*) FROM expr_ref e JOIN net n ON n.id = e.net_id
        WHERE n.name='quiet_gate' AND e.role='control'""") == 1,
          "and it is recorded as the control reference it is")
    # One reference split across two targets: the row names the whole of
    # what the RTL wrote, the dependencies take their own halves.
    check(one("""
        SELECT count(*) FROM hier_ref
        WHERE path='u.split' AND access='read'
          AND lo IS NULL AND hi IS NULL AND is_exact=1""") == 1,
          "a split reference is recorded whole, once")
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='split' AND tgt_name IN ('sp_hi','sp_lo')
          AND src_lo IS NOT NULL AND src_hi IS NOT NULL""") == 2,
          "while each dependency through it carries its own bits")
    # Two call sites reading outward: two statements, two references, each
    # dependency pointing at the one its own statement made.
    check(one("""
        SELECT count(*) FROM hier_ref WHERE path='u.x' AND access='read'""") >= 3,
          "each call site records its own outward reference")
    check(one("""
        SELECT count(DISTINCT d.stmt_id) FROM net_dep d
        JOIN hier_ref h ON h.id = d.src_hier_ref_id
        JOIN net t ON t.id = d.tgt_net_id
        WHERE t.name='seen'""") == 2,
          "and the two body statements read it independently")
    # A system task's write is a driver, told apart from a tie-off.
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name='loaded_mem' AND driver_kind='system_task'""") == 1,
          "a system task that writes its argument drives it")
    check(one("""
        SELECT count(*) FROM v_driver d JOIN v_stmt s
          ON s.stmt_id = d.stmt_id
        WHERE d.driver_kind='system_task' AND s.construct='$readmemh'""") == 2,
          "and each row names the call that did it")
    # A write whose source the schema cannot name AND whose target is in
    # another instance: the far net still has a driver, or a trace back
    # from it says nothing ever wrote it.
    check(one("""
        SELECT count(*) FROM v_driver d
        JOIN v_stmt s ON s.stmt_id = d.stmt_id
        WHERE d.signal_name='far_mem' AND d.driver_kind='system_task'
          AND d.signal_inst_id != s.inst_id""") == 1,
          "a system task writing across the boundary drives the far net")
    check(one("""
        SELECT count(*) FROM v_driver d
        JOIN v_stmt s ON s.stmt_id = d.stmt_id
        WHERE d.signal_name='tied' AND d.driver_kind='constant'
          AND d.signal_inst_id != s.inst_id""") == 1,
          "and a constant driving an outward target does too")
    # Both are reachable walking back from what reads them.
    check(one("""
        WITH RECURSIVE cone(net) AS (
            SELECT net_id FROM v_net WHERE net_name='far_o'
            UNION
            SELECT d.driver_net_id FROM cone c
            JOIN v_driver d ON d.signal_net_id = c.net
            WHERE d.driver_net_id IS NOT NULL)
        SELECT count(*) FROM cone
        JOIN v_net n ON n.net_id = cone.net
        WHERE n.net_name IN ('far_mem','tied')""") == 2,
          "and a fan-in cone reaches both rather than stopping short")
    # The design boundary is visible in both directions.
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name='a' AND driver_kind='terminal'""") == 1,
          "a top-level input drives its net as a terminal")
    check(one("""
        SELECT count(*) FROM v_load
        WHERE signal_name='q' AND load_kind='terminal'""") == 1,
          "a top-level output reads its net as a terminal")

if mode == "alias":
    # An alias binds nets into one object, so each side is the other's
    # driver AND the other's load -- and three names bound in one
    # statement bind every pair, not a chain.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE dep_kind='alias'""") == 6,
          "three aliased names bind all six ordered pairs")
    for a, b in (("left", "right"), ("right", "third"), ("left", "third")):
        check(one("""
            SELECT count(*) FROM v_driver
            WHERE signal_name=? AND driver_name=? AND driver_kind='alias'""",
                  a, b) == 1, f"{b} drives {a} through the alias")
        check(one("""
            SELECT count(*) FROM v_load
            WHERE signal_name=? AND load_name=? AND load_kind='alias'""",
                  a, b) == 1, f"and {b} reads {a} through it")
    # The statement is its own kind: an alias is not an assignment, and a
    # multiple-driver query must be able to leave it out.
    check(one("""
        SELECT count(*) FROM v_stmt
        WHERE stmt_kind='alias' AND construct='alias'
          AND assign_kind IS NULL AND proc_id IS NULL""") == 1,
          "the alias statement is its own kind, not an assignment")
    # What the whole thing is for: the trace crosses the alias.
    check(one("""
        WITH RECURSIVE f(n) AS (
            SELECT net_id FROM v_net WHERE net_name='in_side'
            UNION SELECT l.load_net_id FROM f
            JOIN v_load l ON l.signal_net_id = f.n
            WHERE l.load_net_id IS NOT NULL)
        SELECT count(*) FROM f JOIN v_net ON net_id = f.n
        WHERE net_name IN ('right','third','out_side')""") == 3,
          "and a trace from one side reaches the others")

if mode == "aliascat":
    # LRM 10.11 -- `alias {a, b} = c;`. A side had to yield exactly one
    # reference, and a concatenation is one expression yielding several, so
    # the statement carried no dependency at all and a and b were aliased to
    # nothing.
    #
    # Each reference is its own side, paired only against the OTHER written
    # side: a and b are different bits of c, never aliases of each other.
    check(one("""
        SELECT count(*) FROM v_net_dep WHERE dep_kind='alias'""") == 6,
          "the two statements bind three pairs, both directions")
    for x, y in (("a", "c"), ("b", "c")):
        check(one("""
            SELECT count(*) FROM v_net_dep
            WHERE dep_kind='alias' AND map_exact=0
              AND ((src_name=? AND tgt_name=?) OR (src_name=? AND tgt_name=?))
            """, x, y, y, x) == 2,
              f"{x} and {y} are bound both ways, mapping coarse")
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE dep_kind='alias'
          AND src_name IN ('a','b') AND tgt_name IN ('a','b')""") == 0,
          "and the two operands of one side are never bound to each other")
    # The control: a plain two-name alias keeps its exact mapping.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE dep_kind='alias' AND map_exact=1
          AND src_name IN ('c','d') AND tgt_name IN ('c','d')""") == 2,
          "while a plain two-name alias maps bit for bit")


if mode == "external":
    # LRM 23.8 -- an upward reference from a shared body. The level that
    # answers `tb_top.glob` is the occurrence's own, so both occurrences
    # resolve, and onto the one net the source names.
    check(one("""
        SELECT count(DISTINCT h.resolved_net_id) FROM hier_ref h
        JOIN net n ON n.id = h.resolved_net_id
        WHERE h.path='tb_top.glob' AND n.name='glob'""") == 1,
          "an upward reference from a shared body resolves to one net")
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE driver_kind='data' AND signal_name='up_o'
          AND driver_name='glob' AND driver_ref='tb_top.glob'""") == 2,
          "and drives both occurrences' outputs, naming its spelling")
    # The same climb from a CONDITION -- the half that carries a branch. It
    # resolves like the read beside it, so the gating is a control dependency
    # with a real source rather than one with none.
    check(one("""
        SELECT count(*) FROM hier_ref
        WHERE path='tb_top.gmode' AND branch_id IS NOT NULL
          AND resolved_net_id IS NOT NULL""") == 4,
          "an upward condition resolves and keeps its branch")
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE dep_kind='control' AND src_name='gmode' AND tgt_name='ug'
          AND src_net_id IS NOT NULL""") == 4,
          "and gates as a control dependency with a source net")
    check(one("""
        SELECT count(DISTINCT src_net_id) FROM v_net_dep
        WHERE dep_kind='control' AND src_name='gmode'""") == 1,
          "and both occurrences are gated by the one net the source names")
    # What the per-occurrence search finds need not be the KIND of thing the
    # analysed body found: `blk` is an instance above one occurrence and a
    # generate block above the other. A generate level holds no nets, so the
    # second resolves to nothing -- where a separate instance column could
    # have named the level itself and broken its own foreign key.
    check(one("""
        SELECT count(*) FROM hier_ref h
        JOIN net n ON n.id = h.resolved_net_id
        JOIN tree_node t ON t.id = n.inst_id
        WHERE h.path='blk.sig' AND t.node_kind='instance' AND n.name='sig'""") == 1,
          "an upward anchor that finds an instance resolves")
    check(one("""
        SELECT count(*) FROM hier_ref
        WHERE path='blk.sig' AND resolved_net_id IS NULL""") == 1,
          "and one that finds a generate block stays NULL")
    # A $unit object is what still leaves the model once packages and upward
    # names resolve: nothing stamps the compilation unit, so the dependency
    # carries a NULL source net and the reference on the source end.
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE driver_kind='external' AND signal_name='o'""") >= 1,
          "an output driven by a $unit object is external")
    # The window and the name, both off the v_driver row itself, so reading
    # them costs no dep_id -> net_dep -> hier_ref walk: two more queries per
    # external row, the second against a base table rather than a view.
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE driver_kind='external' AND signal_name='nib'
          AND driver_ref='cu_glob'
          AND driver_lo=0 AND driver_hi=3 AND driver_exact=1""") >= 1,
          "the windowed external read keeps its window and names what it reads")
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE driver_kind='external' AND driver_ref IS NULL""") == 0,
          "and every external row in this design names its reference")
    # The reference behind that spelling is reachable in contract now, and
    # says the thing the kind depends on: it did not resolve here.
    check(one("""
        SELECT count(*) FROM v_driver v
        JOIN v_net_dep d ON d.dep_id = v.dep_id
        JOIN v_hier_ref h ON h.hier_ref_id = d.src_hier_ref_id
        WHERE v.driver_kind='external' AND v.signal_name='nib'
          AND h.access='read' AND h.resolved_net_id IS NULL
          AND h.lo=0 AND h.hi=3""") >= 1,
          "and v_hier_ref holds the reference that row named")
    check(one("""
        SELECT count(*) FROM net_dep
        WHERE src_net_id IS NULL AND src_hier_ref_id IS NOT NULL
          AND dep_kind='control'""") >= 1,
          "an external condition gates as a control dependency with no source")
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE driver_kind='external' AND signal_name='g'""") >= 1,
          "so the externally gated target shows its external control")
    # o and nib are pure external reads -- no constant hides among their
    # drivers. (g legitimately also has constant drivers: the 8'hFF/8'h00 it
    # assigns under the external condition.)
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name IN ('o','nib') AND driver_kind='constant'""") == 0,
          "and no external source is misreported as a constant")

if mode == "callsite":
    # A constant actual ties the formal off, the same fact `.p(8'h5A)`
    # records on a pin. Without it the formal came back with the two
    # net-fed calls as its only drivers and this one invisible.
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name = 'tie.v' AND driver_kind = 'constant'
          AND driver_net_id IS NULL AND stmt_id IS NOT NULL""") == 1,
          "a constant actual drives its formal as a tie-off")
    # A source-less dependency is anchored by a target row here as
    # everywhere, so the two views agree on what the call writes.
    check(one("""
        SELECT count(*) FROM v_stmt_target t JOIN v_driver d
          ON d.stmt_id = t.stmt_id AND d.signal_net_id = t.net_id
        WHERE t.net_name = 'tie.v' AND d.driver_kind = 'constant'
          AND t.target_kind = 'written_by'""") == 1,
          "and the statement that ties it names it as a target")
    # Two calls to one task, from two sites -- both bump, both outermost.
    check(one("""SELECT count(*) FROM v_call_site
                 WHERE subroutine_name='bump' AND depth=1""") == 2,
          "the task is called from two call sites")
    # And each expansion carries the gating of ITS site: one body walked
    # twice is two sets of statements under two different levels, which is
    # the whole reason the walk is per call site rather than per subroutine.
    levels = sorted(con.execute("""
        SELECT cs.call_site_id, b.branch_kind, b.sense, b.src_line
        FROM v_stmt s
        JOIN v_call_site cs ON cs.call_site_id = s.call_site_id
        JOIN v_branch b ON b.branch_id = s.branch_id
        WHERE cs.subroutine_name='bump'""").fetchall())
    check(len(levels) == 2 and levels[0][3] != levels[1][3]
          and all(r[1] == "if" and r[2] == "then" for r in levels),
          "each expansion sits in the level of its own call site",
          f"got {levels}")
    # A call in a control expression (`pick(c)` in the condition) has no
    # owning statement: its call_site names no caller statement, and its
    # argument binding carries no call_site_id (the universal invariant
    # "a dependency in a call carries its statement" holds because such a
    # dependency is tagged only when it has a statement).
    check(one("""SELECT count(*) FROM v_call_site
                 WHERE subroutine_name='pick' AND caller_stmt_id IS NULL
                   AND depth=1""") == 1,
          "a control-expression call names no caller statement")
    check(one("""
        SELECT count(*) FROM net_dep d
        JOIN net f ON f.id = d.tgt_net_id
        WHERE f.name LIKE 'pick.%'
          AND d.stmt_id IS NULL
          AND d.call_site_id IS NOT NULL""") == 0,
          "and its statement-less binding carries no call_site_id")
    # The BODY is a different matter, and is tagged: `pick` is called in a
    # control expression, so the binding has no statement to hang on -- but the
    # `return` inside it does, and every row a body walk produces names the
    # site it was walked for. Asserting zero tagged rows for `pick` altogether
    # would hold only while the body contributed nothing at all.
    check(one("""
        SELECT count(*) FROM net_dep d
        JOIN call_site cs ON cs.id = d.call_site_id
        WHERE cs.subroutine_name='pick' AND d.stmt_id IS NOT NULL""") > 0,
          "while the body it walked is")
    # An output actual is written by the calling statement: the driver and
    # the statement-target row are two halves of one fact, and a reader that
    # asks either view must get the same answer.
    check(one("""SELECT count(*) FROM v_driver
                 WHERE signal_name='w' AND driver_kind='procedure'""") == 1,
          "an output actual has a procedure driver")
    check(one("""SELECT count(*) FROM v_stmt_target t JOIN net n ON n.id=t.net_id
                 WHERE n.name='w' AND t.target_kind='written_by'""") == 1,
          "and the calling statement records it as a target")

    # Each call's argument binds to the shared formal under its OWN site.
    check(one("""
        SELECT count(DISTINCT call_site_id) FROM v_net_dep
        WHERE tgt_name='bump.v' AND dep_kind='procedure'
          AND src_name IN ('a','b')""") == 2,
          "each argument binds the formal under its own call site")
    # The body's write is walked once per site: bump.v -> r under both.
    check(one("""
        SELECT count(DISTINCT call_site_id) FROM v_net_dep
        WHERE tgt_name='r' AND src_name='bump.v'""") == 2,
          "the body write is stamped once per call site")

    # The payoff, as a cone. Filtering r's fan-in to call site 1 (plus the
    # site-less module-level rows) reaches a and g1 -- call 1's real
    # combination -- and NEVER b or g2, the cross combination the shared
    # formal would otherwise admit.
    def cone(cs):
        return set(r[0] for r in con.execute(f"""
            WITH RECURSIVE c(n) AS (
                SELECT tgt_net_id FROM net_dep
                    WHERE tgt_net_id IN (SELECT net_id FROM v_net WHERE net_name='r')
                      AND call_site_id = {cs}
                UNION
                SELECT d.src_net_id FROM c JOIN net_dep d ON d.tgt_net_id = c.n
                    WHERE d.src_net_id IS NOT NULL
                      AND (d.call_site_id = {cs} OR d.call_site_id IS NULL))
            SELECT DISTINCT net_name FROM c JOIN v_net ON net_id = n""")) - {'r'}
    c1, c2 = cone(1), cone(2)
    check('a' in c1 and 'g1' in c1 and 'b' not in c1 and 'g2' not in c1,
          "call site 1's cone is a and g1, never b or g2", f"got {sorted(c1)}")
    check('b' in c2 and 'g2' in c2 and 'a' not in c2 and 'g1' not in c2,
          "call site 2's cone is b and g2, never a or g1", f"got {sorted(c2)}")

    # The same answer without touching a base table. A walk that begins at a
    # STATEMENT -- "which statements write r, and which call does each belong
    # to" -- is the side a consumer falls back to when a dependency is
    # missing, so v_stmt carries the tag too and the documented recipe is
    # executable from either end.
    sites_r = set(r[0] for r in con.execute("""
        SELECT t.call_site_id FROM v_stmt_target t
        JOIN v_net n ON n.net_id = t.net_id
        WHERE n.net_name = 'r' AND t.target_kind = 'written_by'"""))
    check({1, 2} <= sites_r and None in sites_r,
          "the writes of r are one per call site plus the module-level one",
          f"got {sorted(sites_r, key=lambda v: (v is not None, v))}")
    # The shared formal, from the read side: ONE net, two operand rows, and
    # only the tag separates them. A consumer that groups reads by net_id
    # here without it offers a combination no call makes -- the same mixing
    # call_site_id was introduced to prevent one layer down.
    sites_v = set(r[0] for r in con.execute("""
        SELECT o.call_site_id FROM v_stmt_operand o
        JOIN v_net n ON n.net_id = o.net_id
        WHERE n.net_name = 'bump.v'"""))
    check(sites_v == {1, 2},
          "the two reads of the shared formal are one per call site",
          f"got {sorted(sites_v, key=lambda v: (v is not None, v))}")
    check(one("""
        SELECT count(DISTINCT o.net_id) FROM v_stmt_operand o
        JOIN v_net n ON n.net_id = o.net_id
        WHERE n.net_name = 'bump.v'""") == 1,
          "over one formal net, which is what makes the tag necessary")
    def view_cone(cs):
        return set(r[0] for r in con.execute(f"""
            WITH RECURSIVE c(n) AS (
                SELECT t.net_id FROM v_stmt_target t
                JOIN v_stmt s ON s.stmt_id = t.stmt_id
                JOIN v_net n ON n.net_id = t.net_id
                WHERE n.net_name='r' AND t.target_kind='written_by'
                  AND s.call_site_id = {cs}
                UNION
                SELECT d.driver_net_id FROM c
                JOIN v_driver d ON d.signal_net_id = c.n
                WHERE d.driver_net_id IS NOT NULL
                  AND (d.call_site_id = {cs} OR d.call_site_id IS NULL))
            SELECT DISTINCT net_name FROM c JOIN v_net ON net_id = n""")) - {'r'}
    v1, v2 = view_cone(1), view_cone(2)
    check('a' in v1 and 'b' not in v1,
          "and a cone built only from views keeps call site 1's argument",
          f"got {sorted(v1)}")
    check('b' in v2 and 'a' not in v2,
          "and call site 2's", f"got {sorted(v2)}")

if mode == "package":
    # A package is parentless without being a root, so it anchors its own
    # path and contributes nothing to any instance path.
    check(one("""
        SELECT count(*) FROM v_node_path p JOIN v_tree_node t
          ON t.node_id = p.node_id
        WHERE t.node_kind = 'package' AND p.node_path = t.node_name""") == 1,
          "a package path is its own name and no more")
    check(one("""
        SELECT count(*) FROM v_node_path p JOIN v_tree_node t
          ON t.node_id = p.node_id
        WHERE t.node_kind != 'package'
          AND p.node_path LIKE (SELECT node_name FROM v_tree_node
                                WHERE node_kind = 'package') || '.%'""") == 0,
          "and no elaborated path runs through one")
    # A package is a pseudo-occurrence now: node_kind='package', a matching
    # inst with parent_inst_id NULL and a def_kind='package' module.
    check(one("""
        SELECT count(*) FROM v_tree_node
        WHERE node_kind='package' AND node_name='cfg_pkg'""") == 1,
          "the package is a tree node of its own kind")
    check(one("""
        SELECT count(*) FROM inst i JOIN tree_node t ON t.id=i.id
        JOIN module m ON m.id=i.module_id
        WHERE t.node_kind='package' AND i.parent_inst_id IS NULL
          AND m.def_kind='package'""") == 1,
          "with a parentless inst and a package module")
    # Its variables are nets of that occurrence.
    check(one("""
        SELECT count(*) FROM v_net n JOIN v_tree_node t ON t.node_id=n.inst_id
        WHERE t.node_kind='package' AND n.net_name IN ('mask','enable')""") == 2,
          "the package variables are nets")
    # The payoff: cfg_pkg::mask resolves to a real driver, not 'external',
    # and BOTH readers meet on the one package net.
    check(one("""
        SELECT count(*) FROM v_driver WHERE driver_kind='external'""") == 0,
          "no reference is left external once the package resolves")
    check(one("""
        SELECT count(DISTINCT driver_net_id) FROM v_driver
        WHERE driver_name='mask' AND driver_kind='data'""") == 1,
          "both readers are driven by the one package net")
    # Three readers now: the two modules, and the package's own task body,
    # whose write to `enable` reads `mask` from inside the package.
    check(one("""
        SELECT count(DISTINCT signal_inst_id) FROM v_driver
        WHERE driver_name='mask' AND driver_kind='data'""") == 3,
          "and there really are three distinct readers of it")
    check(one("""
        SELECT count(*) FROM hier_ref
        WHERE path LIKE 'cfg_pkg::%' AND resolved_net_id IS NOT NULL""") >= 1,
          "the pkg:: reference is recorded as written and resolved")
    # A task that lives in the package, called twice from one procedure. The
    # body is walked per call, so its statement rows come in two sets -- and
    # the statement layer is where a consumer meets code it did not write
    # itself, which is where "which call does this statement belong to" most
    # needs an answer.
    check(one("""
        SELECT count(*) FROM v_call_site
        WHERE subroutine_name='arm' AND depth=1""") == 2,
          "the package task is called from two call sites")
    sites = set(r[0] for r in con.execute("""
        SELECT s.call_site_id FROM v_stmt s
        JOIN v_call_site cs ON cs.call_site_id = s.call_site_id
        WHERE cs.subroutine_name='arm'"""))
    check(len(sites) == 2,
          "and its body's statements name one call site each",
          f"got {sorted(sites)}")
    check(one("""
        SELECT count(DISTINCT src_line) FROM v_stmt s
        JOIN v_call_site cs ON cs.call_site_id = s.call_site_id
        WHERE cs.subroutine_name='arm'""") == 1,
          "over one written statement, which is what the tag is for")

if mode == "incomplete":
    # ---- a definition that is missing
    check(status == "partial",
          "a missing definition leaves the export partial")
    # ---- an upward name that answers for one occurrence and not the other
    # Both surroundings hold an `anchor`; only one of them holds the net. The
    # occurrence that misses it resolves to NEITHER half -- the generic check
    # above rules out the third state, and this pins which of the two rows is
    # which, so a route that stops half way is caught here rather than by a
    # consumer.
    check(one("""
        SELECT count(*) FROM hier_ref h
        JOIN tree_node t ON t.id = h.inst_id
        JOIN net n ON n.id = h.resolved_net_id
        WHERE h.path='anchor.sig' AND n.name='sig'""") == 1,
          "an upward name resolves where the surroundings hold the net")
    check(one("""
        SELECT count(*) FROM hier_ref
        WHERE path='anchor.sig' AND resolved_net_id IS NULL""") == 1,
          "and does not resolve where they do not")
    check(one("""
        SELECT count(*) FROM tree_node t JOIN inst i ON i.id = t.id
        WHERE t.node_kind='unresolved' AND i.unresolved_def='ghost'""") == 1,
          "the black box names the definition it wanted")
    # Terminals for what the parent connected, direction unknown.
    check(one("""
        SELECT count(*) FROM term t JOIN tree_node n ON n.id = t.inst_id
        JOIN inst i ON i.id = n.id
        WHERE n.node_kind='unresolved' AND i.unresolved_def='ghost'
          AND t.direction IS NULL""") == 5,
          "the black box has a terminal per connection, direction unknown")
    check(one("""
        SELECT count(*) FROM net_conn c JOIN term t ON t.id = c.term_id
        JOIN tree_node n ON n.id = t.inst_id
        JOIN inst i ON i.id = n.id
        WHERE n.node_kind='unresolved' AND i.unresolved_def='ghost'
          AND c.conn_kind='signal'""") >= 3,
          "the connections that reach the black box are recorded")
    check(one("""
        SELECT count(*) FROM net_conn c JOIN term t ON t.id = c.term_id
        JOIN tree_node n ON n.id = t.inst_id
        JOIN inst i ON i.id = n.id
        WHERE n.node_kind='unresolved' AND i.unresolved_def='ghost'
          AND c.conn_kind='unconnected'""") == 1,
          "its unconnected pin is recorded as unconnected")
    # And ONLY that pin. A sequence connection is not a simple expression, so
    # it fell through with no expression at all and was recorded as absent --
    # a claim the parent wired nothing, on a pin it wired two nets to. The
    # leaves are recordable even when the shape is not.
    check(one("""
        SELECT count(*) FROM net_conn c JOIN term t ON t.id = c.term_id
        JOIN tree_node n ON n.id = t.inst_id
        WHERE n.node_kind='unresolved' AND t.name='seq'
          AND c.conn_kind='expression_operand'""") == 2,
          "a sequence connection names the nets it reaches")
    # The trace stops AT the box: mid still has its consumer.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='mid' AND tgt_name='gnt'""") == 1,
          "the design around the hole keeps its dataflow")
    # An instantiation with no instance name is named after its definition,
    # not after the instance holding it: `$def$n`, '$'-prefixed because no
    # identifier the source could write starts that way, and counted per
    # scope because siblings are what a name has to separate.
    check(one("""
        SELECT count(*) FROM tree_node t JOIN tree_node p
          ON p.id = t.parent_node_id
        WHERE t.name = p.name""") == 0,
          "no node takes the name of the node above it")
    check(one("""
        SELECT count(*) FROM (SELECT parent_node_id, name, count(*) c
                              FROM tree_node GROUP BY parent_node_id, name
                              HAVING c > 1)""") == 0,
          "and no two siblings share a name")
    check(int(meta["duplicate_path_count"]) == 0,
          "so the design reports no duplicate paths")
    # The names are synthesised, the diagnostics are not: an unnamed module
    # instantiation is still an elaboration error, and the export still says
    # the design did not fully compile.
    check(status == "partial",
          "and the export still reports what slang rejected")
    # The two in the top body, the one inside anon_mid, and one per
    # generate level.
    check(one("""
        SELECT count(*) FROM tree_node t JOIN inst i ON i.id = t.id
        JOIN module m ON m.id = i.module_id
        WHERE t.node_kind='instance' AND m.name='anon_leaf'
          AND t.name LIKE '$anon_leaf$%'""") == 5,
          "every unnamed instantiation is named from its definition")
    # A gate and an instantiation in one scope draw from ONE counter, so
    # the numbering is a single sequence per scope. Two counters could only
    # collide if a primitive and a module definition shared a name, which
    # the language does not allow -- one sequence means not having to say
    # so.
    check(one("""
        SELECT count(*) FROM tree_node t JOIN tree_node p
          ON p.id = t.parent_node_id
        WHERE p.name='u_anon' AND t.name IN
              ('$buf$0', '$anon_leaf$1', '$anon_leaf$2', '$anon_ghost$3')
        """) == 4,
          "the gate and the instantiations beside it number consecutively")
    # Per scope, not per instance: each generate element restarts at 0, and
    # the two are siblings of nothing.
    check(one("""
        SELECT count(*) FROM tree_node t JOIN tree_node p
          ON p.id = t.parent_node_id
        WHERE p.node_kind='generate' AND p.name IN ('g[0]', 'g[1]')
          AND t.name='$anon_leaf$0'""") == 2,
          "a generate level counts its own children")
    # An unnamed instantiation of a definition that is missing too: the
    # black box keeps both its synthesised segment and its definition name.
    check(one("""
        SELECT count(*) FROM tree_node t JOIN inst i ON i.id = t.id
        WHERE t.node_kind='unresolved' AND t.name='$anon_ghost$3'
          AND i.unresolved_def='anon_ghost'""") == 1,
          "an unnamed black box keeps the definition it wanted")
    # The names are segments like any other, so the tree still walks:
    # anonymous.u_mid.$anon_leaf$0 is three (parent_node_id, name) lookups.
    check(one("""
        SELECT count(*) FROM tree_node t
        JOIN tree_node p ON p.id = t.parent_node_id
        JOIN tree_node g ON g.id = p.parent_node_id
        WHERE g.name='u_anon' AND p.name='u_mid'
          AND t.name='$anon_leaf$0'""") == 1,
          "and a path resolves through one segment per level")


if mode == "recursion":
    # Illegal RTL that slang rejects, so the database is hierarchy-only by
    # the same path any fatally-errored compilation takes. Asserted first:
    # everything below is about a tree built without dataflow.
    check(meta["analysis_status"] == "hierarchy_only",
          "a recursive hierarchy is a fatally errored compilation",
          f"got {meta['analysis_status']!r}")

    # The contract the fix carries: an instance whose module AND parameters
    # are already those of one of its own ancestors is stamped, and stops
    # there. Ancestry by the tree_node chain, module identity by module_id --
    # not by name, since two libraries may define one name -- and the
    # parameters with it, because the guard keys on the pair. A finite
    # parameterised recursion repeats the module and never the pair, which is
    # why it is stamped whole; the paramrec half of params.sv is that control.
    ANCESTORS = """
        WITH RECURSIVE anc(node, ancestor) AS (
            SELECT id, parent_node_id FROM tree_node
            WHERE parent_node_id IS NOT NULL
          UNION ALL
            SELECT a.node, t.parent_node_id FROM anc a
            JOIN tree_node t ON t.id = a.ancestor
            WHERE t.parent_node_id IS NOT NULL)
        SELECT %s FROM anc a
        JOIN inst i  ON i.id = a.node
        JOIN inst ia ON ia.id = a.ancestor
        WHERE i.module_id = ia.module_id
          AND i.param_signature IS ia.param_signature"""

    # Not vacuous: the file has three of them -- selfchain's one child and
    # selffan's two, which are the two shapes the two guards fail on
    # differently, a stack overflow and a walk that never returns.
    check(one(ANCESTORS % "count(DISTINCT a.node)") == 3,
          "three instances re-enter a module of their own ancestry")
    # And the count is in the FILE, not only on stderr, so a consumer holding
    # a truncated database can tell it from a whole one.
    check(int(meta["recursion_count"]) == 3,
          "meta records all three", f"got {meta['recursion_count']}")
    # No INSTANCE below a cut one, at any depth. Its generate scopes and its
    # primitives are stamped -- those come before the guard and are part of
    # the level the cut keeps -- so a check over children of every kind would
    # forbid what the fix deliberately preserves.
    check(one(ANCESTORS % "count(*)" + """
          AND EXISTS (SELECT 1 FROM anc d JOIN inst di ON di.id = d.node
                      WHERE d.ancestor = a.node)""") == 0,
          "and no instance stands below one of them")
    # The whole tree, so an unrolled level is a failure and not just an
    # unasserted extra: root, two children, three cut leaves.
    check(one("SELECT count(*) FROM tree_node") == 6,
          "the tree is the design plus one level of each recursion")

    # Cut, not dropped: each keeps the terminals its module declares and the
    # connections its parent wrote to them, so a trace reaches the recursion
    # and stops AT it rather than losing the wires that arrive.
    check(one(ANCESTORS % "count(*)" + """
          AND (SELECT count(*) FROM term WHERE inst_id = a.node) != 2""") == 0,
          "each cut instance keeps both its terminals")
    check(one(ANCESTORS % "count(*)" + """
          AND (SELECT count(*) FROM net_conn c JOIN term t ON t.id = c.term_id
               WHERE t.inst_id = a.node) != 2""") == 0,
          "and both connections its parent made to them")

finish()
