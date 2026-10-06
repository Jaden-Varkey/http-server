#include "parse.hpp"

#include <algorithm>
#include <cctype>

namespace http {
namespace {

using Kind = ParseResult::Kind;

ParseResult reject(int status)
{
    ParseResult r;
    r.kind = Kind::Error;
    r.status = status;
    return r;
}

bool is_ows(char c)
{
    return c == ' ' || c == '\t';
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && is_ows(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && is_ows(s.back()))
        s.remove_suffix(1);
    return s;
}

bool iequals(std::string_view a, std::string_view b)
{
    return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
    });
}

// True if the comma-separated list contains tok.
bool has_token(std::string_view list, std::string_view tok)
{
    while (!list.empty()) {
        size_t comma = list.find(',');
        if (iequals(trim(list.substr(0, comma)), tok))
            return true;
        if (comma == std::string_view::npos)
            break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

bool has_bare_newline(std::string_view line)
{
    return line.find_first_of("\r\n") != std::string_view::npos;
}

int hex(char c)
{
    return std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (c | 0x20) - 'a' + 10;
}

}  // namespace

ParseResult parse_request(std::string_view buf)
{
    constexpr auto npos = std::string_view::npos;
    size_t end = buf.find("\r\n\r\n");
    if (end == npos)
        return {};

    std::string_view head = buf.substr(0, end);
    size_t eol = head.find("\r\n");
    std::string_view line = head.substr(0, eol);
    std::string_view headers = eol == npos ? std::string_view{} : head.substr(eol + 2);

    size_t sp1 = line.find(' ');
    size_t sp2 = sp1 == npos ? npos : line.find(' ', sp1 + 1);
    if (sp1 == 0 || sp2 == npos || has_bare_newline(line))
        return reject(400);

    std::string_view method = line.substr(0, sp1);
    std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string_view version = line.substr(sp2 + 1);

    auto is_ctl = [](unsigned char c) { return c <= 0x20 || c == 0x7f; };
    if (target.empty() || target[0] != '/' || std::ranges::any_of(target, is_ctl))
        return reject(400);
    if (version.size() != 8 || !version.starts_with("HTTP/"))
        return reject(400);
    if (!version.starts_with("HTTP/1.") || (version[7] != '0' && version[7] != '1'))
        return reject(505);

    ParseResult result;
    result.kind = Kind::Ok;
    result.used = end + 4;
    result.request.target = target;
    result.request.keep_alive = version[7] == '1';
    if (method == "GET")
        result.request.method = Method::Get;
    else if (method == "HEAD")
        result.request.method = Method::Head;
    else
        return reject(405);

    while (!headers.empty()) {
        size_t next = headers.find("\r\n");
        std::string_view h = headers.substr(0, next);
        headers = next == npos ? std::string_view{} : headers.substr(next + 2);

        size_t colon = h.find(':');
        if (colon == 0 || colon == npos || is_ows(h[0]) || has_bare_newline(h))
            return reject(400);

        std::string_view name = h.substr(0, colon);
        std::string_view value = trim(h.substr(colon + 1));
        if (iequals(name, "connection")) {
            if (has_token(value, "close"))
                result.request.keep_alive = false;
            else if (has_token(value, "keep-alive"))
                result.request.keep_alive = true;
        } else if (iequals(name, "content-length")) {
            if (value.empty() || !std::ranges::all_of(value, [](char c) { return c == '0'; }))
                return reject(400);
        } else if (iequals(name, "transfer-encoding")) {
            return reject(501);
        }
    }
    return result;
}

std::optional<std::string> target_to_path(std::string_view target)
{
    constexpr size_t max_path = 1024;
    std::string path;
    size_t i = target.find_first_not_of('/');
    for (; i < target.size(); i++) {
        char c = target[i];
        if (c == '?' || c == '#')
            break;
        if (c == '%') {
            if (i + 2 >= target.size() ||
                !std::isxdigit(static_cast<unsigned char>(target[i + 1])) ||
                !std::isxdigit(static_cast<unsigned char>(target[i + 2])))
                return std::nullopt;
            c = static_cast<char>(hex(target[i + 1]) * 16 + hex(target[i + 2]));
            i += 2;
            if (c == '\0')
                return std::nullopt;
        }
        path.push_back(c);
        if (path.size() >= max_path)
            return std::nullopt;
    }
    if (path.empty() || path.back() == '/')
        path += "index.html";
    return path;
}

}  // namespace http
