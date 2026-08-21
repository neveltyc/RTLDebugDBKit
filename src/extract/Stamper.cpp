// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Pass 2. See extract/Stamper.h for the model; the ordering constraints are
// stated at the points that depend on them.

#include "extract/Stamper.h"

#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "slang/ast/Compilation.h"
#include "slang/ast/Scope.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/NetType.h"
#include "slang/ast/types/Type.h"
#include "slang/text/SourceManager.h"

#include "extract/SymbolText.h"
#include "extract/Template.h"

using namespace slang;
using namespace slang::ast;

namespace designdb::detail {

namespace {

class Stamper {
public:
    Stamper(Compilation& comp, Writer& w, SourceLocator& loc, Stats& s,
            TemplateSet& ts) :
        compilation(comp), writer(w), locator(loc), stats(s),
        templates(ts.byKey), instanceGroup(ts.groupOf),
        moduleCount(ts.moduleCount) {}

    void run() {
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
    }

private:

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
            // A connection with no terminal has nowhere to hang. The sibling
            // path in stampUnresolved has always guarded this; here it was
            // merely unreachable, because a lookup failure dropped the whole
            // connection before it became a row. Now that the lookup is by
            // symbol and succeeds, the guard has to be real: -1 would resolve
            // to childTermBase, the last id issued BEFORE this child, and name
            // a terminal of a different instance.
            if (conn.childTerm < 0)
                continue;
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
        int64_t nextModuleId = int64_t(moduleCount);
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
    Writer& writer;
    SourceLocator& locator;
    Stats& stats;
    /// Pass 1's output, by reference. Copying it would duplicate every row of
    /// every analyzed body in the design.
    std::map<std::string, Template>& templates;
    std::unordered_map<const InstanceSymbol*, std::string>& instanceGroup;
    /// Module rows pass 1 issued; package pseudo-modules number from here.
    std::size_t moduleCount = 0;

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

};

} // namespace

void stampDesign(Compilation& compilation, Writer& writer, SourceLocator& locator,
                 Stats& stats, TemplateSet& templates) {
    Stamper stamper(compilation, writer, locator, stats, templates);
    stamper.run();
}

} // namespace designdb::detail
