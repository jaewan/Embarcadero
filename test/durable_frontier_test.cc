#include <gtest/gtest.h>

#include "../src/common/durable_frontier.h"

namespace {

TEST(DurableFrontierTest, ReleasesOnlyContiguousCompletions) {
  Embarcadero::DurableFrontier frontier;
  std::vector<Embarcadero::DurableBatch> released;

  EXPECT_TRUE(frontier.Record(1, {2, 22}, &released));
  EXPECT_TRUE(released.empty());
  EXPECT_EQ(frontier.pending_size(), 1u);

  EXPECT_TRUE(frontier.Record(0, {3, 11}, &released));
  ASSERT_EQ(released.size(), 2u);
  EXPECT_EQ(released[0].message_count, 3u);
  EXPECT_EQ(released[0].client_id, 11u);
  EXPECT_EQ(released[1].message_count, 2u);
  EXPECT_EQ(released[1].client_id, 22u);
  EXPECT_EQ(frontier.next_sequence(), 2u);
  EXPECT_EQ(frontier.pending_size(), 0u);
}

TEST(DurableFrontierTest, RejectsDuplicatesAndEmptyBatches) {
  Embarcadero::DurableFrontier frontier;
  std::vector<Embarcadero::DurableBatch> released;
  EXPECT_FALSE(frontier.Record(0, {0, 1}, &released));
  EXPECT_TRUE(frontier.Record(0, {1, 1}, &released));
  EXPECT_FALSE(frontier.Record(0, {1, 1}, &released));
  EXPECT_EQ(frontier.next_sequence(), 1u);
}

}  // namespace
