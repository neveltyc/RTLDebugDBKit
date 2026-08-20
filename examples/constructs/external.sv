// Sources this export can name but not resolve: a package variable has no
// net row (a package is not an occurrence), so before v12 a target fed
// from one reported NO driver at all -- silence indistinguishable from
// genuinely undriven. v12 writes the dependency anyway, with a NULL source
// net and the reference on the source end; v_driver reports it as
// driver_kind='external', the path and location on the hier_ref row.

package ext_pkg;
    logic [7:0] mask;
    logic       mode;
endpackage

module ext_use(input logic [7:0] d, output logic [7:0] q,
               output logic [7:0] g, output logic [3:0] nib);
    // Data source beside a local one: q is driven by d AND by the
    // package variable, and only the second needed the new kind.
    assign q = d & ext_pkg::mask;
    // A windowed read: the spelled range survives onto the external
    // driver row, because the referenced object's bits are real even
    // when unnamed here.
    assign nib = ext_pkg::mask[3:0];
    // Control source: the condition is a package variable no dependency
    // could carry before -- the gated target showed only its local data
    // driver, as though the gate were not there.
    always_comb begin
        if (ext_pkg::mode) g = d;
        else               g = '0;
    end
endmodule

module external_top(input logic [7:0] din, output logic [7:0] qout,
                    output logic [7:0] gout, output logic [3:0] nibout);
    ext_use u_e (.d(din), .q(qout), .g(gout), .nib(nibout));
endmodule
