#include <chrono>
#include <iostream>
#include <librdkafka/rdkafkacpp.h>
#include <string>
#include <unistd.h>
#include <vector>
#include <fstream>
#include <yaml-cpp/yaml.h>

#include "benchmark.hpp"


static long *latency = NULL;
static bool measure_latency = false;

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

    while (true) {
        struct kafka_benchmark_spec spec;
        read(p.first, &spec, sizeof(spec));
        long payload_count = spec.payload_count;
        long payload_msg_size = spec.payload_msg_size;
        long consumed = 0;
        std::chrono::time_point<std::chrono::steady_clock> start, end;
        RdKafka::ErrorCode err;

        switch (spec.type) {
            case B_END2END:
                measure_latency = true;
                [[fallthrough]];
            case B_SINGLE:
                {
                auto consumer = create_consumer(
                    bench_config["bootstrap"].as<std::string>()
                );
                latency = (long *)calloc(payload_count, sizeof(long));
                start = std::chrono::steady_clock::now();
                err = consumer->subscribe({bench_config["topic"]["name"].as<std::string>()});
                if (err) {
                    std::cerr << "Failed to subscribe to topic: " << RdKafka::err2str(err) << std::endl;
                    exit(1);
                }
                while (consumed < payload_count) {
                    auto message = consumer->consume(1000);
                    if (message == NULL) {
                        std::cerr << "Failed to consume message" << std::endl;
                        continue;
                    }
                    if (message->err()) {
                        if (message->err() != RdKafka::ERR__TIMED_OUT)
                            std::cerr << "Failed to consume message: " << message->errstr() << std::endl;
                    }
                    else
                        latency[consumed++] = timestamp_now() - get_timestamp(message->payload());
                    delete message;
                };
                err = consumer->close();
                if (err)
                    std::cerr << "Failed to close consumer: " << RdKafka::err2str(err) << std::endl;
                end = std::chrono::steady_clock::now();

                long latency_sum = 0;
                if (measure_latency) {
                    std::ofstream outFile("end2end_latency.csv");
                    if (!outFile) {
                        std::cerr << "Failed to open file" << std::endl;
                        break;
                    }
                    for (size_t i = 0; i < payload_count; ++i) {
                        latency_sum += latency[i];
                        outFile << latency[i];
                        if (i != payload_count-1) {
                            outFile << ',';
                        }
                    }
                    outFile.close();
                }

                struct kafka_benchmark_throughput_report report;
                report.start = start;
                report.end = end;
                report.latency = std::chrono::microseconds(latency_sum / payload_count);
                write(p.second, &report, sizeof(report));

                free(latency);
                latency = NULL;
                break;
                }
            case B_END:
                return 0;
        }
    }
}
