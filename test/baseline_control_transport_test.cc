#include <gtest/gtest.h>

#include <stdexcept>

#include "common/baseline_control_transport.h"

namespace {
using Embarcadero::BaselineControlTransport;

TEST(BaselineControlTransport, DefaultAndValidValues) {
  EXPECT_EQ(Embarcadero::ParseBaselineControlTransportValue(""),
            BaselineControlTransport::kGrpc);
  EXPECT_EQ(Embarcadero::ParseBaselineControlTransportValue("grpc"),
            BaselineControlTransport::kGrpc);
  EXPECT_EQ(Embarcadero::ParseBaselineControlTransportValue("cxl_mailbox"),
            BaselineControlTransport::kCxlMailbox);
}

TEST(BaselineControlTransport, RejectsInvalidValue) {
  EXPECT_THROW(Embarcadero::ParseBaselineControlTransportValue("mailbox"), std::invalid_argument);
  EXPECT_THROW(Embarcadero::ParseBaselineControlTransportValue("GRPC"), std::invalid_argument);
}

TEST(BaselineControlTransport, NamesRoundTrip) {
  for (const auto transport : {BaselineControlTransport::kGrpc,
                               BaselineControlTransport::kCxlMailbox}) {
    EXPECT_EQ(Embarcadero::ParseBaselineControlTransportValue(
                  Embarcadero::BaselineControlTransportName(transport)),
              transport);
  }
}
}  // namespace
