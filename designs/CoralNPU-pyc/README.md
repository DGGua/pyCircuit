# Coral NPU in pyCircuit

Single-issue RV32I slice of [Coral NPU](https://github.com/google-coral/coralnpu),
rewritten in pyCircuit. The Chisel and SystemVerilog sources stay in the local
clone at `/home/lidongzhe/coralnpu` and are not built here.

The scalar core issues up to four instructions from a fetch window. A later
lane waits when it reads or writes a register an earlier lane writes. Branches,
jumps, loads, stores, multiplies, divides, and vector ops end the window.
Multiply commits on the second cycle. Divide steps one bit per cycle. DTCM is
the 32 KB window at `0x00010000`. Address 0 is still the `tohost` mailbox.

Integer RVV is VLEN=128, SEW=32, LMUL=1 (four lanes). `vsetvli` writes VLMAX=4.
`vadd.vv`, `vand.vv`, `vor.vv`, and `vxor.vv` retire in the issue cycle.
`vle32.v` and `vse32.v` move one word per cycle through the same DTCM port.
The matrix tile word stays idle. GEMM is a later stage.

```bash
PYTHONPATH=compiler/frontend:designs/CoralNPU-pyc \
python3 -m pycircuit.cli build \
  designs/CoralNPU-pyc/tb/tb_nop.py \
  --out-dir /tmp/coralnpu_nop --target cpp --jobs 4
```

The same command with `tb_alu.py`, `tb_branch.py`, `tb_load_store.py`,
`tb_muldiv.py`, or `tb_rvv.py` covers the other tests. A packet may take more
than one cycle when a later instruction depends on an earlier one, or when the
packet ends on a branch, a memory operation, or a vector instruction.
