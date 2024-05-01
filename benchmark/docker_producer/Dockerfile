# Use a base image
FROM base-image:latest

COPY dependencies/libyaml-cpp.so.0.7 /usr/local/lib/libyaml-cpp.so.0.7
COPY dependencies/librdkafka++.so.1 /usr/local/lib/librdkafka++.so.1
COPY dependencies/libsasl2.so.2 /usr/local/lib/libsasl2.so.2
COPY dependencies/librdkafka.so.1 /usr/local/lib/librdkafka.so.1 
ENV LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH

# Set the working directory inside the container
WORKDIR /app

# Copy the producer executable to the container
COPY producer /app/

COPY /config/fixed_producer.yaml /app/config/fixed_producer.yaml

# Define the command to run when the container starts
CMD ["./producer"]
