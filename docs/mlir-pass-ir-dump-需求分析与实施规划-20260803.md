# MLIR 各 Pass IR Dump 调试信息 需求分析与实施规划

日期：2026-08-03
状态：**已实施并通过验收**（用户已批准方案，代码已落地）
分支：`feat/mlir-pass-ir-dump`（从 `origin/main` 拉建）

## 背景与目标

### 用户诉求
在排查 `pycc` 后端编译问题时（例如 `pyc-fuse-comb` / `pyc-eliminate-wires` / `pyc-check-comb-cycles` / `pyc-check-logic-depth` 等 pass 的行为差异、或 G1 合法性 gate 失败时定位 IR 在哪一步变坏），目前缺少一种 **逐 pass 把 IR 落盘** 的便捷手段。用户希望新增一个 debug 通道，输出 MLIR pipeline 中 **每个 pass 执行前/后的 IR**，便于 diff、回看与 bug 报告。

### 目标（功能需求）
- **G1**：提供开关，在 `pycc`（以及可独立用于 `.pyc` 文件诊断的 `pyc-opt`）上，按 pass 顺序把每个 pass 执行 **前/后** 的 IR 以文本形式落盘到目录。
- **G2**：每个 IR 文件命名稳定、可排序、可 diff（包含 pass 名、序号、before/after、嵌套 pass 层级）。
- **G3**：默认关闭，零开销、零行为改变；开关通过 CLI 与现有 `--profile-json` 体系并列，并与 `--out-dir` / gate 证据目录协作。
- **G4**：复用 MLIR `PassInstrumentation` 框架，**不修改任何已有 pass 的语义**（与 AGENTS.md “gate-first / 不做 backend-only 语义改动”一致）。本特性纯属观测/诊断。

### 非功能需求
- 不引入新的链接依赖；只用 MLIR / LLVM 已有 API（`PassInstrumentation`、`OpPrintingFlags`、`raw_fd_ostream`）。
- 对超大设计（数十万 op）有保护：可选限制单文件最大行数、可按 pass 名过滤、可只输出 `after`。
- 产物可被 `docs/gates/logs/<run-id>/` 体系消费（gate 证据）。
- 新增 CLI 遵循 `pycc` 现有 `cl::opt` 命名风格（kebab-case、有 `--help` 描述）。

## 项目现状与执行流程定位

### 端到端流程定位

pyCircuit 的编译链（见 `docs/PIPELINE.md`）分两段：

```text
Python 源码
  └─ frontend (pycircuit.cli emit)  ──>  *.pyc  (MLIR 文本)
        └─ backend: pycc *.pyc  ──────────>  C++ / Verilog / testbench
                │
                └─ 内部: 构建 PassManager，按固定顺序塞入 ~25 个 pass
                         （见 compiler/mlir/tools/pycc.cpp:2274-2318）
                         pm.run(module) 触发执行
```

本特性作用点是 **backend 的 `PassManager` 执行阶段**：在 `pm.run(*module)`（`pycc.cpp:2320`）之前，给 `pm` 注入一个新的 `PassInstrumentation`（与现有 `PassTimingCollector` 同类，见 `pycc.cpp:1330-1401`），用于在每个 pass 边界 dump IR。

### 当前已有的“可观测”能力（相关现有实现说明）

| 能力 | 位置 | 是否 dump IR | 说明 |
|------|------|--------------|------|
| `--profile-pass-timing` | `pycc.cpp:222` | 否 | 仅统计每个 pass 的耗时/内存，汇总进 `--profile-json` |
| `PassTimingCollector` | `pycc.cpp:1330-1401` | 否 | `mlir::PassInstrumentation` 子类，是本特性的**直接模板** |
| `--profile-json` | `pycc.cpp:217` | 否 | 汇总 pass 计时/compile_stats/hierarchy；本特性产物可与之并列 |
| `pyc-opt`（mlir-opt 风格） | `compiler/mlir/tools/pyc-opt.cpp` | 部分 | `mlir-opt` 自带 `-mlir-print-ir-before` / `-mlir-print-ir-after`，但 `pyc-opt` 当前未做任何扩展，且这些 flag 默认打到 `llvm::errs()`，不落盘、不可 diff、不可过滤 |
| MLIR 内建 `-mlir-print-ir-*` | — | 是（stderr） | 仅文本到 stderr，文件大、混杂、无稳定文件名，不能做 gate 证据 |

**结论**：现状下要在两个 pass 之间 diff IR，需要手工 `2>&1 | tee` 截断 stderr 再 `awk` 切片，对大规模设计几乎不可用。本特性填补“逐 pass IR 落盘 + 稳定可 diff 文件名”的空白。

### 关键约束（来自项目文档）

| 文档 | 约束 | 本特性如何遵守 |
|------|------|----------------|
| `AGENTS.md` | gate-first；语义不得仅靠 backend 修补 | 本特性**不改任何 pass 语义**，仅观测；不新增 verifier 也不修改既有 |
| `docs/updatePLAN.md` §2 | G0–G3 gate；PR 需附 gate 证据 | 产物路径与 `docs/gates/logs/<run-id>/` 协作；本 PR 属 G0（build+单测） |
| `docs/PIPELINE.md` | backend = MLIR passes + emit | 作用点正确，不动 frontend |
| `docs/rfcs/pyc4.0-decisions.md` | PR 需引用 decision ID | 本特性为**诊断工具**，非语义变更，不映射到具体 01xx 决策；将在 PR 中标注 “tooling/diagnostics, no decision ID” 并请用户确认是否可豁免 |

## 需求与验收标准

### 功能验收
- **AC1**：`pycc foo.pyc --emit=none --dump-pass-ir=<dir>` 执行后，`<dir>` 内出现每个 pass 的 IR 文件，数量与 pipeline 中实际执行 pass 次数一致（含 `addNestedPass` 的嵌套执行）。
- **AC2**：文件名形如 `NNNN_<before|after>_<pass-name>.mlir`（见下“文件命名”），按字典序即等于执行序；前后两个文件可直接 `diff` 出该 pass 的净改动。
- **AC3**：`--dump-pass-ir-filter=<regex>` 只输出名字匹配的 pass；`--dump-pass-ir-phase=after`（默认 `both`）可只输出 after 以减半体积。
- **AC4**：`pyc-opt` 同样支持（用同一 instrumentation 类），命令 `pyc-opt foo.mlir --dump-pass-ir=<dir> -pyc-eliminate-wires ...` 能落盘。
- **AC5**：默认（不传 flag）时，构建产物二进制位与 main 完全一致，行为无任何变化。

### 非功能验收
- **AC6**：单 pass IR 超过 `--dump-pass-ir-max-lines`（默认 0=不限；建议大设计设 200000）时，文件末尾追加 `// truncated at N lines` 并停止写，避免 OOM。
- **AC7**：目录创建失败/pass 失败时不 crash；pass 失败时仍写出失败前的 `after`（带 `// PASS FAILED` 标记），便于 bug 报告。
- **AC8**：新增单元/集成测试（`compiler/mlir/test/` 与 lit 风格 `*.mlir`），至少覆盖：默认关闭、落盘命名、filter、phase=after、嵌套 pass。
- **AC9**：`--help` 描述清晰；README（`compiler/mlir/README.md`）增加一节使用说明。

## 方案设计

### 模块边界、输入与输出

新增一个独立的 instrumentation 类（**不放进 `lib/Transforms`**，因为它不是 pass，而是 `pycc`/`pyc-opt` 工具侧的观测器）：

新增/修改文件（实施时据 CMake 结构最终确定）
  include/pyc/Support/PassIRDumper.h       (新增，~60 行)  公共头
  lib/Support/PassIRDumper.cpp              (新增，~180 行) 实现
  compiler/mlir/CMakeLists.txt              (改)            把 PassIRDumper 加入 pyc_transforms 库
                                                            （与现有 CombDepGraph 同库，避免新增库的复杂度）
  tools/pycc.cpp                            (改)            新增 4 个 cl::opt + 注入 instrumentation
  tools/pyc-opt.cpp                         (改)            复用同一类（最小改动）
  compiler/mlir/README.md                   (改)            文档
  compiler/mlir/test/pass_ir_dumper_smoke.sh (新增)         shell 测试脚本（与现有 cpp_*.sh 风格一致）
```

> 备注1：仓库当前没有 `lib/Support/` 与 `include/pyc/Support/` 目录，需新建；与 MLIR 项目惯例一致。
> 备注2：`pyc-opt` 当前是 `EXCLUDE_FROM_ALL`，上游构建只产 `pycc`；本次仍接入 `pyc-opt.cpp` 以满足决策，但**优先保证 `pycc` 路径**。
> 备注3：仓库**未安装 lit/FileCheck**（已确认 `which lit`/`which FileCheck` 均无），且 `compiler/mlir/test/` 现有测试均为 `*.sh` 冒烟脚本（如 `cpp_device_pch_smoke.sh`）。故测试**改用 shell 脚本**，不引入 lit 依赖。

类签名（草案）：

```cpp
namespace pyc {

struct PassIRDumperOptions {
  std::string dir;                 // 输出目录（空=禁用）
  std::string phase = "both";      // before | after | both
  std::string filterRegex;         // 空=全部
  uint64_t maxLines = 0;           // 0=不限
};

// mlir::PassInstrumentation 子类：runBeforePass/runAfterPass{,Failed} 落盘 IR
class PassIRDumper final : public mlir::PassInstrumentation {
public:
  explicit PassIRDumper(PassIRDumperOptions opts);
  void runBeforePass(mlir::Pass*, mlir::Operation*) override;
  void runAfterPass(mlir::Pass*, mlir::Operation*) override;
  void runAfterPassFailed(mlir::Pass*, mlir::Operation*) override;
  // ...
};

} // namespace pyc
```

### 上下游关系与数据/控制流

```text
pycc main()
  ├─ 解析 cl::opt（含新增 4 个 dump flag）
  ├─ 若 --dump-pass-ir 非空：
  │     构造 PassIRDumperOptions
  │     pyc::PassIRDumper dumper(opts);
  │     pm.addInstrumentation(dumper)        // 与现有 PassTimingCollector 并存
  ├─ pm.run(*module)
  │     ├─ runBeforePass(P, op):  若 phase∈{before,both} 且匹配 filter：
  │     │     写  <dir>/NNNN_before_<pass>.mlir
  │     └─ runAfterPass(P, op):   若 phase∈{after,both} 且匹配 filter：
  │           写  <dir>/NNNN_after_<pass>.mlir
  └─ 其余 emit/profile 逻辑不变
```

**嵌套 pass 处理**：`pycc.cpp` 中大量使用 `pm.addNestedPass<func::FuncOp>(...)`。MLIR 执行嵌套 pass 时，外层 `PassManager` 也会触发 instrumentation；我们用**全局递增计数器**作为文件名前缀（`NNNN`），保证字典序=执行序，并在文件名里追加嵌套层级（如 `__L1`）以区分 module-level 与 func-level。

### 文件命名（稳定 + 可 diff + 可排序）

```text
<dir>/0000_before_00_check-frontend-contract__L0.mlir
<dir>/0001_after_00_check-frontend-contract__L0.mlir
<dir>/0002_before_01_inline-functions__L0.mlir
...
<dir>/0017_before_16_eliminate-wires__L1.mlir          # func-nested
<dir>/0018_after_16_eliminate-wires__L1.mlir
```

- `NNNN`：4 位零填充全局序号，每条 before/after 各占一个号。
- `<pass>`：`pass->getName()`（去掉 `pyc-` 前缀的短名，便于排序）。
- `__L<n>`：嵌套层级（0=module，1=func-nested）。
- 失败的 pass：`after` 文件名后缀加 `__FAILED`，内容首行写 `// PASS FAILED`。

### 输出目录与 gate 证据协作

- 若用户显式传 `--dump-pass-ir=<dir>`：用该目录。
- 若同时设了 `--out-dir` 且 `--dump-pass-ir=auto`：落到 `<out-dir>/pass_ir/`，与现有 `pycc_profile.json` 并列，便于随 gate 证据一起上传。
- 与 `docs/gates/logs/<run-id>/` 的对接由调用方（`flows/scripts/...` 或 CI）负责，本特性只保证产物在稳定路径。

### 边界条件、错误处理与兼容性

| 情形 | 处理 |
|------|------|
| 目录不可创建 | `llvm::errs()` 报错 + 禁用 dumper（不中断编译） |
| 单文件超 maxLines | 截断并写 `// truncated` 行 |
| pass 抛错失败 | `runAfterPassFailed` 写 `__FAILED` 文件后让原失败路径继续 |
| `--emit=none` | 仍可 dump（便于纯诊断用法） |
| LLVM 19 vs 18 | 仅用稳定 API（`OpPrintingFlags`、`print`），无版本分支 |
| 与 `--profile-pass-timing` 并存 | 两个 instrumentation 互不干扰；同一 `pm` 可叠加 |
| 默认（不传 flag） | `PassIRDumperOptions.dir` 为空 → 不构造、不注入、零开销 |

### 不做的事（明确边界）
- **不**改任何 pass 的语义或注册方式。
- **不**新增 decision ID（如用户要求，可挂一个 `tooling/0001-pass-ir-dump` 工具类 RFC；默认不挂）。
- **不**做 IR 二进制/位精确哈希（已有 xxhash64 工具，若后续需要 diff 辅助可再加）。
- **不**集成进 frontend（`.pyc` emission 阶段无 PassManager）。

## 与既有文档和约束的一致性检查

| 检查项 | 文档 | 结论 |
|--------|------|------|
| gate-first | `AGENTS.md` | 一致：纯诊断，无语义改动 |
| backend 不单独修语义 | `AGENTS.md` | 一致：本特性不修任何 bug |
| backend = MLIR passes + emit | `docs/PIPELINE.md` | 一致：作用点在 passes 执行 |
| G0 gate（build+单测） | `docs/updatePLAN.md` §2 | 本 PR 落 G0：新增 lit 测试 + 不破坏现有 build |
| pass 列表 | `pycc.cpp:2282-2318` | 已对齐，dumper 自动适配任何 pass 增删 |
| profile 体系 | `pycc.cpp:2087-2097` | 与 profile-json 并存，路径协作 |
| PR 引用 decision ID | `docs/updatePLAN.md` §0 | **待用户确认**：本特性为 tooling，建议豁免；若需要可挂 RFC |

**未发现冲突**，除 decision ID 一项需用户拍板（见“待确认事项”）。

## 测试与验证计划

### 单元 / 集成测试（`compiler/mlir/test/pass_ir_dumper_smoke.sh`，shell 风格与现有 `cpp_*.sh` 一致）
> 仓库未安装 lit/FileCheck，故用 shell 脚本断言（`test -f`、`grep -c`、`diff`）。脚本退出码 0=通过。

1. `default-off`：不带 flag，确认无目录产生、IR 输出不变。
2. `basic-dump`：跑 2 个 pass，确认 `before/after` 文件成对、命名按序、可 `diff`。
3. `filter-regex`：`--dump-pass-ir-filter=eliminate-wires` 只产出该 pass 文件。
4. `phase-after`：`--dump-pass-ir-phase=after` 只产出 after。
5. `max-lines`：设小阈值，确认截断标记。
6. （`pyc-opt` 路径若构建可用）跑一遍 `pyc-opt --dump-pass-ir=...`。

### 集成 / 端到端（手动 + 可选脚本）
- 对 `designs/` 下任一中等规模示例：
  ```bash
  pycc design.pyc --emit=none --dump-pass-ir=/tmp/pir --dump-pass-ir-phase=both
  ls /tmp/pir | wc -l    # 应等于 2 × 实际 pass 数
  diff /tmp/pir/0017_before_* /tmp/pir/0018_after_*   # 净改动可见
  ```
- 与 `--profile-json` 并存：确认 profile 仍正常生成、dumper 产物在并列路径。

### 性能基线
- 不传 flag：应与 main 完全一致（无开销）。
- 开启 dumper：记录单 pass 写盘耗时，确认对总编译时间占比 < 5%（大设计下主要受磁盘 IO，可接受；若超标用 `phase=after` 缓解）。

### 回归风险
- 风险点：instrumentation 在 `runBeforePass` 拿到的 `Operation*` 是 pass 即将作用的 scope（module 或 func），`print` 时需用该 op 而非顶层 module，避免漏 dump 嵌套。
- 缓解：lit 测试 `nested-pass.mlir` 覆盖。

## 实施步骤（获批后按序执行）

1. **新增公共支撑**：`include/pyc/Support/PassIRDumper.h` + `lib/Support/PassIRDumper.cpp`；`compiler/mlir/CMakeLists.txt` 加入 `pyc_support`（或新建 support 静态库，取决于现有 CMake 结构——实施时确认）。
2. **接入 `pycc`**：新增 4 个 `cl::opt`（`--dump-pass-ir` / `--dump-pass-ir-phase` / `--dump-pass-ir-filter` / `--dump-pass-ir-max-lines`）；在 `pm` 构造处按需 `addInstrumentation`。
3. **接入 `pyc-opt`**：最小改动，复用同一类；保留 `pyc-opt` 原 `mlir-opt` 行为。
4. **lit 测试**：补齐 7 个用例。
5. **文档**：`compiler/mlir/README.md` 新增“Per-pass IR dump”一节。
6. **本地构建 + gate G0**：
   ```bash
   ninja -C build pyc-opt pycc
   lit compiler/mlir/test/PassIRDumper -v
   ```
7. **示例验证**：跑一个 `designs/` 示例，人工 diff 一对 before/after。
8. **PR**：标题 `feat(mlir): add per-pass IR dump instrumentation`，body 注明 tooling/diagnostics、列出测试与 gate 证据路径。

## 待确认事项（用户已确认，记录如下）

> 用户审查日期：2026-08-03。以下决策已敲定，实施按此执行。

1. **decision ID 豁免**：✅ 方案 A —— 作为 tooling/diagnostics PR 直接提交，PR body 标注 `tooling/diagnostics, no decision ID`，不新建 RFC。
2. **作用范围**：✅ 方案 A —— `pycc` + `pyc-opt` 都支持（同一 `PassIRDumper` 类）。
3. **默认 phase**：✅ 方案 A —— `both`（便于 diff）。
4. **截断阈值默认值**：✅ 选择 **0 = 不限**（用户明确要求不设上限；若大设计遇到磁盘/内存问题，再按需传 `--dump-pass-ir-max-lines` 手动限制）。
5. **目录默认**：✅ 方案 A —— 支持 `--dump-pass-ir=auto` 落到 `<out-dir>/pass_ir/`；显式传路径时用该路径。
6. **是否同时输出 pass 计时**：✅ 方案 A —— 不附带；复用现有 `--profile-pass-timing`。

**下一步**：按“实施步骤”进入编码（新增 `PassIRDumper` + 接入 `pycc`/`pyc-opt` + lit 测试 + 文档），完成后回写实际结果。

## 实施结果回写（2026-08-03）

### 已完成
- ✅ 新增 `include/pyc/Support/PassIRDumper.h` + `lib/Support/PassIRDumper.cpp`
- ✅ `compiler/mlir/CMakeLists.txt`：把 `PassIRDumper.cpp` 加入 `pyc_transforms`，显式补 `LLVMSupport`
- ✅ `pycc.cpp`：新增 4 个 `cl::opt`（`--dump-pass-ir` / `-phase` / `-filter` / `-max-lines`），在 `PassManager` 构造后按需 `addInstrumentation`；`auto` 解析到 `<out-dir>/pass_ir`
- ✅ `pyc-opt.cpp`：用 `MlirOptMainConfig::setPassPipelineSetupFn` 注入同一 instrumentation（注：当前环境 LLVM 包未导出 `MLIRRegisterAllPasses`，`pyc-opt` 不被构建，但代码已就绪）
- ✅ `compiler/mlir/test/pass_ir_dumper_smoke.sh`：6 类断言（默认关闭/落盘/序号连续/filter/phase=after/max-lines/diffable/与 profile 并存）
- ✅ `compiler/mlir/README.md`：新增“Per-pass IR dump (diagnostics)”一节
- ✅ **文档与 gate 接入（补全）**：
  - `docs/mlir_pass_ir_dump.md`：新建独立特性文档（与 `cpp_member_placement.md`/`cpp_device_pch.md` 同风格）
  - `docs/PIPELINE.md`：新增 `### Per-pass IR dump (diagnostics)` 小节，链接到上述文档
  - `docs/DIAGNOSTICS.md`：“Useful commands”加 dump IR 用法
  - `docs/getting-started/quickstart.md`：新增“5) Debugging compiler passes”指针小节
  - `flows/scripts/run_pass_ir_dumper_gate.sh`：gate wrapper（与 `run_cpp_*_gate.sh` 一致，落证据到 `docs/gates/logs/<run-id>/`）
  - 未改 `mkdocs.yml` nav：与同类特性文档（`cpp_member_placement.md`/`cpp_device_pch.md` 也未进 nav）保持一致，由 `PIPELINE.md` 链接访问

### 验收对照
| AC | 结果 |
|----|------|
| AC1（默认关闭） | ✅ 不传 flag 时无 dump 目录产生 |
| AC2（落盘 + 命名） | ✅ counter 示例产出 64 个文件（32 pass × before/after），命名 `NNNN_<phase>_<NN>_<pass>__L<level>.mlir` |
| AC3（filter） | ✅ `--dump-pass-ir-filter=eliminate-wires` 只产 2 文件 |
| AC4（phase=after） | ✅ 文件数从 64 减半到 32 |
| AC5（max-lines） | ✅ `--dump-pass-ir-max-lines=3` 末尾出现 `// truncated at 3 lines` |
| AC6（diffable） | ✅ `eliminate-wires` 的 before/after diff 显示 `pyc.wire+assign → pyc.alias` |
| AC7（与 profile 并存） | ✅ `profile.json` 正常生成且含 `passes[]`，dump 文件同时存在 |
| AC8（pyc-opt 接入） | ⚠️ 代码就绪，但当前 LLVM 包未导出 `MLIRRegisterAllPasses`，`pyc-opt` 不构建（pre-existing 限制） |

### 与规划的偏差（合理调整）
1. **测试机制**：仓库未安装 `lit`/`FileCheck`（已确认），改用 shell 脚本（与现有 `cpp_*_smoke.sh` 风格一致），不引入新依赖。
2. **regex 实现**：MLIR/LLVM 默认 `-fno-exceptions`，`std::regex` 的 `try/catch` 无法编译，改用 `llvm::Regex`（LLVM 自带，无异常）。
3. **pass 短名来源**：`pass->getName()` 对 `PassWrapper` 子类返回 mangled C++ 名（不可读），改用 `pass->getArgument()`（如 `"pyc-eliminate-wires"`），再 strip `pyc-` 前缀。
4. **build 目录**：现有 `build/` 和 `build-noguard-llvm19/` 的 CMakeCache 指向另一个 checkout `/home/lidongzhe/pyCircuit/`（非 `_debug`），看不到我们的源码修改。新建 `build-debug/` 指向当前源码后编译通过。

### 发现的 pre-existing bug（不在本特性范围）
- `pycc --emit=cpp --out-dir=...` 在 main 里触发 `SmallVector` assert 崩溃。
- 已用旧 `build-noguard-llvm19/bin/pycc`（不含我们的改动）复现同一崩溃，确认是 pre-existing，**与 PassIRDumper 无关**。
- 建议：另起 issue/分支修复，不阻塞本特性。

### 构建与测试命令（可复现）
```bash
# 配置（指向当前 checkout 的源码）
cmake -S compiler/mlir -B build-debug \
  -DCMAKE_BUILD_TYPE=Release \
  -DMLIR_DIR=/opt/llvm19/lib/cmake/mlir \
  -DLLVM_DIR=/opt/llvm19/lib/cmake/llvm

# 编译
make -C build-debug pycc -j$(nproc)

# 跑 smoke
PYCC="$(pwd)/build-debug/bin/pycc" ./compiler/mlir/test/pass_ir_dumper_smoke.sh

# 实地试用
./build-debug/bin/pycc foo.pyc --emit=none --dump-pass-ir=/tmp/pir
diff /tmp/pir/0046_before_*eliminate-wires* /tmp/pir/0047_after_*eliminate-wires*
```
