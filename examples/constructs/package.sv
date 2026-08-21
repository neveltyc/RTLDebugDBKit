// A package variable is a real object now, not a name that leaves the model.
// Before v13 a `pkg::mask` reference resolved to nothing: the reading module
// showed driver_kind='external' with a NULL far net, and two modules reading
// one package variable did not meet on any row. v13 stamps the package as a
// pseudo-occurrence (node_kind='package'), its variables as nets, so the
// references resolve and the readers join at the package net.

package cfg_pkg;
    logic [7:0] mask;
    logic       enable;
endpackage

module reader_a(input logic [7:0] d, output logic [7:0] q);
    // Resolves to cfg_pkg.mask: driver_kind is data (through the package net),
    // not external.
    assign q = d & cfg_pkg::mask;
endmodule

module reader_b(input logic [7:0] d, output logic [7:0] q);
    import cfg_pkg::*;
    // A bare `mask`, imported: slang still knows its package, so it resolves
    // to the same net reader_a used.
    assign q = enable ? (d | mask) : d;
endmodule

module package_top(input logic [7:0] a, input logic [7:0] b,
                   output logic [7:0] qa, output logic [7:0] qb);
    reader_a u_a (.d(a), .q(qa));
    reader_b u_b (.d(b), .q(qb));
endmodule
