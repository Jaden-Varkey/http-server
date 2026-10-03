#pragma once

#include <string>

namespace http {

struct Config {
    std::string addr = "127.0.0.1";
    int port = 8080;
    int workers = 4;
    std::string root = "www";
    int timeout = 10;
};

int run_server(const Config& cfg);

}  // namespace http
