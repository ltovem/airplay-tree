/*!
 * @file mdns_publisher.cpp
 *
 * Standalone lightweight mDNS publisher (responds to mDNS queries
 * on 224.0.0.251:5353). This avoids requiring Bonjour/Avahi to be
 * pre-installed, while a platform-native resolver (e.g. Bonjour on
 * macOS/iOS, Avahi on Linux) can be layered on top later.
 */
#include "mdns_publisher.h"
#include "platform/platform_log.h"
#include "platform/platform_time.h"
#include <cstring>
#include <cstdio>   // std::snprintf
#include <cctype>   // std::isalnum, std::tolower
#include <string>   // std::stoi（TXT 记录数值字段解析）
#include <random>

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
    #include <arpa/inet.h>
#endif

namespace airplay2 {

namespace {
    constexpr const char* kMcastGroupV4 = "224.0.0.251";
    constexpr uint16_t    kMcastPort    = 5353;
    constexpr uint32_t    kTtlDefault   = 4500; // seconds, mDNS default

    // --- DNS name wire encoding helpers ---
    void append_name(std::vector<uint8_t>& out, const std::string& dotted) {
        size_t pos = 0;
        while (pos < dotted.size()) {
            size_t dot = dotted.find('.', pos);
            if (dot == std::string::npos) dot = dotted.size();
            size_t len = dot - pos;
            if (len == 0 || len > 63) break;
            out.push_back((uint8_t)len);
            out.insert(out.end(), dotted.begin() + (long)pos, dotted.begin() + (long)dot);
            pos = dot + 1;
        }
        out.push_back(0); // root
    }

    void append_u16(std::vector<uint8_t>& out, uint16_t v) {
        out.push_back((uint8_t)(v >> 8));
        out.push_back((uint8_t)v);
    }
    void append_u32(std::vector<uint8_t>& out, uint32_t v) {
        out.push_back((uint8_t)(v >> 24));
        out.push_back((uint8_t)(v >> 16));
        out.push_back((uint8_t)(v >> 8));
        out.push_back((uint8_t)v);
    }

    uint16_t read_u16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }

    // Parse a compressed DNS name from packet; returns bytes consumed or 0 on error
    size_t parse_name(const uint8_t* pkt, size_t pkt_size, size_t offset,
                      std::string& out_name) {
        out_name.clear();
        size_t cur = offset;
        size_t bytes_total = 0;
        bool jumped = false;
        int depth = 0;
        while (cur < pkt_size && depth++ < 16) {
            uint8_t len = pkt[cur];
            if (len == 0) {
                if (!jumped) bytes_total = cur - offset + 1;
                return bytes_total ? bytes_total : cur - offset + 1;
            }
            if ((len & 0xC0) == 0xC0) {
                // pointer
                if (cur + 1 >= pkt_size) return 0;
                size_t ptr = ((size_t(len & 0x3F) << 8) | pkt[cur + 1]);
                if (!jumped) bytes_total = cur - offset + 2;
                jumped = true;
                cur = ptr;
                continue;
            }
            if (len & 0xC0) return 0; // reserved
            if (cur + 1 + len > pkt_size) return 0;
            if (!out_name.empty()) out_name.push_back('.');
            out_name.append((const char*)pkt + cur + 1, len);
            cur += 1 + len;
        }
        return 0;
    }

    std::string hex_encode(const std::vector<uint8_t>& data) {
        static const char hex[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(data.size() * 2);
        for (uint8_t b : data) {
            out.push_back(hex[(b >> 4) & 0xF]);
            out.push_back(hex[b & 0xF]);
        }
        return out;
    }

    std::string format_features(uint32_t f) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%08X", f);
        return buf;
    }
} // anon namespace

MdnsPublisher::MdnsPublisher() = default;
MdnsPublisher::~MdnsPublisher() { stop(); }

bool MdnsPublisher::start(const DeviceInfo& device, const std::string& if_ipv4) {
    stop();
    device_ = device;
    adv_ip_ = (if_ipv4.empty() || if_ipv4 == "0.0.0.0")
                ? platform::get_local_ipv4()
                : if_ipv4;
    // Generate a stable-ish hostname based on device id
    hostname_ = device_.name;
    // Strip whitespace + non-hostname chars
    for (char& c : hostname_) {
        if (!std::isalnum((unsigned char)c) && c != '-' && c != '.') c = '-';
    }
    hostname_.append(".local");

#if AP2_PLATFORM_MACOS || AP2_PLATFORM_IOS
    // Apple 平台优先走系统 Bonjour：注册直接进 mDNSResponder，
    // iPhone/Apple TV 一定能发现，无需自己打组播包
    if (register_with_bonjour()) {
        bonjour_mode_ = true;
        running_.store(true);
        announce_count_ = 0;
        announce_timer_us_ = 0;
        worker_.start([this] { mdns_worker(); }, "ap2-mdns");
        return true;
    }
    bonjour_mode_ = false;
    AP2_LOGW("mdns: Bonjour register failed, falling back to UDP multicast");
#endif

    if (!sock4_.create(platform::SocketProtocol::UDP, false)) {
        AP2_LOGE("mdns: create UDP socket failed");
        return false;
    }
    sock4_.set_option(platform::SOCK_OPT_REUSEADDR, 1);
    sock4_.set_option(platform::SOCK_OPT_REUSEPORT, 1);
    if (!sock4_.bind("0.0.0.0", kMcastPort)) {
        // If port 5353 is taken, the system mDNS responder is likely running.
        // Our backup: we send announcements only (no responses). Acceptable fallback.
        AP2_LOGW("mdns: cannot bind :5353 (system mDNS running?); using ephemeral port for announcements only");
        if (!sock4_.bind("0.0.0.0", 0)) {
            AP2_LOGE("mdns: bind ephemeral failed");
            return false;
        }
    }
    // Join multicast group
    {
#if AP2_PLATFORM_WINDOWS
        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = inet_addr(kMcastGroupV4);
        mreq.imr_interface.s_addr = inet_addr(adv_ip_.c_str());
        if (setsockopt((SOCKET)sock4_.handle(), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       (const char*)&mreq, sizeof(mreq)) != 0) {
            AP2_LOGW("mdns: join multicast failed (err=%d)", ::GetLastError());
        }
#else
        ip_mreq mreq{};
        inet_pton(AF_INET, kMcastGroupV4, &mreq.imr_multiaddr);
        inet_pton(AF_INET, adv_ip_.c_str(), &mreq.imr_interface);
        if (setsockopt((int)sock4_.handle(), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &mreq, sizeof(mreq)) != 0) {
            AP2_LOGW("mdns: join multicast failed");
        }
#endif
    } // 结束 join multicast group 作用域
    sock4_.set_option(platform::SOCK_OPT_MULTICAST_TTL, 255);
    // 显式指定组播发送接口，避免 macOS 多网卡（Wi-Fi/VPN）时选错出口
    {
#if AP2_PLATFORM_WINDOWS
        struct in_addr local_if{};
        local_if.s_addr = inet_addr(adv_ip_.c_str());
        setsockopt((SOCKET)sock4_.handle(), IPPROTO_IP, IP_MULTICAST_IF,
                   (const char*)&local_if, sizeof(local_if));
#else
        struct in_addr local_if{};
        inet_pton(AF_INET, adv_ip_.c_str(), &local_if);
        setsockopt((int)sock4_.handle(), IPPROTO_IP, IP_MULTICAST_IF,
                   &local_if, sizeof(local_if));
#endif
    }
    // 打开组播回环：让同机其他 mDNS 客户端（如 Bonjour 浏览器、本机其他 App）
    // 也能发现本服务；对跨设备（iPhone 等）无影响
    sock4_.set_option(platform::SOCK_OPT_MULTICAST_LOOP, 1);

    running_.store(true);
    announce_count_ = 0;
    announce_timer_us_ = 0;
    worker_.start([this] { mdns_worker(); }, "ap2-mdns");
    return true;
}

void MdnsPublisher::stop() {
#if AP2_PLATFORM_MACOS || AP2_PLATFORM_IOS
    if (bonjour_mode_) {
        // Bonjour 模式：停 worker 后释放注册句柄（mDNSResponder 自动撤下记录）
        running_.store(false);
        worker_.stop_and_join();
        if (airplay_ref_) { DNSServiceRefDeallocate(airplay_ref_); airplay_ref_ = nullptr; }
        if (raop_ref_)    { DNSServiceRefDeallocate(raop_ref_);    raop_ref_    = nullptr; }
        bonjour_mode_ = false;
        return;
    }
#endif
    if (running_.exchange(false)) {
        // Send a goodbye announcement if possible
        try { send_announcements(true); } catch (...) {}
    }
    worker_.stop_and_join();
    sock4_.close();
}

std::vector<uint8_t> MdnsPublisher::build_service_record(
    const std::string& inst_name, const std::string& type, const std::string& domain,
    uint16_t port, const std::map<std::string,std::string>& txt,
    uint32_t ttl, bool goodbye, const std::string& a_record_ip) {

    std::vector<uint8_t> pkt;
    pkt.reserve(512);

    // DNS header: id=0, flags=0x8400 (response authoritative), 0 questions,
    // N answers, 0 NS, 0 additional. 注意：mDNS 主动宣告（unsolicited response）
    // 不带 question 段，且 RFC 6762 禁止在 question 里置 cache-flush 位；
    // 之前放一个 QCLASS=0x8001 的 question 会导致部分 mDNS 实现丢弃整包。
    uint16_t answers = 0;
    size_t hdr_off = pkt.size();
    pkt.resize(12, 0); // header placeholder

    // 服务实例完整名称：inst.type.domain（用于 PTR/SRV/TXT 记录的 owner 名字）
    std::string qname = inst_name + "." + type + "." + domain;

    // We will produce: PTR, SRV, TXT, A
    const uint16_t CLASS_FLUSH = 0x8001;
    uint32_t real_ttl = goodbye ? 0 : ttl;

    size_t answers_off = pkt.size();

    // --- PTR: type.domain -> inst.type.domain ---
    // PTR 记录在 mDNS 中不可 flush（同一名字可有多个值），class 用 0x0001
    std::string ptr_name = type + "." + domain;
    append_name(pkt, ptr_name);
    append_u16(pkt, 12);    // TYPE PTR
    append_u16(pkt, 1);     // CLASS IN
    append_u32(pkt, real_ttl);
    size_t rdlen_ptr_off = pkt.size();
    append_u16(pkt, 0);     // RDLEN placeholder
    size_t rd_ptr_start = pkt.size();
    append_name(pkt, qname);
    uint16_t rdlen = (uint16_t)(pkt.size() - rd_ptr_start);
    pkt[rdlen_ptr_off]     = (uint8_t)(rdlen >> 8);
    pkt[rdlen_ptr_off + 1] = (uint8_t)rdlen;
    answers++;

    // --- SRV: priority 0, weight 0, port, target hostname ---
    append_name(pkt, qname);
    append_u16(pkt, 33); // SRV
    append_u16(pkt, CLASS_FLUSH);
    append_u32(pkt, real_ttl);
    size_t rdlen_srv_off = pkt.size();
    append_u16(pkt, 0);
    size_t rd_srv_start = pkt.size();
    append_u16(pkt, 0); // priority
    append_u16(pkt, 0); // weight
    append_u16(pkt, port);
    append_name(pkt, "target." + domain); // mDNS convention: use hostname we'll advertise
    rdlen = (uint16_t)(pkt.size() - rd_srv_start);
    pkt[rdlen_srv_off]     = (uint8_t)(rdlen >> 8);
    pkt[rdlen_srv_off + 1] = (uint8_t)rdlen;
    answers++;

    // --- TXT ---
    append_name(pkt, qname);
    append_u16(pkt, 16); // TXT
    append_u16(pkt, CLASS_FLUSH);
    append_u32(pkt, real_ttl);
    size_t rdlen_txt_off = pkt.size();
    append_u16(pkt, 0);
    size_t rd_txt_start = pkt.size();
    for (const auto& kv : txt) {
        std::string s = kv.first;
        if (!kv.second.empty()) { s += "="; s += kv.second; }
        if (s.size() > 255) s.resize(255);
        pkt.push_back((uint8_t)s.size());
        pkt.insert(pkt.end(), s.begin(), s.end());
    }
    if (txt.empty()) pkt.push_back(0); // empty TXT requires at least one zero-length string
    rdlen = (uint16_t)(pkt.size() - rd_txt_start);
    pkt[rdlen_txt_off]     = (uint8_t)(rdlen >> 8);
    pkt[rdlen_txt_off + 1] = (uint8_t)rdlen;
    answers++;

    // --- A: hostname -> IPv4 ---
    append_name(pkt, "target." + domain);
    append_u16(pkt, 1); // A
    append_u16(pkt, CLASS_FLUSH);
    append_u32(pkt, real_ttl);
    append_u16(pkt, 4); // RDLEN
    {
        uint8_t ip4[4] = {127,0,0,1};
        (void)platform::parse_ipv4(a_record_ip.empty() ? "127.0.0.1" : a_record_ip, ip4);
        pkt.push_back(ip4[0]);
        pkt.push_back(ip4[1]);
        pkt.push_back(ip4[2]);
        pkt.push_back(ip4[3]);
    }
    answers++;

    // Fill header
    pkt[hdr_off + 0] = 0; pkt[hdr_off + 1] = 0; // txn id
    // flags = 0x8400
    pkt[hdr_off + 2] = 0x84; pkt[hdr_off + 3] = 0x00;
    // QDCOUNT = 0（宣告不带 question）
    pkt[hdr_off + 4] = 0; pkt[hdr_off + 5] = 0;
    // ANCOUNT
    pkt[hdr_off + 6] = (uint8_t)(answers >> 8);
    pkt[hdr_off + 7] = (uint8_t)answers;
    // NSCOUNT = 0
    pkt[hdr_off + 8] = 0; pkt[hdr_off + 9] = 0;
    // ARCOUNT = 0 (we stuffed A in answers for simplicity)
    pkt[hdr_off + 10] = 0; pkt[hdr_off + 11] = 0;
    (void)answers_off;
    return pkt;
}

void MdnsPublisher::send_announcements(bool goodbye) {
    if (!sock4_.valid()) return;

    // Build _airplay._tcp record
    {
        std::map<std::string, std::string> txt;
        // model 空时按能力选：支持视频就用 AppleTV-like，否则用 AudioAccessory1,2
        std::string model = device_.model;
        if (model.empty()) model = device_.supports_video ? "AppleTV6,2" : "AudioAccessory1,2";

        txt["deviceid"]  = device_.device_id;
        // features 发布"双字"格式（UxPlay dnssd 同款："0x5A7FFEE6,0x0"）。
        // 第二字全 0 也要显式写出，部分 iOS 版本按完整 64 位解析，缺失会
        // 把设备误判成纯音频 → 镜像栏显示音响图标、无法镜像。
        txt["features"]  = format_features(device_.features) + ",0x0";
        txt["model"]     = model;
        txt["srcvers"]   = "220.68";   // 对齐 UxPlay GLOBAL_VERSION（Apple TV 3 同款）
        txt["vv"]        = device_.supports_video ? "2" : "1"; // vv=2 显示支持视频
        txt["pk"]        = device_.public_key_b64; // Ed25519 公钥(base64)，iOS 配对必需
        txt["pi"]        = device_.device_id;
        // pw=是否需要密码。空 PIN 时 false（UxPlay 同款：无密码也要显式声明）
        txt["pw"]        = device_.requires_encryption ? "true" : "false";
        // flags=状态位图(20bit)。UxPlay 用 0x4（"ready"），不是 0x84；
        // 0x80 位可能被 iOS 解读为"需要密码"，导致镜像拒绝。
        txt["flags"]     = "0x4";

        auto pkt = build_service_record(device_.name, "_airplay._tcp", "local",
                                        device_.port, txt, kTtlDefault, goodbye, adv_ip_);
        platform::SocketAddr dst; dst.ip = kMcastGroupV4; dst.port = kMcastPort;
        sock4_.sendto(pkt.data(), pkt.size(), dst);
    }
    // Build _raop._tcp record (legacy audio streaming compatible)
    {
        std::map<std::string, std::string> txt;
        // RAOP device id traditionally uses hex without colons
        std::string devid = device_.device_id;
        for (auto it = devid.begin(); it != devid.end(); ) {
            if (*it == ':') it = devid.erase(it);
            else ++it;
        }
        txt["cn"]        = "0,1,2,3";   // audio codecs: 0=pcm, 1=alac, 2=aac, 3=aac-eld
        txt["da"]        = "true";
        txt["et"]        = "0,3,5";
        txt["vv"]        = "2";
        txt["vn"]        = "65537";
        txt["tp"]        = "UDP";
        txt["sm"]        = "false";
        txt["ek"]        = "1";
        txt["rn"]        = "0";
        txt["md"]        = "0,1,2";
        txt["pw"]        = device_.requires_encryption ? "true" : "false";
        txt["sr"]        = "44100";
        txt["ss"]        = "16";
        txt["ch"]        = "2";
        txt["txtvers"]   = "1";          // TXT record 版本（_raop 标准字段）
        txt["sf"]        = "0x4";        // sender features（对齐 UxPlay RAOP_SF）
        txt["sv"]        = "false";      // 是否"软件设备"（对齐 UxPlay RAOP_SV）
        // 对齐 UxPlay dnssd_register_raop：am=设备型号 / ft=features / vs=版本
        // / rhd=硬件版本。这些字段帮助 iOS 正确归类设备能力（音频/镜像）。
        txt["am"]        = device_.model.empty() ? "AppleTV3,2" : device_.model;
        txt["ft"]        = format_features(device_.features) + ",0x0";
        txt["vs"]        = "220.68";
        txt["rhd"]       = "5.6.0.0";
        txt["pk"]        = device_.public_key_b64;

        auto pkt = build_service_record(devid + "@" + device_.name, "_raop._tcp", "local",
                                        device_.port, txt, kTtlDefault, goodbye, adv_ip_);
        platform::SocketAddr dst; dst.ip = kMcastGroupV4; dst.port = kMcastPort;
        sock4_.sendto(pkt.data(), pkt.size(), dst);
    }
}

void MdnsPublisher::handle_query(const uint8_t* pkt, size_t len, const platform::SocketAddr& from) {
    if (len < 12) return;
    uint16_t txn_id = read_u16(pkt);
    uint16_t flags  = read_u16(pkt + 2);
    uint16_t qd     = read_u16(pkt + 4);
    if ((flags & 0x8000) != 0) return; // ignore responses
    if (qd == 0) return;
    size_t p = 12;
    for (uint16_t i = 0; i < qd; ++i) {
        std::string qname;
        size_t consumed = parse_name(pkt, len, p, qname);
        if (consumed == 0 || p + consumed + 4 > len) return;
        p += consumed;
        uint16_t qtype  = read_u16(pkt + p);
        uint16_t qclass = read_u16(pkt + p + 2);
        (void)qtype; (void)qclass;
        p += 4;

        // If question concerns our advertised names/services, send unsolicited response
        bool interested = false;
        std::string lower = qname;
        for (char& c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower.find("_airplay._tcp") != std::string::npos ||
            lower.find("_raop._tcp")    != std::string::npos ||
            lower == "target.local") {
            interested = true;
        }
        if (interested) {
            send_announcements(false);
            (void)from; (void)txn_id;
            return;
        }
    }
}

#if AP2_PLATFORM_MACOS || AP2_PLATFORM_IOS
std::vector<uint8_t> MdnsPublisher::build_txt_blob(
    const std::map<std::string, std::string>& txt) {
    // DNS-SD TXT 记录负载：每一项 = 1 字节长度 + 内容（key=value）
    std::vector<uint8_t> out;
    for (const auto& kv : txt) {
        std::string s = kv.first;
        if (!kv.second.empty()) { s += "="; s += kv.second; }
        if (s.size() > 255) s.resize(255);
        out.push_back((uint8_t)s.size());
        out.insert(out.end(), s.begin(), s.end());
    }
    if (out.empty()) out.push_back(0); // 空 TXT 也必须有至少一个 0 长度项
    return out;
}

bool MdnsPublisher::register_with_bonjour() {
    // TXT 内容与 UDP 路径 send_announcements() 保持一致，
    // 保证不同后端宣告出去的设备能力完全相同
    std::map<std::string, std::string> txt;
    std::string model = device_.model;
    if (model.empty()) model = device_.supports_video ? "AppleTV6,2" : "AudioAccessory1,2";
    txt["deviceid"] = device_.device_id;
    // 与 UDP 路径一致：双字 features / flags=0x4 / srcvers=220.68 / pw，
    // 保证两种后端宣告出去的能力完全一致（iOS 按 64 位解析 features）。
    txt["features"] = format_features(device_.features) + ",0x0";
    txt["model"]    = model;
    txt["srcvers"]  = "220.68";
    txt["vv"]       = device_.supports_video ? "2" : "1";
    txt["pk"]       = device_.public_key_b64; // Ed25519 公钥(base64)，iOS 配对必需
    txt["pi"]       = device_.device_id;
    txt["pw"]       = device_.requires_encryption ? "true" : "false";
    txt["flags"]    = "0x4";
    auto blob = build_txt_blob(txt);

    // host=NULL：让 mDNSResponder 用本机主机名作为 SRV target，A 记录自动解析
    DNSServiceErrorType err = DNSServiceRegister(
        &airplay_ref_, 0, 0, device_.name.c_str(),
        "_airplay._tcp", "local.", nullptr, htons(device_.port),
        (uint16_t)blob.size(), blob.data(), nullptr, nullptr);
    if (err != kDNSServiceErr_NoError) {
        AP2_LOGW("mdns: DNSServiceRegister(_airplay) failed err=%d", (int)err);
        airplay_ref_ = nullptr;
        return false;
    }
    AP2_LOGI("mdns: registered _airplay._tcp '%s' port %u via Bonjour",
             device_.name.c_str(), (unsigned)device_.port);

    // _raop._tcp 兼容注册（旧式音频投送），失败不致命
    std::string devid = device_.device_id;
    for (auto it = devid.begin(); it != devid.end(); ) {
        if (*it == ':') it = devid.erase(it);
        else ++it;
    }
    std::map<std::string, std::string> rtxt;
    rtxt["cn"] = "0,1,2,3";
    rtxt["da"] = "true";
    rtxt["et"] = "0,3,5";
    rtxt["vv"] = "2";
    rtxt["vn"] = "65537";
    rtxt["tp"] = "UDP";
    rtxt["sm"] = "false";
    rtxt["ek"] = "1";
    rtxt["rn"] = "0";
    rtxt["md"] = "0,1,2";
    rtxt["pw"] = device_.requires_encryption ? "true" : "false";
    rtxt["sr"] = "44100";
    rtxt["ss"] = "16";
    rtxt["ch"] = "2";
    rtxt["txtvers"] = "1";  // _raop 标准字段，与 UDP 路径保持一致
    rtxt["sf"] = "0x4";     // sender features（对齐 UxPlay RAOP_SF）
    rtxt["sv"] = "false";   // 对齐 UxPlay RAOP_SV
    // 对齐 UxPlay dnssd_register_raop 的 am/ft/vs/rhd/pk
    rtxt["am"] = device_.model.empty() ? "AppleTV3,2" : device_.model;
    rtxt["ft"] = format_features(device_.features) + ",0x0";
    rtxt["vs"] = "220.68";
    rtxt["rhd"] = "5.6.0.0";
    rtxt["pk"] = device_.public_key_b64;
    auto rblob = build_txt_blob(rtxt);
    err = DNSServiceRegister(
        &raop_ref_, 0, 0, (devid + "@" + device_.name).c_str(),
        "_raop._tcp", "local.", nullptr, htons(device_.port),
        (uint16_t)rblob.size(), rblob.data(), nullptr, nullptr);
    if (err != kDNSServiceErr_NoError) {
        AP2_LOGW("mdns: DNSServiceRegister(_raop) failed err=%d (non-fatal)", (int)err);
        raop_ref_ = nullptr;
    } else {
        AP2_LOGI("mdns: registered _raop._tcp via Bonjour");
    }
    return true;
}
#endif // AP2_PLATFORM_MACOS || AP2_PLATFORM_IOS

void MdnsPublisher::mdns_worker() {
#if AP2_PLATFORM_MACOS || AP2_PLATFORM_IOS
    if (bonjour_mode_) {
        // Bonjour 模式：注册由系统 mDNSResponder 负责宣告/续期，
        // worker 只泵送事件，保证句柄活跃并处理名字冲突
        AP2_LOGI("mdns: Bonjour publishing (ip=%s airplay_port=%u)",
                 adv_ip_.c_str(), device_.port);
        while (running_.load()) {
            if (airplay_ref_) DNSServiceProcessResult(airplay_ref_);
            if (raop_ref_)    DNSServiceProcessResult(raop_ref_);
            platform::sleep_ms(100);
        }
        return;
    }
#endif
    AP2_LOGI("mdns: advertising services (ip=%s airplay_port=%u)", adv_ip_.c_str(), device_.port);
    uint8_t buf[2048];
    const uint64_t ANNOUNCE_INTERVAL_US = 1500000ULL; // 1.5s between first few
    const int INITIAL_ANNOUNCES = 3;

    announce_timer_us_ = platform::time_now_us();
    announce_count_ = 0;

    while (running_.load()) {
        std::vector<platform::Socket*> socks;
        socks.push_back(&sock4_);
        std::vector<size_t> ready;
        bool ok = platform::select_read(socks, ready, 200 /* 200ms */);
        if (!ok) {
            platform::sleep_ms(50);
            continue;
        }
        if (!ready.empty()) {
            platform::SocketAddr from;
            auto r = sock4_.recvfrom(buf, sizeof(buf), &from);
            if (r.ok && r.bytes > 0) {
                handle_query(buf, (size_t)r.bytes, from);
            }
        }
        // Announce loop
        uint64_t now = platform::time_now_us();
        if (announce_count_ < INITIAL_ANNOUNCES &&
            now - announce_timer_us_ >= ANNOUNCE_INTERVAL_US) {
            send_announcements(false);
            announce_timer_us_ = now;
            announce_count_++;
        }
    }
}

} // namespace airplay2
