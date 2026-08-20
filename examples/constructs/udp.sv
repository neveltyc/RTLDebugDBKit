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
            inout wire a, inout wire b, output wire w,
            inout wire ra, inout wire rb, output wire m);
    latch_p u_l (q, d, en);
    tranif1 u_t (a, b, en);
    // A resistive switch and a MOS switch: slang registers both as plain
    // Fixed gates, so prim_kind='switch' used to miss them.
    rtran   u_r (ra, rb);
    nmos    u_n (m, d, en);
    buf     u_b (z, q);
    anon_gates u_anon (.a(d), .b(en), .y(w));
endmodule

// Gates written without an instance name -- legal, and how cell models are
// usually written. Such a symbol has no name of its own, so its
// hierarchical path ends at its parent: taking the last segment named every
// one of these after the enclosing instance, and resolving that segment
// returned all four gates plus the instance itself.
module anon_gates(input logic a, input logic b, output wire y);
    wire t1, t2, t3;
    buf (t1, a);
    not (t2, b);
    and (t3, t1, t2);
    buf #1 (y, t3);
endmodule
