CXX := g++
AR := ar

CXXFLAGS := -std=c++20 -O2 -Wall
ARFLAGS := rcs

LIB := libmycoroutine.a

PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)
SOURCE_DIR := $(PROJECT_ROOT)/src/network/my_coroutine/my_coroutine
CPPFLAGS += -I$(PROJECT_ROOT)/include/network/my_coroutine/my_coroutine \
			-I$(PROJECT_ROOT)/include/logger \
			-I$(PROJECT_ROOT)/include/memory \
			-I$(PROJECT_ROOT)/include/timer
SRCS := $(wildcard $(SOURCE_DIR)/*.cpp)
OBJS := $(notdir $(SRCS:.cpp=.o))

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) $(ARFLAGS) $@ $^

%.o: $(SOURCE_DIR)/%.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(LIB)

.PHONY: all clean