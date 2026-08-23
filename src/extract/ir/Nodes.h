// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// The statement-walk node stream: what one procedure's walk hands to whoever
// is filing it, as six self-contained node kinds instead of four callbacks
// with eleven positional parameters and a shared mutable gating stack.
//
// Each node owns everything about its statement. An assignment owns its
// TARGETS -- the old interface sent one callback per target with a
// `firstTarget` flag, and the receiver reconstructed statement identity from
// the flag; skipping a first target attached the second target of `{a,b} =
// ...` to the previous statement. A system task carries its reads and the
// targets it writes as two named fields -- the old interface overloaded one
// `writes` vector with "driven from outside the model" under one statement
// kind and "released, drives nothing" under another, disambiguated by
// string. The gate is an id into the walk's interned table rather than a
// vector re-sent with every target of every statement.
//
// Nodes still carry slang pointers (Ref does too); the adapter that walls
// slang types off is a later phase's, and until then this header sits in
// ir/ for its shape, not its purity.

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

/// The interned gating contexts of one procedure walk. A branch context is
/// pushed once and referenced by id however many statements it gates; the
/// old interface copied the stack into every emission. Interning is a linear
/// scan over content -- gate stacks are a handful of refs and one procedure
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

/// One assignment statement, owning its targets. `{a, b} = {x, y}` is one
/// node with two TargetRecords; `x++` is one node with one.
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

/// One actual bound to one formal at a call site. The formal is the symbol
/// and the actual is the reference -- the old interface folded both into one
/// Ref whose `sym` was the formal and whose `origin` was the actual's
/// expression, which two receiver-side comments document as a trap.
struct BindNode {
    const slang::ast::FormalArgumentSymbol* formal = nullptr;
    /// The actual as written; what the old folded Ref carried as `origin`.
    const slang::ast::Expression* actualOrigin = nullptr;
    Ref actual;
    bool reads = false;
    bool writes = false;
    bool oneToOne = false;
    bool bindable = true;
    slang::SourceRange where;
};

/// A statement-level event control -- a wait, not sensitivity.
struct EventNode {
    const slang::ast::Expression* expr = nullptr;
    std::string edge;
    int64_t seq = 0;
    slang::SourceRange where;
};

/// A statement whose whole effect is to read: an assertion, a wait
/// condition, a user call's own arguments.
struct ReadNode {
    enum class Kind { Assertion, Wait, Call };
    std::vector<Ref> reads;
    Kind kind = Kind::Call;
    std::string construct;
    GateId gate = 0;
    int64_t seq = 0;
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
    slang::SourceRange where;
};

/// `release` / `deassign`: names its lvalues and drives nothing.
struct ReleaseNode {
    std::vector<Ref> lvalues;
    bool isRelease = true;
    GateId gate = 0;
    int64_t seq = 0;
    slang::SourceRange where;
};

using Node = std::variant<AssignmentNode, BindNode, EventNode, ReadNode,
                          SystemTaskNode, ReleaseNode>;

} // namespace designdb::detail
