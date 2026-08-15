/*!
 * @file nal_reassembler.h
 * @brief H.264 (RFC 6184) + H.265 (RFC 7798) RTP 解包 / NAL 重组
 *
 * AirPlay 视频 RTP 负载规则（与标准 IETF RFC 一致，无私有扩展）：
 *   H.264:
 *     - Single NAL: type 1~23, STAP-A (24), FU-A (28)
 *     - 不使用 FU-B / MTAP16 / MTAP24（AirPlay 发送端实际不会送）
 *   H.265:
 *     - Single NAL: type 0..47
 *     - AP (Aggregation Packet, type 48)
 *     - FU (Fragmentation Unit, type 49)
 *
 * 输出格式统一为 Annex-B Byte Stream：每帧前面加 0x00000001 start code，
 * 便于直接喂 FFmpeg / VideoToolbox / MediaCodec。
 *
 * 低延时策略：
 *   - 任何 FU-A/FU 分片一旦发现"序列号跳 >1"立即标记 has_loss 并丢弃当前帧，
 *     直接等下一个 I 帧
 *   - 不缓存超过 256 个分片
 *   - 帧缓冲 flush 阈值：2*RTP ts (48kHz or 90kHz) 时钟时间窗 + 30ms
 */
#ifndef AIRPLAY2_NAL_REASSEMBLER_H
#define AIRPLAY2_NAL_REASSEMBLER_H

#include "airplay2/video_renderer.h"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <deque>
#include <mutex>

namespace airplay2 {
namespace codec {

/*!
 * @brief 单包 RTP 视频包（解析后）
 */
struct RtpVideoPacket {
    uint16_t seq       = 0;     ///< RTP 序列号
    uint32_t ts        = 0;     ///< RTP 时间戳（视频一般 90000 Hz）
    uint32_t ssrc      = 0;
    bool     marker    = false; ///< RTP Marker：一帧最后一包置位
    bool     has_loss  = false;
    std::vector<uint8_t> payload;
};

/*!
 * @brief NAL 重组器
 *
 * 用法：
 *   NalReassembler n;
 *   n.set_codec(H264);
 *   for each RTP pkt:
 *     if (auto frame = n.push(pkt)) renderer.on_frame(*frame);
 *   // flush 最后一帧
 *   auto tail = n.flush();
 */
class NalReassembler {
public:
    NalReassembler() = default;
    ~NalReassembler() = default;

    void set_codec(VideoCodec c) { codec_ = c; }
    VideoCodec codec() const { return codec_; }

    /// 设置 SPS/PPS（H.264）或 VPS/SPS/PPS（H.265）。
    /// 这些参数会在每次 IDR 帧前自动 prepend 到 Annex-B 输出。
    void set_codec_data(const std::vector<uint8_t>& extra) { codec_extra_ = extra; }
    const std::vector<uint8_t>& codec_data() const { return codec_extra_; }

    /// 喂入一个 RTP 视频包；返回"完整帧"若该包让某帧闭合。
    /// has_loss 在这一帧非空时意味着：要么该帧本身有碎片丢了，要么前面
    /// 已有不可恢复丢包，建议调用方丢掉直到下一关键帧。
    std::unique_ptr<VideoFrame> push(const RtpVideoPacket& pkt);

    /// 强制 flush 未完成的帧（用于流切换 / TEARDOWN / SET_PARAMETER）。
    std::unique_ptr<VideoFrame> flush();

    /// 丢包计数（仅供 stats）
    uint64_t packets_fragmented() const { return frag_ctr_; }
    uint64_t frames_lost() const { return frame_loss_ctr_; }

private:
    /* ---- H.264 NAL header helpers ---- */
    static inline bool h264_is_idr_slice(uint8_t nal_type) {
        return nal_type == 5 /* IDR */ || nal_type == 7 /* SPS */ || nal_type == 8 /* PPS */;
    }
    static inline bool h265_is_irap_slice(uint8_t nal_type_6bits) {
        // H.265: IRAP (Intra Random Access Picture) range 16..23 + 32..35 etc.
        // BLA(16..18), IDR(19..20), CRA(21), RSV IRAP(22..23)
        return (nal_type_6bits >= 16 && nal_type_6bits <= 23);
    }
    static inline void write_start_code(std::vector<uint8_t>& out) {
        out.push_back(0x00); out.push_back(0x00); out.push_back(0x00); out.push_back(0x01);
    }

    VideoCodec codec_ = VideoCodec::H264_AVC;
    std::vector<uint8_t> codec_extra_;

    // 当前帧累积（来自 FU/AP/多 NAL）
    struct PendingFrame {
        uint32_t ts = 0;
        bool     marker_seen = false;
        bool     has_loss = false;
        bool     is_key = false;
        uint16_t last_seq = 0;
        bool     have_last_seq = false;
        std::vector<uint8_t> annex_b;
        // FU 分片重组：记录当前 FU 的 NAL header + 分片 body
        bool     fu_active = false;
        uint8_t  fu_nal_header[2] = {0,0}; // H.265 需要 2 字节
        size_t   fu_acc_bytes = 0;
    };
    PendingFrame frame_;
    uint16_t     next_expected_seq_ = 0;
    bool         have_first_seq_ = false;
    uint64_t     frag_ctr_ = 0;
    uint64_t     frame_loss_ctr_ = 0;
    std::mutex   mu_;

    /// 处理 H.264 STAP-A (24)：多 NAL 聚合包
    void push_h264_stap_a(PendingFrame& f, const uint8_t* payload, size_t len);
    /// 处理 H.264 FU-A (28)：分片
    void push_h264_fu_a(PendingFrame& f, const uint8_t* payload, size_t len, bool start, bool end);
    /// 处理 H.265 AP (48)
    void push_h265_ap(PendingFrame& f, const uint8_t* payload, size_t len);
    /// 处理 H.265 FU (49)
    void push_h265_fu(PendingFrame& f, const uint8_t* payload, size_t len, bool start, bool end);
    /// 完成当前帧（当 marker 或新帧 ts 变化时）
    std::unique_ptr<VideoFrame> close_frame(PendingFrame& f);
};

} // namespace codec
} // namespace airplay2

#endif // AIRPLAY2_NAL_REASSEMBLER_H
