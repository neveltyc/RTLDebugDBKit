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

// The schema is *instance-level*: rows hang off `inst`, the elaborated
// occurrence, not off a folded module variant. Thirty-two copies of one core
// are thirty-two sets of rows, and in exchange every net, terminal and
// statement is one row with one id -- "who drives bit 3 of THIS instance's
// q" is an indexed lookup, and a fan-in cone is a recursive CTE over
// `net_dep` rather than an application-side walk that re-folds the hierarchy
// at every boundary. The fold was v9's trade; this is the other side of it,
// taken deliberately (see doc/designdb-schema.md for the accounting).
//
// Naming rules, so a column can be guessed rather than looked up:
//
//   * Tables are singular_snake_case. `_id` appears exactly where a column
//     holds another table's primary key (`inst.module_id` -> module.id), and
//     nowhere else -- an attribute is never dressed as a key.
//   * A bit range is prefixed with the end it describes (`src_lo`,
//     `tgt_hi`, `term_exact`); a single-range table spells its own range
//     bare (`lo`/`hi`/`is_exact`). `map_exact` always describes the
//     correspondence BETWEEN two ends, never either end's own range.
//   * Kinds and directions are their words (`'input'`, `'constant'`,
//     `'always_ff'`), never integer codes. Three values never deserved the
//     decode table every consumer had to carry; CHECK constraints hold the
//     closed sets.
//   * `ordinal` is position in a declaration or extraction list; `sequence`
//     is execution order inside a procedure. Neither is an identity.
//
// Ranges keep the v7 encoding everywhere: LSB-relative offsets into the
// flattened object, NOT declared indices. `logic [15:8] off` has bit 15 at
// offset 7. NULL bits with exact=1 is the whole object; NULL bits with
// exact=0 is somewhere inside it, unknown where; a present range with
// exact=0 is an upper bound rather than the bits actually touched.
//
// The REFERENCES clauses document the joins; they are not enforced at runtime,
// since `PRAGMA foreign_keys` is left at its default of off. The verifier runs
// `foreign_key_check` on every export -- the ids are all issued by one process
// in one pass, and paying a lookup per inserted row to re-verify them is the
// wrong side of that trade.
constexpr const char* kSchema = R"SQL(
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);

CREATE TABLE src_file(
    id      INTEGER PRIMARY KEY,
    path    TEXT UNIQUE,
    digest  TEXT);

-- `path` is the spelling the rows carry -- as written in the filelist or on
-- the command line -- while `src_file.path` is absolute. The two genuinely
-- differ, so `src_file_id` joins them: without it a consumer checking
-- digests had to match spellings by basename, which breaks on the first
-- design with two files of one name.
CREATE TABLE file(
    id             INTEGER PRIMARY KEY,
    path           TEXT UNIQUE,
    src_file_id    INTEGER REFERENCES src_file(id));

-- Repeated type text, interned -- the one intern table v10 keeps. A packed
-- struct or enum prints as its entire member list, far larger than the row
-- referencing it, and the instance-level model repeats each declaration once
-- per occurrence. Names are not interned: they are short, and the join every
-- query paid the old `name` table was the storage tail wagging the schema.
CREATE TABLE data_type(id INTEGER PRIMARY KEY, text TEXT UNIQUE);

-- A source definition: what was written, not what it elaborated into. The
-- parameter values a body elaborated with are per-occurrence facts and live
-- on `inst.param_signature` -- v9 folded them into module identity, v10
-- puts identity where the source has it. (name, file_id, line) is the
-- definition's own identity; two libraries may define one name.
CREATE TABLE module(
    id              INTEGER PRIMARY KEY,
    name            TEXT NOT NULL,
    def_kind        TEXT NOT NULL
        /*!*/CHECK(def_kind IN ('module','interface','program','checker'))/*!*/,
    file_id         INTEGER REFERENCES file(id),
    line            INTEGER,
    col             INTEGER,
    UNIQUE(name, file_id, line));

-- The hierarchy tree, one node per level, one path segment per node --
-- resolving `a.b[0].c` is one indexed lookup per segment, which is why the
-- paths themselves are never stored. This is the supertype: a module
-- instance is a `tree_node` plus an `inst` row under the same id, a gate is
-- a `tree_node` plus a `primitive` row, a generate block is a bare
-- `tree_node`. node_kind says which, and the verifier holds the bijection.
--
--   root        the top instance; it has an `inst` row and no parent.
--   instance    a resolved module/interface/program instance -> `inst`.
--   generate    a generate block or one element of a generate array. A
--               naming level, not an object; nothing subtypes it.
--   primitive   a gate, switch or UDP -> `primitive`.
--   unresolved  an instance whose definition slang could not find -> `inst`
--               with module_id NULL. A trace really does stop here.
CREATE TABLE tree_node(
    id             INTEGER PRIMARY KEY,
    parent_node_id INTEGER REFERENCES tree_node(id),
    name           TEXT NOT NULL,
    node_kind      TEXT NOT NULL
        /*!*/CHECK(node_kind IN ('root','instance','generate','primitive','unresolved'))/*!*/,
    ordinal        INTEGER NOT NULL);

-- One module instance occurrence. `id` IS the tree_node id -- one id space
-- for the whole hierarchy, so `net.inst_id` and `tree_node.parent_node_id`
-- never disagree about what an instance is.
--
-- `parent_inst_id` is the nearest enclosing module instance, skipping
-- generate levels -- the ancestry `tree_node` already encodes, denormalised
-- one hop because every ownership check walks it. The verifier holds the two
-- encodings equal.
--
-- `param_signature` is the elaborated parameter values, normalised, in
-- declaration order, localparams included (it over-splits, never
-- under-splits). Instances of one module with one signature share a body and
-- carry identical rows -- the sharing v9 stored once, v10 stamps out.
CREATE TABLE inst(
    id                    INTEGER PRIMARY KEY REFERENCES tree_node(id),
    module_id             INTEGER REFERENCES module(id),
    parent_inst_id        INTEGER REFERENCES inst(id),
    param_signature       TEXT,
    unresolved_def        TEXT,
    file_id               INTEGER REFERENCES file(id),
    line                  INTEGER,
    col                   INTEGER);

-- One gate, switch or UDP instance. `id` IS the tree_node id. Not a module
-- instance (it has no body, no ports, no parameters) and not a statement
-- (nothing was assigned); its dataflow is `net_dep` rows with
-- prim_id set. Expression operators (`&`, `+`, `?:`) are not
-- primitives -- they stay inside their statement's own rows.
CREATE TABLE prim(
    id              INTEGER PRIMARY KEY REFERENCES tree_node(id),
    inst_id         INTEGER NOT NULL REFERENCES inst(id),
    prim_kind       TEXT NOT NULL /*!*/CHECK(prim_kind IN ('gate','switch','udp'))/*!*/,
    def_name        TEXT NOT NULL,
    file_id         INTEGER REFERENCES file(id),
    line            INTEGER,
    col             INTEGER);

-- One connectable object in one instance: a net or a variable -- anything
-- that can be driven, read or wired. Parameters, type parameters and
-- interface ports are NOT here: they are not connectable, and v9 mixing them
-- into `symbol` made every "signals of this scope" query filter them out.
--
-- Subroutine formals and locals ARE here (name is the scope-relative dotted
-- path, `bump.v`): a call binds actuals to formals and `net_dep` ends are
-- ids, so a formal without a row would be a dependency with a dangling end.
-- Their scope_node_id is the nearest tree_node, since a subroutine is not a
-- hierarchy level.
--
-- decl_kind is the net type's own word (`wire`, `wand`, `trireg`,
-- ...) or `variable`. is_implicit=1 marks a net slang created for an
-- undeclared identifier under the active `default_nettype`; its
-- decl_kind is that nettype, and its location is the first use.
CREATE TABLE net(
    id               INTEGER PRIMARY KEY,
    inst_id          INTEGER NOT NULL REFERENCES inst(id),
    scope_node_id    INTEGER NOT NULL REFERENCES tree_node(id),
    name             TEXT NOT NULL,
    decl_kind        TEXT NOT NULL,
    data_type_id     INTEGER REFERENCES data_type(id),
    width            INTEGER,
    is_implicit      INTEGER NOT NULL CHECK(is_implicit IN (0,1)),
    file_id          INTEGER REFERENCES file(id),
    line             INTEGER,
    col              INTEGER);

-- One terminal: a port on an instance's boundary. The root instance's
-- terminals are the design's top-level ports; a child's are the pins its
-- parent connects. One noun for both, because they are the same object seen
-- from the two sides v9 conflated in one `port` row.
--
-- direction is NULL for an interface terminal (it has none) and for a
-- terminal of an unresolved instance (nobody knows). is_const marks
-- `const ref`; NULL where constness does not apply. An unresolved instance
-- still gets terminal rows -- one per connection its parent wrote, named as
-- the connection names them -- so `net_conn` has something to bind and
-- "connected to a black box" stays distinct from "connected to nothing".
CREATE TABLE term(
    id            INTEGER PRIMARY KEY,
    inst_id       INTEGER NOT NULL REFERENCES inst(id),
    name          TEXT NOT NULL,
    term_kind     TEXT NOT NULL /*!*/CHECK(term_kind IN ('signal','interface'))/*!*/,
    direction     TEXT /*!*/CHECK(direction IN ('input','output','inout','ref'))/*!*/,
    data_type_id  INTEGER REFERENCES data_type(id),
    width         INTEGER,
    ordinal       INTEGER NOT NULL,
    is_const      INTEGER CHECK(is_const IN (0,1)),
    modport       TEXT,
    file_id       INTEGER REFERENCES file(id),
    line          INTEGER,
    col           INTEGER);

-- The INSIDE of a terminal: which nets of its own instance it stands for,
-- one row per segment. An ANSI port is one whole-to-whole row; a non-ANSI
-- `.p({hi, lo})` formal is one row per element, each with its window of the
-- terminal (`term_lo`/`term_hi`) and of the net. Both nets belong to the
-- terminal's own instance -- the outside is `net_conn`'s business, and
-- keeping the two relations apart is what v9's one-table version kept
-- getting wrong.
CREATE TABLE term_map(
    term_id       INTEGER NOT NULL REFERENCES term(id),
    ordinal       INTEGER NOT NULL,
    inner_net_id  INTEGER NOT NULL REFERENCES net(id),
    term_lo       INTEGER,
    term_hi       INTEGER,
    term_exact    INTEGER NOT NULL CHECK(term_exact IN (0,1)),
    inner_lo      INTEGER,
    inner_hi      INTEGER,
    inner_exact   INTEGER NOT NULL CHECK(inner_exact IN (0,1)),
    map_exact     INTEGER NOT NULL CHECK(map_exact IN (0,1)),
    PRIMARY KEY(term_id, ordinal));

-- The OUTSIDE of a terminal: what the parent connected to it, one row per
-- atomic segment -- each concatenation element and each replication copy is
-- its own row with its own window of the formal, so `.q({2{r}})` is two
-- rows whose windows tile q. conn_kind decides which columns apply:
--
--   signal              outer_net_id names a net of the PARENT instance.
--   constant            a tie-off; no net, but the window is kept so the
--                       formal's bits tile rather than leaving a gap that
--                       reads as an exporter bug.
--   unconnected         nobody connected it. Recorded, not omitted: absence
--                       would also mean "the exporter did not get this far".
--   expression_operand  the actual is an expression; this row is one net it
--                       reads. `.en(state == RUN)` samples state but does
--                       not alias it to en -- map_exact is 0 by
--                       construction.
--   interface           outer_intf_inst_id names the bound interface
--                       instance. No dataflow arc pretends to cross here.
--   external_reference  tied to something with no name in the parent
--                       (`.p(u.g[7:4])`); outer_hier_ref_id says what, and
--                       the arc crosses once that reference resolves.
--
-- Range discipline: columns describing an end that does not exist are NULL
-- -- a constant has no net end, an interface binding has no bit domain --
-- so a tie-off never reads as "the whole of nothing, exactly".
CREATE TABLE net_conn(
    id                 INTEGER PRIMARY KEY,
    outer_net_id       INTEGER REFERENCES net(id),
    term_id            INTEGER NOT NULL REFERENCES term(id),
    ordinal            INTEGER NOT NULL,
    conn_kind          TEXT NOT NULL
        /*!*/CHECK(conn_kind IN ('signal','constant','unconnected',
                                  'expression_operand','interface','external_reference'))/*!*/,
    outer_lo           INTEGER,
    outer_hi           INTEGER,
    outer_exact        INTEGER CHECK(outer_exact IN (0,1)),
    term_lo            INTEGER,
    term_hi            INTEGER,
    term_exact         INTEGER CHECK(term_exact IN (0,1)),
    map_exact          INTEGER CHECK(map_exact IN (0,1)),
    outer_intf_inst_id INTEGER REFERENCES inst(id),
    outer_hier_ref_id  INTEGER REFERENCES hier_ref(id),
    file_id            INTEGER REFERENCES file(id),
    line               INTEGER,
    col                INTEGER,
    UNIQUE(term_id, ordinal));

-- One procedure: an always/initial/final block, or a task/function body.
-- v9 named these with a bare per-module integer that was not a key in any
-- table; this is the object that integer was gesturing at.
CREATE TABLE proc(
    id             INTEGER PRIMARY KEY,
    inst_id        INTEGER NOT NULL REFERENCES inst(id),
    scope_node_id  INTEGER NOT NULL REFERENCES tree_node(id),
    name           TEXT,
    proc_kind      TEXT NOT NULL
        /*!*/CHECK(proc_kind IN ('always','always_ff','always_comb','always_latch',
                                 'initial','final','task','function'))/*!*/,
    ordinal        INTEGER NOT NULL,
    file_id        INTEGER REFERENCES file(id),
    line           INTEGER,
    col            INTEGER);

-- One statement, or one statement-level construct. The statement is the
-- object; its targets, operands and other reads are child rows -- v9's
-- `assignment` was one row per TARGET, so `{a,b} = {x,y}` was two rows
-- claiming one statement and a statement writing only outward was no row at
-- all. Here it is one row regardless.
--
-- sequence is execution order within the procedure, NULL exactly when
-- proc_id is NULL (a continuous assign has no execution order).
-- assign_kind is NULL for non-assignments. delay is the delay control's
-- normalised source text (`#3`, `#(rise, fall)`), not a number this tool
-- pretended to evaluate; NULL when there is none.
--
-- What is deliberately NOT here, unchanged from v9: no branch-condition
-- text, no clocked flag, no source text. `force` records as a blocking
-- assignment; `release` leaves no row.
CREATE TABLE stmt(
    id                    INTEGER PRIMARY KEY,
    inst_id               INTEGER NOT NULL REFERENCES inst(id),
    scope_node_id         INTEGER NOT NULL REFERENCES tree_node(id),
    proc_id               INTEGER REFERENCES proc(id),
    ordinal               INTEGER NOT NULL,
    sequence              INTEGER,
    stmt_kind             TEXT NOT NULL
        /*!*/CHECK(stmt_kind IN ('assignment','assertion','wait','call',
                                 'system_task','event_control','alias'))/*!*/,
    construct             TEXT,
    assign_kind           TEXT
        /*!*/CHECK(assign_kind IN ('continuous','blocking','nonblocking'))/*!*/,
    delay                 TEXT,
    dropped_operand_count INTEGER NOT NULL,
    file_id               INTEGER REFERENCES file(id),
    line                  INTEGER,
    col                   INTEGER);

-- One target reference of an assignment (LHS), in written order. A target
-- whose net lies outside the instance is not here -- it is a `hier_ref`
-- with access='write' on the same stmt_id.
CREATE TABLE assign_target(
    id       INTEGER PRIMARY KEY,
    stmt_id  INTEGER NOT NULL REFERENCES stmt(id),
    ordinal  INTEGER NOT NULL,
    net_id   INTEGER NOT NULL REFERENCES net(id),
    lo       INTEGER,
    hi       INTEGER,
    is_exact INTEGER NOT NULL CHECK(is_exact IN (0,1)),
    UNIQUE(stmt_id, ordinal));

-- One operand reference of an assignment's RHS. Operands belong to the
-- STATEMENT, not to a target: which operand feeds which target is
-- `net_dep`'s answer, and pairing them here is exactly the cross product
-- v7 removed. A read occurring twice is two rows.
CREATE TABLE assign_operand(
    id       INTEGER PRIMARY KEY,
    stmt_id  INTEGER NOT NULL REFERENCES stmt(id),
    ordinal  INTEGER NOT NULL,
    net_id   INTEGER NOT NULL REFERENCES net(id),
    lo       INTEGER,
    hi       INTEGER,
    is_exact INTEGER NOT NULL CHECK(is_exact IN (0,1)),
    UNIQUE(stmt_id, ordinal));

-- One statement read that is not an assignment operand, classified by role:
--
--   control        a branch condition the statement's targets sit under
--   assertion      read by an assert/assume/cover/expect
--   wait           a wait statement's condition
--   event          a sensitivity/event expression that is not a plain net
--                  (those are proc_event rows -- one read, one table, never
--                  both, or v_load counts it twice)
--   call_argument  an actual at a call site
--   system_task    read by $display and friends
CREATE TABLE expr_ref(
    id       INTEGER PRIMARY KEY,
    stmt_id  INTEGER NOT NULL REFERENCES stmt(id),
    ordinal  INTEGER NOT NULL,
    net_id   INTEGER NOT NULL REFERENCES net(id),
    role     TEXT NOT NULL
        /*!*/CHECK(role IN ('control','assertion','wait','event',
                       'call_argument','system_task'))/*!*/,
    lo       INTEGER,
    hi       INTEGER,
    is_exact INTEGER NOT NULL CHECK(is_exact IN (0,1)),
    UNIQUE(stmt_id, ordinal));

-- One edge event a procedure triggers on or waits on. All of them, because
-- an event list has no order and recording "the first" records how the
-- author arranged the list. Which is the clock is deliberately not decided
-- here, exactly as in v9.
--
-- event_kind='sensitivity' rows belong to the procedure header (stmt_id
-- NULL); event_kind='wait' rows are statement-level controls reached during
-- execution (stmt_id set). net_id is NULL when the event expression is not
-- a plain net -- the reads of such an expression are expr_ref rows with
-- role='event', never both.
CREATE TABLE proc_event(
    id           INTEGER PRIMARY KEY,
    proc_id      INTEGER NOT NULL REFERENCES proc(id),
    stmt_id      INTEGER REFERENCES stmt(id),
    net_id       INTEGER REFERENCES net(id),
    event_kind   TEXT NOT NULL /*!*/CHECK(event_kind IN ('sensitivity','wait'))/*!*/,
    edge_kind    TEXT /*!*/CHECK(edge_kind IN ('posedge','negedge','both'))/*!*/,
    file_id      INTEGER REFERENCES file(id),
    line         INTEGER,
    col          INTEGER);

-- One net-to-net dependency occurrence -- the adjacency list v_driver and
-- v_load index, and the provenance record v9's deduplicated `edge` erased.
-- NOT deduplicated: the same source reaching the same target from two
-- statements is two rows, each naming its statement. `{a,b} = {x,y}` is
-- x->a and y->b, each naming its operand and target rows -- never the
-- four-way cross product.
--
-- dep_kind, and what must be set for each (verifier-enforced):
--
--   data       an assignment moves it: stmt_id, and per end either the
--              local reference (assign_operand_id / assign_target_id) or
--              the resolved hierarchical one (src_hier_ref_id /
--              tgt_hier_ref_id). src_net_id NULL with no source
--              reference of either kind is a constant driver (`q <= 8'h0`)
--              -- the row still names the statement, which is what a driver
--              query reports; every source_* column is NULL with it.
--   control    it reaches the target through a branch condition: stmt_id,
--              the condition's expr_ref_id (role='control') or
--              src_hier_ref_id, and the target's reference as above.
--              map_exact is 0 -- a condition gates, it does not map.
--   primitive  a gate/switch/UDP couples them: prim_id, per LRM
--              (input, output) pairing.
--   procedure  a call binds them: actual to formal by argument direction,
--              and formal back to a written actual. stmt_id is the calling
--              statement (NULL for a call in a control expression); the
--              actual's read is expr_ref_id (role='call_argument') or
--              src_hier_ref_id.
--
-- A dependency whose end lies in ANOTHER instance -- `assign q = u.x;`,
-- a write through an interface port -- carries the resolved net id like
-- any other end, and names the hier_ref row it resolved through instead of
-- an operand/target row. The pairing is made where the statement was
-- walked, one row per (source element, target element) that share bits --
-- never by joining hier_ref to operands on stmt_id afterwards, which would
-- resurrect the cross product v7 removed. An unresolved reference produces
-- no dependency: the hier_ref text is the honest record, and a guessed
-- edge would be a wrong one.
--
-- src_net_id/tgt_net_id repeat what the referenced rows already
-- know. Deliberate, verified redundancy: this table is the driver/load
-- index, and the verifier holds the copies equal.
CREATE TABLE net_dep(
    id                 INTEGER PRIMARY KEY,
    src_net_id         INTEGER REFERENCES net(id),
    tgt_net_id         INTEGER NOT NULL REFERENCES net(id),
    stmt_id            INTEGER REFERENCES stmt(id),
    assign_operand_id  INTEGER REFERENCES assign_operand(id),
    assign_target_id   INTEGER REFERENCES assign_target(id),
    expr_ref_id        INTEGER REFERENCES expr_ref(id),
    prim_id            INTEGER REFERENCES prim(id),
    src_hier_ref_id    INTEGER REFERENCES hier_ref(id),
    tgt_hier_ref_id    INTEGER REFERENCES hier_ref(id),
    dep_kind           TEXT NOT NULL
        /*!*/CHECK(dep_kind IN ('data','control','primitive','procedure','alias'))/*!*/,
    src_lo             INTEGER,
    src_hi             INTEGER,
    src_exact          INTEGER CHECK(src_exact IN (0,1)),
    tgt_lo             INTEGER,
    tgt_hi             INTEGER,
    tgt_exact          INTEGER NOT NULL CHECK(tgt_exact IN (0,1)),
    map_exact          INTEGER CHECK(map_exact IN (0,1)));

-- One reference that leaves the instance: an XMR, an interface member, a
-- package item. The path is stored as written (normalised), AND -- new in
-- v10, because an occurrence knows its place in the hierarchy where a
-- folded row could not -- resolved to the target instance and net when
-- slang could resolve it and the target is in this export. NULL
-- resolved_* is "not resolved here", never a fabricated object.
--
-- access: read | write | connect (a port connection tied outward; its
-- net_conn row points back here).
CREATE TABLE hier_ref(
    id               INTEGER PRIMARY KEY,
    inst_id          INTEGER NOT NULL REFERENCES inst(id),
    stmt_id          INTEGER REFERENCES stmt(id),
    path             TEXT NOT NULL,
    access           TEXT NOT NULL /*!*/CHECK(access IN ('read','write','connect'))/*!*/,
    resolved_inst_id INTEGER REFERENCES inst(id),
    resolved_net_id  INTEGER REFERENCES net(id),
    lo               INTEGER,
    hi               INTEGER,
    is_exact         INTEGER NOT NULL CHECK(is_exact IN (0,1)),
    file_id          INTEGER REFERENCES file(id),
    line             INTEGER,
    col              INTEGER);
)SQL";

// UNIQUE constraints already index (stmt_id, ordinal) on the three statement
// child tables and (term_id, ordinal) on net_conn; term_map's primary key
// covers (term_id, ordinal). What is added here is the other direction of
// each relation and the point queries the views document: driver = by
// target, load = by source, provenance = by each reference id.
constexpr const char* kIndexes = R"SQL(
CREATE INDEX tree_node_by_parent    ON tree_node(parent_node_id, ordinal);
-- The documented access path: resolving `a.b[0].c` is one lookup per
-- segment against (parent, name). Without this the by-parent index seeks
-- the parent and then scans its children for the name -- invisible on a
-- deep tree, linear per segment on a flat one with thousands of siblings.
CREATE INDEX tree_node_by_name      ON tree_node(parent_node_id, name);
CREATE INDEX inst_by_parent         ON inst(parent_inst_id);
CREATE INDEX inst_by_module         ON inst(module_id);
CREATE INDEX prim_by_inst      ON prim(inst_id);
CREATE INDEX net_by_inst            ON net(inst_id, name);
CREATE INDEX net_by_scope           ON net(scope_node_id, name);
CREATE INDEX term_by_inst           ON term(inst_id, ordinal);
CREATE INDEX term_by_inst_name      ON term(inst_id, name);
CREATE INDEX term_map_by_net        ON term_map(inner_net_id);
CREATE INDEX net_conn_by_net        ON net_conn(outer_net_id);
-- The reverse of hier_ref's advertised back-pointer. Without it, "what does
-- this net drive across the boundary" is an indexed seek for a locally
-- named connection and a full net_conn scan for an outward tie -- the one
-- path the resolved-reference arcs actually take.
CREATE INDEX net_conn_by_href       ON net_conn(outer_hier_ref_id)
    WHERE outer_hier_ref_id IS NOT NULL;
CREATE INDEX proc_by_inst      ON proc(inst_id, ordinal);
CREATE INDEX stmt_by_inst           ON stmt(inst_id, ordinal);
CREATE INDEX stmt_by_proc      ON stmt(proc_id, sequence)
    WHERE proc_id IS NOT NULL;
CREATE INDEX assign_target_by_net   ON assign_target(net_id);
CREATE INDEX assign_operand_by_net  ON assign_operand(net_id);
CREATE INDEX expr_ref_by_net        ON expr_ref(net_id);
CREATE INDEX proc_event_by_proc ON proc_event(proc_id);
CREATE INDEX proc_event_by_net      ON proc_event(net_id)
    WHERE net_id IS NOT NULL;
CREATE INDEX net_dep_by_src      ON net_dep(src_net_id);
CREATE INDEX net_dep_by_tgt      ON net_dep(tgt_net_id);
CREATE INDEX net_dep_by_stmt        ON net_dep(stmt_id);
CREATE INDEX net_dep_by_operand     ON net_dep(assign_operand_id)
    WHERE assign_operand_id IS NOT NULL;
CREATE INDEX net_dep_by_tgt_ref  ON net_dep(assign_target_id);
CREATE INDEX net_dep_by_expr_ref    ON net_dep(expr_ref_id)
    WHERE expr_ref_id IS NOT NULL;
CREATE INDEX net_dep_by_primitive   ON net_dep(prim_id)
    WHERE prim_id IS NOT NULL;
CREATE INDEX net_dep_by_src_href ON net_dep(src_hier_ref_id)
    WHERE src_hier_ref_id IS NOT NULL;
CREATE INDEX net_dep_by_tgt_href ON net_dep(tgt_hier_ref_id)
    WHERE tgt_hier_ref_id IS NOT NULL;
CREATE INDEX hier_ref_by_inst       ON hier_ref(inst_id);
CREATE INDEX hier_ref_by_stmt       ON hier_ref(stmt_id);
CREATE INDEX hier_ref_by_net        ON hier_ref(resolved_net_id)
    WHERE resolved_net_id IS NOT NULL;
)SQL";

// The stable query interface: twelve views, the v10 consumption contract.
// Their existence, column sets, column semantics, NULL rules and row
// granularity are what a consumer may rely on; changing any of those bumps
// the schema version, changing only how one is computed does not.
// verify-designdb.py asserts all of it on every export.
//
// Ground rules, revised from v8 for the instance-level model:
//
//   * A FACT view's row is one base-table row -- v_tree_node, v_net,
//     v_term, v_term_map, v_net_conn, v_net_dep,
//     v_stmt, v_stmt_target, v_stmt_operand -- and the
//     verifier checks count(view) == count(base). Every internal join is
//     against a primary key, so nothing fans out.
//   * v_driver and v_load are COMPOSITE: UNION ALL branches discriminated by
//     driver_kind/load_kind, each branch's row count reconcilable against
//     its base tables by a formula the verifier evaluates. They are the one
//     place the hierarchy crossing is composed (net_conn against term_map);
//     v9 left that composition to the consumer, and every consumer wrote it
//     differently or wrongly.
//   * v_conn_arc is scaffolding for that composition, NOT part of the
//     contract: consumers must not query it, and it may change or vanish
//     without a version bump. It exists because the same composition feeds
//     four branches and inlining it four times would guarantee drift.
//   * Explicit column lists, never SELECT *. No transitive closure -- a
//     fan-in cone is the consumer's recursive query over net_dep, one step
//     per row here.
//   * Plain CREATE VIEW, not IF NOT EXISTS: the writer only ever creates a
//     fresh database, so a name collision is a bug to fail on.
//
// SQLite resolves a view's column references when the view is *queried*, not
// when it is created -- the verifier's row-count checks query every view,
// which is what makes a stale view a caught error rather than a consumer's
// surprise.
constexpr const char* kViews = R"SQL(
-- The meta table pivoted to one fixed row, so "which schema, which status" is
-- one SELECT with no key-value handling. Counts are CAST so a consumer gets
-- integers, not the TEXT the key-value table stores. The view only reshapes;
-- required-key enforcement stays in the verifier.
CREATE VIEW v_db_info AS
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

-- One row per tree_node. The subtype columns are NULL by node_kind, and that
-- is the contract: root/instance/unresolved have inst_id (and module_id
-- unless unresolved); primitive and generate have inst_id NULL. For a
-- primitive, parent_inst_id is the instance whose body wrote it and
-- def_name is the gate/UDP name; for an unresolved node,
-- def_name is the unresolvable spelling. Location is the
-- instantiation site; the root and generate levels have none.
CREATE VIEW v_tree_node AS
SELECT
    n.id                                   AS node_id,
    n.parent_node_id                       AS parent_node_id,
    n.name                                 AS node_name,
    n.node_kind                            AS node_kind,
    n.ordinal                              AS ordinal,
    i.id                                   AS inst_id,
    COALESCE(i.parent_inst_id, p.inst_id)  AS parent_inst_id,
    i.module_id                            AS module_id,
    m.name                                 AS module_name,
    i.param_signature                      AS param_signature,
    COALESCE(p.def_name, i.unresolved_def) AS def_name,
    f.path                                 AS file_path,
    sf.path                                AS src_path,
    COALESCE(i.line, p.line)               AS src_line,
    COALESCE(i.col, p.col)                 AS src_col
FROM tree_node n
LEFT JOIN inst i         ON i.id = n.id
LEFT JOIN prim p    ON p.id = n.id
LEFT JOIN module m       ON m.id = i.module_id
LEFT JOIN file f         ON f.id = COALESCE(i.file_id, p.file_id)
LEFT JOIN src_file sf ON sf.id = f.src_file_id;

-- One row per net: a connectable object of one concrete instance. No
-- direction column -- direction belongs to terminals, and a net's port-ness
-- is one v_term_map join away. file_path is the spelling as written in
-- the filelist, src_path the absolute path it resolved to; they answer
-- different questions and neither substitutes for the other.
CREATE VIEW v_net AS
SELECT
    nt.id             AS net_id,
    nt.inst_id        AS inst_id,
    i.module_id       AS module_id,
    m.name            AS module_name,
    i.param_signature AS param_signature,
    nt.scope_node_id  AS scope_node_id,
    nt.name           AS net_name,
    nt.decl_kind      AS decl_kind,
    dt.text           AS data_type,
    nt.width          AS width,
    nt.is_implicit    AS is_implicit,
    f.path            AS file_path,
    sf.path           AS src_path,
    nt.line           AS src_line,
    nt.col            AS src_col
FROM net nt
JOIN inst i              ON i.id = nt.inst_id
LEFT JOIN module m       ON m.id = i.module_id
LEFT JOIN data_type dt   ON dt.id = nt.data_type_id
LEFT JOIN file f         ON f.id = nt.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id;

-- One row per terminal. direction NULL is an interface terminal or a
-- terminal of an unresolved instance -- term_kind and the owning
-- instance's node_kind say which.
CREATE VIEW v_term AS
SELECT
    t.id        AS term_id,
    t.inst_id   AS inst_id,
    i.module_id AS module_id,
    m.name      AS module_name,
    t.name      AS term_name,
    t.term_kind AS term_kind,
    t.direction AS direction,
    dt.text     AS data_type,
    t.width     AS width,
    t.ordinal   AS ordinal,
    t.is_const  AS is_const,
    t.modport   AS modport,
    f.path      AS file_path,
    sf.path     AS src_path,
    t.line      AS src_line,
    t.col       AS src_col
FROM term t
JOIN inst i              ON i.id = t.inst_id
LEFT JOIN module m       ON m.id = i.module_id
LEFT JOIN data_type dt   ON dt.id = t.data_type_id
LEFT JOIN file f         ON f.id = t.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id;

-- One row per term_map segment: the inside of a terminal.
CREATE VIEW v_term_map AS
SELECT
    mp.term_id      AS term_id,
    t.inst_id       AS term_inst_id,
    t.name          AS term_name,
    mp.ordinal      AS map_ordinal,
    mp.inner_net_id AS inner_net_id,
    n.name          AS inner_net_name,
    mp.term_lo      AS term_lo,
    mp.term_hi      AS term_hi,
    mp.term_exact   AS term_exact,
    mp.inner_lo     AS inner_lo,
    mp.inner_hi     AS inner_hi,
    mp.inner_exact  AS inner_exact,
    mp.map_exact    AS map_exact
FROM term_map mp
JOIN term t ON t.id = mp.term_id
JOIN net n  ON n.id = mp.inner_net_id;

-- One row per net_conn segment: the outside of a terminal, exactly as
-- written in the parent. Deliberately NOT composed with term_map -- this
-- view is the fact, v_driver/v_load are the composition.
CREATE VIEW v_net_conn AS
SELECT
    c.id                 AS conn_id,
    c.outer_net_id       AS outer_net_id,
    pn.inst_id           AS outer_inst_id,
    pn.name              AS outer_net_name,
    c.term_id            AS term_id,
    t.inst_id            AS term_inst_id,
    t.name               AS term_name,
    t.direction          AS direction,
    c.conn_kind          AS conn_kind,
    c.ordinal            AS ordinal,
    c.outer_lo           AS outer_lo,
    c.outer_hi           AS outer_hi,
    c.outer_exact        AS outer_exact,
    c.term_lo            AS term_lo,
    c.term_hi            AS term_hi,
    c.term_exact         AS term_exact,
    c.map_exact          AS map_exact,
    c.outer_intf_inst_id AS outer_intf_inst_id,
    c.outer_hier_ref_id  AS outer_hier_ref_id,
    f.path               AS file_path,
    sf.path              AS src_path,
    c.line               AS src_line,
    c.col                AS src_col
FROM net_conn c
JOIN term t              ON t.id = c.term_id
LEFT JOIN net pn         ON pn.id = c.outer_net_id
LEFT JOIN file f         ON f.id = c.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id;

-- One row per net_dep: a statement- or primitive-level dependency
-- occurrence, not deduplicated across statements. Location is the
-- statement's, or the primitive's for a primitive arc.
CREATE VIEW v_net_dep AS
SELECT
    d.id                     AS dep_id,
    d.src_net_id             AS src_net_id,
    sn.inst_id               AS src_inst_id,
    sn.name                  AS src_name,
    d.src_lo                 AS src_lo,
    d.src_hi                 AS src_hi,
    d.src_exact              AS src_exact,
    d.tgt_net_id             AS tgt_net_id,
    tn.inst_id               AS tgt_inst_id,
    tn.name                  AS tgt_name,
    d.tgt_lo                 AS tgt_lo,
    d.tgt_hi                 AS tgt_hi,
    d.tgt_exact              AS tgt_exact,
    d.stmt_id                AS stmt_id,
    d.assign_operand_id      AS assign_operand_id,
    d.assign_target_id       AS assign_target_id,
    d.expr_ref_id            AS expr_ref_id,
    d.prim_id                AS prim_id,
    d.src_hier_ref_id        AS src_hier_ref_id,
    d.tgt_hier_ref_id        AS tgt_hier_ref_id,
    d.dep_kind               AS dep_kind,
    d.map_exact              AS map_exact,
    f.path                   AS file_path,
    sf.path                  AS src_path,
    COALESCE(s.line, p.line) AS src_line,
    COALESCE(s.col, p.col)   AS src_col
FROM net_dep d
JOIN net tn              ON tn.id = d.tgt_net_id
LEFT JOIN net sn         ON sn.id = d.src_net_id
LEFT JOIN stmt s         ON s.id = d.stmt_id
LEFT JOIN prim p    ON p.id = d.prim_id
LEFT JOIN file f         ON f.id = COALESCE(s.file_id, p.file_id)
LEFT JOIN src_file sf ON sf.id = f.src_file_id;

-- NOT part of the stable contract. The composition v_driver and v_load
-- project: one row per overlapping (net_conn segment, term_map segment)
-- pair -- a terminal's outside met with its inside. Windows are intersected
-- on the terminal; a side's net range is narrowed through the overlap when
-- every link of that side's chain is exact, passed through untouched when
-- the other side does not narrow it, and degraded to exact=0 otherwise --
-- the range is then an upper bound, which is what exact=0 means everywhere.
CREATE VIEW v_conn_arc AS
-- The outer end is either the connection's own net or, for a tie whose
-- reference resolved, the net that reference landed on. Deriving it with
-- COALESCE across the two tables read well and cost every consumer dearly:
-- a value that may come from either table can be pushed down to neither, so
-- `WHERE signal_net_id = ?` scanned all of net_conn instead of seeking
-- net_conn_by_net. On a design whose clock reaches a few thousand nets, a
-- trace walks one such query per hop.
--
-- The two cases are disjoint on `c.outer_net_id IS NULL` -- which is exactly what
-- COALESCE tests -- so splitting there leaves outer_net_id a plain column
-- reference in each branch and the indexes usable again. The second branch
-- keeps the LEFT JOIN: a constant tie-off has neither a net nor a
-- reference, and must still appear with a NULL outer end, since it drives
-- the inner net.
WITH seg AS (
    SELECT
        c.id                                   AS conn_id,
        c.conn_kind                            AS conn_kind,
        c.outer_net_id                         AS outer_net_id,
        c.outer_lo                             AS c_net_lo,
        c.outer_hi                             AS c_net_hi,
        c.outer_exact                          AS c_net_exact,
        c.term_lo                              AS c_lo,
        c.term_hi                              AS c_hi,
        c.term_exact                           AS c_term_exact,
        c.map_exact                            AS c_map,
        c.file_id                              AS file_id,
        c.line                                 AS line,
        c.col                                  AS col,
        t.id                                   AS term_id,
        t.inst_id                              AS term_inst_id,
        t.direction                            AS direction,
        mp.inner_net_id                        AS inner_net_id,
        mp.term_lo                             AS m_lo,
        mp.term_hi                             AS m_hi,
        mp.term_exact                          AS m_term_exact,
        mp.inner_lo                            AS m_net_lo,
        mp.inner_hi                            AS m_net_hi,
        mp.inner_exact                         AS m_net_exact,
        mp.map_exact                           AS m_map
    FROM net_conn c
    JOIN term t      ON t.id = c.term_id
    JOIN term_map mp ON mp.term_id = c.term_id
    LEFT JOIN hier_ref hr ON hr.id = c.outer_hier_ref_id
    WHERE c.conn_kind IN ('signal', 'expression_operand', 'constant',
                                'external_reference')
      AND (c.conn_kind != 'external_reference'
           OR hr.resolved_net_id IS NOT NULL)
      AND (c.conn_kind != 'expression_operand'
           OR c.outer_net_id IS NOT NULL OR hr.resolved_net_id IS NOT NULL)
      AND (c.term_lo IS NULL OR mp.term_hi IS NULL OR c.term_lo <= mp.term_hi)
      AND (mp.term_lo IS NULL OR c.term_hi IS NULL OR mp.term_lo <= c.term_hi)
      AND c.outer_net_id IS NOT NULL
    UNION ALL
    SELECT
        c.id                                   AS conn_id,
        c.conn_kind                            AS conn_kind,
        hr.resolved_net_id                     AS outer_net_id,
        hr.lo                                  AS c_net_lo,
        hr.hi                                  AS c_net_hi,
        hr.is_exact                            AS c_net_exact,
        c.term_lo                              AS c_lo,
        c.term_hi                              AS c_hi,
        c.term_exact                           AS c_term_exact,
        c.map_exact                            AS c_map,
        c.file_id                              AS file_id,
        c.line                                 AS line,
        c.col                                  AS col,
        t.id                                   AS term_id,
        t.inst_id                              AS term_inst_id,
        t.direction                            AS direction,
        mp.inner_net_id                        AS inner_net_id,
        mp.term_lo                             AS m_lo,
        mp.term_hi                             AS m_hi,
        mp.term_exact                          AS m_term_exact,
        mp.inner_lo                            AS m_net_lo,
        mp.inner_hi                            AS m_net_hi,
        mp.inner_exact                         AS m_net_exact,
        mp.map_exact                           AS m_map
    FROM net_conn c
    JOIN term t      ON t.id = c.term_id
    JOIN term_map mp ON mp.term_id = c.term_id
    LEFT JOIN hier_ref hr ON hr.id = c.outer_hier_ref_id
    WHERE c.conn_kind IN ('signal', 'expression_operand', 'constant',
                                'external_reference')
      AND (c.conn_kind != 'external_reference'
           OR hr.resolved_net_id IS NOT NULL)
      AND (c.conn_kind != 'expression_operand'
           OR c.outer_net_id IS NOT NULL OR hr.resolved_net_id IS NOT NULL)
      AND (c.term_lo IS NULL OR mp.term_hi IS NULL OR c.term_lo <= mp.term_hi)
      AND (mp.term_lo IS NULL OR c.term_hi IS NULL OR mp.term_lo <= c.term_hi)
      AND c.outer_net_id IS NULL
),
arc AS (
    SELECT seg.*,
        CASE WHEN c_lo IS NULL THEN m_lo
             WHEN m_lo IS NULL THEN c_lo
             ELSE MAX(c_lo, m_lo) END AS ilo,
        CASE WHEN c_hi IS NULL THEN m_hi
             WHEN m_hi IS NULL THEN c_hi
             ELSE MIN(c_hi, m_hi) END AS ihi,
        (COALESCE(c_map, 0) = 1 AND COALESCE(c_term_exact, 0) = 1
             AND m_term_exact = 1)    AS outer_chain,
        (m_map = 1 AND COALESCE(c_term_exact, 0) = 1
             AND m_term_exact = 1)    AS inner_chain,
        (m_lo IS NOT NULL OR m_hi IS NOT NULL) AS m_narrows,
        (c_lo IS NOT NULL OR c_hi IS NOT NULL) AS c_narrows
    FROM seg
)
SELECT
    conn_id, conn_kind, term_id, term_inst_id, direction,
    outer_net_id, inner_net_id,
    CASE WHEN outer_net_id IS NULL THEN NULL
         WHEN outer_chain AND c_net_exact = 1 AND ilo IS NOT NULL
              THEN COALESCE(c_net_lo, 0) + ilo - COALESCE(c_lo, 0)
         ELSE c_net_lo END          AS outer_lo,
    CASE WHEN outer_net_id IS NULL THEN NULL
         WHEN outer_chain AND c_net_exact = 1 AND ihi IS NOT NULL
              THEN COALESCE(c_net_lo, 0) + ihi - COALESCE(c_lo, 0)
         ELSE c_net_hi END          AS outer_hi,
    CASE WHEN outer_net_id IS NULL THEN NULL
         WHEN outer_chain OR NOT m_narrows THEN c_net_exact
         ELSE 0 END                 AS outer_exact,
    CASE WHEN inner_chain AND m_net_exact = 1 AND ilo IS NOT NULL
              THEN COALESCE(m_net_lo, 0) + ilo - COALESCE(m_lo, 0)
         ELSE m_net_lo END          AS inner_lo,
    CASE WHEN inner_chain AND m_net_exact = 1 AND ihi IS NOT NULL
              THEN COALESCE(m_net_lo, 0) + ihi - COALESCE(m_lo, 0)
         ELSE m_net_hi END          AS inner_hi,
    CASE WHEN inner_chain OR NOT c_narrows THEN m_net_exact
         ELSE 0 END                 AS inner_exact,
    CASE WHEN outer_net_id IS NULL THEN NULL
         WHEN conn_kind IN ('expression_operand', 'external_reference')
              THEN 0
         ELSE (c_map AND m_map) END AS map_exact,
    file_id, line, col
FROM arc;

-- Every direct driving arc of signal_net, one row each. Branches, told
-- apart by driver_kind:
--
--   data / control / primitive / procedure   a net_dep row, kind carried
--                       through; a data row with no source is 'constant'.
--   connection          the hierarchy crossing: for input/inout/ref, the
--                       parent-side net drives the child's internal net;
--                       for output/inout/ref, the internal net drives the
--                       parent's. inout and ref arc both ways, one row
--                       each. An external tie whose reference resolved
--                       crosses the same way, its far net as the outer
--                       end. Interface bindings do not arc.
--   connection_expression  the actual is an expression; each net it reads
--                       drives the internal net at range granularity.
--   constant            a tie-off or a constant RHS: driver_net_id NULL,
--                       and every driver_* column NULL with it.
--   alias               an alias statement binds the two nets into one
--                       object. It has no direction, so both nets appear
--                       as each other's driver and each other's load; the
--                       kind is what keeps it out of a multiple-driver
--                       count, where it would otherwise look like a second
--                       assignment.
--   system_task         a system task wrote the argument: $readmemh into a
--                       memory, $sscanf or $value$plusargs into a variable,
--                       $cast into its destination. The signal IS driven --
--                       reporting no driver said the design never wrote it
--                       -- but the source is a file or a plusarg, outside
--                       anything this schema names, so driver_net_id is
--                       NULL like a constant's and the kind keeps the two
--                       apart. stmt_id names the call.
--   terminal            the design boundary: a root input/inout/ref
--                       terminal drives the net it stands for. No driver
--                       net exists -- the world outside is the driver --
--                       and term_id names the pin, so "undriven" and
--                       "reaches the boundary" stay distinct answers.
--                       Every driver_* column is NULL, map_exact with
--                       them: there is no end to describe or correspond
--                       with, and the whole point of the null-source
--                       discipline is that a range beside a driver that
--                       does not exist is a claim about nothing. The
--                       terminal's own window is in v_term_map.
--
-- An unconnected terminal contributes no row.
CREATE VIEW v_driver AS
SELECT
    d.tgt_net_id             AS signal_net_id,
    tn.inst_id               AS signal_inst_id,
    tn.name                  AS signal_name,
    d.tgt_lo                 AS signal_lo,
    d.tgt_hi                 AS signal_hi,
    d.tgt_exact              AS signal_exact,
    d.src_net_id             AS driver_net_id,
    sn.inst_id               AS driver_inst_id,
    sn.name                  AS driver_name,
    d.src_lo                 AS driver_lo,
    d.src_hi                 AS driver_hi,
    d.src_exact              AS driver_exact,
    CASE WHEN d.src_net_id IS NOT NULL THEN d.dep_kind
         WHEN s.stmt_kind = 'system_task' THEN 'system_task'
         ELSE 'constant' END AS driver_kind,
    d.id                     AS dep_id,
    NULL                     AS conn_id,
    d.stmt_id                AS stmt_id,
    d.prim_id                AS prim_id,
    NULL                     AS term_id,
    d.map_exact              AS map_exact,
    f.path                   AS file_path,
    sf.path                  AS src_path,
    COALESCE(s.line, p.line) AS src_line,
    COALESCE(s.col, p.col)   AS src_col
FROM net_dep d
JOIN net tn              ON tn.id = d.tgt_net_id
LEFT JOIN net sn         ON sn.id = d.src_net_id
LEFT JOIN stmt s         ON s.id = d.stmt_id
LEFT JOIN prim p    ON p.id = d.prim_id
LEFT JOIN file f         ON f.id = COALESCE(s.file_id, p.file_id)
LEFT JOIN src_file sf ON sf.id = f.src_file_id
UNION ALL
SELECT
    a.inner_net_id, innet.inst_id, innet.name,
    a.inner_lo, a.inner_hi, a.inner_exact,
    a.outer_net_id, outnet.inst_id, outnet.name,
    a.outer_lo, a.outer_hi, a.outer_exact,
    CASE a.conn_kind
         WHEN 'expression_operand' THEN 'connection_expression'
         WHEN 'constant'           THEN 'constant'
         ELSE 'connection' END,
    NULL, a.conn_id, NULL, NULL, NULL,
    a.map_exact,
    f.path, sf.path, a.line, a.col
FROM v_conn_arc a
JOIN net innet           ON innet.id = a.inner_net_id
LEFT JOIN net outnet     ON outnet.id = a.outer_net_id
LEFT JOIN file f         ON f.id = a.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
WHERE a.direction IN ('input', 'inout', 'ref')
UNION ALL
SELECT
    a.outer_net_id, outnet.inst_id, outnet.name,
    a.outer_lo, a.outer_hi, a.outer_exact,
    a.inner_net_id, innet.inst_id, innet.name,
    a.inner_lo, a.inner_hi, a.inner_exact,
    'connection',
    NULL, a.conn_id, NULL, NULL, NULL,
    a.map_exact,
    f.path, sf.path, a.line, a.col
FROM v_conn_arc a
JOIN net innet           ON innet.id = a.inner_net_id
JOIN net outnet          ON outnet.id = a.outer_net_id
LEFT JOIN file f         ON f.id = a.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
WHERE a.direction IN ('output', 'inout', 'ref')
  AND a.conn_kind IN ('signal', 'external_reference')
UNION ALL
SELECT
    m.inner_net_id, n.inst_id, n.name,
    m.inner_lo, m.inner_hi, m.inner_exact,
    NULL, NULL, NULL, NULL, NULL, NULL,
    'terminal',
    NULL, NULL, NULL, NULL, t.id,
    NULL,
    f.path, sf.path, t.line, t.col
FROM term_map m
JOIN term t              ON t.id = m.term_id
JOIN tree_node r         ON r.id = t.inst_id AND r.node_kind = 'root'
JOIN net n               ON n.id = m.inner_net_id
LEFT JOIN file f         ON f.id = t.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
WHERE t.direction IN ('input', 'inout', 'ref');

-- Every recorded read of signal_net, one row each -- v9's generalised load
-- semantics carried to the instance level, plus the crossing. load_kind:
--
--   dataflow      a net_dep row: the signal feeds a target (data, control,
--                 primitive or procedure -- join net_dep on dep_id
--                 for which).
--   connection    the crossing reads it: a parent net feeding an
--                 input/inout/ref terminal, an internal net feeding an
--                 output/inout/ref one, or a resolved external tie either
--                 way. load_net is the far side.
--   sensitivity   a procedure triggers on it (proc_event, or an expr_ref
--                 with role='event' when the expression was not plain).
--   wait          a procedure suspends on it (proc_event, or role='wait').
--   statement     a statement reads it and writes nothing this instance
--                 names: an assertion, a $display, a call argument or
--                 branch condition whose statement has no local target.
--                 load_* are all NULL -- a real reader, no nameable target.
--   terminal      the design boundary: a root output/inout/ref terminal
--                 reads the net it stands for. load_* are NULL and
--                 term_id names the pin, so "unused" and "reaches the
--                 boundary" stay distinct answers.
--
-- One read, one row: an expr_ref or assign_operand that a net_dep row
-- already carries into 'dataflow' is not repeated as 'statement' -- the
-- NOT EXISTS guards are that rule, and the verifier re-derives the counts.
CREATE VIEW v_load AS
SELECT
    d.src_net_id             AS signal_net_id,
    sn.inst_id               AS signal_inst_id,
    sn.name                  AS signal_name,
    d.src_lo                 AS signal_lo,
    d.src_hi                 AS signal_hi,
    d.src_exact              AS signal_exact,
    d.tgt_net_id             AS load_net_id,
    tn.inst_id               AS load_inst_id,
    tn.name                  AS load_name,
    d.tgt_lo                 AS load_lo,
    d.tgt_hi                 AS load_hi,
    d.tgt_exact              AS load_exact,
    CASE WHEN d.dep_kind = 'alias' THEN 'alias'
         ELSE 'dataflow' END AS load_kind,
    d.id                     AS dep_id,
    NULL                     AS conn_id,
    d.stmt_id                AS stmt_id,
    s.proc_id                AS proc_id,
    NULL                     AS term_id,
    d.map_exact              AS map_exact,
    f.path                   AS file_path,
    sf.path                  AS src_path,
    COALESCE(s.line, p.line) AS src_line,
    COALESCE(s.col, p.col)   AS src_col
FROM net_dep d
JOIN net sn              ON sn.id = d.src_net_id
JOIN net tn              ON tn.id = d.tgt_net_id
LEFT JOIN stmt s         ON s.id = d.stmt_id
LEFT JOIN prim p    ON p.id = d.prim_id
LEFT JOIN file f         ON f.id = COALESCE(s.file_id, p.file_id)
LEFT JOIN src_file sf ON sf.id = f.src_file_id
UNION ALL
SELECT
    a.outer_net_id, outnet.inst_id, outnet.name,
    a.outer_lo, a.outer_hi, a.outer_exact,
    a.inner_net_id, innet.inst_id, innet.name,
    a.inner_lo, a.inner_hi, a.inner_exact,
    'connection',
    NULL, a.conn_id, NULL, NULL, NULL,
    a.map_exact,
    f.path, sf.path, a.line, a.col
FROM v_conn_arc a
JOIN net outnet          ON outnet.id = a.outer_net_id
JOIN net innet           ON innet.id = a.inner_net_id
LEFT JOIN file f         ON f.id = a.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
WHERE a.direction IN ('input', 'inout', 'ref')
UNION ALL
SELECT
    a.inner_net_id, innet.inst_id, innet.name,
    a.inner_lo, a.inner_hi, a.inner_exact,
    a.outer_net_id, outnet.inst_id, outnet.name,
    a.outer_lo, a.outer_hi, a.outer_exact,
    'connection',
    NULL, a.conn_id, NULL, NULL, NULL,
    a.map_exact,
    f.path, sf.path, a.line, a.col
FROM v_conn_arc a
JOIN net innet           ON innet.id = a.inner_net_id
JOIN net outnet          ON outnet.id = a.outer_net_id
LEFT JOIN file f         ON f.id = a.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
WHERE a.direction IN ('output', 'inout', 'ref')
  AND a.conn_kind IN ('signal', 'external_reference')
UNION ALL
SELECT
    pe.net_id, n.inst_id, n.name,
    NULL, NULL, 1,
    NULL, NULL, NULL, NULL, NULL, NULL,
    pe.event_kind,
    NULL, NULL, pe.stmt_id, pe.proc_id, NULL, NULL,
    f.path, sf.path, pe.line, pe.col
FROM proc_event pe
JOIN net n               ON n.id = pe.net_id
LEFT JOIN file f         ON f.id = pe.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
UNION ALL
SELECT
    e.net_id, n.inst_id, n.name,
    e.lo, e.hi, e.is_exact,
    NULL, NULL, NULL, NULL, NULL, NULL,
    CASE e.role WHEN 'wait' THEN 'wait'
                WHEN 'event' THEN 'sensitivity'
                ELSE 'statement' END,
    NULL, NULL, e.stmt_id, s.proc_id, NULL, NULL,
    f.path, sf.path, s.line, s.col
FROM expr_ref e
JOIN net n               ON n.id = e.net_id
JOIN stmt s              ON s.id = e.stmt_id
LEFT JOIN file f         ON f.id = s.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
WHERE e.role IN ('assertion', 'wait', 'event', 'system_task')
   OR NOT EXISTS (SELECT 1 FROM net_dep d WHERE d.expr_ref_id = e.id)
UNION ALL
SELECT
    o.net_id, n.inst_id, n.name,
    o.lo, o.hi, o.is_exact,
    NULL, NULL, NULL, NULL, NULL, NULL,
    'statement',
    NULL, NULL, o.stmt_id, s.proc_id, NULL, NULL,
    f.path, sf.path, s.line, s.col
FROM assign_operand o
JOIN net n               ON n.id = o.net_id
JOIN stmt s              ON s.id = o.stmt_id
LEFT JOIN file f         ON f.id = s.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
WHERE NOT EXISTS (SELECT 1 FROM net_dep d WHERE d.assign_operand_id = o.id)
UNION ALL
SELECT
    m.inner_net_id, n.inst_id, n.name,
    m.inner_lo, m.inner_hi, m.inner_exact,
    NULL, NULL, NULL, NULL, NULL, NULL,
    'terminal',
    NULL, NULL, NULL, NULL, t.id, NULL,
    f.path, sf.path, t.line, t.col
FROM term_map m
JOIN term t              ON t.id = m.term_id
JOIN tree_node r         ON r.id = t.inst_id AND r.node_kind = 'root'
JOIN net n               ON n.id = m.inner_net_id
LEFT JOIN file f         ON f.id = t.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
WHERE t.direction IN ('output', 'inout', 'ref');

-- One row per stmt: the statement object. Targets and operands are
-- v_stmt_target / v_stmt_operand rows keyed on stmt_id --
-- one statement is ONE row here no matter how many targets it writes.
CREATE VIEW v_stmt AS
SELECT
    s.id                    AS stmt_id,
    s.inst_id               AS inst_id,
    i.module_id             AS module_id,
    m.name                  AS module_name,
    s.scope_node_id         AS scope_node_id,
    s.proc_id               AS proc_id,
    s.ordinal               AS ordinal,
    s.sequence              AS sequence,
    s.stmt_kind             AS stmt_kind,
    s.construct             AS construct,
    s.assign_kind           AS assign_kind,
    s.delay                 AS delay,
    s.dropped_operand_count AS dropped_operand_count,
    f.path                  AS file_path,
    sf.path                 AS src_path,
    s.line                  AS src_line,
    s.col                   AS src_col
FROM stmt s
JOIN inst i              ON i.id = s.inst_id
LEFT JOIN module m       ON m.id = i.module_id
LEFT JOIN file f         ON f.id = s.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id;

-- One row per assign_target.
CREATE VIEW v_stmt_target AS
SELECT
    a.id       AS target_id,
    a.stmt_id  AS stmt_id,
    a.ordinal  AS ordinal,
    a.net_id   AS net_id,
    n.name     AS net_name,
    a.lo       AS tgt_lo,
    a.hi       AS tgt_hi,
    a.is_exact AS tgt_exact
FROM assign_target a
JOIN net n ON n.id = a.net_id;

-- One row per assign_operand.
CREATE VIEW v_stmt_operand AS
SELECT
    o.id       AS operand_id,
    o.stmt_id  AS stmt_id,
    o.ordinal  AS ordinal,
    o.net_id   AS net_id,
    n.name     AS net_name,
    o.lo       AS operand_lo,
    o.hi       AS operand_hi,
    o.is_exact AS operand_exact
FROM assign_operand o
JOIN net n ON n.id = o.net_id;
)SQL";

// Rows per transaction. Committing per row is orders of magnitude slower;
// never committing means the whole export is one transaction whose rollback
// journal grows without bound.
constexpr int64_t kBatch = 20000;

/// Text is bound with SQLITE_STATIC, not SQLITE_TRANSIENT.
///
/// TRANSIENT asks SQLite to copy, which is a malloc, a strlen and a memcpy
/// for every string of every row -- and these tables are mostly text
/// columns repeated millions of times. STATIC only requires the buffer to
/// outlive the step, and every caller here binds a member of a row struct
/// held by const reference across the bind/step pair, so it does. The
/// interning and provenance writers keep TRANSIENT: they run once per
/// distinct string, not once per row, and some are handed a view whose
/// owner they cannot see.
void bindOptText(sqlite3_stmt* s, int i, const std::string& v) {
    if (v.empty())
        sqlite3_bind_null(s, i);
    else
        sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()),
                          SQLITE_STATIC);
}

void bindOptId(sqlite3_stmt* s, int i, int64_t id) {
    if (id)
        sqlite3_bind_int64(s, i, id);
    else
        sqlite3_bind_null(s, i);
}

/// -1 spells NULL for the tri-state 0/1 columns (a side that does not exist).
void bindTri(sqlite3_stmt* s, int i, int v) {
    if (v < 0)
        sqlite3_bind_null(s, i);
    else
        sqlite3_bind_int(s, i, v ? 1 : 0);
}

/// A location is three columns bound together: no file, no line, no column.
/// Rows that genuinely have no source (the root instance) carry NULLs, not
/// a 0 a consumer would have to know to exclude.
void bindLoc(sqlite3_stmt* s, int i, int64_t fileId, uint32_t line,
             uint32_t column) {
    if (fileId) {
        sqlite3_bind_int64(s, i, fileId);
        sqlite3_bind_int64(s, i + 1, line);
        sqlite3_bind_int64(s, i + 2, column);
    }
    else {
        sqlite3_bind_null(s, i);
        sqlite3_bind_null(s, i + 1);
        sqlite3_bind_null(s, i + 2);
    }
}

/// lo, hi and exact bound as one range with the bool-exact discipline.
void bindRange(sqlite3_stmt* s, int lo,
               const std::optional<std::pair<uint64_t, uint64_t>>& r,
               bool exact) {
    if (!r) {
        sqlite3_bind_null(s, lo);
        sqlite3_bind_null(s, lo + 1);
    }
    else {
        sqlite3_bind_int64(s, lo, static_cast<int64_t>(r->first));
        sqlite3_bind_int64(s, lo + 1, static_cast<int64_t>(r->second));
    }
    sqlite3_bind_int(s, lo + 2, exact ? 1 : 0);
}

/// The tri-state variant: exact < 0 means the whole end does not exist, so
/// all three columns are NULL regardless of what the range says.
void bindRangeTri(sqlite3_stmt* s, int lo,
                  const std::optional<std::pair<uint64_t, uint64_t>>& r,
                  int exact) {
    if (exact < 0) {
        sqlite3_bind_null(s, lo);
        sqlite3_bind_null(s, lo + 1);
        sqlite3_bind_null(s, lo + 2);
        return;
    }
    bindRange(s, lo, r, exact != 0);
}

void bindOptWidth(sqlite3_stmt* s, int i, int64_t width) {
    if (width >= 0)
        sqlite3_bind_int64(s, i, width);
    else
        sqlite3_bind_null(s, i);
}

} // namespace

Writer::Writer(const std::string& path, bool checkConstraints) {
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
        // The database is a build artifact: if the process dies it is rebuilt
        // from source, so paying for durability buys nothing and costs a large
        // fraction of the write time.
        //
        // The cache default is 2 MiB, which on a large export meant evicting
        // and re-reading pages continuously while writing, and again while
        // sorting each index. Sorting is also what temp_store decides;
        // leaving it on disk wrote every index's content twice. Together
        // these take about a third off the index build.
        //
        // page_size is deliberately left alone. 16 KiB pages are ~1% faster
        // on a large design and no smaller, while costing 3.6x on a small
        // one -- a minimum-size page is a poor trade for a schema whose
        // usual database has a few thousand rows.
        exec("PRAGMA journal_mode=OFF");
        exec("PRAGMA synchronous=OFF");
        exec("PRAGMA cache_size=-262144");   // 256 MiB, not pages
        exec("PRAGMA temp_store=MEMORY");
        exec("PRAGMA locking_mode=EXCLUSIVE");
        // The enum CHECK clauses are marked in kSchema so they can be left
        // out. They are the single most expensive thing in the write path
        // -- SQLite evaluates a string IN-list for every row -- and the
        // verifier re-derives the same domains from the finished file, so
        // the default is to spend that time on the export instead.
        if (checkConstraints) {
            std::string ddl(kSchema);
            size_t at = 0;
            while ((at = ddl.find("/*!*/", at)) != std::string::npos)
                ddl.erase(at, 5);
            exec(ddl.c_str());
        }
        else {
            std::string ddl(kSchema);
            size_t open = 0;
            while ((open = ddl.find("/*!*/", open)) != std::string::npos) {
                size_t close = ddl.find("/*!*/", open + 5);
                if (close == std::string::npos)
                    break;
                ddl.erase(open, close + 5 - open);
            }
            exec(ddl.c_str());
        }

        prepare("INSERT INTO module VALUES(?,?,?,?,?,?)", &ins[InsModule]);
        prepare("INSERT INTO tree_node VALUES(?,?,?,?,?)", &ins[InsTreeNode]);
        prepare("INSERT INTO inst VALUES(?,?,?,?,?,?,?,?)", &ins[InsInst]);
        prepare("INSERT INTO prim VALUES(?,?,?,?,?,?,?)", &ins[InsPrimitive]);
        prepare("INSERT INTO net VALUES(?,?,?,?,?,?,?,?,?,?,?)", &ins[InsNet]);
        prepare("INSERT INTO term VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)", &ins[InsTerm]);
        prepare("INSERT INTO term_map VALUES(?,?,?,?,?,?,?,?,?,?)", &ins[InsTermMap]);
        prepare("INSERT INTO net_conn VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                &ins[InsNetConn]);
        prepare("INSERT INTO proc VALUES(?,?,?,?,?,?,?,?,?)", &ins[InsProcedure]);
        prepare("INSERT INTO stmt VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &ins[InsStmt]);
        prepare("INSERT INTO assign_target VALUES(?,?,?,?,?,?,?)",
                &ins[InsAssignTarget]);
        prepare("INSERT INTO assign_operand VALUES(?,?,?,?,?,?,?)",
                &ins[InsAssignOperand]);
        prepare("INSERT INTO expr_ref VALUES(?,?,?,?,?,?,?,?)", &ins[InsExprRef]);
        prepare("INSERT INTO proc_event VALUES(?,?,?,?,?,?,?,?,?)", &ins[InsProcEvent]);
        prepare("INSERT INTO net_dep VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                &ins[InsNetDep]);
        prepare("INSERT INTO hier_ref VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                &ins[InsHierRef]);
        begin();
    }
    catch (...) {
        for (auto* s : ins)
            sqlite3_finalize(s);
        sqlite3_close(db);
        db = nullptr;
        throw;
    }
}

Writer::~Writer() {
    if (db) {
        for (auto* s : ins)
            sqlite3_finalize(s);
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

void Writer::bumped() {
    if (++pending >= kBatch) {
        commit();
        begin();
    }
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
    prepare("INSERT OR IGNORE INTO src_file(path,digest) VALUES(?,?)", &s);
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
    // by hand, and any src_file row inserted without going through
    // addSourceFile would have broken the join silently. A row with no match
    // -- a synthesized buffer, which is not a file and was never hashed --
    // yields NULL, which is what "no origin" already means in this column.
    sqlite3_stmt* s = nullptr;
    prepare("UPDATE file SET src_file_id="
            "(SELECT id FROM src_file WHERE path = ?1) WHERE path = ?2", &s);
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

namespace {
int64_t internInto(sqlite3* db, std::unordered_map<std::string, int64_t>& cache,
                   const char* table, const char* column, const std::string& text) {
    if (text.empty())
        return 0;
    if (auto it = cache.find(text); it != cache.end())
        return it->second;
    std::string sql = std::string("INSERT INTO ") + table + "(" + column + ") VALUES(?)";
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db));
    // Bound with an explicit length: the -1 form stops at the first NUL, so a
    // string carrying one would be stored truncated and alias another entry.
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

int64_t Writer::internDataType(const std::string& text) {
    return internInto(db, dataTypeIds, "data_type", "text", text);
}

int64_t Writer::internFile(const std::string& path) {
    return internInto(db, fileIds, "file", "path", path);
}

void Writer::addModule(const ModuleRow& r) {
    auto* s = ins[InsModule];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.id);
    sqlite3_bind_text(s, 2, r.name.c_str(), static_cast<int>(r.name.size()),
                      SQLITE_STATIC);
    sqlite3_bind_text(s, 3, r.definitionKind.c_str(), static_cast<int>(r.definitionKind.size()),
                      SQLITE_STATIC);
    bindLoc(s, 4, r.fileId, r.line, r.column);
    step(s);
    bumped();
}

void Writer::addTreeNode(const TreeNodeRow& r) {
    auto* s = ins[InsTreeNode];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.id);
    bindOptId(s, 2, r.parentNodeId);
    sqlite3_bind_text(s, 3, r.name.data(), static_cast<int>(r.name.size()),
                      SQLITE_STATIC);
    sqlite3_bind_text(s, 4, r.nodeKind.c_str(), static_cast<int>(r.nodeKind.size()),
                      SQLITE_STATIC);
    sqlite3_bind_int64(s, 5, r.ordinal);
    step(s);
    bumped();
}

void Writer::addInst(const InstRow& r) {
    auto* s = ins[InsInst];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.id);
    bindOptId(s, 2, r.moduleId);
    bindOptId(s, 3, r.parentInstId);
    bindOptText(s, 4, r.parameterSignature);
    bindOptText(s, 5, r.unresolvedDefinition);
    bindLoc(s, 6, r.fileId, r.line, r.column);
    step(s);
    bumped();
}

void Writer::addPrimitive(const PrimitiveRow& r) {
    auto* s = ins[InsPrimitive];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.id);
    sqlite3_bind_int64(s, 2, r.instId);
    sqlite3_bind_text(s, 3, r.primitiveKind.c_str(), static_cast<int>(r.primitiveKind.size()),
                      SQLITE_STATIC);
    sqlite3_bind_text(s, 4, r.definitionName.c_str(), static_cast<int>(r.definitionName.size()),
                      SQLITE_STATIC);
    bindLoc(s, 5, r.fileId, r.line, r.column);
    step(s);
    bumped();
}

void Writer::addNet(const NetRow& r) {
    auto* s = ins[InsNet];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.id);
    sqlite3_bind_int64(s, 2, r.instId);
    sqlite3_bind_int64(s, 3, r.scopeNodeId);
    sqlite3_bind_text(s, 4, r.name.data(), static_cast<int>(r.name.size()),
                      SQLITE_STATIC);
    sqlite3_bind_text(s, 5, r.declarationKind.c_str(), static_cast<int>(r.declarationKind.size()),
                      SQLITE_STATIC);
    bindOptId(s, 6, r.dataTypeId);
    bindOptWidth(s, 7, r.width);
    sqlite3_bind_int(s, 8, r.isImplicit ? 1 : 0);
    bindLoc(s, 9, r.fileId, r.line, r.column);
    step(s);
    bumped();
}

void Writer::addTerm(const TermRow& r) {
    auto* s = ins[InsTerm];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.id);
    sqlite3_bind_int64(s, 2, r.instId);
    sqlite3_bind_text(s, 3, r.name.data(), static_cast<int>(r.name.size()),
                      SQLITE_STATIC);
    sqlite3_bind_text(s, 4, r.terminalKind.c_str(), static_cast<int>(r.terminalKind.size()),
                      SQLITE_STATIC);
    bindOptText(s, 5, r.direction);
    bindOptId(s, 6, r.dataTypeId);
    bindOptWidth(s, 7, r.width);
    sqlite3_bind_int64(s, 8, r.ordinal);
    bindTri(s, 9, r.isConst);
    bindOptText(s, 10, r.modport);
    bindLoc(s, 11, r.fileId, r.line, r.column);
    step(s);
    bumped();
}

void Writer::addTermMap(const TermMapRow& r) {
    auto* s = ins[InsTermMap];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.termId);
    sqlite3_bind_int64(s, 2, r.ordinal);
    sqlite3_bind_int64(s, 3, r.netId);
    bindRange(s, 4, r.termBits, r.termExact);
    bindRange(s, 7, r.netBits, r.netExact);
    sqlite3_bind_int(s, 10, r.mappingExact ? 1 : 0);
    step(s);
    bumped();
}

void Writer::addNetConn(const NetConnRow& r) {
    auto* s = ins[InsNetConn];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.id);
    bindOptId(s, 2, r.netId);
    sqlite3_bind_int64(s, 3, r.termId);
    sqlite3_bind_int64(s, 4, r.ordinal);
    sqlite3_bind_text(s, 5, r.connectionKind.c_str(), static_cast<int>(r.connectionKind.size()),
                      SQLITE_STATIC);
    // A row with no net end has no net range and nothing to correspond with
    // -- the NULL discipline enforced at the one chokepoint every emitter
    // goes through, so a tie-off can never read as "the whole of nothing,
    // exactly".
    if (r.netId == 0) {
        bindRangeTri(s, 6, std::nullopt, -1);
        bindRangeTri(s, 9, r.termBits, r.termExact);
        sqlite3_bind_null(s, 12);
    }
    else {
        bindRangeTri(s, 6, r.netBits, r.netExact);
        bindRangeTri(s, 9, r.termBits, r.termExact);
        bindTri(s, 12, r.mappingExact);
    }
    bindOptId(s, 13, r.interfaceInstId);
    bindOptId(s, 14, r.hierRefId);
    bindLoc(s, 15, r.fileId, r.line, r.column);
    step(s);
    bumped();
}

void Writer::addProcedure(const ProcedureRow& r) {
    auto* s = ins[InsProcedure];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.id);
    sqlite3_bind_int64(s, 2, r.instId);
    sqlite3_bind_int64(s, 3, r.scopeNodeId);
    bindOptText(s, 4, r.name);
    sqlite3_bind_text(s, 5, r.procedureKind.c_str(), static_cast<int>(r.procedureKind.size()),
                      SQLITE_STATIC);
    sqlite3_bind_int64(s, 6, r.ordinal);
    bindLoc(s, 7, r.fileId, r.line, r.column);
    step(s);
    bumped();
}

void Writer::addStmt(const StmtRow& r) {
    auto* s = ins[InsStmt];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.id);
    sqlite3_bind_int64(s, 2, r.instId);
    sqlite3_bind_int64(s, 3, r.scopeNodeId);
    bindOptId(s, 4, r.procedureId);
    sqlite3_bind_int64(s, 5, r.ordinal);
    // NULL exactly when the statement is in no procedure: a continuous
    // assign has no execution order among statements that all run always.
    if (r.procedureId == 0 || r.sequence < 0)
        sqlite3_bind_null(s, 6);
    else
        sqlite3_bind_int64(s, 6, r.sequence);
    sqlite3_bind_text(s, 7, r.statementKind.c_str(), static_cast<int>(r.statementKind.size()),
                      SQLITE_STATIC);
    bindOptText(s, 8, r.construct);
    bindOptText(s, 9, r.assignmentKind);
    bindOptText(s, 10, r.delay);
    sqlite3_bind_int64(s, 11, r.droppedOperandCount);
    bindLoc(s, 12, r.fileId, r.line, r.column);
    step(s);
    bumped();
}

void Writer::addAssignTarget(const AssignTargetRow& r) {
    auto* s = ins[InsAssignTarget];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.id);
    sqlite3_bind_int64(s, 2, r.stmtId);
    sqlite3_bind_int64(s, 3, r.ordinal);
    sqlite3_bind_int64(s, 4, r.netId);
    bindRange(s, 5, r.bits, r.exact);
    step(s);
    bumped();
}

void Writer::addAssignOperand(const AssignOperandRow& r) {
    auto* s = ins[InsAssignOperand];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.id);
    sqlite3_bind_int64(s, 2, r.stmtId);
    sqlite3_bind_int64(s, 3, r.ordinal);
    sqlite3_bind_int64(s, 4, r.netId);
    bindRange(s, 5, r.bits, r.exact);
    step(s);
    bumped();
}

void Writer::addExprRef(const ExprRefRow& r) {
    auto* s = ins[InsExprRef];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.id);
    sqlite3_bind_int64(s, 2, r.stmtId);
    sqlite3_bind_int64(s, 3, r.ordinal);
    sqlite3_bind_int64(s, 4, r.netId);
    sqlite3_bind_text(s, 5, r.role.c_str(), static_cast<int>(r.role.size()),
                      SQLITE_STATIC);
    bindRange(s, 6, r.bits, r.exact);
    step(s);
    bumped();
}

void Writer::addProcEvent(const ProcEventRow& r) {
    auto* s = ins[InsProcEvent];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.id);
    sqlite3_bind_int64(s, 2, r.procedureId);
    bindOptId(s, 3, r.stmtId);
    bindOptId(s, 4, r.netId);
    sqlite3_bind_text(s, 5, r.eventKind.c_str(), static_cast<int>(r.eventKind.size()),
                      SQLITE_STATIC);
    bindOptText(s, 6, r.edgeKind);
    bindLoc(s, 7, r.fileId, r.line, r.column);
    step(s);
    bumped();
}

void Writer::addNetDep(const NetDepRow& r) {
    auto* s = ins[InsNetDep];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.id);
    bindOptId(s, 2, r.sourceNetId);
    sqlite3_bind_int64(s, 3, r.targetNetId);
    bindOptId(s, 4, r.stmtId);
    bindOptId(s, 5, r.assignOperandId);
    bindOptId(s, 6, r.assignTargetId);
    bindOptId(s, 7, r.exprRefId);
    bindOptId(s, 8, r.primitiveId);
    bindOptId(s, 9, r.sourceHierRefId);
    bindOptId(s, 10, r.targetHierRefId);
    sqlite3_bind_text(s, 11, r.dependencyKind.c_str(), static_cast<int>(r.dependencyKind.size()),
                      SQLITE_STATIC);
    // A row with no source has no source end to describe, so every column
    // describing one is NULL -- enforced here rather than in every emitter.
    // Before this discipline, `assign q = 1'b0` carried src_exact=1 and
    // map_exact=1: a per-bit mapping onto a driver that does not exist.
    if (r.sourceNetId == 0) {
        bindRangeTri(s, 12, std::nullopt, -1);
        bindRange(s, 15, r.targetBits, r.targetExact);
        sqlite3_bind_null(s, 18);
    }
    else {
        bindRangeTri(s, 12, r.sourceBits, r.sourceExact);
        bindRange(s, 15, r.targetBits, r.targetExact);
        bindTri(s, 18, r.mappingExact);
    }
    step(s);
    bumped();
}

void Writer::addHierRef(const HierRefRow& r) {
    auto* s = ins[InsHierRef];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.id);
    sqlite3_bind_int64(s, 2, r.instId);
    bindOptId(s, 3, r.stmtId);
    sqlite3_bind_text(s, 4, r.path.data(), static_cast<int>(r.path.size()),
                      SQLITE_STATIC);
    sqlite3_bind_text(s, 5, r.access.c_str(), static_cast<int>(r.access.size()),
                      SQLITE_STATIC);
    bindOptId(s, 6, r.resolvedInstId);
    bindOptId(s, 7, r.resolvedNetId);
    bindRange(s, 8, r.bits, r.exact);
    bindLoc(s, 11, r.fileId, r.line, r.column);
    step(s);
    bumped();
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
