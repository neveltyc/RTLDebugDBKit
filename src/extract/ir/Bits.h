// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// The bit-coverage vocabulary: which bits of one flattened object a reference
// touches, as a closed type rather than a convention.
//
// Before this header the same information lived in three fields -- lo, hi and
// a `whole` bool -- plus a `~uint64_t{0}` sentinel spelled by hand at eight
// construction sites, and `whole == true` meant two different things ("covers
// the object" from refOf, "no bounds were computed at all" from the statement
// collector) that only stayed apart because the second case also cleared
// `exact`. Here the three states are three cases, a range asserts lo <= hi at
// construction, and reading bounds off a case that has none is a programming
// error rather than a garbage answer.
//
// The mapping onto the schema's range encoding (doc/designdb-schema.md, "Bit
// ranges") is one-way and total: Whole with exact -> (NULL, 1); Unknown ->
// (NULL, 0); Range with exact -> (bits, 1); Range without -> (bits, 0).
// Certainty stays a separate bool alongside, as it is in the schema; the one
// invariant tying them is that Unknown is never exact.
//
// Offsets are the schema's throughout: LSB-relative positions into the
// flattened object, never declared indices.

#pragma once

#include <cassert>
#include <cstdint>

namespace designdb::detail {

/// A contiguous run of bits, [lo, hi], both inclusive. lo <= hi always: a
/// range that cannot hold even one bit is not a range, and the empty case is
/// someone else's variant arm, never an encoding of this type.
struct BitRange {
    uint64_t lo;
    uint64_t hi;

    BitRange(uint64_t lo, uint64_t hi) : lo(lo), hi(hi) { assert(lo <= hi); }

    uint64_t width() const { return hi - lo + 1; }
};

/// Which bits of one object a reference covers: all of them, a specific run,
/// or an unknown part -- the three answers an extraction can honestly give.
class BitInterval {
public:
    /// The whole object. Exactness is the caller's separate fact: whole and
    /// exact is "all bits, known"; whole would never pair with inexact --
    /// "somewhere in all of it" is what unknown() says.
    static BitInterval whole() { return BitInterval(Case::Whole, 0, 0); }

    /// Exactly [lo, hi] of the flattened object.
    static BitInterval range(uint64_t lo, uint64_t hi) {
        assert(lo <= hi);
        return BitInterval(Case::Range, lo, hi);
    }
    static BitInterval range(BitRange r) { return range(r.lo, r.hi); }

    /// Somewhere inside the object, position not computed. The one state the
    /// old encoding could not spell apart from whole().
    static BitInterval unknown() { return BitInterval(Case::Unknown, 0, 0); }

    /// [lo, hi] collapsed to whole() when it covers the object -- the rule
    /// refOf and narrowed each stated by hand. A zero total width collapses
    /// too: an object with no bits has nothing narrower than everything.
    static BitInterval forBounds(uint64_t lo, uint64_t hi, uint64_t totalWidth) {
        if (totalWidth == 0 || (lo == 0 && hi + 1 >= totalWidth))
            return whole();
        return range(lo, hi);
    }

    bool isWhole() const { return c == Case::Whole; }
    bool isRange() const { return c == Case::Range; }
    bool isUnknown() const { return c == Case::Unknown; }

    /// The stored run. Asking a case that has none is the bug this type
    /// exists to catch; the old fields answered 0 with a straight face.
    uint64_t lo() const {
        assert(isRange());
        return lo_;
    }
    uint64_t hi() const {
        assert(isRange());
        return hi_;
    }
    BitRange bounds() const { return BitRange(lo(), hi()); }

    bool operator==(const BitInterval&) const = default;

private:
    enum class Case : uint8_t { Whole, Range, Unknown };

    BitInterval(Case c, uint64_t lo, uint64_t hi) : c(c), lo_(lo), hi_(hi) {}

    Case c;
    uint64_t lo_;
    uint64_t hi_;
};

} // namespace designdb::detail
