/*!
 * @file mdns_publisher.h
 * @brief 独立零依赖的 mDNS（Bonjour / Zeroconf）服务宣告器
 *
 * 设计目标：不需要系统预装 Avahi / Bonjour SDK，纯靠原始 UDP socket
 * 组播 224.0.0.251:5353 就能对外宣告 AirPlay 设备，便于 Android /
 * 嵌入式 Linux / Windows 最小依赖部署。
 *
 * 协议参考：
 *   - RFC 6762 Multicast DNS
 *   - RFC 6763 DNS-Based Service Discovery (DNS-SD)
 *   - Apple Bonjour TXT record 约定（服务格式 <Instance>._<type>._tcp.local.）
 *
 * 发布的两条服务（AirPlay 2 发送端要求同时存在）：
 *   1) _airplay._tcp.local.  端口=device_.port（RTSP 控制）
 *      TXT 包含：deviceid= features= fvv= model= 等
 *   2) _raop._tcp.local.     端口=device_.port（数字音频，RAOP 是 AirTunes2）
 *      TXT 包含：cn=0,1,2,3（支持 ALAC/AAC/PCM） et=0 ek=1 ch=2 ss=16 sr=44100 ...
 *
 * @note 这是"简化版 Responder"：
 *       - 只处理 Query 类型（收到 PTR/TXT/A 查询按本地缓存返回）
 *       - 做初始 3 次重复宣告 + 每 75 秒保持刷新
 *       - stop() 时发 TTL=0 的 goodbye 报文，让发送端 1s 内从列表消失
 *       - **不实现** 同时查询（probe）与名字冲突仲裁；依赖调用者保证
 *         DeviceInfo::name 在局域网内大致唯一
 */
#ifndef AIRPLAY2_MDNS_PUBLISHER_H
#define AIRPLAY2_MDNS_PUBLISHER_H

#include "../include/airplay2/airplay_config.h"
#include "platform/platform_socket.h"
#include "platform/platform_thread.h"
#include <map>
#include <string>
#include <vector>
#include <memory>

namespace airplay2 {

/*!
 * @brief AirPlay 专用 mDNS 宣告器
 *
 * 运行模型：单线程 worker，使用阻塞 select(udp socket + 定时器)。
 * 线程只做三件事：
 *   1. 收到 mDNS Query（从 224.0.0.251:5353）→ 对应答包 unicast + multicast
 *   2. 周期性（announce_timer_us_）触发：初始 3 次宣告 / 后续 75s 刷新
 *   3. stop()：发 goodbye → 关 socket → join
 */
class MdnsPublisher {
public:
    MdnsPublisher();
    ~MdnsPublisher();

    /*!
     * @brief 启动 mDNS 宣告（加入组播 + 起 worker 线程）
     * @param device   设备名/端口/feature 等，内部深拷贝
     * @param if_ipv4  指定绑定的本机 IPv4（例如 "192.168.1.10"）；
     *                 传空串或 "0.0.0.0" 时由库自动探测第一张非 loopback 的地址
     * @return true    成功加入组播 224.0.0.251:5353 且 worker 已启动
     *                 false 通常意味着端口 5353 已被系统 mDNSResponder 占用
     *                 （macOS/iOS 常见；此时建议改走系统 Bonjour API，或者
     *                 在启动前停掉系统服务）
     */
    bool start(const DeviceInfo& device, const std::string& if_ipv4 = "0.0.0.0");

    /// 停止宣告（发送 goodbye 包 + join 线程），幂等
    void stop();

    /// 是否处于运行态（worker 活着）
    bool is_running() const { return running_.load(); }

private:
    // ---- worker 线程主循环 ---------------------------------------------------
    //   select(sock4_, 100ms timeout) 同时承担"收查询"和"定时 announce"
    //   的驱动，避免单独搞 timerfd 带来跨平台负担
    void mdns_worker();

    // ---- 发包辅助 -------------------------------------------------------------
    // 构造并通过 sock4_ 发送一到多组宣告；goodbye=true 表示 TTL 置 0
    void send_announcements(bool goodbye = false);

    // ---- 收包处理 -------------------------------------------------------------
    // 从 pkt 解析 DNS header，如果有 QD= 查询 section 并且命中我们的
    // _airplay._tcp / _raop._tcp / hostname.local，就组合对应的
    // PTR/TXT/SRV/A 四合一回答。来自 <5353 的源端口按 RFC 也要回 5353。
    void handle_query(const uint8_t* pkt, size_t len, const platform::SocketAddr& from);

    // ---- 纯函数序列化 ---------------------------------------------------------
    // 把一个服务实例按 DNS wire format 编码成字节数组，供 send_announcements /
    // handle_query 复用。参数 a_record_ip 是回答 A record 时要带的本机 IPv4 文本；
    // 传空则省略 A section（此时依赖对端再独立查询）。
    static std::vector<uint8_t> build_service_record(
        const std::string& name,
        const std::string& type,
        const std::string& domain,
        uint16_t port,
        const std::map<std::string, std::string>& txt,
        uint32_t ttl,
        bool goodbye,
        const std::string& a_record_ip);

    // ---- 成员 -----------------------------------------------------------------
    // 生命周期状态：false → 由 start() 置 true → stop() 置 false；
    // worker_ 循环里每次都会检查，保证退出最久延迟 100ms（select 超时）。
    std::atomic<bool> running_{false};

    platform::Thread worker_;              ///< worker 线程对象（pthread / std::thread 抽象）
    platform::Socket sock4_;               ///< IPv4 UDP socket，加入 224.0.0.251，SO_REUSEADDR

    DeviceInfo device_;                    ///< 深拷贝自 start() 传入的 DeviceInfo
    std::string adv_ip_;                   ///< 实际宣告的本机 IPv4（A record 里带的）
    std::string hostname_;                 ///< <sanitized-name>.local，作为 SRV target

    // RFC 6762 建议：启动时在 t=0, 1s, 3s, 7s, 15s 指数退避发送多次宣告；
    // 实际使用 3 次渐进（0/1/3s）通常就能让对端缓存，之后每 75s 再刷新；
    // announce_count_ 记录已经发过的次数，用于决定"还需不需要再立刻补发"。
    uint64_t announce_timer_us_ = 0;       ///< 下次自动宣告的目标时刻（微秒，平台单调钟）
    int      announce_count_   = 0;        ///< 已发送宣告轮次
};

} // namespace airplay2

#endif // AIRPLAY2_MDNS_PUBLISHER_H
