#ifndef _PUBLISHER_H_
#define _PUBLISHER_H_

#include <stdint.h>

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

#endif // _PUBLISHER_H_