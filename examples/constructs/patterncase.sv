// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// check-rtl: expect-fail verilator -- SV-2009 pattern matching not implemented
// check-rtl: expect-fail icarus -- SV-2009 pattern matching not implemented
//
// LRM 12.6 -- `case … matches`, alone in a file because no open front end
// implements it. slang accepts the construct, so the RTL is valid; the markers
// name the tools that cannot confirm that, and go stale if either ever can.
//
// Without its own handler PatternCaseStatement falls to visitDefault, which
// visits the condition -- and the walker has no handler for a bare value
// expression either. Neither the gating nor even the read survives, leaving
// `sel` with zero load rows anywhere despite selecting the branch.

module patterncase (input logic clk, input logic [3:0] sel,
                    input logic [7:0] x, output logic [7:0] q);
    always_ff @(posedge clk) begin
        case (sel) matches
            4'd0:    q <= x;
            default: q <= 8'd0;
        endcase
    end
endmodule
