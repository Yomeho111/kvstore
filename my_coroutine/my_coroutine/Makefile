CXX := g++
AR := ar

CXXFLAGS := -std=c++20 -O2 -Wall
ARFLAGS := rcs

LIB := libmycoroutine.a

SRCS := $(wildcard *.cpp)
OBJS := $(SRCS:.cpp=.o)

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) $(ARFLAGS) $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(LIB)

.PHONY: all clean