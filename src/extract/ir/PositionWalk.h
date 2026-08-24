// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// The positioned-element walk: which expressions are element-wise, how their
// operands are enumerated, the MSB-first cursor and its overflow guard, the
// zero-width rule, and the predicate behind every positional claim.
//
// Recovery is the caller's, because it differs by product -- dataflow slots,
// aux reads, connection segments each answer a lost cursor differently -- and
// arrives as a callback rather than as a re-implementation of the loop.
//
// The zero-width rule: `{0{x}}` is legal, slang keeps the operand with a void
// type, and an ordinary parameterised pad arrives at zero width. It occupies
// no bits of the result, so it moves no cursor and the operands after it keep
// their positions; walking into it would record a dependency on a signal the
// concatenation does not read.

#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"

#include "Bits.h"

namespace designdb::detail {

/// The MSB-first window cursor: a concatenation is written most significant
/// first, so each member's window counts down from the top. advance() hands
/// back the member's window, or nullopt when the member is wider than what
/// remains -- an overflow that, unguarded, wraps and hands out bit ranges
/// near 2^64.
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

/// Whether `expr` is walked element by element.
///
/// The walk is MSB-first, so an expression qualifies only when its own
/// element order runs that way. Getting this wrong is not a lost position
/// but an inverted one, claimed exact.
///
/// * A concatenation is written most significant first, always.
/// * A simple assignment pattern lists elements in written order, which is
///   declaration order: for a PACKED aggregate that is MSB-first. For an
///   unpacked one it is the opposite -- element zero sits at the LOW
///   offsets -- so those are not walked.
/// * A structured pattern orders its elements by the target's shape rather
///   than by what was written: a packed struct's by declaration (MSB
///   first), but an ARRAY's by ascending index, which is LSB-first in both
///   packed and unpacked form. Only the packed-struct case qualifies.
///   A `default` or type setter disqualifies it separately: that is ONE
///   written expression standing for several members, and positioning it
///   would turn one recorded read into a row per member.
inline bool isElementwise(const slang::ast::Expression& expr) {
    using namespace slang::ast;
    if (expr.kind == ExpressionKind::Concatenation)
        return true;
    if (!expr.type)
        return false;
    const Type& ct = expr.type->getCanonicalType();
    const bool unpacked =
        ct.isUnpackedArray() || ct.isUnpackedStruct() || ct.isUnpackedUnion();
    if (expr.kind == ExpressionKind::SimpleAssignmentPattern)
        return !unpacked;
    if (expr.kind == ExpressionKind::StructuredAssignmentPattern) {
        auto& p = expr.as<StructuredAssignmentPatternExpression>();
        return !unpacked && ct.isStruct() && !p.defaultSetter &&
               p.typeSetters.empty();
    }
    return false;
}

/// The operands of an element-wise expression, MSB first, as written.
inline std::span<const slang::ast::Expression* const>
elementOperands(const slang::ast::Expression& expr) {
    using namespace slang::ast;
    switch (expr.kind) {
        case ExpressionKind::Concatenation:
            return expr.as<ConcatenationExpression>().operands();
        case ExpressionKind::SimpleAssignmentPattern:
            return expr.as<SimpleAssignmentPatternExpression>().elements();
        default:
            return expr.as<StructuredAssignmentPatternExpression>().elements();
    }
}

/// Drive one element-wise expression. Per operand in written (MSB-first)
/// order: the zero-width skip, then `element(op, window)` while the widths
/// add up. At the first operand wider than what remains, `overflow()` fires
/// once and the walk degrades: that operand and every one after it --
/// zero-width ones included, since a lost cursor vouches for nothing -- goes
/// to `degraded(op)` instead. `widthOf` is the caller's, so this header needs
/// no expression-type knowledge of its own.
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
/// Takes the coverage primitives rather than a Ref, so this header depends on
/// Bits.h alone.
inline bool coverFillsWidth(const BitInterval& cover, bool exact, uint64_t width,
                            uint64_t objectWidth) {
    return width != 0 && exact &&
           (cover.isRange() ? cover.bounds().width() == width
                            : objectWidth == width);
}

} // namespace designdb::detail
