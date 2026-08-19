// The v2 constructs the basic example never reaches: self-feedback,
// primitives, statement-level waits, downward hierarchical references,
// part-select and expression port connections, force/release. It also spans
// two files -- a macro at the top and an `include`d task body -- because a
// row's file and line are only provably paired when the statement and its
// procedure live in different files.
//
// Passes scripts/check-rtl.sh (Verilator and Icarus both). Interface
// constructs live in interfaces.sv, which Icarus does not implement yet.

// The driver it expands to is the first one this file contributes, which is
// the case that matters: the exporter learns a file's on-disk origin from the
// first row that mentions it, and a macro body is not a file. Getting that
// wrong left the whole file with no digest to check it against -- and only
// when the macro came first, so a macro further down would not have caught it.
`define DECLARE_TRACE(nm) logic [7:0] nm; assign nm = 8'h5A;

module counter(input logic clk, input logic rst_n, output logic [7:0] cnt);
    logic [7:0] dbg;                        // written only from above, by XMR
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            cnt <= '0;
        else
            cnt <= cnt + 8'd1;              // self-feedback: cnt -> cnt
    end
endmodule

module decode(input logic [3:0] idx, output logic [15:0] onehot);
    assign onehot = 16'b1 << idx;
endmodule

// v10 facts: a delay is a statement's normalised text, never a number this
// tool pretends to evaluate; an undeclared name on a continuous assign's
// left is a real net row with is_implicit set; and one source reaching one
// target from two statements stays two dependencies -- the occurrence
// granularity the old deduplicated edge model erased.
module timing_pair(input logic a, input logic b, output logic r2);
    assign #3 dly_w = a;                    // implicit net, delayed assign
    logic q1;
    always @(a or b) begin
        q1 = #2 a & b;                      // intra-assignment delay
        r2 = a;
        if (b)
            r2 = a;                         // same pair, second statement
    end
    wire dly_r = dly_w | q1;
    logic unused_ok;
    always_comb unused_ok = dly_r;
endmodule

// A net declared with an initialiser is a continuous assignment by the LRM,
// but slang models it as a net carrying an expression rather than as an
// `assign`, so it reached no procedure and `w`/`s` had no driver at all.
module netinit(input logic a, input logic b, output logic y);
    wire w = a & b;
    wire (strong1, weak0) s = a;
    assign y = w | s;
endmodule

// A call binds actuals to formals. The body's write is recorded once; without
// the binding the other half of the chain is missing and `d` reads as unused.
module viacall(input logic clk, input logic [7:0] d, output logic [7:0] q);
    task automatic bump(input logic [7:0] v);
        q <= v + 8'd1;
    endtask
    always_ff @(posedge clk) bump(d);
endmodule

// An assertion writes nothing, so its reads fit no table keyed on a target.
// The concurrent form lives in assertions.sv, which Icarus cannot parse.
module checks(input logic clk, input logic req, input logic ack);
    always_comb assert (req !== 1'bx || ack !== 1'bx);
endmodule

// Statements whose whole effect is to read. Nothing here writes anything the
// module can name, so before these were recorded the signals came back as
// ones no part of the design had ever looked at -- while the source prints
// them and waits on them.
module observers(input logic clk, input logic [7:0] watched, input logic done);
    logic [7:0] loaded [0:1];
    initial begin
        $display("watched=%0h", watched);   // a system task's argument
        wait (done);                        // a wait condition
        // The argument a system task *writes* is not a read of it. Recorded
        // as one, `loaded` said it read itself at the line that loads it.
        $readmemh("nonexistent.hex", loaded);
    end
    final $display("last watched=%0h", watched);
endmodule

module gates(input logic a, input logic b, input logic en, output logic y);
    wire yi, yn, o1, o2, zt, pu;
    wire [2:0] sr;
    and    g_and (yi, a, b);                // n-input gate
    nand   g_nand(yn, a, b);
    buf    g_buf (o1, o2, a);               // n-output gate: outputs first
    bufif1 g_bufz(zt, yi, en);              // enable terminal
    pullup g_pu  (pu);                      // no input terminal at all
    // One net on both terminals, different bits: real dataflow, and the case
    // that a same-symbol self-pairing guard threw away -- and then reported as
    // a gate driving sr[1] from nothing.
    buf    g_sr0 (sr[1], sr[0]);
    buf    g_sr1 (sr[2], sr[1]);
    assign y = zt;
endmodule

// Two assignments to one target, on one line, under a condition. All of
// `edge` and `assignment` agree on module, dst, file and line here, so joining
// the two tables on those columns pairs each statement with every edge --
// including the branch condition's. `assign_operand` is what separates them,
// and the database asserts that it does.
module branches(input logic clk, input logic c, input logic [7:0] a,
                input logic [7:0] b, output logic [7:0] sel);
    always_ff @(posedge clk) begin if (c) sel <= a; else sel <= b; end
endmodule

// Concatenated assignment: both sides are positioned, so the halves correspond.
// Pairing every target with every operand made `swap_lo -> packed_hi` and
// `swap_hi -> packed_lo` -- not conservative but wrong, and marked exact on both
// ends like the two real edges. `bits` is the other half of it: the operands of
// one concatenation land in their own slices of the target rather than each
// driving all of it.
module packing(input logic [3:0] swap_hi, input logic [3:0] swap_lo,
               output logic [3:0] packed_hi, output logic [3:0] packed_lo,
               output logic [7:0] bits);
    assign {packed_hi, packed_lo} = {swap_lo, swap_hi};
    assign bits = {swap_hi, swap_lo};
endmodule

// A dynamic select: which bit of `bus` reaches `q` is decided at runtime, so
// the source range is an upper bound (source_exact=0) and no per-bit mapping
// can be claimed (mapping_exact=0). The index is read as data in its own right.
module pick(input logic [7:0] bus, input logic [2:0] i, output logic q);
    assign q = bus[i];
endmodule

// The bit-precision model crossing the instance boundary. A concatenated
// connection puts each element in its own slice of the formal -- without the
// formal-side window, hi and lo both read as attached to all of q, and the
// per-bit precision edges carry ended at every port.
module bytesink(input logic [7:0] q);
endmodule
module bytesource(output logic [7:0] q);
    assign q = 8'hC3;
endmodule
// A composite formal (`module m(.p({hi, lo}))`) is the fourth window shape and
// already slices the OUTER side per sub-port. It cannot live here: Verilator
// rejects complex ports outright, and check-rtl's expect-fail marker waives a
// whole file rather than one module. slang handles it, and the exporter's
// windows were verified against it by hand -- see the v9 commit.

// A read's load_kind is its semantics, not which table stored it. A plain
// event (`@(posedge clk)`) fits proc_event.signal; a selected-bit event can
// not, and its reads travel through stmt_read -- yet `@(posedge clks[2])` is
// as much a sensitivity read as the plain spelling, and `wait (clks[0])` and
// a statement-level `@(posedge clks[1]);` are both waits. The view must say
// so regardless of the storage split.
module evkinds(input logic [2:0] clks, input logic d, output logic q);
    always_ff @(posedge clks[2]) q <= d;
    initial begin
        @(posedge clks[1]);
        wait (clks[0]);
    end
endmodule

// One definition, two parameterisations: two module variants, two module_ids.
// A driver query by module_name alone returns rows from both, which is why the
// formal interface keys on module_id, taken from the selected tree node --
// name-plus-params is for a human browsing, name alone answers a different
// question than "this instance".
module scaled #(parameter int W = 1)(input logic [W-1:0] d,
                                     output logic [W-1:0] q);
    assign q = ~d;
endmodule

module constructs;
    `DECLARE_TRACE(trace)                   // declared inside a macro body

    logic clk = 1'b0;
    always #5 clk = ~clk;                   // self-feedback, blocking

    logic rst_n = 1'b0;

    logic [7:0]  stim;
    logic [15:0] oh;
    decode u_decode(.idx(stim[3:0]), .onehot(oh));   // part-select connection

    logic [1:0] state = 2'd0;
    localparam logic [1:0] RUN = 2'd1;
    logic gy;
    gates u_gates(.a(clk), .b(state == RUN), .en(1'b1), .y(gy));

    logic [7:0] cnt_o;
    // Written one port per line, which is how any instance wide enough to
    // matter is written. Each connection's row has to name its own line: with
    // the instantiation's line on all of them, no two ports of one instance
    // can be told apart by position, and reading the text back gives the
    // header rather than the connection.
    counter u_cnt(
        .clk   (clk),
        .rst_n (rst_n),
        .cnt   (cnt_o)
    );

    logic ni_y;
    netinit u_netinit(.a(clk), .b(rst_n), .y(ni_y));

    logic [7:0] vq;
    viacall u_viacall(.clk(clk), .d(cnt_o), .q(vq));

    logic ack = 1'b0;
    checks u_checks(.clk(clk), .req(rst_n), .ack(ack));

    logic done = 1'b0;
    observers u_obs(.clk(clk), .watched(cnt_o), .done(done));

    logic [7:0] observed;
    always_comb observed = u_cnt.cnt;       // downward XMR read

    logic [7:0] sel;
    branches u_br(.clk(clk), .c(done), .a(stim), .b(cnt_o), .sel(sel));

    logic [3:0] p_hi, p_lo;
    logic [7:0] p_bits;
    packing u_pack(.swap_hi(stim[7:4]), .swap_lo(stim[3:0]),
                   .packed_hi(p_hi), .packed_lo(p_lo), .bits(p_bits));

    logic pk;
    pick u_pick(.bus(stim), .i(cnt_o[2:0]), .q(pk));
    // One instance carrying the two connection kinds nothing else here has:
    // a port tied to a constant (conn_kind=1) and a port left unconnected
    // (conn_kind=2). Recorded rather than omitted -- "nobody connected it"
    // must stay distinct from "the exporter did not get that far".
    pick u_pick2(.bus(8'h5A), .i(cnt_o[2:0]), .q());

    // A generate loop is a naming level of its own: the tree holds g_rep[0]
    // and g_rep[1] as nodes with neither module nor child row, while the
    // folded `child` rows under `constructs` spell the whole path
    // (`g_rep[0].u_dec`). The two spellings are what instance.child relates.
    for (genvar gi = 0; gi < 2; gi++) begin : g_rep
        logic [15:0] oh_g;
        decode u_dec(.idx(stim[3:0]), .onehot(oh_g));
    end

    logic sc1_q;
    logic [1:0] sc2_d = 2'b00, sc2_q;
    scaled #(.W(1)) u_sc1(.d(trace[0]), .q(sc1_q));
    scaled #(.W(2)) u_sc2(.d(sc2_d), .q(sc2_q));

    // Port windows: an input concat, an output concat, and a replication
    // (two segments, not one deduplicated row).
    bytesink   u_bsink(.q({stim[7:4], cnt_o[3:0]}));
    logic [3:0] bs_a, bs_b;
    bytesource u_bsrc(.q({bs_a, bs_b}));
    logic [3:0] rep_r = 4'h0;
    bytesink   u_brep(.q({2{rep_r}}));

    logic [2:0] ev_clks = 3'b000;
    logic ev_q;
    evkinds u_ev(.clks(ev_clks), .d(done), .q(ev_q));

    logic tp_r2;
    timing_pair u_tp(.a(cnt_o[0]), .b(cnt_o[1]), .r2(tp_r2));

`include "seq.svh"

    initial begin
        stim  = 8'h5A;
        state = RUN;
        @(posedge clk);                     // statement-level wait
        release_reset();                    // its wait and XMR are in seq.svh
        force stim = 8'hA5;
        #1;
        release stim;
    end
endmodule
