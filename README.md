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
design.db: 25 modules, 32 instances, 1703 symbols, 5723 edges, 31 children, 652 ports
```

Try it against the RTL in this repo:

```sh
build/rtl-designdb examples/basic/top.sv --top top -o design.db
sqlite3 design.db "SELECT n.text, e.construct FROM edge e JOIN name n ON n.id = e.dst"
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
| `--diag [N]` | Print the first N elaboration diagnostics. |
| `-q` | Only report problems. |

Bare paths are taken as source files.

## Output

One SQLite file. No runtime, no library, no simulator — anything that speaks SQL
can read it.

| Group | Tables |
|---|---|
| Hierarchy | `module` (a definition plus the parameter values it elaborated with), `instance` (the tree), `child` (what a module instantiates) |
| Declarations | `symbol` — every declaration with kind, type, width, direction, and `file:line:col` |
| Dataflow | `edge` (what drives what, with bit ranges), `assignment` + `assign_operand` (per statement), `proc_event` (procedure sensitivity) |
| Boundaries | `port` — port connections, the rows that let a trace leave the module it started in |
| Provenance | `source_file` (every file slang read, with its SHA-256), `meta` (schema version, tool, top) |

The model is **folded**: rows hang off a *module*, not an instance, so thirty-two
copies of one core share one set of edges and `instance` is the only table that
grows with the design.

**[doc/designdb-schema.md](doc/designdb-schema.md) is the field reference** —
every table and column, the bit-range encoding, the naming rules, what the
schema deliberately does not record, and the known limits.

## Measurements

Release build, macOS arm64, against public designs:

| design | modules | instances | symbols | edges | ports | time | database |
|---|---:|---:|---:|---:|---:|---:|---:|
| picorv32 | 1 | 1 | 269 | 4,378 | 0 | 0.03 s | 0.53 MB |
| tinyriscv | 29 | 43 | 877 | 4,755 | 470 | 0.02 s | 0.69 MB |
| Ibex (`ibex_core`) | 25 | 32 | 1,703 | 5,723 | 652 | 0.04 s | 0.99 MB |
| VeeRwolf (`veerwolf_core`) | 220 | 3,950 | 10,342 | 24,589 | 15,780 | 0.19 s | 5.35 MB |

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
scripts/                build-release.sh (the four release platforms),
                        verify-designdb.py (read an export back, fail if hollow),
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

## Licence

BSD 3-Clause. See [LICENSE](LICENSE).
