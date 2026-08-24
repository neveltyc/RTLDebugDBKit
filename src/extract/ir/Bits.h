// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Which bits of one flattened object a reference touches.
//
// Three cases, because an extraction has three honest answers: all of it, a
// specific run, or an unknown part. Reading bounds off a case that has none
// is a programming error, not a defaulted zero.
//
// The mapping onto the schema's range encoding (doc/designdb-schema.md, "Bit
// ranges") is total: Whole -> (NULL, exact); Unknown -> (NULL, 0); Range ->
// (bits, exact). Exactness stays a separate flag, as it is in the schema.
//
// Offsets are the schema's throughout: LSB-relative positions into the
// flattened object, never declared indices.

#pragma once

#include <cassert>
#include <cstdint>

namespace designdb::detail {

/// A contiguous run of bits, [lo, hi], both inclusive. lo <= hi always: an
/// empty run is another case's business, never an encoding of this one.
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
    /// The whole object.
    static BitInterval whole() { return BitInterval(Case::Whole, 0, 0); }

    /// Exactly [lo, hi] of the flattened object.
    static BitInterval range(uint64_t lo, uint64_t hi) {
        assert(lo <= hi);
        return BitInterval(Case::Range, lo, hi);
    }
    static BitInterval range(BitRange r) { return range(r.lo, r.hi); }

    /// Somewhere inside the object, position not computed.
    static BitInterval unknown() { return BitInterval(Case::Unknown, 0, 0); }

    /// [lo, hi] collapsed to whole() when it covers the object. A zero total
    /// width collapses too: an object with no bits has nothing narrower than
    /// everything.
    static BitInterval forBounds(uint64_t lo, uint64_t hi, uint64_t totalWidth) {
        if (totalWidth == 0 || (lo == 0 && hi + 1 >= totalWidth))
            return whole();
        return range(lo, hi);
    }

    bool isWhole() const { return c == Case::Whole; }
    bool isRange() const { return c == Case::Range; }

    /// The stored run; only a Range has one.
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
