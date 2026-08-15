/*!
 * @file http_server.cpp
 */
#include "http_server.h"
#include "../platform/platform_log.h"
#include "../platform/platform_time.h"
#include <algorithm>
#include <cstring>
#include <cctype>     // std::tolower（HTTP header 名大小写不敏感比较）
#include <exception>  // std::exception（连接处理线程的兜底捕获）

namespace airplay2 {
namespace net {

bool Connection::send(const HttpResponse& resp) {
    std::string s = resp.serialize();
    return send_raw((const uint8_t*)s.data(), s.size());
}

bool Connection::send_raw(const uint8_t* data, size_t len) {
    if (!sock_.valid()) return false;
    size_t written = 0;
    const size_t kDeadlineUs = 10000000ULL; // 10s
    uint64_t start = platform::time_now_us();
    while (written < len) {
        auto r = sock_.send(data + written, len - written);
        if (!r.ok) {
            if (r.would_block) {
                if (platform::time_now_us() - start > kDeadlineUs) return false;
                int revents = 0;
                sock_.poll(0x2, 50, revents); // wait for writable
                continue;
            }
            return false;
        }
        if (r.bytes == 0) return false;
        written += (size_t)r.bytes;
    }
    return true;
}

HttpServer::HttpServer() = default;
HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(const std::string& bind_address, uint16_t port) {
    stop();
    if (!listen_sock_.create(platform::SocketProtocol::TCP, false)) {
        AP2_LOGE("http: TCP socket create failed");
        return false;
    }
    listen_sock_.set_option(platform::SOCK_OPT_REUSEADDR, 1);
    listen_sock_.set_option(platform::SOCK_OPT_NODELAY, 1);
    if (!listen_sock_.bind(bind_address, port)) {
        AP2_LOGE("http: bind %s:%u failed", bind_address.c_str(), port);
        return false;
    }
    if (!listen_sock_.listen(128)) return false;
    platform::SocketAddr local;
    if (listen_sock_.getsockname(local)) port_ = local.port;
    else port_ = port;

    running_.store(true);
    accept_thread_.start([this] { accept_worker(); }, "ap2-http-ac");
    AP2_LOGI("http server listening on %s:%u", bind_address.c_str(), port_);
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;
    listen_sock_.close();
    accept_thread_.stop_and_join();
    std::vector<std::shared_ptr<Connection>> to_close;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : connections_) to_close.push_back(kv.second);
        connections_.clear();
    }
    for (auto& c : to_close) c->socket().close();
}

size_t HttpServer::connection_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return connections_.size();
}

void HttpServer::close_connection(uint64_t id) {
    std::shared_ptr<Connection> c;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = connections_.find(id);
        if (it != connections_.end()) { c = it->second; connections_.erase(it); }
    }
    if (c) c->socket().close();
}

void HttpServer::add_route(const std::string& method, const std::string& uri_prefix,
                           RequestHandler h, bool exact) {
    routes_.push_back({method, uri_prefix, exact, std::move(h)});
}

HttpResponse HttpServer::dispatch(const HttpRequest& req, Connection& conn) {
    for (auto& r : routes_) {
        if (!r.method.empty()) {
            bool match = false;
            if (r.method.size() == req.method.size()) {
                match = true;
                for (size_t i = 0; i < r.method.size(); ++i) {
                    if (std::tolower((unsigned char)r.method[i]) !=
                        std::tolower((unsigned char)req.method[i])) { match = false; break; }
                }
            }
            if (!match) continue;
        }
        bool uri_ok = false;
        if (r.exact) uri_ok = (req.uri == r.uri_prefix);
        else         uri_ok = (req.uri.compare(0, r.uri_prefix.size(), r.uri_prefix) == 0);
        if (!uri_ok) continue;
        return r.handler(req, conn);
    }
    if (default_handler_) return default_handler_(req, conn);
    HttpResponse r; r.code = 404; r.reason = "Not Found";
    r.headers["CSeq"] = std::to_string(req.cseq());
    r.headers["Server"] = "airplay2lib/1.0";
    return r;
}

void HttpServer::accept_worker() {
    while (running_.load()) {
        std::vector<platform::Socket*> socks = { &listen_sock_ };
        std::vector<size_t> ready;
        if (!platform::select_read(socks, ready, 200)) {
            if (!running_.load()) break;
            platform::sleep_ms(50);
            continue;
        }
        if (ready.empty()) continue;
        platform::SocketAddr addr;
        platform::Socket s = listen_sock_.accept(&addr);
        if (!s.valid()) continue;
        s.set_option(platform::SOCK_OPT_NODELAY, 1);
        s.set_option(platform::SOCK_OPT_NONBLOCK, 1);
        uint64_t id = next_conn_id_++;
        auto conn = std::make_shared<Connection>(id, std::move(s));
        conn->set_peer(addr);
        {
            std::lock_guard<std::mutex> lk(mu_);
            connections_[id] = conn;
        }
        AP2_LOGI("http: connection %lu from %s", (unsigned long)id, addr.to_string().c_str());
        // Detached worker thread per connection
        platform::Thread* t = new platform::Thread();
        std::weak_ptr<Connection> wconn = conn;
        HttpServer* self = this;
        t->start([self, wconn, t] {
            // 必须先 detach 再 delete：线程在自己体内析构 Thread 对象时，
            // 析构函数会 join() 自身线程 → self-join 异常 → 进程崩溃
            t->detach();
            if (auto c = wconn.lock()) self->connection_worker(c);
            delete t;
        }, "ap2-http-conn");
    }
}

void HttpServer::connection_worker(std::shared_ptr<Connection> conn) {
    uint8_t buf[8192];
    bool closed = false;
    // 整个连接处理包一层 try/catch：客户端可发送任意畸形数据，
    // 任何解析异常都不允许逃出线程（否则 libc++abi: terminating 直接崩进程）
    try {
    while (running_.load() && !closed) {
        std::vector<platform::Socket*> socks = { &conn->socket() };
        std::vector<size_t> ready;
        if (!platform::select_read(socks, ready, 500)) break;
        if (ready.empty()) continue;
        auto r = conn->socket().recv(buf, sizeof(buf));
        if (!r.ok && !r.would_block) break;
        if (r.disconnected) break;
        if (r.bytes > 0) {
            size_t consumed = conn->parser_.parse(buf, (size_t)r.bytes);
            if (consumed != (size_t)r.bytes) {
                // TODO: accumulate remainder; for now reset
                AP2_LOGW("http: partial parse, consumed=%zu bytes=%zu", consumed, (size_t)r.bytes);
            }
            (void)consumed;
            while (conn->parser_.is_complete()) {
                HttpRequest req = conn->parser_.take_request();
                AP2_LOGD("http[%lu]: %s %s %s",
                         (unsigned long)conn->id(),
                         req.method.c_str(), req.uri.c_str(), req.protocol.c_str());
                HttpResponse resp = dispatch(req, *conn);
                if (!conn->send(resp)) {
                    closed = true;
                    break;
                }
            }
            if (conn->parser_.has_error()) {
                AP2_LOGW("http[%lu]: parse error, closing", (unsigned long)conn->id());
                break;
            }
        }
    }
    // remove from map
    {
        std::lock_guard<std::mutex> lk(mu_);
        connections_.erase(conn->id());
    }
    AP2_LOGI("http: connection %lu closed", (unsigned long)conn->id());
    } catch (const std::exception& e) {
        // 记录异常来源，避免客户端输入导致进程崩溃
        AP2_LOGE("http: connection %lu handler exception: %s",
                 (unsigned long)conn->id(), e.what());
    } catch (...) {
        AP2_LOGE("http: connection %lu handler unknown exception",
                 (unsigned long)conn->id());
    }
}

} // namespace net
} // namespace airplay2
