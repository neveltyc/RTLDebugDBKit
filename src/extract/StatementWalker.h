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

#include <algorithm>
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
    /// The branch levels, owned by the receiver so they outlive the walk
    /// and the nodes that reference into them.
    BranchTable& branches;
    EvalContext& eval;
    const TimingControl* sensitivityTiming = nullptr;
    /// The delay control in force for statements below a `#d` timed statement,
    /// and for a continuous assign's own delay.
    std::string pendingDelay;
    /// The construct word an enclosing `force`/procedural `assign` stamps
    /// on its assignment; null outside one.
    const char* constructOverride = nullptr;
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

    StatementWalker(BranchTable& branches, EmitNode emit, EvalContext& eval) :
        emit(std::move(emit)), branches(branches), eval(eval) {}

    /// One branch level pushed but not yet given a row.
    struct Pending {
        BranchFrame frame;
        BranchId id = -1;
    };
    std::vector<Pending> levels;

    /// The level the statement being walked sits in, materialising the
    /// pending chain down to it.
    ///
    /// On demand, so an `if` arm or a loop that gates nothing leaves no row
    /// behind. A case arm is materialised by its handler regardless: the
    /// arms under a point are the whole case.
    BranchId currentBranch() {
        BranchId parent = -1;
        for (size_t i = 0; i < levels.size(); i++) {
            auto& p = levels[i];
            if (p.id < 0) {
                p.frame.parent = parent;
                p.frame.depth = int32_t(i) + 1;
                p.id = branches.add(std::move(p.frame));
            }
            parent = p.id;
        }
        return parent;
    }

    /// Holds one branch level in force for the statements under it.
    class Level {
    public:
        Level(StatementWalker& w, BranchFrame frame) : w_(w) {
            w_.levels.push_back(Pending{std::move(frame), -1});
        }
        ~Level() { w_.levels.pop_back(); }
        Level(const Level&) = delete;
        Level& operator=(const Level&) = delete;

    private:
        StatementWalker& w_;
    };

    /// Drops the loop indices in force from a set of DATAFLOW references.
    ///
    /// An index is not a design signal: it takes every value of the
    /// iteration space on the way through, so `hi[i] = a[i+2]` recorded
    /// `i -> hi` as data -- three times, in picorv32's multiplier twelve of
    /// eighteen dependency rows -- for a variable no debugging question
    /// reaches. What the loop does with it is the iteration space on the
    /// loop's own branch row.
    ///
    /// Only the data and control paths. Where the RTL reads the index as a
    /// VALUE -- `t(i)`, `$display("%d", i)`, `assert (i < 4)` -- the read
    /// stays: dropping it leaves a bound formal with no driver at all.
    void dropLoopVars(std::vector<Ref>& refs, size_t from = 0) {
        if (loopVars.empty())
            return;
        refs.erase(std::remove_if(refs.begin() + std::ptrdiff_t(from), refs.end(),
                                  [&](const Ref& r) {
                                      return loopVars.count(r.sym) > 0;
                                  }),
                   refs.end());
    }

    void dropLoopVars(std::vector<Slot>& slots) {
        if (loopVars.empty())
            return;
        slots.erase(std::remove_if(slots.begin(), slots.end(),
                                   [&](const Slot& s) {
                                       return loopVars.count(s.ref.sym) > 0;
                                   }),
                    slots.end());
    }

    void handle(const ImmediateAssertionStatement& stmt) {
        std::vector<Ref> reads;
        filteredConstants = 0;
        collectRefs(stmt.cond, eval, reads);
        emit(ReadNode{std::move(reads), ReadNode::Kind::Assertion,
                      assertionWord(stmt.assertionKind), currentBranch(), seq++,
                      filteredConstants, stmt.sourceRange});
        visitDefault(stmt);
    }

    void handle(const ConcurrentAssertionStatement& stmt) {
        std::vector<Ref> reads;
        filteredConstants = 0;
        collectStatementRefs(stmt.propertySpec, reads);
        emit(ReadNode{std::move(reads), ReadNode::Kind::Assertion,
                      assertionWord(stmt.assertionKind), currentBranch(), seq++,
                      filteredConstants, stmt.sourceRange});
        visitDefault(stmt);
    }

    void handle(const WaitStatement& stmt) {
        std::vector<Ref> reads;
        filteredConstants = 0;
        collectRefs(stmt.cond, eval, reads);
        emit(ReadNode{std::move(reads), ReadNode::Kind::Wait, "wait", currentBranch(),
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
        // A bound argument is read by its binding, which names the formal it
        // feeds. Leaving it here too made one read two rows, against the
        // rule that a read lands in exactly one of assign_operand, expr_ref
        // and proc_event. Matched by the reference's own expression, as
        // collectGating matches: `t(y, y)` is two occurrences and two
        // bindings, not one read to drop twice.
        std::set<const Expression*> bound;
        collectBoundArgumentRefs(call, bound);
        if (!bound.empty()) {
            reads.erase(std::remove_if(reads.begin(), reads.end(),
                                       [&](const Ref& r) {
                                           return bound.count(r.origin) > 0;
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
        //
        // A built-in METHOD (`q.push_back(x)`, `q.delete()`) registers as a
        // system call in slang but is not a system task: nothing leaves the
        // language, and stmt_kind has `call` for exactly this. What separates
        // the two is the `$` prefix OR a written argument -- `randomize(a)`
        // carries no `$` and still drives a from outside anything the model
        // names, and classifying it by the prefix alone dropped its write
        // entirely, since only this branch carries writeRefs.
        if (call.isSystemCall() &&
            (call.getSubroutineName().starts_with('$') || !writeRefs.empty())) {
            emit(SystemTaskNode{std::move(reads), std::move(writeRefs),
                                callWord(call), currentBranch(), seq++,
                                filteredConstants, stmt.sourceRange});
        }
        else {
            emit(ReadNode{std::move(reads), ReadNode::Kind::Call,
                          callWord(call), currentBranch(), seq++, filteredConstants,
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

    /// The expressions bindArguments will bind, so a statement-level call
    /// does not record their reads a second time. A system call has no
    /// formals and binds nothing.
    void collectBoundArgumentRefs(const CallExpression& call,
                                  std::set<const Expression*>& out) {
        auto sub = std::get_if<const SubroutineSymbol*>(&call.subroutine);
        if (!sub || !*sub)
            return;
        auto args = call.arguments();
        auto formals = (*sub)->getArguments();
        const size_t n = std::min(args.size(), formals.size());
        for (size_t i = 0; i < n; i++) {
            if (!args[i] || !formals[i])
                continue;
            std::vector<Ref> refs;
            collectRefs(*args[i], eval, refs);
            for (auto& r : refs)
                out.insert(r.origin);
        }
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
                emit(EventNode{expr, edge, currentBranch(), seq++,
                               stmt.sourceRange});
            if (!iffs.empty()) {
                std::vector<Ref> reads;
                filteredConstants = 0;
                for (auto* c : iffs)
                    collectRefs(*c, eval, reads);
                if (!reads.empty())
                    emit(ReadNode{std::move(reads), ReadNode::Kind::Wait,
                                  "wait", currentBranch(), seq++, filteredConstants,
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
    /// The reads land on the level being built, and the condition is
    /// visited BEFORE that level is pushed: a call written in a condition
    /// runs when control reaches the branch, whichever arm it then takes,
    /// so its body's statements belong to the enclosing context and not to
    /// either arm.
    void collectGating(const Expression& expr, std::vector<Ref>& out) {
        const size_t mark = out.size();
        collectRefs(expr, eval, out);
        dropLoopVars(out, mark);
        std::set<const Expression*> written;
        collectCallOutputs(expr, written);
        if (!written.empty()) {
            out.erase(std::remove_if(out.begin() + std::ptrdiff_t(mark), out.end(),
                                     [&](const Ref& r) {
                                         return written.count(r.origin) > 0;
                                     }),
                      out.end());
        }
        emitSystemWritesIn(expr);
    }

    /// A system task called in a CONDITION still writes its argument, and
    /// the write has nowhere to go: the call belongs to no statement, so
    /// v_driver cannot tell it from a tie-off the way it does for a
    /// statement-level one. It gets a row of its own -- the same answer the
    /// procedure-header `event_control` row is to reads with no statement.
    /// Only the write travels; what the condition reads is gating already.
    ///
    /// One row per OUTERMOST call, carrying everything it writes, which is
    /// the shape a statement-level call has. Emitting per nested call
    /// instead recorded an inner write twice: once for the inner call and
    /// again for the outer, whose collection reaches through it.
    void emitSystemWritesIn(const Expression& expr) {
        struct Finder : ASTVisitor<Finder, VisitFlags::AllGood> {
            StatementWalker& self;
            explicit Finder(StatementWalker& self) : self(self) {}
            void handle(const CallExpression& call) {
                if (!call.isSystemCall()) {
                    visitDefault(call);
                    return;
                }
                std::set<const ValueSymbol*> syms;
                std::vector<Ref> writes;
                self.collectWrittenTargets(call, syms, &writes);
                if (writes.empty()) {
                    visitDefault(call);
                    return;
                }
                self.emit(SystemTaskNode{{}, std::move(writes),
                                         callWord(call), self.currentBranch(),
                                         self.seq++, 0, call.sourceRange});
            }
        };
        Finder f(*this);
        expr.visit(f);
    }

    /// The expressions a call inside `expr` writes without reading: the
    /// actuals bound to an `output` formal, and the arguments a system task
    /// writes -- `if ($value$plusargs("SEED=%d", seed))` writes seed and
    /// reads nothing of it. `inout` and `ref` are read as well, so what they
    /// give a condition is a real read and stays. The selectors stay too --
    /// `chk(a, m[i])` reads i to decide where to write.
    void collectCallOutputs(const Expression& expr,
                            std::set<const Expression*>& out) {
        struct Finder : ASTVisitor<Finder, VisitFlags::AllGood> {
            StatementWalker& self;
            std::set<const Expression*>& out;
            Finder(StatementWalker& self, std::set<const Expression*>& out) :
                self(self), out(out) {}
            void handle(const CallExpression& call) {
                visitDefault(call);
                if (call.isSystemCall()) {
                    // slang models the written argument as an assignment
                    // inside the call, the same shape an output actual has.
                    std::set<const ValueSymbol*> syms;
                    std::vector<Ref> writes;
                    self.collectWrittenTargets(call, syms, &writes);
                    for (auto& w : writes)
                        out.insert(w.origin);
                    return;
                }
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

    static CheckKind checkOf(UniquePriorityCheck c) {
        switch (c) {
            case UniquePriorityCheck::Unique:   return CheckKind::Unique;
            case UniquePriorityCheck::Unique0:  return CheckKind::Unique0;
            case UniquePriorityCheck::Priority: return CheckKind::Priority;
            default:                            return CheckKind::None;
        }
    }

    static CaseKind caseKindOf(CaseStatementCondition c) {
        switch (c) {
            case CaseStatementCondition::WildcardJustZ: return CaseKind::Casez;
            case CaseStatementCondition::WildcardXOrZ:  return CaseKind::Casex;
            case CaseStatementCondition::Inside:        return CaseKind::Inside;
            default:                                    return CaseKind::Case;
        }
    }

    /// Whether the condition is settled at elaboration, and which way: 1 the
    /// then arm runs, 0 the else arm does, -1 nothing is decided. A value
    /// with unknown bits decides nothing -- `if (1'bx)` takes the else arm
    /// in simulation, but calling that a constant verdict states more than
    /// the fold knows.
    int staticCondition(const ConditionalStatement& stmt) {
        bool allKnownTrue = true;
        for (auto& cond : stmt.conditions) {
            if (cond.pattern) {
                allKnownTrue = false;
                continue;
            }
            ConstantValue cv = cond.expr->eval(eval);
            if (!cv || cv.hasUnknown()) {
                allKnownTrue = false;
                continue;
            }
            if (!cv.isTrue())
                return 0;
        }
        return allKnownTrue ? 1 : -1;
    }

    void handle(const ConditionalStatement& stmt) {
        BranchFrame arm;
        arm.kind = BranchKind::If;
        arm.check = checkOf(stmt.check);
        arm.where = stmt.sourceRange;
        visitGuarded([&] {
            for (auto& cond : stmt.conditions) {
                collectGating(*cond.expr, arm.refs);
                cond.expr->visit(*this);
            }
        });
        const int taken = staticCondition(stmt);
        {
            BranchFrame t = arm;
            t.sense = BranchSense::Then;
            t.staticTaken = taken;
            Level level(*this, std::move(t));
            stmt.ifTrue.visit(*this);
        }
        if (stmt.ifFalse) {
            BranchFrame e = std::move(arm);
            e.sense = BranchSense::Else;
            e.staticTaken = taken < 0 ? -1 : 1 - taken;
            Level level(*this, std::move(e));
            stmt.ifFalse->visit(*this);
        }
    }

    /// Whether a body can gate anything at all: `2'b01: ;` and `2'b01: begin
    /// end` cannot. Their arm makes no level on demand, so the labels' reads
    /// would have no statement anywhere to land on, and a signal the case
    /// reads would end with no load row in the database at all.
    static bool gatesNothing(const Statement& body) {
        switch (body.kind) {
            case StatementKind::Empty:
                return true;
            case StatementKind::List:
                for (auto* child : body.as<StatementList>().list) {
                    if (!child || !gatesNothing(*child))
                        return false;
                }
                return true;
            case StatementKind::Block:
                return gatesNothing(body.as<BlockStatement>().body);
            default:
                return false;
        }
    }

    /// One label, evaluated. nullopt is a label constant evaluation does not
    /// reach -- an `inside` range over a variable -- whose reads stay on the
    /// item's level like any other control read.
    ///
    /// The normalisation is inst_param's: the elaborated value in full
    /// precision, not the spelling the source used.
    std::optional<std::string> labelValue(const Expression& label) {
        ConstantValue cv = label.eval(eval);
        if (!cv)
            return std::nullopt;
        return cv.toString(SVInt::MAX_BITS, /*exactUnknowns=*/true);
    }

    /// A case is a branch POINT with one level per arm below it: the
    /// selector's reads go on the point, where every arm shares them, and
    /// each item's labels on the item. One level for both would leave a
    /// consumer no way to tell whose label a read was.
    void handle(const CaseStatement& stmt) {
        BranchFrame point;
        point.kind = BranchKind::Case;
        point.caseKind = caseKindOf(stmt.condition);
        point.check = checkOf(stmt.check);
        point.where = stmt.sourceRange;
        const auto [knownBranch, isKnown] = stmt.getKnownBranch(eval);

        // Selector and labels first, under the ENCLOSING level: both are
        // evaluated on reaching the case, whichever arm then runs, so a call
        // or a system write inside one belongs outside every arm -- and
        // materialising the point from a label would leave a point with no
        // arm under it.
        std::vector<BranchFrame> arms;
        visitGuarded([&] {
            collectGating(stmt.expr, point.refs);
            stmt.expr.visit(*this);
            for (auto& item : stmt.items) {
                BranchFrame arm;
                arm.kind = BranchKind::CaseItem;
                arm.ordinal = int32_t(arms.size());
                arm.where = item.expressions.empty() ? item.stmt->sourceRange
                                                     : item.expressions[0]->sourceRange;
                if (isKnown)
                    arm.staticTaken = knownBranch == item.stmt ? 1 : 0;
                for (auto* label : item.expressions) {
                    collectGating(*label, arm.refs);
                    label->visit(*this);
                    arm.labels.push_back(labelValue(*label));
                }
                // An arm that gates nothing hands its labels' reads to the
                // point, where the matching reads them anyway. Left on the
                // arm they would reach no statement and vanish.
                if (gatesNothing(*item.stmt)) {
                    point.refs.insert(point.refs.end(), arm.refs.begin(),
                                      arm.refs.end());
                    arm.refs.clear();
                }
                arms.push_back(std::move(arm));
            }
        });

        Level pointLevel(*this, std::move(point));
        for (size_t i = 0; i < stmt.items.size(); i++) {
            Level armLevel(*this, std::move(arms[i]));
            // Materialised whether or not the body gates anything: one arm
            // missing makes "which values fall through to default"
            // unanswerable.
            currentBranch();
            stmt.items[i].stmt->visit(*this);
        }
        if (stmt.defaultCase) {
            BranchFrame arm;
            arm.kind = BranchKind::CaseDefault;
            arm.ordinal = int32_t(arms.size());
            arm.where = stmt.defaultCase->sourceRange;
            if (isKnown)
                arm.staticTaken = knownBranch == stmt.defaultCase ? 1 : 0;
            Level armLevel(*this, std::move(arm));
            currentBranch();
            stmt.defaultCase->visit(*this);
        }
    }

    /// The object an lvalue ultimately writes, through any number of
    /// selects and member accesses.
    static const ValueSymbol* rootValueSymbol(const Expression& expr) {
        const Expression* e = &expr;
        for (;;) {
            switch (e->kind) {
                case ExpressionKind::ElementSelect:
                    e = &e->as<ElementSelectExpression>().value();
                    continue;
                case ExpressionKind::RangeSelect:
                    e = &e->as<RangeSelectExpression>().value();
                    continue;
                case ExpressionKind::MemberAccess:
                    e = &e->as<MemberAccessExpression>().value();
                    continue;
                case ExpressionKind::NamedValue:
                case ExpressionKind::HierarchicalValue:
                    return &e->as<ValueExpressionBase>().symbol;
                default:
                    return nullptr;
            }
        }
    }

    /// One index a `for` header steps, with the expression that starts it
    /// where the header gives one. Both spellings count: `for (int i = 0;
    /// …)` declares the variable in the header, and `integer i; for (i = 0;
    /// …)` -- what picorv32 and most pre-2005 RTL writes -- steps one
    /// declared outside.
    struct LoopIndex {
        const ValueSymbol* var = nullptr;
        const Expression* init = nullptr;
    };

    /// The objects an expression assigns, by any spelling: `i = e`, `i++`.
    /// In first-encounter order, because a set of pointers iterates by
    /// address and the walk has to be reproducible.
    struct AssignedFinder : ASTVisitor<AssignedFinder, VisitFlags::AllGood> {
        std::vector<const ValueSymbol*>& out;
        std::set<const ValueSymbol*> seen;
        explicit AssignedFinder(std::vector<const ValueSymbol*>& out) : out(out) {}
        void note(const Expression& lvalue) {
            if (auto* v = rootValueSymbol(lvalue); v && seen.insert(v).second)
                out.push_back(v);
        }
        void handle(const AssignmentExpression& e) {
            note(e.left());
            visitDefault(e);
        }
        void handle(const UnaryExpression& e) {
            switch (e.op) {
                case UnaryOperator::Preincrement:
                case UnaryOperator::Predecrement:
                case UnaryOperator::Postincrement:
                case UnaryOperator::Postdecrement:
                    note(e.operand());
                    break;
                default:
                    break;
            }
            visitDefault(e);
        }
        /// An actual bound to an `output`/`inout`/non-const `ref` formal is
        /// moved by the call, and slang models only the output copy-back as
        /// an assignment -- so the two other directions need naming here.
        void handle(const CallExpression& call) {
            visitDefault(call);
            auto sub = std::get_if<const SubroutineSymbol*>(&call.subroutine);
            if (!sub || !*sub)
                return;
            auto args = call.arguments();
            auto formals = (*sub)->getArguments();
            for (size_t i = 0; i < std::min(args.size(), formals.size()); i++) {
                if (!args[i] || !formals[i])
                    continue;
                const auto dir = formals[i]->direction;
                const bool constRef =
                    dir == ArgumentDirection::Ref &&
                    formals[i]->flags.has(VariableFlags::Const);
                if (dir == ArgumentDirection::Out ||
                    dir == ArgumentDirection::InOut ||
                    (dir == ArgumentDirection::Ref && !constRef))
                    note(*args[i]);
            }
        }
    };

    /// The header's indices: the variables its STEP expressions move.
    ///
    /// The step is what makes a variable an iterator, and testing for it is
    /// what keeps `for (acc = 8'd0; kk < 4; kk = kk + 1)` from losing acc:
    /// an initialiser list may hold ordinary variables, and a variable taken
    /// for an index is suppressed as an operand AND as an assignment target,
    /// so acc's rows would vanish outright and whoever read acc would trace
    /// to a signal nothing drives.
    ///
    /// The start value is a separate question -- an index stepped here but
    /// started somewhere else has no iteration space to publish -- so `init`
    /// is null where the header does not give one.
    void registerLoopIndices(const ForLoopStatement& stmt,
                             std::vector<LoopIndex>& mine) {
        std::vector<const ValueSymbol*> stepped;
        AssignedFinder f(stepped);
        for (auto* step : stmt.steps) {
            if (step)
                step->visit(f);
        }
        if (stepped.empty())
            return;
        auto startOf = [&](const ValueSymbol* var) -> const Expression* {
            for (auto* v : stmt.loopVars) {
                if (v == var)
                    return v->getInitializer();
            }
            for (auto* init : stmt.initializers) {
                if (!init || init->kind != ExpressionKind::Assignment)
                    continue;
                auto& a = init->as<AssignmentExpression>();
                if (!a.isCompound() && rootValueSymbol(a.left()) == var)
                    return &a.right();
            }
            return nullptr;
        };
        for (auto* var : stepped) {
            if (loopVars.insert(var).second)
                mine.push_back(LoopIndex{var, startOf(var)});
        }
    }

    /// Whether the body moves `var` -- by assignment, by increment, or by
    /// passing it to a formal that writes. The iteration space is a claim
    /// about the values the index takes, and a body that moves it makes the
    /// claim false; a false window is worse than none.
    static bool bodyAssigns(const Statement& body, const ValueSymbol* var) {
        std::vector<const ValueSymbol*> moved;
        AssignedFinder f(moved);
        body.visit(f);
        return std::find(moved.begin(), moved.end(), var) != moved.end();
    }

    /// The iteration space of a `for`: how many times the body runs and what
    /// values the index takes.
    ///
    /// This is what lets a consumer read a whole-signal `a -> hi` back as
    /// the per-iteration mapping it is, and it is four columns on one row
    /// where unrolling the body would be one statement per iteration --
    /// sixteen for picorv32's carry chain, and the product of the bounds for
    /// a nested pair. The index arithmetic itself stays in the source, where
    /// every other expression shape in this schema lives.
    ///
    /// The enumeration follows slang's own (FlowAnalysisBase::
    /// tryGetLoopIterValues): a local for the index, then step until the
    /// stop condition fails. The local is deleted before the body is walked
    /// -- left in place, every `a[i]` below would fold to whichever value
    /// the enumeration stopped at and claim an exact window from it.
    void describeIterationSpace(const ForLoopStatement& stmt,
                                const std::vector<LoopIndex>& indices,
                                BranchFrame& f) {
        if (indices.size() != 1 || !stmt.stopExpr || stmt.steps.empty())
            return;
        const ValueSymbol* var = indices.front().var;
        f.iterVar = var;
        if (bodyAssigns(stmt.body, var))
            return;
        // No start in the header, no space: taking the type's default would
        // claim an index begins at 0 when a preceding statement set it.
        if (!indices.front().init)
            return;
        ConstantValue start = indices.front().init->eval(eval);
        if (!start)
            return;
        ConstantValue* local = eval.createLocal(var, std::move(start));
        if (!local)
            return;
        struct DropLocal {
            EvalContext& eval;
            const ValueSymbol* var;
            ~DropLocal() { eval.deleteLocal(var); }
        } dropLocal{eval, var};

        std::vector<int64_t> values;
        for (;;) {
            ConstantValue stop = stmt.stopExpr->eval(eval);
            if (!stop || stop.hasUnknown())
                return;
            if (!stop.isTrue())
                break;
            if (values.size() >= kLoopDescribeSteps)
                return;             // longer than this description will run
            if (!local->isInteger())
                return;
            auto v = local->integer().as<int64_t>();
            if (!v)
                return;
            values.push_back(*v);
            for (auto* step : stmt.steps) {
                if (!step->eval(eval))
                    return;
            }
        }
        f.iterCount = int64_t(values.size());
        // A loop the stop condition rejects on the first test runs its body
        // never, which is the same fact about reachability an unreachable
        // arm carries -- and a dead-code filter reads one column, not two.
        if (values.empty())
            f.staticTaken = 0;
        // first/step describe the values only when they really are an
        // arithmetic progression: `i *= 2` is enumerable and not affine, and
        // a consumer substituting first + k*step there would be wrong.
        if (values.size() == 1) {
            f.hasProgression = true;
            f.iterFirst = values.front();
            f.iterStep = 0;
        }
        else if (values.size() >= 2) {
            const int64_t d = values[1] - values[0];
            bool affine = true;
            for (size_t i = 2; i < values.size() && affine; i++)
                affine = values[i] - values[i - 1] == d;
            if (affine) {
                f.hasProgression = true;
                f.iterFirst = values.front();
                f.iterStep = d;
            }
        }
    }

    // The loop handlers visit their condition under visitGuarded and then the
    // BODY, rather than handing the whole statement to visitDefault.
    // visitDefault walks the condition with bindable still true, so a call in
    // a loop condition -- `while (pred(x))` -- was attributed to whatever
    // statement happened to precede it, since curStmt there is a stale earlier
    // one. handle(ConditionalStatement) has always got this right; the loops
    // had not.
    void handle(const ForLoopStatement& stmt) {
        std::vector<LoopIndex> mine;
        registerLoopIndices(stmt, mine);
        BranchFrame f;
        f.kind = BranchKind::Loop;
        f.where = stmt.sourceRange;
        visitGuarded([&] {
            for (auto* init : stmt.initializers)
                init->visit(*this);
            if (stmt.stopExpr) {
                collectGating(*stmt.stopExpr, f.refs);
                stmt.stopExpr->visit(*this);
            }
            for (auto* step : stmt.steps)
                step->visit(*this);
        });
        describeIterationSpace(stmt, mine, f);
        {
            Level level(*this, std::move(f));
            stmt.body.visit(*this);
        }
        for (auto& idx : mine)
            loopVars.erase(idx.var);
    }

    /// `foreach` had no handler at all: its index leaked into every
    /// dependency of the body exactly as a `for`'s did, and the body carried
    /// no loop level. There is no guard expression here -- the dimension
    /// decides the count -- so what the level carries is its iteration
    /// space.
    void handle(const ForeachLoopStatement& stmt) {
        std::vector<const ValueSymbol*> mine;
        for (auto& dim : stmt.loopDims) {
            if (dim.loopVar && loopVars.insert(dim.loopVar).second)
                mine.push_back(dim.loopVar);
        }
        BranchFrame f;
        f.kind = BranchKind::Loop;
        f.where = stmt.sourceRange;
        // One dimension with a static range: the index runs it left to
        // right, which DESCENDS for the ordinary packed declaration --
        // `foreach (src[j])` over `logic [7:0] src` runs 7 down to 0, and
        // publishing 0 upward would mirror every window a consumer
        // reconstructs. More than one dimension and no single variable
        // describes the space.
        if (stmt.loopDims.size() == 1 && stmt.loopDims[0].loopVar &&
            stmt.loopDims[0].range) {
            auto& range = *stmt.loopDims[0].range;
            f.iterVar = stmt.loopDims[0].loopVar;
            f.iterFirst = range.left;
            f.iterStep = range.isDescending() ? -1 : 1;
            f.iterCount = int64_t(range.width());
            f.hasProgression = true;
        }
        {
            Level level(*this, std::move(f));
            visitDefault(stmt);
        }
        for (auto* v : mine)
            loopVars.erase(v);
    }

    /// `forever` has no guard to gate with -- that is what makes it forever
    /// -- so the level carries nothing but the fact that its statements are
    /// a loop body. Without it `while (1)` and `forever`, the same loop,
    /// recorded differently.
    void handle(const ForeverLoopStatement& stmt) {
        BranchFrame f;
        f.kind = BranchKind::Loop;
        f.where = stmt.sourceRange;
        Level level(*this, std::move(f));
        stmt.body.visit(*this);
    }

    void handle(const WhileLoopStatement& stmt) {
        BranchFrame f;
        f.kind = BranchKind::Loop;
        f.where = stmt.sourceRange;
        visitGuarded([&] {
            collectGating(stmt.cond, f.refs);
            stmt.cond.visit(*this);
        });
        Level level(*this, std::move(f));
        stmt.body.visit(*this);
    }

    /// `do … while (c)` had no handler at all, so it fell to visitDefault,
    /// which visits the condition -- and StatementWalker has no handler for a
    /// bare value expression, so nothing was recorded: not the gating, not
    /// even the read. The condition signal had zero load rows in the whole
    /// database despite being read every iteration.
    void handle(const DoWhileLoopStatement& stmt) {
        BranchFrame f;
        f.kind = BranchKind::Loop;
        f.where = stmt.sourceRange;
        visitGuarded([&] {
            collectGating(stmt.cond, f.refs);
            stmt.cond.visit(*this);
        });
        Level level(*this, std::move(f));
        stmt.body.visit(*this);
    }

    void handle(const RepeatLoopStatement& stmt) {
        BranchFrame f;
        f.kind = BranchKind::Loop;
        f.where = stmt.sourceRange;
        visitGuarded([&] {
            collectGating(stmt.count, f.refs);
            stmt.count.visit(*this);
        });
        // No index of its own, and the count is still the iteration space:
        // `repeat (4) q <= q + 1;` runs its body four times, which is the
        // one thing a reader of that single row needs and could not get.
        if (ConstantValue n = stmt.count.eval(eval);
            n && n.isInteger() && !n.hasUnknown()) {
            if (auto v = n.integer().as<int64_t>(); v && *v >= 0)
                f.iterCount = *v;
        }
        Level level(*this, std::move(f));
        stmt.body.visit(*this);
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
                                currentBranch(), expr.sourceRange, seq++,
                                /*blocking=*/true, 0, subDepth > 0,
                                pendingDelay, constructOverride});
        }
        visitDefault(expr);
    }

    /// `-> ev`. Without a row the event had loads and no cause at all,
    /// and the condition gating the trigger had no statement to hang on.
    void handle(const EventTriggerStatement& stmt) {
        std::vector<Ref> events;
        filteredConstants = 0;
        collectRefs(stmt.target, eval, events, /*skipSelectors=*/true);
        emit(TriggerNode{std::move(events), currentBranch(), seq++,
                         filteredConstants, stmt.sourceRange});
        visitDefault(stmt);
    }

    /// `disable blk`. It names a block, not a net, so the row exists for
    /// its gating: the condition reaching it is a read of that signal.
    void handle(const DisableStatement& stmt) {
        emit(ReadNode{{}, ReadNode::Kind::Disable, "disable", currentBranch(),
                      seq++, 0, stmt.sourceRange});
        visitDefault(stmt);
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

            // The formal is the assignment's other side: the actual's
            // elements are positioned in its bit space and paired against
            // it, so a concatenated actual reaches its own window instead
            // of claiming the whole formal, exactly as `{hi, lo} = x` does.
            Ref formalRef;
            formalRef.sym = formals[i];
            formalRef.cover = BitInterval::whole();
            formalRef.exact = true;
            const uint64_t fw = bitWidthOf(*formals[i]);
            const Slot formalSlot =
                fw ? Slot::at(formalRef, BitRange(0, fw - 1), true)
                   : Slot::unpositioned(formalRef);

            // Positioned from the unwrapped actual: an output actual is
            // args[i] wrapped in bindLValue's assignment, and a walk over
            // the wrapper positions nothing and claims no correspondence
            // even where the actual fills the formal exactly.
            std::vector<Slot> actualSlots;
            collectSlots(*actualExpr, eval, 0, actualSlots, /*skipSelectors=*/writes);
            // A constant actual leaves no slot to pair, and the formal
            // then had no driver at all -- while the same tie-off
            // written as a port connection records one. The formal is
            // tied off; say so.
            //
            // Constant, not merely nameless: `t($urandom())` also yields no
            // slot, and it holds the formal at no value at all. Calling
            // that a tie-off states the one thing `constant` means and the
            // one thing it is not.
            if (reads && actualExpr->eval(eval) &&
                std::none_of(actualSlots.begin(), actualSlots.end(),
                             [](const Slot& s) { return s.ref.sym != nullptr; })) {
                emit(BindNode{formals[i], args[i], PairedSrc{{}, formalSlot.ref,
                                                            false, {}},
                              reads, writes, bindable,
                              /*constantActual=*/true, expr.sourceRange});
            }
            for (auto& as : actualSlots) {
                if (!as.ref.sym)
                    continue;
                std::optional<BitRange> span;
                if (!slotsOverlap(formalSlot, as, span))
                    continue;
                PairedSrc pair{narrowed(as, span), narrowed(formalSlot, span),
                               formalSlot.positional && as.positional,
                               as.ref};
                emit(BindNode{formals[i], args[i], std::move(pair), reads,
                              writes, bindable, /*constantActual=*/false,
                              expr.sourceRange});
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
        emit(ReleaseNode{std::move(writes), s.isRelease, currentBranch(), seq++,
                         filteredConstants, s.sourceRange});
        visitDefault(s);
    }

    /// `case … matches` had no handler, so it fell to visitDefault and
    /// recorded nothing at all -- not the gating, not even a read of the
    /// controlling expression, which had zero load rows in the whole database.
    /// Modelled on handle(CaseStatement): the subject and each item's filter
    /// are conditions, the item bodies are not.
    void handle(const PatternCaseStatement& stmt) {
        BranchFrame point;
        point.kind = BranchKind::Case;
        // `matches` whatever wildcard word it carries: the items are
        // patterns, so they have no label values, and that is what a
        // consumer must branch on.
        point.caseKind = CaseKind::Matches;
        point.check = checkOf(stmt.check);
        point.where = stmt.sourceRange;
        std::vector<BranchFrame> arms;
        visitGuarded([&] {
            collectGating(stmt.expr, point.refs);
            stmt.expr.visit(*this);
            for (auto& item : stmt.items) {
                BranchFrame arm;
                arm.kind = BranchKind::CaseItem;
                arm.ordinal = int32_t(arms.size());
                arm.where = item.stmt->sourceRange;
                if (item.filter) {
                    collectGating(*item.filter, arm.refs);
                    item.filter->visit(*this);
                    if (gatesNothing(*item.stmt)) {
                        point.refs.insert(point.refs.end(), arm.refs.begin(),
                                          arm.refs.end());
                        arm.refs.clear();
                    }
                }
                arms.push_back(std::move(arm));
            }
        });

        Level pointLevel(*this, std::move(point));
        for (size_t i = 0; i < stmt.items.size(); i++) {
            Level armLevel(*this, std::move(arms[i]));
            currentBranch();
            stmt.items[i].stmt->visit(*this);
        }
        if (stmt.defaultCase) {
            BranchFrame arm;
            arm.kind = BranchKind::CaseDefault;
            arm.ordinal = int32_t(arms.size());
            arm.where = stmt.defaultCase->sourceRange;
            Level armLevel(*this, std::move(arm));
            currentBranch();
            stmt.defaultCase->visit(*this);
        }
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
        dropLoopVars(rhsSlots);
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
        emit(AssignmentNode{{TargetRecord{dst, std::move(pairs)}}, currentBranch(),
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
        dropLoopVars(rhsSlots);
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
            emit(AssignmentNode{std::move(records), currentBranch(), expr.sourceRange,
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
