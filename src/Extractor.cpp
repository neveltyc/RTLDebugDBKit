// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)

#include "Extractor.h"

#include <algorithm>
#include <optional>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "slang/analysis/AnalysisManager.h"
#include "slang/analysis/AnalyzedProcedure.h"
#include "slang/analysis/ValueDriver.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/expressions/AssertionExpr.h"
#include "slang/ast/Expression.h"
#include "slang/ast/HierarchicalReference.h"
#include "slang/ast/Scope.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/ConditionalStatements.h"
#include "slang/ast/statements/LoopStatements.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/types/NetType.h"
#include "slang/ast/types/Type.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/ValuePath.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/TimingControl.h"
#include "slang/numeric/SVInt.h"
#include "slang/numeric/ConstantValue.h"
#include "slang/syntax/SyntaxNode.h"
#include "slang/text/SourceManager.h"

using namespace slang;
using namespace slang::ast;
using namespace slang::analysis;

namespace designdb {

namespace {

/// True for a symbol that is a compile-time constant rather than a net.
///
/// An enum member or a parameter is not something a waveform carries and not
/// something a trace can step to, so it is not connectivity. Leaving them in
/// also swamped the count of genuinely dropped cross-module references.
bool isConstantSymbol(const ValueSymbol& sym) {
    switch (sym.kind) {
        case SymbolKind::EnumValue:
        case SymbolKind::Parameter:
        case SymbolKind::Specparam:
            return true;
        case SymbolKind::Variable:
            return sym.as<VariableSymbol>().flags.has(VariableFlags::Const);
        default:
            return false;
    }
}

/// The path of `sym` as seen from `body`, i.e. with the instance's own prefix
/// removed. Rows name objects scope-relative (`g[0].sig`, `bump.v`); the
/// template is stamped per occurrence, so the relative spelling is what every
/// occurrence shares.
bool relativePath(const Symbol& sym, const std::string& bodyPrefix, std::string& out) {
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

std::string typeOf(const ValueSymbol& sym) {
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

Where whereOf(SourceLocation loc, const SourceManager& sm) {
    if (!loc)
        return {};
    return Where{std::string(sm.getFileName(loc)),
                 static_cast<uint32_t>(sm.getLineNumber(loc)),
                 static_cast<uint32_t>(sm.getColumnNumber(loc))};
}

/// The canonical text of a reference that leaves its instance: the path as
/// written, with every select resolved to the constant it elaborated to.
/// (See v9's history for why not the raw source text: generate loops share a
/// spelling across distinct references, spellings differ in whitespace, and
/// macro-assembled references span buffers.)
std::string canonicalPath(const Expression* e, EvalContext& eval) {
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
std::string normalizedText(const Expression* e, const SourceManager& sm,
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
std::string delayText(const TimingControl* t) {
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
std::string assertionWord(AssertionKind kind) {
    switch (kind) {
        case AssertionKind::Assume:        return "assume";
        case AssertionKind::CoverProperty:
        case AssertionKind::CoverSequence: return "cover";
        case AssertionKind::Restrict:      return "restrict";
        case AssertionKind::Expect:        return "expect";
        default:                           return "assert";
    }
}

/// The procedure_kind word for a procedural block.
std::string procedureWord(const Symbol& sym) {
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
void collectEdgeEvents(const TimingControl* t,
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
/// full precision (ConstantValue::toString abbreviates above 128 bits, which
/// folded distinct parameterisations).
std::string parameterText(const InstanceBodySymbol& body) {
    std::string out;
    for (auto& member : body.members()) {
        if (!out.empty() && (member.kind == SymbolKind::Parameter ||
                             member.kind == SymbolKind::TypeParameter))
            out += ',';
        if (member.kind == SymbolKind::Parameter) {
            auto& p = member.as<ParameterSymbol>();
            out += std::string(p.name);
            out += '=';
            out += p.getValue().toString(SVInt::MAX_BITS);
        }
        else if (member.kind == SymbolKind::TypeParameter) {
            auto& tp = member.as<TypeParameterSymbol>();
            out += std::string(tp.name);
            out += '=';
            out += tp.targetType.getType().toString();
        }
    }
    return out;
}

/// One reference to a signal, with the bits it touches. Encoding unchanged
/// from v7: `whole` spans the object, `exact=false` means the range is an
/// upper bound (a dynamic selector).
struct Ref {
    const ValueSymbol* sym = nullptr;
    uint64_t lo = 0;
    uint64_t hi = 0;
    bool whole = true;
    bool exact = true;
    /// The expression the reference was written as; consulted when the symbol
    /// lives outside the instance (hier_ref text) and for resolution replay.
    const Expression* origin = nullptr;
};

uint64_t bitWidthOf(const ValueSymbol& sym) {
    return sym.getType().getSelectableWidth();
}

/// Builds a Ref from one of slang's value paths.
Ref refOf(const ValuePath& path) {
    Ref r;
    r.sym = path.rootSymbol();
    if (!r.sym)
        return r;
    r.origin = path.fullExpr;
    r.exact = path.isFullyStatic();
    if (!path.lsp) {
        r.exact = false;
        return r;
    }
    r.lo = path.lspBounds.first;
    r.hi = path.lspBounds.second;
    const uint64_t width = bitWidthOf(*r.sym);
    r.whole = width == 0 || (r.lo == 0 && r.hi + 1 >= width);
    return r;
}

/// Constants filtered per statement; see the v9 note on why accumulated
/// across the three collection passes of one assignment.
inline thread_local int64_t filteredConstants = 0;

void collectRefs(const Expression& expr, EvalContext& ctx, std::vector<Ref>& out,
                 bool skipSelectors = false) {
    ValuePath::visitPaths(
        expr, ctx,
        [&](const ValuePath& path) {
            Ref r = refOf(path);
            if (!r.sym)
                return;
            if (isConstantSymbol(*r.sym)) {
                filteredConstants++;
                return;
            }
            out.push_back(r);
        },
        skipSelectors);
}

/// A reference together with the bits of the *assignment* it occupies -- what
/// makes `{a, b} = {x, y}` answerable without the v7 cross product.
struct Slot {
    Ref ref;
    uint64_t lo = 0;
    uint64_t hi = 0;
    /// True when the reference's own bits map one-to-one onto [lo, hi].
    bool positional = false;
};

constexpr uint64_t kNoWidth = ~uint64_t{0};

/// How many subroutine bodies one module may instantiate across all its
/// procedures.
///
/// Per-call-site expansion is what makes a call's gating and delay its own,
/// but the cycle guard bounds only recursion, not fan-out: a call DAG
/// branching twice per level costs 2^depth, and 21 such levels turned an
/// 88-line file into 3.1 M statements and 1.2 GB. This bounds that.
///
/// The number is set from measurement, not taste: of the designs exported
/// here, the heaviest user of calls is picorv32 with 13 call statements,
/// and tinyriscv and VeeRwolf have none. Four thousand is two orders of
/// magnitude above that, so real RTL -- and any testbench short of a
/// deliberately exponential one -- never reaches it. What it stops is
/// counted and reported, never silently dropped.
constexpr int64_t kCallExpansionBudget = 4000;

uint64_t exprWidthOf(const Expression& e) {
    return e.type ? e.type->getBitWidth() : 0;
}

/// Whether the expression *is* a reference to storage rather than a
/// computation over one; selects and width-preserving conversions are
/// transparent, everything else answers no.
bool isPlainReference(const Expression& e) {
    const Expression* p = &e;
    for (;;) {
        switch (p->kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue:
                return true;
            case ExpressionKind::ElementSelect:
                p = &p->as<ElementSelectExpression>().value();
                break;
            case ExpressionKind::RangeSelect:
                p = &p->as<RangeSelectExpression>().value();
                break;
            case ExpressionKind::MemberAccess:
                p = &p->as<MemberAccessExpression>().value();
                break;
            case ExpressionKind::Conversion: {
                auto& conv = p->as<ConversionExpression>();
                if (exprWidthOf(conv.operand()) != exprWidthOf(*p))
                    return false;
                p = &conv.operand();
                break;
            }
            default:
                return false;
        }
    }
}

/// Every reference in `expr`, tagged with the bits of the assignment it
/// occupies. Concatenations and simple assignment patterns are positioned
/// element by element, MSB first; conversions are transparent when width-
/// preserving or truncating and degrade to range-level when widening.
void collectSlots(const Expression& expr, EvalContext& ctx, uint64_t base,
                  std::vector<Slot>& out, bool skipSelectors = false) {
    const uint64_t width = exprWidthOf(expr);

    const bool elementwise = expr.kind == ExpressionKind::Concatenation ||
                             expr.kind == ExpressionKind::SimpleAssignmentPattern;
    if (elementwise && width) {
        auto ops = expr.kind == ExpressionKind::Concatenation
                       ? expr.as<ConcatenationExpression>().operands()
                       : expr.as<SimpleAssignmentPatternExpression>().elements();
        uint64_t cursor = base + width;
        for (auto* op : ops) {
            if (!op)
                continue;
            const uint64_t w = exprWidthOf(*op);
            if (!w) {
                std::vector<Ref> rest;
                collectRefs(expr, ctx, rest, skipSelectors);
                for (auto& r : rest)
                    out.push_back(Slot{r, 0, kNoWidth, false});
                return;
            }
            cursor -= w;
            collectSlots(*op, ctx, cursor, out, skipSelectors);
        }
        return;
    }

    if (expr.kind == ExpressionKind::Conversion) {
        auto& conv = expr.as<ConversionExpression>();
        const uint64_t iw = exprWidthOf(conv.operand());
        if (width && iw >= width) {
            collectSlots(conv.operand(), ctx, base, out, skipSelectors);
            return;
        }
    }

    std::vector<Ref> refs;
    collectRefs(expr, ctx, refs, skipSelectors);
    if (!width) {
        for (auto& r : refs)
            out.push_back(Slot{r, 0, kNoWidth, false});
        return;
    }
    const bool positional =
        isPlainReference(expr) && refs.size() == 1 && refs[0].exact &&
        (refs[0].whole ? bitWidthOf(*refs[0].sym) == width
                       : refs[0].hi - refs[0].lo + 1 == width);
    for (auto& r : refs)
        out.push_back(Slot{r, base, base + width - 1, positional});
}

bool slotsOverlap(const Slot& a, const Slot& b, uint64_t& lo, uint64_t& hi) {
    if (a.hi == kNoWidth || b.hi == kNoWidth) {
        lo = 0;
        hi = kNoWidth;
        return true;
    }
    lo = std::max(a.lo, b.lo);
    hi = std::min(a.hi, b.hi);
    return lo <= hi;
}

/// The reference narrowed to the part of it landing in [lo, hi] of the
/// assignment. Only meaningful for a positional slot.
Ref narrowed(const Slot& s, uint64_t lo, uint64_t hi) {
    Ref r = s.ref;
    if (!s.positional || s.hi == kNoWidth || hi == kNoWidth || !r.sym)
        return r;
    if (lo <= s.lo && hi >= s.hi)
        return r;
    const uint64_t offset = r.whole ? 0 : r.lo;
    r.lo = offset + (lo - s.lo);
    r.hi = offset + (hi - s.lo);
    const uint64_t total = bitWidthOf(*r.sym);
    r.whole = total == 0 || (r.lo == 0 && r.hi + 1 >= total);
    return r;
}

struct StatementRefCollector : ASTVisitor<StatementRefCollector, VisitFlags::AllGood> {
    std::vector<Ref>& out;
    explicit StatementRefCollector(std::vector<Ref>& out) : out(out) {}
    void handle(const NamedValueExpression& e) { addRef(e); }
    void handle(const HierarchicalValueExpression& e) { addRef(e); }
    /// An assignment's target is written, not read. Visiting it as an
    /// ordinary value made a subroutine's *writes* come back as reads of
    /// the call site: `task touch(); freewr = freerd; endtask` reported
    /// freewr as read at the call, though nothing in the design reads it,
    /// and the same database showed it with a single driver -- two rows
    /// contradicting each other. The selectors on the left ARE reads
    /// (`m[i] = x` reads i), so they are still visited.
    void handle(const AssignmentExpression& e) {
        collectLeftSelectorReads(e.left());
        e.right().visit(*this);
    }
    void collectLeftSelectorReads(const Expression& lhs) {
        switch (lhs.kind) {
            case ExpressionKind::ElementSelect: {
                auto& sel = lhs.as<ElementSelectExpression>();
                sel.selector().visit(*this);
                collectLeftSelectorReads(sel.value());
                return;
            }
            case ExpressionKind::RangeSelect: {
                auto& sel = lhs.as<RangeSelectExpression>();
                sel.left().visit(*this);
                sel.right().visit(*this);
                collectLeftSelectorReads(sel.value());
                return;
            }
            case ExpressionKind::MemberAccess:
                collectLeftSelectorReads(lhs.as<MemberAccessExpression>().value());
                return;
            case ExpressionKind::Concatenation:
                for (auto* op : lhs.as<ConcatenationExpression>().operands())
                    collectLeftSelectorReads(*op);
                return;
            default:
                return;
        }
    }
    void addRef(const ValueExpressionBase& e) {
        Ref r;
        r.sym = &e.symbol;
        r.origin = &e;
        // No bounds computed here: the whole object is an upper bound, and
        // storing it as exact would state uncertainty as fact.
        r.exact = false;
        out.push_back(r);
    }
};

template<typename NodeT>
void collectStatementRefs(const NodeT& node, std::vector<Ref>& out) {
    StatementRefCollector c(out);
    node.visit(c);
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const Ref& r) {
                                 if (!r.sym)
                                     return true;
                                 if (isConstantSymbol(*r.sym)) {
                                     filteredConstants++;
                                     return true;
                                 }
                                 return false;
                             }),
              out.end());
}

/// A called subroutine's free reads -- what it samples beyond its arguments.
void collectCallReadsInto(const Expression& expr,
                          std::set<const SubroutineSymbol*>& active,
                          std::vector<Ref>& out) {
    struct CallFinder : ASTVisitor<CallFinder, VisitFlags::AllGood> {
        std::set<const SubroutineSymbol*>& active;
        std::vector<Ref>& out;
        CallFinder(std::set<const SubroutineSymbol*>& active, std::vector<Ref>& out) :
            active(active), out(out) {}
        void handle(const CallExpression& call) {
            visitDefault(call);
            auto sub = std::get_if<const SubroutineSymbol*>(&call.subroutine);
            if (!sub || !*sub)
                return;
            if (!active.insert(*sub).second)
                return;
            std::vector<Ref> inner;
            collectStatementRefs((*sub)->getBody(), inner);
            const std::string scope = (*sub)->getHierarchicalPath();
            for (auto& r : inner) {
                std::string path = r.sym->getHierarchicalPath();
                const bool isLocal = path.size() > scope.size() &&
                                     path.compare(0, scope.size(), scope) == 0 &&
                                     path[scope.size()] == '.';
                if (!isLocal)
                    out.push_back(r);
            }
            active.erase(*sub);
        }
    };
    CallFinder finder(active, out);
    expr.visit(finder);
}

/// The value symbols an expression reads, subroutine free reads included.
struct ReadCollector : public ASTVisitor<ReadCollector, VisitFlags::AllGood> {
    std::vector<const ValueSymbol*>& out;
    std::set<const SubroutineSymbol*>& active;
    explicit ReadCollector(std::vector<const ValueSymbol*>& out,
                           std::set<const SubroutineSymbol*>& active) :
        out(out), active(active) {}

    void handle(const NamedValueExpression& e) {
        if (!isConstantSymbol(e.symbol))
            out.push_back(&e.symbol);
    }
    void handle(const HierarchicalValueExpression& e) {
        if (!isConstantSymbol(e.symbol))
            out.push_back(&e.symbol);
    }
    void handle(const CallExpression& e) {
        visitDefault(e);
        auto sub = std::get_if<const SubroutineSymbol*>(&e.subroutine);
        if (!sub || !*sub)
            return;
        if (!active.insert(*sub).second)
            return;
        std::vector<const ValueSymbol*> inner;
        ReadCollector c(inner, active);
        (*sub)->getBody().visit(c);
        const std::string scope = (*sub)->getHierarchicalPath();
        for (auto* sym : inner) {
            std::string path = sym->getHierarchicalPath();
            const bool isLocal = path.size() > scope.size() &&
                                 path.compare(0, scope.size(), scope) == 0 &&
                                 path[scope.size()] == '.';
            if (!isLocal)
                out.push_back(sym);
        }
        active.erase(*sub);
    }
};

void collectReads(const Expression& expr, std::vector<const ValueSymbol*>& out) {
    std::set<const SubroutineSymbol*> active;
    ReadCollector c(out, active);
    expr.visit(c);
}

/// The path segment slang gives a generate block: the genvar's *value* for a
/// loop iteration, the block's name otherwise.
std::string generateSegment(const GenerateBlockSymbol& block) {
    if (auto* index = block.getArrayIndex())
        return "[" + index->toString(LiteralBase::Decimal, false) + "]";
    std::string name(block.name);
    return name.empty() ? block.getExternalName() : name;
}

/// The last segment of a symbol's hierarchical path: the leaf name with
/// slang's array-element index already rendered in source numbering
/// (`u[0]`) -- the one spelling a tree node's name must use, since an
/// instance-array element's own `name` is the bare `u`.
std::string leafSegment(const Symbol& sym) {
    std::string full = sym.getHierarchicalPath();
    const size_t dot = full.rfind('.');
    return dot == std::string::npos ? full : full.substr(dot + 1);
}

/// Calls `fn` for every member of `scope` of kind `K`, descending through
/// generate blocks and instance arrays but never into an instance's own body.
template<SymbolKind K, typename SymT, typename F>
void forEachOfKind(const Scope& scope, F&& fn) {
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
void forEachInstance(const Scope& scope, F&& fn) {
    forEachOfKind<SymbolKind::Instance, InstanceSymbol>(scope, fn);
}

/// The declaration_kind word for a net or variable.
std::string declarationKindOf(const Symbol& sym) {
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

std::string directionWord(ArgumentDirection d) {
    switch (d) {
        case ArgumentDirection::In:    return "input";
        case ArgumentDirection::Out:   return "output";
        case ArgumentDirection::InOut: return "inout";
        default:                       return "ref";
    }
}

// ---------------------------------------------------------------- templates
//
// Everything a group's occurrences share, held with template-local indices.
// Stamping is then arithmetic: global id = per-occurrence base + index. The
// invariant that makes this sound: two bodies with one (definition,
// parameters) key are the same AST, so any deterministic traversal of one is
// the same traversal of the other.

struct TplLoc {
    int64_t fileId = 0;
    uint32_t line = 0;
    uint32_t column = 0;
};

struct TplRange {
    std::optional<std::pair<uint64_t, uint64_t>> bits;
    bool exact = true;
};

TplRange rangeOf(const Ref& r) {
    TplRange out;
    if (r.sym && !r.whole)
        out.bits = std::make_pair(r.lo, r.hi);
    out.exact = r.sym ? r.exact : true;
    return out;
}

struct TplScope {          // a generate level below the instance; 0 = the body
    int32_t parent = -1;
    std::string name;
};

struct TplNet {
    int32_t scope = 0;
    std::string name;
    std::string declKind;
    int64_t dataTypeId = 0;
    int64_t width = -1;
    bool isImplicit = false;
    TplLoc loc;
};

struct TplTerm {
    std::string name;
    std::string kind;        // signal | interface
    std::string direction;   // "" = NULL
    int64_t dataTypeId = 0;
    int64_t width = -1;
    int isConst = -1;
    std::string modport;
    TplLoc loc;
};

struct TplTermMap {
    int32_t term = 0;
    int64_t ordinal = 0;
    int32_t net = 0;
    TplRange termR, netR;
    bool mappingExact = false;
};

struct TplProcedure {
    int32_t scope = 0;
    std::string name;
    std::string kind;
    TplLoc loc;
};

struct TplStmt {
    int32_t scope = 0;
    int32_t proc = -1;
    int64_t sequence = -1;
    std::string kind;
    std::string construct;
    std::string assignKind;  // "" = NULL
    std::string delay;
    int64_t dropped = 0;
    TplLoc loc;
};

struct TplStmtRef {          // assign_target and assign_operand share the shape
    int32_t stmt = 0;
    int64_t ordinal = 0;
    int32_t net = 0;
    TplRange r;
};

struct TplExprRef {
    int32_t stmt = 0;
    int64_t ordinal = 0;
    int32_t net = 0;
    std::string role;
    TplRange r;
};

struct TplProcEvent {
    int32_t proc = 0;
    int32_t stmt = -1;
    int32_t net = -1;
    std::string eventKind;
    std::string edgeKind;
    TplLoc loc;
};

struct TplPrim {
    int32_t scope = 0;
    std::string name;
    std::string primKind;
    std::string defName;
    TplLoc loc;
};

struct TplDep {
    int32_t srcNet = -1;     // -1 = constant source
    int32_t tgtNet = 0;
    int32_t stmt = -1;
    int32_t operandRef = -1;
    int32_t targetRef = -1;
    int32_t exprRef = -1;
    int32_t prim = -1;
    std::string kind;
    TplRange srcR, tgtR;
    int mappingExact = -1;
};

/// Replay data for one outward reference: how to find the target from an
/// occurrence of this template. `kind` decides the walk.
struct TplHierRef {
    int32_t stmt = -1;
    std::string path;
    std::string access;      // read | write | connect
    TplRange r;
    TplLoc loc;
    enum ResolveKind {
        None,                // slang gave no target usable per occurrence
        Downward,            // segs descend from the occurrence's own node
        Absolute,            // segs descend from the design root
        ViaIfaceTerm         // segs descend from the interface bound to term
    } resolve = None;
    int32_t ifaceTerm = -1;  // ViaIfaceTerm: which of this template's terms
    std::vector<std::string> segs;   // tree segments to descend
    std::string netName;     // scope-relative net name at the target instance
};

/// One dependency with at least one end outside the instance, paired where
/// the statement was walked -- per (source element, target element), never
/// by joining afterwards -- and materialised once the occurrence's
/// references resolve. An end is a local net index or a hierRefs index,
/// never both.
struct TplCrossDep {
    std::string kind;        // data | control | procedure
    int32_t stmt = -1;
    int32_t srcNet = -1;
    int32_t srcHref = -1;
    int32_t tgtNet = -1;
    int32_t tgtHref = -1;
    int32_t operandRef = -1;
    int32_t targetRef = -1;
    int32_t exprRef = -1;
    TplRange srcR, tgtR;
    int mappingExact = -1;
};

struct TplConn {
    std::string kind;        // net_conn.connection_kind
    int32_t parentNet = -1;  // index into the PARENT template's nets
    int32_t childTerm = -1;  // index into the child template's terms
    int64_t ordinal = 0;     // segment ordinal within that terminal
    TplRange netR;
    int netExact = -1;       // -1 = no net end (writer NULLs the range too)
    TplRange termR;
    int termExact = -1;
    int mappingExact = -1;
    int32_t ifaceChild = -1; // interface binding to a sibling child
    int32_t ifaceOwnTerm = -1; // interface pass-through of the parent's port
    int32_t hierRef = -1;    // external tie: index into parent's hierRefs
    TplLoc loc;
};

struct TplChild {
    int32_t scope = 0;
    std::string name;        // ONE path segment
    enum Kind { Module, Unresolved } kind = Module;
    std::string groupKey;    // Module: which template to stamp
    std::string defName;     // Unresolved: the definition as written
    std::vector<std::string> unresolvedPorts;  // Unresolved: term names in order
    std::vector<TplConn> conns;
    TplLoc loc;
};

struct Template {
    int64_t moduleId = 0;
    std::string params;
    std::vector<TplScope> scopes;
    std::vector<TplNet> nets;
    std::vector<TplTerm> terms;
    std::vector<TplTermMap> termMaps;
    std::vector<TplProcedure> procedures;
    std::vector<TplStmt> stmts;
    std::vector<TplStmtRef> targets;
    std::vector<TplStmtRef> operands;
    std::vector<TplExprRef> exprRefs;
    std::vector<TplProcEvent> procEvents;
    std::vector<TplPrim> prims;
    std::vector<TplDep> deps;
    std::vector<TplHierRef> hierRefs;
    std::vector<TplCrossDep> crossDeps;
    std::vector<TplChild> children;
    std::unordered_map<std::string, int32_t> termIndex;  // name -> terms index
    std::unordered_map<std::string, int32_t> netIndex;   // name -> nets index
    bool hasResolvableRefs = false;
    bool built = false;
};

// ------------------------------------------------------- statement walking
//
// Walks a procedure statement by statement. Ported from v9 with the callback
// layer reshaped: a target arrives with its paired operands and the gating
// stack in one call, because the template needs the pairing (net_dep names
// the operand and target rows) rather than a stream of independent edges.

/// One operand paired with the part of the target its bits actually reach.
///
/// Both ends are narrowed to the overlap, not just the source. Keeping the
/// target whole while narrowing the source is what made
/// `assign swap = {c[3:0], c[7:4]}` export two dependencies each claiming
/// all eight bits of swap with mapping_exact=1 -- a four-bit source cannot
/// map one-to-one onto an eight-bit target, so the row was not merely
/// coarse but impossible, and it said the bytes were not swapped.
struct PairedSrc {
    Ref src;
    Ref tgt;
    bool mapExact = false;
    /// The source as the RTL spells it, before pairing narrowed it to this
    /// target's bits. Only a hier_ref row wants this: it describes the
    /// reference, not one dependency through it.
    Ref srcAsWritten;
};

struct StatementWalker : public ASTVisitor<StatementWalker, VisitFlags::AllGood> {
    /// One assignment target with everything that reaches it. `firstTarget`
    /// opens the statement; the remaining targets of a concatenated left-hand
    /// side share it.
    using EmitTarget = std::function<void(
        const Ref& dst, const std::vector<PairedSrc>& pairs,
        const std::vector<Ref>& gating, SourceRange where, int64_t seq,
        bool blocking, int64_t dropped, bool inSubroutine, bool firstTarget,
        const std::string& delay)>;
    /// A call site's actual bound to its formal, by argument direction.
    /// `bindable` is false when the call sits in a control expression, which
    /// belongs to no statement this schema records.
    using EmitCallBinding = std::function<void(const Ref& formal, const Ref& actual,
                                               bool reads, bool writes,
                                               bool oneToOne, bool bindable,
                                               SourceRange where)>;
    /// A statement-level event control (a wait, not sensitivity).
    using EmitEvent = std::function<void(const Expression* expr, const std::string& edge,
                                         int64_t seq, SourceRange where)>;
    /// A statement that reads without writing anything nameable: an
    /// assertion, a wait condition, a call, a system task.
    ///
    /// The gating stack comes with it. A condition was only ever recorded
    /// while building an assignment's control dependencies, so a branch
    /// holding nothing but `$display` or an assertion dropped its
    /// condition entirely -- `if (gate) $display(payload);` knew about
    /// payload and not about gate, in any procedure, implicit sensitivity
    /// or not. There is no target for a dependency here, but the read is
    /// real and belongs to the statement it gates.
    using EmitRead = std::function<void(const std::vector<Ref>& reads,
                                        const std::vector<Ref>& gating,
                                        const std::vector<Ref>& writes,
                                        const std::string& stmtKind,
                                        const std::string& construct, int64_t seq,
                                        SourceRange where)>;

    EmitTarget emitTarget;
    EmitCallBinding emitBinding;
    EmitEvent emitEvent;
    EmitRead emitRead;
    EvalContext& eval;
    const TimingControl* sensitivityTiming = nullptr;
    /// The delay control in force for statements below a `#d` timed statement,
    /// and for a continuous assign's own delay.
    std::string pendingDelay;
    std::vector<Ref> gating;
    int64_t seq = 0;
    std::set<const SubroutineSymbol*> activeSubs;
    std::set<const ValueSymbol*> loopVars;
    int subDepth = 0;
    /// Remaining subroutine-body instantiations for the whole template, and
    /// the count of call sites whose body was skipped once it ran out. Both
    /// owned by the caller: the budget spans every procedure of one module,
    /// since the blowup compounds across them.
    int64_t* budget = nullptr;
    int64_t* truncated = nullptr;
    /// Whether call bindings currently have a statement to attach to; cleared
    /// while visiting control expressions, whose calls belong to no statement
    /// this schema records.
    bool bindable = true;

    StatementWalker(EmitTarget t, EmitCallBinding b, EmitEvent e, EmitRead r,
                    EvalContext& eval) :
        emitTarget(std::move(t)), emitBinding(std::move(b)),
        emitEvent(std::move(e)), emitRead(std::move(r)), eval(eval) {}

    void handle(const ImmediateAssertionStatement& stmt) {
        std::vector<Ref> reads;
        collectRefs(stmt.cond, eval, reads);
        emitRead(reads, gating, {}, "assertion",
                 assertionWord(stmt.assertionKind), seq++,
                 stmt.sourceRange);
        visitDefault(stmt);
    }

    void handle(const ConcurrentAssertionStatement& stmt) {
        std::vector<Ref> reads;
        collectStatementRefs(stmt.propertySpec, reads);
        emitRead(reads, gating, {}, "assertion",
                 assertionWord(stmt.assertionKind), seq++,
                 stmt.sourceRange);
        visitDefault(stmt);
    }

    void handle(const WaitStatement& stmt) {
        std::vector<Ref> reads;
        collectRefs(stmt.cond, eval, reads);
        emitRead(reads, gating, {}, "wait", "wait", seq++, stmt.sourceRange);
        visitDefault(stmt);
    }

    /// A statement whose whole effect is a call: `$display(...)`, `t(a, b);`.
    /// The statement row exists for writing calls too -- the call is where
    /// the actual-to-formal bindings hang -- but only its *reads* are
    /// recorded here; a written argument's assignment is walked inside the
    /// call expression as usual.
    ///
    /// Only what the call site itself names is read here. A user
    /// subroutine's own reads are recorded by walking its body, which v10
    /// does once per call site -- summarising them here as well reported
    /// every one of them twice, as a `dataflow` load from the body and a
    /// `statement` load from the call, against a schema that promises one
    /// read is one row. A system task has no body to walk, so its free
    /// reads still have to be gathered.
    void handle(const ExpressionStatement& stmt) {
        if (stmt.expr.kind != ExpressionKind::Call) {
            visitDefault(stmt);
            return;
        }
        auto& call = stmt.expr.as<CallExpression>();
        std::vector<Ref> reads;
        collectRefs(stmt.expr, eval, reads);
        if (call.isSystemCall()) {
            std::set<const SubroutineSymbol*> active;
            collectCallReadsInto(stmt.expr, active, reads);
        }
        std::set<const ValueSymbol*> written;
        std::vector<Ref> writeRefs;
        collectWrittenTargets(stmt.expr, written, &writeRefs);
        if (!written.empty()) {
            reads.erase(std::remove_if(reads.begin(), reads.end(),
                                       [&](const Ref& r) {
                                           return r.sym && written.count(r.sym);
                                       }),
                        reads.end());
        }
        const bool sys = call.isSystemCall();
        // A system task that writes an argument -- $readmemh into a memory,
        // $sscanf into a variable, $cast into its destination -- really does
        // drive it, and slang models the write as an assignment inside the
        // call. A user subroutine's write is covered by the formal binding,
        // so only the system case needs targets of its own; without them the
        // argument read as undriven and its procedure as one that wrote
        // nothing at all.
        emitRead(reads, gating, sys ? writeRefs : std::vector<Ref>{},
                 sys ? "system_task" : "call", callWord(call), seq++,
                 stmt.sourceRange);
        visitDefault(stmt);
    }

    void collectWrittenTargets(const Expression& expr,
                               std::set<const ValueSymbol*>& out,
                               std::vector<Ref>* refs = nullptr) {
        struct Finder : ASTVisitor<Finder, VisitFlags::AllGood> {
            StatementWalker& self;
            std::set<const ValueSymbol*>& out;
            std::vector<Ref>* refs;
            Finder(StatementWalker& self, std::set<const ValueSymbol*>& out,
                   std::vector<Ref>* refs) :
                self(self), out(out), refs(refs) {}
            void handle(const AssignmentExpression& e) {
                std::vector<Ref> targets;
                collectRefs(e.left(), self.eval, targets, /*skipSelectors=*/true);
                for (auto& t : targets) {
                    if (!t.sym)
                        continue;
                    out.insert(t.sym);
                    if (refs)
                        refs->push_back(t);
                }
                visitDefault(e);
            }
        };
        Finder f(*this, out, refs);
        expr.visit(f);
    }

    static std::string callWord(const CallExpression& call) {
        if (call.isSystemCall())
            return std::string(call.getSubroutineName());
        return "call";
    }

    /// A statement-level timing control: an event control is a wait; a delay
    /// control is carried onto the statements it prefixes.
    void handle(const TimedStatement& stmt) {
        if (&stmt.timing != sensitivityTiming) {
            std::vector<std::pair<const Expression*, std::string>> raw;
            collectEdgeEvents(&stmt.timing, raw);
            for (auto& [expr, edge] : raw)
                emitEvent(expr, edge, seq++, stmt.sourceRange);
        }
        const std::string d = delayText(&stmt.timing);
        if (!d.empty()) {
            const std::string saved = pendingDelay;
            pendingDelay = d;
            visitDefault(stmt);
            pendingDelay = saved;
            return;
        }
        visitDefault(stmt);
    }

    /// Visits the condition expressions of a branch with `bindable` off --
    /// a call written INSIDE a condition belongs to no statement this
    /// schema records -- and leaves it on for the branch bodies, whose
    /// calls are ordinary statements of their own.
    ///
    /// Clearing it across the whole subtree instead cost every gated call
    /// its statement binding: `if (g) put(b);` produced a `procedure`
    /// dependency with stmt_id and expr_ref_id NULL, while the same call
    /// written ungated kept both. That is exactly the shape the per-call-
    /// site walk exists to record, and every call site in the motivating
    /// case is gated.
    template<typename F>
    void visitGuarded(F&& visitConditions) {
        const bool saved = bindable;
        bindable = false;
        visitConditions();
        bindable = saved;
    }

    void handle(const ConditionalStatement& stmt) {
        const size_t mark = gating.size();
        visitGuarded([&] {
            for (auto& cond : stmt.conditions) {
                collectRefs(*cond.expr, eval, gating);
                cond.expr->visit(*this);
            }
        });
        stmt.ifTrue.visit(*this);
        if (stmt.ifFalse)
            stmt.ifFalse->visit(*this);
        gating.resize(mark);
    }

    void handle(const CaseStatement& stmt) {
        const size_t mark = gating.size();
        visitGuarded([&] {
            collectRefs(stmt.expr, eval, gating);
            stmt.expr.visit(*this);
            for (auto& item : stmt.items) {
                for (auto* label : item.expressions) {
                    collectRefs(*label, eval, gating);
                    label->visit(*this);
                }
            }
        });
        for (auto& item : stmt.items) {
            if (item.stmt)
                item.stmt->visit(*this);
        }
        if (stmt.defaultCase)
            stmt.defaultCase->visit(*this);
        gating.resize(mark);
    }

    void handle(const ForLoopStatement& stmt) {
        const size_t mark = gating.size();
        if (stmt.stopExpr)
            collectRefs(*stmt.stopExpr, eval, gating);
        const size_t loopMark = loopVars.size();
        for (auto* v : stmt.loopVars)
            loopVars.insert(v);
        visitDefault(stmt);
        if (loopVars.size() != loopMark) {
            for (auto* v : stmt.loopVars)
                loopVars.erase(v);
        }
        gating.resize(mark);
    }

    void handle(const WhileLoopStatement& stmt) {
        const size_t mark = gating.size();
        collectRefs(stmt.cond, eval, gating);
        visitDefault(stmt);
        gating.resize(mark);
    }

    void handle(const RepeatLoopStatement& stmt) {
        const size_t mark = gating.size();
        collectRefs(stmt.count, eval, gating);
        visitDefault(stmt);
        gating.resize(mark);
    }

    /// `x++` / `--x`: an assignment in everything but its expression kind.
    void handle(const UnaryExpression& expr) {
        switch (expr.op) {
            case UnaryOperator::Preincrement:
            case UnaryOperator::Predecrement:
            case UnaryOperator::Postincrement:
            case UnaryOperator::Postdecrement:
                break;
            default:
                visitDefault(expr);
                return;
        }
        std::vector<Ref> targets;
        collectRefs(expr.operand(), eval, targets, /*skipSelectors=*/true);
        for (auto& dst : targets) {
            if (loopVars.count(dst.sym))
                continue;
            // Reads and writes the same bits of the same object -- as
            // positional as a mapping gets.
            emitTarget(dst, {PairedSrc{dst, dst, true, dst}}, gating, expr.sourceRange,
                       seq++, true, 0, subDepth > 0, /*firstTarget=*/true,
                       pendingDelay);
        }
        visitDefault(expr);
    }

    void handle(const CallExpression& expr) {
        visitDefault(expr);
        auto sub = std::get_if<const SubroutineSymbol*>(&expr.subroutine);
        if (!sub || !*sub)
            return;
        bindArguments(expr, **sub);
        if (!activeSubs.insert(*sub).second)
            return;                       // recursion guard
        // Per CALL SITE, deliberately. Walking the body once per subroutine
        // read cleaner but lost call-site semantics: in
        // `if (g1) put(d1); if (g2) put(d2);` the body's `q <= v` inherited
        // g1's gating only, so g2 -> q never existed and the driver cone
        // depended on which call was walked first. The body's statements
        // are the effect of THIS call -- its gating stack, its delay -- so
        // each call instantiates them, exactly as the occurrence model
        // stamps each instance.
        //
        // The cost is body rows per call site, and it compounds: the cycle
        // guard above stops recursion but not fan-out, so a call DAG where
        // each level calls the next twice costs 2^depth. Measured at 21
        // such levels: 3.1 M statements and 1.2 GB from an 88-line file.
        // The budget bounds that. It is deliberately generous -- ordinary
        // RTL never approaches it -- and what it skips is counted rather
        // than silently dropped, so a truncated export says so.
        if (budget && *budget <= 0) {
            if (truncated)
                (*truncated)++;
            activeSubs.erase(*sub);
            return;
        }
        if (budget)
            (*budget)--;
        subDepth++;
        (*sub)->getBody().visit(*this);
        subDepth--;
        activeSubs.erase(*sub);
    }

    /// The actuals at a call site, tied to the formals they bind to; an
    /// `input` formal is fed by the actual, an `output` feeds it, and
    /// `inout`/`ref` do both.
    void bindArguments(const CallExpression& expr, const SubroutineSymbol& sub) {
        auto args = expr.arguments();
        auto formals = sub.getArguments();
        const size_t n = std::min(args.size(), formals.size());
        for (size_t i = 0; i < n; i++) {
            if (!args[i] || !formals[i])
                continue;
            const auto dir = formals[i]->direction;
            const bool writes = dir == ArgumentDirection::Out ||
                                dir == ArgumentDirection::InOut ||
                                dir == ArgumentDirection::Ref;
            const bool reads = dir == ArgumentDirection::In ||
                               dir == ArgumentDirection::InOut ||
                               dir == ArgumentDirection::Ref;
            Ref formal;
            formal.sym = formals[i];
            formal.origin = args[i];
            std::vector<Ref> actuals;
            collectRefs(*args[i], eval, actuals, /*skipSelectors=*/writes);
            for (auto& a : actuals) {
                if (!a.sym)
                    continue;
                // Whole-to-whole only when the actual IS a reference filling
                // the formal -- the same leaf rule every positional claim
                // answers to.
                const uint64_t fw = formal.sym ? bitWidthOf(*formal.sym) : 0;
                const bool oneToOne =
                    actuals.size() == 1 && fw != 0 &&
                    isPlainReference(*args[i]) && actuals[0].exact &&
                    (actuals[0].whole ? bitWidthOf(*actuals[0].sym) == fw
                                      : actuals[0].hi - actuals[0].lo + 1 == fw);
                emitBinding(formal, a, reads, writes, oneToOne, bindable,
                            expr.sourceRange);
            }
        }
    }

    void handle(const AssignmentExpression& expr) {
        // The copy-back slang synthesises for an `output`/`inout` actual:
        // `bump(i0, o0)` carries an assignment to o0 whose right side is an
        // empty placeholder. It is not a statement anyone wrote, and it has
        // no operands -- so recording it produced a source-less dependency,
        // which v_driver reports as a CONSTANT tie-off on a signal the task
        // plainly drives. The real record is the `procedure` dependency
        // from the formal, which bindArguments already makes.
        if (expr.right().kind == ExpressionKind::EmptyArgument) {
            visitDefault(expr);
            return;
        }
        std::vector<Ref> targets;
        collectRefs(expr.left(), eval, targets, /*skipSelectors=*/true);
        if (targets.empty()) {
            const Expression* root = &expr.left();
            for (;;) {
                if (root->kind == ExpressionKind::ElementSelect)
                    root = &root->as<ElementSelectExpression>().value();
                else if (root->kind == ExpressionKind::RangeSelect)
                    root = &root->as<RangeSelectExpression>().value();
                else if (root->kind == ExpressionKind::MemberAccess)
                    root = &root->as<MemberAccessExpression>().value();
                else
                    break;
            }
            if (root->kind == ExpressionKind::NamedValue ||
                root->kind == ExpressionKind::HierarchicalValue) {
                Ref r;
                r.sym = &root->as<ValueExpressionBase>().symbol;
                r.origin = root;
                targets.push_back(r);
            }
            else {
                visitDefault(expr);
                return;
            }
        }

        std::vector<Slot> lhsSlots;
        collectSlots(expr.left(), eval, 0, lhsSlots, /*skipSelectors=*/true);
        if (lhsSlots.size() != targets.size()) {
            lhsSlots.clear();
            for (auto& t : targets)
                lhsSlots.push_back(Slot{t, 0, kNoWidth, false});
        }

        std::vector<Slot> rhsSlots;
        filteredConstants = 0;
        collectSlots(expr.right(), eval, 0, rhsSlots);
        collectAuxSlots(expr.right(), 0, rhsSlots, /*selectors=*/false);
        collectAuxSlots(expr.left(), 0, rhsSlots, /*selectors=*/true);
        const int64_t droppedConstants = filteredConstants;

        eval.reset();

        // An intra-assignment delay (`a = #3 b;`) belongs to this statement
        // alone; a statement-level one arrives through pendingDelay.
        std::string delay = pendingDelay;
        if (expr.timingControl) {
            const std::string d = delayText(expr.timingControl);
            if (!d.empty())
                delay = d;
        }

        const int64_t stmtSeq = seq++;
        bool firstTarget = true;
        for (auto& dstSlot : lhsSlots) {
            if (loopVars.count(dstSlot.ref.sym))
                continue;
            std::vector<PairedSrc> pairs;
            for (auto& srcSlot : rhsSlots) {
                uint64_t lo = 0, hi = 0;
                if (!slotsOverlap(dstSlot, srcSlot, lo, hi))
                    continue;
                pairs.push_back(PairedSrc{narrowed(srcSlot, lo, hi),
                                          narrowed(dstSlot, lo, hi),
                                          dstSlot.positional && srcSlot.positional,
                                          srcSlot.ref});
            }
            emitTarget(dstSlot.ref, pairs, gating, expr.sourceRange, stmtSeq,
                       expr.isBlocking(), droppedConstants, subDepth > 0,
                       firstTarget, delay);
            firstTarget = false;
        }

        visitDefault(expr);
    }

    void collectLeftSelectorRefs(const Expression& lhs, std::vector<Ref>& out) {
        switch (lhs.kind) {
            case ExpressionKind::ElementSelect: {
                auto& sel = lhs.as<ElementSelectExpression>();
                collectRefs(sel.selector(), eval, out);
                collectLeftSelectorRefs(sel.value(), out);
                return;
            }
            case ExpressionKind::RangeSelect: {
                auto& sel = lhs.as<RangeSelectExpression>();
                collectRefs(sel.left(), eval, out);
                collectRefs(sel.right(), eval, out);
                collectLeftSelectorRefs(sel.value(), out);
                return;
            }
            case ExpressionKind::MemberAccess:
                collectLeftSelectorRefs(lhs.as<MemberAccessExpression>().value(), out);
                return;
            case ExpressionKind::Concatenation:
                for (auto* op : lhs.as<ConcatenationExpression>().operands())
                    collectLeftSelectorRefs(*op, out);
                return;
            default:
                return;
        }
    }

    void collectCallReads(const Expression& expr, std::vector<Ref>& out) {
        collectCallReadsInto(expr, activeSubs, out);
    }

    /// The reads that ride an element without occupying its bits -- a call's
    /// free reads on the right, a selector's index reads on the left -- each
    /// pinned to the WINDOW of the element they ride, never positional.
    void collectAuxSlots(const Expression& expr, uint64_t base,
                         std::vector<Slot>& out, bool selectors) {
        const uint64_t width = exprWidthOf(expr);
        const bool elementwise =
            expr.kind == ExpressionKind::Concatenation ||
            expr.kind == ExpressionKind::SimpleAssignmentPattern;
        if (elementwise && width) {
            auto ops = expr.kind == ExpressionKind::Concatenation
                           ? expr.as<ConcatenationExpression>().operands()
                           : expr.as<SimpleAssignmentPatternExpression>().elements();
            uint64_t cursor = base + width;
            bool bad = false;
            for (auto* op : ops) {
                if (!op)
                    continue;
                const uint64_t w = exprWidthOf(*op);
                if (!bad && (w == 0 || w > cursor - base))
                    bad = true;
                if (bad) {
                    std::vector<Ref> reads;
                    if (selectors)
                        collectLeftSelectorRefs(*op, reads);
                    else
                        collectCallReads(*op, reads);
                    for (auto& r : reads)
                        out.push_back(Slot{r, 0, kNoWidth, false});
                    continue;
                }
                cursor -= w;
                collectAuxSlots(*op, cursor, out, selectors);
            }
            return;
        }
        if (expr.kind == ExpressionKind::Conversion) {
            auto& conv = expr.as<ConversionExpression>();
            if (width && exprWidthOf(conv.operand()) >= width) {
                collectAuxSlots(conv.operand(), base, out, selectors);
                return;
            }
        }
        std::vector<Ref> reads;
        if (selectors)
            collectLeftSelectorRefs(expr, reads);
        else
            collectCallReads(expr, reads);
        for (auto& r : reads) {
            if (width)
                out.push_back(Slot{r, base, base + width - 1, false});
            else
                out.push_back(Slot{r, 0, kNoWidth, false});
        }
    }
};

// -------------------------------------------------------------- the walker

class Walker {
public:
    Walker(Compilation& comp, AnalysisManager& mgr, Writer& w) :
        compilation(comp), analysis(mgr), writer(w),
        sourceManager(*comp.getSourceManager()) {}

    Stats run() {
        // Pass 1: group every instance by what module it actually is, and
        // pick one body per group to extract from. The group key is
        // (definition, parameter values), not the body pointer: slang shares
        // a canonical body only sometimes, and only the canonical body has
        // an AnalyzedScope -- a non-canonical one would contribute no
        // dataflow at all, silently.
        for (auto inst : compilation.getRoot().topInstances)
            collect(*inst);

        // Module rows: one per source definition, not per parameterisation.
        for (auto& [key, group] : groups)
            internModuleRow(group.body->getDefinition());

        // Terminals first, for every group: a parent's connection templates
        // name its children's terminal indices, and a child's group may be
        // built after the parent's otherwise.
        for (auto& [key, group] : groups)
            buildTerms(templates[key], *group.body);
        // Then the full templates.
        for (auto& [key, group] : groups) {
            auto& t = templates[key];
            t.moduleId = moduleIds[&group.body->getDefinition()];
            t.params = group.params;
            buildTemplate(t, *group.body);
        }

        // Pass 2: stamp the elaborated tree.
        for (auto inst : compilation.getRoot().topInstances) {
            const int64_t nodeId = ++nodeCounter;
            noteChild(0, std::string(inst->name), nodeId);
            stampOccurrence(*inst, instanceGroup[inst], std::string(inst->name),
                            nodeId, /*parentNode=*/0, /*parentInst=*/0,
                            /*ordinal=*/rootOrdinal++, TplLoc{},
                            /*ifaceBind=*/{});
        }

        // Hierarchical references last: an absolute path may land in a
        // subtree stamped after the referring occurrence.
        resolveHierRefs();

        writer.linkSourceFiles(fileOrigins);
        return stats;
    }

private:
    struct Group {
        std::string name;
        std::string params;
        const InstanceBodySymbol* body = nullptr;
    };

    /// The body to extract a group's dataflow from: one the analysis manager
    /// actually analysed, else the first seen.
    void offer(Group& g, const InstanceBodySymbol& body) {
        if (g.body && analysis.getAnalyzedScope(*g.body))
            return;
        if (!g.body || analysis.getAnalyzedScope(body))
            g.body = &body;
    }

    std::string groupKey(const InstanceBodySymbol& body) const {
        return std::string(body.getDefinition().name) + '\n' + parameterText(body);
    }

    void collect(const InstanceSymbol& inst) {
        auto& body = inst.getCanonicalBody() ? *inst.getCanonicalBody() : inst.body;
        auto key = groupKey(body);
        auto& g = groups[key];
        if (g.name.empty()) {
            g.name = std::string(body.getDefinition().name);
            g.params = parameterText(body);
        }
        offer(g, body);
        instanceGroup[&inst] = key;
        forEachInstance(inst.body, [&](const InstanceSymbol& child) { collect(child); });
    }

    // ---------------------------------------------------------- locations

    void noteFile(SourceLocation loc, const std::string& asWritten) {
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

    TplLoc locate(SourceLocation loc, const TplLoc& fallback = {}) {
        if (!loc)
            return fallback;
        Where w = whereOf(loc, sourceManager);
        noteFile(loc, w.file);
        return TplLoc{writer.internFile(w.file), w.line, w.column};
    }

    // ---------------------------------------------------------- terminals

    /// The port list of one group, as terminal templates. A MultiPort (a
    /// non-ANSI `.p({hi, lo})` formal) is one terminal; its inside is the
    /// term_map segments built later.
    void buildTerms(Template& t, const InstanceBodySymbol& body) {
        for (auto* portSym : body.getPortList()) {
            if (!portSym)
                continue;
            TplTerm term;
            term.name = std::string(portSym->name);
            term.loc = locate(portSym->location);
            switch (portSym->kind) {
                case SymbolKind::Port: {
                    auto& p = portSym->as<PortSymbol>();
                    term.kind = "signal";
                    term.direction = directionWord(p.direction);
                    term.dataTypeId = writer.internDataType(p.getType().toString());
                    if (p.getType().isIntegral())
                        term.width = static_cast<int64_t>(p.getType().getBitWidth());
                    term.isConst = 0;
                    if (p.direction == ArgumentDirection::Ref && p.internalSymbol &&
                        ValueSymbol::isKind(p.internalSymbol->kind)) {
                        auto& vs = p.internalSymbol->as<ValueSymbol>();
                        if (vs.kind == SymbolKind::Variable &&
                            vs.as<VariableSymbol>().flags.has(VariableFlags::Const))
                            term.isConst = 1;
                    }
                    break;
                }
                case SymbolKind::MultiPort: {
                    auto& mp = portSym->as<MultiPortSymbol>();
                    term.kind = "signal";
                    term.direction = directionWord(mp.direction);
                    term.dataTypeId = writer.internDataType(mp.getType().toString());
                    if (mp.getType().isIntegral())
                        term.width = static_cast<int64_t>(mp.getType().getBitWidth());
                    term.isConst = 0;
                    break;
                }
                case SymbolKind::InterfacePort: {
                    auto& ip = portSym->as<InterfacePortSymbol>();
                    term.kind = "interface";
                    std::string text = ip.interfaceDef
                                           ? std::string(ip.interfaceDef->name)
                                           : std::string("interface");
                    term.dataTypeId = writer.internDataType(text);
                    if (!ip.modport.empty())
                        term.modport = std::string(ip.modport);
                    break;
                }
                default:
                    continue;
            }
            if (term.name.empty())
                term.name = "<unnamed>";
            t.termIndex.emplace(term.name, int32_t(t.terms.size()));
            t.terms.push_back(std::move(term));
        }
    }

    // ------------------------------------------------------ template build

    /// Per-build state that does not belong in the finished template.
    struct Build {
        Template* t = nullptr;
        const InstanceBodySymbol* body = nullptr;
        std::string prefix;
        std::unordered_map<const Symbol*, int32_t> netOf;
        std::unordered_map<const Symbol*, int32_t> scopeOf;
        std::unordered_map<const SubroutineSymbol*, int32_t> procOf;
        /// (reference expression, is-write, statement) -> hierRefs index.
        /// Keyed by node so a generate loop's four elaborations of one
        /// spelling stay four rows, and mapped rather than a bare set so a
        /// second sighting within one statement pairs its dependency with
        /// the row the first made.
        ///
        /// The statement is part of the key because a body is now walked
        /// once per call site: two calls to `task sample(); q <= u.x;
        /// endtask` are two statements, and with one shared row the second
        /// statement's dependency pointed at a reference belonging to the
        /// first -- so "what does statement 4 read outside this instance"
        /// answered nothing.
        std::map<std::tuple<const Expression*, bool, int32_t>, int32_t> hierSeen;
        int32_t curStmt = -1;      // where call bindings attach
        int32_t curProc = -1;      // procedure of statements being created
        int32_t curScope = 0;
        int64_t targetOrdinal = 0; // per-stmt ordinals
        int64_t operandOrdinal = 0;
        int64_t exprOrdinal = 0;
        std::vector<int32_t> curControlRefs;   // control expr_refs of curStmt
        std::vector<int32_t> curControlHrefs;  // outward conditions, as hierRefs
        std::vector<Ref> curControlSrcs;
        /// Subroutine-body instantiations left for this module, and the
        /// call sites skipped once they ran out. See handle(CallExpression)
        /// for why per-call-site expansion needs a ceiling at all.
        int64_t callBudget = kCallExpansionBudget;
        int64_t truncatedCalls = 0;
    };

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
    int32_t netFor(Build& b, const ValueSymbol& sym) {
        if (auto it = b.netOf.find(&sym); it != b.netOf.end())
            return it->second;
        std::string rel;
        if (!relativePath(sym, b.prefix, rel))
            return -1;
        for (const Scope* s = sym.getParentScope(); s;) {
            auto& owner = s->asSymbol();
            if (owner.kind == SymbolKind::InstanceBody) {
                if (&owner != &b.body->asSymbol())
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
        net.loc = locate(sym.location);
        const int32_t idx = int32_t(b.t->nets.size());
        b.t->netIndex.emplace(net.name, idx);
        b.t->nets.push_back(std::move(net));
        b.netOf.emplace(&sym, idx);
        return idx;
    }

    /// Declared nets and variables of the body and its generate scopes,
    /// eagerly, so a declared-but-unused signal is still a row -- those are
    /// exactly the ones worth asking about. scope indices follow the
    /// generate tree built alongside.
    void collectDeclarations(Build& b, const Scope& scope, int32_t scopeIdx) {
        for (auto& member : scope.members()) {
            switch (member.kind) {
                case SymbolKind::Variable:
                case SymbolKind::Net: {
                    if (member.name.empty())
                        break;
                    auto& vs = member.as<ValueSymbol>();
                    const int32_t idx = netFor(b, vs);
                    if (idx >= 0)
                        b.t->nets[size_t(idx)].scope = scopeIdx;
                    break;
                }
                case SymbolKind::GenerateBlock: {
                    auto& block = member.as<GenerateBlockSymbol>();
                    if (block.isUninstantiated)
                        break;
                    const int32_t idx = addScope(b, block, scopeIdx,
                                                 generateSegment(block));
                    collectDeclarations(b, block, idx);
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
                        const int32_t idx = addScope(b, entry->asSymbol(),
                                                     scopeIdx, segment);
                        collectDeclarations(b, *entry, idx);
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    int32_t addScope(Build& b, const Symbol& sym, int32_t parent,
                     std::string name) {
        const int32_t idx = int32_t(b.t->scopes.size());
        b.t->scopes.push_back(TplScope{parent, std::move(name)});
        b.scopeOf.emplace(&sym, idx);
        return idx;
    }

    int32_t scopeForSymbol(Build& b, const Symbol& sym) {
        // Climb to the nearest scope the template modelled: a generate block
        // or the body itself. Statement blocks and subroutines fold onto it.
        const Scope* s = sym.getParentScope();
        while (s) {
            auto& owner = s->asSymbol();
            if (auto it = b.scopeOf.find(&owner); it != b.scopeOf.end())
                return it->second;
            if (&owner == &b.body->asSymbol())
                return 0;
            s = owner.getParentScope();
        }
        return 0;
    }

    // ----------------------------------------------------- statement rows

    int32_t newStmt(Build& b, std::string kind, std::string construct,
                    std::string assignKind, int64_t seq, std::string delay,
                    int64_t dropped, const TplLoc& loc) {
        TplStmt s;
        s.scope = b.curScope;
        s.proc = b.curProc;
        s.sequence = b.curProc < 0 ? -1 : seq;
        s.kind = std::move(kind);
        s.construct = std::move(construct);
        s.assignKind = std::move(assignKind);
        s.delay = std::move(delay);
        s.dropped = dropped;
        s.loc = loc;
        const int32_t idx = int32_t(b.t->stmts.size());
        b.t->stmts.push_back(std::move(s));
        b.curStmt = idx;
        b.targetOrdinal = 0;
        b.operandOrdinal = 0;
        b.exprOrdinal = 0;
        // All three condition vectors, always together: they are indexed in
        // lockstep by the target loop, so clearing two of them leaves the
        // third holding an earlier statement's entries and every later
        // lookup reads the wrong slot -- a control edge attributed to the
        // wrong signal, or dropped, with nothing in the row to show for it.
        b.curControlRefs.clear();
        b.curControlHrefs.clear();
        b.curControlSrcs.clear();
        return idx;
    }

    int32_t addExprRef(Build& b, int32_t stmt, const Ref& r, std::string role,
                       int32_t netIdx) {
        TplExprRef e;
        e.stmt = stmt;
        e.ordinal = b.exprOrdinal++;
        e.net = netIdx;
        e.role = std::move(role);
        e.r = rangeOf(r);
        const int32_t idx = int32_t(b.t->exprRefs.size());
        b.t->exprRefs.push_back(std::move(e));
        return idx;
    }

    /// Records one reference that leaves the instance -- and, when slang
    /// resolved it, how to find the target again from any occurrence.
    /// `r` is the reference the dependency uses, which pairing may have
    /// narrowed to the bits one target takes; `asWritten`, when given, is
    /// the reference the source actually spells. The row keeps the latter:
    /// `assign {hi, lo} = u.x;` reads the whole of u.x, and storing the
    /// first pairing's half made the database claim the RTL only ever
    /// named four of its bits. The narrowed ranges live in net_dep, where
    /// they describe a particular dependency rather than the reference.
    int32_t addHierRef(Build& b, bool isWrite, const Ref& r,
                       const TplLoc& at, EvalContext& eval,
                       const char* access = nullptr,
                       const Ref* asWritten = nullptr) {
        auto key = std::make_tuple(r.origin, isWrite, b.curStmt);
        if (auto it = b.hierSeen.find(key); it != b.hierSeen.end())
            return it->second;
        std::string text = canonicalPath(r.origin, eval);
        if (text.empty())
            text = normalizedText(r.origin, sourceManager);
        if (text.empty() || (text.find('.') == std::string::npos &&
                             text.find("::") == std::string::npos)) {
            stats.external++;
            b.hierSeen.emplace(key, -1);
            return -1;
        }
        stats.external++;
        TplHierRef row;
        row.stmt = b.curStmt;
        row.path = std::move(text);
        row.access = access ? access : (isWrite ? "write" : "read");
        row.r = rangeOf(asWritten ? *asWritten : r);
        row.loc = at;
        fillResolution(b, row, r);
        const int32_t idx = int32_t(b.t->hierRefs.size());
        if (row.resolve != TplHierRef::None)
            b.t->hasResolvableRefs = true;
        b.t->hierRefs.push_back(std::move(row));
        b.hierSeen.emplace(key, idx);
        return idx;
    }

    /// How to reach the reference's target from an occurrence. Downward
    /// targets replay inside the occurrence's own subtree; absolute ones
    /// replay from the root; a reference through one of this template's own
    /// interface terminals replays from whatever instance the terminal is
    /// bound to in that occurrence. Upward references (upwardCount > 0) stay
    /// unresolved -- the one analysed body speaks for occurrences whose
    /// upward surroundings may differ, and a guess is worse than a NULL.
    void fillResolution(Build& b, TplHierRef& row, const Ref& r) {
        // The reference expression may be wrapped in selects and
        // conversions; the resolved reference lives on the base value node.
        const Expression* e = r.origin;
        while (e) {
            if (e->kind == ExpressionKind::ElementSelect)
                e = &e->as<ElementSelectExpression>().value();
            else if (e->kind == ExpressionKind::RangeSelect)
                e = &e->as<RangeSelectExpression>().value();
            else if (e->kind == ExpressionKind::MemberAccess)
                e = &e->as<MemberAccessExpression>().value();
            else if (e->kind == ExpressionKind::Conversion)
                e = &e->as<ConversionExpression>().operand();
            else
                break;
        }
        if (!e || e->kind != ExpressionKind::HierarchicalValue)
            return;
        auto& hv = e->as<HierarchicalValueExpression>();
        const Symbol* target = hv.ref.target;
        if (!target || !r.sym)
            return;
        if (hv.ref.isUpward())
            return;
        // A modport port stands for the net behind it: the reference
        // resolves to that net, not to the modport's own symbol -- whose
        // path carries the modport level (`bus.src.vld`) that the stamped
        // net names do not.
        if (target->kind == SymbolKind::ModportPort) {
            auto* inner = target->as<ModportPortSymbol>().internalSymbol;
            if (!inner)
                return;   // an explicit modport expression names no one net
            target = inner;
        }
        std::string full = target->getHierarchicalPath();
        // The interface-port case: the reference entered through one of this
        // template's own interface terminals.
        if (hv.ref.isViaIfacePort() && !hv.ref.path.empty()) {
            const Symbol* first = hv.ref.path.front().symbol;
            if (first && first->kind == SymbolKind::InterfacePort) {
                auto it = b.t->termIndex.find(std::string(first->name));
                if (it != b.t->termIndex.end()) {
                    auto& ip = first->as<InterfacePortSymbol>();
                    auto [iface, modport] = ip.getConnection();
                    if (iface) {
                        std::string ifacePrefix = iface->getHierarchicalPath();
                        std::string rel;
                        if (splitBelow(full, ifacePrefix, rel)) {
                            row.resolve = TplHierRef::ViaIfaceTerm;
                            row.ifaceTerm = it->second;
                            splitSegsAndNet(rel, *target, row);
                            return;
                        }
                    }
                }
            }
            return;
        }
        std::string rel;
        if (splitBelow(full, b.prefix, rel)) {
            row.resolve = TplHierRef::Downward;
            splitSegsAndNet(rel, *target, row);
            return;
        }
        row.resolve = TplHierRef::Absolute;
        splitSegsAndNet(full, *target, row);
    }

    static bool splitBelow(const std::string& full, const std::string& prefix,
                           std::string& rel) {
        if (prefix.empty() || full.size() <= prefix.size())
            return false;
        if (full.compare(0, prefix.size(), prefix) != 0 ||
            full[prefix.size()] != '.')
            return false;
        rel = full.substr(prefix.size() + 1);
        return true;
    }

    /// Splits `rel` into the tree segments that lead to the target's
    /// instance and the scope-relative net name inside it. The instance
    /// chain is recovered from the target symbol's own ancestry: every
    /// enclosing InstanceSymbol contributes its path segments.
    void splitSegsAndNet(const std::string& rel, const Symbol& target,
                         TplHierRef& row) {
        // The nearest enclosing instance of the target decides where the
        // tree walk ends and the net name begins.
        const Scope* s = target.getParentScope();
        const InstanceBodySymbol* owner = nullptr;
        while (s) {
            auto& sym = s->asSymbol();
            if (sym.kind == SymbolKind::InstanceBody) {
                owner = &sym.as<InstanceBodySymbol>();
                break;
            }
            s = sym.getParentScope();
        }
        if (!owner) {
            row.resolve = TplHierRef::None;
            return;
        }
        std::string ownerPath = owner->getHierarchicalPath();
        std::string netRel;
        std::string treePath;
        std::string targetFull = target.getHierarchicalPath();
        if (!splitBelow(targetFull, ownerPath, netRel)) {
            row.resolve = TplHierRef::None;
            return;
        }
        // The tree part is what remains of `rel` once the net part (and its
        // dot) is dropped from the end.
        if (netRel.size() + 1 <= rel.size() &&
            rel.compare(rel.size() - netRel.size(), netRel.size(), netRel) == 0 &&
            (rel.size() == netRel.size() ||
             rel[rel.size() - netRel.size() - 1] == '.')) {
            treePath = rel.size() == netRel.size()
                           ? std::string()
                           : rel.substr(0, rel.size() - netRel.size() - 1);
        }
        else if (rel == netRel) {
            treePath.clear();
        }
        else {
            row.resolve = TplHierRef::None;
            return;
        }
        row.netName = netRel;
        row.segs.clear();
        // Split the tree path into per-node segments. Generate levels are
        // one node per segment exactly as instance levels are; an array
        // index `[k]` belongs to the segment before it in the stamped names.
        size_t start = 0;
        while (start < treePath.size()) {
            size_t dot = treePath.find('.', start);
            if (dot == std::string::npos)
                dot = treePath.size();
            row.segs.push_back(treePath.substr(start, dot - start));
            start = dot + 1;
        }
    }

    // ----------------------------------------------------- template build

    void buildTemplate(Template& t, const InstanceBodySymbol& body) {
        Build b;
        b.t = &t;
        b.body = &body;
        b.prefix = body.getHierarchicalPath();
        t.scopes.push_back(TplScope{-1, std::string()});
        b.scopeOf.emplace(&body.asSymbol(), 0);

        collectDeclarations(b, body, 0);
        buildTermMaps(b, body);

        if (auto* scope = analysis.getAnalyzedScope(body)) {
            for (auto& proc : scope->procedures)
                buildProcedure(b, proc);
        }
        buildNetInitialisers(b, body);
        buildPrimitives(b, body);
        buildChildren(b, body);
        stats.truncatedCalls += b.truncatedCalls;
        t.built = true;
    }

    /// The inside of each terminal: which nets it stands for. An ANSI port
    /// maps whole-to-whole onto its internal symbol; a non-ANSI port
    /// expression and a MultiPort produce one segment per element with its
    /// window, through the same machinery the outside uses.
    void buildTermMaps(Build& b, const InstanceBodySymbol& body) {
        EvalContext evalCtx(body);
        for (auto* portSym : body.getPortList()) {
            if (!portSym)
                continue;
            auto termIt = b.t->termIndex.find(std::string(
                portSym->name.empty() ? std::string_view("<unnamed>") : portSym->name));
            if (termIt == b.t->termIndex.end())
                continue;
            const int32_t termIdx = termIt->second;
            int64_t ordinal = 0;
            auto addSeg = [&](int32_t netIdx, const TplRange& termR,
                             const TplRange& netR, bool mapping) {
                if (netIdx < 0)
                    return;
                TplTermMap m;
                m.term = termIdx;
                m.ordinal = ordinal++;
                m.net = netIdx;
                m.termR = termR;
                m.netR = netR;
                m.mappingExact = mapping;
                b.t->termMaps.push_back(std::move(m));
            };
            if (portSym->kind == SymbolKind::Port) {
                auto& p = portSym->as<PortSymbol>();
                if (p.internalSymbol && ValueSymbol::isKind(p.internalSymbol->kind)) {
                    const int32_t netIdx =
                        netFor(b, p.internalSymbol->as<ValueSymbol>());
                    addSeg(netIdx, TplRange{}, TplRange{}, true);
                }
                else if (auto* inner = p.getInternalExpr()) {
                    std::vector<ConnRef> segs;
                    collectConnRefs(*inner, evalCtx, segs);
                    for (auto& cn : segs) {
                        if (!cn.ref.sym)
                            continue;
                        const int32_t netIdx = netFor(b, *cn.ref.sym);
                        if (netIdx < 0)
                            continue;
                        TplRange termR;
                        const uint64_t fw =
                            inner->type ? inner->type->getBitWidth() : 0;
                        if (cn.windowExact && fw &&
                            !(cn.winLo == 0 && cn.winHi + 1 >= fw))
                            termR.bits = std::make_pair(cn.winLo, cn.winHi);
                        termR.exact = cn.windowExact;
                        addSeg(netIdx, termR, rangeOf(cn.ref), cn.positional);
                    }
                }
            }
            else if (portSym->kind == SymbolKind::MultiPort) {
                // Members are declared MSB first, exactly as a concatenation
                // is written; each maps whole onto its own window.
                auto& mp = portSym->as<MultiPortSymbol>();
                uint64_t total = mp.getType().isIntegral()
                                     ? mp.getType().getBitWidth()
                                     : 0;
                uint64_t cursor = total;
                for (auto* member : mp.ports) {
                    if (!member || !member->internalSymbol ||
                        !ValueSymbol::isKind(member->internalSymbol->kind))
                        continue;
                    auto& vs = member->internalSymbol->as<ValueSymbol>();
                    const uint64_t w = member->getType().isIntegral()
                                           ? member->getType().getBitWidth()
                                           : 0;
                    TplRange termR;
                    bool mapping = false;
                    if (total && w && w <= cursor) {
                        cursor -= w;
                        if (!(cursor == 0 && w == total))
                            termR.bits = std::make_pair(cursor, cursor + w - 1);
                        termR.exact = true;
                        mapping = true;
                    }
                    else {
                        termR.exact = false;
                    }
                    addSeg(netFor(b, vs), termR, TplRange{}, mapping);
                }
            }
            // InterfacePort: no nets behind it, no map rows.
        }
    }

    // One procedure: its row, its sensitivity, and its statements.
    void buildProcedure(Build& b, const AnalyzedProcedure& proc) {
        const Symbol& sym = *proc.analyzedSymbol;
        const bool isContinuous = sym.kind == SymbolKind::ContinuousAssign;
        const TplLoc procAt = locate(sym.location);
        EvalContext evalCtx(sym);

        std::string construct;
        if (isContinuous)
            construct = "assign";
        else
            construct = procedureWord(sym);

        int32_t procIdx = -1;
        if (!isContinuous) {
            TplProcedure p;
            p.scope = scopeForSymbol(b, sym);
            p.kind = construct == "assign" ? "always" : construct;
            p.loc = procAt;
            procIdx = int32_t(b.t->procedures.size());
            b.t->procedures.push_back(std::move(p));
        }
        b.curProc = procIdx;
        b.curScope = scopeForSymbol(b, sym);
        b.curStmt = -1;

        auto& sens = proc.getSensitivityList();
        // Sensitivity rows carry the procedure's location. An event whose
        // expression is not a plain net keeps net NULL and its reads land as
        // expr_ref role='event' on a synthetic event_control statement --
        // one read, one table.
        int32_t sensStmt = -1;
        auto sensReadStmt = [&]() {
            if (sensStmt < 0) {
                sensStmt = newStmt(b, "event_control", "sensitivity",
                                   std::string(), -1, std::string(), 0, procAt);
            }
            return sensStmt;
        };
        if (procIdx >= 0) {
            std::vector<std::pair<const Expression*, std::string>> raw;
            std::vector<const Expression*> iffs;
            collectEdgeEvents(sens.timingControl, raw, &iffs);
            for (auto& [expr, edge] : raw)
                addProcEvent(b, procIdx, -1, expr, edge, "sensitivity", procAt,
                             evalCtx, sensReadStmt);
            for (auto* cond : iffs) {
                std::vector<Ref> reads;
                collectRefs(*cond, evalCtx, reads);
                const int32_t s = sensReadStmt();
                for (auto& r : reads)
                    recordRead(b, s, r, "event", procAt, evalCtx);
            }
        }

        std::unordered_set<const ValueSymbol*> inputPorts;
        for (auto* d : proc.getDrivers()) {
            if (d->isInputPort())
                inputPorts.insert(&d->getSymbol());
        }

        bool reached = false;

        StatementWalker walker(
            // ---- one assignment target with its pairs and gating
            [&](const Ref& dst, const std::vector<PairedSrc>& pairs,
                const std::vector<Ref>& gating, SourceRange where, int64_t seq,
                bool blocking, int64_t dropped, bool inSubroutine,
                bool firstTarget, const std::string& delay) {
                reached = true;
                if (!dst.sym || inputPorts.count(dst.sym))
                    return;
                const TplLoc at = locate(where.start(), procAt);
                emitAssignment(b, dst, pairs, gating, at, seq, blocking,
                               dropped, inSubroutine, firstTarget, delay,
                               isContinuous, construct, evalCtx);
            },
            // ---- a call site's actual bound to its formal
            [&](const Ref& formal, const Ref& actual, bool reads, bool writes,
                bool oneToOne, bool bindable, SourceRange where) {
                reached = true;
                emitCallBinding(b, formal, actual, reads, writes, oneToOne,
                                bindable, locate(where.start(), procAt), evalCtx);
            },
            // ---- a statement-level event control (a wait)
            [&](const Expression* e, const std::string& edge, int64_t seq,
                SourceRange where) {
                if (procIdx < 0)
                    return;
                const TplLoc at = locate(where.start(), procAt);
                const int32_t s = newStmt(b, "event_control", "wait",
                                          std::string(), seq, std::string(), 0,
                                          at);
                addProcEvent(b, procIdx, s, e, edge, "wait", at, evalCtx,
                             [&]() { return s; });
            },
            // ---- a statement that reads without writing anything nameable
            [&](const std::vector<Ref>& reads, const std::vector<Ref>& gating,
                const std::vector<Ref>& writes, const std::string& stmtKind,
                const std::string& construct2, int64_t seq, SourceRange where) {
                const TplLoc at = locate(where.start(), procAt);
                filteredConstants = 0;
                const int32_t s = newStmt(b, stmtKind, construct2,
                                          std::string(), seq, std::string(), 0,
                                          at);
                const std::string role = stmtKind == "assertion" ? "assertion"
                                         : stmtKind == "wait"    ? "wait"
                                         : stmtKind == "system_task"
                                             ? "system_task"
                                             : "call_argument";
                for (auto& r : reads)
                    recordRead(b, s, r, role, at, evalCtx);
                // The conditions that gate it. No dependency can exist --
                // the statement writes nothing this instance names -- but
                // the condition IS read, and dropping it lost the signal
                // from every load query.
                for (auto& g : gating)
                    recordRead(b, s, g, "control", at, evalCtx);
                // What the task writes. The source is genuinely unknowable
                // -- a file, a plusarg, a format string -- so the row has
                // no source, and v_driver tells it apart from a constant
                // tie-off by the statement it came from.
                for (auto& w : writes) {
                    // A system task's write is a write: the procedure that
                    // contains one has reached a driver, and counting it as
                    // an empty procedure blamed the walk for a construct it
                    // now models.
                    reached = true;
                    recordSystemWrite(b, s, w, at, evalCtx);
                }
            },
            evalCtx);
        walker.sensitivityTiming = sens.timingControl;
        walker.budget = &b.callBudget;
        walker.truncated = &b.truncatedCalls;
        if (isContinuous) {
            walker.pendingDelay = delayText(
                sym.as<ContinuousAssignSymbol>().getDelay());
        }

        if (sym.kind == SymbolKind::ProceduralBlock)
            sym.as<ProceduralBlockSymbol>().getBody().visit(walker);
        else if (isContinuous)
            sym.as<ContinuousAssignSymbol>().getAssignment().visit(walker);

        if (!reached && !proc.getDrivers().empty())
            stats.emptyProcedures++;
        b.curProc = -1;
        b.curStmt = -1;
    }

    /// The target of a system task's write: a real assign_target plus a
    /// source-less dependency, so the argument has a driver and the
    /// procedure is not mistaken for one that wrote nothing. A target
    /// outside this instance is a hier_ref with access='write', as
    /// everywhere else.
    void recordSystemWrite(Build& b, int32_t stmt, const Ref& r,
                           const TplLoc& at, EvalContext& evalCtx) {
        if (!r.sym)
            return;
        const int32_t netIdx = netFor(b, *r.sym);
        if (netIdx < 0) {
            const int32_t saved = b.curStmt;
            b.curStmt = stmt;
            addHierRef(b, true, r, at, evalCtx);
            b.curStmt = saved;
            return;
        }
        TplStmtRef tr;
        tr.stmt = stmt;
        tr.ordinal = int64_t(b.t->targets.size());
        tr.net = netIdx;
        tr.r = rangeOf(r);
        const int32_t targetIdx = int32_t(b.t->targets.size());
        b.t->targets.push_back(std::move(tr));
        TplDep d;
        d.srcNet = -1;
        d.tgtNet = netIdx;
        d.stmt = stmt;
        d.targetRef = targetIdx;
        d.kind = "data";
        d.tgtR = rangeOf(r);
        b.t->deps.push_back(std::move(d));
    }

    /// One read of a statement, wherever it lands: an expr_ref for a net of
    /// this instance, a hier_ref for anything outside it.
    void recordRead(Build& b, int32_t stmt, const Ref& r, const std::string& role,
                    const TplLoc& at, EvalContext& evalCtx) {
        if (!r.sym)
            return;
        const int32_t netIdx = netFor(b, *r.sym);
        if (netIdx < 0) {
            const int32_t saved = b.curStmt;
            b.curStmt = stmt;
            addHierRef(b, false, r, at, evalCtx);
            b.curStmt = saved;
            return;
        }
        addExprRef(b, stmt, r, role, netIdx);
    }

    void addProcEvent(Build& b, int32_t procIdx, int32_t stmtIdx,
                      const Expression* expr, const std::string& edge,
                      const std::string& eventKind, const TplLoc& at,
                      EvalContext& evalCtx,
                      const std::function<int32_t()>& readStmt) {
        int32_t netIdx = -1;
        if (expr && (expr->kind == ExpressionKind::NamedValue ||
                     expr->kind == ExpressionKind::HierarchicalValue)) {
            auto& vs = expr->as<ValueExpressionBase>().symbol;
            netIdx = netFor(b, vs);
            if (netIdx < 0) {
                Ref r;
                r.sym = &vs;
                r.origin = expr;
                const int32_t saved = b.curStmt;
                b.curStmt = stmtIdx;
                addHierRef(b, false, r, at, evalCtx);
                b.curStmt = saved;
            }
        }
        else if (expr) {
            // Not a plain reference (`@(posedge clks[2])`): net stays NULL
            // and the reads are expr_ref rows on the owning statement.
            std::vector<Ref> reads;
            collectRefs(*expr, evalCtx, reads);
            const int32_t s = readStmt();
            const std::string role = eventKind == "wait" ? "wait" : "event";
            for (auto& r : reads)
                recordRead(b, s, r, role, at, evalCtx);
        }
        TplProcEvent e;
        e.proc = procIdx;
        e.stmt = stmtIdx;
        e.net = netIdx;
        e.eventKind = eventKind;
        e.edgeKind = edge;
        e.loc = at;
        b.t->procEvents.push_back(std::move(e));
    }

    /// One target of one assignment statement, with its statement row on the
    /// first target, its operand rows, and the dependencies that pair them.
    void emitAssignment(Build& b, const Ref& dst,
                        const std::vector<PairedSrc>& pairs,
                        const std::vector<Ref>& gating, const TplLoc& at,
                        int64_t seq, bool blocking, int64_t dropped,
                        bool inSubroutine, bool firstTarget,
                        const std::string& delay, bool isContinuous,
                        const std::string& construct, EvalContext& evalCtx) {
        int32_t stmt = b.curStmt;
        if (firstTarget) {
            const bool continuous = isContinuous && !inSubroutine;
            stmt = newStmt(b, "assignment", construct,
                           continuous ? "continuous"
                                      : (blocking ? "blocking" : "nonblocking"),
                           seq, delay, dropped, at);
            // The control reads gate every target of the statement; recorded
            // once, reused by each target's control dependencies. An outward
            // condition is a hier_ref; its dependency onto each target is
            // paired here and materialised when the reference resolves.
            for (auto& g : gating) {
                if (!g.sym)
                    continue;
                const int32_t netIdx = netFor(b, *g.sym);
                if (netIdx < 0) {
                    b.curControlRefs.push_back(-1);
                    b.curControlHrefs.push_back(
                        addHierRef(b, false, g, at, evalCtx));
                }
                else {
                    b.curControlRefs.push_back(
                        addExprRef(b, stmt, g, "control", netIdx));
                    b.curControlHrefs.push_back(-1);
                }
                b.curControlSrcs.push_back(g);
            }
        }
        if (stmt < 0)
            return;

        // The target row, or the outward write.
        int32_t targetIdx = -1;
        int32_t tgtHref = -1;
        int32_t dstNet = netFor(b, *dst.sym);
        if (dstNet < 0) {
            tgtHref = addHierRef(b, true, dst, at, evalCtx);
        }
        else {
            TplStmtRef tr;
            tr.stmt = stmt;
            tr.ordinal = b.targetOrdinal++;
            tr.net = dstNet;
            tr.r = rangeOf(dst);
            targetIdx = int32_t(b.t->targets.size());
            b.t->targets.push_back(std::move(tr));
        }
        const bool haveTarget = targetIdx >= 0 || tgtHref >= 0;

        // Operands and data dependencies, paired -- never crossed. An end
        // outside the instance keeps the pairing: the dependency is queued
        // against the hier_ref and becomes a real cross-instance row once
        // the reference resolves. Unresolvable stays a hier_ref alone --
        // the honest record, never a fabricated edge.
        bool anySource = false;
        for (auto& p : pairs) {
            if (!p.src.sym)
                continue;
            const int32_t srcNet = netFor(b, *p.src.sym);
            int32_t operandIdx = -1;
            int32_t srcHref = -1;
            if (srcNet >= 0) {
                TplStmtRef orow;
                orow.stmt = stmt;
                orow.ordinal = b.operandOrdinal++;
                orow.net = srcNet;
                orow.r = rangeOf(p.src);
                operandIdx = int32_t(b.t->operands.size());
                b.t->operands.push_back(std::move(orow));
            }
            else {
                srcHref = addHierRef(b, false, p.src, at, evalCtx,
                                     nullptr, &p.srcAsWritten);
            }
            if (!haveTarget)
                continue;
            anySource = anySource || srcNet >= 0 || srcHref >= 0;
            if (srcNet >= 0 && targetIdx >= 0) {
                TplDep d;
                d.srcNet = srcNet;
                d.tgtNet = dstNet;
                d.stmt = stmt;
                d.operandRef = operandIdx;
                d.targetRef = targetIdx;
                d.kind = "data";
                d.srcR = rangeOf(p.src);
                // The bits of the target THIS operand reaches, not the
                // whole target: the `assign_target` row above still spans
                // everything the statement writes.
                d.tgtR = rangeOf(p.tgt);
                d.mappingExact = p.mapExact ? 1 : 0;
                b.t->deps.push_back(std::move(d));
            }
            else if (srcNet >= 0 || srcHref >= 0) {
                TplCrossDep d;
                d.kind = "data";
                d.stmt = stmt;
                d.srcNet = srcNet;
                d.srcHref = srcHref;
                d.tgtNet = dstNet;
                d.tgtHref = tgtHref;
                d.operandRef = operandIdx;
                d.targetRef = targetIdx;
                d.srcR = rangeOf(p.src);
                d.tgtR = rangeOf(p.tgt);
                d.mappingExact = p.mapExact ? 1 : 0;
                b.t->crossDeps.push_back(std::move(d));
            }
        }
        // `q <= 8'h0`: nothing at all reaches the target, and the
        // null-source row records the driving statement. A target whose
        // sources are all OUTWARD is not that -- its drivers are the
        // cross-instance rows above, and claiming a constant here was a
        // wrong fact, not a conservative one.
        if (targetIdx >= 0 && !anySource) {
            TplDep d;
            d.srcNet = -1;
            d.tgtNet = dstNet;
            d.stmt = stmt;
            d.targetRef = targetIdx;
            d.kind = "data";
            d.tgtR = rangeOf(dst);
            b.t->deps.push_back(std::move(d));
        }
        // Control dependencies: each recorded condition read reaches this
        // target through its branch, whichever side of the boundary either
        // end lives on.
        if (haveTarget) {
            for (size_t i = 0; i < b.curControlSrcs.size(); i++) {
                auto& src = b.curControlSrcs[i];
                const int32_t ctrlRef = b.curControlRefs[i];
                const int32_t ctrlHref = b.curControlHrefs[i];
                if (ctrlRef < 0 && ctrlHref < 0)
                    continue;
                if (ctrlRef >= 0 && targetIdx >= 0) {
                    const int32_t srcNet = netFor(b, *src.sym);
                    if (srcNet < 0)
                        continue;
                    TplDep d;
                    d.srcNet = srcNet;
                    d.tgtNet = dstNet;
                    d.stmt = stmt;
                    d.exprRef = ctrlRef;
                    d.targetRef = targetIdx;
                    d.kind = "control";
                    d.srcR = rangeOf(src);
                    d.tgtR = rangeOf(dst);
                    d.mappingExact = 0;
                    b.t->deps.push_back(std::move(d));
                }
                else {
                    TplCrossDep d;
                    d.kind = "control";
                    d.stmt = stmt;
                    d.srcNet = ctrlRef >= 0 ? netFor(b, *src.sym) : -1;
                    d.srcHref = ctrlHref;
                    d.tgtNet = dstNet;
                    d.tgtHref = tgtHref;
                    d.exprRef = ctrlRef;
                    d.targetRef = targetIdx;
                    d.srcR = rangeOf(src);
                    d.tgtR = rangeOf(dst);
                    d.mappingExact = 0;
                    b.t->crossDeps.push_back(std::move(d));
                }
            }
        }
    }

    /// One call binding: the actual and the formal coupled by argument
    /// direction. The formal is a subroutine-scope net (`bump.v`); the
    /// body's own statements belong to the calling procedure, and are
    /// walked once per call site so each carries its caller's gating.
    void emitCallBinding(Build& b, const Ref& formal, const Ref& actual,
                         bool reads, bool writes, bool oneToOne, bool bindable,
                         const TplLoc& at, EvalContext& evalCtx) {
        if (!formal.sym || !actual.sym)
            return;
        const int32_t formalNet = netFor(b, *formal.sym);
        if (formalNet < 0)
            return;
        const int32_t stmt = bindable ? b.curStmt : -1;
        const int32_t actualNet = netFor(b, *actual.sym);
        if (actualNet < 0) {
            // An outward actual still binds: the dependency pairs here and
            // materialises when the reference resolves.
            const int32_t saved = b.curStmt;
            b.curStmt = stmt;
            const int32_t href = addHierRef(b, writes, actual, at, evalCtx);
            b.curStmt = saved;
            if (href < 0)
                return;
            if (reads) {
                TplCrossDep d;
                d.kind = "procedure";
                d.stmt = stmt;
                d.srcHref = href;
                d.tgtNet = formalNet;
                d.srcR = rangeOf(actual);
                d.mappingExact = oneToOne ? 1 : 0;
                b.t->crossDeps.push_back(std::move(d));
            }
            if (writes) {
                TplCrossDep d;
                d.kind = "procedure";
                d.stmt = stmt;
                d.srcNet = formalNet;
                d.tgtHref = href;
                d.tgtR = rangeOf(actual);
                d.mappingExact = oneToOne ? 1 : 0;
                b.t->crossDeps.push_back(std::move(d));
            }
            return;
        }
        if (reads) {
            int32_t exprIdx = -1;
            if (stmt >= 0)
                exprIdx = addExprRef(b, stmt, actual, "call_argument", actualNet);
            TplDep d;
            d.srcNet = actualNet;
            d.tgtNet = formalNet;
            d.stmt = stmt;
            d.exprRef = exprIdx;
            d.kind = "procedure";
            d.srcR = rangeOf(actual);
            d.mappingExact = oneToOne ? 1 : 0;
            b.t->deps.push_back(std::move(d));
        }
        if (writes) {
            TplDep d;
            d.srcNet = formalNet;
            d.tgtNet = actualNet;
            d.stmt = stmt;
            d.kind = "procedure";
            d.tgtR = rangeOf(actual);
            d.mappingExact = oneToOne ? 1 : 0;
            b.t->deps.push_back(std::move(d));
        }
    }

    /// `wire w = a & b;` -- the LRM's continuous assignment spelled as a
    /// declaration, through the same slot machinery as `assign`.
    void buildNetInitialisers(Build& b, const InstanceBodySymbol& body) {
        EvalContext evalCtx(body);
        b.curProc = -1;
        forEachOfKind<SymbolKind::Net, NetSymbol>(body, [&](const NetSymbol& net) {
            const Expression* init = net.getInitializer();
            if (!init)
                return;
            const TplLoc at = locate(net.location);
            std::vector<Slot> rhs;
            filteredConstants = 0;
            collectSlots(*init, evalCtx, 0, rhs);
            {
                std::vector<Ref> callReads;
                std::set<const SubroutineSymbol*> active;
                collectCallReadsInto(*init, active, callReads);
                for (auto& r : callReads)
                    rhs.push_back(Slot{r, 0, kNoWidth, false});
            }
            const int64_t droppedConstants = filteredConstants;
            evalCtx.reset();

            const uint64_t netWidth = bitWidthOf(net);
            Slot dstSlot;
            dstSlot.ref.sym = &net;
            if (netWidth) {
                dstSlot.hi = netWidth - 1;
                dstSlot.positional = true;
            }
            else {
                dstSlot.hi = kNoWidth;
            }
            std::vector<PairedSrc> pairs;
            for (auto& srcSlot : rhs) {
                if (!srcSlot.ref.sym)
                    continue;
                uint64_t lo = 0, hi = 0;
                if (!slotsOverlap(dstSlot, srcSlot, lo, hi))
                    continue;
                pairs.push_back(PairedSrc{narrowed(srcSlot, lo, hi),
                                          narrowed(dstSlot, lo, hi),
                                          dstSlot.positional && srcSlot.positional,
                                          srcSlot.ref});
            }
            b.curScope = 0;
            emitAssignment(b, dstSlot.ref, pairs, {}, at, /*seq=*/-1,
                           /*blocking=*/false, droppedConstants,
                           /*inSubroutine=*/false, /*firstTarget=*/true,
                           std::string(), /*isContinuous=*/true, "assign",
                           evalCtx);
            b.curStmt = -1;
        });
    }

    /// Gate, switch and UDP instances: a tree node, a primitive row, and one
    /// dependency per LRM (input, output) pairing.
    void buildPrimitives(Build& b, const InstanceBodySymbol& body) {
        EvalContext evalCtx(body);
        forEachOfKind<SymbolKind::PrimitiveInstance, PrimitiveInstanceSymbol>(
            body, [&](const PrimitiveInstanceSymbol& prim) {
            auto conns = prim.getPortConnections();
            auto& def = prim.primitiveType;
            TplPrim p;
            p.scope = scopeForSymbol(b, prim);
            std::string name = leafSegment(prim);
            p.name = name.empty() ? "<unnamed>" : name;
            p.primKind = def.primitiveKind == PrimitiveSymbol::UserDefined
                             ? "udp"
                             : def.primitiveKind == PrimitiveSymbol::BiDiSwitch
                                   ? "switch"
                                   : "gate";
            p.defName = std::string(def.name);
            p.loc = locate(prim.location);
            const int32_t primIdx = int32_t(b.t->prims.size());
            b.t->prims.push_back(std::move(p));
            if (conns.empty())
                return;

            // Terminal directions: the built-in gates are variadic, so the
            // LRM fixes their shape; switches and UDPs declare per-port.
            const size_t n = conns.size();
            auto dirOf = [&](size_t i) {
                switch (def.primitiveKind) {
                    case PrimitiveSymbol::NInput:
                        return i == 0 ? PrimitivePortDirection::Out
                                      : PrimitivePortDirection::In;
                    case PrimitiveSymbol::NOutput:
                        return i + 1 == n ? PrimitivePortDirection::In
                                          : PrimitivePortDirection::Out;
                    default:
                        return i < def.ports.size() ? def.ports[i]->direction
                                                    : PrimitivePortDirection::In;
                }
            };

            struct PrimTerm {
                Ref ref;
                size_t terminal;
            };
            b.curStmt = -1;
            std::vector<PrimTerm> reads, writes;
            auto take = [&](size_t i, std::vector<PrimTerm>& into,
                            bool skipSelectors) {
                std::vector<Ref> refs;
                collectRefs(*conns[i], evalCtx, refs, skipSelectors);
                for (auto& r : refs)
                    into.push_back(PrimTerm{r, i});
            };
            for (size_t i = 0; i < n; i++) {
                if (!conns[i])
                    continue;
                switch (dirOf(i)) {
                    case PrimitivePortDirection::In:
                        take(i, reads, /*skipSelectors=*/false);
                        break;
                    case PrimitivePortDirection::InOut:
                        take(i, reads, /*skipSelectors=*/true);
                        take(i, writes, /*skipSelectors=*/true);
                        break;
                    default:
                        take(i, writes, /*skipSelectors=*/true);
                        break;
                }
            }

            for (auto& dstTerm : writes) {
                const Ref& dst = dstTerm.ref;
                const int32_t dstNet = dst.sym ? netFor(b, *dst.sym) : -1;
                if (dstNet < 0) {
                    if (dst.sym)
                        addHierRef(b, true, dst, b.t->prims.back().loc, evalCtx);
                    for (auto& srcTerm : reads) {
                        if (srcTerm.terminal != dstTerm.terminal && srcTerm.ref.sym)
                            addHierRefIfOutward(b, srcTerm.ref, evalCtx);
                    }
                    continue;
                }
                bool anyInput = false;
                for (auto& srcTerm : reads) {
                    // One terminal does not feed itself: a tran's two ends
                    // are both read and driven, and pairing an end with
                    // itself would fabricate dataflow out of a single wire.
                    if (srcTerm.terminal == dstTerm.terminal)
                        continue;
                    const Ref& src = srcTerm.ref;
                    const int32_t srcNet = src.sym ? netFor(b, *src.sym) : -1;
                    if (srcNet < 0) {
                        if (src.sym)
                            addHierRefIfOutward(b, src, evalCtx);
                        continue;
                    }
                    TplDep d;
                    d.srcNet = srcNet;
                    d.tgtNet = dstNet;
                    d.prim = primIdx;
                    d.kind = "primitive";
                    d.srcR = rangeOf(src);
                    d.tgtR = rangeOf(dst);
                    const bool oneBit =
                        src.exact && dst.exact &&
                        (src.whole ? bitWidthOf(*src.sym) == 1
                                   : src.hi == src.lo) &&
                        (dst.whole ? bitWidthOf(*dst.sym) == 1
                                   : dst.hi == dst.lo);
                    d.mappingExact = oneBit ? 1 : 0;
                    b.t->deps.push_back(std::move(d));
                    anyInput = true;
                }
                // pullup(y) has no input terminal; the null-source row names
                // the gate as the driver, as `q <= 8'h0` is named.
                if (!anyInput) {
                    TplDep d;
                    d.srcNet = -1;
                    d.tgtNet = dstNet;
                    d.prim = primIdx;
                    d.kind = "primitive";
                    d.tgtR = rangeOf(dst);
                    b.t->deps.push_back(std::move(d));
                }
            }
        });
    }

    void addHierRefIfOutward(Build& b, const Ref& r, EvalContext& evalCtx) {
        std::string rel;
        if (r.sym && !relativePath(*r.sym, b.prefix, rel))
            addHierRef(b, false, r, TplLoc{}, evalCtx);
    }

    // ------------------------------------------------ connection templates

    /// One net a connection expression attaches, with its window in the
    /// formal -- the boundary twin of Slot. Ported from v9.
    struct ConnRef {
        Ref ref;
        bool expression = false;
        uint64_t winLo = 0, winHi = 0;
        bool windowExact = false;
        bool positional = false;
    };

    static void collectConnRefs(const Expression& expr, EvalContext& ctx,
                                std::vector<ConnRef>& out, uint64_t base = 0,
                                bool degraded = false) {
        const uint64_t width = exprWidthOf(expr);
        auto pushConstant = [&]() {
            ConnRef cr;
            if (!degraded && width) {
                cr.winLo = base;
                cr.winHi = base + width - 1;
                cr.windowExact = true;
            }
            out.push_back(std::move(cr));
        };
        auto push = [&](std::vector<Ref>& refs, bool isExpr) {
            if (refs.empty()) {
                pushConstant();
                return;
            }
            const bool windowed = !degraded && width != 0;
            for (auto& r : refs) {
                ConnRef cr;
                cr.ref = r;
                cr.expression = isExpr;
                if (windowed) {
                    cr.winLo = base;
                    cr.winHi = base + width - 1;
                    cr.windowExact = true;
                    cr.positional =
                        !isExpr && refs.size() == 1 && r.exact &&
                        (r.whole ? bitWidthOf(*r.sym) == width
                                 : r.hi - r.lo + 1 == width);
                }
                out.push_back(std::move(cr));
            }
        };
        switch (expr.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue:
            case ExpressionKind::ElementSelect:
            case ExpressionKind::RangeSelect:
            case ExpressionKind::MemberAccess: {
                std::vector<Ref> refs;
                collectRefs(expr, ctx, refs, /*skipSelectors=*/true);
                push(refs, /*isExpr=*/false);
                return;
            }
            case ExpressionKind::Concatenation: {
                auto ops = expr.as<ConcatenationExpression>().operands();
                uint64_t cursor = base + width;
                bool bad = degraded || width == 0;
                for (auto* op : ops) {
                    if (!op)
                        continue;
                    const uint64_t w = exprWidthOf(*op);
                    if (!bad && (w == 0 || w > cursor - base))
                        bad = true;
                    if (!bad)
                        cursor -= w;
                    collectConnRefs(*op, ctx, out, bad ? 0 : cursor, bad);
                }
                return;
            }
            case ExpressionKind::Replication: {
                // Each copy is its own positionally exact segment.
                auto& rep = expr.as<ReplicationExpression>();
                const uint64_t cw = exprWidthOf(rep.concat());
                if (degraded || width == 0 || cw == 0 || width % cw != 0) {
                    collectConnRefs(rep.concat(), ctx, out, 0, true);
                    return;
                }
                uint64_t cursor = base + width;
                for (uint64_t k = 0; k < width / cw; k++) {
                    cursor -= cw;
                    collectConnRefs(rep.concat(), ctx, out, cursor, degraded);
                }
                return;
            }
            case ExpressionKind::Conversion: {
                auto& conv = expr.as<ConversionExpression>();
                const uint64_t iw = exprWidthOf(conv.operand());
                collectConnRefs(conv.operand(), ctx, out,
                                iw == width ? base : 0,
                                degraded || iw != width);
                return;
            }
            case ExpressionKind::Assignment:
                // An output connection arrives wrapped in the assignment
                // bindLValue builds around it; the lvalue is the connection.
                collectConnRefs(expr.as<AssignmentExpression>().left(), ctx, out,
                                base, degraded);
                return;
            default: {
                std::vector<Ref> refs;
                collectRefs(expr, ctx, refs);
                std::set<const ValueSymbol*> have;
                for (auto& r : refs)
                    have.insert(r.sym);
                std::vector<const ValueSymbol*> all;
                collectReads(expr, all);
                for (auto* s : all) {
                    if (have.insert(s).second) {
                        Ref r;
                        r.sym = s;
                        refs.push_back(r);
                    }
                }
                push(refs, /*isExpr=*/true);
                return;
            }
        }
    }

    /// The children of one body and their connection templates. Children are
    /// recorded in traversal order -- the stamping invariant -- with their
    /// scope index and ONE path segment each (array elements carry their
    /// `[i]` in the segment, exactly as the tree spells them).
    void buildChildren(Build& b, const InstanceBodySymbol& body) {
        std::unordered_map<const Symbol*, int32_t> childOf;
        std::vector<const Symbol*> childSyms;
        registerChildren(b, body, 0, childOf, childSyms);
        // Connections second, template-wide and in child order (the order is
        // part of the template): an interface binding may name a sibling
        // registered after the binder.
        for (size_t i = 0; i < b.t->children.size(); i++) {
            if (b.t->children[i].kind == TplChild::Module && childSyms[i])
                buildInstanceConns(b, childSyms[i]->as<InstanceSymbol>(),
                                   b.t->children[i], childOf);
        }
    }

    void registerChildren(Build& b, const Scope& scope, int32_t scopeIdx,
                          std::unordered_map<const Symbol*, int32_t>& childOf,
                          std::vector<const Symbol*>& childSyms) {
        for (auto& member : scope.members()) {
            switch (member.kind) {
                case SymbolKind::Instance: {
                    auto& inst = member.as<InstanceSymbol>();
                    TplChild c;
                    c.scope = scopeIdx;
                    c.name = leafSegment(inst);
                    if (c.name.empty())
                        c.name = "<unnamed>";
                    c.kind = TplChild::Module;
                    auto& cbody = inst.getCanonicalBody() ? *inst.getCanonicalBody()
                                                          : inst.body;
                    c.groupKey = groupKey(cbody);
                    c.loc = locate(inst.location);
                    childOf.emplace(&inst, int32_t(b.t->children.size()));
                    childSyms.push_back(&inst);
                    b.t->children.push_back(std::move(c));
                    break;
                }
                case SymbolKind::UninstantiatedDef: {
                    auto& u = member.as<UninstantiatedDefSymbol>();
                    TplChild c;
                    c.scope = scopeIdx;
                    c.name = leafSegment(u);
                    if (c.name.empty())
                        c.name = "<unnamed>";
                    c.kind = TplChild::Unresolved;
                    c.defName = std::string(u.definitionName);
                    c.loc = locate(u.location);
                    stats.unresolved++;
                    buildUnresolvedConns(b, u, c);
                    childSyms.push_back(nullptr);
                    b.t->children.push_back(std::move(c));
                    break;
                }
                case SymbolKind::GenerateBlock: {
                    auto& block = member.as<GenerateBlockSymbol>();
                    if (block.isUninstantiated)
                        break;
                    registerChildren(b, block, scopeIndexOf(b, block, scopeIdx),
                                     childOf, childSyms);
                    break;
                }
                case SymbolKind::GenerateBlockArray:
                    for (auto& entry :
                         member.as<GenerateBlockArraySymbol>().entries) {
                        registerChildren(b, *entry,
                                         scopeIndexOf(b, entry->asSymbol(), scopeIdx),
                                         childOf, childSyms);
                    }
                    break;
                case SymbolKind::InstanceArray:
                    registerChildren(b, member.as<InstanceArraySymbol>(), scopeIdx,
                                     childOf, childSyms);
                    break;
                default:
                    break;
            }
        }
    }

    int32_t scopeIndexOf(Build& b, const Symbol& sym, int32_t parent) {
        if (auto it = b.scopeOf.find(&sym); it != b.scopeOf.end())
            return it->second;
        // A generate scope not seen by collectDeclarations (e.g. an array
        // entry wrapper): create it now so the tree stays faithful.
        std::string seg;
        if (sym.kind == SymbolKind::GenerateBlock)
            seg = generateSegment(sym.as<GenerateBlockSymbol>());
        else
            seg = std::string(sym.name);
        return addScope(b, sym, parent, std::move(seg));
    }

    /// One resolved child's connection templates: the outside of each of its
    /// terminals, as written here in the parent.
    void buildInstanceConns(Build& b, const InstanceSymbol& child, TplChild& c,
                            const std::unordered_map<const Symbol*, int32_t>& childOf) {
        auto& childBody = child.getCanonicalBody() ? *child.getCanonicalBody()
                                                   : child.body;
        Template& childT = templates[groupKey(childBody)];
        EvalContext evalCtx(child);
        const TplLoc instAt = locate(child.location);
        bool inArray = false;
        if (auto* ps = child.getParentScope())
            inArray = ps->asSymbol().kind == SymbolKind::InstanceArray;

        std::unordered_map<int32_t, int64_t> segOrdinal;
        for (auto* conn : child.getPortConnections()) {
            if (!conn)
                continue;
            const Expression* connExpr = conn->getExpression();
            const TplLoc at = connExpr
                                  ? locate(connExpr->sourceRange.start(), instAt)
                                  : instAt;
            auto termIt = childT.termIndex.find(std::string(conn->port.name));
            if (termIt == childT.termIndex.end())
                continue;
            const int32_t termIdx = termIt->second;
            auto nextOrdinal = [&]() { return segOrdinal[termIdx]++; };

            if (conn->port.kind == SymbolKind::InterfacePort) {
                auto [ifaceSym, modport] = conn->getIfaceConn();
                TplConn tc;
                tc.kind = "interface";
                tc.childTerm = termIdx;
                tc.ordinal = nextOrdinal();
                tc.loc = at;
                if (ifaceSym) {
                    if (auto it = childOf.find(ifaceSym); it != childOf.end()) {
                        tc.ifaceChild = it->second;
                    }
                    else if (auto* through =
                                 passedThrough(*b.body, ifaceSym)) {
                        auto ownIt = b.t->termIndex.find(
                            std::string(through->name));
                        if (ownIt != b.t->termIndex.end())
                            tc.ifaceOwnTerm = ownIt->second;
                    }
                    else {
                        // An interface array element or another synthesized
                        // shape: no per-occurrence object to point at.
                        stats.external++;
                    }
                }
                c.conns.push_back(std::move(tc));
                continue;
            }
            if (conn->port.kind != SymbolKind::Port &&
                conn->port.kind != SymbolKind::MultiPort)
                continue;

            if (!connExpr) {
                TplConn tc;
                tc.kind = "unconnected";
                tc.childTerm = termIdx;
                tc.ordinal = nextOrdinal();
                tc.loc = at;
                c.conns.push_back(std::move(tc));
                continue;
            }

            std::vector<ConnRef> nets;
            collectConnRefs(*connExpr, evalCtx, nets);
            if (nets.empty()) {
                TplConn tc;
                tc.kind = "constant";
                tc.childTerm = termIdx;
                tc.ordinal = nextOrdinal();
                tc.termExact = 1;   // the whole formal, trivially
                tc.loc = at;
                c.conns.push_back(std::move(tc));
                continue;
            }
            const uint64_t formalWidth =
                (!inArray && connExpr->type) ? connExpr->type->getBitWidth() : 0;
            // The connection expression's type is not always the formal's: an
            // output port narrower than the net it drives arrives as a plain
            // assignment with no conversion node to degrade through, so the
            // walk positions elements against the wrong width and claims a
            // one-to-one mapping across a truncation. The declared terminal
            // width is the authority; when the two disagree, every element's
            // position is unstatable and no mapping is per-bit -- the same
            // degradation a width-changing conversion already gets.
            const int64_t declaredWidth = childT.terms[size_t(termIdx)].width;
            const bool widthMismatch =
                formalWidth && declaredWidth > 0 &&
                uint64_t(declaredWidth) != formalWidth;
            for (auto& cn : nets) {
                TplConn tc;
                tc.childTerm = termIdx;
                tc.ordinal = nextOrdinal();
                tc.loc = at;
                // The formal window, encoded as ranges are everywhere.
                if (!inArray && !widthMismatch && cn.windowExact && formalWidth) {
                    if (!(cn.winLo == 0 && cn.winHi + 1 >= formalWidth))
                        tc.termR.bits = std::make_pair(cn.winLo, cn.winHi);
                    tc.termExact = 1;
                }
                else {
                    tc.termExact = 0;
                }
                if (!cn.ref.sym) {
                    tc.kind = "constant";
                    c.conns.push_back(std::move(tc));
                    continue;
                }
                const int32_t netIdx = netFor(b, *cn.ref.sym);
                if (netIdx < 0) {
                    // Tied to something with no name here. The row exists
                    // either way; what it is tied to is in hier_ref.
                    Ref r = cn.ref;
                    const bool drives = conn->port.kind == SymbolKind::Port &&
                                        (conn->port.as<PortSymbol>().direction ==
                                             ArgumentDirection::Out ||
                                         conn->port.as<PortSymbol>().direction ==
                                             ArgumentDirection::InOut);
                    const int32_t saved = b.curStmt;
                    b.curStmt = -1;
                    const int32_t href =
                        addHierRef(b, drives, r, at, evalCtx, "connect");
                    b.curStmt = saved;
                    tc.kind = cn.expression ? "expression_operand"
                                            : "external_reference";
                    tc.hierRef = href;
                    c.conns.push_back(std::move(tc));
                    continue;
                }
                tc.parentNet = netIdx;
                tc.kind = cn.expression ? "expression_operand" : "signal";
                if (inArray) {
                    // An element shares the whole array's connection
                    // expression: somewhere in the object, honestly.
                    tc.netExact = 0;
                }
                else {
                    tc.netR = rangeOf(cn.ref);
                    tc.netExact = cn.ref.exact ? 1 : 0;
                }
                tc.mappingExact = tc.kind == "expression_operand"
                                      ? 0
                                      : (cn.positional && !inArray &&
                                                 !widthMismatch
                                             ? 1
                                             : 0);
                c.conns.push_back(std::move(tc));
            }
        }
    }

    /// The parent's own interface port that `iface` arrived through, if any.
    static const InterfacePortSymbol* passedThrough(const InstanceBodySymbol& body,
                                                    const Symbol* iface) {
        if (!iface)
            return nullptr;
        for (auto& member : body.members()) {
            if (member.kind != SymbolKind::InterfacePort)
                continue;
            auto& ip = member.as<InterfacePortSymbol>();
            if (ip.getConnection().first == iface)
                return &ip;
        }
        return nullptr;
    }

    /// Terminals and connections of a black box: one terminal per named
    /// connection, direction unknown, so `net_conn` has something to bind
    /// and "connected to a black box" stays distinct from "unconnected".
    void buildUnresolvedConns(Build& b, const UninstantiatedDefSymbol& u,
                              TplChild& c) {
        EvalContext evalCtx(*b.body);
        auto names = u.getPortNames();
        auto conns = u.getPortConnections();
        for (size_t i = 0; i < conns.size(); i++) {
            std::string portName = i < names.size() ? std::string(names[i])
                                                    : std::string();
            if (portName.empty())
                portName = "<port" + std::to_string(i) + ">";
            const int32_t termSlot = int32_t(c.unresolvedPorts.size());
            c.unresolvedPorts.push_back(portName);
            const AssertionExpr* raw = conns[i];
            const Expression* expr = nullptr;
            if (raw && raw->kind == AssertionExprKind::Simple)
                expr = &raw->as<SimpleAssertionExpr>().expr;
            // The port's type is unknowable, so slang wraps the actual in an
            // InvalidExpression; the expression as written is its child. An
            // empty `.extra()` stays empty.
            while (expr && expr->kind == ExpressionKind::Invalid)
                expr = expr->as<InvalidExpression>().child;
            if (expr && expr->kind == ExpressionKind::EmptyArgument)
                expr = nullptr;
            const TplLoc at = locate(u.location);
            if (!expr) {
                TplConn tc;
                tc.kind = "unconnected";
                tc.childTerm = termSlot;
                tc.loc = at;
                c.conns.push_back(std::move(tc));
                continue;
            }
            std::vector<ConnRef> nets;
            collectConnRefs(*expr, evalCtx, nets);
            if (nets.empty()) {
                TplConn tc;
                tc.kind = "constant";
                tc.childTerm = termSlot;
                tc.loc = at;
                c.conns.push_back(std::move(tc));
                continue;
            }
            int64_t ordinal = 0;
            for (auto& cn : nets) {
                TplConn tc;
                tc.childTerm = termSlot;
                tc.ordinal = ordinal++;
                tc.loc = at;
                // No formal knowledge: the terminal side stays NULL.
                if (!cn.ref.sym) {
                    tc.kind = "constant";
                    c.conns.push_back(std::move(tc));
                    continue;
                }
                const int32_t netIdx = netFor(b, *cn.ref.sym);
                if (netIdx < 0) {
                    const int32_t saved = b.curStmt;
                    b.curStmt = -1;
                    const int32_t href = addHierRef(b, false, cn.ref, at,
                                                    evalCtx, "connect");
                    b.curStmt = saved;
                    tc.kind = cn.expression ? "expression_operand"
                                            : "external_reference";
                    tc.hierRef = href;
                    c.conns.push_back(std::move(tc));
                    continue;
                }
                tc.parentNet = netIdx;
                tc.kind = cn.expression ? "expression_operand" : "signal";
                tc.netR = rangeOf(cn.ref);
                tc.netExact = cn.ref.exact ? 1 : 0;
                tc.mappingExact = tc.kind == "expression_operand" ? 0 : -1;
                c.conns.push_back(std::move(tc));
            }
        }
    }

    // ------------------------------------------------------------ modules

    void internModuleRow(const DefinitionSymbol& def) {
        if (moduleIds.count(&def))
            return;
        ModuleRow row;
        row.id = int64_t(moduleIds.size()) + 1;
        row.name = std::string(def.name);
        switch (def.definitionKind) {
            case DefinitionKind::Interface: row.definitionKind = "interface"; break;
            case DefinitionKind::Program:   row.definitionKind = "program";   break;
            default:                        row.definitionKind = "module";    break;
        }
        const TplLoc at = locate(def.location);
        row.fileId = at.fileId;
        row.line = at.line;
        row.column = at.column;
        moduleIds.emplace(&def, row.id);
        writer.addModule(row);
        stats.modules++;
    }

    // ----------------------------------------------------------- stamping

    /// Everything one stamped occurrence needs to remember.
    struct Bases {
        int64_t net = 0, term = 0, proc = 0, stmt = 0, target = 0, operand = 0,
                exprRef = 0, procEvent = 0, dep = 0, hierRef = 0;
    };

    struct ReplayJob {
        int64_t rowId = 0;
        int64_t instNode = 0;         // the occurrence the reference is in
        const Template* t = nullptr;
        size_t refIdx = 0;
        Bases base;
        std::vector<int64_t> ifaceBind; // term index -> bound iface inst id
    };

    /// One queued cross-instance dependency of one occurrence, written once
    /// the occurrence's references resolve.
    struct CrossJob {
        const Template* t = nullptr;
        size_t idx = 0;
        Bases base;
    };

    void stampOccurrence(const InstanceSymbol& instSym, const std::string& key,
                         const std::string& name, int64_t nodeId,
                         int64_t parentNode, int64_t parentInst,
                         int64_t ordinal, const TplLoc& instLoc,
                         std::vector<int64_t> ifaceBind) {
        (void)instSym;
        Template& t = templates[key];

        TreeNodeRow node;
        node.id = nodeId;
        node.parentNodeId = parentNode;
        node.name = name;
        node.nodeKind = parentNode == 0 ? "root" : "instance";
        node.ordinal = ordinal;
        writer.addTreeNode(node);

        InstRow inst;
        inst.id = nodeId;
        inst.moduleId = t.moduleId;
        inst.parentInstId = parentInst;
        inst.parameterSignature = t.params;
        inst.fileId = instLoc.fileId;
        inst.line = instLoc.line;
        inst.column = instLoc.column;
        writer.addInst(inst);
        stats.instances++;

        stampBody(t, nodeId, std::move(ifaceBind));
    }

    /// Stamps one occurrence's template rows and recurses into its children.
    void stampBody(Template& t, int64_t instId, std::vector<int64_t> ifaceBind) {
        // Scope nodes: index 0 is the instance itself; the rest are
        // generate levels, parents guaranteed to precede children.
        std::vector<int64_t> scopeNode(t.scopes.size(), instId);
        std::vector<int64_t> siblingOrdinal(t.scopes.size(), 0);
        for (size_t i = 1; i < t.scopes.size(); i++) {
            const int64_t id = ++nodeCounter;
            const int32_t parent = t.scopes[i].parent;
            const int64_t parentId = scopeNode[size_t(parent < 0 ? 0 : parent)];
            TreeNodeRow node;
            node.id = id;
            node.parentNodeId = parentId;
            node.name = t.scopes[i].name;
            node.nodeKind = "generate";
            node.ordinal = siblingOrdinal[size_t(parent < 0 ? 0 : parent)]++;
            writer.addTreeNode(node);
            noteChild(parentId, node.name, id);
            scopeNode[i] = id;
        }

        Bases base;
        base.net = netCounter;         netCounter += int64_t(t.nets.size());
        base.term = termCounter;       termCounter += int64_t(t.terms.size());
        base.proc = procCounter;       procCounter += int64_t(t.procedures.size());
        base.stmt = stmtCounter;       stmtCounter += int64_t(t.stmts.size());
        base.target = targetCounter;   targetCounter += int64_t(t.targets.size());
        base.operand = operandCounter; operandCounter += int64_t(t.operands.size());
        base.exprRef = exprRefCounter; exprRefCounter += int64_t(t.exprRefs.size());
        base.procEvent = procEventCounter;
        procEventCounter += int64_t(t.procEvents.size());
        base.dep = depCounter;         depCounter += int64_t(t.deps.size());
        base.hierRef = hierRefCounter; hierRefCounter += int64_t(t.hierRefs.size());

        // Primitives are tree nodes; their ids come from the node counter.
        std::vector<int64_t> primNode(t.prims.size(), 0);
        for (size_t i = 0; i < t.prims.size(); i++) {
            const int64_t id = ++nodeCounter;
            const int64_t parentId = scopeNode[size_t(t.prims[i].scope)];
            TreeNodeRow node;
            node.id = id;
            node.parentNodeId = parentId;
            node.name = t.prims[i].name;
            node.nodeKind = "primitive";
            node.ordinal = siblingOrdinal[size_t(t.prims[i].scope)]++;
            writer.addTreeNode(node);
            noteChild(parentId, node.name, id);
            PrimitiveRow p;
            p.id = id;
            p.instId = instId;
            p.primitiveKind = t.prims[i].primKind;
            p.definitionName = t.prims[i].defName;
            p.fileId = t.prims[i].loc.fileId;
            p.line = t.prims[i].loc.line;
            p.column = t.prims[i].loc.column;
            writer.addPrimitive(p);
            primNode[i] = id;
        }

        for (size_t i = 0; i < t.nets.size(); i++) {
            auto& n = t.nets[i];
            NetRow row;
            row.id = base.net + int64_t(i) + 1;
            row.instId = instId;
            row.scopeNodeId = scopeNode[size_t(n.scope)];
            row.name = n.name;
            row.declarationKind = n.declKind;
            row.dataTypeId = n.dataTypeId;
            row.width = n.width;
            row.isImplicit = n.isImplicit;
            row.fileId = n.loc.fileId;
            row.line = n.loc.line;
            row.column = n.loc.column;
            writer.addNet(row);
        }
        stats.nets += int64_t(t.nets.size());

        for (size_t i = 0; i < t.terms.size(); i++) {
            auto& tm = t.terms[i];
            TermRow row;
            row.id = base.term + int64_t(i) + 1;
            row.instId = instId;
            row.name = tm.name;
            row.terminalKind = tm.kind;
            row.direction = tm.direction;
            row.dataTypeId = tm.dataTypeId;
            row.width = tm.width;
            row.ordinal = int64_t(i);
            row.isConst = tm.isConst;
            row.modport = tm.modport;
            row.fileId = tm.loc.fileId;
            row.line = tm.loc.line;
            row.column = tm.loc.column;
            writer.addTerm(row);
        }
        stats.terms += int64_t(t.terms.size());

        for (auto& m : t.termMaps) {
            TermMapRow row;
            row.termId = base.term + m.term + 1;
            row.ordinal = m.ordinal;
            row.netId = base.net + m.net + 1;
            row.termBits = m.termR.bits;
            row.termExact = m.termR.exact;
            row.netBits = m.netR.bits;
            row.netExact = m.netR.exact;
            row.mappingExact = m.mappingExact;
            writer.addTermMap(row);
        }

        for (size_t i = 0; i < t.procedures.size(); i++) {
            auto& p = t.procedures[i];
            ProcedureRow row;
            row.id = base.proc + int64_t(i) + 1;
            row.instId = instId;
            row.scopeNodeId = scopeNode[size_t(p.scope)];
            row.name = p.name;
            row.procedureKind = p.kind;
            row.ordinal = int64_t(i);
            row.fileId = p.loc.fileId;
            row.line = p.loc.line;
            row.column = p.loc.column;
            writer.addProcedure(row);
        }
        stats.procedures += int64_t(t.procedures.size());

        for (size_t i = 0; i < t.stmts.size(); i++) {
            auto& s = t.stmts[i];
            StmtRow row;
            row.id = base.stmt + int64_t(i) + 1;
            row.instId = instId;
            row.scopeNodeId = scopeNode[size_t(s.scope)];
            row.procedureId = s.proc < 0 ? 0 : base.proc + s.proc + 1;
            row.ordinal = int64_t(i);
            row.sequence = s.sequence;
            row.statementKind = s.kind;
            row.construct = s.construct;
            row.assignmentKind = s.assignKind;
            row.delay = s.delay;
            row.droppedOperandCount = s.dropped;
            row.fileId = s.loc.fileId;
            row.line = s.loc.line;
            row.column = s.loc.column;
            writer.addStmt(row);
        }
        stats.stmts += int64_t(t.stmts.size());

        for (size_t i = 0; i < t.targets.size(); i++) {
            auto& r = t.targets[i];
            AssignTargetRow row;
            row.id = base.target + int64_t(i) + 1;
            row.stmtId = base.stmt + r.stmt + 1;
            row.ordinal = r.ordinal;
            row.netId = base.net + r.net + 1;
            row.bits = r.r.bits;
            row.exact = r.r.exact;
            writer.addAssignTarget(row);
        }
        for (size_t i = 0; i < t.operands.size(); i++) {
            auto& r = t.operands[i];
            AssignOperandRow row;
            row.id = base.operand + int64_t(i) + 1;
            row.stmtId = base.stmt + r.stmt + 1;
            row.ordinal = r.ordinal;
            row.netId = base.net + r.net + 1;
            row.bits = r.r.bits;
            row.exact = r.r.exact;
            writer.addAssignOperand(row);
        }
        for (size_t i = 0; i < t.exprRefs.size(); i++) {
            auto& r = t.exprRefs[i];
            ExprRefRow row;
            row.id = base.exprRef + int64_t(i) + 1;
            row.stmtId = base.stmt + r.stmt + 1;
            row.ordinal = r.ordinal;
            row.netId = base.net + r.net + 1;
            row.role = r.role;
            row.bits = r.r.bits;
            row.exact = r.r.exact;
            writer.addExprRef(row);
        }
        for (size_t i = 0; i < t.procEvents.size(); i++) {
            auto& e = t.procEvents[i];
            ProcEventRow row;
            row.id = base.procEvent + int64_t(i) + 1;
            row.procedureId = base.proc + e.proc + 1;
            row.stmtId = e.stmt < 0 ? 0 : base.stmt + e.stmt + 1;
            row.netId = e.net < 0 ? 0 : base.net + e.net + 1;
            row.eventKind = e.eventKind;
            row.edgeKind = e.edgeKind;
            row.fileId = e.loc.fileId;
            row.line = e.loc.line;
            row.column = e.loc.column;
            writer.addProcEvent(row);
        }
        for (size_t i = 0; i < t.deps.size(); i++) {
            auto& d = t.deps[i];
            NetDepRow row;
            row.id = base.dep + int64_t(i) + 1;
            row.sourceNetId = d.srcNet < 0 ? 0 : base.net + d.srcNet + 1;
            row.targetNetId = base.net + d.tgtNet + 1;
            row.stmtId = d.stmt < 0 ? 0 : base.stmt + d.stmt + 1;
            row.assignOperandId = d.operandRef < 0 ? 0 : base.operand + d.operandRef + 1;
            row.assignTargetId = d.targetRef < 0 ? 0 : base.target + d.targetRef + 1;
            row.exprRefId = d.exprRef < 0 ? 0 : base.exprRef + d.exprRef + 1;
            row.primitiveId = d.prim < 0 ? 0 : primNode[size_t(d.prim)];
            row.dependencyKind = d.kind;
            row.sourceBits = d.srcR.bits;
            row.sourceExact = d.srcNet < 0 ? -1 : (d.srcR.exact ? 1 : 0);
            row.targetBits = d.tgtR.bits;
            row.targetExact = d.tgtR.exact;
            row.mappingExact = d.mappingExact;
            writer.addNetDep(row);
        }
        stats.deps += int64_t(t.deps.size());

        // Hierarchical reference rows are written in the final pass, once
        // every subtree they may land in exists; ids are fixed now because
        // net_conn rows below may reference them. The queued cross-instance
        // dependencies follow in the same pass, since their endpoints are
        // those references' resolutions.
        for (size_t i = 0; i < t.hierRefs.size(); i++) {
            ReplayJob job;
            job.rowId = base.hierRef + int64_t(i) + 1;
            job.instNode = instId;
            job.t = &t;
            job.refIdx = i;
            job.base = base;
            job.ifaceBind = ifaceBind;
            replayJobs.push_back(std::move(job));
        }
        for (size_t i = 0; i < t.crossDeps.size(); i++)
            crossJobs.push_back(CrossJob{&t, i, base});
        stats.hierRefs += int64_t(t.hierRefs.size());

        // For hierarchical-reference replay: which template (and net base)
        // this node stamped, so a resolved path can name a net by id.
        nodeTemplate.emplace(instId, std::make_pair(&t, base.net));

        // Children: allocate every child's node id first, so an interface
        // binding to a later sibling has an id to point at.
        std::vector<int64_t> childNode(t.children.size(), 0);
        for (size_t i = 0; i < t.children.size(); i++)
            childNode[i] = ++nodeCounter;

        // Each child's incoming interface bindings -- and, per connection,
        // the bound instance id its row carries. Computed here in the parent
        // where the connections are written.
        std::vector<std::vector<int64_t>> childIfaceBind(t.children.size());
        std::vector<std::vector<int64_t>> connIfaceId(t.children.size());
        for (size_t i = 0; i < t.children.size(); i++) {
            auto& c = t.children[i];
            if (c.kind != TplChild::Module)
                continue;
            auto& ct = templates[c.groupKey];
            childIfaceBind[i].assign(ct.terms.size(), 0);
            connIfaceId[i].assign(c.conns.size(), 0);
            for (size_t k = 0; k < c.conns.size(); k++) {
                auto& conn = c.conns[k];
                if (conn.kind != "interface" || conn.childTerm < 0)
                    continue;
                int64_t bound = 0;
                if (conn.ifaceChild >= 0)
                    bound = childNode[size_t(conn.ifaceChild)];
                else if (conn.ifaceOwnTerm >= 0 &&
                         size_t(conn.ifaceOwnTerm) < ifaceBind.size())
                    bound = ifaceBind[size_t(conn.ifaceOwnTerm)];
                connIfaceId[i][k] = bound;
                if (size_t(conn.childTerm) < childIfaceBind[i].size())
                    childIfaceBind[i][size_t(conn.childTerm)] = bound;
            }
        }

        // Stamp the children, then their connections (the rows need the
        // child terminal ids, which exist once the child is stamped).
        for (size_t i = 0; i < t.children.size(); i++) {
            auto& c = t.children[i];
            const int64_t parentId = scopeNode[size_t(c.scope)];
            const int64_t ord = siblingOrdinal[size_t(c.scope)]++;
            noteChild(parentId, c.name, childNode[i]);
            if (c.kind == TplChild::Module) {
                const int64_t termBase = termCounter;  // the child's terms start here
                stampChildModule(c, childNode[i], parentId, instId, ord,
                                 std::move(childIfaceBind[i]));
                stampConns(c, /*childTermBase=*/termBase, base, connIfaceId[i]);
            }
            else {
                stampUnresolved(c, childNode[i], parentId, instId, ord, base);
            }
        }
    }

    void stampChildModule(const TplChild& c, int64_t nodeId, int64_t parentNode,
                          int64_t parentInst, int64_t ordinal,
                          std::vector<int64_t> ifaceBind) {
        Template& ct = templates[c.groupKey];
        TreeNodeRow node;
        node.id = nodeId;
        node.parentNodeId = parentNode;
        node.name = c.name;
        node.nodeKind = "instance";
        node.ordinal = ordinal;
        writer.addTreeNode(node);

        InstRow inst;
        inst.id = nodeId;
        inst.moduleId = ct.moduleId;
        inst.parentInstId = parentInst;
        inst.parameterSignature = ct.params;
        inst.fileId = c.loc.fileId;
        inst.line = c.loc.line;
        inst.column = c.loc.column;
        writer.addInst(inst);
        stats.instances++;

        stampBody(ct, nodeId, std::move(ifaceBind));
    }

    /// The connection rows of one child, in the parent's id space. The
    /// child's terminal ids are its term base plus the template index --
    /// terminals are stamped first in stampBody, so the base recorded before
    /// recursion is exact.
    void stampConns(const TplChild& c, int64_t childTermBase,
                    const Bases& parentBase, const std::vector<int64_t>& ifaceIds) {
        for (size_t k = 0; k < c.conns.size(); k++) {
            auto& conn = c.conns[k];
            NetConnRow row;
            row.id = ++connCounter;
            row.netId = conn.parentNet < 0 ? 0 : parentBase.net + conn.parentNet + 1;
            row.termId = childTermBase + conn.childTerm + 1;
            row.ordinal = conn.ordinal;
            row.connectionKind = conn.kind;
            row.netBits = conn.netR.bits;
            row.netExact = conn.netExact;
            row.termBits = conn.termR.bits;
            row.termExact = conn.termExact;
            row.mappingExact = conn.mappingExact;
            if (conn.kind == "interface" && k < ifaceIds.size())
                row.interfaceInstId = ifaceIds[k];
            row.hierRefId = conn.hierRef < 0 ? 0
                                             : parentBase.hierRef + conn.hierRef + 1;
            row.fileId = conn.loc.fileId;
            row.line = conn.loc.line;
            row.column = conn.loc.column;
            writer.addNetConn(row);
            stats.conns++;
        }
    }

    void stampUnresolved(const TplChild& c, int64_t nodeId,
                         int64_t parentNode, int64_t parentInst, int64_t ordinal,
                         const Bases& parentBase) {
        TreeNodeRow node;
        node.id = nodeId;
        node.parentNodeId = parentNode;
        node.name = c.name;
        node.nodeKind = "unresolved";
        node.ordinal = ordinal;
        writer.addTreeNode(node);

        InstRow inst;
        inst.id = nodeId;
        inst.parentInstId = parentInst;
        inst.unresolvedDefinition = c.defName;
        inst.fileId = c.loc.fileId;
        inst.line = c.loc.line;
        inst.column = c.loc.column;
        writer.addInst(inst);
        stats.instances++;

        // One terminal per named connection, direction unknown.
        std::vector<int64_t> termIds(c.unresolvedPorts.size(), 0);
        for (size_t i = 0; i < c.unresolvedPorts.size(); i++) {
            TermRow row;
            row.id = ++termCounter;
            row.instId = nodeId;
            row.name = c.unresolvedPorts[i];
            row.terminalKind = "signal";
            row.ordinal = int64_t(i);
            row.fileId = c.loc.fileId;
            row.line = c.loc.line;
            row.column = c.loc.column;
            writer.addTerm(row);
            termIds[i] = row.id;
        }
        stats.terms += int64_t(c.unresolvedPorts.size());

        for (auto& conn : c.conns) {
            NetConnRow row;
            row.id = ++connCounter;
            row.netId = conn.parentNet < 0 ? 0 : parentBase.net + conn.parentNet + 1;
            row.termId = conn.childTerm >= 0 &&
                                 size_t(conn.childTerm) < termIds.size()
                             ? termIds[size_t(conn.childTerm)]
                             : 0;
            if (row.termId == 0)
                continue;
            row.ordinal = conn.ordinal;
            row.connectionKind = conn.kind;
            row.netBits = conn.netR.bits;
            row.netExact = conn.netExact;
            row.termBits = conn.termR.bits;
            row.termExact = conn.termExact;
            row.mappingExact = conn.mappingExact;
            row.hierRefId = conn.hierRef < 0 ? 0
                                             : parentBase.hierRef + conn.hierRef + 1;
            row.fileId = conn.loc.fileId;
            row.line = conn.loc.line;
            row.column = conn.loc.column;
            writer.addNetConn(row);
            stats.conns++;
        }
    }

    // ------------------------------------------------- hier_ref resolution

    /// Registers a stamped node under its parent for path descent, and
    /// counts the collision when the name is already taken -- a design that
    /// did not fully elaborate can produce two siblings of one name, and a
    /// path lookup in it is then ambiguous. The second node keeps its rows;
    /// only the by-name map keeps the first.
    void noteChild(int64_t parent, const std::string& name, int64_t id) {
        if (!childByName[parent].emplace(name, id).second)
            stats.duplicatePaths++;
    }

    /// Writes every hier_ref row, resolving the ones whose replay lands on a
    /// stamped object -- then materialises the queued cross-instance
    /// dependencies whose endpoints those resolutions are. The tree walk is
    /// by name against childByName; the net by name against the target
    /// group's template index -- both spellings the stamping itself
    /// produced.
    void resolveHierRefs() {
        std::unordered_map<int64_t, int64_t> resolvedNet;  // hier_ref id -> net id
        for (auto& job : replayJobs) {
            const TplHierRef& ref = job.t->hierRefs[job.refIdx];
            HierRefRow row;
            row.id = job.rowId;
            row.instId = job.instNode;
            row.stmtId = ref.stmt < 0 ? 0 : job.base.stmt + ref.stmt + 1;
            row.path = ref.path;
            row.access = ref.access;
            row.bits = ref.r.bits;
            row.exact = ref.r.exact;
            row.fileId = ref.loc.fileId;
            row.line = ref.loc.line;
            row.column = ref.loc.column;

            int64_t node = 0;
            switch (ref.resolve) {
                case TplHierRef::Downward:
                    node = descend(job.instNode, ref.segs);
                    break;
                case TplHierRef::Absolute:
                    node = descend(0, ref.segs);
                    break;
                case TplHierRef::ViaIfaceTerm:
                    if (ref.ifaceTerm >= 0 &&
                        size_t(ref.ifaceTerm) < job.ifaceBind.size() &&
                        job.ifaceBind[size_t(ref.ifaceTerm)] != 0)
                        node = descend(job.ifaceBind[size_t(ref.ifaceTerm)],
                                       ref.segs);
                    break;
                default:
                    break;
            }
            if (node != 0) {
                row.resolvedInstId = node;
                if (!ref.netName.empty()) {
                    auto tplIt = nodeTemplate.find(node);
                    if (tplIt != nodeTemplate.end()) {
                        auto& tt = *tplIt->second.first;
                        auto nIt = tt.netIndex.find(ref.netName);
                        if (nIt != tt.netIndex.end()) {
                            row.resolvedNetId =
                                tplIt->second.second + nIt->second + 1;
                            resolvedNet.emplace(row.id, row.resolvedNetId);
                        }
                    }
                }
            }
            writer.addHierRef(row);
        }

        // The cross-instance dependencies. An endpoint is a local net (base
        // plus index) or a reference's resolution; a dependency any of whose
        // referenced ends did not resolve is not written -- the hier_ref
        // rows above are the honest record, and a fabricated edge would be
        // a wrong one.
        for (auto& job : crossJobs) {
            const TplCrossDep& d = job.t->crossDeps[job.idx];
            NetDepRow row;
            row.id = 0;   // assigned below once the row is known writable
            if (d.srcNet >= 0) {
                row.sourceNetId = job.base.net + d.srcNet + 1;
            }
            else if (d.srcHref >= 0) {
                row.sourceHierRefId = job.base.hierRef + d.srcHref + 1;
                auto it = resolvedNet.find(row.sourceHierRefId);
                if (it == resolvedNet.end())
                    continue;
                row.sourceNetId = it->second;
            }
            else {
                continue;
            }
            if (d.tgtNet >= 0) {
                row.targetNetId = job.base.net + d.tgtNet + 1;
            }
            else if (d.tgtHref >= 0) {
                row.targetHierRefId = job.base.hierRef + d.tgtHref + 1;
                auto it = resolvedNet.find(row.targetHierRefId);
                if (it == resolvedNet.end())
                    continue;
                row.targetNetId = it->second;
            }
            else {
                continue;
            }
            row.id = ++depCounter;
            row.stmtId = d.stmt < 0 ? 0 : job.base.stmt + d.stmt + 1;
            row.assignOperandId =
                d.operandRef < 0 ? 0 : job.base.operand + d.operandRef + 1;
            row.assignTargetId =
                d.targetRef < 0 ? 0 : job.base.target + d.targetRef + 1;
            row.exprRefId = d.exprRef < 0 ? 0 : job.base.exprRef + d.exprRef + 1;
            row.dependencyKind = d.kind;
            row.sourceBits = d.srcR.bits;
            row.sourceExact = d.srcR.exact ? 1 : 0;
            row.targetBits = d.tgtR.bits;
            row.targetExact = d.tgtR.exact;
            row.mappingExact = d.mappingExact;
            writer.addNetDep(row);
            stats.deps++;
        }
    }

    int64_t descend(int64_t from, const std::vector<std::string>& segs) {
        int64_t node = from;
        for (auto& seg : segs) {
            auto pIt = childByName.find(node);
            if (pIt == childByName.end())
                return 0;
            auto cIt = pIt->second.find(seg);
            if (cIt == pIt->second.end())
                return 0;
            node = cIt->second;
        }
        return node == from && from == 0 ? 0 : node;
    }

    // ------------------------------------------------------------ members

    Compilation& compilation;
    AnalysisManager& analysis;
    Writer& writer;
    const SourceManager& sourceManager;
    std::map<std::string, Group> groups;
    std::unordered_map<const InstanceSymbol*, std::string> instanceGroup;
    std::map<std::string, Template> templates;
    std::unordered_map<const DefinitionSymbol*, int64_t> moduleIds;
    std::unordered_map<std::string, std::string> fileOrigins;

    // Global id counters; every table's ids are dense and process-issued.
    int64_t nodeCounter = 0;
    int64_t netCounter = 0;
    int64_t termCounter = 0;
    int64_t procCounter = 0;
    int64_t stmtCounter = 0;
    int64_t targetCounter = 0;
    int64_t operandCounter = 0;
    int64_t exprRefCounter = 0;
    int64_t procEventCounter = 0;
    int64_t depCounter = 0;
    int64_t connCounter = 0;
    int64_t hierRefCounter = 0;
    int64_t rootOrdinal = 0;

    std::unordered_map<int64_t, std::unordered_map<std::string, int64_t>> childByName;
    std::vector<ReplayJob> replayJobs;
    std::vector<CrossJob> crossJobs;
    /// node id -> (template, net id base) for net-name resolution at replay.
    std::unordered_map<int64_t, std::pair<const Template*, int64_t>> nodeTemplate;

    Stats stats;
};

} // namespace

Stats extract(Compilation& compilation, AnalysisManager& analysis, Writer& writer) {
    Walker walker(compilation, analysis, writer);
    return walker.run();
}

} // namespace designdb
