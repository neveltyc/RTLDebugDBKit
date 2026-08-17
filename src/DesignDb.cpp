// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)

#include "DesignDb.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <sqlite3.h>

namespace designdb {

namespace {

// The schema is *folded*: rows hang off `module`, not off `instance`. Thirty-two
// copies of the same core therefore share one set of edges, and the database
// tracks the amount of unique RTL rather than the elaborated instance count.
// Column naming follows three rules, so a name can be guessed rather than
// looked up:
//
//   * A bit range is prefixed with the column it describes. `edge` names its
//     ends `src` and `dst`, so their ranges are `src_lo`/`src_hi` and
//     `dst_lo`/`dst_hi`, and `assign_operand` uses `src_*` for the same idea.
//     An earlier spelling invented `sb_`/`db_`, which matched nothing else in
//     the row and read as "database" in a database.
//   * A foreign key is named for the table it points at; a text name ends in
//     `_name`. `child.def_module` is a module id, `child.def_name` is the text.
//   * No column takes a table's name. `proc_event` records an edge kind, not
//     an `edge`, and the two would have collided in every join that used both.
//
// The REFERENCES clauses document the joins; they are not enforced at runtime,
// since `PRAGMA foreign_keys` is left at its default of off. A consumer should
// read them as the shape of the schema rather than as a guarantee already
// checked -- the export is written once and read many times, and paying for a
// lookup per inserted row to re-verify ids this process just issued is the
// wrong side of that trade.
constexpr const char* kSchema = R"SQL(
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);

CREATE TABLE source_file(
    id      INTEGER PRIMARY KEY,
    path    TEXT UNIQUE,
    digest  TEXT);

-- A module together with the parameter values it elaborated with. The
-- parameters are part of the identity: a different parameterisation resolves
-- different generate branches and different widths, so it is a different
-- netlist and must not share rows.
CREATE TABLE module(
    id      INTEGER PRIMARY KEY,
    name    TEXT NOT NULL,
    params  TEXT NOT NULL,
    UNIQUE(name, params));

-- Repeated strings, interned. Types especially: a packed struct or enum prints
-- as its entire member list, which is far larger than the row referencing it.
CREATE TABLE type(id INTEGER PRIMARY KEY, text TEXT UNIQUE);
-- Identifiers: signals, ports, instance names. Short, and repeated far more
-- than they are distinct.
CREATE TABLE name(id INTEGER PRIMARY KEY, text TEXT UNIQUE);
-- `path` is the spelling the rows carry -- as written in the filelist or on
-- the command line -- while `source_file.path` is absolute. The two genuinely
-- differ, so `source_file` joins them: without it a consumer checking digests
-- had to match spellings by basename, which breaks on the first design with
-- two files of one name.
CREATE TABLE file(
    id          INTEGER PRIMARY KEY,
    path        TEXT UNIQUE,
    source_file INTEGER REFERENCES source_file(id));

-- Intra-module dataflow, in the module's own namespace.
--
-- `edge` and `assignment` are two independent projections of the same
-- statements, and there is deliberately no key that recovers one from the
-- other. **(module, dst, file, line) is not a join key.** An earlier version of
-- this comment said it was, which was wrong in a way that returns rows rather
-- than failing:
--
--     always_ff @(posedge clk) begin if (c) q <= a; else q <= b; end
--
-- is two assignments and three edges (a->q, b->q, and c->q with control=1), and
-- every one of those five rows carries the same module, dst, file and line.
-- Joining on them pairs each assignment with all three edges, so a consumer
-- asking what the first statement reads is told `a`, `b` and `c`. The line is
-- shared by construction -- a statement is written where it is written -- so
-- this is the ordinary case, not a corner one.
--
-- Nor can the relation be tightened into a foreign key, in either direction:
--
--   * One edge, many assignments. Two statements writing the same target from
--     the same source on the same line collapse to one row -- deliberately, so
--     a connectivity query answers "does a reach q" once rather than once per
--     statement.
--   * One edge, no assignment. A gate contributes edges and no statement at
--     all, and a subroutine call binds its actuals to its formals -- the
--     `d -> bump.v` half of `always_ff @(posedge clk) bump(d);` is an edge
--     with no assignment row behind it, because no assignment was written.
--
-- So each projection answers its own questions and neither substitutes for the
-- other:
--
--   * What does *this statement* read?  `assign_operand`, keyed on
--     `assignment.id`. It is exact: for the block above it gives `a` to the
--     first assignment and `b` to the second, which is the answer the join
--     above gets wrong.
--   * Does a reach q at all, through anything?  This table.
--
-- What no table answers is which branch condition gates which statement: the
-- `c -> q` edge records that `c` reaches `q` as a condition, not which of the
-- two assignments it guards. That is the deliberate omission `assignment`
-- documents below, not an accident of this one.
CREATE TABLE edge(
    module   INTEGER NOT NULL REFERENCES module(id),
    -- Null when the right-hand side reads nothing at all: `q <= 8'h0`.
    -- The row still names the statement, which is what a driver query reports.
    src      INTEGER REFERENCES name(id),
    dst      INTEGER NOT NULL REFERENCES name(id),
    src_type INTEGER REFERENCES type(id),
    dst_type INTEGER REFERENCES type(id),
    -- continuous_assign | procedural | primitive | procedure. `primitive` is
    -- a gate, switch or UDP instance, whose `construct` names it (`gate:and`,
    -- `udp:my_latch`); it has no procedure and so no `proc_event` row.
    kind      TEXT,
    construct TEXT,
    file      INTEGER REFERENCES file(id),
    line     INTEGER,
    -- 1 when the operand reached the target through a branch condition rather
    -- than through the right-hand side. Its own column rather than a suffix on
    -- `construct`, which made a consumer string-match to recover it.
    --
    -- There is deliberately no `clocked` column. Whether the enclosing
    -- procedure is edge-triggered is a property of that procedure and lives in
    -- `proc_event`; stamping it onto every target claimed each one is held by a
    -- flop, which is false for a block-local temporary written with `=`.
    control  INTEGER,
    -- Bit ranges, NULL when the reference covers the whole object. Read them
    -- with `src_exact`/`dst_exact`: NULL with exact=1 is the whole object, NULL
    -- with exact=0 is somewhere inside it and we cannot say where.
    --
    -- These are LSB-relative offsets into the flattened object, NOT the
    -- declared indices. `logic [15:8] off` has bit 15 as offset 7, and
    -- `logic [0:7] up` has bit 0 as offset 7. A consumer that maps them
    -- straight onto declared indices mislabels every signal not declared
    -- [N-1:0]; the declared range is recoverable from `dst_type`/`src_type`.
    -- `*_exact` is 0 when a dynamic selector meant the range could not be
    -- narrowed, so the range is an upper bound rather than the bits actually
    -- touched. NULL bits with exact=1 is the whole object; NULL bits with
    -- exact=0 is somewhere inside it, unknown where. Storing both as NULL made
    -- the second read as the first.
    src_lo    INTEGER, src_hi INTEGER, src_exact INTEGER,
    dst_lo    INTEGER, dst_hi INTEGER, dst_exact INTEGER,
    -- 1 when the source's bits map one-to-one onto the target's, so a bit-level
    -- trace can follow this edge bit by bit. 0 when the source only influences
    -- the target's range as a whole.
    --
    -- `src_exact` and `dst_exact` each describe *one side*: whether that end's
    -- range could be narrowed. Neither says anything about the relationship
    -- between the ends, and both can be 1 while the correspondence is coarse --
    -- `q = a + b` knows exactly which bits of `a` and of `q` are involved and
    -- still cannot say which bit of `a` reaches which bit of `q`, because a
    -- carry crosses them.
    --
    -- 0 does not mean the dependency is doubtful; the source does reach the
    -- target. It means the pairing is at range granularity. That distinction
    -- exists because it used to be unavailable: `{a, b} = {x, y}` was exported
    -- as four edges, all four marked exact on both sides, and the two of them
    -- that are not dataflow at all -- `y -> a`, `x -> b` -- were
    -- indistinguishable from the two that are.
    map_exact INTEGER);

-- What a module instantiates. Expanding `instance` against this is what turns
-- the folded model back into hierarchy, and `instance.child` is the link that
-- makes the expansion an id lookup rather than a name-parsing exercise.
CREATE TABLE child(
    id         INTEGER PRIMARY KEY,
    module     INTEGER NOT NULL REFERENCES module(id),
    name       INTEGER NOT NULL REFERENCES name(id),
    def_name   INTEGER REFERENCES name(id),      -- what it instantiates, as text
    def_module INTEGER REFERENCES module(id),    -- and as a module row
    -- What sort of thing this is, because `def_module IS NULL` does not say.
    -- Three states shared that one spelling and a consumer could not tell them
    -- apart, though a trace stopping at each means something different:
    --
    --   module      an ordinary instance; `def_module` names its rows.
    --   primitive   a gate, switch or UDP. It has no module row and never will;
    --               its dataflow is already in the parent's `primitive` edges,
    --               so a trace does not stop here, it continues in the parent.
    --   unresolved  a definition slang could not find. There is no dataflow
    --               anywhere for it -- this is a black box, and a trace really
    --               does stop. Counted in `meta.unresolved_count`; this is
    --               where the individual rows are.
    kind       TEXT);

-- Every declaration in a module. The rest of the schema is edge-derived, so a
-- signal appears there only if it takes part in dataflow; this is what makes
-- "what is in this scope", "which outputs are never driven" and "which signals
-- are dead" answerable at all.
--
-- One row per actual signal. slang carries a Port symbol *and* the underlying
-- net or variable for every port, so the port contributes its direction to the
-- signal's row rather than a row of its own.
CREATE TABLE symbol(
    module    INTEGER NOT NULL REFERENCES module(id),
    name      INTEGER NOT NULL REFERENCES name(id),
    -- variable | net | port | parameter | interface_port. An interface port
    -- has no net behind it, so it gets a row of its own; `type` holds the
    -- interface name, with `.modport` appended when the port declares one.
    kind      TEXT,
    type      INTEGER REFERENCES type(id),
    width     INTEGER,                    -- bits, NULL when not integral
    direction INTEGER,                    -- 0=in 1=out 2=inout 3=ref, NULL if not a port
    file      INTEGER REFERENCES file(id),
    line      INTEGER,
    col       INTEGER);

-- Port connections, recorded against the PARENT module that writes them.
-- `outer` is a net in the parent's namespace and `port` a formal inside the
-- child, so following this table in either direction is how a trace crosses a
-- hierarchy boundary.
CREATE TABLE port(
    module       INTEGER NOT NULL REFERENCES module(id),
    -- The child instance's name as the folded model spells it, generate
    -- prefix included. `_name` by the def_name precedent: an interned name id,
    -- disambiguated from `child_id` below, which is the actual child-table
    -- key. (An earlier spelling took the bare word `child`, breaking the
    -- no-column-takes-a-table's-name rule and forcing the real foreign key
    -- into the schema's only `_id` suffix.)
    child_name   INTEGER NOT NULL REFERENCES name(id),
    def_module   INTEGER REFERENCES module(id),
    port         INTEGER NOT NULL REFERENCES name(id),
    -- The signal inside the child that `port` stands for, when the connection
    -- names the formal something else: `module m(.ext_one(inner))` is
    -- `ext_one` here and `inner` in every row the child's own module owns, so
    -- without this a consumer holding the internal name had no way back to the
    -- binding. NULL in the ordinary case where the two are one name.
    --
    -- Paired with `outer` deliberately: `outer` is the net on the parent's
    -- side of the boundary, `inner` the signal on the child's, and `port` the
    -- name the connection is written with.
    inner        INTEGER REFERENCES name(id),
    -- 0=in 1=out 2=inout 3=ref. Three values do not deserve 1.1 MB of text.
    direction    INTEGER,
    outer        INTEGER REFERENCES name(id),
    outer_type   INTEGER REFERENCES type(id),
    -- Width of the connection *expression*, which a concatenation or a slice
    -- makes different from the width of any one net, so it is not derivable
    -- from `outer`. Comparing it against the formal's width in `symbol` is how
    -- a width-mismatched connection is found.
    outer_width  INTEGER,
    -- The bits of `outer` the connection selects: `.idx(stim[3:0])` attaches
    -- bits 0..3 of stim, and without these columns it attached all of stim --
    -- a trace crossing the boundary then fanned out to everything else the
    -- bus feeds. Encoded exactly as `edge` encodes its ranges: LSB-relative
    -- offsets into the flattened object; NULL with exact=1 is the whole net,
    -- NULL with exact=0 is somewhere inside it (a dynamic select, or an
    -- element of an instance array, which shares the whole array's connection
    -- expression the same way outer_width does).
    outer_lo     INTEGER, outer_hi INTEGER, outer_exact INTEGER,
    -- 0=a net, 1=tied to a constant, 2=left unconnected, 3=an operand of an
    -- expression, 4=an interface binding. 3 is not a net: `.en(state == RUN)`
    -- samples `state` but does not alias it to `en`, and recording it as 0
    -- made every reader of `en` count as a reader of `state`. An unconnected
    -- port is recorded rather than omitted: absence would otherwise mean both
    -- "nobody connected it" and "the exporter did not get that far".
    --
    -- For 4, `port` is the child's interface port, `outer` the interface
    -- instance (or the parent's own interface port, passed through) in the
    -- parent's namespace, `outer_type` the interface definition, and
    -- `direction` is NULL -- an interface port has none. This row is the alias
    -- that makes `child.bus.*` resolvable: the signals live in the interface
    -- instance named by `outer`, and without the row they can be reached from
    -- neither side.
    --
    -- 5 = attached to a signal with no name in this module -- `.a(tb.glob)`.
    -- `outer` is NULL because there is nothing here to name; what it is tied
    -- to is in `hier_ref` at the same file and line. The row is written all
    -- the same, so "connected to something I cannot name here" stays distinct
    -- from "nobody connected it", which is the whole point of recording an
    -- unconnected port in the first place.
    conn_kind    INTEGER,
    -- The modport restricting an interface binding, when one does. NULL
    -- otherwise, and for every non-interface row.
    modport      INTEGER REFERENCES name(id),
    file         INTEGER REFERENCES file(id),
    line         INTEGER,
    -- The `child` row this binding belongs to -- the stable key that relates a
    -- port row to the hierarchy, and through instance.child to the expanded
    -- tree. It exists because the *name* here is not that key: `child` above
    -- is the instance name as the folded model spells it, generate prefix
    -- included (`g_rep[0].u_dec`), while the instance tree spells the same
    -- level one segment at a time (`g_rep[0]`, then `u_dec`) -- so a consumer
    -- holding a tree node had to rebuild the folded spelling before it could
    -- find the node's port rows. Nor is (module, name) something to join on:
    -- two unnamed gates in one module legally share a name, and a join on it
    -- fans out. The id is assigned by the exporter, so it is collision-proof
    -- where the name is not.
    child_id     INTEGER NOT NULL REFERENCES child(id),
    -- The bits of the FORMAL this row's element occupies: `.q({hi, lo})` is
    -- two rows, hi at port_lo=4..port_hi=7 of q and lo at 0..3. Without these
    -- the per-bit precision `edge` carries ended at every instance boundary --
    -- both nets read as attached to all of q. Encoded as ranges are
    -- everywhere: NULL + port_exact=1 the whole formal, NULL + port_exact=0
    -- position unstatable (a width-changing conversion, an instance-array
    -- element), and port_exact NULL where the row has no formal bit domain.
    port_lo      INTEGER, port_hi INTEGER, port_exact INTEGER,
    -- edge.map_exact at the boundary: 1 when this row's outer bits map
    -- one-to-one onto its formal window, 0 when the tie is real but only
    -- range-granular (an expression operand, a degraded window), NULL when
    -- there is no outer end to correspond with (a constant, an unconnected
    -- port, an external tie, an interface binding). 0 is not doubt.
    map_exact    INTEGER);

-- Assignments, one row per statement that writes a target.
--
-- `edge` flattens a procedure into "these signals drive that one"; this keeps
-- the statements apart, so a target with four assignments reads as four
-- statements with their own lines, operands and bit ranges rather than as one
-- merged set.
--
-- It deliberately stops there. An earlier draft also stored the branch
-- conditions reaching each assignment, on the theory that a consumer could
-- evaluate them against a waveform and name the assignment in effect at a given
-- time. That does not hold up: the conditions would have to be evaluated as
-- SystemVerilog expressions, which a waveform tool cannot do; the sampling
-- instant is an edge of a clock this schema deliberately does not identify;
-- guard operands are often not in the dump at all; X during reset makes a chain
-- neither true nor false; and for blocking assignments "which one was in
-- effect" has no single answer because all of them ran. Encoding that judgement
-- as data would have produced confident wrong answers about which line of RTL
-- fired. The line numbers are here; the reasoning belongs to the reader.
CREATE TABLE assignment(
    id       INTEGER PRIMARY KEY,
    module   INTEGER NOT NULL REFERENCES module(id),
    dst       INTEGER NOT NULL REFERENCES name(id),
    dst_lo    INTEGER, dst_hi INTEGER, dst_exact INTEGER,
    -- continuous_assign | procedural. Never `primitive`: a gate is not a
    -- statement, so it contributes edges and no assignment row.
    kind      TEXT,
    construct TEXT,
    file     INTEGER REFERENCES file(id),
    line     INTEGER,
    -- Which procedure in the module, and the order within it. Both are needed:
    -- `seq` restarts per procedure, so two assignments to one target from
    -- different `always` blocks can carry the same seq and would otherwise read
    -- as one ordered sequence.
    --
    -- `proc` is NULL when the statement is in no procedure: a net declared with
    -- an initialiser (`wire w = a & b;`) is a continuous assignment written at
    -- the declaration, so it has a `seq` among its peers and no procedure to
    -- belong to.
    proc     INTEGER,
    seq      INTEGER,
    -- 1 for `=`, 0 for `<=`, NULL for a continuous assign. Not decoration: two
    -- assignments to one target in one block resolve by different rules --
    -- non-blocking samples at entry and the last one wins, blocking runs in
    -- order and each intermediate value is visible to the statements after it.
    -- Without this a consumer holding `seq` cannot tell which rule applies.
    blocking  INTEGER,
    -- Operands not recorded: compile-time constants, and references outside
    -- this module. A row with one operand and three dropped is not the same as
    -- a row that reads one signal, and without this they look identical.
    dropped_operands INTEGER,
    -- Which statement in the module wrote this, as an ordinal the exporter
    -- assigns. See `hier_ref.stmt` for what it is for -- it is the same number,
    -- and it is what relates a row here to the outward writes and reads of the
    -- same statement.
    stmt     INTEGER);

-- The edge events a procedure triggers on. Every one of them, because an event
-- list has no order: `@(posedge clk or negedge rst_n)` and
-- `@(negedge rst_n or posedge clk)` are the same block written two ways, and
-- both spellings are common. Recording "the first" would have been recording
-- how the author happened to arrange the list.
--
-- Which of these is a clock and which a reset is not decided here. That needs
-- constraints this tool does not read, and a divided or gated clock cannot be
-- related to its source without them. A consumer that knows gets both events
-- and can say; one that does not is not handed a guess.
--
-- Keyed on the procedure rather than the assignment: every assignment in a
-- block shares its sensitivity list. Level-sensitive lists are not recorded --
-- for `always @(*)` or `always @(a or b)` the sensitivity is the read set,
-- which `assign_operand` already carries.
--
-- Statement-level event controls are rows too: `@(posedge clk);` inside an
-- initial block or a task is a wait rather than sensitivity, but the signal
-- is sampled either way, and without a row the read had no trace at all.
--
-- `wait` is what tells the two apart, and it has to be a column. An earlier
-- version said file/line would do it -- a sensitivity row carrying the
-- procedure's location, a wait its statement's -- which is not something a
-- consumer can evaluate: no table stores a procedure's own location, so there
-- is nothing to compare against, and a wait written on the same line as the
-- header produces two rows that are byte-identical. Without the column an
-- `initial` block containing `@(posedge clk);` reads as a procedure triggered
-- by `clk`, which is a wrong trigger set rather than a missing one.
CREATE TABLE proc_event(
    module INTEGER NOT NULL REFERENCES module(id),
    proc   INTEGER NOT NULL,
    signal INTEGER REFERENCES name(id),   -- NULL when not a plain reference
    edge_kind TEXT,                       -- posedge | negedge | both
    -- 0 = the procedure's sensitivity list, so it triggers the block.
    -- 1 = a wait reached during execution; it suspends the block instead, and
    -- says nothing about what triggers it. An `initial` block has only these.
    wait   INTEGER NOT NULL,
    file   INTEGER REFERENCES file(id),
    line   INTEGER);

CREATE TABLE assign_operand(
    assignment INTEGER NOT NULL REFERENCES assignment(id),
    name       INTEGER NOT NULL REFERENCES name(id),
    src_lo     INTEGER, src_hi INTEGER, src_exact INTEGER);

-- References that leave the module, exactly as written: `bus.vld` through an
-- interface port, `tb.u_dut.state` as an XMR, `pkg::cfg` from a package.
--
-- Their target cannot go into edge or port rows: those name signals in the
-- module's own namespace, shared by every instance of it, and an absolute
-- path would bake one instance's hierarchy into all of them. The *text* has
-- no such problem -- every instance carries the same spelling -- so the text
-- is what is stored, and resolving it against the hierarchy belongs to the
-- consumer. Bare names that leave the module (a subroutine's package-level
-- free variable) are counted but not recorded: a name with no path in it
-- resolves against imports this table cannot see.
--
-- `write`=1 when the module writes the path. `kind`/`construct` are edge's
-- vocabulary, plus kind='port' with the direction in `construct` for a port
-- connection tied to an external signal. Bits as everywhere else.
CREATE TABLE hier_ref(
    module INTEGER NOT NULL REFERENCES module(id),
    path   INTEGER NOT NULL REFERENCES name(id),
    write  INTEGER,
    kind      TEXT,
    construct TEXT,
    file   INTEGER REFERENCES file(id),
    line   INTEGER,
    path_lo INTEGER, path_hi INTEGER, path_exact INTEGER,
    -- Which statement produced this, as an ordinal the exporter assigns while
    -- walking the module. Rows sharing (module, stmt) came from one statement;
    -- the number means nothing outside its module and is not an id in a table.
    --
    -- It exists because a statement whose target is outward has no `assignment`
    -- row to hang its parts from: the write lands here, the reads it made land
    -- in `stmt_read`, and any further outward operand lands here too. Before
    -- this, those parts shared only module, file and line, so
    --
    --     assign top.a = x; assign top.b = y;
    --
    -- exported two writes and two reads that a consumer could only pair four
    -- ways -- `top.a` from `x` *and* from `y`, `top.b` likewise -- when the RTL
    -- says exactly one of those four. `edge` has the same shape of problem and
    -- deliberately does not solve it, because a deduplicated dependency graph
    -- has no statement to point at; these tables are not deduplicated and do.
    stmt   INTEGER);

-- A statement that reads signals without writing anything this module can
-- name. Two shapes reach it: an assertion, which writes nothing at all, and an
-- assignment whose target lies outside the module (`assign bus.vld = |payload;`).
--
-- Their reads had nowhere else to go. `edge` requires a `dst`, and
-- `assign_operand` hangs off an `assignment` row that cannot exist without a
-- module-relative target -- so `payload` and every signal an assertion checks
-- read as though nothing in the design used them, which for assertion-heavy
-- RTL is most of what the file says.
--
-- The write side of the second shape is in `hier_ref` at the same file and
-- line; this is the read side of the same statement.
CREATE TABLE stmt_read(
    module    INTEGER NOT NULL REFERENCES module(id),
    name      INTEGER NOT NULL REFERENCES name(id),
    -- edge.kind's vocabulary. (Not 'assertion': slang wraps even a
    -- module-scope concurrent assertion in an implicit procedure, so the
    -- reading construct's own word lives in `construct` below and the kind
    -- stays the wrapper's.)
    kind      TEXT,
    -- The construct that reads: `assign`, `always_ff`, or the assertion's own
    -- word -- `assert`, `assume`, `cover`, `expect`.
    construct TEXT,
    file      INTEGER REFERENCES file(id),
    line      INTEGER,
    -- Bits of the thing read, spelled `src_*` as everywhere else that records
    -- a read.
    src_lo    INTEGER, src_hi INTEGER, src_exact INTEGER,
    -- The statement these reads belong to, as `hier_ref.stmt`. Joining on it is
    -- what pairs a read with the outward write it fed, rather than with every
    -- outward write on the line.
    stmt      INTEGER);

-- The instance tree. This is the one table that scales with the design rather
-- than with the source, which is why it carries nothing but identity.
CREATE TABLE instance(
    id      INTEGER PRIMARY KEY,
    -- One path segment, never more. A generate block is a level of its own,
    -- so `g_lane[3].u_dp` is two rows rather than one name containing a dot:
    -- resolving a path is then one indexed lookup per segment, which is the
    -- whole reason the paths themselves are not stored.
    name    INTEGER NOT NULL REFERENCES name(id),
    -- NULL for a generate block, which is a naming level rather than an
    -- instantiation and has no module to point at. Also NULL for a primitive
    -- and for an unresolved instantiation, neither of which has a module row --
    -- `child.kind`, reached through `child` below, is what separates those
    -- three; `module IS NULL` alone does not.
    module  INTEGER REFERENCES module(id),
    parent  INTEGER REFERENCES instance(id),
    -- The instantiation in the parent's module body that this row expands.
    --
    -- The two hierarchy tables were otherwise related by nothing: `child` is
    -- folded and spells a generate-nested name whole (`g[0].u_leaf`), while
    -- `instance` is expanded and one segment per row (`g[0]` then `u_leaf`), so
    -- matching them meant re-parsing the text and walking segments -- work the
    -- database could do once and did not. One `child` row expands to as many
    -- `instance` rows as there are instances of the enclosing module, which is
    -- the ordinary one-to-many this column now states.
    --
    -- NULL only where there is genuinely no instantiation to point at: the root,
    -- and a generate block, which is a level the elaboration invented rather
    -- than something written as an instance.
    child   INTEGER REFERENCES child(id));
)SQL";

constexpr const char* kIndexes = R"SQL(
CREATE INDEX edge_by_dst      ON edge(module, dst);
CREATE INDEX edge_by_src      ON edge(module, src);
CREATE INDEX child_by_module  ON child(module);
CREATE INDEX symbol_by_module ON symbol(module);
CREATE INDEX assign_by_dst    ON assignment(module, dst);
CREATE INDEX pevent_by_proc   ON proc_event(module, proc);
-- Two directions, two indexes: pevent_by_proc answers "what is this procedure
-- sensitive to", this one answers "whose trigger is this signal" -- the event
-- arm of v_load's documented point query, which otherwise scanned the
-- module's whole event list through the other index's prefix.
CREATE INDEX pevent_by_signal ON proc_event(module, signal);
CREATE INDEX aop_by_assign    ON assign_operand(assignment);
CREATE INDEX sread_by_name    ON stmt_read(module, name);
CREATE INDEX href_by_module   ON hier_ref(module);
CREATE INDEX symbol_by_name   ON symbol(name);
-- Both directions: outward from a net in the parent, and inward from a formal
-- in the child. A driver query needs the first, a load query the second.
CREATE INDEX port_by_outer    ON port(module, outer);
CREATE INDEX port_by_formal   ON port(def_module, port);
-- The boundary-crossing hop of a trace: a tree node's child_id to its port
-- rows. Not a view index (SQLite has none) -- it is the access path of the
-- port.child_id foreign key, which the documented v_tree_node-to-
-- v_port_connection join takes on every instance boundary.
CREATE INDEX port_by_child    ON port(child_id);
-- The descent index: resolving a hierarchical path means one lookup per
-- segment against this. Not unique -- a design that only partially elaborates
-- can produce two siblings with the same name, and refusing the second would
-- abort an export that is otherwise perfectly usable. The count is reported.
CREATE INDEX instance_by_parent ON instance(parent, name);
CREATE INDEX instance_by_module ON instance(module);
)SQL";

// The stable query interface: nine views that resolve the intern tables so a
// consumer never joins `name`, `type`, `file` or `source_file` itself. They are
// part of the schema-version contract from v8 on -- their existence, their
// column sets and their row granularity are what a consumer may rely on, and
// verify-designdb.py asserts all three.
//
// Ground rules, so the views stay what they claim to be:
//
//   * One view row is one base-table row. Every join here is against a primary
//     key, so no LEFT JOIN can fan out, and the verifier checks
//     count(view) == count(base) to keep it that way.
//   * Explicit column lists, never SELECT * -- a column added to a base table
//     must not silently change a view's contract.
//   * No transitive closure. v_driver and v_load are one step inside one
//     module; walking a fan-in cone or crossing instances is the consumer's
//     loop, composed from v_dependency + v_port_connection + v_tree_node --
//     joined on child_id, the one key both hierarchy views expose.
//   * Few nouns, complete attributes. The views are the object model --
//     database, tree node, signal, binding, dependency, statement -- not one
//     view per storage table. v_load is the whole answer to "who reads it"
//     however the read was stored, the way a netlist database's net.loads
//     includes the flop clock pins; row counts for union views hold as the
//     sum of their parts.
//   * Nothing joins `edge` to `assignment`. The two are independent
//     projections, and (module, dst, file, line) is not a key between them --
//     the views must not resurrect the cross product v7 removed.
//   * Plain CREATE VIEW, not IF NOT EXISTS: the writer only ever creates a
//     fresh database, so a name collision is a bug to fail on, not to accept.
//
// SQLite resolves a view's column references when the view is *queried*, not
// when it is created -- a view naming a dropped column is created without
// complaint and fails on first use. The verifier's row-count checks query
// every view, which is what makes a stale view a caught error rather than a
// consumer's surprise.
constexpr const char* kViews = R"SQL(
-- The meta table pivoted to one fixed row, so "which schema, which status" is
-- one SELECT with no key-value handling. Counts are CAST so a consumer gets
-- integers, not the TEXT the key-value table stores. The view only reshapes;
-- required-key enforcement stays in the verifier.
CREATE VIEW v_database_info AS
SELECT
    CAST(MAX(CASE WHEN key = 'schema_version'
                  THEN value END) AS INTEGER) AS schema_version,
    MAX(CASE WHEN key = 'tool_version'          THEN value END) AS tool_version,
    MAX(CASE WHEN key = 'slang_version'         THEN value END) AS slang_version,
    MAX(CASE WHEN key = 'producer_revision'     THEN value END) AS producer_revision,
    MAX(CASE WHEN key = 'top'                   THEN value END) AS top,
    MAX(CASE WHEN key = 'analysis_status'       THEN value END) AS analysis_status,
    CAST(MAX(CASE WHEN key = 'error_count'
                  THEN value END) AS INTEGER) AS error_count,
    CAST(MAX(CASE WHEN key = 'unresolved_count'
                  THEN value END) AS INTEGER) AS unresolved_count,
    CAST(MAX(CASE WHEN key = 'empty_procedure_count'
                  THEN value END) AS INTEGER) AS empty_procedure_count,
    CAST(MAX(CASE WHEN key = 'duplicate_path_count'
                  THEN value END) AS INTEGER) AS duplicate_path_count,
    MAX(CASE WHEN key = 'config_digest'         THEN value END) AS config_digest
FROM meta;

-- One row per `instance`: the expanded tree, with names resolved and the node's
-- nature spelled out. node_kind is the classification a consumer had to derive
-- from NULL combinations before: root and generate are structural (nothing was
-- written as an instance), the other three are child.kind -- and a trace stops
-- differently at each, which is why the word matters. An inconsistent row
-- yields NULL rather than 'unknown': naming it would hide exactly the state
-- the verifier exists to reject.
--
-- No source location -- instance and child do not carry one, and a view must
-- not invent columns it cannot fill. No precomputed path either: the chain of
-- instance_name up parent_instance_id IS the path, one segment per row.
CREATE VIEW v_tree_node AS
SELECT
    i.id         AS instance_id,
    i.parent     AS parent_instance_id,
    n.text       AS instance_name,
    CASE WHEN i.parent IS NULL THEN 'root'
         WHEN i.child IS NULL AND i.module IS NULL THEN 'generate'
         ELSE c.kind END AS node_kind,
    i.module     AS module_id,
    m.name       AS module_name,
    m.params     AS module_params,
    i.child      AS child_id,
    dn.text      AS definition_name
FROM instance i
JOIN name n        ON n.id = i.name
LEFT JOIN module m ON m.id = i.module
LEFT JOIN child c  ON c.id = i.child
LEFT JOIN name dn  ON dn.id = c.def_name;

-- One row per `symbol`: a declaration in a module variant, with its type text
-- and both spellings of its file -- file_path as written in the filelist,
-- source_path absolute on disk. They genuinely differ and each is the right
-- answer to a different question, so neither substitutes for the other.
CREATE VIEW v_signal AS
SELECT
    s.module     AS module_id,
    m.name       AS module_name,
    m.params     AS module_params,
    n.text       AS signal_name,
    s.kind       AS symbol_kind,
    t.text       AS type_text,
    s.width      AS width,
    CASE s.direction WHEN 0 THEN 'input' WHEN 1 THEN 'output'
                     WHEN 2 THEN 'inout' WHEN 3 THEN 'ref' END AS direction_name,
    f.path       AS file_path,
    sf.path      AS source_path,
    s.line       AS source_line,
    s.col        AS source_column
FROM symbol s
JOIN module m            ON m.id = s.module
JOIN name n              ON n.id = s.name
LEFT JOIN type t         ON t.id = s.type
LEFT JOIN file f         ON f.id = s.file
LEFT JOIN source_file sf ON sf.id = f.source_file;

-- One row per `port`: a binding in the FOLDED model, shared by every instance
-- of the parent -- which is why there is no instance_id here to ask for.
-- child_id is the join key to the hierarchy: v_tree_node.child_id equals it,
-- and that pair is the ONLY documented way to relate the two views. The names
-- do not relate them -- child_instance_name is the folded spelling with the
-- generate prefix (`g_rep[0].u_dec`) while v_tree_node.instance_name is one
-- segment (`u_dec`), so a name join returns nothing exactly where the id is
-- needed most.
-- inner_signal_name is already coalesced: NULL inner means "the formal's own
-- name", and every consumer repeating that COALESCE was the reason to do it
-- once here. connection_kind decides what NULL means elsewhere in the row
-- (constant, unconnected and external ties all have no outer name), so read
-- it rather than testing outer_signal_name IS NULL.
CREATE VIEW v_port_connection AS
SELECT
    p.module      AS parent_module_id,
    pm.name       AS parent_module_name,
    pm.params     AS parent_module_params,
    cn.text       AS child_instance_name,
    p.child_id    AS child_id,
    p.def_module  AS child_module_id,
    cm.name       AS child_module_name,
    cm.params     AS child_module_params,
    fp.text       AS formal_port_name,
    COALESCE(inn.text, fp.text) AS inner_signal_name,
    p.port_lo     AS formal_lo,
    p.port_hi     AS formal_hi,
    p.port_exact  AS formal_exact,
    CASE p.direction WHEN 0 THEN 'input' WHEN 1 THEN 'output'
                     WHEN 2 THEN 'inout' WHEN 3 THEN 'ref' END AS direction_name,
    onm.text      AS outer_signal_name,
    ot.text       AS outer_type_text,
    p.outer_width AS outer_width,
    p.outer_lo    AS outer_lo,
    p.outer_hi    AS outer_hi,
    p.outer_exact AS outer_exact,
    p.map_exact   AS mapping_exact,
    CASE p.conn_kind WHEN 0 THEN 'signal'
                     WHEN 1 THEN 'constant'
                     WHEN 2 THEN 'unconnected'
                     WHEN 3 THEN 'expression_operand'
                     WHEN 4 THEN 'interface'
                     WHEN 5 THEN 'external_reference' END AS connection_kind_name,
    mp.text       AS modport_name,
    f.path        AS file_path,
    sf.path       AS source_path,
    p.line        AS source_line
FROM port p
JOIN module pm           ON pm.id = p.module
JOIN name cn             ON cn.id = p.child_name
LEFT JOIN module cm      ON cm.id = p.def_module
JOIN name fp             ON fp.id = p.port
LEFT JOIN name inn       ON inn.id = p.inner
LEFT JOIN name onm       ON onm.id = p.outer
LEFT JOIN type ot        ON ot.id = p.outer_type
LEFT JOIN name mp        ON mp.id = p.modport
LEFT JOIN file f         ON f.id = p.file
LEFT JOIN source_file sf ON sf.id = f.source_file;

-- One row per `edge`: the direction-neutral base v_driver and v_load rename.
-- source_name NULL is a real record -- a statement that drives the target
-- while reading nothing nameable (`assign q = 1'b0;`) -- and is preserved, not
-- filtered. Bit ranges keep edge's semantics unchanged: LSB-relative offsets
-- into the flattened object, NULL+exact=1 the whole object, NULL+exact=0
-- somewhere unknown inside it; and mapping_exact=0 means the dependency is
-- real but only traceable at range granularity, not that it is doubtful.
CREATE VIEW v_dependency AS
SELECT
    e.module    AS module_id,
    m.name      AS module_name,
    m.params    AS module_params,
    sn.text     AS source_name,
    st.text     AS source_type,
    e.src_lo    AS source_lo,
    e.src_hi    AS source_hi,
    e.src_exact AS source_exact,
    dn.text     AS target_name,
    dt.text     AS target_type,
    e.dst_lo    AS target_lo,
    e.dst_hi    AS target_hi,
    e.dst_exact AS target_exact,
    e.kind      AS dependency_kind,
    e.construct AS construct,
    e.control   AS is_control,
    e.map_exact AS mapping_exact,
    f.path      AS file_path,
    sf.path     AS source_path,
    e.line      AS source_line
FROM edge e
JOIN module m            ON m.id = e.module
LEFT JOIN name sn        ON sn.id = e.src
LEFT JOIN type st        ON st.id = e.src_type
JOIN name dn             ON dn.id = e.dst
LEFT JOIN type dt        ON dt.id = e.dst_type
LEFT JOIN file f         ON f.id = e.file
LEFT JOIN source_file sf ON sf.id = f.source_file;

-- v_dependency read from the target's side: one row per direct driving
-- relation of signal_name. driver_name NULL stays -- the statement drives the
-- signal even though no source can be named -- and control edges stay, marked
-- is_control=1, for the consumer to keep or drop. One step, in-module: the
-- root driver across hierarchy is a walk the consumer composes.
CREATE VIEW v_driver AS
SELECT
    module_id, module_name, module_params,
    target_name  AS signal_name,
    target_type  AS signal_type,
    target_lo    AS signal_lo,
    target_hi    AS signal_hi,
    target_exact AS signal_exact,
    source_name  AS driver_name,
    source_type  AS driver_type,
    source_lo    AS driver_lo,
    source_hi    AS driver_hi,
    source_exact AS driver_exact,
    dependency_kind, construct, is_control, mapping_exact,
    file_path, source_path, source_line
FROM v_dependency;

-- EVERY recorded read of signal_name, one row each -- the storage split
-- undone. "Who reads it" was always one question; the answer lives in three
-- tables for storage reasons (a read with no written target has no edge to
-- ride), and making the consumer know that was exactly the internal knowledge
-- this interface exists to retire. The netlist analogy decides what belongs
-- here: a clock net's loads include the flop clock pins, so a sensitivity
-- read is a load; an assertion's reads are its checker pins, so they are
-- loads too. load_kind says which shape each row is:
--
--   dataflow      an edge: the signal feeds a target this module names.
--                 load_* name the target; is_control and mapping_exact apply.
--   sensitivity   a procedure triggers on it (proc_event, wait=0). construct
--                 is the edge word: posedge / negedge / both.
--   wait          a procedure suspends on it (proc_event, wait=1).
--   statement     a statement reads it and writes nothing nameable: an
--                 assertion, a $display, a writing call's argument.
--                 construct is that statement's own word.
--
-- load_kind is the read's SEMANTICS, not which table stored it. A plain event
-- (`@(posedge clk)`) lands in proc_event; a selected-bit or expression event
-- (`@(posedge clks[2])`) cannot name a plain signal there and lands in
-- stmt_read with construct = sensitivity/wait -- and both spellings of the
-- same semantics must answer to the same load_kind, or the view leaks the
-- storage split it exists to hide.
--
-- Rows with no written target carry load_* all NULL -- symmetric with
-- v_driver keeping driver_name NULL for a driving statement with no nameable
-- source: a real reader with no nameable target. is_control and mapping_exact
-- are NULL there too; nothing exists to correspond with. What this view still
-- cannot include: a read made FROM another module through a hierarchical
-- path -- that is hier_ref text, resolved against the tree by the consumer.
CREATE VIEW v_load AS
SELECT
    module_id, module_name, module_params,
    source_name  AS signal_name,
    source_type  AS signal_type,
    source_lo    AS signal_lo,
    source_hi    AS signal_hi,
    source_exact AS signal_exact,
    target_name  AS load_name,
    target_type  AS load_type,
    target_lo    AS load_lo,
    target_hi    AS load_hi,
    target_exact AS load_exact,
    'dataflow'   AS load_kind,
    dependency_kind, construct, is_control, mapping_exact,
    file_path, source_path, source_line
FROM v_dependency
WHERE source_name IS NOT NULL
UNION ALL
SELECT
    pe.module    AS module_id,
    m.name       AS module_name,
    m.params     AS module_params,
    n.text       AS signal_name,
    NULL         AS signal_type,
    NULL         AS signal_lo,
    NULL         AS signal_hi,
    1            AS signal_exact,
    NULL         AS load_name,
    NULL         AS load_type,
    NULL         AS load_lo,
    NULL         AS load_hi,
    NULL         AS load_exact,
    CASE pe.wait WHEN 1 THEN 'wait' ELSE 'sensitivity' END AS load_kind,
    NULL         AS dependency_kind,
    pe.edge_kind AS construct,
    NULL         AS is_control,
    NULL         AS mapping_exact,
    f.path       AS file_path,
    sf.path      AS source_path,
    pe.line      AS source_line
FROM proc_event pe
JOIN module m            ON m.id = pe.module
-- The inner join on `name` is the NULL filter: an event row whose expression
-- was not a plain signal has nothing to be a load of, and no name row to
-- join. Spelling the filter as a WHERE too pushed the planner onto a range
-- scan where the join gives it an equality seek.
JOIN name n              ON n.id = pe.signal
LEFT JOIN file f         ON f.id = pe.file
LEFT JOIN source_file sf ON sf.id = f.source_file
UNION ALL
SELECT
    r.module     AS module_id,
    m.name       AS module_name,
    m.params     AS module_params,
    n.text       AS signal_name,
    NULL         AS signal_type,
    r.src_lo     AS signal_lo,
    r.src_hi     AS signal_hi,
    r.src_exact  AS signal_exact,
    NULL         AS load_name,
    NULL         AS load_type,
    NULL         AS load_lo,
    NULL         AS load_hi,
    NULL         AS load_exact,
    CASE WHEN r.construct = 'sensitivity' THEN 'sensitivity'
         WHEN r.construct = 'wait'        THEN 'wait'
         ELSE 'statement' END AS load_kind,
    r.kind       AS dependency_kind,
    r.construct  AS construct,
    NULL         AS is_control,
    NULL         AS mapping_exact,
    f.path       AS file_path,
    sf.path      AS source_path,
    r.line       AS source_line
FROM stmt_read r
JOIN module m            ON m.id = r.module
JOIN name n              ON n.id = r.name
LEFT JOIN file f         ON f.id = r.file
LEFT JOIN source_file sf ON sf.id = f.source_file;

-- One row per `assignment`: the statement object, the other half of the
-- edge/assignment dual projection -- and the one noun this database has that
-- a netlist database does not, because a netlist has no statements and no
-- file:line. statement_id is assignment.id, the primary key the schema
-- already maintains because assign_operand references it; `stmt` beside it
-- is the per-module ordinal shared with hier_ref and stmt_read, exposed here
-- because this is the view that makes statement identity queryable at all.
-- `proc` joins proc_event.proc (same module) to reach the statement's
-- sensitivity. No target_type: `assignment` stores none, and a view invents
-- no columns.
CREATE VIEW v_statement AS
SELECT
    a.id        AS statement_id,
    a.module    AS module_id,
    m.name      AS module_name,
    m.params    AS module_params,
    dn.text     AS target_name,
    a.dst_lo    AS target_lo,
    a.dst_hi    AS target_hi,
    a.dst_exact AS target_exact,
    a.kind      AS statement_kind,
    a.construct AS construct,
    a.proc      AS proc,
    a.seq       AS seq,
    a.blocking  AS blocking,
    a.dropped_operands AS dropped_operands,
    a.stmt      AS stmt,
    f.path      AS file_path,
    sf.path     AS source_path,
    a.line      AS source_line
FROM assignment a
JOIN module m            ON m.id = a.module
JOIN name dn             ON dn.id = a.dst
LEFT JOIN file f         ON f.id = a.file
LEFT JOIN source_file sf ON sf.id = f.source_file;

-- One row per `assign_operand`: a statement's exact read set, the attribute
-- the naive edge/assignment join gets wrong and this schema keeps repeating
-- "use assign_operand" about -- now without asking the consumer to intern-join
-- for it. The module columns ride along (through two primary keys, so nothing
-- fans out) so the round-one question "which statements read X in M" is one
-- WHERE clause.
CREATE VIEW v_statement_operand AS
SELECT
    ao.assignment AS statement_id,
    a.module      AS module_id,
    m.name        AS module_name,
    m.params      AS module_params,
    n.text        AS operand_name,
    ao.src_lo     AS operand_lo,
    ao.src_hi     AS operand_hi,
    ao.src_exact  AS operand_exact
FROM assign_operand ao
JOIN assignment a ON a.id = ao.assignment
JOIN module m     ON m.id = a.module
JOIN name n       ON n.id = ao.name;
)SQL";

// Rows per transaction. Committing per row is orders of magnitude slower;
// never committing means the whole export is one transaction whose rollback
// journal grows without bound.
constexpr int64_t kBatch = 20000;

void bindOptText(sqlite3_stmt* s, int i, const std::string& v) {
    if (v.empty())
        sqlite3_bind_null(s, i);
    else
        sqlite3_bind_text(s, i, v.c_str(), -1, SQLITE_TRANSIENT);
}

void bindOptId(sqlite3_stmt* s, int i, int64_t id) {
    if (id)
        sqlite3_bind_int64(s, i, id);
    else
        sqlite3_bind_null(s, i);
}

void bindOptRange(sqlite3_stmt* s, int lo, int hi,
                  const std::optional<std::pair<uint64_t, uint64_t>>& r) {
    if (!r) {
        sqlite3_bind_null(s, lo);
        sqlite3_bind_null(s, hi);
    }
    else {
        sqlite3_bind_int64(s, lo, static_cast<int64_t>(r->first));
        sqlite3_bind_int64(s, hi, static_cast<int64_t>(r->second));
    }
}

} // namespace

Writer::Writer(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        // sqlite3_open allocates a handle even when it fails, and the
        // destructor does not run for a constructor that throws.
        std::string msg = sqlite3_errmsg(db);
        sqlite3_close(db);
        db = nullptr;
        throw std::runtime_error("cannot create " + path + ": " + msg);
    }
    try {

    // The database is a build artifact: if the process dies it is rebuilt from
    // source, so paying for durability buys nothing and costs a large fraction
    // of the write time.
    exec("PRAGMA journal_mode=OFF");
    exec("PRAGMA synchronous=OFF");
    exec(kSchema);

    prepare("INSERT INTO edge VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &insEdge);
    prepare("INSERT INTO child VALUES(?,?,?,?,?,?)", &insChild);
    prepare("INSERT INTO instance VALUES(?,?,?,?,?)", &insInstance);
    prepare("INSERT INTO port VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            &insPort);
        prepare("INSERT INTO symbol VALUES(?,?,?,?,?,?,?,?,?)", &insSymbol);
        prepare("INSERT INTO assignment VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &insAssign);
        prepare("INSERT INTO proc_event VALUES(?,?,?,?,?,?,?)", &insProcEvent);
        prepare("INSERT INTO assign_operand VALUES(?,?,?,?,?)", &insAssignOp);
        prepare("INSERT INTO stmt_read VALUES(?,?,?,?,?,?,?,?,?,?)", &insStmtRead);
        prepare("INSERT INTO hier_ref VALUES(?,?,?,?,?,?,?,?,?,?,?)", &insHierRef);
        begin();
    }
    catch (...) {
        sqlite3_finalize(insEdge);
        sqlite3_finalize(insChild);
        sqlite3_finalize(insInstance);
        sqlite3_finalize(insPort);
        sqlite3_finalize(insSymbol);
        sqlite3_finalize(insAssign);
        sqlite3_finalize(insProcEvent);
        sqlite3_finalize(insAssignOp);
        sqlite3_finalize(insStmtRead);
        sqlite3_finalize(insHierRef);
        sqlite3_close(db);
        db = nullptr;
        throw;
    }
}

Writer::~Writer() {
    if (db) {
        sqlite3_finalize(insEdge);
        sqlite3_finalize(insChild);
        sqlite3_finalize(insInstance);
        sqlite3_finalize(insPort);
        sqlite3_finalize(insSymbol);
        sqlite3_finalize(insAssign);
        sqlite3_finalize(insProcEvent);
        sqlite3_finalize(insAssignOp);
        sqlite3_finalize(insStmtRead);
        sqlite3_finalize(insHierRef);
        sqlite3_close(db);
    }
}

void Writer::prepare(const char* sql, sqlite3_stmt** out) {
    if (sqlite3_prepare_v2(db, sql, -1, out, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("sqlite: preparing \"") + sql +
                                 "\": " + sqlite3_errmsg(db));
    }
}

/// Runs a statement to completion. An unchecked step is how a table quietly
/// comes out short: SQLITE_FULL or an I/O error mid-export would otherwise be
/// invisible and the truncated database reported as a success.
void Writer::step(sqlite3_stmt* stmt) {
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db));
}

void Writer::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("sqlite: " + msg);
    }
}

void Writer::begin() {
    if (!inTransaction) {
        exec("BEGIN");
        inTransaction = true;
    }
}

void Writer::commit() {
    if (inTransaction) {
        exec("COMMIT");
        inTransaction = false;
    }
    pending = 0;
}

void Writer::setMeta(std::string_view key, std::string_view value) {
    sqlite3_stmt* s = nullptr;
    prepare("INSERT OR REPLACE INTO meta VALUES(?,?)", &s);
    sqlite3_bind_text(s, 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE)
        throw std::runtime_error(std::string("sqlite: writing meta: ") + sqlite3_errmsg(db));
}

void Writer::addSourceFile(const std::string& path, const std::string& digest) {
    sqlite3_stmt* s = nullptr;
    prepare("INSERT OR IGNORE INTO source_file(path,digest) VALUES(?,?)", &s);
    sqlite3_bind_text(s, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, digest.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("sqlite: recording source file ") + path + ": " +
                                 sqlite3_errmsg(db));
    }
}

void Writer::linkSourceFiles(
    const std::unordered_map<std::string, std::string>& origins) {
    // The id comes from the subquery, not from a map this class maintains
    // alongside the table. The map had to be kept consistent with insert order
    // by hand, and any source_file row inserted without going through
    // addSourceFile would have broken the join silently. A row with no match
    // -- a synthesized buffer, which is not a file and was never hashed --
    // yields NULL, which is what "no origin" already means in this column.
    sqlite3_stmt* s = nullptr;
    prepare("UPDATE file SET source_file="
            "(SELECT id FROM source_file WHERE path = ?1) WHERE path = ?2", &s);
    for (auto& [asWritten, full] : origins) {
        sqlite3_reset(s);
        sqlite3_bind_text(s, 1, full.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, asWritten.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) != SQLITE_DONE) {
            std::string msg = sqlite3_errmsg(db);
            sqlite3_finalize(s);
            throw std::runtime_error("sqlite: linking file rows: " + msg);
        }
    }
    sqlite3_finalize(s);
}

int64_t Writer::internModule(const std::string& name, const std::string& params) {
    sqlite3_stmt* s = nullptr;
    prepare("SELECT id FROM module WHERE name=? AND params=?", &s);
    sqlite3_bind_text(s, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, params.c_str(), -1, SQLITE_TRANSIENT);
    int64_t id = 0;
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW)
        id = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("sqlite: looking up module ") + name + ": " +
                                 sqlite3_errmsg(db));
    }
    if (id)
        return id;

    prepare("INSERT INTO module(name,params) VALUES(?,?)", &s);
    sqlite3_bind_text(s, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, params.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);
    sqlite3_finalize(s);
    // Checked for the same reason internInto states: last_insert_rowid() after a
    // failed insert returns the id of whatever went in before it, so a single
    // failure here would silently point every row referencing this module --
    // its edges, symbols, ports and children -- at a different module.
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("sqlite: interning module ") + name + ": " +
                                 sqlite3_errmsg(db));
    }
    return sqlite3_last_insert_rowid(db);
}

namespace {
int64_t internInto(sqlite3* db, std::unordered_map<std::string, int64_t>& cache,
                   const char* table, const char* column, const std::string& text) {
    if (text.empty())
        return 0;
    if (auto it = cache.find(text); it != cache.end())
        return it->second;
    std::string sql = std::string("INSERT INTO ") + table + "(" + column + ") VALUES(?)";
    // (single-column insert)
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db));
    // Bound with an explicit length: the -1 form stops at the first NUL, so a
    // name carrying one would be stored truncated and alias another entry.
    sqlite3_bind_text(s, 1, text.data(), static_cast<int>(text.size()),
                      SQLITE_TRANSIENT);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    // Never fall back to last_insert_rowid() on failure. That returns the id of
    // whatever was inserted *before*, so one failed insert would silently make
    // every later reference to this string resolve to a different one.
    if (rc != SQLITE_DONE)
        throw std::runtime_error(std::string("sqlite: interning into ") + table + ": " +
                                 sqlite3_errmsg(db));
    int64_t id = sqlite3_last_insert_rowid(db);
    cache.emplace(text, id);
    return id;
}
} // namespace

int64_t Writer::internType(const std::string& text) {
    return internInto(db, typeIds, "type", "text", text);
}

int64_t Writer::internFile(const std::string& path) {
    return internInto(db, fileIds, "file", "path", path);
}

int64_t Writer::internName(const std::string& text) {
    return internInto(db, nameIds, "name", "text", text);
}

namespace {
/// 0=in 1=out 2=inout 3=ref, matching the schema comment. Anything else is
/// 4=unknown rather than being folded into `ref`, so a direction this tool does
/// not model yet is visible instead of being quietly mislabelled.
int directionCode(const std::string& d) {
    if (d == "in") return 0;
    if (d == "out") return 1;
    if (d == "inout") return 2;
    if (d == "ref") return 3;
    return 4;
}
} // namespace

void Writer::addEdges(int64_t moduleId, const std::vector<EdgeRow>& rows) {
    for (auto& r : rows) {
        sqlite3_reset(insEdge);
        sqlite3_bind_int64(insEdge, 1, moduleId);
        bindOptId(insEdge, 2, internName(r.src));
        sqlite3_bind_int64(insEdge, 3, internName(r.dst));
        bindOptId(insEdge, 4, internType(r.srcType));
        bindOptId(insEdge, 5, internType(r.dstType));
        bindOptText(insEdge, 6, r.kind);
        bindOptText(insEdge, 7, r.construct);
        bindOptId(insEdge, 8, internFile(r.file));
        sqlite3_bind_int64(insEdge, 9, r.line);
        sqlite3_bind_int(insEdge, 10, r.control ? 1 : 0);
        // A row with no source has no source end to describe, so every column
        // describing one is NULL -- the discipline `port` applies to a tie-off
        // ("so it does not read as 'the whole of nothing, exactly'"), applied
        // here at the one chokepoint every emitter goes through. Before this,
        // `assign q = 1'b0` carried src_exact=1 and map_exact=1: a driver
        // query reported a per-bit mapping onto a driver that does not exist,
        // and a bit-level tracer was invited to follow it.
        if (r.src.empty()) {
            bindOptRange(insEdge, 11, 12, std::nullopt);
            sqlite3_bind_null(insEdge, 13);
        }
        else {
            bindOptRange(insEdge, 11, 12, r.srcBits);
            sqlite3_bind_int(insEdge, 13, r.srcExact ? 1 : 0);
        }
        bindOptRange(insEdge, 14, 15, r.dstBits);
        sqlite3_bind_int(insEdge, 16, r.dstExact ? 1 : 0);
        if (r.src.empty())
            sqlite3_bind_null(insEdge, 17);
        else
            sqlite3_bind_int(insEdge, 17, r.mapExact ? 1 : 0);
        step(insEdge);
        if (++pending >= kBatch) {
            commit();
            begin();
        }
    }
}

std::vector<int64_t> Writer::addChildren(int64_t moduleId,
                                         const std::vector<ChildRow>& rows) {
    std::vector<int64_t> ids;
    ids.reserve(rows.size());
    for (auto& r : rows) {
        sqlite3_reset(insChild);
        sqlite3_bind_null(insChild, 1);                    // autoincrement id
        sqlite3_bind_int64(insChild, 2, moduleId);
        sqlite3_bind_int64(insChild, 3, internName(r.name));
        bindOptId(insChild, 4, internName(r.defName));
        if (r.defModule)
            sqlite3_bind_int64(insChild, 5, r.defModule);
        else
            sqlite3_bind_null(insChild, 5);
        sqlite3_bind_text(insChild, 6,
                          r.kind == ChildKind::Primitive    ? "primitive"
                          : r.kind == ChildKind::Unresolved ? "unresolved"
                                                            : "module",
                          -1, SQLITE_STATIC);
        step(insChild);
        ids.push_back(sqlite3_last_insert_rowid(db));
        if (++pending >= kBatch) {
            commit();
            begin();
        }
    }
    return ids;
}

void Writer::addPorts(int64_t moduleId, int64_t defModuleId, int64_t childId,
                      const std::vector<PortRow>& rows) {
    for (auto& r : rows) {
        sqlite3_reset(insPort);
        sqlite3_bind_int64(insPort, 1, moduleId);
        sqlite3_bind_int64(insPort, 2, internName(r.child));
        bindOptId(insPort, 3, defModuleId);
        sqlite3_bind_int64(insPort, 4, internName(r.port));
        // An interface port has no direction at all; NULL says so, where a
        // made-up code would read as a direction this tool failed to model.
        if (r.direction.empty())
            sqlite3_bind_null(insPort, 6);
        else
            sqlite3_bind_int(insPort, 6, directionCode(r.direction));
        bindOptId(insPort, 5, internName(r.inner));
        bindOptId(insPort, 7, internName(r.outer));
        bindOptId(insPort, 8, internType(r.outerType));
        if (r.outerWidth >= 0)
            sqlite3_bind_int64(insPort, 9, r.outerWidth);
        else
            sqlite3_bind_null(insPort, 9);
        // A row with no outer net has no bits to describe; all three stay NULL
        // so a tie-off does not read as "the whole of nothing, exactly".
        if (r.outer.empty()) {
            bindOptRange(insPort, 10, 11, std::nullopt);
            sqlite3_bind_null(insPort, 12);
        }
        else {
            bindOptRange(insPort, 10, 11, r.outerBits);
            sqlite3_bind_int(insPort, 12, r.outerExact ? 1 : 0);
        }
        sqlite3_bind_int(insPort, 13, static_cast<int>(r.conn));
        bindOptId(insPort, 14, internName(r.modport));
        bindOptId(insPort, 15, internFile(r.file));
        sqlite3_bind_int64(insPort, 16, r.line);
        sqlite3_bind_int64(insPort, 17, childId);
        bindOptRange(insPort, 18, 19, r.portBits);
        if (r.portExact < 0)
            sqlite3_bind_null(insPort, 20);
        else
            sqlite3_bind_int(insPort, 20, r.portExact);
        if (r.mapExact < 0)
            sqlite3_bind_null(insPort, 21);
        else
            sqlite3_bind_int(insPort, 21, r.mapExact);
        step(insPort);
        if (++pending >= kBatch) {
            commit();
            begin();
        }
    }
}

void Writer::addSymbols(int64_t moduleId, const std::vector<SymbolRow>& rows) {
    for (auto& r : rows) {
        sqlite3_reset(insSymbol);
        sqlite3_bind_int64(insSymbol, 1, moduleId);
        sqlite3_bind_int64(insSymbol, 2, internName(r.name));
        bindOptText(insSymbol, 3, r.kind);
        bindOptId(insSymbol, 4, internType(r.type));
        if (r.width >= 0)
            sqlite3_bind_int64(insSymbol, 5, r.width);
        else
            sqlite3_bind_null(insSymbol, 5);
        if (r.direction.empty())
            sqlite3_bind_null(insSymbol, 6);
        else
            sqlite3_bind_int(insSymbol, 6, directionCode(r.direction));
        bindOptId(insSymbol, 7, internFile(r.file));
        sqlite3_bind_int64(insSymbol, 8, r.line);
        sqlite3_bind_int64(insSymbol, 9, r.col);
        step(insSymbol);
        if (++pending >= kBatch) {
            commit();
            begin();
        }
    }
}

void Writer::addStmtReads(int64_t moduleId, const std::vector<StmtReadRow>& rows) {
    for (auto& r : rows) {
        sqlite3_reset(insStmtRead);
        sqlite3_bind_int64(insStmtRead, 1, moduleId);
        sqlite3_bind_int64(insStmtRead, 2, internName(r.name));
        bindOptText(insStmtRead, 3, r.kind);
        bindOptText(insStmtRead, 4, r.construct);
        bindOptId(insStmtRead, 5, internFile(r.file));
        sqlite3_bind_int64(insStmtRead, 6, r.line);
        bindOptRange(insStmtRead, 7, 8, r.bits);
        sqlite3_bind_int(insStmtRead, 9, r.exact ? 1 : 0);
        // Ordinals start at 1, so 0 spells "not attributed to a statement" and
        // stores as NULL -- the stored number is the ordinal itself, unshifted.
        bindOptId(insStmtRead, 10, r.stmt);
        step(insStmtRead);
        if (++pending >= kBatch) {
            commit();
            begin();
        }
    }
}

int64_t Writer::addAssignment(int64_t moduleId, const AssignRow& row,
                              const std::vector<OperandRow>& operands) {
    sqlite3_reset(insAssign);
    sqlite3_bind_null(insAssign, 1);                       // autoincrement id
    sqlite3_bind_int64(insAssign, 2, moduleId);
    sqlite3_bind_int64(insAssign, 3, internName(row.dst));
    bindOptRange(insAssign, 4, 5, row.dstBits);
    sqlite3_bind_int(insAssign, 6, row.dstExact ? 1 : 0);
    bindOptText(insAssign, 7, row.kind);
    bindOptText(insAssign, 8, row.construct);
    bindOptId(insAssign, 9, internFile(row.file));
    sqlite3_bind_int64(insAssign, 10, row.line);
    // NULL when the statement is not inside a procedure at all -- a net
    // declared with an initialiser. Spelled the way `blocking` two columns
    // along already spells "does not apply", rather than as a -1 a consumer
    // would have to know to exclude before joining on it.
    if (row.proc < 0)
        sqlite3_bind_null(insAssign, 11);
    else
        sqlite3_bind_int64(insAssign, 11, row.proc);
    sqlite3_bind_int64(insAssign, 12, row.seq);
    if (row.blocking < 0)
        sqlite3_bind_null(insAssign, 13);
    else
        sqlite3_bind_int(insAssign, 13, row.blocking);
    sqlite3_bind_int64(insAssign, 14, row.dropped);
    bindOptId(insAssign, 15, row.stmt);
    step(insAssign);
    const int64_t id = sqlite3_last_insert_rowid(db);

    for (auto& op : operands) {
        sqlite3_reset(insAssignOp);
        sqlite3_bind_int64(insAssignOp, 1, id);
        sqlite3_bind_int64(insAssignOp, 2, internName(op.name));
        bindOptRange(insAssignOp, 3, 4, op.bits);
        sqlite3_bind_int(insAssignOp, 5, op.exact ? 1 : 0);
        step(insAssignOp);
    }

    pending += 1 + static_cast<int64_t>(operands.size());
    if (pending >= kBatch) {
        commit();
        begin();
    }
    return id;
}

void Writer::addProcEvents(int64_t moduleId, int64_t proc,
                           const std::vector<ProcEventRow>& events) {
    for (auto& e : events) {
        sqlite3_reset(insProcEvent);
        sqlite3_bind_int64(insProcEvent, 1, moduleId);
        sqlite3_bind_int64(insProcEvent, 2, proc);
        bindOptId(insProcEvent, 3, internName(e.signal));
        bindOptText(insProcEvent, 4, e.edge);
        sqlite3_bind_int(insProcEvent, 5, e.wait ? 1 : 0);
        bindOptId(insProcEvent, 6, internFile(e.file));
        sqlite3_bind_int64(insProcEvent, 7, e.line);
        step(insProcEvent);
        pending++;
    }
    if (pending >= kBatch) {
        commit();
        begin();
    }
}

void Writer::addHierRefs(int64_t moduleId, const std::vector<HierRefRow>& rows) {
    for (auto& r : rows) {
        sqlite3_reset(insHierRef);
        sqlite3_bind_int64(insHierRef, 1, moduleId);
        sqlite3_bind_int64(insHierRef, 2, internName(r.path));
        sqlite3_bind_int(insHierRef, 3, r.write ? 1 : 0);
        bindOptText(insHierRef, 4, r.kind);
        bindOptText(insHierRef, 5, r.construct);
        bindOptId(insHierRef, 6, internFile(r.file));
        sqlite3_bind_int64(insHierRef, 7, r.line);
        bindOptRange(insHierRef, 8, 9, r.bits);
        sqlite3_bind_int(insHierRef, 10, r.exact ? 1 : 0);
        bindOptId(insHierRef, 11, r.stmt);
        step(insHierRef);
        if (++pending >= kBatch) {
            commit();
            begin();
        }
    }
}

void Writer::addInstance(const std::string& name, int64_t moduleId, int64_t parentId,
                         int64_t rowId, int64_t childId) {
    sqlite3_reset(insInstance);
    sqlite3_bind_int64(insInstance, 1, rowId);
    sqlite3_bind_int64(insInstance, 2, internName(name));
    bindOptId(insInstance, 3, moduleId);   // NULL for a generate block
    if (parentId)
        sqlite3_bind_int64(insInstance, 4, parentId);
    else
        sqlite3_bind_null(insInstance, 4);
    bindOptId(insInstance, 5, childId);    // NULL for the root and generate levels
    step(insInstance);
    if (++pending >= kBatch) {
        commit();
        begin();
    }
}

void Writer::finish() {
    commit();
    exec(kIndexes);
    // After the indexes: views are stored SQL and cost the write path nothing,
    // but creating them here keeps the publish order legible -- data, then
    // indexes, then the query interface over both.
    exec(kViews);
}

// ---------------------------------------------------------------- SHA-256
//
// Self-contained rather than pulled from a dependency: the exporter needs
// exactly one hash of one file at a time, and a crypto library is a large thing
// to add and to license for that.
namespace {

struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint64_t len = 0;
    unsigned char buf[64] = {};
    size_t bufLen = 0;

    static uint32_t ror(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

    void block(const unsigned char* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
            0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
            0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
            0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                   (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const unsigned char* p, size_t n) {
        len += n;
        while (n) {
            size_t take = std::min(n, size_t(64) - bufLen);
            std::memcpy(buf + bufLen, p, take);
            bufLen += take; p += take; n -= take;
            if (bufLen == 64) { block(buf); bufLen = 0; }
        }
    }

    std::string final() {
        uint64_t bits = len * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char z = 0;
        while (bufLen != 56)
            update(&z, 1);
        unsigned char tail[8];
        for (int i = 0; i < 8; i++)
            tail[i] = static_cast<unsigned char>(bits >> (56 - i * 8));
        // update() would re-count these into `len`, but `bits` is already fixed.
        std::memcpy(buf + bufLen, tail, 8);
        block(buf);
        static const char* hex = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (uint32_t v : h)
            for (int i = 3; i >= 0; i--) {
                unsigned char byte = static_cast<unsigned char>(v >> (i * 8));
                out += hex[byte >> 4];
                out += hex[byte & 15];
            }
        return out;
    }
};

} // namespace

std::string fileDigest(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    Sha256 s;
    char chunk[65536];
    while (in.read(chunk, sizeof(chunk)) || in.gcount())
        s.update(reinterpret_cast<unsigned char*>(chunk), size_t(in.gcount()));
    return s.final();
}

std::string digest(std::string_view data) {
    Sha256 s;
    s.update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
    return s.final();
}

} // namespace designdb
