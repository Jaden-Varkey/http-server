CXX      ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 -Wall -Wextra -Isrc
CFLAGS   ?= -O2
CFLAGS   += -std=c17 -Wall -Wextra
LDFLAGS  += -pthread

# Sanitizer build: make clean && make test SAN=address,undefined  (or SAN=thread)
ifdef SAN
CXXFLAGS += -g -O1 -fno-omit-frame-pointer -fsanitize=$(SAN)
CFLAGS   += -g -O1 -fno-omit-frame-pointer -fsanitize=$(SAN)
LDFLAGS  += -fsanitize=$(SAN)
endif

SRC = src/main.cpp src/server.cpp src/parse.cpp

http-server: $(SRC) src/sys.o src/server.hpp src/parse.hpp src/sys.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) src/sys.o $(LDFLAGS)

src/sys.o: src/sys.c src/sys.h
	$(CC) $(CFLAGS) -c -o $@ src/sys.c

tests/test_parse: tests/test_parse.cpp src/parse.cpp src/parse.hpp
	$(CXX) $(CXXFLAGS) -o $@ tests/test_parse.cpp src/parse.cpp $(LDFLAGS)

test: http-server tests/test_parse
	./tests/test_parse
	python3 tests/test_server.py

clean:
	rm -f http-server tests/test_parse src/sys.o

.PHONY: test clean
