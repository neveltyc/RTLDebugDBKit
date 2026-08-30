# RTLDebugDBKit

> **SystemVerilog → elaborate → SQLite**

<p>
  <img alt="Release" src="https://img.shields.io/github/v/release/neveltyc/RTLDebugDBKit?sort=semver&style=flat-square&color=3366cc">
  <img alt="CI" src="https://img.shields.io/github/actions/workflow/status/neveltyc/RTLDebugDBKit/ci.yml?branch=main&style=flat-square&label=CI">
  <img alt="schema" src="https://img.shields.io/badge/schema-v21-3366cc?style=flat-square">
  <img alt="slang" src="https://img.shields.io/badge/slang-v11.0-3366cc?style=flat-square">
  <img alt="license" src="https://img.shields.io/badge/license-BSD--3--Clause-3366cc?style=flat-square">
</p>

**简体中文** · [English](README_en.md)

RTLDebugDBKit 用 slang elaborate SystemVerilog，把设计层次、net、端口连接、RTL 语句、
控制条件和静态数据依赖写入 SQLite。

它主要用来查这类问题：

* 一个信号由谁驱动？
* 一个信号被哪些逻辑使用？
* driver 对应哪条 RTL？
* 一条赋值受哪些 `if` / `case` / loop 控制？
* 一个信号的上下游依赖有哪些？
* module / interface 边界两侧怎么连接？

> [!NOTE]
> 数据库记录 elaborate 后的设计结构和静态关系，不包含波形值、仿真时间和运行时状态。

---

## 安装

从 [Releases](https://github.com/neveltyc/RTLDebugDBKit/releases/latest) 下载对应平台的二进制。

```bash
# Linux
chmod +x rtl-designdb-linux-amd64
./rtl-designdb-linux-amd64 --version
```

---

## 命令行

```text
rtl-designdb [options] <source...>
```

| 参数 | 说明 |
| --- | --- |
| `-f`, `-file <file>` | VCS 风格 filelist |
| `+define+A=B` | 定义宏 |
| `+incdir+<dir>` | include 目录 |
| `-I <dir>` | include 目录 |
| `--top <module>` | 指定 top |
| `--single-unit` | 单 compilation unit |
| `-o <file.db>` | 输出数据库 |
| `--diag [N]` | 显示诊断 |
| `--log <file>` | 指定日志文件 |
| `--nolog` | 不写日志 |
| `--time-report` | 显示各阶段耗时 |
| `--quiet` | 减少输出 |
| `--version` | 显示版本 |
| `-h`, `--help` | 显示帮助 |

filelist 支持：
`+define+` · `+incdir+` · `-f` · `-file` · `-v` · `$VAR` · `${VAR}`

相对路径以 filelist 所在目录为基准。

---

## 生成数据库

```bash
# 从 filelist elaborate 顶层并生成 design.db
rtl-designdb -f rtl.f --top veerwolf_core -o design.db
```

查看数据库状态：

```bash
sqlite3 -box design.db "SELECT * FROM v_db_info;"
```

`analysis_status`：

| 状态 | 含义 |
| --- | --- |
| `complete` | elaborate 和数据导出完成 |
| `partial` | 数据库可用，但存在分析缺口 |
| `hierarchy_only` | 只保留层次信息 |

> [!TIP]
> 有分发或归档 `design.db` 的需求时，推荐用 `zstd` 或 `xz` 压缩：SQLite 文件冗余度高，
> 通常可压缩数倍（zstd 约 5–7×、xz 约 12×），显著节省存储空间，用前解压一次即可。

---

## 数据库

当前 schema version：**21**

RTLDebugDBKit 保存的是 **instance-level design**。

```systemverilog
reg_block u_reg0 (...);
reg_block u_reg1 (...);
```

即使两个 instance 来自同一个 module definition，数据库里也会分别记录：

```text
top.u_reg0.q
top.u_reg1.q
```

driver、load、statement 和 dependency 都落在具体 instance 上。

### 主要对象

| 类别 | 表 / View | 内容 |
| --- | --- | --- |
| Metadata | `v_db_info` | schema version、分析状态、缺口统计 |
| Hierarchy | `module` | module definition |
| Hierarchy | `v_tree_node` | instance、generate、primitive 等节点 |
| Hierarchy | `v_node_path` | 完整 hierarchical path |
| Object | `v_net` | net / variable |
| Object | `v_term` | terminal |
| Object | `v_term_map` | terminal 与实例内部对象的映射 |
| Connection | `v_net_conn` | instance boundary 连接 |
| Trace graph | `v_trace_edge` | 规范化的一跳 net → net 依赖 |
| Dataflow | `v_driver` | net 的直接 driver |
| Dataflow | `v_load` | net 的直接 load |
| Dataflow | `v_net_dep` / `net_dep` | net → net 静态依赖 |
| Dataflow | `v_net_attachment` | net 上关联的其他关系 |
| Statement | `v_stmt` | RTL statement 和源码位置 |
| Statement | `v_stmt_target` | statement 写入对象 |
| Statement | `v_stmt_operand` | statement 读取对象 |
| Process | `v_proc_event` | event control |
| Control | `v_branch` | `if` / `case` / loop |
| Control | `branch_ref` | branch condition 读取的 net |
| Control | `branch_ancestor` | branch 嵌套关系 |
| Reference | `v_hier_ref` | hierarchical reference |

完整定义见 [`doc/designdb-schema.md`](doc/designdb-schema.md)。

### 常见字段

| 字段 | 含义 |
| --- | --- |
| `*_id` | 对象 ID |
| `inst_id` | 所属 instance |
| `net_id` | net ID |
| `stmt_id` | statement ID |
| `branch_id` | branch ID |
| `src_path` | 源文件 |
| `src_line`, `src_col` | 源码位置 |
| `width` | 位宽 |
| `*_kind` | 对象或关系类型 |
| `*_exact` | range / mapping 是否精确 |
| `edge_kind` | 一跳依赖的 RTL 语义 |
| `map_kind` | 跨边 bit 对应是 `exact` 或 `inexact` |

> [!TIP]
> 位范围、NULL、row granularity 和 exactness 的具体语义见 schema 文档。

---

## 数据关系

### 数据流

```systemverilog
assign q = a & b;
```

对应：

```text
a ──▶ q
b ──▶ q
```

动态索引等情况仍会保留依赖，但 bit mapping 可能不是 exact。

```systemverilog
out = mem[raddr];
```

这里可以确定 `mem` 影响 `out`，但不一定能静态确定具体元素。

### 模块边界

跨 module 的连接通过 terminal 保留：

```text
outside net ── terminal ── inside net
```

主要查询入口：
`v_trace_edge` · `v_net_conn` · `v_term_map` · `v_driver` · `v_load`

### 跨层次引用

```systemverilog
assign q = u_core.state;
```

如果 slang 能解析 `u_core.state`，数据库会记录目标 instance 和对象；解析失败时仍会保留
reference 和解析状态。

---

## 查询

常见 trace 路径：

> **path → net → trace edge → statement → branch → fan-in**

| 要查什么 | 查询入口 |
| --- | --- |
| 根据路径找 net | `v_node_path` + `v_net` |
| 查 driver | `v_driver` |
| 查 load | `v_load` |
| 查 RTL 位置 | `v_stmt` |
| 查外层控制条件 | `v_branch` + `branch_ancestor` |
| 查 branch 使用的信号 | `branch_ref` |
| 向上/向下追一跳 | `v_trace_edge` |

<details>
<summary><strong>常用 SQL</strong></summary>

```sql
-- 根据 instance path 和信号名找 net
SELECT n.net_id, p.node_path, n.net_name
FROM v_net n JOIN v_node_path p ON p.node_id = n.inst_id
WHERE p.node_path = 'veerwolf_core.rvtop.veer.dec.tlu'
  AND n.net_name = 'mhpmc_inc_e4';
```

```sql
-- 查直接 driver
SELECT driver_kind, driver_name, stmt_id, src_path, src_line
FROM v_driver WHERE signal_net_id = :net_id;
```

```sql
-- 查直接 load
SELECT load_kind, load_name, stmt_id, src_path, src_line
FROM v_load WHERE signal_net_id = :net_id;
```

```sql
-- 查 statement 源码位置
SELECT stmt_kind, src_path, src_line, src_col
FROM v_stmt WHERE stmt_id = :stmt_id;
```

```sql
-- 查 statement 外层 branch
SELECT b.depth, b.branch_kind, b.sense, b.src_path, b.src_line
FROM stmt s
JOIN branch_ancestor a ON a.branch_id = s.branch_id
JOIN v_branch b ON b.branch_id = a.ancestor_branch_id
WHERE s.id = :stmt_id
ORDER BY b.depth;
```

```sql
-- 向上追四层 fan-in
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

## Schema 契约

Schema version：

```text
db_info.schema_version
```

当前 schema v21 有 **19 个公开 `v_*` view**。

常规查询优先使用 view。部分关系直接通过公开基表提供：
`net_dep` · `conn_arc` · `branch_ref` · `branch_ancestor` · `module` · `inst_param` · `expr_ref` · `prim`

> [!WARNING]
> `v_conn_arc` 是内部 view，不属于公开查询接口。

数据库消费端以 [`doc/designdb-schema.md`](doc/designdb-schema.md) 为准。
Schema 变化见 [`doc/schema-history.md`](doc/schema-history.md)。

---

## 从源码构建

要求：

* CMake 3.20+
* C++20 compiler

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

SQLite 默认编译进 `rtl-designdb`。

```bash
# 使用系统 SQLite
cmake -B build -DCMAKE_BUILD_TYPE=Release -DDESIGNDB_SYSTEM_SQLITE=ON
```

系统 SQLite 需要 3.37 或更高版本。

---

## 日志与退出码

默认日志：

```text
rtldbgdb-elab.log
```

| Code | 结果 |
| ---: | --- |
| `0` | `complete` |
| `3` | `partial` |
| `4` | `hierarchy_only` |
| `2` | 输入或参数错误 |
| `1` | 导出失败 |

`0`、`3`、`4` 都会保留数据库。

---

## 性能

Release build，macOS arm64，schema v21：

| 设计 | 定义 | 实例 | 网 | 语句 | 依赖 | Connection arcs | Trace edges | 耗时 | 数据库 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| picorv32 | 1 | 1 | 225 | 744 | 3,373 | 0 | 3,373 | 0.02 s | 0.95 MB |
| tinyriscv | 26 | 43 | 870 | 1,543 | 5,180 | 451 | 5,631 | 0.03 s | 1.51 MB |
| VeeRwolf | 91 | 1,925 | 17,808 | 11,083 | 36,366 | 10,971 | 47,337 | 0.25 s | 12.35 MB |

VeeRwolf 热缓存 SQLite point query，按 id 前 1,000 个 net，5 轮中位数：

| 查询 | Schema v20 | Schema v21 |
| --- | ---: | ---: |
| `net_dep` by source / target | 6.00 / 6.04 µs | 5.63 / 6.09 µs |
| `v_driver` / `v_load` | 24.29 / 25.89 µs | 22.28 / 25.03 µs |
| `conn_arc` by source / destination | — | 3.89 / 3.77 µs |
| `v_trace_edge` by source / destination | — | 8.88 / 9.30 µs |

```bash
# 查看各阶段耗时
rtl-designdb ... --time-report
```

---

## 文档

* [`doc/designdb-schema.md`](doc/designdb-schema.md) — schema、字段和关系
* [`doc/schema-history.md`](doc/schema-history.md) — schema 变更历史
* [`CHANGELOG.md`](CHANGELOG.md) — release 变化
* [`third-party-licenses.md`](third-party-licenses.md) — 第三方许可证

<details>
<summary><strong>仓库结构</strong></summary>

```text
src/                    rtl-designdb
src/sql/                schema / index / views
doc/                    文档
examples/               示例和测试 RTL
scripts/                验证和发布脚本
.github/workflows/      CI / release
```

</details>

---

## 许可证

BSD 3-Clause — [`LICENSE`](LICENSE)
