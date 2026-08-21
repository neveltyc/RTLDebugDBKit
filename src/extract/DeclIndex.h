// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Which template row a declared object is.
//
// Every dependency end is a net id, and an id must exist before anything can
// point at it -- so this is the layer everything else in the template build
// stands on. The statement layer asks it for a net 20 times, the connection
// layer 4; nothing here asks either of them for anything. That one-way traffic
// is why it is a class of its own rather than four more methods on the builder,
// and why it owns the two maps instead of borrowing them from per-build state.
//
// Scoped to one body: an index is a position in that body's template, and the
// answer to "is this symbol mine" is only meaningful against one body.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

#include "slang/ast/Scope.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/NetType.h"
#include "slang/ast/types/Type.h"

#include "DesignDb.h"
#include "extract/SourceLocator.h"
#include "extract/SymbolText.h"
#include "extract/Template.h"

namespace designdb::detail {

using namespace slang;
using namespace slang::ast;

class DeclIndex {
public:
    /// Seeds scope 0, the body itself. The template's scope list and the map
    /// from symbol to scope index have to agree that index 0 is the body and
    /// has no parent; keeping both halves of that in the constructor is what
    /// stops a caller from establishing one and forgetting the other.
    DeclIndex(Template& t, const InstanceBodySymbol& body, std::string prefix,
              SourceLocator& locator, Writer& writer) :
        t(t), body(body), prefix(std::move(prefix)), locator(locator),
        writer(writer) {
        t.scopes.push_back(TplScope{-1, std::string()});
        scopeOf.emplace(&body.asSymbol(), 0);
    }

    /// The net index for a symbol of this body, creating the row on first
    /// sight. Everything the dataflow touches must be a net row -- a
    /// dependency end is an id, and an id must exist -- so subroutine
    /// formals, locals and block variables are added lazily with their
    /// scope-relative dotted names.
    ///
    /// A symbol inside a CHILD instance is not a net of this one, even
    /// though its path extends this body's -- `u_cnt.cnt` is the child's
    /// net, which has a row of its own under the child. v9 stored the dotted
    /// spelling as a local name because the folded model had nothing to
    /// point at; the instance model does, so a downward reference goes to
    /// hier_ref and resolves to the real object.
    int32_t netFor(const ValueSymbol& sym) {
        if (auto it = netOf.find(&sym); it != netOf.end())
            return it->second;
        std::string rel;
        if (!relativePath(sym, prefix, rel))
            return -1;
        for (const Scope* s = sym.getParentScope(); s;) {
            auto& owner = s->asSymbol();
            if (owner.kind == SymbolKind::InstanceBody) {
                if (&owner != &body.asSymbol())
                    return -1;
                break;
            }
            s = owner.getParentScope();
        }
        TplNet net;
        net.scope = 0;
        net.name = std::move(rel);
        net.declKind = declarationKindOf(sym);
        net.dataTypeId = writer.internDataType(typeOf(sym));
        if (sym.getType().isIntegral())
            net.width = static_cast<int64_t>(sym.getType().getBitWidth());
        if (sym.kind == SymbolKind::Net)
            net.isImplicit = sym.as<NetSymbol>().isImplicit;
        net.loc = locator.locate(sym.location);
        const int32_t idx = int32_t(t.nets.size());
        t.netIndex.emplace(net.name, idx);
        t.nets.push_back(std::move(net));
        netOf.emplace(&sym, idx);
        return idx;
    }

    /// Declared nets and variables of the body and its generate scopes,
    /// eagerly, so a declared-but-unused signal is still a row -- those are
    /// exactly the ones worth asking about. scope indices follow the
    /// generate tree built alongside.
    void collectDeclarations(const Scope& scope, int32_t scopeIdx) {
        for (auto& member : scope.members()) {
            switch (member.kind) {
                case SymbolKind::Variable:
                case SymbolKind::Net: {
                    if (member.name.empty())
                        break;
                    auto& vs = member.as<ValueSymbol>();
                    const int32_t idx = netFor(vs);
                    if (idx >= 0)
                        t.nets[size_t(idx)].scope = scopeIdx;
                    break;
                }
                case SymbolKind::GenerateBlock: {
                    auto& block = member.as<GenerateBlockSymbol>();
                    if (block.isUninstantiated)
                        break;
                    const int32_t idx = addScope(block, scopeIdx,
                                                 generateSegment(block));
                    collectDeclarations(block, idx);
                    break;
                }
                case SymbolKind::GenerateBlockArray: {
                    auto& arr = member.as<GenerateBlockArraySymbol>();
                    std::string base(arr.name);
                    if (base.empty())
                        base = arr.getExternalName();
                    for (auto& entry : arr.entries) {
                        std::string segment = base;
                        if (entry->kind == SymbolKind::GenerateBlock)
                            segment += generateSegment(
                                entry->as<GenerateBlockSymbol>());
                        const int32_t idx = addScope(entry->asSymbol(),
                                                     scopeIdx, segment);
                        collectDeclarations(*entry, idx);
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    int32_t addScope(const Symbol& sym, int32_t parent, std::string name) {
        const int32_t idx = int32_t(t.scopes.size());
        t.scopes.push_back(TplScope{parent, std::move(name)});
        scopeOf.emplace(&sym, idx);
        return idx;
    }

    /// The scope index of `sym`, creating it under `parent` if this is the
    /// first sighting -- a generate scope collectDeclarations did not walk
    /// (an array entry wrapper, say), which must still be a node or the tree
    /// misses a level.
    int32_t scopeIndexOf(const Symbol& sym, int32_t parent) {
        if (auto it = scopeOf.find(&sym); it != scopeOf.end())
            return it->second;
        std::string seg;
        if (sym.kind == SymbolKind::GenerateBlock)
            seg = generateSegment(sym.as<GenerateBlockSymbol>());
        else
            seg = std::string(sym.name);
        return addScope(sym, parent, std::move(seg));
    }

    int32_t scopeForSymbol(const Symbol& sym) const {
        // Climb to the nearest scope the template modelled: a generate block
        // or the body itself. Statement blocks and subroutines fold onto it.
        const Scope* s = sym.getParentScope();
        while (s) {
            auto& owner = s->asSymbol();
            if (auto it = scopeOf.find(&owner); it != scopeOf.end())
                return it->second;
            if (&owner == &body.asSymbol())
                return 0;
            s = owner.getParentScope();
        }
        return 0;
    }

private:
    Template& t;
    const InstanceBodySymbol& body;
    std::string prefix;
    SourceLocator& locator;
    Writer& writer;
    std::unordered_map<const Symbol*, int32_t> netOf;
    std::unordered_map<const Symbol*, int32_t> scopeOf;
};

} // namespace designdb::detail
