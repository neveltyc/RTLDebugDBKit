// A package variable is a real object now, not a name that leaves the model.
// Before v13 a `pkg::mask` reference resolved to nothing: the reading module
// showed driver_kind='external' with a NULL far net, and two modules reading
// one package variable did not meet on any row. v13 stamps the package as a
// pseudo-occurrence (node_kind='package'), its variables as nets, so the
// references resolve and the readers join at the package net.

package cfg_pkg;
    logic [7:0] mask;
    logic       enable;
    // A task living in the package, called twice from one procedure. Its
    // body is written once and walked once per call site, so the statement
    // rows come in two sets -- and the statement layer is where a consumer
    // meets code it did not write itself. v_stmt carries the call_site_id
    // that tells the sets apart; before v15 the base table knew and the
    // view did not.
    //
    // It touches only package variables. A package subroutine's FORMALS are
    // not stamped (only package variables become nets), so a call passing
    // actuals through them records the actual as a target and no dataflow
    // at all -- a separate gap, and not one to build a fixture on top of.
    task automatic arm();
        enable = |mask;
    endtask
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

// Two calls to one package task, from one procedure. One written statement
// becomes two rows, identical but for the site they were walked for; only
// the call site tells the expansions apart.
module reader_c(input logic [7:0] d, input logic [7:0] e,
                output logic [7:0] q1, output logic [7:0] q2);
    // Imported rather than called as `cfg_pkg::arm()`: Icarus rejects a
    // package-scoped call as a statement. slang resolves either form to the
    // same package subroutine, so the import costs the fixture nothing.
    import cfg_pkg::arm;
    always_comb begin
        q1 = d;
        arm();
        q2 = e;
        arm();
    end
endmodule

module package_top(input logic [7:0] a, input logic [7:0] b,
                   output logic [7:0] qa, output logic [7:0] qb,
                   output logic [7:0] qc1, output logic [7:0] qc2);
    reader_a u_a (.d(a), .q(qa));
    reader_b u_b (.d(b), .q(qb));
    reader_c u_c (.d(a), .e(b), .q1(qc1), .q2(qc2));
endmodule
