// Interface constructs, split out of constructs.sv.
//
// check-rtl: expect-fail icarus -- interface ports are not in its grammar
//
// Icarus has no interface support at all, so it reports the port list as a
// syntax error rather than declining a construct it knows; that is a gap in
// the front end, not evidence about this file. slang and Verilator accept it,
// and check-rtl.sh will fail if Icarus ever starts to.
//
// What it exercises: an interface port binding (port.conn_kind=4, with the
// declared modport), interface_port symbol rows, and interface member
// references from inside the connected modules -- which leave their module
// and land in hier_ref.

interface simple_bus(input logic clk);
    logic       vld;
    logic [7:0] data;
    modport src(output vld, output data, input clk);
endinterface

module producer(simple_bus.src bus);
    assign bus.vld  = 1'b1;
    assign bus.data = 8'hA5;
endmodule

module consumer(simple_bus bus, output logic seen, output logic [3:0] nib);
    assign seen = bus.vld && bus.data[0];
    // Spelled three ways on purpose: one reference interns as one name only if
    // whitespace and comments come out, and a trailing select belongs in
    // path_lo/path_hi rather than in the text.
    assign nib  = bus . data[3:0] & bus/*same signal*/.data[7:4];
endmodule

module interfaces;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    simple_bus bus(clk);
    logic seen;
    logic [3:0] nib;
    producer u_prod(.bus(bus));
    consumer u_cons(.bus(bus), .seen(seen), .nib(nib));
endmodule
