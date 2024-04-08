#ifndef _ACK_QUEUE_H_
#define _ACK_QUEUE_H_

#include <optional>
#include "folly/MPMCQueue.h"

#include "common/config.h"

namespace Embarcadero {

using AckQueue = folly::MPMCQueue<std::optional<void *>>;
void EnqueueAck(AckQueue *queue, std::optional<void *> maybeTag);
void DequeueAck(AckQueue *queue, std::optional<void *> *maybeTag);

} // End of namespace Embarcadero
#endif // _ACK_QUEUE_H_
