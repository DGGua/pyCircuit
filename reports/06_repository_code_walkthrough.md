# pyCircuit 仓库代码解读：从 Python 源码到 Verilog、C++ 仿真与门禁

> 调研快照：2026-08-04，基于当前工作树。本文不是 API 手册，而是一份“拿到问题后，知道调用链如何走、应从哪个文件开始追”的源码导读。

## 0. 先说结论：这个仓库真正的中心在哪里

当前 pyCircuit 的主干可以压缩成下面这条链：

```text
Python 硬件 DSL / @module
        │
        │ elaboration + AST/JIT 编译
        ▼
PYC dialect 的 MLIR 文本
        │
        │ pycc：解析、验证、lowering、优化、统计
        ▼
合法且较规范的 PYC/MLIR
        ├──────────────────────────────┐
        │                              │
        ▼                              ▼
VerilogEmitter                  CppEmitter
        │                              │
Verilog RTL + primitives        C++ SimObject + runtime
        │                              │
        ├── Verilator 仿真             └── native C++ 仿真
        └── Yosys/Vivado 综合
```

因此，理解这个仓库时最重要的三个判断是：

1. **语义中心是 PYC dialect 和 MLIR passes，不是某一个 Emitter。** 这是 [Decision 0112](../docs/rfcs/pyc4.0-decisions.md#decision-0112-mlir-dialect-is-the-single-semantic-source-c-sim-and-verilog-emission-must-be-logically-equivalent) 的硬约束。
2. **Verilog Emitter 主要负责把已经决定好的硬件结构翻译成 RTL。** 它能影响表达形态，但不能独自决定设计应当有多少寄存器、memory 的读写语义或跨层组合依赖。
3. **Yosys 在当前流程里负责综合和结构统计，不是主要仿真器。** RTL 行为由 Verilator 跑；另一路由生成的 C++ 模型运行。

如果把它类比成软件编译器：

| 软件编译器 | pyCircuit |
|---|---|
| C/C++ 源码 | Python 硬件 DSL |
| AST/语义分析 | `jit.py`、`hw.py`、`v5.py` |
| LLVM IR | PYC dialect on MLIR |
| IR verifier/pass | `PYCOps.cpp`、`Transforms/*.cpp` |
| 汇编/目标代码生成 | Verilog/C++ Emitter |
| CPU 执行 | Verilator 或 native C++ simulator |
| 性能分析 | Yosys/目标 FPGA 或 ASIC 工具的 PPA 报告 |

## 1. 一条命令究竟穿过哪些代码

### 1.1 `pycircuit emit`：只得到前端 IR

Python 包的命令入口由 [pyproject.toml](../pyproject.toml#L56) 注册：

```text
pycircuit = pycircuit.cli:main
```

`pycircuit emit source.py` 的主路径是：

```text
cli.main
  └─ _cmd_emit                         cli.py:164
       ├─ 解析 source.py 和参数
       ├─ 扫描已删除/禁止的旧 API
       ├─ 找到 build 函数
       ├─ jit.compile(build, ...)
       │    ├─ Design(top=...)
       │    ├─ DesignContext.specialize(top)
       │    └─ compile_module(...)
       └─ Design.emit_mlir() → 写出 .pyc
```

对应入口见 [`_cmd_emit`](../compiler/frontend/pycircuit/cli.py#L164) 和 [`compile`](../compiler/frontend/pycircuit/jit.py#L2217)。这个命令还没有运行 `pycc`，所以输出是前端 PYC/MLIR，而不是最终 Verilog。

### 1.2 `pycircuit build`：完整工程构建编排

[`_cmd_build`](../compiler/frontend/pycircuit/cli.py#L2172) 是整个 Python 侧最重的编排函数。它大致做这些事：

```text
加载 source.py / config / testbench
        │
        ├─ API contract 扫描
        ├─ 参数 canonicalization + 源文件/dependency hash
        ├─ JIT/elaboration cache 判断
        ▼
Design + 每个 specialization 的独立 .pyc
        │
        ├─ 先调用 pycc --emit=none 生成/核对 probe catalog
        ├─ 解析 trace/probe/testbench 计划
        ├─ 并行发起每个 module 的 pycc job
        │       ├─ --emit=cpp
        │       └─ --emit=verilog
        ├─ C++：生成 CMake 工程并编译 native simulator
        └─ Verilog：生成 SV testbench，交给 Verilator
```

这里有几个很实用的阅读点：

- [`_detect_pycc`](../compiler/frontend/pycircuit/cli.py#L220) 决定使用环境变量、staged toolchain、构建目录还是 `PATH` 中的 `pycc`。
- [`_emit_multi_pyc_artifacts`](../compiler/frontend/pycircuit/cli.py#L1888) 把一个 `Design` 拆成 module 粒度的 `.pyc` 文件和 project manifest。
- [`_collect_testbench_payload`](../compiler/frontend/pycircuit/cli.py#L1909) 收集 testbench 描述。
- [`_render_tb_cpp`](../compiler/frontend/pycircuit/cli.py#L968) 生成 C++ testbench。
- [`_render_tb_sv`](../compiler/frontend/pycircuit/cli.py#L1449) 生成 SystemVerilog testbench。
- [`_run_backend_job`](../compiler/frontend/pycircuit/cli.py#L1878) 是并行 `pycc` job 的执行包装。

`cli.py` 很大，并不是因为它定义了硬件语义，而是因为它同时承担了 loader、cache、manifest、testbench codegen、并行任务、CMake、Verilator 和 sidecar 的工程编排。

### 1.3 testbench 是一条“旁路”

设备 module 的 `.pyc` 要经过完整 pass pipeline；testbench 则先被 Python 转成 C++/SV 文本，再编码进带有 `pyc.tb.payload` 属性的 MLIR 容器。`pycc` 在识别到这个属性后直接取出文本写文件，见 [`pycc.cpp`](../compiler/mlir/tools/pycc.cpp#L2104)。

因此：

```text
device .pyc ──► dialect verifier / passes ──► Emitter
tb .pyc     ──► 读取 pyc.tb.payload ───────► 原样输出 C++/SV
```

这解释了为什么查 testbench 行为时，除了 `pycc`，更应该看 `cli.py`、`tb.py` 和 `testbench.py`，而不是去 Verilog Emitter 中找。

## 2. 顶层目录地图

| 路径 | 角色 | 重要程度 |
|---|---|---|
| `compiler/frontend/pycircuit/` | Python DSL、AST/JIT、hierarchy、CLI、TB、probe/trace | 核心 |
| `compiler/mlir/include/pyc/Dialect/PYC/` | PYC 类型和 op 的 TableGen 声明 | 语义核心 |
| `compiler/mlir/lib/Dialect/PYC/` | verifier、constant fold、dialect 注册 | 语义核心 |
| `compiler/mlir/lib/Transforms/` | legality gates、结构优化、依赖/深度分析 | 语义与优化核心 |
| `compiler/mlir/lib/Emit/` | Verilog 和 C++ 后端 | 核心 backend |
| `compiler/mlir/tools/pycc.cpp` | production compiler driver | 核心入口 |
| `compiler/mlir/tools/pyc-opt.cpp` | MLIR pass 调试/复现工具 | 开发入口 |
| `runtime/verilog/` | reg/FIFO/memory/CDC 等 RTL primitive | Verilog backend 的组成部分 |
| `runtime/cpp/` | bit-vector、state primitives、TB、trace、probe runtime | C++ simulator 的组成部分 |
| `flows/scripts/` | build、smoke、example、simulation、semantic gates | 验证主入口 |
| `flows/tools/` | manifest、API hygiene、trace dump、可视化等辅助工具 | 工程工具 |
| `tests/vec/` | vector frontend/backend/oracle 测试矩阵 | 当前较系统的 pytest 区域 |
| `designs/examples/` | 小型 litmus 与端到端示例 | 最适合学习/回归 |
| `designs/BypassUnit` 等 | 较真实的设计和生成产物 | 资源分析样本 |
| `iplib/` | 与 `pycircuit.lib` 基本镜像的顶层兼容库；两处实现目前几乎相同 | 次要 |
| `contrib/`、`janus/` | 外部/系统级集成与专用设计 | 需要时再读 |
| `boards/` | FPGA board 说明/落板相关内容 | 非编译器核心 |
| `docs/` | 规范、决策、教程和 gate 证据 | 修改语义前必读 |

`include/cpp/` 里的文件大多是指向 `runtime/cpp/` 的符号链接，作用是为生成代码提供稳定的 include 路径。不要误以为这里还有一份独立 runtime 实现。

## 3. Python 前端：硬件是怎样被“构造”出来的

### 3.1 `__init__.py`：公开 API 的门面

[`compiler/frontend/pycircuit/__init__.py`](../compiler/frontend/pycircuit/__init__.py) 主要重导出：

- `Circuit`、`Wire`、`Reg`、`Vec`、`ClockDomain`；
- `@module`、`@function`、`@const`、`@testbench`、`@probe`；
- `compile` 和 `compile_cycle_aware`；
- connector、record、probe、trace、testbench API。

看到设计文件中的 `from pycircuit import ...` 时，可以先回这里确认公共名字最终来自哪个实现文件。

### 3.2 `dsl.py`：最低层的 PYC 文本构造器

[`Signal`](../compiler/frontend/pycircuit/dsl.py#L126) 很薄，核心只有：

```python
@dataclass(frozen=True)
class Signal:
    ref: str   # 例如 %v3
    ty: str    # 例如 i8 或 vector<8xi16>
```

[`Module`](../compiler/frontend/pycircuit/dsl.py#L134) 保存：

- function 输入/输出；
- 已生成的 MLIR 文本行；
- 临时 SSA 名编号；
- function attributes；
- finalizer；
- 部分 source/provenance 信息。

例如 `m.add(a, b)` 的本质不是立即做整数运算，而是申请一个新 SSA 名并追加一条 `pyc.add` 文本。`m.reg(...)`、`m.instance_op(...)`、`m.sync_mem(...)` 也是同样的模式。最后 [`emit_func_mlir`](../compiler/frontend/pycircuit/dsl.py#L814) 拼出 `func.func`，[`emit_mlir`](../compiler/frontend/pycircuit/dsl.py#L848) 再套上 `module { ... }`。

简化后的形态是：

```python
# Python builder
y = m.add(a, b)
```

```mlir
// 生成的 PYC/MLIR，示意
%v1 = pyc.add %a, %b : i8
```

一个值得明确的架构事实是：**当前 Python 前端主要通过字符串构造 MLIR，而不是使用 MLIR Python bindings 创建 typed op object。** 好处是依赖少、上手直接；代价是：

- 部分错误只能等 C++ MLIR parser/verifier 报出；
- Python 检查与 dialect verifier 之间可能出现覆盖差异；
- 新增 op 时要同步文本语法、TableGen、C++ verifier、两个 Emitter；
- source location 和结构化重写能力不如直接构造 typed IR 自然。

这不是说必须立刻重写前端，而是排查“为什么 Python 接受了，`pycc` 才拒绝”时应记住这层边界。

### 3.3 `hw.py`：大多数硬件作者真正使用的 DSL

[`Wire`](../compiler/frontend/pycircuit/hw.py#L99) 是类型化 signal 包装。它保存底层 `Signal`、owner `Circuit`、signedness、可选 cycle/domain 等信息，并通过运算符重载把：

```python
a + b
a & b
a == b
a[7:0]
```

转换为 `Circuit` 的 builder 调用。`Wire.__bool__` 会拒绝把硬件值当 Python 布尔值，这一点十分重要：

```python
if hardware_wire:       # 不能按普通 Python 条件执行
    ...
```

硬件条件需要 mux、JIT 支持的硬件 `if`，或显式的组合构造。

[`Reg`](../compiler/frontend/pycircuit/hw.py#L484) 是 state 句柄。概念上它持有：

```text
q      当前状态
next   下一状态
clk    更新时钟
rst    reset
en     enable
init   reset value
```

`Reg.set(value, when=...)` 最终驱动为 next-state 预留的 wire/backedge。这样前端允许先拿到 `q`，稍后再补充 next 逻辑，最终在 finalizer 阶段完成 IR。

[`Circuit`](../compiler/frontend/pycircuit/hw.py#L650) 继承底层 `Module`，把低级 `Signal` 提升为 `Wire/Reg/Vec`。它负责：

- 建 input/output/clock/reset；
- 常量、算术、比较、cast、slice、concat；
- state、FIFO、memory、CDC；
- module instance 与 connector；
- 输出命名、probe metadata 和结构信息。

其中 [`Circuit.out`](../compiler/frontend/pycircuit/hw.py#L1015) 很容易让初学者误解。它不是“把 Python 值打印出去”，而是创建一个有 `q/next` 的时序 state。若资源突然增加，搜索所有 `.out(...)`、`domain.signal(...)` 和自动 delay insertion 往往比先看 Verilog 更有效。

[`Vec`](../compiler/frontend/pycircuit/hw.py#L2151) 表达规则向量/多维向量。它可以生成原生 vector op，也可以被 `VectorUnrollPass` 展开。资源相关的重要 API 包括：

- element-wise 算术/比较：通常复制到每个 lane；
- `broadcast`：逻辑上复用数据，但后续运算仍可能按 lane 复制；
- `or_reduce/and_reduce/add_reduce`：`mode="chain"` 和 `mode="tree"` 有不同深度；
- [`priority_mux`](../compiler/frontend/pycircuit/hw.py#L3427)：可能形成大规模选择网络，`assume_onehot` 是否真实成立会影响可采用的实现。

### 3.4 `v5.py`：module-local cycle-aware 子 DSL

[`CycleAwareDomain`](../compiler/frontend/pycircuit/v5.py#L67) 用一个 occurrence/cycle 计数器表示“当前构图位置属于第几拍”。`domain.next()` 只是让 elaboration 游标进入下一拍；真正的周期边界仍要通过寄存器实现。

最核心的算法是 [`delay_to`](../compiler/frontend/pycircuit/v5.py#L308)：

```python
while current_cycle < target_cycle:
    value = m.out(value, init=0)  # 每跨一拍插入一级 register
    current_cycle += 1
```

[`CycleAwareSignal._align`](../compiler/frontend/pycircuit/v5.py#L820) 在两个不同 cycle 的值参与运算时，把较早的值 delay 到较晚的 cycle。例如：

```text
a@cycle0 ── reg ── reg ──► a@cycle2 ─┐
                                     ├─ add@cycle2
b@cycle2 ────────────────────────────┘
```

这非常方便，但也是资源分析中的高风险点：对齐不是免费 metadata，而是实际插入寄存器；位宽越大、跨越拍数越多、扇出分支越多，FF 和 reset routing 越多。

[`compile_cycle_aware`](../compiler/frontend/pycircuit/v5.py#L1129) 同时兼容 eager 构造与 JIT/hierarchical 路径。按照 pyc4.0 Decision 0010，应把它理解为 **module-local 可选子 DSL**，不应让 cycle annotation 穿透全局 module hierarchy。

### 3.5 `design.py`：module、specialization 与 hierarchy

三个 decorator 的分工：

| decorator | 含义 | 默认 hierarchy 行为 |
|---|---|---|
| [`@module`](../compiler/frontend/pycircuit/design.py#L103) | 真正模块模板，可接受静态/value 参数 | 保留边界，`inline=false` |
| [`@function`](../compiler/frontend/pycircuit/design.py#L131) | 可复用硬件构造 helper | 倾向 inline，不拥有独立实例状态 |
| [`@const`](../compiler/frontend/pycircuit/design.py#L151) | 纯编译期元编程 | 不生成硬件 module |

[`Design`](../compiler/frontend/pycircuit/design.py#L339) 保存所有编译后的 specialized module，并输出：

- 合并版 MLIR：适合一次性交给 `pycc`；
- module 粒度 MLIR map：适合增量/并行构建；
- project manifest：描述 top、module 文件和依赖。

[`DesignContext.specialize`](../compiler/frontend/pycircuit/design.py#L522) 是 hierarchy 的关键。它对静态参数、port specs 和 value params 做 canonicalization，把结果放入 cache key，并生成稳定的 specialization symbol：

```text
Fifo(width=32, depth=4)  ─► Fifo__<hash A>
Fifo(width=64, depth=4)  ─► Fifo__<hash B>
```

硬件含义是：不同参数的 module 是不同的硬件模板。重复使用完全相同的 specialization 可以共享 module 定义，但每次 `pyc.instance` 仍代表一份实例资源。

[`_finalize_compiled`](../compiler/frontend/pycircuit/design.py#L608) 会附上 `pyc.base`、参数、module kind、inline、结构 metrics 和端口 metadata。后端的 hierarchy discipline、manifest、probe 路径都依赖这些信息。

### 3.6 `jit.py`：把 Python AST 编译成硬件 IR

[`compile`](../compiler/frontend/pycircuit/jit.py#L2217) 创建 `Design`/`DesignContext` 并 specialize top；[`compile_module`](../compiler/frontend/pycircuit/jit.py#L1998) 则读取函数源码、解析 AST、绑定参数和 builder 环境，然后逐条编译 function body。

它不是通用 Python 编译器，而是一个“可综合 Python 子集”的编译器。要区分两类控制流：

```python
# 静态控制流：elaboration 时决定，不产生 mux
if lanes == 8:
    ...

# 硬件控制流：依赖 Wire，两个分支都描述硬件，最终 lowering 成 mux
if valid:
    x = a
else:
    x = b
```

静态 `for range(...)` 常常代表复制硬件；JIT 也可能先生成 `scf.for/scf.if`，再由 MLIR pass 静态 lower。循环不是“自动时分复用”。

读 `jit.py` 时可按这条路线：

1. 先看 `compile_module` 怎样建立函数环境；
2. 再找 statement dispatcher；
3. 看 hardware `if` 如何收集合流变量；
4. 看 `for` 的常量边界与 unroll 规则；
5. 看 call dispatcher 如何区分 `@module/@function/@const`；
6. 最后再看 diagnostics 和结构 metrics。

### 3.7 connector、spec、record 与 hierarchy 辅助层

- [`connectors.py`](../compiler/frontend/pycircuit/connectors.py) 用 `WireConnector`、`RegConnector`、`VecConnector`、`ConnectorBundle/Struct` 统一读、写、flatten 和 instance handle。它主要解决“端口很多时怎样可靠连接”，不是新增电路语义。
- [`spec/`](../compiler/frontend/pycircuit/spec/) 定义结构化类型、signature、builder 和 DSE 相关抽象；backend 边界仍遵守 flat type gate。
- [`record.py`](../compiler/frontend/pycircuit/record.py) 是较早的结构化 record 辅助层。
- [`hierarchical.py`](../compiler/frontend/pycircuit/hierarchical.py) 提供 hierarchy/spec 推断与 AutoConnect。
- [`wiring/connect.py`](../compiler/frontend/pycircuit/wiring/connect.py) 处理连接规则。
- [`lib/`](../compiler/frontend/pycircuit/lib/) 是打包在 `pycircuit.lib.*` 下的 queue、SRAM、regfile、picker、stream 等可复用硬件库。顶层 `iplib/` 保留一份几乎相同的镜像实现；修改公共 IP 时应检查两处是否需要同步，避免漂移。

### 3.8 testbench、probe、trace 与 sidecar

- [`tb.py`](../compiler/frontend/pycircuit/tb.py) 定义 declarative TB actions：clock、reset、drive、expect、timeout、SVA、random、print。
- [`testbench.py`](../compiler/frontend/pycircuit/testbench.py) 把 `Tb` 转成可序列化 payload/MLIR 容器。
- [`probe.py`](../compiler/frontend/pycircuit/probe.py) 负责 probe catalog、path/glob、manifest 和 testbench probe handle。
- [`trace_dsl.py`](../compiler/frontend/pycircuit/trace_dsl.py) 解析 trace config，并依据 module artifacts 展开 instance/probe 选择。
- [`schedule_ir.py`](../compiler/frontend/pycircuit/schedule_ir.py) 把 TB schedule 压缩/编码成 sidecar sections。
- [`sidecar_sections.py`](../compiler/frontend/pycircuit/sidecar_sections.py) 定义 sidecar 二进制目录、section codec、inspect 和 verify。
- [`path_shortening.py`](../compiler/frontend/pycircuit/path_shortening.py) 在 Python 侧实现稳定的层级路径缩短；C++ runtime 有对应实现。

这些代码主要服务 DFX 和可扩展仿真，不应与“设备 RTL 的组合/时序语义”混为一层。

## 4. PYC dialect：两个 backend 的共同合同

### 4.1 声明层与实现层

PYC dialect 由两组文件共同定义：

- [`PYCOps.td`](../compiler/mlir/include/pyc/Dialect/PYC/PYCOps.td)：TableGen 声明 op 名、operand、result、attribute、trait；
- [`PYCTypes.td`](../compiler/mlir/include/pyc/Dialect/PYC/PYCTypes.td)：`!pyc.clock`、`!pyc.reset`；
- [`PYCDialect.cpp`](../compiler/mlir/lib/Dialect/PYC/PYCDialect.cpp)：dialect/type parse 与注册；
- [`PYCOps.cpp`](../compiler/mlir/lib/Dialect/PYC/PYCOps.cpp)：verifier、fold 和辅助逻辑。

主要 op 可按硬件含义分类：

| 类别 | PYC op |
|---|---|
| 常量/算术 | `constant/add/sub/mul/udiv/urem/sdiv/srem` |
| 选择/逻辑 | `mux/and/or/xor/not/eq/ult/slt` |
| 位宽操作 | `trunc/zext/sext/extract/shli/lshri/ashri/shl/lshr/ashr/concat` |
| 命名/连接 | `alias/wire/assign` |
| 状态 | `reg/fifo/byte_mem/sync_mem/sync_mem_dp/async_fifo/cdc_sync` |
| hierarchy | `instance` |
| vector | `v_get/v_create/v_broadcast/v_broadcast_dim/v_*_reduce` |
| region | `comb/yield` |
| 验证 | `assert` |

### 4.2 SSA value 与可回填 wire

纯组合 op 很适合 SSA：一个 result 只有一个定义，例如：

```mlir
%sum = pyc.add %a, %b : i8
```

但寄存器 next-state 存在“先获得 q、后定义 next”的构图需求，所以 dialect 还提供：

```mlir
%next = pyc.wire : i8
%q = pyc.reg %clk, %rst, %en, %next, %init : i8
pyc.assign %next, %computed : i8
```

这相当于给图构造留一个 backedge placeholder。它也引出重要 legality 问题：wire 必须有合法 driver、不能多驱动、不能组成纯组合环。

当前 [`AssignOp::verify`](../compiler/mlir/lib/Dialect/PYC/PYCOps.cpp#L909) 只验证 destination 是 `pyc.wire`；单驱动/解析网络的完整合同仍需要按 Decision 0130 在 dialect/pass 层收紧。Verilog Emitter 的拓扑排序虽然会识别 multiple driver，却只返回排序失败，调用侧随后退回 lexical order，并不会形成统一的 legality diagnostic；这正说明该合同需要前移到 dialect/pass。

### 4.3 verifier 和 fold 为什么比 Emitter 修补更重要

[`PYCOps.cpp`](../compiler/mlir/lib/Dialect/PYC/PYCOps.cpp) 为算术、mux、cast、vector、memory、instance 等实现：

- 类型与位宽检查；
- attribute 合法性；
- 常量折叠；
- 一些代数简化。

如果发现 C++ 与 Verilog 对某种 op 行为不同，正确路径应是：

```text
先写语义/litmus
  → 在 op verifier 或 MLIR pass 固化合同
  → 两个 emitter/runtime 同时实现
  → C++ vs Verilator 交叉门禁
```

只在 `VerilogEmitter.cpp` 加一个特殊分支，会制造 backend drift。

## 5. `pycc.cpp`：production compiler driver

[`pycc.cpp`](../compiler/mlir/tools/pycc.cpp#L2015) 的 `main` 是 MLIR 之后的总入口。它负责：

1. 解析 CLI options、build profile 和 input；
2. 注册 PYC、arith、func、scf dialect；
3. parse `.pyc` 为 `ModuleOp`；
4. 识别 testbench payload 旁路；
5. 建立 pass pipeline；
6. 运行 pass 并检查 hierarchy；
7. 输出 Verilog、C++、manifest、compile stats 和辅助脚本。

### 5.1 pass pipeline 的真实顺序

当前主顺序见 [`pycc.cpp:2254`](../compiler/mlir/tools/pycc.cpp#L2254)：

```text
CheckFrontendContract
InlineFunctions
[optional] FlattenInstances
CheckHierarchyDiscipline
SymbolDCE
[optional] MLIR Inliner
Canonicalizer → CSE → SCCP → RemoveDeadValues
EliminateDeadInstances → SymbolDCE
LowerSCFToPYCStatic
EliminateWires
EliminateDeadState
[optional] VectorUnroll / [otherwise] SLPPackWires
CombCanonicalize
CheckCombCycles
CheckClockDomains
PackI1Regs
[usually] FuseComb
Canonicalizer → CSE → RemoveDeadValues
EliminateDeadInstances → SymbolDCE
CheckFlatTypes
CheckNoDynamic
CheckLogicDepth
CollectCompileStats
```

顺序不是随意的。例如：

- SCF 必须先 lower，`CheckNoDynamic` 才能保证 backend 看到静态硬件；
- dead cleanup 和 canonicalization 会降低后续 emitter 负担；
- cycle/CDC gate 必须在 emission 前失败；
- logic depth 应在最终结构已较稳定时统计；
- `FuseComb` 改变 region 组织，但不能改变可观察语义。

### 5.2 输出模式

`pycc` 支持单文件和 `--out-dir` 工程输出。Verilog out-dir 通常包含：

```text
<module>.v
pyc_primitives.v
manifest.json
compile_stats.json
yosys_synth.ys
```

C++ out-dir 则包含 module/shard 源文件、headers 和 manifest，随后由 Python CLI 生成 CMake 工程。

`yosys_synth.ys` 是 sanity synthesis 脚本。它不能替代公平的 top-aware、参数一致、目标库一致的资源实验，也不代表 Yosys 在这里做仿真。

### 5.3 `pyc-opt` 的用途

[`pyc-opt.cpp`](../compiler/mlir/tools/pyc-opt.cpp#L41) 是面向开发者的 MLIR optimizer driver。修改 verifier/pass 时，它比完整 `pycircuit build` 更适合做最小复现：给一段手写 `.mlir/.pyc`，只运行目标 pass，检查 diagnostic 或 `FileCheck` 结果。

## 6. Transforms：语义门禁和结构优化分别做什么

### 6.1 文件总览

| 文件 | 作用 |
|---|---|
| `CheckFrontendContractPass.cpp` | 检查 frontend contract/version attributes |
| `CheckHierarchyDisciplinePass.cpp` | 检查 module/function/instance 层级规则 |
| `LowerSCFToPYCStaticPass.cpp` | 静态展开 `scf.for`，把 `scf.if` 变成选择网络 |
| `CheckNoDynamicPass.cpp` | 拒绝残余 `scf.*`、`index` 等动态 IR |
| `CheckFlatTypesPass.cpp` | 保证 backend 边界只含支持的扁平类型 |
| `CombDepGraph.cpp` | 计算跨 module 的组合输入/输出依赖摘要 |
| `CheckCombCyclesPass.cpp` | 检查本地与跨 instance 的组合环 |
| `CheckLogicDepthPass.cpp` | 计算跨 instance 的组合深度 proxy/WNS/TNS attrs |
| `CheckClockDomainsPass.cpp` | 检查 clock domain 与 CDC 使用是否合法 |
| `InlineFunctionsPass.cpp` | 按 pyCircuit function/module 规则 inline helper |
| `FlattenInstancesPass.cpp` | 显式请求时 flatten hierarchy |
| `EliminateWiresPass.cpp` | 删除可安全替换的简单 wire/assign |
| `EliminateDeadStatePass.cpp` | 删除无用 reg/memory/FIFO state |
| `EliminateDeadInstancesPass.cpp` | 删除无用 instance |
| `PrunePortsPass.cpp` | 裁剪无用端口 |
| `CombCanonicalizePass.cpp` | PYC 组合逻辑局部规范化 |
| `FuseCombPass.cpp` | 把组合 op 融成 `pyc.comb` region |
| `VectorUnrollPass.cpp` | 把原生 vector op 展成 scalar/lane op |
| `SLPPackWiresPass.cpp` | 尝试把相似 scalar wire 模式打包为 vector |
| `PackI1RegsPass.cpp` | 保守地合并条件一致、位置连续的 i1 registers |
| `CollectCompileStatsPass.cpp` | 收集 function 级 reg/memory 等静态统计 |

### 6.2 `CombDepGraph`：跨模块分析的关键基础设施

[`CombDepGraph.cpp`](../compiler/mlir/lib/Transforms/CombDepGraph.cpp) 为每个 function 总结：

```text
某个 output 组合地依赖哪些 input？
从每个 input 到 output 增加了多少逻辑深度？
```

顺序 state op 是 cut point：信号进入 register/memory/FIFO 后，上一拍的组合依赖不能直接穿过时钟边界。instance 则通过 callee summary 把依赖传播回 caller。

一个直观例子：

```text
Child: y = (a + 1) ^ b

summary(y):
  depends on a, extra depth ≈ 2
  depends on b, extra depth ≈ 1
```

Parent 实例化 Child 后，不需要 flatten Child，也能知道 `child.y` 与 parent 传入的哪些值组合相连。这正是 Decision 0134/0135 要求的 instance-aware 基础。

### 6.3 `CheckCombCyclesPass`：如何找到跨 instance 组合环

[`CheckCombCyclesPass.cpp`](../compiler/mlir/lib/Transforms/CheckCombCyclesPass.cpp) 先收集 function 内所有 `pyc.wire`，再为每个被驱动 wire 建边：

```text
dst wire ──► driver expression 读取到的其他 wire
```

读取依赖时会：

- 追溯普通 SSA defining op；
- 遇到 sequential op 就停止；
- 遇到 instance result 就查 `CombDepGraph` 的 callee argument dependency；
- 最后用三色 DFS 检测环。

所以这个 pass 检测的是硬件 netlist 的组合反馈，不是 Python 函数递归，也不是 module hierarchy 环。

### 6.4 `CheckLogicDepthPass`：它算的是 proxy，不是 STA

[`CheckLogicDepthPass.cpp`](../compiler/mlir/lib/Transforms/CheckLogicDepthPass.cpp) 给 op 一个粗略单位成本：

- constant、alias、wire、vector get/create/broadcast 等接近 0；
- 普通组合 op 默认约 1；
- chain reduction 约为 `lanes - 1`；
- tree reduction 约为 `ceil(log2(lanes))`；
- sequential op 截断路径。

它把 instance output 的 callee depth summary 加到 caller 输入深度上，最后检查 return、assert 和 sequential endpoint。结果写入 function attrs，并与 `--logic-depth` 限制比较。

这能尽早发现“32 项 reduction 被写成 31 级 chain”，但它不是目标技术 STA：乘法器、carry chain、布线、fanout、FPGA LUT 映射和 ASIC cell delay 都不在这个统一单位中。

一个当前限制是：对只有 declaration、没有 body 的外部 callee，depth 分析只能保守地当作局部 cut，无法凭空知道黑盒内部延迟。做跨 `.pyc` 独立编译时尤其要注意 summary 是否完整。

### 6.5 `CheckClockDomainsPass`：CDC 不是 lint 小问题

[`CheckClockDomainsPass.cpp`](../compiler/mlir/lib/Transforms/CheckClockDomainsPass.cpp) 追踪 state/output 的 clock domain，验证不同 domain 的组合混用和 CDC primitive。它的意义不是让代码“更规范”，而是阻止 metastability 风险和两个 backend 对跨时钟顺序作不同假设。

### 6.6 当前结构优化能力的边界

已有优化并不等于已有成熟综合器：

- `Canonicalizer/CSE/SCCP` 擅长局部常量和相同表达式；
- `Eliminate*` 擅长删除无用结构；
- `PackI1Regs` 条件很保守；
- `SLPPackWires` 覆盖有限；
- 大 priority mux、decoder sharing、memory inference、跨层共享等仍主要交给 Yosys/商业综合器，或需要新增 PYC 级结构 pass。

因此资源优化要先问：“问题结构在 MLIR 中已经固定了吗？”如果答案是是，单改 Verilog pretty-print 通常不会改变数量级。

## 7. Verilog Emitter：从 PYC op 到 RTL

主文件是 [`VerilogEmitter.cpp`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp)。入口有：

- [`emitVerilog`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L1364)：输出整个 MLIR module；
- [`emitVerilogFunc`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L1385)：输出一个 `func.func`；
- [`emitFunc`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L888)：真正生成一个 Verilog module。

### 7.1 名称与类型

Emitter 内部 `NameTable` 给 SSA value 生成确定且合法的 Verilog identifier；优先使用 `pyc.name`，否则使用 op 类型和编号。端口名来自 `arg_names/result_names` attributes。

标量 `iN` 映射到 `[N-1:0]`。MLIR vector 在 module 边界会被 flatten 为 packed Verilog port，module 内部可用 unpacked arrays，并由 [`emitUnpackFromPacked`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L134) / [`emitPackToPacked`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L150) 生成桥接 assign。

这使外部端口稳定且容易交给 Verilator/Yosys，但要警惕 lane 顺序和多维 flatten 约定；相关 gate 在 `tests/vec/test_vec_ops.py`。

### 7.2 先声明，再分类，再按依赖输出

`emitFunc` 大体执行：

1. 打印 `module (...)` 和端口；
2. 为 op results 声明 wire/array；
3. 分类组合 op、state primitive、instance、assert；
4. 对组合 op 做拓扑排序；
5. 输出 continuous assignments/组合表达式；
6. 输出 submodule instances 和 vector port bridge；
7. 输出 reg/FIFO/memory/CDC primitive instances；
8. 连接 return value 到 output port。

[`topoSortCombOps`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L788) 根据 SSA operand 和 wire driver 建依赖图。multiple driver 会让该函数返回 `false`；调用侧目前随后采用 lexical fallback，而不是给出专门错误。这一 backend 容错不应代替前面的组合环/单驱动 gate。

### 7.3 组合 op 如何翻译

典型 lowering 是机械式表达式映射：

```mlir
%sum = pyc.add %a, %b : i8
%y = pyc.mux %sel, %sum, %q : i8
```

```verilog
assign sum = a + b;
assign y = sel ? sum : q;
```

除法/余数显式处理除零，signed 运算需要 `$signed(...)`。vector element-wise op 递归展开 lane 表达式，reduction 根据 `mode` 生成 chain 或 balanced tree。

这里最值得做 code review 的不是空格和声明顺序，而是：

- signed/unsigned cast 是否一致；
- vector flatten 的索引方向；
- division-by-zero、shift amount 越界等 corner case；
- mux/reduction 的结构是否保留了 MLIR 中的意图；
- assertion 是否只用于 simulation，是否被 synthesis guard 正确包围。

### 7.4 state 不在 Emitter 中手写 always block

`pyc.reg`、FIFO、memory 和 CDC 主要实例化 `runtime/verilog/` primitive，而不是每次在 Emitter 中复制一段 `always`。例如概念上：

```verilog
pyc_reg #(.WIDTH(8), ...) count_inst (...);
pyc_sync_mem #(.ADDR_WIDTH(...), .DATA_WIDTH(...), .DEPTH(...)) mem0 (...);
```

所以检查 reset、enable、read-during-write 或 FIFO handshake 时，要同时看：

```text
PYCOps.td/PYCOps.cpp        语义和合法形状
VerilogEmitter.cpp          参数/端口怎样接入 primitive
runtime/verilog/*.v         最终 always/assign 行为
runtime/cpp/*.hpp           C++ backend 是否等价
```

## 8. Verilog runtime primitives

| 文件 | 硬件模型 |
|---|---|
| [`pyc_reg.v`](../runtime/verilog/pyc_reg.v) | 带 reset、enable、init 的寄存器 |
| [`pyc_fifo.v`](../runtime/verilog/pyc_fifo.v) | 单时钟 ready/valid FIFO |
| [`pyc_byte_mem.v`](../runtime/verilog/pyc_byte_mem.v) | async-read、sync-write byte memory |
| [`pyc_sync_mem.v`](../runtime/verilog/pyc_sync_mem.v) | 1R1W synchronous memory，registered read |
| [`pyc_sync_mem_dp.v`](../runtime/verilog/pyc_sync_mem_dp.v) | 2R1W synchronous memory |
| [`pyc_async_fifo.v`](../runtime/verilog/pyc_async_fifo.v) | dual-clock async FIFO，Gray pointer 同步 |
| [`pyc_cdc_sync.v`](../runtime/verilog/pyc_cdc_sync.v) | 目的时钟域 synchronizer |
| `pyc_add/and/or/xor/not/mux.v` | 简单组合 primitive，部分路径/兼容用途 |

资源问题中最值得优先审查的是 memory 和 reset：

- reset 整个 memory array 往往妨碍 BRAM/SRAM inference；
- 大量带 reset 的 datapath FF 会增加控制 routing 和面积；
- async read 与 sync registered read 会推导出完全不同的存储结构；
- read-during-write 的 old/new/no-change 语义必须与目标 memory primitive 匹配。

当前同步 memory 的 C++ runtime 明确采用 old-data 行为，因此 Verilog primitive、litmus 和综合模板都应共同维持 Decision 0114/0122 的合同。

## 9. C++ Emitter：生成的不是 RTL，而是 SimObject 图

主文件 [`CppEmitter.cpp`](../compiler/mlir/lib/Emit/CppEmitter.cpp) 比 Verilog Emitter 更大，因为它不仅要翻译表达式，还要生成软件仿真调度、对象层级、cache、probe 和统计。

### 9.1 一个 function 变成一个 C++ struct

[`emitFunc`](../compiler/mlir/lib/Emit/CppEmitter.cpp#L867) 对每个 `func.func` 生成：

```cpp
struct ModuleName {
  // input/output fields
  // internal Wire/Bits/Vec values
  // local register/FIFO/memory objects
  // child module objects
  // probe/trace registration

  void eval();
  void tick_compute();
  void tick_commit();

  void comb()     { eval(); }
  void tick()     { tick_compute(); }
  void commit()   { tick_commit(); }
  void transfer() { tick_commit(); }
  void step() {
    comb(); tick(); commit(); comb();
  }
};
```

[`emitCpp`](../compiler/mlir/lib/Emit/CppEmitter.cpp#L2714) 先对 module instance graph 做 Kahn 拓扑排序，让 callee struct 在 caller 前定义；若 hierarchy 自身成环则失败。

### 9.2 `eval()`：组合逻辑求稳

Emitter 会尝试为组合 op 和 primitive/instance 建完整拓扑序：

- 若成功，`eval()` 单遍按拓扑顺序执行，必要时拆成多个 helper，避免超大 C++ 函数；
- 若不能得到完整拓扑序，会先执行组合 pass，再使用 SCC worklist 或有界 fixed-point fallback；
- submodule/FIFO/memory eval 带 input cache，输入和内部 state 没变化时可跳过重复计算；
- runtime 可统计 `instance_eval_calls`、`primitive_eval_calls`、cache hits 和 fallback iterations。

这里的 fixed-point 不等于允许非法硬件组合环。合法组合环仍应被前面的 `CheckCombCyclesPass` 拒绝；fallback 主要处理分层 primitive、deferred wire 和保守调度无法一次排序的情况。

### 9.3 `tick_compute/tick_commit`：为什么分两阶段

生成代码先递归调用所有 child/local primitive 的 `tick_compute()`，只计算和锁存 next state；然后统一 `tick_commit()` 更新 current state。简化示意：

```text
旧状态 q(t)
   │
   ├─ eval/comb ─► d(t)
   │
   ├─ tick_compute：所有寄存器读取同一份旧状态并算 next
   │
   ├─ tick_commit：统一发布 q(t+1)
   │
   └─ eval/comb：让新状态后的组合输出稳定
```

如果边算边写，后处理的寄存器可能看到前一个寄存器的“新状态”，结果依赖 C++ 循环顺序，与真实同步硬件不一致。两阶段更新就是软件 simulator 对非阻塞时序语义的实现。

### 9.4 C++ 与 Verilator 的定位不同

| C++ backend | Verilator backend |
|---|---|
| 直接从 PYC 生成软件模型 | 先生成 Verilog，再由 Verilator 编译 |
| 可针对超大设计做专用对象/cache/调度优化 | 更接近最终 RTL 语义与 SystemVerilog 生态 |
| 适合快速功能仿真、probe/sidecar/DFX | 适合 RTL 交叉验证、波形和 integration |
| 可能因 emitter/runtime bug 与 RTL 漂移 | 仍可能因 Verilog emitter/primitive bug 出错 |

因此最有价值的门禁是同一 stimulus 下比较 C++ 与 Verilator 的观察点，而不是只让两边各自“跑通”。

## 10. C++ runtime 文件地图

| 文件 | 作用 |
|---|---|
| [`pyc_bits.hpp`](../runtime/cpp/pyc_bits.hpp#L106) | 固定位宽 `Bits<W>`，mask、算术、比较、slice、cast；宽度大时支持多 word |
| [`pyc_vec.hpp`](../runtime/cpp/pyc_vec.hpp#L19) | `Vec<T,N>` 和 element-wise/reduction/pack helpers |
| [`pyc_primitives.hpp`](../runtime/cpp/pyc_primitives.hpp#L69) | register 和单时钟 FIFO；早期简单组合 primitive 也在此 |
| [`pyc_byte_mem.hpp`](../runtime/cpp/pyc_byte_mem.hpp#L20) | byte memory、异步读/同步写、观察 hooks |
| [`pyc_sync_mem.hpp`](../runtime/cpp/pyc_sync_mem.hpp#L23) | 1R1W/2R1W sync memory，old-data、watch/hash/dump |
| [`pyc_async_fifo.hpp`](../runtime/cpp/pyc_async_fifo.hpp#L32) | 双时钟 FIFO 和独立 in/out tick phase |
| [`pyc_cdc_sync.hpp`](../runtime/cpp/pyc_cdc_sync.hpp#L11) | 多级 CDC synchronizer state |
| [`pyc_tb.hpp`](../runtime/cpp/pyc_tb.hpp#L114) | clock、reset、step、trace phase 和通用 testbench driver |
| [`pyc_probe_registry.hpp`](../runtime/cpp/pyc_probe_registry.hpp#L282) | centralized probe registry、path/glob/kind lookup |
| [`pyc_trace_bin.hpp`](../runtime/cpp/pyc_trace_bin.hpp#L19) | `.pyctrace` binary writer |
| `pyc_vcd.hpp` | VCD 输出 |
| `pyc_linxtrace.hpp` | Linx trace integration；修改时必须使用项目规定的 Linx 流程 |
| `pyc_tb_sidecar*.hpp` | sidecar schedule 的加载与执行 |
| `pyc_sim.hpp` | generated code 使用的总 include |

[`pyc_tb.hpp`](../runtime/cpp/pyc_tb.hpp#L242) 中的 trace phase 尤其值得关注：comb、tick、commit 对应不同观察时刻。排查“波形看起来差一拍”时，应先确认采样点，而不是立刻怀疑寄存器实现。

## 11. 三个纵向例子：一段 DSL 如何穿过整个系统

以下 IR/RTL 省略了具体 SSA 编号和 attributes，目的是展示结构对应关系。

### 11.1 组合加法

```python
y = a + b
m.output("y", y)
```

前端：

```text
Wire.__add__
  → Circuit.add
  → Module.add
  → 追加 pyc.add
```

MLIR：

```mlir
%sum = pyc.add %a, %b : i8
return %sum : i8
```

Verilog：

```verilog
assign sum = a + b;
assign y = sum;
```

C++：

```cpp
sum = a + b;
y = sum;
```

综合后 `sum` 这个名字和中间 wire 可能消失，但 8-bit 加法逻辑通常仍存在。

### 11.2 带 enable 的 counter register

仓库示例见 [`designs/examples/counter/counter.py`](../designs/examples/counter/counter.py#L13)。其关键行为是：

```python
count = domain.signal(width=width, reset_value=0, name="count")
domain.next()
count.assign(count + 1, when=enable)
```

概念 IR：

```mlir
%next = pyc.wire : i8
%q = pyc.reg %clk, %rst, %enable, %next, %zero : i8
%inc = pyc.add %q, %one : i8
pyc.assign %next, %inc : i8
```

Verilog backend 实例化 `pyc_reg`；C++ backend 实例化 `pyc_reg<8>`。二者都要满足：

```text
posedge && reset  → q := init
posedge && enable → q := next
otherwise         → q 保持
```

TB 见 [`tb_counter.py`](../designs/examples/counter/tb_counter.py#L16)，它通过 declarative `clock/reset/drive/expect` 同时生成 C++ 和 SV 测试逻辑。

### 11.3 synchronous memory

仓库 litmus 见 [`mem_rdw_olddata.py`](../designs/examples/mem_rdw_olddata/mem_rdw_olddata.py#L6)。调用 `m.sync_mem(...)` 后：

```text
Circuit.sync_mem
  → Module.sync_mem
  → pyc.sync_mem op
  ├─ VerilogEmitter → pyc_sync_mem instance → runtime/verilog/pyc_sync_mem.v
  └─ CppEmitter     → pyc_sync_mem<...>      → runtime/cpp/pyc_sync_mem.hpp
```

同地址读写时返回 old data，不是 Emitter 随意选择的细节，而是必须由 IR 语义、两个 runtime 和 litmus 一起锁定的合同。

## 12. 测试与 gate：哪个层次能抓住哪类错误

### 12.1 脚本入口

[`flows/scripts/pyc`](../flows/scripts/pyc) 是推荐的人类入口：

```text
./flows/scripts/pyc build    构建并 staged install pycc/pyc-opt/runtime
./flows/scripts/pyc smoke    example compile smoke + 小规模 simulation smoke
```

[`run_examples.sh`](../flows/scripts/run_examples.sh) 覆盖：

- examples 的 emit + pycc；
- multi-`.pyc` project build；
- hierarchy/cache/probe/trace/cosim 等正负用例；
- v4.0 semantic regression lane。

[`run_sims.sh`](../flows/scripts/run_sims.sh) 构建并运行 C++/Verilator cases。[`run_semantic_regressions_v40.sh`](../flows/scripts/run_semantic_regressions_v40.sh) 当前重点覆盖 X/Z trace、reset/invalidate 顺序和 net-resolution/depth smoke。

### 12.2 vector pytest

[`tests/vec/`](../tests/vec/) 的结构很清晰：

```text
cases.py       定义测试矩阵和样例
oracle.py      Python reference result
generate.py    生成临时 pyCircuit design/TB
runner.py      调 CLI、pycc、C++ binary、Verilator
test_vec_ops.py frontend/IR/backend/Yosys smoke assertions
```

它覆盖 vector I/O flatten、rank-2 port、broadcast、cycle alignment、reduce mode、C++ manifest、Verilator 和可选 Yosys parse/synthesis smoke。

### 12.3 当前验证空白

从资源优化视角，当前测试体系仍缺少一类核心 gate：

```text
固定 top + 固定参数 + 固定 target/library
  → 生成 RTL
  → Yosys/目标工具综合
  → 机器可读 cell/LUT/FF/BRAM/depth 指标
  → 与基线按阈值比较
```

`compile_stats.json` 统计的是 MLIR function 定义内的静态对象，不是从指定 top 展开的实例面积，也不是综合后的 cell 数。它适合解释结构变化，不适合单独证明 PPA 改善。

## 13. 遇到问题时先看哪里

| 症状/任务 | 第一站 | 第二站 |
|---|---|---|
| Python API 为什么这样工作 | `hw.py` / `v5.py` | `dsl.py` |
| Python `if/for/call` 如何变硬件 | `jit.py` | `LowerSCFToPYCStaticPass.cpp` |
| 参数化 module 为什么重复/重编 | `design.py::specialize` | `cli.py` cache/manifest |
| `.pyc` 语法或类型错误 | `PYCOps.td` / `PYCOps.cpp` | `dsl.py` 文本生成 |
| 跨模块组合环漏报 | `CombDepGraph.cpp` | `CheckCombCyclesPass.cpp` |
| logic-depth 不合理 | `CheckLogicDepthPass.cpp` | vector reduce/mux 构造 |
| CDC 报错 | `CheckClockDomainsPass.cpp` | `pyc_cdc_sync.*` / async FIFO |
| 生成 Verilog 端口/表达式错误 | `VerilogEmitter.cpp` | 对应 `PYCOps.cpp` 语义 |
| register/reset 行为错误 | `PYCOps.cpp` + semantic decisions | 两侧 `pyc_reg` runtime |
| memory RDW/byte-enable 错误 | memory op verifier | 两侧 memory runtime + litmus |
| C++ 模型差一拍 | `CppEmitter` tick/commit | `pyc_tb.hpp` 采样 phase |
| C++ 模型太慢 | `CppEmitter` eval schedule/cache | sim stats 和大设计 profile |
| Verilog 面积突然变大 | 先比较优化后 MLIR | Verilog + Yosys cell breakdown |
| FF 数过多 | `v5.py::delay_to`、`Circuit.out` | reset 策略、`PackI1RegsPass` |
| mux/LUT 爆炸 | `Vec.priority_mux`、JIT hardware if | reduction/mux MLIR 拓扑、Yosys ABC |
| memory 变成 FF | memory op/primitive 语义 | Yosys memory passes/目标 BRAM 模板 |
| Verilator TB 错误 | `cli.py::_render_tb_sv` | `tb.py` / trace plan |
| C++ TB 错误 | `cli.py::_render_tb_cpp` | `runtime/cpp/pyc_tb.hpp` |
| probe 路径错误 | `probe.py` / `trace_dsl.py` | CppEmitter probe registration/runtime registry |

## 14. 做一次语义修改，通常要改哪些层

假设新增 `pyc.foo`，不要从 Emitter 开始。较完整的工作包是：

```text
1. 决策/语义
   - 输入输出类型、位宽、signedness、corner cases

2. Dialect
   - PYCOps.td 声明
   - PYCOps.cpp verifier/fold/canonicalization

3. Gate
   - pyc-opt 正/负 IR tests
   - legality/depth/CDC 等分析是否要识别该 op

4. Frontend
   - dsl.py builder
   - hw.py API/operator
   - jit.py call/AST 支持（如需要）

5. Backends
   - VerilogEmitter.cpp
   - CppEmitter.cpp / runtime

6. Equivalence
   - 同一 litmus 跑 C++ 与 Verilator
   - corner cases 对 oracle

7. Resource
   - Yosys/目标综合器 A/B
```

若只是做结构优化 pass，也必须证明优化前后：

- PYC 可观察行为一致；
- C++ 与 Verilog 都消费同一优化后 IR；
- 不破坏 hierarchy、probe、clock/reset/memory 合同；
- 资源或时序指标确实改善，而不是只减少 RTL 行数。

## 15. 对当前架构的几点判断

以下是基于源码的工程判断，不等同于仓库已接受的新决策。

### 15.1 做得好的部分

- dialect + passes 被明确设为单一语义源，方向正确；
- Verilog/C++ 两个 backend 从同一 MLIR 出发，具备 differential test 基础；
- instance-aware comb dependency、cycle 和 depth 已有专门基础设施；
- state primitive 集中在 runtime，减少 Emitter 内重复实现；
- C++ SimObject、两阶段更新、probe registry、sidecar 是面向大设计的体系化设计；
- multi-module artifacts、manifest 和 cache 已考虑增量/并行构建。

### 15.2 维护风险

- `hw.py`、`jit.py`、`cli.py`、`pycc.cpp` 和两个 Emitter 都较大，新增功能容易跨层遗漏；
- Python 以文本方式生产 MLIR，使错误边界较晚，也增加前端/dialect 语法同步成本；
- single-driver 合同目前还没有完全前移到统一 verifier/pass；
- C++ Emitter 的调度/cache/SCC/probe 逻辑集中在一个大文件，正确性与性能修改耦合；
- module 粒度独立编译时，外部 declaration 的组合深度摘要能力有限；
- `compile_stats.json` 与真实 top-expanded/Yosys 资源之间存在口径差；
- 当前 resource regression 自动化弱于功能/语义 gate。

### 15.3 优化工作最值得建立的边界

建议把问题分成三类，不要混做：

```text
A. 语义合法性
   single-driver、reset、memory、CDC、组合环
   → dialect verifier / MLIR gate

B. 结构质量
   多余 reg、mux 拓扑、reduction tree、vector unroll、dead state
   → MLIR analysis/transform + equivalence gate

C. 技术映射
   LUT/cell/BRAM/DSP、ABC、目标时钟、library
   → Yosys/FPGA/ASIC flow
```

Emitter 主要位于 B 到 C 的边界。若 A 未定义清楚，不能靠 emitter 猜；若 B 已经很差，也不能期待 Yosys 永远救回来。

## 16. 推荐阅读顺序

### 第一轮：建立全局调用链

1. [`designs/examples/counter/counter.py`](../designs/examples/counter/counter.py)
2. [`compiler/frontend/pycircuit/__init__.py`](../compiler/frontend/pycircuit/__init__.py)
3. [`compiler/frontend/pycircuit/dsl.py`](../compiler/frontend/pycircuit/dsl.py)
4. [`compiler/frontend/pycircuit/hw.py`](../compiler/frontend/pycircuit/hw.py) 中的 `Wire/Reg/Circuit.out`
5. [`compiler/frontend/pycircuit/v5.py`](../compiler/frontend/pycircuit/v5.py) 中的 `CycleAwareDomain/delay_to`
6. [`compiler/mlir/include/pyc/Dialect/PYC/PYCOps.td`](../compiler/mlir/include/pyc/Dialect/PYC/PYCOps.td)
7. [`compiler/mlir/tools/pycc.cpp`](../compiler/mlir/tools/pycc.cpp#L2235) 的 pass pipeline
8. [`VerilogEmitter.cpp`](../compiler/mlir/lib/Emit/VerilogEmitter.cpp#L888) 的 `emitFunc`

### 第二轮：理解 hierarchy 与仿真

1. `design.py::DesignContext.specialize`
2. `jit.py::compile_module`
3. `dsl.py::instance_op`
4. `CppEmitter.cpp::emitFunc`
5. `runtime/cpp/pyc_primitives.hpp`
6. `runtime/cpp/pyc_tb.hpp`
7. `cli.py::_cmd_build`

### 第三轮：进入优化工作

1. `CombDepGraph.cpp`
2. `CheckCombCyclesPass.cpp`
3. `CheckLogicDepthPass.cpp`
4. `EliminateWires/DeadState/DeadInstances`
5. `CombCanonicalize/FuseComb`
6. `VectorUnroll/SLPPackWires/PackI1Regs`
7. 选一个真实 top，对比优化前后 MLIR、Verilog 和 Yosys cell breakdown

## 17. 最后用一句话记住每个核心文件

```text
dsl.py              “怎样写出一条 PYC 文本 op”
hw.py               “用户的硬件对象与运算符是什么意思”
v5.py               “cycle 标注怎样变成真实 delay registers”
jit.py              “Python AST 怎样变成硬件构图动作”
design.py           “module specialization 与 hierarchy 怎样组织”
cli.py              “整个项目如何被加载、缓存、并行构建和仿真”
PYCOps.td/.cpp      “什么 IR 是合法的、每个 op 的共同语义是什么”
Transforms/*.cpp    “哪些结构被拒绝、规范化或优化”
pycc.cpp            “这些 pass 以什么顺序运行，产物怎样落盘”
VerilogEmitter.cpp  “合法 PYC 怎样机械翻译为 RTL”
CppEmitter.cpp      “合法 PYC 怎样变成可调度的 SimObject”
runtime/verilog     “最终 RTL state primitive 的真实行为”
runtime/cpp         “C++ state、位向量、TB、trace/probe 的真实行为”
flows/tests         “上述合同怎样被证明没有漂移”
```

对 pyCircuit 做资源优化时，最有效的阅读方式不是从生成的 `.v` 反向猜，而是选一个昂贵结构，从 `hw.py/v5.py → PYC op → transform → VerilogEmitter → Yosys cell` 做一次完整纵向追踪。这样才能分清资源究竟是在 DSL 构图时产生、在 MLIR 优化时未消掉，还是在目标技术映射时没有被正确识别。
