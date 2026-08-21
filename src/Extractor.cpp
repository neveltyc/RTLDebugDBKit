// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)

#include "Extractor.h"

#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/Compilation.h"
#include "slang/text/SourceManager.h"

#include "DesignDb.h"
#include "extract/SourceLocator.h"
#include "extract/Stamper.h"
#include "extract/TemplateBuilder.h"

namespace designdb {

Stats extract(slang::ast::Compilation& compilation,
              slang::analysis::AnalysisManager& analysis, Writer& writer) {
    Stats stats;
    detail::SourceLocator locator(*compilation.getSourceManager(), writer);

    // Pass 1: one template per (definition, parameter values) group -- the
    // analyzed body's nets, terminals, statements, references and
    // dependencies, held with template-local indices.
    detail::TemplateSet templates =
        detail::buildTemplates(compilation, analysis, writer, locator, stats);

    // Pass 2: replay each template once per elaborated occurrence, with fresh
    // ids. By reference: `templates` holds every row of every analyzed body,
    // which on a large design is most of what the process is holding.
    detail::stampDesign(compilation, writer, locator, stats, templates);

    // Last, and only now: a file row can be interned by anything above, so the
    // as-written spellings are complete only once both passes have run.
    writer.linkSourceFiles(locator.origins());
    return stats;
}

} // namespace designdb
