/*!
 * @file timing.h
 * @brief AirPlay 私有 Timing 协议处理
 *
 * AirPlay 在第 3 个 UDP 竿口上使用私有 timing 包做时钟同步。
 * 发送端会发送 timing request，接收端必须回复 timing response，
 * 否则发送端会认为接收端"卡住"并降低码率或断开。
 *
 * Timing 包格式（基于 RTSP NTP 时间）：
 *   [0]   0x80 (V=2, 标记位)
 *   [1]   PT (payload type: 0x53=timing request, 0x54=timing response)
 *   [2-3] 长度（固定 4 个 32-bit word，不含头）
 *   [4-7]  sender_ssrc
 *   [8-11] timestamp1 (发送端发出请求的时刻, NTP format: 高 16 位秒 + 低 16 位小数)
 *         实际是 32-bit 整数，高 16 = 秒部分, 低 16 = 小数部分
 *   [12-15] timestamp2 (接收端收到时刻, response 才填)
 *   [16-19] timestamp3 (接收端发出 response 时刻, response 才填)
 *
 * 处理流程：
 *   收到 request → 记录 timestamp1 → 填 timestamp2/3 → 回复 response
 */
#ifndef AIRPLAY2_NET_TIMING_H
#define AIRPLAY2_NET_TIMING_H

#include <cstdint>
#include <cstddef>
#include <vector>

namespace airplay2 {
namespace net {

/*!
 * @brief 从 timing request 包里解析出的发送端信息
 *
 * originator_ts 是发送端"发出请求时"的时间戳，response 里必须原样回填
 * 到 timestamp1，发送端据此算 RTT（timestamp3 - timestamp1）和单向延迟。
 * 这里把它展开成 64 位 NTP（高 32 秒 / 低 32 小数）方便和 ntp_now() 对齐。
 */
struct TimingRequest {
    uint32_t ssrc = 0;
    uint64_t originator_ts = 0;  ///< 发送端时间戳（NTP 格式，高 32 = 秒, 低 32 = 小数）
};

/*!
 * @brief AirPlay Timing 协议处理器
 *
 * 设计取舍：
 *   - 不维护会话状态，每个 request 都即时构造一个 response 返回。
 *   - timestamp2 / timestamp3 都填本机 ntp_now()，因为 handler 调用本身
 *     就发生在"收到包的瞬间"，二者差几个微秒对发送端的 RTT 估算可忽略。
 *   - 不区分多发送端：AirPlay 单源场景下 SSRC 是固定的。
 *
 * 线程安全：本类不加锁，调用方（RtpReceiver worker）保证单线程访问即可。
 */
class TimingHandler {
public:
    TimingHandler();
    ~TimingHandler();

    /// 处理收到的 timing 包，如果是 request 则返回要发回的 response 包
    /// @param data     收到的 UDP 包内容
    /// @param len      包长度
    /// @return         非空 vector 表示需要发送的 response；空表示无需回复
    ///
    /// 若收到的不是合法的 timing request（版本错、PT 错、长度错），
    /// 返回空 vector，调用方静默丢弃即可，不要回 RST 之类的错误。
    std::vector<uint8_t> handle_packet(const uint8_t* data, size_t len);

    /// 当前 NTP 时间戳（秒 + 小数），用于填充 timestamp2/3
    /// NTP epoch 是 1900-01-01 00:00:00 UTC
    ///
    /// 返回值布局：高 32 位 = 自 NTP epoch 起的秒数；低 32 位 = 小数部分
    /// （以 2^-32 秒为单位）。
    static uint64_t ntp_now();

private:
    uint32_t our_ssrc_ = 0x41503200; ///< "AP2\0" 作为默认 SSRC
};

}} // namespace airplay2::net

#endif // AIRPLAY2_NET_TIMING_H
