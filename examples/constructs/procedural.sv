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
module evkinds (input logic en, input logic ev, input logic [7:0] d,
                output logic [7:0] latched, output logic [7:0] both_q);
    always_latch if (en) latched <= d;
    always @(edge ev) both_q <= d;
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
        pass(a, nib);       // narrower, so map_exact must be 0 -- and this
    end                     // must remain its ONLY driver
    assign out_arg = scratch;
    assign narrow_arg = nib;
endmodule

module procedural (input logic clk, input logic [7:0] x, k,
                   input logic [3:0] sel, input logic b, g, en, ev,
                   output logic [7:0] explicit_self, compound_self,
                   output logic [7:0] shift_self, masked,
                   output logic [7:0] assign_style, ret_style,
                   output logic [7:0] matched, looped, sum, output logic held,
                   output logic [7:0] latched, both_q,
                   output logic [7:0] via_macro, direct, out_arg,
                   output logic [3:0] narrow_arg);
    compound u_cmp (.clk(clk), .x(x), .explicit_self(explicit_self),
                    .compound_self(compound_self), .shift_self(shift_self),
                    .masked(masked));
    stmtgaps u_stg (.clk(clk), .x(x), .k(k), .sel(sel), .b(b), .g(g),
                    .assign_style(assign_style), .ret_style(ret_style),
                    .matched(matched), .looped(looped), .sum(sum), .held(held));
    evkinds  u_ev  (.en(en), .ev(ev), .d(x), .latched(latched),
                    .both_q(both_q));
    macroloc u_mac (.a(x), .via_macro(via_macro), .direct(direct),
                    .out_arg(out_arg), .narrow_arg(narrow_arg));
endmodule
