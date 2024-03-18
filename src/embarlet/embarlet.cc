#include "config.h"
#include "pub_queue.h"
#include "pub_task.h"
#include "../disk_manager/disk_manager.h"
#include "../network_manager/network_manager.h"
//#include "../cxl_manager/cxl_manager.h"

int main(int argc, char* argv[]) {

	// Create a pub task
	// This is slightly less than ideal, because it involved two heap allocations.
	// But we'll worry about performance optimizations later.
	std::atomic<uint8_t> *num_subtasks = new std::atomic<uint8_t>(1); // this will be freed by the PubTask destructor
	PubTask *pt = new PubTask(num_subtasks);

	// Create a pub queue
	PubQueue *pq = new PubQueue(PUB_QUEUE_CAPACITY);

	pq_enqueue(pq, pt);
	pq_dequeue(pq, &pt);

	uint8_t tasks_left = pt->subtasks_left->fetch_sub(1, std::memory_order_seq_cst);
	printf("Tasks left: %d\n", tasks_left);
	assert(tasks_left == 1);

	pq_enqueue(pq, pt);
	pq_dequeue(pq, &pt);

	tasks_left = pt->subtasks_left->fetch_sub(1, std::memory_order_seq_cst);
	printf("Tasks left: %d\n", tasks_left);
	if (0 == tasks_left) {
		printf("No tasks left! Time to signal the completion queue before freeing the PubTask!\n");
		// TODO: signal completion queue
		delete pt;
	}
	assert(tasks_left == 0);

	delete pq;
	return 0;
}
