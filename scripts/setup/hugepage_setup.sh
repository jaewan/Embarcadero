echo 10420 | sudo tee /proc/sys/vm/nr_hugepages # 20GB of huge pages
sudo sysctl -w net.core.wmem_max=16777216  # 16 MB
sudo sysctl -w net.core.rmem_max=16777216  # 16 MB
sudo sysctl -w net.ipv4.tcp_wmem="4096 65536 16777216"
