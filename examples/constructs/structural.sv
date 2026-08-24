// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// LRM 23 modules and ports, 24 programs, 27 generate -- the structure an
// occurrence model has to keep apart from a definition.
//
//   * the same parameterisation instantiated TWICE is two occurrences with
//     their own rows, not one body two names point at;
//   * a generate array is one naming level and one instance per element, so
//     `lane[2].u_bit` is three lookups rather than a string to parse;
//   * a non-ANSI module declares its directions in the body, and an `inout`
//     terminal arcs both ways -- it is a driver of the outer net and a load
//     of it, and a view that picks one direction loses the other;
//   * a program is its own def_kind. slang's DefinitionKind has three values
//     and `program` is the one no synthesizable fixture reaches, so without a
//     testbench block here the column's third value is never written.

module leaf #(parameter W = 4)(input logic [W-1:0] d, output logic [W-1:0] q);
    assign q = ~d;
endmodule

module oldstyle(a, b, y, t);        // non-ANSI: directions declared in the body
    input  a;
    input  [3:0] b;
    output y;
    inout  t;
    wire   a, t;
    wire   [3:0] b;
    reg    y;
    always @(a or b) y = a ^ (^b);
    assign t = a ? 1'b1 : 1'bz;
endmodule

module hierarchy(input logic clk, input logic [7:0] din,
                 output logic [7:0] dout, inout wire pad);
    // Two occurrences of one parameterisation: same signature, two row sets.
    logic [3:0] lo_q, hi_q;
    leaf #(.W(4)) u_lo (.d(din[3:0]), .q(lo_q));
    leaf #(.W(4)) u_hi (.d(din[7:4]), .q(hi_q));
    assign dout = {hi_q, lo_q};

    // A generate array: each element its own level, instance and rows.
    logic [3:0] g_q;
    for (genvar g = 0; g < 4; g++) begin : lane
        leaf #(.W(1)) u_bit (.d(din[g]), .q(g_q[g]));
    end

    // The non-ANSI child, its inout terminal tied to the pad.
    logic os_y;
    oldstyle u_os (.a(din[0]), .b(din[7:4]), .y(os_y), .t(pad));

    logic sink;
    always_ff @(posedge clk) sink <= os_y | g_q[0];
endmodule

// A testbench block beside the design it watches. It reads and writes nothing
// the design names, so what it contributes is the definition row.
program watcher(input logic clk, input logic [7:0] dout);
    initial $display("dout=%0h", dout);
endprogram

// A port whose type is an unpacked array. It has no PACKED width, only the
// flattened one the schema's bit offsets index, so a width taken from the
// packed one makes the terminal's two sides disagree: the inside maps it
// whole and exactly, the outside cannot place it at all.
module aggport(input logic [7:0] arr [0:1], output logic [7:0] o);
    assign o = arr[0];
endmodule

module structural(input logic clk, input logic [7:0] din,
                  output logic [7:0] dout, inout wire pad,
                  output logic [7:0] agg_o);
    hierarchy u_h (.clk(clk), .din(din), .dout(dout), .pad(pad));
    watcher   u_w (.clk(clk), .dout(dout));

    logic [7:0] mem [0:1];
    aggport   u_ag (.arr(mem), .o(agg_o));
endmodule
