/*!
 * @file platform_socket.h
 * @brief Cross-platform TCP/UDP socket abstraction
 */
#ifndef AIRPLAY2_PLATFORM_SOCKET_H
#define AIRPLAY2_PLATFORM_SOCKET_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace airplay2 {
namespace platform {

using socket_t = intptr_t;
constexpr socket_t INVALID_SOCKET_HANDLE = (socket_t)-1;

enum class SocketProtocol {
    TCP,
    UDP
};

/*!
 * @brief Socket address (IP + port)
 */
struct SocketAddr {
    std::string ip;    ///< IPv4 dotted or IPv6 hex
    uint16_t    port = 0;
    bool        is_v6 = false;
    std::string to_string() const;
    /// 简易有效性：ip 非空 且 端口非 0
    bool valid() const { return !ip.empty() && port != 0; }
};

/*!
 * @brief Socket option flags
 */
enum SocketOption {
    SOCK_OPT_REUSEADDR = 0,
    SOCK_OPT_REUSEPORT,
    SOCK_OPT_NONBLOCK,
    SOCK_OPT_NODELAY,   ///< TCP_NODELAY
    SOCK_OPT_MULTICAST_TTL,
    SOCK_OPT_MULTICAST_LOOP,
    SOCK_OPT_RCVBUF,
    SOCK_OPT_SNDBUF
};

/*!
 * @brief Result of send/recv operations
 */
struct IoResult {
    bool     ok = false;
    bool     would_block = false;   ///< For non-blocking sockets
    bool     disconnected = false;  ///< Peer closed (TCP)
    int64_t  bytes = 0;
};

/*!
 * @brief BSD-style socket wrapper with cross-platform init/cleanup
 */
class Socket {
public:
    Socket();
    ~Socket();
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    /// Create a socket of given protocol (AF_INET only for now)
    bool create(SocketProtocol proto, bool ipv6 = false);

    /// Bind to address
    bool bind(const std::string& ip, uint16_t port);

    /// Listen (TCP)
    bool listen(int backlog = 64);

    /// Accept a new TCP connection. Returns invalid socket on failure.
    Socket accept(SocketAddr* remote_addr = nullptr);

    /// UDP: sendto a specific address
    IoResult sendto(const void* data, size_t len, const SocketAddr& dst);

    /// UDP: 便捷 sendto（直接 ip 字符串 + 端口），内部构造 SocketAddr
    IoResult sendto(const void* data, size_t len, const std::string& ip, uint16_t port);

    /// UDP: recvfrom
    IoResult recvfrom(void* buf, size_t len, SocketAddr* src);

    /// TCP: send (stream)
    IoResult send(const void* data, size_t len);

    /// TCP: recv (stream)
    IoResult recv(void* buf, size_t len);

    /// Connect TCP socket to remote endpoint (blocking)
    bool connect(const SocketAddr& dst);

    /// Close the socket
    void close();

    /// Set socket options
    bool set_option(SocketOption opt, int value);
    bool get_option(SocketOption opt, int& value) const;

    /// Query local bind address
    bool getsockname(SocketAddr& out) const;

    /// Check handle validity
    bool valid() const { return h_ != INVALID_SOCKET_HANDLE; }
    socket_t handle() const { return h_; }

    /// Poll events (combination of POLLIN/POLLOUT/POLLERR bits)
    bool poll(int events, int timeout_ms, int& revents);

    static bool global_init();
    static void global_cleanup();

private:
    socket_t h_ = INVALID_SOCKET_HANDLE;
    SocketProtocol proto_ = SocketProtocol::TCP;
    bool is_v6_ = false;
};

/*!
 * @brief Wait on multiple sockets for readability (like select).
 *
 * @param sockets Input: sockets to watch
 * @param readable Output: indices into sockets that are readable
 * @param timeout_ms -1 for infinite
 * @return true on success, false on error
 */
bool select_read(const std::vector<Socket*>& sockets,
                 std::vector<size_t>& readable,
                 int timeout_ms = -1);

/*!
 * @brief Parse a host:port string; host can be name (uses DNS) or IP literal
 *
 * Resolves to IPv4 first; falls back to IPv6.
 */
bool resolve_host(const std::string& host_and_port, SocketAddr& out);

/*!
 * @brief Get first non-loopback IPv4 address of the machine (best effort)
 */
std::string get_local_ipv4();

/*!
 * @brief Convert a string IP to binary (for mDNS/TXT record checks etc.)
 */
bool parse_ipv4(const std::string& ip_str, uint8_t out[4]);

} // namespace platform
} // namespace airplay2

#endif // AIRPLAY2_PLATFORM_SOCKET_H
