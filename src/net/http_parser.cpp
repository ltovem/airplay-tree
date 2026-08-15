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
    // 跨增量喂入的 CRLF 处理：上一次单独吃 '\r'，本次开头 '\n' 要先跳过
    // 注意：即使 state_ 已 COMPLETE/ERROR，也要跳，否则背靠背 (back-to-back) 请求
    // 会在下一次 parse 开头留一个孤立 '\n'。
    if (skip_next_lf_ && len > 0 && data[0] == '\n') {
        consumed = 1;
        skip_next_lf_ = false;
    }
    // 注意：S_HEADERS_DONE 状态会不消耗字节，但需要切换到 BODY/COMPLETE/CHUNK_*，
    //       所以即便 consumed >= len 也要再跑一次（避免停在中间状态）。
    while (state_ != S_COMPLETE && state_ != S_ERROR &&
           (consumed < len || state_ == S_HEADERS_DONE)) {
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
                        if (term == '\r') {
                            if (consumed < len && data[consumed] == '\n') ++consumed;
                            else skip_next_lf_ = true;   // 等下次 parse 开头再跳
                        }
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
                    // 空行 = headers done. 之前 skip_next_lf_ 已经在 parse 开头处理了遗留 '\n'，
                    // 这里只要 line_buffer 为空 → 空分隔行，不管之前有无 header 条目
                    line_buffer_.clear();
                    state_ = S_HEADERS_DONE;
                    ++consumed;
                    if (t == '\r') {
                        if (consumed < len && data[consumed] == '\n') ++consumed;
                        else skip_next_lf_ = true;
                    }
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
                if (t == '\r') {
                    if (consumed < len && data[consumed] == '\n') ++consumed;
                    else skip_next_lf_ = true;
                }
                break;
            }
            case S_HEADERS_DONE: {
                body_expected_ = (size_t)std::max<int64_t>(0, req_.content_length());
                // Transfer-Encoding: chunked 优先级高于 Content-Length
                std::string te = req_.header("Transfer-Encoding");
                for (char& c : te) c = (char)std::tolower((unsigned char)c);
                body_is_chunked_ = (te.find("chunked") != std::string::npos);
                if (body_is_chunked_) {
                    // chunked：line_buffer_ 开始累积 size 行的十六进制字符
                    line_buffer_.clear();
                    chunk_left_ = 0;
                    state_ = S_CHUNK_SIZE;
                } else if (body_expected_ == 0) {
                    state_ = S_COMPLETE;
                } else {
                    req_.body.reserve(body_expected_);
                    state_ = S_BODY;
                }
                // 这个状态不消费任何字节；直接 continue，进入下一轮 while 处理新状态，
                // 否则外层 while 会因为 consumed==len 跳出导致停在 S_HEADERS_DONE。
                continue;
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
            // -------- chunked 解码状态机（RFC 7230 §4.1）-------------------
            //  简化：忽略 chunk-ext（";..."）、忽略 trailer 字段，
            //  对 AirPlay 2 实际包完全够用。
            //  状态序列：
            //   S_CHUNK_SIZE：逐字节累积到 line_buffer_，遇 \r 为止
            //   S_CHUNK_SIZE_CR → 期望 \n
            //   S_CHUNK_DATA：读 chunk_left_ 字节进 body
            //   S_CHUNK_DATA_CR / _LF：chunk 数据后 "\r\n"
            //   下一个 size 再回到 S_CHUNK_SIZE
            //   size=0 → S_CHUNK_TRAILER 扫到空行 → S_COMPLETE
            case S_CHUNK_SIZE: {
                while (consumed < len) {
                    uint8_t c = data[consumed];
                    if (c == '\r') { consumed++; state_ = S_CHUNK_SIZE_CR; break; }
                    if (c == '\n') { consumed++; break; }
                    line_buffer_.push_back((char)c);
                    consumed++;
                }
                if (state_ == S_CHUNK_SIZE_CR) break; // 下一轮处理 LF
                // '\n' 直接结束本行 → 解析 size
                // 或者遇到异常字符结束（比如空 line_buffer_）——先解析
                {
                    // 把 line_buffer_ 分号前的部分当作 hex
                    std::string& lb = line_buffer_;
                    size_t semi = lb.find(';');
                    std::string hex = (semi == std::string::npos) ? lb : lb.substr(0, semi);
                    chunk_left_ = 0;
                    for (char hc : hex) {
                        int v;
                        if (hc >= '0' && hc <= '9') v = hc - '0';
                        else if (hc >= 'a' && hc <= 'f') v = 10 + (hc - 'a');
                        else if (hc >= 'A' && hc <= 'F') v = 10 + (hc - 'A');
                        else { state_ = S_ERROR; return consumed; }
                        chunk_left_ = (chunk_left_ << 4) | (size_t)v;
                    }
                    lb.clear();
                    if (chunk_left_ == 0) {
                        // terminal chunk → 扫 trailer，空行即结束
                        state_ = S_CHUNK_TRAILER;
                        line_buffer_.clear();
                    } else {
                        state_ = S_CHUNK_DATA;
                    }
                }
                break;
            }
            case S_CHUNK_SIZE_CR: {
                if (consumed >= len) break;
                if (data[consumed] != '\n') { state_ = S_ERROR; return consumed; }
                consumed++;
                // 现在解析 line_buffer_ 中累积的 hex
                {
                    std::string& lb = line_buffer_;
                    size_t semi = lb.find(';');
                    std::string hex = (semi == std::string::npos) ? lb : lb.substr(0, semi);
                    chunk_left_ = 0;
                    for (char hc : hex) {
                        int v;
                        if (hc >= '0' && hc <= '9') v = hc - '0';
                        else if (hc >= 'a' && hc <= 'f') v = 10 + (hc - 'a');
                        else if (hc >= 'A' && hc <= 'F') v = 10 + (hc - 'A');
                        else { state_ = S_ERROR; return consumed; }
                        chunk_left_ = (chunk_left_ << 4) | (size_t)v;
                    }
                    lb.clear();
                    if (chunk_left_ == 0) {
                        state_ = S_CHUNK_TRAILER;
                        line_buffer_.clear();
                    } else {
                        state_ = S_CHUNK_DATA;
                    }
                }
                break;
            }
            case S_CHUNK_DATA: {
                size_t take = std::min(chunk_left_, len - consumed);
                if (take > 0) {
                    req_.body.insert(req_.body.end(), data + consumed, data + consumed + take);
                    consumed += take;
                    chunk_left_ -= take;
                }
                if (chunk_left_ == 0) state_ = S_CHUNK_DATA_CR;
                break;
            }
            case S_CHUNK_DATA_CR: {
                if (consumed >= len) break;
                if (data[consumed] != '\r') { state_ = S_ERROR; return consumed; }
                consumed++;
                state_ = S_CHUNK_DATA_LF;
                break;
            }
            case S_CHUNK_DATA_LF: {
                if (consumed >= len) break;
                if (data[consumed] != '\n') { state_ = S_ERROR; return consumed; }
                consumed++;
                // 下一个 chunk 从 size 行开始
                line_buffer_.clear();
                state_ = S_CHUNK_SIZE;
                break;
            }
            case S_CHUNK_TRAILER: {
                // 读行直到空行；AirPlay 一般直接空行
                // 逐字符累加到 line_buffer_，遇到 \r\n 检查是否空
                while (consumed < len) {
                    uint8_t c = data[consumed++];
                    if (c == '\r') {
                        if (consumed < len && data[consumed] == '\n') consumed++;
                        if (line_buffer_.empty()) {
                            state_ = S_COMPLETE;
                        } else {
                            // 一条 trailer header（忽略）
                            line_buffer_.clear();
                        }
                        break;
                    } else if (c == '\n') {
                        if (line_buffer_.empty()) state_ = S_COMPLETE;
                        else line_buffer_.clear();
                        break;
                    } else {
                        line_buffer_.push_back((char)c);
                    }
                }
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
    body_is_chunked_ = false;
    chunk_left_ = 0;
    header_so_far_ = 0;
    skip_next_lf_ = false;
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
