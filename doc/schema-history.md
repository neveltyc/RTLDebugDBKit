# design.db — schema history

`db_info.schema_version` is the database's consumption contract: the view set,
each view's columns and their order, every column's meaning, value domain and
NULL rules, each view's row granularity, the tables the field reference names,
and the required `db_info` columns. Any change to that contract bumps the
integer; changing only how a view is computed, with its contract intact, does
not. A reader that does not recognise the number must refuse the file rather
than read it as though the layout held, and no database is upgraded in place --
a bump means re-exporting the RTL.

The current contract is [`designdb-schema.md`](designdb-schema.md); the
constant is `SchemaVersion` in [`../src/DesignDb.h`](../src/DesignDb.h). This
file records what each bump changed, newest first. Versions before v10 describe
the folded model -- rows hung off a module rather than an instance -- whose
tables (`edge`, `assignment`, `symbol`, `port`, `child`) v10 replaced with the
instance-level object model the field reference documents.

## v20

v20 removes what the schema was maintaining by checking rather than by
construction. `hier_ref.resolved_inst_id` is the first of them: it held
`net.inst_id` of the net beside it -- an equality the verifier has
asserted since v10 -- so the pair could disagree, and twice did, once
where a route reached an occurrence of a different module and left the
instance named with no net, once where it named a generate level that has
no `inst` row at all. A resolved reference now names a net, and the net
names the instance. `v_hier_ref` publishes `resolved_inst_id` unchanged,
off the net row it was already joining for `resolved_net_name`, so a
consumer reading the view sees nothing move.

The seal is the second: `meta(key, value)` becomes `db_info`, one STRICT
row of typed columns. Sixteen facts that are neither optional nor
open-ended were stored as text pairs, so presence was the verifier's to
check, the counts were TEXT a view had to CAST, and a mistyped key would
have become a seventeenth fact rather than an error. Now every column is
NOT NULL but `top` (NULL when the design elaborated none), a count is an
integer, and the rule that `analysis_status` agrees with the counts
beside it is a CHECK rather than prose in the exporter and an assertion
in the verifier. `v_db_info` keeps its sixteen columns in their order, so
again a consumer reading the view sees nothing move; one reading `meta`
reads `db_info` instead, by column rather than by key.

The third is the gating. A branch condition was filed as a read of every
statement it gated, which is three to six times the rows on real designs
-- VeeRwolf 10 849 control reads for 3 563 distinct ones, one pair
repeated eighty times -- and could not record the case where there is no
statement at all: `if (gate) ; else ;` produced no row of any kind, so a
signal the design reads appeared nowhere in the database. A condition is
written once and evaluated once, so it belongs to its level:
`branch_ref` is that read set, `expr_ref` keeps the reads that really are
a statement's and loses the `control` role and its `branch_id`, and a
gated statement gets the DEPENDENCY, one per target, naming the level in
`net_dep.branch_id`. An outward condition follows the same rule: a
`hier_ref` keyed on the level, `stmt_id` NULL, published on `v_hier_ref`
beside a new `branch_id`. `v_load` gains `condition` for a level that
reads a net and gates nothing this instance names -- `statement` was the
old spelling and it needed a statement.

`branch_ancestor` is what keeps that affordable. Everything gating one
statement was a single indexed lookup and would have become a walk up
`parent_branch_id`: 62.8 us per statement as a recursive CTE against
v19's 3.50 us, and 2 361 us through a view, since SQLite materialises
every chain in the design before it filters. The closure is computed once
per module template, where a tree is a few dozen rows, and shifted per
occurrence like every other id; the query is back to 3.83 us, and both
directions are indexed because both are asked.

## v19

v19 gives the gating a shape. v18 recorded WHICH nets gate a statement
and nothing else, so the two arms of one `if` were indistinguishable
row for row -- same conditions, same `expr_ref` rows, same everything --
and a case statement pushed its selector and EVERY item's labels onto
one gate before walking any body, so each arm saw all of them and the
labels themselves, being constants, were filtered away entirely. None of
that was recoverable from the rows.

`branch` is the gating context as a tree, one row per level, shared by
every statement under it and chained outward by `parent_branch_id` --
the shape `call_site` already uses for nested calls. `stmt.branch_id`
names the level a statement sits in, and `expr_ref.branch_id` (with
`hier_ref.branch_id` for an outward condition) says which level each
control read came from, so a read is attributable to the condition that
made it. An `if` is one level per arm carrying `sense`; a `case` is a
point level carrying the selector's reads and the matching semantics,
with one child level per arm carrying that item's labels -- `branch_label`
holds their evaluated values, normalised as `inst_param.value` is.

Arms are siblings, which changes what a `case (1'b1)` one-hot idiom
records: each arm reads only its own label expression, where v18 gave
every arm every arm's labels. picorv32's decoder read `instr_trap` on 207
control rows and reads it on 36 here, one per statement of the three arms
whose label names it. The priority a plain `case` carries -- arm k is
reached only if arms 0..k-1 did not match -- is `branch.ordinal`, the
written order under the point, rather than a read repeated onto every
later arm; source line cannot carry it, since a whole case may be written
on one. Every written arm gets a row, empty body or not, so the arms
under a point are the whole case; an arm whose body gates nothing has no
statement to carry a read, so its labels' reads go on the point.

`branch.static_taken` publishes the compile-time verdict rather than
acting on it. `localparam bit EN = 0; if (EN) q = b; else q = c;` used
to export `b` as an unconditional driver of q -- EN is a constant symbol,
so it was filtered out of the gating and the dead arm carried no
condition at all, leaving q with two drivers and no way to tell which
one the elaboration had already decided against. The rows stay, because
the statement is in the elaborated design and a source view shows it
where a pruned database would not; what changes is that the database
says which arm cannot be reached. A loop whose stop condition rejects on
its first test carries the same 0.

A loop index is no longer a `data` or `control` dependency source, and
never an operand. `for (int i = 0; i < 2; i++) hi[i] = a[i+2];` recorded
`i -> hi` as data three times -- in picorv32's multiplier, twelve of one
statement's eighteen dependency rows were the two indices -- for a
variable that takes every value of the iteration space and answers no
debugging question. Where the RTL reads it as a VALUE the read stays:
`t(i)` is a `procedure` dependency onto the formal, and suppressing that
would leave the formal with no driver at all. An index is a variable the
header STEPS, not one it merely initialises -- `for (acc = 0; kk < 4; kk
= kk + 1)` would otherwise lose acc's rows outright.

What the loop does with the index is on its branch level: `iter_net_id`
and the iteration space beside it, so a whole-signal `din -> rev` out of
`for (j…) rev[j] = din[7-j];` reads back as the per-iteration mapping it
is. `iter_step` is negative where the loop counts down, which `foreach`
over a packed dimension does. The index arithmetic is not stored, for the
same reason no expression shape is: it is in the source at the location
the row names.

`foreach` and `forever` gained handlers at the same time -- neither had
one, so a `foreach` index leaked exactly as a `for`'s did and both bodies
carried no level at all. A statement-level event control inside a branch
records its gating like every other statement kind; `if (en) @(posedge
clk);` recorded nothing of en.

## v18

v18 drops two columns nothing can fill and widens one NULL rule.

`term.is_const` marked a `const ref` port. There is no such thing:
`const` is rejected on a port declaration, so the column could only ever
hold 0 or NULL and never the fact it existed to carry. `proc.name` held
a task or function name, and v17 removed the rows that had one. Both are
gone rather than kept as columns a consumer must ask about and never
learn from.

`net.width` and `term.width` were NULL for every non-integral type,
which left an unpacked object's ranges unmeasurable: bit offsets index
the FLATTENED space, so `[4:11]` of a four-element byte array is a
window into 32 bits that the row would not name. Both now carry the
flattened width -- the space the offsets are in -- and are NULL only for
a type with no bits at all.

v18 also gives `term_map` a surrogate `id`, as every other child table
has, and points `v_net_attachment`'s two wiring kinds at the row they
describe: `terminal_inside` at `term_map_id`, `actual_outside` at
`conn_id`, in place of the `term_id` both carried. A terminal is not
the attachment -- `.q({2{r}})` gives one pin two connection segments,
and both rows said `term_id=48` with nothing to tell them apart, though
the view's contract is that its typed id names the relation's own row.
`v_term_map` publishes the new id so the join stays inside the view set.

Three columns join the view surface. `v_driver` and `v_load` gain
`signal_ref` beside the `driver_ref`/`load_ref` they already had: either
end of an arc can be the hierarchically named one -- `assign q = u.x`
names the driver, `always_comb u.x = a` names the signal -- and with
only one spelling published the second was reachable in three joins from
the side that asks who drives `x`. `v_tree_node` gains `def_kind`, so an
interface instance and a module instance, both `node_kind='instance'`,
are told apart without joining a table the view set does not cover.
`v_db_info` gains `tool`, which was a required meta key with no column;
the required set is now exactly the view's columns less `top`.

Two views join the set. `v_node_path` publishes the path assembly the
tree deliberately does not store: a segment per node is the right
storage, but every consumer re-derived the join and three plausible
spellings of it are wrong -- anchoring on a null parent sweeps in
packages, and prefixing a net's scope path counts generate levels
twice. `v_proc_event` publishes the events themselves, so "which flops
does this net clock, on which edge" stops needing the base tables:
edge_kind and the procedure's kind arrive on one row.

`v_net_attachment`'s `event` kind moves from `proc_id` to
`proc_event_id` with them, for the reason the two wiring kinds moved:
one procedure takes several events on one net, and the procedure is not
the row.

One spelling for one thing, last: a view whose row has a single bit
range spells it bare, as its base table does. `v_stmt_target`'s
`tgt_*`, `v_stmt_operand`'s `operand_*`, `v_hier_ref`'s `ref_*` and
`v_net_attachment`'s `exact` were four spellings of one concept, none
of them the base tables'. All four are now `lo`/`hi`/`is_exact`; the
prefixed form stays where a row really has two ends, as `v_net_dep`
does.

Two statements that produced no rows at all now do. `-> ev` is what
makes an event happen, and without it an event variable had waiters and
no cause -- a trace back said nothing in the design touches it.
`stmt_kind` gains `trigger`, the target names the event, and the arc is
source-less like a system task's, surfacing as `driver_kind='trigger'`:
nothing FEEDS an event, so `constant` would have said it is tied off.
`disable` gains a kind for a different reason -- it names a block, not
a net -- but the same one underneath: the condition gating it was a
read with no statement to belong to, and vanished with the statement.

A constant bound to a subroutine argument now ties the formal off.
`t(8'h5A, y)` left the formal with no driver at all while the identical
tie written as a port connection recorded `conn_kind='constant'`, so
the same fact answered differently depending on how it was spelled. It
is a source-less `data` arc, which is what `constant` means, anchored
by a target row on the calling statement -- the discipline every other
source-less arc already follows.

A system task called inside a CONDITION writes its argument like any
other, and recorded nothing: v_driver tells a system write from a
tie-off by the statement it came from, and a call in a condition
belongs to none, so the write had no attribution to be given. It now
gets a `stmt` row of its own -- what the procedure-header
`event_control` row already is for reads with nowhere to belong. Only
the write travels with it; what the condition reads is gating already.

An interface ARRAY as a module's own port binds one element per segment
of its terminal, where it used to be a single `net_conn` row naming no
instance -- so every member reference through it stayed text-only and
the interface nets behind it read as undriven. It is the shape a
concatenated actual already has on an ordinary port: one terminal, a
row per piece, in declaration order. A reference through such a port
carries the segment it meant, decided by whose subtree the target sits
in rather than by the written index, since `bus_arr[k]` in a generate
loop spells one thing and lands on a different element each iteration.
A multi-dimensional port binds its LEAVES, since an element of the
outer array is another array and not an instance.

A built-in method's effect on its receiver stays unmodelled, and the
doc says so where the other testbench constructs are declined.

`meta.checker_inst_count` joins the required set, and `v_db_info` the
column for it. A checker instantiation produces no rows at all -- its
symbol is not an InstanceSymbol, so the walk never reaches its ports,
assertions or scope -- and nothing said so, leaving a design that has
checkers indistinguishable from one that has none. It does not make
the export `partial`: a construct this tool declines is not a walk that
fell short, which is the same reason an unresolved instantiation does
not.

## v17

v17 narrows two published value domains to what a producer can write.
Nothing that was exported stops being exported and no row changes; the
documented value domain does -- an exhaustive consumer loses a branch --
and the domain is contract. (Its CHECK spelling in the shipped file is
not: the default export strips the clauses entirely.)

`module.def_kind` loses `checker`. slang's DefinitionKind is Module,
Interface and Program -- a checker is a symbol kind, not a definition kind
-- so no input reaches that value and an exhaustive consumer had a branch
nothing could take. `proc.proc_kind` loses `task` and `function` for the
same reason and one more: the doc has said since v2 that task and function
bodies get no procedure row, their statements belonging to the calling
procedure, so the constraint contradicted the contract beside it.

## v16

v16 is what a review of the five branches that became v15 found and did not
stop to fix, plus two defects older than any of them. Four values change and
the interface grows; the rule is v15's, so it moves for either.

A GATE took its tree node name from the symbol's own `name` rather than
through leafSegment, and so took neither of the two things that function
exists for. An instance array's element carries the bare array name there,
so `buf p [1:0]` gave one scope two nodes called `p`, and once v15 began
synthesising a segment for a nameless instantiation, two called `$buf$n` --
either way `top.p[1]`, which is what the source wrote, was unreachable by
(parent_node_id, name). And an escaped name arrived unescaped, so
`buf \g.1 ()` wrote a node name holding a dot: two path segments where a
node is one, which the verifier's own universal check already forbade.

A call whose formal is no net of the calling body -- a subroutine declared
in a package, an interface or $unit -- now drives its output actual. It
recorded only the target row, so v_stmt_target and v_net_attachment said the
statement wrote the net and v_driver said nothing did: the same two-views-
disagreeing defect the v14 note records fixing for aliases. The row v_driver
gets is the one it has documented since v14 and never received --
`driver_kind='procedure'` with a NULL driver net, because at a call site the
formal is a symbol rather than an expression and cannot honestly be named as
a source. A call written inside a CONDITION gets the same dependency with no
statement and no target row either, a target being a position within a
statement; before this, such a write was not recorded at all.

`v_load.call_site_id` stops being NULL on the three arms that read a base
table directly -- proc_event, expr_ref, assign_operand. v15 contracted the
column as the tag the other views carry, and the recipe it exists for reads
a NULL as "belongs to no call", so one call's sensitivity, wait or statement
read was admitted into every call's cone: the mixing the tag was added to
prevent, on the arms a walk falls back to.

`meta` and `v_db_info` gain `recursion_count`, `truncated_call_count` and
`unanalysed_inst_count`. A recursive hierarchy is stamped one level deep and
the rest of the tree is simply absent, which `analysis_status` does not say
-- `hierarchy_only` says there is no dataflow, not that the tree is a
PREFIX -- so a cut database and a whole one of the same design read alike.
The other two choose `partial` while every published count is zero, which
tells a consumer the export is incomplete and gives them nothing to look at.
Since v5 the status has had to agree with the counts beside it; these are
the counts that were missing from beside it, and the agreement now runs both
ways.

`written_by` keeps its name and widens: an assignment, a system task, or a
call writing its output actual. Nothing that was `written_by` stops being
it; a v15 reader that enumerated the causes rather than the value is the one
this concerns.

The OUTPUT actual of a call inside a control expression stops being a
`control` source of what that expression gates. `control` has always meant
"reaches the target through a branch condition", and `if (chk(a, y))` never
read y -- the call writes it. No column, view or granularity moves, so the
version does not: the rows that violated the documented meaning are gone,
along with the `expr_ref` per gated statement that claimed the same read.
The write is untouched. `inout` and `ref` actuals are read as well as
written and still gate, as do a written actual's selectors.

## v15

v15 corrects two values and adds to the query interface, and it moves the
version for both. Correcting a value is a contract change to whoever read
the old one, which is v14's reason; the rule at the top of
doc/designdb-schema.md moved for the additions. One used to be exempt, on
the reasoning that an older reader would merely not query it. That
reasoning omits the reader who WANTS the new column, for whom the version
integer is the only capability signal there is -- leaving it still would
force exactly the out-of-contract probing (PRAGMA table_info, or
query-and-catch) that the additions here exist to retire. One rule now:
the contract is the view set, each view's columns and their order, their
semantics, NULL rules and row granularity, and any change to it bumps.

The first: an instantiation written without an instance name no longer
answers to the name of the instance holding it. slang leaves such a
symbol's name empty and a hierarchical path built from an empty name ends
at the PARENT, so the last segment WAS the parent's -- the defect v14
fixed for anonymous gates and left standing for instantiations, because
inventing a name for one was a separate decision. It is decided here: the
synthesised `$def$n` segment now covers module instantiations and
unresolved definitions as well, and all three kinds draw from one counter
per scope, so a gate and an instantiation beside it cannot be handed one
name twice.

A module instance name is mandatory, which makes this look like a case
that cannot arise. It arises whenever a macro supplies the name and does
not expand: veerwolf compiled without `TEC_RV_ICG has 302 such nodes, 32
instances and 270 black boxes, every one of them carrying its parent's
name. Two in one scope collided outright -- duplicate_path_count counted
a collision the design does not have, and (parent_node_id, name) answered
with two nodes. One alone was quieter and no better: `free_cg` under
`free_cg` is one name twice, one level apart, and nothing in the source
spells the second. `tree_node.name` is the only column that changes; the
row counts of every table are what they were.

The second is a field pair: `hier_ref.resolved_inst_id` and
`resolved_net_id` are no longer NULL for a path anchored at `$root`, and
a v14 consumer read that NULL as "this reference cannot be resolved per
occurrence" -- a statement about the reference rather than about the
exporter.

An absolute path names one object, seen from any occurrence of the body
that spells it, which is exactly the property that makes replay sound;
the resolution machinery for it existed on both ends and was cut off in
the middle. slang's HierarchicalReference::isUpward() is
`upwardCount > 0 || path[0] is Root`, and the exporter tested it whole,
so `$root.a.b.c` was dropped alongside the upward names it has nothing in
common with. Both ends of a dependency through such a reference move
with it: a `$root` READ was a `net_dep` with no source net that
`v_driver` reported as `external`, and a `$root` WRITE could not be
materialised at all -- the target net had no driver row of any kind, so a
trace back from it said the design never wrote it. Both now carry the
resolved net, like any downward reference.

Upward references are unchanged and still NULL, for the reason they
always were: one analysed body cannot answer for surroundings that differ
per occurrence.

Riding along, neither of them a contract change. `analysis_status` no
longer tests slang's analysed-scope count, which could not be zero unless
the compilation was fatally errored -- the branch beside it -- and
`hierarchy_only` therefore has one cause rather than the two it named. It
gains a `partial` disjunct in its place, for an occurrence stamped from a
module body the analysis never reached; that is a guard against slang's
descent and the template walk drifting apart, unreachable while they
agree. The only fixture that reaches it is the recursive hierarchy, which
slang rejects outright -- so `fatal` has already made it `hierarchy_only`
and the disjunct chooses nothing there either. And the concatenation
cursor walk in Ref.h gained the wider-than-remaining guard its twin in
StatementWalker.h already had, so the two agree about when an operand
walk stops meaning anything.

The other half of that agreement is a value change, so it is not riding
along. An operand of NO width used to stop both walks; it is not a stop
in either now. `{0{x}}` is legal, slang keeps it among the operands with
the void type, and a parameterised pad degenerates to it -- `{a, {PAD{x}},
b}` at PAD = 0 -- so this is reached from ordinary RTL rather than from a
malformed expression. It occupies no bits of the result, so the cursor
does not move and the operands beside it keep their windows: `a` and `b`
land on the same bits they would with the pad written out of the source,
where before the whole concatenation degraded to one unpositioned
reference per operand. And `x`, which the pad names but the
concatenation does not read, no longer gets a `net_dep` at all -- an edge
v14 recorded for a signal that reaches nothing.

The additions change no extraction and move no value; three of them are
answers a consumer could not get from the views alone, all three reported
from a real consumer.

`driver_ref`/`load_ref` name the far end of an arc as it was SPELLED, when
it was reached by a hierarchical name. A `driver_kind='external'` row has no
driver net by definition, so before this it named nothing at all: the path
lived on a hier_ref reachable only through dep_id and a base table, one
extra query per row on the one path where the row was least self-sufficient.
A crossing whose tie resolved (`.p(u.g[7:4])`) carries it too, beside the
net -- where the bits live and what the parent wrote are different answers.

`v_hier_ref` is the fifteenth view, and closes a hole older than the
request: the contract published four foreign keys into hier_ref
(v_net_dep's two, v_net_conn's, v_net_attachment's) and no view to resolve
one against. A published key with nothing to follow is a dangling reference
in the contract itself.

`v_stmt_target.target_kind` is v_net_attachment's three-way distinction --
written_by, release_target, alias_binding -- stated from the statement side.
stmt_target holds lvalues, and a `release`/`deassign` or an `alias` names
one while driving nothing; without the column the only path that reaches a
release was also the only one that could report it as a driver. That
confusion has now been made twice, once in this repository (fa9545b).

`call_site_id` reaches the statement layer: v_stmt, v_stmt_target and
v_stmt_operand all carry it now. It was on the base table since v13 and
exposed on v_net_dep, v_driver and v_load, which left the documented
context-sensitive recipe unexecutable from the statement side -- the side a
walk falls back to when a dependency did not survive its sources -- and left
"which call does this statement in a shared body belong to", a lookup rather
than a trace, with no answer at all.

## v14

v14 is a correctness batch, and it moves the version because correcting a
value is a contract change to anyone who read the old one. A branch-
condition audit of the extractor found 26 defects; what follows is what a
v13 consumer can no longer assume.

`driver_kind` keeps its vocabulary but loses a NULL rule: `primitive` and
`procedure` may now name no driver net. v_driver's ELSE was claiming
`constant` for any source-less row, against its own contract ("kind carried
through; a DATA row with no source is 'constant'"), so a `pullup` -- which
has no input terminal -- read as a tie-off and inflated every
multiple-driver count with a conflict that was not one.

`attachment_kind` gains `alias_binding`, for the reason `release_target`
exists: the storage is a stmt_target row and the statement writes nothing.
v_driver already excluded an alias by kind, so the two views answered "who
writes this net" differently, and v_net_attachment named nets no assignment
in the design touches.

`conn_kind` stops saying `unconnected` about a pin the parent wired. A
black box's connections arrive as AssertionExpr because the construct may
be a sequence; only the simple kind was unwrapped, so `.p(a ##1 b)`
recorded absence. Its leaves are `expression_operand` rows now.

Values that were simply wrong, and change: `col` was 0 on every row whose
location came from a macro (208 of veerwolf's 11,087 statements) because
slang's getColumnNumber takes a file location and was handed an expansion;
`stmt.scope_node_id` pointed at the instance for a net initialiser or an
alias inside a generate block, contradicting the `net` row for the same
declaration; `stmt_target.ordinal` was a template index rather than a
position within its statement; `tree_node.name` split an escaped identifier
on the dot inside it; and `map_exact` claimed a one-to-one map across
unequal widths at a port carrying a select, while claiming none at all for
an output argument that was whole-to-whole.

Rows that were missing and now exist -- additive, but they change what a
query returns: the self-read of a compound assignment (`a += b` reads a,
and `d <<= 2` had reported a CONSTANT driver on a signal fed by itself),
the body of a function written with `return`, the conditions of `do while`
and `case matches`, the outside of a MultiPort or unnamed-port connection,
and every outward reference whose path had no dot -- a $unit name, or one
built by a macro -- which had been dropped and read as `constant`.

One export that used to fail now succeeds: two unnamed ports in one module
collided on a synthesized name and aborted on a UNIQUE constraint.

## v13

v13 sharpens what a v12 reader could not ask precisely.
`v_net_attachment`'s one polymorphic `other_id` becomes seven typed
nullable ids (exactly one non-null per row, the one attachment_kind
names) -- the exclusive-arc shape net_dep already uses, so a consumer
joins the right base table without decoding the kind.

Packages become first-class objects. A package is a pseudo-occurrence --
a tree_node/inst under node_kind/def_kind 'package', above the roots
(parent_inst_id NULL), its variables `net` rows -- so a `pkg::x`
reference resolves to a real net (driver_kind='data') instead of
dead-ending at 'external', and two modules reading one package variable
meet on it. 'external' now means only the genuinely unresolvable: an
upward hierarchical reference from a shared body, an interface-array
binding. ($unit compilation-unit items are not stamped yet.)

And subroutine dataflow gets call-site identity. A task or function body
is walked once per call site, but the formal is one shared net, so a
fan-in cone mixed one call's gating with another's argument -- a
combination no real call makes. Every stmt and net_dep a body walk
produces now names its site in call_site_id, a new call_site table
(caller_stmt, subroutine, depth, parent_call_site_id for the call
string), exposed on v_net_dep/v_driver/v_load and v_call_site. A
consumer follows only one call's rows at each hop and keeps each call's
real combination. Additive: the column is nullable and the table new, so
a reader that ignores both is unaffected.

## v12

v12 is mostly a naming pass, and moves the version because renames are
contract changes. One classic abbreviation per word, every identifier,
no more tables-say-`inst` / views-say-`instance` split: `src_file`,
`prim`, `proc`, and the view family `v_db_info`/`v_term`/`v_term_map`/
`v_net_conn`/`v_net_dep`/`v_stmt*`. The two sides of a terminal stop
sharing column names -- `net_conn` wears `outer_*` (the actual, VPI's
highConn), `term_map` wears `inner_*` (vpiLowConn). `assign_target`
becomes `stmt_target`: it holds the lvalue of a release and a system
task's write as well as an assignment's, so it was never assignment-
only (`assign_operand`, which is, keeps its name).

Riding along, because the version was already moving: an unresolved
SOURCE reference is written as a `net_dep` (NULL source net, the
reference on the source end) and surfaces as `driver_kind='external'`,
so a target fed only from a package variable or an upward name is no
longer silently undriven; `force`/`release` are recorded (construct
`force`/`proc_assign`, and `stmt_kind='release'`) instead of a force
masquerading as a plain blocking assignment; an external tie carries a
`map_exact`, so its crossing traces bit by bit; and `prim_kind='switch'`
covers the LRM's whole switch family, not just what slang labels
bidirectional. Additive, so no reader must change: `inst_param` makes
the parameter signature queryable, and `v_net_attachment` is a
thirteenth view -- one row per relation touching a net.

## v11

v11 records the alias statement. `alias a = b;` binds two nets into one
object, and v10 exported nothing at all for it: the two halves were
simply disconnected, so asking what drove one answered "nothing" and
asking what read the other left out every reader of the first. It is
its own kind rather than a pair of continuous assignments, because it
is not one -- an alias has no direction and no driver, and counting it
among the assignments would have made every multiple-driver query wrong
in a new way. `stmt.stmt_kind`, `net_dep.dep_kind` and the
driver/load kinds each gain `alias`; that is a value-domain change, so
the version moves even though no column does.

## v10

v10 unfolds the model. Rows hang off the elaborated instance occurrence,
not the module variant: `tree_node`/`inst`/`primitive` subtype the
hierarchy under one id space, `net`/`term`/`term_map`/`net_conn` replace
`symbol`/`port` with the boundary's two sides in two relations,
`procedure`/`stmt`/`assign_target`/`assign_operand`/`expr_ref` replace
`assignment`/`assign_operand`/`stmt_read` with real statement objects, and
`net_dep` replaces `edge` -- per statement occurrence, no cross-statement
dedup, every dependency naming the operand, target, expression reference,
primitive or call it came from. `module` returns to being the source
definition; the parameter values a body elaborated with live on each
`inst.param_signature`. The `name` intern table is gone (names are
TEXT on their object rows; only `data_type` still interns), the 0-5 and
0-3 integer codes are gone (kinds and directions are their words), and
every relation is by object id, never by (module, name) string pairing.
Nine views become twelve; v_driver/v_load now compose the hierarchy
crossing (net_conn against term_map) that v9 left to the consumer.

A v9 database cannot be upgraded in place: the fold shared one row set
across occurrences and the edge dedup erased statement provenance, so the
per-occurrence identity v10 stores was never in the file. v10 databases
are produced only by re-exporting the RTL.

## v9

v9 carried the bit-precision model across the instance boundary. `port`
gained `port_lo`/`port_hi`/`port_exact` -- the bits of the FORMAL a
connection element occupies -- and `map_exact`, edge.map_exact's boundary
twin. Before it, `.q({hi, lo})` exported two rows that both claimed all of
q with outer_exact=1: the per-bit precision v7 built for assignments ended
at every port, and `.q({2{r}})` was worse -- dedup keyed only on the outer
side folded the two copies into one row, deleting the second window
entirely. The window is part of the dedup key now, so a replication is as
many segments as it has copies.

v9 also finished the interface's object model, seven views to nine:
v_load became every recorded read of a signal (dataflow / sensitivity /
wait / statement, discriminated by load_kind -- the flop clock pins of a
netlist database are loads of the clock net, and so are ours), and the
statement layer gained v_stmt and v_stmt_operand, the half of
the edge/assignment dual projection that had no interface.

## v8

v8 added the stable query interface: seven views (v_db_info,
v_tree_node, v_signal, v_port_connection, v_dependency, v_driver, v_load)
that resolve the intern tables and spell out the NULL conventions, so an
ordinary consumer never joins `name`/`type`/`file` or decodes `conn_kind`
itself. The version moves because a v8 consumer may rely on the views
existing with exactly their contracted columns -- a v7 file answers those
queries with "no such table". From here on: removing or renaming a view or
a view column, changing a column's semantics or NULL rules, or changing a
view's row granularity all bump the version; changing only how a view is
computed, with the contract intact, does not.

v8 also gave `port` a `child_id` foreign key, surfaced by both hierarchy
views, because without it the composition the views promise did not hold:
the tree spells an instance one segment at a time (`u_dec`) and the folded
port rows spell it whole (`g_rep[0].u_dec`), so joining the two by name
returned nothing exactly at generate boundaries -- and (module, name) is no
substitute key, since two unnamed gates in one module legally share a name.

## v7

v7 stopped crossing the two sides of an assignment. Every target used to be
paired with every operand, so `{a, b} = {x, y}` exported four edges where the
RTL has two -- and `y -> a` and `x -> b`, which are not dataflow at all,
carried src_exact=1 and dst_exact=1 like the two real ones. Both sides are now
walked as positioned slots and paired only where their bits meet, which also
narrows the target: `q[7:0] = {hi, lo}` records `hi -> q[7:4]` rather than `hi
-> q`. `assign_operand` was crossed the same way and is fixed with it.

`edge.map_exact` is the new column, and it is not a restatement of
src_exact/dst_exact: those describe each end's own range, while this describes
the correspondence between the ends. `q = a + b` knows both ranges exactly and
still cannot say which bit reaches which, because a carry crosses them.

## v6

v6 gave the relations that were being guessed from file and line a name.
`assignment`, `hier_ref` and `stmt_read` gained `stmt`, a per-module
statement ordinal, because a statement writing an *outward* target has no
assignment row to gather its parts: `assign top.a = x; assign top.b = y;`
exported two writes and two reads that could be paired four ways, of which
the RTL supports one. `child` gained `id` and `kind`, and `instance` gained
`child`: `def_module IS NULL` meant "primitive" and "unresolved black box"
indistinguishably though a trace stopping at each means something different,
the two hierarchy tables were related by nothing a consumer could join on,
and an unresolved instantiation had no `instance` row at all -- so a path
through a black box did not resolve, and the tree reported the same emptiness
as a module that instantiates nothing.

## v5

v5 moved with no table or column change at all, which is the point: the
version is the *consumption contract*, not the DDL. `meta` gained
`duplicate_path_count` and `producer_revision` as required rows, and
`analysis_status` gained a constraint it did not have -- it must now agree
with the counts beside it, so `complete` alongside a non-zero error count is
a malformed file rather than a merely surprising one. Both a database
written before this and one written after would otherwise answer `4`, and a
consumer checking the version could not tell which contract it held. Since
`meta` is key-value, the rows are invisible to a reader that does not look
for them; it is the *required* set that changed, and that is exactly what a
version exists to state.

## v4

v4 makes a name one path segment and a gate an instance. A generate block had
no row of its own -- its segment was glued onto the child's leaf name, so
`g_lane[3].u_dp` was one row holding two path segments and the per-segment
resolution the tree trades storing paths for stalled at the first generate
level. It is a level now, `module` NULL because it instantiates nothing. Gate,
switch and UDP instances had no `child` or `instance` row at all, so a
netlist-style module read as instantiating nothing -- the confusion
`child.def_module` NULL was meant to prevent -- and the gate's instance name
and the UDP's own name appeared nowhere; both tables carry them now. The
version moves because `instance.module` gained NULL and `child.def_module`
NULL gained a second meaning, both of which a v3 reader misreads rather than
misses; a primitive is told from a generate level by having a `child` row
under its parent's module. The folding claim is corrected in the same pass:
folding is across instances, not within one, so a generate loop's rows are
per iteration.

## v3

v3 lets a statement be in no procedure. A net declared with an initialiser is a
continuous assignment written at the declaration and belongs to no procedure,
and the round that gave nets their initialiser drivers had filed `-1` in
`assignment.proc` for them. `blocking`, two columns along, already spells "does
not apply" as NULL, and a consumer joining `proc` against `proc_event.proc` or
indexing procedures by it would read `-1` as a fact about some procedure -- so
it is NULL now. That value domain growing under an existing column moves the
version. The rest of the round -- the `stmt_read` table, `port.inner`,
initialiser drivers, a call's actuals bound to its formals, an `outer` on
interface-array bindings -- is addition a v2 reader would merely not ask about,
and would not have moved it alone.

## v2

v2 has port rows carry the connection's bit range and tell expressions from
nets. `.idx(stim[3:0])` had recorded `outer=stim` and dropped the select, so a
trace crossing the boundary fanned out to everything else `stim` feeds; port
rows now carry `outer_lo`/`outer_hi`/`outer_exact` in edge's encoding, and an
instance-array element keeps NULL bits with `exact=0` because it shares the
whole array's connection expression. `.en(state == RUN)` had recorded
`outer=state` with the same `conn_kind` as `.a(x)`, so every reader of `en` was
counted a reader of `state`; the operands of an expression connection now carry
their own `conn_kind`, and the wired forms -- a name, a select, a concatenation
of those -- keep the plain one. The version moves to 2 because `conn_kind`'s
domain grew, and a v1 reader would misread the new kind as a net -- the exact
confusion it exists to end.

## v1

v1 is the initial schema, and it is folded: every row hangs off a *module* -- a
definition together with the parameter values it elaborated with -- rather than
off an instance, so thirty-two copies of one core share one set of edges and
the file is sized by the amount of unique RTL rather than by the elaborated
instance count. `instance` is the only table that grows with the design, and it
carries nothing but identity -- a leaf name and a parent link -- so a
hierarchical path resolves by one indexed lookup per segment rather than by
recursion. Bodies are grouped by (definition, parameters) rather than by
slang's shared body pointer, which is canonical only sometimes and would
otherwise emit one module per instance or silently drop a non-canonical body's
dataflow. Three tables intern repeated strings, because a SystemVerilog enum or
packed struct prints as its whole member list and one edge row can carry
kilobytes of type text; identifiers share one `name` table. Columns are named
by rule so a name can be guessed: a bit range is prefixed with the column it
describes, a foreign key is named for the table it points at while the text
form ends in `_name`, and no column takes a table's name. The write path is
shaped by the database being a build artifact -- no journalling or synchronous
writes, batched inserts, indexes built after the tables are filled, every step
checked, and an intern that never falls back to `last_insert_rowid()`. The
field reference is written alongside it, including the section on what the
schema deliberately does not record: no clock domains, no `clocked` flag, no
single clock per procedure, and no branch conditions on assignments -- each
built, measured, and taken out for claiming more than a structural export can
support.
