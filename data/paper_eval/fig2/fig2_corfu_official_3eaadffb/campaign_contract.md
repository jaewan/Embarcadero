# Fig2 campaign contract

- Campaign: `fig2_corfu_official_3eaadffb`
- Pass: `20260718T064355Z`
- Commit: `3eaadffb61eb3c4698aeb16a1cc845ed456ea035`
- Dirty: no

## Primary panel (append / coordination claim)
- Embar **ORDER=5 ACK=2 RF=2 DRAM replica** append→ACK vs offered load
- Sink claim: DRAM replica completion (CXL + DRAM copy; **no** media fdatasync)
- Pacing: `steady` (paper table uses steady; do not mix open_loop in one plot)
- Load points (MB/s): 100 250 500 750 1000 1500 2000
- Publisher: 1× `c4` → `10.10.10.10`
- **Primary metric:** append→ack p50/p99 (µs, batch) from `pub_ack_*`
- Deliver inset scoped ≤~270 MB/s ordered-consume ceiling

## Disk ablation (matched load = 250 MB/s)
- `fig2_embar_o5_ack2_rf2_disk` — media-durable RF2 cost (Fig1 disk contract)
- SKIP_DISK_ABLATION=1

## Mechanism ablation (matched load = 250 MB/s)
- `fig2_mech_embar_o0_ack1_rf0` — unordered floor
- `fig2_mech_embar_o5_ack1_rf0` — + ordering
- `fig2_mech_embar_o5_ack2_rf2_mem` — + DRAM RF2 (primary sink)
- `fig2_mech_embar_o5_ack2_rf2_disk` — + media-durable RF2
- Table metric: **append→ack** p50/p99

## Baselines (matched RF2 ACK2 **DRAM** — same sink as primary)
- Loads: `100 250 500 1000 2000` (INCLUDE_BASELINES=1)
- `fig2_corfu_o2_ack2_rf2_mem`, `fig2_scalog_o1_ack2_rf2_mem`

## Knobs
- Msg / bytes: 1024 B / 4294967296 B
- Epoch µs: 500
- CXL: size=77309411328 zero=metadata populate=0
- Broker ready timeout: 900s
- Replica dirs (disk ablation): /home/domin/Embarcadero/.Replication/disk0,/mnt/nvme0/replication/disk1
- Requires `-DCOLLECT_LATENCY_STATS=ON`
