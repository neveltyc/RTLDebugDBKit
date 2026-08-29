# Changelog

All notable changes to this project are documented here. The format is loosely
based on [Keep a Changelog](https://keepachangelog.com/). The tool uses
[Semantic Versioning](https://semver.org/); the database contract consumers read
carries its own integer, `db_info.schema_version`, which moves independently.

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
