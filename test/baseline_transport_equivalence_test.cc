#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "cxl_manager/lazylog_binding_core.h"
#include "cxl_manager/scalog_global_ordering_core.h"

namespace {
using Embarcadero::cxl_manager::LazyLogBindingCore;
using Embarcadero::cxl_manager::ScalogGlobalOrderingCore;

TEST(ScalogTransportEquivalence, CanonicalCutsMatchAcrossGrpcAndMailboxCadence) {
  for (int rf : {1, 2, 3}) {
    ScalogGlobalOrderingCore grpc(rf), mailbox(rf);
    for (int broker = 0; broker < 2; ++broker) {
      grpc.RegisterBroker(broker, rf);
      mailbox.RegisterBroker(broker, rf);
    }
    // Two complete report rounds, with a duplicate and a regressing report in between.
    for (int epoch = 0; epoch < 2; ++epoch) {
      for (int broker = 0; broker < 2; ++broker) {
        for (int replica = 0; replica < rf; ++replica) {
          const int64_t cut = (epoch + 1) * 10 + broker * 3 + replica;
          grpc.AddLocalCut(broker, replica, epoch, cut);
          mailbox.AddLocalCut(broker, replica, epoch, cut);
        }
      }
      if (epoch == 0) {
        grpc.AddLocalCut(0, 0, epoch, 10);       // duplicate
        mailbox.AddLocalCut(0, 0, epoch, 10);
        grpc.AddLocalCut(0, 0, epoch, 9);        // regression
        mailbox.AddLocalCut(0, 0, epoch, 9);
      }
      absl::btree_map<int, int64_t> grpc_cut, mailbox_cut;
      int64_t grpc_epoch = -1, mailbox_epoch = -1;
      ASSERT_TRUE(grpc.ComputeGlobalCut(2, grpc_cut, &grpc_epoch, false));
      ASSERT_TRUE(mailbox.ComputeGlobalCut(2, mailbox_cut, &mailbox_epoch, true));
      EXPECT_EQ(grpc_cut, mailbox_cut) << "RF=" << rf << " epoch=" << epoch;
      EXPECT_EQ(grpc_cut, (absl::btree_map<int, int64_t>{{0, (epoch + 1) * 10},
                                                          {1, (epoch + 1) * 10 + 3}}));
      EXPECT_EQ(mailbox_epoch, epoch);
    }
  }
}

TEST(LazyLogTransportEquivalence, CanonicalBindingsMatchAcrossGrpcAndMailboxCadence) {
  LazyLogBindingCore grpc, mailbox;
  for (int broker = 0; broker < 2; ++broker) {
    grpc.RegisterBroker(broker);
    mailbox.RegisterBroker(broker);
  }
  for (int epoch = 0; epoch < 2; ++epoch) {
    for (int broker = 0; broker < 2; ++broker) {
      const int64_t progress = (epoch + 1) * 100 + broker * 7;
      grpc.AddLocalProgress(broker, epoch, progress);
      mailbox.AddLocalProgress(broker, epoch, progress);
    }
    if (epoch == 0) {
      grpc.AddLocalProgress(0, epoch, 100);       // duplicate
      mailbox.AddLocalProgress(0, epoch, 100);
      grpc.AddLocalProgress(0, epoch, 99);        // regression
      mailbox.AddLocalProgress(0, epoch, 99);
    }
    absl::btree_map<int, int64_t> grpc_binding, mailbox_binding;
    int64_t grpc_epoch = -1, mailbox_epoch = -1;
    ASSERT_TRUE(grpc.ComputeGlobalBinding(2, grpc_binding, &grpc_epoch, false));
    ASSERT_TRUE(mailbox.ComputeGlobalBinding(2, mailbox_binding, &mailbox_epoch, true));
    EXPECT_EQ(grpc_binding, mailbox_binding) << "epoch=" << epoch;
    EXPECT_EQ(grpc_binding, (epoch == 0
        ? absl::btree_map<int, int64_t>{{0, 100}, {1, 107}}
        : absl::btree_map<int, int64_t>{{0, 100}, {1, 100}}));
    EXPECT_EQ(mailbox_epoch, epoch);
  }
}
}  // namespace
