#ifndef _REQ_QUEUE_H_
#define _REQ_QUEUE_H_

#include <optional>
#include "folly/MPMCQueue.h"

#include "common/config.h"

namespace Embarcadero {

using ReqQueue = folly::MPMCQueue<std::optional<struct PublishRequest>>;

using AckQueue = folly::MPMCQueue<std::optional<void *>>;
void EnqueueReq(std::shared_ptr<ReqQueue> queue, std::optional<struct PublishRequest> maybeTag);
void DequeueReq(std::shared_ptr<ReqQueue> queue, std::optional<struct PublishRequest> *maybeTag);

} // End of namespace Embarcadero
#endif // _REQ_QUEUE_H_
