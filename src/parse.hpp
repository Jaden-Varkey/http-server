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
    std::string_view target;
};

struct ParseResult {
    enum class Kind { Incomplete, Ok, Error } kind = Kind::Incomplete;
    size_t used = 0;
    int status = 0;
    Request request;
};

ParseResult parse_request(std::string_view buf);

std::optional<std::string> target_to_path(std::string_view target);

}  // namespace http
