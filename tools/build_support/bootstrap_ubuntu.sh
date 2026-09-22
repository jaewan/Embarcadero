#!/usr/bin/env bash
# Run only in a disposable Ubuntu 24.04 container/rootfs; never tunes the host.
set -euo pipefail
if [[ ${EMBARCADERO_DISPOSABLE_ROOTFS:-} != 1 || $(id -u) != 0 ]]; then
  echo 'Requires root inside an explicitly disposable rootfs (EMBARCADERO_DISPOSABLE_ROOTFS=1).' >&2
  exit 2
fi
source /etc/os-release
[[ $ID == ubuntu && $VERSION_ID == 24.04 ]] || { echo 'Ubuntu 24.04 required' >&2; exit 2; }
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential cmake ninja-build git ca-certificates curl \
  python3 pkg-config libnuma-dev numactl libevent-dev libboost-all-dev libdouble-conversion-dev \
  libgflags-dev libgoogle-glog-dev libssl-dev libsystemd-dev libyaml-cpp-dev libunwind-dev \
  liblz4-dev libzstd-dev libsodium-dev libsnappy-dev zlib1g-dev libbz2-dev liblzma-dev \
  libjemalloc-dev libfmt-dev libmimalloc-dev libcxxopts-dev libgtest-dev libgmock-dev libbenchmark-dev
# Folly is the sole non-packaged C++ dependency. Fetch the exact audited revision.
readonly folly_commit=213881d77db9d36335ebca18d2d30fcf4c51b2d5
readonly bootstrap_root=${EMBARCADERO_BOOTSTRAP_ROOT:-/opt/embarcadero-deps}
mkdir -p "$bootstrap_root"
if [[ ! -d $bootstrap_root/folly/.git ]]; then
  git clone --no-checkout https://github.com/facebook/folly.git "$bootstrap_root/folly"
fi
git -C "$bootstrap_root/folly" checkout --detach "$folly_commit"
[[ $(git -C "$bootstrap_root/folly" rev-parse HEAD) == "$folly_commit" ]]
cmake -S "$bootstrap_root/folly" -B "$bootstrap_root/folly-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DBUILD_BENCHMARKS=OFF
cmake --build "$bootstrap_root/folly-build" -j "${EMBARCADERO_BUILD_JOBS:-8}"
cmake --install "$bootstrap_root/folly-build"
ldconfig
dpkg-query -W -f='${Package}\t${Version}\n' > "$bootstrap_root/ubuntu-packages.tsv"
printf '%s\n' "$folly_commit" > "$bootstrap_root/folly-revision.txt"
