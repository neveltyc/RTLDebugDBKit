// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// The one positioned-element walk. Before this header the MSB-first cursor
// over a concatenation's operands existed four times -- Ref.h's collectSlots,
// StatementWalker's collectAuxSlots, TemplateBuilder_Conn's collectConnRefs,
// and twice over MultiPort members in TemplateBuilder -- and the same two
// corners were answered three different ways: a zero-width operand moved no
// cursor in two of them and poisoned the walk in a third, and the
// width-overflow guard was present in some and a silent unsigned wrap in
// another until 728b07b aligned them by hand.
//
// What is single-sourced here: which expressions are element-wise, how their
// operands are enumerated, the cursor arithmetic with its overflow guard, the
// zero-width rule, and the one-reference-fills-the-window predicate behind
// every positional and one-to-one claim. What deliberately stays with each
// caller is its RECOVERY -- what to do when the widths do not add up -- and
// its leaf collection, because those differ by product (dataflow slots, aux
// reads, connection segments) and the difference is the point: each recovery
// is now a named function argument at the call site rather than a divergent
// re-implementation of the loop around it.
//
// The zero-width rule, stated once: `{0{x}}` is legal, slang keeps the
// operand with a void type, and an ordinary parameterised pad arrives at
// zero width. It occupies no bits of the result, so it moves no cursor, the
// operands after it keep the positions they had, and nothing of it reaches
// the target -- walking into it would record a dependency on a signal the
// concatenation does not read.

#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "slang/ast/expressions/OperatorExpressions.h"

#include "Bits.h"

namespace designdb::detail {

/// The MSB-first window cursor: a concatenation is written most significant
/// first, so each member's window counts down from the top. advance() hands
/// back the member's window, or nullopt when the member is wider than what
/// is left -- the overflow that, unguarded, was an unsigned wrap handing out
/// bit ranges near 2^64 with a straight face.
class MsbCursor {
public:
    MsbCursor(uint64_t base, uint64_t totalWidth) :
        base_(base), cur_(base + totalWidth) {}

    std::optional<BitRange> advance(uint64_t width) {
        if (width == 0 || width > cur_ - base_)
            return std::nullopt;
        cur_ -= width;
        return BitRange(cur_, cur_ + width - 1);
    }

    /// The LSB the next member would start above; the MultiPort walks record
    /// this directly instead of a window.
    uint64_t position() const { return cur_; }

private:
    uint64_t base_;
    uint64_t cur_;
};

/// Whether `expr` is walked element by element. One list, however many
/// walks: adding a kind here used to mean finding every copy of the loop.
inline bool isElementwise(const slang::ast::Expression& expr) {
    using slang::ast::ExpressionKind;
    return expr.kind == ExpressionKind::Concatenation ||
           expr.kind == ExpressionKind::SimpleAssignmentPattern;
}

/// The operands of an element-wise expression, MSB first, as written.
inline std::span<const slang::ast::Expression* const>
elementOperands(const slang::ast::Expression& expr) {
    using namespace slang::ast;
    return expr.kind == ExpressionKind::Concatenation
               ? expr.as<ConcatenationExpression>().operands()
               : expr.as<SimpleAssignmentPatternExpression>().elements();
}

/// Drive one element-wise expression. Per operand in written (MSB-first)
/// order: the shared zero-width skip, then `element(op, window)` while the
/// widths add up. At the first operand wider than what remains, `overflow()`
/// fires once and the walk degrades: that operand and every one after it --
/// zero-width ones now included, since a lost cursor vouches for nothing --
/// goes to `degraded(op)` instead. Each caller states its recovery in those
/// two callbacks, where the old copies of this loop each buried a different
/// one. `widthOf` is the caller's width oracle so this header stays free of
/// the expression-type spelunking around it.
template <typename WidthFn, typename ElementFn, typename OverflowFn,
          typename DegradedFn>
inline void walkElements(const slang::ast::Expression& expr, uint64_t base,
                         uint64_t totalWidth, WidthFn&& widthOf,
                         ElementFn&& element, OverflowFn&& overflow,
                         DegradedFn&& degraded) {
    MsbCursor cursor(base, totalWidth);
    bool bad = false;
    for (auto* op : elementOperands(expr)) {
        if (!op)
            continue;
        if (bad) {
            degraded(*op);
            continue;
        }
        const uint64_t w = widthOf(*op);
        if (w == 0)
            continue;
        if (auto window = cursor.advance(w)) {
            element(*op, *window);
        }
        else {
            bad = true;
            overflow();
            degraded(*op);
        }
    }
}

/// The predicate behind every positional and one-to-one claim: an exact
/// reference with this coverage fills a window of `width` bits edge to edge.
/// Stated once; the clauses around it (plain reference, sole reference)
/// read the same at each claim site. Takes the coverage primitives rather
/// than a Ref so this header depends on Bits.h alone.
inline bool coverFillsWidth(const BitInterval& cover, bool exact, uint64_t width,
                            uint64_t objectWidth) {
    return width != 0 && exact &&
           (cover.isRange() ? cover.bounds().width() == width
                            : objectWidth == width);
}

} // namespace designdb::detail
