// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// check-rtl: expect-fail icarus -- "sorry: Reference ports not supported yet"
//
// LRM 23.2.2.4 -- a `ref` port. It is the fourth port direction and the one no
// synthesizable fixture reaches, so without it `term.direction` never takes
// its fourth value and the branches keyed on it are never walked.
//
// It is not `inout` with another name. An `inout` is a net both sides drive;
// a `ref` binds the actual VARIABLE itself, so the child assigns to the
// parent's variable directly. Both arc in both directions -- a ref terminal
// is a driver of the outer net and a load of it -- which is why the arc
// formulas name `ref` beside `inout` on both sides, and why a fixture that
// exercises only `inout` leaves half of each formula unwalked.

module refsink(ref logic [7:0] shared, input logic clk, input logic [7:0] step);
    always_ff @(posedge clk) shared <= shared + step;
endmodule

// A `const ref` ARGUMENT is the same binding read-only: the reference is
// passed to avoid copying an aggregate, and `const` is what stops the body
// assigning through it. It is the one `ref` that drives nothing, so a rule
// keyed on the direction alone gives its actual a driver no call can make.
module refport(input logic clk, input logic [7:0] step,
               output logic [7:0] o, output logic [7:0] scanned);
    logic [7:0] shared;
    logic [7:0] table_ro;
    refsink u_rs (.shared(shared), .clk(clk), .step(step));
    assign o = shared;

    function automatic logic [7:0] scan(const ref logic [7:0] big,
                                        input logic [7:0] sel);
        return big ^ sel;
    endfunction
    always_comb scanned = scan(table_ro, step);
endmodule
