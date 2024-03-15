#include "pub_queue.h"

folly::MPMCQueue<PubTask> create_pub_queue(void) {
    folly::MPMCQueue<PubTask> pq(PUB_QUEUE_ELEMENTS);
    return pq;
}