# Total Order Sequencer - Epoch-Based Implementation
# Makefile for building the sequencer benchmark

CXX = g++
CXXFLAGS = -O3 -std=c++17 -pthread -march=native -Wall -Wextra
TARGET = sequencer
SOURCE = sequencer.cpp

# Default target
all: $(TARGET)

# Build the sequencer
$(TARGET): $(SOURCE)
	$(CXX) $(CXXFLAGS) $(SOURCE) -o $(TARGET)

# Build baseline comparison
baseline_comparison: baseline_comparison.cpp
	$(CXX) $(CXXFLAGS) baseline_comparison.cpp -o baseline_comparison

# Debug build with symbols and sanitizers
debug: CXXFLAGS = -g -O0 -std=c++17 -pthread -fsanitize=address -fsanitize=thread -Wall -Wextra
debug: $(TARGET)

# Clean build artifacts
clean:
	rm -f $(TARGET) baseline_comparison

# Run the benchmark
run: $(TARGET)
	./$(TARGET)

# Run with specific number of queues (usage: make test QUEUES=4)
test: $(TARGET)
	@echo "Running sequencer with $(QUEUES) queues..."
	./$(TARGET) $(QUEUES)

# Help target
help:
	@echo "Available targets:"
	@echo "  all                - Build the sequencer (default)"
	@echo "  baseline_comparison - Build baseline comparison benchmark"
	@echo "  debug              - Build with debug symbols and sanitizers"
	@echo "  clean              - Remove build artifacts"
	@echo "  run                - Build and run the benchmark"
	@echo "  test               - Run with specific queue count (use QUEUES=N)"
	@echo "  help               - Show this help message"

.PHONY: all debug clean run test help
