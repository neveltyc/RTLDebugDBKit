#!/usr/bin/env bash
# Export the local open-source CPU designs as extra test cases.
#
# These live outside the repository (they are large third-party trees), so
# this is a developer tool rather than CI: point DESIGNS_DIR at a checkout that
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
designs="${DESIGNS_DIR:-}"

if [ -z "$designs" ]; then
    for cand in "$here/../designs" "$here/../../designs" "$here/../../../designs" \
                "$here/../../../../designs"; do
        if [ -d "$cand/picorv32" ]; then
            designs="$(cd "$cand" && pwd)"
            break
        fi
    done
fi
if [ -z "$designs" ] || [ ! -d "$designs" ]; then
    echo "error: no design directory found; set DESIGNS_DIR" >&2
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
    # 0, 3 and 4 all wrote a database -- complete, partial and hierarchy
    # only. A third-party tree that does not fully elaborate is still worth
    # exporting and checking; only "no database" is a failure here.
    (cd "$dir" && "$bin" "$@" -o "$db")
    rc=$?
    if [ "$rc" != 0 ] && [ "$rc" != 3 ] && [ "$rc" != 4 ]; then
        echo "FAIL: export $name (exit $rc)" >&2
        fail=1
        return
    fi
    end=$(date +%s%N 2>/dev/null || date +%s)
    if ! sqlite3 "$db" "PRAGMA foreign_keys=ON; PRAGMA foreign_key_check;" >/dev/null; then
        echo "FAIL: foreign_key_check $name" >&2
        fail=1
        return
    fi
    if [ -f "$verify" ]; then
        if ! python3 "$verify" "$db" >/dev/null; then
            echo "FAIL: verify $name" >&2
            fail=1
            return
        fi
    fi
    # examples/reorder covers reproducibility in CI at five files; these designs
    # cover it at a scale where a race has room to happen. Two exports, diffed.
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

run picorv32  "$designs/picorv32"  "$designs/picorv32/picorv32.v" --top picorv32 --quiet
run tinyriscv "$designs/tinyriscv" -f tiny.f --top tinyriscv_soc_top --quiet

veer="$designs/veerwolf_run/build/veerwolf_0.7.5/sim-verilator"
if [ -d "$veer" ] && [ -f "$veer/designdb_real.f" ]; then
    run veerwolf "$veer" -f designdb_real.f --top veerwolf_core --quiet
else
    echo "skip: veerwolf (no fusesoc work root at $veer)"
fi

exit $fail
