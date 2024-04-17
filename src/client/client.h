#ifndef _CLIENT_H_
#define _CLIENT_H_

#include <stdint.h>
#include <string>
#include <atomic>
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

struct alignas(64) MessageHeader{
	int client_id;
	size_t client_order;
	volatile size_t size;
	volatile size_t total_order;
	volatile size_t paddedSize;
	void* segment_header;
	size_t logical_offset;
	void* next_message;
};

// Class for a single broker
class PubSubClient {
    public:
        /// Constructor for Client
        explicit PubSubClient(PubConfig *config, std::shared_ptr<Channel> channel, int num_threads, size_t message_size)
            : config_(config), stub_(PubSub::NewStub(channel)), num_threads_(num_threads), message_size_(message_size) {
			messages_ = (void**)malloc(sizeof(void*)*num_threads);
			for (int i=0; i<num_threads; i++){
				messages_[i] = malloc(message_size);
				MessageHeader *header = (MessageHeader*)messages_[i];
				header->client_id = 0;
				header->client_order = 0;
				header->size = message_size;
				header->total_order = 0;
				size_t padding = message_size%64;
				if(padding > 0)
					padding = 64 - padding;
				header->total_order = 0;
				header->paddedSize = message_size + padding;
				header->segment_header = nullptr;
				header->logical_offset = (size_t)-1;;
				header->next_message = nullptr;
			}
		}
		~PubSubClient(){
			for (int i=0; i<num_threads_; i++){
				free(messages_[i]);
			}
			free(messages_);
		}

        PublisherError Publish(std::string topic, int tid, std::atomic<unsigned int> *client_order_) {
			unsigned int order = client_order_->fetch_add(1);
			if((message_size_ * order)>(1UL<<36)){
				LOG(INFO) << "Publish will overflow";
			}
			MessageHeader *header = (MessageHeader*)messages_[tid];
			header->client_order = order;
            GRPCPublishRequest req;
            req.set_topic(topic);
            req.set_acknowledge(config_->acknowledge);
            req.set_client_id(config_->client_id);
            req.set_client_order(order);
            req.set_payload(messages_[tid], message_size_);
            req.set_payload_size(message_size_);
			//config_->client_order++;

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
		int num_threads_;
		size_t message_size_;
		void** messages_;
};

#endif // _CLIENT_H_
