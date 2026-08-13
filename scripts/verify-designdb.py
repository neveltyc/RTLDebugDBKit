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

if mode == "constructs":
    check("the self-feedback edge (cnt -> cnt)", """
        SELECT count(*) FROM edge e
        JOIN name s ON s.id = e.src JOIN name d ON d.id = e.dst
        WHERE s.text = 'cnt' AND d.text = 'cnt'""")
    check("primitive edges", "SELECT count(*) FROM edge WHERE kind='primitive'", 6)
    check("the pullup's null-source row", """
        SELECT count(*) FROM edge
        WHERE construct = 'gate:pullup' AND src IS NULL""")
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

print("OK")
