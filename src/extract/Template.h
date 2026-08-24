// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// The in-memory rows of one analyzed body, before they become database rows.
//
// A template is built once per (definition, parameter values) group and
// replayed once per elaborated occurrence, so every id in here is a
// template-local index -- an int32_t position in one of these vectors, never a
// database id. Stamping turns each into (index + that occurrence's base).
//
// Plain data: no slang types and no AST vocabulary, so a row cannot reach
// back into the compilation it was read from.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "extract/ir/Vocab.h"

namespace designdb::detail {

/// Holds one key in a set for the lifetime of a walk down, and takes it out
/// on the way back up -- a PATH set, which is what tells a cycle from a
/// module legitimately instantiated twice by one parent.
///
/// RAII rather than a matching erase at the end of the function, because the
/// two walks that use it are long and the cost of one missed erase is
/// silent: the path set becomes a VISITED set, and the guard starts cutting
/// legitimate sibling re-instantiation instead of recursion. `entered()`
/// says whether this call is the one that inserted the key; a false means
/// the key was already on the path, and nothing is erased on the way out.
template<typename Set>
class OnPath {
public:
    OnPath(Set& set, typename Set::value_type key)
        : set_(set), key_(std::move(key)),
          entered_(set.insert(key_).second) {}
    ~OnPath() {
        if (entered_)
            set_.erase(key_);
    }
    OnPath(const OnPath&) = delete;
    OnPath& operator=(const OnPath&) = delete;
    bool entered() const { return entered_; }

private:
    Set& set_;
    typename Set::value_type key_;
    bool entered_;
};


// Everything a group's occurrences share, held with template-local indices.
// Stamping is then arithmetic: global id = per-occurrence base + index. The
// invariant that makes this sound: two bodies with one (definition,
// parameters) key are the same AST, so any deterministic traversal of one is
// the same traversal of the other.

struct TplLoc {
    int64_t fileId = 0;
    uint32_t line = 0;
    uint32_t column = 0;
};

struct TplRange {
    std::optional<std::pair<uint64_t, uint64_t>> bits;
    bool exact = true;
};

struct TplScope {          // a generate level below the instance; 0 = the body
    int32_t parent = -1;
    std::string name;
};

struct TplNet {
    int32_t scope = 0;
    std::string name;
    std::string declKind;
    int64_t dataTypeId = 0;
    int64_t width = -1;
    bool isImplicit = false;
    TplLoc loc;
};

struct TplTerm {
    std::string name;
    TermKind kind = TermKind::Signal;
    Direction direction = Direction::None;
    int64_t dataTypeId = 0;
    int64_t width = -1;
    int isConst = -1;
    std::string modport;
    TplLoc loc;
};

struct TplTermMap {
    int32_t term = 0;
    int64_t ordinal = 0;
    int32_t net = 0;
    TplRange termR, netR;
    bool mappingExact = false;
};

struct TplProcedure {
    int32_t scope = 0;
    std::string name;
    ProcKind kind = ProcKind::Always;
    TplLoc loc;
};

struct TplStmt {
    int32_t scope = 0;
    int32_t proc = -1;
    int64_t sequence = -1;
    StmtKind kind = StmtKind::Assignment;
    /// Open domain: a system task's own name, a construct word.
    std::string construct;
    AssignKind assignKind = AssignKind::None;
    std::string delay;
    int64_t dropped = 0;
    int32_t callSite = -1;   // the call-site expansion this belongs to (-1 = none)
    TplLoc loc;
};

/// One subroutine-body expansion: a body is walked once per call site, and
/// this is that site's identity, so a consumer can partition a cone by it
/// and never mix one call's gating with another's argument. `callerStmt` is
/// the statement making the call; `parentCallSite` is the enclosing
/// expansion (a call-string for nested calls); ids are template-relative and
/// stamped per occurrence like everything else.
struct TplCallSite {
    int32_t callerStmt = -1;
    int32_t parentCallSite = -1;
    std::string subName;
    int64_t depth = 0;
};

struct TplStmtRef {          // stmt_target and assign_operand share the shape
    int32_t stmt = 0;
    int64_t ordinal = 0;
    int32_t net = 0;
    TplRange r;
};

struct TplExprRef {
    int32_t stmt = 0;
    int64_t ordinal = 0;
    int32_t net = 0;
    RefRole role = RefRole::Control;
    TplRange r;
};

struct TplProcEvent {
    int32_t proc = 0;
    int32_t stmt = -1;
    int32_t net = -1;
    EventKind eventKind = EventKind::Sensitivity;
    Edge edgeKind = Edge::None;
    TplLoc loc;
};

struct TplPrim {
    int32_t scope = 0;
    std::string name;
    PrimKind primKind = PrimKind::Gate;
    std::string defName;
    TplLoc loc;
};

/// One end of a dependency: a net of this instance, a reference that leaves
/// it (a hierRefs index), or nothing at all -- a constant source, or the
/// deliberate no-source of a system task's write. Never both at once.
struct TplEnd {
    int32_t net = -1;
    int32_t href = -1;

    bool outward() const { return href >= 0; }
};

/// One dependency, paired where the statement was walked -- per (source
/// element, target element), never by joining afterwards. A row whose ends
/// are all local stamps inline with its occurrence's reserved id block; a
/// row with an outward end is deferred until the final pass, when the
/// occurrence's references have resolved. `deferred()` is that split.
struct TplDep {
    TplEnd src, tgt;
    /// True when the dependency has no source BY DESIGN -- a system task
    /// writing across the boundary. Without it, "no source reference" and
    /// "the source reference did not resolve" look alike, and the second
    /// must be dropped while the first must not.
    bool sourceless = false;
    int32_t stmt = -1;
    int32_t operandRef = -1;
    int32_t targetRef = -1;
    int32_t exprRef = -1;
    int32_t prim = -1;
    DepKind kind = DepKind::Data;
    TplRange srcR, tgtR;
    int mappingExact = -1;
    int32_t callSite = -1;   // the call-site expansion, or -1 at module level

    bool deferred() const { return src.outward() || tgt.outward(); }
};

/// Replay data for one outward reference: how to find the target from an
/// occurrence of this template. `kind` decides the walk.
struct TplHierRef {
    int32_t stmt = -1;
    std::string path;
    Access access = Access::Read;
    TplRange r;
    TplLoc loc;
    /// How the reference resolves per occurrence -- or why it does not.
    /// The first three all stamp NULL resolved ids, and are spelled apart
    /// because "nothing to resolve", "tried and could not" and "must not
    /// guess" are different facts about the same NULL.
    enum ResolveKind {
        NotHierarchical,     // a bare or package-free name; nothing to walk
        Failed,              // a target existed and no replay could be built
        Upward,              // climbs out of the analysed body; a guess is
                             // worse than a NULL, so deliberately unresolved
        Downward,            // segs descend from the occurrence's own node
        Absolute,            // segs descend from the design root
        ViaIfaceTerm,        // segs descend from the interface bound to term
        Package              // segs[0] names a package; netName its member
    } resolve = NotHierarchical;
    int32_t ifaceTerm = -1;  // ViaIfaceTerm: which of this template's terms
    std::vector<std::string> segs;   // tree segments to descend
    std::string netName;     // scope-relative net name at the target instance

    bool resolvable() const { return resolve >= Downward; }
};

struct TplConn {
    ConnKind kind = ConnKind::Signal;
    int32_t parentNet = -1;  // index into the PARENT template's nets
    int32_t childTerm = -1;  // index into the child template's terms
    int64_t ordinal = 0;     // segment ordinal within that terminal
    TplRange netR;
    int netExact = -1;       // -1 = no net end (writer NULLs the range too)
    TplRange termR;
    int termExact = -1;
    int mappingExact = -1;
    int32_t ifaceChild = -1; // interface binding to a sibling child
    int32_t ifaceOwnTerm = -1; // interface pass-through of the parent's port
    int32_t hierRef = -1;    // external tie: index into parent's hierRefs
    TplLoc loc;
};

struct TplChild {
    int32_t scope = 0;
    std::string name;        // ONE path segment
    enum Kind { Module, Unresolved } kind = Module;
    std::string groupKey;    // Module: which template to stamp
    std::string defName;     // Unresolved: the definition as written
    std::vector<std::string> unresolvedPorts;  // Unresolved: term names in order
    std::vector<TplConn> conns;
    TplLoc loc;
};

struct Template {
    int64_t moduleId = 0;
    std::string params;
    std::vector<std::pair<std::string, std::string>> paramPairs;
    std::vector<TplScope> scopes;
    std::vector<TplNet> nets;
    std::vector<TplTerm> terms;
    std::vector<TplTermMap> termMaps;
    std::vector<TplProcedure> procedures;
    std::vector<TplStmt> stmts;
    std::vector<TplCallSite> callSites;
    std::vector<TplStmtRef> targets;
    std::vector<TplStmtRef> operands;
    std::vector<TplExprRef> exprRefs;
    std::vector<TplProcEvent> procEvents;
    std::vector<TplPrim> prims;
    std::vector<TplDep> deps;
    std::vector<TplHierRef> hierRefs;
    std::vector<TplChild> children;
    // The port-symbol -> terminal-slot map moved to the builder (pass-1
    // state, slang-keyed); its story -- terminals found by symbol, never by
    // name -- travels with TermSlot in TemplateBuilderImpl.h.
    std::unordered_map<std::string, int32_t> netIndex;   // name -> nets index
    bool hasResolvableRefs = false;
    /// Whether the body this template was built from had an AnalyzedScope.
    /// False means every procedure of the module is absent from it, so each
    /// occurrence stamped from it carries hierarchy and connections and no
    /// procedural dataflow at all.
    bool analysedBody = false;
    bool built = false;
};

} // namespace designdb::detail
