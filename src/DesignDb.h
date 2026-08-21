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

/// v4 gives a generate block an `instance` row of its own, so a name is
/// always one path segment and `instance.module` is NULL for that level; and
/// it gives gate, switch and UDP instances `child` and `instance` rows, so
/// `child.def_module` NULL now means "a primitive, which has no module row"
/// as well as "a definition slang could not resolve" -- both readings a v3
/// consumer would get wrong rather than merely miss.
///
/// Bumped whenever a column's meaning changes -- or its value domain grows, as
/// in v2, where `port.conn_kind` gained kinds a v1 reader would have misread
/// as plain nets. A reader that does not know the version must refuse the file
/// rather than read it as though the layout held.
///
/// v3 moved for the same reason rather than for the new `stmt_read` table or
/// the new `port.inner` column, both of which a v2 reader would merely not
/// query: `assignment.proc` gained NULL, for a statement that is in no
/// procedure. A reader that joins it against `proc_event.proc`, or indexes
/// procedures by it, reads that as a fact about procedure 0 unless it is told
/// the layout changed.
///
/// v5 moved with no table or column change at all, which is the point: the
/// version is the *consumption contract*, not the DDL. `meta` gained
/// `duplicate_path_count` and `producer_revision` as required rows, and
/// `analysis_status` gained a constraint it did not have -- it must now agree
/// with the counts beside it, so `complete` alongside a non-zero error count is
/// a malformed file rather than a merely surprising one. Both a database
/// written before this and one written after would otherwise answer `4`, and a
/// consumer checking the version could not tell which contract it held. Since
/// `meta` is key-value, the rows are invisible to a reader that does not look
/// for them; it is the *required* set that changed, and that is exactly what a
/// version exists to state.
///
/// v6 gave the relations that were being guessed from file and line a name.
/// `assignment`, `hier_ref` and `stmt_read` gained `stmt`, a per-module
/// statement ordinal, because a statement writing an *outward* target has no
/// assignment row to gather its parts: `assign top.a = x; assign top.b = y;`
/// exported two writes and two reads that could be paired four ways, of which
/// the RTL supports one. `child` gained `id` and `kind`, and `instance` gained
/// `child`: `def_module IS NULL` meant "primitive" and "unresolved black box"
/// indistinguishably though a trace stopping at each means something different,
/// the two hierarchy tables were related by nothing a consumer could join on,
/// and an unresolved instantiation had no `instance` row at all -- so a path
/// through a black box did not resolve, and the tree reported the same emptiness
/// as a module that instantiates nothing.
/// v7 stopped crossing the two sides of an assignment. Every target used to be
/// paired with every operand, so `{a, b} = {x, y}` exported four edges where the
/// RTL has two -- and `y -> a` and `x -> b`, which are not dataflow at all,
/// carried src_exact=1 and dst_exact=1 like the two real ones. Both sides are now
/// walked as positioned slots and paired only where their bits meet, which also
/// narrows the target: `q[7:0] = {hi, lo}` records `hi -> q[7:4]` rather than `hi
/// -> q`. `assign_operand` was crossed the same way and is fixed with it.
///
/// `edge.map_exact` is the new column, and it is not a restatement of
/// src_exact/dst_exact: those describe each end's own range, while this describes
/// the correspondence between the ends. `q = a + b` knows both ranges exactly and
/// still cannot say which bit reaches which, because a carry crosses them.
///
/// v8 added the stable query interface: seven views (v_db_info,
/// v_tree_node, v_signal, v_port_connection, v_dependency, v_driver, v_load)
/// that resolve the intern tables and spell out the NULL conventions, so an
/// ordinary consumer never joins `name`/`type`/`file` or decodes `conn_kind`
/// itself. The version moves because a v8 consumer may rely on the views
/// existing with exactly their contracted columns -- a v7 file answers those
/// queries with "no such table". From here on: removing or renaming a view or
/// a view column, changing a column's semantics or NULL rules, or changing a
/// view's row granularity all bump the version; changing only how a view is
/// computed, with the contract intact, does not.
///
/// v8 also gave `port` a `child_id` foreign key, surfaced by both hierarchy
/// views, because without it the composition the views promise did not hold:
/// the tree spells an instance one segment at a time (`u_dec`) and the folded
/// port rows spell it whole (`g_rep[0].u_dec`), so joining the two by name
/// returned nothing exactly at generate boundaries -- and (module, name) is no
/// substitute key, since two unnamed gates in one module legally share a name.
///
/// v9 carried the bit-precision model across the instance boundary. `port`
/// gained `port_lo`/`port_hi`/`port_exact` -- the bits of the FORMAL a
/// connection element occupies -- and `map_exact`, edge.map_exact's boundary
/// twin. Before it, `.q({hi, lo})` exported two rows that both claimed all of
/// q with outer_exact=1: the per-bit precision v7 built for assignments ended
/// at every port, and `.q({2{r}})` was worse -- dedup keyed only on the outer
/// side folded the two copies into one row, deleting the second window
/// entirely. The window is part of the dedup key now, so a replication is as
/// many segments as it has copies.
///
/// v9 also finished the interface's object model, seven views to nine:
/// v_load became every recorded read of a signal (dataflow / sensitivity /
/// wait / statement, discriminated by load_kind -- the flop clock pins of a
/// netlist database are loads of the clock net, and so are ours), and the
/// statement layer gained v_stmt and v_stmt_operand, the half of
/// the edge/assignment dual projection that had no interface.
///
/// v10 unfolds the model. Rows hang off the elaborated instance occurrence,
/// not the module variant: `tree_node`/`inst`/`primitive` subtype the
/// hierarchy under one id space, `net`/`term`/`term_map`/`net_conn` replace
/// `symbol`/`port` with the boundary's two sides in two relations,
/// `procedure`/`stmt`/`assign_target`/`assign_operand`/`expr_ref` replace
/// `assignment`/`assign_operand`/`stmt_read` with real statement objects, and
/// `net_dep` replaces `edge` -- per statement occurrence, no cross-statement
/// dedup, every dependency naming the operand, target, expression reference,
/// primitive or call it came from. `module` returns to being the source
/// definition; the parameter values a body elaborated with live on each
/// `inst.param_signature`. The `name` intern table is gone (names are
/// TEXT on their object rows; only `data_type` still interns), the 0-5 and
/// 0-3 integer codes are gone (kinds and directions are their words), and
/// every relation is by object id, never by (module, name) string pairing.
/// Nine views become twelve; v_driver/v_load now compose the hierarchy
/// crossing (net_conn against term_map) that v9 left to the consumer.
///
/// A v9 database cannot be upgraded in place: the fold shared one row set
/// across occurrences and the edge dedup erased statement provenance, so the
/// per-occurrence identity v10 stores was never in the file. v10 databases
/// are produced only by re-exporting the RTL.
/// v11 records the alias statement. `alias a = b;` binds two nets into one
/// object, and v10 exported nothing at all for it: the two halves were
/// simply disconnected, so asking what drove one answered "nothing" and
/// asking what read the other left out every reader of the first. It is
/// its own kind rather than a pair of continuous assignments, because it
/// is not one -- an alias has no direction and no driver, and counting it
/// among the assignments would have made every multiple-driver query wrong
/// in a new way. `stmt.stmt_kind`, `net_dep.dep_kind` and the
/// driver/load kinds each gain `alias`; that is a value-domain change, so
/// the version moves even though no column does.
///
/// v12 is mostly a naming pass, and moves the version because renames are
/// contract changes. One classic abbreviation per word, every identifier,
/// no more tables-say-`inst` / views-say-`instance` split: `src_file`,
/// `prim`, `proc`, and the view family `v_db_info`/`v_term`/`v_term_map`/
/// `v_net_conn`/`v_net_dep`/`v_stmt*`. The two sides of a terminal stop
/// sharing column names -- `net_conn` wears `outer_*` (the actual, VPI's
/// highConn), `term_map` wears `inner_*` (vpiLowConn). `assign_target`
/// becomes `stmt_target`: it holds the lvalue of a release and a system
/// task's write as well as an assignment's, so it was never assignment-
/// only (`assign_operand`, which is, keeps its name).
///
/// Riding along, because the version was already moving: an unresolved
/// SOURCE reference is written as a `net_dep` (NULL source net, the
/// reference on the source end) and surfaces as `driver_kind='external'`,
/// so a target fed only from a package variable or an upward name is no
/// longer silently undriven; `force`/`release` are recorded (construct
/// `force`/`proc_assign`, and `stmt_kind='release'`) instead of a force
/// masquerading as a plain blocking assignment; an external tie carries a
/// `map_exact`, so its crossing traces bit by bit; and `prim_kind='switch'`
/// covers the LRM's whole switch family, not just what slang labels
/// bidirectional. Additive, so no reader must change: `inst_param` makes
/// the parameter signature queryable, and `v_net_attachment` is a
/// thirteenth view -- one row per relation touching a net.
///
/// v13 sharpens what a v12 reader could not ask precisely.
/// `v_net_attachment`'s one polymorphic `other_id` becomes seven typed
/// nullable ids (exactly one non-null per row, the one attachment_kind
/// names) -- the exclusive-arc shape net_dep already uses, so a consumer
/// joins the right base table without decoding the kind.
///
/// Packages become first-class objects. A package is a pseudo-occurrence --
/// a tree_node/inst under node_kind/def_kind 'package', above the roots
/// (parent_inst_id NULL), its variables `net` rows -- so a `pkg::x`
/// reference resolves to a real net (driver_kind='data') instead of
/// dead-ending at 'external', and two modules reading one package variable
/// meet on it. 'external' now means only the genuinely unresolvable: an
/// upward hierarchical reference from a shared body, an interface-array
/// binding. ($unit compilation-unit items are not stamped yet.)
/// (Per-call-site dataflow tagging lands in the same version; its note is
/// added as that piece does.)
inline constexpr int SchemaVersion = 13;

/// Every id in these rows is assigned by the extractor, never by SQLite.
/// The stamping pass computes cross-references between tables before any row
/// is written -- a `net_dep` row needs its operand's id, which needs its
/// statement's id -- so the ids must exist ahead of the inserts, and one
/// counter per table in one process is cheaper and more legible than reading
/// last_insert_rowid back per row. 0 in an id field spells "none" and is
/// stored as NULL; real ids start at 1.
///
/// Ranges use one encoding everywhere, unchanged from v7: a range is
/// LSB-relative offsets into the flattened object (not declared indices), an
/// absent range with exact=true is the whole object, an absent range with
/// exact=false is "somewhere inside it, unknown where", and a present range
/// with exact=false is an upper bound rather than the bits actually touched.

/// One source definition: module, interface, program or checker.
struct ModuleRow {
    int64_t id = 0;
    std::string name;
    std::string definitionKind;   // module | interface | program | checker
    int64_t fileId = 0;
    uint32_t line = 0;
    uint32_t column = 0;
};

/// One node of the elaborated hierarchy tree.
struct TreeNodeRow {
    int64_t id = 0;
    int64_t parentNodeId = 0;     // 0 = root (stored NULL)
    std::string name;             // one path segment, never more
    std::string nodeKind;         // root | instance | generate | primitive | unresolved
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
    std::string primitiveKind;    // gate | switch | udp
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
    int64_t scopeNodeId = 0;      // the instance or generate node declaring it
    std::string name;             // scope-relative dotted path (`g[0].sig`, `bump.v`)
    std::string declarationKind;  // wire | tri | ... | variable
    int64_t dataTypeId = 0;
    int64_t width = -1;           // flattened bits; -1 = not integral, stored NULL
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
    std::string terminalKind;     // signal | interface
    std::string direction;        // input | output | inout | ref; "" = none (interface,
                                  // or an unresolved instance), stored NULL
    int64_t dataTypeId = 0;
    int64_t width = -1;           // -1 = NULL
    int64_t ordinal = 0;          // position in the port list
    int isConst = -1;             // const ref; -1 = does not apply, stored NULL
    std::string modport;          // "" = NULL
    int64_t fileId = 0;
    uint32_t line = 0;
    uint32_t column = 0;
};

/// One segment of a terminal's mapping onto nets inside its own instance.
struct TermMapRow {
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
    std::string connectionKind;   // signal | constant | unconnected |
                                  // expression_operand | interface | external_reference
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

/// One procedure: an always/initial/final block, or a task/function body.
struct ProcedureRow {
    int64_t id = 0;
    int64_t instId = 0;
    int64_t scopeNodeId = 0;
    std::string name;             // task/function name; "" = anonymous, stored NULL
    std::string procedureKind;    // always | always_ff | always_comb | always_latch |
                                  // initial | final | task | function
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
    std::string statementKind;    // assignment | assertion | wait | call |
                                  // system_task | event_control
    std::string construct;        // assign | always_ff | assert | $display | ...
    std::string assignmentKind;   // continuous | blocking | nonblocking; "" = NULL
    std::string delay;            // normalised delay control text; "" = NULL
    int64_t droppedOperandCount = 0;
    int64_t fileId = 0;
    uint32_t line = 0;
    uint32_t column = 0;
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
    std::string role;             // control | assertion | wait | event |
                                  // call_argument | system_task
    std::optional<std::pair<uint64_t, uint64_t>> bits;
    bool exact = true;
};

/// One edge event a procedure triggers on or waits on.
struct ProcEventRow {
    int64_t id = 0;
    int64_t procedureId = 0;
    int64_t stmtId = 0;           // the wait statement; 0 for a sensitivity list
    int64_t netId = 0;            // 0 when the event expression is not a plain net
    std::string eventKind;        // sensitivity | wait
    std::string edgeKind;         // posedge | negedge | both; "" = NULL
    int64_t fileId = 0;
    uint32_t line = 0;
    uint32_t column = 0;
};

/// One net-to-net dependency occurrence, with its provenance. An end that
/// resolved through a hierarchical reference carries the resolved net id
/// and names the hier_ref row instead of an operand/target row.
struct NetDepRow {
    int64_t id = 0;
    int64_t sourceNetId = 0;      // 0 = a constant drives the target
    int64_t targetNetId = 0;
    int64_t stmtId = 0;
    int64_t assignOperandId = 0;
    int64_t stmtTargetId = 0;
    int64_t exprRefId = 0;
    int64_t primitiveId = 0;
    int64_t sourceHierRefId = 0;
    int64_t targetHierRefId = 0;
    std::string dependencyKind;   // data | control | primitive | procedure
    std::optional<std::pair<uint64_t, uint64_t>> sourceBits;
    int sourceExact = -1;         // -1 = no source end, stored NULL
    std::optional<std::pair<uint64_t, uint64_t>> targetBits;
    bool targetExact = true;
    int mappingExact = -1;        // -1 = nothing to correspond, stored NULL
};

/// One reference that leaves the instance, as written and, when slang could
/// resolve it, as the object it lands on.
struct HierRefRow {
    int64_t id = 0;
    int64_t instId = 0;
    int64_t stmtId = 0;           // 0 = made by a port connection, not a statement
    std::string path;             // as written, normalised
    std::string access;           // read | write | connect
    int64_t resolvedInstId = 0;   // 0 = not resolved to an object in this export
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
    /// `checkConstraints` keeps the enum-domain CHECK clauses in the DDL.
    /// Off by default: they cost more than the rest of the insert put
    /// together (a string IN-list is evaluated per row, and measured at
    /// +186% on the widest table), while verify-designdb.py checks every
    /// one of those domains on the finished file. On, for a database that
    /// must refuse a bad value at the moment it is written.
    explicit Writer(const std::string& path, bool checkConstraints = false);
    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    void setMeta(std::string_view key, std::string_view value);

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
        InsTerm, InsTermMap, InsNetConn, InsProcedure, InsStmt,
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
