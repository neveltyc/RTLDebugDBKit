// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Walking one procedure, statement by statement.
//
// The visitor emits the node stream of extract/ir/Nodes.h and knows nothing
// about the template, the database or the hierarchy: a statement arrives with
// its targets, its paired operands and its gate, and what to do with that is
// the caller's business.
//
// Header-only, like the layers under it. Splitting the handlers into a .cpp
// would buy compile isolation and cost the reader: for a class that is 25
// short handlers, having to hold a declaration and a definition apart is the
// classic way to make small methods harder to follow, and there is exactly
// one caller to isolate from.

#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/Expression.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/expressions/AssertionExpr.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/statements/ConditionalStatements.h"
#include "slang/ast/statements/LoopStatements.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/SubroutineSymbols.h"

#include "extract/Ref.h"
#include "extract/SymbolText.h"
#include "extract/ir/Nodes.h"

namespace designdb::detail {

using namespace slang;
using namespace slang::ast;

// ------------------------------------------------------- statement walking
//
// Walks a procedure statement by statement. Ported from v9 with the callback
// layer reshaped: a target arrives with its paired operands and the gating
// stack in one call, because the template needs the pairing (net_dep names
// the operand and target rows) rather than a stream of independent edges.

struct StatementWalker : public ASTVisitor<StatementWalker, VisitFlags::AllGood> {
    /// The walk's one output: a stream of self-contained nodes
    /// (extract/ir/Nodes.h). A read-only statement carries its gate too:
    /// `if (gate) $display(payload);` reads gate, and no dependency row can
    /// carry that read because the statement writes nothing.
    using EmitNode = std::function<void(Node&&)>;

    EmitNode emit;
    /// The interned gating contexts, owned by the receiver so it outlives
    /// the walk and the nodes that reference into it.
    GateTable& gates;
    EvalContext& eval;
    const TimingControl* sensitivityTiming = nullptr;
    /// The delay control in force for statements below a `#d` timed statement,
    /// and for a continuous assign's own delay.
    std::string pendingDelay;
    /// The construct word an enclosing `force`/procedural `assign` stamps
    /// on its assignment; null outside one.
    const char* constructOverride = nullptr;
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
    /// The call-site machinery. `callSiteSlot` points at Build::curCallSite so
    /// handle(CallExpression) can set the site in force around a body walk;
    /// `allocCallSite` mints a template call-site row for the entered call and
    /// returns its index. Both null outside a real build (dry runs).
    int32_t* callSiteSlot = nullptr;
    /// The subroutine whose body is being walked, so `return` knows what it
    /// writes. Null outside one.
    const SubroutineSymbol* curSub = nullptr;
    std::function<int32_t(const SubroutineSymbol&, int64_t depth, bool bindable)>
        allocCallSite;

    StatementWalker(GateTable& gates, EmitNode emit, EvalContext& eval) :
        emit(std::move(emit)), gates(gates), eval(eval) {}

    /// The gating stack as it stands, interned.
    GateId gateId() { return gates.intern(gating); }

    void handle(const ImmediateAssertionStatement& stmt) {
        std::vector<Ref> reads;
        filteredConstants = 0;
        collectRefs(stmt.cond, eval, reads);
        emit(ReadNode{std::move(reads), ReadNode::Kind::Assertion,
                      assertionWord(stmt.assertionKind), gateId(), seq++,
                      filteredConstants, stmt.sourceRange});
        visitDefault(stmt);
    }

    void handle(const ConcurrentAssertionStatement& stmt) {
        std::vector<Ref> reads;
        filteredConstants = 0;
        collectStatementRefs(stmt.propertySpec, reads);
        emit(ReadNode{std::move(reads), ReadNode::Kind::Assertion,
                      assertionWord(stmt.assertionKind), gateId(), seq++,
                      filteredConstants, stmt.sourceRange});
        visitDefault(stmt);
    }

    void handle(const WaitStatement& stmt) {
        std::vector<Ref> reads;
        filteredConstants = 0;
        collectRefs(stmt.cond, eval, reads);
        emit(ReadNode{std::move(reads), ReadNode::Kind::Wait, "wait", gateId(),
                      seq++, filteredConstants, stmt.sourceRange});
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
        filteredConstants = 0;
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
        // A system task that writes an argument -- $readmemh into a memory,
        // $sscanf into a variable, $cast into its destination -- really does
        // drive it, and slang models the write as an assignment inside the
        // call. A user subroutine's write is covered by the formal binding,
        // so only the system case needs targets of its own; without them the
        // argument read as undriven and its procedure as one that wrote
        // nothing at all.
        if (call.isSystemCall()) {
            emit(SystemTaskNode{std::move(reads), std::move(writeRefs),
                                callWord(call), gateId(), seq++,
                                filteredConstants, stmt.sourceRange});
        }
        else {
            emit(ReadNode{std::move(reads), ReadNode::Kind::Call,
                          callWord(call), gateId(), seq++, filteredConstants,
                          stmt.sourceRange});
        }
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
            std::vector<std::pair<const Expression*, Edge>> raw;
            // The `iff` qualifier travels too. collectEdgeEvents takes the
            // vector for it and buildProcedure passes one for the sensitivity
            // list; this path did not, so `@(posedge clk iff en)` written as a
            // STATEMENT -- any initial, or a procedure whose sensitivity slang
            // classifies as dynamic -- sampled en and recorded nothing about
            // it. en had zero load rows anywhere.
            std::vector<const Expression*> iffs;
            collectEdgeEvents(&stmt.timing, raw, &iffs);
            for (auto& [expr, edge] : raw)
                emit(EventNode{expr, edge, seq++, stmt.sourceRange});
            if (!iffs.empty()) {
                std::vector<Ref> reads;
                filteredConstants = 0;
                for (auto* c : iffs)
                    collectRefs(*c, eval, reads);
                if (!reads.empty())
                    emit(ReadNode{std::move(reads), ReadNode::Kind::Wait,
                                  "wait", gateId(), seq++, filteredConstants,
                                  stmt.sourceRange});
            }
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

    /// The gating a control expression contributes: its references, less the
    /// actuals a call inside it WRITES.
    ///
    /// A condition is gathered as one expression, and slang models an
    /// `output` actual as an assignment inside the call, so `if (chk(a, y))`
    /// puts y among the condition's operands. The condition does not read y --
    /// the call writes it, and that write is a source-less `procedure`
    /// dependency of its own. The drop matches the reference's own expression
    /// rather than its symbol, so `chk(y, y)` keeps its read: the two
    /// occurrences are two nodes.
    ///
    /// It happens before the expression is visited because the body of the
    /// called subroutine is walked under this gating stack; after, a formal
    /// the body assigns would take the same wrong source.
    void collectGating(const Expression& expr) {
        std::vector<Ref> refs;
        collectRefs(expr, eval, refs);
        std::set<const Expression*> written;
        collectCallOutputs(expr, written);
        if (!written.empty()) {
            refs.erase(std::remove_if(refs.begin(), refs.end(),
                                      [&](const Ref& r) {
                                          return written.count(r.origin) > 0;
                                      }),
                       refs.end());
        }
        gating.insert(gating.end(), refs.begin(), refs.end());
    }

    /// The expressions a call inside `expr` writes without reading: the
    /// actuals bound to an `output` formal. `inout` and `ref` are read as
    /// well, so what they give a condition is a real read and stays. The
    /// selectors stay too -- `chk(a, m[i])` reads i to decide where to write.
    void collectCallOutputs(const Expression& expr,
                            std::set<const Expression*>& out) {
        struct Finder : ASTVisitor<Finder, VisitFlags::AllGood> {
            StatementWalker& self;
            std::set<const Expression*>& out;
            Finder(StatementWalker& self, std::set<const Expression*>& out) :
                self(self), out(out) {}
            void handle(const CallExpression& call) {
                visitDefault(call);
                auto sub = std::get_if<const SubroutineSymbol*>(&call.subroutine);
                if (!sub || !*sub)
                    return;
                auto args = call.arguments();
                auto formals = (*sub)->getArguments();
                const size_t n = std::min(args.size(), formals.size());
                for (size_t i = 0; i < n; i++) {
                    if (!args[i] || !formals[i] ||
                        formals[i]->direction != ArgumentDirection::Out)
                        continue;
                    std::vector<Ref> written;
                    collectRefs(*args[i], self.eval, written,
                                /*skipSelectors=*/true);
                    for (auto& w : written)
                        out.insert(w.origin);
                }
            }
        };
        Finder f(*this, out);
        expr.visit(f);
    }

    void handle(const ConditionalStatement& stmt) {
        const size_t mark = gating.size();
        visitGuarded([&] {
            for (auto& cond : stmt.conditions) {
                collectGating(*cond.expr);
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
            collectGating(stmt.expr);
            stmt.expr.visit(*this);
            for (auto& item : stmt.items) {
                for (auto* label : item.expressions) {
                    collectGating(*label);
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

    // The three loop handlers visit their condition under visitGuarded and
    // then the BODY, rather than handing the whole statement to visitDefault.
    // visitDefault walks the condition with bindable still true, so a call in
    // a loop condition -- `while (pred(x))` -- was attributed to whatever
    // statement happened to precede it, since curStmt there is a stale earlier
    // one. handle(ConditionalStatement) has always got this right; the loops
    // had not.
    void handle(const ForLoopStatement& stmt) {
        const size_t mark = gating.size();
        const size_t loopMark = loopVars.size();
        for (auto* v : stmt.loopVars)
            loopVars.insert(v);
        visitGuarded([&] {
            for (auto* init : stmt.initializers)
                init->visit(*this);
            if (stmt.stopExpr) {
                collectGating(*stmt.stopExpr);
                stmt.stopExpr->visit(*this);
            }
            for (auto* step : stmt.steps)
                step->visit(*this);
        });
        stmt.body.visit(*this);
        if (loopVars.size() != loopMark) {
            for (auto* v : stmt.loopVars)
                loopVars.erase(v);
        }
        gating.resize(mark);
    }

    void handle(const WhileLoopStatement& stmt) {
        const size_t mark = gating.size();
        visitGuarded([&] {
            collectGating(stmt.cond);
            stmt.cond.visit(*this);
        });
        stmt.body.visit(*this);
        gating.resize(mark);
    }

    /// `do … while (c)` had no handler at all, so it fell to visitDefault,
    /// which visits the condition -- and StatementWalker has no handler for a
    /// bare value expression, so nothing was recorded: not the gating, not
    /// even the read. The condition signal had zero load rows in the whole
    /// database despite being read every iteration.
    void handle(const DoWhileLoopStatement& stmt) {
        const size_t mark = gating.size();
        visitGuarded([&] {
            collectGating(stmt.cond);
            stmt.cond.visit(*this);
        });
        stmt.body.visit(*this);
        gating.resize(mark);
    }

    void handle(const RepeatLoopStatement& stmt) {
        const size_t mark = gating.size();
        visitGuarded([&] {
            collectGating(stmt.count);
            stmt.count.visit(*this);
        });
        stmt.body.visit(*this);
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
            // positional as a mapping gets. Each target is its own
            // statement, as it always was here.
            emit(AssignmentNode{{TargetRecord{dst, {PairedSrc{dst, dst, true, dst}}}},
                                gateId(), expr.sourceRange, seq++,
                                /*blocking=*/true, 0, subDepth > 0,
                                pendingDelay, constructOverride});
        }
        visitDefault(expr);
    }

    void handle(const CallExpression& expr) {
        visitDefault(expr);
        auto sub = std::get_if<const SubroutineSymbol*>(&expr.subroutine);
        if (!sub || !*sub)
            return;
        // Enter the call site before binding: the argument bindings and the
        // body statements both belong to THIS call, so both must be tagged
        // with it. Restored on every exit path below.
        const int32_t savedCallSite = callSiteSlot ? *callSiteSlot : -1;
        if (callSiteSlot && allocCallSite)
            *callSiteSlot = allocCallSite(**sub, subDepth + 1, bindable);
        struct Restore {
            int32_t* slot;
            int32_t val;
            ~Restore() { if (slot) *slot = val; }
        } restore{callSiteSlot, savedCallSite};
        bindArguments(expr, **sub);
        if (!activeSubs.insert(*sub).second)
            return;                       // recursion guard (Restore fires)
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
        const SubroutineSymbol* savedSub = curSub;
        curSub = *sub;
        (*sub)->getBody().visit(*this);
        curSub = savedSub;
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
            // A `const ref` is passed by reference to avoid copying and
            // cannot be assigned through, so it drives nothing. slang
            // decides `hasOutputArgs` by this same pair of tests.
            const bool constRef =
                dir == ArgumentDirection::Ref &&
                formals[i]->flags.has(VariableFlags::Const);
            const bool writes = dir == ArgumentDirection::Out ||
                                dir == ArgumentDirection::InOut ||
                                (dir == ArgumentDirection::Ref && !constRef);
            const bool reads = dir == ArgumentDirection::In ||
                               dir == ArgumentDirection::InOut ||
                               dir == ArgumentDirection::Ref;
            // An output or inout actual is not args[i]: Expression::bindLValue
            // wraps it in an AssignmentExpression whose left is the actual and
            // whose right is an EmptyArgumentExpression. Unwrapped, every such
            // binding failed isPlainReference on the wrapper's kind and claimed
            // map_exact=0 -- even `t(x, y)` with y exactly as wide as its
            // formal. buildInstanceConns already unwraps this for the same
            // reason; bindArguments simply had not.
            const Expression* actualExpr = args[i];
            if (actualExpr->kind == ExpressionKind::Assignment &&
                actualExpr->as<AssignmentExpression>().isLValueArg())
                actualExpr = &actualExpr->as<AssignmentExpression>().left();
            std::vector<Ref> actuals;
            collectRefs(*args[i], eval, actuals, /*skipSelectors=*/writes);
            for (auto& a : actuals) {
                if (!a.sym)
                    continue;
                // Whole-to-whole only when the actual IS a reference filling
                // the formal -- the same leaf rule every positional claim
                // answers to.
                const uint64_t fw = bitWidthOf(*formals[i]);
                const bool oneToOne =
                    actuals.size() == 1 && isPlainReference(*actualExpr) &&
                    coverFillsWidth(actuals[0].cover, actuals[0].exact, fw,
                                    bitWidthOf(*actuals[0].sym));
                emit(BindNode{formals[i], args[i], a, reads, writes, oneToOne,
                              bindable, expr.sourceRange});
            }
        }
    }

    void handle(const ProceduralAssignStatement& s) {
        // `force a = b` and procedural `assign a = b` move data exactly as
        // a blocking assignment does, and until now produced exactly the
        // same row -- a hijacked signal's driver could not be told from
        // the logic it overrode. The construct word is the marker;
        // `WHERE construct='force'` is the debug query this exists for.
        const char* saved = constructOverride;
        constructOverride = s.isForce ? "force" : "proc_assign";
        visitDefault(s);
        constructOverride = saved;
    }

    void handle(const ProceduralDeassignStatement& s) {
        // `release`/`deassign` drive nothing and read nothing -- but each
        // is the other half of a force, and leaving no row made "where
        // does the hijack end" unanswerable. The statement records its
        // lvalues and deliberately no dependency.
        //
        // The gating travels with it, for the same reason EmitRead's own
        // comment gives for assertions: `if (g) force y = x; else release y;`
        // recorded g on the force and nothing on the release, so "what decides
        // when the hijack ends" -- the query this row exists for -- had no
        // answer. visitDefault picks up reads in the lvalue's own selectors,
        // the `i` in `release mem[i]`.
        std::vector<Ref> writes;
        filteredConstants = 0;
        collectRefs(s.lvalue, eval, writes, /*skipSelectors=*/true);
        emit(ReleaseNode{std::move(writes), s.isRelease, gateId(), seq++,
                         filteredConstants, s.sourceRange});
        visitDefault(s);
    }

    /// `case … matches` had no handler, so it fell to visitDefault and
    /// recorded nothing at all -- not the gating, not even a read of the
    /// controlling expression, which had zero load rows in the whole database.
    /// Modelled on handle(CaseStatement): the subject and each item's filter
    /// are conditions, the item bodies are not.
    void handle(const PatternCaseStatement& stmt) {
        const size_t mark = gating.size();
        visitGuarded([&] {
            collectGating(stmt.expr);
            stmt.expr.visit(*this);
            for (auto& item : stmt.items) {
                if (item.filter) {
                    collectGating(*item.filter);
                    item.filter->visit(*this);
                }
            }
        });
        for (auto& item : stmt.items)
            item.stmt->visit(*this);
        if (stmt.defaultCase)
            stmt.defaultCase->visit(*this);
        gating.resize(mark);
    }

    /// `return expr;` writes the subroutine's implicit result variable, and
    /// slang does not synthesise that assignment -- ReturnStatement carries
    /// only the expression, and the target is SubroutineSymbol::returnValVar.
    /// With no handler the whole function body recorded nothing, so the two
    /// legal spellings of one function disagreed: `f = a ^ k;` gave f its two
    /// operands while `return a | k;` gave a net that nothing reads and no
    /// statement row at all.
    void handle(const ReturnStatement& stmt) {
        if (!stmt.expr || !curSub || !curSub->returnValVar) {
            visitDefault(stmt);
            return;
        }
        Ref dst;
        dst.sym = curSub->returnValVar;
        dst.origin = stmt.expr;
        // A return writes all of the implicit result variable; whole and
        // exact is the genuine answer here, not a parity default.
        dst.cover = BitInterval::whole();
        dst.exact = true;
        emitAssignmentLike(dst, *stmt.expr, stmt.sourceRange);
        visitDefault(stmt);
    }

    /// One assignment-shaped emission with an explicit target, for the places
    /// slang gives no AssignmentExpression to walk. The target is taken whole
    /// and unpositioned -- an unplaced slot pairs every operand with all of it
    /// and makes `narrowed` a no-op -- which is the honest answer for a
    /// `return`: the expression that produced the value carries between bits.
    void emitAssignmentLike(const Ref& dst, const Expression& src,
                            SourceRange where) {
        std::vector<Slot> rhsSlots;
        filteredConstants = 0;
        collectSlots(src, eval, 0, rhsSlots);
        collectAuxSlots(src, 0, rhsSlots, /*selectors=*/false);
        const int64_t dropped = filteredConstants;
        eval.reset();

        const Slot dstSlot = Slot::unpositioned(dst);
        std::vector<PairedSrc> pairs;
        for (auto& srcSlot : rhsSlots) {
            std::optional<BitRange> span;
            if (!slotsOverlap(dstSlot, srcSlot, span))
                continue;
            pairs.push_back(PairedSrc{narrowed(srcSlot, span),
                                      narrowed(dstSlot, span),
                                      false, srcSlot.ref});
        }
        emit(AssignmentNode{{TargetRecord{dst, std::move(pairs)}}, gateId(),
                            where, seq++, /*blocking=*/true, dropped,
                            subDepth > 0, pendingDelay, constructOverride});
    }

    void handle(const AssignmentExpression& expr) {
        // The copy-back slang synthesises for an `output`/`inout` actual:
        // `bump(i0, o0)` carries an assignment to o0 whose right side is an
        // empty placeholder. It is not a statement anyone wrote, and it has
        // no operands -- so recording it produced a source-less dependency,
        // which v_driver reports as a CONSTANT tie-off on a signal the task
        // plainly drives. The real record is the `procedure` dependency
        // from the formal, which bindArguments already makes.
        //
        // Ask slang rather than testing the kind: the placeholder is not
        // always bare. bindLValue gives it the FORMAL's type and then
        // fromComponents converts it to the ACTUAL's, so an actual of any
        // other type -- a narrower variable, a different sign -- arrives
        // wrapped in a Conversion, and a raw kind test lets exactly those
        // calls back through to the tie-off it exists to prevent.
        if (expr.isLValueArg()) {
            visitDefault(expr);
            return;
        }
        std::vector<Ref> targets;
        collectRefs(expr.left(), eval, targets, /*skipSelectors=*/true);
        // Path analysis found nothing to write. No RTL is known to reach
        // here, and nothing in the fixture corpus or the three real designs
        // does; it stands because losing a driver outright is the one
        // outcome that must not happen. The root is resolved by walking the
        // lvalue rather than through getSymbolReference, which hands back a
        // FIELD symbol for a member access on an unpacked struct.
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
                // The default cover: path analysis computed no bounds, so
                // the object is written somewhere and this records that
                // much. Claiming the whole of it exactly would state the
                // one thing this branch does not know.
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
                lhsSlots.push_back(Slot::unpositioned(t));
        }

        std::vector<Slot> rhsSlots;
        filteredConstants = 0;
        collectSlots(expr.right(), eval, 0, rhsSlots);
        collectAuxSlots(expr.right(), 0, rhsSlots, /*selectors=*/false);
        collectAuxSlots(expr.left(), 0, rhsSlots, /*selectors=*/true);
        // `a += b` reads a, and nothing above finds that read. slang does not
        // rewrite a compound assignment into `a = a + b`; it builds the right
        // side as BinaryExpression(LValueReferenceExpression, b), and an
        // LValueReference is a bare placeholder -- no sub-expressions, no link
        // back to the lvalue, and no case in ValuePath::visitPaths. So the
        // target's own contribution yielded no reference at all: `b += x`
        // recorded only b <- x, and `d <<= 2`, whose whole right side is that
        // placeholder and a constant, recorded d as driven by a CONSTANT --
        // a tie-off claim on a signal fed by itself.
        //
        // positional is cleared because the operator carries between bits:
        // leaving it set would claim `b += x` maps bit for bit, which an adder
        // does not.
        if (expr.isCompound()) {
            std::vector<Slot> selfRead;
            collectSlots(expr.left(), eval, 0, selfRead, /*skipSelectors=*/true);
            for (auto& sr : selfRead)
                sr.positional = false;
            rhsSlots.insert(rhsSlots.end(), selfRead.begin(), selfRead.end());
        }
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
        std::vector<TargetRecord> records;
        for (auto& dstSlot : lhsSlots) {
            if (loopVars.count(dstSlot.ref.sym))
                continue;
            std::vector<PairedSrc> pairs;
            for (auto& srcSlot : rhsSlots) {
                std::optional<BitRange> span;
                if (!slotsOverlap(dstSlot, srcSlot, span))
                    continue;
                pairs.push_back(PairedSrc{narrowed(srcSlot, span),
                                          narrowed(dstSlot, span),
                                          dstSlot.positional && srcSlot.positional,
                                          srcSlot.ref});
            }
            records.push_back(TargetRecord{dstSlot.ref, std::move(pairs)});
        }
        // One node owning every target of the statement -- what the old
        // firstTarget flag reconstructed. No targets, no statement.
        if (!records.empty()) {
            emit(AssignmentNode{std::move(records), gateId(), expr.sourceRange,
                                stmtSeq, expr.isBlocking(), droppedConstants,
                                subDepth > 0, delay, constructOverride});
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
    ///
    /// The element loop and its two corner rules are ir/PositionWalk.h's,
    /// shared with collectSlots. The recovery here: from the overflow on,
    /// each element's reads ride unpositioned -- imprecise exactly where the
    /// wrap would have been wrong.
    void collectAuxSlots(const Expression& expr, uint64_t base,
                         std::vector<Slot>& out, bool selectors) {
        const uint64_t width = exprWidthOf(expr);
        if (isElementwise(expr) && width) {
            walkElements(
                expr, base, width, exprWidthOf,
                [&](const Expression& op, BitRange window) {
                    collectAuxSlots(op, window.lo, out, selectors);
                },
                []() {},
                [&](const Expression& op) {
                    std::vector<Ref> reads;
                    if (selectors)
                        collectLeftSelectorRefs(op, reads);
                    else
                        collectCallReads(op, reads);
                    for (auto& r : reads)
                        out.push_back(Slot::unpositioned(r));
                });
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
                out.push_back(Slot::at(r, BitRange(base, base + width - 1), false));
            else
                out.push_back(Slot::unpositioned(r));
        }
    }
};

} // namespace designdb::detail
