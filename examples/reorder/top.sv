// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// A design whose only point is its filelist: five files, listed in the
// reverse of their alphabetical order, which is the shape that makes the
// `src_file` id order observable.
//
// slang reads a filelist's entries on a thread pool from four entries up, so
// the buffers come back in completion order and interning in that order gave
// `src_file.id` -- and every `file.src_file_id` pointing at it -- a different
// value on every export of an unchanged design. The ids are interned in path
// order instead, which `verify-designdb.py` asserts, and two exports of this
// design agree row for row, which `check-reproducible.py` asserts.
//
// The other examples cannot show either: one and two entries are below
// slang's threshold, so their reads are serial and their buffer order is
// already fixed.
module reorder (input logic a, output logic [3:0] y);
    a_leaf u_a (.a(a), .y(y[0]));
    b_leaf u_b (.a(a), .y(y[1]));
    c_leaf u_c (.a(a), .y(y[2]));
    d_leaf u_d (.a(a), .y(y[3]));
endmodule
