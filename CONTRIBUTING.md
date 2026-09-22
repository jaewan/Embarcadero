# Contributing

## Build & test loop

Start in a disposable Ubuntu 24.04 container/rootfs using the
[clean bootstrap](docs/development-build.md). This installs dependencies without
applying the historical setup script's host tuning. Then run:

```bash
cmake --preset debug
cmake --build --preset debug -j 8
ctest --preset debug
```

The build targets Linux/x86-64 with NUMA and CLFLUSHOPT support. CMake 3.20+, Ninja, and GTest/GoogleMock/pkg-config are required for these presets. Missing test dependencies fail configuration instead of silently omitting tests. The build writes `test-inventory.txt`. `scripts/setup/setup_dependencies.sh` remains a historical, host-changing alternative; inspect it before running it.

`release` produces a portable build within those CPU requirements; `release-native` opts into host ISA tuning. Use matched ISA/compiler/build options for performance comparisons, and record the broker's PBR reservation mode. `asan-ubsan` and `tsan` are separate instrumented configurations. Debug and sanitizer results are not throughput baselines.

Use the [isolated DRAM runner](docs/development-dram.md) for local integration on the two-node development host. Default CTest excludes historical E2E scripts that can terminate unrelated processes. Enabling `EMBARCADERO_ENABLE_UNSAFE_LEGACY_E2E` is explicitly for a disposable dedicated environment; it is not needed for the new runner.

Changes to publication, reservation, lifetime, or fencing must include production-path boundary tests and cross-review by someone other than their author. See [component ownership and invariants](docs/architecture/refactoring-boundaries.md). Keep required correctness fixes separate from performance tuning, and report any cost rather than weakening validation.

## Branches

- Branch off `main`; use descriptive prefixes: `feat/…`, `fix/…`, `perf/…`, `refactor/…`,
  `chore/…`, `docs/…`.
- Keep PRs focused and reviewable. Land structural/mechanical changes (moves, renames,
  formatting) as separate commits from behavior changes.

## Never commit generated data

Benchmark and experiment output must go in the git-ignored **`results/`** tree
(see [`results/README.md`](results/README.md)). Committing CSVs/logs previously bloated the git
history to multiple GB — see [`docs/operations/history-purge-runbook.md`](docs/operations/history-purge-runbook.md).
`.gitignore` blocks `data/`, `results/`, `**/result*.csv`, `*.log`, etc. — do not force-add them.

## Code style

- C++17. New/changed code should follow [`.clang-format`](.clang-format); format only what you
  touch: `git clang-format` (stages) or `clang-format -i <file>`. Do **not** reformat unrelated
  code in the same commit.
- Editor defaults (final newline, trimmed trailing whitespace, UTF-8, LF) are in
  [`.editorconfig`](.editorconfig).
- CXL-specific invariants are checked by the `pre-commit` hook (cache-line alignment on CXL
  structs, cache flushes on CXL writes, no manual destructor calls). The hook is interactive;
  run commits in a terminal, or use `--no-verify` for pure file-move/docs commits.

## Docs

Design/eval/ops docs live under `docs/` (see [`docs/README.md`](docs/README.md)). Update the
relevant doc when you change a protocol, memory layout, or config surface. `docs/context/codebase_map.xml`
is generated — regenerate it rather than editing by hand after structural changes.

## Clean builds and release evidence

See [build and release gates](docs/development-build.md) for the disposable Ubuntu
bootstrap, baseline-free client preset, dependency provenance, and CI contract.
See [security scope](SECURITY.md) before exposing a broker or reporting a
sensitive defect. Fault-injection builds are explicitly test-only; ordinary
builds leave `EMBARCADERO_ENABLE_FAULT_INJECTION=OFF`.
