sudo cpupower frequency-set -g performance
sudo cpupower frequency-set -d 2.25GHz  # Set minimum
sudo cpupower frequency-set -u 3.7GHz   # Enable boost  
sudo cpupower idle-set -D 0              # Prevents deep sleep
