// Production CXL-mailbox global-sequencer entry point. It attaches only to the
// mapping broker 0 initialized and never creates or clears mailbox state. A
// real CXL deployment may expose that mapping either as a DAX device or as a
// shared-memory object whose pages broker 0 bound to the CXL NUMA node. The
// latter is the normal configuration on machines with CXL memory but no
// /dev/dax namespace. Arbitrary file backing remains rejected.

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glog/logging.h>

#include "cxl_manager/baseline_cxl_layout.h"
#include "cxl_manager/corfu_mailbox_sequencer.h"
#include "cxl_manager/lazylog_binding_core.h"
#include "cxl_manager/lazylog_mailbox_sequencer.h"
#include "cxl_manager/scalog_global_ordering_core.h"
#include "cxl_manager/scalog_mailbox_sequencer.h"

#ifndef MAILBOX_SEQUENCER_KIND
#error "MAILBOX_SEQUENCER_KIND must select corfu, scalog, or lazylog"
#endif

namespace {
std::atomic<bool> g_stop{false};
void StopSignal(int) { g_stop.store(true, std::memory_order_relaxed); }

int ParsePositiveEnv(const char* name, int fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  CHECK(end != value && *end == '\0' && parsed > 0 && parsed <= INT32_MAX)
      << "invalid " << name << "='" << value << "'";
  return static_cast<int>(parsed);
}

void PrintUsage(const char* argv0) {
  std::fprintf(stderr,
      "Usage: %s\n"
      "Requires an initialized broker-0 CXL mailbox. Attach via "
      "EMBARCADERO_CXL_DEVICE=/dev/dax* or EMBARCADERO_CXL_SHM_NAME for a "
      "CXL-NUMA shared mapping.\n",
      argv0);
}

std::string SanitizeShmNameForFilePath(const std::string& shm_name) {
  std::string sanitized = shm_name;
  for (char& ch : sanitized) {
    if (ch == '/') ch = '_';
  }
  return "/tmp/embarcadero_cxl" + sanitized;
}

int OpenMailboxBacking(std::string* kind) {
  const char* device = std::getenv("EMBARCADERO_CXL_DEVICE");
  if (device != nullptr && device[0] != '\0') {
    struct stat st {};
    PCHECK(stat(device, &st) == 0) << "cannot stat CXL device " << device;
    CHECK(S_ISCHR(st.st_mode)) << "EMBARCADERO_CXL_DEVICE must be a DAX character device: "
                                << device;
    const int fd = open(device, O_RDWR);
    PCHECK(fd >= 0) << "cannot open CXL device " << device;
    *kind = "dax";
    return fd;
  }

  const char* name_env = std::getenv("EMBARCADERO_CXL_SHM_NAME");
  CHECK(name_env != nullptr && name_env[0] != '\0')
      << "set EMBARCADERO_CXL_DEVICE or EMBARCADERO_CXL_SHM_NAME";
  const std::string shm_name(name_env);
  int fd = shm_open(shm_name.c_str(), O_RDWR, 0);
  if (fd < 0 && (errno == EACCES || errno == EPERM)) {
    const std::string fallback = SanitizeShmNameForFilePath(shm_name);
    fd = open(fallback.c_str(), O_RDWR);
  }
  PCHECK(fd >= 0) << "cannot attach CXL shared mapping " << shm_name;
  *kind = "shm_numa_cxl";
  return fd;
}
}  // namespace

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  FLAGS_logtostderr = 1;
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    PrintUsage(argv[0]);
    return 0;
  }
  if (argc != 1) {
    PrintUsage(argv[0]);
    return 2;
  }

  std::string backing_kind;
  const int fd = OpenMailboxBacking(&backing_kind);
  // The fixed CXL layout uses the configured *maximum* broker count, which
  // need not equal the active run's broker count nor a launcher default. Map
  // just the published segment header first, derive its exact extent, then
  // remap that extent before AttachInPlace validates it. This keeps a
  // standalone sequencer from guessing a smaller mapping and SIGABRTing on a
  // perfectly valid production mailbox.
  const off_t mailbox_offset = static_cast<off_t>(Embarcadero::cxl_manager::BaselineMailboxOffset());
  void* header_base = mmap(nullptr, sizeof(Embarcadero::cxl_transport::MailboxSegmentHeader),
                           PROT_READ | PROT_WRITE, MAP_SHARED, fd, mailbox_offset);
  PCHECK(header_base != MAP_FAILED) << "cannot map mailbox header from " << backing_kind;
  auto* header = reinterpret_cast<Embarcadero::cxl_transport::MailboxSegmentHeader*>(header_base);
  Embarcadero::CXL::invalidate_cacheline_for_read(header);
  Embarcadero::CXL::full_fence();
  CHECK_EQ(header->magic, Embarcadero::cxl_transport::kMailboxSegmentMagic)
      << "broker 0 has not initialized the mailbox";
  CHECK_EQ(header->version, Embarcadero::cxl_transport::kMailboxSegmentVersion)
      << "mailbox version mismatch";
  CHECK_GT(header->num_brokers, 0u);
  const size_t bytes = static_cast<size_t>(header->down_base) +
      static_cast<size_t>(header->down_stride) * header->num_brokers;
  CHECK_LE(bytes, Embarcadero::cxl_manager::BaselineMailboxBytes(32))
      << "published mailbox extent exceeds supported layout maximum";
  PCHECK(munmap(header_base, sizeof(Embarcadero::cxl_transport::MailboxSegmentHeader)) == 0);
  void* base = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mailbox_offset);
  PCHECK(base != MAP_FAILED) << "cannot map mailbox extent from " << backing_kind;
  close(fd);
  std::unique_ptr<Embarcadero::cxl_transport::MailboxSegment> segment =
      Embarcadero::cxl_transport::MailboxSegment::AttachInPlace(base, bytes);
  CHECK_EQ(segment->record_size(), Embarcadero::cxl_manager::kMailboxRecordSize);
  CHECK_EQ(segment->up_capacity(), Embarcadero::cxl_manager::kMailboxCapacity);
  CHECK_EQ(segment->down_capacity(), Embarcadero::cxl_manager::kMailboxCapacity);
  LOG(INFO) << "BASELINE_MAILBOX_SEQUENCER_ATTACHED backing=" << backing_kind << " layout_v"
            << Embarcadero::cxl_manager::kCxlLayoutVersion;

  std::signal(SIGINT, StopSignal);
  std::signal(SIGTERM, StopSignal);
#if MAILBOX_SEQUENCER_KIND == 1
  CorfuSequencerImpl impl;
  Embarcadero::cxl_manager::CorfuMailboxSequencer sequencer(&impl, segment.get());
#elif MAILBOX_SEQUENCER_KIND == 2
  Embarcadero::cxl_manager::ScalogGlobalOrderingCore core(
      ParsePositiveEnv("EMBARCADERO_REPLICATION_FACTOR", 1));
  for (uint32_t b = 0; b < segment->num_brokers(); ++b) {
    core.RegisterBroker(static_cast<int>(b), ParsePositiveEnv("EMBARCADERO_REPLICATION_FACTOR", 1));
  }
  Embarcadero::cxl_manager::ScalogMailboxSequencer sequencer(&core, segment.get());
#elif MAILBOX_SEQUENCER_KIND == 3
  Embarcadero::cxl_manager::LazyLogBindingCore core;
  for (uint32_t b = 0; b < segment->num_brokers(); ++b) core.RegisterBroker(static_cast<int>(b));
  Embarcadero::cxl_manager::LazyLogMailboxSequencer sequencer(&core, segment.get());
#endif
  sequencer.StartThread();
  while (!g_stop.load(std::memory_order_relaxed)) usleep(10000);
  sequencer.Stop();
  sequencer.Join();
  munmap(base, bytes);
  return 0;
}
