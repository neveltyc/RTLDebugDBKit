// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// One of four leaves, each in its own file so the filelist has enough
// entries for slang to read them on a thread pool.
module a_leaf (input logic a, output logic y);
    assign y = ~a;
endmodule
