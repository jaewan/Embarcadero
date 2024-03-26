#ifndef _PUB_QUEUE_H_
#define _PUB_QUEUE_H_

// library includes
#include "folly/MPMCQueue.h"

// local includes
#include "pub_task.h"
#include <atomic>

// defines
#define PUB_QUEUE_CAPACITY 1024 // TODO: is this bytes or slots? Need to double check.

// types
using PubQueue = folly::MPMCQueue<PubTask*>;

// function definitions
void pq_enqueue(PubQueue *pq, PubTask *pt);
void pq_dequeue(PubQueue *pq, PubTask **pt_dest);

struct publish_request{
	int client_id;
	int request_id;
	char topic[32];
	int offset;
	std::atomic<int> *counter;
	void* payload_address;
};

#endif // _PUB_QUEUE_H_
