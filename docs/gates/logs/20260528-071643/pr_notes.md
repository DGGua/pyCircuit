# PR notes: C++ comb member placement

## Summary

Adds optional `--cpp-localize-members` (requires `--cpp-split=module`) to classify
comb-region temporaries as method-local `Wire<>` via MLIR pass `pyc-cpp-placement`,
reducing struct member count and C++ TU compile cost.

## Decision IDs

- **0141** (incremental build / compile-scope minimization)
- **0147** (stable emission artifacts)

## Gate evidence

- `docs/gates/logs/20260528-071643/commands.txt`
- `docs/gates/logs/20260528-071643/summary.json`
- `docs/gates/logs/20260528-071643/pyc_build.{stdout,stderr}`
- `docs/gates/logs/20260528-071643/cpp_member_placement_smoke.{stdout,stderr}`

## Testing

```bash
bash flows/scripts/run_cpp_member_placement_gate.sh
bash compiler/mlir/test/cpp_member_placement_smoke.sh
```

## Docs

- `docs/cpp_member_placement.md`
- `docs/PIPELINE.md` (optional C++ member placement section)
