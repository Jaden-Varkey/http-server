#include "server.hpp"
#include "parse.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <netinet/tcp.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace http {
namespace {

constexpr size_t kInSize = 8192;
constexpr size_t kOutSize = 8192;
constexpr int kMaxEvents = 256;

int g_stop_fd = -1;  // written by the signal handler to wake every worker

// Owns a file descriptor and closes it exactly once.
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
    Fd file;                  // file still to be sent with sendfile
    bool writing = false;     // registered for EPOLLOUT instead of EPOLLIN
    bool close_after = false; // close once the pending response is sent
    off_t file_off = 0, file_left = 0;
    size_t index = 0;         // position in the worker's connection vector
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

time_t clock_sec()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return ts.tv_sec;
}

// Opens path relative to root; the kernel refuses any escape from it.
int open_beneath(int root, const char* path)
{
    open_how how{};
    how.flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
    return static_cast<int>(syscall(SYS_openat2, root, path, &how, sizeof how));
}

const char* mime_type(const std::string& path)
{
    static constexpr struct {
        const char *ext, *type;
    } table[] = {
        {".html", "text/html; charset=utf-8"}, {".css", "text/css"},
        {".js", "text/javascript"},            {".json", "application/json"},
        {".txt", "text/plain; charset=utf-8"}, {".png", "image/png"},
        {".jpg", "image/jpeg"},                {".gif", "image/gif"},
        {".svg", "image/svg+xml"},             {".ico", "image/x-icon"},
    };
    const char* dot = std::strrchr(path.c_str(), '.');
    if (dot)
        for (auto& [ext, type] : table)
            if (strcasecmp(dot, ext) == 0)
                return type;
    return "application/octet-stream";
}

const char* status_text(int status)
{
    switch (status) {
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default:  return "Internal Server Error";
    }
}

// req is null when the request could not be parsed; the connection then closes.
void reply_error(Conn& c, int status, const Request* req)
{
    const char* text = status_text(status);
    bool keep_alive = req && req->keep_alive;
    int n = std::snprintf(c.out.data(), kOutSize,
                          "HTTP/1.1 %d %s\r\n%sContent-Type: text/plain\r\n"
                          "Content-Length: %zu\r\nConnection: %s\r\n\r\n",
                          status, text, status == 405 ? "Allow: GET, HEAD\r\n" : "",
                          std::strlen(text) + 1, keep_alive ? "keep-alive" : "close");
    if (!req || req->method == Method::Get)
        n += std::snprintf(c.out.data() + n, kOutSize - n, "%s\n", text);
    c.out_off = 0;
    c.out_len = n;
    c.close_after = !keep_alive;
}

// Builds the response in c.out, leaving large bodies to be sent with sendfile.
void respond(Conn& c, const Request& req, int root)
{
    auto path = target_to_path(req.target);
    if (!path) {
        reply_error(c, 400, &req);
        return;
    }
    Fd file(open_beneath(root, path->c_str()));
    struct stat st;
    if (!file || fstat(file.get(), &st) < 0 || !S_ISREG(st.st_mode)) {
        reply_error(c, 404, &req);
        return;
    }

    c.close_after = !req.keep_alive;
    c.out_off = 0;
    c.out_len = std::snprintf(c.out.data(), kOutSize,
                              "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                              "Content-Length: %lld\r\nConnection: %s\r\n\r\n",
                              mime_type(*path), static_cast<long long>(st.st_size),
                              req.keep_alive ? "keep-alive" : "close");

    if (req.method == Method::Head)
        return;
    if (st.st_size <= static_cast<off_t>(kOutSize - c.out_len)) {
        ssize_t n = pread(file.get(), c.out.data() + c.out_len, st.st_size, 0);
        if (n != st.st_size)
            reply_error(c, 500, nullptr);
        else
            c.out_len += n;
    } else {
        c.file = std::move(file);
        c.file_off = 0;
        c.file_left = st.st_size;
    }
}

class Worker {
public:
    Worker(const Shared& shared, Fd listen_fd)
        : shared_(shared), listen_(std::move(listen_fd)), ep_(epoll_create1(EPOLL_CLOEXEC))
    {
        add(listen_.get(), this);
        add(g_stop_fd, &g_stop_fd);
    }

    void start() { thread_ = std::jthread([this] { run(); }); }
    void join() { thread_.join(); }
    const Stats& stats() const { return stats_; }

private:
    void add(int fd, void* tag)
    {
        epoll_event ev{.events = EPOLLIN, .data = {.ptr = tag}};
        epoll_ctl(ep_.get(), EPOLL_CTL_ADD, fd, &ev);
    }

    void watch(Conn& c, uint32_t events)
    {
        epoll_event ev{.events = events, .data = {.ptr = &c}};
        epoll_ctl(ep_.get(), EPOLL_CTL_MOD, c.fd.get(), &ev);
    }

    // Removes and destroys c (closing its descriptors); c is dead afterwards.
    void drop(Conn& c)
    {
        size_t i = c.index;
        if (i != conns_.size() - 1) {
            conns_[i] = std::move(conns_.back());
            conns_[i]->index = i;
        }
        conns_.pop_back();
    }

    // Sends the pending response. Returns 1 when done, 0 if it would block, -1 on error.
    int flush(Conn& c)
    {
        if (c.out_len == 0)
            return 1;
        while (c.out_off < c.out_len) {
            int more = c.file_left > 0 ? MSG_MORE : 0;
            ssize_t n = send(c.fd.get(), c.out.data() + c.out_off, c.out_len - c.out_off, MSG_NOSIGNAL | more);
            if (n < 0)
                return errno == EAGAIN ? 0 : -1;
            c.out_off += n;
        }
        while (c.file_left > 0) {
            ssize_t n = sendfile(c.fd.get(), c.file.get(), &c.file_off, c.file_left);
            if (n < 0)
                return errno == EAGAIN ? 0 : -1;
            if (n == 0)
                return -1;  // file shrank under us; Content-Length is now a lie
            c.file_left -= n;
        }
        c.file.reset();
        c.out_off = c.out_len = 0;
        c.deadline = now_ + shared_.timeout;
        return 1;
    }

    // Sends what is pending, then answers every complete request already buffered.
    void serve(Conn& c)
    {
        for (;;) {
            int r = flush(c);
            if (r < 0 || (r > 0 && c.close_after)) {
                drop(c);
                return;
            }
            if (r == 0) {
                if (!c.writing) {
                    c.writing = true;
                    c.deadline = now_ + shared_.timeout;
                    watch(c, EPOLLOUT);
                }
                return;
            }
            if (c.writing) {
                c.writing = false;
                watch(c, EPOLLIN);
            }

            auto parsed = parse_request({c.in.data(), c.in_len});
            using Kind = ParseResult::Kind;
            if (parsed.kind == Kind::Incomplete && c.in_len < kInSize)
                return;
            if (parsed.kind == Kind::Ok) {
                respond(c, parsed.request, shared_.root.get());  // target still points into c.in
                c.in_len -= parsed.used;
                std::memmove(c.in.data(), c.in.data() + parsed.used, c.in_len);
                stats_.requests++;
            } else {
                reply_error(c, parsed.kind == Kind::Incomplete ? 431 : parsed.status, nullptr);
            }
        }
    }

    void on_event(Conn& c, uint32_t events)
    {
        if (events & (EPOLLERR | EPOLLHUP)) {
            drop(c);
            return;
        }
        if (!c.writing) {
            ssize_t n = read(c.fd.get(), c.in.data() + c.in_len, kInSize - c.in_len);
            if (n < 0 && errno == EAGAIN)
                return;
            if (n <= 0) {
                drop(c);
                return;
            }
            c.in_len += n;
        }
        serve(c);
    }

    void accept_all()
    {
        for (int i = 0; i < 64; i++) {
            Fd fd(accept4(listen_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (!fd)
                return;
            int one = 1;
            setsockopt(fd.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

            auto c = std::make_unique<Conn>();
            c->fd = std::move(fd);
            c->index = conns_.size();
            c->deadline = now_ + shared_.timeout;
            epoll_event ev{.events = EPOLLIN, .data = {.ptr = c.get()}};
            epoll_ctl(ep_.get(), EPOLL_CTL_ADD, c->fd.get(), &ev);
            conns_.push_back(std::move(c));
            stats_.accepted++;
        }
    }

    // Run once a second, so a timeout fires up to 1s late. Walks backwards
    // because drop() moves the last connection into the freed slot.
    void sweep()
    {
        for (size_t i = conns_.size(); i-- > 0;) {
            if (conns_[i]->deadline <= now_) {
                stats_.timeouts++;
                drop(*conns_[i]);
            }
        }
    }

    void run()
    {
        epoll_event evs[kMaxEvents];
        time_t swept = 0;
        for (bool stop = false; !stop;) {
            int n = epoll_wait(ep_.get(), evs, kMaxEvents, 1000);
            now_ = clock_sec();
            for (int i = 0; i < n && !stop; i++) {
                void* tag = evs[i].data.ptr;
                if (tag == &g_stop_fd)
                    stop = true;
                else if (tag == this)
                    accept_all();
                else
                    on_event(*static_cast<Conn*>(tag), evs[i].events);
            }
            if (now_ != swept) {
                swept = now_;
                sweep();
            }
        }
        conns_.clear();
    }

    const Shared& shared_;
    Fd listen_, ep_;
    std::vector<std::unique_ptr<Conn>> conns_;
    Stats stats_;
    time_t now_ = 0;
    std::jthread thread_;
};

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

    std::vector<std::unique_ptr<Worker>> workers;
    for (int i = 0; i < cfg.workers; i++) {
        Fd listener = listen_on(cfg.addr, cfg.port);
        if (!listener)
            return -1;
        workers.push_back(std::make_unique<Worker>(shared, std::move(listener)));
    }
    for (auto& w : workers)
        w->start();

    std::printf("listening on %s:%d with %d workers\n", cfg.addr.c_str(), cfg.port, cfg.workers);
    std::fflush(stdout);

    Stats total;
    for (auto& w : workers) {
        w->join();
        total.accepted += w->stats().accepted;
        total.requests += w->stats().requests;
        total.timeouts += w->stats().timeouts;
    }
    std::printf("accepted=%lu requests=%lu timeouts=%lu\n", total.accepted, total.requests, total.timeouts);
    return 0;
}

}  // namespace http
