// check-rtl: expect-fail icarus -- $root is not in its grammar
//
// A path anchored at $root is ABSOLUTE: it names one object, and every
// occurrence of the body that spells it means that same object. An upward
// name is the opposite -- it means whatever the surroundings of THIS
// occurrence hold, which is why one analysed body cannot answer for it and
// the export leaves it NULL.
//
// slang's HierarchicalReference::isUpward() answers true for both (it is
// `upwardCount > 0 || path[0] is Root`), so a single test against it dropped
// the absolute ones with the upward ones and no $root reference ever
// resolved. The two spellings of one target below are the point: they name
// the same net and only one of them can be replayed per occurrence.

module rootref_leaf;
    logic [7:0] q;
endmodule

// The case that tells Absolute from Downward. This body sits BELOW the path
// it spells, so the path does split below the one body that was analysed --
// and replaying it downward from each occurrence would answer `a.deep.q` for
// a and `b.deep.q` for b. Both must answer a.deep.q, and the local `deep.q`
// beside it is what makes the difference visible: from b, one row names b's
// own leaf and the other names a's.
module rootref_below(input logic clk, input logic [7:0] d,
                     output logic [7:0] abs_o, output logic [7:0] own_o);
    rootref_leaf deep();
    assign deep.q = d;
    always_ff @(posedge clk) abs_o <= $root.rootref.u_below_a.deep.q;
    always_ff @(posedge clk) own_o <= deep.q;
endmodule

module rootref_reader(input logic clk, output logic [7:0] abs_o,
                      output logic [7:0] up_o, output logic [7:0] shallow_o);
    // Two occurrences of this body, one absolute path. Both must resolve to
    // rootref.u_leaf.q -- the object the source names -- and not to anything
    // reached by descending from the occurrence itself, which is what a
    // downward replay would have done and what makes r1 the case that tells
    // the two apart.
    always_ff @(posedge clk) abs_o <= $root.rootref.u_leaf.q;
    // The same net, spelled upward. It resolves for r0 and r1 alike here,
    // but the body cannot know that: another instantiation of this module
    // would find a different `rootref` above it, or none. Stays NULL.
    always_ff @(posedge clk) up_o <= rootref.u_leaf.q;
    // An absolute path whose target is a net of the TOP instance: one tree
    // segment, so the descent from the root node has to consume exactly it
    // and stop -- the shortest path the replay can be handed.
    always_ff @(posedge clk) shallow_o <= $root.rootref.own;
endmodule

module rootref(input logic clk, input logic [7:0] d,
               output logic [7:0] abs_a, output logic [7:0] up_a,
               output logic [7:0] abs_b, output logic [7:0] up_b,
               output logic [7:0] shallow_a, output logic [7:0] shallow_b,
               output logic [7:0] below_abs_a, output logic [7:0] below_own_a,
               output logic [7:0] below_abs_b, output logic [7:0] below_own_b);
    rootref_leaf u_leaf();
    logic [7:0] own;

    // An absolute WRITE. The target is a net of another instance, so without
    // resolution it was a dependency with no source net at all and u_leaf.q
    // read as undriven.
    always_ff @(posedge clk) $root.rootref.u_leaf.q <= d;

    // What the readers reach for through the shortest absolute path there
    // is: a net of the root instance itself.
    assign own = d;

    rootref_reader r0(.clk(clk), .abs_o(abs_a), .up_o(up_a),
                      .shallow_o(shallow_a));
    rootref_reader r1(.clk(clk), .abs_o(abs_b), .up_o(up_b),
                      .shallow_o(shallow_b));

    rootref_below u_below_a(.clk(clk), .d(d), .abs_o(below_abs_a),
                            .own_o(below_own_a));
    rootref_below u_below_b(.clk(clk), .d(d), .abs_o(below_abs_b),
                            .own_o(below_own_b));
endmodule
