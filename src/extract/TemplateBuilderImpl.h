// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// The pass-1 builder itself. Private to TemplateBuilder.cpp and
// TemplateBuilder_Conn.cpp -- extract/TemplateBuilder.h is the interface, and
// nothing else should include this.
//
// The class is split across two translation units because it does two
// separable things: reading a body (its declarations, statements and
// dataflow) and reading what that body wires its children to. The second is
// 408 lines that touch the first at three points -- buildTemplate calls
// buildChildren, and two connection paths record an outward reference -- so
// the seam is thin, but it is not a seam that separates state, which is why
// this stays one class rather than becoming two.
//
// Declarations here, definitions there. For methods averaging 65 lines, with
// several over 150, that also buys the thing a 2,000-line class most wants: a
// list of what it does, in one screen.

#pragma once

#include "extract/TemplateBuilder.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "slang/analysis/AnalysisManager.h"
#include "slang/analysis/AnalyzedProcedure.h"
#include "slang/analysis/ValueDriver.h"
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/Expression.h"
#include "slang/ast/HierarchicalReference.h"
#include "slang/ast/Scope.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/ValuePath.h"
#include "slang/ast/expressions/AssertionExpr.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
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
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/NetType.h"
#include "slang/ast/types/Type.h"
#include "slang/numeric/ConstantValue.h"
#include "slang/numeric/SVInt.h"
#include "slang/syntax/SyntaxNode.h"
#include "slang/text/SourceManager.h"
#include "extract/DeclIndex.h"
#include "extract/Ref.h"
#include "extract/SymbolText.h"

using namespace slang;
using namespace slang::ast;
using namespace slang::analysis;

namespace designdb::detail {

class TemplateBuilder {
public:
    TemplateBuilder(Compilation& comp, AnalysisManager& mgr, Writer& w,
                    SourceLocator& loc, Stats& s) :
        compilation(comp), analysis(mgr), writer(w), locator(loc), stats(s),
        sourceManager(*comp.getSourceManager()) {}

    /// Groups every instance, writes the module rows, then builds each group's
    /// terminals and finally its full template.
    TemplateSet run();

private:
    struct Group {
        std::string name;
        std::string params;
        std::vector<std::pair<std::string, std::string>> paramPairs;
        const InstanceBodySymbol* body = nullptr;
    };

    /// Per-build state that does not belong in the finished template.
    struct Build {
        Template* t = nullptr;
        const InstanceBodySymbol* body = nullptr;
        /// The declaration index for this body. Owns what used to be
        /// netOf/scopeOf, and is the only thing here that hands out net
        /// and scope indices.
        DeclIndex* decl = nullptr;
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
        int32_t curCallSite = -1;  // the call-site body walk in force (-1 = top)
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

    /// One net a connection expression attaches, with its window in the
    /// formal -- the boundary twin of Slot. Ported from v9.
    struct ConnRef {
        Ref ref;
        bool expression = false;
        uint64_t winLo = 0, winHi = 0;
        bool windowExact = false;
        bool positional = false;
    };

    /// The body to extract a group's dataflow from: one the analysis manager
    /// actually analysed, else the first seen.
    void offer(Group& g, const InstanceBodySymbol& body);

    /// The key that decides which occurrences share a template: (definition
    /// identity, parameter values). NOT the definition NAME -- two libraries
    /// may define one name (which is why `module` keys on (name, file,
    /// line), not name), and a name+params string would fold their distinct
    /// bodies into one group, then stamp every occurrence of both from
    /// whichever body was analysed. The source location is the definition's
    /// stable identity; a raw pointer would be unique too but would make the
    /// module-id assignment order (a walk over `groups`, sorted by this key)
    /// depend on the address, and the export must be reproducible. Every
    /// component is length-prefixed, so a string parameter whose value holds
    /// a delimiter cannot alias a different split -- the old bare
    /// `name=value` join could.
    std::string groupKey(const InstanceBodySymbol& body) const;

    void collect(const InstanceSymbol& inst);

    // ---------------------------------------------------------- terminals

    /// The port list of one group, as terminal templates. A MultiPort (a
    /// non-ANSI `.p({hi, lo})` formal) is one terminal; its inside is the
    /// term_map segments built later.
    void buildTerms(Template& t, const InstanceBodySymbol& body);

    // ----------------------------------------------------- statement rows

    int32_t newStmt(Build& b, std::string kind, std::string construct,
                    std::string assignKind, int64_t seq, std::string delay,
                    int64_t dropped, const TplLoc& loc);

    int32_t addExprRef(Build& b, int32_t stmt, const Ref& r, std::string role,
                       int32_t netIdx);

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
                       const Ref* asWritten = nullptr);

    /// How to reach the reference's target from an occurrence. Downward
    /// targets replay inside the occurrence's own subtree; absolute ones
    /// replay from the root; a reference through one of this template's own
    /// interface terminals replays from whatever instance the terminal is
    /// bound to in that occurrence. Upward references (upwardCount > 0) stay
    /// unresolved -- the one analysed body speaks for occurrences whose
    /// upward surroundings may differ, and a guess is worse than a NULL.
    void fillResolution(Build& b, TplHierRef& row, const Ref& r);

    static bool splitBelow(const std::string& full, const std::string& prefix,
                           std::string& rel);

    /// Splits `rel` into the tree segments that lead to the target's
    /// instance and the scope-relative net name inside it. The instance
    /// chain is recovered from the target symbol's own ancestry: every
    /// enclosing InstanceSymbol contributes its path segments.
    void splitSegsAndNet(const std::string& rel, const Symbol& target,
                         TplHierRef& row);

    // ----------------------------------------------------- template build

    void buildTemplate(Template& t, const InstanceBodySymbol& body);

    /// The inside of each terminal: which nets it stands for. An ANSI port
    /// maps whole-to-whole onto its internal symbol; a non-ANSI port
    /// expression and a MultiPort produce one segment per element with its
    /// window, through the same machinery the outside uses.
    void buildTermMaps(Build& b, const InstanceBodySymbol& body);

    // One procedure: its row, its sensitivity, and its statements.
    void buildProcedure(Build& b, const AnalyzedProcedure& proc);

    /// The lvalue a release/deassign lets go of: a real stmt_target row
    /// -- or a hier_ref with access='write' for a name outside this
    /// instance -- and deliberately NO dependency. Nothing is driven; the
    /// row answers "where does the force end", never "who drives this".
    void recordReleaseTarget(Build& b, int32_t stmt, const Ref& r,
                             const TplLoc& at, EvalContext& evalCtx);

    /// The target of a system task's write: a real stmt_target plus a
    /// source-less dependency, so the argument has a driver and the
    /// procedure is not mistaken for one that wrote nothing. A target
    /// outside this instance is a hier_ref with access='write', as
    /// everywhere else.
    void recordSystemWrite(Build& b, int32_t stmt, const Ref& r,
                           const TplLoc& at, EvalContext& evalCtx);

    /// One read of a statement, wherever it lands: an expr_ref for a net of
    /// this instance, a hier_ref for anything outside it.
    void recordRead(Build& b, int32_t stmt, const Ref& r, const std::string& role,
                    const TplLoc& at, EvalContext& evalCtx);

    void addProcEvent(Build& b, int32_t procIdx, int32_t stmtIdx,
                      const Expression* expr, const std::string& edge,
                      const std::string& eventKind, const TplLoc& at,
                      EvalContext& evalCtx,
                      const std::function<int32_t()>& readStmt);

    /// One target of one assignment statement, with its statement row on the
    /// first target, its operand rows, and the dependencies that pair them.
    void emitAssignment(Build& b, const Ref& dst,
                        const std::vector<PairedSrc>& pairs,
                        const std::vector<Ref>& gating, const TplLoc& at,
                        int64_t seq, bool blocking, int64_t dropped,
                        bool inSubroutine, bool firstTarget,
                        const std::string& delay, bool isContinuous,
                        const std::string& construct, EvalContext& evalCtx);

    /// One call binding: the actual and the formal coupled by argument
    /// direction. The formal is a subroutine-scope net (`bump.v`); the
    /// body's own statements belong to the calling procedure, and are
    /// walked once per call site so each carries its caller's gating.
    void emitCallBinding(Build& b, const Ref& formal, const Ref& actual,
                         bool reads, bool writes, bool oneToOne, bool bindable,
                         const TplLoc& at, EvalContext& evalCtx);

    /// `wire w = a & b;` -- the LRM's continuous assignment spelled as a
    /// declaration, through the same slot machinery as `assign`.
    void buildNetInitialisers(Build& b, const InstanceBodySymbol& body);

    /// `alias a = b;` binds nets into one object.
    ///
    /// It is not an assignment and has no direction: neither side drives
    /// the other, they ARE each other. So every reference is recorded as
    /// both a target and an operand, and every ordered pair gets a
    /// dependency -- which is what makes each side appear as the other's
    /// driver and the other's load. An N-way `alias a = b = c;` is N
    /// references and N*(N-1) dependencies, since the LRM binds all of
    /// them mutually rather than in a chain.
    ///
    /// The kind is its own. Exporting a pair of continuous assignments
    /// would have answered the connectivity questions correctly and made
    /// every multiple-driver query wrong, since an alias contributes no
    /// driver at all.
    void buildNetAliases(Build& b, const InstanceBodySymbol& body);

    /// Gate, switch and UDP instances: a tree node, a primitive row, and one
    /// dependency per LRM (input, output) pairing.
    void buildPrimitives(Build& b, const InstanceBodySymbol& body);

    void addHierRefIfOutward(Build& b, const Ref& r, EvalContext& evalCtx);

    // ------------------------------------------------ connection templates

    static void collectConnRefs(const Expression& expr, EvalContext& ctx,
                                std::vector<ConnRef>& out, uint64_t base = 0,
                                bool degraded = false);

    /// The children of one body and their connection templates. Children are
    /// recorded in traversal order -- the stamping invariant -- with their
    /// scope index and ONE path segment each (array elements carry their
    /// `[i]` in the segment, exactly as the tree spells them).
    void buildChildren(Build& b, const InstanceBodySymbol& body);

    void registerChildren(Build& b, const Scope& scope, int32_t scopeIdx,
                          std::unordered_map<const Symbol*, int32_t>& childOf,
                          std::vector<const Symbol*>& childSyms);

    /// One resolved child's connection templates: the outside of each of its
    /// terminals, as written here in the parent.
    void buildInstanceConns(Build& b, const InstanceSymbol& child, TplChild& c,
                            const std::unordered_map<const Symbol*, int32_t>& childOf);

    /// The parent's own interface port that `iface` arrived through, if any.
    static const InterfacePortSymbol* passedThrough(const InstanceBodySymbol& body,
                                                    const Symbol* iface);

    /// Terminals and connections of a black box: one terminal per named
    /// connection, direction unknown, so `net_conn` has something to bind
    /// and "connected to a black box" stays distinct from "unconnected".
    void buildUnresolvedConns(Build& b, const UninstantiatedDefSymbol& u,
                              TplChild& c);

    // ------------------------------------------------------------ modules

    void internModuleRow(const DefinitionSymbol& def);

    // ------------------------------------------------------------ members

    Compilation& compilation;
    AnalysisManager& analysis;
    Writer& writer;
    SourceLocator& locator;
    Stats& stats;
    /// For whereOf and normalizedText, which read source text directly rather
    /// than going through the locator's interning.
    const SourceManager& sourceManager;
    std::map<std::string, Group> groups;
    std::unordered_map<const InstanceSymbol*, std::string> instanceGroup;
    std::map<std::string, Template> templates;
    std::unordered_map<const DefinitionSymbol*, int64_t> moduleIds;
};

} // namespace designdb::detail
