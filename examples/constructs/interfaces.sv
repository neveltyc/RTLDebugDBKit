// Interface constructs, split out of constructs.sv: Icarus does not
// implement interfaces ("sorry"), so scripts/check-rtl.sh cannot vouch for
// this file with both front ends. slang and Verilator both accept it.
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

module consumer(simple_bus bus, output logic seen);
    assign seen = bus.vld && bus.data[0];
endmodule

module interfaces;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    simple_bus bus(clk);
    logic seen;
    producer u_prod(.bus(bus));
    consumer u_cons(.bus(bus), .seen(seen));
endmodule
