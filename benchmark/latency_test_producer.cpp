#include <iostream>
#include <string>
#include <librdkafka/rdkafkacpp.h>

class KafkaProducer {
    std::string errstr;
    std::string brokers;
    std::string topic_name;
    RdKafka::Producer *producer;
    RdKafka::Topic *topic;

public:
    KafkaProducer(const std::string& brokers, const std::string& topic_name)
        : brokers(brokers), topic_name(topic_name) {
        RdKafka::Conf *conf = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);

        if (conf->set("bootstrap.servers", brokers, errstr) != RdKafka::Conf::CONF_OK) {
            std::cerr << "% " << errstr << std::endl;
            exit(1);
        }

        if (conf->set("acks", "0", errstr) != RdKafka::Conf::CONF_OK) {
            std::cerr << "% " << errstr << std::endl;
            exit(1);
        }

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
                                        0,
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
    std::string brokers = "0.0.0.0:9092,0.0.0.0:9093";
    std::string topic_name = "misc";

    std::string payload = "Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
                          "Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. "
                          "Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat. "
                          "Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. "
                          "Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est laborum.";

    KafkaProducer kp(brokers, topic_name);
    for (int i = 0; i < 10000; ++i) {
        RdKafka::ErrorCode err = kp.produce(payload);
        if (err != RdKafka::ERR_NO_ERROR) {
            std::cerr << "Failed to produce message: " << RdKafka::err2str(err) << std::endl;
        }

        kp.poll(0);
    }

    std::cerr << "% Flushing final messages..." << std::endl;
    kp.flush(10 * 1000 /* wait for max 10 seconds */);

    if (kp.outq_len() > 0)
        std::cerr << "% " << kp.outq_len()
                << " message(s) were not delivered" << std::endl;

    return 0;
}
