/*!
 * @file rtcp.cpp
 * @brief RTCP 处理器实现
 *
 * 只实现 AirPlay 需要的最小子集：
 *   - 解析 SR（PT=200），提取 NTP MSW/LSW + RTP TS 对应关系
 *   - 构造 RR（PT=201），含 1 个 report block
 *   - 其它 PT（SDES=202/BYE=203/APP=204）按 length 字段跳过
 *
 * RTCP 包头格式（RFC 3550 §6）：
 *   [0]   V(2) | P(1) | RC(5)     —— V 必须是 2，RC 是 report block 数量
 *   [1]   PT                     —— payload type
 *   [2-3] length                 —— 以 32-bit word 计的长度，不含这 4 字节头
 */
#include "rtcp.h"
#include "../platform/platform_log.h"
#include <cstring>

namespace airplay2 {
namespace net {

// RTCP Payload Types（RFC 3550）
static constexpr uint8_t kRtcpPtSr   = 200;  ///< Sender Report
static constexpr uint8_t kRtcpPtRr   = 201;  ///< Receiver Report
static constexpr uint8_t kRtcpPtSdes = 202;  ///< Source Description
static constexpr uint8_t kRtcpPtBye  = 203;  ///< Goodbye

RtcpHandler::RtcpHandler() = default;
RtcpHandler::~RtcpHandler() = default;

// 内联小工具：从大端字节流里读一个 32-bit 无符号整数
static inline uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

void RtcpHandler::handle_packet(const uint8_t* data, size_t len) {
    if (data == nullptr || len < 4) return;

    // 一个 UDP 数据报可能 compound 多个 RTCP 子包，按 length 步进扫描。
    // 每个子包都对齐到 32-bit 边界，所以步进 = (length + 1) * 4。
    const uint8_t* p    = data;
    const uint8_t* end  = data + len;
    while (p + 4 <= end) {
        // 版本号必须在高 2 位且等于 2；否则认为包损坏，直接放弃
        uint8_t v = (p[0] >> 6) & 0x03;
        if (v != 2) {
            AP2_LOGW("rtcp: bad version %u, drop rest", v);
            return;
        }
        uint8_t  pt     = p[1];
        // length 字段单位是 32-bit word，且不含头本身这 1 个 word
        uint16_t length = (uint16_t(p[2]) << 8) | p[3];
        size_t   pkt_bytes = (size_t(length) + 1) * 4;

        if (p + pkt_bytes > end) {
            // 声明的长度超过实际剩余字节，包被截断，丢弃
            AP2_LOGW("rtcp: truncated packet pt=%u len=%u", pt, length);
            return;
        }

        // 只关心 SR；其它类型（RR/SDES/BYE/APP）一概跳过
        if (pt == kRtcpPtSr && pkt_bytes >= 28) {
            // SR 布局（去掉 4 字节公共头之后）：
            //   [4-7]   SSRC of sender
            //   [8-11]  NTP timestamp MSW（高 32 位：秒）
            //   [12-15] NTP timestamp LSW（低 32 位：小数）
            //   [16-19] RTP timestamp
            //   [20-23] sender's packet count
            //   [24-27] sender's octet count
            //   [28..]  0 或多个 report block
            RtcpSrInfo info;
            info.ssrc    = read_be32(p + 4);
            info.ntp_msw = read_be32(p + 8);
            info.ntp_lsw = read_be32(p + 12);
            info.rtp_ts  = read_be32(p + 16);

            last_sr_ = info;
            has_sr_  = true;

            AP2_LOGD("rtcp: SR ssrc=0x%08x ntp=%llu.%llu rtp_ts=%u",
                     info.ssrc,
                     (unsigned long long)info.ntp_msw,
                     (unsigned long long)info.ntp_lsw,
                     info.rtp_ts);
        }
        // 步进到下一个子包（按 32-bit 对齐）
        p += pkt_bytes;
    }
}

std::vector<uint8_t> RtcpHandler::build_rr(uint32_t sender_ssrc,
                                           uint32_t our_ssrc,
                                           uint32_t highest_seq,
                                           uint32_t packets_lost,
                                           uint32_t jitter) {
    // RR 包结构（含 1 个 report block）：
    //   header(4) + sender SSRC(4) + report block(24) = 32 字节
    // length 字段 = (32 / 4) - 1 = 7
    std::vector<uint8_t> pkt(32, 0);

    // [0] V=2, P=0, RC=1（1 个 report block）
    pkt[0] = 0x80 | 0x01;
    // [1] PT = 201 (RR)
    pkt[1] = kRtcpPtRr;
    // [2-3] length = 7（32-bit words，不含头）
    pkt[2] = 0x00;
    pkt[3] = 0x07;

    // [4-7] SSRC of report sender（本端 SSRC）
    pkt[4] = (uint8_t)(our_ssrc >> 24);
    pkt[5] = (uint8_t)(our_ssrc >> 16);
    pkt[6] = (uint8_t)(our_ssrc >> 8);
    pkt[7] = (uint8_t)(our_ssrc);

    // ---- report block 开始（针对被报告的 sender_ssrc）----------------
    // [8-11] SSRC of source（被报告的发送端 SSRC）
    pkt[8]  = (uint8_t)(sender_ssrc >> 24);
    pkt[9]  = (uint8_t)(sender_ssrc >> 16);
    pkt[10] = (uint8_t)(sender_ssrc >> 8);
    pkt[11] = (uint8_t)(sender_ssrc);

    // [12] fraction_lost：高 8 位百分比（0-255 表示 0%-100%）。
    // AirPlay 接收侧没有维护"自上次 RR 以来的期望包数"，所以这里无法精确
    // 计算增量丢包率；为了简单且不误导发送端，固定填 0（接收端用 cum_lost
    // 总量来评估，发送端也不会因为 fraction_lost=0 就提速到爆）。
    pkt[12] = 0x00;

    // [13-15] cumulative number of packets lost（24 位有符号）。
    // 若累计丢包超过 2^23-1，截断到最大正值，避免负数解释成"多收"。
    uint32_t cum_lost = packets_lost;
    if (cum_lost > 0x7FFFFF) cum_lost = 0x7FFFFF;
    pkt[13] = (uint8_t)(cum_lost >> 16);
    pkt[14] = (uint8_t)(cum_lost >> 8);
    pkt[15] = (uint8_t)(cum_lost);

    // [16-19] extended highest sequence number received
    //   高 16 位是 seq cycle 计数（这里始终 0，因为 RtpReceiver 不维护），
    //   低 16 位是最高 RTP seq。
    pkt[16] = 0x00;
    pkt[17] = 0x00;
    pkt[18] = (uint8_t)(highest_seq >> 8);
    pkt[19] = (uint8_t)(highest_seq);

    // [20-23] interarrival jitter（接收端估计的包间隔抖动，时钟单元）
    pkt[20] = (uint8_t)(jitter >> 24);
    pkt[21] = (uint8_t)(jitter >> 16);
    pkt[22] = (uint8_t)(jitter >> 8);
    pkt[23] = (uint8_t)(jitter);

    // [24-27] LSR（Last SR timestamp）：SR 里 NTP 时间戳的中间 32 位。
    //   若从未收到 SR，填 0；否则填 last_sr_ 的 (MSW<<16)|(LSW>>16)。
    // [28-31] DLSR（delay since last SR）：收到 SR 到发出此 RR 的间隔，
    //   单位 1/65536 秒。这里不精确测量，统一填 0。
    // AirPlay 发送端不依赖 LSR/DLSR 算 RTT，所以这里简化处理。
    if (has_sr_) {
        uint32_t lsr = (uint32_t)((last_sr_.ntp_msw << 16) & 0xFFFFFFFF) |
                       (uint32_t)(last_sr_.ntp_lsw >> 16);
        pkt[24] = (uint8_t)(lsr >> 24);
        pkt[25] = (uint8_t)(lsr >> 16);
        pkt[26] = (uint8_t)(lsr >> 8);
        pkt[27] = (uint8_t)(lsr);
    }
    // pkt[28..31] 已经在初始化时被置为 0，无需再写

    return pkt;
}

}} // namespace airplay2::net
