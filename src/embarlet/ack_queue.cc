#include "ack_queue.h"
#include "common/config.h"

namespace Embarcadero {

void EnqueueAck(std::shared_ptr<AckQueue> queue, std::optional<void *> maybeTag) {
    queue->blockingWrite(maybeTag);
}

void DequeueAck(std::shared_ptr<AckQueue> queue, std::optional<void *> *maybeTag) {
    queue->blockingRead(*maybeTag);
}

} // end namespace Embarcadero