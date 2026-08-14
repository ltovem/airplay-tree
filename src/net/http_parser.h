/*!
 * @file http_parser.h
 * @brief Minimal HTTP/RTSP request/response parser
 *
 * AirPlay uses RTSP-over-HTTP style framing:
 *   - Request line:  "METHOD uri RTSP/1.0" (or HTTP/1.1)
 *   - Status  line:  "RTSP/1.0 200 OK"
 *   - Headers:       "CSeq: 1\r\n", etc.
 *   - Empty line separates headers from body (\r\n\r\n)
 *   - Body length is from Content-Length header
 */
#ifndef AIRPLAY2_HTTP_PARSER_H
#define AIRPLAY2_HTTP_PARSER_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace airplay2 {
namespace net {

struct HttpHeaderCmp {
    bool operator()(const std::string& a, const std::string& b) const {
        if (a.size() != b.size()) return a.size() < b.size();
        for (size_t i = 0; i < a.size(); ++i) {
            int ca = std::tolower((unsigned char)a[i]);
            int cb = std::tolower((unsigned char)b[i]);
            if (ca != cb) return ca < cb;
        }
        return false;
    }
};
using HeaderMap = std::map<std::string, std::string, HttpHeaderCmp>;

struct HttpRequest {
    std::string method;        // "OPTIONS", "SETUP", "POST", "GET", ...
    std::string uri;           // "/info", "/stream", "*", ...
    std::string protocol;      // "RTSP/1.0" or "HTTP/1.1"
    HeaderMap   headers;
    std::vector<uint8_t> body;

    std::string header(const std::string& key, const std::string& dflt = "") const;
    int64_t     content_length() const;
    int         cseq() const;
};

struct HttpResponse {
    int         code = 0;      // 200, 404, ...
    std::string reason;
    HeaderMap   headers;
    std::vector<uint8_t> body;

    void set_content_type(const std::string& ct) { headers["Content-Type"] = ct; }
    void set_body(std::vector<uint8_t> b);
    void set_body_str(const std::string& s) { set_body(std::vector<uint8_t>(s.begin(), s.end())); }
    std::string serialize() const;
};

/*!
 * @brief Incremental HTTP/RTSP request parser.
 *
 * Feed bytes via parse(). When a full request is ready, parse()
 * returns the number of bytes consumed from the input. You then
 * call take_request() to retrieve it.
 */
class HttpRequestParser {
public:
    enum State {
        S_METHOD = 0,
        S_URI,
        S_PROTO,
        S_HEADER_NAME,
        S_HEADER_VALUE,
        S_HEADERS_DONE,
        S_BODY,
        S_COMPLETE,
        S_ERROR
    };

    HttpRequestParser() = default;

    /// Parse incoming bytes. Returns bytes consumed, or 0 on error.
    size_t parse(const uint8_t* data, size_t len);

    State  state() const { return state_; }
    bool   has_error() const { return state_ == S_ERROR; }
    bool   is_complete() const { return state_ == S_COMPLETE; }

    /// Retrieve completed request (resets parser)
    HttpRequest take_request();

private:
    State state_ = S_METHOD;
    HttpRequest req_;
    size_t body_expected_ = 0;
    size_t header_so_far_ = 0;
    std::string cur_header_name_;
    std::string cur_header_value_;
    std::string line_buffer_;
};

/// Helper: build a simple 200 OK RTSP response with CSeq
HttpResponse make_rtsp_ok(int cseq,
                          const HeaderMap& extra_headers = {},
                          std::vector<uint8_t> body = {},
                          const std::string& content_type = "");

} // namespace net
} // namespace airplay2

#endif // AIRPLAY2_HTTP_PARSER_H
