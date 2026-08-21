#!/usr/bin/env python3
# Copyright (c) 2026 neveltyc
# released under the BSD 3-Clause License (see LICENSE)
#
# Read an exported database back and fail if it is hollow or malformed. A
# build that links proves the slang pin resolves; this proves the exporter
# still writes rows -- and that the rows keep every contract the schema
# documents: the subtype bijections, the ownership rules, the provenance
# matrix behind net_dep, the range discipline, and the fourteen stable views
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
#   verify-designdb.py <design.db> alias        + examples/constructs/alias.sv facts
import sqlite3
import sys

MODES = ("constructs", "interfaces", "assertions", "hierarchy", "udp",
         "unresolved", "xmr", "alias", "external", "package", "callsite")
if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] not in MODES):
    sys.exit(f"usage: {sys.argv[0]} <design.db> [{'|'.join(MODES)}]")

con = sqlite3.connect(sys.argv[1])
mode = sys.argv[2] if len(sys.argv) == 3 else None

SCHEMA_VERSION = "13"


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

baddig = one("SELECT count(*) FROM src_file "
             "WHERE digest IS NULL OR length(digest) != 64")
total_sf = one("SELECT count(*) FROM src_file")
if total_sf == 0 or baddig:
    sys.exit(f"{baddig} of {total_sf} src_file row(s) lack a SHA-256 digest")

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
# open sets (decl_kind) and the NULL-required combinations CHECK
# cannot express.
for tbl, col, values, nullable in (
    ("module", "def_kind", ("module", "interface", "program", "checker", "package"), False),
    ("tree_node", "node_kind", ("root", "instance", "generate", "primitive", "unresolved", "package"), False),
    ("prim", "prim_kind", ("gate", "switch", "udp"), False),
    ("term", "term_kind", ("signal", "interface"), False),
    ("term", "direction", ("input", "output", "inout", "ref"), True),
    ("net_conn", "conn_kind",
     ("signal", "constant", "unconnected", "expression_operand", "interface",
      "external_reference"), False),
    ("proc", "proc_kind",
     ("always", "always_ff", "always_comb", "always_latch", "initial", "final",
      "task", "function"), False),
    ("stmt", "stmt_kind",
     ("assignment", "assertion", "wait", "call", "system_task", "event_control",
      "alias", "release"), False),
    ("stmt", "assign_kind", ("continuous", "blocking", "nonblocking"), True),
    ("expr_ref", "role",
     ("control", "assertion", "wait", "event", "call_argument", "system_task"), False),
    ("proc_event", "event_kind", ("sensitivity", "wait"), False),
    ("proc_event", "edge_kind", ("posedge", "negedge", "both"), True),
    ("net_dep", "dep_kind",
     ("data", "control", "primitive", "procedure", "alias"), False),
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
check(one("SELECT count(*) FROM tree_node WHERE instr(name, '.') > 0") == 0,
      "every tree node name is a single path segment")
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
             -- A condition always has a source: without this, a control row
             -- with src_net_id NULL passed every check and surfaced in
             -- v_driver as a CONSTANT tie-off on a gated signal. The source
             -- may be a reference that resolved to no net ('external'), but
             -- it must exist as a row of one of the two kinds.
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
             OR d.stmt_target_id IS NOT NULL OR d.assign_operand_id IS NOT NULL
             OR (d.src_net_id IS NULL AND d.src_hier_ref_id IS NULL)
             -- The reading side names where the actual came from, exactly
             -- as the doc promises: an argument reference or a resolved
             -- outward one. The write-back direction (formal -> actual) has
             -- neither, and is told apart by the formal being the source.
             OR (d.expr_ref_id IS NOT NULL AND d.src_hier_ref_id IS NOT NULL)
        ELSE 1 END""") == 0,
      "net_dep provenance columns match dep_kind")
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
# reference; a resolved cross-instance dependency is the point of v10's
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
# impossible. `assign swap = {c[3:0], c[7:4]}` used to export two rows each
# claiming all eight bits of swap from a four-bit source -- provably false,
# and every column in it well formed.
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
    SELECT count(*) FROM hier_ref h JOIN net n ON n.id = h.resolved_net_id
    WHERE h.resolved_inst_id IS NULL OR n.inst_id != h.resolved_inst_id""") == 0,
      "a resolved net lies inside its resolved instance")
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

info = con.execute("SELECT * FROM v_db_info").fetchone()
info_cols = [d[0] for d in con.execute("SELECT * FROM v_db_info LIMIT 0").description]
by = dict(zip(info_cols, info))
if str(by["schema_version"]) != meta["schema_version"] or \
   not isinstance(by["schema_version"], int):
    sys.exit("v_db_info.schema_version disagrees with meta or is not INTEGER")
for k in ("error_count", "unresolved_count", "empty_procedure_count",
          "duplicate_path_count"):
    if by[k] != int(meta[k]) or not isinstance(by[k], int):
        sys.exit(f"v_db_info.{k} disagrees with meta or is not INTEGER")
print("ok: v_db_info agrees with meta and casts its counts")

# --------------------------------------------------------- view contract
# The fourteen stable views: existence, exact columns in exact order, and row
# formulas. v_conn_arc is scaffolding, not contract, and is deliberately
# absent from this list.
VIEW_COLUMNS = {
    "v_db_info": [
        "schema_version", "tool_version", "slang_version", "producer_revision",
        "top", "analysis_status", "error_count", "unresolved_count",
        "empty_procedure_count", "duplicate_path_count", "config_digest"],
    "v_tree_node": [
        "node_id", "parent_node_id", "node_name", "node_kind", "ordinal",
        "inst_id", "parent_inst_id", "module_id", "module_name",
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
        "ordinal", "is_const", "modport", "file_path", "src_path",
        "src_line", "src_col"],
    "v_term_map": [
        "term_id", "term_inst_id", "term_name",
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
        "signal_net_id", "signal_inst_id", "signal_name", "signal_lo",
        "signal_hi", "signal_exact", "driver_net_id", "driver_inst_id",
        "driver_name", "driver_lo", "driver_hi", "driver_exact",
        "driver_kind", "dep_id", "conn_id", "stmt_id",
        "prim_id", "term_id", "map_exact", "call_site_id", "file_path",
        "src_path", "src_line", "src_col"],
    "v_load": [
        "signal_net_id", "signal_inst_id", "signal_name", "signal_lo",
        "signal_hi", "signal_exact", "load_net_id", "load_inst_id",
        "load_name", "load_lo", "load_hi", "load_exact", "load_kind",
        "dep_id", "conn_id", "stmt_id", "proc_id",
        "term_id", "map_exact", "call_site_id", "file_path", "src_path",
        "src_line", "src_col"],
    "v_stmt": [
        "stmt_id", "inst_id", "module_id", "module_name",
        "scope_node_id", "proc_id", "ordinal", "sequence",
        "stmt_kind", "construct", "assign_kind", "delay",
        "dropped_operand_count", "file_path", "src_path", "src_line",
        "src_col"],
    "v_stmt_target": [
        "target_id", "stmt_id", "ordinal", "net_id", "net_name",
        "tgt_lo", "tgt_hi", "tgt_exact"],
    "v_stmt_operand": [
        "operand_id", "stmt_id", "ordinal", "net_id", "net_name",
        "operand_lo", "operand_hi", "operand_exact"],
    "v_net_attachment": [
        "net_id", "inst_id", "net_name", "attachment_kind",
        "lo", "hi", "exact", "stmt_id",
        "term_id", "stmt_target_id", "assign_operand_id", "expr_ref_id",
        "proc_id", "dep_id", "hier_ref_id"],
    "v_call_site": [
        "call_site_id", "inst_id", "module_id", "module_name",
        "caller_stmt_id", "parent_call_site_id", "subroutine_name", "depth"],
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
print("ok: the fourteen stable views exist with their contracted columns")

# Fact views: one view row is one base row.
for view, base in (
    ("v_tree_node", "tree_node"), ("v_net", "net"), ("v_term", "term"),
    ("v_term_map", "term_map"), ("v_net_conn", "net_conn"),
    ("v_net_dep", "net_dep"), ("v_stmt", "stmt"),
    ("v_stmt_target", "stmt_target"),
    ("v_stmt_operand", "assign_operand"),
    ("v_call_site", "call_site"),
):
    nv = one(f'SELECT count(*) FROM "{view}"')
    nb = one(f'SELECT count(*) FROM "{base}"')
    if nv != nb:
        sys.exit(f"{view} has {nv} rows but {base} has {nb}")
print("ok: every fact view's row count equals its base table's")

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
if n_driver != want:
    sys.exit(f"v_driver has {n_driver} rows, branch sum says {want}")

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
if n_load != want:
    sys.exit(f"v_load has {n_load} rows, branch sum says {want}")
print("ok: v_driver and v_load reconcile with their branch formulas")

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
if n_att != want:
    sys.exit(f"v_net_attachment has {n_att} rows, branch sum says {want}")
print("ok: v_net_attachment reconciles with its branch formula")
check(one("""
    SELECT count(*) FROM v_net_attachment
    WHERE attachment_kind NOT IN ('terminal_inside','actual_outside',
        'written_by','release_target','read_by','condition','statement_read',
        'event','dep_in','dep_out','named_from_outside')""") == 0,
      "attachment_kind stays in its vocabulary")
# The bug two correct commits made together: release stores its lvalue as
# an stmt_target (commit 6) and every stmt_target became written_by
# (commit 8), so a release read as a writer -- of a net it drives NOTHING
# of, the whole point of giving it its own statement kind. A written_by
# must never trace back to a release.
check(one("""
    SELECT count(*) FROM v_net_attachment a JOIN stmt s ON s.id = a.stmt_id
    WHERE a.attachment_kind = 'written_by' AND s.stmt_kind = 'release'""") == 0,
      "no release is mislabelled as a writer")
check(one("""
    SELECT count(*) FROM v_net_attachment a JOIN stmt s ON s.id = a.stmt_id
    WHERE a.attachment_kind = 'release_target'
      AND s.stmt_kind != 'release'""") == 0,
      "and release_target is exactly the releases")
# Exclusive arc, like net_dep: exactly one of the seven typed id columns is
# non-null per row, and it is the one attachment_kind names -- so a consumer
# joins the right base table without decoding the kind, and no row smuggles
# an id into a slot its kind does not own.
check(one("""
    SELECT count(*) FROM v_net_attachment
    WHERE (term_id IS NOT NULL) + (stmt_target_id IS NOT NULL)
        + (assign_operand_id IS NOT NULL) + (expr_ref_id IS NOT NULL)
        + (proc_id IS NOT NULL) + (dep_id IS NOT NULL)
        + (hier_ref_id IS NOT NULL) != 1""") == 0,
      "every attachment names exactly one typed id")
check(one("""
    SELECT count(*) FROM v_net_attachment WHERE CASE attachment_kind
        WHEN 'terminal_inside'    THEN term_id IS NULL
        WHEN 'actual_outside'     THEN term_id IS NULL
        WHEN 'written_by'         THEN stmt_target_id IS NULL
        WHEN 'release_target'     THEN stmt_target_id IS NULL
        WHEN 'read_by'            THEN assign_operand_id IS NULL
        WHEN 'condition'          THEN expr_ref_id IS NULL
        WHEN 'statement_read'     THEN expr_ref_id IS NULL
        WHEN 'event'              THEN proc_id IS NULL
        WHEN 'dep_in'             THEN dep_id IS NULL
        WHEN 'dep_out'            THEN dep_id IS NULL
        WHEN 'named_from_outside' THEN hier_ref_id IS NULL
        ELSE 1 END""") == 0,
      "and it is the typed id its attachment_kind implies")
# Each typed id resolves in its own table -- the join a consumer would make.
for col, tbl in (("term_id", "term"), ("stmt_target_id", "stmt_target"),
                 ("assign_operand_id", "assign_operand"),
                 ("expr_ref_id", "expr_ref"), ("proc_id", "proc"),
                 ("dep_id", "net_dep"), ("hier_ref_id", "hier_ref")):
    check(one(f"""SELECT count(*) FROM v_net_attachment a
        WHERE a.{col} IS NOT NULL
          AND NOT EXISTS (SELECT 1 FROM "{tbl}" b WHERE b.id = a.{col})""") == 0,
          f"v_net_attachment.{col} resolves in {tbl}")

check(one("""
    SELECT count(*) FROM v_driver
    WHERE (driver_net_id IS NULL)
          != (driver_kind IN ('constant','terminal','system_task',
                              'external'))""") == 0,
      "net-less rows are exactly constant/terminal/system_task/external")
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
check(one("""
    SELECT count(*) FROM v_driver
    WHERE driver_kind IN ('constant', 'terminal', 'system_task')
      AND (driver_name IS NOT NULL
       OR driver_lo IS NOT NULL OR driver_hi IS NOT NULL
       OR driver_exact IS NOT NULL OR map_exact IS NOT NULL)""") == 0,
      "a driver-less row describes no driver end")
# The same discipline on the load side, which had no such check at all --
# so the terminal branch drifted into carrying ranges for an end that does
# not exist, exactly the shape the null-source rule exists to forbid.
check(one("""
    SELECT count(*) FROM v_load
    WHERE load_net_id IS NULL AND (load_name IS NOT NULL
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
                              'external')""") == 0,
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
# traced signal into one full scan per hop. It regressed once already:
# deriving the outer end of a crossing with COALESCE over two tables left
# the value attributable to neither, so neither table's index could be
# used, and tracing a clock took minutes.
for view, col in (("v_driver", "signal_net_id"), ("v_load", "signal_net_id"),
                  ("v_net_dep", "tgt_net_id"),
                  ("v_net_conn", "outer_net_id"),
                  ("v_net_attachment", "net_id")):
    plan = con.execute(
        f"EXPLAIN QUERY PLAN SELECT * FROM {view} WHERE {col} = 1").fetchall()
    scanned = [r[3] for r in plan
               if r[3].startswith("SCAN ") and not r[3].startswith(f"SCAN {view}")]
    check(not scanned, f"{view} seeks rather than scans for one {col}",
          "; ".join(scanned))

# ------------------------------------------------------ mode-gated checks
if mode:
    check(one("""
        SELECT count(*) FROM file
        WHERE src_file_id IS NULL""") == 0,
          "every file row joined to src_file")
    top = meta.get("top")
    want_top = {"callsite": "callsite_top", "constructs": "constructs", "interfaces": "interfaces",
                "assertions": "assertions", "hierarchy": "hierarchy",
                "udp": "udps", "unresolved": "unresolved", "xmr": "xmr",
                "alias": "alias_top", "external": "tb_top",
                "package": "package_top"}[mode]
    check(top == want_top, f"meta.top is {want_top}", f"got {top!r}")


def net_id(inst_name, net_name):
    return one("""
        SELECT n.id FROM net n JOIN tree_node t ON t.id = n.inst_id
        WHERE t.name = ? AND n.name = ?""", inst_name, net_name)


if mode == "constructs":
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

if mode == "interfaces":
    check(one("""
        SELECT count(*) FROM term WHERE term_kind='interface'""") >= 3,
          "interface terminals")
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

if mode == "hierarchy":
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

if mode == "udp":
    check(one("""
        SELECT count(*) FROM prim WHERE prim_kind='udp'
          AND def_name='latch_p'""") == 1,
          "the UDP is a primitive of its own kind")
    check(one("""
        SELECT count(*) FROM prim WHERE prim_kind='switch'
          AND def_name='tranif1'""") == 1,
          "the switch is a primitive of its own kind")
    # The LRM's whole switch family, not just what slang labels BiDiSwitch:
    # the resistive variants and the MOS switches used to read as gates.
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

if mode == "unresolved":
    check(status == "partial",
          "a missing definition leaves the export partial")
    check(one("""
        SELECT count(*) FROM tree_node t JOIN inst i ON i.id = t.id
        WHERE t.node_kind='unresolved' AND i.unresolved_def='ghost'""") == 1,
          "the black box names the definition it wanted")
    # Terminals for what the parent connected, direction unknown.
    check(one("""
        SELECT count(*) FROM term t JOIN tree_node n ON n.id = t.inst_id
        WHERE n.node_kind='unresolved' AND t.direction IS NULL""") == 4,
          "the black box has a terminal per connection")
    check(one("""
        SELECT count(*) FROM net_conn c JOIN term t ON t.id = c.term_id
        JOIN tree_node n ON n.id = t.inst_id
        WHERE n.node_kind='unresolved' AND c.conn_kind='signal'""") >= 3,
          "the connections that reach the black box are recorded")
    check(one("""
        SELECT count(*) FROM net_conn c JOIN term t ON t.id = c.term_id
        JOIN tree_node n ON n.id = t.inst_id
        WHERE n.node_kind='unresolved' AND c.conn_kind='unconnected'""") == 1,
          "its unconnected pin is recorded as unconnected")
    # The trace stops AT the box: mid still has its consumer.
    check(one("""
        SELECT count(*) FROM v_net_dep
        WHERE src_name='mid' AND tgt_name='gnt'""") == 1,
          "the design around the hole keeps its dataflow")

if mode == "xmr":
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
          AND outer_hier_ref_id IS NOT NULL""") == 1,
          "a port tied outward keeps its connection row")
    check(one("""
        SELECT count(*) FROM v_driver d
        JOIN v_tree_node t ON t.node_id = d.signal_inst_id
        WHERE t.node_name='u_sink' AND d.signal_name='p'
          AND d.driver_kind='connection' AND d.driver_name='g'""") == 1,
          "and the resolved tie drives the formal across the boundary")
    # v11 left external ties without a mapping, so this arc reported 0
    # even though both windows are exact -- the tie is a positional
    # element with a per-bit correspondence, and now says so.
    check(one("""
        SELECT count(*) FROM v_driver d
        JOIN v_tree_node t ON t.node_id = d.signal_inst_id
        WHERE t.node_name='u_sink' AND d.signal_name='p'
          AND d.driver_kind='connection' AND d.driver_name='g'
          AND d.driver_lo=4 AND d.driver_hi=7 AND d.driver_exact=1
          AND d.map_exact=1""") == 1,
          "bit for bit: the external tie is traceable at bit granularity")
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
        JOIN v_term t ON t.term_id = a.term_id
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
        WHERE path='u.split' AND lo IS NULL AND hi IS NULL AND is_exact=1""") == 1,
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

if mode == "external":
    # After v13 taught packages to resolve, what still leaves the model is an
    # upward hierarchical reference from a shared body: tb_top.glob climbs out
    # of up_leaf, and the one analysed body cannot say where each of its two
    # occurrences sits. The dependency carries a NULL source net and the
    # reference on the source end; v_driver says 'external' -- not undriven.
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE driver_kind='external' AND signal_name='o'""") >= 1,
          "an output driven by an upward reference is external")
    check(one("""
        SELECT count(*) FROM v_driver v JOIN net_dep d ON d.id = v.dep_id
        JOIN hier_ref h ON h.id = d.src_hier_ref_id
        WHERE v.driver_kind='external' AND v.signal_name='nib'
          AND h.resolved_net_id IS NULL
          AND v.driver_lo=0 AND v.driver_hi=3 AND v.driver_exact=1""") >= 1,
          "the windowed upward read keeps its window on the external driver")
    check(one("""
        SELECT count(*) FROM net_dep
        WHERE src_net_id IS NULL AND src_hier_ref_id IS NOT NULL
          AND dep_kind='control'""") >= 1,
          "an upward condition gates as a control dependency with no source")
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE driver_kind='external' AND signal_name='g'""") >= 1,
          "so the upward-gated target shows its external control")
    # o and nib are pure upward reads -- no constant hides among their
    # drivers. (g legitimately also has constant drivers: the 8'hFF/8'h00 it
    # assigns under the upward condition.)
    check(one("""
        SELECT count(*) FROM v_driver
        WHERE signal_name IN ('o','nib') AND driver_kind='constant'""") == 0,
          "and no upward source is misreported as a constant")

if mode == "callsite":
    # Two calls to one task, from two sites -- both bump, both outermost.
    check(one("""SELECT count(*) FROM v_call_site
                 WHERE subroutine_name='bump' AND depth=1""") == 2,
          "the task is called from two call sites")
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
        JOIN call_site cs ON cs.id = d.call_site_id
        WHERE cs.subroutine_name='pick'""") == 0,
          "and its statement-less binding carries no call_site_id")
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

if mode == "package":
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
    check(one("""
        SELECT count(DISTINCT signal_inst_id) FROM v_driver
        WHERE driver_name='mask' AND driver_kind='data'""") == 2,
          "and there really are two distinct readers of it")
    check(one("""
        SELECT count(*) FROM hier_ref
        WHERE path LIKE 'cfg_pkg::%' AND resolved_net_id IS NOT NULL""") >= 1,
          "the pkg:: reference is recorded as written and resolved")

print("OK")
