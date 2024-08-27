#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "../src/cxl_manager/cxl_manager.h"
#include "../src/embarlet/heartbeat.h"

using namespace Embarcadero;
using ::testing::Return;
using ::testing::StrEq;

class MockCXLManager : public CXLManager {
public:
    MockCXLManager(size_t queueCapacity, int broker_id, CXL_Type cxl_type, std::string head_address, int num_io_threads)
        : CXLManager(queueCapacity, broker_id, cxl_type, head_address, num_io_threads) {}

    MOCK_METHOD(void*, GetTInode, (const char*), (override));
};

class MockScalogSequencerService : public ScalogSequencerService {
public:
    MockScalogSequencerService(CXLManager* cxl_manager, int broker_id, HeartBeatManager *broker, void* cxl_addr, std::string scalog_seq_address) 
        : ScalogSequencerService(cxl_manager, broker_id, broker, cxl_addr, scalog_seq_address) {}

    MOCK_METHOD(void, SendLocalCut, (int epoch, int offset, const char* topic), (override));
    MOCK_METHOD(void, ReceiveLocalCut, (int epoch, const char* topic, int offset), (override));
};

class MockHeartBeatManager : public HeartBeatManager {
public:
    MockHeartBeatManager(bool is_head_node, std::string head_address)
        : HeartBeatManager(is_head_node, head_address) {}
};

// Test fixture for ScalogSequencerService
class ScalogSequencerServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_cxl_manager_ = new MockCXLManager((1UL<<22), 0, CXL_Type::Emul, "127.0.0.1", NUM_CXL_IO_THREADS);
        mock_scalog_sequencer_service_ = new MockScalogSequencerService(mock_cxl_manager_, 0, nullptr, nullptr, "");
        mock_heartbeat_manager_ = new MockHeartBeatManager(true, "127.0.0.1:12140");
    }

    void TearDown() override {
        delete mock_cxl_manager_;
        delete mock_scalog_sequencer_service_;
        delete mock_heartbeat_manager_;
    }

    MockCXLManager* mock_cxl_manager_;
    MockScalogSequencerService* mock_scalog_sequencer_service_;
    MockHeartBeatManager* mock_heartbeat_manager_;
};

TEST_F(ScalogSequencerServiceTest, LocalSequencerDuration) {
    // Create and initialize TInode
    TInode TInode;
    strncpy(TInode.topic, "test", sizeof(TInode.topic) - 1);
    TInode.order = 1;
    TInode.seq_type = SequencerType::SCALOG;
    TInode.offsets[0].written = 0;

    // Set default behavior to return the initialized TInode
    ON_CALL(*mock_cxl_manager_, GetTInode(::testing::_))
        .WillByDefault(::testing::Return(&TInode));

    auto start = std::chrono::high_resolution_clock::now();
    mock_scalog_sequencer_service_->LocalSequencer("test");
    auto end = std::chrono::high_resolution_clock::now();
    
    // Calculate the duration
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Assert that the duration is equal to 5 milliseconds
    EXPECT_EQ(duration.count(), 5);
}

TEST_F(ScalogSequencerServiceTest, HandleSendLocalCutTest) {
    grpc::ServerContext context;
    SendLocalCutRequest request;
    SendLocalCutResponse response;

    request.set_topic("test_topic");
    request.set_epoch(0);
    request.set_local_cut(100);
    request.set_broker_id(0);

    // Expect the ReceiveLocalCut method to be called with the correct arguments
    EXPECT_CALL(*mock_scalog_sequencer_service_, ReceiveLocalCut(0, StrEq("test_topic"), 0))
        .Times(1);

    grpc::Status status = mock_scalog_sequencer_service_->HandleSendLocalCut(&context, &request, &response);

    ASSERT_TRUE(status.ok());
    // Verify that the response contains the expected global cut
    auto global_cut_map = response.global_cut();
    ASSERT_EQ(global_cut_map.size(), 1);
    ASSERT_EQ(global_cut_map[0], 100);
}