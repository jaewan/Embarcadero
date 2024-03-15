#ifndef _PUB_QUEUE_H_
#define _PUB_QUEUE_H_

// library includes
#include "folly/MPMCQueue.h"

// local includes
#include "pub_task.h"

// defines
#define PUB_QUEUE_ELEMENTS 1024

// function definitions
folly::MPMCQueue<PubTask> create_pub_queue(void);

#endif // _PUB_QUEUE_H_