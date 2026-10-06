#ifndef HTTP_SYS_H
#define HTTP_SYS_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

time_t clock_sec(void);
void raise_fd_limit(void);

/* Returns an eventfd that becomes readable on SIGINT/SIGTERM, or -1. */
int install_stop_signals(void);

int open_beneath(int root, const char* path);
int listen_on(const char* addr, int port);

const char* mime_type(const char* path);
const char* status_text(int status);

#ifdef __cplusplus
}
#endif

#endif
