CXX := g++
CXXFLAGS := -O2 -std=c++11 -Wall

TARGET := kv_bench
TARGET_10M := kv_bench_10m
OBJS := nvme_passthru.o

all: $(TARGET) $(TARGET_10M)

$(TARGET): kv_bench.cc $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(TARGET_10M): kv_bench_10m.cc $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

nvme_passthru.o: nvme_passthru.cc nvme_passthru.h
	$(CXX) $(CXXFLAGS) -c nvme_passthru.cc -o nvme_passthru.o

clean:
	rm -f $(TARGET) $(TARGET_10M) $(OBJS)

.PHONY: all clean

