# Embarcedero a totally ordered pub/sub system with CXL

## Version
- Version 0: Support a single topic, no CXL memory layout, running on Emulated Environment
	* 0.1: A single topic support

- Version 1: Support multi-topic, fault tolerance (dynamic broker addition/removal, replication)

## Building

```bash
mkdir build
cmake ../
cmake --build .
```
The generated executable will be in ```build/bin/embarlet```.

## TestBed
Refer to [Test README](Embarcadero/tests/README.md)
