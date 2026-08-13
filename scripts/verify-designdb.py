#!/usr/bin/env python3
# Copyright (c) 2026 neveltyc
# released under the BSD 3-Clause License (see LICENSE)
#
# Read an exported database back and fail if it is hollow. A build that links
# proves the slang pin resolves; this proves the exporter still writes rows.
# CI runs it against examples/basic/top.sv on every platform binary it builds,
# so it asserts only what that small design must produce, not exact counts.
import sqlite3
import sys

if len(sys.argv) != 2:
    sys.exit(f"usage: {sys.argv[0]} <design.db>")

con = sqlite3.connect(sys.argv[1])
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
print("OK")
