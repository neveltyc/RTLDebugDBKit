// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Instantiations written without an instance name. NOT a check-rtl fixture:
// the LRM makes the name mandatory for a module instantiation, so both lints
// reject this file -- and that is the point. It is what a design elaborates
// to when the name came from a macro that did not expand. veerwolf's clock
// gating is written TEC_RV_ICG clkhdr (.*) and TEC_RV_ICG rvclkhdr (.*),
// both behind a macro tick; compile it without the macro and those two lines
// stamp 302 nodes with no instance name at all.
//
// slang leaves such a symbol's name empty, and a hierarchical path built
// from an empty name ends at the PARENT -- so the last segment was the
// parent's own name. Every one of these answered to the instance holding it,
// two in one scope answered to each other, and (parent_node_id, name), the
// only lookup the tree has, stopped identifying a node. Each gets a
// synthesised $def$n segment instead: the same shape an anonymous gate gets,
// from the same per-scope counter.

module anon_leaf(input logic a, output logic y);
    assign y = ~a;
endmodule

// One unnamed child, alone in its body: it used to be called `anon_mid`,
// after the very instance it hangs under.
module anon_mid(input logic a, output logic y);
    anon_leaf (.a(a), .y(y));           // $anon_leaf$0
endmodule

module anonymous(input logic a, input logic b, output logic [4:0] y);
    wire g0, g1;
    logic m0, m1, m2;

    // A gate and two instantiations in one scope, siblings of each other.
    // The gate draws from the counter first and the instantiations continue
    // it: one sequence per scope, not one per kind of thing being named.
    buf (g0, a);                        // $buf$0
    anon_leaf (.a(a), .y(m0));          // $anon_leaf$1
    anon_leaf (.a(b), .y(m1));          // $anon_leaf$2

    // No definition anywhere and no instance name either -- an unresolved
    // node, which is also where a nameless UDP instance lands when the
    // compilation has no source for the primitive.
    anon_ghost (.a(a), .y(g1));         // $anon_ghost$3

    anon_mid u_mid (.a(b), .y(m2));

    // A generate level is its own scope, so its own counter: this child is
    // $anon_leaf$0 under g[0] and under g[1], and a sibling of neither.
    for (genvar i = 0; i < 2; i++) begin : g
        anon_leaf (.a(a), .y(y[i]));    // g[i].$anon_leaf$0
    end

    assign y[2] = m0 ^ m1;
    assign y[3] = m2;
    assign y[4] = g0 & g1;
endmodule
