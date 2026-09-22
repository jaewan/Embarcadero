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

- Obtain the owner's project license choice and inventory third-party licenses;
  no project license is selected by these changes.
- Enable private vulnerability reporting and identify maintaining contacts.
- Enumerate supported order/ACK/sink modes and reject unsupported combinations.
- Pass clean bootstrap, supported tests, instrumented tests, and owned DRAM
  integration on the release revision; preserve failures as well as passes.
- Keep finite DRAM, sustained reuse, disk durability, real CXL, and independent
  host recovery evidence separate. No build or short pilot closes those gates.

The legacy setup script remains for historical hosts. Prefer the disposable
bootstrap for release qualification instead of applying its host tuning.
