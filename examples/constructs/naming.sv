// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// check-rtl: expect-fail icarus -- no support for escaped identifiers here
//
// LRM 5.6.1 escaped identifiers and LRM 23.3.2 instance arrays -- the names
// that are wrong if a leaf segment is recovered by splitting a string.
//
//   * an escaped identifier may contain a '.', because slang writes it
//     verbatim as `\name ` with no quoting. Splitting the hierarchical path on
//     its last dot names `\u.1 `'s tree node `1`, and no path lookup reaches
//     that instance. The leaf has to come from the symbol, with slang's own
//     escaping rule and array-index suffixes, so `u[0]` still spells `u[0]`.
//
//   * a GATE is the same symbol base and must spell its leaf the same way. Its
//     own `name` carries neither: an instance array's element holds the bare
//     array name there, so `buf p [1:0]` spells both elements `p` -- one scope,
//     two nodes, one name, and `top.p[1]` unreachable by the only lookup the
//     tree has. An escaped gate name arrives unescaped with it, so `\g.1 `
//     writes a node name holding a dot: two path segments where a node is one.
//
//   * a net initialiser and an alias inside a generate block belong to the
//     generate level that declares them, not to the instance. The net rows for
//     the same declarations are filed by scope, so filing these by instance
//     makes the two tables contradict each other and `g[0]`'s initialiser
//     indistinguishable from `g[1]`'s.
//
//   * a generate LABEL and a reference PATH are segments too, and take the
//     same escaping. An unescaped label writes a tree node the net paths
//     beside it spell differently; a reference path that drops the
//     terminating space respells `\u.1 .v` as `u.1.v`, an identifier that
//     names something else.
//
// (`alias {a, b} = c;` is the same class of name recovery and lives in
// aliascat.sv, which Verilator cannot lint.)

module naming_leaf; logic v; endmodule

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

    if (1) begin : \gn.1          // an escaped generate label is a segment too
        wire gw = ia[0] ^ ib[0];
    end

    wire ev = \u.1 .v;            // a reference path keeps the terminator
endmodule
