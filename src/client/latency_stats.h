#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

namespace Embarcadero {

class LatencyStats {
public:
	enum PercentileMethod {
		kNearestRank = 0,
		kLinearInterpolation = 1
	};
	static constexpr PercentileMethod kPercentileMethod = kNearestRank;

	struct Summary {
		double p50_us = 0.0;
		double p95_us = 0.0;
		double p99_us = 0.0;
		double p999_us = 0.0;
		double max_us = 0.0;
		double p90_us = 0.0;
		double average_us = 0.0;
		double min_us = 0.0;
		size_t count = 0;
	};

	static Summary ComputeSummary(const std::vector<long long>& latencies_us) {
		Summary s{};
		if (latencies_us.empty()) {
			return s;
		}

		// ComputeSummary expects sorted input from callers.
		s.count = latencies_us.size();
		s.min_us = static_cast<double>(latencies_us.front());
		s.max_us = static_cast<double>(latencies_us.back());
		const long double sum = std::accumulate(
			latencies_us.begin(), latencies_us.end(), static_cast<long double>(0.0L));
		s.average_us = static_cast<double>(sum / static_cast<long double>(s.count));

		auto percentile = [&](double p) -> double {
			if (s.count == 0) return 0.0;
			const double rank = std::ceil(p * static_cast<double>(s.count));
			size_t idx = (rank <= 1.0) ? 0 : static_cast<size_t>(rank - 1.0);
			if (idx >= s.count) idx = s.count - 1;
			return static_cast<double>(latencies_us[idx]);
		};

		s.p50_us = percentile(0.50);
		s.p90_us = percentile(0.90);
		s.p95_us = percentile(0.95);
		s.p99_us = percentile(0.99);
		s.p999_us = percentile(0.999);
		return s;
	}
};

} // namespace Embarcadero
