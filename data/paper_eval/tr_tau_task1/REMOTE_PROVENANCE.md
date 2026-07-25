# Remote host verification (2026-07-25) — for Task 3 remote publishers

Local source under commit d987cf53 (PBR-lock fix committed). Remotes synced via
scripts/rsync_code_to_remote.sh (source only; build/ and .git/ excluded, so remote
`git HEAD` stays at its base commit while working-tree source matches local —
verified by source file hashes below). throughput_test rebuilt on each remote with
-DCOLLECT_LATENCY_STATS=ON.

Local reference source hashes (must match remote):
- src/cxl_manager/cxl_datastructure.h = 7b41c5c8ba1d2d51b1c1e6928ee582c13cfd0d508050c7c6f9b424a55f8eff7a
- src/client/publisher.cc              = 58fb2dfa92c1fd66bfb5eec2b2aa33635b935fef48e004cad579b446f61f8dde

| host | hostname | base git | source match | throughput_test sha256 | status |
|------|----------|----------|--------------|------------------------|--------|
| c4 | mos182 | 16309d06 | YES | 01dd66702a90443944d56f2dbea2d564bad0ec4ec73f33d05292d1995e244ad6 | VERIFIED |
| c2 | mos144 | 0a3460a  | YES | af1d7919f6c0d264492af4f865b795a9b0381a4ace8bbd762d6cb3f4f39ab479 | VERIFIED |
| c3 | mos181 | 16309d06 | source synced | (stale) | BUILD FAILED: host Boost 1.90 cmake-configure error (BoostConfig.cmake:141) |

## Roster decision
Task 3 needs 2 remote publishers. Paper default is c4+c3; c3's build is broken by a
host-local Boost/cmake issue (the CMakeLists-mtime reconfigure pitfall). Use
CLIENT_HOSTS_CSV="c4 c2" (both VERIFIED, source-matched, remote over 100GbE). Both
are remote-NIC publishers; the substitution is documented and does not change the
2-remote-publisher topology. c2 also serves the RF=1 latency package (REMOTE_CLIENT_HOST=c2).
