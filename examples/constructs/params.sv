// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// LRM 23.10 -- a parameterisation is part of an instance's identity, and the
// exporter keys a template on (definition, parameter TEXT). Three ways that
// key goes wrong, and the control for each.
//
//   paramfold   two values that PRINT alike. SVInt::toString renders a value
//               with unknown bits, wider than 64 and neither all-x nor all-z
//               as the single letter `X` unless asked for exact unknowns, so
//               folding on the printed text replays the second instance from
//               the first one's analysis and reports a body it never
//               elaborated. These two differ in P[1], which selects opposite
//               generate branches, so the fold is visible in the tree.
//
//   typeparam   two SPELLINGS of one type. slang's instance cache folds
//               `reg1 #(.T(alpha_t))` and `reg1 #(.T(beta_t))` onto one body
//               and elaborates the second no further, so pass 1 ends with a
//               parameterisation group it never gets an analysed body for.
//               The claim is that this costs nothing: every child a template
//               names comes from the analysed body it was built from, so all
//               four flops are stamped and the status stays complete.
//
//   deadarm     a procedural `if` on a parameter, one variant per value.
//               Where the generate form elaborates the untaken branch away,
//               this one keeps its statement -- a simulator executes the
//               procedure and a source view shows the line -- so both arms
//               record their rows and the branch level says which one
//               nothing can reach. v18 dropped the constant out of the
//               gating altogether (a parameter is filtered as a constant
//               operand), leaving the dead arm with no condition at all and
//               `q` reading as unconditionally driven from two places.
//
//   paramrec    a module that instantiates ITSELF, legally, because the
//               parameter shrinks each level. The recursion guard keys on
//               (module, parameters); keyed on the module alone it would cut
//               here at the first level, and a reduction tree, a recursive
//               adder and a Benes network are all written this way. Four
//               levels -- W=8 over two W=4 over four W=2 over eight W=1 --
//               so the module repeats at every level and the pair at none.
//               examples/constructs/recursion.sv is the other side of this
//               guard, and cannot make this claim: everything in that file
//               is fatally errored, so nothing below a cut would be stamped
//               for any reason.

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

module reg1 #(parameter type T = logic) (input logic clk, input T d, output T q);
    always_ff @(posedge clk) q <= d;
endmodule

module typeparam_pair(input logic clk, input logic [7:0] d,
                      output logic [7:0] qa, output logic [7:0] qb);
    typedef logic [7:0] alpha_t;
    typedef logic [7:0] beta_t;
    reg1 #(.T(alpha_t)) u_a (.clk(clk), .d(d), .q(qa));
    reg1 #(.T(beta_t))  u_b (.clk(clk), .d(d), .q(qb));
endmodule

module typeparam(input logic clk, input logic [7:0] d,
                 output logic [7:0] q0a, output logic [7:0] q0b,
                 output logic [7:0] q1a, output logic [7:0] q1b);
    // p1 is the cache hit: its body is never elaborated past the header, and
    // the template walk descends it all the same.
    typeparam_pair p0(.clk(clk), .d(d), .qa(q0a), .qb(q0b));
    typeparam_pair p1(.clk(clk), .d(d), .qa(q1a), .qb(q1b));
endmodule

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

module deadarm #(parameter bit EN = 1'b0)
                (input logic dead, live, output logic q);
    always_comb begin
        if (EN) q = dead;
        else    q = live;
    end
endmodule

module deadarm_pair (input logic dead, live, output logic q_off, q_on);
    deadarm #(.EN(1'b0)) u_off (.dead(dead), .live(live), .q(q_off));
    deadarm #(.EN(1'b1)) u_on  (.dead(dead), .live(live), .q(q_on));
endmodule

module paramrec (input logic [7:0] a, output logic y);
    redtree #(.W(8)) u (.a(a), .y(y));
endmodule

module params (input logic clk, input logic bit_in, input logic [7:0] word_in,
               output logic fold_a, output logic fold_b,
               output logic [7:0] tp0a, output logic [7:0] tp0b,
               output logic [7:0] tp1a, output logic [7:0] tp1b,
               output logic rec_y,
               output logic dead_off, dead_on);
    paramfold u_fold (.i(bit_in), .a(fold_a), .b(fold_b));
    typeparam u_type (.clk(clk), .d(word_in),
                      .q0a(tp0a), .q0b(tp0b), .q1a(tp1a), .q1b(tp1b));
    paramrec  u_rec  (.a(word_in), .y(rec_y));
    deadarm_pair u_dead (.dead(bit_in), .live(~bit_in),
                         .q_off(dead_off), .q_on(dead_on));
endmodule
