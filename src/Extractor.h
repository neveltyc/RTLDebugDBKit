// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Walks an elaborated design and hands rows to the writer.
//
// Two passes, matching how the work divides -- extract/TemplateBuilder.h and
// extract/Stamper.h are their interfaces:
//
//   templates   one per (definition, parameter values) group -- the analyzed
//               body's nets, terminals, statements, references and
//               dependencies, held in memory with template-local indices.
//               The analysis manager only analyses canonical bodies, so this
//               is the one place dataflow can be read from.
//   stamping    a walk of the elaborated instance tree that replays each
//               occurrence's template with fresh ids -- template-local index
//               plus per-occurrence base. Identical instances share one
//               template and one analysis; what differs per occurrence (the
//               place in the tree, the connections written in the parent,
//               the resolution of hierarchical references) is computed here.
//
// Everything below those two interfaces lives in namespace designdb::detail and
// is nobody else's business; this header is the whole public surface.

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
    int64_t modules = 0;      // source definitions
    int64_t instances = 0;    // inst rows (occurrences)
    int64_t nets = 0;
    int64_t terms = 0;
    int64_t conns = 0;        // net_conn rows
    int64_t procedures = 0;
    int64_t stmts = 0;
    int64_t callSites = 0;    // call_site rows
    int64_t deps = 0;         // net_dep rows
    int64_t hierRefs = 0;
    /// References to symbols outside their instance that could not be stored
    /// as a path (bare imported names, macro-assembled spellings). Counted so
    /// the run says so.
    int64_t external = 0;
    /// Instantiations whose module slang could not resolve. Recorded as
    /// unresolved tree nodes rather than dropped, and counted.
    int64_t unresolved = 0;
    /// Procedures the analysis says drive something but from which nothing
    /// could be extracted -- normally a statement slang marked bad, which
    /// takes its enclosing block with it. Counted per analyzed body.
    int64_t emptyProcedures = 0;
    /// Tree nodes whose (parent, name) was already taken. Non-zero means the
    /// design did not fully elaborate; a path lookup may be ambiguous.
    int64_t duplicatePaths = 0;
    /// Instances that re-enter a module already on their own hierarchy path
    /// -- an infinitely recursive instantiation, which is illegal RTL and
    /// which slang reports as a fatal error. The instance is stamped; its
    /// children are not, because the recursion has no end. Non-zero means the
    /// tree stops short there.
    int64_t recursiveInstances = 0;
    /// Call sites whose subroutine body was not instantiated because the
    /// module hit its expansion budget. Non-zero means the dataflow through
    /// those calls is incomplete -- reported rather than left to look like
    /// a subroutine that reads and writes nothing.
    int64_t truncatedCalls = 0;
};

/// Extracts `compilation` into `writer`. `analysis` must already have run.
Stats extract(slang::ast::Compilation& compilation,
              slang::analysis::AnalysisManager& analysis, Writer& writer);

} // namespace designdb
