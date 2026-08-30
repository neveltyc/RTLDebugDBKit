# Changelog

All notable changes to this project are documented here. The format is loosely
based on [Keep a Changelog](https://keepachangelog.com/). The tool uses
[Semantic Versioning](https://semver.org/); the database contract consumers read
carries its own integer, `db_info.schema_version`, which moves independently.

## [Unreleased]

## [0.3.0] — 2026-08-31

### Added

* Schema v22 exposes `map_kind` (`exact`/`inexact`) on `v_driver` and `v_load`,
  the normalized mapping precision previously only on `v_trace_edge`. Its value
  matches `v_trace_edge` for a dependency and the physical `conn_arc` for a
  crossing, and is `inexact` (never NULL) where a row has no second end. A
  consumer that needs per-arc spelling and provenance no longer recomputes
  precision from the raw `map_exact`/`*_exact` flags.
* `v_load` gains `dep_kind`, the dependency's own
  `data`/`control`/`primitive`/`procedure`/`alias` that `load_kind` folds into
  `dataflow`, so distinguishing a datum read from a gate read needs no rejoin to
  `net_dep`.

### Changed

* The public database contract is schema v22: still 27 tables and 19 stable
  views, with the two added columns and unchanged row granularity. The raw
  `src_exact`/`tgt_exact`/`map_exact` and range columns remain. Consumers must
  re-export RTL before reading the schema v22 layout.
* Database verification holds the new columns equal to the graph contract row
  for row: each view's `map_kind` against `v_trace_edge` and `conn_arc`, and
  `v_load.dep_kind` against the dependency's own kind.

## [0.2.0] — 2026-08-30

### Added

* Schema v21 materializes directed `conn_arc` rows from effective
  `net_conn × term_map` overlaps. Every arc keeps both provenance ids, explicit
  conservative ranges, `exact`/`inexact` mapping precision, and indexes on its
  source and destination nets.
* `v_trace_edge` provides one normalized graph contract over `net_dep` and
  `conn_arc`. It separates RTL semantics in `edge_kind` from bit correspondence
  in `map_kind`, preserves statement and source provenance, and supports indexed
  one-hop lookup in either direction without storing a transitive closure.

### Changed

* The public database contract now contains 27 tables and 19 stable views.
  Existing fact tables and the `v_driver` / `v_load` compatibility views remain
  available. Consumers must re-export RTL before reading the schema v21 layout.
* Database verification independently recomputes connection overlap geometry
  from the fact tables, checks materialized-arc equivalence, exact-range
  invariants, concat pairing, dynamic-index semantics, and indexed query plans.
* The field reference, schema history, coverage report, bilingual README, and
  measured generation/query performance describe the schema v21 contract.

## [0.1.0] — 2026-08-28

First release.

`rtl-designdb` elaborates a SystemVerilog design with slang v11.0 and writes it
to one SQLite file — hierarchy, declarations, and net-to-net dataflow — with no
simulator involved and no runtime in the output.

Ships schema v20: 26 tables, and a stable interface of 18 `v_` views whose
columns, NULL rules and row granularity are the versioned contract. Rows hang off
the elaborated instance rather than a folded module. Gating levels form a tree
with a precomputed transitive closure. Every database carries a typed `db_info`
seal, and the process exit code reports the same completeness without opening the
file: `0` complete, `3` partial, `4` hierarchy only, `2` bad input, `1` failed.

Input is a VCS-style filelist. See [README.md](README.md) for the command
surface and [doc/designdb-schema.md](doc/designdb-schema.md) for the field
reference.
