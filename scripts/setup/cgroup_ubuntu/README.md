# 1) Create 4 isolated “slots”, each with even CPU + memory, 10 Gbit/s net cap
sudo ./create_cgroup_ubuntu.sh setup 4

# If you want to evenly split disk bandwidth across slots:
# Provide the *total* device bandwidth and the script will divide it evenly:
#   - Units accepted: K, M, G (decimal) or raw bytes.
sudo IO_TOTAL_RBPS=2400M IO_TOTAL_WBPS=1200M ./create_cgroup_ubuntu.sh setup 4

# Or auto-probe totals (approximate; requires fio):
sudo apt-get install -y fio
sudo IO_AUTO_PROBE=1 ./create_cgroup_ubuntu.sh setup 4

# 3) Verify limits
sudo ./create_cgroup_ubuntu.sh verify

# 3) Run a command in slot 0 (namespace ns0 + cgroup g0)
sudo ./create_cgroup_ubuntu.sh run 0 -- bash -lc 'hostname; ip a s dev veth0ns; nproc; grep Cpus_allowed_list /proc/self/status'

# 4) Prove network isolation at 10 Gbit/s (needs iperf3)
sudo apt-get update && sudo apt-get install -y iperf3
sudo ./create_cgroup_ubuntu.sh test-net 0 1  # client in ns0 -> server in ns1 (JSON output shows Mbps/Gbps)
