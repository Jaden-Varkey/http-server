# http-server

A small, fast HTTP/1.1 static file server for Linux, written in C++20. Each worker thread runs its own
`epoll` event loop and its own `SO_REUSEPORT` listener, so workers share no state and never take a lock.

```sh
make
./http-server -p 8080 -w 4 www        # [-b addr] [-p port] [-w workers] [-t timeout_s] [root]
make test                              # unit tests + end-to-end tests
```

Requires Linux 5.6+ (for `openat2`), g++ or clang++ with C++20, and python3 for the tests.

## Design

- **One loop per worker.** `epoll_wait` drives non-blocking sockets. The kernel spreads new connections
  across the workers' listeners. A connection lives and dies on a single thread.
- **Parser** (`src/parse.cpp`) is a pure function over a `std::string_view`. It reports bytes consumed,
  "need more", or the HTTP status to reject with, so fragmented and pipelined requests need no special
  cases. The parsed target is a view into the connection buffer, so parsing allocates nothing.
- **Ownership.** `Fd` is a move-only RAII wrapper and each connection is a `unique_ptr<Conn>` owned by
  exactly one worker, so every error path closes its descriptors without explicit cleanup.
- **Pipelining and keep-alive.** After a response is fully written, the loop parses the next request
  already sitting in the buffer. Reading pauses while a response is blocked on a full socket, so memory per
  connection is fixed (8 KiB in + 8 KiB out).
- **Responses.** Files that fit in the output buffer go out with the header in one `send`. Larger files use
  `sendfile`, with `MSG_MORE` on the header so it shares a packet with the first data.
- **Path safety.** Paths are percent-decoded and opened with `openat2(RESOLVE_BENEATH |
  RESOLVE_NO_MAGICLINKS)` against the document root. The kernel rejects `..` and symlink escapes
  atomically, so there is no check-then-open race.
- **Timeouts.** Each connection has one deadline, reset when a response completes. A client that idles,
  trickles a request, or stops reading a response is closed within `-t` seconds (+1s sweep granularity).
- **Shutdown.** SIGINT/SIGTERM write to an `eventfd` that every worker polls. Workers close their
  connections and exit, and the process prints one stats line (`accepted`, `requests`, `timeouts`).

Supported: `GET` and `HEAD`, HTTP/1.0 and 1.1, `Content-Length` and `Content-Type`.
Rejected with the matching status: other methods (405), request bodies and `Transfer-Encoding` (400/501),
headers over 8 KiB (431), other HTTP versions (505). No TLS; put it behind a proxy for that.

## Tests

`make test` runs:

- `tests/test_parse.cpp`: 6 parser and path-decoding unit tests (malformed lines, bad headers, pipelined
  buffers, keep-alive token rules, `%00`, over-long paths).
- `tests/test_server.py`: end-to-end tests against the real binary: keep-alive reuse, pipelining
  (including 4 MiB responses behind a 4 KiB receive window), byte-at-a-time requests, HEAD framing,
  traversal and symlink escapes, 300 concurrent connections, idle/stalled-client timeouts, and clean
  SIGTERM shutdown.

That is 22 test cases (6 unit, 16 end-to-end). CI (`.github/workflows/ci.yml`) runs them with g++ and
clang++, and under AddressSanitizer + UBSan and ThreadSanitizer.

## Benchmark

`scripts/bench.sh` serves a ~230 B page and a 16 KiB file, and drives them with `wrk`.

Hardware: Intel i7-14650HX (24 threads), WSL2 Ubuntu 24.04, g++ 13 `-O2`, loopback, `wrk` on the same
machine, 4 server workers. Median of 3 runs of 10 s each, after a warmup. No socket errors or non-2xx
responses.

| Response | Connections | Requests/s | p99 latency |
| --- | ---: | ---: | ---: |
| ~230 B HTML | 1 | 21.8K | 0.89 ms |
| ~230 B HTML | 16 | 233.0K | 0.76 ms |
| ~230 B HTML | 64 | 356.1K | 1.72 ms |
| ~230 B HTML | 256 | 402.9K | 1.89 ms |
| 16 KiB binary | 64 | 303.3K (about 5 GB/s) | 1.18 ms |

These are loopback numbers on one box where the load generator competes for the same cores, so they show
the server's own overhead, not what a network deployment would see. Run-to-run spread is real: the
256-connection p99 across the three runs was 1.86 to 2.01 ms.
