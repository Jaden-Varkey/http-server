#pragma once

#include <string>

namespace http {

struct Config {
    std::string addr = "127.0.0.1";
    int port = 8080;
    int workers = 4;
    std::string root = "www";
    int timeout = 10;  // seconds a connection may sit idle or mid-request/response
};

// Serves files under cfg.root until SIGINT/SIGTERM. Each worker thread owns an
// epoll instance and its own SO_REUSEPORT listener. Returns 0 on clean
// shutdown, -1 on setup failure.
int run_server(const Config& cfg);

}  // namespace http
