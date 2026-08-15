/*!
 * @file video_rtp.h
 * @brief AirPlay 视频 RTP 接收器（与音频独立，3 端口 UDP）
 *
 * AirPlay 视频 / 屏幕镜像使用与音频相同的 3-UDP-port 结构，但：
 *   - 视频 RTP 时钟频率 = 90000 Hz（不是音频 44100/48000）
 *   - Payload type 默认 96（H.264）或 97（H.265），具体在 SDP 里协商
 *   - 没有 ALAC/AAC，而是 RTP packetization (RFC 6184/7798)
 *   - 控制面仍用 RTCP SR/RR + Timing Protocol（和音频走同一套）
 *
 * 因此我们复用 rtp_receiver 里的 UDP 端口分配 / RR / Timing 逻辑，
 * 只替换 RTP data 的处理为 NAL 重组。
 */
#ifndef AIRPLAY2_VIDEO_RTP_H
#define AIRPLAY2_VIDEO_RTP_H

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>

#include "../platform/platform_socket.h"
#include "../platform/platform_thread.h"
#include "../codec/nal_reassembler.h"
#include "airplay2/video_renderer.h"

namespace airplay2 {
namespace net {

/*!
 * @brief 视频 RTP 接收器：1 RTP data port + reuse ctrl/timing from audio
 *
 * 低延时设计：
 *   - 收到 RTP 包立刻送 NalReassembler，产生一帧就立即回调
 *   - 不做大的环形缓冲（解码器自己做缓冲）
 *   - 丢 P 帧时直接 has_loss=true 通知解码器跳过
 */
class VideoRtpReceiver {
public:
    using VideoFrameCb = std::function<void(const VideoFrame& f)>;

    VideoRtpReceiver();
    ~VideoRtpReceiver();

    /// 设置视频 codec（在 SETUP + SDP 解析后调用）
    void set_codec(VideoCodec c) { reassembler_.set_codec(c); }
    void set_codec_data(const std::vector<uint8_t>& d) { reassembler_.set_codec_data(d); }

    /// 绑定视频 RTP 本地端口（data = 视频用；ctrl 和 timing 一般共享音频那对，
    /// 可传 -1 表示不绑定，由外部管理）。返回 true 成功
    bool open(uint16_t data_port_min, uint16_t data_port_max, uint16_t& out_data_port);

    /// 启动后台收包线程
    void start();
    /// 停止线程并关闭 socket
    void stop();

    /// 设置完整帧回调（通常在 Session 里连接到 IVideoRenderer）
    void set_frame_callback(VideoFrameCb cb) { cb_ = std::move(cb); }

    /// 设置 AES-128-CTR 解密参数（视频也可能被加密）
    bool set_decryption_params(const std::string& aes_key_hex, const std::string& aes_iv_hex);

    /// 设置 AES-128-CTR 解密参数（字节数组版，AP2 密钥派生结果）
    bool set_decryption_key(const uint8_t key[16], const uint8_t iv[16]);

    /// 对端地址（可选，用于 RR）
    void set_remote_address(const std::string& ip, int port) {
        sender_ip_ = ip; sender_port_ = port;
    }

    /// flush 缓冲（seek / flush 时）
    void flush() { reassembler_.flush(); }

    bool is_open() const { return data_sock_.valid(); }

private:
    void receiver_worker();

    VideoCodec codec_ = VideoCodec::H264_AVC;
    codec::NalReassembler reassembler_;
    VideoFrameCb cb_;

    platform::Socket data_sock_;
    std::string sender_ip_;
    int sender_port_ = 0;

    std::atomic<bool> running_{false};
    platform::Thread worker_;
    std::mutex mu_;

    // AES-128-CTR
    // 复用 crypto::AesCtr — 但该头在另一个文件，这里用"前置声明式"指针，
    // 避免增加循环 include。实际实现放在 video_rtp.cpp 内。
    struct AesCtx;
    std::unique_ptr<AesCtx> aes_;
};

} // namespace net
} // namespace airplay2

#endif // AIRPLAY2_VIDEO_RTP_H
