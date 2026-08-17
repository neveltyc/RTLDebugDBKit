#!/usr/bin/env python3
# Copyright (c) 2026 neveltyc
# released under the BSD 3-Clause License (see LICENSE)
#
# Read an exported database back and fail if it is hollow. A build that links
# proves the slang pin resolves; this proves the exporter still writes rows.
# CI runs it against examples/ on every platform binary it builds, so it
# asserts only what those small designs must produce, not exact counts.
#
#   verify-designdb.py <design.db>              the hollow check
#   verify-designdb.py <design.db> constructs   + what examples/constructs/constructs.sv must yield
#   verify-designdb.py <design.db> interfaces   + what examples/constructs/interfaces.sv must yield
#   verify-designdb.py <design.db> assertions   + what examples/constructs/assertions.sv must yield
import sqlite3
import sys

if len(sys.argv) not in (2, 3):
    sys.exit(f"usage: {sys.argv[0]} <design.db> [constructs|interfaces|assertions]")

con = sqlite3.connect(sys.argv[1])
mode = sys.argv[2] if len(sys.argv) == 3 else None

counts = {
    t: con.execute(f'SELECT count(*) FROM "{t}"').fetchone()[0]
    for t in ("module", "instance", "symbol", "edge", "port")
}
print("row counts:", counts)
empty = [t for t, n in counts.items() if n == 0]
if empty:
    sys.exit(f"exported database has no rows in: {', '.join(empty)}")
digest = con.execute("SELECT digest FROM source_file").fetchone()
if not digest or len(digest[0]) != 64:
    sys.exit("source_file.digest is missing or not a SHA-256")

# Structural integrity — catches corruption and generator bugs.
ic = con.execute("PRAGMA integrity_check").fetchone()[0]
if ic != "ok":
    sys.exit(f"integrity_check failed: {ic}")
con.execute("PRAGMA foreign_keys = ON")
fk_errs = con.execute("PRAGMA foreign_key_check").fetchall()
if fk_errs:
    sys.exit(f"foreign_key_check failed: {len(fk_errs)} violation(s), "
             f"first: table={fk_errs[0][0]} rowid={fk_errs[0][1]}")
print("ok: integrity_check and foreign_key_check pass")

# Value domain: boolean-ish columns must be 0, 1, or NULL.
for tbl, cols in (
    ("edge", ("control", "src_exact", "dst_exact")),
    ("port", ("outer_exact",)),
    ("proc_event", ("wait",)),
    ("assign_operand", ("src_exact",)),
    ("stmt_read", ("src_exact",)),
    ("hier_ref", ("path_exact",)),
):
    for col in cols:
        bad = con.execute(
            f'SELECT count(*) FROM "{tbl}" WHERE "{col}" NOT IN (0, 1) '
            f'AND "{col}" IS NOT NULL').fetchone()[0]
        if bad:
            sys.exit(f"{bad} row(s) in {tbl}.{col} outside {{0, 1, NULL}}")
print("ok: boolean columns are 0/1/NULL")

# Range consistency: lo <= hi when both are non-NULL.
for tbl, lo, hi in (
    ("edge", "src_lo", "src_hi"), ("edge", "dst_lo", "dst_hi"),
    ("port", "outer_lo", "outer_hi"),
    ("assignment", "dst_lo", "dst_hi"),
    ("assign_operand", "src_lo", "src_hi"),
    ("stmt_read", "src_lo", "src_hi"),
    ("hier_ref", "path_lo", "path_hi"),
):
    bad = con.execute(
        f'SELECT count(*) FROM "{tbl}" WHERE "{lo}" IS NOT NULL '
        f'AND "{hi}" IS NOT NULL AND "{lo}" > "{hi}"').fetchone()[0]
    if bad:
        sys.exit(f"{bad} row(s) in {tbl} have {lo} > {hi}")
# direction is an enum, not a boolean: 0=in 1=out 2=inout 3=ref, NULL when
# the declaration is not a port or the binding is an interface.
for tbl in ("symbol", "port"):
    bad = con.execute(
        f'SELECT count(*) FROM "{tbl}" WHERE direction NOT IN (0, 1, 2, 3) '
        f'AND direction IS NOT NULL').fetchone()[0]
    if bad:
        sys.exit(f"{bad} row(s) in {tbl}.direction outside {{0, 1, 2, 3, NULL}}")
print("ok: direction values are valid")
print("ok: range lo <= hi")

# child.kind is what `def_module IS NULL` could not say: a gate whose dataflow is
# already in the parent's edges, versus a black box that has none anywhere.
bad_kind = con.execute(
    "SELECT count(*) FROM child WHERE kind NOT IN ('module','primitive','unresolved')"
).fetchone()[0]
if bad_kind:
    sys.exit(f"{bad_kind} child row(s) have a kind outside "
             "{module, primitive, unresolved}")
mismatched = con.execute("""
    SELECT count(*) FROM child
    WHERE (kind = 'module') != (def_module IS NOT NULL)""").fetchone()[0]
if mismatched:
    sys.exit(f"{mismatched} child row(s) disagree between kind and def_module; "
             "only kind='module' has a module row, and it always has one")
print("ok: child.kind agrees with def_module")

# The two hierarchy tables must actually be related, in both directions.
# A `child` nobody expands means the folded row describes an instantiation the
# tree does not have; a module instance with no `child` means the tree has a node
# nothing declared. Both were unnoticeable before the column existed.
unexpanded = con.execute("""
    SELECT count(*) FROM child c
    WHERE NOT EXISTS (SELECT 1 FROM instance i WHERE i.child = c.id)""").fetchone()[0]
if unexpanded:
    sys.exit(f"{unexpanded} child row(s) are never expanded into an instance")
unlinked = con.execute("""
    SELECT count(*) FROM instance
    WHERE parent IS NOT NULL AND module IS NOT NULL AND child IS NULL""").fetchone()[0]
if unlinked:
    sys.exit(f"{unlinked} module instance(s) below the root name no child row; "
             "the folded and expanded hierarchies have come apart")
# The root is the one instance nothing declares, and there is exactly one per
# elaborated top.
rootless = con.execute(
    "SELECT count(*) FROM instance WHERE parent IS NULL AND child IS NOT NULL"
).fetchone()[0]
if rootless:
    sys.exit(f"{rootless} root instance(s) claim a child row; nothing declares a top")
print("ok: every child expands and every module instance names its child")

# Global meta keys -- these are written unconditionally, so a database
# that lacks them was either not finished or produced by an older version.
#
# schema_version belongs here and not beside meta.top: what `top` should be
# depends on which example was loaded, but every database this tool writes is
# one fixed version whatever the design. Checking it only in the mode-specific
# branch meant a plain `verify-designdb.py design.db` accepted a database
# claiming schema 999.
#
# The required `meta` key set below is part of that version: a database written
# before those keys existed is not a v5 database, and this check is what stops
# it being read as one.
SCHEMA_VERSION = "6"
version = con.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()
if not version or version[0] != SCHEMA_VERSION:
    sys.exit(f"schema_version is {version and version[0]!r}, expected {SCHEMA_VERSION!r}")
status = con.execute("SELECT value FROM meta WHERE key='analysis_status'").fetchone()
if not status or status[0] not in ("complete", "partial", "hierarchy_only"):
    sys.exit(f"analysis_status is {status and status[0]!r}, expected one of "
             "complete/partial/hierarchy_only")
counts = {}
for key in ("error_count", "warning_count", "unresolved_count",
            "empty_procedure_count", "duplicate_path_count"):
    row = con.execute("SELECT value FROM meta WHERE key=?", (key,)).fetchone()
    if not row or not row[0].isdigit():
        sys.exit(f"meta.{key} is missing or non-numeric")
    counts[key] = int(row[0])
for key in ("tool_version", "slang_version", "producer_revision"):
    row = con.execute("SELECT value FROM meta WHERE key=?", (key,)).fetchone()
    if not row or not row[0]:
        sys.exit(f"meta.{key} is missing")
cd = con.execute("SELECT value FROM meta WHERE key='config_digest'").fetchone()
if not cd or len(cd[0]) != 64:
    sys.exit("meta.config_digest is missing or not a SHA-256")

# The seal has to agree with itself. Presence alone let a database say
# `complete` while also reporting ten errors and three skipped procedures --
# a consumer trusting the status word would read the missing dataflow as
# design fact. warning_count is exempt: plenty of warnings say nothing about
# whether extraction finished. unresolved_count is exempt too -- a black box
# is a design that was elaborated as fully as its sources allow.
INCOMPLETE = ("error_count", "empty_procedure_count", "duplicate_path_count")
if status[0] == "complete":
    contradicting = {k: counts[k] for k in INCOMPLETE if counts[k]}
    if contradicting:
        sys.exit(f"analysis_status is 'complete' but {contradicting} is non-zero; "
                 "the status and the counts disagree")
elif status[0] == "partial":
    if not any(counts[k] for k in INCOMPLETE):
        sys.exit("analysis_status is 'partial' but every count that would "
                 f"explain it is zero: {({k: counts[k] for k in INCOMPLETE})}")
print(f"ok: meta seal present and self-consistent (analysis_status={status[0]})")


def check(what, sql, expect_at_least=1):
    n = con.execute(sql).fetchone()[0]
    if n < expect_at_least:
        sys.exit(f"expected {what}, found {n} row(s)")
    print(f"ok: {what} ({n})")


if mode:
    # Only `top` is mode-specific: which module elaborates as top is a property
    # of the example, not of the format. schema_version is checked universally.
    top = con.execute("SELECT value FROM meta WHERE key='top'").fetchone()
    if not top or top[0] != mode:
        sys.exit(f"meta.top is {top and top[0]!r}, expected {mode!r} without --top")
    check("file rows joined to source_file",
          "SELECT count(*) FROM file WHERE source_file IS NOT NULL")
    # Every file row, not merely one. The origin of a file is learned from the
    # first row that mentions it, and a row whose location sits inside a macro
    # body names no file at all -- which left that file, and only that file,
    # with no digest to check it against while the count above still passed.
    unlinked = con.execute(
        "SELECT count(*) FROM file WHERE source_file IS NULL").fetchone()[0]
    if unlinked:
        sys.exit(f"{unlinked} file row(s) have no source_file; a macro-expanded "
                 "or otherwise fileless location poisoned the origin lookup")
    print("ok: every file row joined to source_file")

if mode:
    # Every statement that writes a target must appear in BOTH tables. `edge`
    # deduplicates and `assignment` does not, so a key that is too coarse
    # deletes edges while leaving the assignment behind -- which is exactly
    # what happened when the key carried a line number without the file, and a
    # task body in an `include`d header shared a line with a statement in the
    # module's own file. This invariant catches that whole class without
    # needing two files to collide on a chosen number.
    #
    # An EXISTENCE check, not a pairing. These columns are shared by every
    # statement on a line, so matching one here says an edge survived for that
    # statement's target -- not that this edge came from this assignment.
    # Joining the two tables on them to recover operands returns a cross
    # product; `assign_operand` is what answers that, keyed on assignment.id.
    orphaned = con.execute("""
        SELECT count(*) FROM assignment a
        WHERE NOT EXISTS (
            SELECT 1 FROM edge e
            WHERE e.module = a.module AND e.dst = a.dst
              AND e.file IS a.file AND e.line = a.line)""").fetchone()[0]
    if orphaned:
        sys.exit(f"{orphaned} assignment(s) have no edge at the same "
                 "module/dst/file/line; the edge dedup key is dropping statements")
    print("ok: every assignment is matched by an edge at the same place")
    # A name in the instance tree is one path segment. A generate block glued
    # onto the child's leaf name made a row the documented per-segment walk
    # cannot resolve.
    multi = con.execute("""
        SELECT count(*) FROM instance i JOIN name n ON n.id = i.name
        WHERE instr(n.text, '.') > 0""").fetchone()[0]
    if multi:
        sys.exit(f"{multi} instance row(s) hold more than one path segment; "
                 "a generate block is a level of its own")
    print("ok: every instance name is a single path segment")

if mode == "constructs":
    check("the self-feedback edge (cnt -> cnt)", """
        SELECT count(*) FROM edge e
        JOIN name s ON s.id = e.src JOIN name d ON d.id = e.dst
        WHERE s.text = 'cnt' AND d.text = 'cnt'""")
    check("primitive edges", "SELECT count(*) FROM edge WHERE kind='primitive'", 6)
    # A gate is a `child` row of its own kind, and the instance-tree node for it
    # points back at that row. Both were NULL-and-guess before.
    check("gates recorded as primitive children",
          "SELECT count(*) FROM child WHERE kind = 'primitive'", 6)
    check("a gate's tree node names the child row it expands", """
        SELECT count(*) FROM instance i
        JOIN child c ON c.id = i.child
        WHERE c.kind = 'primitive' AND i.module IS NULL""", 6)
    check("the pullup's null-source row", """
        SELECT count(*) FROM edge
        WHERE construct = 'gate:pullup' AND src IS NULL""")
    # One net on both terminals of a gate, different bits. Guarding on the
    # symbol dropped the edge and then called the target undriven, which
    # severed every stage of a gate-level chain and mislabelled it.
    # The reason the schema version moved to 3: a statement in no procedure.
    # Spelled NULL, as `blocking` in the same table already spells "does not
    # apply" -- a -1 would have to be known about and excluded before joining.
    check("the net initialiser's assignment has no procedure", """
        SELECT count(*) FROM assignment a JOIN name d ON d.id = a.dst
        WHERE a.proc IS NULL AND d.text IN ('w', 's')""", 2)
    orphan = con.execute("""
        SELECT count(*) FROM assignment WHERE proc = -1""").fetchone()[0]
    if orphan:
        sys.exit(f"{orphan} assignment(s) carry proc=-1; a statement in no "
                 "procedure is spelled NULL, like blocking beside it")
    print("ok: no assignment uses a -1 sentinel for proc")
    check("a gate driving one bit of a net from another", """
        SELECT count(*) FROM edge e
        JOIN name d ON d.id = e.dst JOIN name s ON s.id = e.src
        WHERE e.construct = 'gate:buf' AND d.text = 'sr' AND s.text = 'sr'
          AND e.dst_lo = e.src_lo + 1""", 2)
    nulls = con.execute("""
        SELECT count(*) FROM edge JOIN name d ON d.id = edge.dst
        WHERE edge.construct = 'gate:buf' AND d.text = 'sr'
          AND edge.src IS NULL""").fetchone()[0]
    if nulls:
        sys.exit(f"{nulls} gate row(s) on `sr` claim no source; the chain is severed")
    print("ok: no gate on `sr` claims to drive from nothing")
    # A connection is written where it is written. `u_cnt` spans one port per
    # line, so its three rows must carry three different lines -- taking the
    # instantiation's line for all of them made the ports indistinguishable by
    # position and pointed a driver query at the header.
    spread = con.execute("""
        SELECT count(DISTINCT p.line) FROM port p JOIN name c ON c.id = p.child
        WHERE c.text = 'u_cnt'""").fetchone()[0]
    if spread < 3:
        sys.exit(f"u_cnt's connections share {spread} distinct line(s) across 3 "
                 "ports; they are written one per line and must say so")
    print(f"ok: each connection on u_cnt names its own line ({spread})")
    check("the part-select port connection (.idx(stim[3:0]))", """
        SELECT count(*) FROM port
        WHERE outer_lo = 0 AND outer_hi = 3 AND outer_exact = 1""")
    check("an expression-operand connection (conn_kind=3)",
          "SELECT count(*) FROM port WHERE conn_kind = 3")
    check("the statement-level wait, at its own line", """
        SELECT count(*) FROM proc_event pe
        JOIN module m ON m.id = pe.module
        WHERE m.name = 'constructs' AND pe.edge_kind = 'posedge'
          AND pe.line IS NOT NULL""")
    check("the downward XMR as a dotted edge name", """
        SELECT count(*) FROM edge e JOIN name s ON s.id = e.src
        WHERE s.text = 'u_cnt.cnt'""")
    # A net declared with an initialiser drives it, exactly as `assign` does.
    check("the net initialiser's drivers", """
        SELECT count(*) FROM edge e
        JOIN name d ON d.id = e.dst JOIN name s ON s.id = e.src
        WHERE d.text = 'w' AND s.text IN ('a', 'b')""", 2)
    undriven = con.execute("""
        SELECT count(*) FROM symbol sy JOIN module m ON m.id = sy.module
        JOIN name n ON n.id = sy.name
        WHERE m.name = 'netinit' AND n.text IN ('w', 's')
          AND NOT EXISTS (SELECT 1 FROM edge e
                          WHERE e.module = sy.module AND e.dst = sy.name)""").fetchone()[0]
    if undriven:
        sys.exit(f"{undriven} net(s) declared with an initialiser have no driver")
    print("ok: no net initialiser leaves its net undriven")
    # A call binds its actuals, so the chain through the task body is whole.
    check("the call's actual bound to the formal", """
        SELECT count(*) FROM edge e
        JOIN name d ON d.id = e.dst JOIN name s ON s.id = e.src
        WHERE s.text = 'd' AND d.text = 'bump.v'""")
    check("the task body's write, the other half of that chain", """
        SELECT count(*) FROM edge e
        JOIN name d ON d.id = e.dst JOIN name s ON s.id = e.src
        WHERE s.text = 'bump.v' AND d.text = 'q'""")
    # A statement whose whole effect is to read -- a system task's argument, a
    # wait condition -- writes nothing the module can name, so its reads have
    # no target to hang from. Without these the signal answers "nobody reads
    # me" while the source is printing it.
    check("the system task argument's read", """
        SELECT count(*) FROM stmt_read r JOIN name n ON n.id = r.name
        JOIN module m ON m.id = r.module
        WHERE m.name = 'observers' AND n.text = 'watched'
          AND r.construct = '$display'""", 2)
    # A system task's *written* argument is not a read of it.
    loadedread = con.execute("""
        SELECT count(*) FROM stmt_read r JOIN name n ON n.id = r.name
        WHERE n.text = 'loaded'""").fetchone()[0]
    if loadedread:
        sys.exit(f"{loadedread} stmt_read row(s) for `loaded`, which $readmemh "
                 "writes rather than reads")
    print("ok: a system task's written argument is not recorded as a read")
    check("the wait condition's read", """
        SELECT count(*) FROM stmt_read r JOIN name n ON n.id = r.name
        JOIN module m ON m.id = r.module
        WHERE m.name = 'observers' AND n.text = 'done' AND r.construct = 'wait'""")
    # A call that *does* write is attributed through its edges instead, and
    # must not also be recorded as a bare read.
    dupe = con.execute("""
        SELECT count(*) FROM stmt_read r JOIN module m ON m.id = r.module
        WHERE m.name = 'viacall'""").fetchone()[0]
    if dupe:
        sys.exit(f"{dupe} stmt_read row(s) for a call that writes; its reads "
                 "are already attributed through the edge chain")
    print("ok: a writing call is not also recorded as a bare read")
    # An immediate assertion writes nothing, so its reads live in stmt_read.
    check("the immediate assertion's reads", """
        SELECT count(*) FROM stmt_read r JOIN module m ON m.id = r.module
        WHERE m.name = 'checks' AND r.construct = 'assert'""", 2)
    # The statements in seq.svh belong to a procedure whose header is in
    # constructs.sv. Their rows must say seq.svh: the file has to travel with
    # the line it is paired with, or the pair names a line in the wrong file.
    check("the included wait, attributed to seq.svh", """
        SELECT count(*) FROM proc_event pe JOIN file f ON f.id = pe.file
        WHERE f.path LIKE '%seq.svh'""")
    # An initial block is not sensitive to anything. Its event controls are
    # waits, and a reader filtering wait=0 must get nothing for it -- without
    # the column those rows read as a trigger set and the block looks clocked.
    check("the initial block's events are all waits", """
        SELECT count(*) FROM proc_event WHERE wait = 1""", 2)
    sensitive = con.execute("""
        SELECT count(*) FROM proc_event pe JOIN module m ON m.id = pe.module
        WHERE m.name = 'constructs' AND pe.wait = 0""").fetchone()[0]
    if sensitive:
        sys.exit(f"the initial block reports {sensitive} sensitivity row(s); "
                 "an initial block triggers on nothing")
    print("ok: the initial block reports no sensitivity")
    check("the always_ff's real sensitivity list", """
        SELECT count(*) FROM proc_event pe JOIN module m ON m.id = pe.module
        WHERE m.name = 'counter' AND pe.wait = 0""", 2)
    # A downward XMR resolves inside the module that writes it, so it is an
    # ordinary assignment under a dotted name rather than a hier_ref -- the
    # point here is only that the row names the file the statement is in.
    check("the included XMR, attributed to seq.svh", """
        SELECT count(*) FROM assignment a
        JOIN file f ON f.id = a.file JOIN name d ON d.id = a.dst
        WHERE f.path LIKE '%seq.svh' AND d.text = 'u_cnt.dbg'""")

    # `if (c) sel <= a; else sel <= b;` -- two statements, three edges (the
    # condition is one), and all five rows carrying the same module, dst, file
    # and line. This is why those columns are not a join key, and the assertion
    # is here so the claim in the schema doc stays true rather than becoming
    # folklore: the naive join must still over-pair, and `assign_operand` must
    # still be the thing that does not.
    pairs = con.execute("""
        SELECT count(*) FROM assignment a
        JOIN edge e ON e.module = a.module AND e.dst = a.dst
                   AND e.file IS a.file AND e.line = a.line
        JOIN module m ON m.id = a.module
        WHERE m.name = 'branches'""").fetchone()[0]
    if pairs != 6:
        sys.exit(f"the branches join produced {pairs} pair(s), expected the 2x3 "
                 "cross product; the example no longer demonstrates why "
                 "(module, dst, file, line) cannot recover the relation")
    print(f"ok: the naive edge/assignment join over-pairs, as documented ({pairs})")
    ops = con.execute("""
        SELECT a.seq, n.text FROM assign_operand ao
        JOIN assignment a ON a.id = ao.assignment
        JOIN name n ON n.id = ao.name
        JOIN module m ON m.id = a.module
        WHERE m.name = 'branches' ORDER BY a.seq, n.text""").fetchall()
    if ops != [(0, "a"), (1, "b")]:
        sys.exit(f"branches assign_operand is {ops}, expected [(0,'a'), (1,'b')]; "
                 "the per-statement read set is the one thing the join cannot give")
    print("ok: assign_operand separates the two statements exactly")

if mode == "interfaces":
    check("the interface binding (conn_kind=4)",
          "SELECT count(*) FROM port WHERE conn_kind = 4", 2)
    check("the binding's declared modport", """
        SELECT count(*) FROM port p JOIN name n ON n.id = p.modport
        WHERE p.conn_kind = 4 AND n.text = 'src'""")
    # `relay` owns no interface instance: it forwards its own port, so the row
    # must name that port. A NULL outer here breaks the chain in the middle,
    # and it resolves from neither end.
    check("the pass-through binding names the forwarding port", """
        SELECT count(*) FROM port p
        JOIN module m ON m.id = p.module JOIN name o ON o.id = p.outer
        WHERE m.name = 'relay' AND p.conn_kind = 4 AND o.text = 'bus'""")
    unbound = con.execute("""
        SELECT count(*) FROM port WHERE conn_kind = 4 AND outer IS NULL"""
    ).fetchone()[0]
    if unbound:
        sys.exit(f"{unbound} interface binding(s) have no outer; "
                 "the alias they exist to record is missing")
    print("ok: every interface binding names its outer side")
    check("interface_port symbol rows",
          "SELECT count(*) FROM symbol WHERE kind = 'interface_port'", 2)
    check("interface member writes in hier_ref", """
        SELECT count(*) FROM hier_ref h JOIN name n ON n.id = h.path
        WHERE h.write = 1 AND n.text LIKE 'bus.%'""", 2)
    check("interface member reads in hier_ref", """
        SELECT count(*) FROM hier_ref h JOIN name n ON n.id = h.path
        WHERE h.write = 0 AND n.text LIKE 'bus.%'""")
    # `assign seen = bus.vld && bus.data[0]` reads nothing this module can
    # name, but it plainly drives `seen`. Without a null-source row a driver
    # query answers "nothing drives it" -- and under v2 every module computing
    # from interface members is this case.
    check("a driver row for a target fed only from outside the module", """
        SELECT count(*) FROM edge e JOIN name d ON d.id = e.dst
        WHERE d.text = 'seen' AND e.src IS NULL""")
    # `bus.data` is written three ways in `consumer` -- spaced, commented, and
    # with two different part-selects. All of them are one signal, so all of
    # them intern as one name, with the bits in the range columns.
    check("the differently-spelled references intern as one name", """
        SELECT count(*) FROM name WHERE text = 'bus.data'""")
    # A port tied to a signal with no name here still gets a row. Dropping it
    # made an external tie read exactly like a port nobody connected.
    check("a port tied outside the module keeps its row", """
        SELECT count(*) FROM port p JOIN module m ON m.id = p.module
        WHERE m.name = 'watcher' AND p.conn_kind = 5 AND p.outer IS NULL""")
    # `assign bus.vld = ready; assign bus.data = payload;` -- two outward writes
    # on one line, each fed by its own local signal. The write is a hier_ref row
    # and the read a stmt_read row, so `stmt` is the only thing that says which
    # read fed which write; module/file/line are identical across all four.
    # Pairing on it must give exactly two rows, not the 2x2 cross product.
    chain = con.execute("""
        SELECT hp.text, rn.text FROM hier_ref h
        JOIN name hp ON hp.id = h.path
        JOIN module m ON m.id = h.module
        JOIN stmt_read r ON r.module = h.module AND r.stmt = h.stmt
        JOIN name rn ON rn.id = r.name
        WHERE m.name = 'driver_pair' AND h.write = 1
        ORDER BY hp.text""").fetchall()
    if chain != [("bus.data", "payload"), ("bus.vld", "ready")]:
        sys.exit(f"driver_pair's outward chain is {chain}, expected bus.data<-payload "
                 "and bus.vld<-ready; the statement ordinal is not separating "
                 "two outward writes that share a line")
    print("ok: each outward write pairs with the read that fed it")
    # And the ordinal is what does it: without it these rows share every other
    # column, which is the state this example exists to keep fixed.
    loose = con.execute("""
        SELECT count(*) FROM hier_ref h
        JOIN module m ON m.id = h.module
        JOIN stmt_read r ON r.module = h.module AND r.file IS h.file
                        AND r.line = h.line
        WHERE m.name = 'driver_pair' AND h.write = 1""").fetchone()[0]
    if loose <= len(chain):
        sys.exit(f"pairing driver_pair on file/line gave {loose} row(s), so this "
                 "example no longer shows why `stmt` is needed")
    print(f"ok: the same pairing on file/line alone over-matches ({loose})")
    for bad, what in ((" ", "a space"), ("/*", "a comment"), ("[", "a select")):
        n = con.execute("SELECT count(*) FROM name WHERE instr(text, ?) > 0",
                        (bad,)).fetchone()[0]
        if n:
            sys.exit(f"{n} interned name(s) contain {what}; "
                     "hier_ref paths are not being normalised")
    print("ok: no interned name carries a space, a comment or a select")

if mode == "assertions":
    check("the concurrent assertion's reads", """
        SELECT count(*) FROM stmt_read WHERE construct = 'assert'""", 3)
    check("assume keeps its own word", """
        SELECT count(*) FROM stmt_read WHERE construct = 'assume'""", 3)
    check("cover keeps its own word", """
        SELECT count(*) FROM stmt_read WHERE construct = 'cover'""", 2)
    # The bits a property reads are not resolved, so the row must say so
    # rather than claim the whole signal as fact.
    claimed = con.execute("""
        SELECT count(*) FROM stmt_read
        WHERE src_lo IS NULL AND src_exact = 1""").fetchone()[0]
    if claimed:
        sys.exit(f"{claimed} assertion read(s) claim the whole signal exactly; "
                 "an unresolved range must be inexact")
    print("ok: unresolved assertion reads are marked inexact")

print("OK")
