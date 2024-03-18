#include "pub_queue.h"

void pq_enqueue(PubQueue *pq, PubTask *pt) {
    pq->blockingWrite(pt);
}

void pq_dequeue(PubQueue *pq, PubTask **pt_dest) {
    pq->blockingRead(*pt_dest);
}