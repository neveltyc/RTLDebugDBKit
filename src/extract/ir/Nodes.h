// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// The statement-walk node stream: what one procedure's walk hands to whoever
// files it.
//
// Each node owns everything about its statement -- an assignment owns its
// targets, so statement identity is the node rather than a flag on the first
// of several calls; a system task names its reads and its writes apart. The
// branch is an id into the walk's BranchTable, so a branch context is stored
// once however many statements it gates.
//
// Nodes carry slang pointers, as Ref does: a hier_ref's text is recovered
// from the expression it was written as, so the AST has to stay reachable
// from a filed node.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/text/SourceLocation.h"

#include "../Ref.h"
#include "Vocab.h"

namespace designdb::detail {

/// Index into the walk's BranchTable; -1 is ungated.
using BranchId = int32_t;

/// One level of the gating context, as the walk pushed it.
///
/// The level is identified by WHERE it was pushed, never by what it reads.
/// Interning gate stacks by content -- what v18 did -- gave the two arms of
/// one `if` a single id, since both carry exactly the condition's
/// references, and no consumer could then tell a then-arm assignment from an
/// else-arm one.
struct BranchFrame {
    BranchId parent = -1;
    int32_t depth = 1;
    /// Position among the arms of one case point, in written order; -1 on
    /// every level that is not an arm. It is what makes a plain `case`'s
    /// priority readable -- arm k runs only if arms 0..k-1 did not match --
    /// which source line cannot when the arms share one.
    int32_t ordinal = -1;
    BranchKind kind = BranchKind::If;
    BranchSense sense = BranchSense::None;
    CaseKind caseKind = CaseKind::None;
    CheckKind check = CheckKind::None;
    /// The compile-time verdict for this arm: 1 taken, 0 unreachable, -1
    /// when the condition is not a constant. A statically false arm keeps
    /// its rows -- the statement is in the elaborated design and a source
    /// view shows it -- and says so here.
    int staticTaken = -1;
    /// The iteration variable of a `for`/`foreach`; null for the loops that
    /// have none, and on every non-loop level.
    const slang::ast::ValueSymbol* iterVar = nullptr;
    /// How many times the body runs, or -1 when that is not statically
    /// known. `first`/`step` describe the values the variable takes and are
    /// published only when they form an arithmetic progression, which
    /// `hasProgression` says.
    int64_t iterCount = -1;
    int64_t iterFirst = 0;
    int64_t iterStep = 0;
    bool hasProgression = false;
    /// This level's own control reads: an `if`'s condition, a case point's
    /// selector, one item's labels, a loop's guard.
    std::vector<Ref> refs;
    /// A case item's labels, evaluated. nullopt is a label no constant
    /// evaluation reaches -- an `inside` range over a variable -- whose
    /// reads are in `refs` like any other.
    std::vector<std::optional<std::string>> labels;
    slang::SourceRange where;
};

/// One control read together with the level that contributed it.
struct GateRef {
    Ref ref;
    BranchId branch = -1;
    /// Where the LEVEL is written, not the statement it gates: the condition
    /// is read once where it stands, however many statements sit under it.
    SourceRange where;
};

/// The branch levels of one procedure walk, as a tree. Levels are only ever
/// appended, so an index stays valid for the whole walk and the parent link
/// is the nesting.
class BranchTable {
public:
    BranchId add(BranchFrame frame) {
        frames_.push_back(std::move(frame));
        return BranchId(frames_.size() - 1);
    }

    const BranchFrame& at(BranchId id) const { return frames_[size_t(id)]; }
    size_t size() const { return frames_.size(); }

    /// The reads of the chain ending at `id`, outermost level first, each
    /// naming the level it came from. The order is the order the walk
    /// pushed them in, which is the order the flat gating stack had.
    std::vector<GateRef> chain(BranchId id) const {
        std::vector<BranchId> levels;
        for (BranchId cur = id; cur >= 0; cur = frames_[size_t(cur)].parent)
            levels.push_back(cur);
        std::vector<GateRef> out;
        for (auto it = levels.rbegin(); it != levels.rend(); ++it)
            for (auto& r : frames_[size_t(*it)].refs)
                out.push_back(GateRef{r, *it, frames_[size_t(*it)].where});
        return out;
    }

private:
    std::vector<BranchFrame> frames_;
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
    BranchId branch = -1;
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
    /// The actual is a constant, so `pair` names no source: the formal
    /// is tied off rather than fed by a net.
    bool constantActual = false;
    slang::SourceRange where;
};

/// A statement-level event control -- a wait, not sensitivity.
struct EventNode {
    const slang::ast::Expression* expr = nullptr;
    Edge edge = Edge::None;
    BranchId branch = -1;
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
    BranchId branch = -1;
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
    BranchId branch = -1;
    int64_t seq = 0;
    int64_t dropped = 0;
    slang::SourceRange where;
};

/// `-> ev`: names the event it triggers. The cause is control reaching
/// the statement, which is no net, so the arc has no source -- the shape
/// a system task's write already has.
struct TriggerNode {
    std::vector<Ref> events;
    BranchId branch = -1;
    int64_t seq = 0;
    int64_t dropped = 0;
    slang::SourceRange where;
};

/// `release` / `deassign`: names its lvalues and drives nothing.
struct ReleaseNode {
    std::vector<Ref> lvalues;
    bool isRelease = true;
    BranchId branch = -1;
    int64_t seq = 0;
    int64_t dropped = 0;
    slang::SourceRange where;
};

using Node = std::variant<AssignmentNode, BindNode, EventNode, ReadNode,
                          SystemTaskNode, TriggerNode, ReleaseNode>;

} // namespace designdb::detail
