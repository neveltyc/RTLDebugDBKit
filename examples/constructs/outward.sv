// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// check-rtl: expect-fail icarus -- no output arguments on a function
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
// no driver at all. Recording only the write-back's TARGET row, which came
// next, left the two views that answer "what writes this net" disagreeing:
// v_stmt_target and v_net_attachment named the statement, v_driver named
// nothing. The dependency beside it carries no source -- the formal is not a
// net here to name -- which is the shape v_driver documents as
// `driver_kind='procedure'` with a NULL driver.
//
// `setit` is the same shape with the reading half removed. It matters because
// the input actual of `bump` is itself an outward name, so `bump`'s statement
// has an unresolved read on it -- and the invariant that every target has a
// dependency was satisfied by that read rather than by the write-back. With
// no argument to read, nothing stands in.
//
// `chk` is the same write from a CONDITION, which belongs to no statement this
// schema records. There is no stmt_target to hang it on -- a target is a
// position within a statement -- so the dependency goes out on its own, with
// no source, no statement and no target row: "something writes this, and the
// database cannot say what or where". That is the answer the null-source
// discipline is for, and it beats the one this used to give, which was that
// nothing wrote it at all.

`define TOPNAME outward_tb

package outward_pkg;
    task automatic bump(input logic [7:0] a, output logic [7:0] y);
        y = a + 8'd1;
    endtask
    task automatic setit(output logic [7:0] o);
        o = 8'hA5;
    endtask
    function automatic bit chk(input logic [7:0] a, output logic [7:0] o);
        o = a ^ 8'h0F;
        return |a;
    endfunction
endpackage

logic       unit_en;         // $unit scope: bare names, no separator
logic [7:0] unit_cfg;

module outward_leaf (input logic clk, output logic [7:0] q, gated, taken,
                     output logic [7:0] setb, output logic [7:0] condb,
                     output logic hit);
    import outward_pkg::*;
    always_ff @(posedge clk)
        q <= `TOPNAME.glob;              // macro-built upward reference
    always_ff @(posedge clk)
        if (unit_en) gated <= unit_cfg;  // both the read AND the gating
    always_comb bump(unit_cfg, taken);   // package task: formal is outward
    always_comb setit(setb);             // the same, with nothing read
    always_comb begin                    // and the same from a condition
        hit = 1'b0;
        if (chk(unit_cfg, condb))
            hit = 1'b1;
    end
endmodule

module outward_tb;
    logic clk;
    logic [7:0] glob;
    logic [7:0] q, gated, taken, setb, condb;
    logic hit;
    outward_leaf u (.clk(clk), .q(q), .gated(gated), .taken(taken),
                    .setb(setb), .condb(condb), .hit(hit));
endmodule
