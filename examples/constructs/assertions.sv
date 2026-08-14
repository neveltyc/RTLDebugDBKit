// Concurrent assertions, split out of constructs.sv.
//
// check-rtl: expect-fail icarus -- concurrent assertions are not in its grammar
//
// Icarus has no `property`/`assert property` support, so it reports the module
// item as a syntax error rather than declining a construct it knows; that is a
// gap in the front end, not evidence about this file. slang and Verilator both
// accept it, and check-rtl.sh will fail if Icarus ever starts to.
//
// What it exercises: an assertion writes nothing, so its reads fit in no table
// keyed on a target -- `edge` needs a `dst` and `assign_operand` needs an
// `assignment` row. Without `stmt_read` every signal a property checks reads as
// though no part of the design looked at it, which for verification-heavy RTL
// is most of what the file says.

module monitored(input logic clk, input logic req, input logic ack,
                 input logic [3:0] tag);
    // Bound to a property, so the reads arrive through an assertion expression
    // rather than an ordinary one.
    property handshake;
        @(posedge clk) req |-> ##1 ack;
    endproperty
    assert property (handshake);

    // A part-select inside a property. The read is recorded; its bits are not
    // -- an assertion expression is walked for value references rather than
    // through slang's path analysis, so the row covers the whole signal. That
    // is the conservative answer and it is what `src_exact` is for.
    property tag_nonzero;
        @(posedge clk) req |-> tag[1:0] != 2'b00;
    endproperty
    assume property (tag_nonzero);

    // Cover publishes its own word rather than being folded into `assert`.
    cover property (@(posedge clk) ack);
endmodule

module assertions;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    logic req = 1'b0;
    logic ack = 1'b0;
    logic [3:0] tag = 4'h0;

    monitored u_mon(.clk(clk), .req(req), .ack(ack), .tag(tag));
endmodule
