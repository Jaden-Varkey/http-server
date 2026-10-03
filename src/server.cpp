#include "server.hpp"
#include "parse.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace http {
namespace {

constexpr size_t kInSize = 8192;
constexpr size_t kOutSize = 8192;

int g_stop_fd = -1;

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Fd& operator=(Fd&& o) noexcept
    {
        if (this != &o) {
            reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    ~Fd() { reset(); }

    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }
    void reset()
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
};

struct Conn {
    Fd fd;
    Fd file;
    bool writing = false;
    bool close_after = false;
    off_t file_off = 0, file_left = 0;
    size_t index = 0;
    size_t in_len = 0, out_off = 0, out_len = 0;
    time_t deadline = 0;
    std::array<char, kInSize> in;
    std::array<char, kOutSize> out;
};

struct Shared {
    Fd root;
    int timeout;
};

struct Stats {
    unsigned long accepted = 0, requests = 0, timeouts = 0;
};

int open_beneath(int root, const char* path)
{
    open_how how{};
    how.flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
    return static_cast<int>(syscall(SYS_openat2, root, path, &how, sizeof how));
}

Fd listen_on(const std::string& addr, int port)
{
    int one = 1;
    Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    if (fd && inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) == 1 &&
        setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0 &&
        setsockopt(fd.get(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) == 0 &&
        bind(fd.get(), reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0 && listen(fd.get(), 1024) == 0)
        return fd;

    std::perror(addr.c_str());
    return {};
}

void on_signal(int)
{
    int saved = errno;
    uint64_t one = 1;
    ssize_t r = write(g_stop_fd, &one, sizeof one);
    (void)r;
    errno = saved;
}

}  // namespace

int run_server(const Config& cfg)
{
    std::signal(SIGPIPE, SIG_IGN);
    rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    Shared shared{Fd(open(cfg.root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)), cfg.timeout};
    if (!shared.root) {
        std::perror(cfg.root.c_str());
        return -1;
    }
    Fd probe(open_beneath(shared.root.get(), "."));
    if (!probe) {
        std::perror("openat2 (needs Linux 5.6+)");
        return -1;
    }
    probe.reset();

    Fd stop(eventfd(0, EFD_CLOEXEC));
    g_stop_fd = stop.get();
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    std::vector<Fd> listeners;
    for (int i = 0; i < cfg.workers; i++) {
        Fd listener = listen_on(cfg.addr, cfg.port);
        if (!listener)
            return -1;
        listeners.push_back(std::move(listener));
    }

    std::printf("listening on %s:%d with %d workers\n", cfg.addr.c_str(), cfg.port, cfg.workers);
    std::fflush(stdout);

    uint64_t n;
    while (read(stop.get(), &n, sizeof n) < 0 && errno == EINTR) {
    }
    return 0;
}

}  // namespace http
