# RTLDebugDBKit

> **SystemVerilog → elaborate → SQLite**

<p>
  <img alt="Release" src="https://img.shields.io/github/v/release/neveltyc/RTLDebugDBKit?sort=semver&style=flat-square&color=3366cc">
  <img alt="CI" src="https://img.shields.io/github/actions/workflow/status/neveltyc/RTLDebugDBKit/ci.yml?branch=main&style=flat-square&label=CI">
  <img alt="schema" src="https://img.shields.io/badge/schema-v22-3366cc?style=flat-square">
  <img alt="slang" src="https://img.shields.io/badge/slang-v11.0-3366cc?style=flat-square">
  <img alt="license" src="https://img.shields.io/badge/license-BSD--3--Clause-3366cc?style=flat-square">
</p>

[简体中文](README.md) · **English**

RTLDebugDBKit elaborates SystemVerilog with slang and writes the design
hierarchy, nets, port connections, RTL statements, control conditions and
static data dependencies into SQLite.

It exists to answer questions like:

* What drives this signal?
* What logic reads this signal?
* Which RTL does a driver correspond to?
* Which `if` / `case` / loop gates an assignment?
* What are a signal's upstream and downstream dependencies?
* How are the two sides of a module / interface boundary wired?

> [!NOTE]
> The database records the elaborated design structure and its static
> relationships. It holds no waveform values, no simulation time, and no
> runtime state.

---

## Install

Download the binary for your platform from
[Releases](https://github.com/neveltyc/RTLDebugDBKit/releases/latest).

```bash
# Linux
chmod +x rtl-designdb-linux-amd64
./rtl-designdb-linux-amd64 --version
```

---

## Command line

```text
rtl-designdb [options] <source...>
```

| Option | Meaning |
| --- | --- |
| `-f`, `-file <file>` | VCS-style filelist |
| `+define+A=B` | Define a macro |
| `+incdir+<dir>` | Include directory |
| `-I <dir>` | Include directory |
| `--top <module>` | Set the top module |
| `--single-unit` | One compilation unit |
| `-o <file.db>` | Output database |
| `--diag [N]` | Show diagnostics |
| `--log <file>` | Log file path |
| `--nolog` | Do not write a log |
| `--time-report` | Report per-phase timing |
| `--quiet` | Reduce output |
| `--version` | Print version |
| `-h`, `--help` | Show help |

The filelist understands:
`+define+` · `+incdir+` · `-f` · `-file` · `-v` · `$VAR` · `${VAR}`

Relative paths resolve against the filelist's own directory.

---

## Generate a database

```bash
# Elaborate the top from a filelist and write design.db
rtl-designdb -f rtl.f --top veerwolf_core -o design.db
```

Check the database status:

```bash
sqlite3 -box design.db "SELECT * FROM v_db_info;"
```

`analysis_status`:

| Status | Meaning |
| --- | --- |
| `complete` | Elaboration and export finished |
| `partial` | Database is usable, but has analysis gaps |
| `hierarchy_only` | Hierarchy only |

> [!TIP]
> When distributing or archiving `design.db`, compress it with `zstd` or `xz`:
> SQLite files are highly redundant and typically shrink several-fold (about
> 5–7× with zstd, ~12× with xz), a significant storage saving; decompress once
> before use.

---

## The database

Current schema version: **22**

RTLDebugDBKit stores an **instance-level design**.

```systemverilog
reg_block u_reg0 (...);
reg_block u_reg1 (...);
```

Even when two instances come from the same module definition, the database
records them separately:

```text
top.u_reg0.q
top.u_reg1.q
```

Drivers, loads, statements and dependencies all hang off a concrete instance.

### Main objects

| Category | Table / View | Contents |
| --- | --- | --- |
| Metadata | `v_db_info` | schema version, analysis status, gap counts |
| Hierarchy | `module` | module definition |
| Hierarchy | `v_tree_node` | instance / generate / primitive nodes |
| Hierarchy | `v_node_path` | full hierarchical path |
| Object | `v_net` | net / variable |
| Object | `v_term` | terminal |
| Object | `v_term_map` | terminal to inside-instance object mapping |
| Connection | `v_net_conn` | instance-boundary connection |
| Trace graph | `v_trace_edge` | normalized one-hop net → net dependencies |
| Dataflow | `v_driver` | direct driver of a net |
| Dataflow | `v_load` | direct load of a net |
| Dataflow | `v_net_dep` / `net_dep` | net → net static dependency |
| Dataflow | `v_net_attachment` | other relations attached to a net |
| Statement | `v_stmt` | RTL statement and source location |
| Statement | `v_stmt_target` | statement write target |
| Statement | `v_stmt_operand` | statement read operand |
| Process | `v_proc_event` | event control |
| Control | `v_branch` | `if` / `case` / loop |
| Control | `branch_ref` | nets a branch condition reads |
| Control | `branch_ancestor` | branch nesting |
| Reference | `v_hier_ref` | hierarchical reference |

Full definitions are in [`doc/designdb-schema.md`](doc/designdb-schema.md).

### Common columns

| Column | Meaning |
| --- | --- |
| `*_id` | Object ID |
| `inst_id` | Owning instance |
| `net_id` | Net ID |
| `stmt_id` | Statement ID |
| `branch_id` | Branch ID |
| `src_path` | Source file |
| `src_line`, `src_col` | Source location |
| `width` | Bit width |
| `*_kind` | Object or relation type |
| `*_exact` | Whether the range / mapping is exact |
| `edge_kind` | RTL semantics of a one-hop dependency |
| `map_kind` | `exact` or `inexact` bit correspondence across an edge |

> [!TIP]
> The precise semantics of bit ranges, NULL, row granularity and exactness
> are in the schema document.

---

## Relationships

### Dataflow

```systemverilog
assign q = a & b;
```

becomes:

```text
a ──▶ q
b ──▶ q
```

Cases like a dynamic index still keep the dependency, but the bit mapping may
not be exact.

```systemverilog
out = mem[raddr];
```

Here `mem` is known to affect `out`, but the specific element may not be
statically determinable.

### Module boundary

Connections across a module are kept through the terminal:

```text
outside net ── terminal ── inside net
```

Main query entries:
`v_trace_edge` · `v_net_conn` · `v_term_map` · `v_driver` · `v_load`

### Hierarchical reference

```systemverilog
assign q = u_core.state;
```

If slang can resolve `u_core.state`, the database records the target instance
and object; on a resolution failure it still keeps the reference and its
resolution status.

---

## Querying

A common trace path:

> **path → net → trace edge → statement → branch → fan-in**

| To find | Query entry |
| --- | --- |
| A net from its path | `v_node_path` + `v_net` |
| Drivers | `v_driver` |
| Loads | `v_load` |
| RTL location | `v_stmt` |
| Enclosing control conditions | `v_branch` + `branch_ancestor` |
| Nets a branch uses | `branch_ref` |
| One hop upstream/downstream | `v_trace_edge` |

<details>
<summary><strong>Handy SQL</strong></summary>

```sql
-- A net from an instance path and signal name
SELECT n.net_id, p.node_path, n.net_name
FROM v_net n JOIN v_node_path p ON p.node_id = n.inst_id
WHERE p.node_path = 'veerwolf_core.rvtop.veer.dec.tlu'
  AND n.net_name = 'mhpmc_inc_e4';
```

```sql
-- Direct drivers
SELECT driver_kind, driver_name, stmt_id, src_path, src_line
FROM v_driver WHERE signal_net_id = :net_id;
```

```sql
-- Direct loads
SELECT load_kind, load_name, stmt_id, src_path, src_line
FROM v_load WHERE signal_net_id = :net_id;
```

```sql
-- Statement source location
SELECT stmt_kind, src_path, src_line, src_col
FROM v_stmt WHERE stmt_id = :stmt_id;
```

```sql
-- Branches enclosing a statement
SELECT b.depth, b.branch_kind, b.sense, b.src_path, b.src_line
FROM stmt s
JOIN branch_ancestor a ON a.branch_id = s.branch_id
JOIN v_branch b ON b.branch_id = a.ancestor_branch_id
WHERE s.id = :stmt_id
ORDER BY b.depth;
```

```sql
-- Fan-in, four levels up
WITH RECURSIVE c(net_id, depth) AS (
  SELECT :net_id, 0
  UNION
  SELECT e.src_net_id, c.depth + 1
  FROM v_trace_edge e JOIN c ON e.dst_net_id = c.net_id
  WHERE e.src_net_id IS NOT NULL AND c.depth < 4
)
SELECT depth, count(*) FROM c GROUP BY depth ORDER BY depth;
```

</details>

---

## Schema contract

The schema version:

```text
db_info.schema_version
```

Schema v22 publishes **19 public `v_*` views**.

Prefer a view for ordinary queries. A few relations are exposed directly as
public base tables:
`net_dep` · `conn_arc` · `branch_ref` · `branch_ancestor` · `module` · `inst_param` · `expr_ref` · `prim`

> [!WARNING]
> `v_conn_arc` is an internal view and is not part of the public query
> interface.

Consumers should treat [`doc/designdb-schema.md`](doc/designdb-schema.md) as
authoritative. Schema changes are in
[`doc/schema-history.md`](doc/schema-history.md).

---

## Build from source

Requirements:

* CMake 3.20+
* A C++20 compiler

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

SQLite is compiled into `rtl-designdb` by default.

```bash
# Use the system SQLite
cmake -B build -DCMAKE_BUILD_TYPE=Release -DDESIGNDB_SYSTEM_SQLITE=ON
```

The system SQLite must be 3.37 or newer.

---

## Logging and exit codes

Default log:

```text
rtldbgdb-elab.log
```

| Code | Result |
| ---: | --- |
| `0` | `complete` |
| `3` | `partial` |
| `4` | `hierarchy_only` |
| `2` | Input or option error |
| `1` | Export failure |

`0`, `3` and `4` all keep the database.

---

## Performance

Release build, macOS arm64, schema v21:

| Design | Definitions | Instances | Nets | Statements | Dependencies | Connection arcs | Trace edges | Time | DB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| picorv32 | 1 | 1 | 225 | 744 | 3,373 | 0 | 3,373 | 0.02 s | 0.95 MB |
| tinyriscv | 26 | 43 | 870 | 1,543 | 5,180 | 451 | 5,631 | 0.03 s | 1.51 MB |
| VeeRwolf | 91 | 1,925 | 17,808 | 11,083 | 36,366 | 10,971 | 47,337 | 0.25 s | 12.35 MB |

Warm-cache SQLite point queries on the first 1,000 VeeRwolf net ids, median of
five runs:

| Query | Schema v20 | Schema v21 |
| --- | ---: | ---: |
| `net_dep` by source / target | 6.00 / 6.04 µs | 5.63 / 6.09 µs |
| `v_driver` / `v_load` | 24.29 / 25.89 µs | 22.28 / 25.03 µs |
| `conn_arc` by source / destination | — | 3.89 / 3.77 µs |
| `v_trace_edge` by source / destination | — | 8.88 / 9.30 µs |

```bash
# See per-phase timing
rtl-designdb ... --time-report
```

---

## Documentation

* [`doc/designdb-schema.md`](doc/designdb-schema.md) — schema, columns and relationships
* [`doc/schema-history.md`](doc/schema-history.md) — schema history
* [`CHANGELOG.md`](CHANGELOG.md) — release changes
* [`third-party-licenses.md`](third-party-licenses.md) — third-party licences

<details>
<summary><strong>Repository layout</strong></summary>

```text
src/                    rtl-designdb
src/sql/                schema / index / views
doc/                    documentation
examples/               example and test RTL
scripts/                verification and release scripts
.github/workflows/      CI / release
```

</details>

---

## Licence

BSD 3-Clause — [`LICENSE`](LICENSE)
