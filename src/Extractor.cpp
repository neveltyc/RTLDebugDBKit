// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)

#include "Extractor.h"
#include "extract/DeclIndex.h"
#include "extract/Ref.h"
#include "extract/SourceLocator.h"
#include "extract/StatementWalker.h"
#include "extract/SymbolText.h"
#include "extract/Template.h"

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

// The leaf layers this file was built out of. `using namespace detail`
// rather than qualifying every use: the code below is moved, not
// rewritten, and it named these unqualified all along.
using namespace detail;

// -------------------------------------------------------------- the walker

class Walker {
public:
    Walker(Compilation& comp, AnalysisManager& mgr, Writer& w) :
        compilation(comp), analysis(mgr), writer(w),
        sourceManager(*comp.getSourceManager()),
        locator(sourceManager, w) {}

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
            t.paramPairs = group.paramPairs;
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

        // Packages after the module tree: their nets must exist before
        // hier_ref resolution binds `pkg::x` references to them.
        stampPackages();

        // Hierarchical references last: an absolute path may land in a
        // subtree stamped after the referring occurrence.
        resolveHierRefs();

        writer.linkSourceFiles(locator.origins());
        return stats;
    }

private:
    struct Group {
        std::string name;
        std::string params;
        std::vector<std::pair<std::string, std::string>> paramPairs;
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
    std::string groupKey(const InstanceBodySymbol& body) const {
        auto& def = body.getDefinition();
        Where w = whereOf(def.location, sourceManager);
        std::string key;
        auto add = [&](std::string_view s) {
            key += std::to_string(s.size());
            key += ':';
            key += s;
        };
        add(def.name);
        add(w.file);
        add(std::to_string(w.line));
        add(std::to_string(w.column));
        for (auto& [name, value] : parameterPairs(body)) {
            add(name);
            add(value);
        }
        return key;
    }

    void collect(const InstanceSymbol& inst) {
        auto& body = inst.getCanonicalBody() ? *inst.getCanonicalBody() : inst.body;
        auto key = groupKey(body);
        auto& g = groups[key];
        if (g.name.empty()) {
            g.name = std::string(body.getDefinition().name);
            g.paramPairs = parameterPairs(body);
            g.params = parameterText(body);
        }
        offer(g, body);
        instanceGroup[&inst] = key;
        forEachInstance(inst.body, [&](const InstanceSymbol& child) { collect(child); });
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
            term.loc = locator.locate(portSym->location);
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
        s.callSite = b.curCallSite;
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
        // A package item -- `pkg::mask`, or a bare `mask` imported from one.
        // slang resolves the `::` at compile time, so this is a NamedValue,
        // not a HierarchicalValue: it is caught here on the symbol's own
        // scope, before the HierarchicalValue gate below. It resolves per
        // occurrence once packages are stamped as pseudo-occurrences; segs[0]
        // carries the package name, netName the member. ($unit compilation-
        // unit items are not stamped yet, so they fall through and stay
        // external -- no worse than before.)
        if (r.sym) {
            if (auto* scope = r.sym->getParentScope()) {
                auto& owner = scope->asSymbol();
                if (owner.kind == SymbolKind::Package && !owner.name.empty() &&
                    !r.sym->name.empty()) {
                    row.resolve = TplHierRef::Package;
                    row.segs = {std::string(owner.name)};
                    row.netName = std::string(r.sym->name);
                    return;
                }
            }
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
        // The index first: its constructor establishes scope 0, and nothing
        // below can name a net or a scope until it exists.
        DeclIndex decl(t, body, body.getHierarchicalPath(), locator, writer);

        Build b;
        b.t = &t;
        b.body = &body;
        b.prefix = body.getHierarchicalPath();
        b.decl = &decl;

        b.decl->collectDeclarations(body, 0);
        buildTermMaps(b, body);

        if (auto* scope = analysis.getAnalyzedScope(body)) {
            for (auto& proc : scope->procedures)
                buildProcedure(b, proc);
        }
        buildNetInitialisers(b, body);
        buildNetAliases(b, body);
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
                        b.decl->netFor(p.internalSymbol->as<ValueSymbol>());
                    addSeg(netIdx, TplRange{}, TplRange{}, true);
                }
                else if (auto* inner = p.getInternalExpr()) {
                    std::vector<ConnRef> segs;
                    collectConnRefs(*inner, evalCtx, segs);
                    for (auto& cn : segs) {
                        if (!cn.ref.sym)
                            continue;
                        const int32_t netIdx = b.decl->netFor(*cn.ref.sym);
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
                    addSeg(b.decl->netFor(vs), termR, TplRange{}, mapping);
                }
            }
            // InterfacePort: no nets behind it, no map rows.
        }
    }

    // One procedure: its row, its sensitivity, and its statements.
    void buildProcedure(Build& b, const AnalyzedProcedure& proc) {
        const Symbol& sym = *proc.analyzedSymbol;
        const bool isContinuous = sym.kind == SymbolKind::ContinuousAssign;
        const TplLoc procAt = locator.locate(sym.location);
        EvalContext evalCtx(sym);

        std::string construct;
        if (isContinuous)
            construct = "assign";
        else
            construct = procedureWord(sym);

        int32_t procIdx = -1;
        if (!isContinuous) {
            TplProcedure p;
            p.scope = b.decl->scopeForSymbol(sym);
            p.kind = construct == "assign" ? "always" : construct;
            p.loc = procAt;
            procIdx = int32_t(b.t->procedures.size());
            b.t->procedures.push_back(std::move(p));
        }
        b.curProc = procIdx;
        b.curScope = b.decl->scopeForSymbol(sym);
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
                bool firstTarget, const std::string& delay,
                const char* constructWord) {
                reached = true;
                if (!dst.sym || inputPorts.count(dst.sym))
                    return;
                const TplLoc at = locator.locate(where.start(), procAt);
                emitAssignment(b, dst, pairs, gating, at, seq, blocking,
                               dropped, inSubroutine, firstTarget, delay,
                               isContinuous,
                               constructWord ? constructWord : construct,
                               evalCtx);
            },
            // ---- a call site's actual bound to its formal
            [&](const Ref& formal, const Ref& actual, bool reads, bool writes,
                bool oneToOne, bool bindable, SourceRange where) {
                reached = true;
                emitCallBinding(b, formal, actual, reads, writes, oneToOne,
                                bindable, locator.locate(where.start(), procAt), evalCtx);
            },
            // ---- a statement-level event control (a wait)
            [&](const Expression* e, const std::string& edge, int64_t seq,
                SourceRange where) {
                if (procIdx < 0)
                    return;
                const TplLoc at = locator.locate(where.start(), procAt);
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
                const TplLoc at = locator.locate(where.start(), procAt);
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
                    // now models. A release is the opposite: it names its
                    // lvalue and drives nothing, so it gets a target row
                    // and deliberately no dependency.
                    reached = true;
                    if (stmtKind == "release")
                        recordReleaseTarget(b, s, w, at, evalCtx);
                    else
                        recordSystemWrite(b, s, w, at, evalCtx);
                }
            },
            evalCtx);
        walker.sensitivityTiming = sens.timingControl;
        walker.budget = &b.callBudget;
        walker.truncated = &b.truncatedCalls;
        walker.callSiteSlot = &b.curCallSite;
        walker.allocCallSite = [&b](const SubroutineSymbol& sub, int64_t depth,
                                    bool bindable) -> int32_t {
            TplCallSite cs;
            // NULL for a call in a control expression (`if (f())`): it has no
            // owning statement, and b.curStmt there is a stale earlier one.
            cs.callerStmt = bindable ? b.curStmt : -1;
            cs.parentCallSite = b.curCallSite; // the enclosing expansion
            cs.subName = std::string(sub.name);
            cs.depth = depth;
            const int32_t idx = int32_t(b.t->callSites.size());
            b.t->callSites.push_back(std::move(cs));
            return idx;
        };
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

    /// The lvalue a release/deassign lets go of: a real stmt_target row
    /// -- or a hier_ref with access='write' for a name outside this
    /// instance -- and deliberately NO dependency. Nothing is driven; the
    /// row answers "where does the force end", never "who drives this".
    void recordReleaseTarget(Build& b, int32_t stmt, const Ref& r,
                             const TplLoc& at, EvalContext& evalCtx) {
        if (!r.sym)
            return;
        const int32_t netIdx = b.decl->netFor(*r.sym);
        if (netIdx < 0) {
            const int32_t saved = b.curStmt;
            b.curStmt = stmt;
            addHierRef(b, true, r, at, evalCtx);
            b.curStmt = saved;
            return;
        }
        TplStmtRef tr;
        tr.stmt = stmt;
        tr.ordinal = b.targetOrdinal++;
        tr.net = netIdx;
        tr.r = rangeOf(r);
        b.t->targets.push_back(std::move(tr));
    }

    /// The target of a system task's write: a real stmt_target plus a
    /// source-less dependency, so the argument has a driver and the
    /// procedure is not mistaken for one that wrote nothing. A target
    /// outside this instance is a hier_ref with access='write', as
    /// everywhere else.
    void recordSystemWrite(Build& b, int32_t stmt, const Ref& r,
                           const TplLoc& at, EvalContext& evalCtx) {
        if (!r.sym)
            return;
        const int32_t netIdx = b.decl->netFor(*r.sym);
        if (netIdx < 0) {
            // `$readmemh("f.hex", u.mem)` -- the task drives a memory in
            // another instance. Recording only the reference left that
            // memory with no driver at all, so a trace back from whatever
            // reads it stopped dead one step later.
            const int32_t saved = b.curStmt;
            b.curStmt = stmt;
            const int32_t href = addHierRef(b, true, r, at, evalCtx);
            b.curStmt = saved;
            if (href < 0)
                return;
            TplCrossDep d;
            d.kind = "data";
            d.sourceless = true;
            d.stmt = stmt;
            d.tgtHref = href;
            d.tgtR = rangeOf(r);
            d.callSite = b.curCallSite;
            b.t->crossDeps.push_back(std::move(d));
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
        d.callSite = b.curCallSite;
        b.t->deps.push_back(std::move(d));
    }

    /// One read of a statement, wherever it lands: an expr_ref for a net of
    /// this instance, a hier_ref for anything outside it.
    void recordRead(Build& b, int32_t stmt, const Ref& r, const std::string& role,
                    const TplLoc& at, EvalContext& evalCtx) {
        if (!r.sym)
            return;
        const int32_t netIdx = b.decl->netFor(*r.sym);
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
            netIdx = b.decl->netFor(vs);
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
                const int32_t netIdx = b.decl->netFor(*g.sym);
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
        int32_t dstNet = b.decl->netFor(*dst.sym);
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
            const int32_t srcNet = b.decl->netFor(*p.src.sym);
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
                // whole target: the `stmt_target` row above still spans
                // everything the statement writes.
                d.tgtR = rangeOf(p.tgt);
                d.mappingExact = p.mapExact ? 1 : 0;
                d.callSite = b.curCallSite;
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
                d.callSite = b.curCallSite;
                b.t->crossDeps.push_back(std::move(d));
            }
        }
        // `q <= 8'h0`: nothing at all reaches the target, and the
        // null-source row records the driving statement. A target whose
        // sources are all OUTWARD is not that -- its drivers are the
        // cross-instance rows above, and claiming a constant here was a
        // wrong fact, not a conservative one.
        if (haveTarget && !anySource) {
            if (targetIdx >= 0) {
                TplDep d;
                d.srcNet = -1;
                d.tgtNet = dstNet;
                d.stmt = stmt;
                d.targetRef = targetIdx;
                d.kind = "data";
                d.tgtR = rangeOf(dst);
                d.callSite = b.curCallSite;
                b.t->deps.push_back(std::move(d));
            }
            else {
                // The target is in another instance: `assign u.x = 8'h5A;`.
                // Gating the constant row on a LOCAL target left every
                // outward constant write with no driver whatsoever, so a
                // trace back from the far net said nothing wrote it.
                TplCrossDep d;
                d.kind = "data";
                d.sourceless = true;
                d.stmt = stmt;
                d.tgtHref = tgtHref;
                d.tgtR = rangeOf(dst);
                d.callSite = b.curCallSite;
                b.t->crossDeps.push_back(std::move(d));
            }
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
                    const int32_t srcNet = b.decl->netFor(*src.sym);
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
                    d.callSite = b.curCallSite;
                    b.t->deps.push_back(std::move(d));
                }
                else {
                    TplCrossDep d;
                    d.kind = "control";
                    d.stmt = stmt;
                    d.srcNet = ctrlRef >= 0 ? b.decl->netFor(*src.sym) : -1;
                    d.srcHref = ctrlHref;
                    d.tgtNet = dstNet;
                    d.tgtHref = tgtHref;
                    d.exprRef = ctrlRef;
                    d.targetRef = targetIdx;
                    d.srcR = rangeOf(src);
                    d.tgtR = rangeOf(dst);
                    d.mappingExact = 0;
                    d.callSite = b.curCallSite;
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
        const int32_t formalNet = b.decl->netFor(*formal.sym);
        if (formalNet < 0)
            return;
        const int32_t stmt = bindable ? b.curStmt : -1;
        const int32_t actualNet = b.decl->netFor(*actual.sym);
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
                d.callSite = b.curCallSite;
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
                d.callSite = b.curCallSite;
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
            d.callSite = b.curCallSite;
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
            d.callSite = b.curCallSite;
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
            const TplLoc at = locator.locate(net.location);
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
    void buildNetAliases(Build& b, const InstanceBodySymbol& body) {
        EvalContext evalCtx(body);
        b.curProc = -1;
        forEachOfKind<SymbolKind::NetAlias, NetAliasSymbol>(
            body, [&](const NetAliasSymbol& al) {
            auto refs = al.getNetReferences();
            if (refs.size() < 2)
                return;
            const TplLoc at = locator.locate(al.location);
            b.curScope = 0;
            const int32_t stmt = newStmt(b, "alias", "alias", std::string(),
                                         /*seq=*/-1, std::string(), 0, at);
            if (stmt < 0)
                return;

            // One target and one operand per reference, in written order.
            struct Side { int32_t target = -1; int32_t operand = -1;
                          int32_t net = -1; Ref ref; };
            std::vector<Side> sides;
            for (auto* e : refs) {
                if (!e)
                    continue;
                std::vector<Ref> got;
                collectRefs(*e, evalCtx, got, /*skipSelectors=*/true);
                if (got.size() != 1 || !got[0].sym)
                    continue;
                Side sd;
                sd.ref = got[0];
                sd.net = b.decl->netFor(*sd.ref.sym);
                if (sd.net < 0) {
                    // Nothing in this instance to bind; the reference is
                    // the record, as everywhere else.
                    const int32_t saved = b.curStmt;
                    b.curStmt = stmt;
                    addHierRef(b, true, sd.ref, at, evalCtx);
                    b.curStmt = saved;
                    continue;
                }
                TplStmtRef tr;
                tr.stmt = stmt;
                tr.ordinal = b.targetOrdinal++;
                tr.net = sd.net;
                tr.r = rangeOf(sd.ref);
                sd.target = int32_t(b.t->targets.size());
                b.t->targets.push_back(std::move(tr));

                TplStmtRef orow;
                orow.stmt = stmt;
                orow.ordinal = b.operandOrdinal++;
                orow.net = sd.net;
                orow.r = rangeOf(sd.ref);
                sd.operand = int32_t(b.t->operands.size());
                b.t->operands.push_back(std::move(orow));
                sides.push_back(std::move(sd));
            }

            for (size_t i = 0; i < sides.size(); i++) {
                for (size_t j = 0; j < sides.size(); j++) {
                    if (i == j)
                        continue;
                    TplDep d;
                    d.srcNet = sides[i].net;
                    d.tgtNet = sides[j].net;
                    d.stmt = stmt;
                    d.operandRef = sides[i].operand;
                    d.targetRef = sides[j].target;
                    d.kind = "alias";
                    d.srcR = rangeOf(sides[i].ref);
                    d.tgtR = rangeOf(sides[j].ref);
                    // An alias is bit for bit by definition; it is only
                    // coarse if a side could not be narrowed.
                    d.mappingExact =
                        (sides[i].ref.exact && sides[j].ref.exact) ? 1 : 0;
                    d.callSite = b.curCallSite;
                    b.t->deps.push_back(std::move(d));
                }
            }
            b.curStmt = -1;
        });
    }

    /// Gate, switch and UDP instances: a tree node, a primitive row, and one
    /// dependency per LRM (input, output) pairing.
    void buildPrimitives(Build& b, const InstanceBodySymbol& body) {
        EvalContext evalCtx(body);
        std::unordered_map<int32_t, int> anonPrims;
        forEachOfKind<SymbolKind::PrimitiveInstance, PrimitiveInstanceSymbol>(
            body, [&](const PrimitiveInstanceSymbol& prim) {
            auto conns = prim.getPortConnections();
            auto& def = prim.primitiveType;
            TplPrim p;
            p.scope = b.decl->scopeForSymbol(prim);
            // A gate may be written without an instance name -- `buf (y, a);`
            // is legal and usual in cell models. Such a symbol has no name of
            // its own, so its hierarchical path ends at its PARENT, and
            // taking the last segment gave every anonymous gate the name of
            // the instance holding it: four gates in one cell became four
            // siblings called after the cell, and resolving that path
            // segment returned all of them plus the instance itself.
            //
            // Anonymous gates get a synthesised segment instead, counted per
            // scope so siblings differ, and prefixed with '$' so it cannot
            // collide with an identifier the source could have written.
            std::string name(prim.name);
            if (name.empty()) {
                auto& n = anonPrims[p.scope];
                name = "$" + std::string(def.name) + "$" + std::to_string(n++);
            }
            p.name = name;
            // slang labels only tran/tranif* as BiDiSwitch; the resistive
            // variants and the whole MOS family register as Fixed like any
            // gate, so prim_kind='switch' silently missed rtran and nmos.
            // The LRM's own switch list (28.7-28.8) decides instead.
            static const std::set<std::string_view> kSwitches = {
                "nmos", "pmos", "rnmos", "rpmos", "cmos", "rcmos",
                "tran", "rtran", "tranif0", "tranif1", "rtranif0", "rtranif1"};
            p.primKind = def.primitiveKind == PrimitiveSymbol::UserDefined
                             ? "udp"
                             : kSwitches.count(def.name) ? "switch" : "gate";
            p.defName = std::string(def.name);
            p.loc = locator.locate(prim.location);
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
                const int32_t dstNet = dst.sym ? b.decl->netFor(*dst.sym) : -1;
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
                    const int32_t srcNet = src.sym ? b.decl->netFor(*src.sym) : -1;
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
                    d.callSite = b.curCallSite;
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
                    d.callSite = b.curCallSite;
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
                    c.loc = locator.locate(inst.location);
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
                    c.loc = locator.locate(u.location);
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
                    registerChildren(b, block, b.decl->scopeIndexOf(block, scopeIdx),
                                     childOf, childSyms);
                    break;
                }
                case SymbolKind::GenerateBlockArray:
                    for (auto& entry :
                         member.as<GenerateBlockArraySymbol>().entries) {
                        registerChildren(b, *entry,
                                         b.decl->scopeIndexOf(entry->asSymbol(), scopeIdx),
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

    /// One resolved child's connection templates: the outside of each of its
    /// terminals, as written here in the parent.
    void buildInstanceConns(Build& b, const InstanceSymbol& child, TplChild& c,
                            const std::unordered_map<const Symbol*, int32_t>& childOf) {
        auto& childBody = child.getCanonicalBody() ? *child.getCanonicalBody()
                                                   : child.body;
        Template& childT = templates[groupKey(childBody)];
        EvalContext evalCtx(child);
        const TplLoc instAt = locator.locate(child.location);
        bool inArray = false;
        if (auto* ps = child.getParentScope())
            inArray = ps->asSymbol().kind == SymbolKind::InstanceArray;

        std::unordered_map<int32_t, int64_t> segOrdinal;
        for (auto* conn : child.getPortConnections()) {
            if (!conn)
                continue;
            const Expression* connExpr = conn->getExpression();
            const TplLoc at = connExpr
                                  ? locator.locate(connExpr->sourceRange.start(), instAt)
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
                const int32_t netIdx = b.decl->netFor(*cn.ref.sym);
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
                    // An external tie has a formal to measure against like
                    // any other connection to a resolved child, so it
                    // states its mapping by the same rule -- v11 left it
                    // NULL and the crossing arc reported 0 even where both
                    // windows were exact, which is what kept those ties
                    // untraceable bit by bit.
                    if (!cn.expression)
                        tc.mappingExact = cn.positional && !inArray &&
                                                  !widthMismatch
                                              ? 1
                                              : 0;
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
            const TplLoc at = locator.locate(u.location);
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
                const int32_t netIdx = b.decl->netFor(*cn.ref.sym);
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
        const TplLoc at = locator.locate(def.location);
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
                exprRef = 0, procEvent = 0, dep = 0, hierRef = 0, callSite = 0;
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
        for (size_t pi = 0; pi < t.paramPairs.size(); pi++)
            writer.addInstParam({nodeId, int64_t(pi), t.paramPairs[pi].first,
                                 t.paramPairs[pi].second});
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
        base.callSite = callSiteCounter;
        callSiteCounter += int64_t(t.callSites.size());

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

        // Call sites first: a stmt row names the site it belongs to, so the
        // site ids must be issued before the statements reference them.
        for (size_t i = 0; i < t.callSites.size(); i++) {
            auto& cs = t.callSites[i];
            CallSiteRow row;
            row.id = base.callSite + int64_t(i) + 1;
            row.instId = instId;
            row.callerStmtId = cs.callerStmt < 0 ? 0 : base.stmt + cs.callerStmt + 1;
            row.parentCallSiteId =
                cs.parentCallSite < 0 ? 0 : base.callSite + cs.parentCallSite + 1;
            row.subroutineName = cs.subName;
            row.depth = cs.depth;
            writer.addCallSite(row);
        }
        stats.callSites += int64_t(t.callSites.size());

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
            row.callSiteId = s.callSite < 0 ? 0 : base.callSite + s.callSite + 1;
            row.fileId = s.loc.fileId;
            row.line = s.loc.line;
            row.column = s.loc.column;
            writer.addStmt(row);
        }
        stats.stmts += int64_t(t.stmts.size());

        for (size_t i = 0; i < t.targets.size(); i++) {
            auto& r = t.targets[i];
            StmtTargetRow row;
            row.id = base.target + int64_t(i) + 1;
            row.stmtId = base.stmt + r.stmt + 1;
            row.ordinal = r.ordinal;
            row.netId = base.net + r.net + 1;
            row.bits = r.r.bits;
            row.exact = r.r.exact;
            writer.addStmtTarget(row);
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
            row.stmtTargetId = d.targetRef < 0 ? 0 : base.target + d.targetRef + 1;
            row.exprRefId = d.exprRef < 0 ? 0 : base.exprRef + d.exprRef + 1;
            row.primitiveId = d.prim < 0 ? 0 : primNode[size_t(d.prim)];
            row.dependencyKind = d.kind;
            row.sourceBits = d.srcR.bits;
            row.sourceExact = d.srcNet < 0 ? -1 : (d.srcR.exact ? 1 : 0);
            row.targetBits = d.tgtR.bits;
            row.targetExact = d.tgtR.exact;
            row.mappingExact = d.mappingExact;
            row.callSiteId = d.callSite < 0 ? 0 : base.callSite + d.callSite + 1;
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
        for (size_t pi = 0; pi < ct.paramPairs.size(); pi++)
            writer.addInstParam({nodeId, int64_t(pi), ct.paramPairs[pi].first,
                                 ct.paramPairs[pi].second});
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

    /// Packages as pseudo-occurrences: each becomes a tree_node/inst above
    /// the roots (node_kind/def_kind 'package'), and its variables become net
    /// rows, so a `pkg::x` reference resolves to a real object instead of
    /// leaving the model as an 'external' driver. No dataflow is walked here
    /// -- a package variable's initializer is not a driver, and its
    /// readers/writers are the modules that reference it, whose cross-refs
    /// resolve onto these nets. Package module ids follow the definition
    /// modules; nodes and nets draw from the same counters as everything else.
    void stampPackages() {
        int64_t nextModuleId = int64_t(moduleIds.size());
        const PackageSymbol* stdPkg = &compilation.getStdPackage();
        for (auto* pkg : compilation.getPackages()) {
            if (!pkg || pkg == stdPkg || pkg->name.empty())
                continue;
            const TplLoc at = locator.locate(pkg->location);
            ModuleRow mrow;
            mrow.id = ++nextModuleId;
            mrow.name = std::string(pkg->name);
            mrow.definitionKind = "package";
            mrow.fileId = at.fileId;
            mrow.line = at.line;
            mrow.column = at.column;
            writer.addModule(mrow);
            stats.modules++;

            const int64_t nodeId = ++nodeCounter;
            TreeNodeRow node;
            node.id = nodeId;
            node.parentNodeId = 0;   // a pseudo-occurrence above the roots
            node.name = std::string(pkg->name);
            node.nodeKind = "package";
            node.ordinal = rootOrdinal++;
            writer.addTreeNode(node);

            InstRow inst;
            inst.id = nodeId;
            inst.moduleId = mrow.id;
            inst.parentInstId = 0;
            inst.fileId = at.fileId;
            inst.line = at.line;
            inst.column = at.column;
            writer.addInst(inst);
            stats.instances++;

            PackageInfo info;
            info.nodeId = nodeId;
            for (auto& member : pkg->members()) {
                if (member.kind != SymbolKind::Variable &&
                    member.kind != SymbolKind::Net)
                    continue;
                if (member.name.empty())
                    continue;
                auto& vs = member.as<ValueSymbol>();
                NetRow nrow;
                nrow.id = ++netCounter;
                nrow.instId = nodeId;
                nrow.scopeNodeId = nodeId;
                nrow.name = std::string(vs.name);
                nrow.declarationKind = declarationKindOf(vs);
                nrow.dataTypeId = writer.internDataType(typeOf(vs));
                nrow.width = vs.getType().isIntegral()
                                 ? int64_t(vs.getType().getBitWidth()) : -1;
                nrow.isImplicit = vs.kind == SymbolKind::Net &&
                                  vs.as<NetSymbol>().isImplicit;
                const TplLoc nloc = locator.locate(vs.location);
                nrow.fileId = nloc.fileId;
                nrow.line = nloc.line;
                nrow.column = nloc.column;
                writer.addNet(nrow);
                stats.nets++;
                info.netByName.emplace(nrow.name, nrow.id);
            }
            packageByName.emplace(std::string(pkg->name), std::move(info));
        }
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
                case TplHierRef::Package: {
                    // Not a tree descent -- a package is not under a normal
                    // parent, and its path uses `::`. Look it up directly and
                    // resolve the member against its own net map; node stays
                    // 0 so the generic net-index block below is skipped.
                    if (ref.segs.empty())
                        break;
                    auto pit = packageByName.find(ref.segs.front());
                    if (pit == packageByName.end())
                        break;
                    row.resolvedInstId = pit->second.nodeId;
                    auto nit = pit->second.netByName.find(ref.netName);
                    if (nit != pit->second.netByName.end()) {
                        row.resolvedNetId = nit->second;
                        resolvedNet.emplace(row.id, row.resolvedNetId);
                    }
                    break;
                }
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
        // plus index) or a reference's resolution. The TARGET must resolve:
        // target_net_id is NOT NULL, and guessing a written object would be
        // a wrong fact. The SOURCE may not: a package variable or an upward
        // name is a real driver this export has no net row for, and v10/v11
        // dropping the whole row made "driven through an unresolvable name"
        // indistinguishable from "undriven". The row is now written with a
        // NULL source net and the reference on the source end; v_driver
        // reports it as 'external'.
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
                if (it != resolvedNet.end())
                    row.sourceNetId = it->second;
            }
            else if (!d.sourceless) {
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
            row.stmtTargetId =
                d.targetRef < 0 ? 0 : job.base.target + d.targetRef + 1;
            row.exprRefId = d.exprRef < 0 ? 0 : job.base.exprRef + d.exprRef + 1;
            row.dependencyKind = d.kind;
            row.sourceBits = d.srcR.bits;
            row.sourceExact = d.srcR.exact ? 1 : 0;
            row.targetBits = d.tgtR.bits;
            row.targetExact = d.tgtR.exact;
            row.mappingExact = d.mappingExact;
            row.callSiteId =
                d.callSite < 0 ? 0 : job.base.callSite + d.callSite + 1;
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
    SourceLocator locator;

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
    int64_t callSiteCounter = 0;
    int64_t rootOrdinal = 0;

    std::unordered_map<int64_t, std::unordered_map<std::string, int64_t>> childByName;
    std::vector<ReplayJob> replayJobs;
    std::vector<CrossJob> crossJobs;
    /// node id -> (template, net id base) for net-name resolution at replay.
    std::unordered_map<int64_t, std::pair<const Template*, int64_t>> nodeTemplate;
    /// A stamped package pseudo-occurrence: its node id and member->net id.
    /// Kept apart from childByName because a package path uses `::`, not `.`,
    /// and a top instance could legally be named like a package.
    struct PackageInfo {
        int64_t nodeId = 0;
        std::unordered_map<std::string, int64_t> netByName;
    };
    std::unordered_map<std::string, PackageInfo> packageByName;

    Stats stats;
};

} // namespace

Stats extract(Compilation& compilation, AnalysisManager& analysis, Writer& writer) {
    Walker walker(compilation, analysis, writer);
    return walker.run();
}

} // namespace designdb
