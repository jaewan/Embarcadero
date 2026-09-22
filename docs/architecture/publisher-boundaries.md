# Publisher implementation boundaries

`Publisher` remains the owner of its queues, sockets, retained batches, session
frontiers, and worker lifetimes. The implementation is split into linked
compilation units; this does not introduce a second state machine or change the
wire protocol.

| File | Responsibility |
|---|---|
| `src/client/publisher.cc` | Construction, discovery, ordinary send workers, queue wakeups, `Poll`, final worker teardown, and throughput instrumentation |
| `src/client/publisher_session.cc` | Session OPEN, retransmission channels, fencing, retained-suffix recovery, and retransmission worker |
| `src/client/publisher_retention.cc` | Retained ownership, capacity bounds, ACK2 copy policy, and monotonic retirement |
| `src/client/publisher_ack.cc` | ACK socket processing, frontier normalization, and optional publish-latency accounting |
| `src/client/publisher_internal.h` | Private shared helpers and network-profile state |

Shared helper functions have external inline linkage inside a private detail
namespace. Their function-local cached settings and profile object therefore
have one instance across compilation units. Session-only socket helpers remain
local to the session implementation. State stays in the existing class layout;
the extraction adds no per-batch allocation, lock, payload copy, or virtual call.

Producer completion is not session completion. ORDER5 ACK-enabled send workers
can park after input finishes and consume a suffix requeued by fencing recovery.
`Poll` keeps those workers available through its authoritative ACK wait. Terminal
worker stop and recovery admission serialize through the existing fence gate;
the gate is released before joining workers. The worker-vector ownership lock
prevents discovery from appending after ownership transfers to teardown.

Retention and ACK transport remain separate operations. The normalized global
ACK frontier is authoritative; the separately sampled raw ACK counter and
retention snapshot can lag it. Moving these methods does not strengthen their
snapshot semantics or change ACK2's sink-dependent durability meaning.

Both independent reviews compared the extracted method bodies with the
pre-extraction source and checked shared helper linkage. Body equivalence is
not performance equivalence: separate compilation can change inlining and code
placement. Linked builds, lifecycle regressions, production fault scenarios,
and a fresh matched performance comparison remain the validation gates for
this revision. Earlier frozen performance results describe their original
binaries only.
