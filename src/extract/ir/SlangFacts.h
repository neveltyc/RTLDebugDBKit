// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Where the extractor touches slang's unstable surface, and the questions it
// asks there, named.
//
// Most of slang's AST is stable enough to use directly, and this header does
// not try to wrap it -- an indirection that only forwards buys nothing. What
// it collects is the API that slang does NOT hold still: the analysis
// library, the value-path layer, and hierarchical-reference resolution, all
// of which changed shape between v10 and v11. CMakeLists.txt pins an exact
// tag because of these; this is the list to re-read when the pin moves.
//
//   ast::ValuePath          -- root symbol, longest static prefix and its bit
//                              bounds. Converted to our own Ref by refOf()
//                              in extract/Ref.h, which is the one place the
//                              shape of a path becomes the shape of a
//                              reference.
//   analysis::AnalysisManager
//                           -- getAnalyzedScope() below; also analyze() and
//                              getStats() in main.cpp, which brackets the
//                              analysis with Compilation::freeze() because
//                              the manager reads the compilation from worker
//                              threads.
//   analysis::AnalyzedProcedure
//                           -- analyzedSymbol, getSensitivityList(). Note
//                              getDrivers() is deliberately NOT the source of
//                              the dependency rows: a ValueDriver carries no
//                              statement, no operands and no gating, which is
//                              most of what a net_dep row is. It appears once,
//                              as a sanity check on procedures the walk
//                              reached nothing in.
//   ast::HierarchicalReference
//                           -- target, path, isUpward(), isViaIfacePort(),
//                              read in TemplateBuilder::fillResolution.
//   InstanceSymbol::getCanonicalBody()
//                           -- canonicalBodyOf() below.

#pragma once

#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/symbols/InstanceSymbols.h"

namespace designdb::detail {

/// The body slang analysed for this instance.
///
/// Identical instances share one canonical body, and that shared body is the
/// one the analysis manager has procedures for; `inst.body` is the
/// occurrence's own and may have none. Every template is built from the
/// canonical one, which is also why a child's port symbols belong to the
/// child's own body and must be looked up there rather than shared.
inline const slang::ast::InstanceBodySymbol& canonicalBodyOf(
    const slang::ast::InstanceSymbol& inst) {
    return inst.getCanonicalBody() ? *inst.getCanonicalBody() : inst.body;
}

/// Whether the analysis manager produced a scope for this body -- i.e.
/// whether any procedural dataflow can be read from it at all. A body
/// without one still yields nets, terminals and children.
inline bool wasAnalysed(slang::analysis::AnalysisManager& mgr,
                        const slang::ast::InstanceBodySymbol& body) {
    return mgr.getAnalyzedScope(body) != nullptr;
}

} // namespace designdb::detail
