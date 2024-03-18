#ifndef _PUB_TASK_H_
#define _PUB_TASK_H_

#include <cstddef> // NULL
#include <cstdint> // uint8_t
#include <atomic>

class PubTask { 
  public:
    // Msg palceholder
    void *msg_ptr;

    // Completion queue placeholder
    void *cq;

    // Tracks the subtasks completed - when it hits zero, completion can be signaled.
    std::atomic<uint8_t> *subtasks_left;
  
  // PubTask constructor
  PubTask(std::atomic<uint8_t> *num_tasks) {
    msg_ptr = NULL;
    cq = NULL;
    subtasks_left = num_tasks;
  }

  // Pub task destructor
  ~PubTask(void) {
    delete subtasks_left;
  }
};

#endif // _PUB_TASK_H_