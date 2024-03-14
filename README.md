# Embarcedero a totally ordered pub/sub system with CXL

## Usage
The first node you start Embarcadero is the head node (works as a rendezvous point to exchange info of brokers)

Start by
```bash
./embarlet --head
```
for other nodes start by
```bash
./embarlet --follower
```


## Version
- Version 0: Support a single topic, no CXL memory layout, running on Emulated Environment
	* 0.1: A single topic support

- Version 1: Support multi-topic, fault tolerance (dynamic broker addition/removal, replication)


## TestBed
Refer to [Test README](Embarcadero/tests/README.md)
