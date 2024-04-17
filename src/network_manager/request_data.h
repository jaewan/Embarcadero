#ifndef _REQUEST_DATA_H_
#define _REQUEST_DATA_H_

#include <optional>
#include <grpcpp/grpcpp.h>

#include <pubsub.grpc.pb.h>
#include "../client/client.h"
#include "../embarlet/req_queue.h"
#include "../embarlet/ack_queue.h"

using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerCompletionQueue;
using grpc::ServerContext;
using grpc::Status;

namespace Embarcadero{

class RequestData {
public:
    // What we get from the client.
    GRPCPublishRequest request_;

    // Take in the "service" instance (in this case representing an asynchronous
    // server) and the completion queue "cq" used for asynchronous communication
    // with the gRPC runtime.
    RequestData(PubSub::AsyncService* service, ServerCompletionQueue* cq, std::shared_ptr<ReqQueue> reqQueueCXL,
	std::shared_ptr<ReqQueue> reqQueueDisk)
            : service_(service),
			cq_(cq), responder_(&ctx_), 
			status_(CREATE), 
			reqQueueCXL_(reqQueueCXL), 
			reqQueueDisk_(reqQueueDisk) {
        // Invoke the serving logic right away.
        Proceed();
    }

    void Proceed() {
		VLOG(3) << "In RequestData.Proceed() with status_ == " << status_;
        if (status_ == CREATE) {
            // Make this instance progress to the PROCESS state.
            VLOG(3) << "Creating call data, asking for RPC";
            status_ = PROCESS;
			reply_.set_error(ERR_NO_ERROR);

            // As part of the initial CREATE state, we *request* that the system
            // start processing SayHello requests. In this request, "this" acts are
            // the tag uniquely identifying the request (so that different RequestData
            // instances can serve different requests concurrently), in this case
            // the memory address of this RequestData instance.
            service_->RequestPublish(&ctx_, &request_, &responder_, cq_, cq_, this);
        } else if (status_ == PROCESS) {
            // Spawn a new RequestData instance to serve new clients while we process
            // the one for this RequestData. The instance will deallocate itself as
            // part of its FINISH state.
			VLOG(3) << "Creating new RequestData object in RequestData";
            new RequestData(service_, cq_, reqQueueCXL_, reqQueueDisk_);
			/*
            PublishRequest req;
			counter_ = new std::atomic<int>(2);
            req.counter = counter_;
			req.grpcTag = this;
			auto maybeReq = std::make_optional(req);
			//TODO(Erika)
			//We have two requests, publish and subscribe.

            if (request_.acknowledge()) {
                // Wait for acknowlegment to respond
                status_ = ACKNOWLEDGE;
            } else {
                // If no acknowledgement is needed, respond now and signal this object ready for destruction
                status_ = FINISH;
                responder_.Finish(reply_, Status::OK, this);
            }

            // No matter what, we need to do processing tasks.
            EnqueueReq(reqQueueCXL_, maybeReq);
		    EnqueueReq(reqQueueDisk_, maybeReq);
			*/
			status_ = FINISH;
			responder_.Finish(reply_, Status::OK, this);
			//Finish();
        /* } else if (status_ == ACKNOWLEDGE) {
            VLOG(3) << "Acknowledging the RequestData() object";

            // And we are done! Let the gRPC runtime know we've finished, using the
            // memory address of this instance as the uniquely identifying tag for
            // the event.
            status_ = FINISH;
            responder_.Finish(reply_, Status::OK, this);
			VLOG(3) << "Ack responder_.Finish called called";
        */
		} else {
            // If we asked for acknowledgement, we can destruct right after moving to FINISH
            // If we did not ask for acknowledgement, we will let cxl/disk to destroy the object
            // by calling the Finish() function.
            //if (request_.acknowledge()) {
                Finish();
            //}
        }
		
    }

  	void SetError(PublisherError err) {
		// Only overwrite error if currently a success
		if (reply_.error() == ERR_NO_ERROR) {
			  reply_.set_error(err);
		}
  	}

    void Finish() {
        VLOG(3) << "Finish() called";
		//while(counter_->load() != 0){std::this_thread::yield();}
        GPR_ASSERT(status_ == FINISH);
        VLOG(3) << "destructing requestdata";
        // Once in the FINISH state, deallocate ourselves (RequestData).
		//delete counter_;
        delete this;
    }

private:
    // The means of communication with the gRPC runtime for an asynchronous
    // server.
    PubSub::AsyncService* service_;
    // The producer-consumer queue where for asynchronous server notifications.
    ServerCompletionQueue* cq_;
    // Context for the rpc, allowing to tweak aspects of it such as the use
    // of compression, authentication, as well as to send metadata back to the
    // client.
    ServerContext ctx_;

    // What we send back to the client.
    GRPCPublishResponse reply_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<GRPCPublishResponse> responder_;

    // Let's implement a tiny state machine with the following states.
    enum CallStatus { CREATE, PROCESS, ACKNOWLEDGE, FINISH };
    CallStatus status_;  // The current serving state.
	std::atomic<int> *counter_;

    std::shared_ptr<ReqQueue> reqQueueCXL_;
    std::shared_ptr<ReqQueue> reqQueueDisk_;
  };

} // End of namespace Embarcadero
#endif // _REQUEST_DATA_H_
