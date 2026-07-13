#!/usr/bin/env bash
# Print the production chaos-matrix gate checklist (claim boundary aware).
# Does not run chaos; documents required proofs before multi-host claims.
set -euo pipefail

cat <<'EOF'
Embarcadero chaos matrix (publication→production gate)
======================================================

Single-host process failure (IN SCOPE today)
  [ ] Kill broker mid ORDER=5 ACK1 run — no ordered-prefix rollback
  [ ] Kill broker mid ORDER=5 ACK2 RF=2 run — ACK2 never covers unsynced bytes
  [ ] Kill head / sequencer — reconnect fences + recovered frontiers
  [ ] Kill after pwrite before fdatasync — restart reconstructs from synced replica only
  [ ] ENOSPC / missing replica dir — fail closed (no ACK2 advance)
  [ ] Segment rollover + restart — reconstruction across segments

Host / power domain (OUT OF SCOPE until rdma_replica + quorum)
  [ ] Distinct-host replica placement
  [ ] Quorum sequencer election with old-head fence
  [ ] Network partition without split-brain ACK
  [ ] Host power loss with multi-host RF

Observability / security prerequisites
  [ ] EMBARCADERO_STAGE_TRACE overhead measured vs control
  [ ] [EFFECTIVE_CONFIG] includes failure_domain + host_failure_tolerant=0
  [ ] mTLS on data/control paths
  [ ] Long-running soak without unbounded segment/export growth

Claim rule: do not advertise host_failure_tolerant unless
ClaimHostFailureTolerance() is true (rdma_replica + explicit config).
EOF
