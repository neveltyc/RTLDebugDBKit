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

/// Bumped whenever a column's meaning changes -- or its value domain grows, as
/// in v2, where `port.conn_kind` gained kinds a v1 reader would have misread
/// as plain nets. A reader that does not know the version must refuse the file
/// rather than read it as though the layout held.
inline constexpr int SchemaVersion = 2;

/// One intra-module dataflow edge, in the module's own namespace.
///
/// Paths are relative to the module body, not to any instance: that is what
/// makes the row shared by every instance of the module. A reader stamps an
/// instance path in front of it.
struct EdgeRow {
    std::string src;
    std::string dst;
    std::string srcType;
    std::string dstType;
    std::string kind;         // continuous_assign | procedural | procedure
    std::string construct;        // always_ff / assign / ...
    std::string file;
    uint32_t line = 0;
    // Bit ranges, absent for a whole-signal edge. LSB-relative offsets into
    // the flattened object, not declared indices -- see the schema comment.
    std::optional<std::pair<uint64_t, uint64_t>> srcBits;
    std::optional<std::pair<uint64_t, uint64_t>> dstBits;
    // False when a dynamic selector meant the range could not be narrowed, so
    // it is an upper bound rather than the bits actually touched.
    bool srcExact = true;
    bool dstExact = true;
    // The operand reached the target through a condition, not the RHS.
    bool control = false;
};

/// One assignment: a statement that writes a target.
///
/// `edge` flattens a procedure; this keeps its statements apart, so a target
/// assigned in four places reads as four statements rather than one merged set
/// of sources.
struct AssignRow {
    std::string dst;
    std::optional<std::pair<uint64_t, uint64_t>> dstBits;
    std::string kind;         // continuous_assign | procedural
    std::string construct;    // assign | always_ff | ...
    std::string file;
    uint32_t line = 0;
    int64_t proc = 0;         // which procedure in the module
    int64_t seq = 0;          // order within that procedure
    int blocking = -1;        // 1 for `=`, 0 for `<=`, -1 for a continuous assign
    bool dstExact = true;
    /// Operands not recorded: compile-time constants, and references to symbols
    /// outside this module. Without it a row with one operand cannot be told
    /// apart from a row with one operand and three that were filtered.
    int64_t dropped = 0;   // operands not recorded
};

/// One operand of an assignment.
struct OperandRow {
    std::string name;
    std::optional<std::pair<uint64_t, uint64_t>> bits;
    bool exact = true;
};

/// One declaration inside a module: a signal, a port, or a parameter.
///
/// The rest of the database is derived from *edges*, so a signal exists in it
/// only if it takes part in dataflow. That is enough to trace a driver and
/// wrong for everything else: a declared-but-unused signal, an output nobody
/// drives, and a clock that only appears in a sensitivity list are all absent,
/// and those are exactly the ones worth asking about.
struct SymbolRow {
    std::string name;         // module-relative, generate-block prefix included
    std::string kind;         // variable | net | port | parameter
    std::string type;
    int64_t width = -1;       // bits; -1 when the type is not integral
    std::string direction;    // "" unless this signal is a port
    std::string file;
    uint32_t line = 0;
    uint32_t col = 0;
};

/// How a child instance's port is attached. `Expression` is an operand of an
/// expression tied to the port, not a wired net: `.en(state == RUN)` samples
/// `state` but does not alias it to `en`, and a consumer must be able to tell.
/// `Interface` binds a child's interface port to an interface instance in the
/// parent: the row is the alias that lets `child.bus.*` resolve at all.
enum class PortConn { Net = 0, Constant = 1, Unconnected = 2, Expression = 3,
                      Interface = 4 };

/// One port connection on a child instance, as written in the parent.
///
/// This is the only row type that spans two modules, and it is what lets a
/// trace leave one and continue in the other: `outer` names a net in the
/// parent's namespace, `port` names the formal it ties to inside the child.
/// A module that is nothing but instantiations -- 38% of them on one SoC --
/// has no dataflow rows at all and is reachable only through these.
struct PortRow {
    std::string child;        // child instance name, generate prefix included
    std::string port;         // formal port name inside the child
    std::string direction;    // in | out | inout | ref
    std::string outer;        // the connected net, in the PARENT's namespace
    std::string outerType;
    int64_t outerWidth = -1;  // width of the connection expression, not of the net
    // The bits of `outer` the connection selects, absent when it attaches the
    // whole net. Same encoding as EdgeRow: read with outerExact.
    std::optional<std::pair<uint64_t, uint64_t>> outerBits;
    bool outerExact = true;
    PortConn conn = PortConn::Net;
    // For an interface binding: the modport restricting it, when one does.
    std::string modport;
    std::string file;
    uint32_t line = 0;
};

/// One instantiation inside a module body.
struct ChildRow {
    std::string name;         // instance name, generate-block prefix included
    std::string defName;   // the definition it instantiates
    int64_t defModule = 0;  // module row id, resolved by the caller
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

    void setMeta(std::string_view key, std::string_view value);

    /// Records a source file and its SHA-256, so a consumer can tell that the
    /// database and the RTL have diverged instead of answering from stale data.
    void addSourceFile(const std::string& path, const std::string& digest);

    /// Interns a module (name + its elaborated parameter text) and returns its
    /// row id. Parameters are part of the identity because they change the
    /// module's contents, not just its numbers.
    int64_t internModule(const std::string& name, const std::string& params);

    void addEdges(int64_t moduleId, const std::vector<EdgeRow>& rows);

    /// Interns a repeated string into its own table and returns the row id.
    ///
    /// Type text dominates the file otherwise: a SystemVerilog enum or packed
    /// struct prints as its whole member list, so one edge row can carry
    /// kilobytes of it. On one design 16.3 MB of a 19.1 MB database was the
    /// `src_type` column holding 103 distinct values.
    int64_t internType(const std::string& text);
    int64_t internFile(const std::string& path);

    /// Interns an identifier -- a signal, a port, an instance name.
    ///
    /// These are short but repeat enormously: a child instance's name is
    /// written once per port it connects, which on one SoC meant 25.8 MB of
    /// text for 20207 distinct strings. Every identifier shares one table, so a
    /// name used as a port on one row and as a net on another is stored once.
    int64_t internName(const std::string& text);
    void addChildren(int64_t moduleId, const std::vector<ChildRow>& rows);
    void addPorts(int64_t moduleId, int64_t defModuleId, const std::vector<PortRow>& rows);
    void addSymbols(int64_t moduleId, const std::vector<SymbolRow>& rows);

    /// Writes one assignment with its operands, and returns its row id.
    /// Every edge event a procedure triggers on. An event list has no order, so
    /// all of them are recorded and none is singled out.
    void addProcEvents(int64_t moduleId, int64_t proc,
                       const std::vector<std::pair<std::string, std::string>>& events);

    int64_t addAssignment(int64_t moduleId, const AssignRow& row,
                          const std::vector<OperandRow>& operands);

    /// One instance-tree row.
    ///
    /// Holds the leaf name and a parent link rather than the full hierarchical
    /// path: the paths are all distinct by construction, so interning them saves
    /// nothing, while storing them costs the text twice over once the lookup
    /// index is counted. A consumer resolves `a.b.c` by walking down from the
    /// root a segment at a time against the (parent, name) index -- three
    /// indexed lookups, no recursion over the table.
    void addInstance(const std::string& name, int64_t moduleId, int64_t parentId,
                     int64_t rowId);

    /// Builds the indexes and closes. Indexes are created last: filling a table
    /// that already carries them costs far more than one build at the end.
    void finish();

private:
    void exec(const char* sql);
    void prepare(const char* sql, sqlite3_stmt** out);
    void step(sqlite3_stmt* stmt);
    void begin();
    void commit();

    sqlite3* db = nullptr;
    sqlite3_stmt* insEdge = nullptr;
    sqlite3_stmt* insChild = nullptr;
    sqlite3_stmt* insInstance = nullptr;
    sqlite3_stmt* insPort = nullptr;
    sqlite3_stmt* insSymbol = nullptr;
    sqlite3_stmt* insAssign = nullptr;
    sqlite3_stmt* insProcEvent = nullptr;
    sqlite3_stmt* insAssignOp = nullptr;
    std::unordered_map<std::string, int64_t> typeIds;
    std::unordered_map<std::string, int64_t> fileIds;
    std::unordered_map<std::string, int64_t> nameIds;
    bool inTransaction = false;
    int64_t pending = 0;
};

/// SHA-256 of a file, empty when it cannot be read.
std::string fileDigest(const std::string& path);

} // namespace designdb
