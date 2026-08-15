/*!
 * @file http_parser.cpp
 */
#include "http_parser.h"
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <string>     // std::stoi / std::stoll（Content-Length 等头解析）

namespace airplay2 {
namespace net {

std::string HttpRequest::header(const std::string& key, const std::string& dflt) const {
    auto it = headers.find(key);
    if (it == headers.end()) return dflt;
    return it->second;
}
int64_t HttpRequest::content_length() const {
    std::string v = header("Content-Length");
    if (v.empty()) return 0;
    try { return std::stoll(v); } catch (...) { return 0; }
}
int HttpRequest::cseq() const {
    std::string v = header("CSeq");
    if (v.empty()) return 0;
    try { return std::stoi(v); } catch (...) { return 0; }
}

void HttpResponse::set_body(std::vector<uint8_t> b) {
    body = std::move(b);
    headers["Content-Length"] = std::to_string(body.size());
}

std::string HttpResponse::serialize() const {
    std::string out;
    // Status line
    out += "RTSP/1.0 ";
    out += std::to_string(code);
    out += " ";
    out += reason.empty() ? (code == 200 ? "OK" : "Status") : reason;
    out += "\r\n";
    // Headers
    for (auto& [k, v] : headers) {
        out += k; out += ": "; out += v; out += "\r\n";
    }
    out += "\r\n";
    // Body
    if (!body.empty()) {
        out.insert(out.end(), body.begin(), body.end());
    }
    return out;
}

static inline std::string trim_space(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

size_t HttpRequestParser::parse(const uint8_t* data, size_t len) {
    size_t consumed = 0;
    while (consumed < len && state_ != S_COMPLETE && state_ != S_ERROR) {
        switch (state_) {
            case S_METHOD: case S_URI: case S_PROTO: {
                // Read up to SP / CRLF
                size_t i = consumed;
                for (; i < len; ++i) {
                    uint8_t c = data[i];
                    if (c == ' ' || c == '\r' || c == '\n') break;
                }
                // No terminator found yet — append to line buffer
                if (i == len) {
                    line_buffer_.append((const char*)data + consumed, len - consumed);
                    return len;
                }
                line_buffer_.append((const char*)data + consumed, i - consumed);
                consumed = i;
                // Now process terminator character
                uint8_t term = data[consumed];
                if (term == ' ') {
                    if (state_ == S_METHOD) {
                        req_.method = std::move(line_buffer_);
                        line_buffer_.clear();
                        state_ = S_URI;
                    } else if (state_ == S_URI) {
                        req_.uri = std::move(line_buffer_);
                        line_buffer_.clear();
                        state_ = S_PROTO;
                    } else {
                        state_ = S_ERROR;
                        return consumed;
                    }
                    ++consumed; // skip space
                } else if (term == '\r' || term == '\n') {
                    if (state_ == S_PROTO) {
                        req_.protocol = std::move(line_buffer_);
                        line_buffer_.clear();
                        state_ = S_HEADER_NAME;
                        ++consumed; // skip \r or \n
                        if (term == '\r' && consumed < len && data[consumed] == '\n') ++consumed;
                    } else {
                        state_ = S_ERROR;
                        return consumed;
                    }
                } else {
                    state_ = S_ERROR; return consumed;
                }
                break;
            }
            case S_HEADER_NAME: {
                // Read until ':' or '\r'
                size_t i = consumed;
                for (; i < len; ++i) {
                    uint8_t c = data[i];
                    if (c == ':' || c == '\r' || c == '\n') break;
                }
                if (i == len) {
                    line_buffer_.append((const char*)data + consumed, len - consumed);
                    return len;
                }
                line_buffer_.append((const char*)data + consumed, i - consumed);
                consumed = i;
                uint8_t t = data[consumed];
                if (t == ':') {
                    cur_header_name_ = trim_space(line_buffer_);
                    line_buffer_.clear();
                    state_ = S_HEADER_VALUE;
                    ++consumed;
                } else if (t == '\r' || t == '\n') {
                    // Empty header line = headers done
                    line_buffer_.clear();
                    state_ = S_HEADERS_DONE;
                    ++consumed;
                    if (t == '\r' && consumed < len && data[consumed] == '\n') ++consumed;
                } else {
                    state_ = S_ERROR; return consumed;
                }
                break;
            }
            case S_HEADER_VALUE: {
                size_t i = consumed;
                for (; i < len; ++i) {
                    uint8_t c = data[i];
                    if (c == '\r' || c == '\n') break;
                }
                if (i == len) {
                    line_buffer_.append((const char*)data + consumed, len - consumed);
                    return len;
                }
                line_buffer_.append((const char*)data + consumed, i - consumed);
                consumed = i;
                cur_header_value_ = trim_space(line_buffer_);
                line_buffer_.clear();
                if (!cur_header_name_.empty()) {
                    req_.headers[cur_header_name_] = cur_header_value_;
                }
                cur_header_name_.clear(); cur_header_value_.clear();
                state_ = S_HEADER_NAME;
                // skip \r\n
                uint8_t t = data[consumed++];
                if (t == '\r' && consumed < len && data[consumed] == '\n') ++consumed;
                break;
            }
            case S_HEADERS_DONE: {
                body_expected_ = (size_t)std::max<int64_t>(0, req_.content_length());
                if (body_expected_ == 0) {
                    state_ = S_COMPLETE;
                } else {
                    req_.body.reserve(body_expected_);
                    state_ = S_BODY;
                }
                break;
            }
            case S_BODY: {
                size_t need = body_expected_ - req_.body.size();
                size_t take = std::min(need, len - consumed);
                if (take > 0) {
                    req_.body.insert(req_.body.end(), data + consumed, data + consumed + take);
                    consumed += take;
                }
                if (req_.body.size() == body_expected_) state_ = S_COMPLETE;
                break;
            }
            default:
                return consumed;
        }
    }
    return consumed;
}

HttpRequest HttpRequestParser::take_request() {
    HttpRequest r = std::move(req_);
    state_ = S_METHOD;
    body_expected_ = 0;
    header_so_far_ = 0;
    line_buffer_.clear();
    cur_header_name_.clear();
    cur_header_value_.clear();
    req_ = HttpRequest{};
    return r;
}

HttpResponse make_rtsp_ok(int cseq, const HeaderMap& extra_headers,
                          std::vector<uint8_t> body, const std::string& content_type) {
    HttpResponse r;
    r.code = 200;
    r.reason = "OK";
    r.headers["CSeq"] = std::to_string(cseq);
    r.headers["Server"] = "airplay2lib/1.0";
    for (auto& [k, v] : extra_headers) r.headers[k] = v;
    if (!body.empty()) {
        if (!content_type.empty()) r.set_content_type(content_type);
        r.set_body(std::move(body));
    }
    return r;
}

} // namespace net
} // namespace airplay2
