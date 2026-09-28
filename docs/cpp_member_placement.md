# C++ member placement in NewCircuit

NewCircuit applies C++ member placement after SimGraph optimization, while
building the verified SimulationPlan. A temporary whose definition and every
executed use belong to one generated method can live in that method instead of
the SimObject header. Expressions eliminated by inlining or replication do not
need a member. This reduces header size and the host compiler's work on large
SimObject types.

The optimization is always enabled for C++ emission. It is adapted from
`feat/cpp-localize-pch` (`6e07126`, with the review fixes in `05093c2`), preserving
NewCircuit's SimGraph and SimulationPlan pipeline. It does not restore the older
`pyc-cpp-placement` MLIR pass or its emitter interface.

## Storage contract

- Ports, primitive bindings, state, named probes and observation roots retain
  stable storage in the SimObject.
- Values read by another method, input caches or activity change detection also
  retain member storage across evaluations.
- Method-local values are declared before the method's statements, including
  values assigned by both arms of a shared mux condition.
- Nested combinational regions keep their existing legality and binding rules.
- The plan verifier checks ownership and lifetime before emission. The C++
  emitter consumes the plan's storage decisions.

Placement retains GSIM group boundaries and their activity protocol. Pure
expression sequences can use locality-aware ordering and weighted chunk cuts
inside a group or combinational region. Sequences containing shared-condition
batches or nested-region steps keep their outer statement order.
The cut search uses at most 64 deterministic deficit states and a bounded work
estimate for large chunk limits; small searches retain exact dynamic programming.
A candidate is accepted only when its weighted crossing cost is no worse than
the original fixed cuts.

Decisions 0001, 0004, 0121, 0141 and 0147 remain binding. Hardware semantics and
the Verilog pipeline are unchanged.

## Build integration

```bash
pycc design.pyc --emit=cpp --out-dir out --cpp-split=module --cpp-pch
python3 -m pycircuit.cli build design.py --out-dir out --target cpp --cpp-pch
```

PCH is optional and independent of member placement; see
[device PCH](cpp_device_pch.md). Large state-update, initialization and probe
registration methods also use bounded helpers so method placement and file
sharding can remain effective.

The C++ manifest's `profile_summary.cpp_placement` records `struct_members`,
`local_in_method`, `probe_pinned_struct`, `cross_part_promoted`,
`scheduled_cross_method`, `scheduled_cut_weight` and `omitted_values`.
The optional profile JSON exposes the same placement summary.
Cut weights describe the retained cross-method values: a scalar has weight
`1 + ceil(width / 64)`, multiplied by the element count for a vector.

The member-placement and large-state gates under `tests/newcircuit/` validate
storage lifetime, probe visibility, activity skips, chunk boundaries and C++ /
Verilog behavior. PCH integration has a separate smoke gate under
`compiler/mlir/test/`.
