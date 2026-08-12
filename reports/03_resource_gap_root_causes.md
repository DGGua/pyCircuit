# 生成 Verilog 相对 Chisel/手写 RTL 的资源差距分析

## 1. 总体判断

目前能确认“存在高风险结构”，但不能确认一个统一的“pyCircuit 固有面积倍率”。资源差往往来自以下乘法关系：

```text
状态/算法选择
  × 自动流水与 reset 策略
  × mux/比较网络拓扑
  × memory 是否成功推断
  × hierarchy/constant propagation
  × 目标技术映射
```

Emitter 的 RTL 文本风格只占其中一部分，而且许多文本冗余会被 Yosys 消掉。

## 2. 根因证据矩阵

| 项目 | 当前证据 | 判断 | 优先验证方式 |
|---|---|---|---|
| register file 用 FF 阵列 + 全表读 mux | 源码直接证明 | 已确认的高风险结构 | 与等价 banked SRAM/macro 方案做同脚本 A/B |
| cycle-aware 自动对齐产生大量寄存器 | `delay_to()` 和 Bypass raw MLIR 证明 | 已确认；是否“冗余”需逐 case 判断 | 按来源/宽度/周期边统计，做共享与 reset A/B |
| 所有 balance reg 都同步 reset | `m.out(... init=0)` + `pyc_reg` | 已确认 | 保持观测语义下比较 resettable/non-resettable state |
| 普通 priority mux 映射差 | frontend 注释记录 ABC 实测现象 | 强证据，仍需当前版本复测 | one-hot proven true/false，固定 Yosys/ABC A/B |
| vector `v_get` 扰动 ABC | frontend 注释和生成形状 | 强推断 | proc 后 netlist 对比 scalar chain/vector-backed chain |
| packed/unpacked bridge 增面积 | 只是逐 bit assign | 通常不是直接原因 | `proc; opt_clean` 后确认 bridge cell 已消失 |
| 每个 op 都声明 wire 增面积 | RTL 文本确实增大 | 通常不是综合网表原因 | 比较 RTL LOC 与 post-opt cell，避免混为一谈 |
| `pyc.assert` 增面积 | 被 `SYNTHESIS` 排除 | 否 | 无需作为优化目标 |
| generic Yosys memory 展开导致面积大 | 当前脚本无 target/macro policy | 待验证但风险高 | pre-memory、post-memory、target mapping 三段统计 |
| pyCircuit 比 Chisel 本身低效 | 当前没有等价三方基线 | 尚未证明 | 先完成 benchmark contract 和双后端等价 gate |

## 3. 根因一：register file 的结构选择

通用 library 版本把存储显式写成两组 32-bit state register，见 [lib/regfile.py:65](../compiler/frontend/pycircuit/lib/regfile.py#L65)；每个 read port 遍历所有 entry，生成 equality + mux chain，见 [lib/regfile.py:91](../compiler/frontend/pycircuit/lib/regfile.py#L91)。每个 write port也对每个 entry 做地址比较并更新。

XiangShan-pyc 版本更明显：默认 224×64-bit state、14 个组合读端口、8 个写端口，见 [regfile.py:1](../designs/XiangShan-pyc/backend/regfile/regfile.py#L1)。源码结构是：

```python
regs = [domain.signal(width=64, reset_value=0) for i in range(224)]
for read_port in range(14):
    for entry in range(224):
        rd_val = mux(rd_addr == entry, regs[entry], rd_val)
for write_port in range(8):
    for entry in range(224):
        regs[entry].assign(wr_data, when=wr_en & (wr_addr == entry))
```

仅存储下限就是 `224 × 64 = 14336` 个带 reset 的 state bits，还不含 14 组大读网和 8×224 组写 decode。若对照实现使用 banked SRAM、multi-pumped memory、复制读 bank 或 foundry register-file macro，资源自然不在同一数量级。

方案不能简单地“让 Emitter 把这些寄存器打印成 memory”：14R8W 的端口/RDW/latency/冲突语义必须先在 dialect 明确。可行方向是显式的 regfile/multiport-memory op、banking/replication pass 和目标 macro mapping，并用 Decisions 0114/0122 的 memory litmus 保证 C++/Verilog 等价。

## 4. 根因二：自动周期对齐与 reset 成本

实际 cycle-aware frontend 的 [`delay_to`](../compiler/frontend/pycircuit/v5.py#L308) 对每一拍都创建新的 `_v5_bal_N`：

```python
for _ in range(to_cycle - from_cycle):
    r = m.out(name, domain=cd, width=width, init=0)
    r.set(cur)
    cur = r.q
```

当前没有共享 cache；同一逻辑值沿多个表达式路径对齐时，可能得到结构相近但独立的 pipeline。更重要的是 `init=0` 让所有这些 data stages 都进入带 reset 的 `pyc_reg`。

对当前 Bypass 源码做 frontend-only 定位实验（`lanes=4, data_width=32, ptag_count=64`）：

| 源文件 | raw MLIR `pyc.reg` | reg bits |
|---|---:|---:|
| `bypass_unit.py` | 0 | 0 |
| `bypass_unit_v5.py` | 352 | 1456 |

这证明差异在 Emitter 之前已经出现，但这两个版本的流水语义不同，所以它不是公平 PPA 对比，只是“首次出现层级”的证据。对 exact `(source, from_cycle, to_cycle, width)` 做一次临时 cache 试验没有减少该 case 的 352/1456，说明本例不能靠最简单的对象级 memoization 解决；还需分析 vector extraction、结构等价值和所需 pipeline 边。

已提交的 [Bypass compile_stats](../designs/BypassUnit/build/verilog/compile_stats.json) 则是 616 reg / 2584 bits，对应 [bypass_unit.v](../designs/BypassUnit/build/verilog/bypass_unit.v) 中 616 个 `_v5_bal_*`，文件为 12392 行、408572 bytes。它与当前源码小参数实验不一致，表明已提交 build artifact 可能来自不同源码/参数，不能混在一张趋势图里。

建议先实现 instance-aware 的“周期边/延迟来源”统计，再在 MLIR 层做：

- 同一 value、clock/reset domain、目标周期的 delay sharing；
- vector-wide delay canonicalization，避免每个 lane/每个 extraction 独立建链；
- dead/duplicate pipeline elimination；
- 明确区分必须 reset 的 architectural state 和 reset 后尚未 valid、可不 reset 的 data pipeline。

最后一项是语义变化，必须先扩展 dialect reset intent，并同时实现 C++/Verilog backend。

## 5. 根因三：priority/one-hot mux 拓扑

[`Vec.priority_mux`](../compiler/frontend/pycircuit/hw.py#L3427) 默认提供真实的 index-0-wins priority 语义，因此按 reverse chain 生成。代码注释记录：对于实际 one-hot selector，这个镜像拓扑在 ABC 中明显更差，一个 128:1、64-bit read mux 曾观察到约多 500 LUT；`assume_onehot=True` 的 forward chain 更接近手写 one-hot mux。

同时 vector-backed selector 的逐 lane 索引会留下 `v_get` extraction，注释指出它也可能扰动 ABC cut heuristic，见 [hw.py:3451](../compiler/frontend/pycircuit/hw.py#L3451)。

这里不能全局把默认值改成 one-hot：多个 selector 同时为 1 时结果会改变。正确方向是：

1. 在 MLIR 中显式携带 `onehot`/`onehot0` 已证明合同；
2. verifier 或 assertion gate 验证作者声明；
3. true-priority 与 proven-onehot 使用不同 canonical lowering；
4. 对 one-hot 尝试 masked-OR、forward chain 和平衡树，按统一 target A/B；
5. canonicalize vector extraction，再交给 ABC。

## 6. 根因四：memory inference 与目标不一致

当前同步 memory primitive 本身具有 FPGA block RAM attribute，但只有 `--target=fpga` 才定义 `PYC_TARGET_FPGA`。自动 Yosys stub 又没有指定器件 family。组合读 `pyc_byte_mem`、多读口 memory、byte enables 和 reset/read latency 都会改变可推断结构。

典型误判是：

- Chisel 结果保留为 SRAM/BRAM macro；
- pyCircuit generic `synth` 把同容量 memory 展成 FF+mux；
- 最后拿 FF/LUT 数直接比较并归因于语言。

必须同时报告“memory abstract bits/ports”和“mapped macro/BRAM”，并让三方使用同一映射政策。

## 7. 度量与层级问题会制造假差距

### 7.1 `compile_stats` 不是 top area

它对每个函数定义只求和一次，不考虑 instance count 和 top reachability；也没有统计 FIFO/async FIFO/cdc 的全部内部 state，更没有组合 cell。详见 [Yosys 报告第 7 节](02_yosys_verilator_flow.md#7-当前产物为什么不能直接作面积报告)。

### 7.2 XiangShan-pyc 不是等价 PPA 基线

[README](../designs/XiangShan-pyc/README.md#xiangshan-pyc-xiangshan-kunminghu-in-pycircuit-v5) 明确说明代码从头编写，原 Chisel 只作为端口、参数和行为规范参考。同名模块不自动意味着同样的 pipeline、state、memory macro、bypass 或 blackbox。

旧 [`build_verilog.py`](../designs/XiangShan-pyc/build_verilog.py#L725) 的 hierarchical 模式先独立编译每个模块，再生成 wrapper；wrapper 把每个 child 的非 clock/reset 端口全部暴露到边界，见 [build_verilog.py:547](../designs/XiangShan-pyc/build_verilog.py#L547)。它没有连接 parent-child 数据流，不能作为整核 top PPA。

## 8. 优先级结论

建议先按以下顺序量化：

1. 建立同语义、同参数、真实连接 top 的比较基线；
2. register file/memory 是否被实现成等价的 macro/BRAM；
3. balance register bit 数、reset 类型和共享机会；
4. one-hot/priority mux 与 vector extraction；
5. 扩展 MLIR structural optimization；
6. 最后再处理仅影响 RTL 可读性、对 post-opt cell 无影响的 wire/bridge 文本。

