# Vec Dim-Reduce Changes

`vec` 分支把 `Vec` 从 Python 容器推进成贯穿 frontend / MLIR / backend / 测试的 first-class vector IR。下面先看能写出什么，再看底层怎么落地。整体遵循 pyc4.0：语义落在 dialect + MLIR pass + gate，backend 只发受验证 IR。

---

# 第一部分：效果

## 1. 多 lane 匹配逻辑矩阵化（BypassUnit）

bypass search 的本质是「N 个源 × M 个 lane」的外积匹配。向量化前要手写双层循环和一堆 per-lane scalar port；现在用 broadcast 一次形成 match 矩阵，再 `or_reduce(dim=1)` 压成 per-source hit（`designs/BypassUnit/bypass_unit.py:43`）：

```python
# src: Vec<N>, lane: Vec<M>  —— 各自广播到 Vec<N,M>
sv_bc = src_valid_v.broadcast(dim=1, size=n)   # 行复制
lv_bc = lane_valid.broadcast(dim=0, size=n)    # 列复制
match = sv_bc & lv_bc & (lp_bc == sp_bc) & (lt_bc == st_bc)   # N×M 命中矩阵

has = match.or_reduce(dim=1)                   # Vec<N>：每个源是否有命中
sel_lane = Vec([match[i].priority_mux(lane_nums, zero=zero_lane) for i in range(n)])
```

一句 broadcast + 一句 dim reduce，替代了原来的标量嵌套循环。

## 2. 端口收敛 + 综合工具友好

向量化前端口是散开的 per-lane scalar：

```text
w1_valid_0, w1_valid_1, ... w1_valid_7
w1_ptag_0,  w1_ptag_1,  ...
```

现在收敛为 vector port：

```text
w1_valid: vector<8xi1>
w1_ptag:  vector<8xi8>
```

更进一步，**Verilog 发射把这些 port flatten 成 packed bus**，对综合工具（yosys 等）友好：

```verilog
input  [15:0] a;     // vector<4xi4> 打包成一条总线，不再是 input [3:0] a [0:3]
output [11:0] out;   // rank-2 vector<2x3xi2> 同样 flatten
```

已加 yosys synthesis smoke 测试保证可综合。内部 IR 仍是 unpacked vector 视图，instance bridge wiring 负责 pack/flatten。

## 3. dim reduce 一行表达行列归约

rank-2 `Vec` 约定 `vector<rows x cols x elem>`，`dim=0` 列归约、`dim=1` 行归约。IssueQueue 的 issue winner 选择就是典型（`designs/IssueQueue/issq.py:91`）：

```python
issue_win = Vec(issue_sel).or_reduce(dim=0)     # 多端口的选择位按位或
occupancy = fields["valid"].reduce_sum(width=occ_w)   # 占用计数，自动定宽 + ceil_log2
```

`reduce_sum` 默认位宽 = `max_input_width + ceil_log2(len)`，无符号先 `zext`、`signed=True` 先 `sext`。

## 4. 归约形态可选（chain / tree）

`or_reduce` / `and_reduce` / `reduce_sum` 带 `mode` 参数：

```python
v.or_reduce(mode="chain")   # 默认：源序链式，贴近手写优先级逻辑，Yosys/ABC 映射可预测
v.or_reduce(mode="tree")    # balanced 二叉树，lane 多时组合深度低
```

默认 `chain`，unroll 后的标量形态和手写一致；需要压深度时切 `tree`。`priority_mux` 同理改成线性 chain（最左候选 wins，对非 one-hot 也有定义），one-hot 场景可传 `assume_onehot=True` 走源序。

## 5. 状态批量更新（lane-wise assign）

vector 化的 cycle-aware 设计更新状态不用再手写 per-lane 循环：

```python
dst_vec.assign(src_vec, when=mask_vec)   # 等价于 dst[i].assign(src[i], when=mask[i])
```

`value` / `when` 可为匹配长度的 Vec/list，或标量广播。vector-backed 表达式 Vec 只读，不可 assign。

---

# 第二部分：技术细节

## Frontend / API

### 双层 `Vec` 表示

`Circuit.input(..., shape=...)` 返回 vector-backed `Vec`（底层 MLIR vector value），标量输入仍返回 `Wire`；`Circuit.output(name, vec)` 输出矩形 `Vec`。

- `elems`：始终保留 Python lane list。
- `sig`：可向量化时对应的 MLIR vector `Signal`。

普通 `Vec([w0, w1, ...])` 构造时生成一次 `pyc.v_create` 缓存到 `sig`，后续 op / reduce / broadcast 复用，不再重复合成。vector op 结果用 `pyc.v_get` 构造 lane 视图，原 result 存 `Vec.sig`。

`Vec` 可接受 cycle-aware lane wrapper（如 `cas(domain, wire, cycle=N)`），但仅作构造入口：检查所有 lane cycle 一致后解包为 `Wire` 走普通 vector IR；不允许混用 cycle-aware lane 和 raw `Wire`/`Reg`/`Vec`。

### 关键 API 签名（`compiler/frontend/pycircuit/hw.py`）

```python
def or_reduce(self, dim=None, *, mode="chain")        # hw.py:3210
def and_reduce(self, dim=None, *, mode="chain")       # hw.py:3222
def reduce_sum(self, ..., width=None, dim=None, signed=False, mode="chain")  # hw.py:3234
def priority_mux(self, vals, *, zero=0, assume_onehot=False) -> Wire         # hw.py:3427
def broadcast(self, *, dim, size) -> Vec              # hw.py:3451
def assign(self, value, *, when=None) -> Vec          # hw.py:2358
```

### Element-Wise Ops

arithmetic（`+ - * // %`）、bitwise（`& | ^ ~`）、compare（`== != < > <= >= ult slt`）、shifts（`<< >> lshr ashr shl`）、mux/select。混合 scalar/vector operand 自动广播走 fast path。

### Reduce fallback

优先走 native vector IR（带 `mode` attr）；`Vec` 不能提升成单一 vector signal 时回退到标量归约，按 `mode` 选链式 / 树状。

## MLIR Dialect 和 Gate

vector ops：`pyc.v_create`、`pyc.v_get`、`pyc.v_broadcast`、`pyc.v_broadcast_dim`、`pyc.v_or_reduce`、`pyc.v_and_reduce`、`pyc.v_add_reduce`。

verifier 约束：

- `v_create`：非空、元素类型一致，结果 shape 匹配。
- `v_broadcast_dim`：operand 是 vector，`size > 0`，`dim ∈ [0, rank]`，结果 rank = src rank + 1。
- `v_*_reduce`：支持 rank-1/rank-2，维度非空，`dim` 合法；omitted `dim` 仅 rank-1；result type = 删去 reduce 维度的 shape，完全归约返回元素整数类型。
- `mode` attr（`"chain"`/`"tree"`）可选，缺省按 chain。

`pyc-check-flat-types` 允许 vector-of-integer，vector IR 可作合法 emission surface。需要标量化输出时 `pycc --unroll-vector` 在优化和 legality gate 前运行 `pyc-unroll-vector`，按 `mode` 展开为 per-lane scalar op。

## Backend

### C++ Emitter

vector 类型映射到嵌套 `pyc::cpp::Vec`：

```text
vector<4x8xi32> -> pyc::cpp::Vec<pyc::cpp::Vec<Wire<32>, 8>, 4>
```

支持 `v_get` / `v_create`、scalar/dim broadcast、rank-1/rank-2 reduce（尊重 `mode`）、vector mux（含 vector select signal）、vector width cast。vector mux/zext/sext/trunc 用 `mux_vec`/`*_vec` helper 走 vector-only overload，避免被标量模板抢走。

### Verilog Emitter

- **Packed vector ports**：Vec module port → packed bus（`input [15:0] a`）。rank-2 同样 flatten（`vector<2x3xi2>` → `input [11:0] a`）。内部视图仍 unpacked，JIT instance 桥接端口带 `__flat` 后缀。
- reduce 按 `mode` 发射：`tree` 为 balanced 二叉树，`chain`（默认）为源序链。
- vector elementwise op 按 lane 发射。
- signed div/rem 除零保护已对齐 oracle：zero 分支显式 `$signed(...)`，修复 Verilator 把三元当 unsigned 的问题（`Vec` `sdiv`/`srem` 已恢复覆盖）。

### Vector Unroll Pass

`pyc-unroll-vector` 是 IR-level pass（非 backend fallback），覆盖 elementwise op、`v_get`、`v_broadcast`、`v_broadcast_dim`、`v_*_reduce`、vector `wire`/`assign`/`reg`。reduce 按 `mode` attr 选链式 / 树状：`v_get` 提取 scalar lane → 标量 op → `v_create` 还原（reduce 直接生成标量/低维结果）。

## 设计落地

- **`IssueQueue`**（`designs/IssueQueue/issq.py`）：`Vec` 表达 ready table、age matrix、wake 匹配、issue winner、allocation；`priority_mux(..., assume_onehot=True)`、`reduce_sum(width=...)`、`Vec(issue_sel).or_reduce(dim=0)`。`issq_config.py` helper 改用 `mux()` 和 `m.const()` 走 MLIR 路径。
- **`BypassUnit`**（`designs/BypassUnit/bypass_unit.py`）：vector-shaped port + broadcast 矩阵 bypass search + per-row `priority_mux`。重复常量预计算复用，eager MLIR `pyc.constant` 从 70 降到 16。

## 测试

`tests/vec`：`cases.py`（case 定义）、`generate.py`（生成 DUT/testbench）、`runner.py`（build + IR token 检查 + C++ binary，有 Verilator 时查 Verilog）、`test_vec_ops.py`（vector-shaped IO、JIT instance Vec port、function-style cast API、cycle-aware lane 规则、2-D dim reduce、rank-2 packed port + yosys smoke、reduce mode chain/tree）。

```bash
PYTHONPATH=compiler/frontend pytest tests/vec -m vec -q
PYTHONPATH=compiler/frontend pytest tests/vec -m "vec and not slow"
bash tests/vec/run_vec_ops.sh
make vec-smoke
```

## 当前限制

- MLIR verifier/emitter 只承诺 rank-1/rank-2 reduce，更高维需扩展。
- `dim=None` 仅 rank-1；rank>1 必须显式 `dim`。
- `Vec.broadcast()` 要求输入可向量化；非矩形 / 跨 module / 混合 leaf 类型的 irregular Vec 会被拒。
- cycle-aware lane 仅在构造入口做同 cycle 检查和解包；`Vec` 本身不是 `CycleAwareVec`，不自动插 delay 或保留 cycle provenance。
