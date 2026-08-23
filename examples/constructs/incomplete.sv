// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// check-rtl: expect-fail verilator -- a missing definition and unnamed instances
// check-rtl: expect-fail icarus -- a missing definition and unnamed instances
//
// Elaboration that could not complete, and what the export owes anyway. Both
// markers are declared because every front end MUST reject this file; one that
// starts accepting it is reported as STALE.
//
//   * `ghost` has no definition anywhere, so its instantiation stays a black
//     box. The export has to say where the hole is, with a terminal per
//     connection the parent made, so a trace stops AT the box rather than
//     losing the wires that arrive.
//
//   * an instantiation written with no instance name is what a design
//     elaborates to when the name came from a macro that did not expand --
//     veerwolf's clock gating is `TEC_RV_ICG clkhdr (.*)` behind a macro tick,
//     and compiling it without the macro stamps 302 nodes with no instance
//     name at all. slang leaves such a symbol's name empty, and a hierarchical
//     path built from an empty name ends at the PARENT, so the last segment is
//     the parent's own name: every one of these answers to the instance
//     holding it, two in one scope answer to each other, and
//     (parent_node_id, name) -- the only lookup the tree has -- stops
//     identifying a node. Each gets a synthesised $def$n segment instead, from
//     the same per-scope counter an anonymous gate uses.
//
// The two belong together: `anon_ghost` is both at once, an unnamed
// instantiation of a definition that is missing too.

module unresolved(input logic clk, input logic [3:0] req,
                  output logic [3:0] gnt);
    logic [3:0] mid;
    // `.seq()` is a sequence expression, which is legal against an unresolved
    // name -- slang hands a black box's connections back as AssertionExpr for
    // exactly that reason. Unwrapping only the Simple kind records this one as
    // `unconnected`: an assertion that the parent wired NOTHING, where it
    // wired req and clk.
    ghost #(.MODE(2)) u_g (.clk(clk), .req(req), .ack(mid), .extra(),
                           .seq(req[0] ##1 clk));
    always_ff @(posedge clk) gnt <= mid;
endmodule

module anon_leaf(input logic a, output logic y);
    assign y = ~a;
endmodule

// One unnamed child, alone in its body: taking the last segment of its path
// names it `anon_mid`, after the very instance it hangs under.
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

module incomplete(input logic clk, input logic [3:0] req, input logic a,
                  input logic b, output logic [3:0] gnt, output logic [4:0] y);
    unresolved u_unres (.clk(clk), .req(req), .gnt(gnt));
    anonymous  u_anon  (.a(a), .b(b), .y(y));
endmodule
