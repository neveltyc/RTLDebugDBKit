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
| `--timing` | Report how long each phase took. |
| `--check-constraints` | Keep the enum-domain `CHECK` clauses in the schema, so a bad value is refused as it is written. Off by default: a string `IN`-list is evaluated per row and costs more than the rest of the insert put together, while `verify-designdb.py` derives the same domains from the finished file. |
| `-q` | Only report problems. |

Bare paths are taken as source files.

## Output

One SQLite file. No runtime, no library, no simulator — anything that speaks SQL
can read it.

| Group | Tables |
|---|---|
| Hierarchy | `module` (the source definition), `tree_node` (the elaborated tree, one id space), `inst` (each module instance occurrence, with its parameter signature), `primitive` (gates, switches, UDPs) |
| Objects | `net` (every connectable object of every instance, implicit nets flagged), `term` + `term_map` (each instance's terminals and what they stand for inside) |
| Dataflow | `net_dep` — net-to-net dependencies, one row per statement occurrence, each naming the operand, target, condition, call or primitive it came from; `procedure`, `stmt`, `assign_target`, `assign_operand`, `expr_ref`, `proc_event` — the statement layer those rows point into |
| Boundaries | `net_conn` — what the parent wired to each terminal, segment by segment with bit windows; `hier_ref` — references that leave an instance, as written *and* resolved to the target instance and net where slang could |
| Provenance | `src_file` (every file slang read, with its SHA-256), `meta` (schema version, tool, top) |

The model is **instance-level**: rows hang off the elaborated occurrence, so
"who drives bit 3 of *this* instance's `q`" is one indexed lookup and a fan-in
cone is a recursive query — the folded model this replaced could answer
neither without application-side path algebra. Thirty-two copies of one core
are thirty-two row sets, stamped from one analysis; the measured cost on a
real SoC is about 2× the folded file size.

**[doc/designdb-schema.md](doc/designdb-schema.md) is the field reference** —
every table and column, the bit-range encoding, the naming rules, what the
schema deliberately does not record, and the known limits. Consumers start at
its **stable query interface**: thirteen views (`v_tree_node`, `v_net`,
`v_driver`, `v_load`, `v_stmt`, …) whose columns, NULL rules and row
granularity are the versioned contract.

## Measurements

Release build, macOS arm64, against public designs, schema v12:

| design | definitions | instances | nets | statements | dependencies | time | database |
|---|---:|---:|---:|---:|---:|---:|---:|
| picorv32 | 1 | 1 | 225 | 746 | 4,888 | 0.03 s | 1.07 MB |
| tinyriscv | 26 | 43 | 870 | 1,543 | 5,355 | 0.04 s | 1.35 MB |
| VeeRwolf (`veerwolf_core`) | 86 | 1,920 | 17,808 | 11,088 | 36,621 | 0.34 s | 10.7 MB |

The instance-level expansion is the column to watch: VeeRwolf's 1,920
occurrences stamp out from 164 parameterised bodies (10× replication), and
the database lands at roughly **2× the folded v9 file** rather than 10× —
type text stays interned, and the biggest tables scale with statements, not
with statements times fan-out. `scripts/export-real-designs.sh` reproduces
this table against a local checkout of the designs.

The shape holds well past these: a 320k-line design elaborating to 145k
instances and 4.8M rows exports in about 4 seconds and 324 MB. `--timing`
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
src/                    main.cpp (CLI + filelist parsing), Extractor, DesignDb
doc/designdb-schema.md  the field reference
examples/basic/         RTL small enough to read, exported by CI
examples/constructs/    self-feedback, primitives, UDPs, waits, delays,
                        cross-instance references, per-call-site tasks,
                        level-sensitive events, interfaces, assertions,
                        generate arrays, non-ANSI ports, net aliases, a
                        deliberate black box -- exported and asserted by CI
scripts/                build-release.sh (the four release platforms),
                        verify-designdb.py (read an export back, fail if hollow),
                        designdb-coverage.py (what an export had to approximate),
                        export-real-designs.sh (the measurements table, from a
                        local checkout of the public designs),
                        check-rtl.sh (validate RTL against Verilator and Icarus)
```

[CI](.github/workflows/ci.yml) builds both SQLite configurations on every push,
exports `examples/basic/top.sv`, and reads the database back — a build that
links proves the slang pin resolves, not that the exporter still writes rows.
The same push builds all four release binaries
([binaries.yml](.github/workflows/binaries.yml)) and repeats the export on
each platform — the Linux pair builds inside an Alpine container and then
runs on the bare glibc runner, so a dynamic dependency that crept into the
"static" binary fails in CI rather than on a farm.
[release.yml](.github/workflows/release.yml) ships exactly that pipeline's
output when a `v*` tag is pushed.

## Testing RTL

Anything used as a test case should pass `scripts/check-rtl.sh <file.sv> [top]`
first, which runs it past Verilator and Icarus. They disagree in both
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
`examples/constructs/interfaces.sv` is the only file that carries one.

## Licence

BSD 3-Clause. See [LICENSE](LICENSE).
