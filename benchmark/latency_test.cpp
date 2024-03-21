#include <librdkafka/rdkafka.h>
#include <iostream>
#include <string>
#include <string.h>

class KafkaProducer {
    char hostname[128];
    char errstr[512];

    public:
        rd_kafka_conf_t *conf;
        rd_kafka_topic_conf_t *topic_conf;
        rd_kafka_t *rk;
        rd_kafka_topic_t *rkt;
        const char *topic;


        KafkaProducer() {
            conf = rd_kafka_conf_new();

            if (rd_kafka_conf_set(conf, "bootstrap.servers", "0.0.0.0:9092,0.0.0.0:9092",
                                errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
                fprintf(stderr, "%% %s\n", errstr);
                exit(1);
            }

            topic_conf = rd_kafka_topic_conf_new();

            if (rd_kafka_topic_conf_set(topic_conf, "acks", "all",
                                errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
                fprintf(stderr, "%% %s\n", errstr);
                exit(1);
            }

            /* Create Kafka producer handle */
            if (!(rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf,
                                    errstr, sizeof(errstr)))) {
                fprintf(stderr, "%% Failed to create new producer: %s\n", errstr);
                exit(1);
            }

            topic = "misc-topic";
            rkt = rd_kafka_topic_new(rk, topic, topic_conf);
        }
};

class KafkaConsumer {
    char hostname[128];
    char errstr[512];

    public:
        rd_kafka_conf_t *conf;
        rd_kafka_t *rk;

        KafkaConsumer() {
            conf = rd_kafka_conf_new();

            if (rd_kafka_conf_set(conf, "bootstrap.servers", "0.0.0.0:9092,0.0.0.0:9092",
                                errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
            fprintf(stderr, "%% %s\n", errstr);
            exit(1);
            }

            /* Create Kafka consumer handle */
            rk;
            if (!(rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf,
                                errstr, sizeof(errstr)))) {
                fprintf(stderr, "%% Failed to create new consumer: %s\n", errstr);
                exit(1);
            }
        }
};

int main() {
    char payload[] = "test";
    size_t payload_len = strlen(payload);

    KafkaProducer kp;
    if (rd_kafka_produce(kp.rkt, RD_KAFKA_PARTITION_UA,
                        RD_KAFKA_MSG_F_COPY,
            payload, payload_len,
                        NULL, NULL,
    NULL) == -1) {
        fprintf(stderr, "%% Failed to produce to topic %s: %s\n",
        kp.topic, rd_kafka_err2str(rd_kafka_errno2err(errno)));
    }

    KafkaConsumer kc;
    // consume message
    

    return 0;
}