// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// check-rtl: expect-fail verilator -- concatenation alias operands unsupported
// check-rtl: expect-fail icarus -- no alias support
//
// `alias {a, b} = c;` is legal (LRM 10.11) and was dropped whole.
//
// A side had to yield exactly one reference, and getNetReferences hands a
// concatenation back as one expression yielding several -- so the statement
// carried no dependency at all and a and b were aliased to nothing.
//
// Each reference is its own side now, paired only against the OTHER written
// side: a and b are different bits of c, not aliases of each other, so the
// pairs are a<->c and b<->c and never a<->b. The mapping is coarse because
// which bits of c each side meets is not tracked; a plain two-name alias keeps
// its exact mapping.

module aliascat (input logic [3:0] ia, ib);
    wire [3:0] a, b;
    wire [7:0] c, d;
    assign a = ia;
    assign b = ib;
    alias {a, b} = c;    // two references on one side
    alias c = d;         // the control: exact mapping
endmodule
