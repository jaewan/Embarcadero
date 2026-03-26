# Corfu global sequencer on host c3 (ORDER=2)

This runbook matches the Embarcadero Corfu path (`SEQUENCER=CORFU`, `ORDER=2`). Brokers and clients reach the sequencer via gRPC on the **sequencer port** (default **50052**).

## Network

- **Planned control/data plane IP on c3:** `10.10.10.181` (adjust if the host listens elsewhere).
- **Verify on c3** before starting clients:

```bash
ip -brief addr
ss -ltnp | grep 50052
```

Use the address that actually owns the listening socket in client/broker config or `EMBARCADERO_CORFU_SEQ_IP`.

## Config keys

| Mechanism | Key / variable |
|-----------|----------------|
| YAML | `embarcadero.corfu.sequencer_ip` in `config/embarcadero.yaml` |
| YAML | `embarcadero.corfu.sequencer_port` (default `50052`) |
| Env override | `EMBARCADERO_CORFU_SEQ_IP` |
| Env override | `EMBARCADERO_CORFU_SEQ_PORT` |

Client code builds the gRPC target as `sequencer_ip` + `CORFU_SEQ_PORT` (see `src/client/common.h` and `CORFU_SEQUENCER_ADDR`).

## Start the sequencer on c3

From the build tree (after `cmake --build`):

```bash
cd /path/to/Embarcadero/build/bin
export EMBARCADERO_CONFIG=/path/to/Embarcadero/config/embarcadero.yaml
# Optional: bind-specific port if not using yaml default
# export EMBARCADERO_CORFU_SEQ_PORT=50052
./corfu_global_sequencer
```

By default the process listens on **`0.0.0.0:50052`** (see `RunServer()` in `src/cxl_manager/corfu_global_sequencer.cc`). For host-only binding, change the listen address in that file or put a firewall/reverse proxy in front; production setups often keep `0.0.0.0` on the rack dataplane NIC.

## Point brokers and clients at c3

On **every** machine running `embarlet` or publishers:

```bash
export EMBARCADERO_CORFU_SEQ_IP=10.10.10.181
# Optional if non-default:
# export EMBARCADERO_CORFU_SEQ_PORT=50052
```

Or set `corfu.sequencer_ip: "10.10.10.181"` in the shared YAML all processes load.

## Ordering invariant (code ↔ paper)

- **Appendix** (`Paper/Text/Appendix.tex`, `\ref{app:corfu-cxl}`): per-client token order across brokers.
- **Implementation:** `src/cxl_manager/corfu_sequencer_service.cc` — per-`client_id` mutex before `next_order_.fetch_add`.
- **Test:** `build/bin/corfu_sequencer_fifo_smoke` (also registered as CTest `corfu_sequencer_fifo_smoke`).

## Remote helper scripts

`scripts/run_pub.sh` and related helpers accept `corfu_global_sequencer` as the sequencer binary name for `start_remote_sequencer` patterns; align the remote path and env with the variables above.
