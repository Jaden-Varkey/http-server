#include "parse.hpp"

#include <cstdio>
#include <string>

using namespace http;
using Kind = ParseResult::Kind;

static int failures;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

static int status_of(std::string_view raw)
{
    auto r = parse_request(raw);
    return r.kind == Kind::Error ? r.status : r.kind == Kind::Ok ? 200 : 0;
}

static void test_basic()
{
    std::string req = "GET /a.html HTTP/1.1\r\nHost: x\r\n\r\n";
    auto r = parse_request(req);
    CHECK(r.kind == Kind::Ok && r.used == req.size());
    CHECK(r.request.method == Method::Get && r.request.keep_alive && r.request.target == "/a.html");
    CHECK(parse_request("HEAD / HTTP/1.1\r\n\r\n").request.method == Method::Head);
}

static void test_incomplete()
{
    CHECK(parse_request("").kind == Kind::Incomplete);
    CHECK(parse_request("GET / HTTP/1.1\r\nHost: x\r\n").kind == Kind::Incomplete);
    CHECK(parse_request("GET / HTTP/1.1\r\n\r").kind == Kind::Incomplete);
}

static void test_pipelined_consumes_one()
{
    std::string two = "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n";
    auto first = parse_request(two);
    CHECK(first.used == 19 && first.request.target == "/a");
    auto second = parse_request(std::string_view(two).substr(first.used));
    CHECK(second.used == 19 && second.request.target == "/b");
}

static void test_keep_alive_rules()
{
    auto ka = [](std::string_view raw) { return parse_request(raw).request.keep_alive; };
    CHECK(!ka("GET / HTTP/1.0\r\n\r\n"));
    CHECK(ka("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"));
    CHECK(!ka("GET / HTTP/1.1\r\nconnection: Close\r\n\r\n"));
    CHECK(!ka("GET / HTTP/1.1\r\nConnection: TE , close\r\n\r\n"));
    CHECK(ka("GET / HTTP/1.1\r\nConnection: not-close\r\n\r\n"));
}

static void test_rejections()
{
    CHECK(status_of("POST / HTTP/1.1\r\n\r\n") == 405);
    CHECK(status_of("GET / HTTP/2.0\r\n\r\n") == 505);
    CHECK(status_of("GET / HTTP/1.1x\r\n\r\n") == 400);
    CHECK(status_of("GET /\r\n\r\n") == 400);
    CHECK(status_of("GET index.html HTTP/1.1\r\n\r\n") == 400);
    CHECK(status_of("GET /a b HTTP/1.1\r\n\r\n") == 400);
    CHECK(status_of("\r\n\r\n") == 400);
    CHECK(status_of("GET / HTTP/1.1\r\nno colon here\r\n\r\n") == 400);
    CHECK(status_of("GET / HTTP/1.1\r\n folded: x\r\n\r\n") == 400);
    CHECK(status_of("GET / HTTP/1.1\r\nHost: x\rY\r\n\r\n") == 400);
    CHECK(status_of("GET / HTTP/1.1\r\nContent-Length: 5\r\n\r\n") == 400);
    CHECK(status_of("GET / HTTP/1.1\r\nContent-Length: x\r\n\r\n") == 400);
    CHECK(status_of("GET / HTTP/1.1\r\nContent-Length: 0\r\n\r\n") == 200);
    CHECK(status_of("GET / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") == 501);
}

static void test_target_to_path()
{
    auto path = [](std::string_view t) { return target_to_path(t); };
    CHECK(path("/") == "index.html");
    CHECK(path("/?q=1") == "index.html");
    CHECK(path("/css/site.css?v=2#top") == "css/site.css");
    CHECK(path("/docs/") == "docs/index.html");
    CHECK(path("/a%20b.txt") == "a b.txt");
    CHECK(path("//etc/passwd") == "etc/passwd");
    CHECK(path("/%2e%2E/x") == "../x");
    CHECK(!path("/bad%zz"));
    CHECK(!path("/bad%2"));
    CHECK(!path("/nul%00byte"));
    CHECK(!path("/" + std::string(2000, 'a')));
}

int main()
{
    test_basic();
    test_incomplete();
    test_pipelined_consumes_one();
    test_keep_alive_rules();
    test_rejections();
    test_target_to_path();
    if (failures == 0)
        std::puts("parse tests passed");
    return failures != 0;
}
