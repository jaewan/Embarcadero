#ifndef _PUB_TASK_H_
#define _PUB_TASK_H_

#include <cstdint> // uint8_t

class PubTask { 
  public:
    // Number of sub tasks that need to be completed before this task is fully done
    // and a completion may be triggered
    uint8_t num_subtasks;
  
  // PubTask constructor
  PubTask(uint8_t num_tasks) {
    num_tasks = num_tasks;
  }

  // Pub task destructor
  ~PubTask(void) { }
};

#endif // _PUB_TASK_H_