# ! /bin/bash
# Run this script the first time you create this project

# Abseil
git clone https://github.com/abseil/abseil-cpp.git
cd abseil-cpp
mkdir build && cd build
git clone https://github.com/abseil/abseil-cpp.git
cmake --build . --target all

