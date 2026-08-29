# pyCircuit (pyc4.0 / 0.40) agent instructions

This repo follows the pyc4.0 (0.35 → 0.40) hard-break upgrade plan.

## Read first (mandatory)

- `docs/updatePLAN.md`
- `docs/rfcs/pyc4.0-decisions.md`

## Codex skills (mandatory)

- Apply `$pyc4` first for the decision IDs + non-negotiable contracts.
- Use `$pyc-build-v40` when running builds/gates.
- Use `$linx-pycircuit` when touching Linx integration flows.

## Ground rules

- Gate-first: add/extend MLIR verifiers/passes before changing semantics.
- No backend-only semantic fixes: semantics live in the dialect + MLIR passes.

## Local build environment

- Do not use Docker for this repository.
- Use the existing Miniconda environment before configuring, building, or
  running tests:

  ```bash
  source "$HOME/miniconda3/etc/profile.d/conda.sh"
  conda activate llvm19
  ```

- The `llvm19` environment contains LLVM/MLIR 19.1.7 development packages and
  Python 3.11. Configure CMake with the environment's package directories:

  ```bash
  cmake -S . -B .pycircuit_out/toolchain/conda-build -G Ninja \
    -DCMAKE_PREFIX_PATH="$CONDA_PREFIX" \
    -DCMAKE_INSTALL_PREFIX="$PWD/.pycircuit_out/toolchain/conda-install" \
    -DLLVM_DIR="$CONDA_PREFIX/lib/cmake/llvm" \
    -DMLIR_DIR="$CONDA_PREFIX/lib/cmake/mlir"
  cmake --build .pycircuit_out/toolchain/conda-build \
    --target pycc pyc4_runtime -j 8
  cmake --install .pycircuit_out/toolchain/conda-build
  export PYC_TOOLCHAIN_ROOT="$PWD/.pycircuit_out/toolchain/conda-install"
  ```

