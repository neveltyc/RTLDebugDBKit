# design.db — the field reference

Schema version 11. The version is the *consumption contract*, not the DDL: a
reader that does not know the number must refuse the file rather than read it
as though the layout held. What bumps it: removing or renaming a table the
contract names, a view or a view column; changing a column's meaning, value
domain, NULL rules, or a view's row granularity; changing the required `meta`
set. What does not: adding a table or column a v11 reader would merely not
query, or changing how a view is computed while its contract holds.

A v9 database cannot be upgraded in place. The folded model shared one row
set across every occurrence of a module variant and deduplicated its edges
across statements, so the per-occurrence identity v10 stores was never in
the file. v10 databases are produced only by re-exporting the RTL.

## What changed in v11

`alias a = b;` binds nets into one object. v10 exported nothing for it at
all, so the two halves were simply disconnected: asking what drove one
answered "nothing", and asking what read the other left out every reader
of the first.

It is now a statement of its own kind, with a dependency in each
direction between every pair of names it binds. `stmt.statement_kind`,
`net_dep.dependency_kind`, `v_driver.driver_kind` and `v_load.load_kind`
each gain `alias` — a value-domain change, which is what moves the
version even though no column does.

It is deliberately *not* modelled as a pair of continuous assignments.
That would have answered the connectivity questions correctly and made
every multiple-driver query wrong: an alias has no direction and
contributes no driver, so a net aliased to a driven one would have
reported two drivers where the design has one.

## What changed in v10

v9 folded rows onto the module variant: `symbol`, `edge`, `port` and
`assignment` hung off "a definition plus the parameter values it elaborated
with", every instance of that variant shared them, and `instance` alone
scaled with the design. v10 unfolds:

* **`module` is the source definition again** — what was written, keyed by
  (name, file, line). The parameter values a body elaborated with are
  per-occurrence facts on `inst.parameter_signature`.
* **The hierarchy is one id space.** `tree_node` is the supertype; a module
  instance is a `tree_node` plus an `inst` row under the same id, a gate a
  `tree_node` plus a `primitive` row, a generate level a bare node.
* **Every connectable object of every occurrence is a row.** `net` and
  `term` replace `symbol` and the two-faced `port`; the inside of a terminal
  (`term_map`) and its outside (`net_conn`) are separate relations, each
  with its own bit windows.
* **Dependencies are occurrences, not summaries.** `net_dep` replaces
  `edge`: one row per statement-level dependency, never deduplicated across
  statements, each naming the operand, target, condition reference, call or
  primitive it came from. The statement layer behind it (`procedure`,
  `stmt`, `assign_target`, `assign_operand`, `expr_ref`, `proc_event`) has
  real keys — v9's bare `proc`/`stmt` integers are gone.
* **References that leave an instance resolve, and carry dataflow.**
  `hier_ref` still stores the path as written; where slang resolved it and
  the target is in the export, `resolved_inst_id`/`resolved_net_id` name
  the actual rows — and the dependency that used the reference is a real
  `net_dep` row across the boundary, naming that reference on the end it
  crossed.
* **The intern tables are gone, except `data_type`.** Names are TEXT on
  their object rows; kinds and directions are words, never 0/1/2/3 codes;
  `_id` appears exactly where a column holds another table's key.
* **Nine views become twelve**, and `v_driver`/`v_load` now compose the
  hierarchy crossing (`net_conn` against `term_map`) that v9 left to every
  consumer to reinvent.

Why unfold at all: in the folded model, "this instance's `q`" was not a row
— it was an (instance path × module variant × name) combination computed by
every consumer, and a trace that crossed a boundary re-derived it at every
step. Instance-level rows give every net one id, which is what makes "who
drives bit 3 of THIS `q`" an indexed lookup and a fan-in cone a recursive
CTE instead of application-side path algebra. The price is replication:
identical instances no longer share rows. Measured on VeeRwolf (1,920
occurrences from 164 parameterised bodies, 10× replication) the database is
about 2× the folded file, not 10× — the widest tables scale with statements
and the type text stays interned.

The exporter pays analysis once per (definition, parameters) group — the
body slang's analysis manager actually analysed — and stamps each
occurrence's rows from that template. Two instances with one signature carry
identical rows under different ids; what genuinely differs per occurrence
(the place in the tree, the connections written in the parent, the
resolution of hierarchical references) is computed per occurrence.

## What it answers

* *Where am I?* `v_tree_node`: one node per level, one path segment per
  node, resolved one indexed lookup per segment.
* *What is here?* `v_net`, `v_terminal`: every declaration of this
  occurrence, implicit nets flagged, directions on the terminals that have
  them.
* *Who drives it / who reads it?* `v_driver`, `v_load`: every recorded arc,
  in-module and across the boundary, discriminated by kind.
* *Which statement did that?* `v_statement` and its target/operand views;
  every dependency names its statement.
* *What leaves this instance?* `hier_ref`, as written and — where possible
  — resolved.

## Tables

| Group | Table | One row is |
|---|---|---|
| provenance | `meta` | one key/value of the seal |
| | `source_file` | one file slang read, with its SHA-256 |
| | `file` | one path spelling rows carry, joined to its source_file |
| | `data_type` | one interned type text |
| hierarchy | `module` | one source definition |
| | `tree_node` | one level of the elaborated tree |
| | `inst` | one module/interface/program instance occurrence |
| | `primitive` | one gate, switch or UDP instance |
| objects | `net` | one connectable object of one occurrence |
| | `term` | one terminal on one occurrence's boundary |
| | `term_map` | one segment of a terminal's inside |
| | `net_conn` | one segment of a terminal's outside |
| statements | `procedure` | one always/initial/final block |
| | `stmt` | one statement or statement-level construct |
| | `assign_target` | one left-hand-side reference |
| | `assign_operand` | one right-hand-side reference |
| | `expr_ref` | one non-operand read, classified by role |
| | `proc_event` | one edge event triggered or waited on |
| dataflow | `net_dep` | one net-to-net dependency occurrence |
| boundary | `hier_ref` | one reference that leaves its instance |

The DDL in `src/DesignDb.cpp` carries the authoritative per-column comments;
this file states the semantics a consumer builds on.

### Hierarchy

**`module`** — `id, name, definition_kind, file_id, line, column`, unique on
(name, file_id, line). `definition_kind` is `module | interface | program |
checker`. However many parameterisations elaborate, the definition is one
row.

**`tree_node`** — `id, parent_node_id, name, node_kind, ordinal`. One path
segment per node, `[i]` included for array elements (`u[0]`, `lane[3]`), so
resolving `a.b[0].c` is one indexed lookup per segment against
(parent_node_id, name) and no path strings are stored. An anonymous gate
(`buf (y, a);`, the usual spelling in cell models) has no segment of its
own in the source, so it gets a synthesised one — `$buf$0`: `$`-prefixed
so it cannot collide with an identifier the source could have written,
counted per scope so siblings differ. Without it every anonymous gate
answered to the name of the instance holding it, and (parent_node_id,
name) stopped being a lookup. `ordinal` is the
order among siblings. `node_kind`:

* `root` — a top instance; has an `inst` row, no parent.
* `instance` — a resolved module/interface/program instance; has an `inst`
  row.
* `generate` — a generate block or one element of a generate array. A
  naming level; nothing subtypes it.
* `primitive` — a gate, switch or UDP; has a `primitive` row.
* `unresolved` — an instantiation whose definition slang could not find;
  has an `inst` row with `module_id` NULL and `unresolved_definition` set.
  A trace really does stop here, and that is different from stopping at a
  gate — which is why the kinds are distinct words.

The verifier holds the bijections: instance-like kinds have `inst` rows and
no others do; `primitive` likewise.

**`inst`** — `id` IS the tree_node id (one id space for the hierarchy).
`parent_inst_id` is the nearest enclosing module instance, skipping generate
levels — the ancestry `tree_node` already encodes, denormalised one hop
because every ownership rule walks it; the verifier holds the two encodings
equal. `parameter_signature` is the elaborated parameter values, normalised,
declaration order, localparams included — it over-splits and never
under-splits, exactly as v9's variant key did. Instances of one module with
one signature carry identical row sets. Location is the instantiation site;
the root has none.

**`primitive`** — `id` IS the tree_node id; `inst_id` the instance whose
body wrote it; `primitive_kind` is `gate | switch | udp`; `definition_name`
the gate's own word (`and`, `tranif1`) or the UDP's name. A primitive is not
a statement and has no terminals of its own: its dataflow is `net_dep` rows
carrying `primitive_id`, one per LRM (input, output) pairing. Expression
operators (`&`, `+`, `?:`) are not primitives — they stay inside their
statement's rows.

### Objects

**`net`** — every object that can be driven, read or wired: nets and
variables, of the instance body and its generate scopes, and — because a
dependency end is an id and an id must exist — subroutine formals, locals
and block variables, named by their scope-relative dotted path (`bump.v`,
`g[0].sig`). Parameters, type parameters and specparams are *not* here: they
are not connectivity, and folding them into the object list made every
"signals of this scope" query filter them back out. `declaration_kind` is
the net type's own word (`wire`, `wand`, `trireg`, a user-defined nettype's
name) or `variable`. `is_implicit=1` marks a net slang created for an
undeclared identifier under the active `` `default_nettype ``; its location
is the first use. `width` is the flattened bit width, NULL when the type is
not integral — bit *offsets* still index the flattened space slang computes
for unpacked objects, so ranges on a NULL-width net remain meaningful.
`scope_node_id` is the instance or generate node that declares it.

**`term`** — one terminal per port, in port-list order (`ordinal`). The
root's terminals are the design's top-level ports; a child's are the pins
its parent connects — one noun, because they are the same object seen from
two sides. `terminal_kind` is `signal | interface`; `direction` is `input |
output | inout | ref`, NULL for an interface terminal (it has none) and for
a terminal of an unresolved instance (nobody knows). `modport` is the
declared modport, when the port declares one. A non-ANSI `.p({hi, lo})`
formal is one terminal whose inside has two segments. An unresolved instance
still gets terminal rows — one per connection its parent wrote, named as
the connection names them — so "connected to a black box" stays distinct
from "unconnected".

**`term_map`** — the INSIDE of a terminal: which nets of its own instance it
stands for, one row per segment, keyed (term_id, ordinal). An ANSI port is
one whole-to-whole segment with `mapping_exact=1`; a port expression
produces one segment per element with its window of the terminal
(`term_lo/term_hi`) and of the net. Both nets belong to the terminal's own
instance; the outside is `net_conn`'s business, and keeping the two
relations apart is what v9's one-table version kept getting wrong.

**`net_conn`** — the OUTSIDE of a terminal: what the parent wired to it, one
row per atomic segment. Each concatenation element and each replication copy
is its own row with its own window of the formal — `.q({2{r}})` is two rows
whose windows tile `q`. `connection_kind`:

* `signal` — `net_id` names a net of the parent instance.
* `constant` — a tie-off. No net, but the window is kept, so the formal's
  bits tile rather than leaving a gap indistinguishable from an exporter
  bug.
* `unconnected` — recorded, not omitted: absence would also mean "the
  exporter did not get this far".
* `expression_operand` — the actual is an expression; this row is one net
  it reads. `.en(state == RUN)` samples `state` but does not alias it to
  `en`; `mapping_exact` is 0 by construction. An operand with no name in
  the parent carries `hier_ref_id` instead of `net_id`.
* `interface` — `interface_inst_id` names the bound interface instance,
  through pass-through chains: a grandchild handed the parent's own
  interface port resolves to the instance the parent was handed. NULL when
  the binding has no per-occurrence object (an interface array element).
  No dataflow arc pretends to cross an interface binding.
* `external_reference` — tied to something with no name in the parent
  (`.p(u.g[7:4])`); `hier_ref_id` says what, with `access='connect'`. It
  crosses like any other connection once that reference resolves; an
  upward tie (`.a(tb.glob)`) does not resolve, so it stays a recorded
  connection with no arc.

Width degradation: when the connection expression's width and the declared
terminal width disagree (an output narrower than the net it drives arrives
as a plain assignment with no conversion node), every element's position is
unstatable and no mapping is per-bit — `term_exact=0`, `mapping_exact=0`,
the same degradation a width-changing conversion gets. Instance-array
elements share the whole array's connection expression and degrade the same
way. Connections to an unresolved instance have no formal to measure
against: their terminal side is NULL and `mapping_exact` NULL.

### Statements

**`procedure`** — one row per always/always_ff/always_comb/always_latch/
initial/final block, `ordinal` in declaration order. Task and function
bodies do not get procedure rows: their statements belong to the calling
procedure — a `=` inside a function reached from an `assign` is still
`blocking`, and executes in no procedure (`procedure_id` NULL).

A body is walked **once per call site**, not once per subroutine. The
statements are the effect of *that* call, so they carry its gating stack
and its delay:

```systemverilog
always_ff @(posedge clk) begin
    if (g1) put(d1);
    if (g2) put(d2);
end
```

records the body's write twice, once under `g1` and once under `g2`.
Walking the body once instead was cheaper and wrong: the write inherited
only the first call site's condition, so `g2` never reached the target and
the answer depended on which call the walk happened to reach first.

Recursion is bounded by an active-call guard, so a self-calling task is
expanded once, not forever. Fan-out is bounded by a per-module budget: the
guard stops cycles but not a call DAG that branches, which costs 2^depth,
so a module that exceeds the budget stops instantiating bodies, counts the
call sites it skipped, and reports `analysis_status='partial'`. Measured
RTL does not come close — the heaviest caller among the designs exported
here has thirteen call statements.

**`stmt`** — one row per statement or statement-level construct; `{a,b} =
{x,y}` is ONE row however many targets it writes. `statement_kind` is
`assignment | assertion | wait | call | system_task | event_control |
alias`;
`construct` the construct's own word (`assign`, `always_ff`, `assert`,
`$display`, `call`, `sensitivity`, `wait`). `assignment_kind` (`continuous |
blocking | nonblocking`) is set exactly on assignments; `sequence` is
execution order within the procedure (NULL outside one, and on the
procedure-header `event_control` row that holds a non-plain sensitivity's
reads). `delay` is the delay control's normalised source text — `#3`,
`#(rise, fall)` — never a number this tool pretended to evaluate; intra-
assignment delays land on their own statement. `dropped_operand_count` is
operands not recorded: compile-time constants, and references that could not
be stored as a path.

**`assign_target` / `assign_operand`** — the statement's left- and right-
hand references, (stmt_id, ordinal)-unique, in written order. Operands
belong to the STATEMENT, not to a target: which operand feeds which target
is `net_dep`'s answer, and pairing them here is exactly the cross product v7
removed. A read occurring twice is two rows. A target outside the instance
is not here — it is a `hier_ref` with `access='write'` on the same
statement.

**`expr_ref`** — every statement read that is not an assignment operand,
classified: `control` (a branch condition over the statement — including one that
writes nothing this instance names, where no dependency can carry it and
this reference is the only record),
`assertion`, `wait` (a wait's condition), `event` (a sensitivity expression
that is not a plain net), `call_argument`, `system_task`. One read lands in
exactly one of `assign_operand`, `expr_ref` or `proc_event` — the verifier
holds the view formulas that make double counting visible.

**`proc_event`** — one row per event a procedure triggers on
(`event_kind='sensitivity'`, `stmt_id` NULL) or waits on (`'wait'`, the
statement set). All of them, since an event list has no order. `edge_kind`
is `posedge`/`negedge`/`both`, or NULL for a level-sensitive event written
explicitly (`always @(b)`) — the event is a fact whether or not it names an
edge, and a signal appearing *only* in a sensitivity list is otherwise
invisible to every load query. `net_id` is NULL when the expression is not
a plain net; the reads then live as `expr_ref` rows with role `event`.

An *implicit* list (`always @*`, `always_comb`, `always_latch`) gets no
event rows: its sensitivity IS the read set, which the dataflow rows
already carry, and duplicating it would report every combinational read
twice — once as `dataflow` and once as `sensitivity`. Which event is the
clock is deliberately not decided here.

### Dataflow

**`net_dep`** — the adjacency list `v_driver` and `v_load` index, and the
provenance record v9's deduplicated `edge` erased. One row per statement- or
primitive-level dependency occurrence: the same source reaching the same
target from two statements is two rows, each naming its statement. `{a,b} =
{x,y}` is `x -> a` and `y -> b`, each naming its operand and target rows —
never the four-way cross product. `dependency_kind`, and what must be set
(verifier-enforced):

| kind | means | names |
|---|---|---|
| `data` | an assignment moves it | `stmt_id`, and per end either the local reference (`assign_target_id` / `assign_operand_id`) or the resolved hierarchical one (`target_hier_ref_id` / `source_hier_ref_id`) — exactly one of the two per end. `source_net_id` NULL *with no source reference of either kind* is a constant driver (`q <= 8'h0`); the row still names the statement, which is what a driver query reports, and every source column is NULL with it. |
| `control` | it reaches the target through a branch condition | `stmt_id`, the condition as `expr_ref_id` (role `control`) or `source_hier_ref_id`, the target as `assign_target_id` or `target_hier_ref_id`; `mapping_exact` 0 — a condition gates, it does not map |
| `primitive` | a gate/switch/UDP couples them | `primitive_id`, per LRM (input, output) pairing; scalar-to-scalar couplings are per-bit |
| `alias` | an `alias` statement binds them into one object | `stmt_id`, and both an `assign_target_id` and an `assign_operand_id`, since every name an alias binds is written and read at once. One row per ordered pair: `alias a = b = c;` binds every pair mutually rather than in a chain, so it is six rows, not two. `mapping_exact` is 1 — an alias is bit for bit by definition — unless a side could not be narrowed. |
| `procedure` | a call binds them | actual to formal by argument direction, formal to actual for outputs; `stmt_id` the calling statement (NULL for a call in a control expression), `expr_ref_id` (role `call_argument`) or `source_hier_ref_id` on the reading side |

A dependency can cross by name on the target side with no source at all:
`assign u.x = 8'h5A;` and `$readmemh("f.hex", u.mem)` both drive an object
in another instance from something this schema cannot name. Those record a
source-less dependency against the resolved target, so the far net has a
driver — `constant` or `system_task` — instead of appearing never to have
been written.

**Dataflow that crosses by name.** `assign q = u.x;` and a modport write
are dataflow like any other, and they are recorded like any other: the
dependency carries the resolved net at both ends and names the `hier_ref`
row the reference went through instead of a local operand/target row. The
pairing is made where the statement was walked — one row per (source
element, target element) that share bits — never by joining `hier_ref` to
operands on `stmt_id` afterwards, which would resurrect the cross product
v7 removed. A reference that did not resolve produces no dependency at all:
the `hier_ref` text is the honest record, and a guessed edge would be a
wrong one. (In v10's first cut these produced only `hier_ref` rows, so a
target fed entirely from outside reported `constant` — a confident wrong
answer, which is exactly what this schema exists not to give.)

`source_net_id`/`target_net_id` repeat what the referenced rows already
know. That is deliberate, *verified* redundancy — this table is the
driver/load index, and the verifier holds the copies equal — not two
independent sources of truth.

### Leaving the instance

**`hier_ref`** — one row per (reference, direction, statement) that leaves
its instance: an XMR, an interface member, a package item. `path` is the
canonical text as written (selects resolved to the constants they
elaborated to, whitespace and comments removed); `access` is
`read | write | connect`. The statement is part of the identity because a
task body is walked once per call site: two calls to a task reading `u.x`
are two statements and two rows, so "what does *this* statement read
outside the instance" has an answer for each.

The range is the one the RTL spells, not the one a dependency uses.
`assign {hi, lo} = u.x;` records one reference to the whole of `u.x`,
while the two dependencies through it carry `[7:4]` and `[3:0]` in
`net_dep` — where a range describes a particular dependency rather than
the reference itself. New in v10, because
an occurrence knows its place in the hierarchy where a folded row could not:
`resolved_inst_id` and `resolved_net_id` name the actual rows when the
export could replay the reference —

* downward (`u_cnt.cnt`): resolved, per occurrence;
* absolute and package-scoped paths into the exported tree: resolved;
* through one of the instance's own interface ports (`bus.vld`, modports
  included): resolved to the interface instance each occurrence is actually
  bound to;
* upward references and interface-array bindings: NULL. The one analysed
  body speaks for occurrences whose surroundings may differ, and a guess
  stored as fact is the one thing the columns must never hold.

NULL `resolved_*` is "not resolved here", never a fabricated object. Bare
names that leave the instance (a package-level free variable used without
its package) resolve against imports this table cannot see; they are
counted, not stored.

## Bit ranges

One encoding everywhere, unchanged since v7. A range is LSB-relative offsets
into the flattened object, NOT declared indices: `logic [15:8] off` has bit
15 at offset 7, `logic [0:7] up` has bit 0 at offset 7. A consumer that maps
offsets straight onto declared indices mislabels every signal not declared
`[N-1:0]`; the declared shape is recoverable from the type text.

* NULL bits with `exact=1` — the whole object.
* NULL bits with `exact=0` — somewhere inside it, unknown where.
* present bits with `exact=1` — exactly those bits.
* present bits with `exact=0` — an upper bound, not the bits actually
  touched (a dynamic selector).

`mapping_exact` is never a restatement of the two sides' own `exact` flags:
those describe each end's range, this describes the correspondence BETWEEN
the ends. `q = a + b` knows both ranges exactly and still cannot say which
bit of `a` reaches which bit of `q`, because a carry crosses them. 0 is not
doubt — the dependency is real — it is range granularity. NULL is "no
second end to correspond with" (a constant, an unconnected pin).

Columns describing an end that does not exist are NULL together: a tie-off
never reads as "the whole of nothing, exactly".

## The stable query interface

Twelve views. Their existence, column sets and order, column semantics, NULL
rules and row granularity are the contract; `verify-designdb.py` asserts all
of it on every export. Ground rules:

* A FACT view's row is one base-table row — `v_tree_node`, `v_net`,
  `v_terminal`, `v_terminal_map`, `v_net_connection`, `v_net_dependency`,
  `v_statement`, `v_statement_target`, `v_statement_operand` — and
  count(view) == count(base) is checked. Every internal join is against a
  primary key; nothing fans out.
* `v_driver` and `v_load` are COMPOSITE: UNION ALL branches discriminated
  by `driver_kind`/`load_kind`, each branch's row count reconcilable by a
  formula the verifier evaluates. The dependency, event, statement and
  terminal branches reconcile against base tables; the two crossing
  branches reconcile against `v_conn_arc`, which is the composition
  itself — so a fault inside that composition would inflate both sides
  equally and pass. That is the one seam these formulas do not check.
* `v_conn_arc` exists in the file but is NOT contract: it is the scaffolding
  the two composite views share, may change or vanish without a version
  bump, and consumers must not query it.
* Point queries seek. `v_driver` and `v_load` by `signal_net_id`,
  `v_net_dependency` by `target_net_id`, `v_net_connection` by `net_id`
  use an index, never a base-table scan — the closure is the consumer's,
  one point query per hop, so a scan per hop would be a scan per net in
  the cone. The verifier asserts the query plan itself; a change to how a
  view is computed that regresses this fails the export rather than
  shipping a database that answers slowly.
* Explicit column lists, never `SELECT *`; no transitive closure — a
  fan-in cone is the consumer's recursive query, one step per row here.

**`v_database_info`** — the meta seal as one row, counts CAST to INTEGER:
`schema_version, tool_version, slang_version, producer_revision, top,
analysis_status, error_count, unresolved_count, empty_procedure_count,
duplicate_path_count, config_digest`.

**`v_tree_node`** — one row per node: `node_id, parent_node_id, node_name,
node_kind, ordinal, instance_id, parent_instance_id, module_id, module_name,
parameter_signature, definition_name, file_path, source_path, source_line,
source_column`. NULL by kind: `generate` has no subtype columns; `primitive`
has `instance_id` NULL, `parent_instance_id` its owning instance and
`definition_name` the gate/UDP name; `unresolved` has `module_id` NULL and
`definition_name` the unresolvable spelling; the root and generate levels
have no location.

**`v_net`** — one row per net: `net_id, instance_id, module_id, module_name,
parameter_signature, scope_node_id, net_name, declaration_kind, data_type,
width, is_implicit, file_path, source_path, source_line, source_column`.
No direction column — direction belongs to terminals, and a net's port-ness
is one `v_terminal_map` join away.

**`v_terminal`** — one row per terminal: `terminal_id, instance_id,
module_id, module_name, terminal_name, terminal_kind, direction, data_type,
width, ordinal, is_const, modport, file_path, source_path, source_line,
source_column`.

**`v_terminal_map`** — one row per inside segment: `terminal_id,
terminal_instance_id, terminal_name, mapping_ordinal, internal_net_id,
internal_net_name, terminal_lo, terminal_hi, terminal_exact, net_lo, net_hi,
net_exact, mapping_exact`.

**`v_net_connection`** — one row per outside segment: `connection_id,
net_id, net_instance_id, net_name, terminal_id, terminal_instance_id,
terminal_name, direction, connection_kind, ordinal, net_lo, net_hi,
net_exact, terminal_lo, terminal_hi, terminal_exact, mapping_exact,
interface_instance_id, hier_ref_id, file_path, source_path, source_line,
source_column`. Deliberately NOT composed with `term_map` — this view is
the fact, `v_driver`/`v_load` are the composition.

**`v_net_dependency`** — one row per dependency: `dependency_id,
source_net_id, source_instance_id, source_name, source_lo, source_hi,
source_exact, target_net_id, target_instance_id, target_name, target_lo,
target_hi, target_exact, statement_id, assign_operand_id, assign_target_id,
expression_reference_id, primitive_id, source_hier_ref_id,
target_hier_ref_id, dependency_kind, mapping_exact, file_path, source_path,
source_line, source_column`. Location is the statement's, or the
primitive's for a primitive arc. A row whose `source_instance_id` and
`target_instance_id` differ crossed by name; the `*_hier_ref_id` column on
that end names the reference it went through. Not deduplicated.

**`v_driver`** — every direct driving arc of `signal_net`, one row each:
`signal_net_id, signal_instance_id, signal_name, signal_lo, signal_hi,
signal_exact, driver_net_id, driver_instance_id, driver_name, driver_lo,
driver_hi, driver_exact, driver_kind, dependency_id, connection_id,
statement_id, primitive_id, terminal_id, mapping_exact, file_path,
source_path, source_line, source_column`. `driver_kind`:

* `data | control | primitive | procedure` — a `net_dep` row, kind carried
  through.
* `connection` — the crossing: for an input/inout/ref terminal the
  parent-side net drives the child's internal net; for output/inout/ref the
  internal net drives the parent's. `inout` and `ref` arc both ways, one
  row each. An external tie (`.p(u.g[7:4])`) whose reference resolved
  crosses the same way, its far net as the outer end; one that did not
  resolve contributes no arc. The two windows are
  intersected on the terminal; a side's range is narrowed through the
  overlap when every link of its chain is exact, and degraded to `exact=0`
  otherwise. Interface bindings do not arc.
* `connection_expression` — the actual is an expression; each net it reads
  drives the internal net at range granularity.
* `constant` — a tie-off or constant right-hand side: `driver_net_id` NULL
  and every driver column NULL with it. The statement or connection is
  still named — that is what a driver query reports.
* `alias` — an `alias` statement binds the two nets into one object.
  Both directions exist, so each is the other's driver and the other's
  load. The kind is what keeps it out of a multiple-driver count.
* `system_task` — a system task wrote the argument. `driver_net_id` is
  NULL, as for a constant, because the source is a file or a plusarg
  rather than a net; `statement_id` names the call. Kept apart from
  `constant` so "is this tied off?" and "is this loaded at startup?" are
  different answers.
* `terminal` — the design boundary. A root instance's input/inout/ref
  terminal drives the net it stands for: `driver_net_id` is NULL (the
  world outside the export is the driver) and `terminal_id` names the pin.
  Without it, a top-level input's net reported no driver at all, and a
  consumer could not tell "nothing drives this" from "this is where the
  design ends" — two answers that mean opposite things.

An unconnected terminal contributes no row.

**`v_load`** — every recorded read of `signal_net`, one row each:
`signal_net_id, signal_instance_id, signal_name, signal_lo, signal_hi,
signal_exact, load_net_id, load_instance_id, load_name, load_lo, load_hi,
load_exact, load_kind, dependency_id, connection_id, statement_id,
procedure_id, terminal_id, mapping_exact, file_path, source_path,
source_line, source_column`. `load_kind`: `dataflow` (a dependency reads
it), `connection` (the crossing reads it; `load_net` is the far side),
`alias` (the other name the same object goes by),
`sensitivity`, `wait`, `statement` (an assertion, a `$display`, a read
whose statement has no local target — including the *condition* gating
such a statement, which no dependency can carry), `terminal` (a root output/inout/ref
terminal reads the net it stands for — the boundary counterpart of
v_driver's `terminal`). The last four have `load_*` NULL: a real reader
with no nameable target. One read, one row: a reference already carried
into `dataflow` by a dependency is not repeated as `statement`. The netlist
analogy decides membership: a clock net's loads include the flop clock
pins, so sensitivity is a load.

**`v_statement`** — one row per statement: `statement_id, instance_id,
module_id, module_name, scope_node_id, procedure_id, ordinal, sequence,
statement_kind, construct, assignment_kind, delay, dropped_operand_count,
file_path, source_path, source_line, source_column`.

**`v_statement_target` / `v_statement_operand`** — one row per reference:
`target_id/operand_id, statement_id, ordinal, net_id, net_name, *_lo, *_hi,
*_exact`.

`file_path` is the spelling as written in the filelist; `source_path` the
absolute path it resolved to. They answer different questions and neither
substitutes for the other.

## Naming rules

* Tables are `singular_snake_case`. `_id` appears exactly where a column
  holds another table's primary key, and nowhere else.
* A bit range is prefixed with the end it describes (`source_lo`,
  `term_exact`); a single-range table spells its own bare (`lo`/`hi`/
  `is_exact`).
* Kinds, directions and roles are their words. The words are a wire format:
  they come from this schema, not from slang's enum printer, so a slang
  upgrade cannot change the vocabulary underneath a consumer.
* `ordinal` is position in a declaration or extraction list; `sequence` is
  execution order inside a procedure. Neither is an identity.
* Every id is issued by the exporter in one pass; 0 is never an id. The
  REFERENCES clauses are enforced by the verifier's `foreign_key_check`,
  not per-insert — and the enum CHECK clauses are, by default, not in the
  shipped file at all: a string IN-list evaluated per row costs more than
  the rest of the insert, so the exporter writes them only under
  `--check-constraints`, and the verifier re-derives every domain from the
  finished file either way. The value domains are contract; their CHECK
  spelling is not. Read the domains from this document, never from
  `.schema`.

## Provenance

`source_file` holds every file slang actually read, absolute path and
SHA-256, so a consumer can tell the database and the RTL diverged instead of
answering from stale data. `file` holds the spellings rows carry — as
written in the filelist — joined to their source_file. `meta` is the seal;
its required keys are the `v_database_info` columns plus `tool`, except
`top` — the space-separated names of the elaborated top instances — which
is absent when the design elaborates none.
`analysis_status` is `complete | partial | hierarchy_only` and must agree
with the counts beside it: errors, skipped procedures, duplicated paths
(two siblings answering one (parent, name) pair, so a path lookup stops
resolving uniquely) and truncated call expansions make `partial`.
Unresolved instantiations deliberately do not — a design instantiating a
vendor macro it has no source for is as complete as this tool can make
it, and the count is there for a consumer that judges otherwise.
`unresolved_count` counts unresolved
instantiation *sites* (one per written instantiation, however many
occurrences stamp out); the per-occurrence picture is
`tree_node.node_kind='unresolved'`. `config_digest` fingerprints the inputs;
two exports with one digest saw the same filelist, defines and flags.

## What is deliberately not here

Unchanged from v9, and still deliberate:

* No clock domains, no `clocked` flag, no election of "the" clock. That
  needs constraints this tool does not read; a consumer that knows gets
  both events of `@(posedge clk or negedge rst_n)` and can say.
* No branch-condition truth tables on statements. Which assignment "was in
  effect" is not evaluable from a waveform by SQL, and encoding the
  judgement as data would produce confident wrong answers. The conditions
  ARE recorded — as `control` dependencies with their expression
  references — the reasoning belongs to the reader.
* No source text. The file, line and column are here; the text is in the
  file.
* No expression trees, no temporaries. `assign y = (a & b) | c` is three
  operands, three dependencies onto `y`, `mapping_exact=0`, and no
  fabricated `tmp` net. A consumer that needs the expression's shape reads
  the source at the location the row names.
* A system task that writes an argument — `$readmemh` into a memory,
  `$sscanf` or `$value$plusargs` into a variable, `$cast` into its
  destination — records a target and a source-less dependency, surfacing
  in `v_driver` as `system_task`. The signal is genuinely driven; the
  source is a file or a plusarg, which is outside anything this schema
  names. It is not a `constant`, and the kinds are separate so a consumer
  cannot mistake one for the other.
* `force` records as a blocking assignment; `release` leaves no row.
* Variable initialisers (`logic [7:0] c = 0`) are not drivers; net
  initialisers (`wire w = a & b`) are, because the LRM says so.

## Known limits

* Upward hierarchical references (`$root.`-relative climbs from a shared
  body) keep `resolved_*` NULL — the one analysed body speaks for
  occurrences whose surroundings may differ. The path text is still there.
* Interface-array element bindings (`.b(arr[k])`) have no per-occurrence
  instance id; `interface_instance_id` stays NULL and the member references
  through them stay text-only.
* A non-ANSI concatenation formal (`.p({hi, lo})`) is exported as one
  terminal with its two inside segments, but no lint fixture covers it:
  Verilator rejects the construct, so it is verified by hand.
* Task and function bodies have no `procedure` rows; their statements
  belong to the procedure that reached them, and a subroutine no procedure
  walks contributes nothing. A body called from N sites is N sets of rows,
  bounded by a per-module expansion budget — a pathological call DAG that
  exceeds it reports the skipped sites rather than exhausting memory, and
  `meta` says the export is `partial`.
* A subroutine's formals are one net per subroutine, not one per call site.
  Each call's gating and delay are its own, but the formal is shared, so
  the transitive cone through a task called twice admits combinations no
  single call makes (`g1` with the second call's argument). Per-call-site
  formals would fix it and are not in v10.
* A resolved external tie crosses at range granularity: `net_conn` does
  not record a `mapping_exact` for it, so the arc reports 0 even where
  both windows are exact. Recording it would let those ties be traced bit
  by bit; it is not in v10.
* A function call contributes both a summary arc (each argument to the
  call's target) and the detail arcs through its formals, so a fan-out
  count over both double-counts that read. The detail path also stops at
  the function's return net, which has no arc onward to the target.
* A macro-assembled reference spans two buffers and cannot be recovered as
  one span; it is counted (`meta` external tally), not stored.
* Statements slang marks bad take their enclosing block out of the walk;
  `empty_procedure_count` says how often, and the diagnostics say why.

## Reading it honestly

The database is a build artifact: rebuilt from source, sealed by digest,
version-gated. Read `v_database_info` first; refuse a version you do not
know. Treat `exact=0` ranges as upper bounds, `mapping_exact=0` as "follow
at range granularity", NULL resolved columns as "resolve it yourself or do
not" — every one of those is the exporter telling the truth about what it
could not narrow, and the verifier exists to keep it that way.
