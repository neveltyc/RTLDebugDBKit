// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// The control for examples/constructs/recursion.sv: a module that instantiates
// itself and is perfectly legal, because the parameter shrinks each level and
// the generate condition ends it.
//
// The guard those two files share cuts an instance whose module AND parameters
// are already one of its own ancestors'. Keyed on the module alone it would
// cut here too, at the first level -- and a reduction tree, a recursive adder,
// a Benes network are all written this way, so the false positive would land
// on ordinary RTL and truncate a hierarchy nobody would think to check. That
// this file stamps whole is the assertion; the recursion fixture on its own
// cannot make it, since everything in that file is fatally errored and nothing
// below the cut would have been stamped for any reason.
//
// Four levels: W=8 over two W=4 over four W=2 over eight W=1, so fifteen
// `redtree` instances and eight leaves. Every level repeats the module and
// none repeats the pair.

module redtree #(parameter int W = 8) (input logic [W-1:0] a, output logic y);
    if (W == 1) begin : leaf
        assign y = a[0];
    end
    else begin : node
        localparam int LO = W / 2;
        logic lo_y, hi_y;
        redtree #(.W(LO))     u_lo (.a(a[LO-1:0]),  .y(lo_y));
        redtree #(.W(W - LO)) u_hi (.a(a[W-1:LO]),  .y(hi_y));
        assign y = lo_y & hi_y;
    end
endmodule

module paramrec (input logic [7:0] a, output logic y);
    redtree #(.W(8)) u (.a(a), .y(y));
endmodule
