// LRM 23.8 -- the two ways a name leaves the instance it is written in, which
// resolve differently and must.
//
// `tb_top.glob` climbs out of up_leaf to the enclosing top and back down. The
// body is stamped for two instances, so the level that answers it belongs to
// the occurrence, not to the one body that was analysed: each occurrence's own
// surroundings are searched, and here both find the same net. `tb_top.gmode`
// is the same climb from a CONDITION, which is the half that carries a branch
// and becomes a control dependency rather than a data one.
//
// `cu_glob` lives in the compilation unit, which this model does not stamp.
// There is no net row to resolve onto, so the dependency is written with a
// NULL source net and the reference on the source end; v_driver reports
// driver_kind='external' -- not undriven, not a constant.

logic [7:0] cu_glob;
logic       cu_mode;

module up_leaf(output logic [7:0] up_o, output logic [7:0] ug,
               output logic [7:0] o, output logic [3:0] nib,
               output logic [7:0] g);
    assign up_o = tb_top.glob;      // upward -> resolved per occurrence
    always_comb begin
        if (tb_top.gmode) ug = 8'hFF;  // upward condition -> resolved control
        else              ug = 8'h00;
    end
    assign o    = cu_glob;          // $unit object -> external data
    assign nib  = cu_glob[3:0];     // a window of it -- the window survives
    always_comb begin
        if (cu_mode) g = 8'hFF;     // $unit condition -> external control
        else         g = 8'h00;
    end
endmodule

// The name an upward reference is anchored at is searched for again per
// occurrence, and what it finds there need not be the KIND of thing the
// analysed body found: `blk` is an instance above one occurrence of shadow_rd
// and a generate block above the other. The first resolves. The second is
// where the lookup stopped, and a generate level holds no nets -- so the
// reference resolves to nothing rather than searching on for some other
// `blk` under a level the source did not name.
module shadow_leaf;
    logic [7:0] sig;
endmodule

module shadow_rd(output logic [7:0] o);
    assign o = blk.sig;
endmodule

module shadow_inst(output logic [7:0] o);
    shadow_leaf blk();
    shadow_rd r(.o(o));
endmodule

module shadow_gen(output logic [7:0] o);
    if (1) begin : blk
        logic [7:0] sig;
        assign sig = 8'h5A;
    end
    shadow_rd r(.o(o));
endmodule

module tb_top(output logic [7:0] u1_o, output logic [7:0] ug1,
              output logic [7:0] o1, output logic [3:0] n1,
              output logic [7:0] g1,
              output logic [7:0] u2_o, output logic [7:0] ug2,
              output logic [7:0] o2,
              output logic [7:0] shadow_i, output logic [7:0] shadow_g);
    logic [7:0] glob;
    logic       gmode;
    assign glob  = 8'hA5;
    assign gmode = 1'b1;
    up_leaf u1 (.up_o(u1_o), .ug(ug1), .o(o1), .nib(n1), .g(g1));
    up_leaf u2 (.up_o(u2_o), .ug(ug2), .o(o2), .nib(), .g());  // body is shared
    shadow_inst u_si (.o(shadow_i));
    shadow_gen  u_sg (.o(shadow_g));
endmodule
