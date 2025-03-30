# Embarcedero a totally ordered pub/sub system with CXL

## Usage
The first node you start Embarcadero is the head node (works as a rendezvous point to exchange info of brokers)

Start by
```bash
./embarlet --head
```
for other nodes start by
```bash
./embarlet --follower'ADDR:PORT'
or
./embarlet --follower
```

### System Dependencies

**Debian/Ubuntu**:
```bash
sudo apt-get update
```


## Version
- Version 0: Support a single topic, no CXL memory layout, running on Emulated Environment
	* 0.1: A single topic support

- Version 1: Support multi-topic, fault tolerance (dynamic broker addition/removal, replication)

## Building
To enable cgroup, add permission to the executables.
```bash
./scripts/setup/setup_dependencies.sh # Must run this from project's root directory
mkdir build
cmake ../
cmake --build .
cd bin
sudo setcap cap_sys_admin,cap_dac_override,cap_dac_read_search=eip ./embarlet 
```
The generated executable will be in ```build/src/embarlet```.

## TestBed
Refer to [Test README](Embarcadero/tests/README.md)
