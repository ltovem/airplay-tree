/*!
 * @file http_server.h
 * @brief TCP HTTP/RTSP server with per-connection request dispatch
 */
#ifndef AIRPLAY2_HTTP_SERVER_H
#define AIRPLAY2_HTTP_SERVER_H

#include "http_parser.h"
#include "../platform/platform_socket.h"
#include "../platform/platform_thread.h"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace airplay2 {
namespace net {

class Connection;

using RequestHandler = std::function<HttpResponse(const HttpRequest&,
                                                  Connection& conn)>;
/*!
 * @brief Represents one client connection to the HTTP/RTSP server
 */
class Connection {
public:
    Connection(uint64_t id, platform::Socket sock)
        : id_(id), sock_(std::move(sock)) {}

    uint64_t id() const { return id_; }
    platform::Socket& socket() { return sock_; }

    std::string peer_ip() const { return peer_addr_.ip; }
    uint16_t    peer_port() const { return peer_addr_.port; }
    void        set_peer(const platform::SocketAddr& a) { peer_addr_ = a; }

    /// Store arbitrary user data per connection (session state)
    void  set_user_data(void* p) { user_data_ = p; }
    void* user_data() const { return user_data_; }

    /// Send a response (synchronously on the connection thread)
    bool send(const HttpResponse& resp);

    /// Send raw bytes (for custom protocols, e.g., event streams)
    bool send_raw(const uint8_t* data, size_t len);

private:
    friend class HttpServer;
    uint64_t id_;
    platform::Socket sock_;
    platform::SocketAddr peer_addr_;
    HttpRequestParser parser_;
    std::vector<uint8_t> recv_buf_;
    void* user_data_ = nullptr;
};

/*!
 * @brief Multi-session HTTP/RTSP server
 */
class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    /// Start listening on bind_address:port. Returns false on bind failure.
    bool start(const std::string& bind_address, uint16_t port);

    /// Stop server, disconnect all connections
    void stop();

    bool is_running() const { return running_.load(); }
    uint16_t port() const { return port_; }

    /// Register the default handler invoked for every incoming request
    void set_default_handler(RequestHandler h) { default_handler_ = std::move(h); }

    /// Register handler by method+URI prefix match (exact if exact=true)
    void add_route(const std::string& method, const std::string& uri_prefix,
                   RequestHandler h, bool exact = false);

    /// Get current connection count
    size_t connection_count() const;

    /// Force-close a connection by ID
    void close_connection(uint64_t id);

private:
    struct Route {
        std::string method;
        std::string uri_prefix;
        bool exact;
        RequestHandler handler;
    };
    void accept_worker();
    void connection_worker(std::shared_ptr<Connection> conn);
    HttpResponse dispatch(const HttpRequest& req, Connection& conn);

    std::atomic<bool> running_{false};
    platform::Socket listen_sock_;
    platform::Thread accept_thread_;
    uint16_t port_ = 0;
    std::atomic<uint64_t> next_conn_id_{1};

    RequestHandler default_handler_;
    std::vector<Route> routes_;
    mutable std::mutex mu_;
    std::map<uint64_t, std::shared_ptr<Connection>> connections_;
};

} // namespace net
} // namespace airplay2

#endif // AIRPLAY2_HTTP_SERVER_H
