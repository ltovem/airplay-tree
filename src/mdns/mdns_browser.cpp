/*!
 * @file mdns_browser.cpp
 */
#include "mdns_browser.h"
#include "platform/platform_log.h"
#include "platform/platform_time.h"
#include <cstring>
#include <cinttypes>
#include <arpa/inet.h>

#if AP2_PLATFORM_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
#endif

namespace airplay2 {

namespace {
    constexpr const char* kMcastV4 = "224.0.0.251";
    constexpr uint16_t kMcastPort = 5353;

    uint16_t rd_u16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
    uint32_t rd_u32(const uint8_t* p) {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
    }

    size_t parse_name(const uint8_t* pkt, size_t pkt_size, size_t offset,
                      std::string& out) {
        out.clear();
        size_t cur = offset, consumed = 0;
        bool jumped = false;
        int depth = 0;
        while (cur < pkt_size && depth++ < 20) {
            uint8_t len = pkt[cur];
            if (len == 0) {
                if (!jumped) consumed = cur - offset + 1;
                return consumed ? consumed : cur - offset + 1;
            }
            if ((len & 0xC0) == 0xC0) {
                if (cur + 1 >= pkt_size) return 0;
                size_t ptr = (size_t(len & 0x3F) << 8) | pkt[cur + 1];
                if (!jumped) consumed = cur - offset + 2;
                jumped = true;
                cur = ptr;
                continue;
            }
            if (len & 0xC0) return 0;
            if (cur + 1 + len > pkt_size) return 0;
            if (!out.empty()) out.push_back('.');
            out.append((const char*)pkt + cur + 1, len);
            cur += 1 + len;
        }
        return 0;
    }

    std::vector<uint8_t> build_query(const std::string& type_domain) {
        std::vector<uint8_t> pkt;
        pkt.resize(12, 0);
        // header: id=random, flags=0x0100 (standard query), 1 question
        static uint16_t s_id = 0x1234;
        pkt[0] = (s_id >> 8) & 0xFF; pkt[1] = s_id & 0xFF;
        s_id++;
        pkt[2] = 0x01; pkt[3] = 0x00; // standard query
        pkt[4] = 0; pkt[5] = 1;       // 1 question
        // QNAME
        size_t pos = 0;
        std::string name = type_domain;
        if (!name.empty() && name.back() == '.') name.pop_back();
        while (pos < name.size()) {
            size_t dot = name.find('.', pos);
            if (dot == std::string::npos) dot = name.size();
            size_t len = dot - pos;
            pkt.push_back((uint8_t)len);
            pkt.insert(pkt.end(), name.begin() + (long)pos, name.begin() + (long)dot);
            pos = dot + 1;
        }
        pkt.push_back(0);
        pkt.push_back(0); pkt.push_back(12);  // QTYPE PTR
        pkt.push_back(0); pkt.push_back(1);   // QCLASS IN
        return pkt;
    }
} // anon

MdnsBrowser::MdnsBrowser() = default;
MdnsBrowser::~MdnsBrowser() { stop(); }

bool MdnsBrowser::start(DeviceDiscoveredCb cb) {
    stop();
    cb_ = std::move(cb);
    if (!sock4_.create(platform::SocketProtocol::UDP, false)) return false;
    sock4_.set_option(platform::SOCK_OPT_REUSEADDR, 1);
    sock4_.set_option(platform::SOCK_OPT_REUSEPORT, 1);
    if (!sock4_.bind("0.0.0.0", kMcastPort)) {
        if (!sock4_.bind("0.0.0.0", 0)) {
            AP2_LOGE("mdns browser: bind failed");
            return false;
        }
    }
#if AP2_PLATFORM_WINDOWS
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(kMcastV4);
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt((SOCKET)sock4_.handle(), IPPROTO_IP, IP_ADD_MEMBERSHIP,
               (const char*)&mreq, sizeof(mreq));
#else
    ip_mreq mreq{};
    inet_pton(AF_INET, kMcastV4, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt((int)sock4_.handle(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
#endif
    sock4_.set_option(platform::SOCK_OPT_MULTICAST_TTL, 255);
    running_.store(true);
    last_query_us_ = 0;
    worker_.start([this] { worker(); }, "ap2-mdns-br");
    return true;
}

void MdnsBrowser::stop() {
    running_.store(false);
    worker_.stop_and_join();
    sock4_.close();
}

std::vector<DiscoveredAirPlayDevice> MdnsBrowser::devices() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<DiscoveredAirPlayDevice> out;
    out.reserve(devices_.size());
    for (auto& kv : devices_) out.push_back(kv.second);
    return out;
}

void MdnsBrowser::worker() {
    AP2_LOGI("mdns browser: started");
    uint8_t buf[4096];
    while (running_.load()) {
        uint64_t now = platform::time_now_us();
        if (now - last_query_us_ >= 5000000ULL) { // every 5s
            auto q1 = build_query("_airplay._tcp.local");
            auto q2 = build_query("_raop._tcp.local");
            platform::SocketAddr dst; dst.ip = kMcastV4; dst.port = kMcastPort;
            sock4_.sendto(q1.data(), q1.size(), dst);
            sock4_.sendto(q2.data(), q2.size(), dst);
            last_query_us_ = now;
        }
        std::vector<platform::Socket*> socks = { &sock4_ };
        std::vector<size_t> ready;
        if (!platform::select_read(socks, ready, 250)) { platform::sleep_ms(50); continue; }
        if (ready.empty()) continue;
        platform::SocketAddr from;
        auto r = sock4_.recvfrom(buf, sizeof(buf), &from);
        if (r.ok && r.bytes > 12) {
            parse_response(buf, (size_t)r.bytes);
        }
    }
}

void MdnsBrowser::parse_response(const uint8_t* pkt, size_t len) {
    uint16_t flags  = rd_u16(pkt + 2);
    uint16_t qd     = rd_u16(pkt + 4);
    uint16_t an     = rd_u16(pkt + 6);
    if ((flags & 0x8000) == 0) return; // not response

    size_t p = 12;
    // skip questions
    for (uint16_t i = 0; i < qd; ++i) {
        std::string qn;
        size_t c = parse_name(pkt, len, p, qn);
        if (c == 0 || p + c + 4 > len) return;
        p += c + 4;
    }
    // answers: collect names and PTR/SRV/TXT/A records
    std::map<std::string, std::string> ptr_targets;    // service name -> instance
    std::map<std::string, std::pair<uint16_t,std::string>> srv_info; // instance -> (port, host)
    std::map<std::string, std::map<std::string,std::string>> txt_info; // instance -> txt kv
    std::map<std::string, std::string> a_info;          // hostname -> ip

    for (uint16_t i = 0; i < an; ++i) {
        std::string name;
        size_t c = parse_name(pkt, len, p, name);
        if (c == 0 || p + c + 10 > len) return;
        p += c;
        uint16_t rtype  = rd_u16(pkt + p);
        uint16_t rclass = rd_u16(pkt + p + 2);
        uint32_t ttl    = rd_u32(pkt + p + 4);
        uint16_t rdlen  = rd_u16(pkt + p + 8);
        (void)rclass; (void)ttl;
        p += 10;
        if (p + rdlen > len) return;
        const uint8_t* rd = pkt + p;

        if (rtype == 12) { // PTR
            std::string target;
            parse_name(pkt, len, p, target);
            ptr_targets[name] = target;
        } else if (rtype == 33 && rdlen >= 7) { // SRV
            uint16_t port = rd_u16(rd + 4);
            std::string target;
            parse_name(pkt, len, p + 6, target);
            srv_info[name] = {port, target};
        } else if (rtype == 16) { // TXT
            std::map<std::string,std::string> kv;
            size_t tp = 0;
            while (tp < rdlen) {
                uint8_t slen = rd[tp++];
                if (tp + slen > rdlen) break;
                std::string s((const char*)rd + tp, slen);
                tp += slen;
                size_t eq = s.find('=');
                if (eq == std::string::npos) kv[s] = "";
                else kv[s.substr(0, eq)] = s.substr(eq + 1);
            }
            txt_info[name] = std::move(kv);
        } else if (rtype == 1 && rdlen == 4) { // A
            char ip[INET_ADDRSTRLEN];
            std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u", rd[0], rd[1], rd[2], rd[3]);
            a_info[name] = ip;
        }
        p += rdlen;
    }

    // Assemble devices for _airplay._tcp
    uint64_t now = platform::time_now_us();
    auto assemble = [&](const std::string& svc) {
        for (auto& [svc_name, inst] : ptr_targets) {
            std::string lower = svc_name;
            for (char& c : lower) c = (char)std::tolower((unsigned char)c);
            if (lower.find(svc) == std::string::npos) continue;
            DiscoveredAirPlayDevice d;
            // inst name is typically "Living Room._airplay._tcp.local"
            size_t dot = inst.find('.');
            d.name = dot == std::string::npos ? inst : inst.substr(0, dot);

            if (auto it = srv_info.find(inst); it != srv_info.end()) {
                auto& [port, host] = it->second;
                d.port = port;
                auto ait = a_info.find(host);
                if (ait != a_info.end()) d.ip = ait->second;
            }
            if (auto it = txt_info.find(inst); it != txt_info.end()) {
                for (auto& [k,v] : it->second) {
                    std::string lk = k;
                    for (char& c : lk) c = (char)std::tolower((unsigned char)c);
                    if (lk == "deviceid") d.device_id = v;
                    else if (lk == "model") d.model = v;
                    else if (lk == "features") {
                        try { d.features = (uint32_t)std::stoul(v, nullptr, 0); } catch (...) {}
                    } else if (lk == "vv") {
                        try { d.vv = std::stoi(v); } catch (...) {}
                    }
                }
            }
            if (d.port == 0 || d.ip.empty()) continue;
            d.last_seen_us = now;
            std::string key = d.ip + ":" + std::to_string(d.port);
            bool added = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                added = (devices_.count(key) == 0);
                devices_[key] = d;
            }
            if (cb_) cb_(d, added);
        }
    };
    assemble("_airplay._tcp");
    assemble("_raop._tcp");
}

} // namespace airplay2
