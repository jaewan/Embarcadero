#pragma once

#include <mutex>
#include <string>
#include <string_view>

namespace Embarcadero {

// GOI/CV are region-global in the current prototype. Keep one topic identity
// for the lifetime of its manager, including after failed creation or deletion:
// those operations do not reset the region's ordering and replication state.
// TopicManager calls this while holding topics_mutex_, before allocation or
// metadata writes. The internal lock also makes independent admission atomic.
class SingleTopicAdmission {
 public:
  bool TryAdmit(std::string_view topic) {
    if (topic.empty() || topic.size() >= 256 || topic.find('\0') != std::string_view::npos)
      return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (topic_.empty()) topic_.assign(topic.data(), topic.size());
    return topic_ == topic;
  }

 private:
  std::mutex mutex_;
  std::string topic_;
};

}  // namespace Embarcadero
