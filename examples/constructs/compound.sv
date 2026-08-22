// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Compound assignment reads its target.
//
// slang does not rewrite `a += b` into `a = a + b`. It builds the right side
// as BinaryExpression(LValueReferenceExpression, b), and an LValueReference is
// a bare placeholder -- no sub-expressions, no link back to the lvalue, and no
// case in ValuePath::visitPaths -- so walking the right side finds b and never
// finds a.
//
// `explicit_self` and `compound_self` must export the same dependencies. The
// interesting one is `shift_self`: its entire right side is the placeholder
// and a constant, so before the fix it recorded no source at all and surfaced
// as driver_kind='constant' -- a tie-off claim on a signal fed by itself.
//
// Not a synthetic concern. On VeeR-EH1 this recovers the GPR read mux
// (`rd0 |= ...` accumulated across two nested loops) and the LSU byte-hit
// reduction (`ld_full_hit_lo_dc2 &= ...`).

module compound (input logic clk, input logic [7:0] x,
                 output logic [7:0] explicit_self, compound_self, shift_self,
                 output logic [7:0] masked);
    always_ff @(posedge clk) begin
        explicit_self = explicit_self + x;   // the control
        compound_self += x;                  // same dependencies as above
        shift_self <<= 2;                    // reads itself, nothing else
        masked &= x;                         // bitwise, still not per-bit
    end
endmodule
