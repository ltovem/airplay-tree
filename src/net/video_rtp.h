/*!
 * @file video_rtp.h
 * @brief AirPlay 视频 RTP 接收器（UDP RTP 或 TCP Data Push 两种模式）
 *
 * AirPlay 屏幕镜像（Screen Mirroring / Data Push）有两种传输：
 *   - **TCP Data Push**（AirPlay 2 镜像默认，iOS 走这条）：
 *     在 SETUP(110) 响应返回的 dataPort 上建立 TCP listener，iOS 主动
 *     connect 后按 "128 字节头 + payload" 帧格式推流。头里前 4 字节是
 *     payload 长度（大端），packet[4] 标识类型（0x00=加密VCL、
 *     0x01=未加密 SPS/PPS、0x02=旧协议空包、0x05=streaming report），
 *     packet[8:16] 是 NTP 时间戳（自开机纳秒，无 1900 偏移）。
 *     负载是 [4B NAL 长度][NAL] 序列，AES-CTR 加密。
 *   - **UDP RTP**（AirPlay 1 视频 / HLS 前的旧路径）：3 端口结构，
 *     RTP packetization (RFC 6184/7798)，与音频共享 ctrl/timing。
 *
 * 本类两种模式都支持：open() 绑 UDP，open_tcp() 绑 TCP listener；
 * 输出统一为 Annex-B 的 VideoFrame 回调（IVideoRenderer）。
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

    /// 绑定 TCP Data Push 监听端口（AirPlay 2 镜像默认传输）。
    /// iOS 会主动 connect 到 out_data_port 并按 128B 头 + payload 帧格式推流。
    bool open_tcp(uint16_t port_min, uint16_t port_max, uint16_t& out_data_port);

    /// 是否 TCP Data Push 模式（决定 receiver_worker 的帧解析路径）
    bool is_tcp() const { return tcp_mode_; }

    /// 启动后台收包线程。若端口尚未绑定（AP2 中 RECORD 可能早于镜像 SETUP），
    /// 会延迟到 open() 成功后自动启动。
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
    void tcp_push_worker();

    VideoCodec codec_ = VideoCodec::H264_AVC;
    codec::NalReassembler reassembler_;
    VideoFrameCb cb_;

    platform::Socket data_sock_;
    // TCP Data Push 模式下 accept 到的数据连接（iOS 主动连入）
    platform::Socket push_conn_;
    bool tcp_mode_ = false;
    std::string sender_ip_;
    int sender_port_ = 0;

    std::atomic<bool> running_{false};
    // start() 在端口绑定前被调用时置位；open() 绑定成功后自动补启动。
    bool start_deferred_ = false;
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
