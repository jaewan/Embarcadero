/**
 * Buffer-only throughput benchmark. All scenarios use ONE publisher thread (matches Embarcadero).
 *
 * Real scenarios:
 *   0. Write-only: 1 publisher writing to buffer(s), 0 consumers. Vary num_buffers (1, 4, 12).
 *   1. 1 publisher, 1 consumer, 1 buffer
 *   2. 1 publisher, n consumers, n buffers (Embarcadero-like)
 *
 * Run from build/bin:
 *   ./buffer_benchmark --config ../../config/client.yaml --total_bytes 10737418240 --num_buffers 12 --scenario 2 --use_queue_buffer --trials 3
 *
 * Apples-to-apples with E2E (Publisher::Publish() overhead):
 *   --e2e_overhead     Producer loop mirrors Publish(): chrono x2, atomic order, per-msg padding, profile.
 *   --no_chrono        E2E mode: skip two chrono::now() per message.
 *   --no_atomic        E2E mode: use loop index instead of fetch_add.
 *   --padding_once     E2E mode: precompute padding once.
 *   --no_profile       E2E mode: skip profile recording.
 *   --e2e_sweep        Run 6 configs (Direct, E2E baseline, then one overhead removed at a time) and report table.
 *
 * Why benchmark (~10 GB/s) vs Embarcadero profile (~7 GB/s)?
 *   This benchmark measures only the QueueBuffer pipeline: 1 producer → N queues → N consumers, in-process,
 *   no network, no broker, no CXL, no ACK. Embarcadero adds: TCP send/recv, kernel buffers, broker recv(),
 *   GetCXLBuffer, CXL writes, ACK path, and EAGAIN backoff. The slowest of those limits end-to-end throughput,
 *   so full-stack is lower (~7 GB/s) than buffer-only (~10 GB/s).
 */
#include "../client/buffer.h"
#include "../client/queue_buffer.h"
#include "../client/publisher_profile.h"
#include "../common/configuration.h"
#include "../cxl_manager/cxl_datastructure.h"
#include "../cxl_manager/cxl_datastructure.h"
#include "../common/performance_utils.h"
#include <glog/logging.h>
#include <cxxopts.hpp>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <string>
#include <algorithm>
#include <functional>

static constexpr size_t kDefaultTotalBytes = 1024UL * 1024 * 1024;  // 1 GB
static constexpr size_t kDefaultMessageSize = 1024;  // 1KB - matches Embarcadero publisher
static constexpr size_t kDefaultNumBuffers = 12;
static constexpr int kOrder = 5;  // Match Embarcadero batch path (ORDER=5)
static constexpr int kDefaultTrials = 3;

// Padded size for message (header + payload, 64B aligned). Match Buffer logic.
static size_t PaddedSize(size_t message_size) {
	size_t padding = message_size % 64;
	if (padding) padding = 64 - padding;
	return message_size + padding + sizeof(Embarcadero::MessageHeader);
}

// Scenario 0: Write-only. 1 publisher, 0 consumers, num_buffers buffers. Returns write MB/s.
static double RunWriteOnly(size_t num_buffers, size_t total_bytes, size_t message_size, size_t padded_size) {
	const size_t num_messages = total_bytes / message_size;
	Buffer buf(num_buffers, num_buffers, 0, message_size, kOrder);
	if (!buf.AddBuffers(0)) return 0.0;
	buf.WarmupBuffers();

	std::vector<char> msg(message_size, 'x');
	auto t0 = std::chrono::steady_clock::now();
	for (size_t i = 0; i < num_messages; i++) {
		if (!buf.Write(static_cast<size_t>(i), msg.data(), message_size, padded_size)) {
			LOG(ERROR) << "Write failed at message " << i;
			return 0.0;
		}
	}
	buf.SealAll();
	buf.WriteFinished();
	auto t1 = std::chrono::steady_clock::now();
	double sec = std::chrono::duration<double>(t1 - t0).count();
	size_t bytes = num_messages * padded_size;  // bytes written through buffer
	return (bytes / (1024.0 * 1024.0)) / sec;
}

// Scenario 1: 1 publisher, 1 consumer, 1 buffer. Returns pipeline MB/s (payload + header through buffer).
static double Run1Producer1Consumer1Buffer(Buffer& buf, size_t total_bytes, size_t message_size, size_t padded_size) {
	const size_t num_messages = total_bytes / message_size;
	std::atomic<bool> producer_done{false};
	std::atomic<size_t> consumer_bytes{0};

	std::thread producer([&]() {
		std::vector<char> msg(message_size, 'x');
		for (size_t i = 0; i < num_messages; i++) {
			if (!buf.Write(static_cast<size_t>(i), msg.data(), message_size, padded_size)) {
				LOG(ERROR) << "Write failed at message " << i;
				break;
			}
		}
		buf.SealAll();
		buf.WriteFinished();
		producer_done.store(true, std::memory_order_release);
	});

	std::thread consumer([&]() {
		size_t total = 0;
		while (true) {
			Embarcadero::BatchHeader* batch = static_cast<Embarcadero::BatchHeader*>(buf.Read(0));
			if (batch && batch->total_size != 0 && batch->num_msg != 0) {
				total += batch->total_size;
			} else if (producer_done.load(std::memory_order_acquire)) {
				break;
			} else {
				Embarcadero::CXL::cpu_pause();
			}
		}
		consumer_bytes.store(total, std::memory_order_release);
	});

	auto t0 = std::chrono::steady_clock::now();
	producer.join();
	consumer.join();
	buf.ReturnReads();
	auto t1 = std::chrono::steady_clock::now();
	double sec = std::chrono::duration<double>(t1 - t0).count();
	size_t bytes = consumer_bytes.load(std::memory_order_acquire);
	return (bytes / (1024.0 * 1024.0)) / sec;
}

// Scenario 2: 1 publisher, n consumers, n buffers (Embarcadero-like). Returns pipeline MB/s.
static double Run1PublisherNConsumersNBuffers(Buffer& buf, size_t total_bytes, size_t message_size, size_t padded_size, size_t num_buffers) {
	const size_t num_messages = total_bytes / message_size;
	std::atomic<bool> producer_done{false};
	std::atomic<size_t> total_consumer_bytes{0};

	std::thread producer([&]() {
		std::vector<char> msg(message_size, 'x');
		for (size_t i = 0; i < num_messages; i++) {
			if (!buf.Write(static_cast<size_t>(i), msg.data(), message_size, padded_size)) {
				LOG(ERROR) << "Write failed at message " << i;
				break;
			}
		}
		buf.SealAll();
		buf.WriteFinished();
		producer_done.store(true, std::memory_order_release);
	});

	std::vector<std::thread> consumers;
	consumers.reserve(num_buffers);
	for (size_t idx = 0; idx < num_buffers; idx++) {
		consumers.emplace_back([&, idx]() {
			size_t total = 0;
			while (true) {
				Embarcadero::BatchHeader* batch = static_cast<Embarcadero::BatchHeader*>(buf.Read(static_cast<int>(idx)));
				if (batch && batch->total_size != 0 && batch->num_msg != 0) {
					total += batch->total_size;
				} else if (producer_done.load(std::memory_order_acquire)) {
					break;
				} else {
					Embarcadero::CXL::cpu_pause();
				}
			}
			total_consumer_bytes.fetch_add(total, std::memory_order_relaxed);
		});
	}

	auto t0 = std::chrono::steady_clock::now();
	producer.join();
	for (auto& t : consumers) t.join();
	buf.ReturnReads();
	auto t1 = std::chrono::steady_clock::now();
	double sec = std::chrono::duration<double>(t1 - t0).count();
	size_t bytes = total_consumer_bytes.load(std::memory_order_acquire);
	return (bytes / (1024.0 * 1024.0)) / sec;
}

// --- QueueBuffer (queue_buffer.cc) variants: same scenarios + ReleaseBatch after consume ---

static double RunWriteOnlyNew(size_t num_buffers, size_t total_bytes, size_t message_size, size_t padded_size) {
	const size_t num_messages = total_bytes / message_size;
	QueueBuffer buf(num_buffers, num_buffers, 0, message_size, kOrder);
	if (!buf.AddBuffers(0)) return 0.0;
	buf.WarmupBuffers();

	std::vector<char> msg(message_size, 'x');
	auto t0 = std::chrono::steady_clock::now();
	for (size_t i = 0; i < num_messages; i++) {
		auto [ok, sealed] = buf.Write(static_cast<size_t>(i), msg.data(), message_size, padded_size);
		if (!ok) {
			LOG(ERROR) << "QueueBuffer Write failed at message " << i;
			return 0.0;
		}
	}
	buf.SealAll();
	buf.WriteFinished();
	auto t1 = std::chrono::steady_clock::now();
	double sec = std::chrono::duration<double>(t1 - t0).count();
	size_t bytes = num_messages * padded_size;
	return (bytes / (1024.0 * 1024.0)) / sec;
}

static double Run1Producer1Consumer1BufferNew(QueueBuffer& buf, size_t total_bytes, size_t message_size, size_t padded_size) {
	const size_t num_messages = total_bytes / message_size;
	std::atomic<bool> producer_done{false};
	std::atomic<size_t> consumer_bytes{0};

	std::thread producer([&]() {
		std::vector<char> msg(message_size, 'x');
		for (size_t i = 0; i < num_messages; i++) {
			auto [ok, sealed] = buf.Write(static_cast<size_t>(i), msg.data(), message_size, padded_size);
			if (!ok) {
				LOG(ERROR) << "QueueBuffer Write failed at message " << i;
				break;
			}
		}
		buf.SealAll();
		buf.WriteFinished();
		producer_done.store(true, std::memory_order_release);
	});

	std::thread consumer([&]() {
		size_t total = 0;
		while (true) {
			Embarcadero::BatchHeader* batch = static_cast<Embarcadero::BatchHeader*>(buf.Read(0));
			if (batch && batch->total_size != 0) {
				total += batch->total_size;
				buf.ReleaseBatch(batch);
				continue;
			}
			if (producer_done.load(std::memory_order_acquire)) {
				// Drain all remaining batches before exiting.
				while ((batch = static_cast<Embarcadero::BatchHeader*>(buf.Read(0))) != nullptr &&
				       batch->total_size != 0) {
					total += batch->total_size;
					buf.ReleaseBatch(batch);
				}
				break;
			}
			std::this_thread::yield();
		}
		consumer_bytes.store(total, std::memory_order_release);
	});

	auto t0 = std::chrono::steady_clock::now();
	producer.join();
	consumer.join();
	buf.ReturnReads();
	auto t1 = std::chrono::steady_clock::now();
	double sec = std::chrono::duration<double>(t1 - t0).count();
	size_t bytes = consumer_bytes.load(std::memory_order_acquire);
	return (bytes / (1024.0 * 1024.0)) / sec;
}

static double Run1PublisherNConsumersNBuffersNew(QueueBuffer& buf, size_t total_bytes, size_t message_size, size_t padded_size, size_t num_buffers) {
	const size_t num_messages = total_bytes / message_size;
	std::atomic<bool> producer_done{false};
	std::atomic<size_t> total_consumer_bytes{0};
	// First publish to last consume: bandwidth = total_bytes / (last_consume_ns - first_write_ns).
	std::atomic<uint64_t> first_write_ns{0};
	std::atomic<uint64_t> last_consume_ns{0};
	static constexpr int kConsumerSpinCount = 128;  // Spin before yield when Read returns nullptr

	std::thread producer([&]() {
		std::vector<char> msg(message_size, 'x');
		for (size_t i = 0; i < num_messages; i++) {
			if (i == 0) {
				uint64_t t = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count());
				uint64_t expected = 0;
				first_write_ns.compare_exchange_strong(expected, t, std::memory_order_relaxed, std::memory_order_relaxed);
			}
			auto [ok, sealed] = buf.Write(static_cast<size_t>(i), msg.data(), message_size, padded_size);
			if (!ok) {
				LOG(ERROR) << "QueueBuffer Write failed at message " << i;
				break;
			}
		}
		buf.SealAll();
		buf.WriteFinished();
		producer_done.store(true, std::memory_order_release);
	});

	std::vector<std::thread> consumers;
	consumers.reserve(num_buffers);
	for (size_t idx = 0; idx < num_buffers; idx++) {
		consumers.emplace_back([&, idx]() {
			size_t my_total = 0;
			while (true) {
				Embarcadero::BatchHeader* batch = static_cast<Embarcadero::BatchHeader*>(buf.Read(static_cast<int>(idx)));
				if (batch && batch->total_size != 0) {
					my_total += batch->total_size;
					buf.ReleaseBatch(batch);
					// Record last consume when global total first reaches total_bytes (first to cross wins).
					size_t prev = total_consumer_bytes.fetch_add(batch->total_size, std::memory_order_relaxed);
					if (prev < total_bytes && prev + batch->total_size >= total_bytes) {
						uint64_t t = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
							std::chrono::steady_clock::now().time_since_epoch()).count());
						uint64_t expected = 0;
						last_consume_ns.compare_exchange_strong(expected, t, std::memory_order_relaxed, std::memory_order_relaxed);
					}
					continue;
				}
				if (producer_done.load(std::memory_order_acquire)) {
					// Drain all remaining batches for this consumer before exiting.
					while ((batch = static_cast<Embarcadero::BatchHeader*>(buf.Read(static_cast<int>(idx)))) != nullptr &&
					       batch->total_size != 0) {
						my_total += batch->total_size;
						buf.ReleaseBatch(batch);
						size_t prev = total_consumer_bytes.fetch_add(batch->total_size, std::memory_order_relaxed);
						if (prev < total_bytes && prev + batch->total_size >= total_bytes) {
							uint64_t t = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
								std::chrono::steady_clock::now().time_since_epoch()).count());
							uint64_t expected = 0;
							last_consume_ns.compare_exchange_strong(expected, t, std::memory_order_relaxed, std::memory_order_relaxed);
						}
					}
					break;
				}
				// [[PERF]] Spin before yield to reduce context switches when producer is about to push.
				for (int s = 0; s < kConsumerSpinCount; s++) {
					Embarcadero::CXL::cpu_pause();
				}
				std::this_thread::yield();
			}
			// Ensure we didn't miss counting (drain path already fetch_add'd)
			(void)my_total;
		});
	}

	auto t0 = std::chrono::steady_clock::now();
	producer.join();
	for (auto& t : consumers) t.join();
	buf.ReturnReads();
	auto t1 = std::chrono::steady_clock::now();
	size_t bytes = total_consumer_bytes.load(std::memory_order_acquire);
	uint64_t fw = first_write_ns.load(std::memory_order_acquire);
	uint64_t lc = last_consume_ns.load(std::memory_order_acquire);
	double sec;
	if (fw != 0 && lc != 0 && lc > fw) {
		sec = static_cast<double>(lc - fw) / 1e9;
	} else {
		sec = std::chrono::duration<double>(t1 - t0).count();
	}
	return (bytes / (1024.0 * 1024.0)) / sec;
}

/**
 * E2E-like producer: same structure as Publisher::Publish() so benchmark is apples-to-apples.
 * Toggles allow testing one component at a time (chrono, atomic, padding, profile).
 */
static double Run1PublisherNConsumersNBuffersE2ELike(
	QueueBuffer& buf, size_t total_bytes, size_t message_size, size_t padded_size, size_t num_buffers,
	bool use_chrono, bool use_atomic, bool padding_once, bool use_profile) {
	const size_t num_messages = total_bytes / message_size;
	const size_t header_size = sizeof(Embarcadero::MessageHeader);
	std::atomic<bool> producer_done{false};
	std::atomic<size_t> total_consumer_bytes{0};
	std::atomic<uint64_t> first_write_ns{0};
	std::atomic<uint64_t> last_consume_ns{0};
	static constexpr int kConsumerSpinCount = 128;

	// Simulate profile recording cost when use_profile (stub is no-op; same atomic work as E2E).
	static std::atomic<uint64_t> sim_publish_total_ns{0};
	static std::atomic<uint64_t> sim_publish_total_bytes{0};

	std::thread producer([&]() {
		std::atomic<size_t> client_order{0};
		std::vector<char> msg(message_size, 'x');
		for (size_t i = 0; i < num_messages; i++) {
			auto t0 = use_chrono ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			size_t padded_total;
			if (padding_once) {
				padded_total = padded_size;
			} else {
				size_t len = message_size;
				size_t padded = len % 64;
				if (padded) padded = 64 - padded;
				padded_total = len + padded + header_size;
			}
			size_t my_order = use_atomic ? client_order.fetch_add(1, std::memory_order_acq_rel) : i;
			auto [ok, sealed] = buf.Write(my_order, msg.data(), message_size, padded_total);
			if (!ok) {
				LOG(ERROR) << "QueueBuffer Write failed at message " << i;
				break;
			}
			auto t1 = use_chrono ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			uint64_t ns = use_chrono ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) : 0;
			if (use_profile) {
				sim_publish_total_ns.fetch_add(ns, std::memory_order_relaxed);
				sim_publish_total_bytes.fetch_add(padded_total, std::memory_order_relaxed);
			}
			if (i == 0) {
				uint64_t t = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count());
				uint64_t expected = 0;
				first_write_ns.compare_exchange_strong(expected, t, std::memory_order_relaxed, std::memory_order_relaxed);
			}
		}
		buf.SealAll();
		buf.WriteFinished();
		producer_done.store(true, std::memory_order_release);
	});

	std::vector<std::thread> consumers;
	consumers.reserve(num_buffers);
	for (size_t idx = 0; idx < num_buffers; idx++) {
		consumers.emplace_back([&, idx]() {
			size_t my_total = 0;
			while (true) {
				Embarcadero::BatchHeader* batch = static_cast<Embarcadero::BatchHeader*>(buf.Read(static_cast<int>(idx)));
				if (batch && batch->total_size != 0) {
					my_total += batch->total_size;
					buf.ReleaseBatch(batch);
					size_t prev = total_consumer_bytes.fetch_add(batch->total_size, std::memory_order_relaxed);
					if (prev < total_bytes && prev + batch->total_size >= total_bytes) {
						uint64_t t = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
							std::chrono::steady_clock::now().time_since_epoch()).count());
						uint64_t expected = 0;
						last_consume_ns.compare_exchange_strong(expected, t, std::memory_order_relaxed, std::memory_order_relaxed);
					}
					continue;
				}
				if (producer_done.load(std::memory_order_acquire)) {
					while ((batch = static_cast<Embarcadero::BatchHeader*>(buf.Read(static_cast<int>(idx)))) != nullptr &&
					       batch->total_size != 0) {
						my_total += batch->total_size;
						buf.ReleaseBatch(batch);
						size_t prev = total_consumer_bytes.fetch_add(batch->total_size, std::memory_order_relaxed);
						if (prev < total_bytes && prev + batch->total_size >= total_bytes) {
							uint64_t t = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
								std::chrono::steady_clock::now().time_since_epoch()).count());
							uint64_t expected = 0;
							last_consume_ns.compare_exchange_strong(expected, t, std::memory_order_relaxed, std::memory_order_relaxed);
						}
					}
					break;
				}
				for (int s = 0; s < kConsumerSpinCount; s++) Embarcadero::CXL::cpu_pause();
				std::this_thread::yield();
			}
			(void)my_total;
		});
	}

	auto t0 = std::chrono::steady_clock::now();
	producer.join();
	for (auto& t : consumers) t.join();
	buf.ReturnReads();
	auto t1 = std::chrono::steady_clock::now();
	size_t bytes = total_consumer_bytes.load(std::memory_order_acquire);
	uint64_t fw = first_write_ns.load(std::memory_order_acquire);
	uint64_t lc = last_consume_ns.load(std::memory_order_acquire);
	double sec;
	if (fw != 0 && lc != 0 && lc > fw) {
		sec = static_cast<double>(lc - fw) / 1e9;
	} else {
		sec = std::chrono::duration<double>(t1 - t0).count();
	}
	return (bytes / (1024.0 * 1024.0)) / sec;
}

// Scenario 3: N independent Buffers, N producers, N consumers (each pair on one buffer)
static double RunNProducersNConsumersNBuffers(size_t total_bytes, size_t message_size, size_t padded_size, size_t num_buffers) {
	const size_t bytes_per_buffer = total_bytes / num_buffers;
	const size_t num_messages_per_buffer = bytes_per_buffer / message_size;

	std::vector<std::unique_ptr<Buffer>> buffers;
	std::vector<std::atomic<bool>> producer_done(num_buffers);
	for (size_t i = 0; i < num_buffers; i++) {
		producer_done[i].store(false, std::memory_order_relaxed);
		buffers.push_back(std::make_unique<Buffer>(1, 1, 0, static_cast<size_t>(message_size), kOrder));
		if (!buffers[i]->AddBuffers(0)) {
			LOG(ERROR) << "AddBuffers failed for buffer " << i;
			return 0.0;
		}
		buffers[i]->WarmupBuffers();  // Fair comparison with Scenarios 1 and 2
	}

	std::atomic<size_t> total_consumer_bytes{0};
	std::vector<std::thread> producers;
	std::vector<std::thread> consumers;
	producers.reserve(num_buffers);
	consumers.reserve(num_buffers);

	for (size_t idx = 0; idx < num_buffers; idx++) {
		Buffer* b = buffers[idx].get();
		std::atomic<bool>* done = &producer_done[idx];
		producers.emplace_back([b, done, num_messages_per_buffer, message_size, padded_size]() {
			std::vector<char> msg(message_size, 'x');
			for (size_t i = 0; i < num_messages_per_buffer; i++) {
				b->Write(static_cast<size_t>(i), msg.data(), message_size, padded_size);
			}
			b->SealAll();
			b->WriteFinished();
			done->store(true, std::memory_order_release);
		});
		consumers.emplace_back([b, done, bytes_per_buffer, &total_consumer_bytes]() {
			size_t total = 0;
			while (true) {
				Embarcadero::BatchHeader* batch = static_cast<Embarcadero::BatchHeader*>(b->Read(0));
				if (batch && batch->total_size != 0 && batch->num_msg != 0) {
					total += batch->total_size;
				} else if (done->load(std::memory_order_acquire) && total >= bytes_per_buffer) {
					break;
				} else {
					Embarcadero::CXL::cpu_pause();
				}
			}
			total_consumer_bytes.fetch_add(total, std::memory_order_relaxed);
		});
	}

	auto t0 = std::chrono::steady_clock::now();
	for (auto& t : producers) t.join();
	for (auto& t : consumers) t.join();
	for (auto& b : buffers) b->ReturnReads();
	auto t1 = std::chrono::steady_clock::now();
	double sec = std::chrono::duration<double>(t1 - t0).count();
	size_t bytes = total_consumer_bytes.load(std::memory_order_acquire);
	return (bytes / (1024.0 * 1024.0)) / sec;
}

static double Median(std::vector<double>& v) {
	if (v.empty()) return 0.0;
	std::sort(v.begin(), v.end());
	size_t n = v.size();
	return (n % 2 == 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

int main(int argc, char** argv) {
	google::InitGoogleLogging(argv[0]);
	cxxopts::Options options("buffer_benchmark", "Buffer-only throughput (no network)");
	options.add_options()
		("config", "Config file path", cxxopts::value<std::string>()->default_value("config/client.yaml"))
		("total_bytes", "Total bytes to push through buffer(s)", cxxopts::value<size_t>()->default_value(std::to_string(kDefaultTotalBytes)))
		("message_size", "Message size bytes", cxxopts::value<size_t>()->default_value(std::to_string(kDefaultMessageSize)))
		("scenario", "1=1p1c, 2=1pNc, 3=NpNc, 0=all", cxxopts::value<int>()->default_value("0"))
		("num_buffers", "Number of buffers (scenarios 2 and 3)", cxxopts::value<size_t>()->default_value(std::to_string(kDefaultNumBuffers)))
		("trials", "Number of trials per scenario (report median)", cxxopts::value<int>()->default_value(std::to_string(kDefaultTrials)))
		("use_queue_buffer", "Use QueueBuffer (queue_buffer.cc) instead of Buffer", cxxopts::value<bool>()->default_value("false"))
		("e2e_overhead", "Producer loop mirrors Publisher::Publish() (chrono, atomic, per-msg padding, profile)", cxxopts::value<bool>()->default_value("false"))
		("no_chrono", "E2E mode: skip two chrono::now() per message (use with e2e_overhead)", cxxopts::value<bool>()->default_value("false"))
		("no_atomic", "E2E mode: use loop index instead of fetch_add (use with e2e_overhead)", cxxopts::value<bool>()->default_value("false"))
		("padding_once", "E2E mode: precompute padding once (use with e2e_overhead)", cxxopts::value<bool>()->default_value("false"))
		("no_profile", "E2E mode: skip profile recording (use with e2e_overhead)", cxxopts::value<bool>()->default_value("false"))
		("e2e_sweep", "Run 6 configs: Direct, E2E baseline, E2E-no_chrono, E2E-no_atomic, E2E-padding_once, E2E-no_profile", cxxopts::value<bool>()->default_value("false"))
		("v,log_level", "Log level", cxxopts::value<int>()->default_value("0"));
	auto result = options.parse(argc, argv);

	FLAGS_v = result["log_level"].as<int>();
	size_t total_bytes = result["total_bytes"].as<size_t>();
	size_t message_size = result["message_size"].as<size_t>();
	size_t padded_size = PaddedSize(message_size);
	int scenario = result["scenario"].as<int>();
	size_t num_buffers = result["num_buffers"].as<size_t>();
	int trials = std::max(1, result["trials"].as<int>());
	bool use_queue_buffer = result["use_queue_buffer"].as<bool>();
	bool e2e_overhead = result["e2e_overhead"].as<bool>();
	bool no_chrono = result["no_chrono"].as<bool>();
	bool no_atomic = result["no_atomic"].as<bool>();
	bool padding_once = result["padding_once"].as<bool>();
	bool no_profile = result["no_profile"].as<bool>();
	bool e2e_sweep = result["e2e_sweep"].as<bool>();

	Embarcadero::Configuration& config = Embarcadero::Configuration::getInstance();
	if (!config.loadFromFile(result["config"].as<std::string>())) {
		LOG(WARNING) << "Config load failed, using defaults (buffer_size_mb from config may be wrong)";
	}

	std::cout << "Buffer benchmark (Embarcadero-aligned, 1KB messages): total_bytes=" << total_bytes
	          << " message_size=" << message_size << " padded_size=" << padded_size
	          << " scenario=" << scenario << " num_buffers=" << num_buffers << " trials=" << trials
	          << (use_queue_buffer ? " [QueueBuffer]" : " [Buffer]")
	          << (e2e_sweep ? " [E2E_SWEEP]" : (e2e_overhead ? " [E2E_overhead]" : "")) << "\n";

	auto run_and_report = [&](const std::string& name, std::function<double()> run_once) {
		std::vector<double> mb_s_list;
		mb_s_list.reserve(static_cast<size_t>(trials));
		for (int t = 0; t < trials; t++) {
			double mb_s = run_once();
			if (mb_s > 0.0) mb_s_list.push_back(mb_s);
		}
		if (mb_s_list.empty()) {
			std::cout << name << ": no valid runs\n";
			return;
		}
		double median_mb_s = Median(mb_s_list);
		double min_mb_s = *std::min_element(mb_s_list.begin(), mb_s_list.end());
		double max_mb_s = *std::max_element(mb_s_list.begin(), mb_s_list.end());
		std::cout << name << ": median " << std::fixed << std::setprecision(2) << median_mb_s << " MB/s (min=" << min_mb_s << ", max=" << max_mb_s << ", n=" << mb_s_list.size() << ")\n";
	};

	// Real scenarios: all use ONE publisher thread.
	if (scenario == 0 || scenario == 9) {
		if (use_queue_buffer) {
			if (scenario == 9) {
				for (size_t k : {1UL, 4UL, 12UL}) {
					run_and_report("Write-only [QueueBuffer] (" + std::to_string(k) + " buffer(s))", [&, k]() {
						return RunWriteOnlyNew(k, total_bytes, message_size, padded_size);
					});
				}
			} else {
				run_and_report("Write-only [QueueBuffer] (" + std::to_string(num_buffers) + " buffers)", [&]() {
					return RunWriteOnlyNew(num_buffers, total_bytes, message_size, padded_size);
				});
			}
		} else {
			if (scenario == 9) {
				for (size_t k : {1UL, 4UL, 12UL}) {
					run_and_report("Write-only (1 publisher, 0 consumers, " + std::to_string(k) + " buffer(s))", [&, k]() {
						return RunWriteOnly(k, total_bytes, message_size, padded_size);
					});
				}
			} else {
				run_and_report("Write-only (1 publisher, 0 consumers, " + std::to_string(num_buffers) + " buffers)", [&]() {
					return RunWriteOnly(num_buffers, total_bytes, message_size, padded_size);
				});
			}
		}
	}
	if (scenario == 1 || scenario == 9) {
		if (use_queue_buffer) {
			run_and_report("1 publisher, 1 consumer, 1 buffer [QueueBuffer]", [&]() {
				QueueBuffer buf(1, 1, 0, message_size, kOrder);
				if (!buf.AddBuffers(0)) return 0.0;
				buf.WarmupBuffers();
				return Run1Producer1Consumer1BufferNew(buf, total_bytes, message_size, padded_size);
			});
		} else {
			run_and_report("1 publisher, 1 consumer, 1 buffer", [&]() {
				Buffer buf(1, 1, 0, message_size, kOrder);
				if (!buf.AddBuffers(0)) return 0.0;
				buf.WarmupBuffers();
				return Run1Producer1Consumer1Buffer(buf, total_bytes, message_size, padded_size);
			});
		}
	}
	if (scenario == 2 || scenario == 9) {
		if (use_queue_buffer) {
			if (e2e_sweep) {
				// Run 6 configs: Direct, E2E baseline, then E2E with one overhead removed at a time.
				std::cout << "\n=== E2E apples-to-apples sweep (one component removed at a time) ===\n";
				auto run_one = [&](const std::string& label, bool e2e, bool chrono, bool atomic, bool pad_once, bool profile) {
					std::vector<double> mb_s_list;
					mb_s_list.reserve(static_cast<size_t>(trials));
					for (int t = 0; t < trials; t++) {
						QueueBuffer buf(num_buffers, num_buffers, 0, message_size, kOrder);
						if (!buf.AddBuffers(0)) return;
						buf.WarmupBuffers();
						double mb_s = e2e
							? Run1PublisherNConsumersNBuffersE2ELike(buf, total_bytes, message_size, padded_size, num_buffers, chrono, atomic, pad_once, profile)
							: Run1PublisherNConsumersNBuffersNew(buf, total_bytes, message_size, padded_size, num_buffers);
						if (mb_s > 0.0) mb_s_list.push_back(mb_s);
					}
					if (mb_s_list.empty()) { std::cout << "  " << label << ": no valid runs\n"; return; }
					double median_mb_s = Median(mb_s_list);
					std::cout << "  " << std::left << std::setw(28) << label << " median " << std::fixed << std::setprecision(2) << median_mb_s << " MB/s\n";
				};
				run_one("Direct Write (no E2E overhead)", false, true, true, true, true);
				run_one("E2E baseline (all overheads)", true, true, true, false, true);
				run_one("E2E --no_chrono", true, false, true, false, true);
				run_one("E2E --no_atomic", true, true, false, false, true);
				run_one("E2E --padding_once", true, true, true, true, true);
				run_one("E2E --no_profile", true, true, true, false, false);
				std::cout << "=== end sweep ===\n\n";
			} else if (e2e_overhead) {
				run_and_report("1pNc [QueueBuffer] E2E-like (chrono=" + std::to_string(!no_chrono) + " atomic=" + std::to_string(!no_atomic) + " padding_once=" + std::to_string(padding_once) + " profile=" + std::to_string(!no_profile) + ")",
					[&]() {
						QueueBuffer buf(num_buffers, num_buffers, 0, message_size, kOrder);
						if (!buf.AddBuffers(0)) return 0.0;
						buf.WarmupBuffers();
						return Run1PublisherNConsumersNBuffersE2ELike(buf, total_bytes, message_size, padded_size, num_buffers,
							!no_chrono, !no_atomic, padding_once, !no_profile);
					});
			} else {
				run_and_report("1 publisher, n consumers, n buffers [QueueBuffer] (n=" + std::to_string(num_buffers) + ")", [&]() {
					QueueBuffer buf(num_buffers, num_buffers, 0, message_size, kOrder);
					if (!buf.AddBuffers(0)) return 0.0;
					buf.WarmupBuffers();
					return Run1PublisherNConsumersNBuffersNew(buf, total_bytes, message_size, padded_size, num_buffers);
				});
			}
		} else {
			run_and_report("1 publisher, n consumers, n buffers (n=" + std::to_string(num_buffers) + ")", [&]() {
				Buffer buf(num_buffers, num_buffers, 0, message_size, kOrder);
				if (!buf.AddBuffers(0)) return 0.0;
				buf.WarmupBuffers();
				return Run1PublisherNConsumersNBuffers(buf, total_bytes, message_size, padded_size, num_buffers);
			});
		}
	}
	if (scenario == 3) {
		run_and_report("N producers, N consumers, N buffers (n=" + std::to_string(num_buffers) + ")", [&]() {
			return RunNProducersNConsumersNBuffers(total_bytes, message_size, padded_size, num_buffers);
		});
	}

	std::cout << "Scenarios 1,2,9 use ONE publisher thread. Scenario 3 uses N producers. 1KB message size matches Embarcadero.\n";
	return 0;
}
