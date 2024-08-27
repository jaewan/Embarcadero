#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "../src/cxl_manager/cxl_manager.h"

using namespace Embarcadero;

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
};

// Test fixture for ScalogSequencerService
class ScalogSequencerServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_cxl_manager_ = new MockCXLManager((1UL<<22), 0, CXL_Type::Emul, "127.0.0.1", NUM_CXL_IO_THREADS);
        service_ = new MockScalogSequencerService(mock_cxl_manager_, 0, nullptr, nullptr, "");
    }

    void TearDown() override {
        delete service_;
        delete mock_cxl_manager_;
    }

    MockCXLManager* mock_cxl_manager_;
    MockScalogSequencerService* service_;
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
    service_->LocalSequencer("test");
    auto end = std::chrono::high_resolution_clock::now();
    
    // Calculate the duration
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Assert that the duration is equal to 5 milliseconds
    EXPECT_EQ(duration.count(), 5);
}
