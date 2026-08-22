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
//
// Each file is a self-contained module, like examples/options -- nothing here
// instantiates anything from another file, so every one of the five passes
// check-rtl.sh on its own.
module reorder (input logic a, output logic y);
    assign y = ~a;
endmodule
