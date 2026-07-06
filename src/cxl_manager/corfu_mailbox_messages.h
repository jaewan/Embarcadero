#pragma once

// Corfu CXL mailbox PODs — the fixed-layout records the Corfu token exchange rides on
// once the transport is swapped from gRPC to the CXL mailbox (docs/baselines/porting_rule.md).
// These are a pure re-encoding of TotalOrderRequest / TotalOrderResponse: same fields, same
// unary request/response, no protocol change. Plain POD with fixed layout and NO pointers,
// because the bytes cross the (non-coherent) CXL link and may be mapped at different virtual
// addresses in the broker and the co-located sequencer.

#include <cstdint>

namespace Embarcadero {
namespace cxl_manager {

// Broker -> sequencer, posted on up(broker_id). Mirrors TotalOrderRequest.
struct alignas(8) CorfuTokenRequest {
	uint64_t client_id;
	uint64_t batch_seq;
	uint64_t num_msg;
	uint64_t total_size;
	uint32_t broker_id;
	uint32_t pad;  // explicit padding: fixed 40-byte layout, no implicit tail padding
};
static_assert(sizeof(CorfuTokenRequest) == 40, "CorfuTokenRequest must be 40 bytes (fixed CXL layout)");

// Sequencer -> broker, posted on down(broker_id). Mirrors TotalOrderResponse, plus an echo
// of client_id + batch_seq for defensive request/response correlation, plus a status field
// carrying CorfuSequencerImpl::TokenStatus (0=OK, 1=INVALID_ARGUMENT, 2=ALREADY_PROCESSED,
// 3=OUT_OF_ORDER). total_order / log_idx / broker_batch_seq are meaningful only when
// status == 0 (OK).
struct alignas(8) CorfuTokenGrant {
	uint64_t total_order;
	uint64_t log_idx;
	uint64_t broker_batch_seq;
	uint64_t client_id;   // echo for correlation
	uint64_t batch_seq;   // echo for correlation
	uint32_t status;      // TokenStatus as uint32_t
	uint32_t pad;         // explicit padding: fixed 48-byte layout
};
static_assert(sizeof(CorfuTokenGrant) == 48, "CorfuTokenGrant must be 48 bytes (fixed CXL layout)");

}  // namespace cxl_manager
}  // namespace Embarcadero
