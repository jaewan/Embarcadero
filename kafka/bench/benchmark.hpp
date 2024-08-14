#include <utility>
#include <chrono>

typedef std::pair<int, int> Pipe;

enum bench_type {
    B_SINGLE,
    B_END2END,
    B_END,
};

struct kafka_benchmark_spec {
    enum bench_type type;
    long payload_count;
    long payload_msg_size;
};

struct kafka_benchmark_throughput_report {
    std::chrono::time_point<std::chrono::steady_clock> start;
    std::chrono::time_point<std::chrono::steady_clock> end;
    std::chrono::microseconds latency;
};

#define timestamp_now() (std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count())
#define set_timestamp(buf) (*((long *)(buf)) = timestamp_now())
#define get_timestamp(buf) (*((long *)(buf)))
