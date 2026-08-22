// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// check-rtl: expect-fail verilator -- a module instantiating itself is illegal
// check-rtl: expect-fail icarus -- a module instantiating itself is illegal
//
// Illegal RTL, and every front end says so -- including slang, which marks the
// compilation fatally errored before the exporter writes a row. The markers
// above are not waivers for an unimplemented construct; they record that this
// file MUST be rejected, and check-rtl reports a front end that starts
// accepting it as STALE.
//
// It is here because "slang rejected it" was not the end of the story. The
// exporter goes on to write the hierarchy-only database its own warning
// promises, and both walks that database is built from used to follow the
// recursion instead of the design:
//
//   selfchain   one self-instantiation. slang bounds instantiation DEPTH at
//               128 and hands back the ~130 levels it elaborated before it
//               noticed, so pass 1 finished -- but those 130 levels all share
//               one (definition, parameters) group, whose template therefore
//               names its own key as a child. Pass 2 unrolled that cycle until
//               the stack ran out: `module m; m u(); endmodule` was a segfault
//               (exit 139), not a database.
//
//   selffan     two self-instantiations. Depth 128 is now 2^128 instances, and
//               pass 1 never returns at all -- no crash, just an export that
//               runs until someone kills it, which is the worse of the two.
//
// Both modules are here because the two guards are separate, and either one
// alone still fails: with only the pass-2 guard this file hangs, and with only
// the pass-1 guard it exits 139. Pass 1 runs first, so what the file as a
// whole did to the old binary is selffan's hang -- the segfault is what the
// chain produced on its own.
//
// Both walks now stop at an instance whose module is already one of its own
// ancestors. The instance is still stamped, so its nets, terminals and the
// connections its parent made to them are all there and the recursion is
// visible at the one place it happens; only its children are missing, because
// there is no end to them.
//
// One level, not 130: the depth slang reached is its own limit, not a fact
// about the design, and a database that unrolled it would be inviting a reader
// to believe in a hierarchy nobody wrote.

module recursion (input logic a, output logic b);
    logic chain, fan;
    selfchain u_chain (.a(a), .b(chain));
    selffan   u_fan   (.a(a), .b(fan));
    assign b = chain ^ fan;
endmodule

// The stack-overflow case: one child, so the elaborated tree is a chain.
module selfchain (input logic a, output logic b);
    logic mid;
    selfchain u (.a(a), .b(mid));
    assign b = ~mid;
endmodule

// The never-returns case: two children, so the elaborated tree branches.
// Both are cut, so both are counted -- three in this file altogether.
module selffan (input logic a, output logic b);
    logic m1, m2;
    selffan u1 (.a(a), .b(m1));
    selffan u2 (.a(a), .b(m2));
    assign b = m1 | m2;
endmodule
