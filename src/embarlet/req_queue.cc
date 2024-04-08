#include "req_queue.h"
#include "common/config.h"

namespace Embarcadero {

void EnqueueReq(std::shared_ptr<ReqQueue> queue, std::optional<struct PublishRequest> maybeReq) {
    queue->blockingWrite(maybeReq);
}

void DequeueReq(std::shared_ptr<ReqQueue> queue, std::optional<struct PublishRequest> *maybeReq) {
    queue->blockingRead(*maybeReq);
}

} // end namespace Embarcadero