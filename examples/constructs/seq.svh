// Included by constructs.sv, and deliberately so: a row's `file` and `line`
// have to come from the same place, and only a statement that lives in a
// different file from its enclosing procedure can tell whether they do. An
// earlier version took the line from the statement and the file from the
// procedure header, which named lines this file has and constructs.sv does
// not.
//
// The padding keeps the line numbers here well past the end of any procedure
// header in constructs.sv, so a mismatched pair is a line number the other
// file cannot even contain rather than a plausible-looking wrong answer.



task automatic release_reset;
    @(posedge clk);          // a wait, in a file of its own
    rst_n = 1'b1;
    u_cnt.dbg = 8'h42;       // a downward XMR, in a file of its own
endtask
