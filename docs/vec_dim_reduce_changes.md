# Vec Dim-Reduce Changes

本文整理 `vec` 分支上与 `Vec`、维度归约、广播和向量化设计改写相关的修改。当前实现遵循 pyc4.0 的基本约束：语义先落在 frontend/MLIR dialect/pass/gate 上，backend 只做受验证 IR 的发射；`pyc-check-flat-types` 负责确认最终 emission surface 只包含整数、时钟/复位和 vector-of-integer 等可发射类型。

## 1. 目标和范围

这批修改的直接目标是让 pyCircuit 可以用 Pythonic 的 `Vec` 写多 lane 硬件逻辑，同时在 MLIR 中保留 first-class vector IR，避免一开始就展开成大量标量 wire。

核心覆盖范围：

- 1-D/2-D `Vec` 运算：算术、位运算、比较、移位、mux、reduce。
- 维度归约：`or_reduce(dim=...)`、`and_reduce(dim=...)`、`reduce_sum(dim=...)`。
- 向量广播：scalar-to-vector `v_broadcast` 和 vector shape 插维广播 `v_broadcast_dim`。
- 后端发射：C++/Verilog 直接发射 vector IR，或通过 `--unroll-vector` 在 MLIR pass 中展开。
- 设计落地：`IssueQueue` 和 `BypassUnit` 开始使用 vector-backed IO、broadcast、dim reduce 和 balanced priority mux。

## 2. Frontend/API 改进

### Vector-Backed `Vec`

`Circuit.input(..., shape=...)` 现在返回 vector-backed `Vec`，底层是一个 MLIR vector value；普通标量输入仍返回 `Wire`。`Circuit.output(name, vec)` 也支持输出矩形 `Vec`，将其作为 vector port 发出。

这让端口可以从原来的 per-lane scalar 命名：

```python
w10_valid, w11_valid, ...
```

收敛为 vector-shaped port：

```python
w1_valid: vector<lanesxi1>
w1_ptag: vector<lanesxiN>
```

`Vec` 现在采用 Wire 风格命名的双层表示：`elems` 始终保留 Python lane list，`sig` 保存可安全向量化时对应的底层 MLIR vector `Signal`。普通 `Vec([wire0, wire1, ...])` 构造时会生成一次 `pyc.v_create` 并保存到 `sig`，后续 `_as_vector_signal()`、element-wise op、reduce 和 broadcast 复用同一个 vector signal，不再重复合成。来自 vector op 的结果会用 `pyc.v_get` 构造 Python lane list 视图，同时原始 vector op result 保存在 `Vec.sig`。

### Element-Wise Vector Ops

`Vec` 已覆盖主要逐 lane 运算：

- arithmetic: `+`、`-`、`*`、`//`、`%`
- bitwise: `&`、`|`、`^`、`~`
- compare: `==`、`!=`、`<`、`>`、`<=`、`>=`、`ult`、`slt`
- shifts: `<<`、`>>`、`lshr()`、`ashr()`、`shl()`
- mux/select: `cond.select(a, b)` 和 Python conditional JIT 路径

混合 scalar/vector operand 会尽量走 vector IR fast path：scalar 自动 broadcast 到每个 lane，避免手写重复 lane 逻辑。

## 3. Dim Reduce 语义

### API 约定

`Vec` 归约 API 支持两种模式：

- `dim=None`：只允许 1-D leaf `Vec`，返回单个 `Wire`。
- `dim=int`：沿指定维度归约，rank>1 时返回降一维后的 `Vec`；rank-1 路径返回 `Wire`。

当前公开方法：

```python
a.or_reduce()
a.or_reduce(dim=0)
a.or_reduce(dim=1)

a.and_reduce()
a.and_reduce(dim=0)
a.and_reduce(dim=1)

a.reduce_sum()
a.reduce_sum(dim=0)
a.reduce_sum(dim=1)
```

`reduce_sum(width=None)` 默认输出位宽为：

```text
max_input_width + ceil_log2(reduce_len)
```

如果显式传入 `width`，要求 `width >= max_input_width`。无符号求和先 `zext` 到输出宽度；`signed=True` 时先 `sext`，并返回 signed wire/vector。

### Rank-2 行列归约

rank-2 shape 约定为：

```text
vector<rows x cols x elem>
```

- `dim=0`：跨 rows 做列归约，输出 `vector<cols x elem>`。
- `dim=1`：每 row 内归约 cols，输出 `vector<rows x elem>`。

这正好覆盖 bypass/issue queue 中常见的矩阵匹配：

```python
match = src.broadcast(dim=1, size=M) & lane.broadcast(dim=0, size=N)
has = match.or_reduce(dim=1)
```

### Fast Path 和 Fallback

实现优先走 native vector IR：

- `pyc.v_or_reduce`
- `pyc.v_and_reduce`
- `pyc.v_add_reduce`

当 `Vec` 不能提升成单一 vector signal 时，frontend 会回退为递归/树状标量归约。树状归约避免生成长链组合逻辑，和 backend 端的 balanced reduce 发射策略保持一致。

## 4. MLIR Dialect 和 Gate

新增或扩展的 vector ops：

- `pyc.v_create`
- `pyc.v_get`
- `pyc.v_broadcast`
- `pyc.v_broadcast_dim`
- `pyc.v_or_reduce`
- `pyc.v_and_reduce`
- `pyc.v_add_reduce`

关键 verifier 约束：

- `v_create` 要求非空、元素类型一致，结果 shape 必须匹配。
- `v_broadcast_dim` 要求 operand 是 vector，`size > 0`，`dim` 在 `[0, rank]`，结果 rank 必须是 `src rank + 1`。
- `v_*_reduce` 当前支持 rank-1/rank-2 vector，维度非空，`dim` 在合法范围内。
- omitted `dim` 只允许 rank-1 reduce。
- result type 必须等于删除 reduce 维度后的 shape；完全归约时返回元素整数类型。

`pyc-check-flat-types` 已允许 vector-of-integer，因此 vector IR 可以作为合法 emission surface 存在。对于需要标量化输出的场景，`pycc --unroll-vector` 会在优化和 legality gate 前运行 `pyc-unroll-vector`，把 vector op 展开为 per-lane scalar op。

## 5. Backend/Runtime 改进

### C++ Emitter

C++ backend 将 MLIR vector 类型映射到嵌套 `pyc::cpp::Vec`：

```text
vector<4x8xi32> -> pyc::cpp::Vec<pyc::cpp::Vec<Wire<32>, 8>, 4>
```

已支持：

- vector `v_get` / `v_create`
- scalar broadcast 和 dim broadcast
- rank-1/rank-2 reduce
- vector mux，包括 vector select signal
- vector width casts 的 emitter disambiguation：`trunc_vec`、`zext_vec`、`sext_vec`

此前 C++ emitter 在 vector mux、vector zext/sext/trunc 上容易被标量模板 overload 抢走调用；现在用 `mux_vec` 和 `*_vec` helper 明确走 vector-only overload。

### Verilog Emitter

Verilog backend 直接发射 unpacked vector lanes，并支持：

- `v_broadcast_dim` 按 result lane 映射回 source lane。
- rank-1/rank-2 reduce 生成 balanced binary expression，而不是线性长链。
- vector elementwise ops 按 lane 发射。

balanced reduce 对 dim-reduce 很重要：它降低组合深度，避免 `or_reduce` / `and_reduce` / `add_reduce` 在 lane 很多时成为一条长路径。

### Vector Unroll Pass

`pyc-unroll-vector` 是 IR-level pass，不是 backend-only fallback。它覆盖：

- elementwise vector op
- `v_get`
- `v_broadcast`
- `v_broadcast_dim`
- `v_or_reduce` / `v_and_reduce` / `v_add_reduce`
- vector `wire` / `assign` / `reg`

pass 通过 `v_get` 提取 scalar lane，构造标量 op，再用 `v_create` 还原 vector，或在 reduce 场景中直接生成标量/低维结果。

## 6. Broadcast 和 Priority Mux

### `Vec.broadcast(dim, size)`

`Vec.broadcast()` 用 `Vec.sig` 生成 `pyc.v_broadcast_dim`，在指定位置插入新维度：

```python
v.broadcast(dim=0, size=6)  # Vec<8,T> -> Vec<6, Vec<8,T>>
v.broadcast(dim=1, size=6)  # Vec<8,T> -> Vec<8, Vec<6,T>>
```

这让外积匹配可以写成自然的矩阵表达：

```python
src_bc = src_ptag.broadcast(dim=1, size=lanes)
lane_bc = lane_ptag.broadcast(dim=0, size=lanes)
match = src_valid_bc & lane_valid_bc & (src_bc == lane_bc)
```

### `priority_mux`

原 `onehot_mux` 已替换为 balanced `priority_mux`。新语义：

- selector 从左到右优先。
- selector one-hot 时得到精确候选值。
- 多 bit 同时为 1 时结果仍定义良好：最左候选 wins。
- 结构是 balanced binary tree，组合深度比线性 mux 链更可控。

## 7. 设计侧落地

### `IssueQueue`

`designs/IssueQueue/issq.py` 使用 `Vec` 表达 ready table、age matrix、wake 匹配、issue winner 和 allocation 逻辑。典型用法包括：

- `(tags == ptag_wire) & ready_v).or_reduce()`
- `Vec(issue_sel).or_reduce(dim=0)`
- `sel.priority_mux(fields["..."], zero=...)`
- `Vec(issue_valid).reduce_sum(width=...)`

这验证了 `Vec` 对实际多端口调度逻辑的表达能力。

### `BypassUnit`

`designs/BypassUnit/bypass_unit.py` 已从 per-lane scalar ports 改为 vector-shaped ports：

- `w1_valid` / `w2_valid` / `w3_valid`
- `w*_ptag`
- `w*_ptype`
- `w*_data`
- `i2_srcL_*`
- `i2_srcR_*`

核心 bypass search 现在通过 broadcast 形成 `N sources x M lanes` 的 match 矩阵：

```python
sv_bc = src_valid_v.broadcast(dim=1, size=n)
sp_bc = src_ptag_v.broadcast(dim=1, size=n)
st_bc = src_ptype_v.broadcast(dim=1, size=n)
lv_bc = lane_valid.broadcast(dim=0, size=n)
lp_bc = lane_ptag.broadcast(dim=0, size=n)
lt_bc = lane_ptype.broadcast(dim=0, size=n)
match = sv_bc & lv_bc & (lp_bc == sp_bc) & (lt_bc == st_bc)
has = match.or_reduce(dim=1)
```

每个 source row 再用 `priority_mux` 选择 lane id 和 data。testbench 对 vector-shaped ports 做 lane packing：

```text
lane i -> bits [i * lane_width +: lane_width]
```

同时 SVA match 表达式改为 vector slice 形式，例如 `w1_ptag[lane * ptag_w +: ptag_w]`。

常量复用也已收敛：`BypassUnit` 中重复的 zero/one/stage 常量改为预计算后复用，当前 eager MLIR 的 `pyc.constant` 数量从 70 降到 16，同时 `v_broadcast_dim` 和 `v_or_reduce` 结构保持不变。

## 8. 测试和 Gate

新增 `tests/vec` 作为 Vec operator 测试矩阵：

- `cases.py` 定义算术、位运算、比较、shift、select、reduce 等 case。
- `generate.py` 生成临时 DUT/testbench。
- `runner.py` 负责 build、检查 IR token、运行 C++ binary，并在有 Verilator 时检查 Verilog。
- `test_vec_ops.py` 额外覆盖 vector-shaped IO 和 2-D dim reduce emit+pycc。

推荐 gate：

```bash
PYTHONPATH=compiler/frontend pytest tests/vec -m vec
PYTHONPATH=compiler/frontend pytest tests/vec -m "vec and not slow"
bash tests/vec/run_vec_ops.sh
make vec-smoke
```

已验证命令：

```bash
PYTHONPATH=compiler/frontend .venv/bin/python -m pytest tests/vec -m vec -q
```

结果：`70 passed in 107.75s`。

dim-reduce standalone gate 会生成：

```python
a = m.input("a", width=1, shape=(2, 3))
m.output("or0", a.or_reduce(dim=0))
m.output("or1", a.or_reduce(dim=1))
m.output("and0", a.and_reduce(dim=0))
m.output("and1", a.and_reduce(dim=1))
m.output("sum0", a.reduce_sum(dim=0))
m.output("sum1", a.reduce_sum(dim=1))
```

并检查 `.pyc` 中出现 `vector<`、`pyc.v_or_reduce`、`pyc.v_and_reduce`、`pyc.v_add_reduce`，随后跑 C++/Verilog pycc 发射和 C++ manifest syntax check。

## 9. 当前限制和后续

- MLIR verifier/emitter 当前只承诺 rank-1/rank-2 reduce；更高维 reduce 需要继续扩展 verifier、emitter 和 tests。
- `dim=None` 只允许 rank-1；rank>1 必须显式传 `dim`。
- `Vec.broadcast()` 要求输入是可向量化 `Vec`；普通 eager `Vec([wire...])` 会在构造时缓存 vector signal，因此可以直接 broadcast。非矩形、跨 module 或混合 leaf 类型的 irregular Vec 仍会拒绝。
- signed div/rem 的 Verilator 行为仍和 bit-accurate oracle 有差异，因此相关 case 默认只跑 C++ backend。

## 10. 结论

`vec` 分支已经把 `Vec` 从 Python 侧容器推进为贯穿 frontend、MLIR、backend、runtime 和测试的 first-class vector IR 能力。dim reduce 和 broadcast 是本轮最关键的可组合能力：它们让 BypassUnit/IssueQueue 这类多 lane 结构可以用矩阵表达写清楚，同时通过 verifier、balanced reduce、vector unroll pass 和 backend tests 保持 pyc4.0 的 gate-first 约束。
