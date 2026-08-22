// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// check-rtl: expect-fail verilator -- concatenation alias operands unsupported
// check-rtl: expect-fail icarus -- no alias support
//
// LRM 10.11 -- `alias {a, b} = c;`, the alias shape that gets dropped whole.
// A side that must yield exactly one reference cannot take it: getNetReferences
// hands a concatenation back as one expression yielding several, so the
// statement carries no dependency and a and b are aliased to nothing.
//
// Each reference is its own side, paired only against the OTHER written side.
// a and b are different bits of c, not aliases of each other, so the pairs are
// a<->c and b<->c and never a<->b. Which bits of c each side meets is not
// tracked, so those mappings are coarse; the two-name alias beside them is the
// control that keeps an exact one.

module aliascat (input logic [3:0] ia, ib);
    wire [3:0] a, b;
    wire [7:0] c, d;
    assign a = ia;
    assign b = ib;
    alias {a, b} = c;    // two references on one side
    alias c = d;         // the control: exact mapping
endmodule
