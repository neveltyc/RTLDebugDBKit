// Five files, listed in the reverse of their alphabetical order. The design is
// beside the point -- the filelist is the fixture.
//
// `src_file` ids are interned in PATH order, while slang's source loader hands
// the buffers back in whatever order the reads finished. Interning in buffer
// order gives one unchanged design a different id column on every export, and
// nothing downstream reads an id's value, so the two databases are equivalent
// and still diff.
//
// FIVE, because slang reads a filelist on a thread pool only from four entries
// up (SourceLoader::MinFilesForThreading). Below that it reads serially, the
// buffers come back in filelist order every time, and the nondeterminism
// cannot occur -- the other examples have one and two entries, so neither
// reaches the threaded path.
//
// REVERSED, because if path order and buffer order agreed there would be
// nothing to tell apart.
//
// Each file is a self-contained module, so every one of the five passes
// check-rtl.sh on its own. examples/options is built the same way.
//
// verify-designdb.py asserts the id order within one database;
// check-reproducible.py asserts that two exports agree row for row.
top.sv
d_leaf.sv
c_leaf.sv
b_leaf.sv
a_leaf.sv
