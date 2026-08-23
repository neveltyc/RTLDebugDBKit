// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// check-rtl: expect-fail verilator -- no support for complex (non-ANSI) ports
//
// Port shapes whose terminals cannot be found by name.
//
// A port name is neither unique nor, for a MultiPort, the name the connection
// carries -- so looking a terminal up by it fails three ways, from one cause:
//
//   * two unnamed ports collapse onto one synthesized `<unnamed>`, which
//     writes two term_map rows with the same (term, ordinal) and ABORTS the
//     export on the UNIQUE constraint -- legal source, exit 1, no database;
//   * slang expands a MultiPort connection into one PortConnection per MEMBER,
//     so `.p({hi, lo})` arrives as `hi` and `lo` -- names no terminal answers
//     to, so the connection is dropped and `bus` has no load at all;
//   * a port whose reference carries a select sets BOTH internalSymbol and
//     internalExpr, and testing the symbol first maps a 2-bit terminal onto
//     the whole of a 4-bit net and calls the mapping one-to-one.

module portshape_mp (.p({hi, lo}), y);
    input  [3:0] hi;
    input  [3:0] lo;
    output y;
    assign y = ^{hi, lo};
endmodule

// Two ports with no external name, each a select of a wider net.
module portshape_anon (a[1:0], b[1:0], z);
    input  [3:0] a;
    input  [3:0] b;
    output z;
    assign z = ^{a, b};
endmodule

module portshape (input logic [7:0] bus, input logic [1:0] p, q,
                  output logic y, z);
    portshape_mp   u_mp   (.p(bus), .y(y));
    portshape_anon u_anon (p, q, z);
endmodule
