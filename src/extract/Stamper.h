// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Pass 2: replay each template once per elaborated occurrence.
//
// A template holds template-local indices; stamping turns each into an id by
// adding the occurrence's base, and computes the things that cannot be shared
// -- the place in the tree, the connections written in the parent, the
// resolution of hierarchical references. Thirty-two copies of one core are
// thirty-two row sets from one analysis.
//
// Order inside is load-bearing and stated where it happens: children before
// their connections, packages before reference resolution, resolution last.

#pragma once

#include "DesignDb.h"
#include "Extractor.h"
#include "extract/SourceLocator.h"
#include "extract/TemplateBuilder.h"

namespace slang::ast {
class Compilation;
}

namespace designdb::detail {

/// Stamps the whole elaborated tree, then the packages, then resolves every
/// hierarchical reference. `templates` is taken by reference and not copied.
void stampDesign(slang::ast::Compilation& compilation, Writer& writer,
                 SourceLocator& locator, Stats& stats, TemplateSet& templates);

} // namespace designdb::detail
