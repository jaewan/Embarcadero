# RDMA fabric setup — RoCEv2 on the Embarcadero testbed

**Status as of 2026-07-05:** broker + c1 + c3 configured and **verified** (cross-host RDMA WRITE).
c2 + c4 still to do (need sudo). All config below is **runtime / non-persistent** — see "Make it
durable" to survive reboots.

## Fabric plan

RoCEv2, subnet `10.10.10.0/24`, MTU 9000, all on the ACTIVE `mlx5_0` port of each host.

| Host | mgmt name | RDMA netdev | RDMA IP | RoCEv2 GID idx | sudo | perftest | status |
|------|-----------|-------------|---------|----------------|------|----------|--------|
| broker | moscxl | `enp193s0f0np0` | `10.10.10.10/24` | **3** | ✓ | ✓ (installed) | **done (pre-existing IP)** |
| c1 | mos143 | `enp24s0f0np0` | `10.10.10.11/24` | **5** | ✓ | ✓ | **done** |
| c3 | mos181 | `ens801f0np0` | `10.10.10.13/24` | **5** | ✓ | ✓ (installed) | **done — memserver (W5-B)** |
| c2 | mos144 | `enp24s0f0np0` | `10.10.10.12/24` | (bounce to populate) | ✗ | ✓ | **TODO (needs sudo)** |
| c4 | mos182 | `ens801f0np0` | `10.10.10.14/24` | (bounce to populate) | ✗ | ✓ | **TODO (needs sudo)** |

> GID index note: the IPv4-mapped RoCEv2 GID (`::ffff:0a0a:0a0X`) only appears **after** the IP is
> assigned; on these hosts a `link down/up` was needed to populate it. Always re-scan (below) after
> configuring — do not assume idx 3/5.

## Verified results (2026-07-05)

- broker ↔ c1: `ib_write_bw` RoCEv2, 65 KB msgs → **54.0 Gb/s** avg (1 QP).
- broker ↔ c3: `ib_write_bw` RoCEv2, 65 KB msgs → **88.0 Gb/s** avg (1 QP).
- Jumbo ping (`ping -M do -s 8972`) clean on both. Active MTU negotiated to 4096 B (RoCE MTU).

## Recipe (per remaining host — run c2/c4 like this; needs sudo on that host)

```bash
# on the client host (replace NETDEV + IP per the table above)
NETDEV=enp24s0f0np0        # c2; use ens801f0np0 for c4
IP=10.10.10.12             # c2; 10.10.10.14 for c4
sudo apt-get install -y perftest        # only if missing
sudo ip link set $NETDEV down; sleep 1; sudo ip link set $NETDEV up; sleep 2   # populates RoCEv2 GID
sudo ip addr add $IP/24 dev $NETDEV
sudo ip link set $NETDEV mtu 9000
# find the RoCEv2 IPv4 GID index (look for ffff:0a0a:0a0X with type "RoCE v2"):
LAST=$(echo $IP | awk -F. '{printf "%02x", $4}')   # e.g. .12 -> 0c
for i in $(seq 0 15); do
  g=$(cat /sys/class/infiniband/mlx5_0/ports/1/gids/$i 2>/dev/null)
  t=$(cat /sys/class/infiniband/mlx5_0/ports/1/gid_attrs/types/$i 2>/dev/null)
  echo "$g" | grep -q "0a0a:0a$LAST" && echo "GID idx=$i type=$t"
done
```

## Sanity test (from broker; replace CLIENT_IP / GID indices)

```bash
# broker = server (its RoCEv2 GID idx is 3)
ib_write_bw -d mlx5_0 -x 3 -F -D 4 --report_gbits &
# client (use the client's discovered GID idx; connect to broker's IP)
ssh cN 'ib_write_bw -d mlx5_0 -x <client_gid_idx> -F -D 4 --report_gbits 10.10.10.10'
```
Expect tens of Gb/s (1 QP). Use `-q <n>` for multiple QPs to approach line rate — relevant for the
W5-B memory-server funnel study (Track 05).

> Gotcha that cost time: do **not** clean up with `pkill -f ib_write_bw` from a script — `-f` matches
> the script's own shell (it contains the string) and kills your session. Use `pkill -x ib_write_bw`
> or `kill <pid>`.

## Make it durable (before relying on it across reboots)

Runtime `ip addr add` / link bounce is lost on reboot. Persist via netplan (Ubuntu) on each host,
e.g. `/etc/netplan/99-roce.yaml`:

```yaml
network:
  version: 2
  ethernets:
    enp24s0f0np0:            # per-host RDMA netdev
      mtu: 9000
      addresses: [10.10.10.11/24]
```
`sudo netplan apply`. (Or systemd-networkd equivalent.) The RoCEv2 GID repopulates automatically on
a persistent, up-at-boot interface.

## What this unblocks

- **Track 05 (RDMA variants)** cross-host runs on the broker+c1+c3 triangle: W5-A (broker+c1 as
  broker hosts, kill a host), W5-B (c3 as the dedicated memory server, measure the NIC funnel).
- The all-remote eval phase (E1–E10) once c2/c4 are added for full publisher/broker scale.
