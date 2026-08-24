// check-rtl: expect-fail icarus -- no assignment patterns in a continuous
// assign, and no streaming concatenation
//
// A concatenation is walked by moving a cursor down from its top bit, one
// operand at a time, and both walkers that do it -- Ref.h's collectSlots for
// the operands themselves, StatementWalker's collectAuxSlots for the reads
// that ride them -- must stop on the same condition: an operand WIDER than
// what is left of the concatenation, where `cursor -= w` would wrap through
// zero and hand back bit ranges near 2^64. An operand of NO width is not a
// stop at all -- it occupies no bits, so the cursor does not move and the
// operands beside it keep the positions they had.
//
// No expression here reaches the stop, and none is known to: slang wraps every
// operand whose width differs from its context in a conversion, so a
// well-formed concatenation adds up by construction, and a malformed one loses
// its type and arrives with width 0 instead. What this file holds is the
// shapes that come closest -- truncation, widening, replication, streaming,
// packed patterns, unpacked targets -- so the guard is shown not to have
// narrowed any of them.

typedef struct packed { logic [3:0] hi; logic [3:0] lo; } cc_pair_t;

module concatcursor(input logic clk, input logic [31:0] d, input logic [7:0] e,
                    output logic [31:0] whole, output logic [7:0] cut,
                    output cc_pair_t packed_o, output logic [63:0] doubled,
                    output logic [15:0] streamed, output logic [3:0] narrow,
                    output logic [95:0] mixed, output logic [7:0] arr_a,
                    output logic [7:0] arr_b, output logic [7:0] rode_o,
                    output logic [15:0] padded, output logic [15:0] keyed_o,
                    output logic [7:0] up0, output logic [7:0] uk1);
    logic [7:0] a, b, c, f;
    localparam int PAD = 0;

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

    // The patterns whose element order is NOT most significant first, which
    // is the order the walk assumes. A pattern against an unpacked
    // aggregate lists element zero first and element zero sits at the LOW
    // offsets; a `'{index: value}` pattern against an array of either kind
    // is ordered by ascending index, which is the same inversion. Walking
    // them anyway does not lose a position, it reports the opposite one and
    // calls it exact -- so they take the whole target at range granularity.
    logic [7:0] up_arr [0:1];
    always_comb up_arr = '{a, b};
    logic [1:0][7:0] keyed;
    assign keyed = '{1: a, 0: b};
    logic [7:0] up_keyed [0:1];
    always_comb up_keyed = '{0: a, 1: b};

    // A packed assignment pattern: the same walk, element by element.
    assign packed_o = '{hi: a[3:0], lo: b[3:0]};

    // Replication of a concatenation, and a replication as an operand.
    assign doubled = {2{{a, b, c, f}}};
    assign mixed = {{32{1'b0}}, {a, b, c, f}, 32'(e)};

    // A zero-count replication, which is legal and which slang keeps among
    // the operands with the void type. `{PAD{...}}` at PAD = 0 is how a
    // parameterised pad degenerates, so this arrives from ordinary RTL
    // rather than from a torture case: `a` and `b` must land on the windows
    // they would have without it, and `clk` -- which the replication names
    // but the concatenation does not read -- must not reach `padded` at all.
    assign padded = {a, {PAD{clk}}, b};

    // A streaming concatenation, whose own width is not the sum of the
    // operand widths of the concatenation it sits in.
    assign streamed = {>>{a, b}};

    // A bit-reversing stream into a SPLIT target: which operand bits land in
    // which half is a permutation this model does not compute, so each
    // pairing keeps its operand window as an upper bound -- exact would
    // claim a[5:2] feeds a half it never reaches. The mirror below streams
    // the TARGET, where the unknown side is which target bits each source
    // reaches.
    logic [3:0] sh, sl;
    assign {sh, sl} = {<<{a[5:0], b[1:0]}};
    logic [7:0] sm0, sm1;
    assign {>>{sm0, sm1}} = {a, b};

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

    // A RANGE select over an unpacked array. slang's own bounds for it come
    // back the width of one element however many the select names, so a
    // range recorded from them says the elements past the first are
    // untouched -- an under-claim wearing exact=1, which is the reading that
    // must never be wrong. Recorded as an unknown part of the array instead.
    assign keyed_o = keyed;
    assign up0 = up_arr[0];
    assign uk1 = up_keyed[1];

    logic [7:0] slice_src [0:3];
    logic [7:0] slice_dst [0:1];
    assign slice_dst = slice_src[1:2];
endmodule
