# Historical end-to-end scripts

This directory preserves research scripts with machine-specific launch, configuration, and cleanup assumptions. They are excluded from ordinary CTest by default. Their presence does not establish a current correctness, durability, or failover result.

Use the [current test guide](../README.md) for build and validation commands, the [supported command index](../../docs/development-commands.md) for owned orchestration, and the [DRAM development guide](../../docs/development-dram.md) for local smoke and production fault tests. The supported local profile uses a 64 GiB region with explicit emulation; a 4–32 GiB mapping cannot hold the production metadata.

The historical inventory includes basic publishing, ORDER0/ACK1, ORDER5 session fencing and unacked release, explicit replication, segment allocation, and sequencer-only topology scripts, plus launch and NUMA helpers. `run_all.sh` and `TEST_EXECUTION_GUIDE.md` describe older workflows; they are retained as research references, not the supported developer entrypoint. The retired `test_basic_publish.sh` is not a template for new tests.

Add new live regressions under [integration](../integration/) using the owned runner and native protocol declarations. Recorded qualification scope and remaining limits belong in the [support matrix](../../docs/support-matrix.md) and [completion ledger](../../docs/reviews/2026-09-22-refactoring-completion-plan.md).
