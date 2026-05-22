"""Top-K hardware module for pyCircuit (unified single-engine version).

A streaming Top-K compute module: one ``compile_cycle_aware`` produces ONE
hardware module that supports

  - ``fmt_sel`` ∈ {bf16, fp16, fp32, fp8_e4m3, fp4_e2m1}  (runtime 3-bit input)
  - ``k_in``    ∈ [1, K_MAX]                              (runtime input)

Architecture (single 128-cas bank, time-multiplexed):

  - SORT  : log2(P)·(log2(P)+1)/2 layers, each with P/2 cas-swap ops
            applied in one cycle through the shared bank.
  - MERGE : 2·(log2(P)+1) half-layers, each with P/2 cas-swap ops; full
            merge of (running row || carry) takes two cycles per layer.
  - SRAM  : K_MAX/P rows of P (val,idx) pairs; transparent init with
            ``init_done`` bit-vector lets unwritten rows read as a row
            of ``-inf``-equivalent sentinels.

See ``README.md`` for the architecture overview, walkthrough, and
``figures/topk_all.png`` for the rendered diagrams.
"""
