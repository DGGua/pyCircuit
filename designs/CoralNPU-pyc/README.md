# Coral NPU in pyCircuit

Single-issue RV32I slice of [Coral NPU](https://github.com/google-coral/coralnpu),
rewritten in pyCircuit. The Chisel and SystemVerilog sources stay in the local
clone at `/home/lidongzhe/coralnpu` and are not built here.

S1 covers integer ALU, branch, `jal`/`jalr`, `lui`/`auipc`, and a mailbox store
to address 0 (`tohost`). S2 adds RV32M (`mul` takes 2 cycles, `div`/`rem` take
34 cycles) and byte/half/word loads and stores in the 32 KB DTCM window at
`0x00010000`, including an unaligned word. The vector command port and the
matrix tile word stay idle. Four-wide issue, RVV execution, and GEMM are later
stages.

```bash
PYTHONPATH=compiler/frontend:designs/CoralNPU-pyc \
python3 -m pycircuit.cli build \
  designs/CoralNPU-pyc/tb/tb_nop.py \
  --out-dir /tmp/coralnpu_nop --target cpp --jobs 4
```

The same command with `tb_alu.py`, `tb_branch.py`, `tb_load_store.py`, or
`tb_muldiv.py` covers the other tests.
