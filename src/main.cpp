#include "server.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unistd.h>

static int usage()
{
    std::fprintf(stderr, "usage: http-server [-b addr] [-p port] [-w workers] [-t timeout_s] [root]\n");
    return 2;
}

int main(int argc, char** argv)
{
    http::Config cfg;
    cfg.workers = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    int opt;
    while ((opt = getopt(argc, argv, "b:p:w:t:")) != -1) {
        switch (opt) {
        case 'b': cfg.addr = optarg; break;
        case 'p': cfg.port = std::atoi(optarg); break;
        case 'w': cfg.workers = std::atoi(optarg); break;
        case 't': cfg.timeout = std::atoi(optarg); break;
        default: return usage();
        }
    }
    if (optind < argc)
        cfg.root = argv[optind];
    if (cfg.port < 1 || cfg.port > 65535 || cfg.workers < 1 || cfg.timeout < 1)
        return usage();

    return http::run_server(cfg) < 0;
}
