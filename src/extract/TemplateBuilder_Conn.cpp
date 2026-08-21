// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Pass 1, the boundary half: what a body wires its children to.
//
// Every terminal of every child instantiated here, as written in this parent --
// which nets, which bit windows, and what to do when the expression is not a
// plain reference or the child is a black box slang could not resolve.

#include "extract/TemplateBuilderImpl.h"

namespace designdb::detail {

void TemplateBuilder::collectConnRefs(const Expression& expr, EvalContext& ctx,
                                      std::vector<ConnRef>& out, uint64_t base,
                                      bool degraded) {
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
void TemplateBuilder::buildChildren(Build& b, const InstanceBodySymbol& body) {
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

void TemplateBuilder::registerChildren(Build& b, const Scope& scope, int32_t scopeIdx,
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
void TemplateBuilder::buildInstanceConns(Build& b, const InstanceSymbol& child, TplChild& c,
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
const InterfacePortSymbol* TemplateBuilder::passedThrough(const InstanceBodySymbol& body,
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
void TemplateBuilder::buildUnresolvedConns(Build& b, const UninstantiatedDefSymbol& u,
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

} // namespace designdb::detail
