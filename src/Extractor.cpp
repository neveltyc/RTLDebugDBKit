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

/// `file` and `line` for a symbol, resolved through the source manager.
void locationOf(const Symbol& sym, const SourceManager& sm, std::string& file,
                uint32_t& line) {
    auto loc = sym.location;
    if (!loc)
        return;
    file = std::string(sm.getFileName(loc));
    line = sm.getLineNumber(loc);
}

/// The source text of an expression, exactly as written. Empty when it cannot
/// be recovered whole -- a range that spans buffers, or one outside its
/// buffer's bounds -- in which case the caller records nothing rather than a
/// fragment.
std::string sourceTextOf(const Expression* e, const SourceManager& sm) {
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
    return std::string(text.substr(a, b - a));
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
        out.push_back(r);
    }
};

void collectStatementRefs(const Statement& stmt, std::vector<Ref>& out) {
    StatementRefCollector c(out);
    stmt.visit(c);
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
    using EmitEvent = std::function<void(const Expression* expr, const std::string& edge,
                                         SourceRange where)>;

    Emit emit;
    EmitAssign emitAssign;
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
                    EvalContext& eval) :
        emit(std::move(emit)), emitAssign(std::move(emitAssign)),
        emitEvent(std::move(emitEvent)), eval(eval) {}

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
            for (auto& src : gating)
                emit(dst, src, true, expr.sourceRange);
            emit(dst, dst, false, expr.sourceRange);
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
template<typename F>
void forEachInstance(const Scope& scope, F&& fn) {
    for (auto& member : scope.members()) {
        switch (member.kind) {
            case SymbolKind::Instance:
                fn(member.as<InstanceSymbol>());
                break;
            case SymbolKind::GenerateBlock: {
                auto& block = member.as<GenerateBlockSymbol>();
                // A branch this parameterisation did not take: its contents are
                // present in the AST but are not part of the elaborated design.
                if (block.isUninstantiated)
                    break;
                forEachInstance(block, fn);
                break;
            }
            case SymbolKind::GenerateBlockArray:
                for (auto& entry : member.as<GenerateBlockArraySymbol>().entries)
                    forEachInstance(*entry, fn);
                break;
            case SymbolKind::InstanceArray:
                // `foo u [3:0] (...)` is an InstanceArraySymbol wrapping the
                // elements. Missing this case makes the array and its whole
                // subtree invisible, with no error to say so.
                forEachInstance(member.as<InstanceArraySymbol>(), fn);
                break;
            default:
                break;
        }
    }
}

/// Calls `fn` for every primitive instance directly inside `scope` -- a gate,
/// a switch, a UDP -- descending generate blocks and instance arrays exactly
/// as `forEachInstance` does, and for the same reason: `and g [3:0] (...)`
/// inside a generate loop is the ordinary spelling of replicated logic.
template<typename F>
void forEachPrimitive(const Scope& scope, F&& fn) {
    for (auto& member : scope.members()) {
        switch (member.kind) {
            case SymbolKind::PrimitiveInstance:
                fn(member.as<PrimitiveInstanceSymbol>());
                break;
            case SymbolKind::GenerateBlock: {
                auto& block = member.as<GenerateBlockSymbol>();
                if (block.isUninstantiated)
                    break;
                forEachPrimitive(block, fn);
                break;
            }
            case SymbolKind::GenerateBlockArray:
                for (auto& entry : member.as<GenerateBlockArraySymbol>().entries)
                    forEachPrimitive(*entry, fn);
                break;
            case SymbolKind::InstanceArray:
                forEachPrimitive(member.as<InstanceArraySymbol>(), fn);
                break;
            default:
                break;
        }
    }
}

/// (target, source, line) triples already emitted for one module.
/// (target, source, line, target bits, source bits) already emitted for one
/// module. The bit range belongs in the key: two part-selects of one signal
/// assigned from one source on one line are two edges, and without it the
/// second is discarded.
using SeenSet = std::set<std::tuple<const ValueSymbol*, const ValueSymbol*, uint32_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t>>;

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

    /// Records one reference that leaves the module, as written. Every such
    /// reference also bumps the external counter, which is where all of them
    /// lived before hier_ref existed; the row is added only when the text is
    /// recoverable and actually is a path -- a bare name that leaves the
    /// module (a subroutine's package-level free variable) resolves against
    /// imports this table cannot see, so it stays a count.
    void addHierRef(const Expression* origin, bool isWrite, const Ref& r,
                    const std::string& kind, const std::string& construct,
                    const std::string& file, uint32_t line) {
        std::string text = sourceTextOf(origin, sourceManager);
        if (text.empty() || (text.find('.') == std::string::npos &&
                             text.find("::") == std::string::npos)) {
            stats.external++;
            return;
        }
        // One statement surfaces the same reference through several collection
        // passes (the edge, the assignment's operand list); counted once.
        if (!hierSeen.emplace(text, isWrite, line).second)
            return;
        stats.external++;
        HierRefRow row;
        row.path = std::move(text);
        row.write = isWrite;
        row.kind = kind;
        row.construct = construct;
        row.file = file;
        row.line = line;
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
            locationOf(sym, sourceManager, row.file, row.line);
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
            emitPorts(child, childName, prefix, ports);
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
    }

    /// One event as a proc_event row: named module-relative where it can be,
    /// recorded with an empty signal where it cannot -- the event being an
    /// edge is a fact even where its source is not nameable. An external
    /// event signal (`@(posedge tb.clk)`) also lands in hier_ref.
    ProcEventRow eventRow(const Expression* expr, const std::string& edge,
                          const std::string& prefix, const std::string& kind,
                          const std::string& construct, const std::string& file,
                          uint32_t line) {
        std::string signal;
        if (expr && (expr->kind == ExpressionKind::NamedValue ||
                     expr->kind == ExpressionKind::HierarchicalValue)) {
            if (!relativePath(expr->as<ValueExpressionBase>().symbol, prefix,
                              signal)) {
                signal.clear();
                addHierRef(expr, false, Ref{}, kind, construct, file, line);
            }
        }
        return ProcEventRow{std::move(signal), edge, file, line};
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
        std::string kind, construct, file;
        uint32_t line = 0;
        classify(*proc.analyzedSymbol, kind, construct);
        locationOf(*proc.analyzedSymbol, sourceManager, file, line);

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
                                          file, line));
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

        EvalContext evalCtx(*proc.analyzedSymbol);
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
            // The assignment's own line, not the procedure header's. Using the
            // header made every edge in a 200-line always block report the
            // `always` keyword, and -- because the dedup key includes the line
            // -- made two different statements driving the same pair look like
            // one, so the second was dropped. A control edge emitted early in a
            // block silently suppressed the real data edge later in it.
            const uint32_t stmtLine = where.start()
                                          ? sourceManager.getLineNumber(where.start())
                                          : line;
            // Keyed on the range as it will be *stored*, not as computed. A
            // root whose width is unknown always serialises as whole, but its
            // raw bounds differ per element, so keying on those stops the key
            // collapsing rows that come out byte-identical -- four identical
            // rows for `mem[g] <= a` across a four-iteration generate loop.
            auto keyRange = [](const Ref& r) {
                return r.whole ? std::make_pair<uint64_t, uint64_t>(0, 0)
                               : std::make_pair(r.lo, r.hi);
            };
            const auto dk = keyRange(dst);
            const auto sk = src.sym ? keyRange(src) : std::make_pair<uint64_t, uint64_t>(0, 0);
            if (!seen.emplace(dst.sym, src.sym, stmtLine, dk.first, dk.second,
                              sk.first, sk.second)
                     .second)
                return;
            EdgeRow row;
            if (!relativePath(*dst.sym, prefix, row.dst)) {
                addHierRef(dst.origin, true, dst, kind, construct, file, stmtLine);
                return;
            }
            if (!dst.whole)
                row.dstBits = std::make_pair(dst.lo, dst.hi);
            row.dstExact = dst.exact;
            if (src.sym) {
                if (!relativePath(*src.sym, prefix, row.src)) {
                    addHierRef(src.origin, false, src, kind, construct, file,
                               stmtLine);
                    return;
                }
                row.srcType = typeOf(*src.sym);
                if (!src.whole)
                    row.srcBits = std::make_pair(src.lo, src.hi);
                row.srcExact = src.exact;
            }
            row.dstType = typeOf(*dst.sym);
            row.kind = kind;
            row.construct = construct;
            // Whether the operand reached the target through a condition rather
            // than through the right-hand side. Its own column: appending it to
            // `construct` made a consumer string-match a suffix to recover one of
            // two orthogonal facts.
            row.control = gatingEdge;
            row.file = file;
            row.line = stmtLine;
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
            arow.line = where.start() ? sourceManager.getLineNumber(where.start()) : line;
            if (!relativePath(*dst.sym, prefix, arow.dst)) {
                // The statement itself cannot be a row -- its target has no
                // module-relative name -- but the write is recorded where
                // every outward reference is.
                addHierRef(dst.origin, true, dst, kind, construct, file, arow.line);
                return;
            }
            if (!dst.whole)
                arow.dstBits = std::make_pair(dst.lo, dst.hi);
            arow.dstExact = dst.exact;
            arow.kind = kind;
            arow.construct = construct;
            arow.file = file;
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
                    addHierRef(r.origin, false, r, kind, construct, file,
                               arow.line);
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
        // A wait's event, at its statement's line.
        [&](const Expression* e, const std::string& edge, SourceRange where) {
            const uint32_t stmtLine = where.start()
                                          ? sourceManager.getLineNumber(where.start())
                                          : line;
            events.push_back(eventRow(e, edge, prefix, kind, construct, file,
                                      stmtLine));
        },
        evalCtx);
        walker.sensitivityTiming = sens.timingControl;

        if (proc.analyzedSymbol->kind == SymbolKind::ProceduralBlock)
            proc.analyzedSymbol->as<ProceduralBlockSymbol>().getBody().visit(walker);
        else if (proc.analyzedSymbol->kind == SymbolKind::ContinuousAssign)
            proc.analyzedSymbol->as<ContinuousAssignSymbol>().getAssignment().visit(walker);

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
            std::string file;
            uint32_t line = 0;
            locationOf(prim, sourceManager, file, line);

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

            std::vector<Ref> reads, writes;
            for (size_t i = 0; i < n; i++) {
                if (!conns[i])
                    continue;
                switch (dirOf(i)) {
                    case PrimitivePortDirection::In:
                        collectRefs(*conns[i], evalCtx, reads);
                        break;
                    case PrimitivePortDirection::InOut: {
                        // A tran terminal conducts both ways: it is read and
                        // driven at once, so it lands in both sets and the
                        // pairing below emits both directions.
                        std::vector<Ref> refs;
                        collectRefs(*conns[i], evalCtx, refs, /*skipSelectors=*/true);
                        reads.insert(reads.end(), refs.begin(), refs.end());
                        writes.insert(writes.end(), refs.begin(), refs.end());
                        break;
                    }
                    default:    // Out, OutReg
                        collectRefs(*conns[i], evalCtx, writes, /*skipSelectors=*/true);
                        break;
                }
            }

            auto keyRange = [](const Ref& r) {
                return r.whole ? std::make_pair<uint64_t, uint64_t>(0, 0)
                               : std::make_pair(r.lo, r.hi);
            };
            for (auto& dst : writes) {
                std::string dstRel;
                if (!relativePath(*dst.sym, prefix, dstRel)) {
                    addHierRef(dst.origin, true, dst, "primitive", construct,
                               file, line);
                    continue;
                }
                bool anyInput = false;
                for (auto& src : reads) {
                    // A bidirectional terminal appears in both sets; pairing
                    // it with itself would fabricate self-feedback out of one
                    // terminal, which is wiring, not dataflow.
                    if (src.sym == dst.sym)
                        continue;
                    anyInput = true;
                    const auto dk = keyRange(dst);
                    const auto sk = keyRange(src);
                    if (!seen.emplace(dst.sym, src.sym, line, dk.first, dk.second,
                                      sk.first, sk.second)
                             .second)
                        continue;
                    EdgeRow row;
                    row.dst = dstRel;
                    row.dstType = typeOf(*dst.sym);
                    if (!dst.whole)
                        row.dstBits = std::make_pair(dst.lo, dst.hi);
                    row.dstExact = dst.exact;
                    if (!relativePath(*src.sym, prefix, row.src)) {
                        addHierRef(src.origin, false, src, "primitive",
                                   construct, file, line);
                        continue;
                    }
                    row.srcType = typeOf(*src.sym);
                    if (!src.whole)
                        row.srcBits = std::make_pair(src.lo, src.hi);
                    row.srcExact = src.exact;
                    row.kind = "primitive";
                    row.construct = construct;
                    row.file = file;
                    row.line = line;
                    out.push_back(std::move(row));
                }
                // pullup(y) has no input terminal; the null-source row names
                // the gate as the driver, exactly as `q <= 8'h0` is named.
                if (!anyInput) {
                    const auto dk = keyRange(dst);
                    if (!seen.emplace(dst.sym, nullptr, line, dk.first, dk.second,
                                      uint64_t(0), uint64_t(0))
                             .second)
                        continue;
                    EdgeRow row;
                    row.dst = dstRel;
                    row.dstType = typeOf(*dst.sym);
                    if (!dst.whole)
                        row.dstBits = std::make_pair(dst.lo, dst.hi);
                    row.dstExact = dst.exact;
                    row.kind = "primitive";
                    row.construct = construct;
                    row.file = file;
                    row.line = line;
                    out.push_back(std::move(row));
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
    void emitPorts(const InstanceSymbol& child, const std::string& childName,
                   const std::string& prefix, std::vector<PortRow>& out) {
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
        for (auto* conn : child.getPortConnections()) {
            if (!conn)
                continue;
            std::string file;
            uint32_t line = 0;
            locationOf(child, sourceManager, file, line);

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
                    // The instance (or the parent's own interface port, passed
                    // straight through) in the parent's namespace.
                    std::string outerRel;
                    if (relativePath(*ifaceSym, prefix, outerRel))
                        row.outer = std::move(outerRel);
                    else
                        stats.external++;
                }
                row.file = file;
                row.line = line;
                out.push_back(std::move(row));
                continue;
            }
            if (conn->port.kind != SymbolKind::Port)
                continue;
            auto& port = conn->port.as<PortSymbol>();

            const Expression* expr = conn->getExpression();

            std::string dir;
            switch (port.direction) {
                case ArgumentDirection::In:    dir = "in";    break;
                case ArgumentDirection::Out:   dir = "out";   break;
                case ArgumentDirection::InOut: dir = "inout"; break;
                default:                       dir = "ref";   break;
            }

            if (!expr) {
                // Left unconnected. Recorded rather than skipped: a floating
                // output is a bug worth finding, and omitting the row would make
                // "nobody connected it" indistinguishable from "the exporter did
                // not get that far".
                PortRow row;
                row.child = childName;
                row.port = std::string(port.name);
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
                row.direction = dir;
                std::string outerRel;
                if (!relativePath(*r.sym, prefix, outerRel)) {
                    // Tied to a signal outside this module. An output drives
                    // it, an input samples it; inout is recorded as the write,
                    // being the direction a trace cannot rediscover.
                    const bool drives = port.direction == ArgumentDirection::Out ||
                                        port.direction == ArgumentDirection::InOut;
                    addHierRef(r.origin, drives, r, "port", dir, file, line);
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
                                std::vector<ConnRef>& out, bool inExpression = false) {
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
                for (auto& r : refs)
                    out.push_back({r, inExpression});
                return;
            }
            case ExpressionKind::Concatenation:
                for (auto* op : expr.as<ConcatenationExpression>().operands())
                    collectConnRefs(*op, ctx, out, inExpression);
                return;
            case ExpressionKind::Conversion:
                collectConnRefs(expr.as<ConversionExpression>().operand(), ctx, out,
                                inExpression);
                return;
            case ExpressionKind::Assignment:
                // An output or inout connection arrives wrapped in the
                // assignment `bindLValue` builds around it, with an empty
                // placeholder on the right; the lvalue is the connection.
                // Without this case `.y(gy)` fell through to the expression
                // branch and the plainest wire in the design read as an
                // operand.
                collectConnRefs(expr.as<AssignmentExpression>().left(), ctx, out,
                                inExpression);
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
    std::set<std::tuple<std::string, bool, uint32_t>> hierSeen;
    int64_t instanceRow = 0;
    Stats stats;
};

} // namespace

Stats extract(Compilation& compilation, AnalysisManager& analysis, Writer& writer) {
    Walker walker(compilation, analysis, writer);
    return walker.run();
}

} // namespace designdb
