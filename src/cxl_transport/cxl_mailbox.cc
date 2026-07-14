#include "cxl_transport/cxl_mailbox.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

#include <glog/logging.h>

namespace Embarcadero {
namespace cxl_transport {

namespace {
std::string ShmPath(const std::string& name) {
	// POSIX shm names must start with a single '/'.
	return name.empty() || name[0] == '/' ? name : "/" + name;
}
}  // namespace

size_t MailboxSegment::BytesNeeded(const MailboxParams& p) {
	const size_t up_stride = MailboxRing::BytesNeeded(p.record_size, p.up_capacity);
	const size_t down_stride = MailboxRing::BytesNeeded(p.record_size, p.down_capacity);
	return AlignUp(sizeof(MailboxSegmentHeader), kCacheLine) +
	       up_stride * p.num_brokers + down_stride * p.num_brokers;
}

void MailboxSegment::BindRings(void* base, bool create, const MailboxParams* cp) {
	auto* h = reinterpret_cast<MailboxSegmentHeader*>(base);
	if (create) {
		CHECK_NOTNULL(cp);
		CHECK_GT(cp->num_brokers, 0u);
		CHECK_GT(cp->record_size, 0u);
		CHECK((cp->up_capacity & (cp->up_capacity - 1)) == 0) << "up_capacity must be power of two";
		CHECK((cp->down_capacity & (cp->down_capacity - 1)) == 0) << "down_capacity must be power of two";
		std::memset(h, 0, sizeof(*h));
		h->magic = kMailboxSegmentMagic;
		h->version = kMailboxSegmentVersion;
		h->num_brokers = cp->num_brokers;
		h->record_size = cp->record_size;
		h->up_capacity = cp->up_capacity;
		h->down_capacity = cp->down_capacity;
		h->up_stride = MailboxRing::BytesNeeded(cp->record_size, cp->up_capacity);
		h->down_stride = MailboxRing::BytesNeeded(cp->record_size, cp->down_capacity);
		h->up_base = AlignUp(sizeof(MailboxSegmentHeader), kCacheLine);
		h->down_base = h->up_base + h->up_stride * cp->num_brokers;
		h->broadcast_spin_bound = cp->broadcast_spin_bound;
		CXL::flush_cacheline(h);
		CXL::store_fence();
	} else {
		// The header is untrusted shared state at attach time.  Validate every extent
		// before deriving ring addresses: attaching a stale/different layout must fail
		// closed rather than turn a configuration mismatch into out-of-bounds accesses.
		CHECK_GE(bytes_, sizeof(MailboxSegmentHeader));
		CXL::invalidate_cacheline_for_read(h);
		CXL::full_fence();
		CHECK_EQ(h->magic, kMailboxSegmentMagic) << "not a mailbox segment / uninitialized";
		CHECK_EQ(h->version, kMailboxSegmentVersion) << "mailbox segment version mismatch";
		CHECK_GT(h->num_brokers, 0u);
		CHECK_GT(h->record_size, 0u);
		CHECK_GT(h->up_capacity, 0u);
		CHECK_GT(h->down_capacity, 0u);
		CHECK_EQ(h->up_capacity & (h->up_capacity - 1), 0u)
				<< "mailbox up_capacity is not a power of two";
		CHECK_EQ(h->down_capacity & (h->down_capacity - 1), 0u)
				<< "mailbox down_capacity is not a power of two";
		CHECK_EQ(h->up_stride, MailboxRing::BytesNeeded(h->record_size, h->up_capacity))
				<< "mailbox up-ring layout mismatch";
		CHECK_EQ(h->down_stride, MailboxRing::BytesNeeded(h->record_size, h->down_capacity))
				<< "mailbox down-ring layout mismatch";
		CHECK_GE(h->up_base, sizeof(MailboxSegmentHeader));
		CHECK_GE(h->down_base, h->up_base);
		CHECK_LE(h->up_base, bytes_);
		CHECK_LE(h->up_stride, (bytes_ - h->up_base) / h->num_brokers)
				<< "mailbox upstream extent exceeds mapped region";
		const size_t up_end = h->up_base + h->up_stride * h->num_brokers;
		CHECK_GE(h->down_base, up_end) << "mailbox ring extents overlap";
		CHECK_LE(h->down_base, bytes_);
		CHECK_LE(h->down_stride, (bytes_ - h->down_base) / h->num_brokers)
				<< "mailbox downstream extent exceeds mapped region";
	}

	num_brokers_ = h->num_brokers;
	record_size_ = h->record_size;
	broadcast_spin_bound_ = h->broadcast_spin_bound;
	auto* bytes = reinterpret_cast<uint8_t*>(base);
	up_.clear();
	down_.clear();
	up_.reserve(num_brokers_);
	down_.reserve(num_brokers_);
	for (uint32_t b = 0; b < num_brokers_; ++b) {
		void* up_region = bytes + h->up_base + h->up_stride * b;
		void* down_region = bytes + h->down_base + h->down_stride * b;
		if (create) {
			up_.push_back(MailboxRing::Create(up_region, h->up_stride, h->record_size, h->up_capacity));
			down_.push_back(MailboxRing::Create(down_region, h->down_stride, h->record_size, h->down_capacity));
		} else {
			up_.push_back(MailboxRing::Attach(up_region, h->up_stride));
			down_.push_back(MailboxRing::Attach(down_region, h->down_stride));
		}
	}
}

std::unique_ptr<MailboxSegment> MailboxSegment::CreateShm(const std::string& name, const MailboxParams& p) {
	CHECK_GT(p.num_brokers, 0u);
	const std::string path = ShmPath(name);
	shm_unlink(path.c_str());  // clear any stale segment
	int fd = shm_open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
	PCHECK(fd >= 0) << "shm_open(create) failed for " << path;
	const size_t bytes = BytesNeeded(p);
	PCHECK(ftruncate(fd, static_cast<off_t>(bytes)) == 0) << "ftruncate failed for " << path;
	void* base = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	PCHECK(base != MAP_FAILED) << "mmap failed for " << path;

	// Hand ownership of the fd + mapping to the segment IMMEDIATELY, before BindRings can
	// fail, so the destructor closes the fd / unmaps / unlinks on any later error (P2-2:
	// no fd leak between mmap succeeding and the segment taking ownership).
	std::unique_ptr<MailboxSegment> seg(new MailboxSegment());
	seg->base_ = base;
	seg->bytes_ = bytes;
	seg->shm_fd_ = fd;
	seg->shm_name_ = path;
	seg->owns_mapping_ = true;
	seg->BindRings(base, /*create=*/true, &p);
	LOG(INFO) << "MailboxSegment created: " << path << " (" << p.num_brokers
	          << " brokers, " << bytes << " bytes)";
	return seg;
}

std::unique_ptr<MailboxSegment> MailboxSegment::AttachShm(const std::string& name) {
	const std::string path = ShmPath(name);
	int fd = shm_open(path.c_str(), O_RDWR, 0666);
	PCHECK(fd >= 0) << "shm_open(attach) failed for " << path;
	struct stat st{};
	PCHECK(fstat(fd, &st) == 0) << "fstat failed for " << path;
	const size_t bytes = static_cast<size_t>(st.st_size);
	void* base = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	PCHECK(base != MAP_FAILED) << "mmap failed for " << path;

	std::unique_ptr<MailboxSegment> seg(new MailboxSegment());
	seg->base_ = base;
	seg->bytes_ = bytes;
	seg->shm_fd_ = fd;
	seg->owns_mapping_ = true;  // we mmap'd; but shm_name_ empty => we do not unlink
	seg->BindRings(base, /*create=*/false, nullptr);
	return seg;
}

std::unique_ptr<MailboxSegment> MailboxSegment::CreateInPlace(void* base, size_t bytes, const MailboxParams& p) {
	CHECK(base != nullptr);
	// Enforce 64-byte base alignment: the segment header and every ring header land on
	// cache-line boundaries only if the region base is aligned (P0-1).
	CHECK_EQ(reinterpret_cast<uintptr_t>(base) % kCacheLine, 0UL)
	    << "MailboxSegment base must be 64-byte aligned; got " << base;
	CHECK_GE(bytes, BytesNeeded(p));
	std::unique_ptr<MailboxSegment> seg(new MailboxSegment());
	seg->base_ = base;
	seg->bytes_ = bytes;
	seg->owns_mapping_ = false;
	seg->BindRings(base, /*create=*/true, &p);
	return seg;
}

std::unique_ptr<MailboxSegment> MailboxSegment::AttachInPlace(void* base, size_t bytes) {
	CHECK(base != nullptr);
	// Attach must also reject a misaligned base (a peer may map at a different address).
	CHECK_EQ(reinterpret_cast<uintptr_t>(base) % kCacheLine, 0UL)
	    << "MailboxSegment base must be 64-byte aligned; got " << base;
	std::unique_ptr<MailboxSegment> seg(new MailboxSegment());
	seg->base_ = base;
	seg->bytes_ = bytes;
	seg->owns_mapping_ = false;
	seg->BindRings(base, /*create=*/false, nullptr);
	return seg;
}

BroadcastStatus MailboxSegment::BroadcastDownNonBlocking(const void* record, uint32_t len,
                                                         uint64_t spin_bound_per_ring) {
	BroadcastStatus status;
	status.per_ring.resize(num_brokers_, PerRingStatus::SUCCESS);
	for (uint32_t b = 0; b < num_brokers_; ++b) {
		const PerRingStatus s = down_[b].TryProduceBounded(record, len, spin_bound_per_ring);
		status.per_ring[b] = s;
		if (s == PerRingStatus::WEDGED) {
			++status.wedged_count;
		} else if (s == PerRingStatus::FULL) {
			++status.failed_count;
		}
	}
	return status;
}

BroadcastStatus MailboxSegment::BroadcastDown(const void* record, uint32_t len) {
	// One decoupled, BOUNDED pass: every down ring is attempted independently with the
	// stored per-ring spin bound, so a slow/full/dead broker never blocks delivery to the
	// healthy ones and the call ALWAYS returns (P1-3 — no infinite retry on a wedged ring).
	//
	// A FULL result means len > record_size (a caller error that can never clear) — fail
	// loudly. A WEDGED result means the ring stayed full past the spin bound: the record was
	// NOT delivered to that broker. This is best-effort by design; the caller owns the policy
	// for a wedged broker (retry next epoch, or mark it failed via the lease/failure-detector
	// layer), exactly as a gRPC stream write that errors would be handled — the transport does
	// not have, and must not fake, a failure detector.
	BroadcastStatus status = BroadcastDownNonBlocking(record, len, broadcast_spin_bound_);
	CHECK_EQ(status.failed_count, 0u)
	    << "BroadcastDown: record length " << len << " exceeds down-ring record_size";
	return status;
}

MailboxSegment::~MailboxSegment() {
	if (owns_mapping_ && base_ != nullptr) {
		munmap(base_, bytes_);
	}
	if (shm_fd_ >= 0) {
		close(shm_fd_);
	}
	if (!shm_name_.empty()) {
		shm_unlink(shm_name_.c_str());  // only the creator unlinks
	}
}

}  // namespace cxl_transport
}  // namespace Embarcadero
