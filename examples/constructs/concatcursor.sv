// check-rtl: expect-fail icarus -- no assignment patterns in a continuous
// assign, and no streaming concatenation
//
// A concatenation is walked by moving a cursor down from its top bit, one
// operand at a time, and both walkers that do it -- Ref.h's collectSlots for
// the operands themselves, StatementWalker's collectAuxSlots for the reads
// that ride them -- must stop on the same two conditions: an operand of no
// width, which takes no position, and an operand WIDER than what is left of
// the concatenation, where `cursor -= w` would wrap through zero and hand
// back bit ranges near 2^64.
//
// No expression here reaches the second condition, and none is known to:
// slang wraps every operand whose width differs from its context in a
// conversion, so a well-formed concatenation adds up by construction, and a
// malformed one loses its type and arrives with width 0 instead. What this
// file holds is the shapes that come closest -- truncation, widening,
// replication, streaming, packed patterns, unpacked targets, string
// concatenation -- so that the guard that was added to match its twin is
// shown not to have narrowed any of them.

typedef struct packed { logic [3:0] hi; logic [3:0] lo; } cc_pair_t;

module concatcursor(input logic clk, input logic [31:0] d, input logic [7:0] e,
                    output logic [31:0] whole, output logic [7:0] cut,
                    output cc_pair_t packed_o, output logic [63:0] doubled,
                    output logic [15:0] streamed, output logic [3:0] narrow,
                    output logic [95:0] mixed, output logic [7:0] arr_a,
                    output logic [7:0] arr_b, output logic [7:0] rode_o);
    logic [7:0] a, b, c, f;

    // The exact split: four operands whose widths add up to the target, and
    // the case every positional slot depends on.
    assign {a, b, c, f} = d;
    assign whole = {a, b, c, f};

    // A truncating conversion OVER a concatenation: the walk descends into
    // an expression wider than the context it was reached in, which is the
    // one place the cursor legitimately starts above the target's top bit.
    assign cut = 8'({a, b});

    // A widening one, which does not descend -- the operand cannot cover the
    // context, so the reference degrades to the whole range.
    assign narrow = 4'({a, b, c});

    // A packed assignment pattern: the same walk, element by element.
    assign packed_o = '{hi: a[3:0], lo: b[3:0]};

    // Replication of a concatenation, and a replication as an operand.
    assign doubled = {2{{a, b, c, f}}};
    assign mixed = {{32{1'b0}}, {a, b, c, f}, 32'(e)};

    // A streaming concatenation, whose own width is not the sum of the
    // operand widths of the concatenation it sits in.
    assign streamed = {>>{a, b}};

    // Unpacked targets on the left of one concatenation.
    logic [7:0] arr [0:3];
    always_comb {arr[0], arr[1]} = {a, b};
    assign arr_a = arr[0];
    assign arr_b = arr[1];

    // A select whose index is itself read: the aux walk positions the index
    // reads on the window of the element they ride, never positionally.
    logic [1:0] idx;
    logic [7:0] rode;
    assign idx = e[1:0];
    always_ff @(posedge clk) {arr[2], rode} <= {e, f};
    always_ff @(posedge clk) {arr[idx], arr[3]} <= {f, e};
    assign rode_o = rode;
endmodule
