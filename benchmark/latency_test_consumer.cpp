#include <iostream>
#include <fstream>
#include <librdkafka/rdkafkacpp.h>
#include <ctime>
#include <chrono>

class KafkaConsumer {
public:
    std::string errstr;
    RdKafka::Conf *conf;
    RdKafka::KafkaConsumer *rk;

    KafkaConsumer() {
        conf = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);

        if (conf->set("metadata.broker.list", "0.0.0.0:9092,0.0.0.0:9093", errstr) != RdKafka::Conf::CONF_OK) {
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
    KafkaConsumer kc;
    std::string topic_name = "misc";

    RdKafka::Topic *topic = RdKafka::Topic::create(kc.rk, topic_name, nullptr, kc.errstr);

    RdKafka::ErrorCode err = kc.rk->subscribe({topic_name});
    if (err) {
        std::cerr << "% Failed to subscribe to topic: " << RdKafka::err2str(err) << std::endl;
        exit(1);
    }

    // Open the file for writing
    std::ofstream outputFile("latency.txt");
    if (!outputFile.is_open()) {
        std::cerr << "Error: Unable to open file for writing." << std::endl;
        return 1;
    }

    while (true) {
        RdKafka::Message *rkmessage = kc.rk->consume(20000);
        if (rkmessage) {
            if (rkmessage->err()) {
                std::cerr << "% Consumer error: " << rkmessage->errstr() << std::endl;
            } else {
                int64_t timestamp = rkmessage->timestamp().timestamp;

                // get current ms since epoch
                auto now = std::chrono::system_clock::now();
                int64_t millis_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

                int64_t latency = millis_since_epoch - timestamp;

                outputFile << latency << std::endl;
            }
            delete rkmessage;
        }
    }

    return 0;
}
