// Instance-level identity, exercised: the same parameterisation twice is two
// occurrences with their own rows; a generate array of instances is one node
// and one instance per element; a non-ANSI module keeps its declared
// directions; an inout terminal arcs both ways.
//
// Passes scripts/check-rtl.sh (Verilator and Icarus both).

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
