#!/usr/bin/env bash
# Prove two builds of the exporter write the same database.
#
# A pure refactor must not change a single row. This exports every fixture with
# both binaries and compares the SQLite dumps -- which is a stronger check than
# the schema verifier, since it compares values, not just invariants.
#
# Two things have to be normalised out before the comparison, and exactly two:
#
#   src_file / file      Row ids here follow the order the source manager hands
#                        back its buffers, and parsing runs on a thread pool --
#                        so two runs of the SAME binary disagree on a multi-file
#                        design. Measured on veerwolf: 492 differing lines out
#                        of 159,590, all of them in these two tables. The id
#                        assignment is not deterministic; the CONTENT is, so the
#                        tables are compared as a sorted (path, digest) set
#                        rather than dropped.
#
#   meta.producer_revision   The git describe of the build, which is supposed to
#                        differ between the two binaries being compared.
#
# Anything else that differs is a real behaviour change.
#
#   usage: refactor-equivalence.sh <reference-binary> [candidate-binary]
#
# The candidate defaults to build/rtl-designdb. Exit status is 0 only when every
# fixture matches.
set -u

here="$(cd "$(dirname "$0")/.." && pwd)"
ref="${1:?usage: refactor-equivalence.sh <reference-binary> [candidate-binary]}"
new="${2:-$here/build/rtl-designdb}"
work="${WORK_DIR:-${TMPDIR:-/tmp}/designdb-equiv}"
verify="$here/scripts/verify-designdb.py"

for bin in "$ref" "$new"; do
    if [ ! -x "$bin" ]; then
        echo "error: $bin is not executable" >&2
        exit 2
    fi
done

# Absolute, because every fixture runs from the design's own directory and a
# relative binary path would resolve against that instead of the cwd.
ref="$(cd "$(dirname "$ref")" && pwd)/$(basename "$ref")"
new="$(cd "$(dirname "$new")" && pwd)/$(basename "$new")"

rm -rf "$work"
mkdir -p "$work"
echo "reference: $ref"
echo "candidate: $new"
echo

fail=0
checked=0

# The dump, minus the two non-deterministic tables and the revision row.
normalise() {
    sqlite3 "$1" .dump |
        grep -v "^INSERT INTO src_file VALUES" |
        grep -v "^INSERT INTO file VALUES" |
        grep -v "^INSERT INTO meta VALUES('producer_revision'"
}

# src_file's content IS deterministic -- only its id assignment is not. Compare
# the (path, digest) pairs as a set, so a genuinely lost or altered source still
# fails even though the ids are ignored above.
sources() {
    sqlite3 "$1" "SELECT path, digest FROM src_file ORDER BY path, digest"
}

# One fixture: export with both binaries, compare, and read the result back.
run() {
    name="$1"; dir="$2"; shift 2
    checked=$((checked + 1))
    printf '%-14s ' "$name"

    if ! (cd "$dir" && "$ref" "$@" -o "$work/$name.ref.db" -q) 2>"$work/$name.ref.log"; then
        echo "FAIL (reference export)"; sed 's/^/    /' "$work/$name.ref.log" >&2; fail=1; return
    fi
    if ! (cd "$dir" && "$new" "$@" -o "$work/$name.new.db" -q) 2>"$work/$name.new.log"; then
        echo "FAIL (candidate export)"; sed 's/^/    /' "$work/$name.new.log" >&2; fail=1; return
    fi

    normalise "$work/$name.ref.db" > "$work/$name.ref.sql"
    normalise "$work/$name.new.db" > "$work/$name.new.sql"
    if ! diff -q "$work/$name.ref.sql" "$work/$name.new.sql" >/dev/null; then
        n=$(diff "$work/$name.ref.sql" "$work/$name.new.sql" | grep -c '^[<>]')
        echo "FAIL ($n differing rows)"
        diff "$work/$name.ref.sql" "$work/$name.new.sql" | head -20 | sed 's/^/    /' >&2
        fail=1; return
    fi

    sources "$work/$name.ref.db" > "$work/$name.ref.src"
    sources "$work/$name.new.db" > "$work/$name.new.src"
    if ! diff -q "$work/$name.ref.src" "$work/$name.new.src" >/dev/null; then
        echo "FAIL (src_file content)"
        diff "$work/$name.ref.src" "$work/$name.new.src" | head -10 | sed 's/^/    /' >&2
        fail=1; return
    fi

    # stderr too: the warning and note lines are part of what the tool reports,
    # and a refactor that drops one is a behaviour change the rows cannot show.
    if ! diff -q "$work/$name.ref.log" "$work/$name.new.log" >/dev/null; then
        echo "FAIL (stderr differs)"
        diff "$work/$name.ref.log" "$work/$name.new.log" | head -10 | sed 's/^/    /' >&2
        fail=1; return
    fi

    if ! sqlite3 "$work/$name.new.db" "PRAGMA foreign_keys=ON; PRAGMA foreign_key_check;" | head -1 | grep -q .; then
        : # empty output means no violations
    else
        echo "FAIL (foreign_key_check)"; fail=1; return
    fi

    if [ -f "$verify" ]; then
        if ! python3 "$verify" "$work/$name.new.db" >"$work/$name.verify.log" 2>&1; then
            echo "FAIL (verify-designdb)"; tail -5 "$work/$name.verify.log" >&2; fail=1; return
        fi
    fi

    rows=$(wc -l < "$work/$name.ref.sql" | tr -d ' ')
    echo "ok  ($rows rows identical)"
}

# ------------------------------------------------------------------ fixtures

run basic "$here" examples/basic/top.sv --top top

# The constructs family: no --top, so slang's own top election is asserted too.
for f in constructs interfaces assertions hierarchy udp unresolved xmr alias \
         external package callsite; do
    [ -f "$here/examples/constructs/$f.sv" ] || continue
    run "$f" "$here" "examples/constructs/$f.sv"
done

# Real designs, when a local checkout is present. These are what actually
# exercise scale -- the examples are a few hundred rows, veerwolf is 160k.
rwa="${RWA_DIR:-}"
if [ -z "$rwa" ]; then
    for cand in "$here/../rwa" "$here/../../rwa" "$HOME/Documents/Projects/rwa"; do
        if [ -d "$cand/picorv32" ]; then rwa="$(cd "$cand" && pwd)"; break; fi
    done
fi
if [ -n "$rwa" ] && [ -d "$rwa" ]; then
    [ -f "$rwa/picorv32/picorv32.v" ] &&
        run picorv32 "$rwa/picorv32" "$rwa/picorv32/picorv32.v" --top picorv32
    [ -f "$rwa/tinyriscv/tiny.f" ] &&
        run tinyriscv "$rwa/tinyriscv" -f tiny.f --top tinyriscv_soc_top
    veer="$rwa/veerwolf_run/build/veerwolf_0.7.5/sim-verilator"
    [ -f "$veer/designdb_real.f" ] &&
        run veerwolf "$veer" -f designdb_real.f --top veerwolf_core
else
    echo "note: no rwa checkout found; real designs skipped (set RWA_DIR)"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS: $checked fixture(s) byte-identical"
else
    echo "FAIL: see above ($checked fixture(s) checked)"
fi
exit $fail
