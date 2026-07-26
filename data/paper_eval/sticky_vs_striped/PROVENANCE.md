# Provenance

- Broker host/address: `moscxl`, `10.10.10.10`
- Publisher host/NUMA node: `c4`, node 1
- Brokers: 4
- Ordering/ACK/replication: ORDER=5, ACK1, RF0
- Record/load: 4 KiB, 20 GiB per trial
- Publisher threads: 6 per eligible broker
- CXL region/segment: 256 GiB / 24 GiB
- CXL initialization: metadata zeroing, lazy mapping
- Session lease: 180,000 ms (outside every measurement window)
- Minimum accepted overlap: 2,000 ms
- Striped harness commit: `e03173371aa9f057701a41af742a806eea50158d`
- Sticky harness commit: `1e1f84a430d51a984d505f6a3dbdac8ccdf5749a`
- Broker binary SHA-256:
  `002769abfce2d0de0c56fa27424f36d38ea459ca7a44bb30e05617f7c980484a`
- Local and remote publisher binary SHA-256:
  `4ff5c27c5463670648711395adc0fcb130cd9044ed792146d5bae968fb1de9ea`
- Broker configuration SHA-256:
  `430711ea5b0d41a530cd715d279f1589a3d9b1bf3b58cbd437f20745046d0752`
- Client configuration SHA-256:
  `459caeef82bd49308c9370283d5df0345342ad0e991b14298de3e12e4b0a343d`

The two harness commits differ only in experiment orchestration. The production
broker and publisher binaries are byte-identical across both conditions.
