// A task called twice, from two call sites with different gating and
// different arguments. The body is walked once per call site (its write
// carries that call's gating), but the formal `v` is one shared net -- so a
// fan-in cone of `r` that does not know call sites mixes call 1's gating (g1)
// with call 2's argument (b), a combination no real call makes. v13 tags
// every row a body walk produces with a call_site_id, so a consumer can
// follow, at each hop through the subroutine, only the rows of one call and
// keep each call's real combination: {g1, a} and {g2, b}, never {g1, b}.

module callsite_top(input logic g1, input logic g2,
                    input logic [7:0] a, input logic [7:0] b,
                    output logic [7:0] r);
    task automatic bump(input logic [7:0] v);
        r = v;                 // the body's write, walked once per call site
    endtask
    always_comb begin
        r = 8'h00;
        if (g1) bump(a);       // call site 1: gated by g1, argument a
        if (g2) bump(b);       // call site 2: gated by g2, argument b
    end
endmodule
