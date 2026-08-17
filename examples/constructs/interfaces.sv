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

// A level that owns no interface instance and only forwards its own port. The
// elaborated interface lives two levels up, so it has no name here; the row
// has to say `bus`, the port, or the chain from `producer` to the instance is
// broken in the middle and resolves from neither end.
module relay(simple_bus.src bus);
    producer u_prod(.bus(bus));
endmodule

// Two outward writes, each fed by a different local signal, on one line. The
// write lands in hier_ref and the read in stmt_read, so before those rows
// carried a statement ordinal the two were indistinguishable: `bus.vld` could
// be read as fed by `ready` or by `payload`, and so could `bus.data`. This is
// the shape interface RTL mostly is -- a modport driver computing from local
// state -- which is why it is the case worth pinning.
module driver_pair(simple_bus.src bus, input logic ready, input logic [7:0] payload);
    assign bus.vld = ready; assign bus.data = payload;
endmodule

module sink(input logic s);
endmodule

// A child port tied to a signal this module cannot name. The row must still
// exist, with a NULL outer: without it, "attached to something outside" and
// "nobody connected it" are the same absence.
module watcher;
    sink u_sink(.s(interfaces.seen));
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
    relay    u_relay(.bus(bus));
    consumer u_cons(.bus(bus), .seen(seen), .nib(nib));
    watcher  u_watch();

    simple_bus  bus2(clk);
    logic       ready = 1'b0;
    logic [7:0] payload = 8'h00;
    driver_pair u_drv(.bus(bus2), .ready(ready), .payload(payload));
endmodule
