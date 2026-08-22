// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// Pass 1: reading one analyzed body -- its declarations, terminals, statements
// and dataflow. What the body wires its children to is TemplateBuilder_Conn.cpp.
//
// See extract/TemplateBuilder.h for what this produces and why the grouping
// key is what it is; extract/TemplateBuilderImpl.h for the class.

#include "extract/TemplateBuilderImpl.h"

// Only buildProcedure drives it, so it stays out of the shared header and out
// of TemplateBuilder_Conn.cpp.
#include "extract/StatementWalker.h"

namespace designdb::detail {

    /// Groups every instance, writes the module rows, then builds each group's
    /// terminals and finally its full template.
TemplateSet TemplateBuilder::run() {
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

    // Moved, never copied: `templates` is every row of every analyzed
    // body in the design.
    return TemplateSet{std::move(templates), std::move(instanceGroup),
                       moduleIds.size()};
}

    /// The body to extract a group's dataflow from: one the analysis manager
    /// actually analysed, else the first seen.
void TemplateBuilder::offer(Group& g, const InstanceBodySymbol& body) {
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
std::string TemplateBuilder::groupKey(const InstanceBodySymbol& body) const {
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

void TemplateBuilder::collect(const InstanceSymbol& inst) {
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

    /// The port list of one group, as terminal templates. A MultiPort (a
    /// non-ANSI `.p({hi, lo})` formal) is one terminal; its inside is the
    /// term_map segments built later.
/// Where each port symbol of ONE body sits among that body's terminals.
///
/// Recomputed per body rather than stored once, because what a group's bodies
/// share is the port ORDER, not the port symbols: slang gives identical
/// instances a single canonical body, so a child's `conn->port` belongs to that
/// child's own body while its template was built from the canonical one. A
/// stored symbol-keyed map looks right on a design where nothing shares a body
/// and silently drops every connection of every instance on one where something
/// does.
void TemplateBuilder::collectTermSlots(const InstanceBodySymbol& body,
                                       TermSlotMap& out) {
    int32_t termIdx = 0;
    for (auto* portSym : body.getPortList()) {
        if (!portSym)
            continue;
        auto width = [](const Type& ty) {
            return ty.isIntegral() ? int64_t(ty.getBitWidth()) : int64_t(-1);
        };
        switch (portSym->kind) {
            case SymbolKind::Port:
                out.emplace(static_cast<const void*>(portSym),
                            Template::TermSlot{
                                termIdx, 0,
                                width(portSym->as<PortSymbol>().getType())});
                break;
            case SymbolKind::MultiPort: {
                auto& mp = portSym->as<MultiPortSymbol>();
                out.emplace(static_cast<const void*>(portSym),
                            Template::TermSlot{termIdx, 0, width(mp.getType())});
                // Members MSB first, as a concatenation is written, so the
                // cursor counts down to each member's LSB -- the same offsets
                // buildTermMaps lays the inside out with, and the same ones
                // slang's expandMultiPortConn accumulates walking the reverse.
                uint64_t cursor =
                    mp.getType().isIntegral() ? mp.getType().getBitWidth() : 0;
                for (auto* member : mp.ports) {
                    if (!member)
                        continue;
                    const uint64_t w = member->getType().isIntegral()
                                           ? member->getType().getBitWidth()
                                           : 0;
                    if (w && w <= cursor)
                        cursor -= w;
                    out.emplace(static_cast<const void*>(member),
                                Template::TermSlot{termIdx, cursor,
                                                   w ? int64_t(w) : -1});
                }
                break;
            }
            case SymbolKind::InterfacePort:
                out.emplace(static_cast<const void*>(portSym),
                            Template::TermSlot{termIdx, 0, -1});
                break;
            default:
                continue;   // no terminal, so no index consumed
        }
        termIdx++;
    }
}

void TemplateBuilder::buildTerms(Template& t, const InstanceBodySymbol& body) {
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
        t.terms.push_back(std::move(term));
    }
    collectTermSlots(body, t.termOf);
}

int32_t TemplateBuilder::newStmt(Build& b, std::string kind, std::string construct,
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

int32_t TemplateBuilder::addExprRef(Build& b, int32_t stmt, const Ref& r, std::string role,
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
int32_t TemplateBuilder::addHierRef(Build& b, bool isWrite, const Ref& r,
                                    const TplLoc& at, EvalContext& eval,
                                    const char* access,
                                    const Ref* asWritten) {
    auto key = std::make_tuple(r.origin, isWrite, b.curStmt);
    if (auto it = b.hierSeen.find(key); it != b.hierSeen.end())
        return it->second;
    std::string text = canonicalPath(r.origin, eval);
    if (text.empty())
        text = normalizedText(r.origin, sourceManager);
    // The symbol knows its own name, and that is always a usable path.
    //
    // Both spellings above can fail. canonicalPath has no case for a
    // HierarchicalValue, so every cross-module reference falls to
    // normalizedText -- which recovers text by slicing a source buffer and
    // returns nothing when the reference's ends sit in different buffers, i.e.
    // when any part of the name came from a macro. `q <= `TOP.glob` was
    // therefore dropped where `q <= tb_top.glob` was recorded.
    if (text.empty() && r.sym)
        text = r.sym->getHierarchicalPath();
    // Empty is the only reason left to drop one. There used to be a second --
    // a path had to contain a '.' or a '::' -- which discarded every reference
    // to a $unit-scope object, whose name is bare. Dropping it here produced no
    // hier_ref, so the dependency became a net_dep with a null source AND a
    // null reference: the exact shape v_driver classifies as a CONSTANT. The
    // database then said a signal fed by an outward name was tied off, and the
    // gating went with it, since a control dependency needs one of the two
    // indices to survive.
    if (text.empty()) {
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
void TemplateBuilder::fillResolution(Build& b, TplHierRef& row, const Ref& r) {
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
            auto it = b.t->termOf.find(static_cast<const void*>(first));
            if (it != b.t->termOf.end()) {
                auto& ip = first->as<InterfacePortSymbol>();
                auto [iface, modport] = ip.getConnection();
                if (iface) {
                    std::string ifacePrefix = iface->getHierarchicalPath();
                    std::string rel;
                    if (splitBelow(full, ifacePrefix, rel)) {
                        row.resolve = TplHierRef::ViaIfaceTerm;
                        row.ifaceTerm = it->second.term;
                        splitSegsAndNet(rel, *target, row);
                        return;
                    }
                }
            }
        }
        return;
    }
    std::string rel;
    if (splitBelow(full, b.decl->bodyPrefix(), rel)) {
        row.resolve = TplHierRef::Downward;
        splitSegsAndNet(rel, *target, row);
        return;
    }
    row.resolve = TplHierRef::Absolute;
    splitSegsAndNet(full, *target, row);
}

bool TemplateBuilder::splitBelow(const std::string& full, const std::string& prefix,
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
void TemplateBuilder::splitSegsAndNet(const std::string& rel, const Symbol& target,
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

void TemplateBuilder::buildTemplate(Template& t, const InstanceBodySymbol& body) {
    // The index first: its constructor establishes scope 0, and nothing
    // below can name a net or a scope until it exists.
    DeclIndex decl(t, body, body.getHierarchicalPath(), locator, writer);

    Build b;
    b.t = &t;
    b.body = &body;
    b.decl = &decl;

    b.decl->collectDeclarations(body, 0);
    buildTermMaps(b, body);

    if (auto* scope = analysis.getAnalyzedScope(body)) {
        for (auto& proc : scope->procedures)
            buildProcedure(b, proc);
    }
    buildNetInitialisers(b, body);
    buildNetAliases(b, body);
    // Primitives before children: an anonymous gate and an unnamed
    // instantiation in one scope are siblings drawing from one counter, so
    // the order they draw in is what their names are.
    buildPrimitives(b, body);
    buildChildren(b, body);
    stats.truncatedCalls += b.truncatedCalls;
    t.built = true;
}

    /// The inside of each terminal: which nets it stands for. An ANSI port
    /// maps whole-to-whole onto its internal symbol; a non-ANSI port
    /// expression and a MultiPort produce one segment per element with its
    /// window, through the same machinery the outside uses.
void TemplateBuilder::buildTermMaps(Build& b, const InstanceBodySymbol& body) {
    EvalContext evalCtx(body);
    for (auto* portSym : body.getPortList()) {
        if (!portSym)
            continue;
        auto termIt = b.t->termOf.find(static_cast<const void*>(portSym));
        if (termIt == b.t->termOf.end())
            continue;
        const int32_t termIdx = termIt->second.term;
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
        // The internal expression FIRST, and internalSymbol only as the
        // fallback. The two are not alternatives: slang sets both when the
        // port reference carries a select (`.p(hi[1:0])` gets an
        // Expression::bindSelector into internalExpr and keeps hi as the
        // symbol), so testing the symbol first took a 2-bit formal onto the
        // whole of a 4-bit net and called the mapping exact -- a one-to-one
        // claim across widths that cannot hold.
        auto internalSegs = [&](const Expression& inner) {
            std::vector<ConnRef> segs;
            collectConnRefs(inner, evalCtx, segs);
            for (auto& cn : segs) {
                if (!cn.ref.sym)
                    continue;
                const int32_t netIdx = b.decl->netFor(*cn.ref.sym);
                if (netIdx < 0)
                    continue;
                TplRange termR;
                const uint64_t fw = inner.type ? inner.type->getBitWidth() : 0;
                if (cn.windowExact && fw &&
                    !(cn.winLo == 0 && cn.winHi + 1 >= fw))
                    termR.bits = std::make_pair(cn.winLo, cn.winHi);
                termR.exact = cn.windowExact;
                addSeg(netIdx, termR, rangeOf(cn.ref), cn.positional);
            }
        };
        if (portSym->kind == SymbolKind::Port) {
            auto& p = portSym->as<PortSymbol>();
            if (auto* inner = p.getInternalExpr())
                internalSegs(*inner);
            else if (p.internalSymbol && ValueSymbol::isKind(p.internalSymbol->kind)) {
                const int32_t netIdx =
                    b.decl->netFor(p.internalSymbol->as<ValueSymbol>());
                addSeg(netIdx, TplRange{}, TplRange{}, true);
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
void TemplateBuilder::buildProcedure(Build& b, const AnalyzedProcedure& proc) {
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

    bool reached = false;

    StatementWalker walker(
        // ---- one assignment target with its pairs and gating
        [&](const Ref& dst, const std::vector<PairedSrc>& pairs,
            const std::vector<Ref>& gating, SourceRange where, int64_t seq,
            bool blocking, int64_t dropped, bool inSubroutine,
            bool firstTarget, const std::string& delay,
            const char* constructWord) {
            reached = true;
            // No input-port filter and no null-symbol guard, because neither
            // could fire. slang builds every driver in AnalyzedProcedure and
            // AnalysisManager with DriverFlags::None, so isInputPort() is
            // false for all of them -- 3,945 procedure drivers across
            // picorv32, tinyriscv and veerwolf, none of them an input port --
            // and every Ref reaching this callback came from collectRefs or
            // collectSlots, which never emit a null symbol. Skipping a first
            // target here would also have been a trap: the second target of a
            // concatenated left-hand side would then arrive with
            // firstTarget=false and a stale curStmt, attaching its rows to the
            // previous statement.
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
void TemplateBuilder::recordReleaseTarget(Build& b, int32_t stmt, const Ref& r,
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
void TemplateBuilder::recordSystemWrite(Build& b, int32_t stmt, const Ref& r,
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
    // Per statement, like every other target site. This one used the template
    // vector's size, which is a global index leaked into a column the doc
    // defines as "position in a declaration or extraction list".
    tr.ordinal = b.targetOrdinal++;
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
void TemplateBuilder::recordRead(Build& b, int32_t stmt, const Ref& r, const std::string& role,
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

void TemplateBuilder::addProcEvent(Build& b, int32_t procIdx, int32_t stmtIdx,
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
void TemplateBuilder::emitAssignment(Build& b, const Ref& dst,
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
void TemplateBuilder::emitCallBinding(Build& b, const Ref& formal, const Ref& actual,
                                      bool reads, bool writes, bool oneToOne, bool bindable,
                                      const TplLoc& at, EvalContext& evalCtx) {
    if (!formal.sym || !actual.sym)
        return;
    const int32_t stmt = bindable ? b.curStmt : -1;
    const int32_t formalNet = b.decl->netFor(*formal.sym);
    if (formalNet < 0) {
        // The formal is not a net of THIS body, which is what a subroutine
        // declared in a package, an interface or $unit looks like from here.
        // Dropping the binding took the actual with it -- and the actual is
        // usually a perfectly good local net, so a task that plainly writes
        // its argument left that argument with no driver at all.
        //
        // Only the actual's half is recorded. The formal cannot be: at a call
        // site it is a symbol, not an expression, and the Ref built for it
        // borrows the ACTUAL's origin -- so putting it through addHierRef
        // names it with the actual's text and resolves it against the actual's
        // target. Both wrong, and quietly so.
        const int32_t actualIdx = b.decl->netFor(*actual.sym);
        if (actualIdx < 0 || stmt < 0)
            return;
        if (reads)
            addExprRef(b, stmt, actual, "call_argument", actualIdx);
        if (writes) {
            // The target row, and deliberately no dependency. A `procedure`
            // arc names two nets and is told apart from the reading direction
            // by the formal being its source -- and the formal is exactly what
            // is missing here, so a source-less one would be a shape the kind
            // does not have. What is true and recordable is that this
            // statement writes the argument: `v_net_attachment` answers "what
            // writes this net", while `v_driver` reports no arc because there
            // is no nameable one. Better than the old behaviour, which
            // recorded neither.
            TplStmtRef tr;
            tr.stmt = stmt;
            tr.ordinal = b.targetOrdinal++;
            tr.net = actualIdx;
            tr.r = rangeOf(actual);
            b.t->targets.push_back(std::move(tr));
        }
        return;
    }
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
void TemplateBuilder::buildNetInitialisers(Build& b, const InstanceBodySymbol& body) {
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
        // The generate level that declares it, not the instance. forEachOfKind
        // descends into generate blocks, so hard-coding 0 filed `wire w = …`
        // inside `g[0]` under the instance node -- while the net row for the
        // same declaration was filed correctly, so the two tables contradicted
        // each other, and one generate iteration's initialiser could not be
        // told from another's.
        b.curScope = b.decl->scopeForSymbol(net);
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
void TemplateBuilder::buildNetAliases(Build& b, const InstanceBodySymbol& body) {
    EvalContext evalCtx(body);
    b.curProc = -1;
    forEachOfKind<SymbolKind::NetAlias, NetAliasSymbol>(
        body, [&](const NetAliasSymbol& al) {
        auto refs = al.getNetReferences();
        if (refs.size() < 2)
            return;
        const TplLoc at = locator.locate(al.location);
        b.curScope = b.decl->scopeForSymbol(al);
        const int32_t stmt = newStmt(b, "alias", "alias", std::string(),
                                     /*seq=*/-1, std::string(), 0, at);
        if (stmt < 0)
            return;

        // One target and one operand per reference, in written order.
        // `group` is which written side the reference came from. An alias
        // binds the SIDES to each other, so two references of one
        // concatenation are not aliases of each other -- `alias {a, b} = c`
        // makes a and b different bits of c, not copies of one another.
        struct Side { int32_t target = -1; int32_t operand = -1;
                      int32_t net = -1; int32_t group = -1; Ref ref; };
        std::vector<Side> sides;
        // A side that is a concatenation is N references, not one. LRM 10.11
        // allows `alias {a, b} = c;` and getNetReferences hands the
        // concatenation back as a single expression yielding several refs --
        // requiring exactly one dropped the whole side, so a and b were
        // aliased to nothing and the statement carried no dependency at all.
        // Each ref becomes its own side; what is lost is only which bits of
        // the other side it meets, and that shows as a coarse mapping rather
        // than a missing one.
        bool anyMultiRef = false;
        int32_t group = 0;
        for (auto* e : refs) {
            if (!e)
                continue;
            const int32_t thisGroup = group++;
            std::vector<Ref> got;
            collectRefs(*e, evalCtx, got, /*skipSelectors=*/true);
            anyMultiRef = anyMultiRef || got.size() > 1;
            for (auto& one : got) {
            if (!one.sym)
                continue;
            Side sd;
            sd.group = thisGroup;
            sd.ref = one;
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
        }

        for (size_t i = 0; i < sides.size(); i++) {
            for (size_t j = 0; j < sides.size(); j++) {
                if (i == j || sides[i].group == sides[j].group)
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
                d.mappingExact = (!anyMultiRef && sides[i].ref.exact &&
                                  sides[j].ref.exact)
                                     ? 1
                                     : 0;
                d.callSite = b.curCallSite;
                b.t->deps.push_back(std::move(d));
            }
        }
        b.curStmt = -1;
    });
}

std::string TemplateBuilder::anonSegment(Build& b, int32_t scopeIdx,
                                         std::string_view defName) {
    // A definition name that is not a plain identifier may hold a '.', and a
    // tree node name holding one is a path with two segments -- the very
    // thing an escaped name is written `\name ` to avoid. The name is
    // decoration here: '$' and the counter are what make the segment unique
    // and unspellable, so a dotted definition simply contributes nothing.
    std::string_view label =
        defName.find('.') == std::string_view::npos ? defName : std::string_view();
    return "$" + std::string(label) + "$" +
           std::to_string(b.anonSeq[scopeIdx]++);
}

    /// Gate, switch and UDP instances: a tree node, a primitive row, and one
    /// dependency per LRM (input, output) pairing.
void TemplateBuilder::buildPrimitives(Build& b, const InstanceBodySymbol& body) {
    EvalContext evalCtx(body);
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
        if (name.empty())
            name = anonSegment(b, p.scope, def.name);
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

void TemplateBuilder::addHierRefIfOutward(Build& b, const Ref& r, EvalContext& evalCtx) {
    std::string rel;
    if (r.sym && !relativePath(*r.sym, b.decl->bodyPrefix(), rel))
        addHierRef(b, false, r, TplLoc{}, evalCtx);
}

void TemplateBuilder::internModuleRow(const DefinitionSymbol& def) {
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

// The interface extract/TemplateBuilder.h declares.
TemplateSet buildTemplates(Compilation& compilation, AnalysisManager& analysis,
                           Writer& writer, SourceLocator& locator, Stats& stats) {
    TemplateBuilder builder(compilation, analysis, writer, locator, stats);
    return builder.run();
}

} // namespace designdb::detail
