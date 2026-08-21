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
// Plain data: no slang types at all, and the only thing it knows about the AST
// is Ref, for rangeOf.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "extract/Ref.h"

namespace designdb::detail {

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

inline TplRange rangeOf(const Ref& r) {
    TplRange out;
    if (r.sym && !r.whole)
        out.bits = std::make_pair(r.lo, r.hi);
    out.exact = r.sym ? r.exact : true;
    return out;
}

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
    std::string kind;        // signal | interface
    std::string direction;   // "" = NULL
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
    std::string kind;
    TplLoc loc;
};

struct TplStmt {
    int32_t scope = 0;
    int32_t proc = -1;
    int64_t sequence = -1;
    std::string kind;
    std::string construct;
    std::string assignKind;  // "" = NULL
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
    std::string role;
    TplRange r;
};

struct TplProcEvent {
    int32_t proc = 0;
    int32_t stmt = -1;
    int32_t net = -1;
    std::string eventKind;
    std::string edgeKind;
    TplLoc loc;
};

struct TplPrim {
    int32_t scope = 0;
    std::string name;
    std::string primKind;
    std::string defName;
    TplLoc loc;
};

struct TplDep {
    int32_t srcNet = -1;     // -1 = constant source
    int32_t tgtNet = 0;
    int32_t stmt = -1;
    int32_t operandRef = -1;
    int32_t targetRef = -1;
    int32_t exprRef = -1;
    int32_t prim = -1;
    std::string kind;
    TplRange srcR, tgtR;
    int mappingExact = -1;
    int32_t callSite = -1;   // the call-site expansion, or -1 at module level
};

/// Replay data for one outward reference: how to find the target from an
/// occurrence of this template. `kind` decides the walk.
struct TplHierRef {
    int32_t stmt = -1;
    std::string path;
    std::string access;      // read | write | connect
    TplRange r;
    TplLoc loc;
    enum ResolveKind {
        None,                // slang gave no target usable per occurrence
        Downward,            // segs descend from the occurrence's own node
        Absolute,            // segs descend from the design root
        ViaIfaceTerm,        // segs descend from the interface bound to term
        Package              // segs[0] names a package; netName its member
    } resolve = None;
    int32_t ifaceTerm = -1;  // ViaIfaceTerm: which of this template's terms
    std::vector<std::string> segs;   // tree segments to descend
    std::string netName;     // scope-relative net name at the target instance
};

/// One dependency with at least one end outside the instance, paired where
/// the statement was walked -- per (source element, target element), never
/// by joining afterwards -- and materialised once the occurrence's
/// references resolve. An end is a local net index or a hierRefs index,
/// never both.
struct TplCrossDep {
    std::string kind;        // data | control | procedure
    /// True when the dependency has no source BY DESIGN -- a system task
    /// writing across the boundary. Without it, "no source reference" and
    /// "the source reference did not resolve" look alike, and the second
    /// must be dropped while the first must not.
    bool sourceless = false;
    int32_t stmt = -1;
    int32_t srcNet = -1;
    int32_t srcHref = -1;
    int32_t tgtNet = -1;
    int32_t tgtHref = -1;
    int32_t operandRef = -1;
    int32_t targetRef = -1;
    int32_t exprRef = -1;
    TplRange srcR, tgtR;
    int mappingExact = -1;
    int32_t callSite = -1;   // the call-site expansion, or -1 at module level
};

struct TplConn {
    std::string kind;        // net_conn.conn_kind
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
    std::vector<TplCrossDep> crossDeps;
    std::vector<TplChild> children;
    /// Where a port symbol sits in this template's terminals.
    ///
    /// Terminals are found by symbol, never by name -- there was a name map
    /// here and every one of its lookups was wrong in some case. A port name
    /// is not unique (two unnamed ports collapse onto one synthesized
    /// `<unnamed>`), and for a MultiPort it is not even the name the
    /// connection carries: slang's expandMultiPortConn hands back one
    /// PortConnection per MEMBER, so `.p({hi, lo})` arrives as `hi` and `lo`,
    /// neither of which is a terminal. Identity works for both.
    ///
    /// `lsb` and `width` describe the SYMBOL, not the terminal: a MultiPort
    /// member occupies its own window of the terminal it belongs to, and a
    /// connection's bits are relative to that window.
    struct TermSlot {
        int32_t term = -1;
        uint64_t lsb = 0;
        int64_t width = -1;
    };
    std::unordered_map<const void*, TermSlot> termOf;
    std::unordered_map<std::string, int32_t> netIndex;   // name -> nets index
    bool hasResolvableRefs = false;
    bool built = false;
};

} // namespace designdb::detail
