// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Half of the --single-unit fixture. The macro below is defined HERE and used
// in opts_b.sv, so the two files elaborate to different widths depending on
// whether they share a compilation unit -- which is the whole point of the
// flag, and is invisible to any fixture built from one file.
`define OPTS_SHARED_WIDTH 6

module opts_a (input logic clk, input logic [3:0] d, output logic [3:0] q);
    always_ff @(posedge clk)
        q <= d;
endmodule
