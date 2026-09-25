# Coral NPU in pyCircuit

Single-issue RV32I slice of [Coral NPU](https://github.com/google-coral/coralnpu),
rewritten in pyCircuit. The Chisel and SystemVerilog sources stay in the local
clone at `/home/lidongzhe/coralnpu` and are not built here.

S1 covers integer ALU, branch, `jal`/`jalr`, `lui`/`auipc` in the decoder, and a
mailbox store to address 0 (`tohost`). The vector command port and the matrix
tile word are tied idle. Four-wide issue, full DTCM, RVV execution, and GEMM
are later stages.

```bash
PYTHONPATH=compiler/frontend:designs/CoralNPU-pyc \
python3 -m pycircuit.cli build \
  designs/CoralNPU-pyc/tb/tb_nop.py \
  --out-dir /tmp/coralnpu_nop --target cpp --jobs 4
```

The same command with `tb_alu.py` or `tb_branch.py` covers the other two S1 tests.
