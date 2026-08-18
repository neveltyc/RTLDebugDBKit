// Dataflow that crosses an instance boundary by NAME rather than by port.
// The occurrence model can resolve these to the real object, so they must
// become real dependencies -- a hierarchical write that produced only a
// hier_ref row left the target reading as constant-driven, which is a wrong
// answer rather than a missing one.
//
// Also here: a task called from two places under different conditions, and
// a signal that appears only in a sensitivity list.
//
// Passes scripts/check-rtl.sh (Verilator and Icarus both).

module leaf;
    logic x;
    logic [7:0] wide;
    logic en;
    logic rst;
    logic [7:0] g;
endmodule

module sink(input logic [7:0] p, output logic [7:0] seen);
    assign seen = p;
endmodule

module xmr(input logic clk, input logic a, input logic [7:0] d,
           input logic g1, input logic g2, input logic sens_only,
           output logic q, output logic [3:0] slice_o, output logic tick);
    leaf u();

    // Read across the boundary: u.x drives q.
    assign q = u.x;
    // Write across the boundary: a drives u.x.
    always_comb u.x = a;
    // A part-select of a downward reference: bits carry across too.
    assign slice_o = u.wide[3:0];
    // Gated write to a downward target: g1 is a control dependency whose
    // target lives in another instance.
    always_ff @(posedge clk)
        if (g1)
            u.wide <= d;

    // TWO statements gated by DOWNWARD conditions, in one procedure. The
    // second is the case that matters: the per-statement condition vectors
    // are indexed in lockstep, so a stale entry here attributes gate2's
    // edge to gate1's signal, or drops it -- silently, and consistently
    // enough to look like a fact.
    logic [1:0] gated;
    always_ff @(posedge clk) begin
        if (u.en)  gated[0] <= a;
        if (u.rst) gated[1] <= d[0];
    end

    // Two call sites, two conditions. The task body's write must inherit
    // each caller's gating, not only the first one's.
    logic [1:0] hits;
    task automatic put(input logic v);
        hits[0] <= v;
    endtask
    always_ff @(posedge clk) begin
        if (g1) put(a);
        if (g2) put(d[0]);
    end

    // A signal read by nothing but the sensitivity list. Without an event
    // row it reads as though the design never looks at it.
    always @(sens_only)
        tick <= ~tick;

    // A port connection tied to something with no name in this instance,
    // beside a constant that tiles the rest of the formal. The tie resolves
    // downward, so it crosses as a real arc rather than stopping at the
    // hier_ref row -- the path that had no coverage at all until this.
    logic [7:0] ext_seen;
    sink u_sink (.p({u.g[7:4], 4'h0}), .seen(ext_seen));
endmodule
