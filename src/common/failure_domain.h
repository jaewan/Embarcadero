#pragma once

#include <string>

namespace Embarcadero {

/**
 * Explicit transport / failure-domain abstraction for production hardening.
 * Current testbed claims are limited to process failure on a single host
 * (`local_cxl`). Host tolerance requires `rdma_replica` (or equivalent)
 * placement on distinct power/failure domains.
 */
enum class FailureDomainKind {
	LocalCxl = 0,
	RdmaReplica = 1,
};

inline const char* FailureDomainKindName(FailureDomainKind kind) {
	switch (kind) {
		case FailureDomainKind::LocalCxl: return "local_cxl";
		case FailureDomainKind::RdmaReplica: return "rdma_replica";
	}
	return "unknown";
}

struct FailureDomainConfig {
	FailureDomainKind kind{FailureDomainKind::LocalCxl};
	std::string domain_id{"moscxl"};
	bool host_failure_tolerant{false};
};

inline FailureDomainConfig DefaultSingleHostDomain() {
	FailureDomainConfig cfg;
	cfg.kind = FailureDomainKind::LocalCxl;
	cfg.domain_id = "moscxl";
	cfg.host_failure_tolerant = false;
	return cfg;
}

/** True only when replicas are placed on distinct host/power domains. */
inline bool ClaimHostFailureTolerance(const FailureDomainConfig& cfg) {
	return cfg.host_failure_tolerant && cfg.kind == FailureDomainKind::RdmaReplica;
}

}  // namespace Embarcadero
