#include "req_queue.h"
#include "common/config.h"

namespace Embarcadero {

void EnqueueReq(ReqQueue *queue, std::optional<struct PublishRequest> maybeReq) {
    queue->blockingWrite(maybeReq);
}

void DequeueReq(ReqQueue *queue, std::optional<struct PublishRequest> *maybeReq) {
    queue->blockingRead(*maybeReq);
}

} // end namespace Embarcadero