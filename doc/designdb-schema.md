# Design database — internals and field reference

The complete schema of the design database: every table, every column, how the
model is folded, how bit ranges are encoded, and what the database deliberately
does *not* record.

**In:** a VCS-style SystemVerilog filelist (or bare source paths), plus defines,
include directories, and optionally a top module.
**Out:** one SQLite file. No runtime, no library, no simulator — anything that
speaks SQL can read it.

**Schema version 1.** `meta.schema_version` carries it. A reader that does not
know the version should refuse the file rather than read it as though the layout
had held.

## Contents

- [What it answers](#what-it-answers)
- [The model: folded](#the-model-folded)
- [Table index](#table-index)
- [Hierarchy](#hierarchy) — `module`, `instance`, `child`
- [Declarations](#declarations) — `symbol`
- [Dataflow](#dataflow) — `edge`, `assignment`, `assign_operand`, `proc_event`
- [Crossing a module boundary](#crossing-a-module-boundary) — `port`
- [Bit ranges](#bit-ranges)
- [Provenance](#provenance) — `source_file`, `meta`
- [Naming rules](#naming-rules)
- [What is deliberately not here](#what-is-deliberately-not-here)
- [Known limits](#known-limits)

## What it answers

A waveform carries values over time and no connectivity at all. A netlist
carries connectivity and has lost the source. This database is the missing
half: the elaborated structure of the design, addressable by name, with a
`file:line` on every row.

| Question | Where the answer is |
|---|---|
| What drives this signal? | `edge` where `dst` is the signal — with the construct and the statement's own line |
| What reads it? | `edge` where `src` is the signal |
| Which *statement* wrote it, when several do? | `assignment` (one row per statement, with `proc`/`seq`/`blocking`) + `assign_operand` |
| Which *bits* came from where? | `src_lo`/`src_hi`/`dst_lo`/`dst_hi` on `edge`, read with `*_exact` |
| Where does this path leave the module? | `port` — the only table spanning two modules |
| What is this instance, and what is it inside? | `instance` (leaf name + parent chain) and `module` |
| How wide is it, which direction, where is it declared? | `symbol` |
| What is this procedure sensitive to? | `proc_event` |
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
difference between practical and not. `instance` is the only table that grows
with the design; everything else grows with the source.

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
| [`child`](#hierarchy) | instantiation written inside a module | unique RTL |
| [`symbol`](#declarations) | declaration inside a module | unique RTL |
| [`edge`](#dataflow) | dependency (one signal drives another) | unique RTL |
| [`assignment`](#dataflow) | statement that writes a target | unique RTL |
| [`assign_operand`](#dataflow) | signal read by an assignment's RHS | unique RTL |
| [`proc_event`](#dataflow) | edge event a procedure triggers on | unique RTL |
| [`port`](#crossing-a-module-boundary) | port connection on a child instance | unique RTL |
| [`source_file`](#provenance) | file slang actually read | source tree |
| [`meta`](#provenance) | key/value (schema version, tool, top) | fixed |

## Hierarchy

| table | column | meaning |
|---|---|---|
| `module` | `id`, `name`, `params` | A definition plus its elaborated parameter values. The parameters are part of the identity: a different parameterisation resolves different generate branches and different widths, so it is a different netlist. |
| `instance` | `id`, `name`, `module`, `parent` | The instance tree. `name` is the leaf, generate block included; the full path is the chain of names to the root. Paths are not stored — they are all distinct, so interning saves nothing and storing them costs the text twice once the lookup index is counted. |
| `child` | `module`, `name`, `def_name`, `def_module` | What a module instantiates. `def_module` is NULL when slang could not resolve the definition — recorded rather than dropped, so "there is an instance here whose module I do not have" is distinguishable from "this module instantiates nothing". |

## Declarations

| table | column | meaning |
|---|---|---|
| `symbol` | `module`, `name`, `kind`, `type`, `width`, `direction`, `file`, `line`, `col` | Every declaration. `kind` is `variable`/`net`/`parameter`/`port`. `direction` is `0`=in, `1`=out, `2`=inout, `3`=ref, and non-NULL exactly when the signal is a port. `width` is in bits, NULL when the type is not integral. |

One row per real signal. slang carries a `Port` symbol *and* the net or variable
behind it; the port contributes its direction to the signal's row rather than a
row of its own.

This table exists because the rest of the schema is derived from edges, so a
signal would otherwise appear only if it took part in dataflow. That is enough
to trace a driver and wrong for everything else — a declared-but-unused signal,
an output nobody drives, and a clock that only appears in a sensitivity list are
all absent from an edge-derived view, and those are the ones worth asking about.

```sql
-- outputs nothing drives
SELECT n.text FROM symbol s JOIN name n ON n.id = s.name
WHERE s.direction = 1
  AND NOT EXISTS (SELECT 1 FROM edge e WHERE e.module = s.module AND e.dst = s.name);
```

## Dataflow

| table | column | meaning |
|---|---|---|
| `edge` | `module`, `src`, `dst` | One dependency inside a module. `src` is NULL when the right-hand side reads nothing at all (`q <= 8'h0`) — the row still names the statement. Self-feedback is a real row: `cnt <= cnt + 1` records `cnt → cnt`, which is what makes "who reads cnt" answerable. |
| | `src_type`, `dst_type` | Type text. |
| | `kind`, `construct` | `continuous_assign`/`procedural`, and the construct: `assign`, `always_ff`, `always_comb`, … |
| | `control` | 1 when the operand reached the target through a branch condition rather than the right-hand side. |
| | `file`, `line` | The statement's own line, not the enclosing procedure's. |
| | `src_lo`/`src_hi`/`src_exact`, `dst_lo`/`dst_hi`/`dst_exact` | Bit ranges — see [below](#bit-ranges). |

| table | column | meaning |
|---|---|---|
| `assignment` | `id`, `module`, `dst`, `dst_lo`/`dst_hi`/`dst_exact` | One statement that writes a target. |
| | `kind`, `construct`, `file`, `line` | As `edge`. |
| | `proc`, `seq` | Which procedure in the module, and the order within it. Both are needed: `seq` restarts per procedure, so without `proc` two assignments from different `always` blocks read as one ordered sequence. |
| | `blocking` | 1 for `=`, 0 for `<=`, NULL for a continuous assign. Not decoration — two assignments to one target in one block resolve by different rules, and it separates a block-local temporary written with `=` inside an `always_ff` from a real register. |
| | `dropped_operands` | Operands not recorded: compile-time constants, and references outside this module. A row with one operand and three dropped is not a row that reads one signal. |
| `assign_operand` | `assignment`, `name`, `src_lo`/`src_hi`/`src_exact` | What the right-hand side reads. |
| `proc_event` | `module`, `proc`, `signal`, `edge_kind` | Every edge event a procedure triggers on. `signal` is NULL when the event expression is not a plain reference. |

`edge` flattens a procedure into "these signals drive that one". `assignment`
keeps the statements apart, so a target written in four places reads as four
statements rather than one merged set.

## Crossing a module boundary

| table | column | meaning |
|---|---|---|
| `port` | `module`, `child`, `def_module`, `port` | A port connection, recorded against the parent module that writes it. `module` is the parent, `def_module` the child's module, `port` the formal inside it. |
| | `direction` | 0=in, 1=out, 2=inout, 3=ref, 4=unknown. |
| | `outer`, `outer_type` | The net in the *parent's* namespace. |
| | `outer_width` | The width of the connection **as written**, looking through the implicit conversion slang inserts to fit the formal. Comparing it against the formal's `symbol.width` is how a width-mismatched connection is found. NULL for an element of an instance array, where every element shares the array's connection expression and the comparison has no meaning. |
| | `conn_kind` | 0=a net, 1=tied to a constant, 2=left unconnected. |
| | `file`, `line` | Where the connection is written, in the parent. |

Both directions are indexed, because the two queries need opposite ones: a
driver query walks inward from a net, a load query outward from a formal. This
is the only table spanning two modules, and it is what lets a trace leave the
module it started in — which matters more than it sounds: on real designs a
large share of modules are pure structural wrappers with no procedural logic at
all, and without port rows every path through a register leaves the graph.

## Bit ranges

`src_lo`/`src_hi` and `dst_lo`/`dst_hi` are **LSB-relative offsets into the
flattened object**, not declared indices. `logic [15:8] off` has bit 15 at
offset 7; `logic [0:7] up` has bit 0 at offset 7. A consumer mapping them onto
declared indices mislabels every signal not declared `[N-1:0]`; the declared
range is recoverable from `src_type`/`dst_type`.

Read them together with `*_exact`:

| bits | exact | meaning |
|---|---|---|
| NULL | 1 | the whole object |
| NULL | 0 | somewhere inside it, and we cannot say where (`q[i] <= d`) |
| set | 1 | exactly those bits |

Storing both NULL cases the same way made uncertainty read as fact.

## Provenance

| table | meaning |
|---|---|
| `source_file` | Every file slang actually read, with its SHA-256 — including headers reached by `` `include ``, which are exactly the files that change without the filelist changing. |
| `meta` | Schema version, tool name, top module. `top` holds the *elaborated* top(s), space-separated when the design has several — written whether or not `--top` was passed. A reader that does not know the version should refuse the file rather than read it as though the layout had held. |

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
