// Two actual manager objects, each with immutable independent allocator geometry.
// Backend attachment/startup is intentionally outside this bounded fixture.
#include "cxl_manager/cxl_manager.h"
#include "cxl_manager/shared_mapping.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sys/mman.h>
#include <vector>

#if EMBARCADERO_ENABLE_FAULT_INJECTION != 1
#error "Compile every linked manager source with the same fault-enabled class definition"
#endif
namespace Embarcadero {
CXLManager::CXLManager(AllocatorFixtureTag, const cxl_manager::RegionLayout& layout,
                       void* owned_mapping)
    : broker_id_(0), head_ip_(), cxl_size_(layout.region_bytes), layout_(layout),
      topic_manager_(nullptr), network_manager_(nullptr), cxl_addr_(owned_mapping),
      control_block_(nullptr), completion_vector_(nullptr), goi_(nullptr),
      base_for_regions_(static_cast<uint8_t*>(owned_mapping) + layout.base_regions_offset),
      bitmap_(static_cast<uint8_t*>(base_for_regions_) + layout.tinode_bytes),
      batchHeaders_(static_cast<uint8_t*>(bitmap_) + layout.bitmap_bytes),
      session_table_(reinterpret_cast<SessionEntry*>(static_cast<uint8_t*>(batchHeaders_) + layout.batch_headers_bytes)),
      segments_(static_cast<uint8_t*>(owned_mapping) + layout.metadata_bytes),
      current_log_addr_(nullptr) {}

class CxlManagerGeometryTestAccess {
 public:
  static std::unique_ptr<CXLManager> Create(const cxl_manager::RegionLayout& layout) {
    void* base = mmap(nullptr, layout.region_bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) throw std::runtime_error("allocator fixture mmap failed");
    cxl_manager::SharedMapping owner(base, layout.region_bytes);
    // Only allocator metadata is initialized; no shared descriptor, GOI, topic,
    // baseline mailbox, global configuration mutation, or worker is needed.
    auto manager = std::unique_ptr<CXLManager>(
        new CXLManager(CXLManager::AllocatorFixtureTag{}, layout, base));
    owner.release();
    return manager;
  }
  static uint8_t* Payload(CXLManager& manager) { return static_cast<uint8_t*>(manager.segments_); }
};
}
namespace {
using namespace Embarcadero;
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
struct Pool {
  const cxl_manager::RegionLayout layout;
  std::unique_ptr<CXLManager> manager;
  std::vector<bool> claimed;
  std::vector<uint8_t*> retained;
  const uint8_t pattern;
  Pool(size_t bytes, size_t stride, uint8_t byte)
      : layout(cxl_manager::CalculateRegionLayout(bytes, stride, 1, 1, 4096, 8192)),
        manager(CxlManagerGeometryTestAccess::Create(layout)),
        claimed(layout.segment_count), pattern(byte) {
    Check(layout.segment_count > 1 && layout.segment_count < 512, "invalid bounded test capacity");
    // Real GetNewSegment must clear exactly its first cache line, not data in
    // this or any other segment. Distinct patterns make cross-manager writes visible.
    std::memset(CxlManagerGeometryTestAccess::Payload(*manager), pattern, layout.payload_bytes);
  }
  bool Allocate() {
    void* allocation = manager->GetNewSegment();
    if (!allocation) {
      Check(retained.size() == layout.segment_count, "allocator exhausted early or reused another manager's capacity");
      return false;
    }
    auto* p = static_cast<uint8_t*>(allocation);
    const auto base = reinterpret_cast<uintptr_t>(CxlManagerGeometryTestAccess::Payload(*manager));
    const auto address = reinterpret_cast<uintptr_t>(p);
    Check(address >= base && address - base < layout.segment_count * layout.segment_size,
          "allocation outside its manager's segment pool");
    const size_t delta = address - base;
    Check(delta % layout.segment_size == 0, "allocator reused another manager's segment stride");
    const size_t index = delta / layout.segment_size;
    Check(!claimed[index], "duplicate allocation before exhaustion");
    for (size_t i = 0; i < 64; ++i) Check(p[i] == 0, "segment metadata was not initialized");
    Check(p[64] == pattern && p[layout.segment_size - 1] == pattern,
          "allocator overwrote retained payload");
    claimed[index] = true; retained.push_back(p);
    p[64] = static_cast<uint8_t>(pattern ^ 0x5a);
    return true;
  }
  void VerifyRetained() const {
    for (auto* p : retained) {
      Check(p[64] == static_cast<uint8_t>(pattern ^ 0x5a) &&
            p[layout.segment_size - 1] == pattern, "retained data changed across allocation/exhaustion");
    }
  }
};
}
void RunCxlManagerGeometryTest() {
  // Same calling thread deliberately carries GetNewSegment's legitimate TLS
  // search hint between managers; count and stride must still come from each instance.
  Pool small(2 * 1024 * 1024, 64 * 1024, 0x31);
  Pool large(4 * 1024 * 1024, 256 * 1024, 0x72);
  Check(small.layout.segment_count != large.layout.segment_count, "fixture capacities must differ");
  for (size_t i = 0; i < std::max(small.layout.segment_count, large.layout.segment_count); ++i) {
    small.Allocate(); large.Allocate();
    small.VerifyRetained(); large.VerifyRetained();
  }
  Check(!small.Allocate() && !large.Allocate(), "exhaustion must remain terminal");
  // Construct a third manager after the first two are exhausted to catch sticky
  // process-static geometry or exhausted allocation state across additional instances.
  Pool third(3 * 1024 * 1024, 128 * 1024, 0x19);
  while (third.Allocate()) {}
  small.VerifyRetained(); large.VerifyRetained(); third.VerifyRetained();
  std::cout << "PASS actual CXLManager independent geometry, stride, exhaustion and retention\n";
}
