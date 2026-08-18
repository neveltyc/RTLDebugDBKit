// check-rtl: expect-fail verilator -- UDP tables and tran switches are not
// in its supported subset; Icarus accepts both.
//
// A user-defined primitive and a bidirectional switch: primitive rows with
// their own kinds, and dependencies that carry primitive provenance rather
// than a statement's.

primitive latch_p(output reg q, input d, input en);
    table
    //  d  en : q : q+
        1  1  : ? : 1;
        0  1  : ? : 0;
        ?  0  : ? : -;
    endtable
endprimitive

module udps(input logic d, input logic en, output wire q, output wire z,
            inout wire a, inout wire b);
    latch_p u_l (q, d, en);
    tranif1 u_t (a, b, en);
    buf     u_b (z, q);
endmodule
