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

    // What pass 1 grouped, before any of it is walked. A group whose chosen
    // body has no AnalyzedScope yields a template with no procedure in it;
    // see designdb::Stats for why that happens and why no row moves.
    for (auto& [key, group] : groups) {
        if (!analysis.getAnalyzedScope(*group.body))
            stats.unanalysedBodies++;
    }

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

    /// Groups one instance and descends into its children.
    ///
    /// The descent stops at an instance whose group is already on the branch
    /// above it -- a module instantiating itself with identical parameters.
    /// That is illegal, and slang says so, but it says so having already
    /// elaborated the instance tree it was walking when it noticed: slang
    /// bounds the *depth* at 128 and nothing else. One self-instantiation per
    /// body is therefore 130-odd instances, and two is 2^128 -- an
    /// elaborated tree this walk cannot finish, ever, on a design whose only
    /// fault is one line of illegal RTL. The guard is a path set rather than
    /// a visited set on purpose: a module legitimately instantiated twice by
    /// one parent is two separate branches and must be collected on both,
    /// and only a repeat *on the way down* is the impossible one.
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

    // The instance itself is grouped either way -- it is a real occurrence
    // and its body is a real template. Only the descent is cut.
    detail::OnPath guard(onPath, key);
    if (!guard.entered())
        return;
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
                out.emplace(portSym,
                            TermSlot{
                                termIdx, 0,
                                width(portSym->as<PortSymbol>().getType())});
                break;
            case SymbolKind::MultiPort: {
                auto& mp = portSym->as<MultiPortSymbol>();
                out.emplace(portSym,
                            TermSlot{termIdx, 0, width(mp.getType())});
                // Members MSB first, as a concatenation is written, so the
                // cursor counts down to each member's LSB -- the same offsets
                // buildTermMaps lays the inside out with, and the same ones
                // slang's expandMultiPortConn accumulates walking the reverse.
                MsbCursor cursor(0, mp.getType().isIntegral()
                                        ? mp.getType().getBitWidth()
                                        : 0);
                for (auto* member : mp.ports) {
                    if (!member)
                        continue;
                    const uint64_t w = member->getType().isIntegral()
                                           ? member->getType().getBitWidth()
                                           : 0;
                    const auto window = w ? cursor.advance(w) : std::nullopt;
                    out.emplace(member,
                                TermSlot{termIdx,
                                                   window ? window->lo
                                                          : cursor.position(),
                                                   w ? int64_t(w) : -1});
                }
                break;
            }
            case SymbolKind::InterfacePort:
                out.emplace(portSym,
                            TermSlot{termIdx, 0, -1});
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
                term.kind = TermKind::Signal;
                term.direction = directionOf(p.direction);
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
                term.kind = TermKind::Signal;
                term.direction = directionOf(mp.direction);
                term.dataTypeId = writer.internDataType(mp.getType().toString());
                if (mp.getType().isIntegral())
                    term.width = static_cast<int64_t>(mp.getType().getBitWidth());
                term.isConst = 0;
                break;
            }
            case SymbolKind::InterfacePort: {
                auto& ip = portSym->as<InterfacePortSymbol>();
                term.kind = TermKind::Interface;
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
    collectTermSlots(body, termSlots[&t]);
}

int32_t TemplateBuilder::newStmt(Build& b, StmtKind kind, std::string construct,
                                 AssignKind assignKind, int64_t seq, std::string delay,
                                 int64_t dropped, const TplLoc& loc) {
    TplStmt s;
    s.scope = b.curScope;
    s.proc = b.curProc;
    s.sequence = b.curProc < 0 ? -1 : seq;
    s.kind = kind;
    s.construct = std::move(construct);
    s.assignKind = assignKind;
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
    return idx;
}

int32_t TemplateBuilder::addExprRef(Build& b, int32_t stmt, const Ref& r, RefRole role,
                                    int32_t netIdx) {
    TplExprRef e;
    e.stmt = stmt;
    e.ordinal = b.exprOrdinal++;
    e.net = netIdx;
    e.role = role;
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
                                    std::optional<Access> access,
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
    row.access = access.value_or(isWrite ? Access::Write : Access::Read);
    row.r = rangeOf(asWritten ? *asWritten : r);
    row.loc = at;
    fillResolution(b, row, r);
    const int32_t idx = int32_t(b.t->hierRefs.size());
    if (row.resolvable())
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
    if (!target || !r.sym) {
        row.resolve = TplHierRef::Failed;
        return;
    }
    // slang's isUpward() is true for two unrelated shapes, and only one of
    // them is unresolvable here: a name that climbed OUT of this body
    // (upwardCount > 0), and a name anchored at $root. The first is a
    // genuine unknown -- the one analysed body speaks for occurrences whose
    // upward surroundings may differ, and a guess is worse than a NULL. The
    // second is the opposite: an absolute path names the same object seen
    // from every occurrence, which is exactly what TplHierRef::Absolute is
    // for. Asking isUpward() alone dropped `$root.a.b.c` with the upward
    // ones and left Absolute unreachable.
    const bool fromRoot = !hv.ref.path.empty() && hv.ref.path.front().symbol &&
                          hv.ref.path.front().symbol->kind == SymbolKind::Root;
    if (!fromRoot && hv.ref.isUpward()) {
        row.resolve = TplHierRef::Upward;
        return;
    }
    // A modport port stands for the net behind it: the reference
    // resolves to that net, not to the modport's own symbol -- whose
    // path carries the modport level (`bus.src.vld`) that the stamped
    // net names do not.
    if (target->kind == SymbolKind::ModportPort) {
        auto* inner = target->as<ModportPortSymbol>().internalSymbol;
        if (!inner) {
            // An explicit modport expression names no one net.
            row.resolve = TplHierRef::Failed;
            return;
        }
        target = inner;
    }
    std::string full = target->getHierarchicalPath();
    // The interface-port case: the reference entered through one of this
    // template's own interface terminals.
    if (hv.ref.isViaIfacePort() && !hv.ref.path.empty()) {
        const Symbol* first = hv.ref.path.front().symbol;
        if (first && first->kind == SymbolKind::InterfacePort) {
            auto it = b.termOf->find(first);
            if (it != b.termOf->end()) {
                auto& ip = first->as<InterfacePortSymbol>();
                auto [iface, modport] = ip.getConnection();
                if (iface) {
                    std::string ifacePrefix = iface->getHierarchicalPath();
                    std::string rel;
                    if (splitBelow(full, ifacePrefix, rel)) {
                        row.resolve = TplHierRef::ViaIfaceTerm;
                        row.ifaceTerm = it->second.term;
                        if (!segsFromAncestry(nullptr, iface, *target, row))
                            row.resolve = TplHierRef::Failed;
                        return;
                    }
                }
            }
        }
        row.resolve = TplHierRef::Failed;
        return;
    }
    // A $root path must not be re-read as a downward one even when it does
    // sit below this body's prefix. The prefix belongs to the ONE body that
    // was analysed; replaying `$root.top.u.x` from each occurrence would
    // answer `top.u.x` for the occurrence that happens to be `top` and
    // `outer.m.u.x` for one instantiated deeper, and only the first is what
    // the source spells.
    std::string rel;
    if (!fromRoot && splitBelow(full, b.decl->bodyPrefix(), rel)) {
        row.resolve = TplHierRef::Downward;
        if (!segsFromAncestry(b.body, nullptr, *target, row))
            row.resolve = TplHierRef::Failed;
        return;
    }
    // getHierarchicalPath() stops at Root (Symbol.cpp:117 walks up only
    // while the parent is neither Root nor CompilationUnit), so `full` is
    // already root-relative and its first segment is a top instance name --
    // which is what Stamper's descend(0, segs) consumes.
    row.resolve = TplHierRef::Absolute;
    if (!segsFromAncestry(nullptr, nullptr, *target, row))
        row.resolve = TplHierRef::Failed;
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

    /// Fills the replay route to `target`: the tree segments from the stop
    /// level down to the target's instance, and the scope-relative net name
    /// inside it. Stops at `stopBody` (the analysed body: a Downward route),
    /// at `stopInst` (the instance an interface terminal is bound to), or at
    /// the design root when both are null (an Absolute route).
    ///
    /// The segments come from the target's own ancestry through the same
    /// producers that named the tree -- leafSegment per instance level,
    /// generateSegment per generate level, an array element fused with its
    /// array's name exactly as DeclIndex spells its scope. They used to be
    /// recovered by splitting slang's hierarchical-path string on '.', which
    /// agreed with the tree only where slang's spelling and ours happened to
    /// coincide: an escaped identifier with a dot in it split apart, and an
    /// unnamed instance -- whose level slang's path omits entirely -- left a
    /// gap no lookup could cross. The net name stays the slang-path suffix
    /// below the owner, because net rows are named from that same suffix
    /// (relativePath), and one producer per channel is the point.
    ///
    /// False means no route: the ancestry never met the stop level, or a
    /// level on the way has no name of its own (a nameless instantiation's
    /// tree segment is synthesised per scope and cannot be respelled here).
bool TemplateBuilder::segsFromAncestry(const InstanceBodySymbol* stopBody,
                                       const Symbol* stopInst,
                                       const Symbol& target, TplHierRef& row) {
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
    if (!owner)
        return false;
    std::string netRel;
    if (!splitBelow(target.getHierarchicalPath(), owner->getHierarchicalPath(),
                    netRel))
        return false;

    std::vector<std::string> segs;   // collected leaf to root, then reversed
    const InstanceBodySymbol* body = owner;
    bool reachedRoot = false;
    while (!reachedRoot) {
        if (stopBody && body == stopBody)
            break;
        const InstanceSymbol* inst = body->parentInstance;
        if (!inst) {
            // A body with no instance above it. An Absolute route ends
            // here; any other stop level was never met.
            if (stopBody || stopInst)
                return false;
            break;
        }
        if (stopInst && static_cast<const Symbol*>(inst) == stopInst)
            break;
        std::string seg = leafSegment(*inst);
        if (seg.empty())
            return false;
        segs.push_back(std::move(seg));
        // The generate levels holding this instance inside its parent body,
        // innermost first. An array element and its array are one tree
        // level, spelled base[k], exactly as DeclIndex names the scope.
        const Scope* up = inst->getParentScope();
        for (;;) {
            if (!up)
                return false;
            auto& sym = up->asSymbol();
            if (sym.kind == SymbolKind::InstanceBody) {
                body = &sym.as<InstanceBodySymbol>();
                break;
            }
            // Above a top instance sits the design root, not a body. That
            // is where an Absolute route finishes -- descend(0, segs)
            // starts at a top instance name -- and where any other stop
            // level has been missed.
            if (sym.kind == SymbolKind::Root ||
                sym.kind == SymbolKind::CompilationUnit) {
                if (stopBody || stopInst)
                    return false;
                reachedRoot = true;
                break;
            }
            if (sym.kind == SymbolKind::GenerateBlock) {
                auto& block = sym.as<GenerateBlockSymbol>();
                std::string level = generateSegment(block);
                const Scope* parent = block.getParentScope();
                if (parent && parent->asSymbol().kind ==
                                  SymbolKind::GenerateBlockArray) {
                    auto& arr =
                        parent->asSymbol().as<GenerateBlockArraySymbol>();
                    std::string base(arr.name);
                    if (base.empty())
                        base = arr.getExternalName();
                    level = base + level;
                    up = arr.getParentScope();
                }
                else {
                    up = block.getParentScope();
                }
                segs.push_back(std::move(level));
                continue;
            }
            up = sym.getParentScope();
        }
    }
    row.netName = std::move(netRel);
    row.segs.assign(segs.rbegin(), segs.rend());
    return true;
}

void TemplateBuilder::buildTemplate(Template& t, const InstanceBodySymbol& body) {
    // The index first: its constructor establishes scope 0, and nothing
    // below can name a net or a scope until it exists.
    DeclIndex decl(t, body, body.getHierarchicalPath(), locator, writer);

    Build b;
    b.t = &t;
    b.termOf = &termSlots[&t];
    b.body = &body;
    b.decl = &decl;

    b.decl->collectDeclarations(body, 0);
    buildTermMaps(b, body);

    // Recorded, not just used: a body with no AnalyzedScope yields no
    // procedure here, and the rows it does produce -- nets, terminals,
    // children -- look exactly like a module that has no always block and
    // no continuous assignment. The stamper counts the occurrences that
    // inherit the gap so the export can say so rather than let a consumer
    // read absence as fact.
    if (auto* scope = analysis.getAnalyzedScope(body)) {
        t.analysedBody = true;
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
        auto termIt = b.termOf->find(portSym);
        if (termIt == b.termOf->end())
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
            const uint64_t total = mp.getType().isIntegral()
                                       ? mp.getType().getBitWidth()
                                       : 0;
            MsbCursor cursor(0, total);
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
                const auto window =
                    (total && w) ? cursor.advance(w) : std::nullopt;
                if (window) {
                    if (!(window->lo == 0 && w == total))
                        termR.bits = std::make_pair(window->lo, window->hi);
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
        // A continuous assign gets no procedure row, so the word that
        // distinguishes it never reaches here; every other procedure is
        // classified from the symbol rather than re-read out of the
        // construct word it also produced.
        p.kind = procKindOf(sym);
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
            sensStmt = newStmt(b, StmtKind::EventControl, "sensitivity",
                               AssignKind::None, -1, std::string(), 0, procAt);
        }
        return sensStmt;
    };
    if (procIdx >= 0) {
        std::vector<std::pair<const Expression*, Edge>> raw;
        std::vector<const Expression*> iffs;
        collectEdgeEvents(sens.timingControl, raw, &iffs);
        for (auto& [expr, edge] : raw)
            addProcEvent(b, procIdx, -1, expr, edge, EventKind::Sensitivity, procAt,
                         evalCtx, sensReadStmt);
        for (auto* cond : iffs) {
            std::vector<Ref> reads;
            collectRefs(*cond, evalCtx, reads);
            const int32_t s = sensReadStmt();
            for (auto& r : reads)
                recordRead(b, s, r, RefRole::Event, procAt, evalCtx);
        }
    }

    bool reached = false;

    // The walk hands over self-contained nodes and the receiver files them
    // directly. An assignment node owns its targets, so the statement row
    // and its control reads happen once in fileAssignment -- no firstTarget
    // flag, no Build-wide condition vectors. No input-port filter and no
    // null-symbol guard on the way in, because neither could fire: slang
    // builds every driver with DriverFlags::None, and every Ref in a node
    // came from collectRefs or collectSlots, which never emit a null symbol.
    GateTable gates;

    // ---- a statement-level event control (a wait)
    auto onEvent =
        [&](const Expression* e, Edge edge, int64_t seq, SourceRange where) {
            if (procIdx < 0)
                return;
            const TplLoc at = locator.locate(where.start(), procAt);
            const int32_t s = newStmt(b, StmtKind::EventControl, "wait",
                                      AssignKind::None, seq, std::string(), 0,
                                      at);
            addProcEvent(b, procIdx, s, e, edge, EventKind::Wait, at, evalCtx,
                         [&]() { return s; });
        };
    // ---- a statement that reads without writing anything nameable
    auto fileReadLike =
        [&](const std::vector<Ref>& reads, const std::vector<Ref>& gating,
            const std::vector<Ref>& writes, StmtKind stmtKind, RefRole role,
            bool writesAreReleased, const std::string& construct2,
            int64_t seq, int64_t dropped, SourceRange where) {
            const TplLoc at = locator.locate(where.start(), procAt);
            const int32_t s = newStmt(b, stmtKind, construct2,
                                      AssignKind::None, seq, std::string(),
                                      dropped, at);
            for (auto& r : reads)
                recordRead(b, s, r, role, at, evalCtx);
            // The conditions that gate it. No dependency can exist --
            // the statement writes nothing this instance names -- but
            // the condition IS read, and dropping it lost the signal
            // from every load query.
            for (auto& g : gating)
                recordRead(b, s, g, RefRole::Control, at, evalCtx);
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
                if (writesAreReleased)
                    recordReleaseTarget(b, s, w, at, evalCtx);
                else
                    recordSystemWrite(b, s, w, at, evalCtx);
            }
        };

    StatementWalker walker(
        gates,
        [&](Node&& node) {
            std::visit(
                [&](auto&& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, AssignmentNode>) {
                        reached = true;
                        fileAssignment(b, n.targets, gates.refs(n.gate),
                                       locator.locate(n.where.start(), procAt),
                                       n.seq, n.blocking, n.dropped,
                                       n.inSubroutine, n.delay, isContinuous,
                                       n.constructWord ? n.constructWord
                                                       : construct,
                                       evalCtx);
                    }
                    else if constexpr (std::is_same_v<T, BindNode>) {
                        reached = true;
                        fileBinding(b, n,
                                    locator.locate(n.where.start(), procAt),
                                    evalCtx);
                    }
                    else if constexpr (std::is_same_v<T, EventNode>) {
                        onEvent(n.expr, n.edge, n.seq, n.where);
                    }
                    else if constexpr (std::is_same_v<T, ReadNode>) {
                        // The node kind IS the statement kind and the read
                        // role; the receiver used to recover both from a
                        // string it had just been handed.
                        const auto [kind, role] =
                            n.kind == ReadNode::Kind::Assertion
                                ? std::pair{StmtKind::Assertion,
                                            RefRole::Assertion}
                            : n.kind == ReadNode::Kind::Wait
                                ? std::pair{StmtKind::Wait, RefRole::Wait}
                                : std::pair{StmtKind::Call,
                                            RefRole::CallArgument};
                        fileReadLike(n.reads, gates.refs(n.gate), {}, kind,
                                     role, /*writesAreReleased=*/false,
                                     n.construct, n.seq, n.dropped, n.where);
                    }
                    else if constexpr (std::is_same_v<T, SystemTaskNode>) {
                        fileReadLike(n.reads, gates.refs(n.gate), n.writes,
                                     StmtKind::SystemTask, RefRole::SystemTask,
                                     /*writesAreReleased=*/false, n.construct,
                                     n.seq, n.dropped, n.where);
                    }
                    else if constexpr (std::is_same_v<T, ReleaseNode>) {
                        fileReadLike({}, gates.refs(n.gate), n.lvalues,
                                     StmtKind::Release, RefRole::CallArgument,
                                     /*writesAreReleased=*/true,
                                     n.isRelease ? "release" : "deassign",
                                     n.seq, n.dropped, n.where);
                    }
                },
                std::move(node));
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
        TplDep d;
        d.kind = DepKind::Data;
        d.sourceless = true;
        d.stmt = stmt;
        d.tgt.href = href;
        d.tgtR = rangeOf(r);
        d.callSite = b.curCallSite;
        b.t->deps.push_back(std::move(d));
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
    d.src.net = -1;
    d.tgt.net = netIdx;
    d.stmt = stmt;
    d.targetRef = targetIdx;
    d.kind = DepKind::Data;
    d.tgtR = rangeOf(r);
    d.callSite = b.curCallSite;
    b.t->deps.push_back(std::move(d));
}

    /// One read of a statement, wherever it lands: an expr_ref for a net of
    /// this instance, a hier_ref for anything outside it.
void TemplateBuilder::recordRead(Build& b, int32_t stmt, const Ref& r, RefRole role,
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
                                   const Expression* expr, Edge edge,
                                   EventKind eventKind, const TplLoc& at,
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
            // Parity with the pre-BitInterval default: an event reference
            // that leaves the instance claimed the whole object exactly.
            r.cover = BitInterval::whole();
            r.exact = true;
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
        const RefRole role =
            eventKind == EventKind::Wait ? RefRole::Wait : RefRole::Event;
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
void TemplateBuilder::fileAssignment(Build& b, const std::vector<TargetRecord>& targets,
                                     const std::vector<Ref>& gate, const TplLoc& at,
                                     int64_t seq, bool blocking, int64_t dropped,
                                     bool inSubroutine, const std::string& delay,
                                     bool isContinuous, const std::string& construct,
                                     EvalContext& evalCtx) {
    if (targets.empty())
        return;
    const bool continuous = isContinuous && !inSubroutine;
    const int32_t stmt = newStmt(b, StmtKind::Assignment, construct,
                                 continuous  ? AssignKind::Continuous
                                 : blocking  ? AssignKind::Blocking
                                             : AssignKind::Nonblocking,
                                 seq, delay, dropped, at);
    // The control reads gate every target of the statement; recorded once,
    // reused by each target's control dependencies. An outward condition is
    // a hier_ref; its dependency onto each target is paired here and
    // materialised when the reference resolves. One record per condition --
    // the three Build-wide vectors this replaces were indexed in lockstep
    // by a per-target callback and cleared as a trio in newStmt.
    struct ControlRec {
        int32_t exprRef = -1;
        int32_t href = -1;
        Ref src;
    };
    std::vector<ControlRec> controls;
    for (auto& g : gate) {
        if (!g.sym)
            continue;
        ControlRec c;
        c.src = g;
        const int32_t netIdx = b.decl->netFor(*g.sym);
        if (netIdx < 0)
            c.href = addHierRef(b, false, g, at, evalCtx);
        else
            c.exprRef = addExprRef(b, stmt, g, RefRole::Control, netIdx);
        controls.push_back(std::move(c));
    }

    for (auto& rec : targets) {
    const Ref& dst = rec.dst;
    const std::vector<PairedSrc>& pairs = rec.pairs;

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
                                 std::nullopt, &p.srcAsWritten);
        }
        if (!haveTarget)
            continue;
        anySource = anySource || srcNet >= 0 || srcHref >= 0;
        if (srcNet >= 0 && targetIdx >= 0) {
            TplDep d;
            d.src.net = srcNet;
            d.tgt.net = dstNet;
            d.stmt = stmt;
            d.operandRef = operandIdx;
            d.targetRef = targetIdx;
            d.kind = DepKind::Data;
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
            TplDep d;
            d.kind = DepKind::Data;
            d.stmt = stmt;
            d.src.net = srcNet;
            d.src.href = srcHref;
            d.tgt.net = dstNet;
            d.tgt.href = tgtHref;
            d.operandRef = operandIdx;
            d.targetRef = targetIdx;
            d.srcR = rangeOf(p.src);
            d.tgtR = rangeOf(p.tgt);
            d.mappingExact = p.mapExact ? 1 : 0;
            d.callSite = b.curCallSite;
            b.t->deps.push_back(std::move(d));
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
            d.src.net = -1;
            d.tgt.net = dstNet;
            d.stmt = stmt;
            d.targetRef = targetIdx;
            d.kind = DepKind::Data;
            d.tgtR = rangeOf(dst);
            d.callSite = b.curCallSite;
            b.t->deps.push_back(std::move(d));
        }
        else {
            // The target is in another instance: `assign u.x = 8'h5A;`.
            // Gating the constant row on a LOCAL target left every
            // outward constant write with no driver whatsoever, so a
            // trace back from the far net said nothing wrote it.
            TplDep d;
            d.kind = DepKind::Data;
            d.sourceless = true;
            d.stmt = stmt;
            d.tgt.href = tgtHref;
            d.tgtR = rangeOf(dst);
            d.callSite = b.curCallSite;
            b.t->deps.push_back(std::move(d));
        }
    }
    // Control dependencies: each recorded condition read reaches this
    // target through its branch, whichever side of the boundary either
    // end lives on.
    if (haveTarget) {
        for (auto& c : controls) {
            auto& src = c.src;
            if (c.exprRef < 0 && c.href < 0)
                continue;
            if (c.exprRef >= 0 && targetIdx >= 0) {
                const int32_t srcNet = b.decl->netFor(*src.sym);
                if (srcNet < 0)
                    continue;
                TplDep d;
                d.src.net = srcNet;
                d.tgt.net = dstNet;
                d.stmt = stmt;
                d.exprRef = c.exprRef;
                d.targetRef = targetIdx;
                d.kind = DepKind::Control;
                d.srcR = rangeOf(src);
                d.tgtR = rangeOf(dst);
                d.mappingExact = 0;
                d.callSite = b.curCallSite;
                b.t->deps.push_back(std::move(d));
            }
            else {
                TplDep d;
                d.kind = DepKind::Control;
                d.stmt = stmt;
                d.src.net = c.exprRef >= 0 ? b.decl->netFor(*src.sym) : -1;
                d.src.href = c.href;
                d.tgt.net = dstNet;
                d.tgt.href = tgtHref;
                d.exprRef = c.exprRef;
                d.targetRef = targetIdx;
                d.srcR = rangeOf(src);
                d.tgtR = rangeOf(dst);
                d.mappingExact = 0;
                d.callSite = b.curCallSite;
                b.t->deps.push_back(std::move(d));
            }
        }
    }
    }
}

    /// One call binding: the actual and the formal coupled by argument
    /// direction. The formal is a subroutine-scope net (`bump.v`); the
    /// body's own statements belong to the calling procedure, and are
    /// walked once per call site so each carries its caller's gating.
void TemplateBuilder::fileBinding(Build& b, const BindNode& n, const TplLoc& at,
                                  EvalContext& evalCtx) {
    const Ref& actual = n.actual;
    const bool reads = n.reads;
    const bool writes = n.writes;
    const bool oneToOne = n.oneToOne;
    if (!n.formal || !actual.sym)
        return;
    const int32_t stmt = n.bindable ? b.curStmt : -1;
    const int32_t formalNet = b.decl->netFor(*n.formal);
    if (formalNet < 0) {
        // The formal is not a net of THIS body, which is what a subroutine
        // declared in a package, an interface or $unit looks like from here.
        // Dropping the binding took the actual with it -- and the actual is
        // usually a perfectly good local net, so a task that plainly writes
        // its argument left that argument with no driver at all.
        //
        // Only the actual's half is recorded. The formal cannot be: at a
        // call site it is a symbol with no expression of its own, so there
        // is no reference text to file a hier_ref under -- the node carries
        // the two halves apart precisely so nobody resolves the formal
        // against the actual's spelling again.
        const int32_t actualIdx = b.decl->netFor(*actual.sym);
        if (actualIdx < 0)
            return;
        if (reads && stmt >= 0)
            addExprRef(b, stmt, actual, RefRole::CallArgument, actualIdx);
        if (writes) {
            // The target row AND a source-less `procedure` dependency, which
            // is the shape v_driver has documented since v14: "`procedure`
            // with a NULL driver_net_id is a call into a subroutine declared
            // outside this instance, whose formal is no net here". The view
            // learned to label the row; nothing emitted one, so the two
            // contracted views answered "what writes this net" differently --
            // v_net_attachment and v_stmt_target said this statement did,
            // v_driver said nothing did. A package task that plainly writes
            // its output actual read as undriven.
            //
            // The formal cannot be named as the source: at a call site it
            // is a symbol with no expression of its own, so there is no
            // reference text to resolve. A NULL source is the honest answer
            // and the one the vocabulary already has -- `data` is the kind
            // that must not be used here, since a source-less `data` row is
            // what `constant` means.
            // A call written inside a CONDITION belongs to no statement this
            // schema records, so there is no stmt_target to hang the write
            // on -- stmt_target.stmt_id is NOT NULL, and rightly, since a
            // target is a position within a statement. The dependency still
            // goes out: `if (chk(a, y))` writes y, and without the row the
            // database said nothing did, which is the answer this whole
            // branch exists to stop giving.
            int32_t targetIdx = -1;
            if (stmt >= 0) {
                TplStmtRef tr;
                tr.stmt = stmt;
                tr.ordinal = b.targetOrdinal++;
                tr.net = actualIdx;
                tr.r = rangeOf(actual);
                targetIdx = int32_t(b.t->targets.size());
                b.t->targets.push_back(std::move(tr));
            }
            TplDep d;
            d.src.net = -1;
            d.tgt.net = actualIdx;
            d.stmt = stmt;
            d.targetRef = targetIdx;
            d.kind = DepKind::Procedure;
            d.tgtR = rangeOf(actual);
            // mappingExact stays NULL: there is no source end to correspond
            // with, and a correspondence beside a driver that does not exist
            // is a claim about nothing.
            d.callSite = b.curCallSite;
            b.t->deps.push_back(std::move(d));
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
            TplDep d;
            d.kind = DepKind::Procedure;
            d.stmt = stmt;
            d.src.href = href;
            d.tgt.net = formalNet;
            d.srcR = rangeOf(actual);
            d.mappingExact = oneToOne ? 1 : 0;
            d.callSite = b.curCallSite;
            b.t->deps.push_back(std::move(d));
        }
        if (writes) {
            TplDep d;
            d.kind = DepKind::Procedure;
            d.stmt = stmt;
            d.src.net = formalNet;
            d.tgt.href = href;
            d.tgtR = rangeOf(actual);
            d.mappingExact = oneToOne ? 1 : 0;
            d.callSite = b.curCallSite;
            b.t->deps.push_back(std::move(d));
        }
        return;
    }
    if (reads) {
        int32_t exprIdx = -1;
        if (stmt >= 0)
            exprIdx = addExprRef(b, stmt, actual, RefRole::CallArgument, actualNet);
        TplDep d;
        d.src.net = actualNet;
        d.tgt.net = formalNet;
        d.stmt = stmt;
        d.exprRef = exprIdx;
        d.kind = DepKind::Procedure;
        d.srcR = rangeOf(actual);
        d.mappingExact = oneToOne ? 1 : 0;
        d.callSite = b.curCallSite;
        b.t->deps.push_back(std::move(d));
    }
    if (writes) {
        TplDep d;
        d.src.net = formalNet;
        d.tgt.net = actualNet;
        d.stmt = stmt;
        d.kind = DepKind::Procedure;
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
                rhs.push_back(Slot::unpositioned(r));
        }
        const int64_t droppedConstants = filteredConstants;
        evalCtx.reset();

        const uint64_t netWidth = bitWidthOf(net);
        Ref dstRef;
        dstRef.sym = &net;
        // A net initialiser drives all of the net; whole and exact is the
        // genuine answer, not a parity default.
        dstRef.cover = BitInterval::whole();
        dstRef.exact = true;
        const Slot dstSlot = netWidth
                                 ? Slot::at(dstRef, BitRange(0, netWidth - 1), true)
                                 : Slot::unpositioned(dstRef);
        std::vector<PairedSrc> pairs;
        for (auto& srcSlot : rhs) {
            if (!srcSlot.ref.sym)
                continue;
            std::optional<BitRange> span;
            if (!slotsOverlap(dstSlot, srcSlot, span))
                continue;
            pairs.push_back(PairedSrc{narrowed(srcSlot, span),
                                      narrowed(dstSlot, span),
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
        fileAssignment(b, {TargetRecord{dstSlot.ref, std::move(pairs)}}, {},
                       at, /*seq=*/-1, /*blocking=*/false, droppedConstants,
                       /*inSubroutine=*/false, std::string(),
                       /*isContinuous=*/true, "assign", evalCtx);
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
        const int32_t stmt = newStmt(b, StmtKind::Alias, "alias",
                                     AssignKind::None, /*seq=*/-1,
                                     std::string(), 0, at);
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
                d.src.net = sides[i].net;
                d.tgt.net = sides[j].net;
                d.stmt = stmt;
                d.operandRef = sides[i].operand;
                d.targetRef = sides[j].target;
                d.kind = DepKind::Alias;
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
        //
        // Through leafSegment, like every other child: taking `prim.name`
        // raw skipped both of the things that function exists for. An array
        // element's own name is the bare array name, so `buf u[2:1]` gave
        // one scope two nodes called `u` -- and, once the empty-name branch
        // below was reached instead, two called `$buf$n`, which `top.u[1]`
        // cannot find either. And a name needing escaping arrived unescaped,
        // so `buf \my.gate ()` wrote a node name with a dot in it: two path
        // segments where the tree contracts one.
        std::string name = leafSegment(prim);
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
                         ? PrimKind::Udp
                         : kSwitches.count(def.name) ? PrimKind::Switch
                                                     : PrimKind::Gate;
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
                d.src.net = srcNet;
                d.tgt.net = dstNet;
                d.prim = primIdx;
                d.kind = DepKind::Primitive;
                d.srcR = rangeOf(src);
                d.tgtR = rangeOf(dst);
                const bool oneBit =
                    src.exact && dst.exact &&
                    (src.cover.isRange() ? src.cover.bounds().width() == 1
                                         : bitWidthOf(*src.sym) == 1) &&
                    (dst.cover.isRange() ? dst.cover.bounds().width() == 1
                                         : bitWidthOf(*dst.sym) == 1);
                d.mappingExact = oneBit ? 1 : 0;
                d.callSite = b.curCallSite;
                b.t->deps.push_back(std::move(d));
                anyInput = true;
            }
            // pullup(y) has no input terminal; the null-source row names
            // the gate as the driver, as `q <= 8'h0` is named.
            if (!anyInput) {
                TplDep d;
                d.src.net = -1;
                d.tgt.net = dstNet;
                d.prim = primIdx;
                d.kind = DepKind::Primitive;
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
        case DefinitionKind::Interface:
            row.definitionKind = word(DefKind::Interface);
            break;
        case DefinitionKind::Program:
            row.definitionKind = word(DefKind::Program);
            break;
        default:
            row.definitionKind = word(DefKind::Module);
            break;
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
