// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Two things slang's API gets asked for and will answer wrongly if asked
// wrongly.
//
// A location inside a macro expansion. getFileName and getLineNumber expand
// internally; getColumnNumber does not -- its precondition is a FILE location,
// and a macro location's buffer holds an ExpansionInfo, so it finds no
// FileInfo and returns 0. File and line named the expansion site while the
// column said 0, which is not a column in any 1-based numbering. On VeeR-EH1
// that was 208 of 11,087 statement rows, all inside `assert expansions.
//
// And an output argument. Expression::bindLValue wraps the actual in an
// AssignmentExpression, so a plain-reference test on the raw argument sees the
// wrapper and every output/inout binding claimed map_exact=0 -- including one
// exactly as wide as its formal.

`define DRIVE(D, S) assign D = (S)

module macroloc (input logic [7:0] a, output logic [7:0] via_macro, direct,
                 output logic [7:0] out_arg);
    `DRIVE(via_macro, a);        // the row for this must carry a real column
    assign direct = a;           // the control, never in a macro

    logic [7:0] scratch;
    task automatic pass(input logic [7:0] i, output logic [7:0] o);
        o = i;
    endtask
    always_comb pass(a, scratch);   // whole-to-whole, so map_exact must be 1
    assign out_arg = scratch;
endmodule
