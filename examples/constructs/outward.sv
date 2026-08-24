// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// check-rtl: expect-fail icarus -- no output arguments on a function, and
// no queue methods
//
// References that leave the instance and have no dotted path to be stored
// under. A reference with no hier_ref row leaves a net_dep with a null source
// AND a null reference -- exactly the shape v_driver classifies as a CONSTANT,
// so the database would claim a signal fed from outside is tied off, and the
// gating would go with it.
//
// Three shapes, three causes:
//
//   * a $unit-scope object, whose name is bare -- no '.' and no '::' to
//     recognise it by;
//   * a name built partly by a macro. canonicalPath declines an upward
//     reference, which then falls back to slicing the source buffer -- and
//     that returns nothing when the reference's ends sit in different
//     buffers. `\`TOPNAME.glob` and `outward_tb.glob` are the same reference
//     and only the second survives that. The symbol knows its own name in
//     both cases, which is what the fallback uses.
//   * a subroutine declared outside the instance body -- here in a package.
//     Its formal is not a net of this module, so the binding has no source to
//     name. What holds `taken` up is the write-back: a dependency with no
//     source beside the target row that says the statement writes the actual.
//     Recording either without the other leaves v_stmt_target and
//     v_net_attachment naming the statement while v_driver names nothing.
//
// `setit` is the third shape with the reading half removed. It matters because
// `bump`'s input actual is itself an outward name, so `bump`'s statement has
// an unresolved read on it -- and "every target has a dependency" is satisfied
// by that read rather than by the write-back. With no argument to read,
// nothing stands in.
//
// `chk` is the same write from a CONDITION, which belongs to no statement this
// schema records. There is no stmt_target to hang it on -- a target is a
// position within a statement -- so the dependency goes out on its own, with
// no source, no statement and no target row: "something writes this, and the
// database cannot say what or where", which is what the null-source discipline
// is for.

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

    // A built-in method registers as a system call in slang, but nothing
    // here leaves the language: stmt_kind must say `call`, construct the
    // method's own word.
    int log_q[$];
    always_ff @(posedge clk)
        log_q.push_back(int'(q));
endmodule

module outward_tb;
    logic clk;
    logic [7:0] glob;
    logic [7:0] q, gated, taken, setb, condb;
    logic hit;
    outward_leaf u (.clk(clk), .q(q), .gated(gated), .taken(taken),
                    .setb(setb), .condb(condb), .hit(hit));
endmodule
