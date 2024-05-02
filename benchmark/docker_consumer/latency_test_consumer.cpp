#include <iostream>
#include <fstream>
#include <librdkafka/rdkafkacpp.h>
#include <ctime>
#include <chrono>
#include <yaml-cpp/yaml.h>

class KafkaConsumer {
public:
    std::string errstr;
    RdKafka::Conf *conf;
    RdKafka::KafkaConsumer *rk;

    KafkaConsumer(const std::string& brokers) {
        conf = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);

        if (conf->set("metadata.broker.list", brokers, errstr) != RdKafka::Conf::CONF_OK) {
            std::cerr << "% " << errstr << std::endl;
            exit(1);
        }

        // set group id
        if (conf->set("group.id", "1", errstr) != RdKafka::Conf::CONF_OK) {
            std::cerr << "% " << errstr << std::endl;
            exit(1);
        }

        /* Create Kafka consumer handle */
        rk = RdKafka::KafkaConsumer::create(conf, errstr);
        if (!rk) {
            std::cerr << "% Failed to create new consumer: " << errstr << std::endl;
            exit(1);
        }

        printf("Created consumer %s\n", rk->name().c_str());
    }
};

int main() {
    std::ifstream file("config/fixed_producer.yaml");
    if (!file.is_open()) {
        std::cerr << "Failed to open YAML file." << std::endl;
        return 1;
    }

    // Parse the YAML file
    YAML::Node config = YAML::Load(file);

    std::string brokers = config["brokers"].as<std::string>();
    std::string topic_name = config["topic"].as<std::string>();
    int num_messages = config["numMessages"].as<int>();
    int num_bytes = config["messageSize"].as<int>();

    KafkaConsumer kc(brokers);
    RdKafka::Topic *topic = RdKafka::Topic::create(kc.rk, topic_name, nullptr, kc.errstr);

    RdKafka::ErrorCode err = kc.rk->subscribe({topic_name});
    if (err) {
        std::cerr << "% Failed to subscribe to topic: " << RdKafka::err2str(err) << std::endl;
        exit(1);
    }

    // initialize empty vector for latencies
    std::vector<int64_t> latencies;
    int64_t count = 0;
    std::chrono::time_point<std::chrono::system_clock> start;
    std::chrono::time_point<std::chrono::system_clock> end;
    int64_t producer_start_time;
    while (true) {
        RdKafka::Message *rkmessage = kc.rk->consume(1000);
        if (rkmessage) {
            if (rkmessage->err() && !latencies.empty()) {
                // Open the file for writing
                std::ofstream outputFile("latency.txt");
                if (!outputFile.is_open()) {
                    std::cerr << "Error: Unable to open file for writing." << std::endl;
                    return 1;
                }

                // clear the file first
                outputFile.clear();

                for (auto latency : latencies) {
                    outputFile << latency << std::endl;
                }

                // delete content in latencies
                latencies.clear();
            } else if (rkmessage->err()) {
                //std::cerr << "% Consumer error: " << rkmessage->errstr() << std::endl;
            } else {
                int64_t timestamp = rkmessage->timestamp().timestamp;

                // get current ms since epoch
                auto now = std::chrono::system_clock::now();
                int64_t millis_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

                int64_t latency = millis_since_epoch - timestamp;
                
                latencies.push_back(latency);

                count++;
                if (count == 1) {
                    // start the time
                    start = std::chrono::system_clock::now();

                    // mark the first msg sent by producer's timestamp
                    producer_start_time = timestamp;
                }

                if (count == num_messages) {
                    end = std::chrono::system_clock::now();
                    std::chrono::duration<double> elapsed_seconds = end - start;
        
                    // calculate throughput
                    double throughput = (static_cast<double>(num_bytes) * num_messages) / elapsed_seconds.count();
                    throughput /= 1024 * 1024;

                    std::cerr << "Throughput for " << num_bytes << " bytes: " << throughput << " MB/s" << std::endl;
                    
                    // Print average latency
                    int64_t sum = 0;
                    for (auto latency : latencies) {
                        sum += latency;
                    }
                    std::cerr << "Average latency: " << sum / latencies.size() << " ms" << std::endl;

                    // Print end to end throughput
                    std::chrono::duration<double> elapsed_seconds_end_to_end = end - std::chrono::system_clock::from_time_t(producer_start_time / 1000);
                    double throughput_end_to_end = (static_cast<double>(num_bytes) * num_messages) / elapsed_seconds_end_to_end.count();
                    throughput_end_to_end /= 1024 * 1024;
                    std::cerr << "End to end throughput for " << num_bytes << " bytes: " << throughput_end_to_end << " MB/s" << std::endl;

                    count = 0;
                }
            }
            delete rkmessage;
        }
    }

    return 0;
}
