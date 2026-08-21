// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// The other half. `OPTS_SHARED_WIDTH comes from opts_a.sv and only reaches here
// under --single-unit; `EXTRA_WIDTH comes from the command line, so the two
// +define+ paths (filelist and argv) both land in the config digest.
`ifndef OPTS_SHARED_WIDTH
  `define OPTS_SHARED_WIDTH 1
`endif
`ifndef EXTRA_WIDTH
  `define EXTRA_WIDTH 1
`endif
`include "defs.svh"

module opts_b (input logic clk, output logic [`OPTS_SHARED_WIDTH-1:0] wide,
               output logic [`EXTRA_WIDTH-1:0] extra);
    always_ff @(posedge clk) begin
        wide  <= wide + 1'b1;
        extra <= extra + 1'b1;
    end
endmodule
