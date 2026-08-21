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
#                        of 159,590, all of them in these two tables.
#
#                        Only the id assignment is non-deterministic; the CONTENT
#                        is not, so BOTH tables are compared separately as sorted
#                        sets -- src_file as (path, digest), file as (path,
#                        resolved src_file path). Dropping them from the dump
#                        without that compensation is a real hole rather than a
#                        theoretical one: a database with garbage in file.path
#                        and a NULL src_file_id passes every other check here,
#                        and the file table is precisely what SourceLocator and
#                        linkSourceFiles produce.
#
#   meta.producer_revision   The git describe of the build, which is supposed to
#                        differ between the two binaries being compared.
#
# Anything else that differs is a real behaviour change.
#
# Exit codes are COMPARED, not merely required to be zero, and three fixtures
# expect a non-zero one -- a gate made only of success cases would pass a build
# whose every error path had been rewired to return success.
#
#   usage: refactor-equivalence.sh <reference-binary> [candidate-binary]
#
# The candidate defaults to build/rtl-designdb. Exit status is 0 only when every
# fixture matches.
set -u
set -o pipefail    # else a failing sqlite3 leaves both dumps empty and diff says "same"

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

# Same for `file`, and for the same reason. Dropping both tables from the dump
# without this left the entire as-written-spelling to real-path mapping
# invisible -- which is exactly what SourceLocator and linkSourceFiles produce,
# so the one thing the gate could not see was the thing being refactored. A
# database with garbage in file.path and a NULL src_file_id passed every other
# check here.
files() {
    sqlite3 "$1" "SELECT f.path, COALESCE(s.path, '<null>')
                  FROM file f LEFT JOIN src_file s ON s.id = f.src_file_id
                  ORDER BY 1, 2"
}

# One fixture: export with both binaries, compare, and read the result back.
run() {
    name="$1"; dir="$2"; shift 2
    checked=$((checked + 1))
    printf '%-14s ' "$name"

    (cd "$dir" && "$ref" "$@" -o "$work/$name.ref.db" -q) 2>"$work/$name.ref.log"
    rc_ref=$?
    (cd "$dir" && "$new" "$@" -o "$work/$name.new.db" -q) 2>"$work/$name.new.log"
    rc_new=$?
    # Compared, not merely required to be zero. The commit that rewired every
    # exit path would have passed a gate that only demanded success.
    if [ "$rc_ref" -ne "$rc_new" ]; then
        echo "FAIL (exit $rc_ref vs $rc_new)"; fail=1; return
    fi
    if [ "$rc_ref" -ne 0 ]; then
        # An expected failure: no database to compare, so stderr is the check.
        if ! diff -q "$work/$name.ref.log" "$work/$name.new.log" >/dev/null; then
            echo "FAIL (stderr differs on exit $rc_ref)"
            diff "$work/$name.ref.log" "$work/$name.new.log" | head -10 | sed 's/^/    /' >&2
            fail=1; return
        fi
        echo "ok  (both exit $rc_ref, same message)"
        return
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

    files "$work/$name.ref.db" > "$work/$name.ref.f"
    files "$work/$name.new.db" > "$work/$name.new.f"
    if ! diff -q "$work/$name.ref.f" "$work/$name.new.f" >/dev/null; then
        echo "FAIL (file content)"
        diff "$work/$name.ref.f" "$work/$name.new.f" | head -10 | sed 's/^/    /' >&2
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

# A fixture that must FAIL. Runs both binaries with exactly the given argv --
# run() appends its own -o and -q, which is fine for success cases and wrong for
# a case whose point is the -o. Requires the same non-zero status and the same
# message from both.
run_fail() {
    name="$1"; dir="$2"; want="$3"; shift 3
    checked=$((checked + 1))
    printf '%-14s ' "$name"

    (cd "$dir" && "$ref" "$@") >"$work/$name.ref.out" 2>"$work/$name.ref.log"
    rc_ref=$?
    (cd "$dir" && "$new" "$@") >"$work/$name.new.out" 2>"$work/$name.new.log"
    rc_new=$?

    if [ "$rc_ref" -ne "$want" ]; then
        echo "FAIL (reference exited $rc_ref, fixture expects $want)"; fail=1; return
    fi
    if [ "$rc_ref" -ne "$rc_new" ]; then
        echo "FAIL (exit $rc_ref vs $rc_new)"; fail=1; return
    fi
    if ! diff -q "$work/$name.ref.log" "$work/$name.new.log" >/dev/null ||
       ! diff -q "$work/$name.ref.out" "$work/$name.new.out" >/dev/null; then
        echo "FAIL (output differs on exit $rc_ref)"
        diff "$work/$name.ref.log" "$work/$name.new.log" | head -10 | sed 's/^/    /' >&2
        fail=1; return
    fi
    echo "ok  (both exit $rc_ref, same message)"
}

# ------------------------------------------------------------------ fixtures

run basic "$here" examples/basic/top.sv --top top

# The constructs family: no --top, so slang's own top election is asserted too.
for f in constructs interfaces assertions hierarchy udp unresolved xmr alias \
         external package callsite; do
    [ -f "$here/examples/constructs/$f.sv" ] || continue
    run "$f" "$here" "examples/constructs/$f.sv"
done

# The option matrix. Every fixture above is a bare .sv with -q, which leaves
# whole code paths untouched exactly where the refactor was working:
# --single-unit is the addSeparateUnit call that moved into parseSources,
# --diag is the entire DiagnosticEngine block that moved into reportDiagnostics,
# --check-constraints changes the DDL that actually gets executed, and with no
# +define+ or +incdir+ the two ppOpts loops in buildOptionBag and the matching
# loops in configDigest could be deleted without changing a byte of output.
run single-unit  "$here/examples/options" -f opts.f --single-unit \
                 "+incdir+$here/examples/options"
run multi-unit   "$here/examples/options" -f opts.f "+incdir+$here/examples/options"
run opts-digest  "$here/examples/options" -f opts.f --single-unit \
                 "+incdir+$here/examples/options" +define+EXTRA_WIDTH=4
run constraints  "$here" examples/constructs/constructs.sv --check-constraints
run diag-all     "$here" examples/constructs/external.sv --diag
run diag-capped  "$here" examples/constructs/external.sv --diag 2

# Paths that must FAIL, with the same code and the same message. The commit that
# rewired every exit path would have passed a gate made only of success cases.
run_fail missing-src "$here" 2 examples/constructs/does-not-exist.sv -o "$work/x.db" -q
run_fail bad-top     "$here" 2 examples/basic/top.sv --top no_such_module -o "$work/x.db" -q
# A directory where the output file should go: the temp database writes fine and
# the rename onto it fails, which is publish()'s error path. Created here
# because $work is wiped at startup.
mkdir -p "$work/adir"
run_fail bad-output  "$here" 1 examples/basic/top.sv --top top -o "$work/adir" -q

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

# ------------------------------------------------------------ terminal output
#
# Everything above runs with -q, so the reporting path is untested by it: the
# phase timings, the analysis line, and the note/warning lines that say what the
# export could not do. Those are part of what the tool IS, and a refactor that
# drops one or reorders the phases is a behaviour change the rows cannot show.
# Durations are normalised away; their labels and order are not.
echo
printf '%-14s ' "reporting"
tcheck=0
for f in xmr unresolved package callsite; do
    [ -f "$here/examples/constructs/$f.sv" ] || continue
    for tag in ref new; do
        eval bin=\$$tag
        # Same -o for both: the summary line echoes the output path, so two
        # different names would report as a difference that is only the flag.
        (cd "$here" && "$bin" "examples/constructs/$f.sv" --timing \
            -o "$work/tm.db") >"$work/tm.$tag.out" 2>"$work/tm.$tag.err"
        sed 's/[0-9][0-9]* ms/N ms/' "$work/tm.$tag.err" > "$work/tm.$tag.err.n"
    done
    if ! diff -q "$work/tm.ref.out" "$work/tm.new.out" >/dev/null ||
       ! diff -q "$work/tm.ref.err.n" "$work/tm.new.err.n" >/dev/null; then
        echo "FAIL ($f)"
        diff "$work/tm.ref.err.n" "$work/tm.new.err.n" | head -10 | sed 's/^/    /' >&2
        diff "$work/tm.ref.out" "$work/tm.new.out" | head -10 | sed 's/^/    /' >&2
        fail=1
        break
    fi
    tcheck=$((tcheck + 1))
done
[ "$fail" -eq 0 ] && echo "ok  ($tcheck fixture(s), --timing and notes identical)"

echo
if [ "$fail" -eq 0 ]; then
    if [ -z "$rwa" ] || [ ! -d "$rwa" ]; then
        echo "PASS: $checked fixture(s) byte-identical (examples only -- the three"
        echo "      real designs were NOT run; set RWA_DIR for coverage at scale)"
    else
        echo "PASS: $checked fixture(s) byte-identical"
    fi
else
    echo "FAIL: see above ($checked fixture(s) checked)"
fi
exit $fail
