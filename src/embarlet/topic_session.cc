#include "topic.h"
#include "topic_session_key.h"
#include "session_admission.h"
#include "common/fault_injection.h"
#include "common/performance_utils.h"
#include <glog/logging.h>

namespace Embarcadero {

// Authoritative session admission/publication. These methods own the shared
// table operations; the sequencer still owns classification and commit order.
SessionEntry* Topic::FindSessionEntry(uint64_t session_key) {
	if (session_key == 0 || session_table_ == nullptr) return nullptr;
	const size_t start = static_cast<size_t>(Mix64(session_key) % kMaxSessions);
	for (size_t probe = 0; probe < kMaxSessions; ++probe) {
		SessionEntry* entry = &session_table_[(start + probe) % kMaxSessions];
		CXL::invalidate_cacheline_for_read(entry);
		CXL::load_fence();
		const uint64_t observed = entry->session_key.load(std::memory_order_acquire);
		if (observed == session_key) return entry;
		if (observed == 0) return nullptr;
	}
	return nullptr;
}

SessionEntry* Topic::FindOrCreateSessionEntry(uint64_t session_key) {
  bool claimed = false;
  SessionEntry* entry = FindOrClaimSession(session_table_, kMaxSessions, session_key,
      static_cast<size_t>(Mix64(session_key) % kMaxSessions), [](SessionEntry* candidate) {
        CXL::invalidate_cacheline_for_read(candidate);
        CXL::load_fence();
      }, &claimed);
  if (entry && SessionClaimNeedsFlush(claimed,
          entry->state_word.load(std::memory_order_acquire))) {
    CXL::flush_cacheline(entry);
    CXL::store_fence();
  }
  return entry;
}

Topic::SessionAdmission Topic::TryAdmitSession(uint32_t client_id, uint32_t epoch) {
  if (client_id == 0 || epoch == 0 || IsBLogCapacityExhausted())
    return SessionAdmission::unavailable;
  SessionEntry* entry = FindOrCreateSessionEntry(MakeSessionKey(client_id, epoch));
  if (!entry) return SessionAdmission::unavailable;
  CXL::invalidate_cacheline_for_read(entry);
  CXL::load_fence();
  return (entry->state_word.load(std::memory_order_acquire) & kSessionEntryFlagFenced)
      ? SessionAdmission::fenced : SessionAdmission::admitted;
}

bool Topic::PublishSessionEntry(uint64_t session_key, const SessionPublishSnapshot& snapshot) {
	if (snapshot.session_epoch == 0) return true;
	SessionEntry* entry = FindOrCreateSessionEntry(session_key);
    if (entry == nullptr) {
      // Commit admission and SessionOpen must reserve this identity first.
      LOG(ERROR) << "Authoritative session table unavailable during publication topic=" << topic_name_;
      return false;
    }

    StoreSessionMaximum(entry->expected_seq, snapshot.expected_seq);
#if EMBARCADERO_ENABLE_FAULT_INJECTION == 1
    // Observe the real intermediate publication while the caller retains its gate.
    // Cancellation releases the pause but must not leave a half-published snapshot.
    (void)fault::Pause("session.after_expected_before_hwm",
        {session_key >> 32, snapshot.session_epoch, snapshot.committed_hwm,
         snapshot.expected_seq, snapshot.fenced ? 1ULL : 0ULL}, &stop_threads_);
#endif
    StoreSessionMaximum(entry->committed_hwm, snapshot.committed_hwm);
    StoreSessionMaximum(entry->highest_sequenced, snapshot.highest_sequenced);
	CXL::store_fence();
	CXL::flush_cacheline(entry);
	CXL::store_fence();

    // Skip an unchanged locked RMW, but retain both visibility sequences:
    // the completed prefix flush above must precede ACTIVE/fence publication.
    MergeSessionState(entry->state_word, snapshot.session_epoch, snapshot.fenced);
	CXL::store_fence();
	CXL::flush_cacheline(&entry->state_word);
	CXL::store_fence();
    return true;
}

}  // namespace Embarcadero
