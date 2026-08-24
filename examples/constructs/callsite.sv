// A task called twice, from two call sites with different gating and
// different arguments. The body is walked once per call site (its write
// carries that call's gating), but the formal `v` is one shared net -- so a
// fan-in cone of `r` that does not know call sites mixes call 1's gating (g1)
// with call 2's argument (b), a combination no real call makes. Every row a
// body walk produces carries a call_site_id, so a consumer can follow, at
// each hop through the subroutine, only the rows of one call and keep each
// call's real combination: {g1, a} and {g2, b}, never {g1, b}.
//
// It also carries the two argument forms whose row set is decided by the
// binding rather than by the body: an `output` actual, which the calling
// statement writes as it writes any other target, and a CONSTANT actual,
// which ties the formal off exactly as `.p(8'h5A)` ties off a pin.

module callsite_top(input logic g1, input logic g2,
                    input logic [7:0] a, input logic [7:0] b,
                    input logic [7:0] c, output logic [7:0] r,
                    output logic [7:0] s, output logic [7:0] w);
    task automatic bump(input logic [7:0] v);
        r = v;                 // the body's write, walked once per call site
    endtask
    // A call in a CONTROL EXPRESSION: `pick` is called in the condition, so
    // its binding has no owning statement. Its call_site carries a NULL
    // caller_stmt_id and its argument binding carries a NULL call_site_id --
    // a call site never tags a statement-less row.
    function automatic logic pick(input logic [7:0] v);
        return v != 8'h00;
    endfunction
    // An output actual is written BY the call, so it is a target of the
    // calling statement like any other write -- v_stmt_target's own
    // vocabulary names it.
    task automatic store(input logic [7:0] v, output logic [7:0] o);
        o = v;
    endtask
    // A CONSTANT actual. There is no net to name as the source, so the
    // formal is tied off -- the same fact `.p(8'h5A)` records on a pin,
    // which the call side did not record at all.
    task automatic tie(input logic [7:0] v);
        s = s | v;
    endtask
    always_comb begin
        r = 8'h00;
        s = 8'h00;
        if (g1) bump(a);       // call site 1: gated by g1, argument a
        if (g2) bump(b);       // call site 2: gated by g2, argument b
        if (pick(c)) s = c;    // call site 3: call in a control expression
        store(a, w);           // an output actual: w is written by the call
        tie(8'h5A);            // a constant actual: the formal is tied off
    end
endmodule
