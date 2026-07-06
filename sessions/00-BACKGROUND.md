# Session Background — read this first

You are one of several parallel Claude Code sessions working on the **Embarcadero** paper
resubmission (OSDI'27 target). Read this file fully, then read your assigned track file
(`sessions/0N-*.md`). Do only your track's work; respect the file-ownership boundaries so parallel
sessions don't collide.

---

## ⚠ Coordination addendum (v4.1 — added 2026-07-05, after sessions launched; supersedes conflicting brief text)

**Testbed RDMA fabric (brought up + VERIFIED 2026-07-05).** RoCEv2 over 100 GbE, subnet
`10.10.10.0/24`, MTU 9000. **Working cross-host RDMA WRITE confirmed:** broker `10.10.10.10` ↔ c1
`10.10.10.11` (54 Gb/s 1-QP) and broker ↔ c3 `10.10.10.13` (88 Gb/s 1-QP); jumbo pings clean;
perftest installed on broker + c3. Full recipe, GID indices, and TODO in
[`sessions/rdma_fabric_setup.md`](rdma_fabric_setup.md).
- **Still to do (needs sudo — c2/c4 lack passwordless sudo):** c2 `10.10.10.12` and c4
  `10.10.10.14` (same recipe; perftest already present). **All current config is non-persistent**
  (runtime `ip addr`); make it durable (netplan/systemd-networkd) before relying on it across reboots.
- **Host roles:** broker/moscxl = CXL host + sequencer; **c3 (mos181, 1.26 TB, sudo ✓) = dedicated
  memory server (W5-B)** — revised from c4, which lacks passwordless sudo; c1/c2/c4 = brokers/clients.
- **Track 05** cross-host runs are now **unblocked on the broker+c1+c3 triangle** (enough for an
  initial W5-A/W5-B topology); add c2/c4 for full scale. Scaffolding/loopback needs no fabric.
- Multi-host RDMA is a **hard requirement** for Track 05's evidence and the eval phase. The "upside,
  not a gate" label applies **only to multi-host *CXL***, never to RDMA.

**Shared-file ownership fixes (resolve two brief collisions).**
- `src/cxl_transport/` belongs to **Track 03** (CXL mailbox lib). **Track 04 uses `test/faultinj/`**,
  not `src/cxl_transport/faultinj/`.
- **One shared accessor seam.** Tracks 04 (fault-injection shim) and 05 (RDMA transport) both need to
  interpose on the CXL accessors. Do **not** add two seams to Track-01 files — define a **single
  `RegionAccessor` interface** that both implement. Track 01 exposes it at design-freeze; 04 and 05
  consume it. Coordinate the one interpose point through the human.
- **CMake integrator = Track 01.** Each track edits only its own subdir CMake; leave root
  `CMakeLists.txt` / `src/CMakeLists.txt` `option()` / `add_subdirectory()` additions to Track 01 (or
  the human at merge) to avoid conflicts.
- **`Paper/Text/Sec1_Introduction.tex` = Track 06 only.** Track 07 proposes intro tweaks via handoff.

**Protobuf.** `BrokerInfo` RDMA fields (gid/qpn/rkey/remote_addr) are additive and welcome, but
**Track 01 owns `src/protobuf/`** — Track 05 proposes exact fields; Track 01 applies.

---

## What Embarcadero is

A distributed, totally-ordered pub/sub **shared log over CXL disaggregated memory**. Log servers
(brokers) append payloads to per-server logs in CXL shared memory and publish 64-byte metadata
records; a sequencer polls those records over memory loads, assigns global order, and uses a hold
buffer to preserve per-client FIFO/ack ordering — without sitting on the write path. Correctness on
non-coherent CXL 2.0 relies on single-writer ownership, monotonic updates, host-local cache-line
flushes, and poll-based state transitions.

Authoritative design: `docs/design/EMBARCADERO_DEFINITIVE_DESIGN.md` and
`docs/design/CXL_MEMORY_LAYOUT_v2.md`. Overview: `README.md`, `ARCHITECTURE.md`.

## The resubmission thesis (why we're doing this) — v4

Full plan: **`Paper/improvement_plan.md`** (it is **v4**; read the parts your track touches — the
framing below is load-bearing and changed twice from earlier drafts). One sentence:

> Write-before-order logs abandoned per-client FIFO because ordering *repair* (straggler wait, gap
> fill, failure detection, retransmit) cost network rounds and died with brokers. Over RDMA the two
> properties that make repair cheap are **mutually exclusive**; CXL is the first substrate to
> provide **both at once**, so a multi-broker log can offer an **unconditional per-session prefix
> guarantee** for the first time.

**The conjunction is the thesis (not failure-independence alone).** Cheap repair needs two things
*simultaneously*: **(leg 1) failure independence** (log state outlives any host) **and (leg 2) no
shared network endpoint** (every host reaches state at memory speed with nothing to funnel through).
Over RDMA you get either but not both — Blogs in broker DRAM lose leg 1; Blogs on a memory server
lose leg 2 (the NIC becomes the funnel). CXL delivers both because it is **passive memory**. Leg 1
*alone* is the whole RDMA-disaggregated-memory literature (FaRM, Clover, Clio, LegoOS, AIFM) — so it
is **not** the contribution; the **conjunction** is, and it is *measurable on the ConnectX-5 hardware
we already own* by building both RDMA variants (W5-A/W5-B) and showing each surrenders exactly one leg.

**Validated in silicon on owned hardware (v4).** There is no accessible multi-host CXL, so **no
load-bearing claim depends on it**. The coherence-free discipline is validated in silicon on our
RDMA hardware, because **one-sided RDMA to remote memory is itself cross-host non-coherent** (stale
reads, torn multi-cacheline objects, no cheap cross-host atomics — the same hazard family CXL has).
Single-host CXL supplies the substrate numbers it can prove (latency, bandwidth, 64 B TLP atomicity);
TLA⁺ covers interleavings; multi-host CXL integration is the **disclosed residual gap** (I.6), and
any CXL access that lands is **upside, not a gate**.

Compressed: **the semantics are the capability, the conjunction is the thesis, the RDMA variants are
the in-silicon evidence, and multi-host CXL is upside — not a gate.** The prior SOSP'26 reject was of
the *execution*: the FIFO guarantee had degraded to "prefix-FIFO with causal exceptions," and
evaluations favored us. **Target venue: SOSP'27 is the realistic primary; OSDI'27 only if the
schedule genuinely lands** — neither is gated on multi-host CXL.

## Repo layout (post-reorg)

| Path | Contents |
|------|----------|
| `src/embarlet/` | Broker + **sequencer/epoch-collector** (`topic.cc`/`topic.h` — the hot, central, high-conflict file) |
| `src/cxl_manager/` | CXL coordination + baseline sequencers (scalog/lazylog/corfu, local+global) |
| `src/disk_manager/` | Replication (chain + per-baseline) |
| `src/network_manager/`, `src/client/`, `src/common/`, `src/protobuf/` | networking, pub/sub client lib, config/utils, gRPC schemas |
| `benchmarks/` | `kv_store/`, `micro/`, `sequencer/`, `sequencer5_ablation/` |
| `test/` | unit + e2e (CTest) |
| `scripts/` | experiment launchers (`scripts/README.md`), `setup/`, `lib/`, `publication/`, `plot/`, `network/` |
| `docs/` | `design/ baselines/ experiments/ operations/ agent-prompts/` (`docs/README.md`) |
| `results/` | generated experiment output — **git-ignored; never commit data** |
| `Paper/Text/` | LaTeX manuscript (`Main.tex` includes `SecN_*.tex`) |

## Git conventions

- **Base your branch off the integration branch** the human designates (currently
  `chore/repo-reorg`; will become `main` once the reorg lands). Name it `session/<your-track-id>`
  (e.g. `session/02-tla-spec`).
- **Do not commit or push unless the human asks.** When you finish a unit of work, stage it and
  write the intended commit message + pathspec into your track's "Handoff" note, same as the reorg
  did in `REORG_COMMIT_PLAN.md`. (If told to commit: end messages with the repo's Co-Authored-By
  trailer; the `pre-commit` hook uses interactive prompts — use `--no-verify` for docs/paper/spec
  commits.)
- **Only the owner of a file edits it.** `src/embarlet/topic.{cc,h}` and the epoch collector belong
  to **Track 01 only**. If your track needs to read from it, add new files / interpose — never edit
  it. Paper `.tex` files are split by track (see each brief).
- Never `rm`/overwrite generated data or the `results/` tree.

## Remote testbed: `ssh broker`  (domin@moscxl, 512 cores, 1.7 TB RAM)

This is the shared build + CXL testbed. The local machine (macOS) **cannot build** — always build
and test on `broker`.

### Per-session isolated build tree (compiles need NO lock)

Each session works in its own remote dir so parallel compiles don't clobber each other:

```bash
# One-time / on each change: sync your working tree to YOUR dir (never use --delete carelessly)
rsync -az --exclude='.git/' --exclude='build/' --exclude='data/' --exclude='data_backup/' \
  --exclude='results/' --exclude='Paper/' --exclude='*.o' --exclude='*/build/' --exclude='.claude/' \
  ./ broker:~/Embarcadero-sessions/<your-track-id>/

# Configure + build (reuse the cached gRPC source to skip the heavy download/build).
# Use -j 64 (not 96): up to ~4 sessions may build at once; 4×64 < 512 cores, stay neighborly.
ssh broker 'cd ~/Embarcadero-sessions/<your-track-id> && \
  cmake -S . -B build -DFETCHCONTENT_SOURCE_DIR_GRPC=/home/domin/Embarcadero-main-ab/build/_deps/grpc-src && \
  cmake --build build -j 64'
```

Binaries land in `~/Embarcadero-sessions/<your-track-id>/build/bin/`.

> Known baseline: at integration-branch HEAD the whole tree builds (Track 01 fixed the
> `embarlet` `btree_map.push_back` bug). If `embarlet` fails to build, check whether Track 01's fix
> is in your base branch.

### Exclusive testbed runs (running brokers / benchmarks / CXL / cgroups / ports) — USE THE LOCK

Running the actual system (bind ports, map CXL/`/dev/shm` segments, cgroups, clock-synced clients)
is **not** safe to do concurrently. Serialize with a coarse-grained `flock`. The lock auto-releases
when your command exits (no stale locks):

```bash
ssh broker 'flock -w 2400 ~/Embarcadero-sessions/testbed.lock -c "
  cd ~/Embarcadero-sessions/<your-track-id> &&
  echo \"[$(date -u +%FT%TZ)] <your-track-id>: START <what>\" >> ~/Embarcadero-sessions/activity.log &&
  <your run/benchmark/ctest commands> ;
  echo \"[$(date -u +%FT%TZ)] <your-track-id>: END\" >> ~/Embarcadero-sessions/activity.log
"'
```

- `-w 2400` waits up to 40 min for the lock, then fails — if it times out, check
  `~/Embarcadero-sessions/activity.log` to see who's running, and retry; don't force.
- Hold the lock only for the run itself. **Compile outside the lock** (in your own tree), then take
  the lock just to execute.
- Keep exclusive runs short; batch what you need. Long soak tests: announce in `activity.log` and
  coordinate with the human.

### Etiquette

- Stay inside `~/Embarcadero-sessions/<your-track-id>/`. Never touch `~/Embarcadero` (the human's
  live tree with real data) or another session's dir.
- Clean your build tree when done if asked: `ssh broker 'rm -rf ~/Embarcadero-sessions/<your-track-id>/build'`.

## Dependency map (who can start when)

| Track | Starts | Depends on |
|---|---|---|
| 01 core-protocol (W1→W3, D1–D4) | **now** (critical path / trunk) | — (self-gating via W1) |
| 02 tla-spec (W2) | **now** | design D1–D4 (already specified) |
| 03 baseline-ports (W4) | **now** | — (separate baseline files) |
| 04 fault-injection (W6) | **now** | — (interposes; no protocol edits) |
| 05 rdma-variants (W5-A **and** W5-B) | after 01's session/lease **design** is frozen | 01 design (not code) |
| 06 related-work + positioning (Part I) | **now** | — (pure writing) |
| 07 presentation artifacts (D8, D9, G5, V.2) | **now** | reads 01 design as it firms up |

### W1's "seven on-paper closures" (v4/§W1 items 5–11) — DO THESE FIRST, ~2-week deadline, no code
These are the highest-leverage items and are **writing/spec**, not implementation. They are already
folded into the writing/spec tracks below — each of those tracks should treat its closure as its
**first deliverable**:
- (5) conjunction-thesis rewrite + RDMA-disagg-memory engagement → **Track 06**
- (6) two-variant W5 redesign spec → **Track 05** (spec first; code after)
- (7) two-claim novelty compression + Kafka-partition rebuttal → **Track 06**
- (8) SLO-**curve** substitution + per-baseline knee predictions → **predictions file** (Track 01 owner or human; see below)
- (9) ex ante porting rule → **Track 03**
- (10) mechanism self-audit table (D9) → **Track 07**
- (11) stale-CV ACK-relay scenario added to the TLA⁺ list → **Track 02**

**Do not modify `Paper/improvement_plan.md`** — it's the source of truth (v4). Propose changes to the
human instead.

## Not a gate anymore (v4): multi-host CXL
Earlier drafts made two-host CXL a week-8 go/no-go **gate**. v4 demotes it: the paper stands on
RDMA-in-silicon + single-host CXL + TLA⁺, so **no track should block on CXL hardware**. W7 (hardware
pursuit) is human outreach for *upside*; the week-8 decision now chooses *which paper exists* (full
CXL-thesis vs. narrower substrate-comparative, SOSP'27), not whether work proceeds.

## When you finish

Write a short **Handoff** at the end of your track file (or a sibling `sessions/handoff-0N.md`):
what you did, what you staged (pathspec + commit message), what you verified on `broker`, what you
found that other tracks need to know, and open questions for the human.
