// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Statement forms the walker had no handler for, and one it mis-scoped.
//
//   * `return e;` writes the subroutine's implicit result variable, and slang
//     does not synthesise that assignment -- the target is
//     SubroutineSymbol::returnValVar. With no handler the whole body recorded
//     nothing, so `ret_style` and `assign_style` disagreed although they
//     compute the same thing.
//   * `do … while (c)` fell through to visitDefault, which visits the
//     condition -- and there is no handler for a bare value expression, so
//     neither the gating nor even the read was recorded. Its condition signal
//     had zero load rows anywhere. (`case … matches` had the same gap and is
//     covered by patterncase.sv, which no open front end can lint.)
//   * a call in a loop condition was attributed to whatever statement preceded
//     it, because the loop handlers walked their condition with call bindings
//     still enabled while `if`/`case` did not.
//   * `release` recorded no gating, so "what decides when the hijack ends"
//     had no answer.
//   * and a for-loop's INITIALISER was gated by the loop condition, which it
//     does not run under -- it executes once, before the condition is ever
//     evaluated.

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
