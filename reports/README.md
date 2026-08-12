# pyCircuit Emitter、RTL 仿真与资源优化调研

> 调研快照：2026-08-04，基于当前工作树。本文档集聚焦 `Emitter → Verilog → Verilator/Yosys`，并向前追溯会改变最终硬件结构的 frontend/MLIR 决策。

## 先读结论

1. 项目里的 **Verilator 才是 RTL 仿真器**；`pycc` 生成的 Yosys 脚本只是综合 sanity check。把后者称作“Yosys 仿真”会混淆功能正确性和 PPA 分析。
2. Verilog Emitter 基本是机械式 lowering。寄存器数量、存储器实现、mux 拓扑和流水级通常在进入 Emitter 前已经决定；因此资源优化不能只改字符串输出器。
3. 现有 `compile_stats.json` 是 MLIR 中所有 `func.func` 定义的静态汇总，不是从指定 top 展开的实例面积，也不是 Yosys cell/LUT/FF/BRAM 统计。
4. 当前 XiangShan-pyc 产物不能直接作为“与 Chisel 同设计”的 PPA 基线：README 明确它是按规范重新实现；旧 hierarchy wrapper 只是把子模块并排实例化并暴露所有端口，没有连接真实数据通路。
5. 已找到的高优先级资源风险是：
   - register file 展开为大量可复位 FF、全表比较器和线性 mux；
   - cycle-aware 对齐寄存器逐次插入，且每级都带同步复位；
   - one-hot 选择若按普通 priority mux 生成，ABC 映射可能显著变差；
   - memory 是否保持为 memory/BRAM/macro，远比 RTL 行数重要；
   - 现有优化 pass 对算术向量和大规模选择结构的覆盖有限。
6. 目前还不能诚实地给出“各原因占面积差的百分比”：本环境没有 Yosys，而且仓库中没有一组已证明参数、时序、复位、存储器和黑盒都等价的 pyCircuit/Chisel/手写三方基线。

## 主报告

- **[最终主报告：Verilog 生成、RTL 验证、Yosys 综合与资源问题](FINAL_emitter_verilog_yosys_report.md)**：以 Counter 为贯穿例子，收束 Emitter、生成 RTL、Verilator、Yosys 和资源优化主线。若只读一份，建议读这一份。

## 专题附录（需要时查阅）

- [00：项目与 RTL 工具链入门总览](00_project_overview_guidance.md)：从基本术语、编译器类比和 Counter 例子建立完整心智模型，并给出仓库代码地图。概念不熟时查阅。
- [01：从 Emitter 到 Verilog 的结构](01_verilog_emitter_pipeline.md)：代码入口、pass pipeline、Verilog 组织和 runtime primitives。
- [02：Verilator 与 Yosys 流程](02_yosys_verilator_flow.md)：仿真/综合边界、当前脚本能力、可复现的公平比较方法。
- [03：资源差距根因分析](03_resource_gap_root_causes.md)：证据分级、已确认问题、非问题和待验证假设。
- [04：gate-first 优化路线](04_optimization_roadmap.md)：先补语义/度量门禁，再做结构优化和 PPA 回归。
- [05：Verilog、资源与 Yosys 反串讲讲稿](05_talk_script_verilog_yosys_resources.md)：35–45 分钟逐页讲稿，含代码展示、现场演示、常见问答和 20 分钟精简版。
- [06：仓库代码解读](06_repository_code_walkthrough.md)：按 `frontend → MLIR dialect/passes → Verilog/C++ Emitter → runtime → gates` 的调用链逐层解读核心文件，并附纵向例子、问题定位索引和推荐阅读顺序。
- [07：生成 C++ Model 的结构与性能分析](07_cpp_model_performance_analysis.md)：统计 30 个样例的生成结构，通过 Counter、BF16 FMAC、SW5809S 微基准解释 O0/O2、全网表求值、边沿调度、FIFO copy、缓存与 trace 的性能问题，并给出 gate-first 优化顺序。
- [08：C++ Model 与 Verilator 性能对比实验](08_cpp_vs_verilator_performance.md)（[交互式 HTML](08_cpp_vs_verilator_performance.html)）：从同一份最新 PYC IR 重生成 30 组 C++/Verilog，统一 GCC O2、CPU、刺激和观察方式，给出完整吞吐结果、跨后端 checksum 门禁、全部 30 case 的 perf stat，以及 Counter、BF16 FMAC、SW5809S 的 perf record/gprof 瓶颈分析。

## 证据等级

- **已确认**：可由当前源码或已提交产物直接证明。
- **强推断**：结构与综合原理明确，但仍需统一 Yosys/技术库 A/B 数据量化。
- **待验证**：合理假设，不能在没有实验时当结论。

本报告遵守项目的 pyc4.0 约束：MLIR 是语义唯一来源，语义修改必须先有 dialect/verifier/pass 与跨后端门禁。依据见 [updatePLAN](../docs/updatePLAN.md) 和 [pyc4.0 decisions](../docs/rfcs/pyc4.0-decisions.md#decision-0112-mlir-dialect-is-the-single-semantic-source-c-sim-and-verilog-emission-must-be-logically-equivalent)。

## 本次验证边界

- 已完整审阅强制文档、Emitter、`pycc` pipeline、Verilog primitives、CLI testbench/Verilator 路径、主要优化 pass、cycle-aware 对齐逻辑及代表性设计产物。
- 当前环境存在 `/usr/local/bin/verilator`，但没有 `yosys`，因此本报告没有伪造综合数字。
- 仓库要求的 `$pyc4`、`$pyc-build-v40`、`$linx-pycircuit` 技能未在当前会话可用；本次直接按其对应的 decisions、gate-first 和“不做 backend-only semantic fix”约束执行。此次没有修改 Linx 流程。
