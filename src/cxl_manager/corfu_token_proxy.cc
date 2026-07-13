#include "cxl_manager/corfu_token_proxy.h"

#include <chrono>
#include <grpcpp/grpcpp.h>

namespace Corfu {

GrpcCorfuTokenProxy::GrpcCorfuTokenProxy(std::string global_sequencer_endpoint)
    : stub_(corfusequencer::CorfuSequencer::NewStub(
          grpc::CreateChannel(global_sequencer_endpoint, grpc::InsecureChannelCredentials()))) {}

grpc::Status GrpcCorfuTokenProxy::GetTotalOrder(
    grpc::ServerContext* context, const corfutokenproxy::TokenRequest* request,
    corfutokenproxy::TokenResponse* response) {
  // Serialize cache miss handling. This keeps a retry from issuing a second
  // token for the same logical (client_id, broker_id, original batchseq) key.
  std::lock_guard<std::mutex> lock(cache_mu_);
  const auto now = std::chrono::steady_clock::now();
  for (auto it = grants_.begin(); it != grants_.end();) {
    if (now - it->second.created > std::chrono::seconds(60)) it = grants_.erase(it);
    else ++it;
  }
  const std::string key = CacheKey(*request);
  if (const auto found = grants_.find(key); found != grants_.end()) {
    response->set_total_order(found->second.total_order);
    response->set_log_idx(found->second.log_idx);
    response->set_broker_batch_seq(found->second.broker_batch_seq);
    return grpc::Status::OK;
  }
  corfusequencer::TotalOrderRequest forwarded;
  forwarded.set_client_id(request->client_id());
  forwarded.set_batchseq(request->batchseq());
  forwarded.set_num_msg(request->num_msg());
  forwarded.set_total_size(request->total_size());
  forwarded.set_broker_id(request->broker_id());
  corfusequencer::TotalOrderResponse grant;
  grpc::ClientContext upstream;
  upstream.set_deadline(context->deadline());
  const grpc::Status status = stub_->GetTotalOrder(&upstream, forwarded, &grant);
  if (!status.ok()) return status;
  response->set_total_order(grant.total_order());
  response->set_log_idx(grant.log_idx());
  response->set_broker_batch_seq(grant.broker_batch_seq());
  if (grants_.size() >= 65536) grants_.erase(grants_.begin());
  grants_.emplace(key, CachedGrant{grant.total_order(), grant.log_idx(),
      grant.broker_batch_seq(), now});
  return grpc::Status::OK;
}

std::string GrpcCorfuTokenProxy::CacheKey(
    const corfutokenproxy::TokenRequest& request) const {
  return std::to_string(request.client_id()) + ":" +
      std::to_string(request.broker_id()) + ":" + std::to_string(request.batchseq());
}

CorfuTokenProxyServer::CorfuTokenProxyServer(std::string listen_endpoint,
    std::string global_sequencer_endpoint)
    : listen_endpoint_(std::move(listen_endpoint)),
      global_sequencer_endpoint_(std::move(global_sequencer_endpoint)) {}

bool CorfuTokenProxyServer::Start(std::string* error) {
  service_ = std::make_unique<GrpcCorfuTokenProxy>(global_sequencer_endpoint_);
  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_endpoint_, grpc::InsecureServerCredentials());
  builder.RegisterService(service_.get());
  server_ = builder.BuildAndStart();
  if (!server_) {
    if (error) *error = "could not listen on " + listen_endpoint_;
    service_.reset();
    return false;
  }
  return true;
}

void CorfuTokenProxyServer::Shutdown() {
  if (server_) server_->Shutdown();
  server_.reset();
  service_.reset();
}

}  // namespace Corfu
