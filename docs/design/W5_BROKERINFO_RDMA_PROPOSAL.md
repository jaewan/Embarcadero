# Proposal — additive RDMA (RoCEv2) fields for `heartbeat.proto`

**From:** Track 05 (RDMA variants). **To:** Track 01 (owns `src/protobuf/heartbeat.proto`).
**Status:** proposal for Track 01 to apply. Q3 answered "yes, additive — propose the exact fields."
**Context:** RoCEv2 fabric on `10.10.10.0/24`, MTU 9000, ConnectX-5 `mlx5_0`. See
`sessions/rdma_fabric_setup.md`. Design: `docs/design/W5_RDMA_VARIANTS_SPEC.md`.

## What the transport needs to bootstrap

For one-sided RDMA (RC), a peer that wants to READ/WRITE another host's region needs, per region:
its **base virtual address**, **rkey**, and **size**. To establish the RC connection it needs the
remote host's **RoCEv2 GID + GID index** (or just its RDMA IP, if we use `rdma_cm`). QP number / PSN
are only needed for the **manual-QP** bring-up path; with `librdmacm` the CM handshake carries them,
so those two fields are optional.

**Recommendation:** use `librdmacm` for connection establishment (mesh-friendly on the existing IP
subnet; no manual QPN/PSN bookkeeping across the broker+c1+c3 mesh). Then `BrokerInfo` only needs to
advertise `rdma_ip` + `gid_index` + the region descriptors. The `qp_num`/`psn` fields below are kept
for a manual-QP fast path but can stay unset.

## Proposed messages (additive — no existing field numbers change)

```proto
// One RC endpoint on a host's active mlx5_0 port (RoCEv2).
message RdmaEndpoint {
  string rdma_ip   = 1;  // 10.10.10.x on the RDMA subnet (for rdma_cm address resolution)
  bytes  gid       = 2;  // 16-byte RoCEv2 GID (IPv4-mapped ::ffff:0a0a:0a0X)
  uint32 gid_index = 3;  // local GID-table index of the RoCEv2 IPv4 entry
                         //   (per fabric doc: broker=3, c1=5, c3=5 — do NOT hardcode; scan+advertise)
  uint32 port_num  = 4;  // HCA port (1)
  uint32 path_mtu  = 5;  // active RoCE path MTU enum (negotiated 4096 on this fabric)
  uint32 qp_num    = 6;  // manual-QP path only; unset when using rdma_cm
  uint32 psn       = 7;  // manual-QP path only; unset when using rdma_cm
}

enum RdmaRegionKind {
  RDMA_REGION_UNKNOWN           = 0;
  RDMA_REGION_CONTROL_BLOCK     = 1;  // epoch / sequencer_lease / sequencer_id / committed_seq
  RDMA_REGION_COMPLETION_VECTOR = 2;  // per-broker ACK frontier
  RDMA_REGION_PBR_RING          = 3;  // per-broker BatchHeader ring
  RDMA_REGION_BLOG              = 4;  // per-broker payload log
  RDMA_REGION_SENTINEL_ARRAY    = 5;  // packed publish_commit sentinels for gather-poll (spec E2 fix)
}

// A registered MR a host exposes for one-sided access.
message RdmaRegion {
  RdmaRegionKind kind            = 1;
  int32          owner_broker_id = 2;  // single-writer owner; -1 = memserver-hosted / shared
  uint64         remote_addr     = 3;  // MR base virtual address
  uint32         rkey            = 4;  // remote key for READ/WRITE
  uint64         size            = 5;  // region size in bytes
}
```

## Additive changes to `BrokerInfo` (currently uses field numbers 1–4)

```proto
message BrokerInfo {
  int32  broker_id        = 1;
  string address          = 2;
  string network_mgr_addr = 3;
  bool   accepts_publishes = 4;
  // --- proposed additive (Track 05) ---
  RdmaEndpoint        rdma_endpoint = 5;  // this host's RC endpoint
  repeated RdmaRegion rdma_regions  = 6;  // MRs this host/memserver exposes for one-sided access
}
```

## Placement semantics (W5-A vs W5-B) the fields must express

- **W5-A (`BLOG_PLACEMENT=dram`):** each broker host advertises its own `PBR_RING` + `BLOG` regions
  with `owner_broker_id = self`. The sequencer host advertises `CONTROL_BLOCK` +
  `COMPLETION_VECTOR` (+ `SENTINEL_ARRAY` if used).
- **W5-B (`BLOG_PLACEMENT=memserver`):** the **memory server (c3)** advertises every broker's
  `PBR_RING` + `BLOG` (+ `SENTINEL_ARRAY`), each tagged with its `owner_broker_id` (the single writer)
  but hosted on c3. `CONTROL_BLOCK`/`COMPLETION_VECTOR` placement per spec §2.1 (see the open item on
  hosting `CONTROL_BLOCK` on the memserver for a consistent "state survives host death" story).

Because `RdmaRegion` carries both `kind` and `owner_broker_id` independently of *which host advertises
it*, the same schema expresses both variants — the only difference is which host lists the Blog/PBR
regions. That matches the spec's "one switch, everything else identical" rule.

## Notes for Track 01

- All additions are additive; wire-compatible with existing brokers that don't set them.
- Do not hardcode GID indices — they differ per host and only populate after the IP is assigned
  (fabric doc). Each host scans its GID table and advertises the discovered index.
- If Track 01 prefers a separate `rdma.proto` over extending `heartbeat.proto`, these messages move
  verbatim; only the `BrokerInfo` embedding needs a home. Track 05 is fine either way.
