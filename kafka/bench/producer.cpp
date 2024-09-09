#include <chrono>
#include <iostream>
#include <librdkafka/rdkafkacpp.h>
#include <string>
#include <unistd.h>
#include <vector>
#include <fstream>
#include <yaml-cpp/yaml.h>
#include <thread>

#include "benchmark.hpp"

#define POLLING_THREAD 4

static Pipe pp;
static long payload_count = 0, produced = 0;
static volatile long delivered = 0;
static std::chrono::time_point<std::chrono::steady_clock> start;
static long *latency = NULL;
static bool measure_latency = false;

class DeliveryReportCb : public RdKafka::DeliveryReportCb {
public:
    void dr_cb(RdKafka::Message &message) override {
        if (message.err())
            std::cerr << "Message delivery failed: " << message.errstr() << std::endl;
        else {
            long idx = __sync_fetch_and_add(&delivered, 1);
            latency[idx] = timestamp_now() - get_timestamp(message.payload());
            if (delivered == payload_count) {
                struct kafka_benchmark_throughput_report report;
                report.start = start;
                report.end = std::chrono::steady_clock::now();

                long latency_sum = 0;
                if (measure_latency) {
                    // (junbong): Ensure other threads finish store latency
                    // sleep(1);
                    std::ofstream outFile("producer_latency.csv");
                    if (!outFile) {
                        std::cerr << "Failed to open file" << std::endl;
                        return;
                    }
                    for (size_t i = 0; i < payload_count; i++) {
                        latency_sum += latency[i];
                        outFile << latency[i];
                        if (i != payload_count-1) {
                            outFile << ',';
                        }
                    }
                    outFile.close();
                }

                report.latency = std::chrono::microseconds(latency_sum / payload_count);
                write(pp.second, &report, sizeof(report));
            }
        }
    }
};
DeliveryReportCb dr_cb;

RdKafka::Producer *create_producer(const std::string& brokers, const std::string& ack) {
    std::string errstr;
    auto conf = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);

    if (conf->set("bootstrap.servers", brokers, errstr) != RdKafka::Conf::CONF_OK) {
        std::cerr << "% " << errstr << std::endl;
        exit(1);
    }

    if (conf->set("acks", ack, errstr) != RdKafka::Conf::CONF_OK) {
        std::cerr << "% " << errstr << std::endl;
        exit(1);
    }

    // if (conf->set("linger.ms", "0", errstr) != RdKafka::Conf::CONF_OK) {
    //     std::cerr << "% " << errstr << std::endl;
    //     exit(1);
    // }

    // if (conf->set("batch.size", "1", errstr) != RdKafka::Conf::CONF_OK) {
    //     std::cerr << "% " << errstr << std::endl;
    //     exit(1);
    // }

    if (conf->set("message.max.bytes", "2097152", errstr) != RdKafka::Conf::CONF_OK) {
        std::cerr << "% " << errstr << std::endl;
        exit(1);
    }

    // default: 100000
    // if (conf->set("queue.buffering.max.messages", "1", errstr) != RdKafka::Conf::CONF_OK) {
    //     std::cerr << "% " << errstr << std::endl;
    //     exit(1);
    // }

    // Create the delivery report callback
    if (conf->set("dr_cb", &dr_cb, errstr) != RdKafka::Conf::CONF_OK) {
        std::cerr << "% " << errstr << std::endl;
        exit(1);
    }

    auto producer = RdKafka::Producer::create(conf, errstr);
    if (!producer) {
        std::cerr << "Failed to create producer: " << errstr << std::endl;
        exit(1);
    }
    return producer;
}

void ppoll(RdKafka::Producer *producer) {
    while (delivered < payload_count) {
        producer->poll(10);
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <rpipe> <wpipe>" << std::endl;
        return 1;
    }
    pp = std::make_pair(atoi(argv[1]), atoi(argv[2]));
    
    auto bench_config = YAML::LoadFile("./config.yaml");
    auto producer = create_producer(
        bench_config["bootstrap"].as<std::string>(),
        bench_config["ack"].as<std::string>()
    );

    while (true) {
        struct kafka_benchmark_spec spec;
        read(pp.first, &spec, sizeof(spec));
        auto payload_msg_size = spec.payload_msg_size;
        payload_count = spec.payload_count;
        produced = delivered = 0;
        void *buf;
        switch (spec.type) {
            case B_SINGLE:
                measure_latency = true;
                [[fallthrough]];
            case B_END2END:
                {
                std::vector<std::thread> poll_threads;
                buf = malloc(payload_msg_size);
                latency = (long *)calloc(payload_count, sizeof(long));
                memset(buf, 0xAB, payload_msg_size);
                for (auto i = 0; i < POLLING_THREAD; i++)
                    poll_threads.emplace_back(ppoll, producer);
                start = std::chrono::steady_clock::now();
                // std::cout << "first produce" << std::endl;
                while (produced < payload_count) {
                    set_timestamp(buf);
                    auto err = producer->produce(
                        bench_config["topic"]["name"].as<std::string>(),
                        RdKafka::Topic::PARTITION_UA,
                        RdKafka::Producer::RK_MSG_COPY,
                        buf,
                        payload_msg_size,
                        NULL, 0,
                        0,
                        NULL
                    );
                    if (err == RdKafka::ERR__QUEUE_FULL)
                        producer->poll(1);
                    else if (err != RdKafka::ERR_NO_ERROR)
                        std::cerr << "Failed to produce message: " << RdKafka::err2str(err) << std::endl;
                    else
                        produced++;
                };
                // std::cout << "last produce" << std::endl;
                producer->flush(10 * 1000);
                for (auto &t : poll_threads) {
                    if (t.joinable()) {
                        t.join();
                    }
                }
                free(buf);
                free(latency);
                buf = latency = NULL;
                break;
                }
            case B_END:
                return 0;
        }
    }
}
