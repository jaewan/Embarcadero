cmake_minimum_required(VERSION 3.16)

# Add micro benchmarks subdirectory (existing)
add_subdirectory(micro)

# Add KV store benchmark subdirectory (new)
add_subdirectory(kv_store)
