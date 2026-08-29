// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// The SQLite side of the exporter: schema, and a writer that streams rows.

#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <string>
#include <unordered_map>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace designdb {

/// The database's consumption contract, an integer bumped whenever that
/// contract changes: the view set, each view's columns and their order, every
/// column's meaning, value domain and NULL rules, each view's row granularity,
/// the tables the field reference names, and the required db_info columns. A
/// reader that does not recognise the number must refuse the file rather than
/// read it as though the layout held; no database is upgraded in place, so a
/// bump means re-exporting the RTL. What each past bump changed is in
/// doc/schema-history.md.
inline constexpr int SchemaVersion = 20;

/// Every id in these rows is assigned by the extractor, never by SQLite.
/// The stamping pass computes cross-references between tables before any row
/// is written -- a `net_dep` row needs its operand's id, which needs its
/// statement's id -- so the ids must exist ahead of the inserts, and one
/// counter per table in one process is cheaper and more legible than reading
/// last_insert_rowid back per row. 0 in an id field spells "none" and is
/// stored as NULL; real ids start at 1.
///
/// `src_file`, `file` and `data_type` are the exceptions: they are interned
/// through SQLite, so their insert order is their id order. `src_file` is the
/// one whose insert order is CHOSEN rather than incidental -- the other two
/// follow the extraction walk, which is already fixed -- so main.cpp interns
/// its rows in path order. slang returns the buffers in the order its source
/// loader finished reading files, which is a thread pool's completion order,
/// and an id that followed it made two exports of an unchanged design differ.
///
/// Ranges use one encoding everywhere: a range is
/// LSB-relative offsets into the flattened object (not declared indices), an
/// absent range with exact=true is the whole object, an absent range with
/// exact=false is "somewhere inside it, unknown where", and a present range
/// with exact=false is an upper bound rather than the bits actually touched.
///
/// Closed vocabularies (kinds, directions, roles) live in extract/ir/Vocab.h;
/// a field holding one names its enum rather than re-listing the words, which
/// drifted every time a domain moved.

/// One source definition.
struct ModuleRow {
    int64_t id = 0;
    std::string name;
    std::string definitionKind;   // DefKind's word
    int64_t fileId = 0;
    uint32_t line = 0;
    uint32_t column = 0;
};

/// One node of the elaborated hierarchy tree.
struct TreeNodeRow {
    int64_t id = 0;
    int64_t parentNodeId = 0;     // 0 = root (stored NULL)
    std::string name;             // one path segment, never more
    std::string nodeKind;         // NodeKind's word
    int64_t ordinal = 0;          // order among siblings
};

/// One module instance occurrence. `id` is the same value as its tree_node id.
struct InstRow {
    int64_t id = 0;
    int64_t moduleId = 0;         // 0 when the definition did not resolve
    int64_t parentInstId = 0;     // nearest enclosing module instance; 0 for the root
    std::string parameterSignature;
    std::string unresolvedDefinition;  // the name as written, when moduleId is 0
    int64_t fileId = 0;           // the instantiation site; 0 for the root
    uint32_t line = 0;
    uint32_t column = 0;
};

/// One elaborated parameter value of one occurrence -- param_signature made
/// queryable. Same normalisation, same order, same over-split.
struct InstParamRow {
    int64_t instId = 0;
    int64_t ordinal = 0;          // declaration order, as the signature has it
    std::string name;
    std::string value;
};

/// One gate, switch or UDP instance. `id` is the same value as its tree_node id.
struct PrimitiveRow {
    int64_t id = 0;
    int64_t instId = 0;           // the module instance whose body wrote it
    std::string primitiveKind;    // PrimKind's word
    std::string definitionName;   // and | bufif1 | the UDP's name
    int64_t fileId = 0;
    uint32_t line = 0;
    uint32_t column = 0;
};

/// One connectable object inside an instance: a net or a variable, subroutine
/// formals and locals included (they are dependency endpoints, so they must be
/// rows -- a name with no row cannot be referenced by id).
struct NetRow {
    int64_t id = 0;
    int64_t instId = 0;
    int64_t scopeNodeId = 0;      // the instance or generate node declaring it;
                                  // NOT the name's anchor
    std::string name;             // dotted path relative to instId, generate and
                                  // subroutine segments included (`g[0].sig`, `bump.v`)
    std::string declarationKind;  // wire | tri | ... | variable
    int64_t dataTypeId = 0;
    int64_t width = -1;           // flattened bits, the space offsets index;
                                  // -1 = a type with no bits, stored NULL
    bool isImplicit = false;
    int64_t fileId = 0;
    uint32_t line = 0;
    uint32_t column = 0;
};

/// One terminal of an instance: a port on its boundary. The root instance's
/// terminals are the top-level ports; a child's are its pins.
struct TermRow {
    int64_t id = 0;
    int64_t instId = 0;
    std::string name;
    std::string terminalKind;     // TermKind's word
    std::string direction;        // Direction's word; "" = none (interface,
                                  // or an unresolved instance), stored NULL
    int64_t dataTypeId = 0;
    int64_t width = -1;           // -1 = NULL
    int64_t ordinal = 0;          // position in the port list
    std::string modport;          // "" = NULL
    int64_t fileId = 0;
    uint32_t line = 0;
    uint32_t column = 0;
};

/// One segment of a terminal's mapping onto nets inside its own instance.
struct TermMapRow {
    int64_t id = 0;
    int64_t termId = 0;
    int64_t ordinal = 0;
    int64_t netId = 0;
    std::optional<std::pair<uint64_t, uint64_t>> termBits;
    bool termExact = true;
    std::optional<std::pair<uint64_t, uint64_t>> netBits;
    bool netExact = true;
    bool mappingExact = false;
};

/// One segment of a terminal's outside connection, written in the parent.
struct NetConnRow {
    int64_t id = 0;
    int64_t netId = 0;            // parent-side net; 0 when the kind has none
    int64_t termId = 0;
    int64_t ordinal = 0;
    std::string connectionKind;   // ConnKind's word
    std::optional<std::pair<uint64_t, uint64_t>> netBits;
    int netExact = -1;            // -1 = no net end at all, stored NULL
    std::optional<std::pair<uint64_t, uint64_t>> termBits;
    int termExact = -1;           // -1 = no formal bit domain, stored NULL
    int mappingExact = -1;        // -1 = nothing to correspond, stored NULL
    int64_t interfaceInstId = 0;  // the bound interface instance, kind=interface
    int64_t hierRefId = 0;        // the outward tie, kind=external_reference
    int64_t fileId = 0;
    uint32_t line = 0;
    uint32_t column = 0;
};

/// One procedure: an always/initial/final block. Task and function bodies get
/// no row; their statements belong to the calling procedure.
struct ProcedureRow {
    int64_t id = 0;
    int64_t instId = 0;
    int64_t scopeNodeId = 0;
    std::string procedureKind;    // ProcKind's word
    int64_t ordinal = 0;
    int64_t fileId = 0;
    uint32_t line = 0;
    uint32_t column = 0;
};

/// One statement, or one statement-level construct.
struct StmtRow {
    int64_t id = 0;
    int64_t instId = 0;
    int64_t scopeNodeId = 0;
    int64_t procedureId = 0;      // 0 = not in a procedure (continuous assign)
    int64_t ordinal = 0;          // order within the instance
    int64_t sequence = -1;        // execution order within the procedure; -1 = NULL
    std::string statementKind;    // StmtKind's word
    std::string construct;        // assign | always_ff | assert | $display | ...
    std::string assignmentKind;   // AssignKind's word; "" = NULL
    std::string delay;            // normalised delay control text; "" = NULL
    int64_t droppedOperandCount = 0;
    int64_t callSiteId = 0;       // the call-site expansion this belongs to; 0 = NULL
    int64_t branchId = 0;         // the gating level it sits in; 0 = NULL
    int64_t fileId = 0;
    uint32_t line = 0;
    uint32_t column = 0;
};

/// One level of the gating context a statement sits under.
struct BranchRow {
    int64_t id = 0;
    int64_t instId = 0;
    int64_t parentBranchId = 0;   // the level outside this one; 0 = NULL
    int64_t depth = 1;
    int64_t ordinal = -1;         // arm position under a case point; -1 = NULL
    std::string branchKind;       // BranchKind's word
    std::string sense;            // BranchSense's word; "" = NULL
    std::string caseKind;         // CaseKind's word; "" = NULL
    std::string checkKind;        // CheckKind's word; "" = NULL
    int staticTaken = -1;         // -1 = NULL
    int64_t iterNetId = 0;        // the loop index; 0 = NULL
    int64_t iterCount = -1;       // -1 = NULL
    int64_t iterFirst = 0;
    int64_t iterStep = 0;
    bool hasProgression = false;  // false NULLs iter_first and iter_step
    int64_t fileId = 0;
    uint32_t line = 0;
    uint32_t column = 0;
    int64_t procedureId = 0;      // the procedure it is inside; 0 = NULL
    int64_t callSiteId = 0;       // the expansion it was walked for; 0 = NULL
};

/// One label of a case item, evaluated.
struct BranchLabelRow {
    int64_t id = 0;
    int64_t branchId = 0;
    int64_t ordinal = 0;
    std::string value;
    bool hasValue = false;        // false = NULL: not constant-evaluable
};

/// One (level, ancestor) pair of the gating tree, the level itself included.
struct BranchAncestorRow {
    int64_t branchId = 0;
    int64_t ancestorBranchId = 0;
    int64_t distance = 0;
};

/// One read of one level's condition.
struct BranchRefRow {
    int64_t id = 0;
    int64_t branchId = 0;
    int64_t ordinal = 0;
    int64_t netId = 0;
    std::optional<std::pair<uint64_t, uint64_t>> bits;
    bool exact = true;
};

/// One subroutine-body expansion: a body walked once per call site.
struct CallSiteRow {
    int64_t id = 0;
    int64_t instId = 0;
    int64_t callerStmtId = 0;      // the statement making the call; 0 = NULL
    int64_t parentCallSiteId = 0;  // the enclosing expansion; 0 = NULL (top)
    std::string subroutineName;
    int64_t depth = 0;
};

/// One assignment target reference (LHS).
struct StmtTargetRow {
    int64_t id = 0;
    int64_t stmtId = 0;
    int64_t ordinal = 0;
    int64_t netId = 0;
    std::optional<std::pair<uint64_t, uint64_t>> bits;
    bool exact = true;
};

/// One assignment operand reference (RHS).
struct AssignOperandRow {
    int64_t id = 0;
    int64_t stmtId = 0;
    int64_t ordinal = 0;
    int64_t netId = 0;
    std::optional<std::pair<uint64_t, uint64_t>> bits;
    bool exact = true;
};

/// One statement read that is not an assignment operand: a branch condition,
/// an assertion's read, a wait condition, a call argument.
struct ExprRefRow {
    int64_t id = 0;
    int64_t stmtId = 0;
    int64_t ordinal = 0;
    int64_t netId = 0;
    std::string role;             // RefRole's word
    std::optional<std::pair<uint64_t, uint64_t>> bits;
    bool exact = true;
};

/// One edge event a procedure triggers on or waits on.
struct ProcEventRow {
    int64_t id = 0;
    int64_t procedureId = 0;
    int64_t stmtId = 0;           // the wait statement; 0 for a sensitivity list
    int64_t netId = 0;            // 0 when the event expression is not a plain net
    std::string eventKind;        // EventKind's word
    std::string edgeKind;         // Edge's word; "" = NULL
    int64_t fileId = 0;
    uint32_t line = 0;
    uint32_t column = 0;
};

/// One net-to-net dependency occurrence, with its provenance. An end that
/// resolved through a hierarchical reference carries the resolved net id
/// and names the hier_ref row instead of an operand/target row.
struct NetDepRow {
    int64_t id = 0;
    int64_t sourceNetId = 0;      // 0 = no nameable source (stored NULL): a
                                  // constant, an unresolved external reference,
                                  // an input-less primitive, or a procedure
                                  // write-back -- dependencyKind and the
                                  // reference ids tell which (see v_driver)
    int64_t targetNetId = 0;
    int64_t stmtId = 0;
    int64_t assignOperandId = 0;
    int64_t stmtTargetId = 0;
    int64_t exprRefId = 0;
    int64_t branchId = 0;
    int64_t primitiveId = 0;
    int64_t sourceHierRefId = 0;
    int64_t targetHierRefId = 0;
    std::string dependencyKind;   // DepKind's word
    std::optional<std::pair<uint64_t, uint64_t>> sourceBits;
    int sourceExact = -1;         // -1 = no source end, stored NULL
    std::optional<std::pair<uint64_t, uint64_t>> targetBits;
    bool targetExact = true;
    int mappingExact = -1;        // -1 = nothing to correspond, stored NULL
    int64_t callSiteId = 0;       // the call-site expansion this belongs to; 0 = NULL
};

/// The seal: the whole of `db_info`, written once.
///
/// Every field is required, which is why they are values and not options --
/// a writer that forgets one fails to compile rather than producing a
/// database missing a fact. `top` is the exception the schema names: NULL when
/// the design elaborated no top at all.
struct DbInfoRow {
    int schemaVersion = 0;
    std::string tool;
    std::string toolVersion;
    std::string slangVersion;
    std::string producerRevision;
    std::string top;              // empty = no top elaborated, written NULL
    std::string analysisStatus;
    int64_t errorCount = 0;
    int64_t unresolvedCount = 0;
    int64_t emptyProcedureCount = 0;
    int64_t duplicatePathCount = 0;
    int64_t recursionCount = 0;
    int64_t truncatedCallCount = 0;
    int64_t checkerInstCount = 0;
    int64_t unanalysedInstCount = 0;
    std::string configDigest;
};

/// One reference that leaves the instance, as written and, when slang could
/// resolve it, as the object it lands on.
struct HierRefRow {
    int64_t id = 0;
    int64_t instId = 0;
    int64_t stmtId = 0;           // 0 = made by a port connection, not a statement
    int64_t branchId = 0;         // set only on a branch condition; 0 = NULL
    std::string path;             // as written, normalised
    std::string access;           // Access's word
    int64_t resolvedNetId = 0;
    std::optional<std::pair<uint64_t, uint64_t>> bits;
    bool exact = true;
    int64_t fileId = 0;
    uint32_t line = 0;
    uint32_t column = 0;
};

/// Writes the database. One writer, deliberately: SQLite serialises writers, so
/// a second one contends on the same lock rather than adding throughput. At
/// ~1.2 M rows/s it is far ahead of the producer either way.
class Writer {
public:
    /// Creates `path`, replacing any existing file.
    explicit Writer(const std::string& path);
    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /// The seal, in one statement. Called after finish(), for the reason the
    /// caller gives: the data and the indexes are complete before the row
    /// that says the export ran to completion exists at all.
    void setDbInfo(const DbInfoRow& r);

    /// Records a source file and its SHA-256, so a consumer can tell that the
    /// database and the RTL have diverged instead of answering from stale data.
    void addSourceFile(const std::string& path, const std::string& digest);

    /// Joins `file` rows to `src_file` rows: `origins` maps each as-written
    /// spelling to the absolute path the buffer really came from. Called once,
    /// after rows are written (file paths intern lazily) and before finish().
    void linkSourceFiles(
        const std::unordered_map<std::string, std::string>& origins);

    /// Interns a repeated string into `data_type` and returns the row id.
    ///
    /// The one intern table v10 keeps: a SystemVerilog enum or packed struct
    /// prints as its whole member list, so one net row can carry kilobytes of
    /// it, and the instance-level model repeats each row once per occurrence.
    /// Names are not interned -- they are short, and the join-per-query the
    /// name table cost every consumer was what the views existed to hide.
    int64_t internDataType(const std::string& text);
    int64_t internFile(const std::string& path);

    void addModule(const ModuleRow& r);
    void addTreeNode(const TreeNodeRow& r);
    void addInst(const InstRow& r);
    void addInstParam(const InstParamRow& r);
    void addPrimitive(const PrimitiveRow& r);
    void addNet(const NetRow& r);
    void addTerm(const TermRow& r);
    void addTermMap(const TermMapRow& r);
    void addNetConn(const NetConnRow& r);
    void addProcedure(const ProcedureRow& r);
    void addBranch(const BranchRow& r);
    void addBranchLabel(const BranchLabelRow& r);
    void addBranchRef(const BranchRefRow& r);
    void addBranchAncestor(const BranchAncestorRow& r);
    void addCallSite(const CallSiteRow& r);
    void addStmt(const StmtRow& r);
    void addStmtTarget(const StmtTargetRow& r);
    void addAssignOperand(const AssignOperandRow& r);
    void addExprRef(const ExprRefRow& r);
    void addProcEvent(const ProcEventRow& r);
    void addNetDep(const NetDepRow& r);
    void addHierRef(const HierRefRow& r);

    /// Commits, then builds the indexes and creates the views. Indexes come
    /// after the data: filling a table that already carries them costs far
    /// more than one build at the end.
    void finish();

private:
    void exec(const char* sql);
    void prepare(const char* sql, sqlite3_stmt** out);
    void step(sqlite3_stmt* stmt);
    void begin();
    void commit();
    void bumped();

    enum Ins {
        InsModule, InsTreeNode, InsInst, InsInstParam, InsPrimitive, InsNet,
        InsTerm, InsTermMap, InsNetConn, InsProcedure, InsBranch,
        InsBranchLabel,
        InsBranchRef,
        InsBranchAncestor, InsCallSite, InsStmt,
        InsStmtTarget, InsAssignOperand, InsExprRef, InsProcEvent,
        InsNetDep, InsHierRef,
        InsCount
    };

    sqlite3* db = nullptr;
    sqlite3_stmt* ins[InsCount] = {};
    std::unordered_map<std::string, int64_t> dataTypeIds;
    std::unordered_map<std::string, int64_t> fileIds;
    bool inTransaction = false;
    int64_t pending = 0;
};

/// SHA-256 of a file, empty when it cannot be read.
std::string fileDigest(const std::string& path);

/// SHA-256 of an in-memory buffer.
std::string digest(std::string_view data);

} // namespace designdb
