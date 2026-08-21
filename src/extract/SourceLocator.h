// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Source locations, interned.
//
// The one thing the two passes share. When the extractor was one class, exactly
// two of its 47 methods were reachable from both the template build and the
// stamping: these. Everything else belonged to one side or the other, which is
// what made splitting along that seam worth doing -- and why TemplateBuilder
// and Stamper each take a locator rather than owning one.
//
// A location is a file id plus a line and column, and the three travel
// together -- taking the line from a statement and the file from its enclosing
// procedure names a line in a file that does not contain it.

#pragma once

#include <string>
#include <unordered_map>

#include "DesignDb.h"
#include "extract/SymbolText.h"
#include "extract/Template.h"

namespace designdb::detail {

class SourceLocator {
public:
    SourceLocator(const slang::SourceManager& sm, Writer& w) :
        sourceManager(sm), writer(w) {}

    /// Interns `loc`'s file, line and column as a template location. `fallback`
    /// is what an absent location becomes -- normally the enclosing
    /// procedure's, so a statement slang gave no position still lands
    /// somewhere true rather than nowhere.
    TplLoc locate(slang::SourceLocation loc, const TplLoc& fallback = {}) {
        if (!loc)
            return fallback;
        Where w = whereOf(loc, sourceManager);
        noteFile(loc, w.file);
        return TplLoc{writer.internFile(w.file), w.line, w.column};
    }

    /// Every as-written spelling mapped to the absolute path the buffer really
    /// came from, for Writer::linkSourceFiles once the rows are out.
    const std::unordered_map<std::string, std::string>& origins() const {
        return fileOrigins;
    }

private:
    void noteFile(slang::SourceLocation loc, const std::string& asWritten) {
        if (!loc || asWritten.empty())
            return;
        if (fileOrigins.count(asWritten))
            return;
        auto path = sourceManager.getFullPath(
            sourceManager.getFullyExpandedLoc(loc).buffer());
        if (path.empty())
            return;
        fileOrigins.emplace(asWritten, path.string());
    }

    const slang::SourceManager& sourceManager;
    Writer& writer;
    std::unordered_map<std::string, std::string> fileOrigins;
};

} // namespace designdb::detail
