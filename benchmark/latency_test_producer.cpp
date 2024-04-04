#include <iostream>
#include <string>
#include <librdkafka/rdkafkacpp.h>
#include <chrono>
#include <fstream>
#include <unordered_map> 
#include <yaml-cpp/yaml.h>

class KafkaProducer {
    std::string errstr;
    std::string brokers;
    std::string topic_name;
    RdKafka::Producer *producer;
    RdKafka::Topic *topic;

public:
    KafkaProducer(const std::string& brokers, const std::string& topic_name, const std::string& ack)
        : brokers(brokers), topic_name(topic_name) {
        RdKafka::Conf *conf = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);

        if (conf->set("bootstrap.servers", brokers, errstr) != RdKafka::Conf::CONF_OK) {
            std::cerr << "% " << errstr << std::endl;
            exit(1);
        }

        if (conf->set("acks", ack, errstr) != RdKafka::Conf::CONF_OK) {
            std::cerr << "% " << errstr << std::endl;
            exit(1);
        }

        // Might need these 2 configs to optimize for latency
        // if (conf->set("linger.ms", "0", errstr) != RdKafka::Conf::CONF_OK) {
        //     std::cerr << "% " << errstr << std::endl;
        //     exit(1);
        // }

        // if (conf->set("batch.size", "1000000", errstr) != RdKafka::Conf::CONF_OK) {
        //     std::cerr << "% " << errstr << std::endl;
        //     exit(1);
        // }

        producer = RdKafka::Producer::create(conf, errstr);
        if (!producer) {
            std::cerr << "Failed to create producer: " << errstr << std::endl;
            exit(1);
        }

        // topic = RdKafka::Topic::create(producer, topic_name, nullptr, errstr);
        // if (!topic) {
        //     std::cerr << "Failed to create topic: " << errstr << std::endl;
        //     exit(1);
        // }

        printf("Created producer %s\n", producer->name().c_str());
    }

    ~KafkaProducer() {
        delete topic;
        delete producer;
    }

    RdKafka::ErrorCode produce(const std::string& message) {
        RdKafka::ErrorCode err = producer->produce(
                                        topic_name,
                                        RdKafka::Topic::PARTITION_UA,
                                        RdKafka::Producer::RK_MSG_COPY,
                                        const_cast<char *>(message.c_str()), message.size(),
                                        /* Key */
                                        NULL, 0,
                                        /* Timestamp (defaults to current time) */
                                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(),
                                        /* Message headers, if any */
                                        NULL,
                                        /* Per-message opaque value passed to
                                        * delivery report */
                                        NULL);
        return err;
    }

    void flush(int timeout_ms) {
        producer->flush(timeout_ms);
    }

    int outq_len() {
        return producer->outq_len();
    }

    void poll(int timeout_ms) {
        producer->poll(timeout_ms);
    }
};

int main() {
    std::ifstream file("config/producer.yaml");
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
    std::string ack = config["ack"].as<std::string>();
    std::string payload = config["payload"].as<std::string>();

    std::vector<int> byte_sizes = {num_bytes};

    // load payload from file
    std::ifstream file(payload);    
    std::string payload((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    KafkaProducer kp(brokers, topic_name, ack);
    // for each byte size, produce 10000 messages    
    for (auto num_bytes : byte_sizes) {

        /* For constant payload */
        // start time for throughput
        // std::string payload;
        // payload.reserve(num_bytes);

        // // Generate random characters
        // for (int i = 0; i < num_bytes; ++i) {
        //     payload += alphanum[rand() % (sizeof(alphanum) - 1)];
        // }

        auto start = std::chrono::system_clock::now();
        for (int i = 0; i < num_messages; ++i) {

            /* For random payload */
            // start time for throughput
            // std::string payload;
            // payload.reserve(num_bytes);

            // // Generate random characters
            // for (int i = 0; i < num_bytes; ++i) {
            //     payload += alphanum[rand() % (sizeof(alphanum) - 1)];
            // }
            
            RdKafka::ErrorCode err = kp.produce(payload);
            if (err != RdKafka::ERR_NO_ERROR) {
                std::cerr << "Failed to produce message: " << RdKafka::err2str(err) << std::endl;
            }

            kp.poll(0);
        }

        std::cerr << "% Flushing final messages..." << std::endl;
        kp.flush(10 * 100000 /* wait for max 1000 seconds */);

        if (kp.outq_len() > 0)
            std::cerr << "% " << kp.outq_len()
                    << " message(s) were not delivered" << std::endl;

        // end time for throughput
        auto end = std::chrono::system_clock::now();
        std::chrono::duration<double> elapsed_seconds = end - start;
        
        // calculate throughput
        double throughput = (static_cast<double>(num_bytes) * num_messages) / elapsed_seconds.count();
        throughput /= 1024 * 1024;

        std::cerr << "Throughput for " << num_bytes << " bytes: " << throughput << " MB/s" << std::endl;
    }

    return 0;
}
