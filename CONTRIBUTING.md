# Contributing

## Build & test loop

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Dependencies are installed once via `scripts/setup/setup_dependencies.sh`. The build targets
Linux/x86-64 (uses `-march=native` for CXL flush intrinsics, NUMA, and CLFLUSHOPT).

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
