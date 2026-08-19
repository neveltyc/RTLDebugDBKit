// Deliberately incomplete: `ghost` has no definition anywhere, so its
// instantiation stays a black box. NOT a check-rtl fixture -- both lints
// reject a missing module, and that is the point: the export must still say
// where the hole is, with terminals for what the parent connected so a trace
// stops AT the box rather than losing the connections that reach it.

module unresolved(input logic clk, input logic [3:0] req,
                  output logic [3:0] gnt);
    logic [3:0] mid;
    ghost #(.MODE(2)) u_g (.clk(clk), .req(req), .ack(mid), .extra());
    always_ff @(posedge clk) gnt <= mid;
endmodule
