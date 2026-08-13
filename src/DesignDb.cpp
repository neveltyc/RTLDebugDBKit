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
CREATE TABLE file(id INTEGER PRIMARY KEY, path TEXT UNIQUE);

-- Intra-module dataflow, in the module's own namespace.
CREATE TABLE edge(
    module   INTEGER NOT NULL REFERENCES module(id),
    -- Null when the right-hand side reads nothing at all: `q <= 8'h0`.
    -- The row still names the statement, which is what a driver query reports.
    src      INTEGER REFERENCES name(id),
    dst      INTEGER NOT NULL REFERENCES name(id),
    src_type INTEGER REFERENCES type(id),
    dst_type INTEGER REFERENCES type(id),
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
    dst_lo    INTEGER, dst_hi INTEGER, dst_exact INTEGER);

-- What a module instantiates. Expanding `instance` against this is what turns
-- the folded model back into hierarchy.
CREATE TABLE child(
    module       INTEGER NOT NULL REFERENCES module(id),
    name       INTEGER NOT NULL REFERENCES name(id),
    def_name   INTEGER REFERENCES name(id),      -- what it instantiates, as text
    def_module INTEGER REFERENCES module(id));   -- and as a module row

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
    kind      TEXT,                       -- variable | net | port | parameter
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
    child        INTEGER NOT NULL REFERENCES name(id),
    def_module   INTEGER REFERENCES module(id),
    port         INTEGER NOT NULL REFERENCES name(id),
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
    -- expression. 3 is not a net: `.en(state == RUN)` samples `state` but does
    -- not alias it to `en`, and recording it as 0 made every reader of `en`
    -- count as a reader of `state`. An unconnected port is recorded rather
    -- than omitted: absence would otherwise mean both "nobody connected it"
    -- and "the exporter did not get that far".
    conn_kind    INTEGER,
    file         INTEGER REFERENCES file(id),
    line         INTEGER);

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
    kind      TEXT,
    construct TEXT,
    file     INTEGER REFERENCES file(id),
    line     INTEGER,
    -- Which procedure in the module, and the order within it. Both are needed:
    -- `seq` restarts per procedure, so two assignments to one target from
    -- different `always` blocks can carry the same seq and would otherwise read
    -- as one ordered sequence.
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
    dropped_operands INTEGER);

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
CREATE TABLE proc_event(
    module INTEGER NOT NULL REFERENCES module(id),
    proc   INTEGER NOT NULL,
    signal INTEGER REFERENCES name(id),   -- NULL when not a plain reference
    edge_kind TEXT);                      -- posedge | negedge | both

CREATE TABLE assign_operand(
    assignment INTEGER NOT NULL REFERENCES assignment(id),
    name       INTEGER NOT NULL REFERENCES name(id),
    src_lo     INTEGER, src_hi INTEGER, src_exact INTEGER);

-- The instance tree. This is the one table that scales with the design rather
-- than with the source, which is why it carries nothing but identity.
CREATE TABLE instance(
    id      INTEGER PRIMARY KEY,
    name    INTEGER NOT NULL REFERENCES name(id),
    module  INTEGER NOT NULL REFERENCES module(id),
    parent  INTEGER REFERENCES instance(id));
)SQL";

constexpr const char* kIndexes = R"SQL(
CREATE INDEX edge_by_dst      ON edge(module, dst);
CREATE INDEX edge_by_src      ON edge(module, src);
CREATE INDEX child_by_module  ON child(module);
CREATE INDEX symbol_by_module ON symbol(module);
CREATE INDEX assign_by_dst    ON assignment(module, dst);
CREATE INDEX pevent_by_proc   ON proc_event(module, proc);
CREATE INDEX aop_by_assign    ON assign_operand(assignment);
CREATE INDEX symbol_by_name   ON symbol(name);
-- Both directions: outward from a net in the parent, and inward from a formal
-- in the child. A driver query needs the first, a load query the second.
CREATE INDEX port_by_outer    ON port(module, outer);
CREATE INDEX port_by_formal   ON port(def_module, port);
-- The descent index: resolving a hierarchical path means one lookup per
-- segment against this. Not unique -- a design that only partially elaborates
-- can produce two siblings with the same name, and refusing the second would
-- abort an export that is otherwise perfectly usable. The count is reported.
CREATE INDEX instance_by_parent ON instance(parent, name);
CREATE INDEX instance_by_module ON instance(module);
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

    prepare("INSERT INTO edge VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &insEdge);
    prepare("INSERT INTO child VALUES(?,?,?,?)", &insChild);
    prepare("INSERT INTO instance VALUES(?,?,?,?)", &insInstance);
    prepare("INSERT INTO port VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &insPort);
        prepare("INSERT INTO symbol VALUES(?,?,?,?,?,?,?,?,?)", &insSymbol);
        prepare("INSERT INTO assignment VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &insAssign);
        prepare("INSERT INTO proc_event VALUES(?,?,?,?)", &insProcEvent);
        prepare("INSERT INTO assign_operand VALUES(?,?,?,?,?)", &insAssignOp);
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
        bindOptRange(insEdge, 11, 12, r.srcBits);
        sqlite3_bind_int(insEdge, 13, r.srcExact ? 1 : 0);
        bindOptRange(insEdge, 14, 15, r.dstBits);
        sqlite3_bind_int(insEdge, 16, r.dstExact ? 1 : 0);
        step(insEdge);
        if (++pending >= kBatch) {
            commit();
            begin();
        }
    }
}

void Writer::addChildren(int64_t moduleId, const std::vector<ChildRow>& rows) {
    for (auto& r : rows) {
        sqlite3_reset(insChild);
        sqlite3_bind_int64(insChild, 1, moduleId);
        sqlite3_bind_int64(insChild, 2, internName(r.name));
        bindOptId(insChild, 3, internName(r.defName));
        if (r.defModule)
            sqlite3_bind_int64(insChild, 4, r.defModule);
        else
            sqlite3_bind_null(insChild, 4);
        step(insChild);
        if (++pending >= kBatch) {
            commit();
            begin();
        }
    }
}

void Writer::addPorts(int64_t moduleId, int64_t defModuleId,
                      const std::vector<PortRow>& rows) {
    for (auto& r : rows) {
        sqlite3_reset(insPort);
        sqlite3_bind_int64(insPort, 1, moduleId);
        sqlite3_bind_int64(insPort, 2, internName(r.child));
        bindOptId(insPort, 3, defModuleId);
        sqlite3_bind_int64(insPort, 4, internName(r.port));
        sqlite3_bind_int(insPort, 5, directionCode(r.direction));
        bindOptId(insPort, 6, internName(r.outer));
        bindOptId(insPort, 7, internType(r.outerType));
        if (r.outerWidth >= 0)
            sqlite3_bind_int64(insPort, 8, r.outerWidth);
        else
            sqlite3_bind_null(insPort, 8);
        // A row with no outer net has no bits to describe; all three stay NULL
        // so a tie-off does not read as "the whole of nothing, exactly".
        if (r.outer.empty()) {
            sqlite3_bind_null(insPort, 9);
            sqlite3_bind_null(insPort, 10);
            sqlite3_bind_null(insPort, 11);
        }
        else {
            bindOptRange(insPort, 9, 10, r.outerBits);
            sqlite3_bind_int(insPort, 11, r.outerExact ? 1 : 0);
        }
        sqlite3_bind_int(insPort, 12, static_cast<int>(r.conn));
        bindOptId(insPort, 13, internFile(r.file));
        sqlite3_bind_int64(insPort, 14, r.line);
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
    sqlite3_bind_int64(insAssign, 11, row.proc);
    sqlite3_bind_int64(insAssign, 12, row.seq);
    if (row.blocking < 0)
        sqlite3_bind_null(insAssign, 13);
    else
        sqlite3_bind_int(insAssign, 13, row.blocking);
    sqlite3_bind_int64(insAssign, 14, row.dropped);
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
                           const std::vector<std::pair<std::string, std::string>>& events) {
    for (auto& e : events) {
        sqlite3_reset(insProcEvent);
        sqlite3_bind_int64(insProcEvent, 1, moduleId);
        sqlite3_bind_int64(insProcEvent, 2, proc);
        bindOptId(insProcEvent, 3, internName(e.first));
        bindOptText(insProcEvent, 4, e.second);
        step(insProcEvent);
        pending++;
    }
    if (pending >= kBatch) {
        commit();
        begin();
    }
}

void Writer::addInstance(const std::string& name, int64_t moduleId, int64_t parentId,
                         int64_t rowId) {
    sqlite3_reset(insInstance);
    sqlite3_bind_int64(insInstance, 1, rowId);
    sqlite3_bind_int64(insInstance, 2, internName(name));
    sqlite3_bind_int64(insInstance, 3, moduleId);
    if (parentId)
        sqlite3_bind_int64(insInstance, 4, parentId);
    else
        sqlite3_bind_null(insInstance, 4);
    step(insInstance);
    if (++pending >= kBatch) {
        commit();
        begin();
    }
}

void Writer::finish() {
    commit();
    exec(kIndexes);
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

} // namespace designdb
