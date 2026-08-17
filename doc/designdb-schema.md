# Design database — internals and field reference

The complete schema of the design database: every table, every column, how the
model is folded, how bit ranges are encoded, and what the database deliberately
does *not* record.

**In:** a VCS-style SystemVerilog filelist (or bare source paths), plus defines,
include directories, and optionally a top module.
**Out:** one SQLite file. No runtime, no library, no simulator — anything that
speaks SQL can read it.

**Schema version 8.** `meta.schema_version` carries it. A reader that does not
know the version should refuse the file rather than read it as though the layout
had held.

Version 8 added the [stable query interface](#stable-query-interface): seven
views — `v_database_info`, `v_tree_node`, `v_signal`, `v_port_connection`,
`v_dependency`, `v_driver`, `v_load` — that resolve the intern tables and spell
out the NULL conventions, so an ordinary consumer never joins `name`/`type`/
`file` or decodes `conn_kind` itself. The version moves because the views'
existence and column sets are now contract, and a v7 file answers those queries
with "no such table". From here on, removing or renaming a view or view column,
changing a column's semantics or NULL rules, or changing a view's row
granularity bumps the version; changing only how a view is computed, with its
contract intact, does not.

v8 also gave `port` a **`child_id`** foreign key (indexed, surfaced by both
hierarchy views), because without it the composition the views promise did not
hold: the tree spells an instance one segment at a time while port rows carry
the folded generate-prefixed spelling, so joining the two by name returned
nothing exactly at generate boundaries — and `(module, name)` is no substitute
key, since two unnamed gates in one module legally share a name.

Version 7 stopped crossing the two sides of an assignment. Every target used to
be paired with every operand, so

```systemverilog
assign {a, b} = {x, y};
```

exported four edges where the RTL has two — and `y → a` and `x → b`, which are
not dataflow at all, carried `src_exact = 1` and `dst_exact = 1` exactly like the
two real ones. Both sides are now walked as positioned slots and paired only
where their bits meet, which also narrows the target: `q[7:0] = {hi, lo}` records
`hi → q[7:4]` rather than `hi → q`. `assign_operand` was crossed the same way and
is fixed with it. The new `edge.map_exact` column says whether the pairing is
per-bit — see [Bit correspondence](#bit-correspondence).

Version 6 gave names to two relations that were being guessed from file and line.

`assignment`, `hier_ref` and `stmt_read` gained **`stmt`**, a per-module
statement ordinal. A statement whose target is *outward* has no `assignment` row
to gather its parts — the write goes to `hier_ref` and the reads to `stmt_read` —
so `assign top.a = x; assign top.b = y;` exported two writes and two reads that
a consumer could pair four ways when the RTL supports one. See
[Statement identity](#statement-identity).

`child` gained **`id`** and **`kind`**, and `instance` gained **`child`**.
`def_module IS NULL` had meant "a primitive" and "a definition slang could not
find" indistinguishably, though a trace stopping at one means something quite
different from a trace stopping at the other; the folded and expanded hierarchy
tables were related by nothing a consumer could join on; and an unresolved
instantiation had no `instance` row at all, so a path through a black box did not
resolve and the tree reported the same emptiness as a module that instantiates
nothing. All three are fixed together in [Hierarchy](#hierarchy).

Version 5 changed no table and no column. It moved because the version is the
**consumption contract**, not the DDL: `meta` gained `duplicate_path_count` and
`producer_revision` as rows that are now always written, and `analysis_status`
gained a constraint it did not have — it must agree with the counts beside it,
so `complete` next to a non-zero `error_count` is a malformed file rather than a
merely surprising one. Because `meta` is key-value, a v4 reader sees the new
rows as rows it does not query; what changed is the set a consumer may *rely*
on, and a database written before this would otherwise also answer `4`.

Version 4 made `instance.name` **one path segment, always**. A generate block
is now a level of the tree in its own right, so `g_lane[3].u_dp` is two rows
rather than one name containing a dot, and the walk below resolves one segment
at a time as it says. Gate, switch and UDP instances gained `child` and
`instance` rows too, so a netlist-style module no longer reads as instantiating
nothing and `u_and` is a name you can look up.

Both are why the version moved rather than being additions. `instance.module`
is **NULL** for a level that is not a module instance, and `child.def_module`
NULL now means "a primitive, which has no module row" as well as "a definition
slang could not resolve". A v3 reader gets both wrong rather than missing them.
To tell a generate level from a primitive: a primitive has a `child` row under
its parent's module and a generate block does not.

Version 3 added: the [`stmt_read`](#reads-with-no-target) table, so an
assertion's reads and the read side of an outward write have somewhere to live;
[`port.inner`](#crossing-a-module-boundary), for a formal whose connection
names it something else (`.ext_one(inner)`); drivers for nets declared with an
initialiser (`wire w = a & b`), which had none at all; a call's actuals tied to
the formals they bind to, so a trace crosses `bump(d)`; and an `outer` on
interface-array bindings, which carried none.

Those are all additions a v2 reader would simply not ask about. What moved the
version is `assignment.proc`, which **gained NULL** — a statement in no
procedure at all, which is what a net's initialiser is. A reader that joins it
against `proc_event.proc`, or indexes procedures by it, reads NULL as a fact
about procedure 0 unless it knows the layout changed.

Version 2 added: the [`hier_ref`](#leaving-the-hierarchy) table; bit ranges,
interface bindings and expression operands on [`port`](#crossing-a-module-boundary)
— `conn_kind` gained values 3, 4 and 5, which a v1 reader would misread as plain
nets, and that misreading is why the version moved rather than staying a pure
addition; statement-level waits, with `wait`, `file` and `line`, in
`proc_event` — a v1 reader treating those rows as sensitivity reads an
`initial` block as clocked, which is the second reason the version moved;
primitive edges (`kind='primitive'`); `symbol.kind='interface_port'`;
`file.source_file`; self-feedback edges; and `meta.top` written from the
elaborated tops whether or not `--top` was passed.

## Contents

- [What it answers](#what-it-answers)
- [The model: folded](#the-model-folded)
- [Table index](#table-index)
- [Stable query interface](#stable-query-interface) — the seven views
- [Hierarchy](#hierarchy) — `module`, `instance`, `child`
- [Declarations](#declarations) — `symbol`
- [Dataflow](#dataflow) — `edge`, `assignment`, `assign_operand`, `proc_event`
- [Crossing a module boundary](#crossing-a-module-boundary) — `port`
- [Leaving the hierarchy](#leaving-the-hierarchy) — `hier_ref`
- [Reads with no target](#reads-with-no-target) — `stmt_read`
- [Bit ranges](#bit-ranges)
- [Provenance](#provenance) — `source_file`, `meta`
- [Naming rules](#naming-rules)
- [What is deliberately not here](#what-is-deliberately-not-here)
- [Why in-module replication is not folded](#why-in-module-replication-is-not-folded)
- [Known limits](#known-limits)

## What it answers

A waveform carries values over time and no connectivity at all. A netlist
carries connectivity and has lost the source. This database is the missing
half: the elaborated structure of the design, addressable by name, with a
`file:line` on every row.

| Question | Where the answer is |
|---|---|
| What drives this signal? | `edge` where `dst` is the signal — with the construct and the statement's own line |
| Which *statement* wrote it, when several do? | `assignment` (one row per statement, with `proc`/`seq`/`blocking`) + `assign_operand` |
| Which *bits* came from where? | `src_lo`/`src_hi`/`dst_lo`/`dst_hi` on `edge`, read with `*_exact` |
| Where does this path leave the module? | `port` — the only table spanning two modules, interface bindings included |
| What does this module touch outside itself? | `hier_ref` — every reference that leaves it, exactly as written |
| What reads it? | the union of `edge` where `src` is it, `assign_operand`, `proc_event`, `hier_ref` with `write = 0`, and `stmt_read` — each records a different kind of read |
| What is this instance, and what is it inside? | `instance` (leaf name + parent chain) and `module` |
| How wide is it, which direction, where is it declared? | `symbol` |
| What is this procedure sensitive to, or waiting on? | `proc_event` |
| Is my database still valid for this source tree? | `source_file` — every file read, with its SHA-256 |

Two properties make it usable as a *back end* rather than a report: the names
are the elaborated names a waveform also uses, so a signal in a dump joins to a
row by path; and every row carries `file`/`line`, so an answer is always
something the reader can open.

## The model: folded

Rows hang off a **module** — a definition together with the parameter values it
elaborated with — not off an instance. Thirty-two copies of one core share one
set of edges, and a separate `instance` table expands them back into hierarchy.

That is the difference between a database sized by the amount of unique RTL and
one sized by the elaborated instance count, and on a large SoC it is the
difference between practical and not.

Folding is **across** instances, not within one. Replication written *inside* a
module — a generate loop, an instance array — is elaborated before the fold is
applied, so `child`, `symbol`, `port`, `edge` and `assignment` each carry one
row per iteration. The same source with four lanes and with sixty-four gives
6 and 66 `child` rows. What they grow with is the module's elaborated contents,
not the file's line count; only `instance` grows with the whole design.

Names inside a module are **module-relative**, generate-block prefix included
(`g_lane[3].u_dp.data`). A consumer resolves a hierarchical path by walking down
one segment at a time against the `(parent, name)` index — one indexed lookup
per segment, no recursion over the table — then reads the rows of that
instance's module and prefixes them with the path it walked.

```sql
-- what drives `data_o` in top.u_core.u_dp
WITH RECURSIVE walk(id, rest) AS (
    SELECT NULL, 'top.u_core.u_dp'
    -- …descend segment by segment against instance(parent, name)…
)
SELECT s.text, e.construct, f.path, e.line
FROM edge e
JOIN name d ON d.id = e.dst
LEFT JOIN name s ON s.id = e.src
LEFT JOIN file f ON f.id = e.file
WHERE e.module = :module_of_that_instance AND d.text = 'data_o';
```

## Table index

Repeated strings are interned into three tables — `name` (identifiers), `type`
(type text), and `file` (paths) — each `(id, text)`. **Every `INTEGER` column
referencing one of those is a row id**, so reading a name means joining
`name.id`; the examples throughout show the join.

| Table | One row per | Grows with |
|---|---|---|
| [`module`](#hierarchy) | definition + parameter values it elaborated with | unique RTL |
| [`instance`](#hierarchy) | elaborated instance | **the design** |
| [`child`](#hierarchy) | instantiation written inside a module — modules, primitives and unresolved black boxes, told apart by `kind` | elaborated module contents |
| [`symbol`](#declarations) | declaration inside a module | elaborated module contents |
| [`edge`](#dataflow) | dependency (one signal drives another) | elaborated module contents |
| [`assignment`](#dataflow) | statement that writes a target | elaborated module contents |
| [`assign_operand`](#dataflow) | signal read by an assignment's RHS | unique RTL |
| [`proc_event`](#dataflow) | edge event a procedure triggers on or waits on | unique RTL |
| [`port`](#crossing-a-module-boundary) | port connection on a child instance | elaborated module contents |
| [`hier_ref`](#leaving-the-hierarchy) | reference that leaves the module, as written | unique RTL |
| [`stmt_read`](#reads-with-no-target) | signal read by a statement that writes nothing nameable | unique RTL |
| [`source_file`](#provenance) | file slang actually read | source tree |
| [`meta`](#provenance) | key/value (schema version, tool, top) | fixed |

## Stable query interface

**Start here as a consumer.** Seven views resolve the intern tables and spell
out the NULL conventions, so ordinary queries never join `name`, `type`, `file`
or `source_file`, and never decode `conn_kind` or NULL patterns by hand. The
base tables remain the normative storage layer — the views add no data, no
precomputation and no size — but from v8 the views' existence and column sets
are part of the schema contract, verified by `verify-designdb.py` on every
export.

| view | one row is | built on |
|---|---|---|
| `v_database_info` | the whole database (exactly one row) | `meta`, pivoted to fixed columns with counts CAST to INTEGER |
| `v_tree_node` | one node of the expanded instance tree | `instance` + resolved names + `node_kind` |
| `v_signal` | one declaration in one module variant | `symbol` + type text + both file spellings |
| `v_port_connection` | one port binding in the **folded** model | `port` + `connection_kind_name` + coalesced inner name + `child_id`, the join key to `v_tree_node` |
| `v_dependency` | one intra-module dependency | `edge`, direction-neutral |
| `v_driver` | one direct driving relation of `signal_name` | `v_dependency`, read from the target's side |
| `v_load` | one direct load of `signal_name` | `v_dependency`, read from the source's side, null-source rows excluded |

Rules a consumer can rely on:

- **One view row is one base row.** Every join inside a view is against a
  primary key, so nothing fans out; `count(view) == count(base)` is asserted.
- **`v_driver` and `v_load` are one step, inside one module.** Fan-in cones,
  root drivers and anything crossing an instance boundary are the consumer's
  loop: follow `v_driver`/`v_load` inside the module, cross the boundary with
  `v_port_connection`, and walk levels with `v_tree_node`.
- **The two hierarchy views join on `child_id`, and on nothing else.**
  `v_tree_node.child_id = v_port_connection.child_id` is the boundary-crossing
  hop. The *names* do not relate them: the tree spells an instance one segment
  at a time (`u_dec`) while port rows carry the folded spelling with the
  generate prefix (`g_rep[0].u_dec`), so a name join returns nothing exactly
  where a trace needs it.
- **Key on `module_id`, not `module_name`.** One definition elaborated with two
  parameter sets is two module variants, and a `module_name` filter returns rows
  from all of them. The tree node you selected carries the right `module_id`;
  name (plus `module_params`) is for a human browsing.
- **`v_dependency` does not name the assignment statement.** `edge` and
  `assignment` are [independent projections](#they-are-two-projections-not-two-halves-of-a-join);
  per-statement operands stay in `assign_operand`.
- **Bit ranges keep `edge`'s semantics unchanged**: LSB-relative offsets into
  the flattened object, `NULL` + `exact=1` the whole object, `NULL` + `exact=0`
  somewhere unknown inside it — see [Bit ranges](#bit-ranges).
- **`mapping_exact=0` does not mean the dependency is doubtful.** The source
  reaches the target; only the bit-for-bit correspondence is unavailable — see
  [Bit correspondence](#bit-correspondence).
- **`driver_name` NULL is a real driving statement** (`assign q = 1'b0;`, a
  target fed only from outside the module). It is kept in `v_driver` and, being
  no signal's load, excluded from `v_load`.
- **Read `connection_kind`, not NULL patterns**: a constant tie, an unconnected
  port and an external tie all have `outer_signal_name` NULL, and the kind is
  what tells them apart.
- **`node_kind` classifies the tree**: `root` and `generate` are structural
  levels nothing declared; `module` descends; `primitive`'s dataflow is already
  in the parent's edges; `unresolved` is a black box with no dataflow anywhere.

Four queries that cover most consumers. The driver/load forms take `module_id`
from the tree node already selected — filtering by `module_name` instead would
mix every parameterisation of that definition:

```sql
-- The children of one tree node.
SELECT instance_id, instance_name, node_kind, module_id, module_name
FROM v_tree_node
WHERE parent_instance_id = ?;
```

```sql
-- What directly drives this signal, in the selected node's module variant.
SELECT driver_name, driver_lo, driver_hi, driver_exact,
       is_control, mapping_exact, file_path, source_line
FROM v_driver
WHERE module_id = ? AND signal_name = ?;
```

```sql
-- What this signal directly feeds, in the same variant.
SELECT load_name, load_lo, load_hi, is_control, file_path, source_line
FROM v_load
WHERE module_id = ? AND signal_name = ?;
```

```sql
-- Cross the boundary: the selected instance's port bindings.
SELECT formal_port_name, direction_name, outer_signal_name,
       connection_kind_name, outer_lo, outer_hi
FROM v_port_connection
WHERE child_id = ?;   -- the tree node's child_id
```

All four resolve through the existing indexes (`edge_by_dst`, `edge_by_src`,
`instance_by_parent`, `port_by_child`); the views add none of their own.

## Hierarchy

| table | column | meaning |
|---|---|---|
| `module` | `id`, `name`, `params` | A definition plus its elaborated parameter values — **and its localparams**, which slang models the same way. That only ever over-splits, never under-splits, so it remains a sound identity; but a key rebuilt from the overrides written at instantiation will not match it. The parameters are part of the identity: a different parameterisation resolves different generate branches and different widths, so it is a different netlist. |
| `instance` | `id`, `name`, `module`, `parent` | The instance tree. `name` is **one path segment** — a generate block is its own level, so the chain of names to the root *is* the hierarchical path, joined with `.`. `module` is NULL for a level that is not a module instance (a generate block, a primitive, or an unresolved black box). Paths are not stored — they are all distinct, so interning saves nothing and storing them costs the text twice once the lookup index is counted. |
| | `child` | The instantiation in the parent's module body this row expands. NULL only for the root and for a generate block — neither is something anyone wrote as an instance. See below. |
| `child` | `id`, `module`, `name`, `def_name`, `def_module` | What a module instantiates. `def_name` always names the definition as written (`counter`, `and`, `my_latch`); `def_module` points at its rows when there are any. |
| | `kind` | `module`, `primitive`, or `unresolved` — what `def_module IS NULL` could not say. |

### The two hierarchies, and how they relate

`child` is folded (one row per instantiation *written*, shared by every instance
of the enclosing module) and `instance` is expanded (one row per instantiation
*elaborated*). `instance.child` is the link, and it is one-to-many in the
direction you would expect: a `child` row of a module instantiated 32 times has
32 `instance` rows pointing at it.

The two also spell names differently, which is why the link is worth storing
rather than recomputing. For

```systemverilog
for (genvar i = 0; i < 2; i++) begin : g
    leaf u_leaf(...);
end
```

`child` holds `g[0].u_leaf` and `g[1].u_leaf` — the whole path, relative to the
module body — while `instance` holds `g[0]` and `g[1]` as levels of their own with
`u_leaf` beneath each. Recovering one from the other meant re-parsing the text and
walking it segment by segment; `instance.child` is that work done once.

**`child.kind` is not decoration — the three kinds stop a trace differently.**

| kind | `def_module` | has an `instance` row | what it means when a trace arrives |
|---|---|---|---|
| `module` | its module | yes | Descend: the module's own rows continue the trace. |
| `primitive` | NULL | yes | Do not descend. A gate has no module and never will; its dataflow is already in the parent's `edge` rows with `kind='primitive'`. |
| `unresolved` | NULL | yes | Stop. This is a black box — slang could not find the definition, so no dataflow for it exists anywhere in the database. Counted in `meta.unresolved_count`; these are the individual rows. |

A generate level is none of the three: it has `module IS NULL` *and*
`child IS NULL`, which is what distinguishes it from a primitive or a black box.

## Declarations

| table | column | meaning |
|---|---|---|
| `symbol` | `module`, `name`, `kind`, `type`, `width`, `direction`, `file`, `line`, `col` | Every declaration. `kind` is `variable`/`net`/`parameter`/`type_parameter`/`specparam`/`port`/`interface_port`. A `type_parameter`'s `type` is the type it stands for; a generic `interface` port's is the word `interface`, since it names no definition. `direction` is `0`=in, `1`=out, `2`=inout, `3`=ref, and non-NULL exactly when the signal is an ordinary port — an `interface_port` has no direction. For an `interface_port`, `type` is the interface definition, with the declared modport when the port restricts itself to one (`simple_bus.master`); it is the name a reference like `bus.vld` resolves its first segment against. |

One row per real signal. slang carries a `Port` symbol *and* the net or variable
behind it; the port contributes its direction to the signal's row rather than a
row of its own.

This table exists because the rest of the schema is derived from edges, so a
signal would otherwise appear only if it took part in dataflow — a
declared-but-unused signal, and a clock that only appears in a sensitivity
list, would not be here at all.

It does **not** offer "which outputs are undriven" as a query, and an earlier
draft that did was wrong: an output driven by a child instance's output port
has a `port` row and no `edge` row, so a `symbol` MINUS `edge` test reports
every structurally driven output as dead — on a design of ordinary shape,
nothing but false positives. Deciding that a signal is undriven means reading
`edge`, `port` and `hier_ref` together and knowing which of them the design
uses, which is the consumer's judgement to make, not a column here.

## Dataflow

| table | column | meaning |
|---|---|---|
| `edge` | `module`, `src`, `dst` | One dependency inside a module. `src` is NULL when no operand has a name in this module's namespace — the right-hand side read nothing at all (`q <= 8'h0`), or everything it read lives outside the module (`assign seen = bus.vld`, where the operands are in `hier_ref` at the same `module`/`file`/`line`). Either way the row names the statement, so a driver query answers. Self-feedback is a real row: `cnt <= cnt + 1` records `cnt → cnt`, which is what makes "who reads cnt" answerable. A downward hierarchical reference that stays inside the module's subtree is an ordinary row whose name is dotted (`u_child.sig`). |
| | `src_type`, `dst_type` | Type text. |
| | `kind`, `construct` | `continuous_assign`/`procedural`/`primitive`, and the construct: `assign`, `always_ff`, `gate:and`, `udp:my_latch`, … A gate, switch or UDP instance is one edge per (input, output) pairing — `and (y, a, b)` is `a → y` and `b → y`. |
| | `control` | 1 when the operand reached the target through a branch condition rather than the right-hand side. |
| | `file`, `line` | The statement's own line, not the enclosing procedure's. |
| | `src_lo`/`src_hi`/`src_exact`, `dst_lo`/`dst_hi`/`dst_exact` | Bit ranges — see [below](#bit-ranges). |
| | `map_exact` | Whether the source's bits map one-to-one onto the target's. Not a restatement of the two `*_exact` columns — see [Bit correspondence](#bit-correspondence). |

| table | column | meaning |
|---|---|---|
| `assignment` | `id`, `module`, `dst`, `dst_lo`/`dst_hi`/`dst_exact` | One statement that writes a target. |
| | `kind`, `construct`, `file`, `line` | As `edge`. |
| | `proc`, `seq` | Which procedure in the module, and the order within it — within the procedure's own statements. A subroutine's body is numbered *after* the call that invokes it, so `seq` is not execution order across a call. Both are needed: `seq` restarts per procedure, so without `proc` two assignments from different `always` blocks read as one ordered sequence. `proc` is **NULL** when the statement is in no procedure — a net declared with an initialiser is a continuous assignment written at the declaration. |
| | `blocking` | 1 for `=`, 0 for `<=`, NULL for a continuous assign. It describes **the statement**, so a `=` inside a function keeps its 1 whether the function was reached from an `assign` or from an `always_ff` — while `kind`/`construct` beside it name the *enclosing* construct, which is how the same body reports `assign` from one caller and `always_ff` from another. Not decoration — two assignments to one target in one block resolve by different rules, and it separates a block-local temporary written with `=` inside an `always_ff` from a real register. |
| | `dropped_operands` | Operands not recorded: compile-time constants, and references outside this module. A row with one operand and three dropped is not a row that reads one signal. |
| | `stmt` | Which statement in the module, as [an ordinal](#statement-identity). Shared with the `hier_ref` and `stmt_read` rows of the same statement — which is how an assignment that also reads something outward is related to that reference. |
| `assign_operand` | `assignment`, `name`, `src_lo`/`src_hi`/`src_exact` | What the right-hand side reads. |
| `proc_event` | `module`, `proc`, `signal`, `edge_kind`, `wait`, `file`, `line` | Every edge event a procedure triggers on **or waits on**. `signal` is NULL when the event expression is not a plain reference. `wait` is the discriminator: `0` is the procedure's sensitivity list, so the event triggers the block; `1` is an event control reached during execution (`@(posedge clk);` inside an initial block or a task), which suspends it and says nothing about what triggers it. **Filter on `wait = 0` to ask what a procedure is sensitive to** — an `initial` block has only `wait = 1` rows, and reading those as a trigger set makes it look clocked. |

`edge` flattens a procedure into "these signals drive that one". `assignment`
keeps the statements apart, so a target written in four places reads as four
statements rather than one merged set.

### They are two projections, not two halves of a join

There is no key that recovers one from the other, and
**`(module, dst, file, line)` is not one.** Those columns are shared by every
statement written on a line, so joining on them returns a cross product rather
than an error:

```systemverilog
always_ff @(posedge clk) begin if (c) q <= a; else q <= b; end
```

is two `assignment` rows and three `edge` rows — `a → q`, `b → q`, and `c → q`
with `control = 1` — and all five carry the same module, `dst`, file and line.
Joined on them, each assignment pairs with all three edges, and a consumer
asking what the first statement reads is told `a`, `b` *and* `c`.

Ask each table what it can answer:

| question | table |
|---|---|
| What does **this statement** read? | `assign_operand`, keyed on `assignment.id`. Exact: `a` for the first assignment above, `b` for the second. |
| Does `a` reach `q` **at all**? | `edge`. Deduplicated on purpose — one row however many statements produced it. |
| In what order, in which procedure, blocking or not? | `assignment` — `proc`, `seq`, `blocking`. |

Neither direction can be tightened into a foreign key. One edge covers many
assignments by design; and some edges have no assignment at all — a gate is not
a statement, and a subroutine call's actual-to-formal binding
(`always_ff @(posedge clk) bump(d);`) is dataflow nobody wrote an assignment
for.

**Which branch condition guards which statement is not recorded.** The `c → q`
edge says `c` reaches `q` as a condition, not which of the two assignments it
gates. That is the [deliberate omission](#what-is-deliberately-not-here) below,
not a gap in this pair of tables.

### Bit correspondence

Three columns on `edge` describe precision, and they answer different questions.
`src_exact` and `dst_exact` each describe **one end**: whether that end's bit
range could be narrowed, or is an upper bound. **`map_exact` describes the
relationship between the ends** — whether a bit of the source can be followed to
a specific bit of the target.

They are independent. `q = a + b` knows both ranges exactly and still cannot say
which bit of `a` reaches which bit of `q`, because a carry crosses them:
`src_exact = 1`, `dst_exact = 1`, `map_exact = 0`.

| what the RTL says | edges | `map_exact` |
|---|---|---|
| `{a, b} = {x, y}` | `x → a`, `y → b` | 1 — the halves correspond, and the pairs that do not share a bit are not edges at all |
| `q[7:0] = {hi, lo}` | `hi → q[7:4]`, `lo → q[3:0]` | 1 — each operand drives its own slice |
| `q = a + b` | `a → q`, `b → q` | 0 — real dependencies, no per-bit mapping |
| `q = {2{a}}` | `a → q` | 0 — one source in two places |
| `q = f(a)` | `a → q` | 0 — opaque |

**`map_exact = 0` does not mean the dependency is doubtful.** The source does
reach the target; the pairing is simply at range granularity rather than bit
granularity. A consumer doing bit-level tracing should follow `map_exact = 1`
edges bit by bit and treat `map_exact = 0` edges as coupling the whole ranges.

A few conservative cases report 0 where a sharper answer exists: a widened
operand (`q = a` with `a` narrower), a truncated one, and bitwise operators like
`~a` that really are per-bit. Being wrong in the other direction — claiming a
correspondence that does not hold — is what this column exists to prevent, so the
rule is deliberately narrow: only a plain reference filling its window exactly.

### Statement identity

`assignment`, `hier_ref` and `stmt_read` each carry **`stmt`**: an ordinal the
exporter assigns while walking a module. Rows sharing `(module, stmt)` came from
one statement. The number is meaningless outside its module and is not an id into
any table — it exists to say "these rows are one statement".

For a statement writing a target the module *can* name, `assignment.id` already
did this job and `assign_operand` hangs off it. `stmt` matters where there is no
assignment row to hang from — a write that leaves the module:

```systemverilog
assign top.a = x; assign top.b = y;
```

The writes land in `hier_ref`, the reads in `stmt_read`, and every one of those
four rows carries the same module, file and line. Pairing them on those columns
gives `top.a` fed by both `x` and `y`, and `top.b` likewise — four answers where
the source gives two. Pairing on `stmt` gives exactly `top.a ← x` and
`top.b ← y`:

```sql
SELECT hp.text AS writes, rn.text AS fed_by
FROM hier_ref h
JOIN name hp ON hp.id = h.path
JOIN stmt_read r ON r.module = h.module AND r.stmt = h.stmt
JOIN name rn ON rn.id = r.name
WHERE h.write = 1;
```

This is what makes an interface or XMR driver chain resolvable at all, which is
most of what a modport driver is: `assign bus.vld = ready;` beside
`assign bus.data = payload;` is the ordinary shape of that code, not a
contrived one.

`stmt` is NULL where a row belongs to no statement the walk opened. `edge`
deliberately has no `stmt` column: it is deduplicated, so one row can be several
statements and there would be nothing single to point at — see
[above](#they-are-two-projections-not-two-halves-of-a-join).

## Crossing a module boundary

| table | column | meaning |
|---|---|---|
| `port` | `module`, `child`, `def_module`, `port` | A port connection, recorded against the parent module that writes it. `module` is the parent, `def_module` the child's module, `port` the formal inside it. |
| | `direction` | 0=in, 1=out, 2=inout, 3=ref, 4=unknown. NULL for an interface binding — an interface port has no direction. |
| | `inner` | The signal inside the child that `port` stands for, when the connection names the formal something else (`module m(.ext_one(inner))`). NULL when the two are one name, which is the ordinary case. Without it a consumer holding the internal name has no way back to the binding. |
| | `outer`, `outer_type` | The net in the *parent's* namespace. |
| | `outer_width` | The width of the connection **as written**, looking through the implicit conversion slang inserts to fit the formal. Comparing it against the formal's `symbol.width` is how a width-mismatched connection is found. NULL for an element of an instance array, where every element shares the array's connection expression and the comparison has no meaning — and NULL whenever the connection's type is not integral (an unpacked array or struct port), so `outer_width IS NULL` is not a test for "array element". |
| | `outer_lo`/`outer_hi`/`outer_exact` | The bits of `outer` the connection selects: `.idx(stim[3:0])` attaches bits 0..3 of `stim`, not all of it. Same encoding as `edge` — see [Bit ranges](#bit-ranges). NULL with exact=0 for an element of an instance array, as with `outer_width`. |
| | `conn_kind` | 0=a net, 1=tied to a constant, 2=left unconnected, 3=an operand of an expression, 4=an interface binding, 5=attached to a signal with no name in this module. |
| | `modport` | The modport restricting an interface binding, when one does. NULL otherwise. |
| | `file`, `line` | Where the connection itself is written, in the parent — not the instantiation's own line, which on an instance written one port per line would be the same for every row and would tell two ports of it apart from neither each other nor the header. A port left **unconnected** has no connection text to point at and falls back to the instantiation. |
| | `child_id` | The [`child`](#hierarchy) row this binding belongs to — the stable key relating a port row to the hierarchy, and through `instance.child` to the expanded tree. The *name* in `child` above is not that key: it carries the folded spelling with the generate prefix (`g_rep[0].u_dec`) while the tree spells one segment per level, and two unnamed gates in one module legally share a name. Indexed (`port_by_child`): a trace crosses every boundary through this column. |

Both directions are indexed, because the two queries need opposite ones: a
driver query walks inward from a net, a load query outward from a formal. This
is the only table spanning two modules, and it is what lets a trace leave the
module it started in — which matters more than it sounds: on real designs a
large share of modules are pure structural wrappers with no procedural logic at
all, and without port rows every path through a register leaves the graph.

**`conn_kind` 3 is not a wire.** `.en(state == RUN)` samples `state`, but it
does not alias it to `en`: treating the operand as a connection attributes
every reader of `en` to `state`. The operands are still recorded — they are
what the expression reads, selector indices included — flagged so a consumer
can tell wire from computation.

**`conn_kind` 5 has a NULL `outer` and is still a connection.** `.a(tb.glob)`
attaches the port to a signal this module cannot name, so there is nothing to
put in `outer`; what it is tied to is in `hier_ref` at the same `file`/`line`.
The row is written anyway, because dropping it made a port tied to a
testbench signal read exactly like a port nobody connected — the distinction
`conn_kind` 2 exists to draw.

**`conn_kind` 4 is the alias that makes `child.bus.*` resolvable.** For an
interface binding, `port` is the child's interface port, `outer` the interface
instance — or the parent's own interface port, passed through — in the parent's
namespace, and `outer_type` the interface definition. The signals live in the
interface instance; this row is how a reference on either side finds them.

## Leaving the hierarchy

| table | column | meaning |
|---|---|---|
| `hier_ref` | `module`, `path` | One reference that leaves the module, **as written and normalised**: `bus.vld` through an interface port, `tb.u_dut.state` as an XMR. Whitespace and comments are removed, so one reference interns as one name however it was spelled. Constant selects are resolved only where the reference is a chain of member accesses slang keeps as such; an XMR or an interface member is resolved to a *single* symbol, and there the text is kept verbatim — `arr[LO+1].vld` interns separately from `arr[1].vld`, and a genvar stays unresolved. Trailing selects are stripped to the root object, because that is what `path_lo`/`path_hi` are offsets into. A trailing select is *not* part of the path — the bits it selects are in `path_lo`/`path_hi`, as in `edge`. |
| | `write` | 1 when the module writes the path, 0 when it reads it. An inout port connection tied to an external signal is recorded as the write. |
| | `kind`, `construct` | The enclosing construct, in `edge`'s vocabulary — plus `kind='port'` with the direction in `construct` for a port connection tied to an external signal. |
| | `file`, `line` | Where the reference is written. |
| | `path_lo`/`path_hi`/`path_exact` | The bits the reference touches, as everywhere else. |
| | `stmt` | Which statement produced the reference, as [an ordinal](#statement-identity). This is what pairs an outward *write* with the `stmt_read` rows that fed it; file and line cannot, because two outward writes on one line share them. |

The target of such a reference cannot go into `edge` or `port` rows: those name
signals in the module's own namespace, shared by every instance of it, and an
absolute path would bake one instance's hierarchy into all of them. The
as-written *text* has no such problem — every instance of the module carries
the same spelling — so the text is what is stored. Resolving it against the
hierarchy belongs to the consumer, who has the `instance` tree and the
interface bindings (`port.conn_kind=4`) this table's rows resolve through.

Two deliberate exclusions. A **bare name** that leaves the module — typically a
package-level variable read by a called subroutine — is counted in the
exporter's external-reference note but not stored: with no path in the text, it
resolves against imports a reader cannot see, and a bare `mask` row would be
noise pretending to be an answer (`pkg::mask`, written with its package, is
stored). And a **downward reference that stays inside the module's subtree**
(`u_child.sig`) was never external at all: it lands in ordinary `edge` and
`assignment` rows as a dotted module-relative name.

## Reads with no target

| table | column | meaning |
|---|---|---|
| `stmt_read` | `module`, `name` | One signal read by a statement that writes nothing this module can name. |
| | `kind`, `construct` | As `edge`, plus the reading statement's own word as the construct: an assertion's (`assert`, `assume`, `cover`, `restrict`, `expect`), a system task's name (`$display`, `$error`, …), `wait` for a wait condition, or `call` for a void call to a user subroutine that assigns nothing. |
| | `file`, `line` | The statement's own location. |
| | `src_lo`/`src_hi`/`src_exact` | Bits of the thing read, spelled as everywhere else. A **concurrent** assertion's operands are walked for value references rather than through path analysis, so their ranges come back NULL with `src_exact = 0`. An **immediate** assertion, a `wait` condition and a system task's arguments go through path analysis like any other read and carry real ranges. |
| | `stmt` | The statement this read belongs to, as [an ordinal](#statement-identity). Join it against `hier_ref.stmt` to find the outward write this read actually fed, rather than every outward write on the line. |

One rule decides what lands here: **a statement that reads and writes nothing
this module can name**. Such a statement has nowhere else to go — `edge`
requires a `dst`, and an `assign_operand` row hangs off an `assignment` that
cannot exist without a module-relative target.

- **An assertion** writes nothing at all. `assert (req !== 1'bx)` and
  `assert property (@(posedge clk) req |-> ##1 ack)` read `req`, `ack` and
  `clk`, and without this table those signals read as though no part of the
  design looked at them.
- **An assignment whose target lies outside the module** —
  `assign bus.vld = |payload;` through an interface port. The write is in
  `hier_ref`; this is the read side of the same statement, at the same
  `file`/`line`. An operand that is *itself* outward goes to `hier_ref` too.
- **A statement whose whole effect is to read**: a system task's arguments
  (`$display("%0h", watched);`), a wait condition (`wait (done);`), a void call
  to a subroutine that assigns nothing. A testbench that only ever prints a
  signal was otherwise reporting it as one nothing had looked at.

A call that *does* write is not here: its reads are already attributed through
the edges pairing them with what the subroutine assigns, and recording them
again would double-count. The same goes for a condition whose branch writes —
`if (x) r <= 1;` makes `x` a control edge on `r`, which is the more informative
row.

**Answering "what reads this signal" means a union**: `edge.src`,
`assign_operand.name`, `proc_event.signal`, `hier_ref` where `write = 0`, and
`stmt_read.name`. Each records a different kind of read, and none subsumes
another.

## Bit ranges

`src_lo`/`src_hi` and `dst_lo`/`dst_hi` — and the same pairs on
`assign_operand`, `port.outer_*` and `hier_ref.path_*` — are **LSB-relative
offsets into the flattened object**, not declared indices. `logic [15:8] off` has bit 15 at
offset 7; `logic [0:7] up` has bit 0 at offset 7. A consumer mapping them onto
declared indices mislabels every signal not declared `[N-1:0]`; the declared
range is recoverable from `src_type`/`dst_type`.

Read them together with `*_exact`:

| bits | exact | meaning |
|---|---|---|
| NULL | 1 | the whole object |
| NULL | 0 | somewhere inside it, and we cannot say where (`q[i] <= d`) |
| set | 1 | exactly those bits |
| set | 0 | somewhere **within** those bits — a static outer select with a dynamic inner one (`mem2[1][i]`) narrows the range without pinning it |

Storing both NULL cases the same way made uncertainty read as fact.

`*_exact` describes **one end's own range**. Whether the two ends correspond bit
for bit is a separate question with a separate column — see
[Bit correspondence](#bit-correspondence).

## Provenance

| table | meaning |
|---|---|
| `source_file` | Every file slang actually read, with its SHA-256 — including headers reached by `` `include ``, which are exactly the files that change without the filelist changing. `file.source_file` joins every interned as-written path to its hashed row, so checking freshness never means matching spellings. |
| `meta` | Key/value. Every key below is written on every successful export — that set is part of the schema version, so a v5 database always has all of them. A reader that does not know the version should refuse the file rather than read it as though the layout had held. |

Every row of `meta` is written after the data and the indexes are complete, so
a file carrying them is a file whose export ran to the end.

| key | meaning |
|---|---|
| `schema_version` | This document's version. `5`. |
| `tool` | Always `rtl-designdb`. |
| `top` | The *elaborated* top(s), space-separated when the design has several — written whether or not `--top` was passed. |
| `analysis_status` | `complete`, `partial`, or `hierarchy_only`. See below. |
| `error_count`, `warning_count` | Elaboration diagnostics. |
| `unresolved_count` | Instantiations whose module slang could not resolve — black boxes. Recorded with a null `child.def_module` rather than dropped. |
| `empty_procedure_count` | Procedures the analysis says drive something but from which nothing could be extracted, normally a statement slang marked bad. |
| `duplicate_path_count` | Instances whose hierarchical path was already taken. Non-zero means a path lookup may be ambiguous. |
| `tool_version` | The exporter's release version. |
| `slang_version` | The slang tag it was built against. |
| `producer_revision` | The commit it was built from (`git describe --always --dirty --tags`), or `unknown` outside a checkout. `tool_version` moves once a release; extraction semantics move more often, so this is what tells two exporters apart. |
| `config_digest` | SHA-256 over the generation conditions: input files, include directories, defines, single/multi-unit, elaborated tops, default timescale, tool and slang version — each tagged and length-prefixed, so `+incdir+a +define+b` and `+incdir+a+b` do not collide. Equal digests mean two databases were produced under the same conditions; it is not a build command you can replay. |

**`analysis_status` agrees with the counts.** `complete` requires
`error_count`, `empty_procedure_count` and `duplicate_path_count` all zero;
`partial` requires at least one of them non-zero. `hierarchy_only` means the
compilation was fatally errored or nothing was analysed — the dataflow tables
are empty because none could be read, which a consumer must not report as "this
design has no drivers". `warning_count` and `unresolved_count` constrain
nothing: plenty of warnings say nothing about extraction, and a design that
instantiates a vendor macro it has no source for is as complete as its sources
allow.

## Naming rules

So a column name can be guessed rather than looked up:

- A bit range is prefixed with the column it describes: `src_lo` beside `src`,
  `dst_lo` beside `dst`, and `assign_operand` uses `src_*` for the same idea so
  one spelling covers "bits of the thing read" everywhere.
- A foreign key is named for the table it points at; a text name ends in
  `_name`. `child.def_module` is a module id, `child.def_name` is the text.
- No column takes a table's name.

## What is deliberately not here

The database records what the source says. It does not record conclusions drawn
from it, and several fields were written, measured, and then removed for
claiming more than transcription can support.

**No clock domains, and no CDC.** Without constraints, structural analysis can
establish "these two registers are clocked by different nets" and nothing more.
That is a candidate list, not a finding: a divide-by-2 off the same source
counts as a different net and is not a crossing, and no amount of netlist
walking can tell you otherwise. Deciding which net is a clock, whether two are
related, and whether a crossing is real needs an SDC this tool does not read.

**No `clocked` flag on a signal.** It was computed once per procedure and
stamped onto every target in it, which runs "this procedure is edge-triggered"
(a fact) into "this target is held by a flop" (not the same thing) — a
block-local temporary written with `=` inside an `always_ff` got it too. Whether
a procedure has an edge is in `proc_event`, attributed to the procedure, where
it belongs.

**No single "the clock" per procedure.** An event list has no order:
`@(posedge clk or negedge rst_n)` and `@(negedge rst_n or posedge clk)` are the
same block written two ways and both are ordinary. Recording the first would
have recorded how the author arranged the list, and would have named the reset
in half of all async-reset flops. `proc_event` holds all of them.

**No branch conditions on assignments.** An earlier version stored the
`if`/`case` chain reaching each assignment, so a consumer could evaluate it
against a waveform and name the assignment in effect at a given time. It worked
and it was still wrong to ship: evaluating the conditions means evaluating
SystemVerilog expressions, which a waveform tool cannot do; the sampling instant
is an edge of a clock this schema deliberately does not identify; guard operands
are frequently absent from a dump; X during reset makes a chain neither true nor
false; and for blocking assignments the question has no single answer because
all of them ran. The statements and their line numbers are here. The reasoning
belongs to the reader.

**No source text.** `file:line` is enough to find a statement, and a reader who
wants the line can open the file. Storing it would duplicate something that can
go stale against a digest that can only say *that* it changed.

## Known limits

- SystemVerilog only: the front end is slang. VHDL, and mixed-language designs'
  VHDL halves, are out of scope.
- `force` is recorded as an ordinary blocking assignment in its enclosing
  construct; `release` leaves no row. The force's overriding semantics are not
  modelled — a consumer replaying drive order sees it as one more write.
- A statement whose target lives outside the module (`assign bus.vld = x;`)
  has no `assignment` row, since that row cannot exist without a
  module-relative target. The write is in `hier_ref`, an outward operand joins
  it there, and an operand inside the module is in
  [`stmt_read`](#reads-with-no-target). The three carry a shared
  [`stmt`](#statement-identity) ordinal, so they can be reassembled into the one
  statement they came from; what is still missing is the statement's `proc`,
  `seq` and `blocking`, which only `assignment` records and which such a
  statement therefore has nowhere to put.
- A port that stands for a *concatenation* of internal signals
  (`module m(.ext_pair({hi, lo}))`) records the binding, but `hi` and `lo`
  keep a NULL `symbol.direction`: slang offers the concatenation through
  neither the scope's members nor the body's port list, so there is nothing to
  attribute the direction from.
- A variable initialiser (`logic [7:0] c = 0`) is not a driver and has no row.
  It writes once at time zero; treating it as a continuous assignment would
  put one on every register that declares a reset value.
- A reference assembled through a macro (`` `DEFINED_XMR ``, or ``tb.`SIG``)
  is counted in the external-reference note but has no `hier_ref` row: its
  text spans two buffers and cannot be recovered as one span, and the
  elaborated symbol's own path names one instance's hierarchy — which is the
  one thing a row shared by every instance must not carry.
- A reference that slang resolves to a single hierarchical symbol keeps its
  spelling verbatim, so a genvar inside one (`b[g].sig` in a generate loop)
  stays unresolved. Each elaborated reference is its own row and its own
  count — the loop above yields four — but the four share a spelling and only
  `file`/`line` and their order distinguish them.
- Function and task locals appear as edge and assignment endpoints but have no
  `symbol` row — they are not declared in the module.
- `assign_operand` is not deduplicated: an expression reading one signal twice
  yields two rows, which is what the source says.
- A statement slang marks bad is skipped, and a bad child takes its enclosing
  block with it, so a malformed procedure can leave the export silently. Only
  invalid RTL reaches that state, and the diagnostic count is reported —
  warnings included, since the diagnostic responsible can be one.
- Elaboration cost is slang's: memory scales with the number of elaborated
  instances, so a very large flat design wants `--top` on a subtree.

## Why in-module replication is not folded

Folding is across instances. A generate loop or an instance array *inside* a
module is elaborated first, so its rows are per iteration — and that is a
deliberate stop, not an unfinished one.

Measured before deciding. On two real cores the question does not arise:
picorv32 has no replication-derived rows at all among 4,455 edges, tinyriscv
has 2 of 912 symbols and none of its 4,869 edges. On a synthetic bank of N
lanes, rows grow linearly at about **83 bytes each** — a 1,024-lane module,
far past anything ordinary, costs ~1 MB and exports in 0.03 s.

The reason not to fold is not the size, though. It is what the iterations
contain. Take the two statements in that loop:

| statement | rows at N=64 | distinct contents |
|---|---:|---:|
| `assign par[i] = ^din[i*W +: W];` | 64 | **64** |
| `always_ff @(posedge clk) tick <= clk;` | 64 | 1 |

Only the second folds. The first differs in exactly the columns that make it
useful — `dst_lo`/`dst_hi` walk 0,1,2… and `src_lo`/`src_hi` walk 0-7, 8-15,
16-23 — because indexing by the iteration is what a generate loop is *for*. A
loop body that ignores its genvar is the rare case.

To fold the first, the row would have to store a stride and a base rather than
a range, and the consumer would have to evaluate index arithmetic to recover
the bits. That is the same trade this schema refuses for branch conditions:
storing something a reader must compute against, in place of the fact itself.
The bits are the answer, and one row per iteration is what states them.
