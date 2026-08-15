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
#include "slang/ast/Expression.h"
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
#include "slang/ast/EvalContext.h"
#include "slang/ast/ValuePath.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/TimingControl.h"
#include "slang/numeric/SVInt.h"
#include "slang/numeric/ConstantValue.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/types/Type.h"
#include "slang/text/SourceManager.h"

using namespace slang;
using namespace slang::ast;
using namespace slang::analysis;

namespace designdb {

namespace {

/// True for a symbol that is a compile-time constant rather than a net.
///
/// An enum member or a parameter is not something a waveform carries and not
/// something a trace can step to, so it is not an edge. Leaving them in also
/// swamped the count of genuinely dropped cross-module references: on one SoC
/// 47110 "external" symbols turned out to be 36860 enum members and 7330
/// package parameters, none of which were connectivity at all.
bool isConstantSymbol(const ValueSymbol& sym) {
    switch (sym.kind) {
        case SymbolKind::EnumValue:
        case SymbolKind::Parameter:
        case SymbolKind::Specparam:
            return true;
        case SymbolKind::Variable:
            // A `const` variable is a constant in everything but its symbol
            // kind, and `static const` class properties likewise.
            return sym.as<VariableSymbol>().flags.has(VariableFlags::Const);
        default:
            return false;
    }
}


/// The path of `sym` as seen from `body`, i.e. with the module's own prefix
/// removed. This is what makes a row shareable: it names a signal inside the
/// module rather than inside one instance of it.
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
    // Outside the module: an upward hierarchical reference, an interface
    // signal, a package item. Its absolute path cannot go into a row that every
    // instance of this module shares -- it would bake one instance's hierarchy
    // into all of them, and the other end of the reference would never join.
    // Reported as a count instead of stored wrongly.
    out = full;
    return false;
}

std::string typeOf(const ValueSymbol& sym) {
    return sym.getType().toString();
}

/// A file and a line that came from one source location.
///
/// The two travel together deliberately. Taking the line from a statement and
/// the file from its enclosing procedure names a line in a file that does not
/// contain it: a task body pulled in by `include` reported the module's file
/// with the included file's line number, and on a module shorter than that
/// number the pair points past the end of the file.
struct Where {
    std::string file;
    uint32_t line = 0;
};

/// `file` and `line` for a source location. Both are read from the same
/// location, and slang resolves a macro expansion identically for each, so the
/// pair always names a line in the file it says.
Where whereOf(SourceLocation loc, const SourceManager& sm) {
    if (!loc)
        return {};
    return Where{std::string(sm.getFileName(loc)),
                 static_cast<uint32_t>(sm.getLineNumber(loc))};
}

/// The canonical text of a reference that leaves its module: the path as
/// written, with every select resolved to the constant it elaborated to.
///
/// Not the raw source text, which is what this was at first and which fails
/// four different ways. A generate loop writes one `b[g].sig` and elaborates
/// four references from it, so the text was identical for all four and the
/// dedup key folded them into a single unresolvable row naming a genvar. Two
/// spellings of one reference (`tbm . ea` and `tbm.ea`) interned as two names,
/// as did one carrying a comment or a line break. And a reference assembled
/// through a macro spans two buffers, so it could not be recovered at all and
/// was silently dropped.
///
/// Trailing selects are left off: `path` names a signal and the bits it
/// touches are in `path_lo`/`path_hi`, which is how `edge` already spells the
/// same idea. Selects further in are structural -- `b[2].sig` is a member of
/// one interface instance out of an array -- and stay.
///
/// Empty when the reference is not expressible as a path, in which case the
/// caller counts it rather than storing a guess.
std::string canonicalPath(const Expression* e, EvalContext& eval) {
    // Strip the outermost selects: those are the bits, not the path.
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
                // A package item is written with its package, and the doc says
                // that spelling is what makes it storable -- a bare `mask`
                // resolves against imports a reader cannot see. Emitting the
                // name alone dropped the qualifier, so `pkg::mask` became
                // `mask` and was then discarded by the bare-name rule, while
                // `pkg::cfg.mode` survived reading like a module-relative
                // reference to something called `cfg`.
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

/// The reference as written, with whitespace and comments taken out.
///
/// The fallback for a reference `canonicalPath` cannot walk -- an XMR, which
/// slang resolves to a single node rather than to a chain of member accesses.
/// Its text has to stay as written, because that is the only spelling that
/// means the same thing from every instance of the module: the symbol's own
/// elaborated path names one instance's hierarchy, and baking that into a row
/// every instance shares is exactly the mistake the folded model exists to
/// avoid.
///
/// Normalising matters because the text is interned as a name: `tbc . glob`,
/// `tbc.glob` and `tbc/*why*/.glob` are one reference and were three rows.
/// `stripTrailingSelect` decides whether a final `[...]` is part of the name.
/// For a reference it is not -- the bits live in their own columns. For an
/// interface connection it is: `b[0]` names one element of an array, which is
/// structure, not a bit range.
std::string normalizedText(const Expression* e, const SourceManager& sm,
                           bool stripTrailingSelect = true) {
    if (!e)
        return {};
    auto range = e->sourceRange;
    if (!range.start() || !range.end() ||
        range.start().buffer() != range.end().buffer())
        return {};   // assembled through a macro: not recoverable as one span
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
            // A line comment runs to the newline, which is inside the range
            // when the reference spans lines. Skipping it is the same removal
            // the block-comment case does; giving up here discarded the row
            // outright, so `tb. // why\n u_deep.flag` was recorded nowhere.
            auto nl = raw.find('\n', i + 2);
            if (nl == std::string_view::npos)
                break;
            i = nl;
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(raw[i])))
            out += raw[i];
    }
    // Trailing selects are the bit range, which has columns of its own -- and
    // *every* one of them, matched by bracket depth.
    //
    // `rfind('[')` was wrong twice. It finds the innermost bracket when the
    // index itself contains a select, so `mem[idx[1]]` truncated to
    // `mem[idx`, naming nothing. And it removed one group where the bits are
    // offsets into the *root* object: `mem2[1][2]` kept `mem2[1]`, an 8-bit
    // object, beside offsets 10..10 that index the 32-bit one -- a path and a
    // range describing different things.
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
            break;              // unbalanced, or nothing but a select
        out.resize(i);
    }
    return out;
}

/// The `[...]` of the last segment of a hierarchical path -- how slang spells
/// an array element's index, already translated into the source's own
/// numbering rather than the zero-based element order.
std::string arrayIndexSuffix(const std::string& hierPath) {
    const size_t lastDot = hierPath.rfind('.');
    const std::string seg =
        lastDot == std::string::npos ? hierPath : hierPath.substr(lastDot + 1);
    const size_t open = seg.find('[');
    if (open == std::string::npos || seg.empty() || seg.back() != ']')
        return {};
    return seg.substr(open);
}

/// The word an assertion publishes as its `construct`. Spelled out rather than
/// taken from slang's enum printer, for the same reason `classify` is: these
/// are a wire format consumers match on.
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

/// What kind of construct a procedure is.
///
/// The words are spelled out rather than taken from slang's own enum printer,
/// because they are a wire format: `continuous_assign` / `procedural` /
/// `always_ff` are what the database publishes and what consumers match on.
/// Letting slang's spelling leak through would make the vocabulary change under
/// us on an upgrade.
void classify(const Symbol& sym, std::string& kind, std::string& construct) {
    switch (sym.kind) {
        case SymbolKind::ContinuousAssign:
            kind = "continuous_assign";
            construct = "assign";
            return;
        case SymbolKind::ProceduralBlock: {
            kind = "procedural";
            switch (sym.as<ProceduralBlockSymbol>().procedureKind) {
                case ProceduralBlockKind::AlwaysComb:  construct = "always_comb";  break;
                case ProceduralBlockKind::AlwaysLatch: construct = "always_latch"; break;
                case ProceduralBlockKind::AlwaysFF:    construct = "always_ff";    break;
                case ProceduralBlockKind::Always:      construct = "always";       break;
                case ProceduralBlockKind::Initial:     construct = "initial";      break;
                case ProceduralBlockKind::Final:       construct = "final";        break;
                default:                               construct = "procedural";   break;
            }
            return;
        }
        default:
            kind = "procedure";
            construct = "procedure";
            return;
    }
}

/// Every edge-triggered event in a timing control, in the order written.
///
/// All of them, not one: an event list has no ordering semantics, so
/// `@(posedge clk or negedge rst_n)` and `@(negedge rst_n or posedge clk)` are
/// the same block spelled two ways and both spellings are ordinary. Singling
/// out "the first" would record how the author arranged the list, and would
/// name the reset in half of all async-reset flops.
void collectEdgeEvents(const TimingControl* t,
                       std::vector<std::pair<const Expression*, std::string>>& out) {
    if (!t)
        return;
    switch (t->kind) {
        case TimingControlKind::SignalEvent: {
            auto& se = t->as<SignalEventControl>();
            if (se.edge == EdgeKind::None)
                return;                 // level-sensitive: that is the read set
            out.emplace_back(&se.expr, se.edge == EdgeKind::PosEdge   ? "posedge"
                                       : se.edge == EdgeKind::NegEdge ? "negedge"
                                                                      : "both");
            return;
        }
        case TimingControlKind::EventList:
            for (auto* c : t->as<EventListControl>().events)
                collectEdgeEvents(c, out);
            return;
        default:
            return;
    }
}

/// One module's parameter values, as stable text. Part of the module's
/// identity, so it must be deterministic: declaration order, which is what
/// slang preserves.
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
            // Spelled in full. ConstantValue::toString abbreviates above 128
            // bits by dropping the low digits, so two 256-bit INIT/SEED/POLY
            // values differing in their tail print identically and the two
            // parameterisations fold into one module.
            out += p.getValue().toString(SVInt::MAX_BITS);
        }
        else if (member.kind == SymbolKind::TypeParameter) {
            // A type parameter changes the module's contents just as a value
            // one does. Omitting it folded `box #(.T(logic[7:0]))` and
            // `box #(.T(logic[31:0]))` into a single row.
            auto& tp = member.as<TypeParameterSymbol>();
            out += std::string(tp.name);
            out += '=';
            out += tp.targetType.getType().toString();
        }
    }
    return out;
}

/// One reference to a signal, with the bits it touches.
///
/// `whole` is set when the range covers the entire object, which is the common
/// case and is stored as NULL rather than as an explicit 0..width-1 on every
/// row. A dynamic index (`q[i]`) has no static prefix narrower than the object,
/// so it also comes back whole -- the conservative answer, and the right one:
/// claiming a specific bit there would be a guess.
struct Ref {
    const ValueSymbol* sym = nullptr;
    uint64_t lo = 0;
    uint64_t hi = 0;
    bool whole = true;
    /// True when the range is exactly the bits touched. False when a dynamic
    /// selector meant it could not be narrowed, so the recorded range is an
    /// upper bound: `q[i] <= d` touches one bit of `q` and we cannot say which.
    /// Without this, "the whole signal" and "somewhere in the signal" are both
    /// stored as NULL and a consumer reads the second as the first.
    bool exact = true;
    /// The expression the reference was written as. Only consulted when the
    /// symbol turns out to live outside the module: its source text is the
    /// one instance-independent name the reference has (`bus.vld` reads the
    /// same in every instance), and hier_ref stores exactly that.
    const Expression* origin = nullptr;
};

/// How many bits a symbol's type can be selected out of.
///
/// `getBitWidth()` is 0 for anything non-integral, which forced every reference
/// to an unpacked array to be recorded as covering the whole object -- even
/// though slang computes real bounds for `mem[1]` in exactly the flattened
/// space this schema documents. Beyond losing precision that collapsed distinct
/// statements together, since the element index was the only thing telling them
/// apart: four assignments across a generate loop came out as four byte-
/// identical rows. `getSelectableWidth()` covers both.
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
        // `lspBounds` is only meaningful when a longest static prefix exists.
        // When bound computation fails -- an out-of-range or X-valued constant
        // index -- slang leaves it default-constructed {0,0} while rootSymbol()
        // stays valid, which reads as "bit 0" and is a specific wrong answer
        // rather than a vague one.
        return r;
    }
    r.lo = path.lspBounds.first;
    r.hi = path.lspBounds.second;
    const uint64_t width = bitWidthOf(*r.sym);
    // Whole when it spans the object, and whenever the extent is unknown: a
    // partial range on something whose size cannot be stated is not information
    // a consumer can use.
    r.whole = width == 0 || (r.lo == 0 && r.hi + 1 >= width);
    return r;
}

/// A reference's bit range as the dedup key needs it: as it will be *stored*,
/// not as computed. A root whose width is unknown always serialises as whole,
/// but its raw bounds differ per element, so keying on those stops the key
/// collapsing rows that come out byte-identical -- four identical rows for
/// `mem[g] <= a` across a four-iteration generate loop.
///
/// One function rather than one lambda per emitter: the procedure walk and the
/// primitive walk feed the *same* per-module set, so a change to how a range
/// keys has to reach both or they stop deduplicating against each other.
std::pair<uint64_t, uint64_t> keyRange(const Ref& r) {
    return r.whole ? std::make_pair<uint64_t, uint64_t>(0, 0)
                   : std::make_pair(r.lo, r.hi);
}

/// Collects every value path in an expression, each with its bit range.
/// Constants filtered out by the last collectRefs call. A consumer seeing an
/// assignment with one operand cannot otherwise tell "it reads one signal" from
/// "it reads one signal and three parameters I removed".
///
/// Accumulated, not assigned: an assignment's operands are gathered by three
/// passes -- the right-hand side, the subroutines it calls, and the selectors on
/// its left -- and each drops constants of its own. Resetting per pass counted
/// only the last one, so `q[WIDTH-1] <= f(a)` reported none of them. The single
/// reader clears it before the first pass and reads it after the third.
inline thread_local int64_t filteredConstants = 0;

void collectRefs(const Expression& expr, EvalContext& ctx, std::vector<Ref>& out,
                 bool skipSelectors = false) {
    ValuePath::visitPaths(
        expr, ctx,
        [&](const ValuePath& path) {
            Ref r = refOf(path);
            // A constant is not something a waveform carries or a trace can
            // step to. Letting one through is not merely noise: it makes the
            // assignment look as though it has a data source, which suppresses
            // the null-source row that records the statement, and the constant
            // itself is then dropped for living outside the module -- so a
            // signal assigned only from a package enum loses its driver
            // entirely.
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

struct StatementRefCollector : ASTVisitor<StatementRefCollector, VisitFlags::AllGood> {
    std::vector<Ref>& out;
    explicit StatementRefCollector(std::vector<Ref>& out) : out(out) {}
    void handle(const NamedValueExpression& e) { addRef(e); }
    void handle(const HierarchicalValueExpression& e) { addRef(e); }
    void addRef(const ValueExpressionBase& e) {
        Ref r;
        r.sym = &e.symbol;
        r.origin = &e;
        // No bounds were computed here, so the whole object is an upper bound
        // rather than the bits actually touched. Left exact, a property
        // checking `tag[1:0]` claimed to read the whole of `tag` --
        // uncertainty stored as fact, which is the one thing the range
        // encoding exists to keep apart.
        r.exact = false;
        out.push_back(r);
    }
};

/// Templated over the node kind: a subroutine body is a `Statement`, while an
/// assertion's body is an `AssertionExpr`, and both are walked the same way.
template<typename NodeT>
void collectStatementRefs(const NodeT& node, std::vector<Ref>& out) {
    StatementRefCollector c(out);
    node.visit(c);
    // Bit ranges are not resolved here: a subroutine's reads are attributed to
    // its call site, where the caller's own bounds are what matter.
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

/// Collects the value symbols an expression reads, with the bit range each
/// reference touches.
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

    /// A call reads whatever the subroutine reads.
    ///
    /// Without this, `y = masked(a)` depends only on `a`: the `mask` the
    /// function itself reads never reaches `y`, and the trace stops at the call.
    /// Only the subroutine's *free* variables are taken -- its arguments, locals
    /// and return value are internal and would be noise on every call site.
    void handle(const CallExpression& e) {
        visitDefault(e);                        // the arguments, always
        auto sub = std::get_if<const SubroutineSymbol*>(&e.subroutine);
        if (!sub || !*sub)
            return;                             // a system call has no body here
        if (!active.insert(*sub).second)
            return;                             // recursive: already on the stack
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


/// Walks a procedure statement by statement and reports each assignment's own
/// dependencies.
///
/// The alternative — pairing every symbol the procedure reads with every symbol
/// it drives — is what a `getReadSet()` x `getDrivers()` cross product gives,
/// and it is badly wrong for the blocks that matter most. One `always_ff` that
/// updates a dozen registers would report each of them as depending on all the
/// others' operands. Measured on a vendor PHY, a single register came back with
/// 2224 operand rows.
///
/// So: an assignment's right-hand side feeds its own left-hand side, and the
/// conditions of the enclosing `if`/`case` feed everything assigned inside the
/// branch — those genuinely do gate it. `gating` is that enclosing condition
/// stack, unwound on the way back out.
struct StatementWalker : public ASTVisitor<StatementWalker, VisitFlags::AllGood> {
    /// `src.sym` is null for a driver with no external operand.
    using Emit = std::function<void(const Ref& dst, const Ref& src, bool gatingEdge,
                                    SourceRange where)>;
    using EmitAssign = std::function<void(const Ref& dst, const std::vector<Ref>& operands,
                                          SourceRange where, int64_t seq, bool blocking,
                                          int64_t droppedConstants)>;
    /// A statement that reads without writing anything nameable.
    using EmitRead = std::function<void(const std::vector<Ref>& operands,
                                        const std::string& construct, SourceRange where)>;
    using EmitEvent = std::function<void(const Expression* expr, const std::string& edge,
                                         SourceRange where)>;

    Emit emit;
    EmitAssign emitAssign;
    EmitRead emitRead;
    EmitEvent emitEvent;
    EvalContext& eval;
    /// The timing control the procedure's sensitivity list was derived from.
    /// `always_ff @(posedge clk)` keeps its event as the body's leading timed
    /// statement, so without this the same event came out twice: once as
    /// sensitivity and once as a wait.
    const TimingControl* sensitivityTiming = nullptr;
    std::vector<Ref> gating;
    int64_t seq = 0;
    std::set<const SubroutineSymbol*> activeSubs;
    std::set<const SubroutineSymbol*> walkedSubs;
    std::set<const ValueSymbol*> loopVars;


    StatementWalker(Emit emit, EmitAssign emitAssign, EmitEvent emitEvent,
                    EmitRead emitRead, EvalContext& eval) :
        emit(std::move(emit)), emitAssign(std::move(emitAssign)),
        emitEvent(std::move(emitEvent)), emitRead(std::move(emitRead)),
        eval(eval) {}

    /// `assert (req !== 1'bx);` and its siblings.
    ///
    /// An assertion writes nothing, so the walk had nothing to pair its reads
    /// with and the whole statement vanished: the signals it checks read as
    /// though no part of the design looked at them. They are reads like any
    /// other, recorded against the statement rather than against a target.
    void handle(const ImmediateAssertionStatement& stmt) {
        std::vector<Ref> reads;
        collectRefs(stmt.cond, eval, reads);
        emitRead(reads, assertionWord(stmt.assertionKind), stmt.sourceRange);
        visitDefault(stmt);
    }

    /// The concurrent form: `assert property (@(posedge clk) a |-> b);`.
    /// Its body is an assertion expression rather than an ordinary one, so the
    /// reads are gathered by walking it for value references.
    void handle(const ConcurrentAssertionStatement& stmt) {
        std::vector<Ref> reads;
        collectStatementRefs(stmt.propertySpec, reads);
        emitRead(reads, assertionWord(stmt.assertionKind), stmt.sourceRange);
        visitDefault(stmt);
    }

    /// `wait (done);` -- the condition is read, and the statement writes
    /// nothing. The body, if there is one, is walked as usual and its own
    /// writes are recorded normally.
    void handle(const WaitStatement& stmt) {
        std::vector<Ref> reads;
        collectRefs(stmt.cond, eval, reads);
        emitRead(reads, "wait", stmt.sourceRange);
        visitDefault(stmt);
    }

    /// A statement whose whole effect is to read: `$display("%0h", x);`,
    /// `$error(...)`, a void call to a task that only samples.
    ///
    /// The rule the assertion handlers above are a special case of -- a
    /// statement that reads and writes nothing this module can name still
    /// read. Without it, a signal a testbench only ever prints came back as
    /// one nothing in the design had ever looked at, which is a wrong answer
    /// rather than a coarse one: the `$display` is right there in the source.
    ///
    /// Only calls whose own subroutine assigns nothing reach here. A task that
    /// writes is walked by `handle(CallExpression)` and its writes are paired
    /// with their targets in the ordinary way, so its reads are already
    /// attributed and must not be recorded twice.
    void handle(const ExpressionStatement& stmt) {
        if (stmt.expr.kind != ExpressionKind::Call) {
            visitDefault(stmt);
            return;
        }
        auto& call = stmt.expr.as<CallExpression>();
        std::vector<Ref> reads;
        collectRefs(stmt.expr, eval, reads);
        collectCallReads(stmt.expr, reads);
        // What the statement writes is not what it reads. slang models a
        // system task's output argument as an assignment inside the call --
        // which is how the write already reaches `assignment` -- and taking
        // every operand made `$readmemh("f", mem)` report that `mem` reads
        // itself, at the very line that loads it.
        std::set<const ValueSymbol*> written;
        collectWrittenTargets(stmt.expr, written);
        if (!written.empty()) {
            reads.erase(std::remove_if(reads.begin(), reads.end(),
                                       [&](const Ref& r) {
                                           return r.sym && written.count(r.sym);
                                       }),
                        reads.end());
        }
        if (!reads.empty() && !callWrites(call))
            emitRead(reads, callWord(call), stmt.sourceRange);
        visitDefault(stmt);
    }

    /// Every symbol an expression assigns, however deeply nested.
    void collectWrittenTargets(const Expression& expr,
                               std::set<const ValueSymbol*>& out) {
        struct Finder : ASTVisitor<Finder, VisitFlags::AllGood> {
            StatementWalker& self;
            std::set<const ValueSymbol*>& out;
            Finder(StatementWalker& self, std::set<const ValueSymbol*>& out) :
                self(self), out(out) {}
            void handle(const AssignmentExpression& e) {
                std::vector<Ref> targets;
                collectRefs(e.left(), self.eval, targets, /*skipSelectors=*/true);
                for (auto& t : targets)
                    if (t.sym)
                        out.insert(t.sym);
                visitDefault(e);
            }
        };
        Finder f(*this, out);
        expr.visit(f);
    }

    /// The word for a call, as `construct`: the system task's own name
    /// (`$display`), or `call` for a void call to a user subroutine.
    static std::string callWord(const CallExpression& call) {
        if (call.isSystemCall())
            return std::string(call.getSubroutineName());
        return "call";
    }

    /// Whether a call can assign anything at all, directly or through what it
    /// calls. A system call cannot; a user subroutine is asked for its drivers.
    bool callWrites(const CallExpression& call) {
        auto sub = std::get_if<const SubroutineSymbol*>(&call.subroutine);
        if (!sub || !*sub)
            return false;                 // a system call writes no signal
        struct WriteFinder : ASTVisitor<WriteFinder, VisitFlags::AllGood> {
            bool found = false;
            void handle(const AssignmentExpression&) { found = true; }
            void handle(const UnaryExpression& e) {
                switch (e.op) {
                    case UnaryOperator::Preincrement:
                    case UnaryOperator::Predecrement:
                    case UnaryOperator::Postincrement:
                    case UnaryOperator::Postdecrement:
                        found = true;
                        return;
                    default:
                        visitDefault(e);
                        return;
                }
            }
        };
        WriteFinder f;
        (*sub)->getBody().visit(f);
        return f.found;
    }

    /// A statement-level event control: `@(posedge clk); …` in an initial
    /// block or a task. It is a wait rather than sensitivity, but the signal
    /// is sampled either way, and the export had no trace of the read at all.
    void handle(const TimedStatement& stmt) {
        if (&stmt.timing != sensitivityTiming) {
            std::vector<std::pair<const Expression*, std::string>> raw;
            collectEdgeEvents(&stmt.timing, raw);
            for (auto& [expr, edge] : raw)
                emitEvent(expr, edge, stmt.sourceRange);
        }
        visitDefault(stmt);
    }

    void handle(const ConditionalStatement& stmt) {
        const size_t mark = gating.size();
        for (auto& cond : stmt.conditions)
            collectRefs(*cond.expr, eval, gating);
        visitDefault(stmt);
        gating.resize(mark);
    }

    void handle(const CaseStatement& stmt) {
        const size_t mark = gating.size();
        collectRefs(stmt.expr, eval, gating);
        // Item labels select a branch, so they gate it as the case expression
        // does. Constant labels contribute no operand and drop out.
        for (auto& item : stmt.items) {
            for (auto* label : item.expressions)
                collectRefs(*label, eval, gating);
        }
        visitDefault(stmt);
        gating.resize(mark);
    }

    void handle(const ForLoopStatement& stmt) {
        const size_t mark = gating.size();
        if (stmt.stopExpr)
            collectRefs(*stmt.stopExpr, eval, gating);
        // The loop's own control variables are iteration counters, not design
        // signals. Recorded as targets they appear as module-level nets driven
        // by the increment, where they collide with any real signal of the same
        // name -- `for (int i = …)` in two procedures both writing to `i`.
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

    /// `x++` / `--x` are unary expressions, not assignments, so without this a
    /// counter written that way has no driver at all while `x <= x + 1` works.
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
                continue;               // a loop counter, not a design signal
            // `cnt++` writes cnt just as `cnt <= cnt + 1` does; without this the
            // target keeps its edges but loses its statement line, seq, and
            // blocking kind. It *reads* cnt the same way, so the operand and the
            // self-edge are recorded exactly as if it were spelled out.
            emitAssign(dst, {dst}, expr.sourceRange, seq++, true, 0);
            // Data before gating, as the assignment path does. An operand can
            // reach a target both ways at one statement -- `if (c < lim) c++;`
            // reads `c` in the condition and in the increment -- and the edge
            // that survives is whichever is emitted first, because `control`
            // is not part of the dedup key. Emitting gating first here made
            // the increment's own dependency read as a branch condition,
            // while the identical circuit spelled `c <= c + 1` read as data.
            // The data dependency is the stronger fact and the one a consumer
            // filtering `control = 0` is asking for.
            emit(dst, dst, false, expr.sourceRange);
            for (auto& src : gating)
                emit(dst, src, true, expr.sourceRange);
        }
        visitDefault(expr);
    }

    /// A called task or function may itself assign a module signal. Without
    /// following it, `always_ff @(posedge clk) bump();` records nothing at all
    /// and the register `bump` writes has no driver.
    void handle(const CallExpression& expr) {
        visitDefault(expr);
        auto sub = std::get_if<const SubroutineSymbol*>(&expr.subroutine);
        if (!sub || !*sub)
            return;
        // Every call, before the once-per-subroutine guard below: the body is
        // the same wherever it is called from, but the actuals are not.
        bindArguments(expr, **sub);
        if (!activeSubs.insert(*sub).second)
            return;                             // recursion guard
        // Once per subroutine, not once per call. A function called from four
        // places was having its body walked four times, and while `edge`
        // deduplicates, `assignment` does not -- so its internal statements came
        // out four times over. The targets are function locals in any case:
        // they have no `symbol` row and no waveform, so they are attributed but
        // not re-attributed.
        if (walkedSubs.insert(*sub).second)
            (*sub)->getBody().visit(*this);
        activeSubs.erase(*sub);
    }

    /// The actuals at a call site, tied to the formals they bind to.
    ///
    /// The body is walked once and yields `bump.v -> q`; without this the
    /// other half of the chain -- `d -> bump.v` -- was never recorded, so
    /// `always_ff @(posedge clk) bump(d);` left `d` reading as though nothing
    /// used it and `q` as though it came from a local of a task nothing fed.
    /// The formal has no `symbol` row, being a subroutine local, which is the
    /// same footing every other reference to one already stands on.
    ///
    /// Direction decides which way the edge points: an `input` formal is fed
    /// by the actual, an `output` feeds it, and `inout`/`ref` do both.
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
            // A written actual is a target, so its selectors are reads of the
            // index rather than part of what is written -- the same split the
            // assignment path already makes on an lvalue.
            collectRefs(*args[i], eval, actuals, /*skipSelectors=*/writes);
            for (auto& a : actuals) {
                if (!a.sym)
                    continue;
                if (reads)
                    emit(formal, a, false, expr.sourceRange);
                if (writes)
                    emit(a, formal, false, expr.sourceRange);
            }
        }
    }

    /// A call reads whatever the subroutine reads.
    ///
    /// `ValuePath::visitPaths` visits a call's arguments and stops there, so
    /// without this `y = masked(a)` depends on `a` alone and the `mask` the
    /// function itself reads never reaches `y` -- the trace stops at the call.
    /// Only the subroutine's *free* variables are taken; its arguments, locals
    /// and return value are internal and would be noise on every call site.
    void collectCallReads(const Expression& expr, std::vector<Ref>& out) {
        struct CallFinder : ASTVisitor<CallFinder, VisitFlags::AllGood> {
            StatementWalker& self;
            std::vector<Ref>& out;
            CallFinder(StatementWalker& self, std::vector<Ref>& out) :
                self(self), out(out) {}
            void handle(const CallExpression& call) {
                visitDefault(call);
                auto sub = std::get_if<const SubroutineSymbol*>(&call.subroutine);
                if (!sub || !*sub)
                    return;
                if (!self.activeSubs.insert(*sub).second)
                    return;                     // recursive: already on the stack
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
                self.activeSubs.erase(*sub);
            }
        };
        CallFinder finder(*this, out);
        expr.visit(finder);
    }

    void handle(const AssignmentExpression& expr) {
        // Targets come from slang's own path analysis rather than a hand-rolled
        // walk of the lvalue: it resolves the root symbol and the bit range the
        // longest static prefix selects, which is exactly what a member access
        // or a part-select on the left means. A concatenation yields one path
        // per element, so `{carry, sum} = …` drives both.
        std::vector<Ref> targets;
        collectRefs(expr.left(), eval, targets, /*skipSelectors=*/true);
        if (targets.empty()) {
            // Path analysis found nothing to write. No legal RTL is known to
            // reach here -- the case that prompted this, `assign q[i] = …` with
            // a non-constant index, is an elaboration error in slang rather
            // than a path it declines to compute. It is kept because losing a
            // driver outright is the one outcome that must not happen, and it
            // resolves the root by walking the lvalue rather than through
            // `getSymbolReference`, which hands back a *field* symbol for a
            // member access on an unpacked struct or a class handle.
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
                targets.push_back(r);
            }
            else {
                visitDefault(expr);
                return;
            }
        }

        std::vector<Ref> reads;
        filteredConstants = 0;
        collectRefs(expr.right(), eval, reads);
        collectCallReads(expr.right(), reads);
        // An index or part-select on the left is read, not written: `q[i] <= d`
        // depends on `i`. Those live inside the selectors that the target pass
        // skipped, so they are collected separately.
        collectLeftSelectorRefs(expr.left(), reads);
        const int64_t droppedConstants = filteredConstants;

        // Every non-constant selector evaluation appends a diagnostic and a
        // note to the context, and nothing here ever reports them, so on a long
        // procedure they accumulate for the whole traversal. Clearing after each
        // assignment measurably bounds it.
        eval.reset();

        for (auto& dst : targets) {
            if (loopVars.count(dst.sym))
                continue;
            emitAssign(dst, reads, expr.sourceRange, seq++, expr.isBlocking(), droppedConstants);
            for (auto& src : reads)
                emit(dst, src, false, expr.sourceRange);
            for (auto& src : gating)
                emit(dst, src, true, expr.sourceRange);
            // A right-hand side that reads nothing at all -- `q <= 8'h0` --
            // still has a driver, and a query for what drives the target has
            // to be able to name the statement. One row with a null source
            // records it; the schema stores edges, so without this the driver
            // simply is not there. A self-read (`cnt <= cnt + 1`) does not land
            // here: its edge is real and already names the statement.
            if (reads.empty())
                emit(dst, Ref{}, false, expr.sourceRange);
        }

        visitDefault(expr);
    }

    /// Reads that appear inside an lvalue's selectors, excluding the target.
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


};

/// The path segment slang gives a generate block, so a prefix built here joins
/// against the hierarchical paths everything else is named by.
///
/// A loop iteration is named by the genvar's *value*, not by its position among
/// the entries (`slang/source/ast/Symbol.cpp`). Counting entries agrees only
/// when the loop runs 0,1,2,...; `for (genvar g = 1; g < 32; g++)` shifts every
/// name by one, and a descending loop reverses them. On one SoC that mismatched
/// 70 declarations against their own dataflow, and nothing joined.
std::string generateSegment(const GenerateBlockSymbol& block) {
    if (auto* index = block.getArrayIndex())
        return "[" + index->toString(LiteralBase::Decimal, false) + "]";
    std::string name(block.name);
    return name.empty() ? block.getExternalName() : name;
}

/// Calls `fn` for every instance directly inside `scope`, descending through
/// generate blocks but never into an instance's own body.
///
/// Generate blocks matter and are easy to miss: an instance written inside
/// `for (…) begin : g_lane` is a member of the *block*, not of the module body,
/// so a scan of the body's own members finds nothing. A design that puts its
/// replication in a generate loop — which is most of them — would come out with
/// its leaves missing and no error to say so.
/// Calls `fn` for every member of `scope` of kind `K`, descending through
/// generate blocks and instance arrays but never into an instance's own body.
///
/// Generate blocks matter and are easy to miss: an instance written inside
/// `for (…) begin : g_lane` is a member of the *block*, not of the module body,
/// so a scan of the body's own members finds nothing. A design that puts its
/// replication in a generate loop — which is most of them — would come out with
/// its leaves missing and no error to say so.
///
/// One traversal for every leaf kind rather than one copy each. The descent
/// rules are subtle and shared: an uninstantiated generate branch is in the
/// AST but not in the design, a `GenerateBlockArray` holds its iterations as
/// entries, and `foo u [3:0] (...)` is an `InstanceArray` wrapping the
/// elements. Copies of this drifted apart -- one leaf kind would have gone
/// missing from whichever containers only the other copy had learned about,
/// with no error to say so.
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
                // A branch this parameterisation did not take: its contents are
                // present in the AST but are not part of the elaborated design.
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

/// Every instance directly inside `scope`.
template<typename F>
void forEachInstance(const Scope& scope, F&& fn) {
    forEachOfKind<SymbolKind::Instance, InstanceSymbol>(scope, fn);
}

/// Every primitive instance -- a gate, a switch, a UDP -- inside `scope`.
/// `and g [3:0] (...)` inside a generate loop is the ordinary spelling of
/// replicated gate-level logic, so it needs the same descent.
template<typename F>
void forEachPrimitive(const Scope& scope, F&& fn) {
    forEachOfKind<SymbolKind::PrimitiveInstance, PrimitiveInstanceSymbol>(scope, fn);
}

/// (target, source, file, line, target bits, source bits) already emitted for
/// one module.
///
/// The bit range belongs in the key: two part-selects of one signal assigned
/// from one source on one line are two edges, and without it the second is
/// discarded. So does the file, and for a sharper reason -- a line number is
/// only unique within one file, and a module whose task body comes from an
/// `include`d header has two sources of statements. Keyed on the line alone,
/// a statement in the header silently deleted the edges of a statement at the
/// same line number in the module's own file; `assignment`, which does not
/// dedup, kept both, so the two tables disagreed about how many statements
/// drive the target. The file is interned to an id so the key stays cheap to
/// compare.
using SeenSet = std::set<std::tuple<const ValueSymbol*, const ValueSymbol*, uint32_t,
                                    uint32_t, uint64_t, uint64_t, uint64_t, uint64_t>>;

/// Calls `fn` for every instantiation in `scope` whose module slang could not
/// resolve, descending generate blocks exactly as `forEachInstance` does.
///
/// These are instances of a module that failed to parse, or that is simply not
/// in the filelist. slang keeps them as `UninstantiatedDefSymbol` -- the name
/// and the definition it wanted are known, the body is not. Skipping them is
/// the worst option available: the parent then shows fewer children than the
/// RTL has, and nothing in the database says a subtree is missing.
template<typename F>
void forEachUnresolved(const Scope& scope, F&& fn, const std::string& gen = "") {
    for (auto& member : scope.members()) {
        switch (member.kind) {
            case SymbolKind::UninstantiatedDef:
                fn(member.as<UninstantiatedDefSymbol>(), gen);
                break;
            case SymbolKind::GenerateBlock: {
                auto& block = member.as<GenerateBlockSymbol>();
                if (block.isUninstantiated)
                    break;
                forEachUnresolved(block, fn, gen + generateSegment(block) + ".");
                break;
            }
            case SymbolKind::GenerateBlockArray: {
                auto& arr = member.as<GenerateBlockArraySymbol>();
                std::string base(arr.name);
                if (base.empty())
                    base = arr.getExternalName();
                for (auto& entry : arr.entries) {
                    std::string prefix = gen + base;
                    if (entry->kind == SymbolKind::GenerateBlock)
                        prefix += generateSegment(entry->as<GenerateBlockSymbol>());
                    forEachUnresolved(*entry, fn, prefix + ".");
                }
                break;
            }
            case SymbolKind::InstanceArray:
                forEachUnresolved(member.as<InstanceArraySymbol>(), fn, gen);
                break;
            default:
                break;
        }
    }
}

/// Every declaration in a module body, generate blocks included.
///
/// One row per actual signal: slang carries a `Port` symbol *and* the net or
/// variable behind it, so the port's direction is folded onto the signal's row
/// instead of producing a second one. A port whose internal symbol is null (a
/// null port, or one connecting straight to an expression) keeps a row of its
/// own, since there is nothing else to carry it.
template<typename F>
void forEachDeclaration(const Scope& scope, F&& fn, const std::string& gen = "") {
    // First pass: which signal does each port stand for, and in which direction.
    //
    // A port that stands for a concatenation of internal signals
    // (`.ext_pair({hi, lo})`) is not reachable this way -- neither the scope's
    // members nor the body's port list offers it, and `getInternalExpr` is
    // empty for it -- so `hi` and `lo` keep a NULL direction. Recorded as a
    // known limit rather than guessed at; the binding itself is in `port`.
    std::unordered_map<const Symbol*, ArgumentDirection> portDir;
    for (auto& member : scope.members()) {
        if (member.kind != SymbolKind::Port)
            continue;
        auto& port = member.as<PortSymbol>();
        if (port.internalSymbol)
            portDir.emplace(port.internalSymbol, port.direction);
    }

    for (auto& member : scope.members()) {
        switch (member.kind) {
            case SymbolKind::Variable:
            case SymbolKind::Net:
            case SymbolKind::Parameter: {
                auto it = portDir.find(&member);
                fn(member, gen, it == portDir.end()
                                    ? std::optional<ArgumentDirection>{}
                                    : std::optional<ArgumentDirection>{it->second});
                break;
            }
            case SymbolKind::Port: {
                auto& port = member.as<PortSymbol>();
                if (!port.internalSymbol)
                    fn(member, gen, std::optional<ArgumentDirection>{port.direction});
                break;
            }
            case SymbolKind::InterfacePort:
                // No net or variable stands behind an interface port, so it
                // gets a row of its own -- it is the name a reference like
                // `bus.vld` resolves its first segment against, and without
                // the row that segment matches nothing in the module.
                fn(member, gen, std::optional<ArgumentDirection>{});
                break;
            case SymbolKind::GenerateBlock: {
                auto& block = member.as<GenerateBlockSymbol>();
                if (block.isUninstantiated)
                    break;
                forEachDeclaration(block, fn, gen + generateSegment(block) + ".");
                break;
            }
            case SymbolKind::GenerateBlockArray: {
                auto& arr = member.as<GenerateBlockArraySymbol>();
                std::string base(arr.name);
                if (base.empty())
                    base = arr.getExternalName();
                for (auto& entry : arr.entries) {
                    std::string prefix = gen + base;
                    if (entry->kind == SymbolKind::GenerateBlock)
                        prefix += generateSegment(entry->as<GenerateBlockSymbol>());
                    forEachDeclaration(*entry, fn, prefix + ".");
                }
                break;
            }
            default:
                break;
        }
    }
}


/// The word for a symbol kind, in the vocabulary the consumer already uses.
std::string symbolKindName(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::Variable:      return "variable";
        case SymbolKind::Net:           return "net";
        case SymbolKind::Parameter:     return "parameter";
        case SymbolKind::Port:          return "port";
        case SymbolKind::InterfacePort: return "interface_port";
        default:                        return "other";
    }
}

std::string directionName(ArgumentDirection d) {
    switch (d) {
        case ArgumentDirection::In:    return "in";
        case ArgumentDirection::Out:   return "out";
        case ArgumentDirection::InOut: return "inout";
        default:                       return "ref";
    }
}

class Walker {
public:
    Walker(Compilation& comp, AnalysisManager& mgr, Writer& w) :
        compilation(comp), analysis(mgr), writer(w),
        sourceManager(*comp.getSourceManager()) {}

    Stats run() {
        // Pass 1: group every instance by what module it actually is, and pick
        // one body per group to extract from.
        //
        // The group key is (definition, parameter values), not the body pointer.
        // slang shares a canonical body between identical instances only
        // sometimes; where it does not, keying on the pointer would emit one
        // module per instance. It is also not enough to take the first body
        // seen: the analysis manager analyses the canonical body, so a
        // non-canonical one has no AnalyzedScope and would contribute no
        // dataflow at all — silently, since an empty scope looks the same as a
        // module with no logic.
        for (auto inst : compilation.getRoot().topInstances)
            collect(*inst);

        // Every group gets its row id before anything is emitted: a port
        // connection names the module on the other side of it, and that module
        // may well be one this loop has not reached yet.
        for (auto& [key, group] : groups) {
            group.id = writer.internModule(group.name, group.params);
            stats.modules++;
        }
        for (auto& [key, group] : groups)
            emitModule(*group.body, group.id);

        // Pass 2: the instance tree, now that every module has a row.
        for (auto inst : compilation.getRoot().topInstances)
            visitInstance(*inst, 0, "");

        // Last: file rows exist only once something interned them.
        writer.linkSourceFiles(fileOrigins);
        return stats;
    }

private:
    struct Group {
        std::string name;
        std::string params;
        const InstanceBodySymbol* body = nullptr;
        int64_t id = 0;
    };

    /// The body to extract a group's dataflow from: one that the analysis
    /// manager actually analysed, else the first seen.
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

    int64_t moduleIdFor(const InstanceSymbol& inst) {
        auto it = instanceGroup.find(&inst);
        return it == instanceGroup.end() ? 0 : groups[it->second].id;
    }

    /// Remembers which absolute path an as-written file spelling came from, so
    /// the writer can join `file` rows to `source_file` rows at the end. The
    /// two spellings genuinely differ -- rows carry the filelist's relative
    /// path, source_file the hashed absolute one -- and matching them by
    /// basename breaks on the first design with two files of one name.
    void noteFile(SourceLocation loc, const std::string& asWritten) {
        if (!loc || asWritten.empty())
            return;
        if (fileOrigins.count(asWritten))
            return;
        // Through the expansion, not the raw buffer. A location inside a macro
        // body belongs to the macro's own buffer, which is not a file, so
        // `getFullPath` answers with nothing -- and because that nothing was
        // cached like any other answer, a single macro-expanded row anywhere
        // in a file left the whole file with a NULL `source_file` and no
        // digest to check it against. `getFileName` resolves the expansion, so
        // this has to resolve it the same way to name the same file.
        auto path = sourceManager.getFullPath(
            sourceManager.getFullyExpandedLoc(loc).buffer());
        if (path.empty())
            return;   // no origin to record; caching the miss would suppress
                      // the next sighting, which may well have one
        fileOrigins.emplace(asWritten, path.string());
    }

    /// A small id for a file name, so the dedup key can carry the file without
    /// carrying the string. Ids are global rather than per module, which costs
    /// nothing: the set they key is cleared for each module anyway.
    uint32_t fileKey(const std::string& path) {
        if (path.empty())
            return 0;
        auto [it, added] = fileKeyIds.try_emplace(path,
                                                  uint32_t(fileKeyIds.size() + 1));
        (void)added;
        return it->second;
    }

    /// Where a location is, with its file's origin recorded on the way past.
    ///
    /// One call rather than a `whereOf` and a `noteFile`: the two were spelled
    /// out separately at four sites, nothing enforced the pairing, and a site
    /// that resolved a location without noting it left that file unjoinable to
    /// its digest -- invisible until a consumer's join came back empty.
    /// `fallback` is what an unknown location resolves to, which is how a
    /// statement with no location of its own inherits its procedure's.
    Where locate(SourceLocation loc, const Where& fallback = {}) {
        if (!loc)
            return fallback;
        Where w = whereOf(loc, sourceManager);
        noteFile(loc, w.file);
        return w;
    }

    /// The reads of a statement that writes nothing this module can name.
    ///
    /// An assertion writes nothing at all; an assignment whose target is
    /// outward writes something with no module-relative name. Either way the
    /// operands fit in no other table -- `edge` needs a target, and an
    /// `assignment` row cannot exist without one -- and the signals read as
    /// though the design never looked at them. An operand that is itself
    /// outward goes to `hier_ref`, as everywhere else.
    void addStmtReads(const std::vector<Ref>& operands, const std::string& prefix,
                      const std::string& kind, const std::string& construct,
                      const Where& at, EvalContext& eval) {
        for (auto& r : operands) {
            if (!r.sym)
                continue;
            std::string rel;
            if (!relativePath(*r.sym, prefix, rel)) {
                addHierRef(false, r, kind, construct, at, eval);
                continue;
            }
            StmtReadRow row;
            row.name = std::move(rel);
            row.kind = kind;
            row.construct = construct;
            row.file = at.file;
            row.line = at.line;
            if (!r.whole)
                row.bits = std::make_pair(r.lo, r.hi);
            row.exact = r.exact;
            stmtReads.push_back(std::move(row));
        }
    }

    /// Records one reference that leaves the module, as written. Every such
    /// reference also bumps the external counter, which is where all of them
    /// lived before hier_ref existed; the row is added only when the text is
    /// recoverable and actually is a path -- a bare name that leaves the
    /// module (a subroutine's package-level free variable) resolves against
    /// imports this table cannot see, so it stays a count.
    /// The expression comes from `r.origin` rather than as its own argument:
    /// the two were passed side by side at every call site, nothing tied them
    /// together, and a transposed pair would have recorded one reference's
    /// text against another's bit range -- a wrong row, not an error.
    void addHierRef(bool isWrite, const Ref& r, const std::string& kind,
                    const std::string& construct, const Where& at,
                    EvalContext& eval) {
        // One reference, one row, however many passes surface it: the edge, the
        // assignment's operand list and the gating scan all hand over the same
        // expression node, so the node itself is the identity. Keying on the
        // text instead lost data -- a generate loop elaborates `b[g].sig` into
        // four distinct references that share one spelling, and three of them
        // were dropped without being counted.
        if (!hierSeen.emplace(r.origin, isWrite).second)
            return;
        std::string text = canonicalPath(r.origin, eval);
        if (text.empty())
            text = normalizedText(r.origin, sourceManager);
        // A path, or a package qualification. `pkg::mask` is stored for the
        // reason the doc gives -- it names where to look -- while a bare
        // `mask` resolves against imports a reader cannot see and stays a
        // count. Testing only for a dot discarded the qualified form too.
        if (text.empty() || (text.find('.') == std::string::npos &&
                             text.find("::") == std::string::npos)) {
            stats.external++;
            return;
        }
        stats.external++;
        HierRefRow row;
        row.path = std::move(text);
        row.write = isWrite;
        row.kind = kind;
        row.construct = construct;
        row.file = at.file;
        row.line = at.line;
        if (r.sym && !r.whole)
            row.bits = std::make_pair(r.lo, r.hi);
        row.exact = r.sym ? r.exact : true;
        hierRefs.push_back(std::move(row));
    }

    /// Everything that belongs to the module rather than to an instance: its
    /// intra-module dataflow and the list of what it instantiates. Emitted once
    /// per canonical body, however many instances share it.
    void emitModule(const InstanceBodySymbol& body, int64_t moduleId) {
        const std::string prefix = body.getHierarchicalPath();
        hierRefs.clear();
        stmtReads.clear();
        hierSeen.clear();

        std::vector<EdgeRow> edges;
        // One dedup set for the whole module: procedures and primitives can
        // legitimately drive the same pair from the same line only in the
        // replicated-generate case the set exists to fold.
        SeenSet seen;
        if (auto* scope = analysis.getAnalyzedScope(body)) {
            int64_t procIndex = 0;
            for (auto& proc : scope->procedures)
                emitProcedure(proc, prefix, edges, seen, moduleId, procIndex++);
        }
        emitPrimitives(body, prefix, edges, seen);
        emitNetInitialisers(body, prefix, edges, seen, moduleId);
        stats.edges += static_cast<int64_t>(edges.size());
        writer.addEdges(moduleId, edges);

        std::vector<SymbolRow> symbols;
        forEachDeclaration(body, [&](const Symbol& sym, const std::string& gen,
                                     std::optional<ArgumentDirection> dir) {
            SymbolRow row;
            row.name = gen + std::string(sym.name);
            if (row.name.empty() || std::string(sym.name).empty())
                return;                   // an unnamed declaration is not askable
            row.kind = symbolKindName(sym.kind);
            if (dir)
                row.direction = directionName(*dir);
            if (ValueSymbol::isKind(sym.kind)) {
                auto& vs = sym.as<ValueSymbol>();
                row.type = vs.getType().toString();
                if (vs.getType().isIntegral())
                    row.width = static_cast<int64_t>(vs.getType().getBitWidth());
            }
            else if (sym.kind == SymbolKind::InterfacePort) {
                // Not a value symbol, so the type is spelled by hand: the
                // interface definition, with the declared modport when the
                // port restricts itself to one.
                auto& ip = sym.as<InterfacePortSymbol>();
                if (ip.interfaceDef)
                    row.type = std::string(ip.interfaceDef->name);
                if (!ip.modport.empty())
                    row.type += "." + std::string(ip.modport);
            }
            const Where at = locate(sym.location);
            row.file = at.file;
            row.line = at.line;
            if (sym.location)
                row.col = static_cast<uint32_t>(sourceManager.getColumnNumber(sym.location));
            symbols.push_back(std::move(row));
        });
        stats.symbols += static_cast<int64_t>(symbols.size());
        writer.addSymbols(moduleId, symbols);

        std::vector<ChildRow> children;
        forEachInstance(body, [&](const InstanceSymbol& child) {
            // Named relative to the module, generate-block prefix included, so
            // the row reads the same for every instance that shares this body.
            std::string childName;
            relativePath(child, prefix, childName);
            auto it = groups.find(groupKey(child.getCanonicalBody() ? *child.getCanonicalBody()
                                                                    : child.body));
            const int64_t defModule = it == groups.end() ? 0 : it->second.id;
            children.push_back(ChildRow{childName,
                                        std::string(child.getDefinition().name),
                                        defModule});
            std::vector<PortRow> ports;
            emitPorts(child, childName, prefix, ports, body);
            stats.ports += static_cast<int64_t>(ports.size());
            writer.addPorts(moduleId, defModule, ports);
        });
        // Instantiations slang could not resolve are recorded with a null
        // child_module: a consumer can then tell "there is an instance here
        // whose module I do not have" apart from "this module instantiates
        // nothing", which is the distinction that matters when a trace stops.
        forEachUnresolved(body, [&](const UninstantiatedDefSymbol& u,
                                    const std::string& gen) {
            // An unnamed instantiation would intern to name id 0, which is not
            // a row, and every join on it would silently drop the child.
            std::string name(u.name);
            if (name.empty())
                name = "<unnamed>";
            children.push_back(ChildRow{gen + name,
                                        std::string(u.definitionName), 0});
            stats.unresolved++;
        });
        stats.children += static_cast<int64_t>(children.size());
        writer.addChildren(moduleId, children);

        writer.addHierRefs(moduleId, hierRefs);
        writer.addStmtReads(moduleId, stmtReads);
        stats.stmtReads += static_cast<int64_t>(stmtReads.size());
    }

    /// One event as a proc_event row: named module-relative where it can be,
    /// recorded with an empty signal where it cannot -- the event being an
    /// edge is a fact even where its source is not nameable. An external
    /// event signal (`@(posedge tb.clk)`) also lands in hier_ref.
    ProcEventRow eventRow(const Expression* expr, const std::string& edge,
                          const std::string& prefix, const std::string& kind,
                          const std::string& construct, const Where& at,
                          bool isWait, EvalContext& evalCtx) {
        std::string signal;
        if (expr && (expr->kind == ExpressionKind::NamedValue ||
                     expr->kind == ExpressionKind::HierarchicalValue)) {
            if (!relativePath(expr->as<ValueExpressionBase>().symbol, prefix,
                              signal)) {
                signal.clear();
                Ref r;
                r.origin = expr;   // no bits: an event names a whole signal
                addHierRef(false, r, kind, construct, at, evalCtx);
            }
        }
        return ProcEventRow{std::move(signal), edge, isWait, at.file, at.line};
    }

    /// One procedure's edges, paired statement by statement.
    /// `seen` is owned by the caller and spans the whole module: a generate
    /// loop gives each iteration its own AnalyzedProcedure, so a per-procedure
    /// set never sees that all of them assign the same module-level pair from
    /// the same line. Keyed on the line too, since two distinct statements
    /// driving one pair are two edges.
    void emitProcedure(const AnalyzedProcedure& proc, const std::string& prefix,
                       std::vector<EdgeRow>& out, SeenSet& seen, int64_t moduleId,
                       int64_t procIndex) {
        std::string kind, construct;
        classify(*proc.analyzedSymbol, kind, construct);
        // The procedure's own location, and the fallback for any statement
        // inside it that has none of its own.
        const Where procAt = locate(proc.analyzedSymbol->location);

        // Declared before the sensitivity pass, which needs it to resolve the
        // selects in an event expression that leaves the module.
        EvalContext evalCtx(*proc.analyzedSymbol);

        auto& sens = proc.getSensitivityList();

        // Sensitivity rows carry the procedure's own location; the waits the
        // walker reports below carry their statement's. Written together once
        // the walk is done.
        std::vector<ProcEventRow> events;
        {
            std::vector<std::pair<const Expression*, std::string>> raw;
            collectEdgeEvents(sens.timingControl, raw);
            for (auto& [expr, edge] : raw)
                events.push_back(eventRow(expr, edge, prefix, kind, construct,
                                          procAt, /*isWait=*/false, evalCtx));
        }

        // Input-port drivers belong to the parent, not to this module: the port
        // is receiving a value from outside, and reporting it here would name
        // the wrong side of the boundary.
        std::unordered_set<const ValueSymbol*> inputPorts;
        for (auto* d : proc.getDrivers()) {
            if (d->isInputPort())
                inputPorts.insert(&d->getSymbol());
        }

        // Whether the walk found anything at all, as distinct from whether a
        // row survived. Dedup spans the module, and a generate loop gives each
        // iteration its own analyzed procedure, so later iterations legitimately
        // add no rows -- counting appended rows would report every one of them
        // as a procedure that yielded nothing.
        bool reached = false;

        // A null-source edge is the truth only when *no* operand of the
        // statement had a name in this module. The emit callback sees one
        // operand at a time, so the decision waits until the walk is done:
        // `namedSource` records the targets some operand did reach,
        // `pendingNull` the rows to write for the ones none did.
        using StmtKey = std::tuple<const ValueSymbol*, std::string, uint32_t>;
        std::set<StmtKey> namedSource;
        std::map<StmtKey, EdgeRow> pendingNull;

        StatementWalker walker([&](const Ref& dst, const Ref& src, bool gatingEdge,
                                   SourceRange where) {
            reached = true;
            // Self-feedback is kept, same bits included. Following a *driver*
            // backwards, `cnt <= cnt + 1` adds nothing new -- which is why an
            // earlier version dropped it -- but the same row read the other way
            // answers "who reads cnt", and dropping it made the only reader of
            // a free-running counter disappear: a load query answered "nobody"
            // about a signal the simulator's own database reports as read.
            if (inputPorts.count(dst.sym))
                return;
            // The assignment's own location, not the procedure header's. Using
            // the header made every edge in a 200-line always block report the
            // `always` keyword, and -- because the dedup key includes the line
            // -- made two different statements driving the same pair look like
            // one, so the second was dropped. A control edge emitted early in a
            // block silently suppressed the real data edge later in it.
            //
            // The file travels with the line. Taking only the line and keeping
            // the procedure's file broke every statement that lives in another
            // file -- a task body reached by `include` -- into a pair naming a
            // line the file does not have.
            const Where at = locate(where.start(), procAt);
            const uint32_t stmtLine = at.line;
            const auto dk = keyRange(dst);
            const auto sk = src.sym ? keyRange(src) : std::make_pair<uint64_t, uint64_t>(0, 0);
            if (!seen.emplace(dst.sym, src.sym, fileKey(at.file), stmtLine,
                              dk.first, dk.second,
                              sk.first, sk.second)
                     .second)
                return;
            EdgeRow row;
            if (!relativePath(*dst.sym, prefix, row.dst)) {
                addHierRef(true, dst, kind, construct, at, evalCtx);
                // The operand too, gating conditions included -- those reach
                // here and nowhere else, so returning on the target alone lost
                // the only record of what an outward write depends on.
                if (src.sym)
                    addHierRef(false, src, kind, construct, at, evalCtx);
                return;
            }
            if (!dst.whole)
                row.dstBits = std::make_pair(dst.lo, dst.hi);
            row.dstExact = dst.exact;
            if (src.sym) {
                if (!relativePath(*src.sym, prefix, row.src)) {
                    addHierRef(false, src, kind, construct, at, evalCtx);
                    // The operand has no name this row can carry, but the
                    // statement may still drive the target from outside the
                    // module entirely -- `assign seen = bus.vld` in an
                    // interface-using design -- and dropping the row outright
                    // made that read as "nothing drives it".
                    //
                    // Held back rather than written here. Whether a null
                    // source is the truth depends on the statement's *other*
                    // operands, which this callback sees one at a time: an
                    // earlier version wrote the row per outward operand, so
                    // `assign y = a & bus.vld` claimed both that `a` drives y
                    // and that nothing does. It is materialised after the walk,
                    // and only for a target no named operand reached.
                    //
                    // Finished here rather than at the push site below, which
                    // this path never reaches: `relativePath` writes the
                    // *absolute* path into `row.src` when it fails, so leaving
                    // it would leak one instance's hierarchy into a folded row.
                    row.src.clear();
                    row.srcType.clear();
                    row.srcBits.reset();
                    row.srcExact = true;
                    row.dstType = typeOf(*dst.sym);
                    row.kind = kind;
                    row.construct = construct;
                    row.control = gatingEdge;
                    row.file = at.file;
                    row.line = at.line;
                    pendingNull.try_emplace({dst.sym, at.file, stmtLine}, row);
                    return;
                }
                else {
                    row.srcType = typeOf(*src.sym);
                    if (!src.whole)
                        row.srcBits = std::make_pair(src.lo, src.hi);
                    row.srcExact = src.exact;
                }
            }
            row.dstType = typeOf(*dst.sym);
            row.kind = kind;
            row.construct = construct;
            // Whether the operand reached the target through a condition rather
            // than through the right-hand side. Its own column: appending it to
            // `construct` made a consumer string-match a suffix to recover one of
            // two orthogonal facts.
            row.control = gatingEdge;
            row.file = at.file;
            row.line = at.line;
            if (src.sym)
                namedSource.emplace(dst.sym, at.file, at.line);
            out.push_back(std::move(row));
        },
        // One row per assignment statement, so a target written in several
        // places stays several statements rather than one merged set. Only what
        // is written: no branch conditions, because deciding which branch held
        // is the reader's job and encoding it here would state it as fact.
        [&](const Ref& dst, const std::vector<Ref>& operands, SourceRange where,
            int64_t stmtSeq, bool blocking, int64_t droppedConstants) {
            if (!dst.sym || inputPorts.count(dst.sym))
                return;
            AssignRow arow;
            const Where at = locate(where.start(), procAt);
            arow.line = at.line;
            if (!relativePath(*dst.sym, prefix, arow.dst)) {
                // The statement itself cannot be a row -- its target has no
                // module-relative name -- but the write is recorded where
                // every outward reference is, and so is everything it read.
                //
                // Returning before the operand loop threw the read side away
                // entirely, uncounted: `always_ff @(posedge clk) b.rdy <=
                // b.vld & b.ack;` -- a modport driver, which is what interface
                // RTL mostly is -- exported one write-only row, and the two
                // reads appeared in no table and in no total.
                addHierRef(true, dst, kind, construct, at, evalCtx);
                // The read side of the same statement. An operand outside the
                // module joins the write in hier_ref; one inside it has no
                // assignment row to hang from -- the target has no name -- so
                // it goes to stmt_read, without which `payload` in
                // `assign bus.vld = |payload;` read as though nothing used it.
                addStmtReads(operands, prefix, kind, construct, at, evalCtx);
                return;
            }
            if (!dst.whole)
                arow.dstBits = std::make_pair(dst.lo, dst.hi);
            arow.dstExact = dst.exact;
            arow.kind = kind;
            arow.construct = construct;
            arow.file = at.file;
            arow.proc = procIndex;
            arow.seq = stmtSeq;
            arow.blocking = (proc.analyzedSymbol->kind == SymbolKind::ContinuousAssign)
                                ? -1
                                : (blocking ? 1 : 0);

            std::vector<OperandRow> ops;
            for (auto& r : operands) {
                if (!r.sym)
                    continue;
                std::string rel;
                if (!relativePath(*r.sym, prefix, rel)) {
                    arow.dropped++;      // outside the module; cannot be shared
                    addHierRef(false, r, kind, construct, at, evalCtx);
                    continue;
                }
                OperandRow o;
                o.name = std::move(rel);
                if (!r.whole)
                    o.bits = std::make_pair(r.lo, r.hi);
                o.exact = r.exact;
                ops.push_back(std::move(o));
            }
            // Constants never reach here -- they are filtered during collection
            // -- so they are counted where that happens.
            arow.dropped += droppedConstants;
            writer.addAssignment(moduleId, arow, ops);
            stats.assignments++;
        },
        // A wait's event, at its own statement -- which a task body reached by
        // `include` puts in a different file from the procedure header.
        [&](const Expression* e, const std::string& edge, SourceRange where) {
            events.push_back(eventRow(e, edge, prefix, kind, construct,
                                      locate(where.start(), procAt),
                                      /*isWait=*/true, evalCtx));
        },
        // A statement that reads without writing anything nameable: an
        // assertion. Its reads belong to the statement, not to a target.
        [&](const std::vector<Ref>& operands, const std::string& what,
            SourceRange where) {
            const Where at = locate(where.start(), procAt);
            addStmtReads(operands, prefix, kind, what, at, evalCtx);
        },
        evalCtx);
        walker.sensitivityTiming = sens.timingControl;

        if (proc.analyzedSymbol->kind == SymbolKind::ProceduralBlock)
            proc.analyzedSymbol->as<ProceduralBlockSymbol>().getBody().visit(walker);
        else if (proc.analyzedSymbol->kind == SymbolKind::ContinuousAssign)
            proc.analyzedSymbol->as<ContinuousAssignSymbol>().getAssignment().visit(walker);

        // The statements whose every operand turned out to live outside this
        // module: the target is driven, and this row is what says so.
        for (auto& [key, row] : pendingNull) {
            if (namedSource.count(key))
                continue;
            auto& [dstSym, keyFile, keyLine] = key;
            const auto dk = std::make_pair(row.dstBits ? row.dstBits->first : uint64_t(0),
                                           row.dstBits ? row.dstBits->second : uint64_t(0));
            if (!seen.emplace(dstSym, nullptr, fileKey(keyFile), keyLine,
                              dk.first, dk.second, uint64_t(0), uint64_t(0))
                     .second)
                continue;
            out.push_back(row);
        }

        writer.addProcEvents(moduleId, procIndex, events);

        // A procedure the analysis says drives something, but in which the walk
        // found nothing, means the traversal did not see what the analysis did.
        //
        // Keyed on whether the walk *reached* anything rather than on rows
        // appended: dedup spans the module and a generate loop gives each
        // iteration its own analyzed procedure, so later iterations legitimately
        // add no rows. Counting appended rows reported 510 such procedures on
        // one SoC, every one of them a duplicate rather than a loss.
        //
        // It does not catch the case where slang rejects a statement so
        // thoroughly that it reports no drivers either: the node is marked bad,
        // the visitor skips bad nodes, a bad child taints its enclosing block,
        // and the whole `always` leaves the export. Nothing here can see it,
        // because the analysis and the traversal agree there is nothing.
        //
        // Only *invalid* RTL reaches that state -- a reversed slice on an
        // ascending unpacked array is the case found, and Verilator and Icarus
        // reject it too. Legal code that merely warns (a width truncation, a
        // legal ascending slice) keeps its block. So the diagnostic is the
        // signal, and since the one responsible is declared a *warning* rather
        // than an error, warnings are counted and surfaced instead of being
        // filtered out.
        if (!reached && !proc.getDrivers().empty()) {
            stats.emptyProcedures++;
        }
    }

    /// `wire w = a & b;` -- a net declared with an initialiser.
    ///
    /// The LRM makes that a continuous assignment, but slang models it as a
    /// net carrying an initialiser rather than as a `ContinuousAssign` symbol,
    /// so it never reached the procedure walk and the net came out with no
    /// driver at all. Nothing said so: `w` still appeared as a *source* in
    /// whatever read it, so the export looked complete while the question
    /// "what drives w" answered nothing.
    ///
    /// Recorded exactly as the equivalent `assign` is, because that is what it
    /// is -- `continuous_assign`/`assign`, with a NULL-source row when the
    /// initialiser reads nothing this module can name.
    ///
    /// Variable initialisers are deliberately not included: `logic [7:0] c = 0`
    /// writes once at time zero and is not a driver in the same sense, and
    /// recording it as one would put a continuous assignment on every counter
    /// that happens to declare its reset value.
    void emitNetInitialisers(const InstanceBodySymbol& body, const std::string& prefix,
                             std::vector<EdgeRow>& out, SeenSet& seen, int64_t moduleId) {
        EvalContext evalCtx(body);
        int64_t seq = 0;
        forEachDeclaration(body, [&](const Symbol& sym, const std::string& gen,
                                     std::optional<ArgumentDirection>) {
            if (sym.kind != SymbolKind::Net)
                return;
            auto& net = sym.as<NetSymbol>();
            const Expression* init = net.getInitializer();
            if (!init)
                return;
            const Where at = locate(net.location);

            std::vector<Ref> reads;
            filteredConstants = 0;
            collectRefs(*init, evalCtx, reads);
            const int64_t droppedConstants = filteredConstants;
            evalCtx.reset();

            const std::string dstName = gen + std::string(sym.name);

            AssignRow arow;
            arow.dst = dstName;
            arow.kind = "continuous_assign";
            arow.construct = "assign";
            arow.file = at.file;
            arow.line = at.line;
            arow.proc = -1;    // no procedure: the declaration is the statement
            arow.seq = seq++;
            arow.blocking = -1;
            std::vector<OperandRow> ops;
            for (auto& r : reads) {
                if (!r.sym)
                    continue;
                std::string rel;
                if (!relativePath(*r.sym, prefix, rel)) {
                    arow.dropped++;
                    addHierRef(false, r, "continuous_assign", "assign", at, evalCtx);
                    continue;
                }
                OperandRow o;
                o.name = std::move(rel);
                if (!r.whole)
                    o.bits = std::make_pair(r.lo, r.hi);
                o.exact = r.exact;
                ops.push_back(std::move(o));
            }
            arow.dropped += droppedConstants;
            writer.addAssignment(moduleId, arow, ops);
            stats.assignments++;

            EdgeRow base;
            base.dst = dstName;
            base.dstType = typeOf(net);
            base.kind = "continuous_assign";
            base.construct = "assign";
            base.file = at.file;
            base.line = at.line;

            bool anySource = false;
            for (auto& r : reads) {
                if (!r.sym)
                    continue;
                const auto sk = keyRange(r);
                if (!seen.emplace(&net, r.sym, fileKey(at.file), at.line,
                                  uint64_t(0), uint64_t(0),
                                  sk.first, sk.second)
                         .second) {
                    anySource = true;
                    continue;
                }
                EdgeRow row = base;
                if (!relativePath(*r.sym, prefix, row.src))
                    continue;   // outward: already in hier_ref, above
                row.srcType = typeOf(*r.sym);
                if (!r.whole)
                    row.srcBits = std::make_pair(r.lo, r.hi);
                row.srcExact = r.exact;
                anySource = true;
                out.push_back(std::move(row));
            }
            // `wire w = 1'b0;` drives w as surely as `assign w = 1'b0;` does.
            if (!anySource &&
                seen.emplace(&net, nullptr, fileKey(at.file), at.line,
                             uint64_t(0), uint64_t(0),
                             uint64_t(0), uint64_t(0))
                    .second) {
                out.push_back(std::move(base));
            }
        });
    }

    /// Gate, switch and UDP instances, as edges. `and (y, a, b)` is dataflow
    /// at its most literal, and it was entirely absent: the walk knew module
    /// instances only, so a netlist-style module exported empty and every
    /// gate-driven net answered "no driver". No new table -- a primitive is
    /// one edge per (input, output) pairing, with the construct naming the
    /// gate: `gate:and`, `gate:nmos`, `udp:my_latch`.
    void emitPrimitives(const InstanceBodySymbol& body, const std::string& prefix,
                        std::vector<EdgeRow>& out, SeenSet& seen) {
        EvalContext evalCtx(body);
        forEachPrimitive(body, [&](const PrimitiveInstanceSymbol& prim) {
            auto conns = prim.getPortConnections();
            if (conns.empty())
                return;
            auto& def = prim.primitiveType;
            const std::string construct =
                std::string(def.primitiveKind == PrimitiveSymbol::UserDefined
                                ? "udp:"
                                : "gate:") +
                std::string(def.name);
            const Where at = locate(prim.location);

            // A terminal's direction. The built-in gates are variadic -- the
            // definition's port list does not stretch to the instance's
            // terminal count -- and the LRM fixes their shape instead: an
            // n-input gate drives its first terminal, an n-output gate reads
            // its last, and everything else (switches, UDPs) declares one
            // direction per port.
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

            // Each reference remembers which terminal it came from. A
            // bidirectional terminal is both read and driven, and it must not
            // pair with *itself* -- but comparing symbols to detect that was
            // wrong twice over: `buf (sr[1], sr[0])` has one symbol on both
            // terminals and is ordinary dataflow, and the guard discarded it
            // and then, finding no input, claimed the gate drives `sr[1]` from
            // nothing. Every stage of a gate-level shift register came out
            // severed and mislabelled as a constant driver. The terminal index
            // is what "the same terminal" actually means.
            struct Term {
                Ref ref;
                size_t terminal;
            };
            std::vector<Term> reads, writes;
            auto take = [&](size_t i, std::vector<Term>& into, bool skipSelectors) {
                std::vector<Ref> refs;
                collectRefs(*conns[i], evalCtx, refs, skipSelectors);
                for (auto& r : refs)
                    into.push_back(Term{r, i});
            };
            for (size_t i = 0; i < n; i++) {
                if (!conns[i])
                    continue;
                switch (dirOf(i)) {
                    case PrimitivePortDirection::In:
                        take(i, reads, /*skipSelectors=*/false);
                        break;
                    case PrimitivePortDirection::InOut:
                        // A tran terminal conducts both ways: it is read and
                        // driven at once, so it lands in both sets and the
                        // pairing below emits both directions.
                        take(i, reads, /*skipSelectors=*/true);
                        take(i, writes, /*skipSelectors=*/true);
                        break;
                    default:    // Out, OutReg
                        take(i, writes, /*skipSelectors=*/true);
                        break;
                }
            }

            for (auto& dstTerm : writes) {
                const Ref& dst = dstTerm.ref;
                std::string dstRel;
                if (!relativePath(*dst.sym, prefix, dstRel)) {
                    addHierRef(true, dst, "primitive", construct, at, evalCtx);
                    for (auto& srcTerm : reads) {
                        if (srcTerm.terminal != dstTerm.terminal)
                            addHierRef(false, srcTerm.ref, "primitive", construct, at, evalCtx);
                    }
                    continue;
                }
                // The dst half of the row is the same for every source this
                // terminal pairs with, and the type text is a fresh heap
                // string out of slang's type printer -- on a netlist-style
                // module that is one allocation per (output, input) pair
                // where one per output does.
                EdgeRow base;
                base.dst = dstRel;
                base.dstType = typeOf(*dst.sym);
                if (!dst.whole)
                    base.dstBits = std::make_pair(dst.lo, dst.hi);
                base.dstExact = dst.exact;
                base.kind = "primitive";
                base.construct = construct;
                base.file = at.file;
                base.line = at.line;

                const auto dk = keyRange(dst);
                // Whether this terminal has any input at all, which decides
                // the null-source row below. Set only where a row is actually
                // written: counting inputs that then turn out to live outside
                // the module left a gate driven entirely by hierarchical
                // references with no driver row of any kind.
                bool anyInput = false;
                for (auto& srcTerm : reads) {
                    // One terminal does not feed itself. A tran's two ends are
                    // both read and driven, and pairing an end with itself
                    // would fabricate dataflow out of a single wire.
                    if (srcTerm.terminal == dstTerm.terminal)
                        continue;
                    const Ref& src = srcTerm.ref;
                    const auto sk = keyRange(src);
                    if (!seen.emplace(dst.sym, src.sym, fileKey(at.file), at.line,
                                      dk.first, dk.second,
                                      sk.first, sk.second)
                             .second) {
                        anyInput = true;   // already recorded, still an input
                        continue;
                    }
                    EdgeRow row = base;
                    if (!relativePath(*src.sym, prefix, row.src)) {
                        addHierRef(false, src, "primitive", construct, at, evalCtx);
                        continue;
                    }
                    row.srcType = typeOf(*src.sym);
                    if (!src.whole)
                        row.srcBits = std::make_pair(src.lo, src.hi);
                    row.srcExact = src.exact;
                    anyInput = true;
                    out.push_back(std::move(row));
                }
                // pullup(y) has no input terminal, and a gate whose inputs all
                // live outside the module has none this row can name. Either
                // way the null-source row names the gate as the driver,
                // exactly as `q <= 8'h0` is named; without it the net reads as
                // undriven.
                if (!anyInput &&
                    seen.emplace(dst.sym, nullptr, fileKey(at.file), at.line,
                                 dk.first, dk.second,
                                 uint64_t(0), uint64_t(0))
                        .second) {
                    out.push_back(std::move(base));
                }
            }
        });
    }

    /// One child instance's port connections, in the parent's namespace.
    ///
    /// A connection expression is not always a plain net: a concatenation or a
    /// slice ties several parent nets to one formal. One row per (formal, net)
    /// pair keeps that honest rather than picking whichever net happens to be
    /// first, which would silently drop the rest of a bus.
    /// The single internal signal a formal stands for, when the connection
    /// names it something else -- `.ext_one(inner)`. Empty when the formal and
    /// the internal signal share a name, which is the ordinary case, and empty
    /// when the formal covers several signals (`.ext_pair({hi, lo})`), where
    /// slang gives each member its own port and there is no one answer.
    static std::string internalName(const PortSymbol& port) {
        const Symbol* sym = port.internalSymbol;
        if (!sym) {
            auto* inner = port.getInternalExpr();
            if (!inner)
                return {};
            if (inner->kind != ExpressionKind::NamedValue &&
                inner->kind != ExpressionKind::HierarchicalValue)
                return {};
            sym = &inner->as<ValueExpressionBase>().symbol;
        }
        if (sym->name.empty() || sym->name == port.name)
            return {};
        return std::string(sym->name);
    }

    /// The module's own interface port that `iface` arrived through, if it is
    /// one. That is what a pass-through connection names, and the only spelling
    /// of it that is true for every instance of the module.
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

    void emitPorts(const InstanceSymbol& child, const std::string& childName,
                   const std::string& prefix, std::vector<PortRow>& out,
                   const InstanceBodySymbol& parentBody) {
        // An element of an instance array shares the array's connection
        // expression: `foo u [63:0] (.z(bus64))` gives every element the whole
        // 64-bit bus, while each element's formal is one bit. Recording that as
        // the connection width makes each element look width-mismatched.
        // Measured before this check: 1011 "mismatches" on one SoC, every one of
        // them an array element and none of them real. Left NULL instead, so the
        // comparison is simply not offered where it has no meaning.
        bool inArray = false;
        if (auto* ps = child.getParentScope())
            inArray = ps->asSymbol().kind == SymbolKind::InstanceArray;
        // For evaluating the constant selects inside connection expressions.
        EvalContext evalCtx(child);
        // The instantiation's own location. It is the fallback, not the answer:
        // a connection is written where *it* is written.
        //
        // An earlier version resolved this once per instance on the grounds
        // that it is loop-invariant, which is true of the instantiation and
        // false of the thing being recorded. Any instance wide enough to
        // matter is written one port per line, so every connection reported
        // the line of the `foo u_foo (` above it: a driver query pointed at
        // the instantiation instead of at `.wb_rst_i(~rst_n)`, reading the
        // text back gave the instantiation header, and no two ports of one
        // instance could be told apart by position at all. On one SoC that
        // was every multi-port instance without exception.
        const Where instAt = locate(child.location);
        for (auto* conn : child.getPortConnections()) {
            if (!conn)
                continue;
            // Where this connection is written. The expression is the only
            // part of it slang keeps a location for; a port left unconnected
            // has none, and falls back to the instantiation.
            const Expression* connExpr = conn->getExpression();
            const Where at = connExpr ? locate(connExpr->sourceRange.start(), instAt)
                                      : instAt;
            const std::string& file = at.file;
            const uint32_t line = at.line;

            // An interface port carries no net, but the *binding* is the alias
            // that makes `child.bus.*` resolvable at all: the signals live in
            // the interface instance on the parent side, and without this row
            // they can be reached from neither direction. `.dbg` needs its
            // simulated net table to derive the same fact; here it is one row.
            if (conn->port.kind == SymbolKind::InterfacePort) {
                auto& ip = conn->port.as<InterfacePortSymbol>();
                auto [ifaceSym, modport] = conn->getIfaceConn();
                PortRow row;
                row.child = childName;
                row.port = std::string(ip.name);
                row.conn = PortConn::Interface;
                if (ip.interfaceDef)
                    row.outerType = std::string(ip.interfaceDef->name);
                // The modport in force: the one the connection names, else the
                // one the port declares.
                if (modport)
                    row.modport = std::string(modport->name);
                else if (!ip.modport.empty())
                    row.modport = std::string(ip.modport);
                if (ifaceSym) {
                    // The instance in the parent's namespace -- or, when the
                    // parent is passing its *own* interface port down, that
                    // port's name.
                    //
                    // The second case needs looking for. `getIfaceConn` answers
                    // with the elaborated interface instance, which for a
                    // pass-through lives above the parent and so has no
                    // module-relative path; the row came out with a NULL outer
                    // and the alias it exists to record was missing from both
                    // sides. The parent's own port is what was written, and it
                    // is the one spelling every instance of the parent shares.
                    std::string outerRel;
                    if (relativePath(*ifaceSym, prefix, outerRel)) {
                        row.outer = std::move(outerRel);
                    }
                    else if (auto* through = passedThrough(parentBody, ifaceSym)) {
                        row.outer = std::string(through->name);
                    }
                    else {
                        // An array, either whole (`.b(arr)`) or indexed
                        // (`.b(b[0])` out of the parent's own array port).
                        // slang answers both with a synthesized
                        // `InstanceArraySymbol` that is not a member of any
                        // scope, so it has no path to be made relative to and
                        // the row came out with no outer side at all -- which
                        // is every element of every interface array.
                        //
                        // What was written is the answer, and it is already
                        // module-relative: `arr`, `b[0]`. The index is part of
                        // the name here, not a bit range, so it stays.
                        row.outer = normalizedText(conn->getExpression(), sourceManager,
                                                   /*stripTrailingSelect=*/false);
                        // slang's connection expression stops at the port name
                        // -- the element the connection selects is resolved
                        // into the symbol instead -- so `.b(b[0])` and
                        // `.b(b[1])` both read as `b`, and the two elements
                        // became one place. The index comes back from the
                        // symbol's own path, which is where slang renders it
                        // in source numbering already.
                        row.outer += arrayIndexSuffix(ifaceSym->getHierarchicalPath());
                        if (row.outer.empty())
                            stats.external++;
                    }
                }
                row.file = file;
                row.line = line;
                out.push_back(std::move(row));
                continue;
            }
            if (conn->port.kind != SymbolKind::Port)
                continue;
            auto& port = conn->port.as<PortSymbol>();
            const Expression* expr = connExpr;

            std::string dir;
            switch (port.direction) {
                case ArgumentDirection::In:    dir = "in";    break;
                case ArgumentDirection::Out:   dir = "out";   break;
                case ArgumentDirection::InOut: dir = "inout"; break;
                default:                       dir = "ref";   break;
            }

            // The signal inside the child, when the connection names the
            // formal something else. `module m(.ext_one(inner))` is `ext_one`
            // out here and `inner` in every row the child's own module owns,
            // so without this a consumer holding the internal name had no way
            // back to the binding.
            const std::string innerName = internalName(port);

            if (!expr) {
                // Left unconnected. Recorded rather than skipped: a floating
                // output is a bug worth finding, and omitting the row would make
                // "nobody connected it" indistinguishable from "the exporter did
                // not get that far".
                PortRow row;
                row.child = childName;
                row.port = std::string(port.name);
                row.inner = innerName;
                row.direction = dir;
                row.conn = PortConn::Unconnected;
                row.file = file;
                row.line = line;
                out.push_back(std::move(row));
                continue;
            }

            // The width as *written*, looking through the implicit conversion
            // slang inserts to make the connection fit the formal. Without
            // that, this is always the port's own width and a mismatch can
            // never be seen -- a column that always agrees detects nothing.
            const Expression* widthExpr = expr;
            while (widthExpr->kind == ExpressionKind::Conversion &&
                   widthExpr->as<ConversionExpression>().isImplicit()) {
                widthExpr = &widthExpr->as<ConversionExpression>().operand();
            }
            const int64_t exprWidth =
                (!inArray && widthExpr->type && widthExpr->type->isIntegral())
                    ? static_cast<int64_t>(widthExpr->type->getBitWidth())
                    : -1;

            // The nets the connection attaches to, without the selectors used
            // to pick them: `.ready_i(readies[sel])` attaches `readies`, and
            // `sel` is read to choose an element rather than wired to the port.
            // The assignment path already separates the two; this one did not,
            // and reported the index signal as connected.
            std::vector<ConnRef> nets;
            collectConnRefs(*expr, evalCtx, nets);
            if (nets.empty()) {
                // Tied off: the connection is a literal, a parameter, or an
                // enum member, so there is no net on the outside. The
                // connection still exists and a reader looking for why a port
                // never moves wants to see it, so it is recorded with a null
                // `outer` rather than dropped.
                PortRow row;
                row.child = childName;
                row.port = std::string(port.name);
                row.inner = innerName;
                row.direction = dir;
                row.conn = PortConn::Constant;
                row.outerWidth = exprWidth;
                row.file = file;
                row.line = line;
                out.push_back(std::move(row));
                continue;
            }
            // Deduplicated on what will be stored: the same net attached twice
            // with different bits (`.z({x[7:4], x[3:0]})`) is two attachments,
            // not one.
            std::set<std::tuple<const ValueSymbol*, uint64_t, uint64_t, bool, bool>>
                unique;
            for (auto& cn : nets) {
                const Ref& r = cn.ref;
                const uint64_t klo = r.whole ? 0 : r.lo;
                const uint64_t khi = r.whole ? 0 : r.hi;
                if (!unique.emplace(r.sym, klo, khi, r.whole, cn.expression).second)
                    continue;
                PortRow row;
                row.child = childName;
                row.port = std::string(port.name);
                row.inner = innerName;
                row.direction = dir;
                std::string outerRel;
                if (!relativePath(*r.sym, prefix, outerRel)) {
                    // Tied to a signal outside this module. An output drives
                    // it, an input samples it; inout is recorded as the write,
                    // being the direction a trace cannot rediscover.
                    const bool drives = port.direction == ArgumentDirection::Out ||
                                        port.direction == ArgumentDirection::InOut;
                    addHierRef(drives, r, "port", dir, at, evalCtx);
                    // And a row saying the port *is* attached, with no `outer`
                    // because the net has no name here. Skipping it entirely
                    // made `.a(tb.glob)` indistinguishable from a port nobody
                    // connected -- the same ambiguity the unconnected row above
                    // exists to remove, reintroduced one branch later. What it
                    // is tied to is in `hier_ref` at this file and line.
                    // An operand of an expression stays kind 3 even when it
                    // has no name here. Overwriting it with 5 made
                    // `.a(tb.glob & b)` look like `.a(tb.glob)` -- an alias --
                    // so every reader of `a` was attributed to `tb.glob`,
                    // which is the misattribution kind 3 exists to prevent.
                    // Kind 5 is for a whole connection that happens to be
                    // unnameable; what it is tied to is in `hier_ref` either
                    // way.
                    row.conn = cn.expression ? PortConn::Expression
                                             : PortConn::External;
                    row.outerWidth = exprWidth;
                    row.file = file;
                    row.line = line;
                    out.push_back(std::move(row));
                    continue;
                }
                row.outer = std::move(outerRel);
                row.outerType = typeOf(*r.sym);
                row.outerWidth = exprWidth;
                // An element of an instance array shares the whole array's
                // connection expression, so its bits describe the array's tie
                // rather than this element's slice of it. NULL with exact=0 --
                // somewhere in the object -- is the honest reading, exactly as
                // with outer_width above.
                if (inArray) {
                    row.outerExact = false;
                }
                else {
                    if (!r.whole)
                        row.outerBits = std::make_pair(r.lo, r.hi);
                    row.outerExact = r.exact;
                }
                row.conn = cn.expression ? PortConn::Expression : PortConn::Net;
                row.file = file;
                row.line = line;
                out.push_back(std::move(row));
            }
        }
    }

    /// One net a connection expression attaches: the base symbol, the bits the
    /// selector chain picks, and whether it was reached structurally or only
    /// read inside a wider expression.
    struct ConnRef {
        Ref ref;
        bool expression = false;
    };

    /// The value symbols a connection expression attaches to, each with its
    /// bit range.
    ///
    /// A structural connection -- a name, a select, a concatenation of those --
    /// attaches its nets directly, and the selector *reads* (`readies[sel]`
    /// reading `sel`) are not connections and are skipped. Anything else is an
    /// expression: there is no net behind `.en(state == RUN)`, only signals the
    /// expression samples, so those come back flagged, selector reads included,
    /// for the consumer to treat as operands rather than wires.
    static void collectConnRefs(const Expression& expr, EvalContext& ctx,
                                std::vector<ConnRef>& out) {
        switch (expr.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue:
            case ExpressionKind::ElementSelect:
            case ExpressionKind::RangeSelect:
            case ExpressionKind::MemberAccess: {
                // One structural leaf: slang's path analysis resolves the base
                // symbol and the bits its static selects pick, which is what
                // `.idx(stim[3:0])` means -- bits 0..3 of stim, not stim.
                std::vector<Ref> refs;
                collectRefs(expr, ctx, refs, /*skipSelectors=*/true);
                // Reached only by descending structural nodes, so never an
                // expression operand: the branch that produces those does not
                // recurse.
                for (auto& r : refs)
                    out.push_back({r, /*expression=*/false});
                return;
            }
            case ExpressionKind::Concatenation:
                for (auto* op : expr.as<ConcatenationExpression>().operands())
                    collectConnRefs(*op, ctx, out);
                return;
            case ExpressionKind::Replication:
                // `{2{two}}` wires `two` into the formal exactly as
                // `{two, two}` does. Without this case it fell through to the
                // expression branch and the same netlist got kind 3 from one
                // spelling and kind 0 from the other -- opposite advice to the
                // consumer about whether the port aliases the net. The count
                // is a constant and reads nothing.
                collectConnRefs(expr.as<ReplicationExpression>().concat(), ctx, out);
                return;
            case ExpressionKind::Conversion:
                collectConnRefs(expr.as<ConversionExpression>().operand(), ctx, out);
                return;
            case ExpressionKind::Assignment:
                // An output or inout connection arrives wrapped in the
                // assignment `bindLValue` builds around it, with an empty
                // placeholder on the right; the lvalue is the connection.
                // Without this case `.y(gy)` fell through to the expression
                // branch and the plainest wire in the design read as an
                // operand.
                collectConnRefs(expr.as<AssignmentExpression>().left(), ctx, out);
                return;
            default: {
                std::vector<Ref> refs;
                collectRefs(expr, ctx, refs);
                // visitPaths stops at a call's arguments; the subroutine's own
                // free reads still reach the port, so they are appended the way
                // the assignment path appends them -- whole-object, since their
                // bounds belong to expressions inside the callee.
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
                for (auto& r : refs)
                    out.push_back({r, true});
                return;
            }
        }
    }

    /// The instance tree: one row per elaborated instance. This is the only
    /// table that scales with the design rather than with the source, so it
    /// carries nothing but identity.
    void visitInstance(const InstanceSymbol& inst, int64_t parentRow,
                       const std::string& parentPrefix) {
        int64_t moduleId = moduleIdFor(inst);
        int64_t rowId = ++instanceRow;
        // The name relative to the parent instance, so an instance inside a
        // generate block keeps the block in its name (`g_lane[3].u_dp`) and a
        // path still rejoins by concatenating with '.'.
        std::string name;
        if (parentPrefix.empty() || !relativePath(inst, parentPrefix, name))
            name = std::string(inst.name);
        if (!seenSiblings.insert({parentRow, name}).second)
            stats.duplicatePaths++;
        writer.addInstance(name, moduleId, parentRow, rowId);
        stats.instances++;

        const std::string prefix = inst.getHierarchicalPath();
        forEachInstance(inst.body, [&](const InstanceSymbol& child) {
            visitInstance(child, rowId, prefix);
        });
    }

    Compilation& compilation;
    AnalysisManager& analysis;
    Writer& writer;
    const SourceManager& sourceManager;
    std::map<std::string, Group> groups;
    std::unordered_map<const InstanceSymbol*, std::string> instanceGroup;
    std::set<std::pair<int64_t, std::string>> seenSiblings;
    // The module being emitted accumulates its outward references here; one
    // statement can surface the same reference through several collection
    // passes, so the seen-set folds them to one row per (text, rw, line).
    std::vector<HierRefRow> hierRefs;
    std::vector<StmtReadRow> stmtReads;
    /// The reference expressions already recorded for this module, by node.
    /// Not by text: two references can share a spelling and be different
    /// references, which is what a generate loop does to `b[g].sig`.
    std::set<std::pair<const Expression*, bool>> hierSeen;
    std::unordered_map<std::string, uint32_t> fileKeyIds;
    // As-written file spelling -> the absolute path its buffer came from.
    std::unordered_map<std::string, std::string> fileOrigins;
    int64_t instanceRow = 0;
    Stats stats;
};

} // namespace

Stats extract(Compilation& compilation, AnalysisManager& analysis, Writer& writer) {
    Walker walker(compilation, analysis, writer);
    return walker.run();
}

} // namespace designdb
