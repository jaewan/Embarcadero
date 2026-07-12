#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <glog/logging.h>

namespace Embarcadero {

// Resolve writable replication directories for striping replica logs across disks.
// Precedence:
//   1) EMBARCADERO_REPLICA_DISK_DIRS=dir0,dir1,...
//   2) EMBARCADERO_REPLICA_DISK_ROOT/<disk*> children
//   3) Auto-discover .Replication/disk* relative to CWD
// Non-writable entries are skipped (common when a bind-mount stub is root-owned).
inline std::vector<std::string> ResolveWritableReplicationDirs() {
	std::vector<std::string> dirs;

	auto split_csv = [](const char* csv) {
		std::vector<std::string> out;
		if (csv == nullptr) return out;
		std::stringstream ss(csv);
		std::string item;
		while (std::getline(ss, item, ',')) {
			if (!item.empty()) out.push_back(item);
		}
		return out;
	};

	auto collect_disk_children = [&](const std::string& root) {
		std::error_code ec;
		if (!std::filesystem::exists(root, ec) || ec) return;
		for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
			if (ec) break;
			if (!e.is_directory()) continue;
			const std::string n = e.path().filename().string();
			if (n.rfind("disk", 0) == 0) {
				dirs.push_back(e.path().string());
			}
		}
	};

	if (const char* dirs_env = std::getenv("EMBARCADERO_REPLICA_DISK_DIRS")) {
		dirs = split_csv(dirs_env);
	}
	if (dirs.empty()) {
		if (const char* root_env = std::getenv("EMBARCADERO_REPLICA_DISK_ROOT")) {
			collect_disk_children(root_env);
		}
	}
	if (dirs.empty()) {
		const std::array<std::string, 3> defaults = {
			"../../.Replication",
			"../.Replication",
			".Replication"
		};
		for (const auto& root : defaults) {
			const size_t before = dirs.size();
			collect_disk_children(root);
			if (dirs.size() > before) break;
		}
	}

	std::sort(dirs.begin(), dirs.end());
	dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());

	std::vector<std::string> writable_dirs;
	writable_dirs.reserve(dirs.size());
	for (const auto& d : dirs) {
		if (d.empty()) continue;
		if (::access(d.c_str(), W_OK | X_OK) == 0) {
			writable_dirs.push_back(d);
		} else {
			LOG(WARNING) << "Skipping non-writable replication dir: " << d;
		}
	}
	return writable_dirs;
}

// Pick a replication directory for broker_id, striping across writable dirs.
// Returns empty string if none are available.
inline std::string SelectReplicationDirForBroker(
		int broker_id,
		const std::vector<std::string>& dirs) {
	if (dirs.empty()) return {};
	const size_t idx = static_cast<size_t>(broker_id < 0 ? 0 : broker_id) % dirs.size();
	return dirs[idx];
}

}  // namespace Embarcadero
