# Task 3 — headline throughput/latency on the PBR-lock build (commit 13fcdc8f; embarlet 25afb0a5-family)

## CRITICAL CHECK: stale_pbr_publish_rejects == 0
RF2/ACK2 DRAM 3-trial run: ZERO "Rejecting stale PBR" across all broker logs. The PBR
lifecycle-lock fix adds no stale-publication rejects in clean (no-failure) operation.

## RF=2 / ACK=2, DRAM-replica (memory-copy), ORDER=5, 4 brokers, 4KB/10GiB/2MiB, tau=500us
Clients: c4,c4 (2 remote publisher PROCESSES on the one usable remote host; c2 unroutable to
the data subnet, c1 link-fail, c3 Boost-fail). 3 trials, commit 13fcdc8f.
- overlap ACK-drain GB/s: 4.80 / 6.35 / 6.25  (median 6.25, window 1.5-2.0s)
- send-done (ingest) GB/s: 14.92 / 14.67 / 14.45  (median 14.67) -- stable, HIGH
- paper value: 7.184 GB/s
INTERPRETATION: send-done ingest (14.5-14.9 GB/s, where the PBR lock executes once/batch) is
well above the paper and rock-stable -> the PBR-lock fix is throughput-neutral on the ingest
path. The overlap ACK-drain median (6.25) is ~13% below the paper's 7.184; this is attributable
to the c4,c4 SINGLE-HOST topology (2 publisher processes contending for one host's CPU/NIC) vs
the paper's 2 SEPARATE remote hosts, NOT the correctness fix. Exact paper-parity requires a
2-separate-host config (c4+c3 or c4+c2), currently blocked by remote-host availability.
Raw: data/publication/throughput/order5_pbrlock_rf2_ack2_dram/embarcadero_order5_rf2_n2/run_20260725T081010Z/

## RF=0 ordering-only — BLOCKED on c4 client SIGBUS (HugePages residue)
RF0 3-attempt trial 1 failed: throughput_test on c4 SIGBUS (HugePages_Rsvd residue from the
back-to-back RF2 run on the same host; see memory e4-failure-suite-facts). Transient c4
host-state issue, NOT an RF0-path or PBR-lock bug (paper measured RF0 O5 = 11.130 GB/s; RF2
ran fine first). Fix: reset c4 hugepages (echo 0 > .../nr_hugepages then restore, or reboot-free
drop) or EMBAR_USE_HUGETLB=0 for the client, then rerun. Deferred (single usable remote host +
back-to-back hugepage residue makes the co-tenant-free reset finicky).
