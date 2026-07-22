# Fig. 3 failure-recovery summary

- Implementation commit: `8351459f420870dad3e39cd6630c9483386d9a05`
- Working tree at launch: clean
- Pass: `20260718T134142Z`
- Workload: 20 GiB, 1 KiB records, four brokers, remote client `c4`
- Failure: one broker killed 1.8 s after publishing begins
- Ordering ACK: ACK1; replication disabled to isolate ordering recovery
- Prefix-safe lease: 5 s

| Mode | Median bandwidth | Kill→first failed send | Kill→first reconnect | Kill→ACK frontier | Retained suffix |
|---|---:|---:|---:|---:|---:|
| Arrival order | 5,066.42 MB/s | 116 ms | 117 ms | 196 ms | n/a |
| Prefix safe | 1,746.94 MB/s | 114 ms | 115 ms | 7,200 ms | 512 batches |

Every prefix-safe measurement observed one epoch fence, replayed 512–513
pool-owned batches through the normal publish/ACK path, and completed with zero
unacknowledged batches. The prefix-safe bandwidth includes the deliberate
lease-bounded ACK pause and is therefore an end-to-end availability result, not
a steady-state ordering-path ceiling.
