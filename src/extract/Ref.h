// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// A reference to some bits of one object, and the machinery that turns an
// expression into them.
//
// This is the vocabulary the statement walker and the template builder share:
// every dependency end, every assignment side and every port segment is a Ref,
// and pairing two sides means overlapping their Slots. Ranges use the schema's
// encoding throughout -- LSB-relative offsets into the flattened object, never
// declared indices.
//
// Header-only on purpose. slotsOverlap runs once per (target slot, source slot)
// pair, which is the innermost loop in the whole extractor; keeping the layer
// inline means the split cannot have changed its code generation at all.

#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/Expression.h"
#include "slang/ast/ValuePath.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/Type.h"
#include "slang/numeric/ConstantValue.h"
#include "slang/numeric/SVInt.h"

#include "ir/Bits.h"
#include "ir/PositionWalk.h"

namespace designdb::detail {

using namespace slang;
using namespace slang::ast;

/// True for a symbol that is a compile-time constant rather than a net.
///
/// An enum member or a parameter is not something a waveform carries and not
/// something a trace can step to, so it is not connectivity. Leaving them in
/// also swamped the count of genuinely dropped cross-module references.
inline bool isConstantSymbol(const ValueSymbol& sym) {
    switch (sym.kind) {
        case SymbolKind::EnumValue:
        case SymbolKind::Parameter:
        case SymbolKind::Specparam:
            return true;
        case SymbolKind::Variable:
            return sym.as<VariableSymbol>().flags.has(VariableFlags::Const);
        default:
            return false;
    }
}

struct Ref {
    const ValueSymbol* sym = nullptr;
    /// Which bits of the object this reference covers. Unknown until
    /// something computes bounds: a reference nobody has bounded must not
    /// read as one that covers everything.
    BitInterval cover = BitInterval::unknown();
    bool exact = false;
    /// The expression the reference was written as; consulted when the symbol
    /// lives outside the instance (hier_ref text) and for resolution replay.
    const Expression* origin = nullptr;
};

/// The width the schema's bit offsets index into: slang's selectable space,
/// which flattens an unpacked aggregate where getBitWidth -- the PACKED
/// width -- answers zero. Positioning arithmetic must use this one, or a
/// window computed here cannot be compared with a range recorded there.
inline uint64_t flattenedWidth(const Type& type) {
    const Type& ct = type.getCanonicalType();
    // An unpacked aggregate is flattened; everything else is its packed
    // width, and a type with no bits has none. slang's getSelectableWidth
    // answers 1 for that last case so a non-aggregate always has one
    // selectable element -- a count, not a width, and positioning against
    // it puts a void replication operand or an unelaborated port on a bit
    // it does not have.
    if (ct.isUnpackedArray() || ct.isUnpackedStruct() || ct.isUnpackedUnion())
        return ct.getSelectableWidth();
    return ct.getBitWidth();
}

inline uint64_t bitWidthOf(const ValueSymbol& sym) {
    return flattenedWidth(sym.getType());
}

/// Builds a Ref from one of slang's value paths; nullopt when the path has
/// no root symbol to refer to.
inline std::optional<Ref> refOf(const ValuePath& path) {
    const ValueSymbol* sym = path.rootSymbol();
    if (!sym)
        return std::nullopt;
    Ref r;
    r.sym = sym;
    r.origin = path.fullExpr;
    if (!path.lsp)
        return r;
    r.exact = path.isFullyStatic();
    r.cover = BitInterval::forBounds(path.lspBounds.first, path.lspBounds.second,
                                     bitWidthOf(*sym));
    return r;
}

/// Constants filtered per statement; see the v9 note on why accumulated
/// across the three collection passes of one assignment.
inline thread_local int64_t filteredConstants = 0;

inline void collectRefs(const Expression& expr, EvalContext& ctx, std::vector<Ref>& out,
                        bool skipSelectors = false) {
    ValuePath::visitPaths(
        expr, ctx,
        [&](const ValuePath& path) {
            auto r = refOf(path);
            if (!r)
                return;
            if (isConstantSymbol(*r->sym)) {
                filteredConstants++;
                return;
            }
            out.push_back(*r);
        },
        skipSelectors);
}

/// A reference together with the bits of the *assignment* it occupies -- what
/// makes `{a, b} = {x, y}` answerable without the v7 cross product.
struct Slot {
    Ref ref;
    /// The window of the enclosing assignment this reference sits in, or
    /// nullopt when the walk could not place it.
    std::optional<BitRange> pos;
    /// True when the reference's own bits map one-to-one onto `pos`.
    bool positional = false;

    static Slot unpositioned(Ref r) { return Slot{std::move(r), std::nullopt, false}; }
    static Slot at(Ref r, BitRange window, bool positional) {
        return Slot{std::move(r), window, positional};
    }
};

/// How many subroutine bodies one module may instantiate across all its
/// procedures.
///
/// Per-call-site expansion is what makes a call's gating and delay its own,
/// but the cycle guard bounds only recursion, not fan-out: a call DAG
/// branching twice per level costs 2^depth, and 21 such levels turned an
/// 88-line file into 3.1 M statements and 1.2 GB. This bounds that.
///
/// The number is set from measurement, not taste: of the designs exported
/// here, the heaviest user of calls is picorv32 with 13 call statements,
/// and tinyriscv and VeeRwolf have none. Four thousand is two orders of
/// magnitude above that, so real RTL -- and any testbench short of a
/// deliberately exponential one -- never reaches it. What it stops is
/// counted and reported, never silently dropped.
constexpr int64_t kCallExpansionBudget = 4000;

inline uint64_t exprWidthOf(const Expression& e) {
    return e.type ? flattenedWidth(*e.type) : 0;
}

/// Whether the expression *is* a reference to storage rather than a
/// computation over one; selects and width-preserving conversions are
/// transparent, everything else answers no.
inline bool isPlainReference(const Expression& e) {
    const Expression* p = &e;
    for (;;) {
        switch (p->kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue:
                return true;
            case ExpressionKind::ElementSelect:
                p = &p->as<ElementSelectExpression>().value();
                break;
            case ExpressionKind::RangeSelect:
                p = &p->as<RangeSelectExpression>().value();
                break;
            case ExpressionKind::MemberAccess:
                p = &p->as<MemberAccessExpression>().value();
                break;
            case ExpressionKind::Conversion: {
                auto& conv = p->as<ConversionExpression>();
                if (exprWidthOf(conv.operand()) != exprWidthOf(*p))
                    return false;
                p = &conv.operand();
                break;
            }
            default:
                return false;
        }
    }
}

/// Every reference in `expr`, tagged with the bits of the assignment it
/// occupies. Concatenations and simple assignment patterns are positioned
/// element by element, MSB first; conversions are transparent when width-
/// preserving or truncating and degrade to range-level when widening.
///
/// The element loop, its zero-width rule and its overflow guard live in
/// ir/PositionWalk.h, shared with every other positioned walk. The recovery
/// here: the positional slots this level already emitted are dropped -- a
/// walk that has lost the cursor cannot vouch for them, and appending would
/// leave one reference in `out` twice with two contradictory windows -- and
/// the whole expression rides unpositioned instead. No well-formed
/// expression reaches it (slang wraps width mismatches in Conversions), but
/// the wrap it prevents handed out bit ranges near 2^64 before 728b07b.
inline void collectSlots(const Expression& expr, EvalContext& ctx, uint64_t base,
                         std::vector<Slot>& out, bool skipSelectors = false) {
    const uint64_t width = exprWidthOf(expr);

    if (isElementwise(expr) && width) {
        const size_t mark = out.size();
        walkElements(
            expr, base, width, exprWidthOf,
            [&](const Expression& op, BitRange window) {
                collectSlots(op, ctx, window.lo, out, skipSelectors);
            },
            [&]() {
                out.resize(mark);
                std::vector<Ref> rest;
                collectRefs(expr, ctx, rest, skipSelectors);
                for (auto& r : rest)
                    out.push_back(Slot::unpositioned(r));
            },
            [](const Expression&) { /* the fallback above covered the rest */ });
        return;
    }

    if (expr.kind == ExpressionKind::Conversion) {
        auto& conv = expr.as<ConversionExpression>();
        const uint64_t iw = exprWidthOf(conv.operand());
        if (width && iw >= width) {
            collectSlots(conv.operand(), ctx, base, out, skipSelectors);
            return;
        }
    }

    std::vector<Ref> refs;
    collectRefs(expr, ctx, refs, skipSelectors);
    if (!width) {
        for (auto& r : refs)
            out.push_back(Slot::unpositioned(r));
        return;
    }
    const bool positional =
        isPlainReference(expr) && refs.size() == 1 &&
        coverFillsWidth(refs[0].cover, refs[0].exact, width,
                        bitWidthOf(*refs[0].sym));
    for (auto& r : refs)
        out.push_back(Slot::at(r, BitRange(base, base + width - 1), positional));
}

/// Whether two slots of one assignment touch the same bits. False means
/// disjoint; true with a window means exactly that overlap; true with
/// nullopt means either side was never placed, so the overlap is unbounded.
inline bool slotsOverlap(const Slot& a, const Slot& b, std::optional<BitRange>& span) {
    if (!a.pos || !b.pos) {
        span = std::nullopt;
        return true;
    }
    const uint64_t lo = std::max(a.pos->lo, b.pos->lo);
    const uint64_t hi = std::min(a.pos->hi, b.pos->hi);
    if (lo > hi)
        return false;
    span = BitRange(lo, hi);
    return true;
}

/// The reference narrowed to the part of it landing in `span` of the
/// assignment. Only meaningful for a positional slot.
inline Ref narrowed(const Slot& s, const std::optional<BitRange>& span) {
    Ref r = s.ref;
    if (!s.positional || !s.pos || !span || !r.sym)
        return r;
    if (span->lo <= s.pos->lo && span->hi >= s.pos->hi)
        return r;
    // A positional slot's reference is exact -- that is what positional
    // asserts -- so its cover is Whole or Range, never Unknown, and a whole
    // cover starts at bit zero.
    const uint64_t offset = r.cover.isRange() ? r.cover.lo() : 0;
    r.cover = BitInterval::forBounds(offset + (span->lo - s.pos->lo),
                                     offset + (span->hi - s.pos->lo),
                                     bitWidthOf(*r.sym));
    return r;
}

struct StatementRefCollector : ASTVisitor<StatementRefCollector, VisitFlags::AllGood> {
    std::vector<Ref>& out;
    explicit StatementRefCollector(std::vector<Ref>& out) : out(out) {}
    void handle(const NamedValueExpression& e) { addRef(e); }
    void handle(const HierarchicalValueExpression& e) { addRef(e); }
    /// An assignment's target is written, not read. Visiting it as an
    /// ordinary value made a subroutine's *writes* come back as reads of
    /// the call site: `task touch(); freewr = freerd; endtask` reported
    /// freewr as read at the call, though nothing in the design reads it,
    /// and the same database showed it with a single driver -- two rows
    /// contradicting each other. The selectors on the left ARE reads
    /// (`m[i] = x` reads i), so they are still visited.
    void handle(const AssignmentExpression& e) {
        collectLeftSelectorReads(e.left());
        e.right().visit(*this);
    }
    void collectLeftSelectorReads(const Expression& lhs) {
        switch (lhs.kind) {
            case ExpressionKind::ElementSelect: {
                auto& sel = lhs.as<ElementSelectExpression>();
                sel.selector().visit(*this);
                collectLeftSelectorReads(sel.value());
                return;
            }
            case ExpressionKind::RangeSelect: {
                auto& sel = lhs.as<RangeSelectExpression>();
                sel.left().visit(*this);
                sel.right().visit(*this);
                collectLeftSelectorReads(sel.value());
                return;
            }
            case ExpressionKind::MemberAccess:
                collectLeftSelectorReads(lhs.as<MemberAccessExpression>().value());
                return;
            case ExpressionKind::Concatenation:
                for (auto* op : lhs.as<ConcatenationExpression>().operands())
                    collectLeftSelectorReads(*op);
                return;
            default:
                return;
        }
    }
    void addRef(const ValueExpressionBase& e) {
        Ref r;
        r.sym = &e.symbol;
        r.origin = &e;
        // No bounds computed here; unknown() is the default and the honest
        // answer.
        out.push_back(r);
    }
};

template<typename NodeT>
inline void collectStatementRefs(const NodeT& node, std::vector<Ref>& out) {
    StatementRefCollector c(out);
    node.visit(c);
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const Ref& r) {
                                 if (!r.sym)
                                     return true;
                                 if (isConstantSymbol(*r.sym)) {
                                     filteredConstants++;
                                     return true;
                                 }
                                 return false;
                             }),
              out.end());
}

/// Whether `sym` is declared inside `sub` -- a subroutine's own argument,
/// local or block variable, as opposed to something its body samples from
/// the enclosing design.
///
/// By walking the scope chain, not by comparing hierarchical-path strings.
/// The string test asked whether the symbol's path started with the
/// subroutine's path plus a dot, which is two path constructions per symbol
/// per call site and is only as reliable as the paths are injective: a
/// subroutine named `f` and a sibling scope named `f_aux` share a prefix
/// until the dot saves them, and an escaped identifier holding a dot has no
/// such guarantee at all. The chain is exact and stops at the first hit.
inline bool declaredInside(const Symbol& sym, const SubroutineSymbol& sub) {
    for (auto* scope = sym.getParentScope(); scope;
         scope = scope->asSymbol().getParentScope()) {
        auto& owner = scope->asSymbol();
        if (&owner == &sub)
            return true;
        // Statement blocks nest inside the body; anything at or above the
        // enclosing instance is not the subroutine's own.
        if (owner.kind == SymbolKind::InstanceBody ||
            owner.kind == SymbolKind::CompilationUnit)
            return false;
    }
    return false;
}

/// A called subroutine's free reads -- what it samples beyond its arguments.
inline void collectCallReadsInto(const Expression& expr,
                                 std::set<const SubroutineSymbol*>& active,
                                 std::vector<Ref>& out) {
    struct CallFinder : ASTVisitor<CallFinder, VisitFlags::AllGood> {
        std::set<const SubroutineSymbol*>& active;
        std::vector<Ref>& out;
        CallFinder(std::set<const SubroutineSymbol*>& active, std::vector<Ref>& out) :
            active(active), out(out) {}
        void handle(const CallExpression& call) {
            visitDefault(call);
            auto sub = std::get_if<const SubroutineSymbol*>(&call.subroutine);
            if (!sub || !*sub)
                return;
            if (!active.insert(*sub).second)
                return;
            std::vector<Ref> inner;
            collectStatementRefs((*sub)->getBody(), inner);
            for (auto& r : inner) {
                if (!declaredInside(*r.sym, **sub))
                    out.push_back(r);
            }
            active.erase(*sub);
        }
    };
    CallFinder finder(active, out);
    expr.visit(finder);
}

/// The value symbols an expression reads, subroutine free reads included.
struct ReadCollector : public ASTVisitor<ReadCollector, VisitFlags::AllGood> {
    std::vector<const ValueSymbol*>& out;
    std::set<const SubroutineSymbol*>& active;
    explicit ReadCollector(std::vector<const ValueSymbol*>& out,
                           std::set<const SubroutineSymbol*>& active) :
        out(out), active(active) {}

    void handle(const NamedValueExpression& e) {
        if (!isConstantSymbol(e.symbol))
            out.push_back(&e.symbol);
    }
    void handle(const HierarchicalValueExpression& e) {
        if (!isConstantSymbol(e.symbol))
            out.push_back(&e.symbol);
    }
    void handle(const CallExpression& e) {
        visitDefault(e);
        auto sub = std::get_if<const SubroutineSymbol*>(&e.subroutine);
        if (!sub || !*sub)
            return;
        if (!active.insert(*sub).second)
            return;
        std::vector<const ValueSymbol*> inner;
        ReadCollector c(inner, active);
        (*sub)->getBody().visit(c);
        for (auto* sym : inner) {
            if (!declaredInside(*sym, **sub))
                out.push_back(sym);
        }
        active.erase(*sub);
    }
};

inline void collectReads(const Expression& expr, std::vector<const ValueSymbol*>& out) {
    std::set<const SubroutineSymbol*> active;
    ReadCollector c(out, active);
    expr.visit(c);
}

/// One operand paired with the part of the target its bits actually reach.
///
/// Both ends are narrowed to the overlap, not just the source. Keeping the
/// target whole while narrowing the source is what made
/// `assign swap = {c[3:0], c[7:4]}` export two dependencies each claiming
/// all eight bits of swap with map_exact=1 -- a four-bit source cannot
/// map one-to-one onto an eight-bit target, so the row was not merely
/// coarse but impossible, and it said the bytes were not swapped.
struct PairedSrc {
    Ref src;
    Ref tgt;
    bool mapExact = false;
    /// The source as the RTL spells it, before pairing narrowed it to this
    /// target's bits. Only a hier_ref row wants this: it describes the
    /// reference, not one dependency through it.
    Ref srcAsWritten;
};

} // namespace designdb::detail
