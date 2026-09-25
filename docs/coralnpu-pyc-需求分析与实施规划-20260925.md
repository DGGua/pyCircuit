# Coral NPU 用 pyCircuit 重写：需求分析与实施规划

## 背景与目标

Coral NPU（仓库 [google-coral/coralnpu](https://github.com/google-coral/coralnpu)，Apache-2.0）是 Google Research 的开源边缘 NPU。它不是一块只有脉动阵列的加速器，而是一颗 32 位 RISC-V 核，旁边挂着可编程向量后端，文档里还规划了矩阵引擎。三块合在一起，才是用户要的「标量 + Vector + Cube」结构。

本地只读副本在 `/home/lidongzhe/coralnpu`，提交 `ddea421`（浅克隆）。当前 pyCircuit 分支是 `feat/change-driven-scheduler`。本次目标是在该分支上开临时分支，用 pyCircuit 按 Coral 的架构规格重写硬件，并把能对上的测试改写成 pyCircuit 测试。

Coral 自己的实现是 Chisel 加 SystemVerilog，用 Bazel、cocotb、UVM、Verilator 验证。pyCircuit 没有 Chisel 导入器。香山移植（`designs/XiangShan-pyc/docs/port_XiangShan_to_pyc.md`）的做法是：原代码只当行为参考，RTL 和测试用 pyCircuit 重写，不保留原框架的模块骨架。本次沿用这一做法。

## 项目现状与执行流程定位

pyCircuit 写硬件的路径是：Python 里用 `@module` 描述电路，前端在 JIT elaboration 时生成 MLIR，再降到 C++ 仿真或 Verilog。寄存器、FIFO、同步 SRAM（`sync_mem` / `sync_mem_dp`）、向量端口都已经有。设计放在 `designs/` 下，不改编译器方言，除非新语义在现有原语里表达不了。

Coral 的执行链（见其 `doc/overview.md`）是：

1. 取指：128 位宽，从指令紧耦合存储器（ITCM，默认 8 KB）或外部总线取最多 4 条指令。
2. 译码和发射：4 条标量通路，按记分板检查冒险；向量指令最多 2 条进入向量命令队列。
3. 标量执行：4 个 ALU、4 个分支单元、1 个乘法器、1 个除法器、1 个浮点单元、1 个访存单元。发射按序，退休可以乱序完成后再按序提交。
4. 向量后端：RVV 1.0，`VLEN=128`，32 个向量寄存器。命令队列 8 项，微操作队列 16 项，再分到向量 ALU、乘加、除法、浮点和归约单元。
5. 矩阵引擎（文档称 VME / Zvt）：16 个矩阵 tile 寄存器，外积乘加，INT8 / FP32 / BF16。在当前这份源码树里，没有独立的矩阵 PE 阵列实现；概述把它标成实验、可选。能看到的实现是标量核、RVV 接口和 SystemVerilog 向量后端。
6. 存储：数据紧耦合存储器（DTCM，默认 32 KB），以及 AXI4 或 TileLink-UL 对外端口。主机也可以从外面读写 TCM。

pyCircuit 侧对应的落点是新建 `designs/CoralNPU-pyc/`，不改 `compiler/`、`runtime/` 的语义。仿真用现有 pyCircuit 测试台驱动时钟、复位和存储器，核对寄存器和存储器结果。

## 相关现有实现说明

Coral 参考实现（只读，不链进 pyCircuit 构建）：

| 路径 | 现在做什么 |
|---|---|
| `hdl/chisel/src/coralnpu/scalar/` | 标量 ALU、分支、乘除、浮点、译码、寄存器堆、CSR、取指、访存 |
| `hdl/chisel/src/coralnpu/Core.scala` 及 `CoreAxi.scala` | 把前端、执行、TCM、总线粘成 `CoreMini` / `RvvCoreMini` |
| `hdl/chisel/src/coralnpu/rvv/` | 向量指令译码和与标量核的接口，真正的向量数据通路在 SystemVerilog 里 |
| `hdl/verilog/rvv/` | RVV 后端（队列、保留站、ALU、乘加、ROB） |
| `tests/cocotb/` | 用 cocotb 跑 ELF：`nop`、CSR、异常、RVV 算术、load/store、DMA 等 |
| `doc/overview.md`、`doc/microarch/` | 发射宽度、延迟、存储映射 |

pyCircuit 里已有、这次会用到的能力：

- 层次模块、寄存器、组合逻辑、向量线。
- `sync_mem`：ITCM / DTCM 用同步 SRAM 建模。读数据比 Coral 的单周期组合读多一拍时，要在测试预期里记清楚，或在模块内用组合读的 byte memory 对齐 Coral 的单周期 TCM。Coral TCM 是单周期 SRAM；pyCircuit 的 `sync_mem` 读数据打一拍，`byte_mem` 读是组合的。TCM 用 `byte_mem` 才能对齐「发出地址当拍拿到数据」。
- 测试台：Python 里推时钟、检查端口和存储器，不跑 Bazel。

现有 `designs/XiangShan-pyc/` 是另一颗核的移植，目录和测试风格可参考，但不共享 Coral 的指令译码。

## 需求与验收标准

功能需求：

1. 在 `feat/change-driven-scheduler` 上建立临时分支 `tmp/coralnpu-pyc`。不把 Coral 的 Chisel/SystemVerilog 源码拷进 pyCircuit 仓库。
2. 用 pyCircuit 表达 Coral 的三层结构：标量前端与执行、向量后端、矩阵引擎接口。矩阵引擎在参考树里还没有可运行的 PE 阵列，因此第一版只保留与标量/向量之间的命令接口和 tile 寄存器骨架，不宣称 GEMM 已与 Coral 对齐。
3. 行为对齐的范围按阶段验收，不要求第一阶段就跑通 Coral 全部 cocotb。
4. 测试改写成 pyCircuit 测试：同一条指令序列，检查同样的寄存器或存储器结果。不迁移 Bazel、cocotb、UVM、Verilator 脚本。

非功能：

- 不修改 MLIR 方言和 C++/Verilog 原语语义。
- 设计代码放在 `designs/CoralNPU-pyc/`，与编译器主线隔离。
- 注释用英文，说明周期和端口含义；不粘贴 Coral 源码。

阶段验收：

| 阶段 | 做完时必须为真 |
|---|---|
| S0 骨架 | 顶层模块能 elaboration：标量核、向量命令口、矩阵 tile 口、ITCM/DTCM 端口都在，空向量/矩阵单元可被例化 |
| S1 标量整数 | RV32I 的算术、逻辑、移位、分支、跳转、`lui`/`auipc` 在单发射模型上与手写期望一致；至少覆盖 Coral `nop_test`、一组 ALU、一组分支 |
| S2 乘除与访存 | `mul`/`div` 延迟与 Coral 概述一致（乘 2 拍、除多拍）；DTCM 按字节地址 load/store，包含非对齐字的拒绝或拆分（与 Coral 一致的那种） |
| S3 四发射前端 | 无冒险时一拍最多发射 4 条整数指令；RAW 时停顿；顺序退休 |
| S4 RVV | `VLEN=128` 的整数向量加、逻辑、单位步进 load/store，对照 Coral `tests/cocotb/rvv` 里一组汇编的寄存器结果 |
| S5 矩阵 | 仅当参考树里出现可执行的 VME RTL 后，再做 INT8 外积累加；当前阶段不做数值验收 |

## 方案设计

### 模块边界、输入与输出

顶层 `CoralNpu`（对应 Coral 的 `CoreMini`，先不含 AXI 主机口）：

- 输入：时钟、复位、可选的外部 ITCM/DTCM 初始化写口（测试台用）。
- 输出：`halted`、`pc`、`tohost`（测试结束约定：写 DTCM 的某个字或执行 `ebreak`）。
- 内部：
  - `Frontend`：取指缓冲、4 路译码、记分板、退休缓冲。
  - `ScalarExu`：ALU×4、BRU×4、MLU、DVU。浮点 FPU 放到 S2 之后，第一批整数测试不依赖它。
  - `Lsu` + `Itcm` + `Dtcm`。
  - `RvvBackend`：S4 之前是接收命令并立刻回答「未实现」的空模块，避免标量译码表里向量指令被当成非法后无法扩展。
  - `MatrixTiles`：16 个 tile 的存储骨架和 `mset*` 译码占位。没有 PE 时，这些指令在 S1–S4 里视为保留，执行则停机并报告，不假装算对。

第一阶段为了先跑通指令，标量数据通路用单发射、按序退休。S3 再把同一套 ALU 扩成 4 发射。这样 S1 的测试在 S3 仍然有效，只是节拍变少。

### 上下游关系与数据/控制流

测试台把程序镜像写入 ITCM，拉复位，推时钟。核从复位向量取指，译码后进入标量 ALU 或 LSU。LSU 只访问 DTCM。程序结束时写 `tohost`。向量指令在 S4 之前不到达可执行测试。矩阵指令同样不进入验收测试。

不接 AXI、不接 TileLink、不接 DMA、不接调试模块。这些在 Coral 里是 SoC 外壳，不改变「标量 + 向量 + 矩阵」的计算结构。需要外部存储时，后续再加一个 pyCircuit 存储器端口，而不是先搬 AXI。

### 文件和接口改动清单

新增（批准后）：

| 路径 | 作用 |
|---|---|
| `designs/CoralNPU-pyc/README.md` | 阶段、与 Coral 提交的对应关系、怎么跑测试 |
| `designs/CoralNPU-pyc/lib/isa.py` | RV32I 编码常量和译码结果结构 |
| `designs/CoralNPU-pyc/scalar/alu.py`、`bru.py`、`regfile.py`、`decode.py` | 标量叶子 |
| `designs/CoralNPU-pyc/scalar/frontend.py`、`scoreboard.py`、`core.py` | 取指、发射、退休、核顶 |
| `designs/CoralNPU-pyc/mem/tcm.py`、`lsu.py` | ITCM/DTCM 与 load/store |
| `designs/CoralNPU-pyc/rvv/backend.py` | 向量后端占位，S4 再填数据通路 |
| `designs/CoralNPU-pyc/matrix/tiles.py` | 矩阵 tile 骨架 |
| `designs/CoralNPU-pyc/top.py` | 把上述模块接起来 |
| `designs/CoralNPU-pyc/tb/tb_alu.py`、`tb_branch.py`、`tb_nop.py` | 从 Coral cocotb 用例改写的 pyCircuit 测试 |
| `designs/CoralNPU-pyc/programs/*.hex` | 手写或由小型汇编器生成的指令镜像 |

不改：`compiler/`、`runtime/`、`docs/rfcs/pyc4.0-decisions.md`。不把 `/home/lidongzhe/coralnpu` 加为子模块。

### 边界条件、错误处理与兼容性

- `x0` 读为 0，写被丢掉。
- 非法指令：S1 只保证 RV32I 整数子集；浮点、CSR、系统指令、向量、矩阵进入退休前停机，测试不把它们当成功路径。
- 复位后 PC 为 ITCM 基址 0。
- TCM 读使用组合读（`byte_mem`），与 Coral 单周期 SRAM 对齐。容量先固定 8 KB ITCM、32 KB DTCM。
- 四发射未完成前，单发射结果必须与四发射在同一程序上的架构状态一致，节拍可以不同。
- 现有 pyCircuit 测试和香山设计不参与这次构建，避免回归面扩到整个仓库。

## 与既有文档和约束的一致性检查

对照过：

- `AGENTS.md`、`docs/updatePLAN.md`、`docs/rfcs/pyc4.0-decisions.md`：语义留在方言和 MLIR。本次不改语义，只在 `designs/` 增加电路。
- `docs/PRIMITIVES.md`、`docs/v6_PyCircuit_Specification.md`：使用已有寄存器、组合运算、`byte_mem`、层次模块。
- `designs/XiangShan-pyc/docs/port_XiangShan_to_pyc.md`：重写而非翻译，与该文档一致。
- `docs/pycircuit_implementation_method.md`：按模块自底向上，每个叶子带测试。

无冲突。不把 Coral 的测试框架并进 pyCircuit 的默认 CI。

## 测试与验证计划

每个新增模块用 pyCircuit 测试台：

1. `tb_alu.py`：`add`、`sub`、`and`、`or`、`xor`、`sll`、`srl`、`sra`、`slt`、`sltu`，含 `x0` 和负数。
2. `tb_branch.py`：`beq`/`bne`/`blt`/`bge` taken 与 not-taken，以及 `jal`/`jalr` 的链接寄存器。
3. `tb_nop.py`：对应 Coral `tests/cocotb/nop_test.cc` 的行为——若干 `nop` 后写 `tohost`，检查停机且存储器内容正确。
4. S2 增加 `tb_load_store.py`：字节、半字、字的存取，地址在 DTCM 内。
5. S3 用 S1 的同一批程序再跑一遍，断言架构状态不变。
6. 每步运行该目录下的 pyCircuit 仿真入口（与 `designs/XiangShan-pyc` 相同的 emit/sim 方式）。不测量性能；这是功能移植。

不能迁移、因此不作为验收的部分：UVM、riscv-dv 随机指令、FreeRTOS、DMA、FPGA、浮点协仿真。原因是它们依赖 Bazel 与 Coral 的仿真器，不检查 pyCircuit 电路。

## 实施步骤

1. 从 `feat/change-driven-scheduler` 建 `tmp/coralnpu-pyc`。
2. S0：空顶层可 elaboration。
3. S1：ALU、寄存器堆、译码、单发射核、`tb_alu` / `tb_branch` / `tb_nop`。
4. S2：TCM 与 LSU，乘除。
5. S3：4 发射与记分板，复跑 S1 程序。
6. S4：整数 RVV 子集。
7. S5：等 Coral 上游出现可运行的矩阵 RTL，或按 `doc/overview.md` 的 INT8 GEMM 规格单独写 PE。不在 S1–S4 里夹带。

每阶段单独提交。未批准前不创建分支、不写设计代码。

## 执行记录

用户要求方案写完后直接按推荐项实施，不再单独点头。已在 `tmp/coralnpu-pyc` 完成 S0 与 S1。

S1 的三条 pyCircuit 测试（`tb_nop`、`tb_alu`、`tb_branch`）均通过 C++ 仿真。寄存器下一态在 `domain.next()` 之前算好，避免多插入一拍平衡寄存器。Coral 原版 `nop_test.cc` 里的 CSR 与 512×100 空转没有搬进来；对应测试是几条 `nop` 之后写 `tohost`。

S2 已接上。DTCM 在 `0x00010000`，32 KB，用 `byte_mem` 组合读。字节、半字、字以及非对齐字都按字节写入，不因跨 16 字节行而停机。地址 0 的字存储仍是 `tohost`。乘法结果在第二条拍写回。除法按 Coral DVU 的恢复除法每拍一位，共 32 步，加上取指和写回一共 34 拍。`tb_load_store` 与 `tb_muldiv` 已通过 C++ 仿真，S1 三条测试仍然通过。

S3 已接上。取指窗口一拍最多发出四条整数指令。窗口里后面的指令如果读或写前面一条刚写的寄存器，就留到下一组。分支、跳转、访存、乘除都会结束这一组，按顺序退休。同一批程序的 `tohost` 没有变。为了不超过逻辑深度上限，ALU 结果和发射掩码先寄存一拍再提交。

## 待确认事项

### 待确认 1：第一批交付做到哪一层

**为何现在必须决定：**

Coral 当前源码是一颗 4 发射 RV32 核，加上一套完整的 RVV 1.0 向量后端（大量 SystemVerilog），文档里的矩阵引擎还没有可运行的 PE 阵列。pyCircuit 仓库里没有这条核。若不确定第一批做到标量、向量还是矩阵，实现会要么停在空壳，要么试图一次搬完约 150 个 Chisel 文件和 160 个 SystemVerilog 文件，测试也无法一次改写完。

**相关背景：**

Coral 已克隆到 `/home/lidongzhe/coralnpu`（`ddea421`）。计算结构是：标量核执行普通 RISC-V 指令；向量单元执行 RVV，`VLEN=128`；矩阵单元在概述里是 16 个 tile 上的外积 GEMM，但这份树里还没有对应 PE 源码。pyCircuit 将在当前分支 `feat/change-driven-scheduler` 上开 `tmp/coralnpu-pyc`，新代码只放在 `designs/CoralNPU-pyc/`。测试改成 pyCircuit 测试台，不搬 cocotb/UVM。原 Chisel 不编译进本仓库。

**选项与结果：**

- **选项 A：按 S0→S4 分阶段，批准后先做 S0 和 S1**
  - 将做：分支、空顶层（含向量口和矩阵 tile 骨架）、单发射 RV32I 整数核、三条迁移测试（nop、ALU、分支）。
  - 不做：四发射、乘除、TCM 字节访存、RVV 数据通路、GEMM、AXI、DMA、浮点。这些留在后续阶段，每阶段再停一次给结果。
  - 结果：几天内能跑通整数小程序；架构文件里已经有向量和矩阵的位置，但是向量指令还不能算。
  - 风险：和「整颗 Coral 已迁移」的说法有距离，需要后续阶段继续。
- **选项 B：批准后连续做到 S3（含四发射、乘除和 DTCM），仍不做 RVV 数值**
  - 将做：选项 A 的全部，外加乘法器、除法器、ITCM/DTCM、load/store、4 发射记分板，以及 load/store 测试。
  - 不做：RVV 算术、矩阵 GEMM、AXI。
  - 结果：标量整数程序的架构状态可与 Coral 的标量核对照；工作量和调试时间明显大于选项 A，向量测试仍然迁不过来。
  - 风险：四发射记分板一旦和单发射行为不一致，S1 测试会在后期才失败。

**推荐：**

选项 A。先证明 pyCircuit 能跑 Coral 的整数子集和测试写法，再加发射宽度和向量。矩阵等上游有 PE 或单独开一阶段按概述里的 INT8 GEMM 做。
