CXX      ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 -Wall -Wextra -Isrc
LDFLAGS  += -pthread

# Sanitizer build: make clean && make test SAN=address,undefined  (or SAN=thread)
ifdef SAN
CXXFLAGS += -g -O1 -fno-omit-frame-pointer -fsanitize=$(SAN)
LDFLAGS  += -fsanitize=$(SAN)
endif

SRC = src/main.cpp src/server.cpp src/parse.cpp

http-server: $(SRC) src/server.hpp src/parse.hpp
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LDFLAGS)

tests/test_parse: tests/test_parse.cpp src/parse.cpp src/parse.hpp
	$(CXX) $(CXXFLAGS) -o $@ tests/test_parse.cpp src/parse.cpp $(LDFLAGS)

test: http-server tests/test_parse
	./tests/test_parse
	python3 tests/test_server.py

clean:
	rm -f http-server tests/test_parse

.PHONY: test clean
