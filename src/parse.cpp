#include "parse.hpp"

#include <vector>

namespace http {
namespace {

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z')
            x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z')
            y += 'a' - 'A';
        if (x != y)
            return false;
    }
    return true;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

bool is_tchar(char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

bool is_token(std::string_view s)
{
    if (s.empty())
        return false;
    for (char c : s)
        if (!is_tchar(c))
            return false;
    return true;
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Connection is a comma separated list, e.g. "keep-alive, Upgrade".
void apply_connection(std::string_view value, Request& req)
{
    while (!value.empty()) {
        size_t comma = value.find(',');
        std::string_view opt = trim(value.substr(0, comma));
        if (iequals(opt, "close"))
            req.keep_alive = false;
        else if (iequals(opt, "keep-alive"))
            req.keep_alive = true;
        if (comma == std::string_view::npos)
            break;
        value.remove_prefix(comma + 1);
    }
}

ParseResult fail(int status)
{
    ParseResult r;
    r.kind = ParseResult::Kind::Error;
    r.status = status;
    return r;
}

}  // namespace

ParseResult parse_request(std::string_view buf)
{
    size_t end = buf.find("\r\n\r\n");
    if (end == std::string_view::npos)
        return {};

    ParseResult r;
    r.used = end + 4;
    std::string_view head = buf.substr(0, end + 2);

    size_t eol = head.find("\r\n");
    std::string_view line = head.substr(0, eol);
    head.remove_prefix(eol + 2);

    size_t sp1 = line.find(' ');
    size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos || line.find(' ', sp2 + 1) != std::string_view::npos)
        return fail(400);

    std::string_view method = line.substr(0, sp1);
    std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string_view version = line.substr(sp2 + 1);

    if (version.size() != 8 || version.substr(0, 5) != "HTTP/" || version[6] != '.')
        return fail(400);
    if (version[5] != '1')
        return fail(505);
    bool http11 = version[7] != '0';
    r.request.keep_alive = http11;

    if (!is_token(method))
        return fail(400);
    if (method == "GET")
        r.request.method = Method::Get;
    else if (method == "HEAD")
        r.request.method = Method::Head;
    else
        return fail(501);

    if (target.empty() || target[0] != '/')
        return fail(400);
    r.request.target = target;

    bool has_host = false;
    while (!head.empty()) {
        eol = head.find("\r\n");
        line = head.substr(0, eol);
        head.remove_prefix(eol + 2);

        size_t colon = line.find(':');
        if (colon == std::string_view::npos || !is_token(line.substr(0, colon)))
            return fail(400);
        std::string_view name = line.substr(0, colon);
        std::string_view value = trim(line.substr(colon + 1));

        if (iequals(name, "Host")) {
            if (has_host)
                return fail(400);
            has_host = true;
        } else if (iequals(name, "Connection")) {
            apply_connection(value, r.request);
        } else if (iequals(name, "Transfer-Encoding")) {
            return fail(501);
        } else if (iequals(name, "Content-Length")) {
            if (value != "0")
                return fail(413);
        }
    }

    if (http11 && !has_host)
        return fail(400);

    r.kind = ParseResult::Kind::Ok;
    return r;
}

std::optional<std::string> target_to_path(std::string_view target)
{
    target = target.substr(0, target.find_first_of("?#"));

    std::string decoded;
    decoded.reserve(target.size());
    for (size_t i = 0; i < target.size(); i++) {
        char c = target[i];
        if (c == '%') {
            if (i + 2 >= target.size())
                return std::nullopt;
            int hi = hex_value(target[i + 1]), lo = hex_value(target[i + 2]);
            if (hi < 0 || lo < 0)
                return std::nullopt;
            c = static_cast<char>(hi * 16 + lo);
            i += 2;
        }
        if (c == '\0')
            return std::nullopt;
        decoded += c;
    }

    std::vector<std::string_view> parts;
    std::string_view rest = decoded;
    while (!rest.empty()) {
        size_t slash = rest.find('/');
        std::string_view seg = rest.substr(0, slash);
        if (seg == "..") {
            if (parts.empty())
                return std::nullopt;
            parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        if (slash == std::string_view::npos)
            break;
        rest.remove_prefix(slash + 1);
    }

    std::string path;
    for (std::string_view seg : parts) {
        if (!path.empty())
            path += '/';
        path += seg;
    }
    if (decoded.back() == '/' || path.empty())
        path += path.empty() ? "index.html" : "/index.html";
    return path;
}

}  // namespace http
