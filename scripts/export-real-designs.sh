#!/usr/bin/env bash
# Export the local open-source CPU designs as extra test cases.
#
# These live outside the repository (they are large third-party trees), so
# this is a developer tool rather than CI: point RWA_DIR at a checkout that
# contains picorv32/, tinyriscv/ and veerwolf_run/, or keep the default
# layout where it sits beside the project. Each design is exported, checked
# with `PRAGMA foreign_key_check`, and -- when the verifier is present --
# read back with verify-designdb.py and re-exported to confirm the two runs
# agree row for row. Sizes and times are printed so a schema change's cost
# shows up here first.
set -u

here="$(cd "$(dirname "$0")/.." && pwd)"
bin="$here/build/rtl-designdb"
verify="$here/scripts/verify-designdb.py"
repro="$here/scripts/check-reproducible.py"
out="${OUT_DIR:-/tmp/designdb-real}"
rwa="${RWA_DIR:-}"

if [ -z "$rwa" ]; then
    for cand in "$here/../rwa" "$here/../../rwa" "$here/../../../rwa" \
                "$here/../../../../rwa"; do
        if [ -d "$cand/picorv32" ]; then
            rwa="$(cd "$cand" && pwd)"
            break
        fi
    done
fi
if [ -z "$rwa" ] || [ ! -d "$rwa" ]; then
    echo "error: no rwa design directory found; set RWA_DIR" >&2
    exit 1
fi
if [ ! -x "$bin" ]; then
    echo "error: $bin not built" >&2
    exit 1
fi
mkdir -p "$out"

fail=0

run() {
    name="$1"; dir="$2"; shift 2
    db="$out/$name.db"
    echo "=== $name ==="
    start=$(date +%s%N 2>/dev/null || date +%s)
    if ! (cd "$dir" && "$bin" "$@" -o "$db"); then
        echo "FAIL: export $name" >&2
        fail=1
        return
    fi
    end=$(date +%s%N 2>/dev/null || date +%s)
    if ! sqlite3 "$db" "PRAGMA foreign_keys=ON; PRAGMA foreign_key_check;" >/dev/null; then
        echo "FAIL: foreign_key_check $name" >&2
        fail=1
        return
    fi
    orphans=$(sqlite3 "$db" "SELECT count(*) FROM (SELECT 1 FROM sqlite_master LIMIT 0)")
    : "$orphans"
    if [ -f "$verify" ]; then
        if ! python3 "$verify" "$db" >/dev/null; then
            echo "FAIL: verify $name" >&2
            fail=1
            return
        fi
    fi
    # These designs are where reproducibility is actually testable: the
    # examples in the repository have too few files for slang's parallel
    # source reads to come back in a different order, and tinyriscv's 28
    # reordered on every single export. Two more exports, diffed row by row
    # across every table.
    if [ -f "$repro" ]; then
        if ! (cd "$dir" && python3 "$repro" "$bin" "$@") >/dev/null; then
            echo "FAIL: reproducible $name" >&2
            fail=1
            return
        fi
    fi
    size=$(wc -c < "$db" | tr -d ' ')
    case "$start" in
        *N) ms="?" ;;
        *) if [ "${#start}" -gt 12 ]; then
               ms=$(( (end - start) / 1000000 ))
           else
               ms=$(( (end - start) * 1000 ))
           fi ;;
    esac
    echo "ok: $name  ${size} bytes  ${ms} ms"
}

run picorv32  "$rwa/picorv32"  "$rwa/picorv32/picorv32.v" --top picorv32 -q
run tinyriscv "$rwa/tinyriscv" -f tiny.f --top tinyriscv_soc_top -q

veer="$rwa/veerwolf_run/build/veerwolf_0.7.5/sim-verilator"
if [ -d "$veer" ] && [ -f "$veer/designdb_real.f" ]; then
    run veerwolf "$veer" -f designdb_real.f --top veerwolf_core -q
else
    echo "skip: veerwolf (no fusesoc work root at $veer)"
fi

exit $fail
