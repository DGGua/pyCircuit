# Verilator 仿真与 Yosys 综合/资源分析

## 1. 两个工具的职责

| 工具 | 当前项目用途 | 能回答的问题 | 不能直接回答的问题 |
|---|---|---|---|
| Verilator | 编译并运行 SV testbench/DUT，生成 VCD | 周期功能、reset、pre/post observation 是否正确 | LUT/FF/BRAM 或 ASIC 面积 |
| Yosys | 读入 RTL、process/memory lowering、generic/target synthesis | 结构单元、memory 保留、映射后资源 | testbench 的时序功能是否正确 |
| `compile_stats.json` | pass 后 MLIR inventory | IR 中定义了多少 reg/memory bit、logic-depth proxy | 指定 top 的实例化面积或 technology-mapped PPA |

因此建议在项目术语中使用“Verilator RTL 仿真”和“Yosys 综合/统计”。

## 2. 当前 Verilator 路径

`pycircuit build` 的 canonical CLI 定义见 [cli.py:2693](../compiler/frontend/pycircuit/cli.py#L2693)：

```bash
PYTHONPATH=compiler/frontend \
python3 -m pycircuit.cli build path/to/design.py \
  --out-dir build/example \
  --target both \
  --run-verilator
```

构建器收集生成的 primitive 和 module Verilog，创建 `verilator_manifest.json`，再调用 `verilator --binary ... --timing --trace`，见 [cli.py:2508](../compiler/frontend/pycircuit/cli.py#L2508)。

SV testbench 的采样约定是：

- reset 在 negedge 解除，避免和 DUT 的 posedge state update 竞争；
- drive 在 posedge 前施加；
- pre-expect 在 drive 后 `#0` 采样；
- 等待 posedge 再到 negedge后做 post-expect；
- SVA 在 negedge 采样，观察 posedge 更新后的稳定值。

证据见 [cli.py:1696](../compiler/frontend/pycircuit/cli.py#L1696) 和 [cli.py:1731](../compiler/frontend/pycircuit/cli.py#L1731)。这与 Decision 0113 的 observation-point 目标大体对应，但报告/trace 中还应显式记录是 TICK-OBS 还是 XFER-OBS，避免靠 testbench 相位猜测。

## 3. 当前 Yosys 路径及局限

`pycc --out-dir ... --emit=verilog` 自动写出 [yosys_synth.ys](../compiler/mlir/tools/pycc.cpp#L2487)：

```tcl
read_verilog -sv pyc_primitives.v
read_verilog -sv <each-module>.v
hierarchy -top <top>
proc; opt; memory; opt
synth -top <top>
```

源码自己称它为 `sanity synth`。它当前没有：

- `hierarchy -check` / `check -assert`；
- `stat -json` 和 netlist 输出；
- 目标 FPGA family、ASIC Liberty、时钟/时序约束；
- memory macro/BRAM 映射策略；
- 对比所需的 Yosys 版本、脚本 hash 和参数 manifest。

因此它适合检查“能否综合”，不适合直接发表“比 Chisel 多 X% LUT/面积”的结论。

仓库中的 Yosys 自动测试也仅有两个 vector port smoke case，helper 在 [test_vec_ops.py:12](../tests/vec/test_vec_ops.py#L12)，且找不到 Yosys 时会 skip。当前没有 top-level resource regression lane。

## 4. 公平比较的前置合同

在比较 pyCircuit、Chisel、手写 RTL 前，每个 case 必须有同一份 machine-readable benchmark manifest：

| 字段 | 必须固定的内容 |
|---|---|
| top | 真正可达并已连接的 top module |
| parameters | entries、lanes、data width、port 数等全部展开值 |
| interface | port 名/宽度/方向、clock/reset polarity |
| sequential contract | 输入到输出 latency、enable、flush/stall 行为 |
| reset/init | 哪些 state bit 必须 reset，哪些为 don't-care |
| memory | depth/width/ports/read latency/RDW/byte strobe/init |
| blackboxes | SRAM、FIFO、multiplier 等是否视为 macro，面积如何记账 |
| synthesis | Yosys commit/version、script、defines、flatten、target family/Liberty |

如果任一项不同，得到的是“两个不同微结构的面积”，不是代码生成质量差距。

## 5. 建议的 Yosys cut-point 流程

以下是模板，不应直接覆盖现有脚本；先在独立 benchmark 流中验证所用 Yosys 版本支持各参数：

```tcl
read_verilog -sv pyc_primitives.v
read_verilog -sv design.v
hierarchy -check -top TOP

# Cut A: process lowering 后，memory 尚未完全技术映射
proc; opt_clean
check -assert
tee -o 10_proc_stat.json stat -json -top TOP

# Cut B: memory 收集后，检查 $mem 数量/宽深/端口
memory_dff; memory_collect; opt_clean
tee -o 20_memory_stat.json stat -json -top TOP

# Cut C: generic synthesis；所有候选使用相同命令
synth -flatten -top TOP
opt_clean -purge
check -assert
tee -o 30_generic_stat.json stat -json -top TOP
write_json 30_generic_netlist.json
```

目标映射应另开流程：

- FPGA：对同一 family 使用同一 `synth_xilinx`、`synth_intel` 等命令，记录 LUT、FF、BRAM、DSP；
- ASIC：先 `synth -noabc`，再对同一 Liberty 做 `dfflibmap`/`abc -liberty`/`stat -liberty`；
- memory macro：保留 `$mem` 到 macro mapping，或对三方同时把 macro 面积外部计入。不要让一方保留 SRAM、另一方被展开成 FF+mux。

建议至少保存四类指标：

1. sequential bit 与 reset/enable 类型；
2. `$mem` 数量、总 bit、读写端口和是否映射成 BRAM/macro；
3. generic `$mux/$eq/$add/$mul` 等 cell 数与宽度；
4. 映射后的 LUT/FF/BRAM/DSP 或标准单元面积，以及外部 STA 的关键路径。

## 6. 定位差异的 A/B 方法

```text
source model
   │ A：frontend raw MLIR
   │ B：pycc pass 后 MLIR
   │ C：emitted RTL / Yosys proc 后
   │ D：memory collect 后
   │ E：generic synth 后
   └ F：technology map 后
```

- A 就变大：authoring/cycle inference/数据结构问题。
- A 正常、B 变大：lowering 或 pass 结构问题。
- B 正常、C 变大：Emitter/primitive pattern 问题。
- C 正常、D 变大：memory inference/端口语义问题。
- D 正常、F 变大：ABC/tech mapping/约束问题。

每次实验只改一个因素，并保存 source commit、artifact hash、Yosys version、完整日志和 JSON。这样才能把“看起来像”变成可回归的证据。

## 7. 当前产物为什么不能直接作面积报告

`CollectCompileStatsPass` 只按函数统计 `pyc.reg` 和三类 memory，见 [CollectCompileStatsPass.cpp:52](../compiler/mlir/lib/Transforms/CollectCompileStatsPass.cpp#L52)；随后 `pycc` 对 module 中每个函数定义求和，见 [pycc.cpp:1944](../compiler/mlir/tools/pycc.cpp#L1944)。它不会按 top 的 instance multiplicity 展开，也会把不可达函数定义算进去。

例如已提交的 [xs_core compile_stats](../designs/XiangShan-pyc/build/xs_core/verilog/compile_stats.json) 报告 5341 个 reg / 65073 bit、24 个 memory / 1619968 bit；但同目录 [xs_core.v](../designs/XiangShan-pyc/build/xs_core/verilog/xs_core.v) 只有 13 个直接 `pyc_reg`，且没有实例化 `frontend/backend/memblock`。Yosys `hierarchy -top xs_core` 会删除那些不可达 module。因此两套数字回答的是不同问题。

后续应把现有文件重命名/标注为 `ir_definition_inventory`，另生成：

- `reachable_instance_stats.json`：从 top 展开、计 instance multiplicity；
- `yosys_generic_stats.json`：综合后结构；
- `target_resource_stats.json`：指定技术映射后的真实资源。

