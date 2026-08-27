<p align="center">
  <h1 align="center">RTLDebugDBKit</h1>
  <p align="center">
    把 SystemVerilog 设计精化成一个可查询的 <b>SQLite</b> 数据库 &mdash;
    层次、声明、以及<b>什么驱动了什么</b>。<br>
    单文件二进制，不需要仿真器，任何会说 SQL 的东西都能读。
  </p>
</p>

<p align="center">
  <img alt="version" src="https://img.shields.io/badge/version-0.1.0-3366cc?style=flat-square">
  <img alt="schema" src="https://img.shields.io/badge/schema-v20-3366cc?style=flat-square">
  <img alt="slang" src="https://img.shields.io/badge/slang-v11.0-3366cc?style=flat-square">
  <img alt="license" src="https://img.shields.io/badge/license-BSD--3--Clause-3366cc?style=flat-square">
</p>

<p align="center">
  <b>简体中文</b> · <a href="README_en.md">English</a>
</p>

---

## 为什么需要它

波形里有值，没有连接关系。手上一个跑了一夜的回归 FST，你想知道 `veer.dec` 里那个
`mhpmc_inc_e4` 到底被谁驱动、驱动它的那条语句又被什么条件门控住——波形回答不了，它
只知道这一拍是 0 还是 1。

商用的设计数据库是有的，Verdi 的 KDB、Questa 的 `.dbg`，但它们绑定各自的工具链和平台。
这个工具从**源码**产出同一类东西，中间没有仿真器：

```sh
rtl-designdb -f rtl.f --top my_core -o design.db
```

出来的是一个普通 SQLite 文件。没有运行时、没有库、没有私有格式，Python、DuckDB、
`sqlite3` 命令行、任何 ORM 都能直接打开。

## 快速开始

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
build/rtl-designdb examples/basic/top.sv --top top -o design.db
```

```
design.db: 5 modules, 10 instances, 37 nets, 35 terminals, 29 connections, 5 statements, 7 dependencies
```

**谁驱动了这个信号**

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

`u_dp0` 和 `u_dp1` 是同一个模块的两个实例，各自有自己的行——这就是实例级模型：
问题问的是「**这个**实例的 `q`」，不是「`reg` 这个模块的 `q`」。


下面几条是在 VeeRwolf（1,925 个实例）上跑的，展示这个模型真正的用处。

**带完整层次路径找信号**

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

**扇入锥：一条递归查询，不需要应用层走层次**

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

四层锥子，20 毫秒。**这条查询是实例级模型换来的**——折叠模型下它需要应用层自己做路径
代数，因为一个模块体对应 N 个实例。

**一条语句被什么条件门控住**

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

`branch_ancestor` 是门控树的闭包，所以「这条语句头上压着的全部条件」是一次 join，
不是沿 `parent_branch_id` 往上爬。

## 安装

仓库目前是私有的，还没有打过 tag，所以 GitHub Release 页面是空的。要么从源码构建
（见下），要么等第一个 `v*` tag——`release.yml` 会在那时把四个平台的二进制连同
`sha256sums.txt` 一起发布：

| 平台 | 二进制 | 链接方式 |
|:--|:--|:--|
| Linux x86-64 | `rtl-designdb-linux-amd64` | musl，完全静态 |
| Linux ARM64 | `rtl-designdb-linux-arm64` | musl，完全静态 |
| Windows x86-64 | `rtl-designdb-windows-amd64.exe` | MSVC，静态 CRT |
| macOS (Apple Silicon) | `rtl-designdb-macos-arm64` | 原生 |

这套平台组合和 [rwave](https://github.com/neveltyc/RWaveAnalyzer) 一致，是有意的：
这个数据库是配着波形读的，波形工具去哪它就得去哪。rwave 的 linux-amd64 保持 glibc
动态链接（它的厂商后端要 `dlopen`），而这个导出器**完全没有 `dlopen`**——SQLite 编
译进来时关掉了可加载扩展——所以两个 Linux 目标都是 musl 全静态，一个文件，任何发行版、
任何 glibc 都能跑，包括 EDA 工具常年待着的 CentOS 7 那一代机器。

## 从源码构建

需要 CMake 3.20+ 和 C++20 编译器。slang 由 CPM 拉取并**固定在 `v11.0`**，SQLite 用官
方 amalgamation 编译进二进制，所以产物自带一份，不依赖宿主的 libsqlite3。

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

唯一的开关是 `-DDESIGNDB_SYSTEM_SQLITE=ON`，改成链接系统库（要求 ≥ 3.37，因为
`db_info` 是 STRICT 表）。

slang 固定在确切版本而不是范围，是因为这个导出器建在 slang 不保证跨版本稳定的 API 上
（`ValuePath` 的最长静态前缀边界、`AnalysisManager` 的已分析域和过程、
`HierarchicalReference` 的解析路径、规范化实例体），v10 到 v11 直接改坏过好几个。升级
是一次有意的、带差分测试的动作，不是浮动依赖。

[scripts/build-release.sh](scripts/build-release.sh) 在本地构建那四个发布产物：macOS
原生构建自己那个目标，两个 Linux 目标走 Docker Alpine 容器，windows-amd64 需要
Windows 宿主（CI 提供）。

## 输入

刻意做小。它接受 filelist 和 define，像 `vcs -f` 那样，别的都不管——配置、工程布局、
输出格式属于调用它的那一层。

| | |
|---|---|
| `-f`, `-file <file>` | VCS 风格 filelist。认 `+define+`、`+incdir+`、嵌套 `-f`/`-file`、`-v` 库文件，以及环境变量 `$VAR`/`${VAR}`。相对路径按 filelist 自己所在目录解析。 |
| `+define+A=B` | 预处理宏，像 VCS 一样用 `+` 串联。 |
| `+incdir+<dir>`, `-I <dir>` | 包含目录。两种拼写等价。 |
| `--top <module>` | 顶层模块。不给的话 slang 会把每个未被实例化的模块都当顶层。 |
| `--single-unit` | 整个列表编译成一个编译单元，让开头的 `` `define `` 头文件能作用到后面每个文件。VCS 和 Verilator 就是这个行为，slang 默认每文件一个单元。把配置放在头文件里的设计需要它。 |
| `-o <file.db>` | 输出数据库。 |
| `--diag [N]` | 打印精化诊断——全部，或者前 N 条。 |
| `--log <file>` | 把精化日志写到这里，而不是数据库旁边。相对路径按 cwd 解析，和 `-o` 一样。 |
| `--nolog` | 不写精化日志。 |
| `--time-report` | 报告每个阶段耗时。 |
| `--quiet` | 只报告问题。 |

裸路径被当作源文件。

## 日志与退出码

每次运行都写一份精化日志——默认是数据库旁边的 `rtldbgdb-elab.log`，除非 `--log` 指定
了别的路径。里面装着 slang 的全部诊断（不管终端上 `--diag` 显示了多少）和这个工具自己
的发现。`--quiet` 让终端安静，不影响日志；`--nolog` 把文件关掉，同时给 `--log` 路径和
`--nolog` 会被拒绝。

日志的每一行都以固定列打头，写出来就是为了被 grep，而不是被人翻：

```
slang        error|   in instance: soc.u_core
slang        error: rtl/fifo.sv:39:5: error: unknown module 'ghost'
slang        error|     ghost #(.MODE(2)) u_g (.clk(clk), .req(req));
slang        error|     ^~~~~
rtl-designdb note: 2 instantiation(s) name a module that could not be resolved
rtl-designdb note: wrote design.db (partial)
```

```sh
grep -c '^slang  *error:' rtldbgdb-elab.log   # 多少条错误（按条，不按行）
grep '^slang  *error'  rtldbgdb-elab.log      # 每条错误连同源码片段
grep '^rtl-designdb'   rtldbgdb-elab.log      # 导出器自己发现了什么
```

调用方**不需要打开数据库**就能知道这次导出有多完整——退出码和 `db_info.analysis_status`
出自同一次判断，两者永远不会互相矛盾：

| | |
|---|---|
| `0` | 写出了数据库，`analysis_status = 'complete'`。 |
| `3` | 写出了数据库，`'partial'`——有精化错误，或者印章里某个计数指出了缺口。 |
| `4` | 写出了数据库，`'hierarchy_only'`——编译致命错误，只有树没有数据流。 |
| `2` | 没有数据库：调用方式或源码不可用（选项错误、filelist 读不了、`--top` 没精化出来）。 |
| `1` | 没有数据库：导出本身失败。 |

一个由 `--log` 显式指定、却打不开的日志路径，会以 `2` 停下来——**在导出付出任何代价、
或者覆盖掉上一次的数据库之前**。理由是流水线的下一步通常会去 grep 那个文件，而文件不存
在在那里会被读成「没有错误」。默认路径打不开只警告并继续：数据库才是交付物。

## 输出

一个 SQLite 文件，26 张表，分六组。

| 分组 | 表 |
|---|---|
| 层次 | `module`（源码定义）、`tree_node`（精化后的树，一个 id 空间）、`inst`（精化出的每一个模块实例，带参数签名）+ `inst_param`（把签名变得可查）、`prim`（门、开关、UDP） |
| 对象 | `net`（每个实例的每个可连接对象，隐式网标出来）、`term` + `term_map`（每个实例边界上的端子，以及它们在里面代表什么） |
| 数据流 | `net_dep`——网到网的依赖，每条语句实例一行，各自指明来自哪个操作数、目标、门控层、调用或原语；`proc`、`stmt`、`stmt_target`、`assign_operand`、`expr_ref`、`proc_event`——这些行指向的语句层 |
| 门控 | `branch`——语句所处的条件层，是一棵树；`branch_ref`（这一层的条件读了什么）+ `branch_label`（case 分支的标签，求值后的）+ `branch_ancestor`（树的闭包，所以「压着这条语句的全部条件」是一次 join） |
| 边界 | `net_conn`——父层给每个端子接了什么，逐段带位窗口；`hier_ref`——离开实例的引用，既有原样拼写，也有 slang 能解析时解析到的那个网 |
| 溯源 | `src_file`（slang 读过的每个文件，带 SHA-256）、`db_info`（印章：schema 版本、工具、顶层、状态和各项计数——一行带类型的记录） |

模型是**实例级**的：行挂在精化出的实例上，所以「谁驱动了**这个**实例的 `q` 的第 3
位」是一次索引查找，扇入锥是一条递归查询。被它替代的折叠模型两个都答不了，除非应用层
自己做路径代数。一个核复制三十二份就是三十二组行，从一次分析里盖出来；在真实 SoC 上实测
的代价大约是折叠文件的 2 倍。

**[doc/designdb-schema.md](doc/designdb-schema.md) 是字段参考**——每张表每个列、位范围
编码、命名规则、schema 有意不记录什么、以及已知的限制。消费方从它的**稳定查询接口**开始：
19 个视图（`v_tree_node`、`v_net`、`v_driver`、`v_load`、`v_stmt` …），它们的列、NULL
规则和行粒度是带版本的契约。

**版本即消费契约。** `db_info.schema_version` 目前是 **20**。下游消费的字段或 SQL 不变
就不升版本；一旦变化立即升。变更历史写在 `SchemaVersion` 的注释块里，不写进文档。

## 实测

Release 构建，macOS arm64，公开设计，schema v20：

| 设计 | 定义 | 实例 | 网 | 语句 | 依赖 | 耗时 | 数据库 |
|---|---:|---:|---:|---:|---:|---:|---:|
| picorv32 | 1 | 1 | 225 | 744 | 3,373 | 0.03 s | 0.89 MB |
| tinyriscv | 26 | 43 | 870 | 1,543 | 5,180 | 0.03 s | 1.38 MB |
| VeeRwolf (`veerwolf_core`) | 91 | 1,925 | 17,808 | 11,083 | 36,366 | 0.24 s | 11.0 MB |

要看的是实例级展开这一列：VeeRwolf 的 1,925 个实例是从 **169 个参数化模块体**盖出来的，
约 11 倍复制，而数据库只有 11 MB——类型文本保持 interned，最大的几张表随语句数增长，
不是随语句数乘扇出。`scripts/export-real-designs.sh` 用本地的这几个设计复现这张表。

更大的也守得住：一个 32 万行、精化出 14.5 万个实例、480 万行的设计，大约 4 秒、324 MB。
`--time-report` 能拆开看——大致四分之一在 slang，六分之一在遍历，其余在 SQLite 写行和建
索引里。

精化本身的开销是 slang 的：内存随精化出的实例数增长，所以一个非常大的扁平设计应该用
`--top` 切到子树上。

## 仓库结构

```
CMakeLists.txt          构建；slang 和 SQLite 是拉取的，不是 vendored
src/                    main.cpp（CLI + filelist 解析）、Extractor（两趟导出）、
                        DesignDb（写库）
src/sql/                DDL：Schema、Indexes、Views
src/extract/            导出的分层实现。Ref/SymbolText/Template 是词汇层（纯头文件），
                        SourceLocator、DeclIndex、StatementWalker 是其上的服务，
                        TemplateBuilder(+_Conn) 是第一趟、Stamper 是第二趟，
                        各自藏在一个单函数接口后面
doc/designdb-schema.md  字段参考
examples/basic/         小到能一眼读完的 RTL，CI 会导出它
examples/constructs/    每个 LRM 构造族一个 fixture，CI 各自按自己的验证器模式导出并断言
examples/options/       不是构造 fixture：两个文件、两个顶层、宏在一个文件里定义在另一个
                        里使用、头文件只能通过 +incdir+ 找到——好让 --single-unit、
                        +define+ 和配置摘要有出错的余地
examples/reorder/       也不是构造 fixture：五个反字母序的文件，是唯一长到能触发 slang
                        多线程源加载器的例子，因此也是唯一能抓住 src_file id 跟着缓冲区
                        跑的例子
scripts/                见下
.github/workflows/      ci.yml、binaries.yml（四平台产物）、release.yml（v* tag 发布）
```

| 脚本 | 做什么 | 谁在跑 |
|---|---|---|
| `verify-designdb.py` | 把数据库读回来，空洞或畸形就失败。`--list-modes` 是 fixture 集合的唯一命名处，`--domain-coverage` 检查整个语料是否覆盖了全部已发布的枚举值 | CI |
| `check-reproducible.py` | 同一个二进制导两遍，逐行比对 | CI |
| `build-release.sh` | 四个平台的发布二进制 | CI |
| `diff-designdb.py` | 两个二进制导同一批语料，逐行比对，未申报的差异即失败——重构的迁移门 | 本地 |
| `designdb-coverage.py` | 这次导出有多少东西是近似的（不判通过失败） | 本地 |
| `export-real-designs.sh` | 用本地的公开设计复现上面那张实测表 | 本地 |
| `check-rtl.sh` | 用 Verilator 和 Icarus 双前端验证 RTL 合法性 | 本地 |

## 测试

```sh
# 导出并读回，一个构造族一遍（CI 的循环）
for mode in $(python3 scripts/verify-designdb.py --list-modes); do
  build/rtl-designdb "examples/constructs/$mode.sv" -o "$mode.db"
  python3 scripts/verify-designdb.py "$mode.db" "$mode"
done

# 整个语料是否覆盖了 schema 公布的每一个值
python3 scripts/verify-designdb.py --domain-coverage ./*.db
```

[CI](.github/workflows/ci.yml) 构建两种 SQLite 配置，导出整个 `examples/` 并把每个数据库
读回来——能链接只说明 slang 的固定版本解析得了，不说明导出器还在写行。然后拿语料对着
schema 检查：**已发布的枚举域里的每个值、四套视图词汇里的每个词，都必须由某个 fixture
真正产出**。所以测试集合是被契约驱动的，而不是被「哪个构造曾经坏过」驱动的。同一次
push 还会构建四个发布二进制（[binaries.yml](.github/workflows/binaries.yml)），并在每个
平台上重跑导出——Linux 那一对在 Alpine 容器里构建、然后在裸 glibc runner 上运行，所以
混进「静态」二进制里的动态依赖会在 CI 阶段暴露，而不是等到机房里才发现。

### 测试用的 RTL

任何拿来当测试用例的 RTL，都应该先过 `scripts/check-rtl.sh <file.sv> [top]`。它把文件
分别喂给 Verilator 和 Icarus。两者会**双向**分歧——Verilator 接受一个带变量下标的连续
赋值而 Icarus 正确地拒绝，Icarus 拒绝一个 Verilator 正确接受的非压缩数组切片——所以分歧
是「去读标准」的提示，不是判决。开发过程中好几个「缺陷」最后查出来是手写的非法 RTL。

一个前端如果压根没实现某个构造，那它说明不了 RTL 的任何问题，所以文件可以声明其中之一
无法接受它：

```
// check-rtl: expect-fail icarus -- interface ports are not in its grammar
```

声明了的失败算通过，而工具**接受**该文件反而算失败——所以这个标记不会活得比它记录的那个
限制更久。双向都检查也正是它留在本地而不进 CI 的原因：标记集合只对一组固定版本的前端有
确切含义，而这里的版本跟着 Homebrew 走。Ubuntu 24.04 的 Verilator 5.020 会拒绝四个这套
标记没有豁免的文件。

## 给消费方和 AI agent

数据库自己带着**印章**：`db_info` 一行，说明 schema 版本、工具版本、slang 版本、产出它
的那次 commit、顶层、分析状态和各项缺口计数。所以拿到一个陌生的 `.db`，第一条查询永远是：

```sh
sqlite3 -box design.db "SELECT * FROM v_db_info"
```

对 agent 来说要点有三条：

1. **退出码就是契约**，不用打开文件就能分支（0 / 3 / 4 都写出了数据库）。
2. **只查 `v_` 视图**。列、NULL 规则、行粒度是带版本的契约；基表不是。
3. **诊断在日志里**，`grep '^slang  *error:' rtldbgdb-elab.log` 一条一行，各自点名文件
   和错因。不需要为了看错误重跑导出。

## 变更记录

每个版本的变更记在 [CHANGELOG.md](CHANGELOG.md)。工具版本用语义化版本，数据库契约另有
一个整数 `db_info.schema_version`，两者独立推进。

## 许可证

BSD 3-Clause，见 [LICENSE](LICENSE)。slang 和 SQLite 在构建时拉取，各自保留自己的许可证。
