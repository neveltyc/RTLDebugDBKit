// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// check-rtl: expect-fail icarus -- no support for escaped identifiers here
//
// Names and scopes that were derived by splitting strings.
//
//   * an escaped identifier may contain a '.', because slang writes it
//     verbatim as `\name ` with no quoting. Taking the leaf by splitting the
//     hierarchical path on the last dot named `\u.1 `'s tree node `1`, and no
//     path lookup could reach that instance. The leaf comes from the symbol
//     now, with the same escaping rule and the same array-index suffixes
//     slang applies, so `u[0]` still spells `u[0]`.
//
//   * a GATE took neither. Its name came from the symbol's own `name` rather
//     than through the same function, and an instance array's element carries
//     the bare array name there -- so `buf p [1:0]` spelled both elements `p`,
//     one scope with two nodes of one name, and `top.p[1]` was unreachable by
//     the only lookup the tree has. An escaped gate name arrived unescaped
//     with it, so `\g.1 ` wrote a node name holding a dot: two path segments
//     where a node is one.
//
//   * a net initialiser and an alias inside a generate block were filed under
//     the INSTANCE rather than the generate level that declares them, while
//     the net rows for the same declarations were filed correctly -- so the
//     two tables contradicted each other and `g[0]`'s initialiser could not be
//     told from `g[1]`'s.
//
//
// (`alias {a, b} = c;` had the same class of bug and lives in aliascat.sv,
// which Verilator cannot lint.)

module naming_leaf; endmodule

module naming (input logic [1:0] ia, ib, output logic [1:0] o,
               output logic [1:0] pe, output logic [1:0] pv);
    naming_leaf \u.1  ();      // escaped, and it contains a dot
    naming_leaf \u[2] ();      // escaped, no dot
    naming_leaf u [1:0] ();    // an array: leaves stay u[0], u[1]

    // The same three shapes as a gate, which is the same symbol base and
    // must spell its leaf the same way.
    buf \g.1  (pe[0], ia[0]);  // escaped, and it contains a dot
    buf \g[2] (pe[1], ia[1]);  // escaped, no dot
    buf p [1:0] (pv, ia);      // an array: leaves stay p[0], p[1]

    genvar i;
    for (i = 0; i < 2; i = i + 1) begin : g
        wire w = ia[i] & ib[i];   // must be filed under g[i], not the instance
        wire z;
        alias z = w;              // likewise
        assign o[i] = z;
    end
endmodule
