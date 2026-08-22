// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// AST to text, and the small traversals that go with it.
//
// Everything here answers "what does this symbol call itself in a row": the
// scope-relative path, the declaration kind, the normalised delay, the
// canonical spelling of an outward reference. Independent of Ref.h -- nothing
// in this file knows what a bit range is.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Scope.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/expressions/AssertionExpr.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/NetType.h"
#include "slang/ast/types/Type.h"
#include "slang/numeric/ConstantValue.h"
#include "slang/numeric/SVInt.h"
#include "slang/syntax/SyntaxNode.h"
#include "slang/text/SourceManager.h"

namespace designdb::detail {

using namespace slang;
using namespace slang::ast;

/// The path of `sym` as seen from `body`, i.e. with the instance's own prefix
/// removed. Rows name objects scope-relative (`g[0].sig`, `bump.v`); the
/// template is stamped per occurrence, so the relative spelling is what every
/// occurrence shares.
inline bool relativePath(const Symbol& sym, const std::string& bodyPrefix, std::string& out) {
    std::string full = sym.getHierarchicalPath();
    if (!bodyPrefix.empty() && full.size() > bodyPrefix.size() &&
        full.compare(0, bodyPrefix.size(), bodyPrefix) == 0 &&
        full[bodyPrefix.size()] == '.') {
        out = full.substr(bodyPrefix.size() + 1);
        return true;
    }
    if (full == bodyPrefix) {
        out = full;
        return true;
    }
    // Outside the instance: an upward hierarchical reference, an interface
    // signal, a package item. Recorded as hier_ref or counted, never stored
    // as a net of this instance.
    out = full;
    return false;
}

inline std::string typeOf(const ValueSymbol& sym) {
    return sym.getType().toString();
}

/// A file, line and column that came from one source location. The three
/// travel together: taking the line from a statement and the file from its
/// enclosing procedure names a line in a file that does not contain it.
struct Where {
    std::string file;
    uint32_t line = 0;
    uint32_t column = 0;
};

inline Where whereOf(SourceLocation loc, const SourceManager& sm) {
    if (!loc)
        return {};
    // Expanded once, here, so all three come from the same place.
    //
    // getFileName and getLineNumber expand internally; getColumnNumber does
    // not -- slang states the precondition on it, "location must be a file
    // location", and for a macro location the buffer holds an ExpansionInfo,
    // so it finds no FileInfo and returns 0. Passing the raw location gave the
    // file and line of the expansion site and a column of 0, which is not a
    // column in any 1-based numbering, and broke the one rule this struct
    // exists to keep. getFullyExpandedLoc is idempotent, so the two that
    // already expand are unaffected and keep their `line-directive handling.
    const SourceLocation at = sm.getFullyExpandedLoc(loc);
    return Where{std::string(sm.getFileName(at)),
                 static_cast<uint32_t>(sm.getLineNumber(at)),
                 static_cast<uint32_t>(sm.getColumnNumber(at))};
}

/// The canonical text of a reference that leaves its instance: the path as
/// written, with every select resolved to the constant it elaborated to.
/// (See v9's history for why not the raw source text: generate loops share a
/// spelling across distinct references, spellings differ in whitespace, and
/// macro-assembled references span buffers.)
inline std::string canonicalPath(const Expression* e, EvalContext& eval) {
    for (;;) {
        if (e && e->kind == ExpressionKind::ElementSelect)
            e = &e->as<ElementSelectExpression>().value();
        else if (e && e->kind == ExpressionKind::RangeSelect)
            e = &e->as<RangeSelectExpression>().value();
        else
            break;
    }
    std::string out;
    auto build = [&](auto&& self, const Expression* x) -> bool {
        if (!x)
            return false;
        switch (x->kind) {
            case ExpressionKind::NamedValue: {
                auto& sym = x->as<ValueExpressionBase>().symbol;
                if (sym.name.empty())
                    return false;
                // A package item keeps its package: a bare `mask` resolves
                // against imports a reader cannot see, `pkg::mask` says where
                // to look.
                if (auto* scope = sym.getParentScope()) {
                    auto& owner = scope->asSymbol();
                    if (owner.kind == SymbolKind::Package && !owner.name.empty()) {
                        out += owner.name;
                        out += "::";
                    }
                }
                out += sym.name;
                return true;
            }
            case ExpressionKind::MemberAccess: {
                auto& ma = x->as<MemberAccessExpression>();
                if (!self(self, &ma.value()))
                    return false;
                if (ma.member.name.empty())
                    return false;
                out += '.';
                out += ma.member.name;
                return true;
            }
            case ExpressionKind::ElementSelect: {
                auto& sel = x->as<ElementSelectExpression>();
                if (!self(self, &sel.value()))
                    return false;
                auto cv = sel.selector().eval(eval);
                if (!cv)
                    return false;   // a runtime index names no one element
                out += '[';
                out += cv.toString();
                out += ']';
                return true;
            }
            case ExpressionKind::Conversion:
                return self(self, &x->as<ConversionExpression>().operand());
            default:
                return false;
        }
    };
    if (!build(build, e))
        return {};
    return out;
}

/// The reference as written, with whitespace and comments taken out -- the
/// fallback for a reference `canonicalPath` cannot walk (an XMR slang
/// resolves to a single node). Empty for a macro-assembled span.
inline std::string normalizedText(const Expression* e, const SourceManager& sm,
                                  bool stripTrailingSelect = true) {
    if (!e)
        return {};
    auto range = e->sourceRange;
    if (!range.start() || !range.end() ||
        range.start().buffer() != range.end().buffer())
        return {};
    auto text = sm.getSourceText(range.start().buffer());
    const size_t a = range.start().offset();
    const size_t b = range.end().offset();
    if (a >= b || b > text.size())
        return {};
    std::string out;
    auto raw = text.substr(a, b - a);
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] == '/' && i + 1 < raw.size() && raw[i + 1] == '*') {
            auto end = raw.find("*/", i + 2);
            if (end == std::string_view::npos)
                return {};
            i = end + 1;
            continue;
        }
        if (raw[i] == '/' && i + 1 < raw.size() && raw[i + 1] == '/') {
            auto nl = raw.find('\n', i + 2);
            if (nl == std::string_view::npos)
                break;
            i = nl;
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(raw[i])))
            out += raw[i];
    }
    // Trailing selects are the bit range, which has columns of its own --
    // every group of them, matched by bracket depth.
    while (stripTrailingSelect && !out.empty() && out.back() == ']') {
        int depth = 0;
        size_t i = out.size();
        while (i > 0) {
            --i;
            if (out[i] == ']')
                depth++;
            else if (out[i] == '[' && --depth == 0)
                break;
        }
        if (depth != 0 || i == 0)
            break;
        out.resize(i);
    }
    return out;
}

/// The normalised text of a timing control: its syntax with whitespace runs
/// collapsed. Stored as text, never evaluated -- `#(rise, fall)` and
/// min:typ:max forms are not one number, and pretending otherwise would
/// store a guess.
inline std::string delayText(const TimingControl* t) {
    if (!t || !t->syntax)
        return {};
    switch (t->kind) {
        case TimingControlKind::Delay:
        case TimingControlKind::Delay3:
        case TimingControlKind::CycleDelay:
            break;
        default:
            return {};
    }
    std::string raw = t->syntax->toString();
    std::string out;
    bool pendingSpace = false;
    for (char c : raw) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out += ' ';
            pendingSpace = false;
        }
        out += c;
    }
    return out;
}

/// The word an assertion publishes as its `construct`. Spelled out rather
/// than taken from slang's enum printer: these are a wire format.
inline std::string assertionWord(AssertionKind kind) {
    switch (kind) {
        case AssertionKind::Assume:        return "assume";
        case AssertionKind::CoverProperty:
        case AssertionKind::CoverSequence: return "cover";
        case AssertionKind::Restrict:      return "restrict";
        case AssertionKind::Expect:        return "expect";
        default:                           return "assert";
    }
}

/// The proc_kind word for a procedural block.
inline std::string procedureWord(const Symbol& sym) {
    if (sym.kind != SymbolKind::ProceduralBlock)
        return "always";
    switch (sym.as<ProceduralBlockSymbol>().procedureKind) {
        case ProceduralBlockKind::AlwaysComb:  return "always_comb";
        case ProceduralBlockKind::AlwaysLatch: return "always_latch";
        case ProceduralBlockKind::AlwaysFF:    return "always_ff";
        case ProceduralBlockKind::Always:      return "always";
        case ProceduralBlockKind::Initial:     return "initial";
        case ProceduralBlockKind::Final:       return "final";
        default:                               return "always";
    }
}

/// Every edge-triggered event in a timing control, in the order written --
/// all of them, since an event list has no ordering semantics.
inline void collectEdgeEvents(const TimingControl* t,
                              std::vector<std::pair<const Expression*, std::string>>& out,
                              std::vector<const Expression*>* iffs = nullptr) {
    if (!t)
        return;
    switch (t->kind) {
        case TimingControlKind::SignalEvent: {
            auto& se = t->as<SignalEventControl>();
            // An explicitly written event is recorded whether or not it
            // names an edge: `always @(b)` samples b, and a signal that
            // appears ONLY in a sensitivity list was otherwise invisible to
            // every load query. edge_kind stays NULL for the plain form.
            // Implicit lists (`@*`, always_comb) are deliberately NOT
            // expanded here: their sensitivity IS the read set, which the
            // dataflow rows already carry, and duplicating it would list
            // every combinational read twice.
            out.emplace_back(&se.expr,
                             se.edge == EdgeKind::PosEdge   ? "posedge"
                             : se.edge == EdgeKind::NegEdge ? "negedge"
                             : se.edge == EdgeKind::BothEdges ? "both"
                                                              : "");
            // `@(posedge clk iff en)` samples `en` too; it qualifies the
            // event rather than being one.
            if (iffs && se.iffCondition)
                iffs->push_back(se.iffCondition);
            return;
        }
        case TimingControlKind::EventList:
            for (auto* c : t->as<EventListControl>().events)
                collectEdgeEvents(c, out, iffs);
            return;
        default:
            return;
    }
}

/// One group's parameter values, as stable text: declaration order, values in
/// full precision.
///
/// The text is a template's identity -- two bodies sharing a (definition,
/// parameters) key are replayed from one analysis -- so any abbreviation here
/// folds designs that are not the same. SVInt::toString abbreviates two ways
/// and both have to be defeated: MAX_BITS turns off the length cut above 128
/// bits, and exactUnknowns turns off the one that matters more. Left false, a
/// value that has unknown bits, is wider than 64, and is neither
/// all-x nor all-z prints as the single letter `X` (SVInt.cpp's base
/// heuristic), so `128'h...0x` and `128'h...1x` become one key -- and the
/// second instance is then stamped from the first one's body, reporting the
/// generate branch it did not elaborate.
inline std::vector<std::pair<std::string, std::string>>
parameterPairs(const InstanceBodySymbol& body) {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& member : body.members()) {
        if (member.kind == SymbolKind::Parameter) {
            auto& p = member.as<ParameterSymbol>();
            out.emplace_back(std::string(p.name),
                             p.getValue().toString(SVInt::MAX_BITS,
                                                   /*exactUnknowns=*/true));
        }
        else if (member.kind == SymbolKind::TypeParameter) {
            auto& tp = member.as<TypeParameterSymbol>();
            out.emplace_back(std::string(tp.name),
                             tp.targetType.getType().toString());
        }
    }
    return out;
}

/// The signature is the pairs, joined -- one normalisation, two
/// representations, and the verifier holds them equal per occurrence.
inline std::string parameterText(const InstanceBodySymbol& body) {
    std::string out;
    for (auto& [name, value] : parameterPairs(body)) {
        if (!out.empty())
            out += ',';
        out += name;
        out += '=';
        out += value;
    }
    return out;
}

/// One reference to a signal, with the bits it touches. Encoding unchanged
/// from v7: `whole` spans the object, `exact=false` means the range is an
/// upper bound (a dynamic selector).


/// The path segment slang gives a generate block: the genvar's *value* for a
/// loop iteration, the block's name otherwise.
inline std::string generateSegment(const GenerateBlockSymbol& block) {
    if (auto* index = block.getArrayIndex())
        return "[" + index->toString(LiteralBase::Decimal, false) + "]";
    std::string name(block.name);
    return name.empty() ? block.getExternalName() : name;
}

/// The last segment of a symbol's hierarchical path: the leaf name with
/// slang's array-element index already rendered in source numbering
/// (`u[0]`) -- the one spelling a tree node's name must use, since an
/// instance-array element's own `name` is the bare `u`.
///
/// Empty when the symbol has no name of its own; the caller synthesises one.
inline std::string leafSegment(const Symbol& sym) {
    // Built from the symbol, not by splitting its path on the last '.'.
    // slang escapes a name that is not a plain identifier as `\name ` --
    // verbatim, with no quoting of an embedded dot -- so `sub \u.1 ();`
    // has a path with three dots, only the first of which is a separator.
    // Splitting it named the tree node `1 `, and no path lookup could reach
    // that instance.
    //
    // Same escaping rule and same array-index suffixes slang's own
    // appendHierarchicalPath applies, so the segment matches what the rest of
    // the path spells.
    auto needsEscaping = [](std::string_view t) {
        if (t.empty())
            return false;
        auto plain = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_';
        };
        if (!plain(t[0]) || (t[0] >= '0' && t[0] <= '9'))
            return true;
        for (size_t i = 1; i < t.size(); i++) {
            if (!plain(t[i]) && t[i] != '$')
                return true;
        }
        return false;
    };
    // An array element carries no name of its own -- slang spells `u[0]` as
    // the ARRAY symbol's name plus the element's index suffix, with no
    // separator between them -- so the base comes from the nearest named
    // enclosing InstanceArraySymbol.
    std::string_view base = sym.name;
    if (base.empty()) {
        for (auto* sc = sym.getParentScope(); sc; ) {
            auto& owner = sc->asSymbol();
            if (owner.kind != SymbolKind::InstanceArray)
                break;
            if (!owner.name.empty()) {
                base = owner.name;
                break;
            }
            sc = owner.getParentScope();
        }
    }
    if (base.empty()) {
        // An instantiation with no name of its own: legal for a gate or a
        // UDP, and what a module instantiation degrades to when the name
        // came from a macro that did not expand. slang leaves the name
        // empty and the hierarchical path then ends at the PARENT, so
        // taking the last segment named the child after the instance
        // holding it -- every such child answered to its parent's name,
        // two of them in one scope answered to each other's, and
        // (parent_node_id, name) stopped being a lookup.
        //
        // Nothing here can do better: a segment that is unique among
        // siblings needs to know the siblings, which only the caller
        // building them does. Empty says one must be synthesised.
        //
        // The array suffix goes with it. `arrayPath` indexes an array that
        // has no name either, so a bare `[0]` is no more of a segment than
        // the empty string is -- and two unnamed arrays of one shape in a
        // scope would spell their elements identically.
        return {};
    }
    std::string out = needsEscaping(base) ? "\\" + std::string(base) + " "
                                          : std::string(base);
    if (sym.kind == SymbolKind::Instance || sym.kind == SymbolKind::CheckerInstance) {
        auto& inst = sym.as<InstanceSymbolBase>();
        if (!inst.arrayPath.empty()) {
            SmallVector<ConstantRange, 8> dims;
            inst.getArrayDimensions(dims);
            if (dims.size() == inst.arrayPath.size()) {
                for (size_t i = 0; i < dims.size(); i++)
                    out += "[" + std::to_string(int32_t(inst.arrayPath[i]) +
                                                dims[i].lower()) + "]";
            }
        }
    }
    return out;
}

/// Calls `fn` for every member of `scope` of kind `K`, descending through
/// generate blocks and instance arrays but never into an instance's own body.
template<SymbolKind K, typename SymT, typename F>
inline void forEachOfKind(const Scope& scope, F&& fn) {
    for (auto& member : scope.members()) {
        if (member.kind == K) {
            fn(member.as<SymT>());
            continue;
        }
        switch (member.kind) {
            case SymbolKind::GenerateBlock: {
                auto& block = member.as<GenerateBlockSymbol>();
                if (block.isUninstantiated)
                    break;
                forEachOfKind<K, SymT>(block, fn);
                break;
            }
            case SymbolKind::GenerateBlockArray:
                for (auto& entry : member.as<GenerateBlockArraySymbol>().entries)
                    forEachOfKind<K, SymT>(*entry, fn);
                break;
            case SymbolKind::InstanceArray:
                forEachOfKind<K, SymT>(member.as<InstanceArraySymbol>(), fn);
                break;
            default:
                break;
        }
    }
}

template<typename F>
inline void forEachInstance(const Scope& scope, F&& fn) {
    forEachOfKind<SymbolKind::Instance, InstanceSymbol>(scope, fn);
}

/// The decl_kind word for a net or variable.
inline std::string declarationKindOf(const Symbol& sym) {
    if (sym.kind != SymbolKind::Net)
        return "variable";
    auto& nt = sym.as<NetSymbol>().netType;
    switch (nt.netKind) {
        case NetType::Wire:         return "wire";
        case NetType::WAnd:         return "wand";
        case NetType::WOr:          return "wor";
        case NetType::Tri:          return "tri";
        case NetType::TriAnd:       return "triand";
        case NetType::TriOr:        return "trior";
        case NetType::Tri0:         return "tri0";
        case NetType::Tri1:         return "tri1";
        case NetType::TriReg:       return "trireg";
        case NetType::Supply0:      return "supply0";
        case NetType::Supply1:      return "supply1";
        case NetType::UWire:        return "uwire";
        case NetType::Interconnect: return "interconnect";
        case NetType::UserDefined:
            return nt.name.empty() ? "wire" : std::string(nt.name);
        default:                    return "wire";
    }
}

inline std::string directionWord(ArgumentDirection d) {
    switch (d) {
        case ArgumentDirection::In:    return "input";
        case ArgumentDirection::Out:   return "output";
        case ArgumentDirection::InOut: return "inout";
        default:                       return "ref";
    }
}

} // namespace designdb::detail
