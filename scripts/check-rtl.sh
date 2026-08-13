#!/bin/bash
# Validate SystemVerilog against two independent open-source front ends before
# using it as a test case for the exporter.
#
# Why this exists: several "defects" chased during development turned out to be
# invalid RTL written by hand -- a continuous assign with a non-constant lvalue
# select, a reversed slice on an ascending unpacked array. slang rejected them
# correctly and the time went into investigating a tool that was behaving.
# Anything that fails here is a bug in the test, not in the exporter.
#
# Verilator and Icarus are both used because they disagree, and the disagreement
# is the useful part -- but it is a prompt to check the LRM, not a verdict:
#
#   assign q[i] = …   variable index    Verilator accepts, Icarus rejects,
#                                       and the LRM is with Icarus.
#   mem[0:1] <= …     unpacked slice    Verilator accepts, Icarus rejects,
#                                       and the LRM is with Verilator (Icarus
#                                       does not implement SV-2009 array slices).
#
# One front end agreeing with you is not evidence; two disagreeing means read
# the standard.
#
#   usage: check-rtl.sh <file.sv> [top-module]
#
# Exit status is 0 only when both accept it -- unless the file declares that
# one of them cannot, with a marker among its first 15 lines:
#
#   // check-rtl: expect-fail icarus -- no interface support
#
# That is for a front end that has not implemented the construct at all, which
# is not evidence the RTL is wrong. It is deliberately not a blanket waiver: a
# declared failure that starts passing fails this script too, so the marker
# cannot outlive the limitation it documents.

set -u
file="${1:?usage: check-rtl.sh <file.sv> [top]}"
top="${2:-}"

# The file's own directory is on the include path, which is where a quoted
# `include names first for every tool that implements one. Without it a fixture
# split across a header cannot be checked at all, and splitting one is how a
# cross-file bug gets a test.
incdir="$(dirname "$file")"

have() { command -v "$1" >/dev/null 2>&1; }

vrc=0
if have verilator; then
    vout=$(verilator --lint-only -Wno-fatal --timing -sv -I"$incdir" "$file" \
                     ${top:+--top-module "$top"} 2>&1) || vrc=$?
else
    vout="(verilator not installed)"; vrc=127
fi

irc=0
if have iverilog; then
    iout=$(iverilog -g2012 -t null -I"$incdir" "$file" 2>&1) || irc=$?
else
    iout="(iverilog not installed)"; irc=127
fi

# A declared expected failure, per tool.
expected() { head -15 "$file" | grep -qiE "^[[:space:]]*//[[:space:]]*check-rtl:.*expect-fail[[:space:]]+$1"; }

status() {
    rc=$1; tool=$2
    if expected "$tool"; then
        # Passing while declared failing is a failure of its own: the marker
        # is stale and the file should simply be checked like any other.
        [ "$rc" -eq 0 ] && echo STALE && return
        [ "$rc" -eq 127 ] && echo SKIP || echo XFAIL
        return
    fi
    [ "$rc" -eq 0 ] && echo OK || { [ "$rc" -eq 127 ] && echo SKIP || echo FAIL; }
}

vst=$(status $vrc verilator)
ist=$(status $irc icarus)
printf '%-30s verilator:%-5s icarus:%-5s\n' "$(basename "$file")${top:+ [$top]}" "$vst" "$ist"

[ "$vst" = FAIL ] && \
    printf '%s\n' "$vout" | grep -E '^%(Error|Warning)' | head -4 | sed 's/^/    verilator: /'
[ "$ist" = FAIL ] && \
    printf '%s\n' "$iout" | grep -Ei 'error|sorry' | head -4 | sed 's/^/    icarus:    /'
for t in "verilator:$vst" "icarus:$ist"; do
    [ "${t#*:}" = STALE ] && printf '    %s now accepts this file; drop its expect-fail marker\n' "${t%%:*}"
done

# Success is every front end either accepting the file or failing exactly as
# the file said it would. SKIP is not success: a front end that is absent has
# vouched for nothing. Nor is STALE: the marker has outlived its reason.
ok() { [ "$1" = OK ] || [ "$1" = XFAIL ]; }
ok "$vst" && ok "$ist"
