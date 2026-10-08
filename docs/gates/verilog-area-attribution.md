# Gate: Verilog post-synthesis area attribution

目的:在改动任何 pass 之前,量化 pycircuit 生成 Verilog 相对 Chisel/手写 RTL
综合后资源膨胀的**归因**,并作为后续每条优化(state-opt 解除 cpp-only、
observation-demand 推广、reg 发射重构、RAM 模板修复)的回归基线。

## 工具

`flows/tools/verilog_area_attribution.py`:同一输入 `.pyc` 跑多个
`pycc --emit=verilog` 变体,用 pycc 自己生成的 `yosys_synth.ys` 综合
(flat 后 `stat -json`),输出 LUT/FF/RAM/CARRY 对比表 + markdown/json 摘要。

```bash
PYCC=<path-to-pycc> python3 flows/tools/verilog_area_attribution.py \
    --pyc <design>.pyc --top <top> \
    --pycc-args "--hierarchy-policy=strict --inline-policy=off --unroll-vector --logic-depth=512" \
    --out .pycircuit_out/area_attribution/<design>
```

变体:`baseline` / `flatten` / `comb-struct-off` / `flatten+comb-struct-off`。

## 已确认的证据(2026-10-08, pycc@b4af86d, yosys 0.52)

1. **管线不对称(主因)**:verilog 路径 `compile_stats.json` 显示
   `state_opt_policy: "off"`、`state_retime_policy: "off"`、
   `observe_named: "off"`。即 Decision 0121 观测需求/状态优化套件只对
   `--emit=cpp` 生效(verilog 下所有 `pyc.name`/probe 急切保留)。
   位置:`compiler/mlir/tools/pycc.cpp`(step 17/23 的 cppOnly 分支)。
2. **量级确认**:XiangShan-pyc `bpu`(--small, 16-bit, 单模块 flat)综合
   108,011 LUT / 31,084 FF;`ras`(small)7,111 LUT / 1,737 FF。
   同等 Chisel/手写参考实现远低于此(待补基线数据)。
3. flat 单模块设计上 `--flatten` / `--emit-structural=off` 无差异
   (预期);层次化设计的差异测量需要先修复 merged-hierarchy 的 top
   选择问题(见 Limitations)。

## 实验:bpu-small 修改前后(2026-10-08, 16-bit, single flat module)

| 变体 | LUTs | FFs | 备注 |
|---|---|---|---|
| baseline(旧 pycc,state-opt off) | 108,011 | 31,084 | netlist 158,904 cells |
| `--state-opt-on-verilog`(本分支补丁) | 107,432 (−0.5%) | 31,084 (0%) | pass 实际生效:合并 14,430 regs、删除 1,570 state 原语、打包 2,239 ops;netlist cells 146,125(−8%)但综合器 opt 本就做等位合并,后端面积不变 |
| 剥离全部 52,286 个 `pyc.name` | 107,948 (−0.06%) | 31,084 | 观测命名不是 LUT 驱动因素 |
| SLP 路径(去掉 --unroll-vector) | 107,432 | 31,084 | unroll todo 的 bug 已不存在,但面积不变 |

**结论:组合逻辑是面积大头,其中存储建模是第一驱动**:bpu `mem_count=0`,
TAGE/UBTB/RAS 表在前端按"每表项一个命名信号"建模(`domain.signal` per
entry),综合成触发器堆 + 多路读出 mux —— bpu 单元分布:39,463 `$_MUX_`
(25%)、17,596 `$_XOR_`(11%,tag 比较)。Chisel/XiangShan 对应实现用
`SyncReadMem`(SRAM/LUTRAM),无此开销。

### 修订后的方案排序

1. **表/阵列存储建模**(最高优先):短期在前端用 `pyc.sync_mem` 建模
   表结构;中期在 MLIR 加"寄存器阵列 → memory 提升" pass + verifier
   gate(gate-first,识别按地址索引的 reg 阵列)。
2. 解除 state-opt 的 cpp-only 门(本分支已加 `--state-opt-on-verilog`,
   保留 IR 层收益与 netlist 体积 −8%,对后端影响小但无害)。
3. observation-demand 推广到 verilog:需 emitter 支持 `pyc.observe_lazy`
   (当前完全没有),面积收益预期有限(实验 3),按 DFX 需求排期。
4. RAM 模板 byte-strobe 写修复(对 mem_count>0 的设计才生效,如 icache/ftq)。

## 初步方案排序(原始记录,已被上节修订)

1. 解除 `pyc-state-optimize` / `pyc-apply-observation-demand` 的
   cpp-only 限制(MLIR pass,无 backend drift;补 verilog litmus 回归)。
2. RAM 模板修 byte-strobe 写(`pyc_sync_mem*.v` 阻断 BRAM/LUTRAM 推断)。
3. reg 发射重构:向量 reg 单宽发射;`init` 常量折叠为 INIT 属性。
4. 位宽收窄 pass(range 分析)+ verifier gate(中期)。

## Limitations / TODO

- [ ] name/probe 急切保留的归因变体:需要一个 strip-observation 的
  MLIR 工具或 pass(与方案 1 一起做),当前 4 个变体测不到这一项。
- [ ] merged_hierarchy.pyc 只含一个 `func.func`、无 `pyc.instance`,
  `--hierarchy-policy=instantiate` 会把业务模块 dce 掉(top 误判为
  xs_top);层次化归因需先修 `build_verilog.py` 的 merge 或 pycc 的
  top 推断。
- [ ] Chisel/手写基线(同功能模块经 chiselRTL/手写 RTL + 同一 yosys 流程)
  纳入对比表。
- [ ] 把 XiangShan 全模块 small 构建纳入批量回归(gate 日志入
  `.pycircuit_out/area_attribution/`)。
