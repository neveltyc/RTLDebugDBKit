// Reachable only through +incdir+: the harness passes this directory, and
// without it opts_b.sv fails to elaborate rather than quietly using a default.
`define DEFS_HEADER_SEEN 1
