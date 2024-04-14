#ifndef _CLIENT_H_
#define _CLIENT_H_

#include <stdint.h>
#include <string>
#include <grpcpp/grpcpp.h>
#include <pubsub.grpc.pb.h>
#include "common/config.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

// Note: if you change this type, change also in pubsub.proto
typedef uint64_t Topic;

// Note: if you change this type, change the type in pubsub.proto as well!
enum PublisherError : uint32_t
{
  ERR_NO_ERROR,
  // ERR_QUEUE_FULL,
  // ERR_UNKNOWN_PARTITION,
  // ERR_UNKNOWN_TOPIC,
  // ERR_TIMED_OUT,
  // ERR_INVALID_ARG,
  ERR_GRPC_ERROR,
  ERR_NOT_IMPLEMENTED,
};

struct PubConfig {
    bool acknowledge;
    uint64_t client_id;
    uint64_t client_order;
};

/// Class for a single broker
class PubSubClient {
    public:
        /// Constructor for Client
        explicit PubSubClient(PubConfig *config, std::shared_ptr<Channel> channel)
            : stub_(PubSub::NewStub(channel)), config_(config) {}

        PublisherError Publish(std::string topic, char *payload, uint64_t payload_size) {
            GRPCPublishRequest req;
            req.set_topic(topic);
            req.set_acknowledge(config_->acknowledge);
            req.set_client_id(config_->client_id);
            req.set_client_order(config_->client_order);
            req.set_payload(payload, payload_size);

            GRPCPublishResponse res;

            ClientContext context;

            Status status = stub_->Publish(&context, req, &res);

            if (status.ok()) {
                return PublisherError(res.error());
            } else {
                std::cerr << status.error_code() << ": " << status.error_message() << std::endl;
                return ERR_GRPC_ERROR;
            }


        }

    private:
        PubConfig *config_;
        std::unique_ptr<PubSub::Stub> stub_;
};

#endif // _CLIENT_H_