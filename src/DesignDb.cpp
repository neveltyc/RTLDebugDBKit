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

// The DDL lives in src/sql/, one file per phase, because it is 1,370 lines of
// SQL against 750 lines of C++ -- reading the writer meant scrolling past the
// whole schema and every view to reach it. Each file holds one raw string
// literal, so the `#include` has to sit OUTSIDE the literal: the preprocessor
// does not run inside R"SQL(...)SQL", and a directive written in there would be
// stored as SQL text and handed to SQLite verbatim.
//
// The design commentary travels with the SQL it describes rather than staying
// here; what the three names mean is the writer's business, and how the schema
// is shaped is the schema's.
constexpr const char* kSchema =
#include "sql/Schema.inc"
    ;

constexpr const char* kIndexes =
#include "sql/Indexes.inc"
    ;

constexpr const char* kViews =
#include "sql/Views.inc"
    ;

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
        prepare("INSERT INTO inst_param VALUES(?,?,?,?)", &ins[InsInstParam]);
        prepare("INSERT INTO prim VALUES(?,?,?,?,?,?,?)", &ins[InsPrimitive]);
        prepare("INSERT INTO net VALUES(?,?,?,?,?,?,?,?,?,?,?)", &ins[InsNet]);
        prepare("INSERT INTO term VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)", &ins[InsTerm]);
        prepare("INSERT INTO term_map VALUES(?,?,?,?,?,?,?,?,?,?)", &ins[InsTermMap]);
        prepare("INSERT INTO net_conn VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                &ins[InsNetConn]);
        prepare("INSERT INTO proc VALUES(?,?,?,?,?,?,?,?,?)", &ins[InsProcedure]);
        prepare("INSERT INTO call_site VALUES(?,?,?,?,?,?)", &ins[InsCallSite]);
        prepare("INSERT INTO stmt VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &ins[InsStmt]);
        prepare("INSERT INTO stmt_target VALUES(?,?,?,?,?,?,?)",
                &ins[InsStmtTarget]);
        prepare("INSERT INTO assign_operand VALUES(?,?,?,?,?,?,?)",
                &ins[InsAssignOperand]);
        prepare("INSERT INTO expr_ref VALUES(?,?,?,?,?,?,?,?)", &ins[InsExprRef]);
        prepare("INSERT INTO proc_event VALUES(?,?,?,?,?,?,?,?,?)", &ins[InsProcEvent]);
        prepare("INSERT INTO net_dep VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
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

void Writer::addInstParam(const InstParamRow& r) {
    auto* s = ins[InsInstParam];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.instId);
    sqlite3_bind_int64(s, 2, r.ordinal);
    sqlite3_bind_text(s, 3, r.name.data(), static_cast<int>(r.name.size()),
                      SQLITE_STATIC);
    sqlite3_bind_text(s, 4, r.value.data(), static_cast<int>(r.value.size()),
                      SQLITE_STATIC);
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
    // A row with no net end has no net range -- the NULL discipline
    // enforced at the one chokepoint every emitter goes through, so a
    // tie-off can never read as "the whole of nothing, exactly". An
    // external tie is the exception for the MAPPING alone: its outer end
    // exists (the hier_ref names it), so the correspondence with the
    // formal is a real fact even though the outer range lives on the
    // reference row rather than here.
    if (r.netId == 0) {
        bindRangeTri(s, 6, std::nullopt, -1);
        bindRangeTri(s, 9, r.termBits, r.termExact);
        if (r.hierRefId == 0)
            sqlite3_bind_null(s, 12);
        else
            bindTri(s, 12, r.mappingExact);
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
    bindOptId(s, 12, r.callSiteId);
    bindLoc(s, 13, r.fileId, r.line, r.column);
    step(s);
    bumped();
}

void Writer::addCallSite(const CallSiteRow& r) {
    auto* s = ins[InsCallSite];
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.id);
    sqlite3_bind_int64(s, 2, r.instId);
    bindOptId(s, 3, r.callerStmtId);
    bindOptId(s, 4, r.parentCallSiteId);
    sqlite3_bind_text(s, 5, r.subroutineName.c_str(),
                      static_cast<int>(r.subroutineName.size()), SQLITE_STATIC);
    sqlite3_bind_int64(s, 6, r.depth);
    step(s);
    bumped();
}

void Writer::addStmtTarget(const StmtTargetRow& r) {
    auto* s = ins[InsStmtTarget];
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
    bindOptId(s, 6, r.stmtTargetId);
    bindOptId(s, 7, r.exprRefId);
    bindOptId(s, 8, r.primitiveId);
    bindOptId(s, 9, r.sourceHierRefId);
    bindOptId(s, 10, r.targetHierRefId);
    sqlite3_bind_text(s, 11, r.dependencyKind.c_str(), static_cast<int>(r.dependencyKind.size()),
                      SQLITE_STATIC);
    // A row with no source END has no source range to describe, so every
    // column describing one is NULL -- enforced here rather than in every
    // emitter. Before this discipline, `assign q = 1'b0` carried
    // src_exact=1 and map_exact=1: a per-bit mapping onto a driver that
    // does not exist. A source that exists but resolves to no net row (an
    // 'external' reference) keeps its range: the range describes the
    // referenced object's bits, which are real even when unnamed here.
    if (r.sourceNetId == 0 && r.sourceHierRefId == 0) {
        bindRangeTri(s, 12, std::nullopt, -1);
        bindRange(s, 15, r.targetBits, r.targetExact);
        sqlite3_bind_null(s, 18);
    }
    else {
        bindRangeTri(s, 12, r.sourceBits, r.sourceExact);
        bindRange(s, 15, r.targetBits, r.targetExact);
        bindTri(s, 18, r.mappingExact);
    }
    // A call site tags a statement's dataflow, so a row with no statement
    // carries none -- the argument bindings of a call in a control
    // expression (`if (f(x))`) have stmt_id NULL and get call_site_id NULL
    // with it, rather than a tag pointing into a call that owns no statement.
    bindOptId(s, 19, r.stmtId ? r.callSiteId : 0);
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
