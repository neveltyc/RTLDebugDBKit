// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// check-rtl: expect-fail verilator -- SV-2009 pattern matching not implemented
// check-rtl: expect-fail icarus -- SV-2009 pattern matching not implemented
//
// LRM 12.4.2, 12.5.4 and 12.6 -- the branch spellings the open front ends do
// not parse, together because the markers waive a whole file. slang accepts
// all of them, so the RTL is valid; the markers name the tools that cannot
// confirm that, and go stale if either ever can. Verilator parses `priority
// if` and `case … inside`; Icarus parses neither, and neither parses
// `matches`, so the pair of waivers is what the file as a whole needs.
//
// Without its own handler PatternCaseStatement falls to visitDefault, which
// visits the condition -- and the walker has no handler for a bare value
// expression either. Neither the gating nor even the read survives, leaving
// `sel` with zero load rows anywhere despite selecting the branch.

module patterncase (input logic clk, input logic [3:0] sel,
                    input logic [7:0] x, output logic [7:0] q,
                    output logic [7:0] ranged, qualified);
    always_ff @(posedge clk) begin
        case (sel) matches
            4'd0:    q <= x;
            default: q <= 8'd0;
        endcase
    end

    // A range label evaluates to no single constant, so its value is NULL
    // while its reads stay on the item's level like any other label's.
    always_comb begin
        case (sel) inside
            [4'd0:4'd3]: ranged = x;
            4'd7, 4'd9:  ranged = ~x;
            default:     ranged = 8'd0;
        endcase
    end

    // The qualifier belongs to the level, not to the statement under it.
    always_comb begin
        priority if (sel[3]) qualified = x;
        else if (sel[2])     qualified = ~x;
        else                 qualified = 8'd0;
    end
endmodule
