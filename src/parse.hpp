#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace http {

enum class Method { Get, Head };

struct Request {
    Method method = Method::Get;
    bool keep_alive = true;
    std::string_view target;  // points into the buffer that was parsed
};

struct ParseResult {
    enum class Kind { Incomplete, Ok, Error } kind = Kind::Incomplete;
    size_t used = 0;  // bytes consumed when Ok
    int status = 0;   // HTTP status to reject with when Error
    Request request;
};

// Parses one request from the front of buf. Request bodies are not supported:
// a non-zero Content-Length or any Transfer-Encoding is rejected.
ParseResult parse_request(std::string_view buf);

// Turns a request target into a path relative to the document root: drops the
// query, percent-decodes, and maps directories to index.html. Fails on a
// malformed escape, an embedded NUL, or an over-long path.
std::optional<std::string> target_to_path(std::string_view target);

}  // namespace http
