#define _GNU_SOURCE
#include "sys.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

static int stop_fd = -1;

time_t clock_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return ts.tv_sec;
}

void raise_fd_limit(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

static void on_signal(int sig)
{
    (void)sig;
    int saved = errno;
    uint64_t one = 1;
    ssize_t r = write(stop_fd, &one, sizeof one);
    (void)r;
    errno = saved;
}

int install_stop_signals(void)
{
    stop_fd = eventfd(0, EFD_CLOEXEC);
    if (stop_fd < 0)
        return -1;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    return stop_fd;
}

/* The kernel refuses any path that would resolve outside root. */
int open_beneath(int root, const char* path)
{
    struct open_how how;
    memset(&how, 0, sizeof how);
    how.flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
    return (int)syscall(SYS_openat2, root, path, &how, sizeof how);
}

int listen_on(const char* addr, int port)
{
    int one = 1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    if (fd >= 0 && inet_pton(AF_INET, addr, &sa.sin_addr) == 1 &&
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) == 0 &&
        bind(fd, (struct sockaddr*)&sa, sizeof sa) == 0 && listen(fd, 1024) == 0)
        return fd;

    perror(addr);
    if (fd >= 0)
        close(fd);
    return -1;
}

static const struct {
    const char *ext, *type;
} mime_table[] = {
    {".html", "text/html; charset=utf-8"}, {".css", "text/css"},
    {".js", "text/javascript"},            {".json", "application/json"},
    {".txt", "text/plain; charset=utf-8"}, {".png", "image/png"},
    {".jpg", "image/jpeg"},                {".gif", "image/gif"},
    {".svg", "image/svg+xml"},             {".ico", "image/x-icon"},
};

const char* mime_type(const char* path)
{
    const char* dot = strrchr(path, '.');
    if (dot)
        for (size_t i = 0; i < sizeof mime_table / sizeof mime_table[0]; i++)
            if (strcasecmp(dot, mime_table[i].ext) == 0)
                return mime_table[i].type;
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
