#include <gtest/gtest.h>

#include "cxl_manager/baseline_cxl_layout.h"

namespace {
using namespace Embarcadero::cxl_manager;

TEST(BaselineCxlLayout, MailboxStartsAfterEntireGOI) {
  EXPECT_EQ(GOIEndOffset(), kGOIOffset + kMaxGOIEntries * sizeof(Embarcadero::GOIEntry));
  EXPECT_GE(BaselineMailboxOffset(), GOIEndOffset());
  EXPECT_EQ(BaselineMailboxOffset() % Embarcadero::cxl_transport::kCacheLine, 0U);
}

TEST(BaselineCxlLayout, FixedMailboxReservationDoesNotDependOnActiveBrokers) {
  constexpr uint32_t kConfiguredMaxBrokers = 32;
  const size_t mailbox_end = BaselineMailboxOffset() + BaselineMailboxBytes(kConfiguredMaxBrokers);
  EXPECT_EQ(BaseRegionsOffset(kConfiguredMaxBrokers), mailbox_end);
  EXPECT_GT(BaselineMailboxBytes(kConfiguredMaxBrokers), 0U);
  EXPECT_EQ(BaselineMailboxBytes(kConfiguredMaxBrokers) % kPageSize, 0U);
}
}  // namespace
