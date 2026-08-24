// Interface constructs, split out of constructs.sv.
//
// check-rtl: expect-fail icarus -- interface ports are not in its grammar
//
// Icarus has no interface support at all, so it reports the port list as a
// syntax error rather than declining a construct it knows; that is a gap in
// the front end, not evidence about this file. slang and Verilator accept it,
// and check-rtl.sh will fail if Icarus ever starts to.
//
// What it exercises: an interface port binding (net_conn.conn_kind='interface',
// naming the interface instance, with the declared modport on the terminal),
// interface terminals (term.term_kind='interface'), and interface member
// references from inside the connected modules -- which leave their module and
// land in hier_ref.
//
// Also here: a task DECLARED IN THE INTERFACE and called through a port. Its
// body is walked in the caller's template, so its names -- the interface's
// own variables -- arrive there as bare identifiers of a body the caller
// cannot place. They resolve through the terminal the interface is bound to,
// the same route `bus.vld` takes; without that the interface's nets read as
// undriven while the task plainly wrote them.

interface simple_bus(input logic clk);
    logic       vld;
    logic [7:0] data;
    modport src(output vld, output data, input clk);

    // Writes one of its own variables from another. The formal `x` is not a
    // net of the interface -- no procedure of its own walks this body -- so
    // what it contributes stays an unresolved reference, exactly as a
    // package subroutine's formal does.
    task automatic stamp(input logic [7:0] x);
        data = x ^ {8{vld}};
    endtask
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
// write lands in hier_ref and the read in assign_operand, and only the
// statement they share tells them apart: without it `bus.vld` reads as fed by
// `ready` or by `payload` indifferently, and so does `bus.data`. This is the
// shape interface RTL mostly is -- a modport driver computing from local
// state -- which is why it is the case worth pinning.
module driver_pair(simple_bus.src bus, input logic ready, input logic [7:0] payload);
    assign bus.vld = ready; assign bus.data = payload;
endmodule

module sink(input logic s);
endmodule

// An interface ARRAY as the module's OWN port. One terminal binds an element
// per segment -- the shape a concatenated actual has on an ordinary port --
// and recorded as a single row it named no instance at all, so every member
// reference through it stayed text-only and the interface nets behind it
// read as undriven.
module arrayed(simple_bus bus_arr[2]);
    assign bus_arr[0].vld  = 1'b1;
    assign bus_arr[1].data = 8'hA5;
endmodule

// The same port with two dimensions. The elements of the outer array are
// arrays themselves, so a walk that takes them for instances binds nothing:
// the segments must be the LEAVES, in declaration order, on both the
// connection side and the reference side or the two disagree about which
// segment is which.
module arrayed2(simple_bus grid[2][2]);
    assign grid[0][1].vld  = 1'b1;
    assign grid[1][0].data = 8'h3C;
endmodule

// Calls the interface's own task. The body's write to `data` and its read of
// `vld` belong to the interface instance this occurrence is bound to, not to
// this module, and must arrive as cross-instance dataflow.
module stamper(simple_bus bus, input logic [7:0] din);
    always_comb bus.stamp(din);
endmodule

// The same call with a SECOND interface port in scope. A call does not
// record which port it went through, so the only handle on the body's names
// is the interface body they are declared in -- and here two terminals reach
// it, because `u_pair_same` binds both ports to one instance. That says
// nothing about `u_pair_apart`, which binds them apart: a terminal chosen
// from the analysed occurrence would send the write to `alt`. Both stay
// unresolved rather than one of them being wrong.
module stamp_pair(simple_bus bus, simple_bus alt, input logic [7:0] din);
    always_comb bus.stamp(din);
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

    simple_bus  bus3(clk);
    logic [7:0] stamp_in = 8'h00;
    stamper     u_stamp(.bus(bus3), .din(stamp_in));

    simple_bus bus4(clk);
    simple_bus bus5(clk);
    stamp_pair u_pair_same (.bus(bus4), .alt(bus4), .din(stamp_in));
    stamp_pair u_pair_apart(.bus(bus4), .alt(bus5), .din(stamp_in));

    simple_bus barr[2](clk);
    arrayed    u_arr(.bus_arr(barr));

    simple_bus bgrid[2][2](clk);
    arrayed2   u_grid(.grid(bgrid));
endmodule
