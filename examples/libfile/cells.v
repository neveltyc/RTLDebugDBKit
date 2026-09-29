// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// A `-v` library: `m` duplicates a source module and must lose to it without
// a warning, `lib_cell` fills in what the sources leave undefined, and `spare`
// is instantiated by nobody, so it must not become a top.
module m (output logic o);
    assign o = 1'b1;
endmodule

module lib_cell (output logic p);
    assign p = 1'b1;
endmodule

module spare (output logic z);
    assign z = 1'b0;
endmodule
