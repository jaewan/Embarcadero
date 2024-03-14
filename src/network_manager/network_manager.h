#ifndef _NETWORK_MANAGER_H_
#define _NETWORK_MANAGER_H_

#include <cstdint> // uint8_t

class PublisherTask { 
  public:
    uint8_t num_tasks;

  // Member functions definitions including constructor
  PublisherTask(int num_tasks) {
    num_tasks = num_tasks;
  }

  ~PublisherTask(void) { }
};

void do_something(void);

#endif // _NETWORK_MANAGER_H_