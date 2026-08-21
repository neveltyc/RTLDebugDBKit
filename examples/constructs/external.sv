// What still leaves the model after v13 taught packages to resolve: an
// UPWARD hierarchical reference from a shared body. `tb_top.glob` climbs out
// of up_leaf to the enclosing top and back down; the body is stamped for two
// instances, so the one analysed body cannot speak for where each occurrence
// actually sits, and the reference stays unresolved. The dependency is
// written with a NULL source net and the reference on the source end;
// v_driver reports driver_kind='external' -- not undriven, not a constant.

module up_leaf(output logic [7:0] o, output logic [3:0] nib,
               output logic [7:0] g);
    assign o   = tb_top.glob;       // whole upward object -> external data
    assign nib = tb_top.glob[3:0];  // a window of it -- the window survives
    always_comb begin
        if (tb_top.gmode) g = 8'hFF; // upward condition -> external control
        else              g = 8'h00;
    end
endmodule

module tb_top(output logic [7:0] o1, output logic [3:0] n1,
              output logic [7:0] g1, output logic [7:0] o2);
    logic [7:0] glob;
    logic       gmode;
    assign glob  = 8'hA5;
    assign gmode = 1'b1;
    up_leaf u1 (.o(o1), .nib(n1), .g(g1));
    up_leaf u2 (.o(o2), .nib(), .g());   // second instance: body is shared
endmodule
