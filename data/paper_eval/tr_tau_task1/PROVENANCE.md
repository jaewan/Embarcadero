# Provenance — PBR-lock build (foundation for Tasks 1/2/3)

## Source state (recorded 2026-07-25)
- root_repo_commit: `7a82b3150fcad4f5b454d5fc42edb45023621b35`
- root_repo_state: DIRTY (uncommitted PBR lifecycle-lock correctness fix — PRESERVED, not committed)
- dirty_patch: `data/paper_eval/tr_tau_task1/dirty.patch`
  - dirty_patch_sha256: `e00da69171f5e6d4aa7e4cc056be6cdef198d36cf3f2c8d06246d21e3ce4254f`
  - files: src/cxl_manager/cxl_datastructure.h, src/embarlet/goi_recovery_thread.cc,
    src/embarlet/topic.cc, src/embarlet/topic.h, test/phase2_integration_test.cc,
    test/phase3_recovery_test.cc
- paper_repo_commit: `9fefd61f3da9dda3e283215524b1d3080cdb6c61`

## PBR lifecycle-lock fix summary (the build under test)
- topic.h: `absl::Mutex pbr_slot_lifecycle_mu_` + `std::atomic<uint64_t> stale_pbr_publish_rejects_`
  + `GetStalePBRPublishRejects()`.
- cxl_datastructure.h: `BatchHeaderClaimOwnedBy(slot, expected)` — a receiver may publish
  only while the physical slot still holds its (pbr_absolute_index, batch_id) unpublished
  CLAIMED claim; writer-side ABA defense.
- topic.cc `PublishPBRSlotAfterRecv` (3160): under `pbr_slot_lifecycle_mu_`, if
  `!BatchHeaderClaimOwnedBy(...)` → `stale_pbr_publish_rejects_++` and fail closed
  (no overwrite of a reused slot).
- Task 3 acceptance: `stale_pbr_publish_rejects == 0` in clean no-failure runs.

## Build + tests (validated)
- Build: `cmake --build build --target embarlet phase2_integration_test phase3_recovery_test`
  → BUILD_EXIT=0.
- embarlet_pbrlock_sha256: `7913a9cf27ae936dea921c9543d057f75aed732ab61db427a0faf6fb1a872136`
  (build/bin/embarlet, 21117152 B)
- phase2_integration_test: 15/15 PASSED (build/test/phase2_integration_test, log phase2_test.log)
- phase3_recovery_test: 6/6 PASSED (build/test/phase3_recovery_test, log phase3_test.log)

## Task-1 code-trace conclusion (see CODE_TRACE_FINDING.md)
Release of a held suffix is EPOCH-SEAL-gated (τ), not scanner-pass-gated (P). A newly
arrived predecessor cannot release the held suffix before the next seal (fast-seal is
disabled whenever total_hold_size_>0). Release opportunities over skew window T = T/τ ≈ 3
at τ=500µs, NOT T/P ≈ 30. Empirical confirmation pending (instrumentation + τ-sweep).
