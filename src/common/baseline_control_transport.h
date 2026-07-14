#pragma once

#include <string_view>

namespace Embarcadero {

// Control-plane transport for the baseline ablation.  This deliberately does
// not select a payload path: changing that here would invalidate the ablation.
enum class BaselineControlTransport { kGrpc, kCxlMailbox };

// Parses the externally supplied spelling.  Kept separate from getenv so unit
// tests and launchers can validate values without mutating process state.
BaselineControlTransport ParseBaselineControlTransportValue(std::string_view value);

// Reads EMBARCADERO_BASELINE_CONTROL_TRANSPORT.  An unset value is grpc for
// backwards compatibility; every other spelling is rejected rather than
// silently selecting a different experiment.
BaselineControlTransport ParseBaselineControlTransport();

const char* BaselineControlTransportName(BaselineControlTransport transport);

}  // namespace Embarcadero
