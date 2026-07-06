# TLC results summary

TLC on `broker` (OpenJDK 21), safety-only (`-deadlock`), symmetry reduction.
Reproduce with `bash spec/run_all.sh`. Raw output: `spec/results/<cfg>.txt`.

| Scenario (cfg) | W2 | Sessions | Result | Distinct states | Depth | Time |
|---|---|---|---|---|---|---|
| `core` | — | 2 | ✅ no error | 57,018,923 | 47 | 5m32s |
| `crash_pre_post_pbr` | #1 | 2 | ✅ no error | 154,619,513 | 49 | 15m |
| `retransmit_race` | #2 | 2 | ✅ no error | 154,619,513 | 49 | 15m |
| `dup_in_guard_window` | #3 (named) | 1 | ✅ no error | 388,282 | 32 | 4s |
| `lease_false_positive` | #4 | 2 | ✅ no error | 228,054,453 | 49 | 23m |
| `stale_cv_ack_relay` | #5 (priority) | 1 | ✅ no error | 180,496 | 30 | 3s |
| `stale_cv_bug_demo` | #5 (necessity) | 1 | ⚠ **expected** `AckedIffCommitted` violation | 1,524 | 11 | 1s |
| `client_crash_midstream` | #6 | 2 | ✅ no error | 154,619,513 | 49 | 15m |
| `seq_failover_open_gap` | #7 | 1 | ✅ no error | 388,282 | 32 | 4s |
| `module_loss_ack_window` | #8 | 1 | ✅ no error | 13,965 | 26 | 1s |
| `reader_agreement` | +D5 clause 3 | 1 | ✅ no error | 565,954 | 32 | 5s |
| `exactly_once_fence` | +D1 exactly-once | 1 | ✅ no error | 53,820 | 31 | 1s |

**Every safety scenario passes.** `stale_cv_bug_demo` (the ACK-relay epoch check
turned OFF) produces the intended counterexample — a broker relays an ACK for a
failover-truncated batch (`AckedIffCommitted` violated) — proving the epoch check
(D2 / W2 #5) is load-bearing. With the check ON (`stale_cv_ack_relay`), safety holds.

## Scope notes (see spec/README.md for the full "checked vs argued" boundary)

- Safety-only; liveness argued in prose.
- 2 clients / 2 brokers / `MaxSeq`=2 for non-failover scenarios; **1 client** for
  the failover- and feature-heavy scenarios (the checked properties are
  per-session; cross-session interleaving is covered by the 2-client scenarios) so
  the state space exhausts. ≤1 failover, ≤1 re-open.
- A 2-client run of the priority stale-CV scenario (earlier module revision)
  explored >2×10⁸ distinct states without a violation before being cut for time
  (bounded corroboration of the 1-client exhaustive result).
