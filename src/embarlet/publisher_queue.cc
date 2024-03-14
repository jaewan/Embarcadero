#include "folly/MPMCQueue.h"

void create_queue(void) {
    folly::MPMCQueue<int> cq(10);
}