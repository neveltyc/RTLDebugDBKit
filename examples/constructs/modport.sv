// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// check-rtl: expect-fail icarus -- interface ports are not in its grammar
// check-rtl: expect-fail verilator -- internal error on an explicit modport
// expression (V3Const "Not linked")
//
// LRM 25.5.4 -- the explicit modport expression, in a file of its own
// because Verilator crashes on it and waiving interfaces.sv whole would
// cost that file its lint.
//
// `modport renamed(output .d(data))` gives the port a name the interface
// declares nothing under: `p.d` names no symbol, only the modport's
// connection expression says it is `data`. Resolving through it is what
// keeps `data` from reading as undriven. Only a PLAIN name resolves; a
// connection through a select (`.n(data[3:0])`) has the port's bit
// geometry, not the net's, and a reference range replayed onto the wider
// net would claim bits it does not touch -- that one stays unresolved.

interface rn_bus;
    logic       vld;
    logic [7:0] data;
    modport renamed(output .d(data), output .v(vld), output .n(data[3:0]));
endinterface

module renamer(rn_bus.renamed p, output logic done);
    assign p.d = 8'h3C;
    assign p.v = 1'b0;
    assign done = 1'b1;
endmodule

// The select form, in its own module so its unresolved reference sits
// beside the resolved ones rather than replacing them.
module narrow_renamer(rn_bus.renamed p);
    assign p.n = 4'h7;
endmodule

module modport_top;
    rn_bus bus();
    logic done;
    renamer u_ren(.p(bus), .done(done));
    rn_bus bus2();
    narrow_renamer u_nar(.p(bus2));
endmodule
