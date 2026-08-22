// Two spellings of one type, and what slang does with them: `alpha_t` and
// `beta_t` are the same type, so the instance cache folds `reg1 #(.T(alpha_t))`
// and `reg1 #(.T(beta_t))` onto one body and elaborates the second no
// further. The exporter's group key is the parameter TEXT, so it sees two
// parameterisations where slang saw one.
//
// The consequence is a template built from a body the analysis never
// analysed -- it holds no procedure at all. It costs no row: every child a
// template names comes from the analysed body it was built from, so nothing
// is ever stamped from that group, and all four reg1 occurrences carry the
// flop. The export says so as a note rather than letting a template built
// and thrown away pass unremarked.
//
// It is also the shape that shows why `analysis_status` cannot rest on
// slang's scope count: the analysis here runs, reports four scopes, and
// still leaves a body group behind.

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
