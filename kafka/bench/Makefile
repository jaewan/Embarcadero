# Makefile
CXX = g++
CXXFLAGS = -std=c++11 -g # -O2
LDFLAGS = -lrdkafka++ -lrdkafka -lyaml-cpp

all: producer consumer driver

producer: producer.cpp
	$(CXX) $(CXXFLAGS) -o producer producer.cpp $(LDFLAGS)

consumer: consumer.cpp
	$(CXX) $(CXXFLAGS) -o consumer consumer.cpp $(LDFLAGS)

driver: driver.cpp
	$(CXX) $(CXXFLAGS) -o driver driver.cpp $(LDFLAGS)

clean:
	rm -f producer consumer driver *.csv *.png
