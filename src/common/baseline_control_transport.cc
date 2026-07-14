#include "common/baseline_control_transport.h"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace Embarcadero {

BaselineControlTransport ParseBaselineControlTransportValue(std::string_view value) {
  if (value.empty() || value == "grpc") return BaselineControlTransport::kGrpc;
  if (value == "cxl_mailbox") return BaselineControlTransport::kCxlMailbox;
  throw std::invalid_argument(
      "EMBARCADERO_BASELINE_CONTROL_TRANSPORT must be exactly 'grpc' or "
      "'cxl_mailbox' (got '" + std::string(value) + "')");
}

BaselineControlTransport ParseBaselineControlTransport() {
  const char* value = std::getenv("EMBARCADERO_BASELINE_CONTROL_TRANSPORT");
  return ParseBaselineControlTransportValue(value == nullptr ? std::string_view{} : value);
}

const char* BaselineControlTransportName(BaselineControlTransport transport) {
  switch (transport) {
    case BaselineControlTransport::kGrpc:
      return "grpc";
    case BaselineControlTransport::kCxlMailbox:
      return "cxl_mailbox";
  }
  throw std::invalid_argument("unknown BaselineControlTransport enum value");
}

}  // namespace Embarcadero
