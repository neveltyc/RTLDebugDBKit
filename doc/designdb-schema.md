# design.db — the field reference

Schema version 13. The version is the *consumption contract*, not the DDL: a
reader that does not know the number must refuse the file rather than read it
as though the layout held. What bumps it: removing or renaming a table the
contract names, a view or a view column; changing a column's meaning, value
domain, NULL rules, or a view's row granularity; changing the required `meta`
set. What does not: adding a table or column an older reader would merely not
query, or changing how a view is computed while its contract holds.

No database is upgraded in place: a version bump means re-exporting the RTL.
(For v9 that was structural — the folded model never stored the
per-occurrence identity v10 needs; for v12/v13 it is the renames and the
grown value domains.)

## What changed in v13

Three things a v12 reader either could not ask or could not ask precisely.

**`v_net_attachment` gets typed ids.** Its single polymorphic `other_id`
became seven typed nullable columns — `term_id, stmt_target_id,
assign_operand_id, expr_ref_id, proc_id, dep_id, hier_ref_id` — exactly one
non-null per row, the one `attachment_kind` names. The same exclusive-arc
shape `net_dep` already uses: a consumer joins the right base table without
decoding the kind.

**Packages become first-class objects.** A package is a pseudo-occurrence
now — a `tree_node`/`inst` with `node_kind`/`def_kind` `'package'`, above
the roots (`parent_inst_id` NULL), its variables ordinary `net` rows. So a
`pkg::mask` reference resolves to that net instead of dead-ending: the
reading module shows `driver_kind='data'` through the package net, two
modules reading one package variable meet on it, and `hier_ref` carries the
resolution like any other. This *narrows* `driver_kind='external'` to what
is genuinely unresolvable — an upward hierarchical reference from a shared
body, an interface-array binding. (`$unit` compilation-unit items are not
stamped yet and stay external.)

*(The call-site section is added as that phase lands.)*

## What changed in v12

Three families of change, one bump.

**One spelling per word.** The schema had a deliberate double standard —
tables spelled `inst`, views spelled `instance_id` — and half the
vocabulary appeared both abbreviated and written out. v12 abolishes it:
one classic abbreviation per word, applied to every identifier.

| word | | word | | word | |
|---|---|---|---|---|---|
| instance | `inst` | statement | `stmt` | source | `src` |
| terminal | `term` | procedure | `proc` | target | `tgt` |
| connection | `conn` | primitive | `prim` | declaration | `decl` |
| dependency | `dep` | expression | `expr` | definition | `def` |
| hierarchy | `hier` | reference | `ref` | assignment | `assign` |
| interface | `intf` | parameter | `param` | mapping | `map` |
| database | `db` | column | `col` | | |

The boundary rules: only identifiers — enum values and meta keys are data,
and mostly the LRM's own words, so `'interface'` and `schema_version` stay
spelled out. Role words stay whole (driver, load, signal, resolved,
parent, scope, node). Words with no classic abbreviation are not given an
invented one (operand, ordinal, sequence, signature, width).

Table renames: `source_file → src_file`, `primitive → prim`,
`procedure → proc`, and `assign_target → stmt_target` — it holds a
release's lvalue and a system task's write target as well as an
assignment's, so it was never assignment-only (`assign_operand`, which
is, keeps its name; the FK column `net_dep.stmt_target_id` becomes
`stmt_target_id`). View renames: `v_database_info → v_db_info`,
`v_terminal → v_term`, `v_terminal_map → v_term_map`,
`v_net_connection → v_net_conn`, `v_net_dependency → v_net_dep`,
`v_statement* → v_stmt*`.

The one semantic rename rides along: `net_conn` and `term_map` each
describe one side of a terminal, and only the terminal's own columns said
so. The actual's columns now wear `outer_` (`net_conn` is the pin seen
from the parent — VPI's highConn) and `term_map`'s net columns wear
`inner_` (the pin's inside — vpiLowConn), so the two tables stop reusing
one column name for opposite sides of the boundary, and the crossing
composition reads as the outer × inner join it is.

**The silent answers got kinds or markers.**

* `driver_kind='external'`: a dependency whose SOURCE is a reference this
  export has no net row for — a package variable, an upward name — is now
  written (src net NULL, the reference on the source end) instead of
  dropped. Dropping it had made "driven through a name this export cannot
  resolve" indistinguishable from "undriven".
* `force` stamps `construct='force'` (procedural `assign`,
  `'proc_assign'`) on its otherwise-ordinary blocking assignment, and
  `release`/`deassign` become `stmt_kind='release'` rows naming their
  lvalues and driving nothing — the hijack and its end are both findable.
* An external tie (`.p(u.g[7:4])`) records `map_exact`, so its crossing
  arc is traceable bit by bit instead of pessimised to 0.
* `prim_kind='switch'` covers the LRM's whole switch family; rtran and
  the MOS switches were labelled gates.

**Additive** (no reader obligation): `inst_param` — `param_signature`
made queryable, one row per elaborated parameter value;
`v_net_attachment` — everything touching one net, one row per
attachment, the thirteenth contract view; and an index on
`tree_node(parent_node_id, name)`, the access path the path-resolution
contract always promised.

## What changed in v11

`alias a = b;` binds nets into one object. v10 exported nothing for it at
all, so the two halves were simply disconnected: asking what drove one
answered "nothing", and asking what read the other left out every reader
of the first.

It is now a statement of its own kind, with a dependency in each
direction between every pair of names it binds. `stmt.stmt_kind`,
`net_dep.dep_kind`, `v_driver.driver_kind` and `v_load.load_kind`
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
  per-occurrence facts on `inst.param_signature`.
* **The hierarchy is one id space.** `tree_node` is the supertype; a module
  instance is a `tree_node` plus an `inst` row under the same id, a gate a
  `tree_node` plus a `prim` row, a generate level a bare node.
* **Every connectable object of every occurrence is a row.** `net` and
  `term` replace `symbol` and the two-faced `port`; the inside of a terminal
  (`term_map`) and its outside (`net_conn`) are separate relations, each
  with its own bit windows.
* **Dependencies are occurrences, not summaries.** `net_dep` replaces
  `edge`: one row per statement-level dependency, never deduplicated across
  statements, each naming the operand, target, condition reference, call or
  primitive it came from. The statement layer behind it (`proc`,
  `stmt`, `assign_target`, `assign_operand`, `expr_ref`, `proc_event`) has
  real keys — v9's bare per-module integers are gone.
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
* *What is here?* `v_net`, `v_term`: every declaration of this
  occurrence, implicit nets flagged, directions on the terminals that have
  them.
* *Who drives it / who reads it?* `v_driver`, `v_load`: every recorded arc,
  in-module and across the boundary, discriminated by kind.
* *Which statement did that?* `v_stmt` and its target/operand views;
  every dependency names its statement.
* *What leaves this instance?* `hier_ref`, as written and — where possible
  — resolved.

## Tables

| Group | Table | One row is |
|---|---|---|
| provenance | `meta` | one key/value of the seal |
| | `src_file` | one file slang read, with its SHA-256 |
| | `file` | one path spelling rows carry, joined to its src_file |
| | `data_type` | one interned type text |
| hierarchy | `module` | one source definition |
| | `tree_node` | one level of the elaborated tree |
| | `inst` | one module/interface/program instance occurrence |
| | `inst_param` | one elaborated parameter value of one occurrence |
| | `prim` | one gate, switch or UDP instance |
| objects | `net` | one connectable object of one occurrence |
| | `term` | one terminal on one occurrence's boundary |
| | `term_map` | one segment of a terminal's inside |
| | `net_conn` | one segment of a terminal's outside |
| statements | `proc` | one always/initial/final block |
| | `stmt` | one statement or statement-level construct |
| | `stmt_target` | one statement's target reference (LHS, release, system write) |
| | `assign_operand` | one assignment right-hand-side reference |
| | `expr_ref` | one non-operand read, classified by role |
| | `proc_event` | one edge event triggered or waited on |
| dataflow | `net_dep` | one net-to-net dependency occurrence |
| boundary | `hier_ref` | one reference that leaves its instance |

The DDL in `src/DesignDb.cpp` carries the authoritative per-column comments;
this file states the semantics a consumer builds on.

### Hierarchy

**`module`** — `id, name, def_kind, file_id, line, column`, unique on
(name, file_id, line). `def_kind` is `module | interface | program |
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
* `primitive` — a gate, switch or UDP; has a `prim` row.
* `unresolved` — an instantiation whose definition slang could not find;
  has an `inst` row with `module_id` NULL and `unresolved_def` set.
  A trace really does stop here, and that is different from stopping at a
  gate — which is why the kinds are distinct words.
* `package` — a package, a pseudo-occurrence above the roots: an `inst`
  row with `parent_inst_id` NULL and a `module` of `def_kind='package'`,
  its variables `net` rows. It is not in the elaborated tree the way an
  instance is — it has no parent and instantiates nothing — but giving it
  the same object rows is what lets a `pkg::x` reference resolve to a real
  net. See *Packages*.

The verifier holds the bijections: instance-like kinds (`root`, `instance`,
`unresolved`, `package`) have `inst` rows and no others do; `primitive`
likewise. Parentless nodes are exactly the roots and packages.

**`inst`** — `id` IS the tree_node id (one id space for the hierarchy).
`parent_inst_id` is the nearest enclosing module instance, skipping generate
levels — the ancestry `tree_node` already encodes, denormalised one hop
because every ownership rule walks it; the verifier holds the two encodings
equal. `param_signature` is the elaborated parameter values, normalised,
declaration order, localparams included — it over-splits and never
under-splits, exactly as v9's variant key did. Instances of one module with
one signature carry identical row sets. Location is the instantiation site;
the root has none.

**`inst_param`** — `param_signature` made queryable: `(inst_id, ordinal,
name, value)`, one row per elaborated parameter value, declaration order,
localparams and type parameters included — the same normalisation the
signature is built from, and the verifier holds the two representations
byte-for-byte equal per occurrence. "Every instance with WIDTH=8" was a
LIKE over the signature (and matched XWIDTH=8); it is an indexed seek on
(name, value) now. Additive in v12.

**`prim`** — `id` IS the tree_node id; `inst_id` the instance whose
body wrote it; `prim_kind` is `gate | switch | udp` — `switch` is the
LRM's whole switch family (tran/tranif\*, the resistive variants, and the
MOS switches), not just what slang labels bidirectional; `def_name`
the gate's own word (`and`, `tranif1`) or the UDP's name. A primitive is not
a statement and has no terminals of its own: its dataflow is `net_dep` rows
carrying `prim_id`, one per LRM (input, output) pairing — an `inout` end
couples both ways, one row per direction, self-pairing excluded, so a
`tran a b` is exactly the two arcs a↔b. Expression
operators (`&`, `+`, `?:`) are not primitives — they stay inside their
statement's rows.

### Objects

**`net`** — every object that can be driven, read or wired: nets and
variables, of the instance body and its generate scopes, and — because a
dependency end is an id and an id must exist — subroutine formals, locals
and block variables, named by their scope-relative dotted path (`bump.v`,
`g[0].sig`). Parameters, type parameters and specparams are *not* here: they
are not connectivity, and folding them into the object list made every
"signals of this scope" query filter them back out. `decl_kind` is
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
two sides. `term_kind` is `signal | interface`; `direction` is `input |
output | inout | ref`, NULL for an interface terminal (it has none) and for
a terminal of an unresolved instance (nobody knows). `modport` is the
declared modport, when the port declares one. A non-ANSI `.p({hi, lo})`
formal is one terminal whose inside has two segments. An unresolved instance
still gets terminal rows — one per connection its parent wrote, named as
the connection names them — so "connected to a black box" stays distinct
from "unconnected".

**`term_map`** — the INSIDE of a terminal (VPI's lowConn): which nets of
its own instance it stands for, one row per segment, keyed (term_id,
ordinal). An ANSI port is one whole-to-whole segment with `map_exact=1`; a
port expression produces one segment per element with its window of the
terminal (`term_lo/term_hi`) and of the net (`inner_lo/inner_hi`). Both
nets belong to the terminal's own instance; the outside is `net_conn`'s
business, and keeping the two relations apart is what v9's one-table
version kept getting wrong.

**`net_conn`** — the OUTSIDE of a terminal (VPI's highConn): what the
parent wired to it, one row per atomic segment. Each concatenation element
and each replication copy is its own row with its own window of the formal
— `.q({2{r}})` is two rows whose windows tile `q`. `conn_kind` decides
which outer column is set — the kind first, then its pointer:

| conn_kind | outer end | |
|---|---|---|
| `signal` | `outer_net_id` | a net of the parent instance |
| `constant` | — | a tie-off; the term window is kept so the formal's bits tile rather than leaving a gap indistinguishable from an exporter bug |
| `unconnected` | — | recorded, not omitted: absence would also mean "the exporter did not get this far" |
| `expression_operand` | `outer_net_id` or `outer_hier_ref_id` | the actual is an expression; this row is one net it reads. `.en(state == RUN)` samples `state` but does not alias it to `en`; `map_exact` is 0 by construction |
| `interface` | `outer_intf_inst_id` | the bound interface instance, through pass-through chains: a grandchild handed the parent's own interface port resolves to the instance the parent was handed. NULL when the binding has no per-occurrence object (an interface array element). No dataflow arc pretends to cross an interface binding |
| `external_reference` | `outer_hier_ref_id` | tied to something with no name in the parent (`.p(u.g[7:4])`); the reference says what, with `access='connect'`. It crosses like any other connection once the reference resolves — with a `map_exact` of its own since v12, so the arc is traceable bit by bit — while an upward tie (`.a(tb.glob)`) stays a recorded connection with no arc |

Width degradation: when the connection expression's width and the declared
terminal width disagree (an output narrower than the net it drives arrives
as a plain assignment with no conversion node), every element's position is
unstatable and no mapping is per-bit — `term_exact=0`, `map_exact=0`,
the same degradation a width-changing conversion gets. Instance-array
elements share the whole array's connection expression and degrade the same
way. Connections to an unresolved instance have no formal to measure
against: their terminal side is NULL and `map_exact` NULL.

### Statements

**`proc`** — one row per always/always_ff/always_comb/always_latch/
initial/final block, `ordinal` in declaration order. Task and function
bodies do not get procedure rows: their statements belong to the calling
procedure — a `=` inside a function reached from an `assign` is still
`blocking`, and executes in no procedure (`proc_id` NULL).

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
{x,y}` is ONE row however many targets it writes. `stmt_kind` is
`assignment | assertion | wait | call | system_task | event_control |
alias | release`;
`construct` the construct's own word (`assign`, `always_ff`, `assert`,
`$display`, `call`, `sensitivity`, `wait` — and `force`/`proc_assign` on
the assignment a `force` or procedural `assign` makes, `release`/
`deassign` on a release row). `assign_kind` (`continuous |
blocking | nonblocking`) is set exactly on assignments; `sequence` is
execution order within the procedure (NULL outside one, and on the
procedure-header `event_control` row that holds a non-plain sensitivity's
reads). `delay` is the delay control's normalised source text — `#3`,
`#(rise, fall)` — never a number this tool pretended to evaluate; intra-
assignment delays land on their own statement. `dropped_operand_count` is
operands not recorded: compile-time constants, and references that could not
be stored as a path.

**`stmt_target` / `assign_operand`** — the statement's target and its
right-hand references, (stmt_id, ordinal)-unique, in written order.
`stmt_target` is any statement's target, not only an assignment's — a
release names its lvalue here (fed by no dependency: releasing is not
driving, so the rows answer "where does the force end", never "who drives
this"), and a system task names its write target here — which is why it
is not `assign_target`. `assign_operand` keeps its name: an operand is an
assignment RHS read and nothing else produces one. Operands belong to the
STATEMENT, not to a target: which operand feeds which target is
`net_dep`'s answer, and pairing them here is exactly the cross product v7
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
never the four-way cross product. `dep_kind`, and what must be set
(verifier-enforced):

| kind | means | names |
|---|---|---|
| `data` | an assignment moves it | `stmt_id`, and per end either the local reference (`stmt_target_id` / `assign_operand_id`) or the hierarchical one (`tgt_hier_ref_id` / `src_hier_ref_id`) — exactly one of the two per end. `src_net_id` NULL *with no source reference of either kind* is a constant driver (`q <= 8'h0`); the row still names the statement, and every src column is NULL with it. `src_net_id` NULL *with* `src_hier_ref_id` is an **external** driver: the reference did not resolve to a net row, the spelled window survives, and `v_driver` says `'external'`. |
| `control` | it reaches the target through a branch condition | `stmt_id`, the condition as `expr_ref_id` (role `control`) or `src_hier_ref_id`, the target as `stmt_target_id` or `tgt_hier_ref_id`; `map_exact` 0 — a condition gates, it does not map |
| `primitive` | a gate/switch/UDP couples them | `prim_id`, per LRM (input, output) pairing; scalar-to-scalar couplings are per-bit |
| `alias` | an `alias` statement binds them into one object | `stmt_id`, and both an `stmt_target_id` and an `assign_operand_id`, since every name an alias binds is written and read at once. One row per ordered pair: `alias a = b = c;` binds every pair mutually rather than in a chain, so it is six rows, not two. `map_exact` is 1 — an alias is bit for bit by definition — unless a side could not be narrowed. |
| `procedure` | a call binds them | actual to formal by argument direction, formal to actual for outputs; `stmt_id` the calling statement (NULL for a call in a control expression), `expr_ref_id` (role `call_argument`) or `src_hier_ref_id` on the reading side |

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
v7 removed.

The two ends part ways on failure. An unresolved TARGET reference
produces no dependency: `tgt_net_id` is NOT NULL, and a guessed written
object would be a loud wrong fact. An unresolved SOURCE reference keeps
its row since v12 — `src_net_id` NULL, `src_hier_ref_id` set — because
dropping it made "driven through a name this export cannot resolve"
indistinguishable from "undriven", a wrong fact of the quieter kind. (In
v10's first cut even resolvable references produced only `hier_ref` rows,
so a target fed entirely from outside reported `constant` — the loud
kind.)

`src_net_id`/`tgt_net_id` repeat what the referenced rows already
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
* absolute paths into the exported tree: resolved;
* through one of the instance's own interface ports (`bus.vld`, modports
  included): resolved to the interface instance each occurrence is actually
  bound to;
* upward references and interface-array bindings: NULL. The one analysed
  body speaks for occurrences whose surroundings may differ, and a guess
  stored as fact is the one thing the columns must never hold;
* package items (`pkg::mask`): NULL always — a package is not an
  occurrence, so its variables have no net rows to resolve to. The path
  keeps the package prefix, so a reader knows where to look.

NULL `resolved_*` is "not resolved here", never a fabricated object — and
since v12 it is no longer silence either: a dependency whose source went
through an unresolved reference is still written, and surfaces in
`v_driver` as `'external'` with this row as its identity. Bare
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

`map_exact` is never a restatement of the two sides' own `exact` flags:
those describe each end's range, this describes the correspondence BETWEEN
the ends. `q = a + b` knows both ranges exactly and still cannot say which
bit of `a` reaches which bit of `q`, because a carry crosses them. 0 is not
doubt — the dependency is real — it is range granularity. NULL is "no
second end to correspond with" (a constant, an unconnected pin).

Columns describing an end that does not exist are NULL together: a tie-off
never reads as "the whole of nothing, exactly". One exception, new in v12:
an `'external'` dependency's source end exists — the reference names it —
so its window and `map_exact` survive beside a NULL `src_net_id`.

**Reading a dynamic index.** `out = mem[raddr]` is ONE dependency row —
`mem` is one net however wide — with `src_lo/src_hi` NULL and
`src_exact=0`: somewhere in `mem`, upper bound the whole of it. That is
the honest static answer, not a defect; the address is not lost either —
`raddr` is an ordinary operand with its own data dependency onto `out`
(an address selects like a mux select does). A tracer that treats
`exact=0` as "prune or widen consciously" keeps its cone honest: widening
through `mem` legitimately admits every writer of `mem`, and the
`exact=0` flag is the marker telling it that this is the step where
precision was lost.

## The stable query interface

Thirteen views. Their existence, column sets and order, column semantics,
NULL rules and row granularity are the contract; `verify-designdb.py`
asserts all of it on every export. Ground rules:

* A FACT view's row is one base-table row — `v_tree_node`, `v_net`,
  `v_term`, `v_term_map`, `v_net_conn`, `v_net_dep`,
  `v_stmt`, `v_stmt_target`, `v_stmt_operand` — and
  count(view) == count(base) is checked. Every internal join is against a
  primary key; nothing fans out.
* `v_driver`, `v_load` and `v_net_attachment` are COMPOSITE: UNION ALL
  branches discriminated by their kind column, each branch's row count
  reconcilable by a formula the verifier evaluates. The dependency, event,
  statement and terminal branches reconcile against base tables; the two
  crossing branches reconcile against `v_conn_arc`, which is the
  composition itself — so a fault inside that composition would inflate
  both sides equally and pass. That is the one seam these formulas do not
  check.
* `v_conn_arc` exists in the file but is NOT contract: it is the scaffolding
  the two composite views share, may change or vanish without a version
  bump, and consumers must not query it.
* Point queries seek. `v_driver`, `v_load` and `v_net_attachment` by
  `signal_net_id`/`net_id`, `v_net_dep` by `tgt_net_id`, `v_net_conn` by
  `outer_net_id`
  use an index, never a base-table scan — the closure is the consumer's,
  one point query per hop, so a scan per hop would be a scan per net in
  the cone. The verifier asserts the query plan itself; a change to how a
  view is computed that regresses this fails the export rather than
  shipping a database that answers slowly.
* Explicit column lists, never `SELECT *`; no transitive closure — a
  fan-in cone is the consumer's recursive query, one step per row here.

**`v_db_info`** — the meta seal as one row, counts CAST to INTEGER:
`schema_version, tool_version, slang_version, producer_revision, top,
analysis_status, error_count, unresolved_count, empty_procedure_count,
duplicate_path_count, config_digest`.

**`v_tree_node`** — one row per node: `node_id, parent_node_id, node_name,
node_kind, ordinal, inst_id, parent_inst_id, module_id, module_name,
param_signature, def_name, file_path, src_path, src_line,
src_col`. NULL by kind: `generate` has no subtype columns; `primitive`
has `inst_id` NULL, `parent_inst_id` its owning instance and
`def_name` the gate/UDP name; `unresolved` has `module_id` NULL and
`def_name` the unresolvable spelling; the root and generate levels
have no location.

**`v_net`** — one row per net: `net_id, inst_id, module_id, module_name,
param_signature, scope_node_id, net_name, decl_kind, data_type,
width, is_implicit, file_path, src_path, src_line, src_col`.
No direction column — direction belongs to terminals, and a net's port-ness
is one `v_term_map` join away.

**`v_term`** — one row per terminal: `term_id, inst_id,
module_id, module_name, term_name, term_kind, direction, data_type,
width, ordinal, is_const, modport, file_path, src_path, src_line,
src_col`.

**`v_term_map`** — one row per inside segment: `term_id,
term_inst_id, term_name, map_ordinal, inner_net_id,
inner_net_name, term_lo, term_hi, term_exact, inner_lo, inner_hi,
inner_exact, map_exact`.

**`v_net_conn`** — one row per outside segment: `conn_id,
outer_net_id, outer_inst_id, outer_net_name, term_id, term_inst_id,
term_name, direction, conn_kind, ordinal, outer_lo, outer_hi,
outer_exact, term_lo, term_hi, term_exact, map_exact,
outer_intf_inst_id, outer_hier_ref_id, file_path, src_path, src_line,
src_col`. Deliberately NOT composed with `term_map` — this view is
the fact, `v_driver`/`v_load` are the composition.

**`v_net_dep`** — one row per dependency: `dep_id,
src_net_id, src_inst_id, src_name, src_lo, src_hi,
src_exact, tgt_net_id, tgt_inst_id, tgt_name, tgt_lo,
tgt_hi, tgt_exact, stmt_id, assign_operand_id, stmt_target_id,
expr_ref_id, prim_id, src_hier_ref_id,
tgt_hier_ref_id, dep_kind, map_exact, file_path, src_path,
src_line, src_col`. Location is the statement's, or the
primitive's for a primitive arc. A row whose `src_inst_id` and
`tgt_inst_id` differ crossed by name; the `*_hier_ref_id` column on
that end names the reference it went through. Not deduplicated.

**`v_driver`** — every direct driving arc of `signal_net`, one row each:
`signal_net_id, signal_inst_id, signal_name, signal_lo, signal_hi,
signal_exact, driver_net_id, driver_inst_id, driver_name, driver_lo,
driver_hi, driver_exact, driver_kind, dep_id, conn_id,
stmt_id, prim_id, term_id, map_exact, file_path,
src_path, src_line, src_col`. `driver_kind`:

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
* `external` — the source is a reference this export has no net row for:
  a package variable, an upward name from a shared body. The signal IS
  driven; `driver_net_id` is NULL because the driver has no row, not
  because it does not exist, and the dependency's `src_hier_ref_id` names
  the reference — path, location, resolution NULL. Unlike a constant, the
  driver window survives: the referenced object's bits are real even when
  unnamed here. Before v12 these rows were dropped and such targets read
  as undriven.
* `alias` — an `alias` statement binds the two nets into one object.
  Both directions exist, so each is the other's driver and the other's
  load. The kind is what keeps it out of a multiple-driver count.
* `system_task` — a system task wrote the argument. `driver_net_id` is
  NULL, as for a constant, because the source is a file or a plusarg
  rather than a net; `stmt_id` names the call. Kept apart from
  `constant` so "is this tied off?" and "is this loaded at startup?" are
  different answers.
* `terminal` — the design boundary. A root instance's input/inout/ref
  terminal drives the net it stands for: `driver_net_id` is NULL (the
  world outside the export is the driver) and `term_id` names the pin.
  Without it, a top-level input's net reported no driver at all, and a
  consumer could not tell "nothing drives this" from "this is where the
  design ends" — two answers that mean opposite things.

An unconnected terminal contributes no row.

**`v_load`** — every recorded read of `signal_net`, one row each:
`signal_net_id, signal_inst_id, signal_name, signal_lo, signal_hi,
signal_exact, load_net_id, load_inst_id, load_name, load_lo, load_hi,
load_exact, load_kind, dep_id, conn_id, stmt_id,
proc_id, term_id, map_exact, file_path, src_path,
src_line, src_col`. `load_kind`: `dataflow` (a dependency reads
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

**`v_stmt`** — one row per statement: `stmt_id, inst_id,
module_id, module_name, scope_node_id, proc_id, ordinal, sequence,
stmt_kind, construct, assign_kind, delay, dropped_operand_count,
file_path, src_path, src_line, src_col`.

**`v_stmt_target` / `v_stmt_operand`** — one row per reference:
`target_id/operand_id, stmt_id, ordinal, net_id, net_name, tgt_lo/hi/
exact` (targets) / `operand_lo/hi/exact` (operands — no classic
abbreviation, so the word stays whole).

**`v_net_attachment`** — everything touching one net, one row per
attachment: `net_id, inst_id, net_name, attachment_kind, lo, hi, exact,
stmt_id, term_id, stmt_target_id, assign_operand_id, expr_ref_id,
proc_id, dep_id, hier_ref_id`. The structural adjacency the directional
views cannot ask flatly — "what hangs off this net" — with
`attachment_kind` naming the relation and exactly ONE of the seven typed
id columns pointing at that relation's own row (the exclusive-arc shape
`net_dep` uses, not one polymorphic id): `terminal_inside` /
`actual_outside` → `term_id`; `written_by` / `release_target` →
`stmt_target_id`; `read_by` → `assign_operand_id`; `condition` /
`statement_read` → `expr_ref_id`; `event` → `proc_id`; `dep_in` /
`dep_out` → `dep_id`; `named_from_outside` → `hier_ref_id`. `lo/hi/exact`
are this net's window in the attachment. Each branch is one base
selection, count-reconciled; a point query by `net_id` seeks on every
branch; and the verifier holds that exactly one typed id is non-null per
row and is the one `attachment_kind` implies.

`file_path` is the spelling as written in the filelist; `src_path` the
absolute path it resolved to. They answer different questions and neither
substitutes for the other.

## Naming rules

One classic abbreviation per word, never mixed with the full spelling —
the v12 dictionary in "What changed in v12" is the whole list, and table
and view surfaces share it (v11's `inst`-in-tables, `instance_id`-in-views
split is gone).

* Tables are `singular_snake_case`. `_id` appears exactly where a column
  holds another table's primary key, and nowhere else.
* A bit range is prefixed with the end it describes (`src_lo`,
  `term_exact`); a single-range table spells its own bare (`lo`/`hi`/
  `is_exact`).
* The two sides of a terminal are `outer_*` (what the parent wired — the
  actual; VPI's highConn) and `inner_*` (what the pin stands for inside;
  vpiLowConn). Direction words never name structure: an `inout` pin's
  outer net drives AND loads the inner one, so driver/load vocabulary
  belongs to `v_driver`/`v_load`, which derive it per port direction.
* Kinds, directions and roles are their words. The words are a wire format:
  they come from this schema, not from slang's enum printer, so a slang
  upgrade cannot change the vocabulary underneath a consumer. Enum values
  and meta keys are data, not identifiers: they stay full words.
* `ordinal` is position in a declaration or extraction list; `sequence` is
  execution order inside a procedure. Neither is an identity.

For a reader arriving with netlist instincts, the objects are the ones the
EDA standards already name:

| here | elsewhere |
|---|---|
| `net` | OpenAccess oaNet, VPI vpiNet |
| `term` on the root | OpenAccess oaTerm — the block-boundary port |
| `term` on a child | OpenAccess oaInstTerm — the instance pin |
| `net_conn` | VPI vpiHighConn — the pin's outside |
| `term_map` | VPI vpiLowConn — the pin's inside |
| `net_dep` | one hop of a fanin/fanout traversal |
| `hier_ref` | an XMR |

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

`src_file` holds every file slang actually read, absolute path and
SHA-256, so a consumer can tell the database and the RTL diverged instead of
answering from stale data. `file` holds the spellings rows carry — as
written in the filelist — joined to their src_file. `meta` is the seal;
its required keys are the `v_db_info` columns plus `tool`, except
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
  operands, three dependencies onto `y`, `map_exact=0`, and no
  fabricated `tmp` net. A consumer that needs the expression's shape reads
  the source at the location the row names.
* A system task that writes an argument — `$readmemh` into a memory,
  `$sscanf` or `$value$plusargs` into a variable, `$cast` into its
  destination — records a target and a source-less dependency, surfacing
  in `v_driver` as `system_task`. The signal is genuinely driven; the
  source is a file or a plusarg, which is outside anything this schema
  names. It is not a `constant`, and the kinds are separate so a consumer
  cannot mistake one for the other.
* `force` records as a blocking assignment marked `construct='force'`
  (procedural `assign`, `'proc_assign'`) — the hijack is findable with
  one WHERE, and its dataflow stays a blocking assignment's.
  `release`/`deassign` record as `stmt_kind='release'`, naming their
  lvalues and driving nothing. What is still absent is any judgement of
  which driver "wins" while a force is active — that is simulation, not
  structure.
* Variable initialisers (`logic [7:0] c = 0`) are not drivers; net
  initialisers (`wire w = a & b`) are, because the LRM says so.

## Known limits

* Upward hierarchical references keep `resolved_*` NULL — the one analysed
  body speaks for occurrences whose surroundings may differ, so
  `$root`-relative climbs from a shared body cannot be pinned per
  occurrence. Their dataflow is not silent (`driver_kind='external'` since
  v12), but the far object has no row: a trace ends at the reference text.
  Package variables no longer share this fate — v13 gives them net rows
  (see *Packages*) — but `$unit` compilation-unit items still do.
* Interface-array element bindings (`.b(arr[k])`) have no per-occurrence
  instance id; `outer_intf_inst_id` stays NULL and the member references
  through them stay text-only.
* Clocking blocks are not modelled: a clocking block declares no nets and
  gets no rows, and a `cb.sig` reference has no special handling. Virtual
  interfaces likewise — a `virtual interface` handle is a run-time value,
  and references through one degrade like any dynamic access. Both are
  testbench constructs; the synthesizable subset does not meet them.
* A non-ANSI concatenation formal (`.p({hi, lo})`) is exported as one
  terminal with its two inside segments, but no lint fixture covers it:
  Verilator rejects the construct, so it is verified by hand.
* Task and function bodies have no `proc` rows; their statements
  belong to the procedure that reached them, and a subroutine no procedure
  walks contributes nothing. A body called from N sites is N sets of rows,
  bounded by a per-module expansion budget — a pathological call DAG that
  exceeds it reports the skipped sites rather than exhausting memory, and
  `meta` says the export is `partial`.
* A subroutine's formals are one net per subroutine, not one per call site.
  Each call's gating and delay are its own, but the formal is shared, so
  the transitive cone through a task called twice admits combinations no
  single call makes (`g1` with the second call's argument). Per-call-site
  formals would fix it and are not in v12.
* A function call contributes both a summary arc (each argument to the
  call's target) and the detail arcs through its formals, so a fan-out
  count over both double-counts that read. The detail path also stops at
  the function's return net, which has no arc onward to the target.
* A macro-assembled reference spans two buffers and cannot be recovered as
  one span; it is counted (`meta` external tally), not stored.
* Statements slang marks bad take their enclosing block out of the walk;
  `empty_procedure_count` says how often, and the diagnostics say why.

## Interfaces

An interface is not a special case in this schema — that is the point.
An interface instance goes through the same template/stamp pipeline as a
module: a `tree_node` plus an `inst` (its `module` row says
`def_kind='interface'`), its variables are `net` rows, its procedures and
statements their ordinary rows. What differs is the boundary:

* On the USING module, an interface port is a `term` with
  `term_kind='interface'`, `direction` NULL (it has none) and `modport`
  when one is named. It has no `term_map` rows — no nets stand behind it —
  and its binding is a `net_conn` with `conn_kind='interface'` naming the
  bound interface `inst`, resolved through pass-through chains.
* No dataflow arc crosses the binding itself. Dataflow happens through
  MEMBER references: `bus.vld` resolves — modports included, past the
  modport symbol to the net behind it — to the interface instance this
  occurrence is actually bound to, and the dependency is an ordinary
  `net_dep` whose far end is that interface's net. Two modules on one
  interface meet at those nets, which is where a trace crosses.

So "what drives `axi_if.master`'s `vld`" is `v_driver` on the interface
instance's own net — the interface occurrence is a first-class place, not
a bundle of wires spliced into its users.

## Packages

A package is a *pseudo-occurrence*: not part of the elaborated tree — it
has no parent and instantiates nothing — but given the same object rows as
one, so its state is a place a trace can reach. It is a `tree_node` with
`node_kind='package'` (`parent_node_id` NULL, above the roots), a matching
`inst` with `parent_inst_id` NULL and a `module` of `def_kind='package'`,
and each package variable is a `net` under it.

That is all it takes for a `pkg::mask` reference to resolve. slang settles
the `::` at compile time, so such a reference is a plain read of the
package variable's symbol; the exporter records it as a `hier_ref` whose
`path` keeps the `pkg::` prefix (`cfg_pkg::mask`) and whose
`resolved_net_id` is the package net. The dependency across it is an
ordinary `net_dep` — `driver_kind='data'`, not `'external'` — and an
imported bare `mask` resolves the same way, because the symbol still knows
its package. Two modules reading one package variable meet on its net,
exactly as two modules on one interface meet on the interface's net.

Only package *variables* become nets. A package of nothing but
`localparam`, `typedef` and functions is a node with no nets — real, and
correctly empty. `$unit` compilation-unit items are not stamped yet and
stay `external`. A package variable's initializer is not a driver (the LRM
rule for variables), so the exporter walks no package-internal dataflow;
its drivers and loads are the modules that reference it.

## Tracing across inout, tran and alias

Bidirectional structure is stored as what it is — arcs both ways — and
three habits keep a walk over it honest:

* **Recursion terminates by dedup, not by direction.** A recursive CTE
  with `UNION` (never `UNION ALL`) carries a visited set for free; a
  `tran a b` (a↔b, two rows) or an aliased pair then costs one revisit
  that dedups away, not a loop.
* **Pairs share provenance.** The two arcs of one switch share `prim_id`;
  the two directions of one inout crossing share `conn_id`; an alias
  pair shares `stmt_id`. Treat rows with one provenance id as one
  undirected edge when the question is "what is electrically one node",
  and as two directed arcs when it is "which way may data flow".
* **Kinds keep counts honest.** `alias` is excluded from multiple-driver
  counts by its kind; an inout bus with N modules on it genuinely has N
  potential drivers, and the schema reports exactly that — the resolution
  of who wins is simulation, not structure.

## Reading it honestly

The database is a build artifact: rebuilt from source, sealed by digest,
version-gated. Read `v_db_info` first; refuse a version you do not
know. Treat `exact=0` ranges as upper bounds, `map_exact=0` as "follow
at range granularity", NULL resolved columns as "resolve it yourself or do
not" — every one of those is the exporter telling the truth about what it
could not narrow, and the verifier exists to keep it that way.
