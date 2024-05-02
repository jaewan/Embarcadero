sudo cp config/fixed_producer.yaml docker_producer/config/fixed_producer.yaml
sudo cp config/fixed_producer.yaml docker_consumer/config/fixed_producer.yaml

sudo cp /usr/lib64/libyaml-cpp.so.0.6 docker_producer/dependencies
sudo cp /usr/lib64/libyaml-cpp.so.0.6 docker_consumer/dependencies

sudo cp /usr/lib64/librdkafka++.so.1 docker_producer/dependencies
sudo cp /usr/lib64/librdkafka++.so.1 docker_consumer/dependencies

sudo cp /usr/lib64/librdkafka.so.1 docker_producer/dependencies
sudo cp /usr/lib64/librdkafka.so.1 docker_consumer/dependencies

sudo cp /usr/lib64/libssl.so.3 docker_producer/dependencies
sudo cp /usr/lib64/libssl.so.3 docker_consumer/dependencies

sudo cp /usr/lib64/libcrypto.so.3 docker_producer/dependencies
sudo cp /usr/lib64/libcrypto.so.3 docker_consumer/dependencies

sudo cp /usr/lib64/libzstd.so.1 docker_producer/dependencies
sudo cp /usr/lib64/libzstd.so.1 docker_consumer/dependencies
