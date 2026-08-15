/*!
 * @file rtcp.h
 * @brief RTCP（RTP Control Protocol）接收端报告处理
 *
 * AirPlay 音频流使用 3 个 UDP 端口：
 *   - data（RTP 音频包）
 *   - control（RTCP：发送端 SR / 接收端 RR / NTP 同步）
 *   - timing（AirPlay 私有 timing 包）
 *
 * RTCP 的作用：
 *   1. 接收端定期发 RR（Receiver Report）告诉发送端丢包率 / 抖动
 *   2. 发送端发 SR（Sender Report）带 NTP 时间戳，接收端用于音视频同步
 *   3. SDES / BYE 等辅助包
 *
 * AirPlay 场景下我们只需：
 *   - 解析收到的 SR（提取 NTP 时间戳 + RTP 时间戳对应关系）
 *   - 定期发 RR（报告接收质量，让发送端做码率自适应）
 */
#ifndef AIRPLAY2_NET_RTCP_H
#define AIRPLAY2_NET_RTCP_H

#include <cstdint>
#include <cstddef>
#include <vector>

namespace airplay2 {
namespace net {

/*!
 * @brief 从一个 SR（Sender Report）中提取的关键信息
 *
 * 这组时间戳对应关系是 RTP 时钟（采样率相关）和 NTP 时钟（墙钟）
 * 之间的"锚点"，接收端可以据此把 RTP timestamp 换算为绝对播放时刻，
 * 进而做音视频同步 / 唇音对齐。
 */
struct RtcpSrInfo {
    uint64_t ntp_msw = 0;  ///< NTP 时间戳高位（秒，从 1900-01-01 起）
    uint64_t ntp_lsw = 0;  ///< NTP 时间戳低位（小数部分，2^32 秒）
    uint32_t rtp_ts  = 0;  ///< 对应的 RTP 时间戳
    uint32_t ssrc    = 0;  ///< 发送端 SSRC
};

/*!
 * @brief 极简 RTCP 处理器：只解析 SR、只构造 RR
 *
 * AirPlay 的 RTCP 信道带宽非常小（通常 < 1 包/秒），且我们只关心
 * "拿到发送端 NTP 锚点"和"反馈丢包率"两件事，所以这里不实现完整的
 * RFC 3550 栈（SDES/BYE/APP 一律跳过，避免引入复杂的状态机）。
 *
 * 线程安全说明：本类自身不加锁。典型用法是只在 RtpReceiver 的 worker
 * 线程里访问它，调用者若跨线程使用需自行加锁。
 */
class RtcpHandler {
public:
    RtcpHandler();
    ~RtcpHandler();

    /// 处理收到的 RTCP 包（从 control socket 收到的）
    /// 解析 SR 包，提取 NTP 时间戳
    ///
    /// 一个 UDP 数据报里可能 compound 多个 RTCP 子包（SR+SDES+BYE 等），
    /// 这里按 32-bit word 步进逐个扫描，遇到 SR 就更新 last_sr_，
    /// 其余类型直接跳过。
    void handle_packet(const uint8_t* data, size_t len);

    /// 构造一个 RR（Receiver Report）包，准备通过 control socket 发回发送端
    /// @param sender_ssrc 发送端 SSRC（从 SR 学到）
    /// @param our_ssrc    本端 SSRC（通常不需要，置 0 即可）
    /// @param highest_seq 收到的最大 RTP seq
    /// @param packets_lost 丢包总数
    /// @param jitter       抖动估计（时钟单元）
    ///
    /// RR 里还会带 LSR（Last SR timestamp）和 DLSR（delay since last SR），
    /// 用来让发送端做 RTT 估算。这里 LSR/DLSR 简化置 0，因为 AirPlay
    /// 发送端不依赖它们做拥塞控制，只看 fraction_lost / cum_lost。
    std::vector<uint8_t> build_rr(uint32_t sender_ssrc, uint32_t our_ssrc,
                                   uint32_t highest_seq, uint32_t packets_lost,
                                   uint32_t jitter);

    /// 最近一次 SR 的信息
    const RtcpSrInfo& last_sr() const { return last_sr_; }

    /// 是否已收到至少一个 SR
    bool has_sr() const { return has_sr_; }

private:
    RtcpSrInfo last_sr_;   ///< 最近一次解析到的 SR
    bool has_sr_ = false;  ///< 是否已收到过 SR（决定 last_sr_ 是否有效）
};

}} // namespace airplay2::net

#endif // AIRPLAY2_NET_RTCP_H
