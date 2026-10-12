CXX ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra -std=c++17
RDMA_LIBS := -lrdmacm -libverbs

SOURCE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
BINS := rdma_sender rdma_recv sendfile_sender sendfile_recv

# Size of the file produced by the `testfile` target, in MiB.
FILE_SIZE_MB ?= 1024
TESTFILE ?= bigfile.dat

.PHONY: all clean testfile

all: $(BINS)

rdma_sender: $(SOURCE_DIR)/rdma_sender.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(RDMA_LIBS)

rdma_recv: $(SOURCE_DIR)/rdma_recv.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(RDMA_LIBS)

sendfile_sender: $(SOURCE_DIR)/sendfile_sender.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

sendfile_recv: $(SOURCE_DIR)/sendfile_recv.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

testfile:
	dd if=/dev/zero of=$(TESTFILE) bs=1M count=$(FILE_SIZE_MB)

clean:
	rm -f $(BINS) bigfile.rdma.out bigfile.sendfile.out