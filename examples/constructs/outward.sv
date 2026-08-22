// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// References that leave the instance and had no path to be stored under.
//
// A reference only got a hier_ref row if its recovered text contained a '.'
// or a '::'. Two shapes fail that and were dropped entirely -- and a dropped
// reference leaves a net_dep with a null source AND a null reference, which is
// exactly the shape v_driver classifies as a CONSTANT. The database said a
// signal fed from outside was tied off, and the gating went with it.
//
//   * a $unit-scope object, whose name is bare;
//   * a name built partly by a macro. canonicalPath has no case for a
//     hierarchical value, so every cross-module reference falls back to
//     slicing the source buffer -- which returns nothing when the reference's
//     ends sit in different buffers. `\`TOPNAME.glob` was dropped where
//     `outward_tb.glob` was recorded, for the same reference.
//
// The symbol knows its own name in both cases, so that is the fallback now.
//
// Third shape, unrelated cause: a subroutine declared outside the instance
// body -- here in a package. Its formal is not a net of this module, and the
// binding used to be dropped whole, taking the actual with it, so `taken` had
// no driver at all.

`define TOPNAME outward_tb

package outward_pkg;
    task automatic bump(input logic [7:0] a, output logic [7:0] y);
        y = a + 8'd1;
    endtask
endpackage

logic       unit_en;         // $unit scope: bare names, no separator
logic [7:0] unit_cfg;

module outward_leaf (input logic clk, output logic [7:0] q, gated, taken);
    import outward_pkg::*;
    always_ff @(posedge clk)
        q <= `TOPNAME.glob;              // macro-built upward reference
    always_ff @(posedge clk)
        if (unit_en) gated <= unit_cfg;  // both the read AND the gating
    always_comb bump(unit_cfg, taken);   // package task: formal is outward
endmodule

module outward_tb;
    logic clk;
    logic [7:0] glob;
    logic [7:0] q, gated, taken;
    outward_leaf u (.clk(clk), .q(q), .gated(gated), .taken(taken));
endmodule
