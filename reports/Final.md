
## 1. VerilogEmitter 的输入

输入是已经通过 `pycc` 检查和优化的 MLIR：

```mlir
func.func @counter(
    %clk: !pyc.clock,
    %rst: !pyc.reset,
    %enable: i1
) -> i8 {
    %next = pyc.add ...
    %q = pyc.reg ...
    func.return %q : i8
}
```

此时：

- 模块层级已经确定；
- `if/for` 等控制结构已经降低；
- 组合环、类型和时钟域已经检查；
- cycle balance 等额外状态如果存在，也已经表现为显式的 `pyc.reg`。

因此：

> VerilogEmitter 不决定是否增加寄存器，只翻译 IR 中已有的寄存器。

核心文件是：

```text
compiler/mlir/lib/Emit/VerilogEmitter.cpp
```

---

## 2. 入口函数

有两个主要入口：

```cpp
emitVerilog(module, os, opts)
emitVerilogFunc(module, func, os, opts)
```

区别是：

- `emitVerilog()`：把一个 MLIR module 中的所有 `func.func` 写入同一文件；
- `emitVerilogFunc()`：只输出指定硬件模块，当前输出目录模式主要使用它。

`pycc --emit=verilog --out-dir ...` 会遍历每个非声明函数：

```cpp
for (auto f : module->getOps<func::FuncOp>()) {
    emitVerilogFunc(module, f, os, opts);
}
```

于是每个硬件模块生成独立的：

```text
ModuleA.v
ModuleB.v
Top.v
```

---

## 3. 单个模块的生成结构

`emitFunc()` 大致按以下顺序工作：

```text
生成 module 端口
    ↓
为 IR 中间值声明 wire
    ↓
收集并分类 operation
    ↓
生成组合 assign
    ↓
生成子模块实例
    ↓
生成时序 primitive 实例
    ↓
连接模块输出
    ↓
endmodule
```

### 3.1 生成模块和端口

```mlir
func.func @counter(...)
```

生成：

```verilog
module counter (
    input clk,
    input rst,
    input enable,
    output [7:0] count
);
```

端口名称来自 IR 中的：

```mlir
arg_names
result_names
```

Emitter 还会进行标识符清理和重名处理。

---

## 4. 中间值变成 wire

Emitter 遍历函数中所有 operation 的 result：

```cpp
f.walk(...)
```

为每个结果分配名称并声明：

```verilog
wire [7:0] pyc_add_11;
wire [7:0] pyc_mux_12;
wire [7:0] pyc_reg_7;
```

需要注意：

```verilog
wire [7:0] temp;
```

只是连线，不等于 8 位寄存器。

真正代表存储资源的是后面的：

```verilog
pyc_reg #(.WIDTH(8)) ...
```

---

## 5. Operation 分类

Emitter 把顶层 operation 分成三类：

```text
combAssignOps：组合逻辑
instOps：子模块实例
seqInstOps：寄存器、FIFO、memory 等状态单元
```

### 组合逻辑

包括：

```text
constant
add/sub/mul/div
mux
and/or/xor/not
eq/ult/slt
trunc/zext/sext
extract/concat
shift
assign
comb
assert
```

### 子模块

```text
pyc.instance
```

### 时序 primitive

```text
pyc.reg
pyc.fifo
pyc.byte_mem
pyc.sync_mem
pyc.sync_mem_dp
pyc.async_fifo
pyc.cdc_sync
```

Emitter 会进行确定性排序；组合逻辑还会按照依赖关系排序，使生成代码保持稳定、易于阅读。

---

## 6. 组合逻辑生成 `assign`

例如：

```mlir
%add = pyc.add %a, %b : i8
%mux = pyc.mux %sel, %add, %old : i8
```

生成：

```verilog
assign pyc_add = a + b;
assign pyc_mux = sel ? pyc_add : old;
```

常见映射为：

```text
pyc.add      → +
pyc.sub      → -
pyc.mul      → *
pyc.mux      → ? :
pyc.and      → &
pyc.or       → |
pyc.xor      → ^
pyc.not      → ~
pyc.eq       → ==
pyc.ult      → <
pyc.slt      → $signed(a) < $signed(b)
pyc.concat   → {a, b}
pyc.extract  → a[msb:lsb]
```

`pyc.comb` 也会被展开为若干连续赋值。

---

## 7. `pyc.assert` 的生成

仿真断言会生成：

```verilog
`ifndef SYNTHESIS
always @(*) begin
    if (!(condition))
        $fatal(1, "assert message");
end
`endif
```

因此：

- Verilator 仿真时断言有效；
- Yosys/Vivado 综合定义 `SYNTHESIS` 时会排除；
- 不会生成实际 FPGA 断言电路。

---

## 8. 子模块生成实例

```mlir
%out = pyc.instance %in {
    callee = @Child,
    name = "child0"
}
```

生成：

```verilog
Child child0 (
    .in_data(in_data),
    .out_data(out_data)
);
```

Emitter 会：

1. 根据 `callee` 查找对应的 `func.func`；
2. 读取它的输入和输出端口；
3. 检查 operand/result 数量；
4. 生成命名端口连接。

因此 module 层级在这里一对一变成 Verilog 层级。

---

## 9. 状态操作生成 primitive

### 寄存器

```mlir
%q = pyc.reg %clk, %rst, %en, %next, %init : i8
```

生成：

```verilog
pyc_reg #(.WIDTH(8)) q_inst (
    .clk(clk),
    .rst(rst),
    .en(en),
    .d(next),
    .init(init),
    .q(q)
);
```

真正的 `always @(posedge clk)` 位于：

```text
runtime/verilog/pyc_reg.v
```

### 其他状态器件

```text
pyc.fifo        → pyc_fifo
pyc.byte_mem    → pyc_byte_mem
pyc.sync_mem    → pyc_sync_mem
pyc.sync_mem_dp → pyc_sync_mem_dp
pyc.async_fifo  → pyc_async_fifo
pyc.cdc_sync    → pyc_cdc_sync
```

因此 emitter 生成的是：

```text
设计模块
+
通用 primitive 实例
```

而不是在每个模块中重复展开 FIFO、memory 和寄存器实现。

---

## 10. 连接模块输出

MLIR 末尾的：

```mlir
func.return %q : i8
```

生成：

```verilog
assign count = q;
```

最后输出：

```verilog
endmodule
```

---

## 11. 输出目录结构

在输出目录模式下，`pycc` 通常生成：

```text
verilog/
├── pyc_primitives.v
├── ModuleA.v
├── ModuleB.v
├── Top.v
├── manifest.json
├── compile_stats.json
└── yosys_synth.ys
```

### `pyc_primitives.v`

把以下运行时 primitive 合并到一个文件：

```text
pyc_reg
pyc_fifo
pyc_byte_mem
pyc_sync_mem
...
```

### `manifest.json`

记录：

```text
顶层模块名称
生成的 Verilog 文件列表
```

### `compile_stats.json`

记录编译规模统计。

---

## 12. 从 Verilog 到 Verilator 仿真

需要特别区分：

> SystemVerilog testbench 不是由 VerilogEmitter 生成的。

Testbench payload 通过另一条专用生成路径变成：

```text
tb/TbTop.sv
```

然后 CLI 收集：

```text
TbTop.sv
pyc_primitives.v
ModuleA.v
ModuleB.v
Top.v
```

调用：

```bash
verilator \
  --binary \
  --timing \
  --trace \
  --top-module TbTop \
  TbTop.sv \
  pyc_primitives.v \
  ModuleA.v \
  ModuleB.v \
  Top.v
```

流程是：

```text
VerilogEmitter 输出 DUT RTL
        +
专用 TB 生成器输出 SystemVerilog TB
        ↓
Verilator 转换成 C++
        ↓
C++ 编译器生成仿真程序
        ↓
运行仿真程序
```

仿真时会执行：

- 时钟和复位；
- 输入激励；
- 结果检查；
- `$fatal` 断言；
- timeout；
- `$finish`；
- 可选 VCD 波形。

---

## 13. 从 Verilog 到 Yosys

Verilog 输出完成后，`pycc` 还会生成：

```text
yosys_synth.ys
```

内容类似：

```yosys
read_verilog -sv pyc_primitives.v
read_verilog -sv ModuleA.v
read_verilog -sv Top.v

hierarchy -top Top
proc
opt
memory
opt
synth -top Top
```

该脚本用于：

- 验证 Verilog 能否被综合；
- 检查模块层级；
- 将 `always` 转换成内部逻辑；
- 优化常量和冗余线网；
- 处理 memory；
- 执行通用逻辑综合；
- 查看寄存器和逻辑单元统计。

但 `pycc` 只生成脚本，不会自动执行它。需要手动运行：

```bash
yosys -s yosys_synth.ys
```

另外，该脚本没有：

```yosys
write_verilog
write_json
```

所以它默认不保存综合后网表，主要是一项综合冒烟检查。

---

## 14. 最精简的流程总结

```text
优化后的 PYC IR
        ↓
VerilogEmitter
        ├─ func.func   → module
        ├─ IR result   → wire
        ├─ 组合操作     → assign
        ├─ pyc.instance→ 子模块实例
        ├─ pyc.reg/mem → primitive 实例
        └─ func.return → 输出 assign
        ↓
模块 .v + pyc_primitives.v
        ├─ 与生成的 TB.sv 一起交给 Verilator仿真
        └─ 通过 yosys_synth.ys 进行可选综合检查
```

最关键的边界是：

```text
VerilogEmitter 不增加周期语义和状态
VerilogEmitter 只翻译输入 IR 中已经存在的硬件结构

Verilator负责 RTL 仿真
Yosys负责综合检查，不是该项目的标准仿真器
```