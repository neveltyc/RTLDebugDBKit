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
    uint64_t lo = 0;
    uint64_t hi = 0;
    bool whole = true;
    bool exact = true;
    /// The expression the reference was written as; consulted when the symbol
    /// lives outside the instance (hier_ref text) and for resolution replay.
    const Expression* origin = nullptr;
};

inline uint64_t bitWidthOf(const ValueSymbol& sym) {
    return sym.getType().getSelectableWidth();
}

/// Builds a Ref from one of slang's value paths.
inline Ref refOf(const ValuePath& path) {
    Ref r;
    r.sym = path.rootSymbol();
    if (!r.sym)
        return r;
    r.origin = path.fullExpr;
    r.exact = path.isFullyStatic();
    if (!path.lsp) {
        r.exact = false;
        return r;
    }
    r.lo = path.lspBounds.first;
    r.hi = path.lspBounds.second;
    const uint64_t width = bitWidthOf(*r.sym);
    r.whole = width == 0 || (r.lo == 0 && r.hi + 1 >= width);
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
            Ref r = refOf(path);
            if (!r.sym)
                return;
            if (isConstantSymbol(*r.sym)) {
                filteredConstants++;
                return;
            }
            out.push_back(r);
        },
        skipSelectors);
}

/// A reference together with the bits of the *assignment* it occupies -- what
/// makes `{a, b} = {x, y}` answerable without the v7 cross product.
struct Slot {
    Ref ref;
    uint64_t lo = 0;
    uint64_t hi = 0;
    /// True when the reference's own bits map one-to-one onto [lo, hi].
    bool positional = false;
};

constexpr uint64_t kNoWidth = ~uint64_t{0};

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
    return e.type ? e.type->getBitWidth() : 0;
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
inline void collectSlots(const Expression& expr, EvalContext& ctx, uint64_t base,
                         std::vector<Slot>& out, bool skipSelectors = false) {
    const uint64_t width = exprWidthOf(expr);

    const bool elementwise = expr.kind == ExpressionKind::Concatenation ||
                             expr.kind == ExpressionKind::SimpleAssignmentPattern;
    if (elementwise && width) {
        auto ops = expr.kind == ExpressionKind::Concatenation
                       ? expr.as<ConcatenationExpression>().operands()
                       : expr.as<SimpleAssignmentPatternExpression>().elements();
        uint64_t cursor = base + width;
        // Where this expression's slots start, so the fallback below can
        // drop the positional ones already emitted for the operands ahead
        // of the one that stopped the walk. Appending to them instead would
        // leave one reference in `out` twice with two contradictory windows.
        const size_t mark = out.size();
        for (auto* op : ops) {
            if (!op)
                continue;
            const uint64_t w = exprWidthOf(*op);
            // Two ways the walk down the operands can stop meaning anything,
            // and the second used to be unguarded here though its twin in
            // StatementWalker's collectAuxSlots has always tested for it: an
            // operand of no width leaves the cursor where it was and gives
            // the element no position, and an operand WIDER than what is left
            // of the concatenation means the widths do not add up to the
            // whole. `cursor -= w` on the second is an unsigned wrap, and the
            // slots below it would carry bit ranges near 2^64 -- a garbage
            // answer offered with the same confidence as a real one.
            //
            // No expression is known to reach it: slang wraps every operand
            // whose width differs from its context in a Conversion, so a
            // well-formed concatenation adds up by construction, and a
            // malformed one loses its type and arrives here with width 0.
            // Probed over the constructs fixtures, a concatenation torture
            // file (packed and unpacked patterns, replication, streaming,
            // string concatenation, truncating and widening conversions,
            // unresolved names and types, out-of-range selects) and
            // picorv32, tinyriscv and veerwolf: not once, in either
            // function. The two are aligned on the safe side rather than the
            // cheap one because the costs are not symmetric -- one more
            // comparison per operand against a silent wrap.
            if (!w || w > cursor - base) {
                out.resize(mark);
                std::vector<Ref> rest;
                collectRefs(expr, ctx, rest, skipSelectors);
                for (auto& r : rest)
                    out.push_back(Slot{r, 0, kNoWidth, false});
                return;
            }
            cursor -= w;
            collectSlots(*op, ctx, cursor, out, skipSelectors);
        }
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
            out.push_back(Slot{r, 0, kNoWidth, false});
        return;
    }
    const bool positional =
        isPlainReference(expr) && refs.size() == 1 && refs[0].exact &&
        (refs[0].whole ? bitWidthOf(*refs[0].sym) == width
                       : refs[0].hi - refs[0].lo + 1 == width);
    for (auto& r : refs)
        out.push_back(Slot{r, base, base + width - 1, positional});
}

inline bool slotsOverlap(const Slot& a, const Slot& b, uint64_t& lo, uint64_t& hi) {
    if (a.hi == kNoWidth || b.hi == kNoWidth) {
        lo = 0;
        hi = kNoWidth;
        return true;
    }
    lo = std::max(a.lo, b.lo);
    hi = std::min(a.hi, b.hi);
    return lo <= hi;
}

/// The reference narrowed to the part of it landing in [lo, hi] of the
/// assignment. Only meaningful for a positional slot.
inline Ref narrowed(const Slot& s, uint64_t lo, uint64_t hi) {
    Ref r = s.ref;
    if (!s.positional || s.hi == kNoWidth || hi == kNoWidth || !r.sym)
        return r;
    if (lo <= s.lo && hi >= s.hi)
        return r;
    const uint64_t offset = r.whole ? 0 : r.lo;
    r.lo = offset + (lo - s.lo);
    r.hi = offset + (hi - s.lo);
    const uint64_t total = bitWidthOf(*r.sym);
    r.whole = total == 0 || (r.lo == 0 && r.hi + 1 >= total);
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
        // No bounds computed here: the whole object is an upper bound, and
        // storing it as exact would state uncertainty as fact.
        r.exact = false;
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
            const std::string scope = (*sub)->getHierarchicalPath();
            for (auto& r : inner) {
                std::string path = r.sym->getHierarchicalPath();
                const bool isLocal = path.size() > scope.size() &&
                                     path.compare(0, scope.size(), scope) == 0 &&
                                     path[scope.size()] == '.';
                if (!isLocal)
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
        const std::string scope = (*sub)->getHierarchicalPath();
        for (auto* sym : inner) {
            std::string path = sym->getHierarchicalPath();
            const bool isLocal = path.size() > scope.size() &&
                                 path.compare(0, scope.size(), scope) == 0 &&
                                 path[scope.size()] == '.';
            if (!isLocal)
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
