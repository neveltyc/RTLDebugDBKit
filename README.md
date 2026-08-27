# RTLDebugDBKit

`rtl-designdb` reads a VCS-style SystemVerilog filelist, elaborates it with
[slang](https://github.com/MikePopoloski/slang), and writes a queryable SQLite
database of the design: its hierarchy, its declarations, and what drives what.

A waveform carries values over time and no connectivity at all. Answering "who
drives this signal", "what reads it", or "where am I in the hierarchy" needs a
design database alongside it. The commercial ones exist — Verdi's KDB, Questa's
`.dbg` — and both are tied to a vendor toolchain and a single platform. This
produces the same kind of thing from source, with no simulator involved.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
build/rtl-designdb -f rtl.f --top my_core -o design.db
```

```
design.db: 26 modules, 43 instances, 870 nets, 487 terminals, 470 connections, 1543 statements, 5355 dependencies
```

Try it against the RTL in this repo:

```sh
build/rtl-designdb examples/basic/top.sv --top top -o design.db
sqlite3 design.db "SELECT signal_name, driver_name, driver_kind FROM v_driver"
```

## Input

Deliberately small. It takes a filelist and defines, the way `vcs -f` does, and
nothing else — configuration, project layout and output formatting belong to
whatever drives it.

| | |
|---|---|
| `-f`, `-file <file>` | VCS-style filelist. Understands `+define+`, `+incdir+`, nested `-f`/`-file`, `-v` library files, and `$VAR`/`${VAR}` from the environment. Relative paths resolve against the filelist's own directory. |
| `+define+A=B` | Preprocessor define, `+`-separated like VCS. |
| `+incdir+<dir>`, `-I <dir>` | Include directory. |
| `--top <module>` | Top module. Without it slang picks every uninstantiated module. |
| `--single-unit` | Compile the whole list as one compilation unit, so a leading `` `define `` header reaches every later file. VCS and Verilator behave this way; slang defaults to per-file units. Designs that keep their configuration in a header need this. |
| `-o <file.db>` | Output database. |
| `--diag [N]` | Print elaboration diagnostics — all of them, or the first N. |
| `--log <file>` | Write the elaboration log here instead of beside the database. Relative paths resolve against the cwd, as `-o`'s do. |
| `--nolog` | Do not write the elaboration log (see *Log and exit code*). |
| `--time-report` | Report how long each phase took. |
| `--quiet` | Only report problems. |

Bare paths are taken as source files.

## Log and exit code

Every run writes an elaboration log — `rtldbgdb-elab.log` beside the database
unless `--log` names another path: slang's own diagnostics — all of them,
whatever `--diag` shows on the terminal — and this tool's findings. `--quiet`
quiets the terminal, not the log; `--nolog` turns the file off, and asking for
both a `--log` path and `--nolog` is refused. The last line names the database
the run wrote, or says it wrote none — a run that fails before publishing
leaves the previous export in place and replaces its log, and that line is
what tells the two apart. That pairing is the default layout's; a log sent
elsewhere with `--log` is the caller's to keep beside the right database.

A log the caller named is treated as a contract, because the next step in a
pipeline usually greps it and an absent file reads there as *no errors*: a
`--log` path that cannot be opened stops the run with exit 2, before the
export has cost anything or replaced an earlier database. The default path
only warns and carries on — the database is the product, and a read-only
directory is not a reason to lose it.

Every line carries its producer and severity in fixed columns. A multi-line
item marks exactly one line `:` — the one carrying the message — and the rest
`|`, so a `:` line always names a file and what went wrong. slang prints its
instance path *before* the diagnostic, so that context arrives as a `|` line
above its own `:` line:

```
slang        error|   in instance: soc.u_core
slang        error: rtl/fifo.sv:39:5: error: unknown module 'ghost'
slang        error|     ghost #(.MODE(2)) u_g (.clk(clk), .req(req));
slang        error|     ^~~~~
rtl-designdb note: 2 instantiation(s) name a module that could not be resolved
rtl-designdb note: wrote design.db (partial)
```

So the file answers questions rather than being scrolled:

```sh
grep -c '^slang  *error:' rtldbgdb-elab.log   # how many errors (items, not lines)
grep '^slang  *error' rtldbgdb-elab.log       # each one with its source snippet
grep '^rtl-designdb' rtldbgdb-elab.log        # what the exporter itself found
grep 'fifo.sv' rtldbgdb-elab.log              # everything about one file
```

The exit code says how complete the export is, so a caller does not have to
open the database to find out. It is the same rule that writes
`db_info.analysis_status`, so the two can never disagree.

| | |
|---|---|
| `0` | Database written, `analysis_status = 'complete'`. |
| `3` | Database written, `'partial'` — elaboration errors, or a gap one of the seal's counts names. |
| `4` | Database written, `'hierarchy_only'` — the compilation is fatally errored, so there is a tree and no dataflow. |
| `2` | No database: the invocation or the sources were unusable (bad option, unreadable filelist, `--top` did not elaborate). |
| `1` | No database: the export itself failed (the file could not be replaced, or an unexpected error). |

## Output

One SQLite file. No runtime, no library, no simulator — anything that speaks SQL
can read it.

| Group | Tables |
|---|---|
| Hierarchy | `module` (the source definition), `tree_node` (the elaborated tree, one id space), `inst` (each module instance occurrence, with its parameter signature) + `inst_param` (that signature made queryable), `prim` (gates, switches, UDPs) |
| Objects | `net` (every connectable object of every instance, implicit nets flagged), `term` + `term_map` (each instance's terminals and what they stand for inside) |
| Dataflow | `net_dep` — net-to-net dependencies, one row per statement occurrence, each naming the operand, target, level, call or primitive it came from; `proc`, `stmt`, `stmt_target`, `assign_operand`, `expr_ref`, `proc_event` — the statement layer those rows point into |
| Gating | `branch` — the level a statement sits under, as a tree; `branch_ref` (what the level's condition reads) + `branch_label` (a case arm's labels, evaluated) + `branch_ancestor` (the tree's closure, so "everything gating this" is one join) |
| Boundaries | `net_conn` — what the parent wired to each terminal, segment by segment with bit windows; `hier_ref` — references that leave an instance, as written *and* resolved to the net where slang could |
| Provenance | `src_file` (every file slang read, with its SHA-256), `db_info` (the seal: schema version, tool, top, status and counts — one typed row) |

The model is **instance-level**: rows hang off the elaborated occurrence, so
"who drives bit 3 of *this* instance's `q`" is one indexed lookup and a fan-in
cone is a recursive query — the folded model this replaced could answer
neither without application-side path algebra. Thirty-two copies of one core
are thirty-two row sets, stamped from one analysis; the measured cost on a
real SoC is about 2× the folded file size.

**[doc/designdb-schema.md](doc/designdb-schema.md) is the field reference** —
every table and column, the bit-range encoding, the naming rules, what the
schema deliberately does not record, and the known limits. Consumers start at
its **stable query interface**: eighteen views (`v_tree_node`, `v_net`,
`v_driver`, `v_load`, `v_stmt`, …) whose columns, NULL rules and row
granularity are the versioned contract.

## Measurements

Release build, macOS arm64, against public designs, schema v20:

| design | definitions | instances | nets | statements | dependencies | time | database |
|---|---:|---:|---:|---:|---:|---:|---:|
| picorv32 | 1 | 1 | 225 | 744 | 3,385 | 0.02 s | 0.98 MB |
| tinyriscv | 26 | 43 | 870 | 1,543 | 5,355 | 0.03 s | 1.53 MB |
| VeeRwolf (`veerwolf_core`) | 91 | 1,925 | 17,808 | 11,083 | 36,414 | 0.22 s | 11.4 MB |

The instance-level expansion is the column to watch: VeeRwolf's 1,925
occurrences stamp out from 164 parameterised bodies (10× replication), and
the database lands at roughly **2× the folded v9 file** rather than 10× —
type text stays interned, and the biggest tables scale with statements, not
with statements times fan-out. `scripts/export-real-designs.sh` reproduces
this table against a local checkout of the designs.

The shape holds well past these: a 320k-line design elaborating to 145k
instances and 4.8M rows exports in about 4 seconds and 324 MB. `--time-report`
breaks that down — roughly a quarter in slang, a sixth in the walk, and the
rest inside SQLite writing rows and building indexes.

Elaboration cost is slang's: memory scales with the number of elaborated
instances, so a very large flat design wants `--top` on a subtree.

## Building

Requires CMake 3.20+ and a C++20 compiler. slang is fetched and pinned by CPM;
SQLite is the official amalgamation, fetched and compiled in, so the binary
carries its own copy and the database does not depend on the host's libsqlite3.
`-DDESIGNDB_SYSTEM_SQLITE=ON` links the system library instead.

### Release binaries

CI builds four platform binaries on every push, and a `v*` tag publishes them
as a GitHub release alongside a `sha256sums.txt`:

| target | binary | linking |
|---|---|---|
| linux-amd64 | `rtl-designdb-linux-amd64` | musl, fully static — any distro, any glibc |
| linux-arm64 | `rtl-designdb-linux-arm64` | musl, fully static |
| windows-amd64 | `rtl-designdb-windows-amd64.exe` | MSVC, static CRT — no DLLs required |
| macos-arm64 | `rtl-designdb-macos-arm64` | native, macOS 11+ (Apple Silicon) |

The set is [rwave](https://github.com/neveltyc/RWaveAnalyzer)'s platform
selection, deliberately: the database is read next to a waveform, so the
exporter ships everywhere the viewer does. rwave keeps its linux-amd64
glibc-dynamic because its vendor waveform backends arrive by `dlopen`; this
exporter has no `dlopen` at all — SQLite is compiled in with loadable
extensions omitted — so both Linux targets are fully static and run unchanged
on the CentOS 7-era farms EDA tools live on.

[scripts/build-release.sh](scripts/build-release.sh) builds the same binaries
locally: macOS builds its own target natively, the Linux pair build through a
Docker Alpine container, and windows-amd64 needs a Windows host, which CI
provides.

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
examples/constructs/    one fixture per LRM construct family, each exported
                        and asserted by CI under its own verifier mode:
                        expressions and assignments, procedural statements and
                        subroutines, modules ports and generate,
                        parameterisation, hierarchical names, packages,
                        primitives, interfaces, assertions, and the
                        deliberately invalid -- a missing definition, an
                        unnamed instantiation, a module that instantiates
                        itself. A family that needs a lint waiver gets a file
                        of its own, because the waiver covers a whole file
examples/options/       not a construct fixture: two files, two tops, a macro
                        defined in one and used in the other, and a header
                        reachable only through +incdir+ -- so --single-unit,
                        +define+ and the config digest have something to be
                        wrong about. Exported and asserted by CI
examples/reorder/       also not a construct fixture: five files in reverse
                        alphabetical order, the one example long enough to
                        reach slang's threaded source loader and so the only
                        one that can catch src_file ids following the buffers
scripts/                build-release.sh (the four release platforms),
                        verify-designdb.py (read an export back, fail if hollow
                        or malformed; --list-modes names the fixture set,
                        --domain-coverage checks the corpus against it),
                        designdb-coverage.py (what an export had to approximate),
                        export-real-designs.sh (the measurements table, from a
                        local checkout of the public designs),
                        check-reproducible.py (export one design twice, fail if
                        any row differs),
                        diff-designdb.py (export one corpus with two binaries,
                        fail on any row diff not declared expected -- the
                        migration gate for internal refactors),
                        check-rtl.sh (validate RTL against Verilator and Icarus)
```

[CI](.github/workflows/ci.yml) builds both SQLite configurations, exports the
whole of `examples/` and reads each database back — a build that links proves
the slang pin resolves, not that the exporter still writes rows. It then checks
the corpus against the schema:
every value the published domains name, and every word in the four view
vocabularies, has to be produced by some fixture, so the test set is driven by
the contract rather than by whichever constructs happened to break once. The
same push builds all four release binaries
([binaries.yml](.github/workflows/binaries.yml)) and repeats the export sweep
on each platform — the Linux pair builds inside an Alpine container and then
runs on the bare glibc runner, so a dynamic dependency that crept into the
"static" binary fails in CI rather than on a farm.
[release.yml](.github/workflows/release.yml) ships exactly that pipeline's
output when a `v*` tag is pushed.

## Testing RTL

Every fixture passes `scripts/check-rtl.sh <file.sv> [top]`, which anything
used as a test case should pass first. It runs the file past Verilator and
Icarus. They disagree in both
directions — Verilator accepts a continuous assign with a variable index that
Icarus correctly rejects, Icarus rejects an unpacked array slice that Verilator
correctly accepts — so disagreement is a prompt to read the LRM, not a verdict.
Several "defects" during development turned out to be invalid RTL written by
hand.

A front end that has not implemented a construct at all says nothing about the
RTL, so a file may declare that one of them cannot accept it:

```
// check-rtl: expect-fail icarus -- interface ports are not in its grammar
```

The declared failure then counts as a pass, and the tool *accepting* the file
counts as a failure — so the marker cannot outlive the limitation it records.
Both directions being checked is also why this stays a developer gate rather
than a CI one: the marker set is only well defined against one pinned pair of
front ends, and the versions here track Homebrew's. Ubuntu 24.04's Verilator
5.020 rejects four files this set does not waive.

A fixture that is deliberately invalid RTL declares both, for the same reason
read the other way: `recursion.sv` and `incomplete.sv` MUST be rejected, and a
front end that starts accepting one is reported as stale rather than quietly
waived. The marker waives a whole file, which is why a construct family that
needs one lives in a file of its own.

## Licence

BSD 3-Clause. See [LICENSE](LICENSE).
