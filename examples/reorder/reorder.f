// Five files, listed in the reverse of their alphabetical order.
//
// Five, because slang reads a filelist on a thread pool only from four
// entries up (SourceLoader::MinFilesForThreading) -- below that it reads
// serially, the buffers come back in filelist order every time, and the
// nondeterminism this example exists to catch cannot occur. The other
// examples have one and two entries, so neither reaches the threaded path
// and neither could regress it.
//
// Reversed, because `src_file` ids are interned in PATH order while the
// buffers arrive in whatever order the reads finished. If the two orders
// agreed there would be nothing to tell apart.
top.sv
d_leaf.sv
c_leaf.sv
b_leaf.sv
a_leaf.sv
