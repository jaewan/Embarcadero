#include <gtest/gtest.h>

#include <string>

#include "session.pb.h"

TEST(SessionProtoRoundTripTest, OpenAckCarriesAssignedEpochAndCommittedHwm) {
	embarcadero::session::SessionOpen open;
	open.set_client_id(42);
	open.set_requested_session_epoch(7);
	open.set_topic("TestTopic");

	std::string wire;
	ASSERT_TRUE(open.SerializeToString(&wire));

	embarcadero::session::SessionOpen decoded_open;
	ASSERT_TRUE(decoded_open.ParseFromString(wire));
	EXPECT_EQ(decoded_open.client_id(), 42u);
	EXPECT_EQ(decoded_open.requested_session_epoch(), 7u);
	EXPECT_EQ(decoded_open.topic(), "TestTopic");

	embarcadero::session::SessionOpenAck ack;
	ack.set_committed_hwm(123);
	ack.set_status(embarcadero::session::SessionOpenAck::FENCED);
	ack.set_assigned_session_epoch(decoded_open.requested_session_epoch());

	ASSERT_TRUE(ack.SerializeToString(&wire));

	embarcadero::session::SessionOpenAck decoded_ack;
	ASSERT_TRUE(decoded_ack.ParseFromString(wire));
	EXPECT_EQ(decoded_ack.committed_hwm(), 123u);
	EXPECT_EQ(decoded_ack.status(), embarcadero::session::SessionOpenAck::FENCED);
	EXPECT_EQ(decoded_ack.assigned_session_epoch(), 7u);
}

TEST(SessionProtoRoundTripTest, FencedPayloadKeepsBatchSeqAuthoritative) {
	embarcadero::session::SessionFenced fenced;
	fenced.set_committed_batch_seq(17);
	fenced.set_committed_msg_hwm(4096);
	fenced.set_control_epoch(11);
	fenced.set_reason(embarcadero::session::SessionFenced::EPOCH_STALE);

	std::string wire;
	ASSERT_TRUE(fenced.SerializeToString(&wire));

	embarcadero::session::SessionFenced decoded;
	ASSERT_TRUE(decoded.ParseFromString(wire));
	EXPECT_EQ(decoded.committed_batch_seq(), 17u);
	EXPECT_EQ(decoded.committed_msg_hwm(), 4096u);
	EXPECT_EQ(decoded.control_epoch(), 11u);
	EXPECT_EQ(decoded.reason(), embarcadero::session::SessionFenced::EPOCH_STALE);
}
