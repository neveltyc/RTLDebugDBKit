// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Walks an elaborated design and hands rows to the writer.
//
// Two things come out, matching the two questions the consumer asks:
//
//   the instance tree      -> "where am I in the hierarchy"
//   per-module dataflow    -> "who drives this / what reads it"
//
// The dataflow is emitted once per *module*, in the module's own namespace,
// and the instance tree is what expands it back into hierarchy. slang already
// shares one canonical body between identical instances, so the fold is its
// deduplication rather than anything reconstructed here.

#pragma once

#include <cstdint>
#include <string>

#include "DesignDb.h"

namespace slang::ast {
class Compilation;
}
namespace slang::analysis {
class AnalysisManager;
}

namespace designdb {

struct Stats {
    int64_t modules = 0;
    int64_t instances = 0;
    int64_t edges = 0;
    int64_t children = 0;
    int64_t ports = 0;
    int64_t symbols = 0;
    int64_t assignments = 0;
    /// References to symbols outside the module being extracted. They cannot be
    /// stored in a row shared by every instance of it, so they are counted.
    int64_t external = 0;
    /// Instantiations whose module slang could not resolve. Recorded with a
    /// null child_module rather than dropped, and counted so the run says so.
    int64_t unresolved = 0;
    /// Procedures the analysis says drive something but from which nothing
    /// could be extracted -- normally a statement slang marked bad, which takes
    /// its enclosing block with it.
    int64_t emptyProcedures = 0;
    /// Instances whose hierarchical path was already taken. Non-zero means the
    /// design did not fully elaborate; a path lookup in it may be ambiguous.
    int64_t duplicatePaths = 0;
};

/// Extracts `compilation` into `writer`. `analysis` must already have run.
Stats extract(slang::ast::Compilation& compilation,
              slang::analysis::AnalysisManager& analysis, Writer& writer);

} // namespace designdb
