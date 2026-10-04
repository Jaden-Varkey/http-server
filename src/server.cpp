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
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
constexpr uint64_t kListenTag = 0;
constexpr uint64_t kStopTag = 1;
constexpr uint64_t kFirstConnTag = 2;

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
    uint32_t events = EPOLLIN;
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

const char* reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    }
    return "Unknown";
}

const char* content_type(std::string_view path)
{
    static constexpr std::pair<std::string_view, const char*> types[] = {
        {".html", "text/html; charset=utf-8"},
        {".htm", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".json", "application/json"},
        {".txt", "text/plain; charset=utf-8"},
        {".xml", "application/xml"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},
        {".webp", "image/webp"},
        {".ico", "image/x-icon"},
        {".woff2", "font/woff2"},
        {".wasm", "application/wasm"},
        {".pdf", "application/pdf"},
    };

    size_t dot = path.rfind('.');
    if (dot != std::string_view::npos && path.find('/', dot) == std::string_view::npos) {
        std::string_view ext = path.substr(dot);
        for (const auto& [e, type] : types)
            if (e == ext)
                return type;
    }
    return "application/octet-stream";
}

class Worker {
public:
    Worker(Fd listener, const Shared& shared) : listener_(std::move(listener)), shared_(shared) {}

    void run();

private:
    void accept_all();
    void on_readable(Conn& c);
    void process(Conn& c);
    void serve(Conn& c, const Request& req);
    void respond_error(Conn& c, int status, bool body = true);
    bool write_out(Conn& c);
    void watch(Conn& c, uint32_t events);
    void close_conn(Conn& c);
    const char* date();

    Fd listener_;
    const Shared& shared_;
    Fd epoll_;
    std::vector<std::unique_ptr<Conn>> conns_;
    std::vector<size_t> free_;
    time_t date_time_ = 0;
    char date_[64] = {};
};

void Worker::run()
{
    epoll_ = Fd(epoll_create1(EPOLL_CLOEXEC));
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u64 = kListenTag;
    epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, listener_.get(), &ev);
    ev.data.u64 = kStopTag;
    epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, g_stop_fd, &ev);

    std::array<epoll_event, 256> events;
    for (;;) {
        int n = epoll_wait(epoll_.get(), events.data(), events.size(), -1);
        if (n < 0 && errno != EINTR) {
            std::perror("epoll_wait");
            return;
        }
        for (int i = 0; i < n; i++) {
            uint64_t tag = events[i].data.u64;
            if (tag == kStopTag)
                return;
            if (tag == kListenTag) {
                accept_all();
                continue;
            }

            Conn* c = conns_[tag - kFirstConnTag].get();
            if (!c)
                continue;
            if (c->writing)
                write_out(*c);
            else
                on_readable(*c);
        }
    }
}

void Worker::accept_all()
{
    for (;;) {
        int fd = accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                std::perror("accept4");
            return;
        }

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        size_t idx;
        if (free_.empty()) {
            idx = conns_.size();
            conns_.emplace_back();
        } else {
            idx = free_.back();
            free_.pop_back();
        }

        auto c = std::make_unique<Conn>();
        c->fd = Fd(fd);
        c->index = idx;

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.u64 = idx + kFirstConnTag;
        if (epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
            free_.push_back(idx);
            continue;
        }
        conns_[idx] = std::move(c);
    }
}

void Worker::on_readable(Conn& c)
{
    while (c.in_len < c.in.size()) {
        ssize_t n = read(c.fd.get(), c.in.data() + c.in_len, c.in.size() - c.in_len);
        if (n > 0) {
            c.in_len += n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        close_conn(c);
        return;
    }
    process(c);
}

void Worker::process(Conn& c)
{
    ParseResult r = parse_request({c.in.data(), c.in_len});
    if (r.kind == ParseResult::Kind::Incomplete) {
        if (c.in_len < c.in.size())
            return;
        respond_error(c, 431);
    } else if (r.kind == ParseResult::Kind::Error) {
        respond_error(c, r.status);
    } else {
        serve(c, r.request);
    }
    write_out(c);
}

void Worker::serve(Conn& c, const Request& req)
{
    bool head = req.method == Method::Head;
    auto path = target_to_path(req.target);
    if (!path)
        return respond_error(c, 400, !head);

    Fd file(open_beneath(shared_.root.get(), path->c_str()));
    if (!file)
        return respond_error(c, errno == EACCES ? 403 : 404, !head);

    struct stat st;
    if (fstat(file.get(), &st) < 0 || !S_ISREG(st.st_mode))
        return respond_error(c, 404, !head);

    int len = std::snprintf(c.out.data(), c.out.size(),
                            "HTTP/1.1 200 OK\r\n"
                            "Server: http-server\r\n"
                            "Date: %s\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Length: %lld\r\n"
                            "Connection: close\r\n\r\n",
                            date(), content_type(*path), static_cast<long long>(st.st_size));
    if (len < 0 || static_cast<size_t>(len) >= c.out.size())
        return respond_error(c, 500, !head);

    c.out_off = 0;
    c.out_len = len;
    if (!head) {
        c.file = std::move(file);
        c.file_off = 0;
        c.file_left = st.st_size;
    }
    c.writing = true;
}

void Worker::respond_error(Conn& c, int status, bool body)
{
    const char* text = reason(status);
    int len = std::snprintf(c.out.data(), c.out.size(),
                            "HTTP/1.1 %d %s\r\n"
                            "Server: http-server\r\n"
                            "Date: %s\r\n"
                            "Content-Type: text/plain; charset=utf-8\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n\r\n",
                            status, text, date(), std::strlen(text) + 5);
    if (body)
        len += std::snprintf(c.out.data() + len, c.out.size() - len, "%d %s\n", status, text);

    c.out_off = 0;
    c.out_len = len;
    c.file.reset();
    c.file_left = 0;
    c.close_after = true;
    c.writing = true;
}

// Returns false if the connection was closed.
bool Worker::write_out(Conn& c)
{
    while (c.out_off < c.out_len || c.file_left > 0) {
        ssize_t n;
        if (c.out_off < c.out_len)
            n = send(c.fd.get(), c.out.data() + c.out_off, c.out_len - c.out_off, MSG_NOSIGNAL);
        else
            n = sendfile(c.fd.get(), c.file.get(), &c.file_off, c.file_left);

        if (n > 0) {
            if (c.out_off < c.out_len)
                c.out_off += n;
            else
                c.file_left -= n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            watch(c, EPOLLOUT);
            return true;
        }
        close_conn(c);
        return false;
    }

    close_conn(c);
    return false;
}

void Worker::watch(Conn& c, uint32_t events)
{
    if (c.events == events)
        return;
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = c.index + kFirstConnTag;
    if (epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, c.fd.get(), &ev) == 0)
        c.events = events;
}

void Worker::close_conn(Conn& c)
{
    size_t idx = c.index;
    conns_[idx].reset();
    free_.push_back(idx);
}

const char* Worker::date()
{
    time_t t = time(nullptr);
    if (t != date_time_) {
        tm g;
        gmtime_r(&t, &g);
        std::strftime(date_, sizeof date_, "%a, %d %b %Y %H:%M:%S GMT", &g);
        date_time_ = t;
    }
    return date_;
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
        workers.push_back(std::make_unique<Worker>(std::move(listener), shared));
    }

    std::printf("listening on %s:%d with %d workers\n", cfg.addr.c_str(), cfg.port, cfg.workers);
    std::fflush(stdout);

    std::vector<std::thread> threads;
    for (auto& w : workers)
        threads.emplace_back(&Worker::run, w.get());
    for (auto& t : threads)
        t.join();
    return 0;
}

}  // namespace http
