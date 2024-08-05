#include <chrono>
#include <iostream>
#include <librdkafka/rdkafkacpp.h>
#include <string>
#include <unistd.h>
#include <vector>
#include <fstream>
#include <numeric>
#include <yaml-cpp/yaml.h>

#include "benchmark.hpp"


static Pipe pp;
static long message_to_produce = 0, message_to_delivered = 0;
static std::chrono::time_point<std::chrono::system_clock> start;
static std::vector<long long> latency;

class DeliveryReportCb : public RdKafka::DeliveryReportCb {
public:
    void dr_cb(RdKafka::Message &message) override {
        if (message.err()) {
            std::cerr << "Message delivery failed: " << message.errstr() << std::endl;
            message_to_produce++;
        } else {
            latency.push_back(timestamp_now() - get_timestamp(message.payload()));
            if (--message_to_delivered == 0) {
                struct kafka_benchmark_throughput_report report;
                report.start = start;
                report.end = std::chrono::system_clock::now();
                report.latency = std::chrono::nanoseconds(
                    std::accumulate(latency.begin(), latency.end(), 0LL) / latency.size()
                );
                write(pp.second, &report, sizeof(report));

                std::ofstream outFile("producer_latency.csv");
                if (!outFile) {
                    std::cerr << "Failed to open file" << std::endl;
                    return;
                }
                for (size_t i = 0; i < latency.size(); ++i) {
                    outFile << latency[i];
                    if (i != latency.size()-1) {
                        outFile << ',';
                    }
                }
                outFile.close();
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

    if (conf->set("linger.ms", "0", errstr) != RdKafka::Conf::CONF_OK) {
        std::cerr << "% " << errstr << std::endl;
        exit(1);
    }

    if (conf->set("batch.size", "1", errstr) != RdKafka::Conf::CONF_OK) {
        std::cerr << "% " << errstr << std::endl;
        exit(1);
    }
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
        message_to_produce = message_to_delivered = spec.payload_count;
        auto payload_msg_size = spec.payload_msg_size;
        void *buf;
        switch (spec.type) {
            case B_BEGIN:
                buf = malloc(payload_msg_size);
                memset(buf, 0xAB, payload_msg_size);
                latency.clear();
                start = std::chrono::system_clock::now();
                while (message_to_delivered > 0) {
                    set_timestamp(buf);
                    if (message_to_produce > 0) {
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
                        if (err != RdKafka::ERR_NO_ERROR)
                            std::cerr << "Failed to produce message: " << RdKafka::err2str(err) << std::endl;
                        else
                            message_to_produce--;
                    }
                    producer->poll(1);
                };
                producer->flush(10 * 100);
                producer->poll(1);
                free(buf);
                break;
            case B_END:
                return 0;
        }
    }
}