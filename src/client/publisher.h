#ifndef _PUBLISHER_H_
#define _PUBLISHER_H_

#include <stdint.h>
#include <string>

#include "client.h"

enum PublisherError : uint16_t
{
  ERR_NO_ERROR,
  // ERR_QUEUE_FULL,
  // ERR_UNKNOWN_PARTITION,
  // ERR_UNKNOWN_TOPIC,
  // ERR_TIMED_OUT,
  // ERR_INVALID_ARG,
  ERR_NOT_IMPLEMENTED,
};

enum AckLevel : uint8_t 
{
  // TODO: make descriptive
  ACK0,
  ACK1,
  ACK2,
};

struct PublisherConfig {
  AckLevel ack_level;
  // TODO: eventually modify to have more than one ip/port
  std::string server_ip;
  uint16_t server_port;
};

class Publisher {
  private:
    PublisherConfig *config;
  
  public:
    Publisher(PublisherConfig *config);
};

#endif // _PUBLISHER_H_