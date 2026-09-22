# Reproducible build and release gates

The default build retains broker, comparison baselines, benchmarks, and supported
tests. `cmake --preset client-minimal` followed by `cmake --build --preset
client-minimal` builds an Embarcadero client without Corfu, Scalog, or LazyLog
protocol generation/linking. Requesting Corfu from that binary fails explicitly.
The broker still contains baseline runtime adapters; disabling standalone
baseline tools does not remove those adapters from the broker.

The four independent CMake options are `EMBARCADERO_BUILD_BROKER`,
`EMBARCADERO_BUILD_BASELINE_TOOLS`, `EMBARCADERO_CLIENT_BASELINES`, and
`EMBARCADERO_BUILD_BENCHMARKS`. Full supported tests retain their baseline
dependencies. The minimal preset also disables tests; use the default Debug
preset to run the complete supported inventory. Verify an actual minimal build
with `python3 tools/build_support/check_minimal_client.py build/client-minimal`.

Run `tools/build_support/bootstrap_ubuntu.sh` only inside a disposable Ubuntu 24.04
container/rootfs with `EMBARCADERO_DISPOSABLE_ROOTFS=1`. It installs packages in
that rootfs, builds Folly at an immutable commit, and records package versions.
It never reserves hugepages, changes sysctls, alters cgroups, or configures CXL.
The GitHub workflow performs a clean full build/test and minimal-client build.
No live cluster or 64 GiB mapping is part of ordinary CI.

Sanitizer configurations instrument fetched C/C++ dependencies as well as
project targets. Use a separate build directory and rebuild those dependencies:
Abseil changes container layout under sanitizer feature macros, so mixing old
uninstrumented archives with instrumented header instantiations is invalid.
ASan/UBSan and TSan builds omit the legacy libc/libstdc++ compatibility shim and
use the build platform's native runtime; symbol interposition during loader
initialization conflicts with sanitizer interceptors. Normal builds retain the
shim. Neither a TSan preset nor a successful configure implies a TSan-clean run.

Folly is pinned to `213881d77db9d36335ebca18d2d30fcf4c51b2d5` and gRPC to
`12161ee3aa7c216741cd7c406573abc0df1d0926`; gRPC's submodules supply its dependency
revisions. Ubuntu package versions are recorded in `ubuntu-packages.tsv`.
The Ubuntu image tag and distribution package repositories can move: this is a
reproducible procedure with recorded resolved versions, not a bit-identical or
fully snapshot-pinned supply chain. Preserve the rootfs/image digest, package
inventory, source snapshot and build manifest with each release. Do not replace
unavailable historical dependencies silently.

`tools/build_support/build_manifest.py BUILD --output manifest.json` records cache,
compile commands, registered tests, binary hashes, linker dependencies and
installed package versions. Archive the exact source bytes as well when the
working tree is dirty; a commit plus status listing cannot reconstruct edits.

Before public release:

- Preserve the project [Apache-2.0 license](../LICENSE), [attribution](../NOTICE),
  and third-party license notices when packaging the release. The project
  license was selected at the owner's direction, following Ray and vLLM.
- Enable private vulnerability reporting and identify maintaining contacts.
- Enumerate supported order/ACK/sink modes and reject unsupported combinations.
- Pass clean bootstrap, supported tests, instrumented tests, and owned DRAM
  integration on the release revision; preserve failures as well as passes.
- Keep finite DRAM, sustained reuse, disk durability, real CXL, and independent
  host recovery evidence separate. No build or short pilot closes those gates.

The legacy setup script remains for historical hosts. Prefer the disposable
bootstrap for release qualification instead of applying its host tuning.

The 2026-09-22 validation used a fresh Ubuntu 24.04.3 base rootfs under a
user-local PRoot, with packages and Folly installed inside that rootfs. Archived
source snapshots `03` and `08` passed the full build, all 46 supported CTests, and the minimal
client build and dependency-exclusion check. The clean bootstrap exposed a
missing GoogleMock development package; the bootstrap now installs it and CMake
requires its headers. No host package changes were needed. Source archives,
resolved package versions, binary manifests, and both successful and failed
bootstrap logs are retained in `results/refactor-build/2026-09-22/` (ignored
generated evidence). These checks establish build/test portability; host
performance comparisons use separately matched compiler flags and dependency
binaries.
Later fault-driver or harness corrections have their own source archives and
targeted checks; they are not retroactively covered by that clean-build result.
Snapshot `09` subsequently passed the clean full build and all 47 supported
CTests, including the new actual EpochBuffer shutdown test. Its only production
changes from `08` are in the broker's Topic implementation; the validated minimal
client inputs are unchanged. The EpochBuffer test also passed a focused
ASan/UBSan run with leak detection.
Snapshot `08` also includes the production-linked publisher rollover regression
for recovery after producer input finishes. Initial partial ASan/UBSan attempts
failed before `main`; their logs remain archived. Rebuilding fetched dependencies
with consistent instrumentation and using the native runtime subsequently passed
all ten linked publisher rollover cases with leak detection and UBSan halt-on-error.
The CMake sanitizer configurations now enforce those two requirements, and CI
includes this linked test. This is component qualification, not a fully
instrumented live cluster or a hosted CI execution claim.

## Focused ThreadSanitizer checks

`bash tools/build_support/run_tsan_components.sh /tmp/embarcadero-tsan-components`
compiles and runs the actual bounded-reservation, session-admission/publication,
and single-topic admission concurrency tests under TSan. It uses `setarch -R`
for each test process to avoid the GCC runtime's unexpected-mapping failure on
high-ASLR kernels; it changes no host sysctl. Unsupported process personality
or race reports fail the command. This is focused component coverage, not a
full instrumented cluster or a model of noncoherent CXL cache visibility.

For the larger production-linked fixtures, run
`bash tools/build_support/run_tsan_linked.sh /tmp/embarcadero-tsan-linked`.
Additional CMake arguments can select an existing pinned gRPC source checkout.
The script instruments fetched dependencies consistently, applies the process
address-layout workaround to build-time generators as well as tests, and fixes
that process's timezone to UTC. It tests the actual publisher, Topic publication,
disk replication, configuration, framing, and epoch shutdown paths. It contains
no race suppressions and does not substitute for a fully instrumented live cluster.

Follow-up source 11 passed 51 registered CTest targets and a fresh minimal-client
build on this host. The extracted publisher passed 10 ASan/UBSan tests with leak
detection; the corrected Topic fixture and five other linked targets passed
TSan, as did the three small concurrency fixtures. See the
[follow-up evidence](reviews/2026-09-22-followup-results.md) for exact source
identities, the test-only post-freeze correction, initial failures and scope.
