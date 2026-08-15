/*!
 * @file platform_socket.cpp
 * @brief Cross-platform socket implementation
 */
#include "platform_socket.h"
#include "platform_log.h"

#include <cstring>
#include <sstream>

#if AP2_PLATFORM_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <iphlpapi.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "iphlpapi.lib")
    using ssize_t = SSIZE_T;
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/ioctl.h>
    #include <sys/select.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <netdb.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <cerrno>
    #include <cstring>
    #if AP2_PLATFORM_LINUX
        #include <linux/netlink.h>
        #include <linux/rtnetlink.h>
        #include <ifaddrs.h>
        #include <net/if.h>
    #elif AP2_PLATFORM_MACOS || AP2_PLATFORM_IOS || AP2_PLATFORM_ANDROID
        // Android API 24+ 才有 getifaddrs；低版本走 fallback 分支
        #include <ifaddrs.h>
        #include <net/if.h>
    #endif
#endif

namespace airplay2 {
namespace platform {

static inline int get_last_error() {
#if AP2_PLATFORM_WINDOWS
    return (int)WSAGetLastError();
#else
    return errno;
#endif
}

static inline bool is_would_block(int err) {
#if AP2_PLATFORM_WINDOWS
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

// ---- SocketAddr ----
std::string SocketAddr::to_string() const {
    std::ostringstream oss;
    if (is_v6) oss << "[";
    oss << ip;
    if (is_v6) oss << "]";
    oss << ":" << port;
    return oss.str();
}

// ---- Socket lifecycle ----
Socket::Socket() = default;
Socket::~Socket() { close(); }

Socket::Socket(Socket&& o) noexcept
    : h_(o.h_), proto_(o.proto_), is_v6_(o.is_v6_) {
    o.h_ = INVALID_SOCKET_HANDLE;
}
Socket& Socket::operator=(Socket&& o) noexcept {
    if (this != &o) {
        close();
        h_ = o.h_; proto_ = o.proto_; is_v6_ = o.is_v6_;
        o.h_ = INVALID_SOCKET_HANDLE;
    }
    return *this;
}

bool Socket::global_init() {
#if AP2_PLATFORM_WINDOWS
    WSADATA wd;
    int r = WSAStartup(MAKEWORD(2, 2), &wd);
    if (r != 0) {
        AP2_LOGE("WSAStartup failed: %d", r);
        return false;
    }
    if (LOBYTE(wd.wVersion) != 2 || HIBYTE(wd.wVersion) != 2) {
        AP2_LOGW("Winsock version not exactly 2.2");
    }
#endif
    return true;
}

void Socket::global_cleanup() {
#if AP2_PLATFORM_WINDOWS
    WSACleanup();
#endif
}

void Socket::close() {
    if (h_ == INVALID_SOCKET_HANDLE) return;
#if AP2_PLATFORM_WINDOWS
    ::closesocket((SOCKET)h_);
#else
    ::close((int)h_);
#endif
    h_ = INVALID_SOCKET_HANDLE;
}

bool Socket::create(SocketProtocol proto, bool ipv6) {
    close();
    int domain  = ipv6 ? AF_INET6 : AF_INET;
    int type    = (proto == SocketProtocol::TCP) ? SOCK_STREAM : SOCK_DGRAM;
    int ptype   = (proto == SocketProtocol::TCP) ? IPPROTO_TCP  : IPPROTO_UDP;
    socket_t s = (socket_t)::socket(domain, type, ptype);
    if (s == INVALID_SOCKET_HANDLE) {
        AP2_LOGE("socket() failed: err=%d", get_last_error());
        return false;
    }
    h_ = s;
    proto_ = proto;
    is_v6_ = ipv6;
    return true;
}

static sockaddr_storage make_sockaddr(const std::string& ip, uint16_t port, bool ipv6, int& out_len) {
    sockaddr_storage ss{};
    memset(&ss, 0, sizeof(ss));
    if (ipv6) {
        sockaddr_in6& a = *(sockaddr_in6*)&ss;
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(port);
        if (ip.empty() || ip == "::") {
            a.sin6_addr = in6addr_any;
        } else {
#if AP2_PLATFORM_WINDOWS
            InetPtonA(AF_INET6, ip.c_str(), &a.sin6_addr);
#else
            inet_pton(AF_INET6, ip.c_str(), &a.sin6_addr);
#endif
        }
        out_len = (int)sizeof(sockaddr_in6);
    } else {
        sockaddr_in& a = *(sockaddr_in*)&ss;
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        if (ip.empty() || ip == "0.0.0.0") {
            a.sin_addr.s_addr = INADDR_ANY;
        } else {
#if AP2_PLATFORM_WINDOWS
            InetPtonA(AF_INET, ip.c_str(), &a.sin_addr);
#else
            inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
#endif
        }
        out_len = (int)sizeof(sockaddr_in);
    }
    return ss;
}

bool Socket::bind(const std::string& ip, uint16_t port) {
    if (!valid()) return false;
    int addr_len = 0;
    sockaddr_storage ss = make_sockaddr(ip, port, is_v6_, addr_len);
    int r = ::bind((int)h_, (sockaddr*)&ss, addr_len);
    if (r != 0) {
        AP2_LOGE("bind(%s:%u) failed: err=%d", ip.c_str(), port, get_last_error());
        return false;
    }
    return true;
}

bool Socket::listen(int backlog) {
    if (!valid() || proto_ != SocketProtocol::TCP) return false;
    int r = ::listen((int)h_, backlog);
    if (r != 0) {
        AP2_LOGE("listen() failed: err=%d", get_last_error());
        return false;
    }
    return true;
}

Socket Socket::accept(SocketAddr* remote_addr) {
    Socket out;
    if (!valid() || proto_ != SocketProtocol::TCP) return out;
    sockaddr_storage ss{};
    socklen_t sl = (socklen_t)sizeof(ss);
    socket_t s = (socket_t)::accept((int)h_, (sockaddr*)&ss, &sl);
    if (s == INVALID_SOCKET_HANDLE) return out;
    out.h_ = s;
    out.proto_ = SocketProtocol::TCP;
    out.is_v6_ = (ss.ss_family == AF_INET6);
    if (remote_addr) {
        char buf[INET6_ADDRSTRLEN] = {0};
        if (out.is_v6_) {
            sockaddr_in6& a = *(sockaddr_in6*)&ss;
#if AP2_PLATFORM_WINDOWS
            InetNtopA(AF_INET6, &a.sin6_addr, buf, sizeof(buf));
#else
            inet_ntop(AF_INET6, &a.sin6_addr, buf, sizeof(buf));
#endif
            remote_addr->ip = buf;
            remote_addr->port = ntohs(a.sin6_port);
        } else {
            sockaddr_in& a = *(sockaddr_in*)&ss;
#if AP2_PLATFORM_WINDOWS
            InetNtopA(AF_INET, &a.sin_addr, buf, sizeof(buf));
#else
            inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
#endif
            remote_addr->ip = buf;
            remote_addr->port = ntohs(a.sin_port);
        }
        remote_addr->is_v6 = out.is_v6_;
    }
    return out;
}

IoResult Socket::sendto(const void* data, size_t len, const SocketAddr& dst) {
    IoResult r;
    if (!valid() || proto_ != SocketProtocol::UDP) return r;
    int al = 0;
    sockaddr_storage ss = make_sockaddr(dst.ip, dst.port, dst.is_v6, al);
    ssize_t n = ::sendto((int)h_, (const char*)data, (int)len, 0, (sockaddr*)&ss, al);
    if (n < 0) {
        int e = get_last_error();
        r.would_block = is_would_block(e);
        return r;
    }
    r.ok = true;
    r.bytes = n;
    return r;
}

IoResult Socket::recvfrom(void* buf, size_t len, SocketAddr* src) {
    IoResult r;
    if (!valid() || proto_ != SocketProtocol::UDP) return r;
    sockaddr_storage ss{};
    socklen_t sl = (socklen_t)sizeof(ss);
    ssize_t n = ::recvfrom((int)h_, (char*)buf, (int)len, 0, (sockaddr*)&ss, &sl);
    if (n < 0) {
        int e = get_last_error();
        r.would_block = is_would_block(e);
        return r;
    }
    if (n == 0) { r.disconnected = true; r.ok = true; return r; }
    r.ok = true; r.bytes = n;
    if (src) {
        bool v6 = (ss.ss_family == AF_INET6);
        char strbuf[INET6_ADDRSTRLEN] = {0};
        if (v6) {
            sockaddr_in6& a = *(sockaddr_in6*)&ss;
#if AP2_PLATFORM_WINDOWS
            InetNtopA(AF_INET6, &a.sin6_addr, strbuf, sizeof(strbuf));
#else
            inet_ntop(AF_INET6, &a.sin6_addr, strbuf, sizeof(strbuf));
#endif
            src->port = ntohs(a.sin6_port);
        } else {
            sockaddr_in& a = *(sockaddr_in*)&ss;
#if AP2_PLATFORM_WINDOWS
            InetNtopA(AF_INET, &a.sin_addr, strbuf, sizeof(strbuf));
#else
            inet_ntop(AF_INET, &a.sin_addr, strbuf, sizeof(strbuf));
#endif
            src->port = ntohs(a.sin_port);
        }
        src->ip = strbuf;
        src->is_v6 = v6;
    }
    return r;
}

IoResult Socket::send(const void* data, size_t len) {
    IoResult r;
    if (!valid()) return r;
    ssize_t n = ::send((int)h_, (const char*)data, (int)len, 0);
    if (n < 0) {
        int e = get_last_error();
        r.would_block = is_would_block(e);
        return r;
    }
    r.ok = true; r.bytes = n;
    return r;
}

IoResult Socket::recv(void* buf, size_t len) {
    IoResult r;
    if (!valid()) return r;
    ssize_t n = ::recv((int)h_, (char*)buf, (int)len, 0);
    if (n < 0) {
        int e = get_last_error();
        r.would_block = is_would_block(e);
        return r;
    }
    if (n == 0) { r.disconnected = true; r.ok = true; return r; }
    r.ok = true; r.bytes = n;
    return r;
}

bool Socket::connect(const SocketAddr& dst) {
    if (!valid()) return false;
    int al = 0;
    sockaddr_storage ss = make_sockaddr(dst.ip, dst.port, dst.is_v6, al);
    int r = ::connect((int)h_, (sockaddr*)&ss, al);
    if (r != 0) {
        AP2_LOGE("connect(%s) failed: err=%d", dst.to_string().c_str(), get_last_error());
        return false;
    }
    return true;
}

// 设置非阻塞：平台函数实现（避免宏和复合字面量问题）
#if AP2_PLATFORM_WINDOWS
static bool set_nonblock_impl(socket_t h, bool enable) {
    u_long mode = enable ? 1UL : 0UL;
    return ioctlsocket((SOCKET)h, FIONBIO, &mode) == 0;
}
#else
static bool set_nonblock_impl(socket_t h, bool enable) {
    int flags = fcntl((int)h, F_GETFL, 0);
    if (flags < 0) return false;
    if (enable) flags |= O_NONBLOCK;
    else        flags &= ~O_NONBLOCK;
    return fcntl((int)h, F_SETFL, flags) == 0;
}
#endif

bool Socket::set_option(SocketOption opt, int value) {
    if (!valid()) return false;
    int level = SOL_SOCKET;
    int name  = 0;
    const void* pval = &value;
    int vlen = sizeof(value);

    switch (opt) {
        case SOCK_OPT_REUSEADDR: {
#if AP2_PLATFORM_WINDOWS
            // SO_REUSEADDR on Windows has semantics of SO_REUSEPORT on Unix
            BOOL v = (value != 0) ? TRUE : FALSE;
            pval = &v; vlen = sizeof(v);
#else
            int v = value ? 1 : 0;
            pval = &v; vlen = sizeof(v);
#endif
            name = SO_REUSEADDR;
            break;
        }
        case SOCK_OPT_REUSEPORT: {
#if defined(SO_REUSEPORT)
            int v = value ? 1 : 0;
            pval = &v; vlen = sizeof(v);
            name = SO_REUSEPORT;
#else
            // Fallback: reuse SO_REUSEADDR behavior where not available
            return set_option(SOCK_OPT_REUSEADDR, value);
#endif
            break;
        }
        case SOCK_OPT_NONBLOCK:
            return set_nonblock_impl(h_, value != 0);

        case SOCK_OPT_NODELAY: {
            level = IPPROTO_TCP;
#if AP2_PLATFORM_WINDOWS
            BOOL v = (value != 0) ? TRUE : FALSE;
            pval = &v; vlen = sizeof(v);
#else
            int v = value ? 1 : 0;
            pval = &v; vlen = sizeof(v);
#endif
            name = TCP_NODELAY;
            break;
        }
        case SOCK_OPT_MULTICAST_TTL: {
            level = IPPROTO_IP;
            unsigned char v = (unsigned char)value;
            pval = &v; vlen = sizeof(v);
            name = IP_MULTICAST_TTL;
            break;
        }
        case SOCK_OPT_MULTICAST_LOOP: {
            level = IPPROTO_IP;
            unsigned char v = (unsigned char)value;
            pval = &v; vlen = sizeof(v);
            name = IP_MULTICAST_LOOP;
            break;
        }
        case SOCK_OPT_RCVBUF:
            name = SO_RCVBUF; break;
        case SOCK_OPT_SNDBUF:
            name = SO_SNDBUF; break;
        default:
            return false;
    }

    return ::setsockopt((int)h_, level, name, (const char*)pval, vlen) == 0;
}

bool Socket::get_option(SocketOption opt, int& value) const {
    if (!valid()) return false;
    if (opt == SOCK_OPT_NONBLOCK) return false; // not a setsockopt option
    int level = SOL_SOCKET;
    int name  = 0;
    int v;
    switch (opt) {
        case SOCK_OPT_REUSEADDR:    name = SO_REUSEADDR; break;
        case SOCK_OPT_REUSEPORT:
#if defined(SO_REUSEPORT)
            name = SO_REUSEPORT;
#else
            return false;
#endif
            break;
        case SOCK_OPT_NODELAY:      level = IPPROTO_TCP; name = TCP_NODELAY; break;
        case SOCK_OPT_MULTICAST_TTL: level = IPPROTO_IP; name = IP_MULTICAST_TTL; break;
        case SOCK_OPT_MULTICAST_LOOP: level = IPPROTO_IP; name = IP_MULTICAST_LOOP; break;
        case SOCK_OPT_RCVBUF: name = SO_RCVBUF; break;
        case SOCK_OPT_SNDBUF: name = SO_SNDBUF; break;
        default: return false;
    }
    socklen_t sl = (socklen_t)sizeof(v);
    if (::getsockopt((int)h_, level, name, (char*)&v, &sl) != 0) return false;
    value = v;
    return true;
}

bool Socket::getsockname(SocketAddr& out) const {
    if (!valid()) return false;
    sockaddr_storage ss{};
    socklen_t sl = (socklen_t)sizeof(ss);
    if (::getsockname((int)h_, (sockaddr*)&ss, &sl) != 0) return false;
    bool v6 = (ss.ss_family == AF_INET6);
    char buf[INET6_ADDRSTRLEN] = {0};
    if (v6) {
        sockaddr_in6& a = *(sockaddr_in6*)&ss;
#if AP2_PLATFORM_WINDOWS
        InetNtopA(AF_INET6, &a.sin6_addr, buf, sizeof(buf));
#else
        inet_ntop(AF_INET6, &a.sin6_addr, buf, sizeof(buf));
#endif
        out.port = ntohs(a.sin6_port);
    } else {
        sockaddr_in& a = *(sockaddr_in*)&ss;
#if AP2_PLATFORM_WINDOWS
        InetNtopA(AF_INET, &a.sin_addr, buf, sizeof(buf));
#else
        inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
#endif
        out.port = ntohs(a.sin_port);
    }
    out.ip = buf;
    out.is_v6 = v6;
    return true;
}

#if AP2_PLATFORM_WINDOWS
    #define AP2_FD(s)  ((SOCKET)(s))
    #define AP2_FD_CAST SOCKET
#else
    #define AP2_FD(s)  ((int)(s))
    #define AP2_FD_CAST int
#endif

bool Socket::poll(int events, int timeout_ms, int& revents) {
    // Simple implementation using select() for portability
    fd_set rfds, wfds, efds;
    FD_ZERO(&rfds); FD_ZERO(&wfds); FD_ZERO(&efds);
    AP2_FD_CAST fh = (AP2_FD_CAST)h_;
    if (events & 0x1) FD_SET(fh, &rfds); // read
    if (events & 0x2) FD_SET(fh, &wfds); // write
    FD_SET(fh, &efds);
    timeval tv;
    timeval* ptv = nullptr;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }
    int maxfd = (int)h_ + 1;
    int r = ::select(maxfd, &rfds, &wfds, &efds, ptv);
    if (r < 0) return false;
    revents = 0;
    if (FD_ISSET(fh, &rfds)) revents |= 0x1;
    if (FD_ISSET(fh, &wfds)) revents |= 0x2;
    if (FD_ISSET(fh, &efds)) revents |= 0x4;
    return true;
}

bool select_read(const std::vector<Socket*>& sockets,
                 std::vector<size_t>& readable,
                 int timeout_ms) {
    readable.clear();
    if (sockets.empty()) return true;
    fd_set rfds;
    FD_ZERO(&rfds);
    int maxfd = -1;
    for (auto* s : sockets) {
        if (!s || !s->valid()) continue;
        AP2_FD_CAST fd = (AP2_FD_CAST)s->handle();
        FD_SET(fd, &rfds);
        if ((int)fd > maxfd) maxfd = (int)fd;
    }
    if (maxfd < 0) return false;
    timeval tv;
    timeval* ptv = nullptr;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }
    int r = ::select(maxfd + 1, &rfds, nullptr, nullptr, ptv);
    if (r < 0) return false;
    for (size_t i = 0; i < sockets.size(); ++i) {
        auto* s = sockets[i];
        if (!s || !s->valid()) continue;
        AP2_FD_CAST fd = (AP2_FD_CAST)s->handle();
        if (FD_ISSET(fd, &rfds)) readable.push_back(i);
    }
    return true;
}

bool resolve_host(const std::string& host_and_port, SocketAddr& out) {
    std::string host;
    int port = 0;
    // Split host:port
    size_t colon = host_and_port.rfind(':');
    if (colon == std::string::npos) return false;
    host = host_and_port.substr(0, colon);
    try { port = std::stoi(host_and_port.substr(colon + 1)); } catch (...) { return false; }

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int rr = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rr != 0 || !res) {
        AP2_LOGE("getaddrinfo(%s) failed: %d", host_and_port.c_str(), rr);
        return false;
    }
    bool ok = false;
    for (addrinfo* p = res; p; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            sockaddr_in& a = *(sockaddr_in*)p->ai_addr;
            char buf[INET_ADDRSTRLEN] = {0};
#if AP2_PLATFORM_WINDOWS
            InetNtopA(AF_INET, &a.sin_addr, buf, sizeof(buf));
#else
            inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
#endif
            out.ip = buf; out.port = ntohs(a.sin_port); out.is_v6 = false;
            ok = true; break;
        }
    }
    if (!ok && res) {
        addrinfo* p = res;
        if (p->ai_family == AF_INET6) {
            sockaddr_in6& a = *(sockaddr_in6*)p->ai_addr;
            char buf[INET6_ADDRSTRLEN] = {0};
#if AP2_PLATFORM_WINDOWS
            InetNtopA(AF_INET6, &a.sin6_addr, buf, sizeof(buf));
#else
            inet_ntop(AF_INET6, &a.sin6_addr, buf, sizeof(buf));
#endif
            out.ip = buf; out.port = ntohs(a.sin6_port); out.is_v6 = true;
            ok = true;
        }
    }
    freeaddrinfo(res);
    return ok;
}

bool parse_ipv4(const std::string& ip_str, uint8_t out[4]) {
    sockaddr_in sa{};
#if AP2_PLATFORM_WINDOWS
    if (InetPtonA(AF_INET, ip_str.c_str(), &sa.sin_addr) != 1) return false;
#else
    if (inet_pton(AF_INET, ip_str.c_str(), &sa.sin_addr) != 1) return false;
#endif
    uint32_t ip = ntohl(sa.sin_addr.s_addr);
    out[0] = (ip >> 24) & 0xFF;
    out[1] = (ip >> 16) & 0xFF;
    out[2] = (ip >> 8) & 0xFF;
    out[3] = ip & 0xFF;
    return true;
}

std::string get_local_ipv4() {
#if AP2_PLATFORM_WINDOWS
    ULONG size = 15000;
    std::vector<uint8_t> buf(size);
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX;
    DWORD ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                     (PIP_ADAPTER_ADDRESSES)buf.data(), &size);
    if (ret != ERROR_SUCCESS) return "127.0.0.1";
    for (PIP_ADAPTER_ADDRESSES p = (PIP_ADAPTER_ADDRESSES)buf.data(); p; p = p->Next) {
        if (p->OperStatus != IfOperStatusUp) continue;
        if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (PIP_ADAPTER_UNICAST_ADDRESS ua = p->FirstUnicastAddress; ua; ua = ua->Next) {
            sockaddr_in* sin = (sockaddr_in*)ua->Address.lpSockaddr;
            char strbuf[INET_ADDRSTRLEN] = {0};
            InetNtopA(AF_INET, &sin->sin_addr, strbuf, sizeof(strbuf));
            std::string ip = strbuf;
            if (ip == "127.0.0.1") continue;
            return ip;
        }
    }
    return "127.0.0.1";
#elif AP2_PLATFORM_MACOS || AP2_PLATFORM_IOS || AP2_PLATFORM_LINUX || AP2_PLATFORM_ANDROID
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return "127.0.0.1";
    std::string best = "127.0.0.1";
    for (ifaddrs* p = ifs; p; p = p->ifa_next) {
        if (!p->ifa_addr) continue;
        if (p->ifa_addr->sa_family != AF_INET) continue;
        if (p->ifa_flags & IFF_LOOPBACK) continue;
        sockaddr_in* sa = (sockaddr_in*)p->ifa_addr;
        char strbuf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &sa->sin_addr, strbuf, sizeof(strbuf));
        std::string ip = strbuf;
        if (ip == "127.0.0.1") continue;
        best = ip;
        // Prefer 192.168.x.x or 10.x.x.x (private / local LAN)
        if (ip.compare(0, 7, "192.168") == 0 || ip.compare(0, 3, "10.") == 0) {
            freeifaddrs(ifs);
            return ip;
        }
    }
    freeifaddrs(ifs);
    return best;
#else
    return "127.0.0.1";
#endif
}

} // namespace platform
} // namespace airplay2
