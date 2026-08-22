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
    logic [7:0] split;
    logic [7:0] far_mem [0:3];
    logic [7:0] tied;
endmodule

module sink(input logic [7:0] p, output logic [7:0] seen);
    assign seen = p;
endmodule

module xmr(input logic clk, input logic a, input logic [7:0] d,
           input logic g1, input logic g2, input logic sens_only,
           input logic quiet_gate,
           output logic q, output logic [3:0] slice_o, output logic tick,
           output logic [3:0] sp_hi, output logic [3:0] sp_lo,
           output logic seen, output logic far_o);
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

    // A condition gating statements that write nothing this instance
    // names. There is no dependency to hang it on, so it used to vanish
    // entirely -- in any procedure, implicit sensitivity or not.
    always @* begin
        if (quiet_gate)
            $display("%0b", d[0]);
    end

    // One reference, split across two targets: the dependencies take their
    // own halves, while the reference itself named the whole of u.split.
    assign {sp_hi, sp_lo} = u.split;

    // A task called from two places, reading outside the instance from
    // each: per-call-site walking makes two statements, so there are two
    // references, each belonging to its own.
    task automatic sample();
        seen <= u.x;
    endtask
    always_comb begin
        if (g1) sample();
        if (g2) sample();
    end

    // A system task that writes its argument. The source is a file, not a
    // net -- but the memory IS driven, and reporting nothing said the
    // design never wrote it.
    logic [7:0] loaded_mem [0:3];
    initial $readmemh("nonexistent.hex", loaded_mem);

    // The same write, but the memory lives in another instance -- and a
    // constant driving an outward target. Both had a source of a kind the
    // schema cannot name AND a target it could only reach by reference, so
    // both used to record the reference and no driver at all: a trace back
    // from the far net said nothing ever wrote it.
    initial $readmemh("nonexistent.hex", u.far_mem);
    assign u.tied = 8'h5A;
    assign far_o = u.far_mem[0][0] ^ u.tied[0];

    // A port connection tied to something with no name in this instance,
    // beside a constant that tiles the rest of the formal. The tie resolves
    // downward, so it crosses as a real arc rather than stopping at the
    // hier_ref row -- the path that had no coverage at all until this.
    logic [7:0] ext_seen;
    sink u_sink (.p({u.g[7:4], 4'h0}), .seen(ext_seen));

    // The same tie on an OUTPUT formal, which is the mirror image: the
    // crossing runs the other way, so the end written hierarchically is the
    // one being driven rather than the one driving. Its spelling therefore
    // rides the LOAD side of the arc, and nothing in this design exercised
    // that branch before.
    sink u_sink2 (.p(8'h00), .seen(u.split));
endmodule
