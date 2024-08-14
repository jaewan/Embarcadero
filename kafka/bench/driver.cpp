#include <chrono>
#include <cstring>
#include <iostream>
#include <cstdlib>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>
#include <librdkafka/rdkafka.h>

#include "benchmark.hpp"

#define CGROUP_BASE "/sys/fs/cgroup/"

std::vector<pid_t> producers, consumers;
std::vector<Pipe> producer_pipes, consumer_pipes;

std::pair<pid_t, Pipe> run_process(const std::string &netns, const std::string &cgroup, const std::string &binary) {
    int pipe_fd1[2], pipe_fd2[2];
    if (pipe(pipe_fd1) == -1 || pipe(pipe_fd2) == -1) {
        std::cerr << "pipe failed" << std::endl;
        exit(1);
    }

    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        auto fd = open((CGROUP_BASE + cgroup + "/cgroup.procs").c_str(), O_RDWR);
        auto pid_str = std::to_string(getpid());
        write(fd, pid_str.c_str(), pid_str.length() + 1);
        close(fd);

        std::string cmd = "ip netns exec " + netns + " " + binary;
        cmd += " " + std::to_string(pipe_fd2[0]);
        cmd += " " + std::to_string(pipe_fd1[1]);
        exit(system(cmd.c_str()));
    } else if (pid > 0) {
        // Parent process
        close(pipe_fd2[0]);
        close(pipe_fd1[1]);
        return std::make_pair(pid, std::make_pair(pipe_fd1[0], pipe_fd2[1]));
    } else {
        std::cerr << "fork failed" << std::endl;
        exit(1);
    }
}

void create_topic(rd_kafka_t *rk, const std::string &topic_name, int num_partitions, int replication_factor) {
    char errstr[512];
    rd_kafka_NewTopic_t *new_topic = rd_kafka_NewTopic_new(topic_name.c_str(), num_partitions, replication_factor, errstr, sizeof(errstr));
    rd_kafka_AdminOptions_t *options = rd_kafka_AdminOptions_new(rk, RD_KAFKA_ADMIN_OP_ANY);

    rd_kafka_event_t *event;
    rd_kafka_queue_t *queue = rd_kafka_queue_new(rk);

    auto err = rd_kafka_NewTopic_set_config(new_topic, "max.message.bytes", "2097152");
    if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
        std::cerr << "Failed to set max.request.size." << std::endl;
        exit(1);
    }

    rd_kafka_CreateTopics(rk, &new_topic, 1, options, queue);
    event = rd_kafka_queue_poll(queue, 10000);

    if (!event) {
        std::cerr << "Failed to create topic." << std::endl;
        exit(1);
    }

    if (rd_kafka_event_error(event) == RD_KAFKA_RESP_ERR_NO_ERROR) {
        std::cout << "Topic " << topic_name << " created successfully." << std::endl;
    } else {
        std::cerr << "Failed to create topic " << topic_name << ": " << rd_kafka_event_error_string(event) << std::endl;
        exit(1);
    }

    rd_kafka_event_destroy(event);
    rd_kafka_queue_destroy(queue);
    rd_kafka_AdminOptions_destroy(options);
    rd_kafka_NewTopic_destroy(new_topic);
}

void delete_topic(rd_kafka_t *rk, const std::string &topic_name) {
    rd_kafka_DeleteTopic_t *del_topic = rd_kafka_DeleteTopic_new(topic_name.c_str());

    char errstr[512];
    rd_kafka_AdminOptions_t *options = rd_kafka_AdminOptions_new(rk, RD_KAFKA_ADMIN_OP_ANY);

    rd_kafka_event_t *event;
    rd_kafka_queue_t *queue = rd_kafka_queue_new(rk);

    rd_kafka_DeleteTopics(rk, &del_topic, 1, options, queue);
    event = rd_kafka_queue_poll(queue, 10000);

    if (!event)
        std::cerr << "Failed to delete topic." << std::endl;

    if (rd_kafka_event_error(event) == RD_KAFKA_RESP_ERR_NO_ERROR) {
        std::cout << "Topic " << topic_name << " deleted successfully." << std::endl;
    } else {
        std::cerr << "Failed to delete topic " << topic_name << ": " << rd_kafka_event_error_string(event) << std::endl;
    }

    rd_kafka_event_destroy(event);
    rd_kafka_queue_destroy(queue);
    rd_kafka_AdminOptions_destroy(options);
    rd_kafka_DeleteTopic_destroy(del_topic);
}

int main(int argc, char **argv) {
    auto bench_config = YAML::LoadFile("./config.yaml");
    std::string entity;
    long payload_total = bench_config["payload"]["total"].as<long>();
    long payload_msg_size;
    if (argc == 1) {
        entity = bench_config["entity"].as<std::string>();
        payload_msg_size = bench_config["payload"]["msgSize"].as<long>();
    }
    else if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " $entity $msg_size" << std::endl;
        return 1;
    }
    else {
        entity = std::string(argv[1]);
        payload_msg_size = std::atoi(argv[2]);
    }

    // Create topic for benchmark
    char errstr[512];
    rd_kafka_conf_t *conf = rd_kafka_conf_new();
    rd_kafka_conf_set(conf, "bootstrap.servers", bench_config["bootstrap"].as<std::string>().c_str(), errstr, sizeof(errstr));

    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
    if (!rk) {
        std::cerr << "Failed to create Kafka producer: " << errstr << std::endl;
        return 1;
    }

    auto topic_name = bench_config["topic"]["name"].as<std::string>();
    auto num_partitions = bench_config["topic"]["partitions"].as<int>();
    auto replication_factor = bench_config["topic"]["replicationFactor"].as<int>();

    create_topic(rk, topic_name, num_partitions, replication_factor);
    rd_kafka_destroy(rk);
    sleep(10);

    // Create producers and consumers
    auto num_producers = bench_config["producer"].as<int>();
    auto num_consumers = bench_config["consumer"].as<int>();

    for (int i = 0; i < num_producers; ++i) {
        auto r = run_process("embarcadero_netns4", "embarcadero_cgroup4", "./producer");
        producers.push_back(r.first);
        producer_pipes.push_back(r.second);
    }
    for (int i = 0; i < num_consumers; ++i) {
        auto r = run_process("embarcadero_netns5", "embarcadero_cgroup5", "./consumer");
        consumers.push_back(r.first);
        consumer_pipes.push_back(r.second);
    }

    // Do benchmark
    struct kafka_benchmark_throughput_report report;
    if (entity == "single") {
        struct kafka_benchmark_spec spec = {
            .type = B_SINGLE
        };
        spec.payload_count = payload_total / payload_msg_size / num_producers;
        spec.payload_msg_size = payload_msg_size;
        for (auto pp : producer_pipes)
            write(pp.second, &spec, sizeof(spec));
        for (auto pp : producer_pipes) {
            read(pp.first, &report, sizeof(report));
            auto sec = static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(report.end - report.start).count()
            ) / 1000 / 1000;
            double throughput = (static_cast<double>(spec.payload_count * spec.payload_msg_size)) / sec;
            std::cout << "Producer: " << (throughput / (1<<20)) << "MiB/s, " << report.latency.count() << "us" << std::endl;
        }

        sleep(10);
        // Consume message
        spec.payload_count = payload_total / payload_msg_size / num_consumers;
        spec.payload_msg_size = payload_msg_size;
        for (auto pp : consumer_pipes)
            write(pp.second, &spec, sizeof(spec));
        for (auto pp : consumer_pipes) {
            read(pp.first, &report, sizeof(report));
            auto sec = static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(report.end - report.start).count()
            ) / 1000 / 1000;
            double throughput = (static_cast<double>(spec.payload_count * spec.payload_msg_size)) / sec;
            std::cout << "Consumer: " << (throughput / (1<<20)) << "MiB/s" << std::endl;
        }
    } else if (entity == "end2end") {
        struct kafka_benchmark_spec spec = {
            .type = B_END2END
        };
        // Start consumer
        spec.payload_count = payload_total / payload_msg_size / num_consumers;
        spec.payload_msg_size = payload_msg_size;
        for (auto pp : consumer_pipes)
            write(pp.second, &spec, sizeof(spec));
        sleep(10);
        // Start producer
        spec.payload_count = payload_total / payload_msg_size / num_producers;
        spec.payload_msg_size = payload_msg_size;
        for (auto pp : producer_pipes)
            write(pp.second, &spec, sizeof(spec));
        auto start = std::chrono::time_point<std::chrono::steady_clock>::max();
        auto end = std::chrono::time_point<std::chrono::steady_clock>::min();
        for (auto pp : producer_pipes) {
            read(pp.first, &report, sizeof(report));
            // Find first producer's start time
            if (report.start < start)
                start = report.start;
        }
        for (auto pp : consumer_pipes) {
            read(pp.first, &report, sizeof(report));
            // Find last consumer's end time
            if (report.end > end)
                end = report.end;
        }
        auto sec = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(report.end - report.start).count()
        ) / 1000 / 1000;
        double throughput = (static_cast<double>(spec.payload_count * spec.payload_msg_size)) / sec;
        std::cout << "End2end: " << (throughput / (1<<20)) << "MiB/s, " << report.latency.count() << "us" << std::endl;
    } else {
        std::cerr << "Unknown entity " << entity << std::endl;
    }

    // Collect producers and consumers
    struct kafka_benchmark_spec spec = {
        .type = B_END
    };
    for (auto pp : producer_pipes)
        write(pp.second, &spec, sizeof(spec));
    for (auto pp : consumer_pipes)
        write(pp.second, &spec, sizeof(spec));

    for (const auto &pid : producers) {
        int status;
        waitpid(pid, &status, 0);
        std::cout << "producer " << pid << " exit " << status << std::endl;
    }
    for (const auto &pid : consumers) {
        int status;
        waitpid(pid, &status, 0);
        std::cout << "consumer " << pid << " exit " << status << std::endl;
    }

    // Delete topic for benchmark
    // delete_topic(rk, topic_name);

    return 0;
}
