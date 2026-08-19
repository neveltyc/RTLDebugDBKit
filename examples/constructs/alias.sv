// check-rtl: expect-fail icarus -- it does not parse `alias` at all.
//
// `alias` binds nets into one object. It has no direction and drives
// nothing, so it is neither an assignment nor a connection: exporting
// nothing for it left the two halves disconnected, and a trace that
// reached one side stopped there.
//
// A bit-selected alias (`alias w[3:0] = n;`) is not exercised here: no
// front end available will lint one, though the exporter handles it.

module aliased(input logic [7:0] in_side, output logic [7:0] out_side,
               output logic [7:0] mid_side);
    wire [7:0] left, right, third;
    // Three names bound mutually, not in a chain: the LRM binds every
    // pair, so a trace from any one of them reaches both others in a
    // single hop.
    alias left = right = third;
    assign left = in_side;
    assign out_side = third;
    assign mid_side = right;
endmodule

module alias_top(input logic [7:0] din, output logic [7:0] dout,
                 output logic [7:0] dmid);
    aliased u_a (.in_side(din), .out_side(dout), .mid_side(dmid));
endmodule
