/*!
 * @file timing.cpp
 * @brief AirPlay Timing 协议处理器实现
 *
 * 关键点：
 *   - ntp_now() 用 platform::wallclock_us() 拿到 Unix 毫秒墙钟，再换算成
 *     NTP epoch（1900-01-01）起的秒 + 小数。NTP 与 Unix epoch 差
 *     2208988800 秒（70 年里的闰年/平年累加）。
 *   - response 的 3 个 timestamp 都是 32 位紧凑 NTP（高 16 秒 + 低 16 小数），
 *     而非完整 64 位 NTP——这是 AirPlay 私有协议的约定，和标准 NTP 不同。
 *     所以我们从 64 位 NTP 截取高 16 位秒和低 16 位小数合成一个 32 位值。
 */
#include "timing.h"
#include "../platform/platform_time.h"
#include "../platform/platform_log.h"
#include <cstring>

namespace airplay2 {
namespace net {

// Unix epoch（1970-01-01）到 NTP epoch（1900-01-01）的秒数差
// = 70 年 = 365*70 + 17 个闰日 = 25567 + 17 = 25567... 经典常量 2208988800
static constexpr uint64_t kNtpUnixOffset = 2208988800ULL;

// AirPlay Timing 包的 payload type
static constexpr uint8_t kTimingPtRequest  = 0x53; ///< timing request
static constexpr uint8_t kTimingPtResponse = 0x54; ///< timing response

// 固定包长：4B header + 4B SSRC + 3 * 4B timestamp = 24 字节
// length 字段 = (24 - 4) / 4 = 4 个 32-bit word
static constexpr size_t kTimingPacketLen = 24;

TimingHandler::TimingHandler() = default;
TimingHandler::~TimingHandler() = default;

uint64_t TimingHandler::ntp_now() {
    // platform::wallclock_us() 返回自 Unix epoch（1970）起的微秒数
    uint64_t us = platform::wallclock_us();

    // 拆成"秒"和"秒内小数（微秒）"
    uint64_t secs = us / 1000000ULL;
    uint64_t frac_us = us % 1000000ULL;

    // 加上 Unix->NTP 偏移得到 NTP 秒
    uint64_t ntp_secs = secs + kNtpUnixOffset;

    // 把微秒小数换算成 2^32 单位：frac_us / 1e6 * 2^32
    // 用 (frac_us << 32) / 1e6 避免浮点，保持整数精度
    uint64_t ntp_frac = (frac_us << 32) / 1000000ULL;

    // 高 32 位 = 秒，低 32 位 = 小数
    return (ntp_secs << 32) | (ntp_frac & 0xFFFFFFFFULL);
}

// 把 64 位 NTP 时间戳压缩成 AirPlay 用的 32 位紧凑 NTP：
//   高 16 位 = NTP 秒的低 16 位
//   低 16 位 = NTP 小数的高 16 位
// 这样既能表示约 18 小时的秒范围（2^16 s），又保留了 16 位小数精度
// （约 15μs），完全够 AirPlay 音频同步用。
static inline uint32_t compact_ntp(uint64_t full_ntp) {
    uint32_t secs  = (uint32_t)((full_ntp >> 32) & 0xFFFFFFFFULL);
    uint32_t frac  = (uint32_t)(full_ntp & 0xFFFFFFFFULL);
    return ((secs & 0xFFFF) << 16) | ((frac >> 16) & 0xFFFF);
}

// 内联小工具：从大端字节流里读 32 位无符号
static inline uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

// 内联小工具：把 32 位值按大端写入缓冲区
static inline void write_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

std::vector<uint8_t> TimingHandler::handle_packet(const uint8_t* data, size_t len) {
    if (data == nullptr || len < kTimingPacketLen) {
        // 长度不足或空指针，无法解析；AirPlay 发送端会重试，这里直接丢弃
        return {};
    }

    // 包头校验：V=2 且 PT 是 timing request
    // [0] 高 2 位 = 版本，必须 = 2（即 (data[0] & 0xC0) == 0x80）
    if ((data[0] & 0xC0) != 0x80) {
        AP2_LOGW("timing: bad version byte 0x%02x", data[0]);
        return {};
    }
    uint8_t pt = data[1];
    if (pt != kTimingPtRequest) {
        // 可能是别人回的 response 误投到我们端口，或者是协议升级后的新类型；
        // 静默丢弃，不回错。
        AP2_LOGD("timing: ignore pt=0x%02x (not request)", pt);
        return {};
    }

    // length 字段必须 = 4（4 个 32-bit word，不含头）
    uint16_t length = (uint16_t(data[2]) << 8) | data[3];
    if (length != 4) {
        AP2_LOGW("timing: bad length %u (expect 4)", length);
        return {};
    }

    // 提取发送端 SSRC 和 originator timestamp（timestamp1）
    uint32_t sender_ssrc = read_be32(data + 4);
    uint32_t ts1_be      = read_be32(data + 8);

    // 把 32 位紧凑 NTP 还原成 64 位完整 NTP（高 32 秒 + 低 32 小数），
    // 仅用于内部记录；response 里仍要原样回填 32 位 ts1。
    // 这里"还原"高 16 位秒时用 0 补高位，因为只用于差值比较，
    // 不需要绝对秒数。
    TimingRequest req;
    req.ssrc = sender_ssrc;
    // 高 16 位扩展为 32 位秒（高位补 0），低 16 位小数扩展为 32 位（左移 16）
    uint64_t originator_secs = (uint64_t)((ts1_be >> 16) & 0xFFFF);
    uint64_t originator_frac = (uint64_t)((ts1_be & 0xFFFF) << 16);
    req.originator_ts = (originator_secs << 32) | originator_frac;
    (void)req; // 当前实现不持久化 request，只用于即时构造 response

    // 构造 response：与 request 同样 24 字节
    std::vector<uint8_t> resp(kTimingPacketLen, 0);

    // [0] V=2, marker=0 → 0x80
    resp[0] = 0x80;
    // [1] PT = 0x54 (timing response)
    resp[1] = kTimingPtResponse;
    // [2-3] length = 4
    resp[2] = 0x00;
    resp[3] = 0x04;

    // [4-7] 我们的 SSRC
    write_be32(&resp[4], our_ssrc_);

    // [8-11] timestamp1 = 原样回填发送端的 originator timestamp
    //   发送端用它来关联 request/response，必须一字不差
    write_be32(&resp[8], ts1_be);

    // [12-15] timestamp2 = 接收端"收到 request 的瞬间"
    // [16-19] timestamp3 = 接收端"发出 response 的瞬间"
    // 两时刻都填 ntp_now()：handler 在收包线程里同步调用，二者相差仅几微秒，
    // 对发送端 RTT 估算的影响远小于网络抖动本身。
    uint64_t now = ntp_now();
    uint32_t ts2 = compact_ntp(now);
    uint32_t ts3 = compact_ntp(now);
    write_be32(&resp[12], ts2);
    write_be32(&resp[16], ts3);

    return resp;
}

}} // namespace airplay2::net
