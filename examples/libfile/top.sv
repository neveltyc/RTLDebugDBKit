// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// The source half of the -v fixture. `m` is defined here AND in the library
// file; a source definition has to win however the filelist orders the two.
module top (output logic o, output logic p);
    m        u_m (.o(o));
    lib_cell u_c (.p(p));
endmodule

module m (output logic o);
    assign o = 1'b0;
endmodule
