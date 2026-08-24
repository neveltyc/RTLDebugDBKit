// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// The statement-walk node stream: what one procedure's walk hands to whoever
// files it.
//
// Each node owns everything about its statement -- an assignment owns its
// targets, so statement identity is the node rather than a flag on the first
// of several calls; a system task names its reads and its writes apart. The
// gate is an id into the walk's interned table, so a branch context is stored
// once however many statements it gates.
//
// Nodes carry slang pointers, as Ref does: a hier_ref's text is recovered
// from the expression it was written as, so the AST has to stay reachable
// from a filed node.

#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/text/SourceLocation.h"

#include "../Ref.h"

namespace designdb::detail {

/// Index into the walk's gate table. Every node carries one; 0 is always the
/// empty gate, so "ungated" needs no special case.
using GateId = int32_t;

inline bool sameRef(const Ref& a, const Ref& b) {
    return a.sym == b.sym && a.origin == b.origin && a.exact == b.exact &&
           a.cover == b.cover;
}

/// The interned gating contexts of one procedure walk. Interning is a linear
/// scan over content: gate stacks hold a handful of refs and one procedure
/// interns a handful of contexts, so an index would cost more than it saves.
class GateTable {
public:
    GateTable() : sets_(1) {}   // id 0: the empty gate

    GateId intern(const std::vector<Ref>& gate) {
        for (size_t i = 0; i < sets_.size(); i++) {
            if (sets_[i].size() == gate.size() &&
                std::equal(sets_[i].begin(), sets_[i].end(), gate.begin(),
                           sameRef))
                return GateId(i);
        }
        sets_.push_back(gate);
        return GateId(sets_.size() - 1);
    }

    const std::vector<Ref>& refs(GateId id) const { return sets_[size_t(id)]; }

private:
    std::vector<std::vector<Ref>> sets_;
};

/// One assignment target with the operands paired onto its bits.
struct TargetRecord {
    Ref dst;
    std::vector<PairedSrc> pairs;
};

/// One assignment statement and every target it writes: `{a, b} = {x, y}` is
/// one node with two TargetRecords.
struct AssignmentNode {
    std::vector<TargetRecord> targets;
    GateId gate = 0;
    slang::SourceRange where;
    int64_t seq = 0;
    bool blocking = false;
    int64_t dropped = 0;
    bool inSubroutine = false;
    std::string delay;
    /// The construct word a `force`/procedural `assign` stamps; null means
    /// the receiver's own word for the procedure.
    const char* constructWord = nullptr;
};

/// One actual bound to one formal at a call site. The formal is a symbol and
/// the actual a reference: at a call site the formal has no expression of its
/// own, so it has no reference text and must never be resolved as if it did.
struct BindNode {
    const slang::ast::FormalArgumentSymbol* formal = nullptr;
    const slang::ast::Expression* actualOrigin = nullptr;
    /// The actual's bits paired with the formal's, as an assignment's
    /// operands are paired with its target: `t({hi, lo})` fills two windows
    /// of one formal, and each element reaches only its own.
    PairedSrc pair;
    bool reads = false;
    bool writes = false;
    bool bindable = true;
    slang::SourceRange where;
};

/// A statement-level event control -- a wait, not sensitivity.
struct EventNode {
    const slang::ast::Expression* expr = nullptr;
    Edge edge = Edge::None;
    int64_t seq = 0;
    slang::SourceRange where;
};

/// A statement whose whole effect is to read: an assertion, a wait
/// condition, a user call's own arguments. `dropped` counts the operands
/// filtered as compile-time constants.
struct ReadNode {
    enum class Kind { Assertion, Wait, Call, Disable };
    std::vector<Ref> reads;
    Kind kind = Kind::Call;
    std::string construct;
    GateId gate = 0;
    int64_t seq = 0;
    int64_t dropped = 0;
    slang::SourceRange where;
};

/// A system task: what it reads, and the arguments it writes -- a write
/// whose source is a file or a plusarg, outside anything the model names.
struct SystemTaskNode {
    std::vector<Ref> reads;
    std::vector<Ref> writes;
    std::string construct;
    GateId gate = 0;
    int64_t seq = 0;
    int64_t dropped = 0;
    slang::SourceRange where;
};

/// `-> ev`: names the event it triggers. The cause is control reaching
/// the statement, which is no net, so the arc has no source -- the shape
/// a system task's write already has.
struct TriggerNode {
    std::vector<Ref> events;
    GateId gate = 0;
    int64_t seq = 0;
    int64_t dropped = 0;
    slang::SourceRange where;
};

/// `release` / `deassign`: names its lvalues and drives nothing.
struct ReleaseNode {
    std::vector<Ref> lvalues;
    bool isRelease = true;
    GateId gate = 0;
    int64_t seq = 0;
    int64_t dropped = 0;
    slang::SourceRange where;
};

using Node = std::variant<AssignmentNode, BindNode, EventNode, ReadNode,
                          SystemTaskNode, TriggerNode, ReleaseNode>;

} // namespace designdb::detail
