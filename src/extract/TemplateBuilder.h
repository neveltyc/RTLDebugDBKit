// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Pass 1: one template per (definition, parameter values) group.
//
// The group key is (definition, parameter values), not the body pointer: slang
// shares a canonical body only sometimes, and only the canonical body has an
// AnalyzedScope -- a non-canonical one would contribute no dataflow at all,
// silently. Everything a group's occurrences have in common is read once here;
// what differs per occurrence is the stamping pass's business.
//
// The interface is one function. The builder behind it is declared in
// extract/TemplateBuilderImpl.h and defined across TemplateBuilder.cpp and
// TemplateBuilder_Conn.cpp; nothing outside those three files should name it.
// It is used exactly once, in order, and every method it has is a step of that
// one traversal.

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <unordered_map>

#include "DesignDb.h"
#include "Extractor.h"
#include "extract/SourceLocator.h"
#include "extract/Template.h"

namespace slang::ast {
class Compilation;
class InstanceSymbol;
}
namespace slang::analysis {
class AnalysisManager;
}

namespace designdb::detail {

/// What pass 1 hands pass 2.
///
/// Held by reference all the way through: `byKey` is every row of every
/// analyzed body in the design -- on a large SoC that is most of the process's
/// memory, and a copy at this boundary would be the single most expensive
/// mistake available here.
struct TemplateSet {
    std::map<std::string, Template> byKey;
    /// Which group each elaborated instance belongs to, so stamping can find
    /// the template an occurrence replays.
    std::unordered_map<const slang::ast::InstanceSymbol*, std::string> groupOf;
    /// Module rows already issued. Package pseudo-modules number from here.
    std::size_t moduleCount = 0;
};

/// Builds every group's template, writing the `module` rows as it goes.
TemplateSet buildTemplates(slang::ast::Compilation& compilation,
                           slang::analysis::AnalysisManager& analysis,
                           Writer& writer, SourceLocator& locator, Stats& stats);

} // namespace designdb::detail
