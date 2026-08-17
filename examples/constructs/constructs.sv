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
