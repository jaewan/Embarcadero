#pragma once

#include <cstdint>

/**
 * Publisher-side pipeline profile (buffer write + send).
 * Lightweight: when disabled, RecordPublisherProfile is a no-op (single atomic load check).
 * Enable via config client.performance.enable_publisher_pipeline_profile or env
 * EMBARCADERO_ENABLE_PUBLISHER_PIPELINE_PROFILE.
 */
enum PublisherPipelineComponent : int {
	kPublisherBufferWrite = 0,
	kPublisherSendPayload,
	kPublisherPublishTotal,  /**< Whole Publish() call (time + bytes for bandwidth). */
	kNumPublisherPipelineComponents
};

/** Record time (ns) for a component. No-op when profiling disabled. */
void RecordPublisherProfile(PublisherPipelineComponent c, uint64_t ns);

/** Record bytes written in buffer write path (for BufferWrite bandwidth). No-op when profiling disabled. */
void RecordPublisherProfileBufferWriteBytes(uint64_t bytes);

/** Record bytes written in Publish() (for Publish() bandwidth). No-op when profiling disabled. */
void RecordPublisherProfilePublishBytes(uint64_t bytes);

/** Enable or disable publisher pipeline profiling (e.g. from config in Publisher::Init). */
void SetPublisherProfileEnabled(bool enabled);

/** Log aggregated profile (total us, count, avg ns, %). Call at shutdown or periodically. */
void LogPublisherProfile();
