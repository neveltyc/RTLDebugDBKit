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
import sqlite3
import sys

if len(sys.argv) not in (2, 3):
    sys.exit(f"usage: {sys.argv[0]} <design.db> [constructs|interfaces]")

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


def check(what, sql, expect_at_least=1):
    n = con.execute(sql).fetchone()[0]
    if n < expect_at_least:
        sys.exit(f"expected {what}, found {n} row(s)")
    print(f"ok: {what} ({n})")


if mode:
    # The v2 facts every reader relies on, whichever example is loaded.
    version = con.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()
    if not version or version[0] != "2":
        sys.exit(f"schema_version is {version and version[0]}, expected 2")
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

if mode == "constructs":
    check("the self-feedback edge (cnt -> cnt)", """
        SELECT count(*) FROM edge e
        JOIN name s ON s.id = e.src JOIN name d ON d.id = e.dst
        WHERE s.text = 'cnt' AND d.text = 'cnt'""")
    check("primitive edges", "SELECT count(*) FROM edge WHERE kind='primitive'", 6)
    check("the pullup's null-source row", """
        SELECT count(*) FROM edge
        WHERE construct = 'gate:pullup' AND src IS NULL""")
    # One net on both terminals of a gate, different bits. Guarding on the
    # symbol dropped the edge and then called the target undriven, which
    # severed every stage of a gate-level chain and mislabelled it.
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

if mode == "interfaces":
    check("the interface binding (conn_kind=4)",
          "SELECT count(*) FROM port WHERE conn_kind = 4", 2)
    check("the binding's declared modport", """
        SELECT count(*) FROM port p JOIN name n ON n.id = p.modport
        WHERE p.conn_kind = 4 AND n.text = 'src'""")
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

print("OK")
