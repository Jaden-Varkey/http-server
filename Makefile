CXX      ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 -Wall -Wextra -Isrc
LDFLAGS  += -pthread

SRC = src/main.cpp src/server.cpp

http-server: $(SRC) src/server.hpp src/parse.hpp
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f http-server

.PHONY: clean
