// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// check-rtl: expect-fail verilator -- a module instantiating itself is illegal
// check-rtl: expect-fail icarus -- a module instantiating itself is illegal
//
// A module that instantiates itself: illegal, and every front end says so --
// including slang, which marks the compilation fatally errored before the
// exporter writes a row. Both markers are declared because this file MUST be
// rejected; check-rtl reports a front end that starts accepting it as STALE.
//
// It is here because "slang rejected it" is not the end of the story. The
// exporter goes on to write the hierarchy-only database its own warning
// promises, and both passes will follow the recursion instead of the design
// unless something stops them. The two modules are the two failure modes, and
// each needs its own guard:
//
//   selfchain   one self-instantiation. slang bounds instantiation DEPTH at
//               128 and hands back the ~130 levels it elaborated before it
//               noticed, so pass 1 finishes -- but those levels all share one
//               (definition, parameters) group, whose template therefore names
//               its own key as a child. Pass 2 unrolls that cycle until the
//               stack runs out: `module m; m u(); endmodule` as exit 139.
//
//   selffan     two self-instantiations. Depth 128 is 2^128 instances, so
//               pass 1 never returns at all -- no crash, an export that runs
//               until someone kills it.
//
// Pass 1 runs first, so the file as a whole shows selffan's hang; the segfault
// is what the chain produces on its own. The cut instance is still stamped --
// its nets, terminals and the connections its parent made to them -- so a
// trace reaches the recursion and stops AT it. One level, not the 130 slang
// happened to reach: that depth is slang's own limit, not a fact about the
// design.

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
