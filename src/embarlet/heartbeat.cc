#include <chrono>

#include "heartbeat.h"

namespace Embarcadero{

void HeartBeatServiceImpl::CheckHeartbeats(){
	static const int timeout = HEARTBEAT_INTERVAL * 3;
	while (!shutdown_) {
		std::this_thread::sleep_for(std::chrono::seconds(timeout));
		absl::MutexLock lock(&mutex_);
		auto now = std::chrono::steady_clock::now();
		for (auto it = nodes_.begin(); it != nodes_.end();) {
			// Do not check head node
			if(it->second.broker_id == 0){
				++it;
				continue;
			}
			if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_heartbeat).count() > timeout) {
				LOG(INFO) << "Node " << it->first << " is dead";
				auto key_to_erase = it->first;
				++it;
				nodes_.erase(key_to_erase);
			} else {
				++it;
			}
		}
	}
}
//************************* Client Part *************************

FollowerNodeClient::FollowerNodeClient(const std::string& node_id, const std::string& address,
		const std::shared_ptr<grpc::Channel>& heartbeat_channel)
	: node_id_(node_id), address_(address), head_alive_(true), wait_called_(false) {
		stub_ = HeartBeat::NewStub(heartbeat_channel);
		Register();
		heartbeat_thread_ = std::thread([this]() {
				this->HeartBeatLoop();
				});
	}

void FollowerNodeClient::Register(){
	NodeInfo node_info;
	node_info.set_node_id(node_id_);
	node_info.set_address(address_);

	RegistrationStatus reply;
	grpc::ClientContext context;

	Status status = stub_->RegisterNode(&context, node_info, &reply);
	if (status.ok() && reply.success()) {
		LOG(INFO) << "Node registered: " << reply.message();
		broker_id_ = reply.broker_id();
	} else {
		LOG(ERROR) << "Failed to register node: " << reply.message();
	}
}

void FollowerNodeClient::SendHeartbeat() {
    HeartbeatRequest request;
    request.set_node_id(node_id_);

    auto call = std::make_unique<AsyncClientCall>();
    call->response_reader = stub_->AsyncHeartbeat(&call->context, request, &cq_);
    call->response_reader->Finish(&call->reply, &call->status, call.get());

    // Set a deadline for the heartbeat
    gpr_timespec deadline = gpr_time_add(
        gpr_now(GPR_CLOCK_REALTIME),
        gpr_time_from_seconds(HEARTBEAT_INTERVAL, GPR_TIMESPAN));
    call->context.set_deadline(deadline);

    // Transfer ownership to the completion queue
    call.release();
}

void FollowerNodeClient::CheckHeartBeatReply() {
    void* got_tag;
    bool ok;

    // Use a timeout when checking for replies
    gpr_timespec deadline = gpr_time_add(
        gpr_now(GPR_CLOCK_REALTIME),
        gpr_time_from_seconds(1, GPR_TIMESPAN)); // 1 second timeout

    while (true) {
        grpc::CompletionQueue::NextStatus status = cq_.AsyncNext(&got_tag, &ok, deadline);

        if (status == grpc::CompletionQueue::SHUTDOWN) {
            break;
        }

        if (status == grpc::CompletionQueue::TIMEOUT) {
            break;;
        }

        auto* call = static_cast<AsyncClientCall*>(got_tag);
        if (shutdown_) {
            delete call;
            continue;
        }
        if (ok && call->status.ok() && call->reply.alive()) {
            head_alive_ = true;
        } else {
            head_alive_ = false;
            LOG(INFO) << "[CheckHeartBeatReply] dead ";
        }
        delete call;
    }
}

void FollowerNodeClient::HeartBeatLoop() {
    while (!shutdown_) {
        SendHeartbeat();
        std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_INTERVAL / 2));
        CheckHeartBeatReply();
        if (!IsHeadAlive()) {
            LOG(ERROR) << "Head is down. Should initiating head election...";
        }
        std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_INTERVAL / 2));
    }

    // Drain the completion queue after shutdown
    void* ignored_tag;
    bool ignored_ok;
    while (cq_.Next(&ignored_tag, &ignored_ok)) {
        delete static_cast<AsyncClientCall*>(ignored_tag);
    }
}
/*
void FollowerNodeClient::SendHeartbeat() {
	VLOG(3) << "[SendHeartbeat] Sending Heartbeat ";
    HeartbeatRequest request;
    request.set_node_id(node_id_);

    auto call = std::make_unique<AsyncClientCall>();
    call->response_reader = stub_->AsyncHeartbeat(&call->context, request, &cq_);
    call->response_reader->Finish(&call->reply, &call->status, call.get());
    gpr_timespec now = gpr_now(gpr_clock_type::GPR_CLOCK_REALTIME);
    now.tv_sec += HEARTBEAT_INTERVAL;
    call->alarm.Set(&cq_, now, call.get());

    // Transfer ownership to the completion queue
    call.release();
}

void FollowerNodeClient::CheckHeartBeatReply() {
	VLOG(3) << "[CheckHeartBeatReply] Checking ";
	void* got_tag;
	bool ok;
	while (cq_.Next(&got_tag, &ok)) {
	VLOG(3) << "[CheckHeartBeatReply] Checking... ";
		auto* call = static_cast<AsyncClientCall*>(got_tag);
		if (shutdown_) {
				delete call;
				continue;
		}
		if (ok && call->status.ok() && call->reply.alive()) {
			head_alive_ = true;
	VLOG(3) << "[CheckHeartBeatReply] alive ";
		} else {
			head_alive_ = false;
	VLOG(3) << "[CheckHeartBeatReply] dead ";
		}
		delete call;
	}
}

void FollowerNodeClient::HeartBeatLoop() {
	while (!shutdown_) {
		SendHeartbeat();
		CheckHeartBeatReply();
		if (!IsHeadAlive()) {
			LOG(ERROR) << "Head is down. Should initiating head election...";
		}
		std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_INTERVAL));
		SetHeadAlive(true); // Reset head alive status for next check
	}
	// Drain the completion queue after shutdown
	void* ignored_tag;
	bool ignored_ok;
	while (cq_.Next(&ignored_tag, &ignored_ok)) {}
}

*/

} // End of namespace Embarcadero