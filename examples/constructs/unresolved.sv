// check-rtl: expect-fail verilator -- `ghost` has no definition anywhere
// check-rtl: expect-fail icarus -- `ghost` has no definition anywhere
//
// Deliberately incomplete: `ghost` has no definition, so its instantiation
// stays a black box. Both markers are declared rather than the file being left
// unchecked -- every front end MUST reject a missing module, and that is the
// point: the export has to say where the hole is, with terminals for what the
// parent connected, so a trace stops AT the box rather than losing the
// connections that reach it.

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
