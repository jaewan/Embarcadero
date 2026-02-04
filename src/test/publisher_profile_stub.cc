/**
 * No-op stubs for publisher pipeline profiling. Used by buffer_benchmark so we
 * don't need to link publisher.cc. All recording is disabled.
 */
#include "../client/publisher_profile.h"

void RecordPublisherProfile(PublisherPipelineComponent /*c*/, uint64_t /*ns*/) {}
void RecordPublisherProfileBufferWriteBytes(uint64_t /*bytes*/) {}
void RecordPublisherProfilePublishBytes(uint64_t /*bytes*/) {}
void SetPublisherProfileEnabled(bool /*enabled*/) {}
void LogPublisherProfile() {}
