// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// LRM 12 procedural statements and LRM 13.4 return -- the forms whose walk is
// not the obvious one, each paired with the spelling it must agree with.
//
//   * `return e;` writes the subroutine's implicit result variable, which
//     slang does not synthesise as an assignment: the target is
//     SubroutineSymbol::returnValVar. Reached any other way the body records
//     nothing, and `ret_style` disagrees with `assign_style` although the two
//     compute the same thing.
//   * `do … while (c)` falls to visitDefault without a handler of its own,
//     and visitDefault visits the condition -- for which there is no bare
//     value-expression handler either, so neither the gating nor even the read
//     survives. (`case … matches` has the same gap and lives in
//     patterncase.sv, which no open front end can lint.)
//   * a call in a loop condition belongs to no statement, so a loop handler
//     that walks its condition with call bindings still enabled attributes it
//     to whatever statement precedes it -- which `if`/`case` do not do.
//   * `release` drives nothing, so it carries no dependency for its gating to
//     ride; the condition that ends the hijack has to be recorded as a
//     reference on the release statement itself.
//   * a for-loop's INITIALISER executes once, before the condition is ever
//     evaluated, so it is not gated by it. The body is.

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
