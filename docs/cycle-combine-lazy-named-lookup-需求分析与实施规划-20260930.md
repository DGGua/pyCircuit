# 未声明名字保留按名查找并在读取时求值需求分析与实施规划

## 背景与目标

用户要按设计里写的 `name=` 在仿真中查找（`findByPath("dut:foo")` / 按名 `dut.read`）。上一轮默认 `--observe-named=demand` 的做法是：没在编译期声明成探针的 `pyc.name` 直接剥掉，C++ `ProbeRegistry` 里没有这条路径。查找失败，不是读到旧值。

用户现在要求改成：

- **编译期声明过的探针**（`@probe`、`--trace-config`、`debug_keep` / `observable` / `probe*` / `trace*`）：继续现在这样，每拍维护好视图，读指针即可。
- **只写了 `name=`、没声明成探针的**：仍然能按这个名字主动查到；**不要每拍为它 `extract` / 抄到 struct 成员**；真正读的时候再求值。

目标是同时保住「按名读」和「无人读时热路径不付观测拷贝」。不恢复 `--observe-named=all` 那种每拍全抄。

「自己进行求值」在本方案里的默认理解：调用方仍然只按名字查找；**移位/掩码由 runtime 在这次 read 里做**，不必手写 `extract(packed, lsb)`。若用户坚持调用方自己算，见待确认。

## 项目现状与执行流程定位

C++ 路径现在是：

```text
combine / retime / pack          身份重映射到 alias / tap / extract
pyc-apply-observation-demand     默认：剥掉未声明的 pyc.name
canonicalize / fuse-comb
cpp-placement                    剩下的 pyc.name 钉 DUT struct
CppEmitter                       每拍 名字 = extract(...)
                                 pyc_register_probes: addReg / addRegSlice
                                 指针指向那根已经抄好的 Wire
```

`ProbeRegistry::Entry` 的 `ptr` 指向一根**已经算好的** `Wire<W>`。`addRegSlice` 虽带 `write_lsb_bits`，那是写 next 时从 packed storage 切片用的；**读 q 仍然走独立的 lane Wire**，不是读时才从 packed 抽。

`cli build` 的 `@probe` 在编译期解析，结果进 `--probe-plan`。仿真里再拼字符串去 `findByPath` 的，demand 模式下现在会失败。

功能数据通路用的 SSA / 功能 `extract` 每拍都在，和探针声明无关。Davinci 那些 `{prefix}_ready_state` 多数是功能信号上的作者名：packing 之后功能 `extract` 仍在；多出来的成本是再挂一根 `名字 = extract` 并钉 struct。

## 相关现有实现说明

### 声明过的需求集合

`ApplyObservationDemandPass` 把下列视为「声明过的探针」：

- `--probe-plan` 里每条 `source_path` 的 `field_path`（`dut:q` → `q`）
- `--trace-codegen-plan` 里该模块的内部场
- IR 上 `shouldKeepStateOptimization`：`debug_keep`、`observable`、`pyc.probe*`、`pyc.trace*`

未命中的 `pyc.name` 被 `removeAttr("pyc.name")`。之后 alias 不再带 Write，观测-only 拷贝可被 DCE，placement / emitter 再也看不见这个名字。

### 查找实际读什么

`state_pack_probe_runtime.cpp` 一类夹具：`findByPath("dut:lane0_state")` 后把 `ptr` 当成 `Wire<W>*` 用。VCD / binary trace 也假定 `ptr` 指向可直接采样的存储。

因此「读时才求值」必须改 registry 契约：懒条目的 `ptr` 不再是那根本地 lane Wire，而是**活着的 packed / survivor 存储**，外加宽度和 lsb。现有 `static_cast<Wire<W>*>(e->ptr)->value()` 对懒条目会读错（读到整段 packed）。必须提供统一的 `read` 辅助，或把懒条目和急切条目从类型上分开。

### 什么值读时还能算出来

| 优化后物理上还在哪 | 读时能否求值 | 说明 |
|---|---|---|
| 功能仍在用的 `extract` / 未打包 `reg.q` | 能 | 不必再抄一根命名 Wire；查找可指向这根已有 SSA，或指向 packed + lsb |
| packed storage 的某 lane，功能 extract 还在 | 能 | `addRegSlice` 式：读 packed、掩码、lsb |
| 共同 delay 下沉后只为观测留下的源 history | 仅当这颗 history 没被 DCE | 当前 demand 剥名字后，这颗只读 history 可能被 `eliminate-dead-state` 删掉，之后无法还原 |
| 观测-only、功能零使用的寄存器 | 仅当对象还在 | 剥名字后会被当死状态删掉 |

懒查找若承诺「凡是起过名的都能读」，优化后必须留下**可还原该名字的存储**（packed / survivor / 下沉源 history），只是不要每拍抄到第三根 Wire。

## 需求与验收标准

1. 未在编译期声明、但带非 `cycle_balance` 的 `pyc.name` 的状态：`findByPath("instance:name")` 必须找到条目。
2. 这类条目在 `eval` / `comb` 热路径上**不得**再多一根只为观测存在的 struct 赋值（`名字 = extract`）。
3. 调用 `read`（或待确认的等价 API）得到的位模式，与现在 `--observe-named=all` 下同一周期读该名一致。
4. 已声明探针行为不变：仍钉 struct、每拍更新、`ptr` 仍可当 `Wire*` 用（兼容现有急切夹具）。
5. 端口始终急切注册，不走懒路径。
6. `--observe-named=all` 仍表示全部急切，行为与 580c1baa 之前的 keep-identity C++ 一致。
7. `--emit=none` catalog 仍枚举全部作者名。
8. 功能 `extract`、merge/retime/pack 证明不放宽。
9. 不在 `eval` 里对懒名字加「这拍有没有人 read」的分支。

## 方案设计

### 模块边界、输入与输出

把「作者名」分成两档发射，而不是「留下 / 删掉」：

| 档 | 谁 | C++ 热路径 | 查找 |
|---|---|---|---|
| **急切** | 声明过的探针 + 端口 | 现有 alias 赋值 + `addReg` / `addRegSlice` 指向 lane Wire | `ptr` 直接读 |
| **懒** | 其余非 `cycle_balance` 的 `pyc.name` | 不钉、不抄；功能 extract 照旧 | registry 记「存储指针 + width + lsb」，`read` 时切片 |

`--observe-named` 语义改为：

- `demand`（默认）：声明过的急切，其余懒查找。
- `all`：全部急切。
- 不再把未声明名字从 registry 里拿掉。

`pyc-apply-observation-demand` 改职责：不要再 `removeAttr("pyc.name")` 让查找消失；改为给未声明名字打 `pyc.observe_lazy = true`（或等价 attr）。随后：

- `AliasOp` 的 Write / fold：懒名字**不**为保活而 Write，观测-only 拷贝可 DCE。
- 在 DCE 之前，把懒名字解析成「指向哪段活存储、lsb、宽度」，写到 function 属性（例如 `pyc.lazy_probes` JSON/字典），供 emitter 使用。解析沿用 emitter 已有的 `findRegQFromValue`。
- `CppPlacementPass`：`pinToStruct` 不因懒名字钉死。
- `FuseCombPass`：懒名字不挡融合（attr 已标 lazy，或名字已从即将 DCE 的 alias 上处理完）。
- `CppEmitter`：急切路径不变；另对 `pyc.lazy_probes` 生成 `addRegLazySlice`（新 API），**不**生成 `名字 = …` 赋值。
- `pyc_trace_vcd`：懒名字默认不挂每拍 `vcdTrace`（它们不在 trace 需求里）。若运行时有人要对懒路径打波，见待确认。

`ProbeRegistry` 新增懒注册，例如：

```text
addRegLazySlice<W, StorageW>(path, packed_q, lsb, pending, packed_qNext)
```

`Entry` 增加 `read_lsb_bits` / `read_storage_width_bits`（或复用 write 侧字段并标明「q 也是 slice」）。提供：

```text
Entry::readU64() / 按宽读到 Bits
```

内部：从 `ptr` 指向的 `Wire<StorageW>` 取位 `[lsb + W - 1 : lsb]`。调用方按名查找后走这个 read，不要再 `static_cast<Wire<W>*>(ptr)`。

急切条目的 `readU64()` 就是读原来的 lane Wire，夹具可以逐步改过去。

### 上下游关系与数据/控制流

```text
身份重映射（不变）
        │
        ▼
apply-observation-demand
        声明过的：保持急切身份
        未声明的：标 lazy，记下 packed/survivor + lsb
        懒观测-only alias 可 DCE
        │
        ▼
fuse-comb / placement / emit
        急切：赋值 + addReg
        懒：仅 registry 切片描述符
        │
        ▼
仿真
        findByPath(name) 有条目
        急切：读 Wire
        懒：本次 read 做 extract
```

下沉源 history：若该源只有作者名、没有声明探针，懒查找仍要读到下沉前的 `x`。必须在标 lazy **之后**仍把这颗 history 视为「有外部可读身份」，避免 `eliminate-dead-state` 删掉唯一存储。也就是：`hasExternalObservationIdentity` 对懒名字仍为真（为了保存储），但 placement/emitter 不把它当急切 Wire。若把「保存储」和「剥名字」做成同一步，下沉源会丢，查找会失败或读到错拍。

### 文件和接口改动清单

| 文件 | 改动目的 |
|---|---|
| `runtime/cpp/pyc_probe_registry.hpp`（及 include 副本若需同步） | 懒条目字段 + `addRegLazySlice` + `Entry` 统一 read |
| `compiler/mlir/lib/Transforms/ApplyObservationDemandPass.cpp` | 改为标 lazy + 收集存储描述符，不再让未声明名字从查找空间消失 |
| `compiler/mlir/lib/Dialect/PYC/PYCOps.cpp` | 懒 alias 不因名字 Write |
| `compiler/mlir/lib/Transforms/CppPlacementPass.cpp` | 懒名字不 `pinToStruct` |
| `compiler/mlir/lib/Transforms/FuseCombPass.cpp` / `hasStableStateName` | 懒名字不是融合屏障 |
| `compiler/mlir/lib/Transforms/EliminateDeadStatePass.cpp` / `hasExternalObservationIdentity` | 懒名字仍保住可还原存储 |
| `compiler/mlir/lib/Emit/CppEmitter.cpp` | 急切/懒分开发射；懒不赋值 |
| `compiler/mlir/tools/pycc.cpp` | `--observe-named=demand` 文档改为「未声明=懒查找」 |
| `compiler/mlir/test/state_delay_optimization_smoke.sh` 等 | demand 下 `lane0_state` 应能 `findByPath`，hpp 中无每拍 `lane0_state =`；`all` 仍急切 |
| `docs/delay_line.md`、`MLIR_PASS_PIPELINE.md`、observation-demand 规划 | 契约从「剥掉」改为「懒查找」 |

不改 Davinci `IGeneratedBackend` 端口路径。不扫 TB 源码。

### 边界条件、错误处理与兼容性

- 无法解析到活存储的懒名字（优化后值已不存在）：`pycc` 失败并指出名字，而不是生成会读垃圾的条目。
- 宽于 64 的懒读：read API 必须按 `Wire<W>` / 多字，不能只提供 `uint64_t`。
- 向量 / mem：本方案只覆盖标量 state 名；向量仍走现有 `addVec`。未声明的向量名若出现，第一期可急切或报错，见实施时按夹具决定，不在此发明半套。
- 已有 C++ 把 `ptr` 当 `Wire<W>*` 的急切夹具：声明过的或 `--observe-named=all` 不受影响。demand 下读懒名字必须改走新 read。
- `cycle_balance` 仍不进查找。
- 坏的 `--probe-plan` 仍失败。

## 与既有文档和约束的一致性检查

| 文档 / 决策 | 结论 |
|---|---|
| Decision 0004 / 0023 | 仍用中央 registry 和 `instance:field`。懒条目也是 probe，只是存储是切片描述符。 |
| Decision 0091 / 0096 | 「未选中的探针不应有运行时开销」：懒条目有表项和一次 hash 查找，**没有**每拍拷贝。比 `all` 更接近决策，比当前 demand（查找失败）更符合「按名读」。 |
| `docs/cycle-combine-observation-demand-views-需求分析与实施规划-20260929.md` | **有意改契约**：demand 不再删除查找；改为懒求值。已拍板的 B（默认 demand）和 A（不要 `--observe-fields`）保留，只改 demand 的含义。 |
| keep-identity 规划 | 优化中仍重映射身份；C++ 对未声明名不再「既重映射又每拍抄」，改为重映射存储 + 读时切片。 |
| 不恢复优化前 strip | 仍不在 merge 之前剥名字。 |

无无法规避的硬冲突。当前 smoke 里「demand 下不得出现 `lane0_state`」必须改成「不得出现每拍赋值，但 `findByPath` 要成功」。

## 测试与验证计划

1. `observation_demand.mlir`：`keep_me` 急切留下 `pyc.name`；`drop_me` 变为 lazy attr 或进入 `pyc.lazy_probes`，IR 里没有每拍要用的观测 alias。
2. pack-probe：`--observe-named=demand` 生成 C++ 无 `lane0_state =`，但 `pyc_register_probes` 有懒切片；runtime `findByPath` + 新 read 得到与 `all` 相同的 lane 值。
3. `--observe-named=all`：仍 ≥4 个急切 `addRegSlice`，旧 `Wire*` 夹具通过。
4. `xz_value_model_smoke` 的 `@probe` 读 `q`：仍急切。
5. 无 `@probe` 的具名模块：`findByPath` 成功，eval 文本没有 `name = extract`。
6. 下沉具名源：demand 下按原名 read 仍是下沉前那一拍。
7. 性能：同一 pack-probe / IssueQueue 生成物，demand 的 `addReg` 急切次数接近声明数；`Wire` / struct 成员相对 `all` 下降；懒 `addRegLazySlice` 条数等于未声明名。墙钟若有性能测试作业再对照，门禁用生成物计数。

## 实施步骤

1. registry 懒条目 + 统一 read，加最小 C++ 单测。
2. demand pass 改为标 lazy 并收集存储描述符；死状态仍认懒名字为「要留存储」。
3. placement / fuse / alias Write 按 lazy 放行。
4. emitter 分急切/懒；禁止懒路径生成赋值。
5. 改 smoke 断言；更新三份规划/管线文档。
6. 编译跑 state-opt / pack-probe / diagnostics；写回实施结果。

## 待确认事项

### 待确认 1：读懒名字时谁做 extract

**为何现在必须决定：**
现在 `findByPath` 返回的 `Entry::ptr` 指向一根已经每拍更新好的 `Wire`。懒路径不再维护这根 Wire。若不先定调用约定，夹具和 Davinci 调试代码不知道该 `cast` 指针还是该自己移位。

**相关背景：**
packing 之后，未声明名字对应的位通常还在 packed `q` 里。`addRegSlice` 已经会在**写 next** 时用 lsb 切片，但读 q 仍走独立 lane Wire。用户说「保留主动查找，只是需要自己进行求值」：查找仍按 `pyc.name`；求值是从活存储抽出那一段。功能仿真不查这些名时，热路径不应抄一份。已声明的 `@probe` 仍走旧的急切指针。

**选项与结果：**

- **选项 A：查找后由 registry 在 `read()` 里切片**
  - 将做什么：`findByPath` 仍可用；新增 `Entry::read…()`，内部对 packed 做移位掩码。调用方不用知道 lsb。
  - 结果：按名读的代码几乎仍是「查到再读」。旧的 `*(Wire<W>*)ptr` 对懒条目会错，必须改文档和 demand 夹具。
  - 风险：所有读懒名字的 C++ 要换 API；漏改会读到整段 packed。

- **选项 B：查找只返回「存储指针 + lsb + 宽度」，调用方自己 extract**
  - 将做什么：不提供糖衣 `read()`，或只提供描述符。
  - 结果：更贴字面「自己求值」。每个 TB 都要写切片，容易错位宽。
  - 风险：使用成本高，和「我想用名字读」不一致。

**推荐：**
选项 A。按名读的人不应被迫理解 packing。求值发生在这次 read，不是每拍；热路径仍然省掉了。

### 待确认 2：默认 `demand` 是改成「未声明=懒查找」，还是再加一档

**为何现在必须决定：**
580c1baa 刚把默认 `demand` 定义成「未声明名字从 C++ 消失」。本方案要让它们能查。若不改默认含义，就要新名字（例如 `lazy`），否则现有「demand 下不得出现 lane0_state」的测试和文档会对不上。

**相关背景：**
`--observe-named=all` 已是「全部急切」。`--observe-named=demand` 是 `cli build` 和 raw `pycc --emit=cpp` 的默认。没有第三档时，性能作业和「偶尔按名 dump」只能二选一。用户现在要的是默认就能按名查、但不每拍抄。

**选项与结果：**

- **选项 A：默认 `demand` 改为「声明急切、其余懒查找」**
  - 将做什么：不再提供「未声明完全消失」的默认。要极限瘦 C++ 只能以后再加档，或不提供。
  - 结果：按名读默认可用；无人读时没有每拍拷贝，仍有 registry 表项。
  - 风险：表项数量仍随作者名增长（初始化内存），但远小于每拍赋值。

- **选项 B：新增 `--observe-named=lazy`，`demand` 仍表示消失**
  - 将做什么：三档：`all` / `lazy` / `demand`。默认若仍是 `demand`，用户按名读仍然失败，除非改默认。
  - 结果：行为分得更细。调用方必须记得传 `lazy`。
  - 风险：和「我就是想用名字读」的默认诉求错位。

**推荐：**
选项 A。默认对准用户这句话；`all` 留给要旧 `Wire*` 指针语义的夹具。

## 已拍板

2026-09-30 用户批准：待确认 1 选 **A**（`Entry::readU64()` 在本次 read 里切片），待确认 2 选 **A**（默认 `demand` 改为声明急切、其余懒查找）。

## 实施结果

已按 AA 改完，未提交。未混入工作区里其它未跟踪文档。

### 契约

- `demand`：声明过的探针 + 端口仍急切；其余标量状态 `pyc.name` 打 `pyc.observe_lazy`，在活存储上挂 `pyc.lazy_probe_slices`，C++ 用 `addRegLazySlice` / `addRegLazyTap`。热路径不再为这些名字生成 `名字 = extract`。
- `findByPath("dut:name")` 对懒名字成功；调用方用 `Entry::readU64()`，不要把懒条目的 `ptr` 当成 lane `Wire*`。
- 无法还原成 packed / survivor / tap 的**标量组合名**改为懒 `addWire`：挂到已有成员（或 fuse 后的 `pyc.comb` 结果），不额外抄第二根命名 Wire。端口 / 向量 / 还原不出 host 的名字仍急切。
- `--observe-named=all` 仍全部急切。`cycle_balance` 仍不进查找。
- `hasExternalObservationIdentity` 对懒名字仍为真，死状态不会删掉唯一存储；`hasStableStateName` 对懒名字为假，不挡 fuse / 不钉 struct。

### 验证

```text
cmake --build .pycircuit_out/toolchain/build --target pycc pyc-opt
PYCC=$PWD/.pycircuit_out/toolchain/build/bin/pycc \
PYC_OPT=$PWD/.pycircuit_out/toolchain/build/bin/pyc-opt \
  bash compiler/mlir/test/state_delay_optimization_smoke.sh
# PASS（observation_demand 保留 drop_me + observe_lazy；
# demand 无 lane0_state= 赋值、有 addRegLazySlice；
# demand runtime findByPath+readU64 与端口一致；
# --observe-named=all 仍 ≥4 个 addRegSlice<8, 16>）

PYCC=… PYC_OPT=… bash compiler/mlir/test/delay_line_diagnostics_smoke.sh
# PASS

pycc --observe-named=maybe → unknown --observe-named（rc≠0）
pyc-opt … probe-plan=/no/such/… → cannot read probe plan（rc≠0）
```

未跑完整 Davinci / 性能测试墙钟。

## 后续：组合名也改懒注册（选项 B）

2026-09-30 用户批准再试 **B**：未声明组合名不再急切钉第二根 Wire。

### 做法

1. `pyc-apply-observation-demand`：状态切片仍走 `pyc.lazy_probe_slices`；否则把名字挂到可取地址的 host（`pyc.and` / 已有 `pyc.comb` 结果 / instance 结果）为 `pyc.lazy_probe_wires`，并打 `pyc.observe_lazy`。
2. `pyc-fuse-comb`：被融合 live-out 上的 `pyc.lazy_probe_wires` 改挂到新 `pyc.comb` 的对应 result，这样名字不挡融合，指针仍指向 struct 成员。
3. `CppPlacementPass`：带 `pyc.lazy_probe_wires` 的值钉 struct，保证 `addWire` 指针活过 `eval`。
4. `CppEmitter`：对这些名字只 `addWire`，不生成 `名字 = …`。`readU64()` 读那根已有成员。

### 验收

```text
FILECHECK=/usr/lib64/llvm19/bin/FileCheck \
PYCC=$PWD/.pycircuit_out/toolchain/build/bin/pycc \
PYC_OPT=$PWD/.pycircuit_out/toolchain/build/bin/pyc-opt \
  bash compiler/mlir/test/state_delay_optimization_smoke.sh
# PASS（含 observation_demand_comb FileCheck、demand 无 ready_state=、
# addWire ready_state/ready_alias、runtime AND 值一致；
# pack-probe 懒切片回归仍过）
```
