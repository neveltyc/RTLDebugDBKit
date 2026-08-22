// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// check-rtl: expect-fail verilator -- SV-2009 pattern matching not implemented
// check-rtl: expect-fail icarus -- SV-2009 pattern matching not implemented
//
// `case … matches`, alone, because no open front end implements it.
//
// Both markers are declared rather than the file being left unchecked: slang
// accepts the construct, so the RTL is valid, and saying which tools cannot
// confirm that is the honest record. If either ever implements it, check-rtl
// reports the marker as stale rather than silently keeping a waiver.
//
// The gap this pins: PatternCaseStatement had no handler, so it fell to
// visitDefault -- which visits the condition, and the walker has no handler
// for a bare value expression. Neither the gating nor even the read was
// recorded, and `sel` had zero load rows in the entire database despite
// selecting the branch.

module patterncase (input logic clk, input logic [3:0] sel,
                    input logic [7:0] x, output logic [7:0] q);
    always_ff @(posedge clk) begin
        case (sel) matches
            4'd0:    q <= x;
            default: q <= 8'd0;
        endcase
    end
endmodule
