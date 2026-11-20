#pragma once

/**
 * @file http_types.hpp
 * @brief HTTP/1.1 request/response models, case-insensitive headers, status reason phrases,
 *        and incremental parsing/serialization helpers.
 */

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace goforge::http {

namespace detail {

inline std::string to_lower_ascii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (unsigned char c : sv) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

inline std::string_view trim_ows(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t')) sv.remove_suffix(1);
    return sv;
}

inline std::string_view default_reason_phrase(int status) noexcept {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Unknown";
    }
}

} // namespace detail

/**
 * @class HeaderMap
 * @brief Case-insensitive HTTP header container preserving canonical wire names for responses.
 */
class HeaderMap {
    struct Entry {
        std::string canonical_name;
        std::string value;
    };
    std::unordered_map<std::string, Entry> entries_;

public:
    void set(std::string_view name, std::string value) {
        std::string lower = detail::to_lower_ascii(name);
        entries_.insert_or_assign(std::move(lower), Entry{std::string(name), std::move(value)});
    }

    [[nodiscard]] bool contains(std::string_view name) const {
        return entries_.contains(detail::to_lower_ascii(name));
    }

    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const {
        auto it = entries_.find(detail::to_lower_ascii(name));
        if (it == entries_.end()) return std::nullopt;
        return std::string_view(it->second.value);
    }

    [[nodiscard]] std::string get_or(std::string_view name, std::string_view fallback = "") const {
        auto v = get(name);
        return v ? std::string(*v) : std::string(fallback);
    }

    void erase(std::string_view name) {
        entries_.erase(detail::to_lower_ascii(name));
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const auto& [_, entry] : entries_) {
            fn(entry.canonical_name, entry.value);
        }
    }
};

/**
 * @struct HttpRequest
 * @brief Parsed HTTP/1.1 request message.
 */
struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string raw_query;
    std::string version{"HTTP/1.1"};
    HeaderMap headers;
    std::string body;
    std::string remote_addr;

    /// Returns whether this connection should remain open for HTTP/1.1 Keep-Alive.
    [[nodiscard]] bool keep_alive() const {
        auto conn = headers.get("Connection");
        if (conn) {
            std::string lower = detail::to_lower_ascii(*conn);
            if (lower.find("close") != std::string::npos) return false;
            if (lower.find("keep-alive") != std::string::npos) return true;
        }
        return version == "HTTP/1.1";
    }
};

/**
 * @struct HttpResponse
 * @brief HTTP/1.1 response message with wire serializer and convenience factories.
 */
struct HttpResponse {
    int status{200};
    std::string status_text;
    HeaderMap headers;
    std::string body;

    static HttpResponse text(int code, std::string content) {
        HttpResponse r;
        r.status = code;
        r.body = std::move(content);
        r.headers.set("Content-Type", "text/plain; charset=utf-8");
        return r;
    }

    static HttpResponse json(int code, std::string json_body) {
        HttpResponse r;
        r.status = code;
        r.body = std::move(json_body);
        r.headers.set("Content-Type", "application/json; charset=utf-8");
        return r;
    }

    static HttpResponse empty(int code = 204) {
        HttpResponse r;
        r.status = code;
        return r;
    }

    /// Serializes this response into an HTTP/1.1 wire buffer.
    [[nodiscard]] std::string serialize(bool keep_alive) const {
        std::string_view reason = status_text.empty()
            ? detail::default_reason_phrase(status)
            : std::string_view(status_text);

        std::string out;
        out.reserve(128 + body.size());
        out += "HTTP/1.1 ";
        out += std::to_string(status);
        out += ' ';
        out += reason;
        out += "\r\n";

        bool has_content_length = false;
        bool has_connection = false;
        bool has_server = false;

        headers.for_each([&](const std::string& k, const std::string& v) {
            std::string lower = detail::to_lower_ascii(k);
            if (lower == "content-length") has_content_length = true;
            if (lower == "connection") has_connection = true;
            if (lower == "server") has_server = true;
            out += k;
            out += ": ";
            out += v;
            out += "\r\n";
        });

        if (!has_server) {
            out += "Server: goforge\r\n";
        }
        if (!has_content_length) {
            out += "Content-Length: ";
            out += std::to_string(body.size());
            out += "\r\n";
        }
        if (!has_connection) {
            out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        }
        out += "\r\n";
        out += body;
        return out;
    }
};

enum class ParseStatus : std::uint8_t {
    Ok,
    Incomplete,
    BadRequest,
    HeadersTooLarge,
    PayloadTooLarge
};

struct ParseHeaderResult {
    ParseStatus status{ParseStatus::Incomplete};
    HttpRequest request;
    std::size_t header_end_pos{0}; ///< Offset immediately after "\r\n\r\n"
    std::size_t content_length{0};
};

/**
 * @brief Parses HTTP/1.1 request line and headers from `raw_buf` (non-destructive).
 */
inline ParseHeaderResult parse_http_headers(
    std::string_view raw_buf,
    std::size_t max_header_bytes,
    std::size_t max_body_bytes
) {
    ParseHeaderResult res;
    auto sep = raw_buf.find("\r\n\r\n");
    if (sep == std::string_view::npos) {
        if (raw_buf.size() > max_header_bytes) {
            res.status = ParseStatus::HeadersTooLarge;
        } else {
            res.status = ParseStatus::Incomplete;
        }
        return res;
    }

    if (sep + 4 > max_header_bytes) {
        res.status = ParseStatus::HeadersTooLarge;
        return res;
    }

    res.header_end_pos = sep + 4;
    std::string_view head = raw_buf.substr(0, sep);

    // 1. Parse request line: METHOD SP REQUEST-TARGET SP HTTP-VERSION
    auto first_crlf = head.find("\r\n");
    std::string_view req_line = (first_crlf == std::string_view::npos) ? head : head.substr(0, first_crlf);

    auto sp1 = req_line.find(' ');
    if (sp1 == std::string_view::npos || sp1 == 0) {
        res.status = ParseStatus::BadRequest;
        return res;
    }
    auto sp2 = req_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos || sp2 == sp1 + 1 || sp2 + 1 >= req_line.size()) {
        res.status = ParseStatus::BadRequest;
        return res;
    }

    std::string_view method = req_line.substr(0, sp1);
    std::string_view target = req_line.substr(sp1 + 1, sp2 - (sp1 + 1));
    std::string_view version = req_line.substr(sp2 + 1);

    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        res.status = ParseStatus::BadRequest;
        return res;
    }

    res.request.method = std::string(method);
    res.request.target = std::string(target);
    res.request.version = std::string(version);

    auto qpos = target.find('?');
    if (qpos == std::string_view::npos) {
        res.request.path = std::string(target);
    } else {
        res.request.path = std::string(target.substr(0, qpos));
        res.request.raw_query = std::string(target.substr(qpos + 1));
    }

    // 2. Parse header lines
    if (first_crlf != std::string_view::npos) {
        std::size_t pos = first_crlf + 2;
        while (pos < head.size()) {
            auto next = head.find("\r\n", pos);
            std::string_view line = (next == std::string_view::npos)
                ? head.substr(pos)
                : head.substr(pos, next - pos);
            pos = (next == std::string_view::npos) ? head.size() : next + 2;

            if (line.empty()) continue;
            auto colon = line.find(':');
            if (colon == std::string_view::npos || colon == 0) {
                res.status = ParseStatus::BadRequest;
                return res;
            }
            std::string_view name = line.substr(0, colon);
            std::string_view val = detail::trim_ows(line.substr(colon + 1));
            res.request.headers.set(name, std::string(val));
        }
    }

    // 3. Parse optional Content-Length
    if (auto cl = res.request.headers.get("Content-Length")) {
        std::size_t len = 0;
        const char* first = cl->data();
        const char* last = first + cl->size();
        auto [ptr, ec] = std::from_chars(first, last, len);
        if (ec != std::errc{} || ptr != last) {
            res.status = ParseStatus::BadRequest;
            return res;
        }
        if (len > max_body_bytes) {
            res.status = ParseStatus::PayloadTooLarge;
            return res;
        }
        res.content_length = len;
    }

    res.status = ParseStatus::Ok;
    return res;
}

} // namespace goforge::http
