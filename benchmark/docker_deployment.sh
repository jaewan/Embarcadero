sudo cp config/fixed_producer.yaml docker_producer/config/fixed_producer.yaml
sudo cp config/fixed_producer.yaml docker_consumer/config/fixed_producer.yaml

sudo cp /usr/lib/x86_64-linux-gnu/libyaml-cpp.so.0.7 docker_producer/dependencies
sudo cp /usr/lib/x86_64-linux-gnu/libyaml-cpp.so.0.7 docker_consumer/dependencies

sudo cp /usr/lib/x86_64-linux-gnu/librdkafka++.so.1 docker_producer/dependencies
sudo cp /usr/lib/x86_64-linux-gnu/librdkafka++.so.1 docker_consumer/dependencies

sudo cp /usr/lib/x86_64-linux-gnu/librdkafka.so.1 docker_producer/dependencies
sudo cp /usr/lib/x86_64-linux-gnu/librdkafka.so.1 docker_consumer/dependencies

sudo cp /usr/lib/x86_64-linux-gnu/libsasl2.so.2 docker_producer/dependencies
sudo cp /usr/lib/x86_64-linux-gnu/libsasl2.so.2 docker_consumer/dependencies
