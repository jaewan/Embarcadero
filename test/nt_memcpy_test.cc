#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "common/performance_utils.h"

namespace {

TEST(NtMemcpyTest, CopiesAlignedAndUnalignedDestinations) {
	constexpr size_t kLen = 4096;
	std::vector<uint8_t> src(kLen);
	for (size_t i = 0; i < kLen; ++i) {
		src[i] = static_cast<uint8_t>(i * 17u);
	}

	alignas(64) uint8_t dst_aligned[kLen + 64];
	std::memset(dst_aligned, 0xA5, sizeof(dst_aligned));
	Embarcadero::CXL::nt_memcpy(dst_aligned, src.data(), kLen);
	EXPECT_EQ(std::memcmp(dst_aligned, src.data(), kLen), 0);

	// Force a non-64-byte-aligned destination so the peel path runs.
	uint8_t* dst_mis = dst_aligned + 7;
	std::memset(dst_mis, 0x5A, kLen);
	Embarcadero::CXL::nt_copy(dst_mis, src.data(), kLen);
	EXPECT_EQ(std::memcmp(dst_mis, src.data(), kLen), 0);
}

TEST(NtMemcpyTest, SmallCopiesUseMemcpyPath) {
	uint8_t src[128];
	uint8_t dst[128];
	for (size_t i = 0; i < sizeof(src); ++i) {
		src[i] = static_cast<uint8_t>(i);
	}
	std::memset(dst, 0, sizeof(dst));
	Embarcadero::CXL::nt_memcpy(dst, src, sizeof(src));
	EXPECT_EQ(std::memcmp(dst, src, sizeof(src)), 0);
}

TEST(NtMemcpyTest, ExplicitFlushFlagRoundTrip) {
	const bool prev = Embarcadero::CXL::ExplicitFlushRequired();
	Embarcadero::CXL::SetExplicitFlushRequired(false);
	EXPECT_FALSE(Embarcadero::CXL::ExplicitFlushRequired());
	Embarcadero::CXL::SetExplicitFlushRequired(true);
	EXPECT_TRUE(Embarcadero::CXL::ExplicitFlushRequired());
	Embarcadero::CXL::SetExplicitFlushRequired(prev);
}

}  // namespace
