// The v2 constructs the basic example never reaches: self-feedback,
// primitives, statement-level waits, downward hierarchical references,
// part-select and expression port connections, force/release.
//
// Passes scripts/check-rtl.sh (Verilator and Icarus both). Interface
// constructs live in interfaces.sv, which Icarus does not implement yet.

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

module gates(input logic a, input logic b, input logic en, output logic y);
    wire yi, yn, o1, o2, zt, pu;
    and    g_and (yi, a, b);                // n-input gate
    nand   g_nand(yn, a, b);
    buf    g_buf (o1, o2, a);               // n-output gate: outputs first
    bufif1 g_bufz(zt, yi, en);              // enable terminal
    pullup g_pu  (pu);                      // no input terminal at all
    assign y = zt;
endmodule

module constructs;
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
    counter u_cnt(.clk(clk), .rst_n(rst_n), .cnt(cnt_o));

    logic [7:0] observed;
    always_comb observed = u_cnt.cnt;       // downward XMR read

    initial begin
        stim  = 8'h5A;
        state = RUN;
        @(posedge clk);                     // statement-level wait
        rst_n = 1'b1;
        u_cnt.dbg = 8'h42;                  // downward XMR write
        force stim = 8'hA5;
        #1;
        release stim;
    end
endmodule
