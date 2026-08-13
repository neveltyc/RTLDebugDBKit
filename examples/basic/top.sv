module register #(parameter int WIDTH = 8) (
    input  logic             clk,
    input  logic [WIDTH-1:0] d,
    output logic [WIDTH-1:0] q
);
    always_ff @(posedge clk) begin
        q <= d;
    end
endmodule

module adder #(parameter int WIDTH = 8) (
    input  logic [WIDTH-1:0] a,
    input  logic [WIDTH-1:0] b,
    output logic [WIDTH-1:0] y
);
    assign y = a + b;
endmodule

module alu (
    input  logic [7:0] a,
    input  logic [7:0] b,
    output logic [7:0] y
);
    adder #(.WIDTH(8)) u_add (
        .a(a),
        .b(b),
        .y(y)
    );
endmodule

module datapath (
    input  logic       clk,
    input  logic [7:0] a,
    input  logic [7:0] b,
    output logic [7:0] y
);
    logic [7:0] q;

    register #(.WIDTH(8)) u_reg (
        .clk(clk),
        .d(a),
        .q(q)
    );

    alu u_alu (
        .a(q),
        .b(b),
        .y(y)
    );
endmodule

module top (
    input  logic       clk,
    input  logic [7:0] a,
    input  logic [7:0] b,
    output logic [7:0] y0,
    output logic [7:0] y1,
    output logic [7:0] extra_q
);
    datapath u_dp0 (
        .clk(clk),
        .a(a),
        .b(b),
        .y(y0)
    );

    datapath u_dp1 (
        .clk(clk),
        .a(b),
        .b(a),
        .y(y1)
    );

    register #(.WIDTH(8)) u_extra_reg (
        .clk(clk),
        .d(a),
        .q(extra_q)
    );
endmodule
