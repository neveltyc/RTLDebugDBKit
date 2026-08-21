// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Two instances whose parameters differ only in bits an abbreviated value
// print would throw away.
//
// A template is keyed by (definition, parameter values), so the parameter text
// IS the identity: fold two values onto one string and the second instance is
// replayed from the first one's analysis, reporting a body it never
// elaborated. `SVInt::toString` prints a value that has unknown bits, is wider
// than 64, and is neither all-x nor all-z as the single letter `X` unless
// asked for exact unknowns -- so these two differ in P[1], which selects
// opposite generate branches, while printing identically under the default.
//
// The assertion this fixture carries: u1 holds `lo.only_when_clear` and u2
// holds `hi.only_when_set`, each exactly once.

module paramfold_sub #(parameter logic [127:0] P = 128'h0)
                     (input logic i, output logic o);
    if (P[1]) begin : hi
        logic only_when_set;
        assign only_when_set = i;
        assign o = only_when_set;
    end
    else begin : lo
        logic only_when_clear;
        assign only_when_clear = ~i;
        assign o = only_when_clear;
    end
endmodule

module paramfold (input logic i, output logic a, output logic b);
    paramfold_sub #(.P({124'b0, 4'b000x})) u1 (.i(i), .o(a));   // P[1] = 0
    paramfold_sub #(.P({124'b0, 4'b001x})) u2 (.i(i), .o(b));   // P[1] = 1
endmodule
