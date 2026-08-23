// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// One of five self-contained modules; see reorder.f for what the set is for.
module a_leaf (input logic a, output logic y);
    assign y = ~a;
endmodule
