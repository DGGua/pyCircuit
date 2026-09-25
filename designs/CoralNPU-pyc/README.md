# Coral NPU in pyCircuit

Single-issue RV32I slice of [Coral NPU](https://github.com/google-coral/coralnpu),
rewritten in pyCircuit. The Chisel and SystemVerilog sources stay in the local
clone at `/home/lidongzhe/coralnpu` and are not built here.

The scalar core issues up to four instructions from a fetch window. A later
lane waits when it reads or writes a register an earlier lane writes. Branches,
jumps, loads, stores, multiplies, and divides end the window. Multiply commits
on the second cycle. Divide steps one bit per cycle. DTCM is the 32 KB window
at `0x00010000`. Address 0 is still the `tohost` mailbox. The vector command
port and the matrix tile word stay idle. RVV execution and GEMM are later stages.

```bash
PYTHONPATH=compiler/frontend:designs/CoralNPU-pyc \
python3 -m pycircuit.cli build \
  designs/CoralNPU-pyc/tb/tb_nop.py \
  --out-dir /tmp/coralnpu_nop --target cpp --jobs 4
```

The same command with `tb_alu.py`, `tb_branch.py`, `tb_load_store.py`, or
`tb_muldiv.py` covers the other tests. A packet may take more than one cycle
when a later instruction depends on an earlier one, or when the packet ends on
a branch or a memory operation.
