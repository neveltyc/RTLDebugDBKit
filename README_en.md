<p align="center">
  <h1 align="center">RTLDebugDBKit</h1>
  <p align="center">
    Elaborate a SystemVerilog design into a queryable <b>SQLite</b> database &mdash;
    its hierarchy, its declarations, and <b>what drives what</b>.<br>
    A single binary, no simulator, and anything that speaks SQL can read the result.
  </p>
</p>

<p align="center">
  <img alt="schema" src="https://img.shields.io/badge/schema-v20-3366cc?style=flat-square">
  <img alt="slang" src="https://img.shields.io/badge/slang-v11.0-3366cc?style=flat-square">
  <img alt="license" src="https://img.shields.io/badge/license-BSD--3--Clause-3366cc?style=flat-square">
</p>

<p align="center">
  <a href="README.md">简体中文</a> · <b>English</b>
</p>

---

## Why this exists

A waveform carries values over time and no connectivity at all. You have an FST
from an overnight regression and you need to know what drives `mhpmc_inc_e4`
down in `veer.dec`, and what condition gates the statement that drives it. The
waveform cannot answer either — it only knows whether this cycle was 0 or 1.

Commercial design databases exist — Verdi's KDB, Questa's `.dbg` — and both are
tied to a vendor toolchain and a single platform. This produces the same kind of
thing from **source**, with no simulator in the loop:

```sh
rtl-designdb -f rtl.f --top my_core -o design.db
```

What comes out is an ordinary SQLite file. No runtime, no library, no proprietary
format: Python, DuckDB, the `sqlite3` shell, and any ORM open it directly.

## Quick start

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
build/rtl-designdb examples/basic/top.sv --top top -o design.db
```

```
design.db: 5 modules, 10 instances, 37 nets, 35 terminals, 29 connections, 5 statements, 7 dependencies
```

**What drives this signal**

```sh
sqlite3 -box design.db "
SELECT p.node_path || '.' || v.signal_name AS signal, v.driver_name, v.driver_kind
FROM v_driver v JOIN v_node_path p ON p.node_id = v.signal_inst_id
WHERE v.driver_kind = 'data'"
```

```
┌─────────────────────────┬─────────────┬─────────────┐
│         signal          │ driver_name │ driver_kind │
├─────────────────────────┼─────────────┼─────────────┤
│ top.u_dp0.u_reg.q       │ d           │ data        │
│ top.u_dp0.u_alu.u_add.y │ a           │ data        │
│ top.u_dp0.u_alu.u_add.y │ b           │ data        │
│ top.u_dp1.u_reg.q       │ d           │ data        │
│ top.u_dp1.u_alu.u_add.y │ a           │ data        │
│ top.u_dp1.u_alu.u_add.y │ b           │ data        │
│ top.u_extra_reg.q       │ d           │ data        │
└─────────────────────────┴─────────────┴─────────────┘
```

`u_dp0` and `u_dp1` are two instances of one module, each with its own rows —
that is the instance-level model: the question is about *this* instance's `q`,
not about the `reg` module's `q`.


The rest of these run against VeeRwolf (1,925 instances), where the model earns
its keep.

**Find a signal by its full hierarchical path**

```sql
SELECT p.node_path || '.' || n.net_name AS path, n.width, n.decl_kind
FROM v_net n JOIN v_node_path p ON p.node_id = n.inst_id
WHERE n.net_name LIKE '%wen%' ORDER BY path LIMIT 3;
```

```
veerwolf_core.rvtop.veer.dec.arf.wen0  1  variable
veerwolf_core.rvtop.veer.dec.arf.wen1  1  variable
veerwolf_core.rvtop.veer.dec.arf.wen2  1  variable
```

**A fan-in cone — one recursive query, no application-side hierarchy walk**

```sql
WITH RECURSIVE cone(net_id, depth) AS (
  SELECT n.net_id, 0 FROM v_net n JOIN v_node_path p ON p.node_id = n.inst_id
   WHERE p.node_path = 'veerwolf_core.rvtop.veer.dec.tlu'
     AND n.net_name = 'mhpmc_inc_e4'
  UNION
  SELECT d.src_net_id, cone.depth + 1
    FROM net_dep d JOIN cone ON d.tgt_net_id = cone.net_id
   WHERE cone.depth < 4
)
SELECT depth, count(*) AS nets FROM cone GROUP BY depth ORDER BY depth;
```

```
depth  nets
0      1
1      51
2      52
3      82
4      107
```

Four levels of cone, 20 ms. **This query is what the instance-level model buys**:
under a folded model it needs application-side path algebra, because one module
body stands for N instances.

**What gates a statement**

```sql
SELECT b.depth, b.branch_kind, b.sense, b.src_line,
       (SELECT group_concat(n.net_name) FROM branch_ref r
          JOIN v_net n ON n.net_id = r.net_id
         WHERE r.branch_id = b.branch_id) AS reads
FROM stmt s
JOIN branch_ancestor a ON a.branch_id = s.branch_id
JOIN v_branch b ON b.branch_id = a.ancestor_branch_id
WHERE s.id = :stmt_id
ORDER BY b.depth;
```

```
depth  branch_kind  sense  src_line  reads
1      loop                91
2      if           then   92        addr_i,addr_map_i
```

`branch_ancestor` is the closure of the gating tree, so "everything gating this
statement" is one join rather than a walk up `parent_branch_id`.

## Install

The repository is private and carries no tags yet, so the GitHub release page is
empty. Build from source (below), or wait for the first `v*` tag — `release.yml`
publishes the four platform binaries alongside a `sha256sums.txt` when one is
pushed:

| Platform | Binary | Linking |
|:--|:--|:--|
| Linux x86-64 | `rtl-designdb-linux-amd64` | musl, fully static |
| Linux ARM64 | `rtl-designdb-linux-arm64` | musl, fully static |
| Windows x86-64 | `rtl-designdb-windows-amd64.exe` | MSVC, static CRT |
| macOS (Apple Silicon) | `rtl-designdb-macos-arm64` | native |

The platform set is [rwave](https://github.com/neveltyc/RWaveAnalyzer)'s,
deliberately: this database is read next to a waveform, so it ships everywhere
the viewer does. rwave keeps its linux-amd64 glibc-dynamic because its vendor
backends arrive by `dlopen`; this exporter has **no `dlopen` at all** — SQLite is
compiled in with loadable extensions omitted — so both Linux targets are musl and
fully static: one file that runs on any distro, any glibc, including the CentOS
7-era farms EDA tools live on.

## Building from source

Requires CMake 3.20+ and a C++20 compiler. slang is fetched by CPM and **pinned
to `v11.0`**; SQLite is the official amalgamation, compiled in, so the binary
carries its own copy and does not depend on the host's libsqlite3.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The only switch is `-DDESIGNDB_SYSTEM_SQLITE=ON`, which links the system library
instead (≥ 3.37 required, because `db_info` is a STRICT table).

slang is pinned to an exact revision rather than a range because the exporter is
built on API slang does not hold stable across releases — `ValuePath` and its
longest-static-prefix bounds, `AnalysisManager`'s analyzed scopes and procedures,
`HierarchicalReference`'s resolved path, canonical instance bodies — and v10 to
v11 broke several of them outright. An upgrade is a deliberate, differentially
tested step, not a float.

[scripts/build-release.sh](scripts/build-release.sh) builds the four release
binaries locally: macOS builds its own target natively, the Linux pair build
through a Docker Alpine container, and windows-amd64 needs a Windows host, which
CI provides.

## Input

Deliberately small. It takes a filelist and defines, the way `vcs -f` does, and
nothing else — configuration, project layout and output formatting belong to
whatever drives it.

| | |
|---|---|
| `-f`, `-file <file>` | VCS-style filelist. Understands `+define+`, `+incdir+`, nested `-f`/`-file`, `-v` library files, and `$VAR`/`${VAR}` from the environment. Relative paths resolve against the filelist's own directory. |
| `+define+A=B` | Preprocessor define, `+`-separated like VCS. |
| `+incdir+<dir>`, `-I <dir>` | Include directory. The two spellings are equivalent. |
| `--top <module>` | Top module. Without it slang picks every uninstantiated module. |
| `--single-unit` | Compile the whole list as one compilation unit, so a leading `` `define `` header reaches every later file. VCS and Verilator behave this way; slang defaults to per-file units. Designs that keep their configuration in a header need this. |
| `-o <file.db>` | Output database. |
| `--diag [N]` | Print elaboration diagnostics — all of them, or the first N. |
| `--log <file>` | Write the elaboration log here instead of beside the database. Relative paths resolve against the cwd, as `-o`'s do. |
| `--nolog` | Do not write the elaboration log. |
| `--time-report` | Report how long each phase took. |
| `--quiet` | Only report problems. |

Bare paths are taken as source files.

## Log and exit code

Every run writes an elaboration log — `rtldbgdb-elab.log` beside the database
unless `--log` names another path. It holds slang's own diagnostics (all of them,
whatever `--diag` shows on the terminal) and this tool's findings. `--quiet`
quiets the terminal, not the log; `--nolog` turns the file off, and asking for
both a `--log` path and `--nolog` is refused.

Every line carries its producer and severity in fixed columns, so the file
answers grep rather than being scrolled:

```
slang        error|   in instance: soc.u_core
slang        error: rtl/fifo.sv:39:5: error: unknown module 'ghost'
slang        error|     ghost #(.MODE(2)) u_g (.clk(clk), .req(req));
slang        error|     ^~~~~
rtl-designdb note: 2 instantiation(s) name a module that could not be resolved
rtl-designdb note: wrote design.db (partial)
```

```sh
grep -c '^slang  *error:' rtldbgdb-elab.log   # how many errors (items, not lines)
grep '^slang  *error'  rtldbgdb-elab.log      # each one with its source snippet
grep '^rtl-designdb'   rtldbgdb-elab.log      # what the exporter itself found
```

The exit code says how complete the export is, so a caller **does not have to
open the database** to find out. It is the same rule that writes
`db_info.analysis_status`, so the two can never disagree:

| | |
|---|---|
| `0` | Database written, `analysis_status = 'complete'`. |
| `3` | Database written, `'partial'` — elaboration errors, or a gap one of the seal's counts names. |
| `4` | Database written, `'hierarchy_only'` — the compilation is fatally errored, so there is a tree and no dataflow. |
| `2` | No database: the invocation or the sources were unusable (bad option, unreadable filelist, `--top` did not elaborate). |
| `1` | No database: the export itself failed. |

A log path named explicitly with `--log` that cannot be opened stops the run with
`2` — **before the export has cost anything or replaced an earlier database**.
The reason is that the next step in a pipeline usually greps that file, and an
absent file reads there as *no errors*. The derived default path only warns and
carries on: the database is the product.

## Output

One SQLite file, 26 tables in six groups.

| Group | Tables |
|---|---|
| Hierarchy | `module` (the source definition), `tree_node` (the elaborated tree, one id space), `inst` (every elaborated module instance, with its parameter signature) + `inst_param` (that signature made queryable), `prim` (gates, switches, UDPs) |
| Objects | `net` (every connectable object of every instance, implicit nets flagged), `term` + `term_map` (each instance's terminals and what they stand for inside) |
| Dataflow | `net_dep` — net-to-net dependencies, one row per statement instance, each naming the operand, target, gating level, call or primitive it came from; `proc`, `stmt`, `stmt_target`, `assign_operand`, `expr_ref`, `proc_event` — the statement layer those rows point into |
| Gating | `branch` — the level a statement sits under, as a tree; `branch_ref` (what the level's condition reads) + `branch_label` (a case arm's labels, evaluated) + `branch_ancestor` (the tree's closure, so "everything gating this" is one join) |
| Boundaries | `net_conn` — what the parent wired to each terminal, segment by segment with bit windows; `hier_ref` — references that leave an instance, as written *and* resolved to the net where slang could |
| Provenance | `src_file` (every file slang read, with its SHA-256), `db_info` (the seal: schema version, tool, top, status and counts — one typed row) |

The model is **instance-level**: rows hang off the elaborated instance, so "who
drives bit 3 of *this* instance's `q`" is one indexed lookup and a fan-in cone is
a recursive query. The folded model this replaced could answer neither without
application-side path algebra. Thirty-two copies of one core are thirty-two row
sets, stamped from one analysis; the measured cost on a real SoC is about 2× the
folded file size.

**[doc/designdb-schema.md](doc/designdb-schema.md) is the field reference** —
every table and column, the bit-range encoding, the naming rules, what the schema
deliberately does not record, and the known limits. Consumers start at its
**stable query interface**: 19 views (`v_tree_node`, `v_net`, `v_driver`,
`v_load`, `v_stmt`, …) whose columns, NULL rules and row granularity are the
versioned contract.

**The version is the consumer contract.** `db_info.schema_version` is currently
**20**. It does not turn while the fields and SQL a consumer reads stay the same,
and it turns immediately when they do not. The change history lives in the
`SchemaVersion` comment block, not in the documentation.

## Measurements

Release build, macOS arm64, against public designs, schema v20:

| design | definitions | instances | nets | statements | dependencies | time | database |
|---|---:|---:|---:|---:|---:|---:|---:|
| picorv32 | 1 | 1 | 225 | 744 | 3,385 | 0.02 s | 0.98 MB |
| tinyriscv | 26 | 43 | 870 | 1,543 | 5,355 | 0.03 s | 1.53 MB |
| VeeRwolf (`veerwolf_core`) | 91 | 1,925 | 17,808 | 11,083 | 36,414 | 0.22 s | 11.4 MB |

The instance-level expansion is the column to watch: VeeRwolf's 1,925 instances
stamp out from 164 parameterised bodies (10× replication), and the database lands
at roughly **2× the folded v9 file** rather than 10× — type text stays interned,
and the biggest tables scale with statements, not with statements times fan-out.
`scripts/export-real-designs.sh` reproduces this table against a local checkout
of the designs.

The shape holds well past these: a 320k-line design elaborating to 145k instances
and 4.8M rows exports in about 4 seconds and 324 MB. `--time-report` breaks that
down — roughly a quarter in slang, a sixth in the walk, and the rest inside
SQLite writing rows and building indexes.

Elaboration cost is slang's: memory scales with the number of elaborated
instances, so a very large flat design wants `--top` on a subtree.

## Repository layout

```
CMakeLists.txt          the build; slang and SQLite are fetched, not vendored
src/                    main.cpp (CLI + filelist parsing), Extractor (the
                        two-pass export), DesignDb (the writer)
src/sql/                the DDL: Schema, Indexes, Views
src/extract/            the export, in layers. Ref/SymbolText/Template are the
                        vocabulary (header-only); SourceLocator, DeclIndex and
                        StatementWalker the services over it; TemplateBuilder
                        (+ _Conn) is pass 1 and Stamper pass 2, each behind a
                        one-function interface
doc/designdb-schema.md  the field reference
examples/basic/         RTL small enough to read, exported by CI
examples/constructs/    one fixture per LRM construct family, each exported and
                        asserted by CI under its own verifier mode
examples/options/       not a construct fixture: two files, two tops, a macro
                        defined in one and used in the other, and a header
                        reachable only through +incdir+ -- so --single-unit,
                        +define+ and the config digest have something to be
                        wrong about
examples/reorder/       also not a construct fixture: five files in reverse
                        alphabetical order, the one example long enough to reach
                        slang's threaded source loader and so the only one that
                        can catch src_file ids following the buffers
scripts/                see below
.github/workflows/      ci.yml, binaries.yml (the four platform artifacts),
                        release.yml (published on a v* tag)
```

| Script | What it does | Who runs it |
|---|---|---|
| `verify-designdb.py` | Read an export back, fail if hollow or malformed. `--list-modes` is the one place the fixture set is named; `--domain-coverage` checks the corpus against every published enum value | CI |
| `check-reproducible.py` | Export one design twice, fail if any row differs | CI |
| `build-release.sh` | The four release platforms | CI |
| `diff-designdb.py` | Export one corpus with two binaries and fail on any row diff not declared expected — the migration gate for internal refactors | local |
| `designdb-coverage.py` | What an export had to approximate, counted (no pass/fail) | local |
| `export-real-designs.sh` | Reproduces the measurements table from a local checkout of the public designs | local |
| `check-rtl.sh` | Validate RTL against the Verilator and Icarus front ends | local |

## Testing

```sh
# Export and read back, one construct family at a time (the CI loop)
for mode in $(python3 scripts/verify-designdb.py --list-modes); do
  build/rtl-designdb "examples/constructs/$mode.sv" -o "$mode.db"
  python3 scripts/verify-designdb.py "$mode.db" "$mode"
done

# Does the corpus produce every value the schema publishes?
python3 scripts/verify-designdb.py --domain-coverage ./*.db
```

[CI](.github/workflows/ci.yml) builds both SQLite configurations, exports the
whole of `examples/` and reads each database back — a build that links proves the
slang pin resolves, not that the exporter still writes rows. It then checks the
corpus against the schema: **every value the published domains name, and every
word in the four view vocabularies, has to be produced by some fixture**. So the
test set is driven by the contract rather than by whichever constructs happened
to break once. The same push builds all four release binaries
([binaries.yml](.github/workflows/binaries.yml)) and repeats the export sweep on
each platform — the Linux pair builds inside an Alpine container and then runs on
the bare glibc runner, so a dynamic dependency that crept into the "static"
binary surfaces in CI rather than on a farm.

### Testing the RTL itself

Anything used as a test case should first pass
`scripts/check-rtl.sh <file.sv> [top]`, which runs the file past Verilator and
Icarus. They disagree in **both** directions — Verilator accepts a continuous
assign with a variable index that Icarus correctly rejects, Icarus rejects an
unpacked array slice that Verilator correctly accepts — so disagreement is a
prompt to read the LRM, not a verdict. Several "defects" during development
turned out to be invalid RTL written by hand.

A front end that has not implemented a construct at all says nothing about the
RTL, so a file may declare that one of them cannot accept it:

```
// check-rtl: expect-fail icarus -- interface ports are not in its grammar
```

The declared failure then counts as a pass, and the tool *accepting* the file
counts as a failure — so the marker cannot outlive the limitation it records.
Both directions being checked is also why this stays a developer gate rather than
a CI one: the marker set is only well defined against one pinned pair of front
ends, and the versions here track Homebrew's. Ubuntu 24.04's Verilator 5.020
rejects four files this set does not waive.

## For consumers and AI agents

The database carries its own **seal**: one `db_info` row naming the schema
version, the tool version, the slang version, the commit that produced it, the
top, the analysis status and every gap count. So the first query against an
unfamiliar `.db` is always:

```sh
sqlite3 -box design.db "SELECT * FROM v_db_info"
```

Three things matter for an agent:

1. **The exit code is the contract** — branch on it without opening the file
   (0 / 3 / 4 all wrote a database).
2. **Query the `v_` views only.** Their columns, NULL rules and row granularity
   are the versioned contract; the base tables are not.
3. **Diagnostics are in the log.** `grep '^slang  *error:' rtldbgdb-elab.log`
   gives one line per error, each naming its file and what went wrong. There is
   no need to re-run the export to see them.

## Licence

BSD 3-Clause — see [LICENSE](LICENSE). slang and SQLite are fetched at build time
and keep their own licences.
