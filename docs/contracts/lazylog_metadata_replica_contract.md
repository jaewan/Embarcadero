# LazyLog sequencing-metadata replica contract

This contract applies only when `EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS` is set.
The value is a comma-separated list of `host:port` endpoints and **must contain
exactly the topic replication factor** (RF includes the primary). Each endpoint
must run `lazylog_metadata_replica` with a distinct durable sidecar and failure
domain.

For an accepted batch, the broker publishes the payload and its PBR descriptor,
then sends one immutable descriptor to every metadata replica. A metadata RPC
success means that replica has appended the descriptor and completed
`fdatasync`. Retrying the same descriptor is idempotent; a different descriptor
for the same `(topic, source_broker_id, source_batch_seq)` fails.

The client fans out to all replicas in parallel and retries transport failures
three times with bounded linear backoff. A semantic rejection (including an
immutable-descriptor conflict) fails immediately. Exhausted retries do not
advance the append frontier; the producer must redrive the unacknowledged
batch. This is deliberately fail-closed rather than treating an unavailable
replica as acknowledged.

`ack1_append_replicated` is emitted only for the contiguous source-log prefix
for which both conditions hold:

1. every configured metadata replica has acknowledged the descriptor; and
2. the data-replica frontier covers that prefix.

The global LazyLog binder consumes this append frontier. Binding makes records
ordered and exportable, but is not an append-ACK prerequisite.

Recovery/read scope is intentionally narrow in this packet. A metadata replica
replays its durable sidecar at restart and continues to provide idempotence and
conflict detection. It does not yet expose a read API or reconstruct a broker's
in-memory append frontier. After a broker restart, clients must redrive
unacknowledged batches; the restored replicas then accept the same descriptor
idempotently. Consequently, this packet is valid for pre-binding append and
retry safety, but not yet a crash-recovery claim for a previously acknowledged
broker-local frontier. That claim remains blocked on M2's production data-media
frontier and a recovered source-frontier implementation.

Every run using this mode must record:

- `lazylog_metadata_replication=enabled`;
- `lazylog_metadata_replica_count=<RF>`;
- `ack1_contract=ack1_append_replicated`;
- the endpoint/failure-domain mapping outside the result CSV (do not place host
  addresses in publication artifacts).

Runs without the endpoint variable use the legacy ordered-visible ACK1 path and
must not be labeled as faithful LazyLog pre-binding append results.

## Deployment preflight

Start one `lazylog_metadata_replica` process per endpoint before invoking
`scripts/run_multiclient.sh`. The launcher verifies every endpoint accepts a
TCP connection (bounded by `LAZYLOG_METADATA_READY_TIMEOUT_SEC`, default 20
seconds) before it starts brokers. In remote-broker mode the endpoint list is
forwarded explicitly to each broker over SSH. A successful local preflight is
not a substitute for the required multi-host/CXL smoke test: that test remains
an evaluation gate because it must exercise the actual broker-to-replica paths.
