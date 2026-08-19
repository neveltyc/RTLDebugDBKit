#!/usr/bin/env python3
# Copyright (c) 2026 neveltyc
# released under the BSD 3-Clause License (see LICENSE)
#
# Read an exported database back and fail if it is hollow or malformed. A
# build that links proves the slang pin resolves; this proves the exporter
# still writes rows -- and that the rows keep every contract the schema
# documents: the subtype bijections, the ownership rules, the provenance
# matrix behind net_dep, the range discipline, and the twelve stable views
# with their exact columns and row formulas. CI runs it against examples/ on
# every platform binary it builds, so the mode branches assert only what
# those small designs must produce, not exact counts.
#
#   verify-designdb.py <design.db>              the universal checks
#   verify-designdb.py <design.db> constructs   + examples/constructs/constructs.sv facts
#   verify-designdb.py <design.db> interfaces   + examples/constructs/interfaces.sv facts
#   verify-designdb.py <design.db> assertions   + examples/constructs/assertions.sv facts
#   verify-designdb.py <design.db> hierarchy    + examples/constructs/hierarchy.sv facts
#   verify-designdb.py <design.db> udp          + examples/constructs/udp.sv facts
#   verify-designdb.py <design.db> unresolved   + examples/constructs/unresolved.sv facts
#   verify-designdb.py <design.db> xmr          + examples/constructs/xmr.sv facts
import sqlite3
import sys

MODES = ("constructs", "interfaces", "assertions", "hierarchy", "udp",
         "unresolved", "xmr")
if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] not in MODES):
    sys.exit(f"usage: {sys.argv[0]} <design.db> [{'|'.join(MODES)}]")

con = sqlite3.connect(sys.argv[1])
mode = sys.argv[2] if len(sys.argv) == 3 else None

SCHEMA_VERSION = "10"


def one(sql, *args):
    return con.execute(sql, args).fetchone()[0]


def check(ok, msg, detail=""):
    if not ok:
        sys.exit(f"FAIL: {msg}" + (f" ({detail})" if detail else ""))
    print(f"ok: {msg}")


# ----------------------------------------------------------------- hollow
counts = {
    t: one(f'SELECT count(*) FROM "{t}"')
    for t in ("module", "tree_node", "inst", "net", "term", "term_map")
}
print("row counts:", counts)
empty = [t for t, n in counts.items() if n == 0]
if empty:
    sys.exit(f"exported database has no rows in: {', '.join(empty)}")

baddig = one("SELECT count(*) FROM source_file "
             "WHERE digest IS NULL OR length(digest) != 64")
total_sf = one("SELECT count(*) FROM source_file")
if total_sf == 0 or baddig:
    sys.exit(f"{baddig} of {total_sf} source_file row(s) lack a SHA-256 digest")

# Structural integrity -- catches corruption and generator bugs. The writer
# leaves foreign_keys off for speed, so this is where the REFERENCES clauses
# are actually enforced.
ic = one("PRAGMA integrity_check")
if ic != "ok":
    sys.exit(f"integrity_check failed: {ic}")
con.execute("PRAGMA foreign_keys = ON")
fk_errs = con.execute("PRAGMA foreign_key_check").fetchall()
if fk_errs:
    sys.exit(f"foreign_key_check failed: {len(fk_errs)} violation(s), "
             f"first: table={fk_errs[0][0]} rowid={fk_errs[0][1]}")
print("ok: integrity_check and foreign_key_check pass")

# ---------------------------------------------------------- value domains
# The DDL carries CHECK constraints for the closed enums; re-checking here
# catches a database written by a producer that dropped them, and covers the
# open sets (declaration_kind) and the NULL-required combinations CHECK
# cannot express.
for tbl, col, values, nullable in (
    ("module", "definition_kind", ("module", "interface", "program", "checker"), False),
    ("tree_node", "node_kind", ("root", "instance", "generate", "primitive", "unresolved"), False),
    ("primitive", "primitive_kind", ("gate", "switch", "udp"), False),
    ("term", "terminal_kind", ("signal", "interface"), False),
    ("term", "direction", ("input", "output", "inout", "ref"), True),
    ("net_conn", "connection_kind",
     ("signal", "constant", "unconnected", "expression_operand", "interface",
      "external_reference"), False),
    ("procedure", "procedure_kind",
     ("always", "always_ff", "always_comb", "always_latch", "initial", "final",
      "task", "function"), False),
    ("stmt", "statement_kind",
     ("assignment", "assertion", "wait", "call", "system_task", "event_control"), False),
    ("stmt", "assignment_kind", ("continuous", "blocking", "nonblocking"), True),
    ("expr_ref", "role",
     ("control", "assertion", "wait", "event", "call_argument", "system_task"), False),
    ("proc_event", "event_kind", ("sensitivity", "wait"), False),
    ("proc_event", "edge_kind", ("posedge", "negedge", "both"), True),
    ("net_dep", "dependency_kind", ("data", "control", "primitive", "procedure"), False),
    ("hier_ref", "access", ("read", "write", "connect"), False),
):
    qs = ",".join("?" for _ in values)
    null = f'OR "{col}" IS NULL' if not nullable else ""
    bad = one(f'SELECT count(*) FROM "{tbl}" WHERE "{col}" NOT IN ({qs}) {null}',
              *values)
    if bad:
        sys.exit(f"{tbl}.{col}: {bad} row(s) outside its value domain")
print("ok: every enum column stays in its domain")

for tbl, cols in (
    ("net", ("is_implicit",)),
    ("term", ("is_const",)),
    ("term_map", ("term_exact", "net_exact", "mapping_exact")),
    ("net_conn", ("net_exact", "term_exact", "mapping_exact")),
    ("assign_target", ("is_exact",)),
    ("assign_operand", ("is_exact",)),
    ("expr_ref", ("is_exact",)),
    ("net_dep", ("source_exact", "target_exact", "mapping_exact")),
    ("hier_ref", ("is_exact",)),
):
    for col in cols:
        bad = one(f'SELECT count(*) FROM "{tbl}" WHERE "{col}" NOT IN (0,1) '
                  f'AND "{col}" IS NOT NULL')
        if bad:
            sys.exit(f"{tbl}.{col}: {bad} value(s) outside 0/1/NULL")
print("ok: boolean columns are 0/1/NULL")

# ------------------------------------------------------- range discipline
# A range's lo and hi are both present or both absent, lo <= hi, and a range
# with real endpoints always says whether it is exact. NULL bits with
# exact=1 is the whole object; NULL bits with exact=0 is somewhere inside
# it; both spellings need the exact bit to be readable at all.
for tbl, lo, hi, exact in (
    ("term_map", "term_lo", "term_hi", "term_exact"),
    ("term_map", "net_lo", "net_hi", "net_exact"),
    ("net_conn", "net_lo", "net_hi", "net_exact"),
    ("net_conn", "term_lo", "term_hi", "term_exact"),
    ("assign_target", "lo", "hi", "is_exact"),
    ("assign_operand", "lo", "hi", "is_exact"),
    ("expr_ref", "lo", "hi", "is_exact"),
    ("net_dep", "source_lo", "source_hi", "source_exact"),
    ("net_dep", "target_lo", "target_hi", "target_exact"),
    ("hier_ref", "lo", "hi", "is_exact"),
):
    bad = one(f'SELECT count(*) FROM "{tbl}" '
              f'WHERE ("{lo}" IS NULL) != ("{hi}" IS NULL)')
    if bad:
        sys.exit(f"{tbl}: {bad} range(s) with only one endpoint ({lo}/{hi})")
    bad = one(f'SELECT count(*) FROM "{tbl}" WHERE "{lo}" > "{hi}"')
    if bad:
        sys.exit(f"{tbl}: {bad} range(s) with {lo} > {hi}")
    bad = one(f'SELECT count(*) FROM "{tbl}" '
              f'WHERE "{lo}" IS NOT NULL AND "{exact}" IS NULL')
    if bad:
        sys.exit(f"{tbl}: {bad} range(s) with endpoints but NULL {exact}")
print("ok: range lo/hi pair up, lo <= hi, endpoints imply exact")

# -------------------------------------------------- tree and the subtypes
check(one("SELECT count(*) FROM tree_node WHERE (parent_node_id IS NULL) != "
          "(node_kind = 'root')") == 0,
      "root nodes are exactly the parentless ones")
check(one("""
    SELECT count(*) FROM tree_node t
    WHERE (t.node_kind IN ('root','instance','unresolved'))
          != EXISTS (SELECT 1 FROM inst i WHERE i.id = t.id)""") == 0,
      "instance-like nodes have inst rows, others do not")
check(one("""
    SELECT count(*) FROM tree_node t
    WHERE (t.node_kind = 'primitive')
          != EXISTS (SELECT 1 FROM primitive p WHERE p.id = t.id)""") == 0,
      "primitive nodes have primitive rows, others do not")
check(one("""
    SELECT count(*) FROM tree_node t JOIN inst i ON i.id = t.id
    WHERE (t.node_kind = 'unresolved') != (i.module_id IS NULL)""") == 0,
      "unresolved is exactly module_id NULL")
check(one("""
    SELECT count(*) FROM tree_node t JOIN inst i ON i.id = t.id
    WHERE t.node_kind = 'unresolved' AND (i.unresolved_definition IS NULL
          OR i.parameter_signature IS NOT NULL)""") == 0,
      "an unresolved inst names its definition and no parameters")
check(one("SELECT count(*) FROM inst WHERE (parent_inst_id IS NULL) != "
          "(id IN (SELECT id FROM tree_node WHERE node_kind='root'))") == 0,
      "the root instances are exactly the parentless inst rows")

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
    SELECT count(*) FROM primitive p JOIN nearest n ON n.node = p.id
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
for tbl in ("net", "procedure", "stmt"):
    check(one(SCOPE_OWNER + f"""
        SELECT count(*) FROM "{tbl}" x JOIN owner o ON o.node = x.scope_node_id
        WHERE o.inst != x.inst_id""") == 0,
          f"{tbl}.scope_node_id lies inside its own instance")
check(one("SELECT count(*) FROM tree_node WHERE instr(name, '.') > 0") == 0,
      "every tree node name is a single path segment")

# ------------------------------------------------------------- ownership
check(one("""
    SELECT count(*) FROM term_map m
    JOIN term t ON t.id = m.term_id JOIN net n ON n.id = m.net_id
    WHERE t.inst_id != n.inst_id""") == 0,
      "term_map stays inside one instance")
check(one("""
    SELECT count(*) FROM net_conn c
    JOIN term t ON t.id = c.term_id
    JOIN net n ON n.id = c.net_id
    JOIN inst child ON child.id = t.inst_id
    WHERE n.inst_id != child.parent_inst_id""") == 0,
      "a connection's net belongs to the terminal's parent instance")
for tbl in ("assign_target", "assign_operand", "expr_ref"):
    check(one(f"""
        SELECT count(*) FROM "{tbl}" x
        JOIN stmt s ON s.id = x.stmt_id JOIN net n ON n.id = x.net_id
        WHERE s.inst_id != n.inst_id""") == 0,
          f"{tbl} references nets of its statement's instance")
check(one("""
    SELECT count(*) FROM proc_event e
    JOIN procedure p ON p.id = e.procedure_id
    LEFT JOIN net n ON n.id = e.net_id
    LEFT JOIN stmt s ON s.id = e.stmt_id
    WHERE (n.id IS NOT NULL AND n.inst_id != p.inst_id)
       OR (s.id IS NOT NULL AND COALESCE(s.procedure_id, 0) != e.procedure_id)""") == 0,
      "proc_event stays inside its procedure")
check(one("""
    SELECT count(*) FROM proc_event
    WHERE (event_kind = 'sensitivity') != (stmt_id IS NULL)""") == 0,
      "sensitivity events belong to the header, waits to a statement")

# ------------------------------------------------------- statement rules
check(one("""
    SELECT count(*) FROM stmt
    WHERE (statement_kind = 'assignment') != (assignment_kind IS NOT NULL)""") == 0,
      "assignment_kind is set exactly on assignments")
# One direction only: a continuous assignment is never inside a procedure,
# but a procedure-less blocking/nonblocking row is legal -- a function body
# reached from an `assign` keeps its own `=`, and executes in no procedure.
check(one("""
    SELECT count(*) FROM stmt
    WHERE assignment_kind = 'continuous' AND procedure_id IS NOT NULL""") == 0,
      "a continuous assignment is never inside a procedure")
check(one("""
    SELECT count(*) FROM stmt
    WHERE procedure_id IS NULL AND sequence IS NOT NULL""") == 0,
      "sequence never exists outside a procedure")
check(one("""
    SELECT count(*) FROM stmt
    WHERE sequence IS NULL AND procedure_id IS NOT NULL
      AND statement_kind != 'event_control'""") == 0,
      "inside a procedure only the header's event_control lacks a sequence")
check(one("""
    SELECT count(*) FROM expr_ref e JOIN stmt s ON s.id = e.stmt_id
    WHERE CASE e.role
        -- A condition gates whatever statement it encloses, including one
        -- that writes nothing this instance names.
        WHEN 'control'     THEN 0
        WHEN 'assertion'   THEN s.statement_kind != 'assertion'
        WHEN 'wait'        THEN s.statement_kind NOT IN ('wait', 'event_control')
        WHEN 'event'       THEN s.statement_kind != 'event_control'
        WHEN 'system_task' THEN s.statement_kind != 'system_task'
        WHEN 'call_argument' THEN s.statement_kind NOT IN
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
    WHERE CASE d.dependency_kind
        WHEN 'data' THEN d.stmt_id IS NULL
             OR d.expr_ref_id IS NOT NULL OR d.primitive_id IS NOT NULL
             OR (d.assign_target_id IS NULL) = (d.target_hier_ref_id IS NULL)
             OR (d.source_net_id IS NULL AND (d.assign_operand_id IS NOT NULL
                  OR d.source_hier_ref_id IS NOT NULL))
             OR (d.source_net_id IS NOT NULL AND
                 (d.assign_operand_id IS NULL) = (d.source_hier_ref_id IS NULL))
        WHEN 'control' THEN d.stmt_id IS NULL
             -- A condition always has a source: without this, a control row
             -- with source_net_id NULL passed every check and surfaced in
             -- v_driver as a CONSTANT tie-off on a gated signal.
             OR d.source_net_id IS NULL
             OR d.assign_operand_id IS NOT NULL OR d.primitive_id IS NOT NULL
             OR (d.expr_ref_id IS NULL) = (d.source_hier_ref_id IS NULL)
             OR (d.assign_target_id IS NULL) = (d.target_hier_ref_id IS NULL)
             -- NULL-safe: `NULL != 0` is NULL, so the plain comparison read
             -- as "0 or NULL" and let an unset mapping through.
             OR d.mapping_exact IS NOT 0
        WHEN 'primitive' THEN d.primitive_id IS NULL OR d.stmt_id IS NOT NULL
             OR d.assign_target_id IS NOT NULL OR d.assign_operand_id IS NOT NULL
             OR d.expr_ref_id IS NOT NULL OR d.source_hier_ref_id IS NOT NULL
             OR d.target_hier_ref_id IS NOT NULL
        WHEN 'procedure' THEN d.primitive_id IS NOT NULL
             OR d.assign_target_id IS NOT NULL OR d.assign_operand_id IS NOT NULL
             OR d.source_net_id IS NULL
             -- The reading side names where the actual came from, exactly
             -- as the doc promises: an argument reference or a resolved
             -- outward one. The write-back direction (formal -> actual) has
             -- neither, and is told apart by the formal being the source.
             OR (d.expr_ref_id IS NOT NULL AND d.source_hier_ref_id IS NOT NULL)
        ELSE 1 END""") == 0,
      "net_dep provenance columns match dependency_kind")
check(one("""
    SELECT count(*) FROM net_dep d JOIN hier_ref h ON h.id = d.source_hier_ref_id
    WHERE h.resolved_net_id IS NULL OR h.resolved_net_id != d.source_net_id""") == 0,
      "a hierarchical source copies its reference's resolution")
# The reference a dependency crossed through is the one its own statement
# made. Sharing rows across statements -- a task body walked once per call
# site, a condition gating several statements -- left the second statement
# pointing at the first's reference, so "what does this statement read
# outside the instance" answered nothing.
check(one("""
    SELECT count(*) FROM net_dep d
    JOIN hier_ref h ON h.id IN (d.source_hier_ref_id, d.target_hier_ref_id)
    WHERE d.stmt_id IS NOT NULL AND h.stmt_id IS NOT NULL
      AND h.stmt_id != d.stmt_id""") == 0,
      "a dependency's reference belongs to its own statement")
check(one("""
    SELECT count(*) FROM net_dep d JOIN hier_ref h ON h.id = d.target_hier_ref_id
    WHERE h.resolved_net_id IS NULL OR h.resolved_net_id != d.target_net_id""") == 0,
      "a hierarchical target copies its reference's resolution")
check(one("""
    SELECT count(*) FROM net_dep d
    WHERE d.source_net_id IS NULL AND (d.source_lo IS NOT NULL
       OR d.source_exact IS NOT NULL OR d.mapping_exact IS NOT NULL)""") == 0,
      "a constant dependency describes no source end")
check(one("""
    SELECT count(*) FROM net_dep d JOIN assign_operand o ON o.id = d.assign_operand_id
    WHERE o.net_id != d.source_net_id OR o.stmt_id != d.stmt_id""") == 0,
      "net_dep's operand copy agrees with the operand row")
check(one("""
    SELECT count(*) FROM net_dep d JOIN assign_target t ON t.id = d.assign_target_id
    WHERE t.net_id != d.target_net_id OR t.stmt_id != d.stmt_id""") == 0,
      "net_dep's target copy agrees with the target row")
check(one("""
    SELECT count(*) FROM net_dep d JOIN expr_ref e ON e.id = d.expr_ref_id
    WHERE e.net_id != d.source_net_id
       OR (d.stmt_id IS NOT NULL AND e.stmt_id != d.stmt_id)
       OR (d.dependency_kind = 'control' AND e.role != 'control')
       OR (d.dependency_kind = 'procedure' AND e.role != 'call_argument')""") == 0,
      "net_dep's expression reference agrees with the expr_ref row")
# Locality holds exactly where no end went through a hierarchical
# reference; a resolved cross-instance dependency is the point of v10's
# occurrence model, not a violation of it.
check(one("""
    SELECT count(*) FROM net_dep d
    JOIN net s ON s.id = d.source_net_id JOIN net t ON t.id = d.target_net_id
    JOIN stmt st ON st.id = d.stmt_id
    WHERE d.dependency_kind IN ('data','control')
      AND d.source_hier_ref_id IS NULL AND d.target_hier_ref_id IS NULL
      AND (s.inst_id != st.inst_id OR t.inst_id != st.inst_id)""") == 0,
      "purely local dependencies stay inside one instance")
check(one("""
    SELECT count(*) FROM assign_target a
    WHERE NOT EXISTS (SELECT 1 FROM net_dep d WHERE d.assign_target_id = a.id)
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
    JOIN term t ON t.id = m.term_id JOIN net n ON n.id = m.net_id
    WHERE m.mapping_exact = 1
      AND COALESCE(m.term_hi - m.term_lo + 1, t.width) IS NOT NULL
      AND COALESCE(m.net_hi - m.net_lo + 1, n.width) IS NOT NULL
      AND COALESCE(m.term_hi - m.term_lo + 1, t.width)
          != COALESCE(m.net_hi - m.net_lo + 1, n.width)""") == 0,
      "an exact term_map maps equal widths")
# The same rule on dependencies, and it is the one that matters most: a
# one-to-one claim between ends of different widths is not coarse, it is
# impossible. `assign swap = {c[3:0], c[7:4]}` used to export two rows each
# claiming all eight bits of swap from a four-bit source -- provably false,
# and every column in it well formed.
check(one("""
    SELECT count(*) FROM net_dep d
    JOIN net s ON s.id = d.source_net_id JOIN net t ON t.id = d.target_net_id
    WHERE d.mapping_exact = 1 AND d.source_exact = 1 AND d.target_exact = 1
      AND COALESCE(d.source_hi - d.source_lo + 1, s.width) IS NOT NULL
      AND COALESCE(d.target_hi - d.target_lo + 1, t.width) IS NOT NULL
      AND COALESCE(d.source_hi - d.source_lo + 1, s.width)
          != COALESCE(d.target_hi - d.target_lo + 1, t.width)""") == 0,
      "a one-to-one dependency maps equal widths")
check(one("""
    SELECT count(*) FROM net_conn c
    JOIN term t ON t.id = c.term_id LEFT JOIN net n ON n.id = c.net_id
    WHERE c.mapping_exact = 1 AND c.term_exact = 1 AND c.net_exact = 1
      AND COALESCE(c.term_hi - c.term_lo + 1, t.width) IS NOT NULL
      AND COALESCE(c.net_hi - c.net_lo + 1, n.width) IS NOT NULL
      AND COALESCE(c.term_hi - c.term_lo + 1, t.width)
          != COALESCE(c.net_hi - c.net_lo + 1, n.width)""") == 0,
      "an exact connection maps equal widths")
# A one-to-one window is exclusive: two segments both claiming a per-bit
# mapping cannot share formal bits. Expression operands are exempt by
# construction -- several reads legitimately feed one element's window, and
# their mapping_exact is 0.
for tbl in ("term_map", "net_conn"):
    check(one(f"""
        SELECT count(*) FROM "{tbl}" a JOIN "{tbl}" b
          ON a.term_id = b.term_id AND a.ordinal < b.ordinal
        WHERE a.mapping_exact = 1 AND b.mapping_exact = 1
          AND a.term_exact = 1 AND b.term_exact = 1
          AND a.term_lo IS NOT NULL AND b.term_lo IS NOT NULL
          AND a.term_lo <= b.term_hi AND b.term_lo <= a.term_hi""") == 0,
          f"one-to-one {tbl} windows on one terminal do not overlap")

# -------------------------------------------------------------- hier_ref
check(one("""
    SELECT count(*) FROM hier_ref h JOIN net n ON n.id = h.resolved_net_id
    WHERE h.resolved_inst_id IS NULL OR n.inst_id != h.resolved_inst_id""") == 0,
      "a resolved net lies inside its resolved instance")
check(one("""
    SELECT count(*) FROM net_conn c JOIN hier_ref h ON h.id = c.hier_ref_id
    WHERE h.access != 'connect'""") == 0,
      "a connection's outward tie is access='connect'")
# A signal connection to a resolved terminal always states its mapping; to
# an unresolved instance's terminal there is no formal end to correspond
# with, and NULL is the honest value.
check(one("""
    SELECT count(*) FROM net_conn c
    WHERE CASE c.connection_kind
        WHEN 'signal' THEN c.net_id IS NULL OR (c.mapping_exact IS NULL
             AND NOT EXISTS (SELECT 1 FROM term t JOIN tree_node n
                             ON n.id = t.inst_id
                             WHERE t.id = c.term_id
                               AND n.node_kind = 'unresolved'))
        WHEN 'expression_operand' THEN
             (c.net_id IS NULL) = (c.hier_ref_id IS NULL)
             OR (c.net_id IS NOT NULL AND c.mapping_exact != 0)
        WHEN 'constant' THEN c.net_id IS NOT NULL OR c.mapping_exact IS NOT NULL
        WHEN 'unconnected' THEN c.net_id IS NOT NULL OR c.mapping_exact IS NOT NULL
        WHEN 'interface' THEN c.net_id IS NOT NULL
        WHEN 'external_reference' THEN c.net_id IS NOT NULL OR c.hier_ref_id IS NULL
        ELSE 1 END""") == 0,
      "connection columns match connection_kind")

# ------------------------------------------------------------------ meta
required = ["schema_version", "analysis_status", "error_count",
            "unresolved_count", "empty_procedure_count", "duplicate_path_count",
            "tool", "tool_version", "slang_version", "producer_revision",
            "config_digest"]
meta = dict(con.execute("SELECT key, value FROM meta"))
missing = [k for k in required if k not in meta or meta[k] is None]
if missing:
    sys.exit(f"meta lacks required key(s): {', '.join(missing)}")
if meta["schema_version"] != SCHEMA_VERSION:
    sys.exit(f"schema_version is {meta['schema_version']}, expected {SCHEMA_VERSION}")
for k in ("error_count", "unresolved_count", "empty_procedure_count",
          "duplicate_path_count"):
    if not meta[k].isdigit():
        sys.exit(f"meta.{k} is not a number: {meta[k]!r}")
status = meta["analysis_status"]
if status not in ("complete", "partial", "hierarchy_only"):
    sys.exit(f"analysis_status is {status!r}")
if status == "complete" and (int(meta["error_count"]) or
                             int(meta["empty_procedure_count"]) or
                             int(meta["duplicate_path_count"])):
    sys.exit("analysis_status says complete beside non-zero counts")
print("ok: meta seal present and self-consistent "
      f"(analysis_status={status})")

info = con.execute("SELECT * FROM v_database_info").fetchone()
info_cols = [d[0] for d in con.execute("SELECT * FROM v_database_info LIMIT 0").description]
by = dict(zip(info_cols, info))
if str(by["schema_version"]) != meta["schema_version"] or \
   not isinstance(by["schema_version"], int):
    sys.exit("v_database_info.schema_version disagrees with meta or is not INTEGER")
for k in ("error_count", "unresolved_count", "empty_procedure_count",
          "duplicate_path_count"):
    if by[k] != int(meta[k]) or not isinstance(by[k], int):
        sys.exit(f"v_database_info.{k} disagrees with meta or is not INTEGER")
print("ok: v_database_info agrees with meta and casts its counts")

# --------------------------------------------------------- view contract
# The twelve stable views: existence, exact columns in exact order, and row
# formulas. v_conn_arc is scaffolding, not contract, and is deliberately
# absent from this list.
VIEW_COLUMNS = {
    "v_database_info": [
        "schema_version", "tool_version", "slang_version", "producer_revision",
        "top", "analysis_status", "error_count", "unresolved_count",
        "empty_procedure_count", "duplicate_path_count", "config_digest"],
    "v_tree_node": [
        "node_id", "parent_node_id", "node_name", "node_kind", "ordinal",
        "instance_id", "parent_instance_id", "module_id", "module_name",
        "parameter_signature", "definition_name", "file_path", "source_path",
        "source_line", "source_column"],
    "v_net": [
        "net_id", "instance_id", "module_id", "module_name",
        "parameter_signature", "scope_node_id", "net_name", "declaration_kind",
        "data_type", "width", "is_implicit", "file_path", "source_path",
        "source_line", "source_column"],
    "v_terminal": [
        "terminal_id", "instance_id", "module_id", "module_name",
        "terminal_name", "terminal_kind", "direction", "data_type", "width",
        "ordinal", "is_const", "modport", "file_path", "source_path",
        "source_line", "source_column"],
    "v_terminal_map": [
        "terminal_id", "terminal_instance_id", "terminal_name",
        "mapping_ordinal", "internal_net_id", "internal_net_name",
        "terminal_lo", "terminal_hi", "terminal_exact", "net_lo", "net_hi",
        "net_exact", "mapping_exact"],
    "v_net_connection": [
        "connection_id", "net_id", "net_instance_id", "net_name",
        "terminal_id", "terminal_instance_id", "terminal_name", "direction",
        "connection_kind", "ordinal", "net_lo", "net_hi", "net_exact",
        "terminal_lo", "terminal_hi", "terminal_exact", "mapping_exact",
        "interface_instance_id", "hier_ref_id", "file_path", "source_path",
        "source_line", "source_column"],
    "v_net_dependency": [
        "dependency_id", "source_net_id", "source_instance_id", "source_name",
        "source_lo", "source_hi", "source_exact", "target_net_id",
        "target_instance_id", "target_name", "target_lo", "target_hi",
        "target_exact", "statement_id", "assign_operand_id",
        "assign_target_id", "expression_reference_id", "primitive_id",
        "source_hier_ref_id", "target_hier_ref_id",
        "dependency_kind", "mapping_exact", "file_path", "source_path",
        "source_line", "source_column"],
    "v_driver": [
        "signal_net_id", "signal_instance_id", "signal_name", "signal_lo",
        "signal_hi", "signal_exact", "driver_net_id", "driver_instance_id",
        "driver_name", "driver_lo", "driver_hi", "driver_exact",
        "driver_kind", "dependency_id", "connection_id", "statement_id",
        "primitive_id", "terminal_id", "mapping_exact", "file_path",
        "source_path", "source_line", "source_column"],
    "v_load": [
        "signal_net_id", "signal_instance_id", "signal_name", "signal_lo",
        "signal_hi", "signal_exact", "load_net_id", "load_instance_id",
        "load_name", "load_lo", "load_hi", "load_exact", "load_kind",
        "dependency_id", "connection_id", "statement_id", "procedure_id",
        "terminal_id", "mapping_exact", "file_path", "source_path",
        "source_line", "source_column"],
    "v_statement": [
        "statement_id", "instance_id", "module_id", "module_name",
        "scope_node_id", "procedure_id", "ordinal", "sequence",
        "statement_kind", "construct", "assignment_kind", "delay",
        "dropped_operand_count", "file_path", "source_path", "source_line",
        "source_column"],
    "v_statement_target": [
        "target_id", "statement_id", "ordinal", "net_id", "net_name",
        "target_lo", "target_hi", "target_exact"],
    "v_statement_operand": [
        "operand_id", "statement_id", "ordinal", "net_id", "net_name",
        "operand_lo", "operand_hi", "operand_exact"],
}
for view, want in VIEW_COLUMNS.items():
    row = con.execute(
        "SELECT count(*) FROM sqlite_master WHERE type='view' AND name=?",
        (view,)).fetchone()
    if not row[0]:
        sys.exit(f"stable view missing: {view}")
    got = [d[0] for d in con.execute(f'SELECT * FROM "{view}" LIMIT 0').description]
    if got != want:
        sys.exit(f"{view} columns diverge from the contract:\n"
                 f"  want {want}\n  got  {got}")
print("ok: the twelve stable views exist with their contracted columns")

# Fact views: one view row is one base row.
for view, base in (
    ("v_tree_node", "tree_node"), ("v_net", "net"), ("v_terminal", "term"),
    ("v_terminal_map", "term_map"), ("v_net_connection", "net_conn"),
    ("v_net_dependency", "net_dep"), ("v_statement", "stmt"),
    ("v_statement_target", "assign_target"),
    ("v_statement_operand", "assign_operand"),
):
    nv = one(f'SELECT count(*) FROM "{view}"')
    nb = one(f'SELECT count(*) FROM "{base}"')
    if nv != nb:
        sys.exit(f"{view} has {nv} rows but {base} has {nb}")
print("ok: every fact view's row count equals its base table's")

# Composite views: the row count is the sum of the branches, each branch
# re-derived here from the base tables.
arcs_in = one("""
    SELECT count(*) FROM v_conn_arc a
    WHERE a.direction IN ('input','inout','ref')""")
arcs_out = one("""
    SELECT count(*) FROM v_conn_arc a
    WHERE a.direction IN ('output','inout','ref')
      AND a.connection_kind IN ('signal', 'external_reference')
      AND a.outer_net_id IS NOT NULL""")
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
if n_driver != want:
    sys.exit(f"v_driver has {n_driver} rows, branch sum says {want}")

arcs_in_load = one("""
    SELECT count(*) FROM v_conn_arc a
    WHERE a.direction IN ('input','inout','ref') AND a.outer_net_id IS NOT NULL""")
n_load = one("SELECT count(*) FROM v_load")
want = (one("SELECT count(*) FROM net_dep WHERE source_net_id IS NOT NULL")
        + arcs_in_load + arcs_out + term_out
        + one("SELECT count(*) FROM proc_event WHERE net_id IS NOT NULL")
        + one("""SELECT count(*) FROM expr_ref e
                 WHERE e.role IN ('assertion','wait','event','system_task')
                    OR NOT EXISTS (SELECT 1 FROM net_dep d
                                   WHERE d.expr_ref_id = e.id)""")
        + one("""SELECT count(*) FROM assign_operand o
                 WHERE NOT EXISTS (SELECT 1 FROM net_dep d
                                   WHERE d.assign_operand_id = o.id)"""))
if n_load != want:
    sys.exit(f"v_load has {n_load} rows, branch sum says {want}")
print("ok: v_driver and v_load reconcile with their branch formulas")

check(one("""
    SELECT count(*) FROM v_driver
    WHERE (driver_net_id IS NULL)
          != (driver_kind IN ('constant','terminal','system_task'))""") == 0,
      "driver-less rows are exactly constants, terminals and system tasks")
check(one("""
    SELECT count(*) FROM v_driver
    WHERE driver_kind IN ('constant', 'terminal', 'system_task')
      AND (driver_name IS NOT NULL
       OR driver_lo IS NOT NULL OR driver_hi IS NOT NULL
       OR driver_exact IS NOT NULL OR mapping_exact IS NOT NULL)""") == 0,
      "a driver-less row describes no driver end")
# The same discipline on the load side, which had no such check at all --
# so the terminal branch drifted into carrying ranges for an end that does
# not exist, exactly the shape the null-source rule exists to forbid.
check(one("""
    SELECT count(*) FROM v_load
    WHERE load_net_id IS NULL AND (load_name IS NOT NULL
       OR load_lo IS NOT NULL OR load_hi IS NOT NULL
       OR load_exact IS NOT NULL OR mapping_exact IS NOT NULL)""") == 0,
      "a target-less load describes no load end")
check(one("""
    SELECT count(*) FROM v_driver
    WHERE (driver_kind = 'terminal') != (terminal_id IS NOT NULL)""") == 0,
      "terminal drivers are exactly the rows naming a terminal")
check(one("""
    SELECT count(*) FROM v_load
    WHERE (load_kind IN ('sensitivity','wait','statement','terminal'))
          != (load_net_id IS NULL)""") == 0,
      "target-less loads are exactly sensitivity/wait/statement/terminal")
check(one("""
    SELECT count(*) FROM v_load
    WHERE (load_kind = 'terminal') != (terminal_id IS NOT NULL)""") == 0,
      "terminal loads are exactly the rows naming a terminal")
check(one("""
    SELECT count(*) FROM v_driver
    WHERE driver_kind NOT IN ('data','control','primitive','procedure',
                              'connection','connection_expression','constant',
                              'terminal','system_task')""") == 0,
      "driver_kind stays in its vocabulary")
check(one("""
    SELECT count(*) FROM v_load
    WHERE load_kind NOT IN ('dataflow','connection','sensitivity','wait',
                            'statement','terminal')""") == 0,
      "load_kind stays in its vocabulary")

# ------------------------------------------------------ mode-gated checks
if mode:
    check(one("""
        SELECT count(*) FROM file
        WHERE source_file_id IS NULL""") == 0,
          "every file row joined to source_file")
    top = meta.get("top")
    want_top = {"constructs": "constructs", "interfaces": "interfaces",
                "assertions": "assertions", "hierarchy": "hierarchy",
                "udp": "udps", "unresolved": "unresolved", "xmr": "xmr"}[mode]
    check(top == want_top, f"meta.top is {want_top}", f"got {top!r}")


def net_id(inst_name, net_name):
    return one("""
        SELECT n.id FROM net n JOIN tree_node t ON t.id = n.inst_id
        WHERE t.name = ? AND n.name = ?""", inst_name, net_name)


if mode == "constructs":
    # Self-feedback survives, and arithmetic is range-level: cnt <= cnt + 1.
    check(one("""
        SELECT count(*) FROM v_net_dependency
        WHERE source_name='cnt' AND target_name='cnt'
          AND source_net_id = target_net_id AND mapping_exact = 0""") >= 1,
          "the self-feedback dependency (cnt -> cnt), range-level")
    # Gates: primitive nodes with their rows, and the sr chain bit by bit.
    check(one("SELECT count(*) FROM primitive WHERE primitive_kind='gate'") >= 6,
          "gate primitives recorded as primitive nodes")
    check(one("""
        SELECT count(*) FROM v_net_dependency
        WHERE dependency_kind='primitive'""") >= 8, "primitive dependencies")
    check(one("""
        SELECT count(*) FROM v_net_dependency
        WHERE dependency_kind='primitive' AND source_name='sr' AND
              target_name='sr' AND source_lo=0 AND source_hi=0 AND
              target_lo=1 AND target_hi=1 AND mapping_exact=1""") == 1,
          "a gate driving one bit of a net from another, bit-exact")
    check(one("""
        SELECT count(*) FROM v_net_dependency
        WHERE dependency_kind='primitive' AND source_net_id IS NULL""") >= 1,
          "the pullup's null-source dependency")
    # Net initialisers are continuous assignments.
    check(one("""
        SELECT count(*) FROM v_statement
        WHERE assignment_kind='continuous' AND procedure_id IS NULL""") >= 2,
          "net initialisers are procedure-less continuous assignments")
    check(one("""
        SELECT count(*) FROM assign_target a JOIN net n ON n.id=a.net_id
        WHERE n.name='w'""") >= 1, "the net initialiser's target (w)")
    # The call chain: d -> bump.v at the call, bump.v -> q in the body.
    check(one("""
        SELECT count(*) FROM v_net_dependency
        WHERE dependency_kind='procedure' AND source_name='d'
          AND target_name='bump.v'""") == 1,
          "the call's actual bound to the formal (d -> bump.v)")
    check(one("""
        SELECT count(*) FROM v_net_dependency
        WHERE source_name='bump.v' AND target_name='q'
          AND dependency_kind='data'""") == 1,
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
        SELECT count(*) FROM stmt WHERE statement_kind='event_control'""") >= 2,
          "statement-level event controls are statements")
    # The downward XMR resolves to the child's real net -- the fact the
    # instance-level model exists to state.
    check(one("""
        SELECT count(*) FROM hier_ref h
        JOIN net n ON n.id = h.resolved_net_id
        JOIN tree_node t ON t.id = h.resolved_inst_id
        WHERE h.path='u_cnt.cnt' AND n.name='cnt' AND t.name='u_cnt'""") >= 1,
          "the downward XMR resolves to the child's net")
    check(one("""
        SELECT count(*) FROM hier_ref h JOIN stmt s ON s.id = h.stmt_id
        JOIN file f ON f.id = s.file_id
        WHERE f.path LIKE '%seq.svh'""") >= 1,
          "the included XMR, attributed to seq.svh")
    # Port windows: the part-select connection and the replication.
    check(one("""
        SELECT count(*) FROM v_net_connection
        WHERE net_name='stim' AND net_lo=0 AND net_hi=3 AND net_exact=1""") >= 1,
          "the part-select port connection (.idx(stim[3:0]))")
    check(one("""
        SELECT count(*) FROM v_net_connection c1
        JOIN v_net_connection c2 ON c1.terminal_id = c2.terminal_id
          AND c1.ordinal < c2.ordinal
        WHERE c1.net_name='rep_r' AND c2.net_name='rep_r'
          AND c1.terminal_lo IS NOT NULL AND c2.terminal_lo IS NOT NULL
          AND c1.mapping_exact=1 AND c2.mapping_exact=1""") >= 1,
          "a replicated connection keeps one exact segment per copy")
    check(one("""
        SELECT count(*) FROM v_net_connection
        WHERE connection_kind='expression_operand'""") >= 1,
          "an expression-operand connection")
    check(one("""
        SELECT count(*) FROM v_net_connection
        WHERE connection_kind='constant'""") >= 1, "a constant tie-off")
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
        SELECT count(DISTINCT i.parameter_signature) FROM inst i
        JOIN module m ON m.id = i.module_id WHERE m.name='scaled'""") == 2,
          "a module's two parameterisations keep distinct signatures")
    check(one("SELECT count(*) FROM module WHERE name='scaled'") == 1,
          "one definition row however many parameterisations")
    # Concatenated assignment: one statement, two targets, no crossing.
    pair = con.execute("""
        SELECT s.id FROM stmt s
        WHERE (SELECT count(*) FROM assign_target a WHERE a.stmt_id=s.id) = 2
        LIMIT 1""").fetchone()
    check(pair is not None, "a concatenated write is one statement, two targets")
    sid = pair[0]
    check(one("""
        SELECT count(*) FROM net_dep d
        WHERE d.stmt_id=? AND d.dependency_kind='data'""", sid) ==
          one("""SELECT count(*) FROM assign_operand o WHERE o.stmt_id=?""", sid),
          "the concatenation pairs halves, it does not cross them")
    # Dynamic select: an upper bound, not a guess (`assign q = bus[i]`).
    check(one("""
        SELECT count(*) FROM v_net_dependency
        WHERE source_name='bus' AND source_exact=0""") >= 1,
          "a dynamic select's read is an upper bound")
    # Dropped operands are counted.
    check(one("""
        SELECT count(*) FROM stmt WHERE dropped_operand_count > 0""") >= 1,
          "dropped constant operands are counted")
    # The crossing: the child counter's clk is driven by the parent's.
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE driver_kind='connection' AND signal_name='clk'
          AND signal_instance_id != driver_instance_id""") >= 1,
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
          AND assignment_kind='continuous'""") == 1,
          "a delayed continuous assign keeps its delay text")
    check(one("""
        SELECT count(*) FROM stmt WHERE delay='#2'
          AND assignment_kind='blocking'""") == 1,
          "an intra-assignment delay belongs to its own statement")
    # The undeclared left-hand side is a real net, marked implicit.
    check(one("""
        SELECT count(*) FROM net WHERE name='dly_w' AND is_implicit=1
          AND declaration_kind='wire'""") == 1,
          "an implicit net is a row with is_implicit set")
    # One pair, two statements, two dependencies -- never folded.
    check(one("""
        SELECT count(DISTINCT d.stmt_id) FROM net_dep d
        JOIN net s ON s.id=d.source_net_id JOIN net t ON t.id=d.target_net_id
        WHERE s.name='a' AND t.name='r2' AND d.dependency_kind='data'""") == 2,
          "the same pair from two statements stays two dependencies")

if mode == "interfaces":
    check(one("""
        SELECT count(*) FROM term WHERE terminal_kind='interface'""") >= 3,
          "interface terminals")
    check(one("""
        SELECT count(*) FROM term
        WHERE terminal_kind='interface' AND modport IS NOT NULL""") >= 2,
          "the binding's declared modport")
    check(one("""
        SELECT count(*) FROM net_conn
        WHERE connection_kind='interface' AND interface_inst_id IS NOT NULL""") >= 3,
          "interface bindings name their interface instance")
    check(one("""
        SELECT count(*) FROM net_conn c
        JOIN inst i ON i.id = c.interface_inst_id
        JOIN module m ON m.id = i.module_id
        WHERE c.connection_kind='interface' AND m.definition_kind='interface'""")
          == one("""SELECT count(*) FROM net_conn
                    WHERE connection_kind='interface'
                      AND interface_inst_id IS NOT NULL"""),
          "every named interface binding points at an interface instance")
    # The pass-through: a grandchild's binding resolves to the same
    # top-level interface instance the parent was handed.
    check(one("""
        SELECT count(DISTINCT c.interface_inst_id) FROM net_conn c
        JOIN term t ON t.id = c.term_id
        JOIN inst child ON child.id = t.inst_id
        JOIN inst parent ON parent.id = child.parent_inst_id
        WHERE c.connection_kind='interface'
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
          AND driver_instance_id != signal_instance_id
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
        JOIN tree_node n ON n.id = d.signal_instance_id
        WHERE d.signal_name='vld' AND d.driver_kind='data'
          AND d.driver_instance_id != d.signal_instance_id""") >= 1,
          "an interface member is driven from the module that writes it")
    check(one("""
        SELECT count(*) FROM v_load
        WHERE signal_name='data' AND load_kind='dataflow'
          AND load_instance_id != signal_instance_id""") >= 1,
          "an interface member is read by the module that samples it")
    check(one("""
        SELECT count(*) FROM net_conn
        WHERE connection_kind='external_reference' AND hier_ref_id IS NOT NULL""") >= 1,
          "a port tied outside the module keeps its row")
    check(one("""
        SELECT count(*) FROM hier_ref
        WHERE instr(path, ' ') > 0 OR instr(path, '/*') > 0""") == 0,
          "no reference path carries a space or a comment")

if mode == "assertions":
    check(one("""
        SELECT count(*) FROM stmt WHERE statement_kind='assertion'""") >= 3,
          "assertion statements")
    for word in ("assert", "assume", "cover"):
        check(one("""
            SELECT count(*) FROM stmt
            WHERE statement_kind='assertion' AND construct=?""", word) >= 1,
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

if mode == "hierarchy":
    # Two occurrences of one parameterisation: one signature, two row sets.
    check(one("""
        SELECT count(*) FROM inst i JOIN module m ON m.id=i.module_id
        WHERE m.name='leaf' AND i.parameter_signature='W=4'""") == 2,
          "the same parameterisation twice is two occurrences")
    check(one("""
        SELECT count(DISTINCT n.id) FROM net n JOIN inst i ON i.id=n.inst_id
        JOIN module m ON m.id=i.module_id
        WHERE m.name='leaf' AND n.name='q' AND i.parameter_signature='W=4'""") == 2,
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
        WHERE m.name='leaf' AND i.parameter_signature='W=1'""") == 4,
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

if mode == "udp":
    check(one("""
        SELECT count(*) FROM primitive WHERE primitive_kind='udp'
          AND definition_name='latch_p'""") == 1,
          "the UDP is a primitive of its own kind")
    check(one("""
        SELECT count(*) FROM primitive WHERE primitive_kind='switch'
          AND definition_name='tranif1'""") == 1,
          "the switch is a primitive of its own kind")
    check(one("""
        SELECT count(*) FROM primitive WHERE primitive_kind='gate'""") >= 1,
          "the buffer stays a gate")
    check(one("""
        SELECT count(*) FROM v_net_dependency d
        JOIN primitive p ON p.id = d.primitive_id
        WHERE p.primitive_kind='udp' AND d.target_name='q'
          AND d.source_name IN ('d','en')""") == 2,
          "the UDP couples its inputs to its output")
    # A tran conducts both ways: each end drives the other.
    check(one("""
        SELECT count(*) FROM v_net_dependency d
        JOIN primitive p ON p.id = d.primitive_id
        WHERE p.primitive_kind='switch'
          AND ((d.source_name='a' AND d.target_name='b')
            OR (d.source_name='b' AND d.target_name='a'))""") == 2,
          "the switch couples both directions")

if mode == "unresolved":
    check(status == "partial",
          "a missing definition leaves the export partial")
    check(one("""
        SELECT count(*) FROM tree_node t JOIN inst i ON i.id = t.id
        WHERE t.node_kind='unresolved' AND i.unresolved_definition='ghost'""") == 1,
          "the black box names the definition it wanted")
    # Terminals for what the parent connected, direction unknown.
    check(one("""
        SELECT count(*) FROM term t JOIN tree_node n ON n.id = t.inst_id
        WHERE n.node_kind='unresolved' AND t.direction IS NULL""") == 4,
          "the black box has a terminal per connection")
    check(one("""
        SELECT count(*) FROM net_conn c JOIN term t ON t.id = c.term_id
        JOIN tree_node n ON n.id = t.inst_id
        WHERE n.node_kind='unresolved' AND c.connection_kind='signal'""") >= 3,
          "the connections that reach the black box are recorded")
    check(one("""
        SELECT count(*) FROM net_conn c JOIN term t ON t.id = c.term_id
        JOIN tree_node n ON n.id = t.inst_id
        WHERE n.node_kind='unresolved' AND c.connection_kind='unconnected'""") == 1,
          "its unconnected pin is recorded as unconnected")
    # The trace stops AT the box: mid still has its consumer.
    check(one("""
        SELECT count(*) FROM v_net_dependency
        WHERE source_name='mid' AND target_name='gnt'""") == 1,
          "the design around the hole keeps its dataflow")

if mode == "xmr":
    # A downward read is a real dependency naming the reference it went
    # through -- not a hier_ref row beside a fabricated constant driver.
    check(one("""
        SELECT count(*) FROM v_net_dependency d
        JOIN hier_ref h ON h.id = d.source_hier_ref_id
        WHERE d.source_name='x' AND d.target_name='q'
          AND d.source_instance_id != d.target_instance_id
          AND h.path='u.x' AND h.access='read'""") == 1,
          "a downward read crosses as a dependency naming its reference")
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name='q' AND driver_kind='constant'""") == 0,
          "and the target is not reported as constant-driven")
    # A downward write likewise, from the writing instance's operand.
    check(one("""
        SELECT count(*) FROM v_net_dependency d
        JOIN hier_ref h ON h.id = d.target_hier_ref_id
        WHERE d.source_name='a' AND d.target_name='x'
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
        SELECT count(*) FROM v_net_dependency
        WHERE source_name='wide' AND target_name='slice_o'
          AND source_lo=0 AND source_hi=3""") == 1,
          "a part-select of a downward reference keeps its bits")
    # A control dependency whose target is outward.
    check(one("""
        SELECT count(*) FROM v_net_dependency
        WHERE source_name='g1' AND target_name='wide'
          AND dependency_kind='control'""") == 1,
          "a condition gating an outward write is a control dependency")
    # Two outward-gated statements in ONE procedure: each target takes its
    # own condition. The per-statement condition vectors are indexed in
    # lockstep, so a stale entry shows up exactly here -- as gate1's signal
    # on gate2's target, or as a missing edge.
    check(one("""
        SELECT count(*) FROM v_net_dependency
        WHERE source_name='en' AND target_name='gated'
          AND dependency_kind='control'""") == 1,
          "the first outward condition gates its own statement")
    check(one("""
        SELECT count(*) FROM v_net_dependency
        WHERE source_name='rst' AND target_name='gated'
          AND dependency_kind='control'""") == 1,
          "and the second gates its own, not the first's")
    check(one("""
        SELECT count(DISTINCT statement_id) FROM v_net_dependency
        WHERE source_name IN ('en','rst') AND target_name='gated'
          AND dependency_kind='control'""") == 2,
          "each outward condition lands on a distinct statement")
    # Two call sites, two conditions: each caller's gating reaches the
    # task body's write.
    for g in ("g1", "g2"):
        check(one("""
            SELECT count(*) FROM v_net_dependency
            WHERE source_name=? AND target_name='hits'
              AND dependency_kind='control'""", g) >= 1,
              f"the task body's write inherits {g} from its own call site")
    check(one("""
        SELECT count(*) FROM v_net_dependency
        WHERE target_name='put.v' AND dependency_kind='procedure'""") == 2,
          "each call site binds its own actual to the formal")
    # Both call sites here are gated. A binding that loses its statement
    # under a branch is the shape that made per-call-site walking useless.
    check(one("""
        SELECT count(*) FROM v_net_dependency
        WHERE target_name='put.v' AND dependency_kind='procedure'
          AND statement_id IS NOT NULL
          AND expression_reference_id IS NOT NULL""") == 2,
          "a gated call keeps its statement and its argument reference")
    check(one("""
        SELECT count(DISTINCT statement_id) FROM v_net_dependency
        WHERE source_name='put.v' AND target_name='hits'""") == 2,
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
        WHERE connection_kind='external_reference'
          AND hier_ref_id IS NOT NULL""") == 1,
          "a port tied outward keeps its connection row")
    check(one("""
        SELECT count(*) FROM v_driver d
        JOIN v_tree_node t ON t.node_id = d.signal_instance_id
        WHERE t.node_name='u_sink' AND d.signal_name='p'
          AND d.driver_kind='connection' AND d.driver_name='g'""") == 1,
          "and the resolved tie drives the formal across the boundary")
    check(one("""
        SELECT count(*) FROM v_driver d
        JOIN v_tree_node t ON t.node_id = d.signal_instance_id
        WHERE t.node_name='u_sink' AND d.signal_name='p'
          AND d.driver_kind='constant'""") == 1,
          "the constant tiling the rest of that formal is still recorded")
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
        WHERE path='u.split' AND lo IS NULL AND hi IS NULL AND is_exact=1""") == 1,
          "a split reference is recorded whole, once")
    check(one("""
        SELECT count(*) FROM v_net_dependency
        WHERE source_name='split' AND target_name IN ('sp_hi','sp_lo')
          AND source_lo IS NOT NULL AND source_hi IS NOT NULL""") == 2,
          "while each dependency through it carries its own bits")
    # Two call sites reading outward: two statements, two references, each
    # dependency pointing at the one its own statement made.
    check(one("""
        SELECT count(*) FROM hier_ref WHERE path='u.x' AND access='read'""") >= 3,
          "each call site records its own outward reference")
    check(one("""
        SELECT count(DISTINCT d.stmt_id) FROM net_dep d
        JOIN hier_ref h ON h.id = d.source_hier_ref_id
        JOIN net t ON t.id = d.target_net_id
        WHERE t.name='seen'""") == 2,
          "and the two body statements read it independently")
    # A system task's write is a driver, told apart from a tie-off.
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name='loaded_mem' AND driver_kind='system_task'""") == 1,
          "a system task that writes its argument drives it")
    check(one("""
        SELECT count(*) FROM v_driver d JOIN v_statement s
          ON s.statement_id = d.statement_id
        WHERE d.driver_kind='system_task' AND s.construct='$readmemh'""") == 2,
          "and each row names the call that did it")
    # A write whose source the schema cannot name AND whose target is in
    # another instance: the far net still has a driver, or a trace back
    # from it says nothing ever wrote it.
    check(one("""
        SELECT count(*) FROM v_driver d
        JOIN v_statement s ON s.statement_id = d.statement_id
        WHERE d.signal_name='far_mem' AND d.driver_kind='system_task'
          AND d.signal_instance_id != s.instance_id""") == 1,
          "a system task writing across the boundary drives the far net")
    check(one("""
        SELECT count(*) FROM v_driver d
        JOIN v_statement s ON s.statement_id = d.statement_id
        WHERE d.signal_name='tied' AND d.driver_kind='constant'
          AND d.signal_instance_id != s.instance_id""") == 1,
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

print("OK")
