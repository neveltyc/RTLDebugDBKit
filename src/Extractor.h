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
    /// Call sites whose subroutine body was not instantiated because the
    /// module hit its expansion budget. Non-zero means the dataflow through
    /// those calls is incomplete -- reported rather than left to look like
    /// a subroutine that reads and writes nothing.
    int64_t truncatedCalls = 0;
    /// The (definition, parameters) groups pass 1 formed whose chosen body
    /// the analysis manager never analysed, so the template built from one
    /// holds no procedure at all.
    ///
    /// Non-zero on real designs -- 11 groups on veerwolf -- and it costs no
    /// row. TemplateBuilder::collect() keys a group on `getCanonicalBody() ?:
    /// body` but then descends `inst.body`, the NON-canonical one. For an
    /// instance slang's cache folded onto an earlier body, that body was
    /// never elaborated, so reaching its members() mints child instances of
    /// its own with no canonical body set; their parameter text becomes a
    /// group key that the canonical subtree does not produce, and no
    /// analysed body is ever offered for it. Nothing is stamped from such a
    /// group either, since every child key a template records comes from the
    /// analysed body it was built from -- which is what makes it a note
    /// rather than a warning. Reported because a template built and never
    /// used is worth saying out loud, not because a query answers
    /// differently.
    int64_t unanalysedBodies = 0;
    /// Occurrences stamped from such a template -- the case where the gap
    /// above would reach the rows. Every procedure of the module would be
    /// absent from that instance, leaving hierarchy, nets and connections
    /// that read exactly like a module with no always block.
    ///
    /// Non-zero only when the compilation is fatally errored, where
    /// analyze() returns before analysing anything and `analysis_status` is
    /// already `hierarchy_only` for that reason. Otherwise zero on every
    /// design measured, and the walk says why: the analysis analyses
    /// `getCanonicalBody() ?: body` -- the very body groupKey is taken
    /// against -- and descends into instances, instance arrays, generate
    /// blocks and generate-array entries as the template walk does. So each
    /// child a template names was analysed if its parent was, and the roots
    /// are analysed by construction.
    ///
    /// The two descents are not identical, which is the reason to count
    /// rather than trust: slang also skips an instance whose body carries
    /// InstanceFlags::Uninstantiated and a generate array that is not
    /// `valid`, and forEachOfKind tests neither. Both are unreachable in the
    /// pinned slang -- an uninstantiated instantiation is an
    /// UninstantiatedDefSymbol rather than an InstanceSymbol, and an invalid
    /// generate array creates no entries to walk -- but they are slang's to
    /// keep, not ours. If a revision descends differently, the export says
    /// so instead of dropping every procedure of a module in silence.
    int64_t unanalysedInsts = 0;
};

/// Extracts `compilation` into `writer`. `analysis` must already have run.
Stats extract(slang::ast::Compilation& compilation,
              slang::analysis::AnalysisManager& analysis, Writer& writer);

} // namespace designdb
