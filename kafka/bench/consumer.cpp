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


static std::vector<long long> latency;

RdKafka::KafkaConsumer *create_consumer(const std::string& brokers) {
    std::string errstr;
    auto conf = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);

    if (conf->set("bootstrap.servers", brokers, errstr) != RdKafka::Conf::CONF_OK) {
        std::cerr << "% " << errstr << std::endl;
        exit(1);
    }

    // set group id
    if (conf->set("group.id", "1", errstr) != RdKafka::Conf::CONF_OK) {
        std::cerr << "% " << errstr << std::endl;
        exit(1);
    }

    if (conf->set("auto.offset.reset", "earliest", errstr) != RdKafka::Conf::CONF_OK) {
        std::cerr << "% " << errstr << std::endl;
        exit(1);
    }


    /* Create Kafka consumer handle */
    auto rk = RdKafka::KafkaConsumer::create(conf, errstr);
    if (!rk) {
        std::cerr << "% Failed to create new consumer: " << errstr << std::endl;
        exit(1);
    }
    return rk;
}


int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <rpipe> <wpipe>" << std::endl;
        return 1;
    }
    Pipe p = std::make_pair(atoi(argv[1]), atoi(argv[2]));
    
    auto bench_config = YAML::LoadFile("./config.yaml");
    auto consumer = create_consumer(
        bench_config["bootstrap"].as<std::string>()
    );

    while (true) {
        struct kafka_benchmark_spec spec;
        struct kafka_benchmark_throughput_report report;
        read(p.first, &spec, sizeof(spec));
        auto payload_count = spec.payload_count;
        auto payload_msg_size = spec.payload_msg_size;
        std::chrono::time_point<std::chrono::system_clock> start, end;
        RdKafka::ErrorCode err;

        switch (spec.type) {
            case B_BEGIN:
                {
                latency.clear();
                start = std::chrono::system_clock::now();
                err = consumer->subscribe({bench_config["topic"]["name"].as<std::string>()});
                if (err) {
                    std::cerr << "Failed to subscribe to topic: " << RdKafka::err2str(err) << std::endl;
                    exit(1);
                }
                while (payload_count-- > 0) {
                    auto message = consumer->consume(1000);
                    if (message == NULL) {
                        std::cerr << "Failed to consume message" << std::endl;
                        payload_count++;
                        continue;
                    }
                    if (message->err()) {
                        std::cerr << "Failed to consume message: " << message->errstr() << std::endl;
                        payload_count++;
                    }
                    latency.push_back(timestamp_now() - get_timestamp(message->payload()));
                    delete message;
                };
                end = std::chrono::system_clock::now();
                report.start = start;
                report.end = end;
                report.latency = std::chrono::nanoseconds(
                    std::accumulate(latency.begin(), latency.end(), 0LL) / latency.size()
                );
                write(p.second, &report, sizeof(report));

                std::ofstream outFile("end2end_latency.csv");
                if (!outFile) {
                    std::cerr << "Failed to open file" << std::endl;
                    break;
                }
                for (size_t i = 0; i < latency.size(); ++i) {
                    outFile << latency[i];
                    if (i != latency.size()-1) {
                        outFile << ',';
                    }
                }
                outFile.close();
                break;
                }
            case B_END:
                return 0;
        }
    }
}
