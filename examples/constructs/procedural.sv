// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// LRM 9 processes, 11.4.1 assignment operators, 12 procedural statements and
// 13 subroutines -- the forms whose walk is not the obvious one, each paired
// with the spelling it must agree with.
//
//   compound    slang does not rewrite `a += b` into `a = a + b`. It builds
//               the right side as BinaryExpression(LValueReferenceExpression,
//               b), and an LValueReference is a bare placeholder -- no
//               sub-expressions, no link back to the lvalue, and no case in
//               ValuePath::visitPaths -- so walking the right side finds b and
//               never finds a. `explicit_self` spells the read out and is the
//               control. `shift_self` is the one that matters: its entire
//               right side is the placeholder and a constant, so a walk that
//               misses the placeholder finds no source at all and the net
//               surfaces as driver_kind='constant' -- a tie-off claim on a
//               signal fed by itself. Not synthetic: on VeeR-EH1 this is the
//               GPR read mux (`rd0 |= ...` across two nested loops) and the
//               LSU byte-hit reduction (`ld_full_hit_lo_dc2 &= ...`).
//
//   stmtgaps    `return e;` writes the subroutine's implicit result variable,
//               which slang does not synthesise as an assignment -- the target
//               is SubroutineSymbol::returnValVar, and reached any other way
//               the body records nothing. `do … while (c)` falls to
//               visitDefault without a handler of its own, and visitDefault
//               visits the condition, for which there is no bare
//               value-expression handler either. A call in a loop condition
//               belongs to no statement, so a loop handler that walks its
//               condition with call bindings still enabled attributes it to
//               whatever statement precedes it. `release` drives nothing, so
//               it carries no dependency for its gating to ride. And a for
//               loop's INITIALISER runs once, before the condition is ever
//               evaluated, so it is not gated by it -- the body is.
//               (`case … matches` has the same gap as `do … while` and lives
//               in patterncase.sv, which no open front end can lint.)
//
//   evkinds     LRM 9.2.2.3 and 9.4.2. `always_latch` is its own procedure
//               kind, and `@(edge s)` its own edge kind: a walk that maps
//               only posedge and negedge silently records the level-sensitive
//               spelling as one of them or as none.
//
//   macroloc    slang calls that answer wrongly when asked wrongly.
//               getFileName and getLineNumber expand a macro location
//               internally; getColumnNumber does not -- its precondition is a
//               FILE location, and a macro location's buffer holds an
//               ExpansionInfo, so it finds no FileInfo and returns 0. File and
//               line name the expansion site while the column says 0, which is
//               not a column in any 1-based numbering. And
//               Expression::bindLValue wraps an output actual in an
//               AssignmentExpression, so a plain-reference test on the raw
//               argument sees the wrapper and claims map_exact=0 for every
//               output binding -- including one exactly as wide as its formal.
//               The two output arguments are also the pair that separates
//               recognising that wrapper by expression kind from asking
//               AssignmentExpression::isLValueArg. bindLValue types the
//               placeholder from the FORMAL and fromComponents converts it to
//               the ACTUAL, so `scratch` -- same type -- arrives bare, while
//               `nib` arrives inside a Conversion. A kind test matches only
//               the first, and the second falls through to ordinary
//               assignment handling: no operands on the right side, hence a
//               source-less dependency, hence the constant tie-off in
//               v_driver that this guard exists to prevent. Only an actual
//               whose type differs from its formal reaches the wrapped form,
//               which is why the same-width control alone did not catch it.

module compound (input logic clk, input logic [7:0] x,
                 output logic [7:0] explicit_self, compound_self, shift_self,
                 output logic [7:0] masked);
    always_ff @(posedge clk) begin
        explicit_self = explicit_self + x;   // the control
        compound_self += x;                  // same dependencies as above
        shift_self <<= 2;                    // reads itself, nothing else
        masked &= x;                         // bitwise, still not per-bit
    end
endmodule

module stmtgaps (input logic clk, input logic [7:0] x, k,
                 input logic [3:0] sel, input logic b, g,
                 output logic [7:0] assign_style, ret_style,
                 output logic [7:0] matched, looped, sum,
                 output logic held);

    function automatic logic [7:0] f_assign(input logic [7:0] a);
        f_assign = a ^ k;              // the implicit-variable spelling
    endfunction
    function automatic logic [7:0] f_return(input logic [7:0] a);
        return a | k;                  // must record the same shape
    endfunction
    assign assign_style = f_assign(x);
    assign ret_style    = f_return(x);

    always_ff @(posedge clk) begin
        case (sel)                     // sel must appear as a control driver
            4'd0:    matched <= x;
            default: matched <= 8'd0;
        endcase
    end

    always_ff @(posedge clk) begin
        do begin looped <= x; end while (b);   // b must be a control driver
    end

    integer i;
    always_comb begin
        sum = 8'd0;
        for (i = 0; i < 4; i = i + 1)  // `i = 0` is NOT gated by `i < 4`
            sum = sum + x;
    end

    always_comb begin
        if (g) force held = b;
        else   release held;           // the release is gated by g too
    end
endmodule

// A level-sensitive procedure and an edge-agnostic event, neither of which
// has a posedge/negedge spelling to be mistaken for.
//
// And the two statements that used to produce nothing at all. `-> fired`
// is what makes an event variable happen: with no row it had waiters and
// no cause, so a trace back from it said the design never touches it, and
// the condition gating the trigger vanished with the statement. `disable`
// names a block rather than a net, so it contributes only its own gating
// -- which is the whole point, since that read had nowhere else to go.
module evkinds (input logic clk, input logic en, input logic ev,
                input logic [7:0] d,
                output logic [7:0] latched, output logic [7:0] both_q,
                output logic [7:0] waited);
    event fired;
    always_latch if (en) latched <= d;
    always @(edge ev) both_q <= d;
    always @(posedge clk) if (en) -> fired;
    always @(fired) waited <= d;
    always @(posedge clk) begin : stopper
        if (en) disable stopper;
        waited <= d;
    end
endmodule

`define DRIVE(D, S) assign D = (S)

module macroloc (input logic [7:0] a, output logic [7:0] via_macro, direct,
                 output logic [7:0] out_arg, output logic [3:0] narrow_arg);
    `DRIVE(via_macro, a);        // the row for this must carry a real column
    assign direct = a;           // the control, one line below, never expanded

    logic [7:0] scratch;
    logic [3:0] nib;
    task automatic pass(input logic [7:0] i, output logic [7:0] o);
        o = i;
    endtask
    always_comb begin
        pass(a, scratch);   // whole-to-whole, so map_exact must be 1
        // The same copy-back, with an actual the formal's type does not
        // match. bindLValue builds the placeholder from the FORMAL's type
        // and fromComponents then converts it to the ACTUAL's, so this one
        // reaches the walk wrapped in a Conversion where `scratch` above
        // arrives bare -- and a copy-back recognised by expression kind
        // rather than by AssignmentExpression::isLValueArg sees only the
        // bare form. The wrapped one fell through to ordinary assignment
        // handling, whose right side has no operands, and `nib` gained a
        // source-less dependency on top of its real `procedure` one: a
        // CONSTANT tie-off in v_driver on a signal the task plainly drives.
        pass(a, nib);       // narrower: the formal's low nibble reaches it,
    end                     // and stays its ONLY driver
    assign out_arg = scratch;
    assign narrow_arg = nib;
endmodule

// LRM 12.4 and 12.5 -- the gating a statement sits under, which v18 recorded
// as a flat set of nets and nothing more. Every `if` here assigns the SAME
// target from both arms, so a database that cannot tell the arms apart
// reports two unconditional drivers of it; every case arm differs from its
// siblings only by its label, so one that cannot tell those apart hands each
// arm every other arm's labels. The qualifiers ride along because slang
// carries them on the same node and nothing else in the corpus reaches them.
module gating (input  logic clk, rst_n, en,
               input  logic [7:0] a, b, c,
               input  logic [3:0] sel,
               output logic [7:0] q, y, z, w, v);

    // Three levels deep, both arms assigning at each. `else if` is not
    // flattened: slang desugars it into an else arm holding a new
    // conditional, so `q <= c` is gated else -> else -> else.
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)      q <= '0;
        else if (en)     q <= a;
        else if (sel[0]) q <= b;
        else             q <= c;
    end

    // The qualifier belongs to the level, not to the statement under it.
    // (`priority if` and `case … inside` are the two spellings Icarus does
    // not parse, and live in patterncase.sv beside `case … matches`.)
    always_comb begin
        priority case (sel)
            4'd8:    z = a;
            4'd4:    z = b;
            default: z = c;
        endcase
    end

    // `?` is a synonym for z in a label, so the evaluated value reads 4'b1zzz
    // -- the elaborated constant, not the spelling.
    always_comb begin
        unique casez (sel)
            4'b1???: y = a;
            4'b01??: y = b;
            default: y = c;
        endcase
    end

    always_comb begin
        unique0 casex (sel)
            4'b??01: w = a;
            4'b0010: w = b;
            default: w = c;
        endcase
    end

    // Two labels on one arm: both are that item's, and v18 put every arm's
    // labels on one gate where a consumer could not tell whose was whose.
    always_comb begin
        case (sel)
            4'd7, 4'd9: v = a;
            4'd1:       v = b;
            default:    v = c;
        endcase
    end
endmodule

// LRM 12.7 -- what a loop level has to say, and the four ways taking its
// index for granted goes wrong.
//
//   passed      an index read as a VALUE is a real read: `add1(i)` feeds the
//               formal, and suppressing it there would leave the formal with
//               no driver at all -- the one direction a debug database must
//               not err in. Only the dataflow paths drop it.
//   header      an index is a variable the header STEPS. Here it steps kk
//               and initialises acc, so acc is an ordinary variable of the
//               body; taking every initialised variable for an index
//               suppressed it as an operand AND as a target, and acc's rows
//               vanished outright -- `carried` traced to a signal nothing
//               drove. kk is an index and starts outside the header, so its
//               level names it and publishes no space: a start value the
//               header does not give is not one to assume.
//   descending  `foreach` runs its dimension LEFT to right, which descends
//               for the ordinary packed declaration: publishing 0 upward
//               mirrors every window a consumer reconstructs.
//   never       a loop the stop condition rejects on its first test is dead
//               code, and says so where an unreachable arm does.
module loopspace (input logic [7:0] d, din,
                  output logic [7:0] summed, carried, mirrored, dead);
    function automatic logic [7:0] add1(input logic [7:0] v);
        add1 = v + 8'd1;
    endfunction

    integer i;
    always_comb begin
        summed = 8'd0;
        for (i = 0; i < 4; i = i + 1) summed = summed + add1(i[7:0]);
    end

    integer kk;
    logic [7:0] acc;
    always_comb begin
        kk = 0;
        for (acc = 8'd0; kk < 4; kk = kk + 1) acc = acc + d;
        carried = acc;
    end

    always_comb foreach (din[j]) mirrored[j] = din[j];

    always_comb begin
        dead = 8'd0;
        for (int z = 4; z < 4; z++) dead = d;
    end
endmodule

// A level exists because the source spells it, not because something under
// it landed a row. A `case` arm has always been materialised for that reason
// -- one arm missing makes "which values fall through to default"
// unanswerable -- while an `if` arm and a loop body were materialised on
// demand: this pair recorded ONE arm where the source has two, and no loop
// level at all, so a reader counting senses got a level the design does not
// have and missed one it does.
module emptylevel (input logic clk, input logic en, input logic gate,
                   input logic [7:0] a, output logic [7:0] q);
    always_ff @(posedge clk) begin
        if (en) ;                             // an arm that gates nothing
        else    q <= a;
        for (int i = 0; i < 2; i = i + 1) ;   // a body that does nothing
        // A level that gates nothing AT ALL. The condition is read by the
        // design; with the read filed on a statement there was no statement
        // to file it on, and `gate` appeared in no row of the database.
        if (gate) ; else ;
    end
endmodule

module procedural (input logic clk, input logic [7:0] x, k,
                   input logic [3:0] sel, input logic b, g, en, ev,
                   output logic [7:0] explicit_self, compound_self,
                   output logic [7:0] shift_self, masked,
                   output logic [7:0] assign_style, ret_style,
                   output logic [7:0] matched, looped, sum, output logic held,
                   output logic [7:0] latched, both_q, waited,
                   output logic [7:0] via_macro, direct, out_arg,
                   output logic [3:0] narrow_arg,
                   input  logic rst_n,
                   output logic [7:0] gated_q, gated_y, gated_z, gated_w,
                   output logic [7:0] gated_v,
                   output logic [7:0] summed, carried, mirrored, dead,
                   output logic [7:0] empty_arm_q);
    compound u_cmp (.clk(clk), .x(x), .explicit_self(explicit_self),
                    .compound_self(compound_self), .shift_self(shift_self),
                    .masked(masked));
    stmtgaps u_stg (.clk(clk), .x(x), .k(k), .sel(sel), .b(b), .g(g),
                    .assign_style(assign_style), .ret_style(ret_style),
                    .matched(matched), .looped(looped), .sum(sum), .held(held));
    evkinds  u_ev  (.clk(clk), .en(en), .ev(ev), .d(x), .latched(latched),
                    .both_q(both_q), .waited(waited));
    macroloc u_mac (.a(x), .via_macro(via_macro), .direct(direct),
                    .out_arg(out_arg), .narrow_arg(narrow_arg));
    gating   u_gat (.clk(clk), .rst_n(rst_n), .en(en), .a(x), .b(k), .c(x),
                    .sel(sel), .q(gated_q), .y(gated_y), .z(gated_z),
                    .w(gated_w), .v(gated_v));
    loopspace u_lsp (.d(x), .din(k), .summed(summed), .carried(carried),
                     .mirrored(mirrored), .dead(dead));
    emptylevel u_emp (.clk(clk), .en(en), .gate(g), .a(x),
                      .q(empty_arm_q));
endmodule
