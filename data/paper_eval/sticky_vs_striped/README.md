# Sticky-versus-striped ingress control

This control compares one remote publisher using Embarcadero ORDER=5 with
ACK1/RF0 against the same publisher constrained to broker 0. Both conditions
use four live brokers, 4 KiB records, 20 GiB per trial, six publisher threads
per eligible broker, a 24 GiB segment per broker, and the same client and
broker binaries.

`striped` uses the normal four-broker routing policy. `sticky_broker0` uses
`CLIENT_ORDER5_BROKER_ALLOWLISTS_PIPE=0`; every trial's routing counter reports
all 5,242,880 messages at broker 0. No trial reports a session fence,
reconnect, or header-send failure.

The paper metric is acknowledged-byte throughput over the common active
timeseries window. The median is 7.771 GiB/s for striped routing and
6.307 GiB/s for broker-0 routing, a 1.23x difference. This is a premise check,
not a durability result: replication is disabled. It shows that a single
broker is already competitive for one 100 GbE publisher, while striping
provides modest ACK/drain parallelism and removes the single-placement
constraint. It must not be described as a universal 1.23x benefit.

Raw, lossless evidence remains under:

- `data/publication/throughput/paper_sticky_vs_striped_20260726_v8/` (striped)
- `data/publication/throughput/paper_sticky_vs_striped_20260726_v9/` (broker 0)
